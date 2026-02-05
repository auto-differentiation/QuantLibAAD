/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*!
 Copyright (C) 2023-2026 Xcelerit Computing Limited

 This file is part of QuantLib / XAD integration module.
 It is modified from QuantLib, a free-software/open-source library
 for financial quantitative analysts and developers - http://quantlib.org/

 QuantLib is free software: you can redistribute it and/or modify it
 under the terms of the QuantLib license.  You should have received a
 copy of the license along with this program; if not, please email
 <quantlib-dev@lists.sf.net>. The license is also available online at
 <http://quantlib.org/license.shtml>.

 This program is distributed in the hope that it will be useful, but WITHOUT
 ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 FOR A PARTICULAR PURPOSE.  See the license for more details.
*/

/* 
This example demonstrates how to use XAD to price an American equity option
using the finite different pricing engine and calculate sensitivities
with XAD.
*/

#include <ql/qldefines.hpp>
#if !defined(BOOST_ALL_NO_LIB) && defined(BOOST_MSVC)
#  include <ql/auto_link.hpp>
#endif
#include <ql/time/calendars/target.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>
#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/termstructures/volatility/equityfx/blackconstantvol.hpp>
#include <ql/instruments/vanillaoption.hpp>
#include <ql/exercise.hpp>
#include <ql/pricingengines/vanilla/baroneadesiwhaleyengine.hpp>
#include <ql/pricingengines/vanilla/fdblackscholesvanillaengine.hpp>

#include <vector>
#include <iostream>
#include <iomanip>

using namespace QuantLib;

Real priceAmerican(Rate riskFreeRate, const Calendar &calendar,
                   Date maturity, Real strike, Date settlementDate,
                   DayCounter dayCounter, Volatility volatility, Date todaysDate,
                   Spread dividendYield, Option::Type type, Real underlying,
                   const std::vector<Date> &exerciseDates)
{

    auto americanExercise = ext::make_shared<AmericanExercise>(settlementDate,
                             maturity);

    Handle<Quote> underlyingH(ext::make_shared<SimpleQuote>(underlying));

    // bootstrap the yield/dividend/vol curves
    Handle<YieldTermStructure> flatTermStructure(ext::make_shared<FlatForward>(settlementDate, riskFreeRate, dayCounter));
    Handle<YieldTermStructure> flatDividendTS(ext::make_shared<FlatForward>(settlementDate, dividendYield, dayCounter));
    Handle<BlackVolTermStructure> flatVolTS(ext::make_shared<BlackConstantVol>(settlementDate, calendar, volatility,
                                 dayCounter));
    auto payoff = ext::make_shared<PlainVanillaPayoff>(type, strike);
    auto bsmProcess = ext::make_shared<BlackScholesMertonProcess>(underlyingH, flatDividendTS,
                                      flatTermStructure, flatVolTS);

    // options
    auto american = ext::make_shared<VanillaOption>(
        payoff,
        americanExercise);
    // computing the option price with Finite differences
    Size timeSteps = 801;
    american->setPricingEngine(ext::make_shared<FdBlackScholesVanillaEngine>(bsmProcess,
                                                                             timeSteps,
                                                                             timeSteps - 1));

    return american->NPV();
}


// Sensitivities with XAD
#ifndef QLAAD_DISABLE_AAD

// create tape
using tape_type = Real::tape_type;
tape_type tape;

Real priceWithSensi(Rate riskFreeRate, const Calendar& calendar,
                    const Date maturity, Real strike, const Date settlementDate,
                    DayCounter dayCounter, Volatility volatility, Date todaysDate,
                    Spread dividendYield, Option::Type type, Real underlying,
                    const std::vector<Date>& exerciseDates, std::vector<Real>& gradient)
{
    // register the independent inputs
    tape.registerInput(riskFreeRate);
    tape.registerInput(strike);
    tape.registerInput(volatility);
    tape.registerInput(underlying);
    tape.registerInput(dividendYield);
    tape.newRecording();

    Real value = priceAmerican(riskFreeRate, calendar, maturity, strike,
                               settlementDate, dayCounter, volatility, todaysDate, dividendYield, type, underlying, exerciseDates);

    // register dependent variables and roll back the adjoints
    tape.registerOutput(value);
    derivative(value) = 1.0;
    tape.computeAdjoints();

    // add adjoints to the gradient vector
    gradient.push_back(derivative(riskFreeRate));
    gradient.push_back(derivative(strike));
    gradient.push_back(derivative(volatility));
    gradient.push_back(derivative(underlying));
    gradient.push_back(derivative(dividendYield));

    return value;
}

#endif

void printResults(Real v, const std::vector<Real> &gradient)
{

    std::cout << "\nGreeks:\n";
    std::cout << "Rho                = " << gradient[0] << "\n";
    std::cout << "Strike Sensitivity = " << gradient[1] << "\n";
    std::cout << "Vega               = " << gradient[2] << "\n";
    std::cout << "Delta              = " << gradient[3] << "\n";
    std::cout << "Dividend Rho       = " << gradient[4] << "\n";

    std::cout << std::endl;
}

int main()
{
    try
    {
        // set up dates
        Calendar calendar = TARGET();
        Date todaysDate(15, May, 1998);
        Date settlementDate(17, May, 1998);
        Settings::instance().evaluationDate() = todaysDate;

        // our option
        Option::Type type(Option::Put);
        Real underlying = 36;
        Real strike = 40;
        Spread dividendYield = 0.00;
        Rate riskFreeRate = 0.06;
        Volatility volatility = 0.20;
        Date maturity(17, May, 1999);
        DayCounter dayCounter = Actual365Fixed();

        std::vector<Date> exerciseDates;
        for (Integer i = 1; i <= 4; i++)
            exerciseDates.push_back(settlementDate + 3 * i * Months);

        std::cout.precision(10);

#ifdef QLAAD_DISABLE_AAD
        std::cout << "Pricing American equity option (without sensitivities)...\n";
        Real v = priceAmerican(riskFreeRate, calendar, maturity, strike,
                    settlementDate, dayCounter, volatility, todaysDate,
                    dividendYield, type, underlying, exerciseDates);
        std::cout << "American equity option value: " << v << "\n";
#else
        std::vector<Real> gradient;
        std::cout << "Pricing American equity option with sensitivities...\n";
        Real v = priceWithSensi(riskFreeRate, calendar, maturity, strike,
                settlementDate, dayCounter, volatility, todaysDate,
                dividendYield, type, underlying, exerciseDates, gradient);
        std::cout << "American equity option value: " << v << "\n";
        printResults(v, gradient);
#endif

        return 0;
    }
    catch (std::exception &e)
    {
        std::cerr << e.what() << std::endl;
        return 1;
    }
    catch (...)
    {
        std::cerr << "unknown error" << std::endl;
        return 1;
    }
}

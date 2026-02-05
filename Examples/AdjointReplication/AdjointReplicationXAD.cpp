/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
 Copyright (C) 2006, 2007 StatPro Italia srl
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
This example is an AAD-enabled version for the Replication sample that ships with
QuantLib. It calculates sensitivities using XAD and measures performance.
When QLAAD_DISABLE_AAD is ON, it calculates the sensitivities using finite differences.
*/

#include <ql/qldefines.hpp>
#if !defined(BOOST_ALL_NO_LIB) && defined(BOOST_MSVC)
#    include <ql/auto_link.hpp>
#endif
#include <ql/exercise.hpp>
#include <ql/instruments/barrieroption.hpp>
#include <ql/instruments/compositeinstrument.hpp>
#include <ql/instruments/europeanoption.hpp>
#include <ql/pricingengines/barrier/analyticbarrierengine.hpp>
#include <ql/pricingengines/vanilla/analyticeuropeanengine.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/volatility/equityfx/blackconstantvol.hpp>
#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/termstructures/yield/zerocurve.hpp>
#include <ql/time/calendars/nullcalendar.hpp>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <vector>

using namespace QuantLib;


Real priceBarrierOption(std::vector<Date>& dates,
                        std::vector<Real>& rates,
                        DayCounter& dayCounter,
                        Date& maturity,
                        Real& strike,
                        Option::Type& type,
                        Barrier::Type& barrierType,
                        Real& underlying,
                        Real& v,
                        Real& barrier,
                        Real& rebate) {

    auto underlyingH = ext::make_shared<SimpleQuote>(underlying);
    auto volatility = ext::make_shared<SimpleQuote>(v);
    Handle<Quote> h2(volatility);

    Handle<YieldTermStructure> ratesYield(ext::make_shared<ZeroCurve>(dates, rates, dayCounter));
    Handle<BlackVolTermStructure> flatVol(
        ext::make_shared<BlackConstantVol>(0, NullCalendar(), h2, dayCounter));

    // instantiate the option
    auto exercise = ext::make_shared<EuropeanExercise>(maturity);
    auto payoff = ext::make_shared<PlainVanillaPayoff>(type, strike);

    auto bsProcess =
        ext::make_shared<BlackScholesProcess>(Handle<Quote>(underlyingH), ratesYield, flatVol);
    // option
    auto referenceOption =
        ext::make_shared<BarrierOption>(barrierType, barrier, rebate, payoff, exercise);

    referenceOption->setPricingEngine(ext::make_shared<AnalyticBarrierEngine>(bsProcess));

    return referenceOption->NPV();
}

Real pricePortfolio(const std::vector<Date>& dates,
                    const std::vector<Real>& riskFreeRates,
                    const DayCounter& dayCounter,
                    Date maturity,
                    Real strike,
                    Option::Type type,
                    Barrier::Type barrierType,
                    Real underlying,
                    Real v,
                    Real barrier,
                    Real rebate,
                    Integer B,
                    Integer t,
                    TimeUnit timeUnit,
                    Date& today) {
    CompositeInstrument portfolio;
    auto underlyingH = ext::make_shared<SimpleQuote>(underlying);
    auto volatility = ext::make_shared<SimpleQuote>(v);
    Handle<Quote> h2(volatility);

    Handle<YieldTermStructure> ratesYield(
        ext::make_shared<ZeroCurve>(dates, riskFreeRates, dayCounter));
    Handle<BlackVolTermStructure> flatVol(
        ext::make_shared<BlackConstantVol>(0, NullCalendar(), h2, dayCounter));

    // instantiate the option
    auto exercise = ext::make_shared<EuropeanExercise>(maturity);
    auto payoff = ext::make_shared<PlainVanillaPayoff>(type, strike);

    auto bsProcess =
        ext::make_shared<BlackScholesProcess>(Handle<Quote>(underlyingH), ratesYield, flatVol);

    auto europeanEngine = ext::make_shared<AnalyticEuropeanEngine>(bsProcess);
    // Final payoff first as shown in Joshi, a put struck at K...
    auto put1 = ext::make_shared<EuropeanOption>(payoff, exercise);
    put1->setPricingEngine(europeanEngine);
    portfolio.add(put1);
    // ...minus a digital put struck at B of notional K-B...
    auto digitalPayoff = ext::make_shared<CashOrNothingPayoff>(Option::Put, barrier, 1.0);
    auto digitalPut = ext::make_shared<EuropeanOption>(digitalPayoff, exercise);
    digitalPut->setPricingEngine(europeanEngine);
    portfolio.subtract(digitalPut, strike - barrier);
    // ...minus a put option struck at B.
    auto lowerPayoff = ext::make_shared<PlainVanillaPayoff>(Option::Put, barrier);
    auto put2 = ext::make_shared<EuropeanOption>(lowerPayoff, exercise);
    put2->setPricingEngine(europeanEngine);
    portfolio.subtract(put2);

    // Now we use puts struck at B to kill the value of the
    // portfolio on a number of points (B,t).

    Integer i;
    for (i = B * t; i >= t; i -= t) {
        Date innerMaturity = today + i * timeUnit;
        auto innerExercise = ext::make_shared<EuropeanExercise>(innerMaturity);
        auto innerPayoff = ext::make_shared<PlainVanillaPayoff>(Option::Put, barrier);
        auto putn = ext::make_shared<EuropeanOption>(innerPayoff, innerExercise);
        putn->setPricingEngine(europeanEngine);
        // ...second, we evaluate the current portfolio and the
        // latest put at (B,t)...
        Date killDate = today + (i - t) * timeUnit;
        Settings::instance().evaluationDate() = killDate;
        underlyingH->setValue(barrier);
        Real portfolioValue = portfolio.NPV();
        Real putValue = putn->NPV();
        // ...finally, we estimate the notional that kills the
        // portfolio value at that point...
        Real notional = portfolioValue / putValue;
        // ...and we subtract from the portfolio a put with such
        // notional.
        portfolio.subtract(putn, notional);
    }
    Settings::instance().evaluationDate() = today;
    underlyingH->setValue(underlying);
    // ...and output the value.
    Real portfolioValue = portfolio.NPV();
    return portfolioValue;
}


#ifndef QLAAD_DISABLE_AAD

// create tape
using tape_type = Real::tape_type;
tape_type tape;


Real priceWithSensi(const std::vector<Date>& dates,
                    const std::vector<Real>& riskFreeRates,
                    const DayCounter& dayCounter,
                    Date maturity,
                    Real strike,
                    Option::Type type,
                    Barrier::Type barrierType,
                    Real underlying,
                    Real v,
                    Real barrier,
                    Real rebate,
                    Integer B,
                    Integer t,
                    TimeUnit timeUnit,
                    Date today,
                    std::vector<Real>& gradient) {
    tape.clearAll();

    auto riskFreeRates_t = riskFreeRates;

    tape.registerInputs(riskFreeRates_t);
    tape.registerInput(strike);
    tape.registerInput(v);
    tape.registerInput(underlying);
    tape.registerInput(barrier);
    tape.newRecording();

    Real value = pricePortfolio(dates, riskFreeRates_t, dayCounter, maturity, strike, type,
                                barrierType, underlying, v, barrier, rebate, B, t, timeUnit, today);

    // get the values
    tape.registerOutput(value);
    derivative(value) = 1.0;
    tape.computeAdjoints();

    gradient.clear();
    for (std::size_t i = 0; i < riskFreeRates_t.size(); ++i) {
        gradient.push_back(derivative(riskFreeRates_t[i]));
    }
    gradient.push_back(derivative(strike));
    gradient.push_back(derivative(v));
    gradient.push_back(derivative(underlying));
    gradient.push_back(derivative(barrier));

    return value;
}


#else

Real priceWithSensi(const std::vector<Date>& dates,
                    const std::vector<Real>& riskFreeRates,
                    const DayCounter& dayCounter,
                    Date maturity,
                    Real strike,
                    Option::Type type,
                    Barrier::Type barrierType,
                    Real underlying,
                    Real v,
                    Real barrier,
                    Real rebate,
                    Integer B,
                    Integer t,
                    TimeUnit timeUnit,
                    Date today,
                    std::vector<Real>& gradient) {

    Real value = pricePortfolio(dates, riskFreeRates, dayCounter, maturity, strike, type,
                                barrierType, underlying, v, barrier, rebate, B, t, timeUnit, today);

    Real eps = 1e-5;
    gradient.clear();
    auto riskFreeRates_t = riskFreeRates;
    for (std::size_t i = 0; i < riskFreeRates_t.size(); ++i) {
        riskFreeRates_t[i] += eps;
        Real v1 =
            pricePortfolio(dates, riskFreeRates_t, dayCounter, maturity, strike, type, barrierType,
                           underlying, v, barrier, rebate, B, t, timeUnit, today);
        gradient.push_back((v1 - value) / eps);
        riskFreeRates_t[i] -= eps;
    }

    Real v1 = pricePortfolio(dates, riskFreeRates, dayCounter, maturity, strike + eps, type,
                             barrierType, underlying, v, barrier, rebate, B, t, timeUnit, today);
    gradient.push_back((v1 - value) / eps);

    v1 = pricePortfolio(dates, riskFreeRates, dayCounter, maturity, strike, type, barrierType,
                        underlying, v + eps, barrier, rebate, B, t, timeUnit, today);
    gradient.push_back((v1 - value) / eps);

    v1 = pricePortfolio(dates, riskFreeRates, dayCounter, maturity, strike, type, barrierType,
                        underlying + eps, v, barrier, rebate, B, t, timeUnit, today);
    gradient.push_back((v1 - value) / eps);


    v1 = pricePortfolio(dates, riskFreeRates, dayCounter, maturity, strike, type, barrierType,
                        underlying, v, barrier + eps, rebate, B, t, timeUnit, today);
    gradient.push_back((v1 - value) / eps);

    return value;
}

#endif

void printResults(Real v, const std::vector<Real>& gradient, Size nRates) {
    std::cout << "Value              = " << v << "\n";
    std::cout << "Rho                = [";
    for (Size i = 0; i < nRates; ++i) {
        std::cout << gradient[i] << ", ";
    }
    std::cout << "]\n";
    std::cout << "Strike Sensitivity = " << gradient[nRates] << "\n";
    std::cout << "Vega               = " << gradient[nRates + 1] << "\n";
    std::cout << "Delta              = " << gradient[nRates + 2] << "\n";
    std::cout << "Barrier            = " << gradient[nRates + 3] << "\n";
}

int main() {
    try {
        std::cout << std::endl;

        Date today(29, May, 2006);
        Settings::instance().evaluationDate() = today;

        // the option to replicate
        Barrier::Type barrierType = Barrier::DownOut;
        Real barrier = 70.0;
        Real rebate = 0.0;
        Option::Type type = Option::Put;
        Real underlying = 100.0;

        Real strike = 101.0;
        Real v = 0.20;

        Date maturity = today + 1 * Years;

        DayCounter dayCounter = Actual365Fixed();


        std::vector<Integer> tn = {13, 41, 75, 165, 256, 345, 524, 703};
        std::vector<Rate> rates = {0.04, 0.04, 0.04, 0.04, 0.04, 0.04, 0.04, 0.04, 0.04};


        std::vector<Date> dates;
        dates.push_back(today);
        Size i;
        for (i = 0; i < 8; ++i) {
            dates.push_back(today + tn[i]);
        }

        std::cout.precision(15);

        // pricing a barrier option base barrier option
        std::cout << "Pricing barrier option...\n";
        Real value = priceBarrierOption(dates, rates, dayCounter, maturity, strike, type,
                                        barrierType, underlying, v, barrier, rebate);

        std::cout << "Original barrier option value : " << value << "\n";

        // pricing a portfolio of a barrier option without AAD
        int B = 26;                // number of dates
        int t = 2;                 // number of the repetition of time unit
        TimeUnit timeUnit = Weeks; // type of time unit

        constexpr int N = 1000;

        std::cout << "Pricing replication portfolio without sensitivities...\n";
        Real v1 = 0.0;
        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < N; ++i) {
            v1 = pricePortfolio(dates, rates, dayCounter, maturity, strike, type, barrierType,
                                underlying, v, barrier, rebate, B, t, timeUnit, today);
        }
        auto end = std::chrono::high_resolution_clock::now();
        auto time_plain =
            static_cast<double>(
                std::chrono::duration_cast<std::chrono::microseconds>(end - start).count()) *
            1e-3 / N;

        std::cout << "Value = " << v1 << "\n";

        // pricing with AADs
        std::vector<Real> gradient;
        std::cout << "Pricing replication portfolio with sensitivities...\n";
        Real v2 = 0.0;
        start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < N; ++i) {
            v2 = priceWithSensi(dates, rates, dayCounter, maturity, strike, type, barrierType,
                                underlying, v, barrier, rebate, B, t, timeUnit, today, gradient);
        }
        end = std::chrono::high_resolution_clock::now();
        auto time_sensi =
            static_cast<double>(
                std::chrono::duration_cast<std::chrono::microseconds>(end - start).count()) *
            1e-3 / N;

        printResults(v2, gradient, rates.size());


        std::cout << "Plain time : " << time_plain << "ms\n"
                  << "Sensi time : " << time_sensi << "ms\n"
                  << "Factor     : " << time_sensi / time_plain << "x\n";

        return 0;
    } catch (std::exception& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "unknown error" << std::endl;
        return 1;
    }
}
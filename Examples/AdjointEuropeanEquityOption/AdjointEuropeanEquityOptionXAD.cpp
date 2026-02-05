/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
 Copyright (C) 2005, 2006, 2007, 2009 StatPro Italia srl
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
This example shows how to price a portfolio of European equity options with
sensitivities computed via XAD (or bumping if QLAAD_DISABLE_AAD is ON).
It measures performance and reports the times.

This sample is used to validate XAD performance with an analytic engine.
*/

#include <ql/qldefines.hpp>
#if !defined(BOOST_ALL_NO_LIB) && defined(BOOST_MSVC)
#    include <ql/auto_link.hpp>
#endif
#include <ql/exercise.hpp>
#include <ql/instruments/vanillaoption.hpp>
#include <ql/pricingengines/vanilla/analyticeuropeanengine.hpp>
#include <ql/processes/blackscholesprocess.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/volatility/equityfx/blackconstantvol.hpp>
#include <ql/termstructures/volatility/equityfx/blackvariancecurve.hpp>
#include <ql/termstructures/volatility/equityfx/blackvoltermstructure.hpp>
#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/termstructures/yield/zerocurve.hpp>
#include <ql/time/calendars/target.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <vector>

using namespace QuantLib;

// to record all sensitivities of the portfolio
struct OptionSensitivities {
    std::vector<Real> rhos;
    std::vector<Real> strikeSensitivities;
    std::vector<Real> deltas;
    std::vector<Real> vegas;
    Real dividendRho;
};

Real priceEuropean(const std::vector<Date>& dates,
                   const std::vector<Rate>& rates,
                   const std::vector<Real>& vols,
                   const Calendar& calendar,
                   const Date maturity,
                   const std::vector<Real>& strikes,
                   const Date settlementDate,
                   DayCounter& dayCounter,
                   Date todaysDate,
                   Spread dividendYield,
                   Option::Type type,
                   const std::vector<Real>& underlyings) {

    auto europeanExercise = ext::make_shared<EuropeanExercise>(maturity);

    // setup the yield/dividend/vol curves
    Handle<YieldTermStructure> termStructure(ext::make_shared<ZeroCurve>(dates, rates, dayCounter));
    Handle<YieldTermStructure> flatDividendTS(
        ext::make_shared<FlatForward>(settlementDate, dividendYield, dayCounter));
    // cut first date (settlement date) for vol curve
    auto dvol = std::vector<Date>(dates.begin() + 1, dates.end());
    Handle<BlackVolTermStructure> volTS(
        ext::make_shared<BlackVarianceCurve>(settlementDate, dvol, vols, dayCounter));

    Real value = 0.0;
    // portfolio with several underlying values
    for (auto& underlying : underlyings) {
        Handle<Quote> underlyingH(ext::make_shared<SimpleQuote>(underlying));
        auto bsmProcess = ext::make_shared<BlackScholesMertonProcess>(underlyingH, flatDividendTS,
                                                                      termStructure, volTS);
        auto engine = ext::make_shared<AnalyticEuropeanEngine>(bsmProcess);

        // and options with several strikes for each underlying value
        for (auto& strike : strikes) {
            auto payoff = ext::make_shared<PlainVanillaPayoff>(type, strike);

            // options
            auto european = ext::make_shared<VanillaOption>(payoff, europeanExercise);
            // computing the option price with the analytic Black-Scholes formulae
            european->setPricingEngine(engine);

            value += european->NPV();
        }
    }
    return value;
}

// price with XAD sensitivities
#ifndef QLAAD_DISABLE_AAD

// create tape
using tape_type = Real::tape_type;
tape_type tape;

Real priceWithSensi(const std::vector<Date>& dates,
                    const std::vector<Rate>& rates,
                    const std::vector<Real>& vols,
                    const Calendar& calendar,
                    const Date& maturity,
                    const std::vector<Real>& strikes,
                    const Date settlementDate,
                    DayCounter& dayCounter,
                    Date todaysDate,
                    Spread dividendYield,
                    Option::Type type,
                    const std::vector<Real>& underlyings,
                    OptionSensitivities& sensiOutput) {
    // to enable repeated runs, we clear the tape first
    tape.clearAll();

    // copy inputs and register them on the tape
    auto t_rates = rates;
    auto t_vols = vols;
    auto t_strikes = strikes;
    auto t_underlyings = underlyings;
    tape.registerInputs(t_rates);
    tape.registerInputs(t_vols);
    tape.registerInputs(t_strikes);
    tape.registerInputs(t_underlyings);
    tape.registerInput(dividendYield);
    tape.newRecording();

    // price
    Real value =
        priceEuropean(dates, t_rates, t_vols, calendar, maturity, t_strikes, settlementDate,
                      dayCounter, todaysDate, dividendYield, type, t_underlyings);

    // register output, set adjoint, and roll back the tape to the inputs
    tape.registerOutput(value);
    derivative(value) = 1.0;
    tape.computeAdjoints();

    // obtain the sensitivities and store them in the result struct
    sensiOutput.rhos.clear();
    sensiOutput.rhos.reserve(t_rates.size());
    for (Size i = 0; i < t_rates.size(); ++i) {
        sensiOutput.rhos.push_back(derivative(t_rates[i]));
    }
    sensiOutput.vegas.clear();
    sensiOutput.vegas.reserve(t_vols.size());
    for (Size i = 0; i < t_vols.size(); ++i) {
        sensiOutput.vegas.push_back(derivative(t_vols[i]));
    }
    sensiOutput.strikeSensitivities.clear();
    sensiOutput.strikeSensitivities.reserve(t_strikes.size());
    for (Size i = 0; i < t_strikes.size(); ++i) {
        sensiOutput.strikeSensitivities.push_back(derivative(t_strikes[i]));
    }
    sensiOutput.deltas.clear();
    sensiOutput.deltas.reserve(t_underlyings.size());
    for (Size i = 0; i < t_underlyings.size(); ++i) {
        sensiOutput.deltas.push_back(derivative(t_underlyings[i]));
    }
    sensiOutput.dividendRho = derivative(dividendYield);

    return value;
}

#else // pricing with bumping, as we don't have XAD available here

Real priceWithSensi(const std::vector<Date>& dates,
                    const std::vector<Rate>& rates,
                    const std::vector<Real>& vols,
                    const Calendar& calendar,
                    const Date& maturity,
                    const std::vector<Real>& strikes,
                    const Date settlementDate,
                    DayCounter& dayCounter,
                    Date todaysDate,
                    Spread dividendYield,
                    Option::Type type,
                    const std::vector<Real>& underlyings,
                    OptionSensitivities& sensiOutput) {

    // copy inputs for bumping
    auto t_rates = rates;
    auto t_vols = vols;
    auto t_strikes = strikes;

    Real value =
        priceEuropean(dates, t_rates, t_vols, calendar, maturity, t_strikes, settlementDate,
                      dayCounter, todaysDate, dividendYield, type, underlyings);

    // bump each input independently and re-run the pricer to see its impact
    Real eps = 1e-5;
    sensiOutput.rhos.clear();
    sensiOutput.rhos.reserve(t_rates.size());
    for (Size i = 0; i < t_rates.size(); ++i) {
        t_rates[i] += eps;
        Real v1 =
            priceEuropean(dates, t_rates, t_vols, calendar, maturity, t_strikes, settlementDate,
                          dayCounter, todaysDate, dividendYield, type, underlyings);
        sensiOutput.rhos.push_back((v1 - value) / eps);
        t_rates[i] -= eps;
    }

    sensiOutput.vegas.clear();
    sensiOutput.vegas.reserve(t_vols.size());
    for (Size i = 0; i < t_vols.size(); ++i) {
        t_vols[i] += eps;
        Real v1 =
            priceEuropean(dates, t_rates, t_vols, calendar, maturity, t_strikes, settlementDate,
                          dayCounter, todaysDate, dividendYield, type, underlyings);
        sensiOutput.vegas.push_back((v1 - value) / eps);
        t_vols[i] -= eps;
    }

    sensiOutput.strikeSensitivities.clear();
    sensiOutput.strikeSensitivities.reserve(t_strikes.size());
    for (Size i = 0; i < t_strikes.size(); ++i) {
        t_strikes[i] += eps;
        Real v1 =
            priceEuropean(dates, t_rates, t_vols, calendar, maturity, t_strikes, settlementDate,
                          dayCounter, todaysDate, dividendYield, type, underlyings);
        sensiOutput.strikeSensitivities.push_back((v1 - value) / eps);
        t_strikes[i] -= eps;
    }

    auto t_underlyings = underlyings;
    sensiOutput.deltas.clear();
    sensiOutput.deltas.reserve(underlyings.size());
    for (Size i = 0; i < t_underlyings.size(); ++i) {
        t_underlyings[i] += eps;
        Real v1 =
            priceEuropean(dates, t_rates, t_vols, calendar, maturity, t_strikes, settlementDate,
                          dayCounter, todaysDate, dividendYield, type, t_underlyings);
        sensiOutput.deltas.push_back((v1 - value) / eps);
        t_underlyings[i] -= eps;
    }

    Real v1 = priceEuropean(dates, t_rates, t_vols, calendar, maturity, t_strikes, settlementDate,
                            dayCounter, todaysDate, dividendYield + eps, type, underlyings);
    sensiOutput.dividendRho = (v1 - value) / eps;

    return value;
}

#endif

void printResults(Real v, const OptionSensitivities& sensiOutput) {
    std::cout << "\nGreeks:\n";
    std::cout << "Rhos                  = [";
    for (auto& rho : sensiOutput.rhos) {
        std::cout << rho << ", ";
    }
    std::cout << "]\n";
    std::cout << "Strike Sensitivities = [";
    for (auto& s : sensiOutput.strikeSensitivities) {
        std::cout << s << ", ";
    }
    std::cout << "]\n";
    std::cout << "Vegas               = [";
    for (auto& vega : sensiOutput.vegas) {
        std::cout << vega << ", ";
    }
    std::cout << "]\n";
    std::cout << "Deltas               = [";
    for (auto& delta : sensiOutput.deltas) {
        std::cout << delta << ", ";
    }
    std::cout << "]\n";
    std::cout << "Dividend Rho         = " << sensiOutput.dividendRho << "\n";

    std::cout << std::endl;
}

int main() {
    try {
        // set up dates
        Calendar calendar = TARGET();
        Date todaysDate(15, May, 1998);
        Date settlementDate(17, May, 1998);
        Settings::instance().evaluationDate() = todaysDate;

        // curves
        std::vector<Integer> t = {13, 41, 75, 165, 256, 345, 524, 703};
        std::vector<Rate> r = {0.0357, 0.0349, 0.0341, 0.0355, 0.0359, 0.0368, 0.0386, 0.0401};
        std::vector<Volatility> vols = {0.20, 0.18, 0.178, 0.183, 0.192, 0.203, 0.215, 0.208};

        std::vector<Date> dates;
        std::vector<Rate> rates;
        dates.push_back(settlementDate);
        rates.push_back(r[0]);
        Size i;
        for (i = 0; i < t.size(); ++i) {
            dates.push_back(settlementDate + t[i]);
            rates.push_back(r[i]);
        }

        // our options
        Option::Type type(Option::Put);
        std::vector<Real> underlyings = {15, 20, 25, 30, 35, 40, 45, 50, 55, 60};
        std::vector<Real> strikes;
        for (Real s = 10.; s < 80.; s += 1.) {
            strikes.push_back(s);
        }
        Spread dividendYield = 0.01;
        Date maturity(17, May, 1999);
        DayCounter dayCounter = Actual365Fixed();

        std::cout.precision(5);
        constexpr int N = 100;

        // pricing without sensitivities
        std::cout << "Pricing european equity option portfolio without sensitivities..\n";
        Real v = 0.0;
        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < N; ++i) {
            v = priceEuropean(dates, rates, vols, calendar, maturity, strikes, settlementDate,
                              dayCounter, todaysDate, dividendYield, type, underlyings);
        }
        auto end = std::chrono::high_resolution_clock::now();
        auto time_plain =
            static_cast<double>(
                std::chrono::duration_cast<std::chrono::microseconds>(end - start).count()) *
            1e-3 / N;
        std::cout << "Portfolio value: " << v << "\n";

        // pricing with sensitivities
        std::vector<Real> gradient;
        std::cout << "Pricing european equity option portfolio with sensitivities...\n";
        OptionSensitivities sensi;
        Real v2 = 0.0;
        start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < N; ++i) {
            v2 = priceWithSensi(dates, rates, vols, calendar, maturity, strikes, settlementDate,
                                dayCounter, todaysDate, dividendYield, type, underlyings, sensi);
        }
        end = std::chrono::high_resolution_clock::now();
        auto time_sensi =
            static_cast<double>(
                std::chrono::duration_cast<std::chrono::microseconds>(end - start).count()) *
            1e-3 / N;
        std::cout << "Portfolio value: " << v2 << "\n";

        printResults(v2, sensi);

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

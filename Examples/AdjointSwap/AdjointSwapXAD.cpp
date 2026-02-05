/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
 Copyright (C) 2022-2026 Xcelerit Computing Limited

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
This example shows how to calculate sensitivities to market quotes for
pricing a portfolio of swaps using XAD.
It also measures the performance for calculating sensitivities,
either with XAD or with plain doubles and bumping (when QLAAD_DISABLE_AAD is ON).
*/


#include <ql/qldefines.hpp>
#if !defined(BOOST_ALL_NO_LIB) && defined(BOOST_MSVC)
#    include <ql/auto_link.hpp>
#endif
#include <ql/indexes/ibor/euribor.hpp>
#include <ql/math/randomnumbers/mt19937uniformrng.hpp>
#include <ql/pricingengines/swap/discountingswapengine.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/iterativebootstrap.hpp>
#include <ql/termstructures/yield/bootstraptraits.hpp>
#include <ql/termstructures/yield/piecewiseyieldcurve.hpp>
#include <ql/termstructures/yield/ratehelpers.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>
#include <ql/time/calendars/target.hpp>
#include <ql/time/daycounters/actual360.hpp>
#include <ql/time/daycounters/thirty360.hpp>
#include <XAD/XAD.hpp>
#include <chrono>
#include <vector>

using namespace QuantLib;

const int Ndepos = 10;
const int Nfra = 5;

// prepares a set of market quotes for deposits, FRAs, and Swap rates, used for curve bootstrapping
void prepareQuotes(Size maximumMaturity, std::vector<double>& marketQuotes) {
    // setting up market quotes
    // deposit quotes on, tn, sn, sw, 1m, ... , 6m
    for (Size i = 0; i < Ndepos; ++i)
        marketQuotes.push_back(0.0010 + i * 0.0002);
    // fra quotes 1-7, ... , 5-11
    for (Size i = 0; i < Nfra; ++i)
        marketQuotes.push_back(0.0030 + i * 0.0005);
    // swap quotes 1y, ... , maximum maturity
    for (Size i = 0; i < maximumMaturity; ++i)
        marketQuotes.push_back(0.0060 + i * 0.0001);
}

// bootstraps the curve used for the swaps
Handle<YieldTermStructure>
bootstrapCurve(Date referenceDate, const std::vector<Real>& marketQuotes, Size maximumMaturity) {
    // build quotes
    std::vector<ext::shared_ptr<SimpleQuote>> quotes;
    std::transform(marketQuotes.begin(), marketQuotes.end(), std::back_inserter(quotes),
                   [](const Real& quote) { return ext::make_shared<SimpleQuote>(quote); });

    std::vector<RelinkableHandle<Quote>> quoteHandles;
    std::transform(quotes.begin(), quotes.end(), std::back_inserter(quoteHandles),
                   [](ext::shared_ptr<SimpleQuote>& q) { return RelinkableHandle<Quote>(q); });

    // rate helpers
    std::vector<ext::shared_ptr<RateHelper>> instruments;

    // deposits
    for (Size i = 0; i < Ndepos; ++i) {
        Period matTmp;
        Size fixingDays = 2;
        switch (i) {
            case 0:
                matTmp = 1 * Days;
                fixingDays = 0;
                break;
            case 1:
                matTmp = 1 * Days;
                fixingDays = 1;
                break;
            case 2:
                matTmp = 1 * Days;
                break;
            case 3:
                matTmp = 1 * Weeks;
                break;
            default:
                matTmp = (i - 3) * Months;
                break;
        }
        instruments.push_back(ext::make_shared<DepositRateHelper>(
            quoteHandles[i], matTmp, fixingDays, TARGET(), ModifiedFollowing, false, Actual360()));
    }

    // FRAs
    for (Size i = 0; i < Nfra; ++i) {
        instruments.push_back(
            ext::make_shared<FraRateHelper>(quoteHandles[Ndepos + i], (i + 1), (i + 7), 2,
                                              TARGET(), ModifiedFollowing, false, Actual360()));
    }

    // swaps
    auto euribor6m = ext::make_shared<Euribor>(6 * Months);
    for (Size i = 0; i < maximumMaturity; ++i) {
        instruments.push_back(ext::make_shared<SwapRateHelper>(
            quoteHandles[Ndepos + Nfra + i], (i + 1) * Years, TARGET(), Annual, ModifiedFollowing,
            Thirty360(Thirty360::European), euribor6m));
    }

    // build a piecewise yield curve
    using CurveType = PiecewiseYieldCurve<ZeroYield, Linear, IterativeBootstrap>;
    return Handle<YieldTermStructure>(
        ext::make_shared<CurveType>(referenceDate, instruments, Actual365Fixed()));
}

// creates the Swap portfolio, given the curve
std::vector<ext::shared_ptr<VanillaSwap>>
setupPortfolio(Size portfolioSize, Size maximumMaturity, Handle<YieldTermStructure> curveHandle) {
    auto euribor6mYts = ext::make_shared<Euribor>(6 * Months, curveHandle);

    // set up a vanilla swap portfolio
    euribor6mYts->addFixing(Date(2, October, 2014), 0.0040);
    euribor6mYts->addFixing(Date(3, October, 2014), 0.0040);
    euribor6mYts->addFixing(Date(6, October, 2014), 0.0040);

    std::vector<ext::shared_ptr<VanillaSwap>> portfolio;
    MersenneTwisterUniformRng mt(42);

    for (Size j = 0; j < portfolioSize; ++j) {
        Real fixedRate = mt.nextReal() * 0.10;
        Date effective(6, October, 2014);
        Date termination = TARGET().advance(
            effective,
            static_cast<Size>(mt.nextReal() * static_cast<double>(maximumMaturity) + 1.) * Years);

        Schedule fixedSchedule(effective, termination, 1 * Years, TARGET(), ModifiedFollowing,
                               Following, DateGeneration::Backward, false);

        Schedule floatSchedule(effective, termination, 6 * Months, TARGET(), ModifiedFollowing,
                               Following, DateGeneration::Backward, false);

        portfolio.push_back(ext::make_shared<VanillaSwap>(
            VanillaSwap::Receiver, 10000000.0 / portfolioSize, fixedSchedule, fixedRate,
            Thirty360(Thirty360::European), floatSchedule, euribor6mYts, 0.0, Actual360()));
    }

    return portfolio;
}

// prices the portfolio using the DiscountingSwapEngine
Real pricePortfolio(Handle<YieldTermStructure> curveHandle,
                    std::vector<ext::shared_ptr<VanillaSwap>>& portfolio) {

    auto pricingEngine = ext::make_shared<DiscountingSwapEngine>(curveHandle);
    Real y = 0.0;
    for (auto& swap : portfolio) {
        swap->setPricingEngine(pricingEngine);
        y += swap->NPV();
    }
    return y;
}

// price with sensitivities using AAD
Real pricePlain(const std::vector<double>& marketQuotes, Size portfolioSize, Size maxMaturity) {
    // create a copy of the market quotes, so we can bump each of them without affecting inputs
    std::vector<Real> marketQuotesInp(marketQuotes.begin(), marketQuotes.end());

    // value the portfolio
    Real v = 0.0;
    {
        auto curveHandle =
            bootstrapCurve(Settings::instance().evaluationDate(), marketQuotesInp, maxMaturity);
        auto portfolio = setupPortfolio(portfolioSize, maxMaturity, curveHandle);
        v = pricePortfolio(curveHandle, portfolio);
    }

    return v;
}

#ifndef QLAAD_DISABLE_AAD

// create tape
using tape_type = Real::tape_type;
tape_type tape;

// price with sensitivities using AAD
double priceWithSensi(const std::vector<double>& marketQuotes,
                      Size portfolioSize,
                      Size maxMaturity,
                      std::vector<double>& gradient) {

    tape.clearAll();
    gradient.clear();

    // convert double market quotes into AD type (Real is active double type)
    std::vector<Real> marketQuotesAD(marketQuotes.begin(), marketQuotes.end());

    // register independent variables with the tape as inputs and start a new recording
    tape.registerInputs(marketQuotesAD);
    tape.newRecording();

    // build curve and price
    auto curveHandle =
        bootstrapCurve(Settings::instance().evaluationDate(), marketQuotesAD, maxMaturity);
    auto portfolio = setupPortfolio(portfolioSize, maxMaturity, curveHandle);
    Real v = pricePortfolio(curveHandle, portfolio);

    // set adjoint of output and roll back the tape (propagate the adjoints)
    derivative(v) = 1.0;
    tape.computeAdjoints();

    // read adjoints of inputs and store them in gradient vector
    std::transform(marketQuotesAD.begin(), marketQuotesAD.end(), std::back_inserter(gradient),
                   [](const Real& q) { return derivative(q); });

    return value(v);
}

#else

// price with sensitivities using Bumping
double priceWithSensi(const std::vector<double>& marketQuotes,
                      Size portfolioSize,
                      Size maxMaturity,
                      std::vector<double>& gradient) {
    gradient.clear();

    // convert double market quotes into AD type (Real is active double type)
    std::vector<Real> marketQuotesCpy(marketQuotes.begin(), marketQuotes.end());

    // build curve and price
    auto curveHandle =
        bootstrapCurve(Settings::instance().evaluationDate(), marketQuotesCpy, maxMaturity);
    auto portfolio = setupPortfolio(portfolioSize, maxMaturity, curveHandle);
    Real v = pricePortfolio(curveHandle, portfolio);

    Real eps = 1e-5;
    for (Size i = 0; i < marketQuotesCpy.size(); ++i) {
        marketQuotesCpy[i] += eps;
        auto curveHandle =
            bootstrapCurve(Settings::instance().evaluationDate(), marketQuotesCpy, maxMaturity);
        auto portfolio = setupPortfolio(portfolioSize, maxMaturity, curveHandle);
        Real v1 = pricePortfolio(curveHandle, portfolio);
        gradient.push_back(xad::value((v - v1) / eps));
        marketQuotesCpy[i] -= eps;
    }

    return xad::value(v);
}


#endif

void printResults(double v, const std::vector<double>& gradient) {
    std::cout.precision(2);
    std::cout << std::fixed;
    std::cout.imbue(std::locale(""));

    std::cout << "Portfolio value: " << v << "\n";

    std::cout << "\nSensitivities w.r.t. deposit quotes:\n";
    for (std::size_t i = 0; i < Ndepos; ++i)
        std::cout << "dv/ddepo[" << i << "] = " << gradient[i] << "\n";

    std::cout << "\nSensitivities w.r.t. FRA quotes:\n";
    for (std::size_t i = Ndepos; i < Ndepos + Nfra; ++i)
        std::cout << "dv/dFRA[" << i - Ndepos << "] = " << gradient[i] << "\n";

    std::cout << "\nSensitivities w.r.t. Swap quotes:\n";
    for (std::size_t i = Ndepos + Nfra; i < gradient.size(); ++i)
        std::cout << "dv/dSwap[" << i - Ndepos - Nfra << "] = " << gradient[i] << "\n";
}

int main() {
    try {
        Size portfolioSize = 50;
        Size maxMaturity = 40;

        // market Quotes
        std::vector<double> marketQuotes;
        prepareQuotes(maxMaturity, marketQuotes);

        // evaluation date
        Date referenceDate(2, January, 2015);
        Settings::instance().evaluationDate() = referenceDate;

        constexpr int N = 20;
        std::cout << "Pricing portfolio of " << portfolioSize << " swaps...\n";
        auto start = std::chrono::high_resolution_clock::now();
        Real v = 0.0;
        for (int i = 0; i < N; ++i)
            v = pricePlain(marketQuotes, portfolioSize, maxMaturity);
        auto end = std::chrono::high_resolution_clock::now();
        auto time_plain =
            static_cast<double>(
                std::chrono::duration_cast<std::chrono::microseconds>(end - start).count()) *
            1e-3 / N;
        std::cout << "portfolio Value: " << v << "\n";

        std::vector<double> gradient;

        std::cout << "Pricing portfolio of " << portfolioSize << " swaps with sensitivities...\n";
        start = std::chrono::high_resolution_clock::now();
        double v2 = 0.0;
        for (int i = 0; i < N; ++i)
            v2 = priceWithSensi(marketQuotes, portfolioSize, maxMaturity, gradient);
        end = std::chrono::high_resolution_clock::now();
        auto time_sensi =
            static_cast<double>(
                std::chrono::duration_cast<std::chrono::microseconds>(end - start).count()) *
            1e-3 / N;

        printResults(v2, gradient);

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
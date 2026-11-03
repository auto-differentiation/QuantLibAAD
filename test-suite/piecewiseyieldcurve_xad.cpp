/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
 Copyright (C) 2026 Xcelerit Computing Limited

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

#include "toplevelfixture.hpp"
#include "utilities_xad.hpp"
#include <ql/indexes/ibor/euribor.hpp>
#include <ql/instruments/vanillaswap.hpp>
#include <ql/pricingengines/swap/discountingswapengine.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/iterativebootstrap.hpp>
#include <ql/termstructures/yield/bootstraptraits.hpp>
#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/termstructures/yield/piecewiseyieldcurve.hpp>
#include <ql/termstructures/yield/ratehelpers.hpp>
#include <ql/time/calendars/target.hpp>
#include <ql/time/daycounters/actual360.hpp>
#include <ql/time/daycounters/thirty360.hpp>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

using namespace QuantLib;
using namespace boost::unit_test_framework;

BOOST_FIXTURE_TEST_SUITE(QuantLibAADTests, TopLevelFixture)

BOOST_AUTO_TEST_SUITE(PiecewiseYieldCurveXadTests)

namespace {

    using Curve = PiecewiseYieldCurve<ForwardRate, BackwardFlat, IterativeBootstrap>;

    // Forward swap priced on a deposit/FRA/swap curve, given the quotes in that order.
    Real priceForwardSwap(const std::vector<Real>& rates) {
        Calendar calendar = TARGET();
        Date settlementDate(8, November, 2001);
        DayCounter dayCounter = Actual360();

        std::vector<ext::shared_ptr<SimpleQuote>> quotes;
        for (const auto& rate : rates)
            quotes.push_back(ext::make_shared<SimpleQuote>(rate));

        Size k = 0;
        std::vector<ext::shared_ptr<RateHelper>> helpers;
        helpers.push_back(ext::make_shared<DepositRateHelper>(Handle<Quote>(quotes[k++]),
                                                              3 * Months, 2, calendar,
                                                              ModifiedFollowing, false, dayCounter));
        for (const auto& months :
             {std::make_pair(3, 6), std::make_pair(6, 9), std::make_pair(9, 12)})
            helpers.push_back(ext::make_shared<FraRateHelper>(Handle<Quote>(quotes[k++]),
                                                              months.first, months.second, 2,
                                                              calendar, ModifiedFollowing, false,
                                                              dayCounter));
        Handle<YieldTermStructure> discount(
            ext::make_shared<FlatForward>(settlementDate, 0.04, Actual360()));
        for (int years : {2, 3, 5, 10, 15})
            helpers.push_back(ext::make_shared<SwapRateHelper>(
                Handle<Quote>(quotes[k++]), years * Years, calendar, Annual, Unadjusted,
                Thirty360(Thirty360::BondBasis), ext::make_shared<Euribor3M>(), Handle<Quote>(),
                0 * Days, discount));

        Handle<YieldTermStructure> forecast(
            ext::make_shared<Curve>(settlementDate, helpers, Actual360(), BackwardFlat()));
        auto index = ext::make_shared<Euribor3M>(forecast);

        Date start = calendar.advance(settlementDate, 1, Years);
        Date end = calendar.advance(start, 5, Years);
        Schedule fixedSchedule(start, end, 1 * Years, calendar, Unadjusted, Unadjusted,
                               DateGeneration::Forward, false);
        Schedule floatingSchedule(start, end, 3 * Months, calendar, ModifiedFollowing,
                                  ModifiedFollowing, DateGeneration::Forward, false);
        VanillaSwap swap(Swap::Payer, 1000000.0, fixedSchedule, 0.04,
                         Thirty360(Thirty360::BondBasis), floatingSchedule, index, 0.0,
                         index->dayCounter());
        swap.setPricingEngine(ext::make_shared<DiscountingSwapEngine>(discount));
        return swap.NPV();
    }

    std::vector<double> quoteDerivativesWithAAD(const std::vector<double>& rates) {
        using tape_type = Real::tape_type;
        tape_type tape;
        std::vector<Real> quotes(rates.begin(), rates.end());
        tape.registerInputs(quotes);
        tape.newRecording();

        Real price = priceForwardSwap(quotes);

        tape.registerOutput(price);
        derivative(price) = 1.0;
        tape.computeAdjoints();

        std::vector<double> derivatives;
        for (const auto& quote : quotes)
            derivatives.push_back(derivative(quote));
        return derivatives;
    }

    double quoteDerivativeWithCentralBump(const std::vector<double>& rates, Size i) {
        const double h = 1e-4 * std::fabs(rates[i]);
        std::vector<Real> up(rates.begin(), rates.end());
        std::vector<Real> down(rates.begin(), rates.end());
        up[i] += h;
        down[i] -= h;
        return value(priceForwardSwap(up) - priceForwardSwap(down)) / (2.0 * h);
    }

}

// Equal FRA quotes make the bootstrap's initial guess for the later FRAs almost exact, so the
// solver converges without moving far from it. The derivatives must still be those of the root.
BOOST_AUTO_TEST_CASE(testForwardSwapQuoteDerivativesWithEqualFraQuotes) {

    BOOST_TEST_MESSAGE("Testing forward swap derivatives on a curve with equal FRA quotes...");

    Settings::instance().evaluationDate() = Date(6, November, 2001);

    const std::vector<std::string> names = {"depo 3M",  "FRA 3x6", "FRA 6x9",
                                            "FRA 9x12", "swap 2Y", "swap 3Y",
                                            "swap 5Y",  "swap 10Y", "swap 15Y"};
    const std::vector<double> rates = {0.0363, 0.037125, 0.037125, 0.037125, 0.037125,
                                       0.0398, 0.0443,   0.05165,  0.055175};

    const double relativeTolerance = 1e-6;
    const double absoluteTolerance = 1e-2; // for sensitivities below 1 in size

    const std::vector<double> adjoints = quoteDerivativesWithAAD(rates);

    for (Size i = 0; i < rates.size(); ++i) {
        const double bumped = quoteDerivativeWithCentralBump(rates, i);
        const double error = std::fabs(adjoints[i] - bumped);
        const double tolerance =
            std::fabs(bumped) < 1.0 ? absoluteTolerance : relativeTolerance * std::fabs(bumped);
        BOOST_TEST_MESSAGE(names[i] << ": adjoint " << adjoints[i] << ", bump " << bumped
                                    << ", error " << error << ", tolerance " << tolerance);
        BOOST_CHECK_MESSAGE(error <= tolerance,
                            names[i] << ": adjoint " << adjoints[i] << ", central bump " << bumped
                                     << ", error " << error << " exceeds tolerance " << tolerance);
    }
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE_END()

/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
 Copyright (C) 2003, 2004 Ferdinando Ametrano
 Copyright (C) 2005, 2007 StatPro Italia srl
 Copyright (C) 2005 Joseph Wang
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

#include "toplevelfixture.hpp"
#include "utilities_xad.hpp"
#include <ql/instruments/creditdefaultswap.hpp>
#include <ql/pricingengines/credit/midpointcdsengine.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/credit/flathazardrate.hpp>
#include <ql/termstructures/defaulttermstructure.hpp>
#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/time/calendars/target.hpp>
#include <ql/time/dategenerationrule.hpp>
#include <ql/time/daycounters/actual360.hpp>
#include <ql/time/schedule.hpp>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <vector>

using namespace QuantLib;
using namespace boost::unit_test_framework;

BOOST_FIXTURE_TEST_SUITE(QuantLibAADTests, TopLevelFixture)

BOOST_AUTO_TEST_SUITE(CreditDefaultSwapXadTests)


namespace {

    struct CreditDefaultSwapData {
        // Build the CDS
        Rate fixedRate;
        Real notional;
        Real recoveryRate;
        Real hazardRate;
        Real riskFreeRate;
    };

}

namespace {

    template <class PriceFunc>
    Real priceWithBumping(const CreditDefaultSwapData& value,
                          CreditDefaultSwapData& derivatives,
                          PriceFunc func) {
        // Bumping
        auto eps = 1e-7;
        auto data = value;
        auto v = func(data);

        data.fixedRate += eps;
        auto vplus = func(data);
        derivatives.fixedRate = (vplus - v) / eps;
        data = value;

        data.notional += 1;
        vplus = func(data);
        derivatives.notional = (vplus - v) / 1;
        data = value;

        data.recoveryRate += eps;
        vplus = func(data);
        derivatives.recoveryRate = (vplus - v) / eps;
        data = value;

        data.hazardRate += eps;
        vplus = func(data);
        derivatives.hazardRate = (vplus - v) / eps;
        data = value;

        data.riskFreeRate += eps;
        vplus = func(data);
        derivatives.riskFreeRate = (vplus - v) / eps;
        data = value;

        return v;
    }

    template <class PriceFunc>
    Real priceWithAAD(const CreditDefaultSwapData& values,
                      CreditDefaultSwapData& derivatives,
                      PriceFunc func) {
        // AAD
        using tape_type = Real::tape_type;
        tape_type tape;
        auto data = values;
        tape.registerInput(data.notional);
        tape.registerInput(data.hazardRate);
        tape.registerInput(data.recoveryRate);
        tape.registerInput(data.fixedRate);
        tape.registerInput(data.riskFreeRate);
        tape.newRecording();

        auto price = func(data);

        tape.registerOutput(price);
        derivative(price) = 1.0;
        tape.computeAdjoints();

        derivatives.notional = derivative(data.notional);
        derivatives.hazardRate = derivative(data.hazardRate);
        derivatives.recoveryRate = derivative(data.recoveryRate);
        derivatives.fixedRate = derivative(data.fixedRate);
        derivatives.riskFreeRate = derivative(data.riskFreeRate);

        return price;
    }
}

namespace {
    Real priceCreditDefaultSwap(const CreditDefaultSwapData& value) {
        DayCounter dayCount = Actual360();
        // Initialize curves
        Settings::instance().evaluationDate() = Date(9, June, 2006);
        Date today = Settings::instance().evaluationDate();
        Calendar calendar = TARGET();

        Handle<Quote> hazardRate(ext::make_shared<SimpleQuote>(value.hazardRate));
        RelinkableHandle<DefaultProbabilityTermStructure> probabilityCurve;
        probabilityCurve.linkTo(
            ext::make_shared<FlatHazardRate>(0, calendar, hazardRate, Actual360()));

        RelinkableHandle<YieldTermStructure> discountCurve;

        discountCurve.linkTo(ext::make_shared<FlatForward>(today, value.riskFreeRate, Actual360()));

        // Build the schedule
        Date issueDate = calendar.advance(today, -1, Years);
        Date maturity = calendar.advance(issueDate, 10, Years);
        Frequency frequency = Semiannual;
        BusinessDayConvention convention = ModifiedFollowing;

        Schedule schedule(issueDate, maturity, Period(frequency), calendar, convention, convention,
                          DateGeneration::Forward, false);

        auto cds =
            ext::make_shared<CreditDefaultSwap>(Protection::Seller, value.notional, value.fixedRate,
                                                schedule, convention, dayCount, true, true);
        cds->setPricingEngine(ext::make_shared<MidPointCdsEngine>(
            probabilityCurve, value.recoveryRate, discountCurve));
        return cds->NPV();
    }
}

BOOST_AUTO_TEST_CASE(testCreditDefaultSwapDerivatives) {

    SavedSettings save;
    BOOST_TEST_MESSAGE("Testing credit default swap derivatives...");

    // input
    auto data = CreditDefaultSwapData{0.0120, 10000.0, 0.4, 0.01234, 0.06};

    // bumping
    auto derivatives_bumping = CreditDefaultSwapData{};
    auto expected = priceWithBumping(data, derivatives_bumping, priceCreditDefaultSwap);

    // aad
    auto derivatives_aad = CreditDefaultSwapData{};
    auto actual = priceWithAAD(data, derivatives_aad, priceCreditDefaultSwap);

    // compare
    QL_CHECK_CLOSE(expected, actual, 1e-9);
    QL_CHECK_CLOSE(derivatives_bumping.notional, derivatives_aad.notional, 1e-3);
    QL_CHECK_CLOSE(derivatives_bumping.hazardRate, derivatives_aad.hazardRate, 1e-3);
    QL_CHECK_CLOSE(derivatives_bumping.fixedRate, derivatives_aad.fixedRate, 1e-3);
    QL_CHECK_CLOSE(derivatives_bumping.recoveryRate, derivatives_aad.recoveryRate, 1e-3);
    QL_CHECK_CLOSE(derivatives_bumping.riskFreeRate, derivatives_aad.riskFreeRate, 1e-3);
}


BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE_END()

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
#include <ql/experimental/callablebonds/callablebond.hpp>
#include <ql/experimental/callablebonds/treecallablebondengine.hpp>
#include <ql/instruments/bonds/fixedratebond.hpp>
#include <ql/models/shortrate/onefactormodels/hullwhite.hpp>
#include <ql/pricingengines/bond/discountingbondengine.hpp>
#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/termstructures/yield/zerocurve.hpp>
#include <ql/time/calendars/unitedstates.hpp>
#include <ql/time/dategenerationrule.hpp>
#include <ql/time/daycounters/thirty360.hpp>
#include <ql/time/schedule.hpp>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <vector>

using namespace QuantLib;
using namespace boost::unit_test_framework;

BOOST_FIXTURE_TEST_SUITE(QuantLibAADTests, TopLevelFixture)

BOOST_AUTO_TEST_SUITE(BondsXadTests)

namespace {

    struct BondsData {
        Real spotRate1;
        Real spotRate2;
        Real spotRate3;
        Real couponRate;
        Real faceValue;
    };

}
namespace {

    template <class PriceFunc>
    Real priceWithBumping(const BondsData& value, BondsData& derivatives, PriceFunc func) {
        // Bumping
        auto eps = 1e-7;
        auto data = value;
        auto v = func(data);

        data.spotRate1 += eps;
        auto vplus = func(data);
        derivatives.spotRate1 = (vplus - v) / eps;
        data = value;

        data.spotRate2 += eps;
        vplus = func(data);
        derivatives.spotRate2 = (vplus - v) / eps;
        data = value;

        data.spotRate3 += eps;
        vplus = func(data);
        derivatives.spotRate3 = (vplus - v) / eps;
        data = value;

        data.couponRate += eps;
        vplus = func(data);
        derivatives.couponRate = (vplus - v) / eps;
        data = value;

        data.faceValue += eps;
        vplus = func(data);
        derivatives.faceValue = (vplus - v) / eps;

        return v;
    }

    template <class PriceFunc>
    Real priceWithAAD(const BondsData& values, BondsData& derivatives, PriceFunc func) {
        // AAD
        using tape_type = Real::tape_type;
        tape_type tape;
        auto data = values;
        tape.registerInput(data.spotRate1);
        tape.registerInput(data.spotRate2);
        tape.registerInput(data.spotRate3);
        tape.registerInput(data.couponRate);
        tape.registerInput(data.faceValue);
        tape.newRecording();

        auto price = func(data);

        tape.registerOutput(price);
        derivative(price) = 1.0;
        tape.computeAdjoints();

        derivatives.spotRate1 = derivative(data.spotRate1);
        derivatives.spotRate2 = derivative(data.spotRate2);
        derivatives.spotRate3 = derivative(data.spotRate3);
        derivatives.couponRate = derivative(data.couponRate);
        derivatives.faceValue = derivative(data.faceValue);

        return price;
    }
}

namespace {
    Real priceBonds(const BondsData& value) {
        Date today = Date(15, January, 2015);
        Settings::instance().evaluationDate() = today;

        std::vector<Date> spotDates = {Date(15, January, 2015), Date(15, July, 2015),
                                       Date(15, January, 2016)};
        std::vector<Real> spotRates = {value.spotRate1, value.spotRate2, value.spotRate3};
        DayCounter dayCount = Thirty360(Thirty360::USA);
        Calendar calendar = UnitedStates(UnitedStates::GovernmentBond);
        Compounding compounding = Compounded;
        Frequency compoundingFrequency = Annual;

        Handle<YieldTermStructure> spotCurveHandle(ext::make_shared<ZeroCurve>(
            spotDates, spotRates, dayCount, calendar, Linear(), compounding, compoundingFrequency));

        Date issueDate = Date(15, January, 2015);
        Date maturityDate = Date(15, January, 2016);

        Schedule schedule(issueDate, maturityDate, Period(Semiannual), calendar, Unadjusted,
                          Unadjusted, DateGeneration::Backward, false);
        // build the coupon
        std::vector<Real> coupons = {value.couponRate};

        // construct the FixedRateBond
        auto fixedRateBond =
            ext::make_shared<FixedRateBond>(0, value.faceValue, schedule, coupons, dayCount);

        auto bondEngine = ext::make_shared<DiscountingBondEngine>(spotCurveHandle);
        fixedRateBond->setPricingEngine(bondEngine);

        return fixedRateBond->NPV();
    }
}

BOOST_AUTO_TEST_CASE(testBondsDerivatives) {

    SavedSettings save;
    BOOST_TEST_MESSAGE("Testing bonds derivatives...");

    // input
    auto data = BondsData{0.0, 0.005, 0.007, 0.06, 100.0};

    // bumping
    auto derivatives_bumping = BondsData{};
    auto expected = priceWithBumping(data, derivatives_bumping, priceBonds);

    // aad
    auto derivatives_aad = BondsData{};
    auto actual = priceWithAAD(data, derivatives_aad, priceBonds);

    // compare
    QL_CHECK_CLOSE(expected, actual, 1e-9);
    QL_CHECK_CLOSE(derivatives_bumping.spotRate1, derivatives_aad.spotRate1, 1e-3);
    QL_CHECK_CLOSE(derivatives_bumping.spotRate2, derivatives_aad.spotRate2, 1e-3);
    QL_CHECK_CLOSE(derivatives_bumping.spotRate3, derivatives_aad.spotRate3, 1e-3);
    QL_CHECK_CLOSE(derivatives_bumping.couponRate, derivatives_aad.couponRate, 1e-3);
    QL_CHECK_CLOSE(derivatives_bumping.faceValue, derivatives_aad.faceValue, 1e-3);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE_END()

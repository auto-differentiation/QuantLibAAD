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
#include <ql/cashflows/coupon.hpp>
#include <ql/indexes/ibor/euribor.hpp>
#include <ql/instruments/swaption.hpp>
#include <ql/instruments/vanillaswap.hpp>
#include <ql/models/shortrate/onefactormodels/hullwhite.hpp>
#include <ql/pricingengines/swap/discountingswapengine.hpp>
#include <ql/pricingengines/swaption/treeswaptionengine.hpp>
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

BOOST_AUTO_TEST_SUITE(BermudanSwaptionXadTests)

namespace {

    struct BermudanSwaptionData {
        Swap::Type type;
        Real nominal = 1000.0;
        Real fixedRate;
        Real forwardRate;
        Real a = 0.048696;
        Real sigma = 0.0058904;
    };

}
namespace {

    template <class PriceFunc>
    Real priceWithBumping(const BermudanSwaptionData& value,
                          BermudanSwaptionData& derivatives,
                          PriceFunc func) {
        // Bumping
        auto eps = 1e-7;
        auto data = value;
        auto v = func(data);

        data.nominal += eps;
        auto vplus = func(data);
        derivatives.nominal = (vplus - v) / eps;
        data = value;

        data.fixedRate += eps;
        vplus = func(data);
        derivatives.fixedRate = (vplus - v) / eps;
        data = value;

        data.forwardRate += eps;
        vplus = func(data);
        derivatives.forwardRate = (vplus - v) / eps;
        data = value;

        data.a += eps;
        vplus = func(data);
        derivatives.a = (vplus - v) / eps;
        data = value;

        data.sigma += eps * .1;
        vplus = func(data);
        derivatives.sigma = (vplus - v) / eps / .1;

        return v;
    }

    template <class PriceFunc>
    Real priceWithAAD(const BermudanSwaptionData& values,
                      BermudanSwaptionData& derivatives,
                      PriceFunc func) {
        // AAD
        using tape_type = Real::tape_type;
        tape_type tape;
        auto data = values;
        tape.registerInput(data.nominal);
        tape.registerInput(data.fixedRate);
        tape.registerInput(data.forwardRate);
        tape.registerInput(data.a);
        tape.registerInput(data.sigma);
        tape.newRecording();

        auto price = func(data);

        tape.registerOutput(price);
        derivative(price) = 1.0;
        tape.computeAdjoints();

        derivatives.nominal = derivative(data.nominal);
        derivatives.fixedRate = derivative(data.fixedRate);
        derivatives.forwardRate = derivative(data.forwardRate);
        derivatives.a = derivative(data.a);
        derivatives.sigma = derivative(data.sigma);

        return price;
    }
}

namespace {
    Real priceBermudanSwaption(const BermudanSwaptionData& value) {
        Integer startYears = 1;
        Integer length = 5;
        Natural settlementDays = 2;
        RelinkableHandle<YieldTermStructure> termStructure;
        termStructure.linkTo(
            flatRate(Date(15, February, 2002), value.forwardRate, Actual365Fixed()));
        auto index = ext::make_shared<Euribor6M>(termStructure);
        Calendar calendar = index->fixingCalendar();
        Date today = calendar.adjust(Date::todaysDate());
        Date settlement = calendar.advance(today, settlementDays, Days);

        Date start = calendar.advance(settlement, startYears, Years);
        Date maturity = calendar.advance(start, length, Years);
        Schedule fixedSchedule(start, maturity, Period(Annual), calendar, Unadjusted, Unadjusted,
                               DateGeneration::Forward, false);
        Schedule floatSchedule(start, maturity, Period(Semiannual), calendar, ModifiedFollowing,
                               ModifiedFollowing, DateGeneration::Forward, false);
        ext::shared_ptr<VanillaSwap> swap(new VanillaSwap(
            value.type, value.nominal, fixedSchedule, value.fixedRate,
            Thirty360(Thirty360::BondBasis), floatSchedule, index, 0.0, index->dayCounter()));
        swap->setPricingEngine(
            ext::shared_ptr<PricingEngine>(new DiscountingSwapEngine(termStructure)));
        auto model = ext::make_shared<HullWhite>(termStructure, value.a, value.sigma);

        std::vector<Date> exerciseDates;
        const Leg& leg = swap->fixedLeg();
        for (const auto& i : leg) {
            auto coupon = ext::dynamic_pointer_cast<Coupon>(i);
            exerciseDates.push_back(coupon->accrualStartDate());
        }
        auto exercise = ext::make_shared<BermudanExercise>(exerciseDates);

        auto treeEngine = ext::make_shared<TreeSwaptionEngine>(model, 50);

        Swaption swaption(swap, exercise);
        swaption.setPricingEngine(treeEngine);

        return swaption.NPV();
    }
}

BOOST_AUTO_TEST_CASE(testBermudanSwaptionDerivatives) {

    SavedSettings save;
    BOOST_TEST_MESSAGE("Testing bermudan swaption derivatives...");

    // input
    auto data = BermudanSwaptionData{Swap::Payer, 1000.00, 0.10, 0.04875825, 0.048696, 0.0058904};

    // bumping
    auto derivatives_bumping = BermudanSwaptionData{};
    auto expected = priceWithBumping(data, derivatives_bumping, priceBermudanSwaption);

    // aad
    auto derivatives_aad = BermudanSwaptionData{};
    auto actual = priceWithAAD(data, derivatives_aad, priceBermudanSwaption);

    // compare
    QL_CHECK_CLOSE(expected, actual, 1e-9);
    if (derivatives_bumping.nominal > 0.1)
        QL_CHECK_CLOSE(derivatives_bumping.nominal, derivatives_aad.nominal, 1e-2);
    else
        QL_CHECK_SMALL(abs(derivatives_aad.nominal - derivatives_bumping.nominal), 1e-3);
    QL_CHECK_CLOSE(derivatives_bumping.fixedRate, derivatives_aad.fixedRate, 1e-3);
    QL_CHECK_CLOSE(derivatives_bumping.forwardRate, derivatives_aad.forwardRate, 1e-3);
    QL_CHECK_CLOSE(derivatives_bumping.a, derivatives_aad.a, 1e-3);
    QL_CHECK_CLOSE(derivatives_bumping.sigma, derivatives_aad.sigma, 1e-3);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE_END()

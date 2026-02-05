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
#include <ql/cashflows/couponpricer.hpp>
#include <ql/cashflows/fixedratecoupon.hpp>
#include <ql/cashflows/iborcoupon.hpp>
#include <ql/currencies/europe.hpp>
#include <ql/indexes/ibor/euribor.hpp>
#include <ql/instruments/vanillaswap.hpp>
#include <ql/pricingengines/swap/discountingswapengine.hpp>
#include <ql/termstructures/volatility/optionlet/constantoptionletvol.hpp>
#include <ql/time/daycounters/simpledaycounter.hpp>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <vector>

using namespace QuantLib;
using namespace boost::unit_test_framework;

BOOST_FIXTURE_TEST_SUITE(QuantLibAADTests, TopLevelFixture)

BOOST_AUTO_TEST_SUITE(SwapXadTests)

namespace {

    struct SwapData {
        VanillaSwap::Type type;
        Real n;       // nominal
        Real s;       // spread
        Rate g;       // gearing
        Volatility v; // volatility
    };

}

namespace {

    template <class PriceFunc>
    Real priceWithBumping(const SwapData& value, SwapData& derivatives, PriceFunc func) {
        // bumping
        auto eps = 1e-7;
        auto data = value;
        auto v = func(data);

        data.n += 1;
        auto vplus = func(data);
        derivatives.n = (vplus - v) / 1;
        data = value;

        data.s += eps;
        vplus = func(data);
        derivatives.s = (vplus - v) / eps;
        data = value;

        data.g += eps;
        vplus = func(data);
        derivatives.g = (vplus - v) / eps;
        data = value;


        data.v += eps;
        vplus = func(data);
        derivatives.v = (vplus - v) / eps;
        data = value;

        return v;
    }

    template <class PriceFunc>
    Real priceWithAAD(const SwapData& values, SwapData& derivatives, PriceFunc func) {
        // AAD
        using tape_type = Real::tape_type;
        tape_type tape;
        auto data = values;

        tape.registerInput(data.n);
        tape.registerInput(data.s);
        tape.registerInput(data.g);
        tape.registerInput(data.v);


        tape.newRecording();

        auto price = func(data);

        tape.registerOutput(price);
        derivative(price) = 1.0;
        tape.computeAdjoints();

        derivatives.n = derivative(data.n);
        derivatives.s = derivative(data.s);
        derivatives.g = derivative(data.g);
        derivatives.v = derivative(data.v);


        return price;
    }
}

namespace {
    Real priceSwap(const SwapData& value) {
        RelinkableHandle<YieldTermStructure> termStructure;
        Calendar calendar = NullCalendar();
        Date today = calendar.adjust(Settings::instance().evaluationDate());
        Date settlement = calendar.advance(today, 2, Days);
        termStructure.linkTo(flatRate(settlement, 0.05, Actual365Fixed()));
        Date maturity = today + 5 * Years;
        Natural fixingDays = 0;

        Schedule schedule(today, maturity, Period(Annual), calendar, Following, Following,
                          DateGeneration::Forward, false);
        DayCounter dayCounter = SimpleDayCounter();
        auto index = ext::make_shared<IborIndex>("dummy", 1 * Years, 0, EURCurrency(), calendar,
                                                 Following, false, dayCounter, termStructure);
        Rate oneYear = 0.05;
        Rate r = std::log(1.0 + oneYear);
        termStructure.linkTo(flatRate(today, r, dayCounter));


        std::vector<Rate> coupons(1, oneYear);
        Leg fixedLeg =
            FixedRateLeg(schedule).withNotionals(value.n).withCouponRates(coupons, dayCounter);

        Handle<OptionletVolatilityStructure> vol(ext::make_shared<ConstantOptionletVolatility>(
            today, NullCalendar(), Following, value.v, dayCounter));
        auto pricer = ext::make_shared<BlackIborCouponPricer>(vol);

        Leg floatingLeg = IborLeg(schedule, index)
                              .withNotionals(value.n)
                              .withPaymentDayCounter(dayCounter)
                              .withFixingDays(fixingDays)
                              .withGearings(value.g)
                              .withSpreads(value.s)
                              .inArrears();
        setCouponPricer(floatingLeg, pricer);

        auto swap = ext::make_shared<Swap>(floatingLeg, fixedLeg);
        swap->setPricingEngine(
            ext::shared_ptr<PricingEngine>(new DiscountingSwapEngine(termStructure)));

        return swap->NPV();
    }
}
BOOST_AUTO_TEST_CASE(testSwapDerivatives) {

    SavedSettings save;
    BOOST_TEST_MESSAGE("Testing European options derivatives...");

    // input
    auto data = SwapData{VanillaSwap::Payer, 1000000.0, -0.001, 0.01, 0.22};

    // Bumping
    auto derivatives_Bumping = SwapData{};
    auto expected = priceWithBumping(data, derivatives_Bumping, priceSwap);

    // aad
    auto derivatives_aad = SwapData{};
    auto actual = priceWithAAD(data, derivatives_aad, priceSwap);

    // compare
    QL_CHECK_CLOSE(expected, actual, 1e-9);
    QL_CHECK_CLOSE(derivatives_Bumping.n, derivatives_aad.n, 1e-3);
    QL_CHECK_CLOSE(derivatives_Bumping.s, derivatives_aad.s, 1e-3);
    QL_CHECK_CLOSE(derivatives_Bumping.g, derivatives_aad.g, 1e-3);
    QL_CHECK_CLOSE(derivatives_Bumping.v, derivatives_aad.v, 1e-3);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE_END()

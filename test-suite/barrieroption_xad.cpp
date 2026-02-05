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
#include <ql/instruments/barrieroption.hpp>
#include <ql/pricingengines/barrier/analyticbarrierengine.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/volatility/equityfx/blackconstantvol.hpp>
#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/time/calendars/nullcalendar.hpp>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <vector>

using namespace QuantLib;
using namespace boost::unit_test_framework;

BOOST_FIXTURE_TEST_SUITE(QuantLibAADTests, TopLevelFixture)

BOOST_AUTO_TEST_SUITE(BarrierOptionXadTest)

namespace {

    struct BarrierOptionData {
        Option::Type type;
        Real strike;
        Real u;       // underlying
        Rate r;       // risk-free rate
        Real b;       // barrier
        Volatility v; // volatility
    };

}
namespace {

    template <class PriceFunc>
    Real priceWithBumping(const BarrierOptionData& value,
                          BarrierOptionData& derivatives,
                          PriceFunc func) {
        // Bumping
        auto eps = 1e-7;
        auto data = value;
        auto v = func(data);

        data.strike += eps;
        auto vplus = func(data);
        derivatives.strike = (vplus - v) / eps;
        data = value;

        data.u += eps;
        vplus = func(data);
        derivatives.u = (vplus - v) / eps;
        data = value;

        data.r += eps;
        vplus = func(data);
        derivatives.r = (vplus - v) / eps;
        data = value;

        data.b += eps;
        vplus = func(data);
        derivatives.b = (vplus - v) / eps;
        data = value;

        data.v += eps;
        vplus = func(data);
        derivatives.v = (vplus - v) / eps;

        return v;
    }

    template <class PriceFunc>
    Real
    priceWithAAD(const BarrierOptionData& values, BarrierOptionData& derivatives, PriceFunc func) {
        // AAD
        using tape_type = Real::tape_type;
        tape_type tape;
        auto data = values;
        tape.registerInput(data.strike);
        tape.registerInput(data.u);
        tape.registerInput(data.r);
        tape.registerInput(data.b);
        tape.registerInput(data.v);
        tape.newRecording();

        auto price = func(data);

        tape.registerOutput(price);
        derivative(price) = 1.0;
        tape.computeAdjoints();

        derivatives.strike = derivative(data.strike);
        derivatives.u = derivative(data.u);
        derivatives.r = derivative(data.r);
        derivatives.b = derivative(data.u);
        derivatives.v = derivative(data.b);

        return price;
    }
}

namespace {
    Real priceBarrierOption(const BarrierOptionData& value) {
        Date today(29, May, 2006);
        Settings::instance().evaluationDate() = today;

        // the option to replicate
        Barrier::Type barrierType = Barrier::DownOut;
        Real barrier = 70.0;
        Real rebate = 0.0;
        Option::Type type = Option::Put;
        Real underlying = 100.0;
        auto underlyingH = ext::make_shared<SimpleQuote>(underlying);
        Real strike = 100.0;
        Real r = 0.04;
        Real v = 0.20;
        auto riskFreeRate = ext::make_shared<SimpleQuote>(r);
        auto volatility = ext::make_shared<SimpleQuote>(v);
        Date maturity = today + 1 * Years;

        DayCounter dayCounter = Actual365Fixed();

        Handle<Quote> h1(riskFreeRate);
        Handle<Quote> h2(volatility);
        Handle<YieldTermStructure> flatRate(
            ext::make_shared<FlatForward>(0, NullCalendar(), h1, dayCounter));
        Handle<BlackVolTermStructure> flatVol(
            ext::make_shared<BlackConstantVol>(0, NullCalendar(), h2, dayCounter));

        // instantiate the option
        auto exercise = ext::make_shared<EuropeanExercise>(maturity);
        auto payoff = ext::make_shared<PlainVanillaPayoff>(type, strike);

        auto bsProcess =
            ext::make_shared<BlackScholesProcess>(Handle<Quote>(underlyingH), flatRate, flatVol);


        /// option
        auto referenceOption =
            ext::make_shared<BarrierOption>(barrierType, barrier, rebate, payoff, exercise);

        referenceOption->setPricingEngine(ext::make_shared<AnalyticBarrierEngine>(bsProcess));

        return referenceOption->NPV();
    }
}

BOOST_AUTO_TEST_CASE(testBarrierOptionDerivatives) {

    SavedSettings save;
    BOOST_TEST_MESSAGE("Testing barrier options derivatives...");

    // input
    auto data = BarrierOptionData{Option::Call, 100.00, 90.00, 0.10, 0.10, 0.10};

    // bumping
    auto derivatives_bumping = BarrierOptionData{};
    auto expected = priceWithBumping(data, derivatives_bumping, priceBarrierOption);

    // aad
    auto derivatives_aad = BarrierOptionData{};
    auto actual = priceWithAAD(data, derivatives_aad, priceBarrierOption);

    // compare
    QL_CHECK_CLOSE(expected, actual, 1e-9);
    QL_CHECK_CLOSE(derivatives_bumping.strike, derivatives_aad.strike, 1e-7);
    QL_CHECK_CLOSE(derivatives_bumping.u, derivatives_aad.u, 1e-7);
    QL_CHECK_CLOSE(derivatives_bumping.r, derivatives_aad.r, 1e-7);
    QL_CHECK_CLOSE(derivatives_bumping.b, derivatives_aad.b, 1e-7);
    QL_CHECK_CLOSE(derivatives_bumping.v, derivatives_aad.v, 1e-7);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE_END()

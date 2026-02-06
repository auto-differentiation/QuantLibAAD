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
#include <ql/instruments/vanillaoption.hpp>
#include <ql/pricingengines/vanilla/baroneadesiwhaleyengine.hpp>
#include <ql/pricingengines/vanilla/bjerksundstenslandengine.hpp>
#include <ql/pricingengines/vanilla/fdblackscholesshoutengine.hpp>
#include <ql/pricingengines/vanilla/fdblackscholesvanillaengine.hpp>
#include <ql/pricingengines/vanilla/juquadraticengine.hpp>
#include <ql/termstructures/volatility/equityfx/blackconstantvol.hpp>
#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/time/daycounters/actual360.hpp>
#include <ql/utilities/dataformatters.hpp>
#include <map>

using namespace QuantLib;
using namespace boost::unit_test_framework;

BOOST_FIXTURE_TEST_SUITE(QuantLibAADTests, TopLevelFixture)

BOOST_AUTO_TEST_SUITE(AmericanOptionXadTests)


namespace {

    struct AmericanOptionData {
        Option::Type type;
        Real strike;
        Real s;       // spot
        Rate q;       // dividend
        Rate r;       // risk-free rate
        Time t;       // time to maturity
        Volatility v; // volatility
    };

}

namespace {

    template <class PriceFunc>
    Real priceWithBumping(const AmericanOptionData& value,
                          AmericanOptionData& derivatives,
                          PriceFunc func) {
        auto eps = 1e-7;
        auto data = value;
        auto v = func(data);

        data.q += eps;
        auto vplus = func(data);
        derivatives.q = (vplus - v) / eps;
        data = value;

        data.r += eps;
        vplus = func(data);
        derivatives.r = (vplus - v) / eps;
        data = value;

        data.s += eps;
        vplus = func(data);
        derivatives.s = (vplus - v) / eps;
        data = value;

        data.strike += eps;
        vplus = func(data);
        derivatives.strike = (vplus - v) / eps;
        data = value;

        data.t += eps;
        vplus = func(data);
        derivatives.t = (vplus - v) / eps;
        data = value;

        data.v += eps;
        vplus = func(data);
        derivatives.v = (vplus - v) / eps;

        return v;
    }

    template <class PriceFunc>
    Real priceWithAAD(const AmericanOptionData& values,
                      AmericanOptionData& derivatives,
                      PriceFunc func) {
        // AAD
        using tape_type = Real::tape_type;
        tape_type tape;
        auto data = values;
        tape.registerInput(data.q);
        tape.registerInput(data.r);
        tape.registerInput(data.s);
        tape.registerInput(data.strike);
        tape.registerInput(data.t);
        tape.registerInput(data.v);
        tape.newRecording();

        auto price = func(data);

        tape.registerOutput(price);
        derivative(price) = 1.0;
        tape.computeAdjoints();

        derivatives.q = derivative(data.q);
        derivatives.r = derivative(data.r);
        derivatives.s = derivative(data.s);
        derivatives.strike = derivative(data.strike);
        derivatives.t = derivative(data.t);
        derivatives.v = derivative(data.v);

        return price;
    }
}

namespace {
    Real priceBaroneAdesiWhaley(const AmericanOptionData& value) {
        Date today = Date::todaysDate();
        DayCounter dc = Actual360();
        auto spot = ext::make_shared<SimpleQuote>(0.0);
        auto qRate = ext::make_shared<SimpleQuote>(0.0);
        ext::shared_ptr<YieldTermStructure> qTS = flatRate(today, qRate, dc);
        auto rRate = ext::make_shared<SimpleQuote>(0.0);
        ext::shared_ptr<YieldTermStructure> rTS = flatRate(today, rRate, dc);
        auto vol = ext::make_shared<SimpleQuote>(0.0);
        ext::shared_ptr<BlackVolTermStructure> volTS = flatVol(today, vol, dc);

        auto payoff = ext::make_shared<PlainVanillaPayoff>(value.type, value.strike);
        Date exDate = today + timeToDays(value.t);
        auto exercise = ext::make_shared<AmericanExercise>(today, exDate);

        spot->setValue(value.s);
        qRate->setValue(value.q);
        rRate->setValue(value.r);
        vol->setValue(value.v);

        auto stochProcess = ext::make_shared<BlackScholesMertonProcess>(
            Handle<Quote>(spot), Handle<YieldTermStructure>(qTS), Handle<YieldTermStructure>(rTS),
            Handle<BlackVolTermStructure>(volTS));

        auto engine = ext::make_shared<BaroneAdesiWhaleyApproximationEngine>(stochProcess);

        VanillaOption option(payoff, exercise);
        option.setPricingEngine(engine);

        return option.NPV();
    }
}

BOOST_AUTO_TEST_CASE(testBaroneAdesiWhaleyValues) {

    BOOST_TEST_MESSAGE("Testing Barone-Adesi and Whaley approximation "
                       "for American options derivatives...");

    // input
    auto data = AmericanOptionData{Option::Call, 100.00, 90.00, 0.10, 0.10, 0.10, 0.15};

    // bump
    auto derivatives_bump = AmericanOptionData{};
    auto expected = priceWithBumping(data, derivatives_bump, priceBaroneAdesiWhaley);

    // aad
    auto derivatives_aad = AmericanOptionData{};
    auto actual = priceWithAAD(data, derivatives_aad, priceBaroneAdesiWhaley);

    // compare
    QL_CHECK_CLOSE(expected, actual, 1e-9);
    QL_CHECK_CLOSE(derivatives_bump.q, derivatives_aad.q, 1e-3);
    QL_CHECK_CLOSE(derivatives_bump.r, derivatives_aad.r, 1e-3);
    QL_CHECK_CLOSE(derivatives_bump.s, derivatives_aad.s, 1e-3);
    QL_CHECK_CLOSE(derivatives_bump.strike, derivatives_aad.strike, 1e-3);
    QL_CHECK_CLOSE(derivatives_bump.t, derivatives_aad.t, 1e-3);
    QL_CHECK_CLOSE(derivatives_bump.v, derivatives_aad.v, 1e-3);
}

namespace {

    Real priceBjerksundStensland(const AmericanOptionData& value) {
        Date today = Date::todaysDate();
        DayCounter dc = Actual360();
        auto spot = ext::make_shared<SimpleQuote>(0.0);
        auto qRate = ext::make_shared<SimpleQuote>(0.0);
        ext::shared_ptr<YieldTermStructure> qTS = flatRate(today, qRate, dc);
        auto rRate = ext::make_shared<SimpleQuote>(0.0);
        ext::shared_ptr<YieldTermStructure> rTS = flatRate(today, rRate, dc);
        auto vol = ext::make_shared<SimpleQuote>(0.0);
        ext::shared_ptr<BlackVolTermStructure> volTS = flatVol(today, vol, dc);

        auto payoff = ext::make_shared<PlainVanillaPayoff>(value.type, value.strike);
        Date exDate = today + timeToDays(value.t);
        auto exercise = ext::make_shared<AmericanExercise>(today, exDate);

        spot->setValue(value.s);
        qRate->setValue(value.q);
        rRate->setValue(value.r);
        vol->setValue(value.v);

        auto stochProcess = ext::make_shared<BlackScholesMertonProcess>(
            Handle<Quote>(spot), Handle<YieldTermStructure>(qTS), Handle<YieldTermStructure>(rTS),
            Handle<BlackVolTermStructure>(volTS));

        auto engine = ext::make_shared<BjerksundStenslandApproximationEngine>(stochProcess);

        VanillaOption option(payoff, exercise);
        option.setPricingEngine(engine);

        return option.NPV();
    }

}

BOOST_AUTO_TEST_CASE(testBjerksundStenslandDerivatives) {
    BOOST_TEST_MESSAGE("Testing Bjerksund and Stensland approximation "
                       "for American options derivatives...");

    // input
    auto data = AmericanOptionData{Option::Call, 40.00, 42.00, 0.08, 0.04, 0.75, 0.35};

    // bump
    auto derivatives_bump = AmericanOptionData{};
    auto expected = priceWithBumping(data, derivatives_bump, priceBjerksundStensland);

    // aad
    auto derivatives_aad = AmericanOptionData{};
    auto actual = priceWithAAD(data, derivatives_aad, priceBjerksundStensland);

    // compare
    QL_CHECK_CLOSE(expected, actual, 1e-9);
    QL_CHECK_CLOSE(derivatives_bump.q, derivatives_aad.q, 1e-4);
    QL_CHECK_CLOSE(derivatives_bump.r, derivatives_aad.r, 1e-4);
    QL_CHECK_CLOSE(derivatives_bump.s, derivatives_aad.s, 1e-4);
    QL_CHECK_CLOSE(derivatives_bump.strike, derivatives_aad.strike, 1e-4);
    QL_CHECK_CLOSE(derivatives_bump.t, derivatives_aad.t, 1e-4);
    QL_CHECK_CLOSE(derivatives_bump.v, derivatives_aad.v, 1e-4);
}

namespace {

    Real priceJu(const AmericanOptionData& juValue) {
        Date today = Date::todaysDate();
        DayCounter dc = Actual360();
        auto spot = ext::make_shared<SimpleQuote>(0.0);
        auto qRate = ext::make_shared<SimpleQuote>(0.0);
        ext::shared_ptr<YieldTermStructure> qTS = flatRate(today, qRate, dc);
        auto rRate = ext::make_shared<SimpleQuote>(0.0);
        ext::shared_ptr<YieldTermStructure> rTS = flatRate(today, rRate, dc);
        auto vol = ext::make_shared<SimpleQuote>(0.0);
        ext::shared_ptr<BlackVolTermStructure> volTS = flatVol(today, vol, dc);

        auto payoff = ext::make_shared<PlainVanillaPayoff>(juValue.type, juValue.strike);
        Date exDate = today + timeToDays(juValue.t);
        ext::shared_ptr<Exercise> exercise = ext::make_shared<AmericanExercise>(today, exDate);

        spot->setValue(juValue.s);
        qRate->setValue(juValue.q);
        rRate->setValue(juValue.r);
        vol->setValue(juValue.v);

        auto stochProcess = ext::make_shared<BlackScholesMertonProcess>(
            Handle<Quote>(spot), Handle<YieldTermStructure>(qTS), Handle<YieldTermStructure>(rTS),
            Handle<BlackVolTermStructure>(volTS));

        auto engine = ext::make_shared<JuQuadraticApproximationEngine>(stochProcess);

        VanillaOption option(payoff, exercise);
        option.setPricingEngine(engine);

        return option.NPV();
    }
}

BOOST_AUTO_TEST_CASE(testJuDerivatives) {
    BOOST_TEST_MESSAGE("Testing Ju approximation for American options derivatives...");

    // input
    auto data = AmericanOptionData{Option::Call, 100.00, 80.00, 0.07, 0.03, 3.0, 0.2};

    // bump
    auto derivatives_bump = AmericanOptionData{};
    auto expected = priceWithBumping(data, derivatives_bump, priceJu);

    // aad
    auto derivatives_aad = AmericanOptionData{};
    auto actual = priceWithAAD(data, derivatives_aad, priceJu);

    // compare
    QL_CHECK_CLOSE(expected, actual, 1e-9);
    QL_CHECK_CLOSE(derivatives_bump.q, derivatives_aad.q, 1e-4);
    QL_CHECK_CLOSE(derivatives_bump.r, derivatives_aad.r, 1e-4);
    QL_CHECK_CLOSE(derivatives_bump.s, derivatives_aad.s, 1e-4);
    QL_CHECK_CLOSE(derivatives_bump.strike, derivatives_aad.strike, 1e-4);
    QL_CHECK_CLOSE(derivatives_bump.t, derivatives_aad.t, 1e-4);
    QL_CHECK_CLOSE(derivatives_bump.v, derivatives_aad.v, 1e-4);
}

namespace {
    Real priceFd(const AmericanOptionData& juValue) {
        Date today = Date::todaysDate();
        DayCounter dc = Actual360();
        auto spot = ext::make_shared<SimpleQuote>(0.0);
        auto qRate = ext::make_shared<SimpleQuote>(0.0);
        ext::shared_ptr<YieldTermStructure> qTS = flatRate(today, qRate, dc);
        auto rRate = ext::make_shared<SimpleQuote>(0.0);
        ext::shared_ptr<YieldTermStructure> rTS = flatRate(today, rRate, dc);
        auto vol = ext::make_shared<SimpleQuote>(0.0);
        ext::shared_ptr<BlackVolTermStructure> volTS = flatVol(today, vol, dc);

        auto payoff = ext::make_shared<PlainVanillaPayoff>(juValue.type, juValue.strike);

        Date exDate = today + timeToDays(juValue.t);
        auto exercise = ext::make_shared<AmericanExercise>(today, exDate);

        spot->setValue(juValue.s);
        qRate->setValue(juValue.q);
        rRate->setValue(juValue.r);
        vol->setValue(juValue.v);

        ext::shared_ptr<BlackScholesMertonProcess> stochProcess =
            ext::make_shared<BlackScholesMertonProcess>(
                Handle<Quote>(spot), Handle<YieldTermStructure>(qTS),
                Handle<YieldTermStructure>(rTS), Handle<BlackVolTermStructure>(volTS));

        ext::shared_ptr<PricingEngine> engine =
            ext::make_shared<FdBlackScholesVanillaEngine>(stochProcess, 100, 100);

        VanillaOption option(payoff, exercise);
        option.setPricingEngine(engine);

        return option.NPV();
    }

}

BOOST_AUTO_TEST_CASE(testFdDerivatives) {
    BOOST_TEST_MESSAGE("Testing finite-difference engine "
                       "for American options derivatives...");

    // input
    auto data = AmericanOptionData{Option::Call, 100.00, 80.00, 0.07, 0.03, 3.0, 0.2};

    // bump
    auto derivatives_bump = AmericanOptionData{};
    auto expected = priceWithBumping(data, derivatives_bump, priceFd);

    // aad
    auto derivatives_aad = AmericanOptionData{};
    auto actual = priceWithAAD(data, derivatives_aad, priceFd);

    // compare
    QL_CHECK_CLOSE(expected, actual, 1e-9);
    QL_CHECK_CLOSE(derivatives_bump.q, derivatives_aad.q, 1e-4);
    QL_CHECK_CLOSE(derivatives_bump.r, derivatives_aad.r, 1e-4);
    QL_CHECK_CLOSE(derivatives_bump.s, derivatives_aad.s, 1e-3);
    QL_CHECK_CLOSE(derivatives_bump.strike, derivatives_aad.strike, 1e-4);
    QL_CHECK_CLOSE(derivatives_bump.t, derivatives_aad.t, 1e-4);
    QL_CHECK_CLOSE(derivatives_bump.v, derivatives_aad.v, 1e-4);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE_END()

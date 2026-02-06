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
#include <ql/pricingengines/vanilla/analyticeuropeanengine.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/volatility/equityfx/blackconstantvol.hpp>
#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/time/calendars/target.hpp>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <vector>

using namespace QuantLib;
using namespace boost::unit_test_framework;

BOOST_FIXTURE_TEST_SUITE(QuantLibAADTests, TopLevelFixture)

BOOST_AUTO_TEST_SUITE(EuropeanOptionXadTests)

namespace {

    struct EuropeanOptionData {
        Option::Type type;
        Real strike;
        Real u;       // underlying
        Rate r;       // risk-free rate
        Real d;       // dividend Yield
        Volatility v; // volatility
    };

}
namespace {

    template <class PriceFunc>
    Real priceWithAnalytics(const EuropeanOptionData& value,
                            EuropeanOptionData& derivatives,
                            PriceFunc func) {
        auto data = value;
        auto v = func(data);

        derivatives.u = v[4];
        derivatives.strike = v[2];
        derivatives.r = v[1];
        derivatives.v = v[3];
        derivatives.d = v[5];

        return v[0];
    }

    template <class PriceFunc>
    Real priceWithAAD(const EuropeanOptionData& values,
                      EuropeanOptionData& derivatives,
                      PriceFunc func) {
        // AAD
        using tape_type = Real::tape_type;
        tape_type tape;
        auto data = values;
        tape.registerInput(data.d);
        tape.registerInput(data.r);
        tape.registerInput(data.strike);
        tape.registerInput(data.u);
        tape.registerInput(data.v);
        tape.newRecording();

        auto price = func(data)[0];

        tape.registerOutput(price);
        derivative(price) = 1.0;
        tape.computeAdjoints();

        derivatives.d = derivative(data.d);
        derivatives.r = derivative(data.r);
        derivatives.strike = derivative(data.strike);
        derivatives.u = derivative(data.u);
        derivatives.v = derivative(data.v);

        return price;
    }
}

namespace {
    std::array<Real, 6> priceEuropeanOption(const EuropeanOptionData& value) {
        // set up dates
        Calendar calendar = TARGET();
        Date todaysDate(15, May, 1998);
        Date settlementDate(17, May, 1998);
        Settings::instance().evaluationDate() = todaysDate;

        // our options
        Option::Type type(Option::Put);
        DayCounter dayCounter = Actual365Fixed();
        Date maturity(17, May, 1999);
        auto exercise = ext::make_shared<EuropeanExercise>(maturity);

        Handle<Quote> underlyingH(ext::make_shared<SimpleQuote>(value.u));

        // bootstrap the yield/dividend/vol curves
        Handle<YieldTermStructure> flatTermStructure(
            ext::make_shared<FlatForward>(settlementDate, value.r, dayCounter));
        Handle<YieldTermStructure> flatDividendTS(
            ext::make_shared<FlatForward>(settlementDate, value.d, dayCounter));
        Handle<BlackVolTermStructure> flatVolTS(
            ext::make_shared<BlackConstantVol>(settlementDate, calendar, value.v, dayCounter));
        auto payoff = ext::make_shared<PlainVanillaPayoff>(type, value.strike);
        auto bsmProcess = ext::make_shared<BlackScholesMertonProcess>(underlyingH, flatDividendTS,
                                                                      flatTermStructure, flatVolTS);

        // option
        auto european = ext::make_shared<VanillaOption>(payoff, exercise);
        // computing the option price with the analytic Black-Scholes formulae
        european->setPricingEngine(ext::make_shared<AnalyticEuropeanEngine>(bsmProcess));
        std::array<Real, 6> f_array = {
            european->NPV(),  european->rho(),   european->strikeSensitivity(),
            european->vega(), european->delta(), european->dividendRho()};
        return f_array;
    }
}

BOOST_AUTO_TEST_CASE(testEuropeanOptionDerivatives) {

    SavedSettings save;
    BOOST_TEST_MESSAGE("Testing European options derivatives...");

    // input
    auto data = EuropeanOptionData{Option::Call, 100.00, 90.00, 0.10, 0.10, 0.10};

    // analytics
    auto derivatives_analytics = EuropeanOptionData{};
    auto expected = priceWithAnalytics(data, derivatives_analytics, priceEuropeanOption);

    // aad
    auto derivatives_aad = EuropeanOptionData{};
    auto actual = priceWithAAD(data, derivatives_aad, priceEuropeanOption);

    // compare
    QL_CHECK_CLOSE(expected, actual, 1e-9);
    QL_CHECK_CLOSE(derivatives_analytics.d, derivatives_aad.d, 1e-7);
    QL_CHECK_CLOSE(derivatives_analytics.r, derivatives_aad.r, 1e-7);
    QL_CHECK_CLOSE(derivatives_analytics.u, derivatives_aad.u, 1e-7);
    QL_CHECK_CLOSE(derivatives_analytics.strike, derivatives_aad.strike, 1e-7);
    QL_CHECK_CLOSE(derivatives_analytics.v, derivatives_aad.v, 1e-7);
}


BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE_END()

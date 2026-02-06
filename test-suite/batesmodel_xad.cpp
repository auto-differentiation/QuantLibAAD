/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
 Copyright (C) 2005, 2008 Klaus Spanderen
 Copyright (C) 2007 StatPro Italia srl
 Copyright (C) 2022 Xcelerit

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
#include <ql/instruments/europeanoption.hpp>
#include <ql/math/optimization/levenbergmarquardt.hpp>
#include <ql/models/equity/batesmodel.hpp>
#include <ql/models/equity/hestonmodelhelper.hpp>
#include <ql/pricingengines/blackformula.hpp>
#include <ql/pricingengines/vanilla/analyticeuropeanengine.hpp>
#include <ql/pricingengines/vanilla/batesengine.hpp>
#include <ql/pricingengines/vanilla/fdbatesvanillaengine.hpp>
#include <ql/pricingengines/vanilla/jumpdiffusionengine.hpp>
#include <ql/pricingengines/vanilla/mceuropeanhestonengine.hpp>
#include <ql/processes/batesprocess.hpp>
#include <ql/processes/merton76process.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/termstructures/yield/zerocurve.hpp>
#include <ql/time/calendars/target.hpp>
#include <ql/time/daycounters/actualactual.hpp>
#include <ql/time/period.hpp>

using namespace QuantLib;
using namespace boost::unit_test_framework;

BOOST_FIXTURE_TEST_SUITE(QuantLibAADTests, TopLevelFixture)

BOOST_AUTO_TEST_SUITE(BatesModelXadTests)

namespace {

    // This is a copy of the bates pricing from the main test suite,
    // separated as a function of independent variables to allow bump vs aad tests
    Real priceBatesModel(Real riskFreeRate, Real dividendRate, Real strike) {
        Date settlementDate = Date::todaysDate();
        Settings::instance().evaluationDate() = settlementDate;

        DayCounter dayCounter = ActualActual(ActualActual::ISDA);
        Date exerciseDate = settlementDate + 6 * Months;

        auto payoff = ext::make_shared<PlainVanillaPayoff>(Option::Put, 30);
        auto exercise = ext::make_shared<EuropeanExercise>(exerciseDate);

        Handle<YieldTermStructure> riskFreeTS(flatRate(riskFreeRate, dayCounter));
        Handle<YieldTermStructure> dividendTS(flatRate(dividendRate, dayCounter));
        Handle<Quote> s0(ext::make_shared<SimpleQuote>(strike));

        Real yearFraction = dayCounter.yearFraction(settlementDate, exerciseDate);
        Real forwardPrice = s0->value() * std::exp((0.1 - 0.04) * yearFraction);

        const Real v0 = 0.05;
        const Real kappa = 5.0;
        const Real theta = 0.05;
        const Real sigma = 1.0e-4;
        const Real rho = 0.0;
        const Real lambda = 0.0001;
        const Real nu = 0.0;
        const Real delta = 0.0001;

        VanillaOption option(payoff, exercise);

        auto process = ext::make_shared<BatesProcess>(riskFreeTS, dividendTS, s0, v0, kappa, theta,
                                                      sigma, rho, lambda, nu, delta);

        auto engine = ext::make_shared<BatesEngine>(ext::make_shared<BatesModel>(process), 64);

        option.setPricingEngine(engine);
        return option.NPV();
    }

    Real priceBatesModelBumping(Real riskFreeRate,
                                Real dividendRate,
                                Real strike,
                                std::vector<Real>& der) {
        Real eps = 1e-7;
        auto v = priceBatesModel(riskFreeRate, dividendRate, strike);
        auto vplus = priceBatesModel(riskFreeRate + eps, dividendRate, strike);
        der.push_back((vplus - v) / eps);
        vplus = priceBatesModel(riskFreeRate, dividendRate + eps, strike);
        der.push_back((vplus - v) / eps);
        vplus = priceBatesModel(riskFreeRate, dividendRate, strike + eps);
        der.push_back((vplus - v) / eps);
        return v;
    }
}

BOOST_AUTO_TEST_CASE(testBatesModelDerivatives) {
    BOOST_TEST_MESSAGE("Testing Bates Model derivatives...");

    SavedSettings backup;

    Real riskFreeRate = 0.1;
    Real dividendRate = 0.04;
    Real strike = 32.0;

    // bumping
    std::vector<Real> gradient_bump;
    auto expected = priceBatesModelBumping(riskFreeRate, dividendRate, strike, gradient_bump);

    // AAD
    using tape_type = QuantLib::Real::tape_type;
    tape_type tape;

    tape.registerInput(riskFreeRate);
    tape.registerInput(dividendRate);
    tape.registerInput(strike);
    tape.newRecording();

    Real price = priceBatesModel(riskFreeRate, dividendRate, strike);

    tape.registerOutput(price);
    derivative(price) = 1.0;
    tape.computeAdjoints();

    // compare
    QL_CHECK_CLOSE(expected, price, 1e-9);
    QL_CHECK_CLOSE(gradient_bump[0], derivative(riskFreeRate), 1e-4);
    QL_CHECK_CLOSE(gradient_bump[1], derivative(dividendRate), 1e-4);
    QL_CHECK_CLOSE(gradient_bump[2], derivative(strike), 1e-4);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE_END()

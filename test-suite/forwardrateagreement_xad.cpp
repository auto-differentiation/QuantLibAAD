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
#include <ql/indexes/ibor/usdlibor.hpp>
#include <ql/instruments/forwardrateagreement.hpp>
#include <ql/termstructures/yield/zerocurve.hpp>
#include <ql/time/calendars/target.hpp>
#include <ql/time/calendars/unitedstates.hpp>
#include <ql/time/daycounters/actual360.hpp>
#include <ql/time/period.hpp>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <vector>

using namespace QuantLib;
using namespace boost::unit_test_framework;

BOOST_FIXTURE_TEST_SUITE(QuantLibAADTests, TopLevelFixture)

BOOST_AUTO_TEST_SUITE(ForwardRateAgreementXadTests)

namespace {

    struct ForwardRateAgreementData {
        Real nominal;
        Real spotRate1;
        Real spotRate2;
        Real spotRate3;
        Real rate;
    };

}

namespace {

    template <class PriceFunc>
    Real priceWithBumping(const ForwardRateAgreementData& value,
                          ForwardRateAgreementData& derivatives,
                          PriceFunc func) {
        // Bumping
        auto eps = 1e-7;
        auto data = value;
        auto v = func(data);

        data.nominal += 1;
        auto vplus = func(data);
        derivatives.nominal = (vplus - v) / 1;
        data = value;

        data.spotRate1 += eps;
        vplus = func(data);
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

        data.rate += eps;
        vplus = func(data);
        derivatives.rate = (vplus - v) / eps;

        return v;
    }

    template <class PriceFunc>
    Real priceWithAAD(const ForwardRateAgreementData& values,
                      ForwardRateAgreementData& derivatives,
                      PriceFunc func) {
        // AAD
        using tape_type = Real::tape_type;
        tape_type tape;
        auto data = values;
        tape.registerInput(data.nominal);
        tape.registerInput(data.spotRate1);
        tape.registerInput(data.spotRate2);
        tape.registerInput(data.spotRate3);
        tape.registerInput(data.rate);
        tape.newRecording();

        auto price = func(data);

        tape.registerOutput(price);
        derivative(price) = 1.0;
        tape.computeAdjoints();

        derivatives.nominal = derivative(data.nominal);
        derivatives.spotRate1 = derivative(data.spotRate1);
        derivatives.spotRate2 = derivative(data.spotRate2);
        derivatives.spotRate3 = derivative(data.spotRate3);
        derivatives.rate = derivative(data.rate);

        return price;
    }
}

namespace {
    Real priceForwardRateAgreement(const ForwardRateAgreementData& value) {
        Date today(30, June, 2020);
        Settings::instance().evaluationDate() = today;

        std::vector<Date> spotDates;
        spotDates.push_back(Date(30, June, 2020));
        spotDates.push_back(Date(31, December, 2020));
        spotDates.push_back(Date(30, June, 2021));
        std::vector<Real> spotRates;
        spotRates.push_back(value.spotRate1);
        spotRates.push_back(value.spotRate2);
        spotRates.push_back(value.spotRate3);

        DayCounter dayConvention = Actual360();
        Calendar calendar = UnitedStates(UnitedStates::GovernmentBond);
        Date startDate = calendar.advance(today, Period(3, Months));
        Date maturityDate = calendar.advance(startDate, Period(3, Months));

        Compounding compounding = Simple;
        Frequency compoundingFrequency = Annual;

        Handle<YieldTermStructure> spotCurve(
            ext::make_shared<ZeroCurve>(spotDates, spotRates, dayConvention, calendar, Linear(),
                                        compounding, compoundingFrequency));
        spotCurve->enableExtrapolation();

        auto index = ext::make_shared<USDLibor>(Period(3, Months), spotCurve);
        index->addFixing(Date(26, June, 2020), 0.05);

        ForwardRateAgreement fra(index, startDate, maturityDate, Position::Long, value.rate,
                                 value.nominal, spotCurve);

        return fra.NPV();
    }
}

BOOST_AUTO_TEST_CASE(testForwardRateAgreementDerivatives) {

    SavedSettings save;
    BOOST_TEST_MESSAGE("Testing forward rate agreement derivatives...");

    // input
    auto data = ForwardRateAgreementData{100000, 0.5, 0.5, 0.5, 0.06};

    // bumping
    auto derivatives_bumping = ForwardRateAgreementData{};
    auto expected = priceWithBumping(data, derivatives_bumping, priceForwardRateAgreement);

    // aad
    auto derivatives_aad = ForwardRateAgreementData{};
    auto actual = priceWithAAD(data, derivatives_aad, priceForwardRateAgreement);

    // compare
    QL_CHECK_CLOSE(expected, actual, 1e-9);
    QL_CHECK_CLOSE(derivatives_bumping.nominal, derivatives_aad.nominal, 1e-3);
    QL_CHECK_CLOSE(derivatives_bumping.spotRate1, derivatives_aad.spotRate1, 1e-3);
    QL_CHECK_CLOSE(derivatives_bumping.spotRate2, derivatives_aad.spotRate2, 1e-3);
    QL_CHECK_CLOSE(derivatives_bumping.spotRate3, derivatives_aad.spotRate3, 1e-3);
    QL_CHECK_CLOSE(derivatives_bumping.rate, derivatives_aad.rate, 1e-3);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE_END()

/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*!
 Copyright (C) 2008 Jose Aparicio
 Copyright (C) 2014 Peter Caspers
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

/*
This example shows how to use XAD to price a CDS with sensitivities.
*/

#include <ql/qldefines.hpp>
#if !defined(BOOST_ALL_NO_LIB) && defined(BOOST_MSVC)
#    include <ql/auto_link.hpp>
#endif
#include <ql/exercise.hpp>
#include <ql/math/interpolations/backwardflatinterpolation.hpp>
#include <ql/pricingengines/credit/midpointcdsengine.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/credit/defaultprobabilityhelpers.hpp>
#include <ql/termstructures/credit/flathazardrate.hpp>
#include <ql/termstructures/credit/interpolatedhazardratecurve.hpp>
#include <ql/termstructures/credit/piecewisedefaultcurve.hpp>
#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/termstructures/yield/zerocurve.hpp>
#include <ql/time/calendars/target.hpp>
#include <ql/time/daycounters/actual360.hpp>
#include <iomanip>
#include <iostream>
#include <vector>

using namespace std;
using namespace QuantLib;


Real priceCDS(const std::vector<Real>& hazardRates,
              const std::vector<Date>& dates,
              const std::vector<Real>& riskFreeRates,
              Date issueDate,
              Date maturity,
              Real recoveryRate,
              Real fixedRate,
              Calendar& calendar,
              const DayCounter& dayCount,
              Real notional) {
    RelinkableHandle<DefaultProbabilityTermStructure> probabilityCurve;
    auto hazardCurve =
        ext::make_shared<InterpolatedHazardRateCurve<BackwardFlat>>(dates, hazardRates, dayCount);
    probabilityCurve.linkTo(hazardCurve);

    RelinkableHandle<YieldTermStructure> discountCurve;

    discountCurve.linkTo(ext::make_shared<ZeroCurve>(dates, riskFreeRates, dayCount));

    Frequency frequency = Semiannual;
    BusinessDayConvention convention = ModifiedFollowing;

    Schedule schedule(issueDate, maturity, Period(frequency), calendar, convention, convention,
                      DateGeneration::Forward, false);

    auto cds = ext::make_shared<CreditDefaultSwap>(Protection::Seller, notional, fixedRate,
                                                   schedule, convention, dayCount, true, true);
    cds->setPricingEngine(
        ext::make_shared<MidPointCdsEngine>(probabilityCurve, recoveryRate, discountCurve));
    return cds->NPV();
}

// Sensitivities with XAD
#ifndef QLAAD_DISABLE_AAD

// create tape
using tape_type = Real::tape_type;
tape_type tape;


Real priceWithSensi(const std::vector<Real>& hazardRates,
                    std::vector<Date>& dates,
                    const std::vector<Real>& riskFreeRate,
                    Date issueDate,
                    Date& maturity,
                    Real recoveryRate,
                    Real fixedRate,
                    Calendar& calendar,
                    DayCounter& dayCount,
                    Real notional,
                    std::vector<Real>& gradient) {
    // register the independent inputs
    auto riskFreeRates_t = riskFreeRate;
    auto hazardRates_t = hazardRates;
    tape.registerInputs(riskFreeRates_t);
    tape.registerInputs(hazardRates_t);
    tape.registerInput(recoveryRate);
    tape.registerInput(fixedRate);
    tape.registerInput(notional);
    tape.newRecording();

    Real value = priceCDS(hazardRates_t, dates, riskFreeRates_t, issueDate, maturity, recoveryRate,
                          fixedRate, calendar, dayCount, notional);

    // register dependent variables and roll back the adjoints
    tape.registerOutput(value);
    derivative(value) = 1.0;
    tape.computeAdjoints();

    for (auto& r : riskFreeRates_t) {
        gradient.push_back(derivative(r));
    }
    for (auto& a : hazardRates_t) {
        gradient.push_back(derivative(a));
    }
    gradient.push_back(derivative(recoveryRate));
    gradient.push_back(derivative(fixedRate));
    gradient.push_back(derivative(notional));

    return value;
}

#endif

void printResults(Real v, const std::vector<Real>& gradient) {
    std::cout << "CDS value           = " << v << "\n";
    std::cout << "Rhos                = "
              << "[";
    for (int i = 0; i < 9; ++i)
        std::cout << gradient[i] << ", ";
    std::cout << "]\n";
    std::cout << "Hazard rate sensitivities = [";
    for (int i = 9; i < 17; ++i)
        std::cout << gradient[i] << ", ";
    std::cout << "]\n";
    std::cout << "Recovery rate sensitivity = " << gradient[17] << "\n";
    std::cout << "Fixed rate sensitivity    = " << gradient[18] << "\n";
    std::cout << "Notional sensitivity      = " << gradient[19] << "\n";
}

int main() {
    try {
        DayCounter dayCount = Actual360();
        // Initialize curves
        Settings::instance().evaluationDate() = Date(9, June, 2006);
        Date today = Settings::instance().evaluationDate();
        Calendar calendar = TARGET();

        Date issueDate = calendar.advance(today, -1, Years);
        Date maturity = calendar.advance(issueDate, 2, Years);


        // Build the CDS
        Rate fixedRate = 0.0120;
        Real notional = 10000.0;
        Real recoveryRate = 0.4;
        std::vector<Integer> tn = {13, 41, 75, 165, 256, 345, 524, 703};
        std::vector<Rate> riskFreeRate = {0.0357, 0.0357, 0.0349, 0.0341, 0.0355,
                                          0.0359, 0.0368, 0.0386, 0.0401};
        std::vector<Rate> hazardRates = {0.0,     0.00234, 0.042,   0.0064, 0.00734,
                                         0.00934, 0.012,   0.01234, 0.01634};

        std::vector<Date> dates;
        dates.push_back(today);
        Size i;
        for (i = 0; i < tn.size(); ++i) {
            dates.push_back(today + tn[i]);
        }

#ifdef QLAAD_DISABLE_AAD
        std::cout << "Pricing a CDS without sensitivities...\n";
        Real v = priceCDS(hazardRates, dates, riskFreeRate, issueDate, maturity, recoveryRate,
                          fixedRate, calendar, dayCount, notional);
        std::cout << "CDS value: " << v << "\n";
#else
        std::vector<Real> gradient;
        std::cout << "Pricing a CDS with sensitivities...\n";
        Real v = priceWithSensi(hazardRates, dates, riskFreeRate, issueDate, maturity,
                                recoveryRate, fixedRate, calendar, dayCount, notional, gradient);
        printResults(v, gradient);
#endif

        return 0;
    } catch (std::exception& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "unknown error" << std::endl;
        return 1;
    }
}
// /* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

// /*
//  Copyright (C) 2023-2026 Xcelerit Computing Limited

//  This file is part of QuantLib / XAD integration module.
//  It is modified from QuantLib, a free-software/open-source library
//  for financial quantitative analysts and developers - http://quantlib.org/

//  QuantLib is free software: you can redistribute it and/or modify it
//  under the terms of the QuantLib license.  You should have received a
//  copy of the license along with this program; if not, please email
//  <quantlib-dev@lists.sf.net>. The license is also available online at
//  <http://quantlib.org/license.shtml>.

//  This program is distributed in the hope that it will be useful, but WITHOUT
//  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
//  FOR A PARTICULAR PURPOSE.  See the license for more details.
// */

#include "toplevelfixture.hpp"
#include "utilities_xad.hpp"
#include <ql/math/optimization/levenbergmarquardt.hpp>
#include <ql/models/equity/hestonmodel.hpp>
#include <ql/models/equity/hestonmodelhelper.hpp>
#include <ql/models/equity/piecewisetimedependenthestonmodel.hpp>
#include <ql/pricingengines/vanilla/analytichestonengine.hpp>
#include <ql/pricingengines/vanilla/coshestonengine.hpp>
#include <ql/pricingengines/vanilla/fdhestonvanillaengine.hpp>
#include <ql/processes/hestonprocess.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/termstructures/yield/zerocurve.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>
#include <ql/time/calendars/nullcalendar.hpp>
#include <ql/time/calendars/target.hpp>
#include <ql/time/daycounters/actual360.hpp>
#include <ql/time/daycounters/actualactual.hpp>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <vector>

using namespace QuantLib;
using namespace boost::unit_test_framework;

BOOST_FIXTURE_TEST_SUITE(QuantLibAADTests, TopLevelFixture)

BOOST_AUTO_TEST_SUITE(HestonModelXadTest)

namespace {

    struct ModelData {
        std::vector<Date> dates;
        std::vector<Real> rates;
        DayCounter dayCounter;
        Calendar calendar;
        std::vector<Integer> t;
        std::vector<Real> v;
        std::vector<Real> strike;
        Handle<Quote> s0;
        Date settlementDate;
    };

}
namespace {

    template <class PriceFunc>
    Real priceWithBumping(ext::shared_ptr<QuantLib::HestonModel>& model,
                          const ModelData& value,
                          std::vector<Real>& derivatives_rates,
                          std::vector<Real>& derivatives_v,
                          std::vector<Real>& derivatives_strike,
                          PriceFunc func) {
        // Bumping
        auto eps = 1e-7;
        auto data = value;
        auto v = func(model, data);

        for (std::size_t i = 0; i < data.strike.size(); ++i) {
            data.strike[i] += 1;
            auto vplus = func(model, data);
            derivatives_strike.push_back((vplus - v) / 1);
            data = value;
        }
        for (std::size_t i = 0; i < data.v.size(); ++i) {
            data.v[i] += eps;
            auto vplus = func(model, data);
            derivatives_v.push_back((vplus - v) / eps);
            data = value;
        }
        for (std::size_t i = 0; i < data.rates.size(); ++i) {
            data.rates[i] += eps;
            auto vplus = func(model, data);
            derivatives_rates.push_back((vplus - v) / eps);
            data = value;
        }

        return v;
    }
    template <class PriceFunc>
    Real priceWithAAD(ext::shared_ptr<QuantLib::HestonModel>& model,
                      const ModelData& value,
                      std::vector<Real>& derivatives_v,
                      std::vector<Real>& derivatives_strikes,
                      std::vector<Real>& derivatives_rates,
                      PriceFunc func) {
        // AAD
        using tape_type = Real::tape_type;
        tape_type tape;

        std::vector<Real> rates = value.rates;
        std::vector<Real> volatilities = value.v;
        std::vector<Real> strikes = value.strike;
        tape.registerInputs(rates);
        tape.registerInputs(strikes);
        tape.registerInputs(volatilities);
        tape.newRecording();

        Real v = func(model, value);

        // get the values
        tape.registerOutput(v);
        derivative(v) = 1.0;
        tape.computeAdjoints();

        std::transform(rates.begin(), rates.end(), std::back_inserter(derivatives_rates),
                       [](const Real& q) { return derivative(q); });
        std::transform(strikes.begin(), strikes.end(), std::back_inserter(derivatives_strikes),
                       [](const Real& q) { return derivative(q); });
        std::transform(volatilities.begin(), volatilities.end(), std::back_inserter(derivatives_v),
                       [](const Real& q) { return derivative(q); });
        return v;
    }
}
namespace {

    struct CalibrationMarketData {
        Handle<Quote> s0;
        Handle<YieldTermStructure> riskFreeTS, dividendYield;
        std::vector<ext::shared_ptr<CalibrationHelper>> options;
    };

    CalibrationMarketData getDAXCalibrationMarketData(const ModelData& value) {
        // FLOATING_POINT_EXCEPTION
        Handle<YieldTermStructure> riskFreeTS(
            ext::make_shared<ZeroCurve>(value.dates, value.rates, value.dayCounter));

        Handle<YieldTermStructure> dividendYield(ext::make_shared<FlatForward>(
            0, NullCalendar(), Handle<Quote>(ext::make_shared<SimpleQuote>(0.0)),
            value.dayCounter));

        std::vector<ext::shared_ptr<CalibrationHelper>> options;

        for (Size s = 0; s < 13; ++s) {
            for (Size m = 0; m < 8; ++m) {
                Handle<Quote> vol(ext::make_shared<SimpleQuote>(value.v[s * 8 + m]));

                Period maturity((int)((value.t[m] + 3) / 7.), Weeks); // round to weeks
                options.push_back(ext::make_shared<HestonModelHelper>(
                    maturity, value.calendar, value.s0, value.strike[s], vol, riskFreeTS,
                    dividendYield, BlackCalibrationHelper::ImpliedVolError));
            }
        }

        CalibrationMarketData marketData = {value.s0, riskFreeTS, dividendYield, options};

        return marketData;
    }

}

ext::shared_ptr<QuantLib::HestonModel> HestonModelCalibration(const ModelData& value) {
    CalibrationMarketData marketData = getDAXCalibrationMarketData(value);

    const Handle<YieldTermStructure> riskFreeTS = marketData.riskFreeTS;
    const Handle<YieldTermStructure> dividendTS = marketData.dividendYield;
    const Handle<Quote> S0 = marketData.s0;

    const std::vector<ext::shared_ptr<CalibrationHelper>>& options = marketData.options;

    const Real v0 = 0.5;
    const Real kappa = 1.0;
    const Real theta = 0.1;
    const Real sigma = 0.5;
    const Real rho = -0.0;

    const ext::shared_ptr<HestonModel> model = ext::make_shared<HestonModel>(
        ext::make_shared<HestonProcess>(riskFreeTS, dividendTS, S0, v0, kappa, theta, sigma, rho));

    auto engine = ext::make_shared<COSHestonEngine>(model);

    const Array params = model->params();
    model->setParams(params);
    for (const auto& option : options)
        ext::dynamic_pointer_cast<BlackCalibrationHelper>(option)->setPricingEngine(engine);

    LevenbergMarquardt om(1e-8, 1e-8, 1e-8);
    model->calibrate(options, om, EndCriteria(400, 40, 1.0e-8, 1.0e-8, 1.0e-8));

    return model;
}

Real priceHestonModel(ext::shared_ptr<QuantLib::HestonModel>& model, const ModelData& value) {
    const Date maturityDate = value.settlementDate + Period(1, Years);

    const ext::shared_ptr<Exercise> exercise = ext::make_shared<EuropeanExercise>(maturityDate);

    const ext::shared_ptr<PricingEngine> cosEngine(
        ext::make_shared<COSHestonEngine>(model, 25, 600));
    CalibrationMarketData marketData = getDAXCalibrationMarketData(value);

    const Handle<Quote> S0 = marketData.s0;

    auto payoff = ext::make_shared<PlainVanillaPayoff>(Option::Call, S0->value() + 20);

    VanillaOption option(payoff, exercise);

    option.setPricingEngine(cosEngine);

    return option.NPV();
}


void printResults(Real v, const std::vector<Real>& derivatives) {


    for (std::size_t i = 0; i < derivatives.size(); ++i)
        std::cout << "derivatives " << i << " = " << derivatives[i] << "\n";
}

BOOST_AUTO_TEST_CASE(testHestonModelDerivatives) {
    Date settlementDate(16, September, 2015);
    Settings::instance().evaluationDate() = settlementDate;

    DayCounter dayCounter = Actual365Fixed();
    Calendar calendar = TARGET();

    std::vector<Integer> t = {13, 41, 75, 165, 256, 345, 524, 703};
    std::vector<Rate> r = {0.0357, 0.0349, 0.0341, 0.0355, 0.0359, 0.0368, 0.0386, 0.0401};


    std::vector<Date> dates;
    std::vector<Rate> rates;
    dates.push_back(settlementDate);
    rates.push_back(0.0357);
    Size i;
    for (i = 0; i < 8; ++i) {
        dates.push_back(settlementDate + t[i]);
        rates.push_back(r[i]);
    }
    std::vector<Volatility> v = {
        0.6625, 0.4875, 0.4204, 0.3667, 0.3431, 0.3267, 0.3121, 0.3121, 0.6007, 0.4543, 0.3967,
        0.3511, 0.3279, 0.3154, 0.2984, 0.2921, 0.5084, 0.4221, 0.3718, 0.3327, 0.3155, 0.3027,
        0.2919, 0.2889, 0.4541, 0.3869, 0.3492, 0.3149, 0.2963, 0.2926, 0.2819, 0.2800, 0.4060,
        0.3607, 0.3330, 0.2999, 0.2887, 0.2811, 0.2751, 0.2775, 0.3726, 0.3396, 0.3108, 0.2781,
        0.2788, 0.2722, 0.2661, 0.2686, 0.3550, 0.3277, 0.3012, 0.2781, 0.2781, 0.2661, 0.2661,
        0.2681, 0.3428, 0.3209, 0.2958, 0.2740, 0.2688, 0.2627, 0.2580, 0.2620, 0.3302, 0.3062,
        0.2799, 0.2631, 0.2573, 0.2533, 0.2504, 0.2544, 0.3343, 0.2959, 0.2705, 0.2540, 0.2504,
        0.2464, 0.2448, 0.2462, 0.3460, 0.2845, 0.2624, 0.2463, 0.2425, 0.2385, 0.2373, 0.2422,
        0.3857, 0.2860, 0.2578, 0.2399, 0.2357, 0.2327, 0.2312, 0.2351, 0.3976, 0.2860, 0.2607,
        0.2356, 0.2297, 0.2268, 0.2241, 0.2320};

    Handle<Quote> s0(ext::make_shared<SimpleQuote>(4468.17));
    std::vector<Real> strike = {3400, 3600, 3800, 4000, 4200, 4400, 4500,
                                4600, 4800, 5000, 5200, 5400, 5600};

    SavedSettings save;
    BOOST_TEST_MESSAGE("Testing Heston model derivatives...");

    // input
    auto data = ModelData{dates, rates, dayCounter, calendar, t, v, strike, s0, settlementDate};
    auto model = HestonModelCalibration(data);

    std::cout.precision(15);

    // bumping
    std::vector<Real> derivatives_bumping_v;
    std::vector<Real> derivatives_bumping_rates;
    std::vector<Real> derivatives_bumping_strikes;


    auto expected = priceWithBumping(model, data, derivatives_bumping_rates, derivatives_bumping_v,
                                     derivatives_bumping_strikes, priceHestonModel);

    // aad
    std::vector<Real> derivatives_aad_v;
    std::vector<Real> derivatives_aad_rates;
    std::vector<Real> derivatives_aad_strikes;

    auto actual = priceWithAAD(model, data, derivatives_aad_v, derivatives_aad_strikes,
                               derivatives_aad_rates, priceHestonModel);

    // compare
    QL_CHECK_CLOSE(expected, actual, 1e-9);
    for (std::size_t i = 0; i < derivatives_aad_v.size(); ++i) {
        QL_CHECK_CLOSE(derivatives_bumping_v[i], derivatives_aad_v[i], 1e-3);
    }
    for (std::size_t i = 0; i < derivatives_aad_strikes.size(); ++i) {
        QL_CHECK_CLOSE(derivatives_bumping_strikes[i], derivatives_aad_strikes[i], 1e-3);
    }
    for (std::size_t i = 0; i < derivatives_aad_rates.size(); ++i) {
        QL_CHECK_CLOSE(derivatives_bumping_rates[i], derivatives_aad_rates[i], 1e-3);
    }
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE_END()
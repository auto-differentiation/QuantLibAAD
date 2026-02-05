/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
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
This example uses XAD to calculate sensitivities of a Heston pricer.

TODO: Use the implicit function theorem for calibration with AAD.
*/

#include <ql/qldefines.hpp>
#if !defined(BOOST_ALL_NO_LIB) && defined(BOOST_MSVC)
#    include <ql/auto_link.hpp>
#endif
#include <ql/exercise.hpp>
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


struct CalibrationMarketData {
    Handle<Quote> s0;
    Handle<YieldTermStructure> riskFreeTS, dividendYield;
    std::vector<ext::shared_ptr<CalibrationHelper>> options;
};

CalibrationMarketData getDAXCalibrationMarketData(const std::vector<Date>& dates,
                                                  const std::vector<Rate>& rates,
                                                  const std::vector<Real>& dividendYields,
                                                  const DayCounter& dayCounter,
                                                  const Calendar& calendar,
                                                  const std::vector<Integer>& t,
                                                  const std::vector<Volatility>& v,
                                                  const std::vector<Real>& strike,
                                                  const Handle<Quote>& s0) {
    Handle<YieldTermStructure> riskFreeTS(ext::make_shared<ZeroCurve>(dates, rates, dayCounter));

    Handle<YieldTermStructure> dividendYield(
        ext::make_shared<ZeroCurve>(dates, dividendYields, dayCounter));

    std::vector<ext::shared_ptr<CalibrationHelper>> options;

    for (Size s = 0; s < 13; ++s) {
        for (Size m = 0; m < 8; ++m) {
            Handle<Quote> vol(ext::make_shared<SimpleQuote>(v[s * 8 + m]));

            Period maturity((int)((t[m] + 3) / 7.), Weeks); // round to weeks
            options.push_back(ext::make_shared<HestonModelHelper>(
                maturity, calendar, s0, strike[s], vol, riskFreeTS, dividendYield,
                BlackCalibrationHelper::ImpliedVolError));
        }
    }

    return {s0, riskFreeTS, dividendYield, options};
}

ext::shared_ptr<QuantLib::HestonModel>
HestonModelCalibration(const std::vector<Date>& dates,
                       const std::vector<Rate>& rates,
                       const std::vector<Real>& dividendYield,
                       const DayCounter& dayCounter,
                       const Calendar& calendar,
                       const std::vector<Integer>& t,
                       const std::vector<Volatility>& v,
                       const std::vector<Real>& strike,
                       const Handle<Quote>& s0,
                       Date settlementDate) {
    CalibrationMarketData marketData = getDAXCalibrationMarketData(
        dates, rates, dividendYield, dayCounter, calendar, t, v, strike, s0);

    const Handle<YieldTermStructure> riskFreeTS = marketData.riskFreeTS;
    const Handle<YieldTermStructure> dividendTS = marketData.dividendYield;
    const Handle<Quote> S0 = marketData.s0;

    const std::vector<ext::shared_ptr<CalibrationHelper>>& options = marketData.options;

    const Real v0 = 0.5;
    const Real kappa = 1.0;
    const Real theta = 0.1;
    const Real sigma = 0.5;
    const Real rho = -0.0;

    const auto model = ext::make_shared<HestonModel>(
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

Real priceHestonModel(ext::shared_ptr<QuantLib::HestonModel>& model,
                      const std::vector<Date>& dates,
                      const std::vector<Rate>& rates,
                      const std::vector<Real>& dividendYield,
                      const DayCounter& dayCounter,
                      const Calendar& calendar,
                      const std::vector<Integer>& t,
                      const std::vector<Volatility>& v,
                      const std::vector<Real>& strike,
                      const Handle<Quote>& s0,
                      Date settlementDate) {
    const Date maturityDate = settlementDate + Period(1, Years);

    const ext::shared_ptr<Exercise> exercise = ext::make_shared<EuropeanExercise>(maturityDate);

    const ext::shared_ptr<PricingEngine> cosEngine(
        ext::make_shared<COSHestonEngine>(model, 25, 600));
    CalibrationMarketData marketData = getDAXCalibrationMarketData(
        dates, rates, dividendYield, dayCounter, calendar, t, v, strike, s0);

    const Handle<Quote> S0 = marketData.s0;

    auto payoff = ext::make_shared<PlainVanillaPayoff>(Option::Call, S0->value() + 20);

    VanillaOption option(payoff, exercise);

    option.setPricingEngine(cosEngine);

    return option.NPV();
}

// price with sensitivities
#ifndef QLAAD_DISABLE_AAD

// create tape
using tape_type = Real::tape_type;
tape_type tape;

Real priceWithSensi(const std::vector<Date>& dates,
                    const std::vector<Rate>& rates,
                    const std::vector<Real>& dividendYield,
                    const DayCounter& dayCounter,
                    const Calendar& calendar,
                    const std::vector<Integer>& t,
                    const std::vector<Volatility>& v,
                    const std::vector<Real>& strike,
                    const Handle<Quote>& s0,
                    std::vector<Real>& gradient_v,
                    std::vector<Real>& gradient_strikes,
                    std::vector<Real>& gradient_rates,
                    std::vector<Real>& gradient_dividendYield,
                    Date settlementDate) {
    // copy inputs and register them on the tape
    auto rates_t = rates;
    auto dividendYield_t = dividendYield;
    auto strike_t = strike;
    auto v_t = v;
    tape.registerInputs(rates_t);
    tape.registerInputs(dividendYield_t);
    tape.registerInputs(strike_t);
    tape.registerInputs(v_t);
    tape.newRecording();

    auto model = HestonModelCalibration(dates, rates_t, dividendYield_t, dayCounter, calendar, t,
                                        v_t, strike_t, s0, settlementDate);

    Real value = priceHestonModel(model, dates, rates_t, dividendYield_t, dayCounter, calendar, t,
                                  v_t, strike_t, s0, settlementDate);

    // register output, set adjoint, and roll-back tape
    tape.registerOutput(value);
    derivative(value) = 1.0;
    tape.computeAdjoints();

    // store derivatives in the gradient vectors
    std::transform(rates_t.begin(), rates_t.end(), std::back_inserter(gradient_rates),
                   [](const Real& q) { return derivative(q); });
    std::transform(dividendYield_t.begin(), dividendYield_t.end(),
                   std::back_inserter(gradient_dividendYield),
                   [](const Real& q) { return derivative(q); });
    std::transform(strike_t.begin(), strike_t.end(), std::back_inserter(gradient_strikes),
                   [](const Real& q) { return derivative(q); });
    std::transform(v_t.begin(), v_t.end(), std::back_inserter(gradient_v),
                   [](const Real& q) { return derivative(q); });
    return value;
}
#endif

void printResults(Real v,
                  const std::vector<Real>& gradient_v,
                  const std::vector<Real>& gradient_strikes,
                  const std::vector<Real>& gradient_rates,
                  const std::vector<Real>& gradient_dividendYield) {

    std::cout << "Value               = " << v << "\n";
    std::cout << "strikeSensitivities = [";
    for (auto& s : gradient_strikes)
        std::cout << s << ", ";
    std::cout << "]\n";
    std::cout << "Rhos                = [";
    for (auto& s : gradient_rates)
        std::cout << s << ", ";
    std::cout << "]\n";
    std::cout << "dividendRhos        = [";
    for (auto& s : gradient_dividendYield)
        std::cout << s << ", ";
    std::cout << "]\n";
    std::cout << "Vegas               = [\n";
    for (auto& s : gradient_v)
        std::cout << s << "\n";
    std::cout << "]\n";
    
}

int main() {

    try {
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
            0.6625, 0.4875, 0.4204, 0.3668, 0.3431, 0.3267, 0.3121, 0.3121, 0.6007, 0.4543, 0.3967,
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
        std::vector<Real> strike = {3401, 4000, 4400, 5000, 5600};

        std::vector<Real> dividendYield = {0.11,   0.12,   0.13,   0.124, 0.1245,
                                           0.1537, 0.1458, 0.1874, 0.1656};

        std::cout.precision(6);

#ifdef QLAAD_DISABLE_AAD
        std::cout << "Pricing with Heston COS engine, no sensitivities...\n";
        auto model = HestonModelCalibration(dates, rates, dividendYield, dayCounter, calendar, t, v,
                                            strike, s0, settlementDate);
        Real value = priceHestonModel(model, dates, rates, dividendYield, dayCounter, calendar, t,
                                      v, strike, s0, settlementDate);
        std::cout << "Value : " << value << "\n";
#else
        std::vector<Real> gradient_v;
        std::vector<Real> gradient_rates;
        std::vector<Real> gradient_dividendYield;
        std::vector<Real> gradient_strikes;
        std::cout << "Pricing with Heston COS engine, with sensitivities...\n";
        Real value = priceWithSensi(dates, rates, dividendYield, dayCounter, calendar, t, v, strike,
                                    s0, gradient_v, gradient_strikes, gradient_rates,
                                    gradient_dividendYield, settlementDate);
        printResults(value, gradient_v, gradient_strikes, gradient_rates, gradient_dividendYield);
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
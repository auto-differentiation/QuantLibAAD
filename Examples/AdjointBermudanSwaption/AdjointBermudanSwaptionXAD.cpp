/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*!
 Copyright (C) 2002, 2003 Sadruddin Rejeb
 Copyright (C) 2004 Ferdinando Ametrano
 Copyright (C) 2005, 2006, 2007 StatPro Italia srl
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
This example demonstrates how to use XAD to price a Bermudan swaption with
sensitivities to the swaption volatilities, through a Hull-White model
calibrated to 5 of them.

The sensitivities go through the calibration with the implicit function
theorem, without recording the optimizer: at the calibrated parameters p the
gradient of the calibration objective in p is zero, so
dV/dvol = dV/dvol|p - (Hpp^-1 dV/dp)' Hpvol, with the Hessian blocks Hpp and
Hpvol from central differences of the XAD gradient in p (as in the
AdjointHestonModel example). Recording the optimizer's iterations instead gives
the derivative of where it stopped, not of the optimum.
*/

#include <ql/qldefines.hpp>
#if !defined(BOOST_ALL_NO_LIB) && defined(BOOST_MSVC)
#    include <ql/auto_link.hpp>
#endif
#include <ql/cashflows/coupon.hpp>
#include <ql/indexes/ibor/euribor.hpp>
#include <ql/instruments/swaption.hpp>
#include <ql/instruments/vanillaswap.hpp>
#include <ql/math/matrix.hpp>
#include <ql/math/optimization/levenbergmarquardt.hpp>
#include <ql/models/shortrate/calibrationhelpers/swaptionhelper.hpp>
#include <ql/models/shortrate/onefactormodels/hullwhite.hpp>
#include <ql/pricingengines/swap/discountingswapengine.hpp>
#include <ql/pricingengines/swaption/fdhullwhiteswaptionengine.hpp>
#include <ql/pricingengines/swaption/jamshidianswaptionengine.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/time/calendars/target.hpp>
#include <ql/time/daycounters/thirty360.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>

using namespace QuantLib;

namespace {

    // the curve, the calibration swaptions and the Bermudan swaption
    struct Market {
        Handle<YieldTermStructure> curve;
        std::vector<ext::shared_ptr<CalibrationHelper>> swaptions;
        ext::shared_ptr<Swaption> bermudan;
    };

    // builds the market from the 5x5 swaption vols (by expiry, then swap length);
    // the model is calibrated to the diagonal 1x5, 2x4, 3x3, 4x2, 5x1
    Market build(const std::vector<Real>& swaptionVols, Real flatRate) {
        Market m;
        Calendar calendar = TARGET();
        Date settlementDate(19, February, 2002);
        m.curve = Handle<YieldTermStructure>(
            ext::make_shared<FlatForward>(settlementDate, flatRate, Actual365Fixed()));
        auto indexSixMonths = ext::make_shared<Euribor6M>(m.curve);

        const Size numRows = 5, numCols = 5;
        const Integer swapLengths[] = {1, 2, 3, 4, 5};
        for (Size i = 0; i < numRows; i++) {
            Size j = numCols - i - 1;
            auto vol = ext::make_shared<SimpleQuote>(swaptionVols[i * numCols + j]);
            m.swaptions.push_back(ext::make_shared<SwaptionHelper>(
                Period(i + 1, Years), Period(swapLengths[j], Years), Handle<Quote>(vol),
                indexSixMonths, indexSixMonths->tenor(), indexSixMonths->dayCounter(),
                indexSixMonths->dayCounter(), m.curve));
        }

        // the ITM payer swap starting in 1 year for 5 years, exercisable on each fixed coupon
        BusinessDayConvention fixedLegConvention = Unadjusted;
        BusinessDayConvention floatingLegConvention = ModifiedFollowing;
        DayCounter fixedLegDayCounter = Thirty360(Thirty360::European);
        Date startDate = calendar.advance(settlementDate, 1, Years, floatingLegConvention);
        Date maturity = calendar.advance(startDate, 5, Years, floatingLegConvention);
        Schedule fixedSchedule(startDate, maturity, Period(Annual), calendar, fixedLegConvention,
                               fixedLegConvention, DateGeneration::Forward, false);
        Schedule floatSchedule(startDate, maturity, Period(Semiannual), calendar,
                               floatingLegConvention, floatingLegConvention,
                               DateGeneration::Forward, false);

        VanillaSwap swap(Swap::Payer, 1000.0, fixedSchedule, 0.03, fixedLegDayCounter,
                         floatSchedule, indexSixMonths, 0.0, indexSixMonths->dayCounter());
        swap.setPricingEngine(ext::make_shared<DiscountingSwapEngine>(m.curve));
        Rate fixedITMRate = swap.fairRate() * 0.8;
        auto itmSwap = ext::make_shared<VanillaSwap>(
            Swap::Payer, 1000.0, fixedSchedule, fixedITMRate, fixedLegDayCounter, floatSchedule,
            indexSixMonths, 0.0, indexSixMonths->dayCounter());

        std::vector<Date> bermudanDates;
        for (const auto& cf : swap.fixedLeg())
            bermudanDates.push_back(ext::dynamic_pointer_cast<Coupon>(cf)->accrualStartDate());
        m.bermudan = ext::make_shared<Swaption>(
            itmSwap, ext::make_shared<BermudanExercise>(bermudanDates));
        return m;
    }

    // Hull-White with parameters p = (a, sigma), pricing the calibration swaptions
    ext::shared_ptr<HullWhite> makeModel(const Market& m, const std::vector<Real>& p) {
        auto model = ext::make_shared<HullWhite>(m.curve, p[0], p[1]);
        for (const auto& helper : m.swaptions)
            ext::dynamic_pointer_cast<BlackCalibrationHelper>(helper)->setPricingEngine(
                ext::make_shared<JamshidianSwaptionEngine>(model));
        return model;
    }

    std::vector<Real> calibrate(const std::vector<Real>& swaptionVols, Real flatRate) {
        Market m = build(swaptionVols, flatRate);
        auto model = makeModel(m, {0.1, 0.01});
        LevenbergMarquardt om;
        model->calibrate(m.swaptions, om, EndCriteria(400, 100, 1.0e-8, 1.0e-8, 1.0e-8));
        Array p = model->params();
        return std::vector<Real>(p.begin(), p.end());
    }

    Real price(const std::vector<Real>& swaptionVols, Real flatRate, const std::vector<Real>& p) {
        Market m = build(swaptionVols, flatRate);
        auto model = makeModel(m, p);
        m.bermudan->setPricingEngine(ext::make_shared<FdHullWhiteSwaptionEngine>(model));
        return m.bermudan->NPV();
    }

#ifndef QLAAD_DISABLE_AAD

    using tape_type = Real::tape_type;

    // f(p, vols), with its gradient with respect to the parameters and the vols
    template <class F>
    double gradient(tape_type& tape,
                  F f,
                  const std::vector<Real>& parameters,
                  const std::vector<Real>& swaptionVols,
                  std::vector<double>& dp,
                  std::vector<double>& dvol) {
        tape.clearAll();
        std::vector<Real> p = parameters, vols = swaptionVols;
        tape.registerInputs(p);
        tape.registerInputs(vols);
        tape.newRecording();

        Real y = f(p, vols);

        tape.registerOutput(y);
        derivative(y) = 1.0;
        tape.computeAdjoints();

        dp.clear();
        for (const auto& pi : p)
            dp.push_back(derivative(pi));
        dvol.clear();
        for (const auto& vi : vols)
            dvol.push_back(derivative(vi));
        return value(y);
    }

    // the price, with its sensitivities to the vols through the calibrated parameters p
    Real implicitFunctionTheorem(tape_type& tape,
                                 const std::vector<Real>& swaptionVols,
                                 Real flatRate,
                                 const std::vector<Real>& p,
                                 std::vector<Real>& vegas) {
        // the sum of squared calibration errors, which calibrate() minimizes
        auto objective = [flatRate](const std::vector<Real>& q, const std::vector<Real>& vols) {
            Market m = build(vols, flatRate);
            makeModel(m, q);
            Real f = 0.0;
            for (const auto& helper : m.swaptions) {
                Real error = helper->calibrationError();
                f += error * error;
            }
            return f;
        };
        auto target = [flatRate](const std::vector<Real>& q, const std::vector<Real>& vols) {
            return price(vols, flatRate, q);
        };

        // Hessian blocks of the objective, from central differences of its gradient
        Size n = p.size(), nv = swaptionVols.size();
        Matrix Hpp(n, n), Hvp(nv, n);
        for (Size k = 0; k < n; ++k) {
            double h = 1e-4 * std::max(std::fabs(value(p[k])), 0.01);
            std::vector<Real> up = p, down = p;
            up[k] += h;
            down[k] -= h;
            std::vector<double> dpUp, dvUp, dpDown, dvDown;
            gradient(tape, objective, up, swaptionVols, dpUp, dvUp);
            gradient(tape, objective, down, swaptionVols, dpDown, dvDown);
            for (Size i = 0; i < n; ++i)
                Hpp[i][k] = (dpUp[i] - dpDown[i]) / (2.0 * h);
            for (Size j = 0; j < nv; ++j)
                Hvp[j][k] = (dvUp[j] - dvDown[j]) / (2.0 * h);
        }

        // dV/dp and the direct dV/dvol, with the parameters held fixed
        std::vector<double> dVdp, dVdvol;
        Real npv = gradient(tape, target, p, swaptionVols, dVdp, dVdvol);

        // the parameters must be at a minimum of the objective, where this is positive
        QL_REQUIRE(determinant(Hpp) > 0.0,
                   "the Hessian of the calibration objective is singular or indefinite");
        Array lambda = inverse(Hpp) * Array(dVdp.begin(), dVdp.end());
        Array v = Array(dVdvol.begin(), dVdvol.end()) - Hvp * lambda;
        vegas.assign(v.begin(), v.end());
        return npv;
    }

#endif

}

void printResults(Real value, const std::vector<Real>& gradient) {
    std::cout.precision(6);
    std::cout << std::fixed;
    std::cout.imbue(std::locale(""));

    std::cout << "Price = " << value << std::endl;
    std::cout << "Vegas:\n";
    for (std::size_t i = 0; i < gradient.size(); ++i)
        std::cout << "Vega #" << i << " = " << gradient[i] << "\n";

    std::cout << std::endl;
}

int main(int argc, char* argv[]) {

    try {

        const int N = argc < 2 ? 1 : std::atol(argv[1]);

        std::cout << std::endl;

        Settings::instance().evaluationDate() = Date(15, February, 2002);

        // rate
        Real flatRate = 0.04875825;

        // volatilities, by expiry (1 to 5 years), then swap length (1 to 5 years)
        std::vector<Real> swaptionVols = {
            0.1490, 0.1340, 0.1228, 0.1189, 0.1148, 0.1290, 0.1201, 0.1146, 0.1108,
            0.1040, 0.1149, 0.1112, 0.1070, 0.1010, 0.0957, 0.1047, 0.1021, 0.0980,
            0.0951, 0.1270, 0.1000, 0.0950, 0.0900, 0.1230, 0.1160};

#ifdef QLAAD_DISABLE_AAD
        std::cout << "Pricing Bermudan swaption without sensitivities...\n";
        Real npv = price(swaptionVols, flatRate, calibrate(swaptionVols, flatRate));
        std::cout << "Price = " << npv << std::endl;
#else
        std::cout << "Pricing Bermudan swaption with sensitivities...\n";
        tape_type tape;
        std::vector<Real> vegas;

        Real npv = 0.0;
        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < N; ++i) {
            std::vector<Real> p = calibrate(swaptionVols, flatRate);
            npv = implicitFunctionTheorem(tape, swaptionVols, flatRate, p, vegas);
        }
        auto end = std::chrono::high_resolution_clock::now();
        auto time =
            static_cast<double>(
                std::chrono::duration_cast<std::chrono::microseconds>(end - start).count()) *
            1e-3 / N;
        printResults(npv, vegas);

        std::cout << std::fixed << std::setprecision(9);
        std::cout << "For " << N << " repetitions, it took on average " << time << " ms\n";
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

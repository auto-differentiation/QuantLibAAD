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
This example demonstrates how to use XAD to price a Bermudan Swaption with
sensitivities.

TODO: Use the implicit function theorem for calibration with AAD.
*/

#include <ql/qldefines.hpp>
#if !defined(BOOST_ALL_NO_LIB) && defined(BOOST_MSVC)
#    include <ql/auto_link.hpp>
#endif
#include <ql/cashflows/coupon.hpp>
#include <ql/indexes/ibor/euribor.hpp>
#include <ql/instruments/swaption.hpp>
#include <ql/instruments/vanillaswap.hpp>
#include <ql/math/optimization/levenbergmarquardt.hpp>
#include <ql/models/shortrate/calibrationhelpers/swaptionhelper.hpp>
#include <ql/models/shortrate/onefactormodels/blackkarasinski.hpp>
#include <ql/pricingengines/swap/discountingswapengine.hpp>
#include <ql/pricingengines/swaption/fdg2swaptionengine.hpp>
#include <ql/pricingengines/swaption/fdhullwhiteswaptionengine.hpp>
#include <ql/pricingengines/swaption/g2swaptionengine.hpp>
#include <ql/pricingengines/swaption/jamshidianswaptionengine.hpp>
#include <ql/pricingengines/swaption/treeswaptionengine.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/time/calendars/target.hpp>
#include <ql/time/daycounters/thirty360.hpp>
#include <ql/utilities/dataformatters.hpp>
#include <iomanip>
#include <iostream>
#include <chrono>

using namespace QuantLib;

// Number of swaptions to be calibrated to...
void calibrateModel(const ext::shared_ptr<ShortRateModel>& model,
                    const std::vector<ext::shared_ptr<BlackCalibrationHelper>>& swaptions,
                    const std::vector<Integer>& swapLengths,
                    const std::vector<Volatility>& swaptionVols,
                    Size numRows,
                    Size numCols) {

    std::vector<ext::shared_ptr<CalibrationHelper>> helpers(swaptions.begin(), swaptions.end());
    LevenbergMarquardt om;
    model->calibrate(helpers, om, EndCriteria(400, 100, 1.0e-8, 1.0e-8, 1.0e-8));
}

Handle<YieldTermStructure> setupYields(Date settlementDate, Real flatRate) {
    // flat yield term structure impling 1x5 swap at 5%
    auto rate = ext::make_shared<SimpleQuote>(flatRate);
    return Handle<YieldTermStructure>(
        ext::make_shared<FlatForward>(settlementDate, Handle<Quote>(rate), Actual365Fixed()));
}

Real priceSwaption(const std::vector<Integer>& swapLengths,
                   const std::vector<Volatility>& swaptionVols,
                   Size numRows,
                   Size numCols,
                   Real flatRate) {

    Date todaysDate(15, February, 2002);
    Calendar calendar = TARGET();
    Date settlementDate(19, February, 2002);
    Settings::instance().evaluationDate() = todaysDate;

    auto rhTermStructure = setupYields(settlementDate, flatRate);

    // Define the ITM swap
    Frequency fixedLegFrequency = Annual;
    BusinessDayConvention fixedLegConvention = Unadjusted;
    BusinessDayConvention floatingLegConvention = ModifiedFollowing;
    DayCounter fixedLegDayCounter = Thirty360(Thirty360::European);
    Frequency floatingLegFrequency = Semiannual;
    Swap::Type type = Swap::Payer;
    Rate dummyFixedRate = 0.03;
    auto indexSixMonths = ext::make_shared<Euribor6M>(rhTermStructure);

    Date startDate = calendar.advance(settlementDate, 1, Years, floatingLegConvention);
    Date maturity = calendar.advance(startDate, 5, Years, floatingLegConvention);
    Schedule fixedSchedule(startDate, maturity, Period(fixedLegFrequency), calendar,
                           fixedLegConvention, fixedLegConvention, DateGeneration::Forward, false);
    Schedule floatSchedule(startDate, maturity, Period(floatingLegFrequency), calendar,
                           floatingLegConvention, floatingLegConvention, DateGeneration::Forward,
                           false);

    auto swap = ext::make_shared<VanillaSwap>(type, 1000.0, fixedSchedule, dummyFixedRate,
                                              fixedLegDayCounter, floatSchedule, indexSixMonths,
                                              0.0, indexSixMonths->dayCounter());
    swap->setPricingEngine(ext::make_shared<DiscountingSwapEngine>(rhTermStructure));
    Rate fixedITMRate = swap->fairRate() * 0.8;

    auto itmSwap = ext::make_shared<VanillaSwap>(type, 1000.0, fixedSchedule, fixedITMRate,
                                                 fixedLegDayCounter, floatSchedule, indexSixMonths,
                                                 0.0, indexSixMonths->dayCounter());

    // defining the swaptions to be used in model calibration
    std::vector<Period> swaptionMaturities = {1 * Years, 2 * Years, 3 * Years, 4 * Years,
                                              5 * Years};

    std::vector<ext::shared_ptr<BlackCalibrationHelper>> swaptions;

    // List of times that have to be included in the timegrid
    std::list<Time> times;

    Size i;
    for (i = 0; i < numRows; i++) {
        Size j = numCols - i - 1; // 1x5, 2x4, 3x3, 4x2, 5x1
        Size k = i * numCols + j;
        ext::shared_ptr<Quote> vol(new SimpleQuote(swaptionVols[k]));
        swaptions.push_back(ext::make_shared<SwaptionHelper>(
            swaptionMaturities[i], Period(swapLengths[j], Years), Handle<Quote>(vol),
            indexSixMonths, indexSixMonths->tenor(), indexSixMonths->dayCounter(),
            indexSixMonths->dayCounter(), rhTermStructure));
        swaptions.back()->addTimesTo(times);
    }

    // Building time-grid
    TimeGrid grid(times.begin(), times.end(), 30);

    // defining the models
    auto modelHW = ext::make_shared<HullWhite>(rhTermStructure);


    // model calibrations
    for (i = 0; i < swaptions.size(); i++)
        swaptions[i]->setPricingEngine(ext::make_shared<JamshidianSwaptionEngine>(modelHW));

    calibrateModel(modelHW, swaptions, swapLengths, swaptionVols, numRows, numCols);


    std::vector<Date> bermudanDates;
    const auto& leg = swap->fixedLeg();
    for (i = 0; i < leg.size(); i++) {
        auto coupon = ext::dynamic_pointer_cast<Coupon>(leg[i]);
        bermudanDates.push_back(coupon->accrualStartDate());
    }

    auto bermudanExercise = ext::make_shared<BermudanExercise>(bermudanDates);
    Swaption itmBermudanSwaption(itmSwap, bermudanExercise);

    // Do the pricing
    // itmBermudanSwaption.setPricingEngine(ext::make_shared<TreeSwaptionEngine>(modelHW, 50));
    itmBermudanSwaption.setPricingEngine(ext::make_shared<FdHullWhiteSwaptionEngine>(modelHW));
    return itmBermudanSwaption.NPV();
}


// Sensitivities with XAD
#ifndef QLAAD_DISABLE_AAD

// create tape
using tape_type = Real::tape_type;
tape_type tape;

Real priceWithSensi(const std::vector<Integer>& swapLengths,
                    const std::vector<Volatility>& swaptionVols,
                    Size numRows,
                    Size numCols,
                    Real flatRate,
                    std::vector<Real>& gradient) {
    tape.clearAll();
    // register the independent inputs
    auto swaptionVols_t = swaptionVols;
    tape.registerInputs(swaptionVols_t);
    tape.newRecording();

    Real v = priceSwaption(swapLengths, swaptionVols_t, numRows, numCols, flatRate);

    // register dependent output, set adjoint, and roll back to input adjoints
    tape.registerOutput(v);
    derivative(v) = 1.0;
    tape.computeAdjoints();

    // store adjoints in gradient vector
    gradient.clear();
    gradient.reserve(swaptionVols_t.size());
    for (auto& vol : swaptionVols_t) {
        gradient.push_back(derivative(vol));
    }

    return v;
}

#endif


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

        // rate
        Real flatRate = 0.04875825;

        // volatilities
        Size numRows = 5;
        Size numCols = 5;

        std::vector<Integer> swapLengths = {1, 2, 3, 4, 5};
        std::vector<Volatility> swaptionVols = {
            0.1490, 0.1340, 0.1228, 0.1189, 0.1148, 0.1290, 0.1201, 0.1146, 0.1108,
            0.1040, 0.1149, 0.1112, 0.1070, 0.1010, 0.0957, 0.1047, 0.1021, 0.0980,
            0.0951, 0.1270, 0.1000, 0.0950, 0.0900, 0.1230, 0.1160};

#ifdef QLAAD_DISABLE_AAD
        std::cout << "Pricing Bermudan swaption without sensitivities...\n";
        Real price = priceSwaption(swapLengths, swaptionVols, numRows, numCols, flatRate);
        std::cout << "Price = " << price << std::endl;
#else
        std::cout << "Pricing Bermudan swaption with sensitivities...\n";
        std::vector<Real> gradient;
        
        Real price = 0.0;
        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < N; ++i) {
            price =
                priceWithSensi(swapLengths, swaptionVols, numRows, numCols, flatRate, gradient);
        }
        auto end = std::chrono::high_resolution_clock::now();
        auto time =
            static_cast<double>(
                std::chrono::duration_cast<std::chrono::microseconds>(end - start).count()) *
            1e-3 / N;
        printResults(price, gradient);

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

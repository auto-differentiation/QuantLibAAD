/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
 Copyright (C) 2000, 2001, 2002, 2003 RiskMap srl
 Copyright (C) 2003, 2004, 2005, 2006, 2007 StatPro Italia srl
 Copyright (C) 2004 Ferdinando Ametrano
 Copyright (C) 2018 Jose Garcia
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
This example demonstrates how to use XAD to calculate sensitivities to
market quotes when multi-curve bootstrapping is used from many input
quotes.
It also measures the performance for calculating sensitivities,
either with XAD or with plain doubles and bumping (when QLAAD_DISABLE_AAD is ON).
*/

#include <ql/qldefines.hpp>
#if !defined(BOOST_ALL_NO_LIB) && defined(BOOST_MSVC)
#    include <ql/auto_link.hpp>
#endif
#include <ql/exercise.hpp>
#include <ql/indexes/ibor/eonia.hpp>
#include <ql/indexes/ibor/euribor.hpp>
#include <ql/math/interpolations/cubicinterpolation.hpp>
#include <ql/math/interpolations/loginterpolation.hpp>
#include <ql/pricingengines/swap/discountingswapengine.hpp>
#include <ql/termstructures/yield/oisratehelper.hpp>
#include <ql/termstructures/yield/piecewiseyieldcurve.hpp>
#include <ql/termstructures/yield/ratehelpers.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>
#include <ql/time/calendars/target.hpp>
#include <ql/time/daycounters/actual360.hpp>
#include <ql/time/daycounters/actualactual.hpp>
#include <ql/time/daycounters/thirty360.hpp>
#include <ql/time/imm.hpp>
#include <chrono>
#include <iomanip>
#include <iostream>

using namespace QuantLib;


Real priceMulticurveBootstrappingSwap(const std::vector<Real>& depos,
                                      const Calendar& calendar,
                                      const std::vector<Real>& shortOis,
                                      const std::vector<Real>& datesOIS,
                                      const std::vector<Real>& longTermOIS,
                                      Date todaysDate,
                                      const DayCounter& termStructureDayCounter,
                                      Real d6MRate,
                                      const std::vector<Real>& fra,
                                      const std::vector<Real>& swapRates,
                                      Date settlementDate,
                                      Date maturity,
                                      Real nominal,
                                      Real fixedRate,
                                      Real spread,
                                      Integer lengthInYears) {
    auto eonia = ext::make_shared<Eonia>();
    std::vector<ext::shared_ptr<RateHelper>> eoniaInstruments;
    // deposits
    std::map<Natural, ext::shared_ptr<Quote>> depoQuotes = {
        // settlement days, quote
        {0, ext::make_shared<SimpleQuote>(depos[0])},
        {1, ext::make_shared<SimpleQuote>(depos[1])},
        {2, ext::make_shared<SimpleQuote>(depos[2])}};

    DayCounter depositDayCounter = Actual360();

    for (const auto& q : depoQuotes) {
        auto settlementDays = q.first;
        auto quote = q.second;
        auto helper =
            ext::make_shared<DepositRateHelper>(Handle<Quote>(quote), 1 * Days, settlementDays,
                                                calendar, Following, false, depositDayCounter);
        eoniaInstruments.push_back(helper);
    }

    // short-term OIS
    std::map<Period, ext::shared_ptr<Quote>> shortOisQuotes = {
        {1 * Weeks, ext::make_shared<SimpleQuote>(shortOis[0])},
        {2 * Weeks, ext::make_shared<SimpleQuote>(shortOis[1])},
        {3 * Weeks, ext::make_shared<SimpleQuote>(shortOis[2])},
        {1 * Months, ext::make_shared<SimpleQuote>(shortOis[3])}};

    for (const auto& q : shortOisQuotes) {
        auto tenor = q.first;
        auto quote = q.second;
        auto helper = ext::make_shared<OISRateHelper>(2, tenor, Handle<Quote>(quote), eonia);
        eoniaInstruments.push_back(helper);
    }

    // Dated OIS
    std::map<std::pair<Date, Date>, ext::shared_ptr<Quote>> datedOisQuotes = {
        {{Date(16, January, 2013), Date(13, February, 2013)},
         ext::make_shared<SimpleQuote>(datesOIS[0])},
        {{Date(13, February, 2013), Date(13, March, 2013)},
         ext::make_shared<SimpleQuote>(datesOIS[1])},
        {{Date(13, March, 2013), Date(10, April, 2013)},
         ext::make_shared<SimpleQuote>(datesOIS[2])},
        {{Date(10, April, 2013), Date(8, May, 2013)}, ext::make_shared<SimpleQuote>(datesOIS[3])},
        {{Date(8, May, 2013), Date(12, June, 2013)}, ext::make_shared<SimpleQuote>(datesOIS[4])},
    };

    for (const auto& q : datedOisQuotes) {
        auto startDate = q.first.first;
        auto endDate = q.first.second;
        auto quote = q.second;
        auto helper =
            ext::make_shared<OISRateHelper>(startDate, endDate, Handle<Quote>(quote), eonia);
        eoniaInstruments.push_back(helper);
    }

    // long-term OIS
    std::map<Period, ext::shared_ptr<Quote>> longOisQuotes = {
        {15 * Months, ext::make_shared<SimpleQuote>(longTermOIS[0])},
        {18 * Months, ext::make_shared<SimpleQuote>(longTermOIS[1])},
        {21 * Months, ext::make_shared<SimpleQuote>(longTermOIS[2])},
        {2 * Years, ext::make_shared<SimpleQuote>(longTermOIS[3])},
        {3 * Years, ext::make_shared<SimpleQuote>(longTermOIS[4])},
        {4 * Years, ext::make_shared<SimpleQuote>(longTermOIS[5])},
        {5 * Years, ext::make_shared<SimpleQuote>(longTermOIS[6])},
        {6 * Years, ext::make_shared<SimpleQuote>(longTermOIS[7])},
        {7 * Years, ext::make_shared<SimpleQuote>(longTermOIS[8])},
        {8 * Years, ext::make_shared<SimpleQuote>(longTermOIS[9])},
        {9 * Years, ext::make_shared<SimpleQuote>(longTermOIS[10])},
        {10 * Years, ext::make_shared<SimpleQuote>(longTermOIS[11])},
        {11 * Years, ext::make_shared<SimpleQuote>(longTermOIS[12])},
        {12 * Years, ext::make_shared<SimpleQuote>(longTermOIS[13])},
        {15 * Years, ext::make_shared<SimpleQuote>(longTermOIS[14])},
        {20 * Years, ext::make_shared<SimpleQuote>(longTermOIS[15])},
        {25 * Years, ext::make_shared<SimpleQuote>(longTermOIS[16])},
        {30 * Years, ext::make_shared<SimpleQuote>(longTermOIS[17])}};

    for (const auto& q : longOisQuotes) {
        auto tenor = q.first;
        auto quote = q.second;
        auto helper = ext::make_shared<OISRateHelper>(2, tenor, Handle<Quote>(quote), eonia);
        eoniaInstruments.push_back(helper);
    }
    // curve
    auto eoniaTermStructure = ext::make_shared<PiecewiseYieldCurve<Discount, Cubic>>(
        todaysDate, eoniaInstruments, termStructureDayCounter);

    eoniaTermStructure->enableExtrapolation();

    // This curve will be used for discounting cash flows
    RelinkableHandle<YieldTermStructure> discountingTermStructure;
    discountingTermStructure.linkTo(eoniaTermStructure);

    std::vector<ext::shared_ptr<RateHelper>> euribor6MInstruments;

    auto euribor6M = ext::make_shared<Euribor6M>();

    auto d6M = ext::make_shared<DepositRateHelper>(
        Handle<Quote>(ext::make_shared<SimpleQuote>(d6MRate)), 6 * Months, 3, calendar, Following,
        false, depositDayCounter);

    euribor6MInstruments.push_back(d6M);

    // FRAs
    std::map<Natural, ext::shared_ptr<Quote>> fraQuotes = {
        {1, ext::make_shared<SimpleQuote>(fra[0])},   {2, ext::make_shared<SimpleQuote>(fra[1])},
        {3, ext::make_shared<SimpleQuote>(fra[2])},   {4, ext::make_shared<SimpleQuote>(fra[3])},
        {5, ext::make_shared<SimpleQuote>(fra[4])},   {6, ext::make_shared<SimpleQuote>(fra[5])},
        {7, ext::make_shared<SimpleQuote>(fra[6])},   {8, ext::make_shared<SimpleQuote>(fra[7])},
        {9, ext::make_shared<SimpleQuote>(fra[8])},   {10, ext::make_shared<SimpleQuote>(fra[9])},
        {11, ext::make_shared<SimpleQuote>(fra[10])}, {12, ext::make_shared<SimpleQuote>(fra[11])},
        {13, ext::make_shared<SimpleQuote>(fra[12])}, {14, ext::make_shared<SimpleQuote>(fra[13])},
        {15, ext::make_shared<SimpleQuote>(fra[14])}, {16, ext::make_shared<SimpleQuote>(fra[15])},
        {17, ext::make_shared<SimpleQuote>(fra[16])}, {18, ext::make_shared<SimpleQuote>(fra[17])}};

    for (const auto& q : fraQuotes) {
        auto monthsToStart = q.first;
        auto quote = q.second;
        auto helper =
            ext::make_shared<FraRateHelper>(Handle<Quote>(quote), monthsToStart, euribor6M);
        euribor6MInstruments.push_back(helper);
    }
    // swaps
    std::map<Period, ext::shared_ptr<Quote>> swapQuotes = {
        {3 * Years, ext::make_shared<SimpleQuote>(swapRates[0])},
        {4 * Years, ext::make_shared<SimpleQuote>(swapRates[1])},
        {5 * Years, ext::make_shared<SimpleQuote>(swapRates[2])},
        {6 * Years, ext::make_shared<SimpleQuote>(swapRates[3])},
        {7 * Years, ext::make_shared<SimpleQuote>(swapRates[4])},
        {8 * Years, ext::make_shared<SimpleQuote>(swapRates[5])},
        {9 * Years, ext::make_shared<SimpleQuote>(swapRates[6])},
        {10 * Years, ext::make_shared<SimpleQuote>(swapRates[7])},
        {12 * Years, ext::make_shared<SimpleQuote>(swapRates[8])},
        {15 * Years, ext::make_shared<SimpleQuote>(swapRates[9])},
        {20 * Years, ext::make_shared<SimpleQuote>(swapRates[10])},
        {25 * Years, ext::make_shared<SimpleQuote>(swapRates[11])},
        {30 * Years, ext::make_shared<SimpleQuote>(swapRates[12])},
        {35 * Years, ext::make_shared<SimpleQuote>(swapRates[13])},
        {40 * Years, ext::make_shared<SimpleQuote>(swapRates[14])},
        {50 * Years, ext::make_shared<SimpleQuote>(swapRates[15])},
        {60 * Years, ext::make_shared<SimpleQuote>(swapRates[16])}};

    Frequency swFixedLegFrequency = Annual;
    BusinessDayConvention swFixedLegConvention = Unadjusted;
    DayCounter swFixedLegDayCounter = Thirty360(Thirty360::European);

    for (const auto& q : swapQuotes) {
        auto tenor = q.first;
        auto quote = q.second;
        auto helper = ext::make_shared<SwapRateHelper>(
            Handle<Quote>(quote), tenor, calendar, swFixedLegFrequency, swFixedLegConvention,
            swFixedLegDayCounter, euribor6M, Handle<Quote>(), 0 * Days,
            discountingTermStructure); // the Eonia curve is used for discounting
        euribor6MInstruments.push_back(helper);
    }
    double tolerance = 1.0e-15;
    auto euribor6MTermStructure = ext::make_shared<PiecewiseYieldCurve<Discount, Cubic>>(
        settlementDate, euribor6MInstruments, termStructureDayCounter,
        PiecewiseYieldCurve<Discount, Cubic>::bootstrap_type(tolerance));

    RelinkableHandle<YieldTermStructure> forecastingTermStructure;
    forecastingTermStructure.linkTo(euribor6MTermStructure);
    auto euriborIndex = ext::make_shared<Euribor6M>(forecastingTermStructure);
    Schedule fixedSchedule(settlementDate, maturity, Period(Annual), calendar, Unadjusted,
                           Unadjusted, DateGeneration::Forward, false);
    Schedule floatSchedule(settlementDate, maturity, Period(Semiannual), calendar,
                           ModifiedFollowing, ModifiedFollowing, DateGeneration::Forward, false);
    VanillaSwap spot5YearSwap(Swap::Payer, nominal, fixedSchedule, fixedRate,
                              Thirty360(Thirty360::European), floatSchedule, euriborIndex, spread,
                              Actual360());

    Date fwdStart = calendar.advance(settlementDate, 1, Years);
    Date fwdMaturity = fwdStart + lengthInYears * Years;
    Schedule fwdFixedSchedule(fwdStart, fwdMaturity, Period(Annual), calendar, Unadjusted,
                              Unadjusted, DateGeneration::Forward, false);
    Schedule fwdFloatSchedule(fwdStart, fwdMaturity, Period(Semiannual), calendar,
                              ModifiedFollowing, ModifiedFollowing, DateGeneration::Forward, false);
    VanillaSwap oneYearForward5YearSwap(Swap::Payer, nominal, fwdFixedSchedule, fixedRate,
                                        Thirty360(Thirty360::European), fwdFloatSchedule,
                                        euriborIndex, spread, Actual360());
    auto s5yRate = swapQuotes[5 * Years];
    ext::shared_ptr<PricingEngine> swapEngine(new DiscountingSwapEngine(discountingTermStructure));

    spot5YearSwap.setPricingEngine(swapEngine);
    oneYearForward5YearSwap.setPricingEngine(swapEngine);

    return spot5YearSwap.NPV();
}

#ifndef QLAAD_DISABLE_AAD

// create tape
using tape_type = Real::tape_type;
tape_type tape;

Real priceWithSensi(const std::vector<Real>& depos,
                    const Calendar& calendar,
                    const std::vector<Real>& shortOis,
                    const std::vector<Real>& datesOIS,
                    const std::vector<Real>& longTermOIS,
                    Date todaysDate,
                    const DayCounter& termStructureDayCounter,
                    Real d6MRate,
                    const std::vector<Real>& fra,
                    const std::vector<Real>& swapRates,
                    Date settlementDate,
                    Date maturity,
                    Real nominal,
                    Real fixedRate,
                    Real spread,
                    Integer lengthInYears,
                    std::vector<Real>& gradient) {
    // clear the tape and gradient to allow for re-running this in a loop
    tape.clearAll();
    gradient.clear();

    // copy independent variables and register them with the tape
    auto depos_t = depos;
    auto shortOis_t = shortOis;
    auto datesOIS_t = datesOIS;
    auto longTermOIS_t = longTermOIS;
    auto swapRates_t = swapRates;
    auto fra_t = fra;
    tape.registerInputs(depos_t);
    tape.registerInputs(shortOis_t);
    tape.registerInputs(datesOIS_t);
    tape.registerInputs(longTermOIS_t);
    tape.registerInputs(swapRates_t);
    tape.registerInputs(fra_t);
    tape.registerInput(fixedRate);
    tape.registerInput(spread);
    tape.registerInput(nominal);
    tape.registerInput(d6MRate);
    tape.newRecording();

    Real value = priceMulticurveBootstrappingSwap(
        depos_t, calendar, shortOis_t, datesOIS_t, longTermOIS_t, todaysDate,
        termStructureDayCounter, d6MRate, fra_t, swapRates_t, settlementDate, maturity, nominal,
        fixedRate, spread, lengthInYears);

    // register output, set its adjoint, and roll-back the tape
    tape.registerOutput(value);
    derivative(value) = 1.0;
    tape.computeAdjoints();

    // obtain the sensitivities (input adjonits)
    for (auto& g : depos_t)
        gradient.push_back(derivative(g));

    for (auto& g : shortOis_t)
        gradient.push_back(derivative(g));

    for (auto& g : datesOIS_t)
        gradient.push_back(derivative(g));

    for (auto& g : longTermOIS_t)
        gradient.push_back(derivative(g));

    for (auto& g : swapRates_t)
        gradient.push_back(derivative(g));

    for (auto& g : fra_t)
        gradient.push_back(derivative(g));


    gradient.push_back(derivative(fixedRate));
    gradient.push_back(derivative(spread));
    gradient.push_back(derivative(nominal));
    gradient.push_back(derivative(d6MRate));

    return value;
}


#else

Real priceWithSensi(const std::vector<Real>& depos,
                    const Calendar& calendar,
                    const std::vector<Real>& shortOis,
                    const std::vector<Real>& datesOIS,
                    const std::vector<Real>& longTermOIS,
                    Date todaysDate,
                    const DayCounter& termStructureDayCounter,
                    Real d6MRate,
                    const std::vector<Real>& fra,
                    const std::vector<Real>& swapRates,
                    Date settlementDate,
                    Date maturity,
                    Real nominal,
                    Real fixedRate,
                    Real spread,
                    Integer lengthInYears,
                    std::vector<Real>& gradient) {
    Real value = priceMulticurveBootstrappingSwap(depos, calendar, shortOis, datesOIS, longTermOIS,
                                                  todaysDate, termStructureDayCounter, d6MRate, fra,
                                                  swapRates, settlementDate, maturity, nominal,
                                                  fixedRate, spread, lengthInYears);

    // bump each value and calculate the sensitivities using finite differences
    Real eps = 1e-5;
    auto depos_t = depos;
    for (std::size_t i = 0; i < depos.size(); ++i) {
        depos_t[i] += eps;
        Real v1 = priceMulticurveBootstrappingSwap(
            depos_t, calendar, shortOis, datesOIS, longTermOIS, todaysDate, termStructureDayCounter,
            d6MRate, fra, swapRates, settlementDate, maturity, nominal, fixedRate, spread,
            lengthInYears);
        gradient.push_back((v1 - value) / eps);
        depos_t[i] -= eps;
    }

    auto shortOis_t = shortOis;
    for (std::size_t i = 0; i < shortOis.size(); ++i) {
        shortOis_t[i] += eps;
        Real v1 = priceMulticurveBootstrappingSwap(
            depos, calendar, shortOis_t, datesOIS, longTermOIS, todaysDate, termStructureDayCounter,
            d6MRate, fra, swapRates, settlementDate, maturity, nominal, fixedRate, spread,
            lengthInYears);
        gradient.push_back((v1 - value) / eps);
        shortOis_t[i] -= eps;
    }

    auto datesOIS_t = datesOIS;
    for (std::size_t i = 0; i < datesOIS.size(); ++i) {
        datesOIS_t[i] += eps;
        Real v1 = priceMulticurveBootstrappingSwap(
            depos, calendar, shortOis, datesOIS_t, longTermOIS, todaysDate, termStructureDayCounter,
            d6MRate, fra, swapRates, settlementDate, maturity, nominal, fixedRate, spread,
            lengthInYears);
        gradient.push_back((v1 - value) / eps);
        datesOIS_t[i] -= eps;
    }

    auto longTermOIS_t = longTermOIS;
    for (std::size_t i = 0; i < longTermOIS.size(); ++i) {
        longTermOIS_t[i] += eps;
        Real v1 = priceMulticurveBootstrappingSwap(
            depos, calendar, shortOis, datesOIS, longTermOIS_t, todaysDate, termStructureDayCounter,
            d6MRate, fra, swapRates, settlementDate, maturity, nominal, fixedRate, spread,
            lengthInYears);
        gradient.push_back((v1 - value) / eps);
        longTermOIS_t[i] -= eps;
    }

    auto swapRates_t = swapRates;
    for (std::size_t i = 0; i < swapRates.size(); ++i) {
        swapRates_t[i] += eps;
        Real v1 = priceMulticurveBootstrappingSwap(depos, calendar, shortOis, datesOIS, longTermOIS,
                                                   todaysDate, termStructureDayCounter, d6MRate,
                                                   fra, swapRates_t, settlementDate, maturity,
                                                   nominal, fixedRate, spread, lengthInYears);
        gradient.push_back((v1 - value) / eps);
        swapRates_t[i] -= eps;
    }

    auto fra_t = fra;
    for (std::size_t i = 0; i < fra.size(); ++i) {
        fra_t[i] += eps;
        Real v1 = priceMulticurveBootstrappingSwap(depos, calendar, shortOis, datesOIS, longTermOIS,
                                                   todaysDate, termStructureDayCounter, d6MRate,
                                                   fra_t, swapRates, settlementDate, maturity,
                                                   nominal, fixedRate, spread, lengthInYears);
        gradient.push_back((v1 - value) / eps);
        fra_t[i] -= eps;
    }

    Real v1 = priceMulticurveBootstrappingSwap(depos, calendar, shortOis, datesOIS, longTermOIS,
                                               todaysDate, termStructureDayCounter, d6MRate, fra,
                                               swapRates, settlementDate, maturity, nominal,
                                               fixedRate + eps, spread, lengthInYears);
    gradient.push_back((v1 - value) / eps);

    v1 = priceMulticurveBootstrappingSwap(depos, calendar, shortOis, datesOIS, longTermOIS,
                                          todaysDate, termStructureDayCounter, d6MRate, fra,
                                          swapRates, settlementDate, maturity, nominal, fixedRate,
                                          spread + eps, lengthInYears);
    gradient.push_back((v1 - value) / eps);

    v1 = priceMulticurveBootstrappingSwap(depos, calendar, shortOis, datesOIS, longTermOIS,
                                          todaysDate, termStructureDayCounter, d6MRate, fra,
                                          swapRates, settlementDate, maturity, nominal + eps,
                                          fixedRate, spread, lengthInYears);
    gradient.push_back((v1 - value) / eps);

    v1 = priceMulticurveBootstrappingSwap(depos, calendar, shortOis, datesOIS, longTermOIS,
                                          todaysDate, termStructureDayCounter, d6MRate + eps, fra,
                                          swapRates, settlementDate, maturity, nominal, fixedRate,
                                          spread, lengthInYears);
    gradient.push_back((v1 - value) / eps);

    return value;
}


#endif


void printResults(Real v,
                  const std::vector<Real>& gradient,
                  Size nDepos,
                  Size nShortOis,
                  Size ndatesOIS,
                  Size nlongOIS,
                  Size nSwapRates,
                  Size nFRA) {
    std::cout << "Price                                 = " << v << "\n";
    std::cout << "Sensitivities w.r.t. depo quotes      = [";
    Size cnt = 0;
    for (Size i = cnt; i < cnt + nDepos; ++i)
        std::cout << gradient[i] << ", ";
    cnt += nDepos;
    std::cout << "]\n";
    std::cout << "Sensitivities w.r.t. short OIS quotes = [";
    for (Size i = cnt; i < cnt + nShortOis; ++i)
        std::cout << gradient[i] << ", ";
    cnt += nShortOis;
    std::cout << "]\n";
    std::cout << "Sensitivities w.r.t. date OIS quotes = [";
    for (Size i = cnt; i < cnt + ndatesOIS; ++i)
        std::cout << gradient[i] << ", ";
    cnt += ndatesOIS;
    std::cout << "]\n";
    std::cout << "Sensitivities w.r.t. long OIS quotes = [";
    for (Size i = cnt; i < cnt + nlongOIS; ++i)
        std::cout << gradient[i] << ", ";
    cnt += nlongOIS;
    std::cout << "]\n";
    std::cout << "Sensitivities w.r.t. swap quotes    = [";
    for (Size i = cnt; i < cnt + nSwapRates; ++i)
        std::cout << gradient[i] << ", ";
    cnt += nSwapRates;
    std::cout << "]\n";
    std::cout << "Sensitivities w.r.t. FRA quotes     = [";
    for (Size i = cnt; i < cnt + nFRA; ++i)
        std::cout << gradient[i] << ", ";
    cnt += nFRA;
    std::cout << "]\n";
    std::cout << "Sensitivity w.r.t. swap fixed Rate  = " << gradient[cnt++] << "\n";
    std::cout << "Sensitivity w.r.t. swap spread      = " << gradient[cnt++] << "\n";
    std::cout << "Sensitivity w.r.t. swap nominal     = " << gradient[cnt++] << "\n";
    std::cout << "Sensitivity w.r.t. swap d6Mrate     = " << gradient[cnt++] << "\n";
}

int main(int, char*[]) {

    try {

        Calendar calendar = TARGET();

        Date todaysDate(11, December, 2012);
        Settings::instance().evaluationDate() = todaysDate;
        todaysDate = Settings::instance().evaluationDate();

        Integer fixingDays = 2;
        Date settlementDate = calendar.advance(todaysDate, fixingDays, Days);
        // must be a business day
        settlementDate = calendar.adjust(settlementDate);

        // EONIA CURVE
        DayCounter termStructureDayCounter = Actual365Fixed();
        std::vector<Real> depos = {0.0004, 0.0004, 0.0004};
        std::vector<Real> shortOis = {0.00070, 0.00069, 0.00078, 0.00074};
        std::vector<Real> datesOIS = {0.000460, 0.000160, -0.000070, -0.000130, -0.000140};
        std::vector<Real> longTermOIS = {0.00002, 0.00008, 0.00021, 0.00036, 0.00127, 0.00274,
                                         0.00456, 0.00647, 0.00827, 0.00996, 0.01147, 0.0128,
                                         0.01404, 0.01516, 0.01764, 0.01939, 0.02003, 0.02038};
        Real d6MRate = 0.00312;
        std::vector<Real> fra = {0.002930, 0.002720, 0.002600, 0.002560, 0.002520, 0.002480,
                                 0.002540, 0.002610, 0.002670, 0.002790, 0.002910, 0.003030,
                                 0.003180, 0.003350, 0.003520, 0.003710, 0.003890, 0.004090};
        std::vector<Real> swapRates = {0.004240, 0.005760, 0.007620, 0.009540, 0.011350, 0.013030,
                                       0.014520, 0.015840, 0.018090, 0.020370, 0.021870, 0.022340,
                                       0.022560, 0.022950, 0.023480, 0.024210, 0.024630};
        Real nominal = 1000000.0;
        Spread spread = 0.0;
        Rate fixedRate = 0.007;
        Integer lengthInYears = 5;
        Date maturity = settlementDate + lengthInYears * Years;

        std::cout.precision(5);
        constexpr int N = 20;


        std::cout << "Pricing swap with multicurve bootstrapping without sensitivities...\n";
        Real v = 0.0;
        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < N; ++i) {
            v = priceMulticurveBootstrappingSwap(depos, calendar, shortOis, datesOIS, longTermOIS,
                                                 todaysDate, termStructureDayCounter, d6MRate, fra,
                                                 swapRates, settlementDate, maturity, nominal,
                                                 fixedRate, spread, lengthInYears);
        }
        auto end = std::chrono::high_resolution_clock::now();
        auto time_plain =
            static_cast<double>(
                std::chrono::duration_cast<std::chrono::microseconds>(end - start).count()) *
            1e-3 / N;
        std::cout << "Value = " << v << "\n";

        // pricing with AAD
        std::vector<Real> gradient;
        std::cout << "Pricing swap with multicurve bootstrapping with sensitivities...\n";
        Real v2 = 0.0;
        start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < N; ++i) {
            v2 = priceWithSensi(depos, calendar, shortOis, datesOIS, longTermOIS, todaysDate,
                                termStructureDayCounter, d6MRate, fra, swapRates, settlementDate,
                                maturity, nominal, fixedRate, spread, lengthInYears, gradient);
        }
        end = std::chrono::high_resolution_clock::now();
        auto time_sensi =
            static_cast<double>(
                std::chrono::duration_cast<std::chrono::microseconds>(end - start).count()) *
            1e-3 / N;

        printResults(v2, gradient, depos.size(), shortOis.size(), datesOIS.size(),
                     longTermOIS.size(), swapRates.size(), fra.size());

        std::cout << "Plain time : " << time_plain << "ms\n"
                  << "Sensi time : " << time_sensi << "ms\n"
                  << "Factor     : " << time_sensi / time_plain << "x\n";
        return 0;
    } catch (std::exception& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "unknown error" << std::endl;
        return 1;
    }
}
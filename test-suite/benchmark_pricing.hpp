/*******************************************************************************
 *
 *  QuantLib-Risks Swaption Benchmark - Templated Pricing Functions
 *
 *  Core pricing logic templated on the Real type (double or AReal<double>).
 *  This allows the same code to be used for both FD and AAD benchmarks.
 *
 *  Copyright (C) 2025 Xcelerit Computing Limited
 *  SPDX-License-Identifier: AGPL-3.0-or-later
 *
 ******************************************************************************/

#ifndef BENCHMARK_PRICING_HPP
#define BENCHMARK_PRICING_HPP

#include "benchmark_common.hpp"

// QuantLib includes
#include <ql/indexes/ibor/euribor.hpp>
#include <ql/indexes/ibor/eonia.hpp>
#include <ql/instruments/vanillaswap.hpp>
#include <ql/pricingengines/swap/discountingswapengine.hpp>
#include <ql/termstructures/yield/piecewiseyieldcurve.hpp>
#include <ql/termstructures/yield/ratehelpers.hpp>
#include <ql/termstructures/yield/oisratehelper.hpp>
#include <ql/termstructures/yield/zerocurve.hpp>
#include <ql/time/calendars/target.hpp>
#include <ql/time/daycounters/actual360.hpp>
#include <ql/time/daycounters/thirty360.hpp>

// LMM Monte Carlo includes
#include <ql/legacy/libormarketmodels/lfmcovarproxy.hpp>
#include <ql/legacy/libormarketmodels/liborforwardmodel.hpp>
#include <ql/legacy/libormarketmodels/lmexpcorrmodel.hpp>
#include <ql/legacy/libormarketmodels/lmlinexpvolmodel.hpp>
#include <ql/math/randomnumbers/rngtraits.hpp>
#include <ql/methods/montecarlo/multipathgenerator.hpp>

namespace benchmark {

// ============================================================================
// Helper: Extract value from Real type (works for both double and AReal)
// ============================================================================

template <typename T>
inline double extractValue(const T& x)
{
    if constexpr (std::is_same_v<T, double>)
        return x;
    else
        return value(x);  // XAD's value() function
}

// ============================================================================
// Helper: Create IborIndex with ZeroCurve (templated)
// ============================================================================

template <typename RealType>
ext::shared_ptr<IborIndex> makeIndexT(std::vector<Date> dates,
                                       const std::vector<RealType>& rates)
{
    DayCounter dayCounter = Actual360();
    RelinkableHandle<YieldTermStructure> termStructure;
    ext::shared_ptr<IborIndex> index(new Euribor6M(termStructure));

    Date todaysDate = index->fixingCalendar().adjust(Date(4, September, 2005));
    Settings::instance().evaluationDate() = todaysDate;

    dates[0] = index->fixingCalendar().advance(todaysDate, index->fixingDays(), Days);

    // Convert rates to Rate type for ZeroCurve
    std::vector<Rate> ratesForCurve;
    for (const auto& r : rates)
        ratesForCurve.push_back(r);

    termStructure.linkTo(ext::shared_ptr<YieldTermStructure>(
        new ZeroCurve(dates, ratesForCurve, dayCounter)));

    return index;
}

// ============================================================================
// Pre-computed LMM Setup (reusable across pricing calls)
// ============================================================================

struct LMMSetup
{
    Calendar calendar;
    Date todaysDate;
    Date settlementDate;
    DayCounter dayCounter;
    Integer fixingDays;

    TimeGrid grid;
    std::vector<Size> location;
    Size numFactors;
    Size exerciseStep;
    Size fullGridSteps;
    Size fullGridRandoms;

    Schedule schedule;
    std::vector<double> accrualStart;
    std::vector<double> accrualEnd;

    // Pre-generated random numbers
    std::vector<std::vector<double>> allRandoms;
    Size maxPaths;

    LMMSetup(const BenchmarkConfig& config)
    {
        calendar = TARGET();
        todaysDate = Date(4, September, 2005);
        Settings::instance().evaluationDate() = todaysDate;
        fixingDays = 2;
        settlementDate = calendar.adjust(calendar.advance(todaysDate, fixingDays, Days));
        dayCounter = Actual360();

        // Build base curve for grid setup
        std::vector<Rate> baseZeroRates = {config.depoRates[0], config.swapRates.back()};
        std::vector<Date> baseDates = {settlementDate, settlementDate + config.curveEndYears * Years};
        auto baseIndex = makeIndexT(baseDates, baseZeroRates);

        ext::shared_ptr<LiborForwardModelProcess> baseProcess(
            new LiborForwardModelProcess(config.size, baseIndex));
        baseProcess->setCovarParam(ext::shared_ptr<LfmCovarianceParameterization>(
            new LfmCovarianceProxy(
                ext::make_shared<LmLinearExponentialVolatilityModel>(
                    baseProcess->fixingTimes(), 0.291, 1.483, 0.116, 0.00001),
                ext::make_shared<LmExponentialCorrelationModel>(config.size, 0.5))));

        // Grid setup
        std::vector<Time> fixingTimes = baseProcess->fixingTimes();
        grid = TimeGrid(fixingTimes.begin(), fixingTimes.end(), config.steps);

        for (Size idx = 0; idx < fixingTimes.size(); ++idx)
        {
            location.push_back(
                std::find(grid.begin(), grid.end(), fixingTimes[idx]) - grid.begin());
        }

        numFactors = baseProcess->factors();
        exerciseStep = location[config.i_opt];
        fullGridSteps = grid.size() - 1;
        fullGridRandoms = fullGridSteps * numFactors;

        // Swap schedule
        BusinessDayConvention convention = baseIndex->businessDayConvention();
        Date fwdStart = settlementDate + Period(6 * config.i_opt, Months);
        Date fwdMaturity = fwdStart + Period(6 * config.j_opt, Months);
        schedule = Schedule(fwdStart, fwdMaturity, baseIndex->tenor(), calendar,
                            convention, convention, DateGeneration::Forward, false);

        // Accrual periods
        accrualStart.resize(config.size);
        accrualEnd.resize(config.size);
        for (Size k = 0; k < config.size; ++k)
        {
            accrualStart[k] = extractValue(baseProcess->accrualStartTimes()[k]);
            accrualEnd[k] = extractValue(baseProcess->accrualEndTimes()[k]);
        }

        // Pre-generate random numbers
        maxPaths = static_cast<Size>(*std::max_element(
            config.pathCounts.begin(), config.pathCounts.end()));

        std::cout << "  Generating " << maxPaths << " x " << fullGridRandoms
                  << " random numbers..." << std::flush;

        typedef PseudoRandom::rsg_type rsg_type;
        rsg_type rsg = PseudoRandom::make_sequence_generator(fullGridRandoms, BigNatural(42));

        allRandoms.resize(maxPaths);
        for (Size n = 0; n < maxPaths; ++n)
        {
            allRandoms[n].resize(fullGridRandoms);
            const auto& seq = rsg.nextSequence();
            for (Size m = 0; m < fullGridRandoms; ++m)
            {
                allRandoms[n][m] = extractValue(seq.value[m]);
            }
        }
        std::cout << " Done." << std::endl;
    }
};

// ============================================================================
// Templated Monte Carlo Pricing Function
// ============================================================================

template <typename RealType>
RealType priceSwaption(const BenchmarkConfig& config,
                       const LMMSetup& setup,
                       const std::vector<RealType>& depositRates,
                       const std::vector<RealType>& swapRates,
                       Size nrTrails)
{
    // Build curve from input rates
    RelinkableHandle<YieldTermStructure> euriborTS;
    auto euribor6m = ext::make_shared<Euribor6M>(euriborTS);
    euribor6m->addFixing(Date(2, September, 2005), 0.04);

    std::vector<ext::shared_ptr<RateHelper>> instruments;
    for (Size idx = 0; idx < config.numDeposits; ++idx)
    {
        auto depoQuote = ext::make_shared<SimpleQuote>(depositRates[idx]);
        instruments.push_back(ext::make_shared<DepositRateHelper>(
            Handle<Quote>(depoQuote), config.depoTenors[idx], setup.fixingDays,
            setup.calendar, ModifiedFollowing, true, setup.dayCounter));
    }
    for (Size idx = 0; idx < config.numSwaps; ++idx)
    {
        auto swapQuote = ext::make_shared<SimpleQuote>(swapRates[idx]);
        instruments.push_back(ext::make_shared<SwapRateHelper>(
            Handle<Quote>(swapQuote), config.swapTenors[idx],
            setup.calendar, Annual, Unadjusted, Thirty360(Thirty360::BondBasis),
            euribor6m));
    }

    auto yieldCurve = ext::make_shared<PiecewiseYieldCurve<ZeroYield, Linear>>(
        setup.settlementDate, instruments, setup.dayCounter);
    yieldCurve->enableExtrapolation();

    // Extract zero rates for LMM
    std::vector<Date> curveDates;
    std::vector<RealType> zeroRates;
    curveDates.push_back(setup.settlementDate);
    zeroRates.push_back(yieldCurve->zeroRate(setup.settlementDate, setup.dayCounter, Continuous).rate());
    Date endDate = setup.settlementDate + config.curveEndYears * Years;
    curveDates.push_back(endDate);
    zeroRates.push_back(yieldCurve->zeroRate(endDate, setup.dayCounter, Continuous).rate());

    // Convert to Rate for ZeroCurve
    std::vector<Rate> zeroRates_ql;
    for (const auto& r : zeroRates) zeroRates_ql.push_back(r);

    // Build LMM process
    RelinkableHandle<YieldTermStructure> termStructure;
    ext::shared_ptr<IborIndex> index(new Euribor6M(termStructure));
    index->addFixing(Date(2, September, 2005), 0.04);
    termStructure.linkTo(ext::make_shared<ZeroCurve>(curveDates, zeroRates_ql, setup.dayCounter));

    ext::shared_ptr<LiborForwardModelProcess> process(
        new LiborForwardModelProcess(config.size, index));
    process->setCovarParam(ext::shared_ptr<LfmCovarianceParameterization>(
        new LfmCovarianceProxy(
            ext::make_shared<LmLinearExponentialVolatilityModel>(
                process->fixingTimes(), 0.291, 1.483, 0.116, 0.00001),
            ext::make_shared<LmExponentialCorrelationModel>(config.size, 0.5))));

    // Get swap rate
    ext::shared_ptr<VanillaSwap> fwdSwap(
        new VanillaSwap(Swap::Receiver, 1.0,
                        setup.schedule, 0.05, setup.dayCounter,
                        setup.schedule, index, 0.0, index->dayCounter()));
    fwdSwap->setPricingEngine(ext::make_shared<DiscountingSwapEngine>(
        index->forwardingTermStructure()));
    RealType swapRate = fwdSwap->fairRate();

    Array initRates = process->initialValues();

    // Monte Carlo simulation
    RealType price = RealType(0.0);
    for (Size n = 0; n < nrTrails; ++n)
    {
        Array asset(config.size);
        for (Size k = 0; k < config.size; ++k)
            asset[k] = initRates[k];

        Array assetAtExercise(config.size);
        for (Size step = 1; step <= setup.fullGridSteps; ++step)
        {
            Size offset = (step - 1) * setup.numFactors;
            Time t = setup.grid[step - 1];
            Time dt = setup.grid.dt(step - 1);

            Array dw(setup.numFactors);
            for (Size f = 0; f < setup.numFactors; ++f)
                dw[f] = setup.allRandoms[n][offset + f];

            asset = process->evolve(t, asset, dt, dw);

            if (step == setup.exerciseStep)
            {
                for (Size k = 0; k < config.size; ++k)
                    assetAtExercise[k] = asset[k];
            }
        }

        // Discount factors
        Array dis(config.size);
        RealType df = RealType(1.0);
        for (Size k = 0; k < config.size; ++k)
        {
            RealType accrual = setup.accrualEnd[k] - setup.accrualStart[k];
            df = df / (RealType(1.0) + assetAtExercise[k] * accrual);
            dis[k] = df;
        }

        // NPV
        RealType npv = RealType(0.0);
        for (Size m = config.i_opt; m < config.i_opt + config.j_opt; ++m)
        {
            RealType accrual = setup.accrualEnd[m] - setup.accrualStart[m];
            npv += (swapRate - assetAtExercise[m]) * accrual * dis[m];
        }

        // max(npv, 0) - use smooth approximation for AD types
        if constexpr (std::is_same_v<RealType, double>)
        {
            price += std::max(npv, 0.0);
        }
        else
        {
            // For AD types, use value() comparison to determine branch
            // This gives correct forward value; derivative uses indicator function
            if (value(npv) > 0.0)
                price += npv;
            // else: price += 0, which is a no-op
        }
    }

    return price / RealType(static_cast<double>(nrTrails));
}

// ============================================================================
// Dual-Curve Monte Carlo Pricing Function (Production config)
// Uses separate forecasting (Euribor) and discounting (OIS) curves
// ============================================================================

template <typename RealType>
RealType priceSwaptionDualCurve(const BenchmarkConfig& config,
                                const LMMSetup& setup,
                                const std::vector<RealType>& depositRates,
                                const std::vector<RealType>& swapRates,
                                const std::vector<RealType>& oisDepoRates,
                                const std::vector<RealType>& oisSwapRates,
                                Size nrTrails)
{
    // ========================================================================
    // Build FORECASTING curve (Euribor deposits + swaps)
    // ========================================================================
    RelinkableHandle<YieldTermStructure> euriborTS;
    auto euribor6m = ext::make_shared<Euribor6M>(euriborTS);
    euribor6m->addFixing(Date(2, September, 2005), 0.04);

    std::vector<ext::shared_ptr<RateHelper>> forecastingInstruments;
    for (Size idx = 0; idx < config.numDeposits; ++idx)
    {
        auto depoQuote = ext::make_shared<SimpleQuote>(depositRates[idx]);
        forecastingInstruments.push_back(ext::make_shared<DepositRateHelper>(
            Handle<Quote>(depoQuote), config.depoTenors[idx], setup.fixingDays,
            setup.calendar, ModifiedFollowing, true, setup.dayCounter));
    }
    for (Size idx = 0; idx < config.numSwaps; ++idx)
    {
        auto swapQuote = ext::make_shared<SimpleQuote>(swapRates[idx]);
        forecastingInstruments.push_back(ext::make_shared<SwapRateHelper>(
            Handle<Quote>(swapQuote), config.swapTenors[idx],
            setup.calendar, Annual, Unadjusted, Thirty360(Thirty360::BondBasis),
            euribor6m));
    }

    auto forecastingCurve = ext::make_shared<PiecewiseYieldCurve<ZeroYield, Linear>>(
        setup.settlementDate, forecastingInstruments, setup.dayCounter);
    forecastingCurve->enableExtrapolation();
    euriborTS.linkTo(forecastingCurve);

    // ========================================================================
    // Build DISCOUNTING curve (OIS deposits + swaps)
    // ========================================================================
    RelinkableHandle<YieldTermStructure> oisTS;
    auto eonia = ext::make_shared<Eonia>(oisTS);

    std::vector<ext::shared_ptr<RateHelper>> discountingInstruments;
    for (Size idx = 0; idx < config.numOisDeposits; ++idx)
    {
        auto oisDepoQuote = ext::make_shared<SimpleQuote>(oisDepoRates[idx]);
        discountingInstruments.push_back(ext::make_shared<DepositRateHelper>(
            Handle<Quote>(oisDepoQuote), config.oisDepoTenors[idx], setup.fixingDays,
            setup.calendar, ModifiedFollowing, true, Actual360()));
    }
    for (Size idx = 0; idx < config.numOisSwaps; ++idx)
    {
        auto oisSwapQuote = ext::make_shared<SimpleQuote>(oisSwapRates[idx]);
        discountingInstruments.push_back(ext::make_shared<OISRateHelper>(
            2, config.oisSwapTenors[idx], Handle<Quote>(oisSwapQuote), eonia));
    }

    auto discountingCurve = ext::make_shared<PiecewiseYieldCurve<ZeroYield, Linear>>(
        setup.settlementDate, discountingInstruments, setup.dayCounter);
    discountingCurve->enableExtrapolation();
    oisTS.linkTo(discountingCurve);

    // ========================================================================
    // Extract zero rates for LMM from forecasting curve
    // ========================================================================
    std::vector<Date> curveDates;
    std::vector<RealType> zeroRates;
    curveDates.push_back(setup.settlementDate);
    zeroRates.push_back(forecastingCurve->zeroRate(setup.settlementDate, setup.dayCounter, Continuous).rate());
    Date endDate = setup.settlementDate + config.curveEndYears * Years;
    curveDates.push_back(endDate);
    zeroRates.push_back(forecastingCurve->zeroRate(endDate, setup.dayCounter, Continuous).rate());

    // Convert to Rate for ZeroCurve
    std::vector<Rate> zeroRates_ql;
    for (const auto& r : zeroRates) zeroRates_ql.push_back(r);

    // ========================================================================
    // Build LMM process using forecasting curve
    // ========================================================================
    RelinkableHandle<YieldTermStructure> termStructure;
    ext::shared_ptr<IborIndex> index(new Euribor6M(termStructure));
    index->addFixing(Date(2, September, 2005), 0.04);
    termStructure.linkTo(ext::make_shared<ZeroCurve>(curveDates, zeroRates_ql, setup.dayCounter));

    ext::shared_ptr<LiborForwardModelProcess> process(
        new LiborForwardModelProcess(config.size, index));
    process->setCovarParam(ext::shared_ptr<LfmCovarianceParameterization>(
        new LfmCovarianceProxy(
            ext::make_shared<LmLinearExponentialVolatilityModel>(
                process->fixingTimes(), 0.291, 1.483, 0.116, 0.00001),
            ext::make_shared<LmExponentialCorrelationModel>(config.size, 0.5))));

    // ========================================================================
    // Get swap rate (using forecasting curve for forwards, discounting for NPV)
    // ========================================================================
    ext::shared_ptr<VanillaSwap> fwdSwap(
        new VanillaSwap(Swap::Receiver, 1.0,
                        setup.schedule, 0.05, setup.dayCounter,
                        setup.schedule, index, 0.0, index->dayCounter()));
    // Use OIS curve for discounting
    fwdSwap->setPricingEngine(ext::make_shared<DiscountingSwapEngine>(
        Handle<YieldTermStructure>(discountingCurve)));
    RealType swapRate = fwdSwap->fairRate();

    Array initRates = process->initialValues();

    // ========================================================================
    // Extract discount factors from OIS curve at exercise date
    // ========================================================================
    std::vector<RealType> oisDiscountFactors(config.size);
    for (Size k = 0; k < config.size; ++k)
    {
        Time t = setup.accrualEnd[k];
        oisDiscountFactors[k] = discountingCurve->discount(t);
    }

    // ========================================================================
    // Monte Carlo simulation
    // ========================================================================
    RealType price = RealType(0.0);
    for (Size n = 0; n < nrTrails; ++n)
    {
        Array asset(config.size);
        for (Size k = 0; k < config.size; ++k)
            asset[k] = initRates[k];

        Array assetAtExercise(config.size);
        for (Size step = 1; step <= setup.fullGridSteps; ++step)
        {
            Size offset = (step - 1) * setup.numFactors;
            Time t = setup.grid[step - 1];
            Time dt = setup.grid.dt(step - 1);

            Array dw(setup.numFactors);
            for (Size f = 0; f < setup.numFactors; ++f)
                dw[f] = setup.allRandoms[n][offset + f];

            asset = process->evolve(t, asset, dt, dw);

            if (step == setup.exerciseStep)
            {
                for (Size k = 0; k < config.size; ++k)
                    assetAtExercise[k] = asset[k];
            }
        }

        // NPV calculation using OIS discount factors for discounting
        // In dual-curve framework, we use OIS curve for discounting cashflows
        RealType npv = RealType(0.0);
        for (Size m = config.i_opt; m < config.i_opt + config.j_opt; ++m)
        {
            RealType accrual = setup.accrualEnd[m] - setup.accrualStart[m];
            npv += (swapRate - assetAtExercise[m]) * accrual * oisDiscountFactors[m];
        }

        // max(npv, 0) - use smooth approximation for AD types
        if constexpr (std::is_same_v<RealType, double>)
        {
            price += std::max(npv, 0.0);
        }
        else
        {
            // For AD types, use value() comparison to determine branch
            // This gives correct forward value; derivative uses indicator function
            if (value(npv) > 0.0)
                price += npv;
            // else: price += 0, which is a no-op
        }
    }

    return price / RealType(static_cast<double>(nrTrails));
}

// ============================================================================
// Unified Pricing Dispatcher (selects single or dual curve based on config)
// ============================================================================

template <typename RealType>
RealType priceSwaptionAuto(const BenchmarkConfig& config,
                           const LMMSetup& setup,
                           const std::vector<RealType>& depositRates,
                           const std::vector<RealType>& swapRates,
                           const std::vector<RealType>& oisDepoRates,
                           const std::vector<RealType>& oisSwapRates,
                           Size nrTrails)
{
    if (config.useDualCurve)
    {
        return priceSwaptionDualCurve(config, setup,
                                      depositRates, swapRates,
                                      oisDepoRates, oisSwapRates,
                                      nrTrails);
    }
    else
    {
        return priceSwaption(config, setup, depositRates, swapRates, nrTrails);
    }
}

} // namespace benchmark

#endif // BENCHMARK_PRICING_HPP

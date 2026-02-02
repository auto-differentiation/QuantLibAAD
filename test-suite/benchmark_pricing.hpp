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
// Shared Payoff Recording - used by FD, JIT, and XAD-Split
// ============================================================================

// Variables holder for payoff computation (templated on AD type)
template <typename ADType>
struct PayoffVariables {
    std::vector<ADType> initRates;
    ADType swapRate;
    std::vector<ADType> oisDiscounts;  // Empty for single-curve
    std::vector<ADType> randoms;       // For JIT only; XAD-Split uses plain doubles
};

// Custom LMM evolve function using LOCAL arrays instead of process's mutable members.
// This fixes XAD-Split tape recording issues caused by mutable state in process->evolve().
// Implements the same predictor-corrector scheme as LiborForwardModelProcess::evolve().
template <typename ADType, typename RandomsType>
void evolveLMM(
    std::vector<ADType>& asset,
    const ext::shared_ptr<LiborForwardModelProcess>& process,
    double t0,
    double dt,
    Size numFactors,
    const RandomsType& randoms,
    Size randomOffset)
{
    const Size size = asset.size();
    const Size m = process->nextIndexReset(t0);
    const ADType sdt = ADType(std::sqrt(dt));

    // Get covariance parameters as plain doubles (these are constant, not AD-active).
    // Extracting to double avoids type mismatches when ADType=double in XAD builds
    // where Matrix elements are AReal<double>.
    Matrix diffM = process->covarParam()->diffusion(t0, Array());
    Matrix covM = process->covarParam()->covariance(t0, Array());
    std::vector<std::vector<double>> diff(size, std::vector<double>(numFactors));
    std::vector<std::vector<double>> covariance(size, std::vector<double>(size));
    for (Size i = 0; i < size; ++i)
    {
        for (Size f = 0; f < numFactors; ++f)
            diff[i][f] = extractValue(diffM[i][f]);
        for (Size j = 0; j < size; ++j)
            covariance[i][j] = extractValue(covM[i][j]);
    }

    // Get accrual periods (constants, not AD)
    const std::vector<Time>& accrualStart = process->accrualStartTimes();
    const std::vector<Time>& accrualEnd = process->accrualEndTimes();
    std::vector<double> tau(size);
    for (Size k = 0; k < size; ++k)
    {
        tau[k] = extractValue(accrualEnd[k]) - extractValue(accrualStart[k]);
    }

    // LOCAL arrays for predictor-corrector (no mutable state!)
    std::vector<ADType> m1(size);
    std::vector<ADType> m2(size);

    // Build dw from randoms
    std::vector<ADType> dw(numFactors);
    for (Size f = 0; f < numFactors; ++f)
        dw[f] = randoms[randomOffset + f];

    for (Size k = m; k < size; ++k)
    {
        // Predictor step
        const ADType y = tau[k] * asset[k];
        m1[k] = y / (ADType(1.0) + y);

        // Drift term using m1
        ADType drift1 = ADType(0.0);
        for (Size j = m; j <= k; ++j)
            drift1 = drift1 + m1[j] * covariance[j][k];
        const ADType d = (drift1 - ADType(0.5) * covariance[k][k]) * dt;

        // Diffusion term
        ADType r = ADType(0.0);
        for (Size f = 0; f < numFactors; ++f)
            r = r + diff[k][f] * dw[f];
        r = r * sdt;

        // Corrector step
        const ADType x = y * exp(d + r);
        m2[k] = x / (ADType(1.0) + x);

        // Drift term using m2
        ADType drift2 = ADType(0.0);
        for (Size j = m; j <= k; ++j)
            drift2 = drift2 + m2[j] * covariance[j][k];

        // Final evolved rate
        asset[k] = asset[k] * exp(ADType(0.5) * (d + (drift2 - ADType(0.5) * covariance[k][k]) * dt) + r);
    }
}

// Compute the MC path payoff (shared between FD, JIT, and XAD-Split)
// RandomsType: std::vector<double> for FD/XAD-Split, std::vector<xad::AD> for JIT
// UseCustomEvolve: true = use evolveLMM (for JIT graph recording, avoids mutable state)
//                  false = use process->evolve() (for XAD-Split/tape, faster due to internal buffers)
template <typename ADType, bool UseDualCurve, bool UseCustomEvolve = false, typename RandomsType>
ADType computePathPayoff(
    const BenchmarkConfig& config,
    const LMMSetup& setup,
    const ext::shared_ptr<LiborForwardModelProcess>& process,
    PayoffVariables<ADType>& vars,
    const RandomsType& randoms)
{
    std::vector<ADType> asset(config.size);
    std::vector<ADType> assetAtExercise(config.size);
    for (Size k = 0; k < config.size; ++k)
        asset[k] = vars.initRates[k];

    for (Size step = 1; step <= setup.fullGridSteps; ++step)
    {
        Size offset = (step - 1) * setup.numFactors;

        if constexpr (UseCustomEvolve)
        {
            // Custom evolveLMM with LOCAL arrays — needed for JIT graph recording
            // (process->evolve() uses mutable members that break JIT's static graph)
            double t0 = extractValue(setup.grid[step - 1]);
            double dt = extractValue(setup.grid.dt(step - 1));
            evolveLMM(asset, process, t0, dt, setup.numFactors, randoms, offset);
        }
        else
        {
            // Use process->evolve() directly — faster, uses pre-allocated internal buffers
            Time t = setup.grid[step - 1];
            Time dt = setup.grid.dt(step - 1);

            Array dw(setup.numFactors);
            for (Size f = 0; f < setup.numFactors; ++f)
                dw[f] = randoms[offset + f];

            // Convert vector<ADType> -> Array (Real), evolve, convert back
            Array assetArr(config.size);
            for (Size k = 0; k < config.size; ++k)
                assetArr[k] = asset[k];
            assetArr = process->evolve(t, assetArr, dt, dw);
            for (Size k = 0; k < config.size; ++k)
            {
                if constexpr (std::is_same_v<ADType, Real>)
                    asset[k] = assetArr[k];           // AReal = AReal (preserves tape)
                else
                    asset[k] = extractValue(assetArr[k]); // double = value(AReal)
            }
        }

        if (step == setup.exerciseStep)
            for (Size k = 0; k < config.size; ++k)
                assetAtExercise[k] = asset[k];
    }

    std::vector<ADType> dis(config.size);
    if constexpr (UseDualCurve)
    {
        // Dual-curve: use OIS discount factors directly
        for (Size k = 0; k < config.size; ++k)
            dis[k] = vars.oisDiscounts[k];
    }
    else
    {
        // Single-curve: compute discount factors from forward rates
        ADType df = ADType(1.0);
        for (Size k = 0; k < config.size; ++k)
        {
            double accrual = setup.accrualEnd[k] - setup.accrualStart[k];
            df = df / (ADType(1.0) + assetAtExercise[k] * accrual);
            dis[k] = df;
        }
    }

    ADType npv = ADType(0.0);
    for (Size m = config.i_opt; m < config.i_opt + config.j_opt; ++m)
    {
        double accrual = setup.accrualEnd[m] - setup.accrualStart[m];
        npv = npv + (vars.swapRate - assetAtExercise[m]) * accrual * dis[m];
    }
    return npv;
}

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

// ============================================================================
// PricingSetup: Persistent QuantLib objects with SimpleQuote handles for FD
// ============================================================================
// Builds QuantLib curves and instruments ONCE, then supports efficient
// bump-and-revalue by changing SimpleQuote values (triggers lazy re-bootstrap).

template <bool UseDualCurve>
struct PricingSetup
{
    const BenchmarkConfig& config;
    const LMMSetup& setup;

    // SimpleQuote handles for all market quotes
    std::vector<ext::shared_ptr<SimpleQuote>> depoQuotes;
    std::vector<ext::shared_ptr<SimpleQuote>> swapQuotes;
    std::vector<ext::shared_ptr<SimpleQuote>> oisDepoQuotes;
    std::vector<ext::shared_ptr<SimpleQuote>> oisSwapQuotes;

    // Persistent curve objects (lazy re-bootstrap on quote change)
    ext::shared_ptr<PiecewiseYieldCurve<ZeroYield, Linear>> forecastingCurve;
    ext::shared_ptr<PiecewiseYieldCurve<ZeroYield, Linear>> discountingCurve;

    // LMM process infrastructure (built once, reuses curves via handles)
    RelinkableHandle<YieldTermStructure> termStructure;
    ext::shared_ptr<IborIndex> index;
    ext::shared_ptr<LiborForwardModelProcess> process;
    ext::shared_ptr<VanillaSwap> fwdSwap;

    // Cached intermediates for MC (refreshed after each bump)
    std::vector<double> initRates;
    double swapRateVal;
    std::vector<double> oisDiscountFactors;

    PricingSetup(const BenchmarkConfig& cfg, const LMMSetup& s)
        : config(cfg), setup(s)
    {
        build();
    }

    void build()
    {
        // ================================================================
        // Build FORECASTING curve with SimpleQuote handles
        // ================================================================
        RelinkableHandle<YieldTermStructure> euriborTS;
        auto euribor6m = ext::make_shared<Euribor6M>(euriborTS);
        euribor6m->addFixing(Date(2, September, 2005), 0.04);

        std::vector<ext::shared_ptr<RateHelper>> forecastingInstruments;
        depoQuotes.resize(config.numDeposits);
        for (Size idx = 0; idx < config.numDeposits; ++idx)
        {
            depoQuotes[idx] = ext::make_shared<SimpleQuote>(config.depoRates[idx]);
            forecastingInstruments.push_back(ext::make_shared<DepositRateHelper>(
                Handle<Quote>(depoQuotes[idx]), config.depoTenors[idx], setup.fixingDays,
                setup.calendar, ModifiedFollowing, true, setup.dayCounter));
        }
        swapQuotes.resize(config.numSwaps);
        for (Size idx = 0; idx < config.numSwaps; ++idx)
        {
            swapQuotes[idx] = ext::make_shared<SimpleQuote>(config.swapRates[idx]);
            forecastingInstruments.push_back(ext::make_shared<SwapRateHelper>(
                Handle<Quote>(swapQuotes[idx]), config.swapTenors[idx],
                setup.calendar, Annual, Unadjusted, Thirty360(Thirty360::BondBasis),
                euribor6m));
        }

        forecastingCurve = ext::make_shared<PiecewiseYieldCurve<ZeroYield, Linear>>(
            setup.settlementDate, forecastingInstruments, setup.dayCounter);
        forecastingCurve->enableExtrapolation();
        euriborTS.linkTo(forecastingCurve);

        // ================================================================
        // Build DISCOUNTING curve (OIS) if dual-curve
        // ================================================================
        if constexpr (UseDualCurve)
        {
            RelinkableHandle<YieldTermStructure> oisTS;
            auto eonia = ext::make_shared<Eonia>(oisTS);

            std::vector<ext::shared_ptr<RateHelper>> discountingInstruments;
            oisDepoQuotes.resize(config.numOisDeposits);
            for (Size idx = 0; idx < config.numOisDeposits; ++idx)
            {
                oisDepoQuotes[idx] = ext::make_shared<SimpleQuote>(config.oisDepoRates[idx]);
                discountingInstruments.push_back(ext::make_shared<DepositRateHelper>(
                    Handle<Quote>(oisDepoQuotes[idx]), config.oisDepoTenors[idx], setup.fixingDays,
                    setup.calendar, ModifiedFollowing, true, Actual360()));
            }
            oisSwapQuotes.resize(config.numOisSwaps);
            for (Size idx = 0; idx < config.numOisSwaps; ++idx)
            {
                oisSwapQuotes[idx] = ext::make_shared<SimpleQuote>(config.oisSwapRates[idx]);
                discountingInstruments.push_back(ext::make_shared<OISRateHelper>(
                    2, config.oisSwapTenors[idx], Handle<Quote>(oisSwapQuotes[idx]), eonia));
            }

            discountingCurve = ext::make_shared<PiecewiseYieldCurve<ZeroYield, Linear>>(
                setup.settlementDate, discountingInstruments, setup.dayCounter);
            discountingCurve->enableExtrapolation();
            oisTS.linkTo(discountingCurve);
        }

        // ================================================================
        // Build LMM process from 2-point ZeroCurve (derived from forecasting)
        // ================================================================
        index = ext::shared_ptr<IborIndex>(new Euribor6M(termStructure));
        index->addFixing(Date(2, September, 2005), 0.04);

        rebuildZeroCurve();

        process = ext::shared_ptr<LiborForwardModelProcess>(
            new LiborForwardModelProcess(config.size, index));
        process->setCovarParam(ext::shared_ptr<LfmCovarianceParameterization>(
            new LfmCovarianceProxy(
                ext::make_shared<LmLinearExponentialVolatilityModel>(
                    process->fixingTimes(), 0.291, 1.483, 0.116, 0.00001),
                ext::make_shared<LmExponentialCorrelationModel>(config.size, 0.5))));

        // ================================================================
        // Build swap for fair rate computation
        // ================================================================
        fwdSwap = ext::shared_ptr<VanillaSwap>(
            new VanillaSwap(Swap::Receiver, 1.0,
                            setup.schedule, 0.05, setup.dayCounter,
                            setup.schedule, index, 0.0, index->dayCounter()));
        if constexpr (UseDualCurve)
        {
            fwdSwap->setPricingEngine(ext::make_shared<DiscountingSwapEngine>(
                Handle<YieldTermStructure>(discountingCurve)));
        }
        else
        {
            fwdSwap->setPricingEngine(ext::make_shared<DiscountingSwapEngine>(
                index->forwardingTermStructure()));
        }

        // ================================================================
        // Extract intermediates
        // ================================================================
        refreshIntermediates();
    }

    // Rebuild the 2-point ZeroCurve from the PiecewiseYieldCurve and relink
    void rebuildZeroCurve()
    {
        std::vector<Date> curveDates;
        curveDates.push_back(setup.settlementDate);
        Date endDate = setup.settlementDate + config.curveEndYears * Years;
        curveDates.push_back(endDate);

        std::vector<Rate> zeroRates;
        zeroRates.push_back(extractValue(
            forecastingCurve->zeroRate(setup.settlementDate, setup.dayCounter, Continuous).rate()));
        zeroRates.push_back(extractValue(
            forecastingCurve->zeroRate(endDate, setup.dayCounter, Continuous).rate()));

        termStructure.linkTo(ext::make_shared<ZeroCurve>(curveDates, zeroRates, setup.dayCounter));
    }

    // Re-extract process initial values, swap rate, and OIS discounts after a bump
    void refreshIntermediates()
    {
        rebuildZeroCurve();

        Array iv = process->initialValues();
        initRates.resize(config.size);
        for (Size k = 0; k < config.size; ++k)
            initRates[k] = extractValue(iv[k]);

        swapRateVal = extractValue(fwdSwap->fairRate());

        if constexpr (UseDualCurve)
        {
            oisDiscountFactors.resize(config.size);
            for (Size k = 0; k < config.size; ++k)
                oisDiscountFactors[k] = extractValue(discountingCurve->discount(setup.accrualEnd[k]));
        }
    }

    // Get the SimpleQuote for flat index q (depo, swap, ois depo, ois swap)
    ext::shared_ptr<SimpleQuote> getQuote(Size q) const
    {
        if (q < config.numDeposits)
            return depoQuotes[q];
        q -= config.numDeposits;
        if (q < config.numSwaps)
            return swapQuotes[q];
        if constexpr (UseDualCurve)
        {
            q -= config.numSwaps;
            if (q < config.numOisDeposits)
                return oisDepoQuotes[q];
            q -= config.numOisDeposits;
            return oisSwapQuotes[q];
        }
        return {};
    }

    // Bump quote q by eps, refresh intermediates, return old value
    double bump(Size q, double eps)
    {
        auto quote = getQuote(q);
        double oldVal = extractValue(quote->value());
        quote->setValue(oldVal + eps);
        refreshIntermediates();
        return oldVal;
    }

    // Reset quote q to oldVal, refresh intermediates
    void reset(Size q, double oldVal)
    {
        auto quote = getQuote(q);
        quote->setValue(oldVal);
        refreshIntermediates();
    }

    // Run MC simulation using Array objects directly (same as priceSwaption)
    // This avoids the vector<double> ↔ Array conversion overhead in computePathPayoff.
    double runMC(Size nrTrails) const
    {
        double price = 0.0;
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

            // Extract double values from Array (which holds Real=AReal in XAD builds)
            double npv = 0.0;
            if constexpr (UseDualCurve)
            {
                for (Size m = config.i_opt; m < config.i_opt + config.j_opt; ++m)
                {
                    double accrual = setup.accrualEnd[m] - setup.accrualStart[m];
                    npv += (swapRateVal - extractValue(assetAtExercise[m])) * accrual * oisDiscountFactors[m];
                }
            }
            else
            {
                double df = 1.0;
                std::vector<double> dis(config.size);
                for (Size k = 0; k < config.size; ++k)
                {
                    double accrual = setup.accrualEnd[k] - setup.accrualStart[k];
                    df = df / (1.0 + extractValue(assetAtExercise[k]) * accrual);
                    dis[k] = df;
                }
                for (Size m = config.i_opt; m < config.i_opt + config.j_opt; ++m)
                {
                    double accrual = setup.accrualEnd[m] - setup.accrualStart[m];
                    npv += (swapRateVal - extractValue(assetAtExercise[m])) * accrual * dis[m];
                }
            }

            price += std::max(npv, 0.0);
        }
        return price / static_cast<double>(nrTrails);
    }
};

} // namespace benchmark

#endif // BENCHMARK_PRICING_HPP

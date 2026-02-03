/*******************************************************************************
 *
 *  QuantLib-Risks Swaption Benchmark - AAD Runner
 *
 *  AAD benchmarks using XAD tape, Forge JIT, and Forge JIT-AVX.
 *  This executable is compiled WITH XAD (and optionally Forge).
 *
 *  Usage:
 *    ./benchmark_aad [--lite|--lite-extended|--production|--all] [--quick] [--xad-only]
 *
 *  Output format is designed to be parsed and combined with FD results.
 *
 *  Copyright (C) 2025 Xcelerit Computing Limited
 *  SPDX-License-Identifier: AGPL-3.0-or-later
 *
 ******************************************************************************/

#include "benchmark_common.hpp"
#include "benchmark_pricing.hpp"

// XAD includes
#include <XAD/XAD.hpp>

// Forge JIT backends (conditionally included)
#if defined(QLRISKS_HAS_FORGE)
#include <xad-forge/ForgeBackend.hpp>
#include <xad-forge/ForgeBackendAVX.hpp>
#endif

#include <chrono>
#include <cstring>
#include <cstdlib>

using namespace benchmark;
using Clock = std::chrono::high_resolution_clock;
using DurationMs = std::chrono::duration<double, std::milli>;

// Use QuantLib's Real which is xad::AReal<double> via qlrisks.hpp
using RealAD = QuantLib::Real;
using tape_type = RealAD::tape_type;

// ============================================================================
// XAD Tape-based AAD Benchmark (unified single/dual-curve)
// ============================================================================

template <bool UseDualCurve>
void runXADBenchmarkT(const BenchmarkConfig& config, const LMMSetup& setup,
                      Size nrTrails, size_t warmup, size_t bench,
                      double& mean, double& stddev,
                      ValidationResult* validation = nullptr)
{
    std::vector<double> times;

    for (size_t iter = 0; iter < warmup + bench; ++iter)
    {
        auto t_start = Clock::now();

        tape_type tape;

        // Register forecasting curve inputs
        std::vector<RealAD> depositRates(config.numDeposits);
        std::vector<RealAD> swapRatesAD(config.numSwaps);
        for (Size idx = 0; idx < config.numDeposits; ++idx)
            depositRates[idx] = config.depoRates[idx];
        for (Size idx = 0; idx < config.numSwaps; ++idx)
            swapRatesAD[idx] = config.swapRates[idx];

        // Register discounting curve inputs (OIS) for dual-curve
        std::vector<RealAD> oisDepoRates;
        std::vector<RealAD> oisSwapRatesAD;
        if constexpr (UseDualCurve)
        {
            oisDepoRates.resize(config.numOisDeposits);
            oisSwapRatesAD.resize(config.numOisSwaps);
            for (Size idx = 0; idx < config.numOisDeposits; ++idx)
                oisDepoRates[idx] = config.oisDepoRates[idx];
            for (Size idx = 0; idx < config.numOisSwaps; ++idx)
                oisSwapRatesAD[idx] = config.oisSwapRates[idx];
        }

        tape.registerInputs(depositRates);
        tape.registerInputs(swapRatesAD);
        if constexpr (UseDualCurve)
        {
            tape.registerInputs(oisDepoRates);
            tape.registerInputs(oisSwapRatesAD);
        }
        tape.newRecording();

        // Price using appropriate function
        RealAD price;
        if constexpr (UseDualCurve)
            price = priceSwaptionDualCurve<RealAD>(
                config, setup, depositRates, swapRatesAD, oisDepoRates, oisSwapRatesAD, nrTrails);
        else
            price = priceSwaption<RealAD>(config, setup, depositRates, swapRatesAD, nrTrails);

        // Compute adjoints
        tape.registerOutput(price);
        derivative(price) = 1.0;
        tape.computeAdjoints();

        auto t_end = Clock::now();

        if (iter >= warmup)
        {
            times.push_back(DurationMs(t_end - t_start).count());
        }

        // Capture validation data on first iteration (before clearing tape)
        if (validation && iter == 0)
        {
            validation->method = "XAD";
            validation->pv = value(price);
            validation->sensitivities.resize(config.numMarketQuotes());
            Size q = 0;
            for (Size idx = 0; idx < config.numDeposits; ++idx)
                validation->sensitivities[q++] = derivative(depositRates[idx]);
            for (Size idx = 0; idx < config.numSwaps; ++idx)
                validation->sensitivities[q++] = derivative(swapRatesAD[idx]);
            if constexpr (UseDualCurve)
            {
                for (Size idx = 0; idx < config.numOisDeposits; ++idx)
                    validation->sensitivities[q++] = derivative(oisDepoRates[idx]);
                for (Size idx = 0; idx < config.numOisSwaps; ++idx)
                    validation->sensitivities[q++] = derivative(oisSwapRatesAD[idx]);
            }
        }

        tape.clearAll();
    }

    mean = computeMean(times);
    stddev = computeStddev(times);
}

// Convenience wrappers for backward compatibility
inline void runXADBenchmark(const BenchmarkConfig& config, const LMMSetup& setup,
                            Size nrTrails, size_t warmup, size_t bench,
                            double& mean, double& stddev,
                            ValidationResult* validation = nullptr)
{
    runXADBenchmarkT<false>(config, setup, nrTrails, warmup, bench, mean, stddev, validation);
}

inline void runXADBenchmarkDualCurve(const BenchmarkConfig& config, const LMMSetup& setup,
                                     Size nrTrails, size_t warmup, size_t bench,
                                     double& mean, double& stddev,
                                     ValidationResult* validation = nullptr)
{
    runXADBenchmarkT<true>(config, setup, nrTrails, warmup, bench, mean, stddev, validation);
}

// ============================================================================
// Chain Rule Helper (used by XAD-Split and JIT methods)
// ============================================================================

inline void applyChainRule(const double* __restrict jacobian,
                           const double* __restrict derivatives,
                           double* __restrict result,
                           std::size_t numIntermediates,
                           std::size_t numInputs)
{
    for (std::size_t j = 0; j < numInputs; ++j)
        result[j] = 0.0;

    for (std::size_t i = 0; i < numIntermediates; ++i)
    {
        const double deriv_i = derivatives[i];
        const double* jac_row = jacobian + i * numInputs;
        for (std::size_t j = 0; j < numInputs; ++j)
        {
            result[j] += deriv_i * jac_row[j];
        }
    }
}

// ============================================================================
// JIT Helper Structures and Functions
// ============================================================================

// Result of Phase 1: Curve bootstrap and Jacobian computation
struct CurveSetupResult {
    Array initRates;                                      // Forward rates from LMM process
    RealAD swapRate;                                      // Fair swap rate
    std::vector<RealAD> intermediates;                    // All intermediates (for tape)
    std::vector<double> jacobian;                         // Jacobian matrix (row-major)
    ext::shared_ptr<LiborForwardModelProcess> process;    // LMM process for path evolution
    Size numIntermediates;                                // Number of intermediates
    Size numMarketQuotes;                                 // Number of market inputs
    // Additional intermediates for dual-curve (OIS discount factors as doubles)
    std::vector<double> oisDiscountFactors;
};

// Phase 1 for Single-Curve: Build curve, extract intermediates, compute Jacobian
CurveSetupResult buildSingleCurveAndJacobian(
    const BenchmarkConfig& config,
    const LMMSetup& setup,
    tape_type& tape)
{
    CurveSetupResult result;
    result.numMarketQuotes = config.numMarketQuotes();
    result.numIntermediates = config.size + 1;  // forward rates + swap rate

    // Register inputs
    std::vector<RealAD> depositRates(config.numDeposits);
    std::vector<RealAD> swapRatesAD(config.numSwaps);
    for (Size idx = 0; idx < config.numDeposits; ++idx)
        depositRates[idx] = config.depoRates[idx];
    for (Size idx = 0; idx < config.numSwaps; ++idx)
        swapRatesAD[idx] = config.swapRates[idx];

    tape.registerInputs(depositRates);
    tape.registerInputs(swapRatesAD);
    tape.newRecording();

    // Build curve and get intermediates
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
        auto swapQuote = ext::make_shared<SimpleQuote>(swapRatesAD[idx]);
        instruments.push_back(ext::make_shared<SwapRateHelper>(
            Handle<Quote>(swapQuote), config.swapTenors[idx],
            setup.calendar, Annual, Unadjusted, Thirty360(Thirty360::BondBasis),
            euribor6m));
    }

    auto yieldCurve = ext::make_shared<PiecewiseYieldCurve<ZeroYield, Linear>>(
        setup.settlementDate, instruments, setup.dayCounter);
    yieldCurve->enableExtrapolation();

    // Extract zero rates
    std::vector<Date> curveDates;
    std::vector<RealAD> zeroRates;
    curveDates.push_back(setup.settlementDate);
    zeroRates.push_back(yieldCurve->zeroRate(setup.settlementDate, setup.dayCounter, Continuous).rate());
    Date endDate = setup.settlementDate + config.curveEndYears * Years;
    curveDates.push_back(endDate);
    zeroRates.push_back(yieldCurve->zeroRate(endDate, setup.dayCounter, Continuous).rate());

    std::vector<Rate> zeroRates_ql;
    for (const auto& r : zeroRates) zeroRates_ql.push_back(r);

    // Build LMM process
    RelinkableHandle<YieldTermStructure> termStructure;
    ext::shared_ptr<IborIndex> index(new Euribor6M(termStructure));
    index->addFixing(Date(2, September, 2005), 0.04);
    termStructure.linkTo(ext::make_shared<ZeroCurve>(curveDates, zeroRates_ql, setup.dayCounter));

    result.process = ext::make_shared<LiborForwardModelProcess>(config.size, index);
    result.process->setCovarParam(ext::shared_ptr<LfmCovarianceParameterization>(
        new LfmCovarianceProxy(
            ext::make_shared<LmLinearExponentialVolatilityModel>(
                result.process->fixingTimes(), 0.291, 1.483, 0.116, 0.00001),
            ext::make_shared<LmExponentialCorrelationModel>(config.size, 0.5))));

    ext::shared_ptr<VanillaSwap> fwdSwap(
        new VanillaSwap(Swap::Receiver, 1.0,
                        setup.schedule, 0.05, setup.dayCounter,
                        setup.schedule, index, 0.0, index->dayCounter()));
    fwdSwap->setPricingEngine(ext::make_shared<DiscountingSwapEngine>(
        index->forwardingTermStructure()));
    result.swapRate = fwdSwap->fairRate();

    // Extract intermediates (forward rates + swap rate)
    result.initRates = result.process->initialValues();
    result.intermediates.resize(result.numIntermediates);
    for (Size k = 0; k < config.size; ++k)
        result.intermediates[k] = result.initRates[k];
    result.intermediates[config.size] = result.swapRate;

    // Register intermediates as outputs
    tape.registerOutputs(result.intermediates);

    // Compute Jacobian: d(intermediates) / d(market inputs)
    result.jacobian.resize(result.numIntermediates * result.numMarketQuotes);
    for (Size i = 0; i < result.numIntermediates; ++i)
    {
        tape.clearDerivatives();
        derivative(result.intermediates[i]) = 1.0;
        tape.computeAdjoints();
        for (Size j = 0; j < config.numDeposits; ++j)
            result.jacobian[i * result.numMarketQuotes + j] = derivative(depositRates[j]);
        for (Size j = 0; j < config.numSwaps; ++j)
            result.jacobian[i * result.numMarketQuotes + config.numDeposits + j] = derivative(swapRatesAD[j]);
    }

    tape.deactivate();

    return result;
}

// Phase 1 for Dual-Curve: Build both curves, extract intermediates, compute Jacobian
CurveSetupResult buildDualCurveAndJacobian(
    const BenchmarkConfig& config,
    const LMMSetup& setup,
    tape_type& tape)
{
    CurveSetupResult result;
    result.numMarketQuotes = config.numMarketQuotes();
    // Intermediates: forward rates + swap rate + OIS discount factors
    result.numIntermediates = config.size + 1 + config.size;  // 2*size + 1

    // Register forecasting curve inputs
    std::vector<RealAD> depositRates(config.numDeposits);
    std::vector<RealAD> swapRatesAD(config.numSwaps);
    for (Size idx = 0; idx < config.numDeposits; ++idx)
        depositRates[idx] = config.depoRates[idx];
    for (Size idx = 0; idx < config.numSwaps; ++idx)
        swapRatesAD[idx] = config.swapRates[idx];

    // Register discounting curve inputs (OIS)
    std::vector<RealAD> oisDepoRates(config.numOisDeposits);
    std::vector<RealAD> oisSwapRatesAD(config.numOisSwaps);
    for (Size idx = 0; idx < config.numOisDeposits; ++idx)
        oisDepoRates[idx] = config.oisDepoRates[idx];
    for (Size idx = 0; idx < config.numOisSwaps; ++idx)
        oisSwapRatesAD[idx] = config.oisSwapRates[idx];

    tape.registerInputs(depositRates);
    tape.registerInputs(swapRatesAD);
    tape.registerInputs(oisDepoRates);
    tape.registerInputs(oisSwapRatesAD);
    tape.newRecording();

    // Build FORECASTING curve (Euribor deposits + swaps)
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
        auto swapQuote = ext::make_shared<SimpleQuote>(swapRatesAD[idx]);
        forecastingInstruments.push_back(ext::make_shared<SwapRateHelper>(
            Handle<Quote>(swapQuote), config.swapTenors[idx],
            setup.calendar, Annual, Unadjusted, Thirty360(Thirty360::BondBasis),
            euribor6m));
    }

    auto forecastingCurve = ext::make_shared<PiecewiseYieldCurve<ZeroYield, Linear>>(
        setup.settlementDate, forecastingInstruments, setup.dayCounter);
    forecastingCurve->enableExtrapolation();
    euriborTS.linkTo(forecastingCurve);

    // Build DISCOUNTING curve (OIS deposits + swaps)
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
        auto oisSwapQuote = ext::make_shared<SimpleQuote>(oisSwapRatesAD[idx]);
        discountingInstruments.push_back(ext::make_shared<OISRateHelper>(
            2, config.oisSwapTenors[idx], Handle<Quote>(oisSwapQuote), eonia));
    }

    auto discountingCurve = ext::make_shared<PiecewiseYieldCurve<ZeroYield, Linear>>(
        setup.settlementDate, discountingInstruments, setup.dayCounter);
    discountingCurve->enableExtrapolation();
    oisTS.linkTo(discountingCurve);

    // Extract zero rates for LMM from forecasting curve
    std::vector<Date> curveDates;
    std::vector<RealAD> zeroRates;
    curveDates.push_back(setup.settlementDate);
    zeroRates.push_back(forecastingCurve->zeroRate(setup.settlementDate, setup.dayCounter, Continuous).rate());
    Date endDate = setup.settlementDate + config.curveEndYears * Years;
    curveDates.push_back(endDate);
    zeroRates.push_back(forecastingCurve->zeroRate(endDate, setup.dayCounter, Continuous).rate());

    std::vector<Rate> zeroRates_ql;
    for (const auto& r : zeroRates) zeroRates_ql.push_back(r);

    // Build LMM process using forecasting curve
    RelinkableHandle<YieldTermStructure> termStructure;
    ext::shared_ptr<IborIndex> index(new Euribor6M(termStructure));
    index->addFixing(Date(2, September, 2005), 0.04);
    termStructure.linkTo(ext::make_shared<ZeroCurve>(curveDates, zeroRates_ql, setup.dayCounter));

    result.process = ext::make_shared<LiborForwardModelProcess>(config.size, index);
    result.process->setCovarParam(ext::shared_ptr<LfmCovarianceParameterization>(
        new LfmCovarianceProxy(
            ext::make_shared<LmLinearExponentialVolatilityModel>(
                result.process->fixingTimes(), 0.291, 1.483, 0.116, 0.00001),
            ext::make_shared<LmExponentialCorrelationModel>(config.size, 0.5))));

    // Get swap rate (using OIS curve for discounting)
    ext::shared_ptr<VanillaSwap> fwdSwap(
        new VanillaSwap(Swap::Receiver, 1.0,
                        setup.schedule, 0.05, setup.dayCounter,
                        setup.schedule, index, 0.0, index->dayCounter()));
    fwdSwap->setPricingEngine(ext::make_shared<DiscountingSwapEngine>(
        Handle<YieldTermStructure>(discountingCurve)));
    result.swapRate = fwdSwap->fairRate();

    // Extract intermediates:
    // [0, size-1]: forward rates from LMM process
    // [size]: swap rate
    // [size+1, 2*size]: OIS discount factors at accrual end times
    result.initRates = result.process->initialValues();
    result.intermediates.resize(result.numIntermediates);

    // Forward rates
    for (Size k = 0; k < config.size; ++k)
        result.intermediates[k] = result.initRates[k];

    // Swap rate
    result.intermediates[config.size] = result.swapRate;

    // OIS discount factors
    for (Size k = 0; k < config.size; ++k)
    {
        Time t = setup.accrualEnd[k];
        result.intermediates[config.size + 1 + k] = discountingCurve->discount(t);
    }

    // Register intermediates as outputs
    tape.registerOutputs(result.intermediates);

    // Compute Jacobian via XAD adjoints
    result.jacobian.resize(result.numIntermediates * result.numMarketQuotes);

    for (Size i = 0; i < result.numIntermediates; ++i)
    {
        tape.clearDerivatives();
        derivative(result.intermediates[i]) = 1.0;
        tape.computeAdjoints();

        Size col = 0;
        // Forecasting inputs
        for (Size j = 0; j < config.numDeposits; ++j)
            result.jacobian[i * result.numMarketQuotes + col++] = derivative(depositRates[j]);
        for (Size j = 0; j < config.numSwaps; ++j)
            result.jacobian[i * result.numMarketQuotes + col++] = derivative(swapRatesAD[j]);
        // Discounting inputs
        for (Size j = 0; j < config.numOisDeposits; ++j)
            result.jacobian[i * result.numMarketQuotes + col++] = derivative(oisDepoRates[j]);
        for (Size j = 0; j < config.numOisSwaps; ++j)
            result.jacobian[i * result.numMarketQuotes + col++] = derivative(oisSwapRatesAD[j]);
    }

    tape.deactivate();

    return result;
}

// ============================================================================
// Shared Payoff Recording - used by both JIT and XAD-Split
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

    // Get covariance parameters (these are constant, not AD)
    Matrix diff = process->covarParam()->diffusion(t0, Array());
    Matrix covariance = process->covarParam()->covariance(t0, Array());

    // Get accrual periods (constants, not AD)
    const std::vector<Time>& accrualStart = process->accrualStartTimes();
    const std::vector<Time>& accrualEnd = process->accrualEndTimes();
    std::vector<double> tau(size);
    for (Size k = 0; k < size; ++k)
    {
        tau[k] = value(accrualEnd[k]) - value(accrualStart[k]);
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

// Compute the MC path payoff (shared between JIT and XAD-Split)
// RandomsType: std::vector<double> for XAD-Split, std::vector<xad::AD> for JIT
template <typename ADType, bool UseDualCurve, typename RandomsType>
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

        // Use custom evolveLMM with LOCAL arrays instead of process->evolve()
        // This fixes XAD-Split tape recording issues caused by mutable member arrays
        double t0 = value(setup.grid[step - 1]);
        double dt = value(setup.grid.dt(step - 1));
        evolveLMM(asset, process, t0, dt, setup.numFactors, randoms, offset);

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
// XAD-Split Benchmark: Jacobian + per-path XAD tape MC + chain rule
// ============================================================================

// XAD-Split Benchmark (Single-Curve)
// Uses per-path tape recording to avoid memory explosion at high path counts
void runXADSplitBenchmark(const BenchmarkConfig& config, const LMMSetup& setup,
                          Size nrTrails, size_t warmup, size_t bench,
                          double& mean, double& stddev,
                          double& fixed_cost_mean,
                          ValidationResult* validation = nullptr)
{
    std::vector<double> times;
    std::vector<double> fixed_times;

    for (size_t iter = 0; iter < warmup + bench; ++iter)
    {
        auto t_start = Clock::now();

        // Phase 1: Curve bootstrap and Jacobian computation (reuse existing function)
        tape_type jacobianTape;
        CurveSetupResult curve = buildSingleCurveAndJacobian(config, setup, jacobianTape);

        auto t_jacobian_end = Clock::now();

        // Phase 2: MC simulation with per-path XAD tape
        // Same structure as JIT but re-record tape each path instead of executing compiled kernel
        tape_type mcTape;
        double mcPrice = 0.0;
        std::vector<double> dPrice_dIntermediates(curve.numIntermediates, 0.0);

        // Cache intermediate values as doubles (same as JIT)
        std::vector<double> initRatesVal(config.size);
        for (Size k = 0; k < config.size; ++k)
            initRatesVal[k] = value(curve.initRates[k]);
        double swapRateVal = value(curve.swapRate);

        for (Size n = 0; n < nrTrails; ++n)
        {
            // Clear tape for each path (reuses internal memory)
            mcTape.clearAll();

            // Setup payoff variables - only initRates and swapRate are AD inputs
            // Randoms are NOT AD inputs (same as full XAD benchmark)
            PayoffVariables<RealAD> vars;
            vars.initRates.resize(config.size);

            // Register inputs: initRates, swapRate (NOT randoms - they're constants)
            for (Size k = 0; k < config.size; ++k)
            {
                vars.initRates[k] = initRatesVal[k];
                mcTape.registerInput(vars.initRates[k]);
            }
            vars.swapRate = swapRateVal;
            mcTape.registerInput(vars.swapRate);

            mcTape.newRecording();

            // Compute payoff - pass randoms as plain doubles (no differentiation through randoms)
            RealAD npv = computePathPayoff<RealAD, false>(config, setup, curve.process, vars, setup.allRandoms[n]);

            // Payoff = max(npv, 0)
            RealAD payoff = (value(npv) > 0.0) ? npv : RealAD(0.0);

            // Compute adjoints for this path
            mcTape.registerOutput(payoff);
            derivative(payoff) = 1.0;
            mcTape.computeAdjoints();

            // Accumulate (same as JIT)
            mcPrice += value(payoff);
            for (Size k = 0; k < config.size; ++k)
                dPrice_dIntermediates[k] += derivative(vars.initRates[k]);
            dPrice_dIntermediates[config.size] += derivative(vars.swapRate);
        }

        // Average
        mcPrice /= static_cast<double>(nrTrails);
        for (Size k = 0; k < curve.numIntermediates; ++k)
            dPrice_dIntermediates[k] /= static_cast<double>(nrTrails);

        // Phase 3: Apply chain rule
        std::vector<double> finalDerivatives(curve.numMarketQuotes);
        applyChainRule(curve.jacobian.data(), dPrice_dIntermediates.data(), finalDerivatives.data(),
                       curve.numIntermediates, curve.numMarketQuotes);

        auto t_end = Clock::now();

        if (iter >= warmup)
        {
            times.push_back(DurationMs(t_end - t_start).count());
            fixed_times.push_back(DurationMs(t_jacobian_end - t_start).count());
        }

        // Capture validation data
        if (validation && iter == 0)
        {
            validation->method = "XADSPLIT";
            validation->pv = mcPrice;
            validation->sensitivities = finalDerivatives;
        }
    }

    mean = computeMean(times);
    stddev = computeStddev(times);
    fixed_cost_mean = computeMean(fixed_times);
}

// XAD-Split Benchmark (Dual-Curve)
// Uses per-path tape recording to avoid memory explosion at high path counts
void runXADSplitBenchmarkDualCurve(const BenchmarkConfig& config, const LMMSetup& setup,
                                    Size nrTrails, size_t warmup, size_t bench,
                                    double& mean, double& stddev,
                                    double& fixed_cost_mean,
                                    ValidationResult* validation = nullptr)
{
    std::vector<double> times;
    std::vector<double> fixed_times;

    for (size_t iter = 0; iter < warmup + bench; ++iter)
    {
        auto t_start = Clock::now();

        // Phase 1: Curve bootstrap and Jacobian computation
        tape_type jacobianTape;
        CurveSetupResult curve = buildDualCurveAndJacobian(config, setup, jacobianTape);

        auto t_jacobian_end = Clock::now();

        // Phase 2: MC simulation with per-path tape (like JIT but interpreted)
        tape_type mcTape;
        double mcPrice = 0.0;
        std::vector<double> dPrice_dIntermediates(curve.numIntermediates, 0.0);

        // Cache intermediate values as doubles (same as JIT)
        std::vector<double> initRatesVal(config.size);
        for (Size k = 0; k < config.size; ++k)
            initRatesVal[k] = value(curve.initRates[k]);
        double swapRateVal = value(curve.swapRate);
        std::vector<double> oisDiscountsVal(config.size);
        for (Size k = 0; k < config.size; ++k)
            oisDiscountsVal[k] = value(curve.intermediates[config.size + 1 + k]);

        for (Size n = 0; n < nrTrails; ++n)
        {
            // Clear tape for each path (reuses internal memory)
            mcTape.clearAll();

            // Setup payoff variables - only initRates, swapRate, oisDiscounts are AD inputs
            // Randoms are NOT AD inputs (same as full XAD benchmark)
            PayoffVariables<RealAD> vars;
            vars.initRates.resize(config.size);
            vars.oisDiscounts.resize(config.size);

            // Register inputs: initRates, swapRate, oisDiscounts (NOT randoms - they're constants)
            for (Size k = 0; k < config.size; ++k)
            {
                vars.initRates[k] = initRatesVal[k];
                mcTape.registerInput(vars.initRates[k]);
            }
            vars.swapRate = swapRateVal;
            mcTape.registerInput(vars.swapRate);

            for (Size k = 0; k < config.size; ++k)
            {
                vars.oisDiscounts[k] = oisDiscountsVal[k];
                mcTape.registerInput(vars.oisDiscounts[k]);
            }

            mcTape.newRecording();

            // Compute payoff - pass randoms as plain doubles (no differentiation through randoms)
            RealAD npv = computePathPayoff<RealAD, true>(config, setup, curve.process, vars, setup.allRandoms[n]);

            // max(npv, 0)
            RealAD payoff = (value(npv) > 0.0) ? npv : RealAD(0.0);

            // Compute adjoints for this path
            mcTape.registerOutput(payoff);
            derivative(payoff) = 1.0;
            mcTape.computeAdjoints();

            // Accumulate (same as JIT)
            mcPrice += value(payoff);
            for (Size k = 0; k < config.size; ++k)
                dPrice_dIntermediates[k] += derivative(vars.initRates[k]);
            dPrice_dIntermediates[config.size] += derivative(vars.swapRate);
            for (Size k = 0; k < config.size; ++k)
                dPrice_dIntermediates[config.size + 1 + k] += derivative(vars.oisDiscounts[k]);
        }

        // Average
        mcPrice /= static_cast<double>(nrTrails);
        for (Size k = 0; k < curve.numIntermediates; ++k)
            dPrice_dIntermediates[k] /= static_cast<double>(nrTrails);

        // Phase 3: Apply chain rule
        std::vector<double> finalDerivatives(curve.numMarketQuotes);
        applyChainRule(curve.jacobian.data(), dPrice_dIntermediates.data(), finalDerivatives.data(),
                       curve.numIntermediates, curve.numMarketQuotes);

        auto t_end = Clock::now();

        if (iter >= warmup)
        {
            times.push_back(DurationMs(t_end - t_start).count());
            fixed_times.push_back(DurationMs(t_jacobian_end - t_start).count());
        }

        // Capture validation data
        if (validation && iter == 0)
        {
            validation->method = "XADSPLIT";
            validation->pv = mcPrice;
            validation->sensitivities = finalDerivatives;
        }
    }

    mean = computeMean(times);
    stddev = computeStddev(times);
    fixed_cost_mean = computeMean(fixed_times);
}

#if defined(QLRISKS_HAS_FORGE)

// ============================================================================
// Forward declaration for recordJITGraph (defined later)
// ============================================================================

template <bool UseDualCurve>
void recordJITGraph(
    xad::JITCompiler<double>& jit,
    const BenchmarkConfig& config,
    const LMMSetup& setup,
    const CurveSetupResult& curve,
    PayoffVariables<xad::AD>& vars);

// ============================================================================
// DIAGNOSTIC: Compare XAD-Split vs JIT derivatives step-by-step
// ============================================================================

void runDiagnosticComparison(const BenchmarkConfig& config, const LMMSetup& setup, Size nrTrails)
{
    std::cout << "\n";
    std::cout << "================================================================================\n";
    std::cout << "  DIAGNOSTIC: XAD-Split vs JIT Derivative Comparison\n";
    std::cout << "================================================================================\n";
    std::cout << "  Config: size=" << config.size << ", numDeposits=" << config.numDeposits
              << ", numSwaps=" << config.numSwaps << ", paths=" << nrTrails << "\n";
    std::cout << "  numIntermediates = " << (config.size + 1) << " (forward rates + swap rate)\n";
    std::cout << "  numMarketQuotes = " << config.numMarketQuotes() << "\n";
    std::cout << std::endl;

    // =========================================================================
    // Phase 1: Build curve and Jacobian (should be identical for both)
    // =========================================================================
    tape_type jacobianTape;
    CurveSetupResult curve = buildSingleCurveAndJacobian(config, setup, jacobianTape);

    std::cout << "  [Phase 1] Curve Bootstrap & Jacobian\n";
    std::cout << "  -------------------------------------\n";
    std::cout << "  Intermediate values (initRates + swapRate):\n";
    for (Size k = 0; k < config.size; ++k)
        std::cout << "    initRates[" << k << "] = " << value(curve.initRates[k]) << "\n";
    std::cout << "    swapRate     = " << value(curve.swapRate) << "\n";
    std::cout << std::endl;

    // Print Jacobian (first few rows/cols for brevity)
    std::cout << "  Jacobian (dIntermediate/dMarket) - first 5 rows, all cols:\n";
    Size printRows = std::min(curve.numIntermediates, Size(5));
    for (Size i = 0; i < printRows; ++i) {
        std::cout << "    Row " << i << ": ";
        for (Size j = 0; j < curve.numMarketQuotes; ++j) {
            std::cout << std::setw(12) << std::scientific << std::setprecision(4)
                      << curve.jacobian[i * curve.numMarketQuotes + j] << " ";
        }
        std::cout << "\n";
    }
    if (curve.numIntermediates > 5) std::cout << "    ... (more rows)\n";
    std::cout << "    Row " << config.size << " (swapRate): ";
    for (Size j = 0; j < curve.numMarketQuotes; ++j) {
        std::cout << std::setw(12) << std::scientific << std::setprecision(4)
                  << curve.jacobian[config.size * curve.numMarketQuotes + j] << " ";
    }
    std::cout << "\n\n";

    // =========================================================================
    // Phase 2a: XAD-Split - per-path tape MC
    // =========================================================================
    std::cout << "  [Phase 2] MC Simulation - Per-path derivatives\n";
    std::cout << "  -----------------------------------------------\n";

    tape_type mcTape;
    double xadsplit_mcPrice = 0.0;
    std::vector<double> xadsplit_dPrice_dIntermediates(curve.numIntermediates, 0.0);
    std::vector<double> initRatesVal(config.size);
    for (Size k = 0; k < config.size; ++k)
        initRatesVal[k] = value(curve.initRates[k]);
    double swapRateVal = value(curve.swapRate);

    // Store per-path derivatives for comparison
    std::vector<std::vector<double>> xadsplit_path_derivs(nrTrails);

    for (Size n = 0; n < nrTrails; ++n)
    {
        mcTape.clearAll();

        PayoffVariables<RealAD> vars;
        vars.initRates.resize(config.size);

        for (Size k = 0; k < config.size; ++k) {
            vars.initRates[k] = initRatesVal[k];
            mcTape.registerInput(vars.initRates[k]);
        }
        vars.swapRate = swapRateVal;
        mcTape.registerInput(vars.swapRate);

        mcTape.newRecording();

        RealAD npv = computePathPayoff<RealAD, false>(config, setup, curve.process, vars, setup.allRandoms[n]);
        RealAD payoff = (value(npv) > 0.0) ? npv : RealAD(0.0);

        mcTape.registerOutput(payoff);
        derivative(payoff) = 1.0;
        mcTape.computeAdjoints();

        xadsplit_mcPrice += value(payoff);
        xadsplit_path_derivs[n].resize(curve.numIntermediates);
        for (Size k = 0; k < config.size; ++k) {
            double d = derivative(vars.initRates[k]);
            xadsplit_dPrice_dIntermediates[k] += d;
            xadsplit_path_derivs[n][k] = d;
        }
        double d_swap = derivative(vars.swapRate);
        xadsplit_dPrice_dIntermediates[config.size] += d_swap;
        xadsplit_path_derivs[n][config.size] = d_swap;
    }

    xadsplit_mcPrice /= static_cast<double>(nrTrails);
    for (Size k = 0; k < curve.numIntermediates; ++k)
        xadsplit_dPrice_dIntermediates[k] /= static_cast<double>(nrTrails);

    // =========================================================================
    // Phase 2b: JIT - compiled kernel MC
    // =========================================================================
    auto backend = std::make_unique<xad::forge::ForgeBackend<double>>(false);
    xad::JITCompiler<double> jit(std::move(backend));

    PayoffVariables<xad::AD> jit_vars;
    recordJITGraph<false>(jit, config, setup, curve, jit_vars);
    jit.compile();

    const auto& graph = jit.getGraph();
    uint32_t outputSlot = graph.output_ids[0];

    double jit_mcPrice = 0.0;
    std::vector<double> jit_dPrice_dIntermediates(curve.numIntermediates, 0.0);
    std::vector<std::vector<double>> jit_path_derivs(nrTrails);

    for (Size n = 0; n < nrTrails; ++n)
    {
        for (Size k = 0; k < config.size; ++k)
            value(jit_vars.initRates[k]) = value(curve.initRates[k]);
        value(jit_vars.swapRate) = value(curve.swapRate);
        for (Size m = 0; m < setup.fullGridRandoms; ++m)
            value(jit_vars.randoms[m]) = setup.allRandoms[n][m];

        double payoff_value;
        jit.forward(&payoff_value);
        jit_mcPrice += payoff_value;

        jit.clearDerivatives();
        jit.setDerivative(outputSlot, 1.0);
        jit.computeAdjoints();

        jit_path_derivs[n].resize(curve.numIntermediates);
        for (Size k = 0; k < config.size; ++k) {
            double d = jit.derivative(graph.input_ids[k]);
            jit_dPrice_dIntermediates[k] += d;
            jit_path_derivs[n][k] = d;
        }
        double d_swap = jit.derivative(graph.input_ids[config.size]);
        jit_dPrice_dIntermediates[config.size] += d_swap;
        jit_path_derivs[n][config.size] = d_swap;
    }

    jit_mcPrice /= static_cast<double>(nrTrails);
    for (Size k = 0; k < curve.numIntermediates; ++k)
        jit_dPrice_dIntermediates[k] /= static_cast<double>(nrTrails);

    // =========================================================================
    // Compare per-path derivatives (first few paths)
    // =========================================================================
    Size printPaths = std::min(nrTrails, Size(5));
    std::cout << "  Per-path derivatives (first " << printPaths << " paths):\n";
    for (Size n = 0; n < printPaths; ++n) {
        std::cout << "  Path " << n << ":\n";
        std::cout << "    Intermediate |    XAD-Split |         JIT |      Diff |   Ratio\n";
        std::cout << "    -------------+-------------+-------------+-----------+---------\n";
        for (Size k = 0; k < curve.numIntermediates; ++k) {
            double xs = xadsplit_path_derivs[n][k];
            double jt = jit_path_derivs[n][k];
            double diff = xs - jt;
            double ratio = (std::abs(jt) > 1e-15) ? xs / jt : 0.0;
            std::string name = (k < config.size) ? "initRates[" + std::to_string(k) + "]" : "swapRate";
            std::cout << "    " << std::setw(12) << name << " | "
                      << std::setw(11) << std::scientific << std::setprecision(4) << xs << " | "
                      << std::setw(11) << jt << " | "
                      << std::setw(9) << diff << " | "
                      << std::fixed << std::setprecision(4) << ratio << "\n";
        }
        std::cout << "\n";
    }

    // =========================================================================
    // Compare averaged dPrice_dIntermediates
    // =========================================================================
    std::cout << "  [Phase 3] Averaged dPrice/dIntermediates (over " << nrTrails << " paths):\n";
    std::cout << "  -----------------------------------------------------------------\n";
    std::cout << "  Intermediate |    XAD-Split |         JIT |      Diff |   Ratio\n";
    std::cout << "  -------------+-------------+-------------+-----------+---------\n";
    for (Size k = 0; k < curve.numIntermediates; ++k) {
        double xs = xadsplit_dPrice_dIntermediates[k];
        double jt = jit_dPrice_dIntermediates[k];
        double diff = xs - jt;
        double ratio = (std::abs(jt) > 1e-15) ? xs / jt : 0.0;
        std::string name = (k < config.size) ? "initRates[" + std::to_string(k) + "]" : "swapRate";
        std::cout << "  " << std::setw(12) << name << " | "
                  << std::setw(11) << std::scientific << std::setprecision(4) << xs << " | "
                  << std::setw(11) << jt << " | "
                  << std::setw(9) << diff << " | "
                  << std::fixed << std::setprecision(4) << ratio << "\n";
    }
    std::cout << "\n";

    // =========================================================================
    // Phase 4: Apply chain rule and compare final sensitivities
    // =========================================================================
    std::vector<double> xadsplit_final(curve.numMarketQuotes);
    std::vector<double> jit_final(curve.numMarketQuotes);
    applyChainRule(curve.jacobian.data(), xadsplit_dPrice_dIntermediates.data(), xadsplit_final.data(),
                   curve.numIntermediates, curve.numMarketQuotes);
    applyChainRule(curve.jacobian.data(), jit_dPrice_dIntermediates.data(), jit_final.data(),
                   curve.numIntermediates, curve.numMarketQuotes);

    std::cout << "  [Phase 4] Final Market Sensitivities (after chain rule):\n";
    std::cout << "  ---------------------------------------------------------\n";
    std::cout << "  Market Input |    XAD-Split |         JIT |      Diff |   Ratio | Status\n";
    std::cout << "  -------------+-------------+-------------+-----------+---------+--------\n";
    for (Size j = 0; j < curve.numMarketQuotes; ++j) {
        double xs = xadsplit_final[j];
        double jt = jit_final[j];
        double diff = xs - jt;
        double ratio = (std::abs(jt) > 1e-15) ? xs / jt : 0.0;
        double relErr = (std::abs(jt) > 1e-15) ? std::abs(diff / jt) * 100.0 : 0.0;
        std::string name = (j < config.numDeposits) ? "deposit[" + std::to_string(j) + "]"
                                                     : "swap[" + std::to_string(j - config.numDeposits) + "]";
        std::string status = (relErr < 0.1) ? "OK" : "FAIL";
        std::cout << "  " << std::setw(12) << name << " | "
                  << std::setw(11) << std::scientific << std::setprecision(4) << xs << " | "
                  << std::setw(11) << jt << " | "
                  << std::setw(9) << diff << " | "
                  << std::fixed << std::setprecision(4) << ratio << " | "
                  << status << "\n";
    }
    std::cout << "\n";

    std::cout << "  Summary:\n";
    std::cout << "    XAD-Split PV: " << xadsplit_mcPrice << "\n";
    std::cout << "    JIT PV:       " << jit_mcPrice << "\n";
    std::cout << "    PV Match:     " << (std::abs(xadsplit_mcPrice - jit_mcPrice) < 1e-10 ? "YES" : "NO") << "\n";
    std::cout << "================================================================================\n\n";
}

// Phase 2: Record JIT graph for payoff (unified single/dual-curve)
// Uses shared computePathPayoff function
template <bool UseDualCurve>
void recordJITGraph(
    xad::JITCompiler<double>& jit,
    const BenchmarkConfig& config,
    const LMMSetup& setup,
    const CurveSetupResult& curve,
    PayoffVariables<xad::AD>& vars)
{
    vars.initRates.resize(config.size);
    vars.randoms.resize(setup.fullGridRandoms);

    for (Size k = 0; k < config.size; ++k)
    {
        vars.initRates[k] = xad::AD(value(curve.initRates[k]));
        jit.registerInput(vars.initRates[k]);
    }
    vars.swapRate = xad::AD(value(curve.swapRate));
    jit.registerInput(vars.swapRate);

    // Register OIS discount factors for dual-curve
    if constexpr (UseDualCurve)
    {
        vars.oisDiscounts.resize(config.size);
        for (Size k = 0; k < config.size; ++k)
        {
            vars.oisDiscounts[k] = xad::AD(value(curve.intermediates[config.size + 1 + k]));
            jit.registerInput(vars.oisDiscounts[k]);
        }
    }

    for (Size m = 0; m < setup.fullGridRandoms; ++m)
    {
        vars.randoms[m] = xad::AD(0.0);
        jit.registerInput(vars.randoms[m]);
    }

    jit.newRecording();

    // Compute NPV using shared function - pass vars.randoms for JIT graph recording
    xad::AD npv = computePathPayoff<xad::AD, UseDualCurve>(config, setup, curve.process, vars, vars.randoms);

    // Payoff = max(npv, 0) - JIT uses xad::less().If() for JIT compatibility
    xad::AD payoff = xad::less(npv, xad::AD(0.0)).If(xad::AD(0.0), npv);
    jit.registerOutput(payoff);
}

// ============================================================================
// Forge JIT Scalar Benchmark (Single-Curve)
// ============================================================================

template <typename BackendType>
void runJITBenchmarkImpl(const BenchmarkConfig& config, const LMMSetup& setup,
                         Size nrTrails, size_t warmup, size_t bench,
                         double& mean, double& stddev,
                         ValidationResult* validation = nullptr)
{
    std::vector<double> times;

    for (size_t iter = 0; iter < warmup + bench; ++iter)
    {
        auto t_start = Clock::now();

        // Phase 1: XAD tape - curve bootstrap and Jacobian computation
        tape_type tape;
        CurveSetupResult curve = buildSingleCurveAndJacobian(config, setup, tape);

        // Phase 2: JIT graph recording and compilation
        auto backend = std::make_unique<BackendType>(false);
        xad::JITCompiler<double> jit(std::move(backend));

        PayoffVariables<xad::AD> vars;
        recordJITGraph<false>(jit, config, setup, curve, vars);

        // Compile the JIT kernel
        jit.compile();

        // Phase 3: Execute JIT kernel for all MC paths
        const auto& graph = jit.getGraph();
        uint32_t outputSlot = graph.output_ids[0];

        double mcPrice = 0.0;
        std::vector<double> dPrice_dIntermediates(curve.numIntermediates, 0.0);

        for (Size n = 0; n < nrTrails; ++n)
        {
            // Set inputs
            for (Size k = 0; k < config.size; ++k)
                value(vars.initRates[k]) = value(curve.initRates[k]);
            value(vars.swapRate) = value(curve.swapRate);
            for (Size m = 0; m < setup.fullGridRandoms; ++m)
                value(vars.randoms[m]) = setup.allRandoms[n][m];

            // Execute forward + backward
            double payoff_value;
            jit.forward(&payoff_value);
            mcPrice += payoff_value;

            // Accumulate gradients w.r.t. intermediates
            jit.clearDerivatives();
            jit.setDerivative(outputSlot, 1.0);
            jit.computeAdjoints();

            for (Size k = 0; k < config.size; ++k)
                dPrice_dIntermediates[k] += jit.derivative(graph.input_ids[k]);
            dPrice_dIntermediates[config.size] += jit.derivative(graph.input_ids[config.size]);
        }

        // Average
        mcPrice /= static_cast<double>(nrTrails);
        for (Size k = 0; k < curve.numIntermediates; ++k)
            dPrice_dIntermediates[k] /= static_cast<double>(nrTrails);

        // Phase 4: Apply chain rule to get market sensitivities
        std::vector<double> finalDerivatives(curve.numMarketQuotes);
        applyChainRule(curve.jacobian.data(), dPrice_dIntermediates.data(), finalDerivatives.data(),
                       curve.numIntermediates, curve.numMarketQuotes);

        auto t_end = Clock::now();

        if (iter >= warmup)
        {
            times.push_back(DurationMs(t_end - t_start).count());
        }

        // Capture validation data on first iteration
        if (validation && iter == 0)
        {
            validation->method = "JIT";
            validation->pv = mcPrice;
            validation->sensitivities = finalDerivatives;
        }
    }

    mean = computeMean(times);
    stddev = computeStddev(times);
}

void runJITBenchmark(const BenchmarkConfig& config, const LMMSetup& setup,
                     Size nrTrails, size_t warmup, size_t bench,
                     double& mean, double& stddev,
                     ValidationResult* validation = nullptr)
{
    runJITBenchmarkImpl<xad::forge::ForgeBackend<double>>(
        config, setup, nrTrails, warmup, bench, mean, stddev, validation);
}

// ============================================================================
// Forge JIT-AVX Benchmark (Single-Curve) - Batched Execution
// ============================================================================

void runJITAVXBenchmark(const BenchmarkConfig& config, const LMMSetup& setup,
                        Size nrTrails, size_t warmup, size_t bench,
                        double& mean, double& stddev,
                        ValidationResult* validation = nullptr)
{
    std::vector<double> times;

    for (size_t iter = 0; iter < warmup + bench; ++iter)
    {
        auto t_start = Clock::now();

        // Phase 1: XAD tape - curve bootstrap and Jacobian computation
        tape_type tape;
        CurveSetupResult curve = buildSingleCurveAndJacobian(config, setup, tape);

        // Phase 2: JIT graph recording (using JITCompiler to record only)
        xad::JITCompiler<double> jit;  // Default interpreter - just for recording

        PayoffVariables<xad::AD> vars;
        recordJITGraph<false>(jit, config, setup, curve, vars);

        // Get the JIT graph and deactivate
        const auto& jitGraph = jit.getGraph();
        jit.deactivate();

        // Phase 3: AVX backend compilation and batched execution
        xad::forge::ForgeBackendAVX<double> avxBackend(false);
        avxBackend.compile(jitGraph);

        constexpr int BATCH_SIZE = xad::forge::ForgeBackendAVX<double>::VECTOR_WIDTH;
        Size numBatches = (nrTrails + BATCH_SIZE - 1) / BATCH_SIZE;

        std::vector<double> inputBatch(BATCH_SIZE);
        std::vector<double> outputBatch(BATCH_SIZE);

        double mcPrice = 0.0;
        std::vector<double> dPrice_dIntermediates(curve.numIntermediates, 0.0);

        // Number of inputs: forward rates + swap rate + randoms
        std::size_t numInputs = config.size + 1 + setup.fullGridRandoms;
        std::vector<double> inputGradients(numInputs * BATCH_SIZE);

        for (Size batch = 0; batch < numBatches; ++batch)
        {
            Size batchStart = batch * BATCH_SIZE;
            Size actualBatchSize = std::min(static_cast<Size>(BATCH_SIZE), nrTrails - batchStart);

            // Set initRates (same for all paths in batch)
            for (Size k = 0; k < config.size; ++k)
            {
                for (int lane = 0; lane < BATCH_SIZE; ++lane)
                    inputBatch[lane] = value(curve.initRates[k]);
                avxBackend.setInput(k, inputBatch.data());
            }

            // Set swapRate (same for all paths in batch)
            for (int lane = 0; lane < BATCH_SIZE; ++lane)
                inputBatch[lane] = value(curve.swapRate);
            avxBackend.setInput(config.size, inputBatch.data());

            // Set random numbers (different for each path in batch)
            for (Size m = 0; m < setup.fullGridRandoms; ++m)
            {
                for (int lane = 0; lane < BATCH_SIZE; ++lane)
                {
                    Size pathIdx = batchStart + lane;
                    inputBatch[lane] = (pathIdx < nrTrails) ? setup.allRandoms[pathIdx][m] : 0.0;
                }
                avxBackend.setInput(config.size + 1 + m, inputBatch.data());
            }

            // Execute forward + backward in one call
            avxBackend.forwardAndBackward(outputBatch.data(), inputGradients.data());

            // Accumulate MC price
            for (Size lane = 0; lane < actualBatchSize; ++lane)
            {
                mcPrice += outputBatch[lane];
            }

            // Accumulate gradients for initRates
            for (Size k = 0; k < config.size; ++k)
            {
                for (Size lane = 0; lane < actualBatchSize; ++lane)
                {
                    dPrice_dIntermediates[k] += inputGradients[k * BATCH_SIZE + lane];
                }
            }

            // Accumulate gradient for swap rate
            for (Size lane = 0; lane < actualBatchSize; ++lane)
            {
                dPrice_dIntermediates[config.size] += inputGradients[config.size * BATCH_SIZE + lane];
            }
        }

        // Average
        mcPrice /= static_cast<double>(nrTrails);
        for (Size k = 0; k < curve.numIntermediates; ++k)
            dPrice_dIntermediates[k] /= static_cast<double>(nrTrails);

        // Phase 4: Apply chain rule to get market sensitivities
        std::vector<double> finalDerivatives(curve.numMarketQuotes);
        applyChainRule(curve.jacobian.data(), dPrice_dIntermediates.data(), finalDerivatives.data(),
                       curve.numIntermediates, curve.numMarketQuotes);

        auto t_end = Clock::now();

        if (iter >= warmup)
        {
            times.push_back(DurationMs(t_end - t_start).count());
        }

        // Capture validation data on first iteration
        if (validation && iter == 0)
        {
            validation->method = "JITAVX";
            validation->pv = mcPrice;
            validation->sensitivities = finalDerivatives;
        }
    }

    mean = computeMean(times);
    stddev = computeStddev(times);
}

// ============================================================================
// Forge JIT Benchmark (Dual-Curve) - Template Implementation
// ============================================================================

template <typename BackendType>
void runJITBenchmarkDualCurveImpl(const BenchmarkConfig& config, const LMMSetup& setup,
                                   Size nrTrails, size_t warmup, size_t bench,
                                   double& mean, double& stddev,
                                   double& phase1_curve_mean, double& phase2_jacobian_mean,
                                   double& phase3_compile_mean,
                                   ValidationResult* validation = nullptr)
{
    std::vector<double> times;
    std::vector<double> phase1_times;  // Curve bootstrap + Jacobian
    std::vector<double> phase2_times;  // (unused, kept for API compatibility)
    std::vector<double> phase3_times;  // JIT record + compile

    for (size_t iter = 0; iter < warmup + bench; ++iter)
    {
        auto t_start = Clock::now();

        // Phase 1: XAD tape - dual-curve bootstrap and Jacobian computation
        tape_type tape;
        CurveSetupResult curve = buildDualCurveAndJacobian(config, setup, tape);

        auto t_curve_end = Clock::now();

        // Phase 2: JIT graph recording and compilation
        auto backend = std::make_unique<BackendType>(false);
        xad::JITCompiler<double> jit(std::move(backend));

        PayoffVariables<xad::AD> vars;
        recordJITGraph<true>(jit, config, setup, curve, vars);

        // Compile the JIT kernel
        jit.compile();

        auto t_compile_end = Clock::now();

        // Phase 3: Execute JIT kernel for all MC paths
        const auto& graph = jit.getGraph();
        uint32_t outputSlot = graph.output_ids[0];

        double mcPrice = 0.0;
        std::vector<double> dPrice_dIntermediates(curve.numIntermediates, 0.0);

        for (Size n = 0; n < nrTrails; ++n)
        {
            // Set inputs
            for (Size k = 0; k < config.size; ++k)
                value(vars.initRates[k]) = value(curve.initRates[k]);
            value(vars.swapRate) = value(curve.swapRate);
            for (Size k = 0; k < config.size; ++k)
                value(vars.oisDiscounts[k]) = value(curve.intermediates[config.size + 1 + k]);
            for (Size m = 0; m < setup.fullGridRandoms; ++m)
                value(vars.randoms[m]) = setup.allRandoms[n][m];

            // Execute forward
            double payoff_value;
            jit.forward(&payoff_value);
            mcPrice += payoff_value;

            // Accumulate gradients w.r.t. intermediates
            jit.clearDerivatives();
            jit.setDerivative(outputSlot, 1.0);
            jit.computeAdjoints();

            // Forward rates
            for (Size k = 0; k < config.size; ++k)
                dPrice_dIntermediates[k] += jit.derivative(graph.input_ids[k]);
            // Swap rate
            dPrice_dIntermediates[config.size] += jit.derivative(graph.input_ids[config.size]);
            // OIS discount factors
            for (Size k = 0; k < config.size; ++k)
                dPrice_dIntermediates[config.size + 1 + k] += jit.derivative(graph.input_ids[config.size + 1 + k]);
        }

        // Average
        mcPrice /= static_cast<double>(nrTrails);
        for (Size k = 0; k < curve.numIntermediates; ++k)
            dPrice_dIntermediates[k] /= static_cast<double>(nrTrails);

        // Phase 4: Apply chain rule to get market sensitivities
        std::vector<double> finalDerivatives(curve.numMarketQuotes);
        applyChainRule(curve.jacobian.data(), dPrice_dIntermediates.data(), finalDerivatives.data(),
                       curve.numIntermediates, curve.numMarketQuotes);

        auto t_end = Clock::now();

        if (iter >= warmup)
        {
            times.push_back(DurationMs(t_end - t_start).count());
            phase1_times.push_back(DurationMs(t_curve_end - t_start).count());
            phase2_times.push_back(0.0);  // Jacobian now included in phase1
            phase3_times.push_back(DurationMs(t_compile_end - t_curve_end).count());
        }

        // Capture validation data on first iteration
        if (validation && iter == 0)
        {
            validation->method = "JIT";
            validation->pv = mcPrice;
            validation->sensitivities = finalDerivatives;
        }
    }

    mean = computeMean(times);
    stddev = computeStddev(times);
    phase1_curve_mean = computeMean(phase1_times);
    phase2_jacobian_mean = computeMean(phase2_times);
    phase3_compile_mean = computeMean(phase3_times);
}

void runJITBenchmarkDualCurve(const BenchmarkConfig& config, const LMMSetup& setup,
                               Size nrTrails, size_t warmup, size_t bench,
                               double& mean, double& stddev,
                               double& phase1_curve_mean, double& phase2_jacobian_mean,
                               double& phase3_compile_mean,
                               ValidationResult* validation = nullptr)
{
    runJITBenchmarkDualCurveImpl<xad::forge::ForgeBackend<double>>(
        config, setup, nrTrails, warmup, bench, mean, stddev,
        phase1_curve_mean, phase2_jacobian_mean, phase3_compile_mean, validation);
}

// ============================================================================
// Forge JIT-AVX Benchmark (Dual-Curve) - Batched Execution
// ============================================================================

void runJITAVXBenchmarkDualCurve(const BenchmarkConfig& config, const LMMSetup& setup,
                                  Size nrTrails, size_t warmup, size_t bench,
                                  double& mean, double& stddev,
                                  double& phase1_curve_mean, double& phase2_jacobian_mean,
                                  double& phase3_compile_mean,
                                  ValidationResult* validation = nullptr)
{
    std::vector<double> times;
    std::vector<double> phase1_times;  // Curve bootstrap + Jacobian
    std::vector<double> phase2_times;  // (unused, kept for API compatibility)
    std::vector<double> phase3_times;  // JIT record + compile

    for (size_t iter = 0; iter < warmup + bench; ++iter)
    {
        auto t_start = Clock::now();

        // Phase 1: XAD tape - dual-curve bootstrap and Jacobian computation
        tape_type tape;
        CurveSetupResult curve = buildDualCurveAndJacobian(config, setup, tape);

        auto t_curve_end = Clock::now();

        // Phase 2: JIT graph recording (using JITCompiler without backend)
        xad::JITCompiler<double> jit;  // Default constructor - for recording only

        PayoffVariables<xad::AD> vars;
        recordJITGraph<true>(jit, config, setup, curve, vars);

        // Get the JIT graph and deactivate
        const auto& jitGraph = jit.getGraph();
        jit.deactivate();

        // Phase 3: AVX backend compilation and batched execution
        xad::forge::ForgeBackendAVX<double> avxBackend(false);
        avxBackend.compile(jitGraph);

        auto t_compile_end = Clock::now();

        constexpr int BATCH_SIZE = xad::forge::ForgeBackendAVX<double>::VECTOR_WIDTH;
        Size numBatches = (nrTrails + BATCH_SIZE - 1) / BATCH_SIZE;

        std::vector<double> inputBatch(BATCH_SIZE);
        std::vector<double> outputBatch(BATCH_SIZE);

        double mcPrice = 0.0;
        std::vector<double> dPrice_dIntermediates(curve.numIntermediates, 0.0);

        // Number of inputs: forward rates + swap rate + OIS discounts + randoms
        std::size_t numInputs = config.size + 1 + config.size + setup.fullGridRandoms;
        std::vector<double> inputGradients(numInputs * BATCH_SIZE);

        for (Size batch = 0; batch < numBatches; ++batch)
        {
            Size batchStart = batch * BATCH_SIZE;
            Size actualBatchSize = std::min(static_cast<Size>(BATCH_SIZE), nrTrails - batchStart);

            // Set initRates (same for all paths in batch)
            for (Size k = 0; k < config.size; ++k)
            {
                for (int lane = 0; lane < BATCH_SIZE; ++lane)
                    inputBatch[lane] = value(curve.initRates[k]);
                avxBackend.setInput(k, inputBatch.data());
            }

            // Set swapRate (same for all paths in batch)
            for (int lane = 0; lane < BATCH_SIZE; ++lane)
                inputBatch[lane] = value(curve.swapRate);
            avxBackend.setInput(config.size, inputBatch.data());

            // Set OIS discount factors (same for all paths in batch)
            for (Size k = 0; k < config.size; ++k)
            {
                for (int lane = 0; lane < BATCH_SIZE; ++lane)
                    inputBatch[lane] = value(curve.intermediates[config.size + 1 + k]);
                avxBackend.setInput(config.size + 1 + k, inputBatch.data());
            }

            // Set random numbers (different for each path in batch)
            Size randomInputOffset = config.size + 1 + config.size;  // After forward rates, swap rate, OIS discounts
            for (Size m = 0; m < setup.fullGridRandoms; ++m)
            {
                for (int lane = 0; lane < BATCH_SIZE; ++lane)
                {
                    Size pathIdx = batchStart + lane;
                    inputBatch[lane] = (pathIdx < nrTrails) ? setup.allRandoms[pathIdx][m] : 0.0;
                }
                avxBackend.setInput(randomInputOffset + m, inputBatch.data());
            }

            // Execute forward + backward in one call
            avxBackend.forwardAndBackward(outputBatch.data(), inputGradients.data());

            // Accumulate MC price
            for (Size lane = 0; lane < actualBatchSize; ++lane)
            {
                mcPrice += outputBatch[lane];
            }

            // Accumulate gradients for initRates
            for (Size k = 0; k < config.size; ++k)
            {
                for (Size lane = 0; lane < actualBatchSize; ++lane)
                {
                    dPrice_dIntermediates[k] += inputGradients[k * BATCH_SIZE + lane];
                }
            }

            // Accumulate gradient for swap rate
            for (Size lane = 0; lane < actualBatchSize; ++lane)
            {
                dPrice_dIntermediates[config.size] += inputGradients[config.size * BATCH_SIZE + lane];
            }

            // Accumulate gradients for OIS discount factors
            for (Size k = 0; k < config.size; ++k)
            {
                for (Size lane = 0; lane < actualBatchSize; ++lane)
                {
                    dPrice_dIntermediates[config.size + 1 + k] += inputGradients[(config.size + 1 + k) * BATCH_SIZE + lane];
                }
            }
        }

        // Average
        mcPrice /= static_cast<double>(nrTrails);
        for (Size k = 0; k < curve.numIntermediates; ++k)
            dPrice_dIntermediates[k] /= static_cast<double>(nrTrails);

        // Phase 4: Apply chain rule to get market sensitivities
        std::vector<double> finalDerivatives(curve.numMarketQuotes);
        applyChainRule(curve.jacobian.data(), dPrice_dIntermediates.data(), finalDerivatives.data(),
                       curve.numIntermediates, curve.numMarketQuotes);

        auto t_end = Clock::now();

        if (iter >= warmup)
        {
            times.push_back(DurationMs(t_end - t_start).count());
            phase1_times.push_back(DurationMs(t_curve_end - t_start).count());
            phase2_times.push_back(0.0);  // Jacobian now included in phase1
            phase3_times.push_back(DurationMs(t_compile_end - t_curve_end).count());
        }

        // Capture validation data on first iteration
        if (validation && iter == 0)
        {
            validation->method = "JITAVX";
            validation->pv = mcPrice;
            validation->sensitivities = finalDerivatives;
        }
    }

    mean = computeMean(times);
    stddev = computeStddev(times);
    phase1_curve_mean = computeMean(phase1_times);
    phase2_jacobian_mean = computeMean(phase2_times);
    phase3_compile_mean = computeMean(phase3_times);
}

#endif // QLRISKS_HAS_FORGE

// ============================================================================
// Main AAD Benchmark Runner (Single-Curve)
// ============================================================================

std::vector<TimingResult> runAADBenchmark(const BenchmarkConfig& config,
                                           bool quickMode, bool xadOnly,
                                           ValidationResult* xadValidation = nullptr,
                                           ValidationResult* xadSplitValidation = nullptr,
                                           ValidationResult* jitValidation = nullptr,
                                           ValidationResult* jitAvxValidation = nullptr)
{
    std::vector<TimingResult> results;

    // Setup LMM
    LMMSetup setup(config);

    std::cout << "================================================================================\n";
    std::cout << "  RUNNING AAD BENCHMARKS\n";
    std::cout << "================================================================================\n";
    std::cout << "\n";

    for (size_t tc = 0; tc < config.pathCounts.size(); ++tc)
    {
        int paths = config.pathCounts[tc];
        Size nrTrails = static_cast<Size>(paths);

        TimingResult result;
        result.pathCount = paths;

        std::cout << "  [" << (tc + 1) << "/" << config.pathCounts.size() << "] "
                  << formatPathCount(paths) << " paths " << std::flush;

        // For high path counts, skip warmup and use fewer iterations
        size_t warmup, bench;
        if (nrTrails >= 100000) {
            warmup = 0;
            bench = 1;  // Single run for 100K+ (too expensive for multiple)
        } else if (nrTrails >= 10000) {
            warmup = 0;
            bench = 2;
        } else {
            warmup = quickMode ? 1 : config.warmupIterations;
            bench = quickMode ? 2 : config.benchmarkIterations;
        }

        // Capture validation at VALIDATION_PATH_COUNT
        bool captureValidation = (paths == VALIDATION_PATH_COUNT);

        // XAD tape
        runXADBenchmark(config, setup, nrTrails, warmup, bench,
                        result.xad_mean, result.xad_std,
                        captureValidation ? xadValidation : nullptr);
        result.xad_enabled = true;
        std::cout << "XAD=" << std::fixed << std::setprecision(1) << result.xad_mean << "ms ";

        // XAD-Split (Jacobian + tape MC on intermediates + chain rule)
        {
            double xad_split_fixed = 0;
            runXADSplitBenchmark(config, setup, nrTrails, warmup, bench,
                                 result.xad_split_mean, result.xad_split_std, xad_split_fixed,
                                 captureValidation ? xadSplitValidation : nullptr);
            result.xad_split_enabled = true;
            result.xad_split_fixed_mean = xad_split_fixed;
            std::cout << "XAD-Split=" << result.xad_split_mean << "ms ";
        }

#if defined(QLRISKS_HAS_FORGE)
        if (!xadOnly)
        {
            // Forge JIT
            runJITBenchmark(config, setup, nrTrails, warmup, bench,
                            result.jit_mean, result.jit_std,
                            captureValidation ? jitValidation : nullptr);
            result.jit_enabled = true;
            std::cout << "JIT=" << result.jit_mean << "ms ";

            // Forge JIT-AVX (batched execution)
            runJITAVXBenchmark(config, setup, nrTrails, warmup, bench,
                               result.jit_avx_mean, result.jit_avx_std,
                               captureValidation ? jitAvxValidation : nullptr);
            result.jit_avx_enabled = true;
            std::cout << "JIT-AVX=" << result.jit_avx_mean << "ms";
        }
#else
        (void)xadOnly;
#endif

        std::cout << "\n";
        results.push_back(result);
    }

    std::cout << "\n";
    return results;
}

// ============================================================================
// Main AAD Benchmark Runner (Dual-Curve / Production)
// ============================================================================

std::vector<TimingResult> runAADBenchmarkDualCurve(const BenchmarkConfig& config,
                                                    bool quickMode, bool xadOnly,
                                                    ValidationResult* xadValidation = nullptr,
                                                    ValidationResult* xadSplitValidation = nullptr,
                                                    ValidationResult* jitValidation = nullptr,
                                                    ValidationResult* jitAvxValidation = nullptr)
{
    std::vector<TimingResult> results;

    // Setup LMM
    LMMSetup setup(config);

    std::cout << "================================================================================\n";
    std::cout << "  RUNNING AAD BENCHMARKS (Dual-Curve)\n";
    std::cout << "================================================================================\n";
    std::cout << "\n";

    for (size_t tc = 0; tc < config.pathCounts.size(); ++tc)
    {
        int paths = config.pathCounts[tc];
        Size nrTrails = static_cast<Size>(paths);

        TimingResult result;
        result.pathCount = paths;

        std::cout << "  [" << (tc + 1) << "/" << config.pathCounts.size() << "] "
                  << formatPathCount(paths) << " paths (" << config.numMarketQuotes()
                  << " sensitivities) " << std::flush;

        // For high path counts, skip warmup and use fewer iterations
        size_t warmup, bench;
        if (nrTrails >= 100000) {
            warmup = 0;
            bench = 1;  // Single run for 100K+ (too expensive for multiple)
        } else if (nrTrails >= 10000) {
            warmup = 0;
            bench = 2;
        } else {
            warmup = quickMode ? 1 : config.warmupIterations;
            bench = quickMode ? 2 : config.benchmarkIterations;
        }

        // Capture validation at VALIDATION_PATH_COUNT
        bool captureValidation = (paths == VALIDATION_PATH_COUNT);

        // XAD tape (dual-curve)
        runXADBenchmarkDualCurve(config, setup, nrTrails, warmup, bench,
                                  result.xad_mean, result.xad_std,
                                  captureValidation ? xadValidation : nullptr);
        result.xad_enabled = true;
        std::cout << "XAD=" << std::fixed << std::setprecision(1) << result.xad_mean << "ms ";

        // XAD-Split (dual-curve)
        {
            double xad_split_fixed = 0;
            runXADSplitBenchmarkDualCurve(config, setup, nrTrails, warmup, bench,
                                           result.xad_split_mean, result.xad_split_std, xad_split_fixed,
                                           captureValidation ? xadSplitValidation : nullptr);
            result.xad_split_enabled = true;
            result.xad_split_fixed_mean = xad_split_fixed;
            std::cout << "XAD-Split=" << result.xad_split_mean << "ms ";
        }

#if defined(QLRISKS_HAS_FORGE)
        if (!xadOnly)
        {
            // Forge JIT (dual-curve)
            double jit_p1 = 0, jit_p2 = 0, jit_p3 = 0;
            runJITBenchmarkDualCurve(config, setup, nrTrails, warmup, bench,
                                      result.jit_mean, result.jit_std,
                                      jit_p1, jit_p2, jit_p3,
                                      captureValidation ? jitValidation : nullptr);
            result.jit_enabled = true;
            result.jit_phase1_curve_mean = jit_p1;
            result.jit_phase2_jacobian_mean = jit_p2;
            result.jit_phase3_compile_mean = jit_p3;
            result.jit_fixed_mean = jit_p1 + jit_p2 + jit_p3;
            std::cout << "JIT=" << result.jit_mean << "ms ";

            // Forge JIT-AVX (dual-curve, batched execution)
            double avx_p1 = 0, avx_p2 = 0, avx_p3 = 0;
            runJITAVXBenchmarkDualCurve(config, setup, nrTrails, warmup, bench,
                                         result.jit_avx_mean, result.jit_avx_std,
                                         avx_p1, avx_p2, avx_p3,
                                         captureValidation ? jitAvxValidation : nullptr);
            result.jit_avx_enabled = true;
            std::cout << "JIT-AVX=" << result.jit_avx_mean << "ms";
        }
#else
        (void)xadOnly;
#endif

        std::cout << "\n";
        results.push_back(result);
    }

    std::cout << "\n";
    return results;
}

// ============================================================================
// Output Results in Machine-Parseable Format
// ============================================================================

void outputResultsForParsing(const std::vector<TimingResult>& results,
                              const std::string& configId)
{
    // XAD results
    std::cout << "XAD_" << configId << ":";
    for (size_t i = 0; i < results.size(); ++i)
    {
        const auto& r = results[i];
        if (i > 0) std::cout << ";";
        std::cout << r.pathCount << "=" << r.xad_mean << "," << r.xad_std
                  << "," << (r.xad_enabled ? "1" : "0");
    }
    std::cout << std::endl;

    // XAD-Split results (includes fixed cost: mean,std,enabled,fixed_cost)
    std::cout << "XADSPLIT_" << configId << ":";
    for (size_t i = 0; i < results.size(); ++i)
    {
        const auto& r = results[i];
        if (i > 0) std::cout << ";";
        std::cout << r.pathCount << "=" << r.xad_split_mean << "," << r.xad_split_std
                  << "," << (r.xad_split_enabled ? "1" : "0")
                  << "," << r.xad_split_fixed_mean;
    }
    std::cout << std::endl;

#if defined(QLRISKS_HAS_FORGE)
    // JIT results (now includes fixed cost: mean,std,enabled,fixed_cost)
    std::cout << "JIT_" << configId << ":";
    for (size_t i = 0; i < results.size(); ++i)
    {
        const auto& r = results[i];
        if (i > 0) std::cout << ";";
        std::cout << r.pathCount << "=" << r.jit_mean << "," << r.jit_std
                  << "," << (r.jit_enabled ? "1" : "0")
                  << "," << r.jit_fixed_mean;
    }
    std::cout << std::endl;

    // JIT-AVX results (now includes fixed cost: mean,std,enabled,fixed_cost)
    std::cout << "JITAVX_" << configId << ":";
    for (size_t i = 0; i < results.size(); ++i)
    {
        const auto& r = results[i];
        if (i > 0) std::cout << ";";
        std::cout << r.pathCount << "=" << r.jit_avx_mean << "," << r.jit_avx_std
                  << "," << (r.jit_avx_enabled ? "1" : "0")
                  << "," << r.jit_fixed_mean;  // Same fixed cost as JIT
    }
    std::cout << std::endl;

    // JIT phase breakdown (one-time costs: phase1_curve, phase2_jacobian, phase3_compile)
    // Output from first result that has JIT enabled (phases are same for all path counts)
    for (const auto& r : results)
    {
        if (r.jit_enabled)
        {
            std::cout << "JIT_PHASES_" << configId << ":"
                      << r.jit_phase1_curve_mean << ","
                      << r.jit_phase2_jacobian_mean << ","
                      << r.jit_phase3_compile_mean << std::endl;
            break;
        }
    }
#endif
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[])
{
    bool runLite = false;
    bool runLiteExtended = false;
    bool runProduction = false;
    bool runAll = true;  // Default: run lite and lite-extended (not production)
    bool quickMode = false;
    bool xadOnly = false;
    bool runDiagnose = false;
    Size diagnosePaths = 100;

    // Parse arguments
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--lite") == 0)
        {
            runLite = true;
            runAll = false;
        }
        else if (strcmp(argv[i], "--lite-extended") == 0)
        {
            runLiteExtended = true;
            runAll = false;
        }
        else if (strcmp(argv[i], "--production") == 0)
        {
            runProduction = true;
            runAll = false;
        }
        else if (strcmp(argv[i], "--all") == 0)
        {
            runLite = true;
            runLiteExtended = true;
            runProduction = true;
            runAll = false;
        }
        else if (strcmp(argv[i], "--quick") == 0)
        {
            quickMode = true;
        }
        else if (strcmp(argv[i], "--xad-only") == 0)
        {
            xadOnly = true;
        }
        else if (strcmp(argv[i], "--diagnose") == 0)
        {
            runDiagnose = true;
            runAll = false;
        }
        else if (strncmp(argv[i], "--diagnose-paths=", 17) == 0)
        {
            diagnosePaths = static_cast<Size>(std::atoi(argv[i] + 17));
        }
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            std::cout << "Usage: " << argv[0] << " [options]\n";
            std::cout << "Options:\n";
            std::cout << "  --lite           Run lite benchmark (1Y into 1Y, 9 sensitivities)\n";
            std::cout << "  --lite-extended  Run lite-extended benchmark (5Y into 5Y, 14 sensitivities)\n";
            std::cout << "  --production     Run production benchmark (5Y into 5Y dual-curve, 47 sensitivities)\n";
            std::cout << "  --all            Run all benchmarks including production\n";
            std::cout << "  --quick          Quick mode (fewer iterations)\n";
            std::cout << "  --xad-only       Run only XAD tape (no JIT)\n";
            std::cout << "  --diagnose       Run diagnostic comparison of XAD-Split vs JIT\n";
            std::cout << "  --diagnose-paths=N  Number of paths for diagnostic (default: 100)\n";
            std::cout << "  --help           Show this message\n";
            std::cout << "\n";
            std::cout << "Default: runs lite and lite-extended (not production)\n";
            return 0;
        }
    }

    // Default behavior: run lite and lite-extended
    if (runAll)
    {
        runLite = true;
        runLiteExtended = true;
        runProduction = false;  // Production opt-in only
    }

    printHeader();
    printEnvironment();

#if defined(QLRISKS_HAS_FORGE)
    // Run diagnostic comparison if requested
    if (runDiagnose)
    {
        std::cout << "\nRunning XAD-Split vs JIT diagnostic comparison...\n";

        // Lite config diagnostic
        {
            BenchmarkConfig liteConfig;
            LMMSetup setup(liteConfig);
            std::cout << "\n--- LITE CONFIG ---\n";
            runDiagnosticComparison(liteConfig, setup, diagnosePaths);
        }

        // Lite-Extended config diagnostic
        {
            BenchmarkConfig liteExtConfig;
            liteExtConfig.setLiteExtendedConfig();
            LMMSetup setup(liteExtConfig);
            std::cout << "\n--- LITE-EXTENDED CONFIG ---\n";
            runDiagnosticComparison(liteExtConfig, setup, diagnosePaths);
        }

        return 0;
    }
#endif

    int benchmarkNum = 1;

    if (runLite)
    {
        BenchmarkConfig liteConfig;
        printBenchmarkHeader(liteConfig, benchmarkNum++);

        ValidationResult xadValidation, xadSplitValidation, jitValidation, jitAvxValidation;
        auto results = runAADBenchmark(liteConfig, quickMode, xadOnly,
                                       &xadValidation, &xadSplitValidation, &jitValidation, &jitAvxValidation);
        printResultsTable(results);
        printResultsFooter(liteConfig);
        outputResultsForParsing(results, liteConfig.configId);
        if (!xadValidation.sensitivities.empty())
            outputValidationData(xadValidation, liteConfig.configId);
        if (!xadSplitValidation.sensitivities.empty())
            outputValidationData(xadSplitValidation, liteConfig.configId);
        if (!jitValidation.sensitivities.empty())
            outputValidationData(jitValidation, liteConfig.configId);
        if (!jitAvxValidation.sensitivities.empty())
            outputValidationData(jitAvxValidation, liteConfig.configId);
    }

    if (runLiteExtended)
    {
        BenchmarkConfig liteExtConfig;
        liteExtConfig.setLiteExtendedConfig();
        printBenchmarkHeader(liteExtConfig, benchmarkNum++);

        ValidationResult xadValidation, xadSplitValidation, jitValidation, jitAvxValidation;
        auto results = runAADBenchmark(liteExtConfig, quickMode, xadOnly,
                                       &xadValidation, &xadSplitValidation, &jitValidation, &jitAvxValidation);
        printResultsTable(results);
        printResultsFooter(liteExtConfig);
        outputResultsForParsing(results, liteExtConfig.configId);
        if (!xadValidation.sensitivities.empty())
            outputValidationData(xadValidation, liteExtConfig.configId);
        if (!xadSplitValidation.sensitivities.empty())
            outputValidationData(xadSplitValidation, liteExtConfig.configId);
        if (!jitValidation.sensitivities.empty())
            outputValidationData(jitValidation, liteExtConfig.configId);
        if (!jitAvxValidation.sensitivities.empty())
            outputValidationData(jitAvxValidation, liteExtConfig.configId);
    }

    if (runProduction)
    {
        BenchmarkConfig prodConfig;
        prodConfig.setProductionConfig();
        printBenchmarkHeader(prodConfig, benchmarkNum++);

        ValidationResult xadValidation, xadSplitValidation, jitValidation, jitAvxValidation;
        auto results = runAADBenchmarkDualCurve(prodConfig, quickMode, xadOnly,
                                                &xadValidation, &xadSplitValidation, &jitValidation, &jitAvxValidation);
        printResultsTable(results);
        printResultsFooter(prodConfig);
        outputResultsForParsing(results, prodConfig.configId);
        if (!xadValidation.sensitivities.empty())
            outputValidationData(xadValidation, prodConfig.configId);
        if (!xadSplitValidation.sensitivities.empty())
            outputValidationData(xadSplitValidation, prodConfig.configId);
        if (!jitValidation.sensitivities.empty())
            outputValidationData(jitValidation, prodConfig.configId);
        if (!jitAvxValidation.sensitivities.empty())
            outputValidationData(jitAvxValidation, prodConfig.configId);
    }

    printFooter();

    return 0;
}

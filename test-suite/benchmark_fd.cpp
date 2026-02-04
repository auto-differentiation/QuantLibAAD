/*******************************************************************************
 *
 *  QuantLib-Risks Swaption Benchmark - FD Runner
 *
 *  Finite Differences benchmark using plain double QuantLib.
 *  This executable is compiled WITHOUT XAD to ensure fair FD comparison.
 *
 *  Usage:
 *    ./benchmark_fd [--production|--cva|--all] [--quick]
 *
 *  Output format is designed to be parsed and combined with AAD results.
 *
 *  Copyright (C) 2025 Xcelerit Computing Limited
 *  SPDX-License-Identifier: AGPL-3.0-or-later
 *
 ******************************************************************************/

#include "benchmark_common.hpp"
#include "benchmark_pricing.hpp"

#include <chrono>
#include <cstring>
#include <fstream>

using namespace benchmark;
using Clock = std::chrono::high_resolution_clock;
using DurationMs = std::chrono::duration<double, std::milli>;

// ============================================================================
// FD Benchmark Runner (unified single/dual-curve)
// ============================================================================

template <bool UseDualCurve>
std::vector<TimingResult> runFDBenchmarkT(const BenchmarkConfig& config, bool quickMode,
                                           ValidationResult* validation = nullptr)
{
    std::vector<TimingResult> results;

    // Setup LMM (pre-compute grid, randoms, etc.)
    LMMSetup setup(config);

    std::cout << "================================================================================\n";
    if constexpr (UseDualCurve)
        std::cout << "  RUNNING FD BENCHMARKS (Dual-Curve)\n";
    else
        std::cout << "  RUNNING FD BENCHMARKS\n";
    std::cout << "================================================================================\n";
    std::cout << "\n";

    const int maxFDPaths = config.getMaxFDPaths();

    for (size_t tc = 0; tc < config.pathCounts.size(); ++tc)
    {
        int paths = config.pathCounts[tc];
        Size nrTrails = static_cast<Size>(paths);

        TimingResult result;
        result.pathCount = paths;

        // Only run FD for small path counts
        if (paths > maxFDPaths)
        {
            std::cout << "  [" << (tc + 1) << "/" << config.pathCounts.size() << "] "
                      << formatPathCount(paths) << " paths - SKIPPED (paths > " << maxFDPaths << ")\n";
            results.push_back(result);
            continue;
        }

        std::cout << "  [" << (tc + 1) << "/" << config.pathCounts.size() << "] "
                  << formatPathCount(paths) << " paths ";
        if constexpr (UseDualCurve)
            std::cout << "(" << config.numMarketQuotes() << " sensitivities) ";
        std::cout << std::flush;

        std::vector<double> fd_times;
        double eps = 1e-5;

        // For high path counts, skip warmup and run once (FD is expensive)
        size_t warmup, bench;
        if (paths >= 10000) {
            warmup = 0;
            bench = 1;
        } else {
            warmup = quickMode ? 1 : config.warmupIterations;
            bench = quickMode ? 2 : config.benchmarkIterations;
        }

        for (size_t iter = 0; iter < warmup + bench; ++iter)
        {
            auto t_start = Clock::now();

            // Base rates as plain double
            std::vector<double> baseDepo(config.depoRates.begin(), config.depoRates.end());
            std::vector<double> baseSwap(config.swapRates.begin(), config.swapRates.end());
            std::vector<double> baseOisDepo;
            std::vector<double> baseOisSwap;
            if constexpr (UseDualCurve)
            {
                baseOisDepo.assign(config.oisDepoRates.begin(), config.oisDepoRates.end());
                baseOisSwap.assign(config.oisSwapRates.begin(), config.oisSwapRates.end());
            }

            // Compute base price
            double basePrice;
            if constexpr (UseDualCurve)
                basePrice = priceSwaptionDualCurve<double>(
                    config, setup, baseDepo, baseSwap, baseOisDepo, baseOisSwap, nrTrails);
            else
                basePrice = priceSwaption<double>(config, setup, baseDepo, baseSwap, nrTrails);

            // Compute FD sensitivities (bump each rate)
            std::vector<double> derivatives(config.numMarketQuotes());
            Size q = 0;

            // Bump forecasting curve deposits
            for (Size idx = 0; idx < config.numDeposits; ++idx, ++q)
            {
                std::vector<double> bumpedDepo = baseDepo;
                bumpedDepo[idx] += eps;

                double bumpedPrice;
                if constexpr (UseDualCurve)
                    bumpedPrice = priceSwaptionDualCurve<double>(
                        config, setup, bumpedDepo, baseSwap, baseOisDepo, baseOisSwap, nrTrails);
                else
                    bumpedPrice = priceSwaption<double>(config, setup, bumpedDepo, baseSwap, nrTrails);
                derivatives[q] = (bumpedPrice - basePrice) / eps;
            }

            // Bump forecasting curve swaps
            for (Size idx = 0; idx < config.numSwaps; ++idx, ++q)
            {
                std::vector<double> bumpedSwap = baseSwap;
                bumpedSwap[idx] += eps;

                double bumpedPrice;
                if constexpr (UseDualCurve)
                    bumpedPrice = priceSwaptionDualCurve<double>(
                        config, setup, baseDepo, bumpedSwap, baseOisDepo, baseOisSwap, nrTrails);
                else
                    bumpedPrice = priceSwaption<double>(config, setup, baseDepo, bumpedSwap, nrTrails);
                derivatives[q] = (bumpedPrice - basePrice) / eps;
            }

            // Bump discounting curve (OIS) for dual-curve only
            if constexpr (UseDualCurve)
            {
                for (Size idx = 0; idx < config.numOisDeposits; ++idx, ++q)
                {
                    std::vector<double> bumpedOisDepo = baseOisDepo;
                    bumpedOisDepo[idx] += eps;

                    double bumpedPrice = priceSwaptionDualCurve<double>(
                        config, setup, baseDepo, baseSwap, bumpedOisDepo, baseOisSwap, nrTrails);
                    derivatives[q] = (bumpedPrice - basePrice) / eps;
                }

                for (Size idx = 0; idx < config.numOisSwaps; ++idx, ++q)
                {
                    std::vector<double> bumpedOisSwap = baseOisSwap;
                    bumpedOisSwap[idx] += eps;

                    double bumpedPrice = priceSwaptionDualCurve<double>(
                        config, setup, baseDepo, baseSwap, baseOisDepo, bumpedOisSwap, nrTrails);
                    derivatives[q] = (bumpedPrice - basePrice) / eps;
                }
            }

            auto t_end = Clock::now();

            if (iter >= warmup)
            {
                fd_times.push_back(DurationMs(t_end - t_start).count());
            }

            // Capture validation data on first iteration at VALIDATION_PATH_COUNT
            if (validation && paths == VALIDATION_PATH_COUNT && iter == 0)
            {
                *validation = ValidationResult("FD", basePrice, derivatives);
            }
        }

        result.fd_mean = computeMean(fd_times);
        result.fd_std = computeStddev(fd_times);
        result.fd_enabled = true;

        std::cout << "done (" << std::fixed << std::setprecision(1)
                  << result.fd_mean << " ms)\n";

        results.push_back(result);
    }

    std::cout << "\n";
    return results;
}

// Convenience wrappers for backward compatibility
inline std::vector<TimingResult> runFDBenchmark(const BenchmarkConfig& config, bool quickMode,
                                                 ValidationResult* validation = nullptr)
{
    return runFDBenchmarkT<false>(config, quickMode, validation);
}

inline std::vector<TimingResult> runFDBenchmarkDualCurve(const BenchmarkConfig& config, bool quickMode,
                                                          ValidationResult* validation = nullptr)
{
    return runFDBenchmarkT<true>(config, quickMode, validation);
}

// ============================================================================
// FD Benchmark Runner for CVA (90 inputs)
// ============================================================================

std::vector<TimingResult> runFDBenchmarkCVA(const BenchmarkConfig& config, bool quickMode,
                                             ValidationResult* validation = nullptr)
{
    std::vector<TimingResult> results;

    // Setup LMM (pre-compute grid, randoms, etc.)
    LMMSetup setup(config);

    std::cout << "================================================================================\n";
    std::cout << "  RUNNING FD BENCHMARKS (CVA - 90 inputs)\n";
    std::cout << "================================================================================\n";
    std::cout << "\n";

    const int maxFDPaths = config.getMaxFDPaths();

    for (size_t tc = 0; tc < config.pathCounts.size(); ++tc)
    {
        int paths = config.pathCounts[tc];
        Size nrTrails = static_cast<Size>(paths);

        TimingResult result;
        result.pathCount = paths;

        // Only run FD for small path counts
        if (paths > maxFDPaths)
        {
            std::cout << "  [" << (tc + 1) << "/" << config.pathCounts.size() << "] "
                      << formatPathCount(paths) << " paths - SKIPPED (paths > " << maxFDPaths << ")\n";
            results.push_back(result);
            continue;
        }

        std::cout << "  [" << (tc + 1) << "/" << config.pathCounts.size() << "] "
                  << formatPathCount(paths) << " paths (" << config.numMarketQuotes() << " sensitivities) " << std::flush;

        std::vector<double> fd_times;
        double eps = 1e-5;

        // For high path counts, skip warmup and run once (FD is expensive)
        size_t warmup, bench;
        if (paths >= 10000) {
            warmup = 0;
            bench = 1;
        } else {
            warmup = quickMode ? 1 : config.warmupIterations;
            bench = quickMode ? 2 : config.benchmarkIterations;
        }

        for (size_t iter = 0; iter < warmup + bench; ++iter)
        {
            auto t_start = Clock::now();

            // Base rates as plain double
            std::vector<double> baseDepo(config.depoRates.begin(), config.depoRates.end());
            std::vector<double> baseSwap(config.swapRates.begin(), config.swapRates.end());
            std::vector<double> baseOisDepo(config.oisDepoRates.begin(), config.oisDepoRates.end());
            std::vector<double> baseOisSwap(config.oisSwapRates.begin(), config.oisSwapRates.end());
            std::vector<double> baseCounterpartyCds(config.counterpartyCdsSpreads.begin(), config.counterpartyCdsSpreads.end());
            std::vector<double> baseOwnCds(config.ownCdsSpreads.begin(), config.ownCdsSpreads.end());

            // Compute base price
            double basePrice = priceSwaptionWithCVA<double>(
                config, setup, baseDepo, baseSwap, baseOisDepo, baseOisSwap,
                baseCounterpartyCds, baseOwnCds, nrTrails);

            // Compute FD sensitivities (bump each rate)
            std::vector<double> derivatives(config.numMarketQuotes());
            Size q = 0;

            // Bump forecasting curve deposits
            for (Size idx = 0; idx < config.numDeposits; ++idx, ++q)
            {
                std::vector<double> bumpedDepo = baseDepo;
                bumpedDepo[idx] += eps;
                double bumpedPrice = priceSwaptionWithCVA<double>(
                    config, setup, bumpedDepo, baseSwap, baseOisDepo, baseOisSwap,
                    baseCounterpartyCds, baseOwnCds, nrTrails);
                derivatives[q] = (bumpedPrice - basePrice) / eps;
            }

            // Bump forecasting curve swaps
            for (Size idx = 0; idx < config.numSwaps; ++idx, ++q)
            {
                std::vector<double> bumpedSwap = baseSwap;
                bumpedSwap[idx] += eps;
                double bumpedPrice = priceSwaptionWithCVA<double>(
                    config, setup, baseDepo, bumpedSwap, baseOisDepo, baseOisSwap,
                    baseCounterpartyCds, baseOwnCds, nrTrails);
                derivatives[q] = (bumpedPrice - basePrice) / eps;
            }

            // Bump discounting curve (OIS) deposits
            for (Size idx = 0; idx < config.numOisDeposits; ++idx, ++q)
            {
                std::vector<double> bumpedOisDepo = baseOisDepo;
                bumpedOisDepo[idx] += eps;
                double bumpedPrice = priceSwaptionWithCVA<double>(
                    config, setup, baseDepo, baseSwap, bumpedOisDepo, baseOisSwap,
                    baseCounterpartyCds, baseOwnCds, nrTrails);
                derivatives[q] = (bumpedPrice - basePrice) / eps;
            }

            // Bump discounting curve (OIS) swaps
            for (Size idx = 0; idx < config.numOisSwaps; ++idx, ++q)
            {
                std::vector<double> bumpedOisSwap = baseOisSwap;
                bumpedOisSwap[idx] += eps;
                double bumpedPrice = priceSwaptionWithCVA<double>(
                    config, setup, baseDepo, baseSwap, baseOisDepo, bumpedOisSwap,
                    baseCounterpartyCds, baseOwnCds, nrTrails);
                derivatives[q] = (bumpedPrice - basePrice) / eps;
            }

            // Bump counterparty CDS spreads
            for (Size idx = 0; idx < config.numCounterpartyCds; ++idx, ++q)
            {
                std::vector<double> bumpedCounterpartyCds = baseCounterpartyCds;
                bumpedCounterpartyCds[idx] += eps;
                double bumpedPrice = priceSwaptionWithCVA<double>(
                    config, setup, baseDepo, baseSwap, baseOisDepo, baseOisSwap,
                    bumpedCounterpartyCds, baseOwnCds, nrTrails);
                derivatives[q] = (bumpedPrice - basePrice) / eps;
            }

            // Bump own CDS spreads
            for (Size idx = 0; idx < config.numOwnCds; ++idx, ++q)
            {
                std::vector<double> bumpedOwnCds = baseOwnCds;
                bumpedOwnCds[idx] += eps;
                double bumpedPrice = priceSwaptionWithCVA<double>(
                    config, setup, baseDepo, baseSwap, baseOisDepo, baseOisSwap,
                    baseCounterpartyCds, bumpedOwnCds, nrTrails);
                derivatives[q] = (bumpedPrice - basePrice) / eps;
            }

            auto t_end = Clock::now();

            if (iter >= warmup)
            {
                fd_times.push_back(DurationMs(t_end - t_start).count());
            }

            // Capture validation data on first iteration at VALIDATION_PATH_COUNT
            if (validation && paths == VALIDATION_PATH_COUNT && iter == 0)
            {
                *validation = ValidationResult("FD", basePrice, derivatives);
            }
        }

        result.fd_mean = computeMean(fd_times);
        result.fd_std = computeStddev(fd_times);
        result.fd_enabled = true;

        std::cout << "done (" << std::fixed << std::setprecision(1)
                  << result.fd_mean << " ms)\n";

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
    // Output format: FD_CONFIG_ID:paths=mean,std;paths=mean,std;...
    std::cout << "FD_" << configId << ":";
    for (size_t i = 0; i < results.size(); ++i)
    {
        const auto& r = results[i];
        if (i > 0) std::cout << ";";
        std::cout << r.pathCount << "=" << r.fd_mean << "," << r.fd_std
                  << "," << (r.fd_enabled ? "1" : "0");
    }
    std::cout << std::endl;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[])
{
    bool runProduction = false;
    bool runCVA = false;
    bool runAll = true;  // Default: run both production and CVA
    bool quickMode = false;

    // Parse arguments
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--production") == 0)
        {
            runProduction = true;
            runAll = false;
        }
        else if (strcmp(argv[i], "--cva") == 0)
        {
            runCVA = true;
            runAll = false;
        }
        else if (strcmp(argv[i], "--all") == 0)
        {
            runProduction = true;
            runCVA = true;
            runAll = false;
        }
        else if (strcmp(argv[i], "--quick") == 0)
        {
            quickMode = true;
        }
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            std::cout << "Usage: " << argv[0] << " [options]\n";
            std::cout << "Options:\n";
            std::cout << "  --production     Run production benchmark (5Y into 5Y dual-curve, 45 sensitivities)\n";
            std::cout << "  --cva            Run CVA benchmark (5Y into 5Y dual-curve + credit, 90 sensitivities)\n";
            std::cout << "  --all            Run all benchmarks (production + CVA)\n";
            std::cout << "  --quick          Quick mode (fewer iterations)\n";
            std::cout << "  --help           Show this message\n";
            std::cout << "\n";
            std::cout << "Default: runs both production and CVA benchmarks\n";
            return 0;
        }
    }

    // Default behavior: run both production and CVA
    if (runAll)
    {
        runProduction = true;
        runCVA = true;
    }

    printHeader();
    printEnvironment();

    int benchmarkNum = 1;

    if (runProduction)
    {
        BenchmarkConfig prodConfig;
        prodConfig.setProductionConfig();
        printBenchmarkHeader(prodConfig, benchmarkNum++);

        ValidationResult validation;
        auto results = runFDBenchmarkDualCurve(prodConfig, quickMode, &validation);
        printResultsTable(results);
        printResultsFooter(prodConfig);
        outputResultsForParsing(results, prodConfig.configId);
        if (!validation.sensitivities.empty())
            outputValidationData(validation, prodConfig.configId);
    }

    if (runCVA)
    {
        BenchmarkConfig cvaConfig;
        cvaConfig.setCVAConfig();
        printBenchmarkHeader(cvaConfig, benchmarkNum++);

        ValidationResult validation;
        auto results = runFDBenchmarkCVA(cvaConfig, quickMode, &validation);
        printResultsTable(results);
        printResultsFooter(cvaConfig);
        outputResultsForParsing(results, cvaConfig.configId);
        if (!validation.sensitivities.empty())
            outputValidationData(validation, cvaConfig.configId);
    }

    printFooter();

    return 0;
}

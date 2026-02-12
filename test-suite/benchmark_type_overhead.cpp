/*******************************************************************************
 *
 *  QuantLib-Risks Type Overhead Benchmark
 *
 *  Measures the passive-mode overhead of xad::AReal<double> vs plain double
 *  for pricing-only workloads (no derivatives, no tape recording).
 *
 *  Both types use the SAME templated pricing code (priceSwaption<T>) from
 *  benchmark_pricing.hpp, compiled into a single binary. This ensures a fair
 *  comparison: same code, same compiler flags, same hardware, same process.
 *
 *  Usage:
 *    ./benchmark-overhead [--lite|--lite-extended|--production|--all] [--quick]
 *
 *  Copyright (C) 2025 Xcelerit Computing Limited
 *  SPDX-License-Identifier: AGPL-3.0-or-later
 *
 ******************************************************************************/

#include "benchmark_common.hpp"
#include "benchmark_pricing.hpp"

// XAD includes
#include <XAD/XAD.hpp>

#include <chrono>
#include <cstring>

using namespace benchmark;
using Clock = std::chrono::high_resolution_clock;
using DurationMs = std::chrono::duration<double, std::milli>;

// Use QuantLib's Real which is xad::AReal<double> via qlrisks.hpp
using RealAD = QuantLib::Real;

// ============================================================================
// Overhead benchmark configuration
// ============================================================================

struct OverheadConfig
{
    size_t warmupIterations = 3;
    size_t benchmarkIterations = 10;
    std::vector<int> pathCounts = {1000, 10000, 100000};
};

// ============================================================================
// Result structure for overhead comparison
// ============================================================================

struct OverheadResult
{
    int pathCount = 0;
    double double_mean = 0, double_std = 0;
    double areal_mean = 0, areal_std = 0;
    double overhead_pct = 0;
    double double_pv = 0, areal_pv = 0;  // For validation
};

// ============================================================================
// Run pricing benchmark for a given type
// ============================================================================

template <typename RealType, bool UseDualCurve>
void runPricingBenchmark(const BenchmarkConfig& config, const LMMSetup& setup,
                         Size nrTrails, size_t warmup, size_t bench,
                         double& mean, double& stddev, double& pv)
{
    std::vector<double> times;

    for (size_t iter = 0; iter < warmup + bench; ++iter)
    {
        // Prepare inputs as RealType
        std::vector<RealType> depositRates(config.numDeposits);
        std::vector<RealType> swapRatesT(config.numSwaps);
        for (Size idx = 0; idx < config.numDeposits; ++idx)
            depositRates[idx] = config.depoRates[idx];
        for (Size idx = 0; idx < config.numSwaps; ++idx)
            swapRatesT[idx] = config.swapRates[idx];

        std::vector<RealType> oisDepoRates;
        std::vector<RealType> oisSwapRatesT;
        if constexpr (UseDualCurve)
        {
            oisDepoRates.resize(config.numOisDeposits);
            oisSwapRatesT.resize(config.numOisSwaps);
            for (Size idx = 0; idx < config.numOisDeposits; ++idx)
                oisDepoRates[idx] = config.oisDepoRates[idx];
            for (Size idx = 0; idx < config.numOisSwaps; ++idx)
                oisSwapRatesT[idx] = config.oisSwapRates[idx];
        }

        auto t_start = Clock::now();

        RealType price;
        if constexpr (UseDualCurve)
            price = priceSwaptionDualCurve<RealType>(
                config, setup, depositRates, swapRatesT,
                oisDepoRates, oisSwapRatesT, nrTrails);
        else
            price = priceSwaption<RealType>(
                config, setup, depositRates, swapRatesT, nrTrails);

        auto t_end = Clock::now();

        if (iter >= warmup)
        {
            times.push_back(DurationMs(t_end - t_start).count());
            // Capture PV from last measured iteration
            pv = extractValue(price);
        }
    }

    mean = computeMean(times);
    stddev = computeStddev(times);
}

// ============================================================================
// Run overhead comparison for a benchmark configuration
// ============================================================================

template <bool UseDualCurve>
std::vector<OverheadResult> runOverheadBenchmark(const BenchmarkConfig& config,
                                                  const OverheadConfig& ohConfig)
{
    std::vector<OverheadResult> results;

    // Setup LMM (pre-compute grid, randoms, etc.)
    LMMSetup setup(config);

    std::cout << "================================================================================\n";
    std::cout << "  RUNNING TYPE OVERHEAD BENCHMARK";
    if constexpr (UseDualCurve)
        std::cout << " (Dual-Curve)";
    std::cout << "\n";
    std::cout << "================================================================================\n";
    std::cout << "\n";
    std::cout << "  Warmup:    " << ohConfig.warmupIterations << " iterations\n";
    std::cout << "  Measured:  " << ohConfig.benchmarkIterations << " iterations\n";
    std::cout << "\n";

    for (size_t tc = 0; tc < ohConfig.pathCounts.size(); ++tc)
    {
        int paths = ohConfig.pathCounts[tc];
        Size nrTrails = static_cast<Size>(paths);

        OverheadResult result;
        result.pathCount = paths;

        std::cout << "  [" << (tc + 1) << "/" << ohConfig.pathCounts.size() << "] "
                  << formatPathCount(paths) << " paths:\n";

        // Run with plain double
        std::cout << "    double      ... " << std::flush;
        runPricingBenchmark<double, UseDualCurve>(
            config, setup, nrTrails,
            ohConfig.warmupIterations, ohConfig.benchmarkIterations,
            result.double_mean, result.double_std, result.double_pv);
        std::cout << std::fixed << std::setprecision(1) << result.double_mean << " ms\n";

        // Run with AReal<double> (no tape, passive mode)
        std::cout << "    AReal       ... " << std::flush;
        runPricingBenchmark<RealAD, UseDualCurve>(
            config, setup, nrTrails,
            ohConfig.warmupIterations, ohConfig.benchmarkIterations,
            result.areal_mean, result.areal_std, result.areal_pv);
        std::cout << std::fixed << std::setprecision(1) << result.areal_mean << " ms\n";

        // Compute overhead
        if (result.double_mean > 0)
            result.overhead_pct = ((result.areal_mean - result.double_mean) / result.double_mean) * 100.0;

        results.push_back(result);
    }

    std::cout << "\n";
    return results;
}

// ============================================================================
// Output: Human-readable table
// ============================================================================

void printOverheadTable(const std::vector<OverheadResult>& results,
                        const BenchmarkConfig& config)
{
    std::cout << "================================================================================\n";
    std::cout << "  TYPE OVERHEAD: " << config.benchmarkName << "\n";
    std::cout << "  " << config.instrumentDesc << "\n";
    std::cout << "================================================================================\n";
    std::cout << "\n";

    std::cout << "| " << std::setw(6) << "Paths"
              << " | " << std::setw(12) << "double (ms)"
              << " | " << std::setw(12) << "AReal (ms)"
              << " | " << std::setw(12) << "Overhead"
              << " | " << std::setw(8) << "PV Match"
              << " |\n";
    std::cout << "|-------:|---------" << "----:|----------"
              << "---:|----------" << "---:|--------"
              << "-:|\n";

    for (const auto& r : results)
    {
        std::string pathStr = formatPathCount(r.pathCount);

        // Check PV agreement
        double pvDiff = std::abs(r.double_pv - r.areal_pv);
        double pvRelDiff = (r.double_pv != 0.0)
            ? std::abs(pvDiff / r.double_pv) * 100.0
            : 0.0;
        std::string pvMatch = (pvRelDiff < 0.01) ? "OK" : "DIFF";

        std::cout << "| " << std::setw(6) << pathStr
                  << " | " << std::setw(12) << std::fixed << std::setprecision(1) << r.double_mean
                  << " | " << std::setw(12) << std::fixed << std::setprecision(1) << r.areal_mean
                  << " | " << std::setw(10) << std::fixed << std::setprecision(1) << r.overhead_pct << "%"
                  << " | " << std::setw(8) << pvMatch
                  << " |\n";
    }

    std::cout << "\n";
    std::cout << "  Overhead = (AReal - double) / double * 100%\n";
    std::cout << "  PV Match: OK = relative PV difference < 0.01%\n";
    std::cout << "\n";
}

// ============================================================================
// Output: Machine-parseable format
// ============================================================================

void outputOverheadForParsing(const std::vector<OverheadResult>& results,
                               const std::string& configId)
{
    // Format: OVERHEAD_CONFIG:paths=double_mean,areal_mean,overhead_pct;...
    std::cout << "OVERHEAD_" << configId << ":";
    for (size_t i = 0; i < results.size(); ++i)
    {
        const auto& r = results[i];
        if (i > 0) std::cout << ";";
        std::cout << r.pathCount << "="
                  << std::fixed << std::setprecision(2) << r.double_mean << ","
                  << std::fixed << std::setprecision(2) << r.areal_mean << ","
                  << std::fixed << std::setprecision(1) << r.overhead_pct;
    }
    std::cout << std::endl;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[])
{
    bool runLite = false;
    bool runLiteExtended = false;
    bool runProduction = false;
    bool runAll = true;
    bool quickMode = false;

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
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            std::cout << "Usage: " << argv[0] << " [options]\n";
            std::cout << "Options:\n";
            std::cout << "  --lite           Run lite benchmark (1Y into 1Y, 9 inputs)\n";
            std::cout << "  --lite-extended  Run lite-extended benchmark (5Y into 5Y, 14 inputs)\n";
            std::cout << "  --production     Run production benchmark (5Y into 5Y dual-curve, 45 inputs)\n";
            std::cout << "  --all            Run all benchmarks\n";
            std::cout << "  --quick          Quick mode (fewer iterations)\n";
            std::cout << "  --help           Show this message\n";
            std::cout << "\n";
            std::cout << "Default: runs lite and lite-extended\n";
            return 0;
        }
    }

    if (runAll)
    {
        runLite = true;
        runLiteExtended = true;
        runProduction = false;
    }

    OverheadConfig ohConfig;
    if (quickMode)
    {
        ohConfig.warmupIterations = 1;
        ohConfig.benchmarkIterations = 3;
    }

    printHeader();
    std::cout << "  TYPE OVERHEAD: double vs xad::AReal<double> (pricing-only, no derivatives)\n";
    std::cout << "================================================================================\n";
    std::cout << "\n";
    printEnvironment();

    std::cout << "  METHODOLOGY\n";
    std::cout << "--------------------------------------------------------------------------------\n";
    std::cout << "  Both types use the same templated priceSwaption<T>() function.\n";
    std::cout << "  No tape recording, no derivatives - purely measuring type overhead.\n";
    std::cout << "  Same binary, same hardware, same random numbers.\n";
    std::cout << "\n";

    int benchmarkNum = 1;

    if (runLite)
    {
        BenchmarkConfig config;
        config.pathCounts = ohConfig.pathCounts;
        config.warmupIterations = ohConfig.warmupIterations;
        config.benchmarkIterations = ohConfig.benchmarkIterations;

        printBenchmarkHeader(config, benchmarkNum++);
        auto results = runOverheadBenchmark<false>(config, ohConfig);
        printOverheadTable(results, config);
        outputOverheadForParsing(results, config.configId);
    }

    if (runLiteExtended)
    {
        BenchmarkConfig config;
        config.setLiteExtendedConfig();
        config.pathCounts = ohConfig.pathCounts;
        config.warmupIterations = ohConfig.warmupIterations;
        config.benchmarkIterations = ohConfig.benchmarkIterations;

        printBenchmarkHeader(config, benchmarkNum++);
        auto results = runOverheadBenchmark<false>(config, ohConfig);
        printOverheadTable(results, config);
        outputOverheadForParsing(results, config.configId);
    }

    if (runProduction)
    {
        BenchmarkConfig config;
        config.setProductionConfig();
        config.pathCounts = ohConfig.pathCounts;
        config.warmupIterations = ohConfig.warmupIterations;
        config.benchmarkIterations = ohConfig.benchmarkIterations;

        printBenchmarkHeader(config, benchmarkNum++);
        auto results = runOverheadBenchmark<true>(config, ohConfig);
        printOverheadTable(results, config);
        outputOverheadForParsing(results, config.configId);
    }

    std::cout << "================================================================================\n";
    std::cout << "  SUMMARY\n";
    std::cout << "================================================================================\n";
    std::cout << "\n";
    std::cout << "  Overhead represents the cost of using xad::AReal<double> instead of\n";
    std::cout << "  plain double, even when not computing derivatives (passive mode).\n";
    std::cout << "\n";

    printFooter();

    return 0;
}

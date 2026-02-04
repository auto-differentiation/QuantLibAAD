/*******************************************************************************
 *
 *  QuantLib-Risks Swaption Benchmark - Common Header
 *
 *  Shared utilities, configuration, and output formatting for the benchmark.
 *
 *  Copyright (C) 2025 Xcelerit Computing Limited
 *  SPDX-License-Identifier: AGPL-3.0-or-later
 *
 ******************************************************************************/

#ifndef BENCHMARK_COMMON_HPP
#define BENCHMARK_COMMON_HPP

#include "PlatformInfo.hpp"

// QuantLib includes
#include <ql/quantlib.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace benchmark {

using namespace QuantLib;

// ============================================================================
// Statistics Helpers
// ============================================================================

inline double computeMean(const std::vector<double>& v)
{
    if (v.empty()) return 0.0;
    return std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
}

inline double computeStddev(const std::vector<double>& v)
{
    if (v.size() <= 1) return 0.0;
    double m = computeMean(v);
    double sq_sum = 0.0;
    for (double x : v)
    {
        double diff = x - m;
        sq_sum += diff * diff;
    }
    return std::sqrt(sq_sum / static_cast<double>(v.size() - 1));
}

// ============================================================================
// Benchmark Configuration
// ============================================================================

// Maximum paths for finite differences (FD is O(n) per path for n parameters)
constexpr int FD_MAX_PATHS = 100000;

// Max paths for production config (45 inputs) - same as regular, run all paths
constexpr int FD_MAX_PATHS_PRODUCTION = 100000;

struct BenchmarkConfig
{
    // Forecasting curve market data (Euribor)
    Size numDeposits = 4;
    Size numSwaps = 5;
    std::vector<Period> depoTenors;
    std::vector<Period> swapTenors;
    std::vector<double> depoRates;
    std::vector<double> swapRates;

    // Discounting curve market data (OIS) - only used when useDualCurve=true
    bool useDualCurve = false;
    Size numOisDeposits = 0;
    Size numOisSwaps = 0;
    std::vector<Period> oisDepoTenors;
    std::vector<Period> oisSwapTenors;
    std::vector<double> oisDepoRates;
    std::vector<double> oisSwapRates;

    // Credit curves (CVA) - only used when useCVA=true
    bool useCVA = false;
    Size numCounterpartyCds = 0;
    Size numOwnCds = 0;
    std::vector<Period> counterpartyCdsTenors;
    std::vector<Period> ownCdsTenors;
    std::vector<double> counterpartyCdsSpreads;
    std::vector<double> ownCdsSpreads;
    Real counterpartyRecovery = 0.4;  // Fixed, not an input
    Real ownRecovery = 0.4;           // Fixed, not an input

    // LMM parameters
    Size size = 10;       // Number of forward rates
    Size i_opt = 2;       // Option exercise index
    Size j_opt = 2;       // Swap length
    Size steps = 8;       // Time steps

    // Curve end date
    int curveEndYears = 6;

    // Test configuration
    std::vector<int> pathCounts;
    size_t warmupIterations = 2;
    size_t benchmarkIterations = 5;

    // Instrument description
    std::string instrumentDesc = "European swaption (1Y into 1Y)";
    std::string benchmarkName = "Lite";
    std::string configId = "LITE";  // For machine-parseable output

    // Default: Lite config (1Y into 1Y, single curve, 9 sensitivities)
    BenchmarkConfig()
    {
        depoTenors = {1 * Days, 1 * Months, 3 * Months, 6 * Months};
        swapTenors = {1 * Years, 2 * Years, 3 * Years, 4 * Years, 5 * Years};
        depoRates = {0.0350, 0.0365, 0.0380, 0.0400};
        swapRates = {0.0420, 0.0480, 0.0520, 0.0550, 0.0575};
        pathCounts = {1000, 10000, 100000};  // Testing: 1k and 10k paths
    }

    // Production config (5Y into 5Y, dual curve, ~45 sensitivities)
    // Uses separate forecasting (Euribor) and discounting (OIS) curves
    // This represents a realistic post-2008 multi-curve setup
    void setProductionConfig()
    {
        useDualCurve = true;

        // Forecasting curve: Euribor deposits + swaps (21 quotes)
        numDeposits = 6;
        depoTenors = {1 * Days, 1 * Weeks, 1 * Months, 2 * Months, 3 * Months, 6 * Months};
        depoRates = {0.0320, 0.0325, 0.0335, 0.0345, 0.0355, 0.0375};

        numSwaps = 15;
        swapTenors = {1 * Years, 2 * Years, 3 * Years, 4 * Years, 5 * Years,
                      6 * Years, 7 * Years, 8 * Years, 9 * Years, 10 * Years,
                      12 * Years, 15 * Years, 20 * Years, 25 * Years, 30 * Years};
        swapRates = {0.0380, 0.0420, 0.0450, 0.0472, 0.0490,
                     0.0505, 0.0518, 0.0528, 0.0536, 0.0542,
                     0.0550, 0.0558, 0.0562, 0.0560, 0.0555};

        // Discounting curve: OIS deposits + swaps (24 quotes)
        // Note: OIS deposits must have tenors > settlement days (2) to be "alive"
        numOisDeposits = 4;
        oisDepoTenors = {1 * Months, 2 * Months, 3 * Months, 6 * Months};
        oisDepoRates = {0.0312, 0.0320, 0.0328, 0.0345};

        numOisSwaps = 20;
        oisSwapTenors = {1 * Years, 2 * Years, 3 * Years, 4 * Years, 5 * Years,
                         6 * Years, 7 * Years, 8 * Years, 9 * Years, 10 * Years,
                         11 * Years, 12 * Years, 13 * Years, 14 * Years, 15 * Years,
                         18 * Years, 20 * Years, 25 * Years, 30 * Years, 40 * Years};
        oisSwapRates = {0.0355, 0.0392, 0.0420, 0.0442, 0.0460,
                        0.0475, 0.0488, 0.0498, 0.0506, 0.0512,
                        0.0517, 0.0520, 0.0523, 0.0525, 0.0526,
                        0.0527, 0.0526, 0.0522, 0.0515, 0.0500};

        // Total: 21 + 24 = 45 sensitivities

        // LMM parameters for 5Y into 5Y swaption
        size = 20;
        i_opt = 10;
        j_opt = 10;
        steps = 20;

        curveEndYears = 22;  // Extended for 20Y swap tenors

        // Full path counts including 100K for production
        pathCounts = {1000, 10000, 100000};  // Testing: 1k and 10k paths

        instrumentDesc = "European swaption (5Y into 5Y, dual-curve)";
        benchmarkName = "Production";
        configId = "PRODUCTION";
    }

    Size numForecastingQuotes() const { return numDeposits + numSwaps; }
    Size numDiscountingQuotes() const { return numOisDeposits + numOisSwaps; }
    Size numCreditQuotes() const { return numCounterpartyCds + numOwnCds; }
    Size numMarketQuotes() const { return numForecastingQuotes() + numDiscountingQuotes() + numCreditQuotes(); }

    int getMaxFDPaths() const { return useDualCurve ? FD_MAX_PATHS_PRODUCTION : FD_MAX_PATHS; }

    // CVA config (5Y into 5Y, dual curve + credit, 90 sensitivities)
    // Extends production config with credit curve inputs for CVA calculation
    void setCVAConfig()
    {
        // Start with production config (45 market quotes)
        setProductionConfig();

        // Enable CVA
        useCVA = true;

        // Counterparty CDS spreads (22 tenors - standard market tenors)
        numCounterpartyCds = 22;
        counterpartyCdsTenors = {
            6 * Months, 1 * Years, 2 * Years, 3 * Years, 4 * Years, 5 * Years,
            6 * Years, 7 * Years, 8 * Years, 9 * Years, 10 * Years,
            12 * Years, 15 * Years, 20 * Years, 25 * Years, 30 * Years,
            35 * Years, 40 * Years, 45 * Years, 50 * Years, 60 * Years, 70 * Years
        };
        // Typical investment-grade CDS spreads (in basis points, converted to decimal)
        counterpartyCdsSpreads = {
            0.0015, 0.0020, 0.0030, 0.0040, 0.0048, 0.0055,  // 6M-5Y
            0.0060, 0.0064, 0.0067, 0.0070, 0.0072,           // 6Y-10Y
            0.0075, 0.0078, 0.0080, 0.0081, 0.0082,           // 12Y-30Y
            0.0082, 0.0083, 0.0083, 0.0084, 0.0084, 0.0085    // 35Y-70Y
        };

        // Own CDS spreads (23 tenors - same as counterparty + one extra)
        numOwnCds = 23;
        ownCdsTenors = {
            6 * Months, 1 * Years, 2 * Years, 3 * Years, 4 * Years, 5 * Years,
            6 * Years, 7 * Years, 8 * Years, 9 * Years, 10 * Years,
            12 * Years, 15 * Years, 20 * Years, 25 * Years, 30 * Years,
            35 * Years, 40 * Years, 45 * Years, 50 * Years, 55 * Years, 60 * Years, 70 * Years
        };
        // Own credit typically slightly tighter than counterparty
        ownCdsSpreads = {
            0.0012, 0.0016, 0.0024, 0.0032, 0.0038, 0.0044,  // 6M-5Y
            0.0048, 0.0051, 0.0054, 0.0056, 0.0058,           // 6Y-10Y
            0.0060, 0.0062, 0.0064, 0.0065, 0.0066,           // 12Y-30Y
            0.0066, 0.0067, 0.0067, 0.0068, 0.0068, 0.0068, 0.0069  // 35Y-70Y
        };

        // Recovery rates (fixed, not inputs)
        counterpartyRecovery = 0.4;
        ownRecovery = 0.4;

        // Total: 45 (production) + 22 (counterparty CDS) + 23 (own CDS) = 90 sensitivities
        instrumentDesc = "European swaption (5Y into 5Y, dual-curve + CVA)";
        benchmarkName = "CVA";
        configId = "CVA";
    }
};

// ============================================================================
// Timing Results Structure
// ============================================================================

struct TimingResult
{
    int pathCount = 0;
    double fd_mean = 0, fd_std = 0;           // Finite differences (bump-and-revalue) with double
    double fd_areal_mean = 0, fd_areal_std = 0; // Finite differences with AReal (no tape, measures type overhead)
    double xad_mean = 0, xad_std = 0;         // XAD tape-based AAD
    double xad_split_mean = 0, xad_split_std = 0; // XAD-Split (Jacobian + tape MC on intermediates + chain rule)
    double jit_mean = 0, jit_std = 0;         // Forge JIT scalar
    double jit_avx_mean = 0, jit_avx_std = 0; // Forge JIT AVX2
    bool fd_enabled = false;                  // Whether FD was run for this path count
    bool fd_areal_enabled = false;            // Whether FD-AReal was run
    bool xad_enabled = false;
    bool xad_split_enabled = false;
    bool jit_enabled = false;
    bool jit_avx_enabled = false;

    // JIT phase decomposition (for understanding where time is spent)
    // Phase 1: Curve bootstrap (XAD tape forward pass)
    double jit_phase1_curve_mean = 0;
    // Phase 2: Jacobian computation (XAD adjoint passes)
    double jit_phase2_jacobian_mean = 0;
    // Phase 3: JIT graph recording + kernel compilation
    double jit_phase3_compile_mean = 0;
    // Phase 4: MC execution loop (scales with paths) - computed as total - phases 1-3

    // Legacy: total fixed cost (phases 1-3, doesn't scale with paths)
    double jit_fixed_mean = 0;

    // XAD-Split fixed cost (phases 1-2, no JIT compilation)
    double xad_split_fixed_mean = 0;
};

// ============================================================================
// Validation Results Structure (for comparing sensitivities across methods)
// ============================================================================

struct ValidationResult
{
    std::string method;           // "FD", "XAD", "XADSPLIT", "JIT", "JITAVX"
    double pv = 0.0;              // Present value (price)
    std::vector<double> sensitivities;  // All sensitivities

    ValidationResult() = default;
    ValidationResult(const std::string& m, double p, const std::vector<double>& s)
        : method(m), pv(p), sensitivities(s) {}
};

// Path count used for validation
constexpr int VALIDATION_PATH_COUNT = 10000;

// Output validation data in machine-parseable format
// Format: VALIDATE_CONFIG_METHOD:pv;sens1,sens2,sens3,...
inline void outputValidationData(const ValidationResult& result, const std::string& configId)
{
    std::cout << "VALIDATE_" << configId << "_" << result.method << ":"
              << std::setprecision(12) << result.pv << ";";
    for (size_t i = 0; i < result.sensitivities.size(); ++i)
    {
        if (i > 0) std::cout << ",";
        std::cout << std::setprecision(12) << result.sensitivities[i];
    }
    std::cout << std::endl;
}

// Compare sensitivities across methods and print a validation table
// Mirrors the validation table produced by the GitHub CI workflow
inline void printValidationComparison(
    const std::string& configName,
    const std::vector<ValidationResult*>& results)
{
    // Filter to non-empty results
    std::vector<const ValidationResult*> valid;
    for (auto* r : results)
        if (r && !r->sensitivities.empty())
            valid.push_back(r);

    if (valid.empty()) return;

    // Find FD and XAD as reference methods
    const ValidationResult* fdRef = nullptr;
    const ValidationResult* xadRef = nullptr;
    for (auto* r : valid)
    {
        if (r->method == "FD") fdRef = r;
        if (r->method == "XAD") xadRef = r;
    }

    auto compareSens = [](const std::vector<double>& a, const std::vector<double>& b,
                          double tolPct) -> std::tuple<int, int, double>
    {
        if (a.size() != b.size())
            return {0, static_cast<int>(std::max(a.size(), b.size())), 999.0};
        int matching = 0;
        double maxDiff = 0.0;
        for (size_t i = 0; i < a.size(); ++i)
        {
            if (std::abs(a[i]) < 1e-12 && std::abs(b[i]) < 1e-12) { matching++; continue; }
            double base = std::max(std::abs(a[i]), std::abs(b[i]));
            double rel = base > 1e-12 ? std::abs(a[i] - b[i]) / base * 100.0 : 0.0;
            maxDiff = std::max(maxDiff, rel);
            if (rel <= tolPct) matching++;
        }
        return {matching, static_cast<int>(a.size()), maxDiff};
    };

    // Display names
    auto displayName = [](const std::string& m) -> std::string {
        if (m == "FDAREAL") return "FD-AReal";
        if (m == "XADFULL") return "XAD-Full";
        if (m == "XAD") return "XAD";
        if (m == "FORGEAVX") return "Forge-AVX";
        if (m == "FORGE") return "Forge";
        return m;
    };

    std::string pathStr = VALIDATION_PATH_COUNT >= 1000
        ? std::to_string(VALIDATION_PATH_COUNT / 1000) + "K"
        : std::to_string(VALIDATION_PATH_COUNT);
    std::cout << "\n  VALIDATION (" << configName << ", at " << pathStr << " paths)\n";
    std::cout << "  --------------------------------------------------------------------------\n";
    std::cout << "  " << std::left << std::setw(10) << "Method"
              << std::right << std::setw(14) << "PV"
              << std::setw(6) << "Sens"
              << std::setw(8) << "vs FD"
              << std::setw(12) << "FD MaxDiff"
              << std::setw(8) << "vs XAD"
              << std::setw(12) << "XAD MaxDiff"
              << "\n";
    std::cout << "  --------------------------------------------------------------------------\n";

    // Method order
    std::vector<std::string> order = {"FD", "FDAREAL", "XADFULL", "XAD", "FORGE", "FORGEAVX"};
    for (const auto& m : order)
    {
        const ValidationResult* r = nullptr;
        for (auto* v : valid)
            if (v->method == m) r = v;
        if (!r) continue;

        std::string vsFd = "-", fdMaxDiff = "-";
        std::string vsXad = "-", xadMaxDiff = "-";

        if (m != "FD" && fdRef)
        {
            auto [match, total, maxd] = compareSens(r->sensitivities, fdRef->sensitivities, 1.0);
            vsFd = std::to_string(match) + "/" + std::to_string(total);
            std::ostringstream oss; oss << std::fixed << std::setprecision(2) << maxd << "%";
            fdMaxDiff = oss.str();
        }
        if (m != "FD" && m != "XAD" && xadRef)
        {
            auto [match, total, maxd] = compareSens(r->sensitivities, xadRef->sensitivities, 0.01);
            vsXad = std::to_string(match) + "/" + std::to_string(total);
            std::ostringstream oss; oss << std::fixed << std::setprecision(4) << maxd << "%";
            xadMaxDiff = oss.str();
        }

        std::cout << "  " << std::left << std::setw(10) << displayName(m)
                  << std::right << std::fixed << std::setprecision(6) << std::setw(14) << r->pv
                  << std::setw(6) << r->sensitivities.size()
                  << std::setw(8) << vsFd
                  << std::setw(12) << fdMaxDiff
                  << std::setw(8) << vsXad
                  << std::setw(12) << xadMaxDiff
                  << "\n";
    }
    std::cout << "\n";
}

// ============================================================================
// Output Formatting
// ============================================================================

inline std::string formatPathCount(int paths)
{
    if (paths >= 1000)
        return std::to_string(paths / 1000) + "K";
    return std::to_string(paths);
}

inline void printHeader()
{
    std::cout << "================================================================================\n";
    std::cout << "  QuantLib-Risks Swaption Benchmark\n";
    std::cout << "================================================================================\n";
    std::cout << "\n";
}

inline void printEnvironment()
{
    std::cout << "  ENVIRONMENT\n";
    std::cout << "--------------------------------------------------------------------------------\n";
    std::cout << "  Platform:     " << platform_info::getPlatformInfo() << "\n";
    std::cout << "  CPU:          " << platform_info::getCpuInfo() << "\n";
    std::cout << "  RAM:          " << platform_info::getMemoryInfo() << "\n";
    std::cout << "  SIMD:         " << platform_info::getSimdInfo() << "\n";
    std::cout << "  Compiler:     " << platform_info::getCompilerInfo() << "\n";
    std::cout << "\n";
}

inline void printBenchmarkHeader(const BenchmarkConfig& config, int benchmarkNum)
{
    std::cout << "================================================================================\n";
    std::cout << "  BENCHMARK " << benchmarkNum << ": " << config.benchmarkName << "\n";
    std::cout << "================================================================================\n";
    std::cout << "\n";

    std::cout << "  INSTRUMENT\n";
    std::cout << "--------------------------------------------------------------------------------\n";
    std::cout << "  Instrument:   " << config.instrumentDesc << "\n";
    std::cout << "  Model:        LIBOR Market Model (LMM)\n";
    std::cout << "  Forward rates:" << config.size << " (semi-annual)\n";
    std::cout << "  Time steps:   " << config.steps << "\n";
    if (config.useDualCurve)
    {
        std::cout << "  Curve setup:  Dual-curve (forecasting + discounting)\n";
        std::cout << "  Forecasting:  " << config.numForecastingQuotes() << " quotes ("
                  << config.numDeposits << " deposits + " << config.numSwaps << " swaps)\n";
        std::cout << "  Discounting:  " << config.numDiscountingQuotes() << " quotes ("
                  << config.numOisDeposits << " OIS deposits + " << config.numOisSwaps << " OIS swaps)\n";
        if (config.useCVA)
        {
            std::cout << "  Credit:       " << config.numCreditQuotes() << " CDS spreads ("
                      << config.numCounterpartyCds << " counterparty + " << config.numOwnCds << " own)\n";
        }
        std::cout << "  Total inputs: " << config.numMarketQuotes() << " market quotes\n";
    }
    else
    {
        std::cout << "  Curve setup:  Single-curve\n";
        std::cout << "  Inputs:       " << config.numMarketQuotes() << " market quotes\n";
    }
    std::cout << "\n";

    std::cout << "  BENCHMARK CONFIGURATION\n";
    std::cout << "--------------------------------------------------------------------------------\n";
    std::cout << "  Path counts:  ";
    for (size_t i = 0; i < config.pathCounts.size(); ++i)
    {
        if (i > 0) std::cout << ", ";
        std::cout << formatPathCount(config.pathCounts[i]);
    }
    std::cout << "\n";
    std::cout << "  Warmup:       " << config.warmupIterations << " iterations\n";
    std::cout << "  Measured:     " << config.benchmarkIterations << " iterations\n";
    std::cout << "\n";

    std::cout << "  METHODS\n";
    std::cout << "--------------------------------------------------------------------------------\n";
    std::cout << "  FD        Finite Differences (bump-and-revalue, paths <= " << FD_MAX_PATHS << " only)\n";
    std::cout << "  FD-AReal  Finite Differences with AReal type (measures AReal overhead, no AAD)\n";
    std::cout << "  XAD-Full  XAD tape-based reverse-mode AAD (full tape)\n";
    std::cout << "  XAD       XAD with decoupled Jacobian + chain rule (optimized)\n";
    std::cout << "  Forge     Forge JIT-compiled native code\n";
    std::cout << "  Forge-AVX Forge JIT + AVX2 SIMD (4 paths/instruction)\n";
    std::cout << "\n";
}

inline void printResultsTable(const std::vector<TimingResult>& results)
{
    std::cout << "================================================================================\n";
    std::cout << "  RESULTS (mean +/- stddev, in ms)\n";
    std::cout << "================================================================================\n";
    std::cout << "\n";

    // Check if any JIT or XAD-Split result has fixed cost data
    bool hasFixedCost = false;
    for (const auto& r : results)
    {
        if ((r.jit_enabled && r.jit_fixed_mean > 0) ||
            (r.xad_split_enabled && r.xad_split_fixed_mean > 0))
        {
            hasFixedCost = true;
            break;
        }
    }

    // Table header
    if (hasFixedCost)
    {
        std::cout << "| Paths  |    Method |     Mean |   StdDev |  Setup* | Speedup |\n";
        std::cout << "|-------:|----------:|---------:|---------:|--------:|--------:|\n";
    }
    else
    {
        std::cout << "| Paths  |    Method |     Mean |   StdDev | Speedup |\n";
        std::cout << "|-------:|----------:|---------:|---------:|--------:|\n";
    }

    for (size_t i = 0; i < results.size(); ++i)
    {
        const auto& r = results[i];
        std::string pathStr = formatPathCount(r.pathCount);

        // Determine baseline for speedup calculation
        double baseline = r.fd_enabled ? r.fd_mean : r.xad_mean;
        bool first = true;

        auto printRow = [&](const std::string& method, double mean, double stddev, bool enabled,
                            double fixed_cost = 0.0, bool showFixed = false)
        {
            if (!enabled) return;

            std::cout << "| " << std::setw(6);
            if (first)
            {
                std::cout << pathStr;
                first = false;
            }
            else
            {
                std::cout << "";
            }

            std::cout << " | " << std::setw(9) << method;
            std::cout << " | " << std::setw(8) << std::fixed << std::setprecision(1) << mean;
            std::cout << " | " << std::setw(8) << std::fixed << std::setprecision(1) << stddev;

            // Setup column (only shown when hasFixedCost is true)
            if (hasFixedCost)
            {
                if (showFixed && fixed_cost > 0)
                {
                    std::cout << " | " << std::setw(7) << std::fixed << std::setprecision(1) << fixed_cost;
                }
                else
                {
                    std::cout << " |     ---";
                }
            }

            if (mean == baseline)
            {
                std::cout << " |     --- |";
            }
            else
            {
                double speedup = baseline / mean;
                std::cout << " | " << std::setw(6) << std::fixed << std::setprecision(2) << speedup << "x |";
            }
            std::cout << "\n";
        };

        printRow("FD", r.fd_mean, r.fd_std, r.fd_enabled);
        printRow("FD-AReal", r.fd_areal_mean, r.fd_areal_std, r.fd_areal_enabled);
        printRow("XAD-Full", r.xad_mean, r.xad_std, r.xad_enabled);
        printRow("XAD", r.xad_split_mean, r.xad_split_std, r.xad_split_enabled, r.xad_split_fixed_mean, true);
        printRow("Forge", r.jit_mean, r.jit_std, r.jit_enabled, r.jit_fixed_mean, true);
        printRow("Forge-AVX", r.jit_avx_mean, r.jit_avx_std, r.jit_avx_enabled, r.jit_fixed_mean, true);

        // Separator between path count groups
        if (i < results.size() - 1)
        {
            if (hasFixedCost)
            {
                std::cout << "|--------+-----------+----------+----------+---------+---------|\n";
            }
            else
            {
                std::cout << "|--------+-----------+----------+----------+---------|\n";
            }
        }
    }

    std::cout << "\n";
    std::cout << "  All times in milliseconds (ms).\n";
    std::cout << "  Speedup = FD / Method (or XAD-Full / Method when FD not available).\n";
    if (hasFixedCost)
    {
        std::cout << "\n";
        std::cout << "  * Setup = one-time cost for XAD/Forge methods (curve bootstrap + Jacobian).\n";
        std::cout << "    This cost is constant regardless of path count. The remaining time scales with paths.\n";
    }
    std::cout << "\n";
}

inline void printResultsFooter(const BenchmarkConfig& config)
{
    std::cout << "  FD only runs for paths <= " << config.getMaxFDPaths()
              << " due to O(n) cost per sensitivity.\n";
    std::cout << "\n";
}

inline void printFooter()
{
    std::cout << "================================================================================\n";
    std::cout << "  All benchmarks complete.\n";
}

} // namespace benchmark

#endif // BENCHMARK_COMMON_HPP

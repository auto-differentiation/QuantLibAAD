/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
 Copyright (C) 2025 Xcelerit Computing Limited

 This file is part of QuantLib-Risks / XAD / Forge integration.

 Tests for the XAD-QuantLib JIT Integration Pipeline:
 - Stage 1: Curve Bootstrapping (XAD Tape Mode)
 - Stage 2: Monte Carlo Swaption Pricing (JIT Mode)
 - Stage 3: Adjoint Relay (Reverse Sweep back to market inputs)

 Reference: docs/examplewish.txt
 Based on: Examples/AdjointMulticurveBootstrapping/AdjointMulticurveBootstrappingXAD.cpp
           test-suite/libormarketmodel.cpp (testSwaptionPricing)
*/

#include "toplevelfixture.hpp"
#include "utilities_xad.hpp"

// Stage 1 includes
#include <ql/indexes/ibor/eonia.hpp>
#include <ql/indexes/ibor/euribor.hpp>
#include <ql/instruments/vanillaswap.hpp>
#include <ql/instruments/swaption.hpp>
#include <ql/math/interpolations/cubicinterpolation.hpp>
#include <ql/pricingengines/swap/discountingswapengine.hpp>
#include <ql/termstructures/yield/oisratehelper.hpp>
#include <ql/termstructures/yield/piecewiseyieldcurve.hpp>
#include <ql/termstructures/yield/ratehelpers.hpp>
#include <ql/termstructures/yield/zerocurve.hpp>
#include <ql/time/calendars/target.hpp>
#include <ql/time/daycounters/actual360.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>
#include <ql/time/daycounters/thirty360.hpp>

// Stage 2 includes (LMM Monte Carlo)
#include <ql/legacy/libormarketmodels/lfmcovarproxy.hpp>
#include <ql/legacy/libormarketmodels/lfmswaptionengine.hpp>
#include <ql/legacy/libormarketmodels/liborforwardmodel.hpp>
#include <ql/legacy/libormarketmodels/lmexpcorrmodel.hpp>
#include <ql/legacy/libormarketmodels/lmlinexpvolmodel.hpp>
#include <ql/math/randomnumbers/rngtraits.hpp>
#include <ql/math/statistics/generalstatistics.hpp>
#include <ql/methods/montecarlo/multipathgenerator.hpp>

#include <boost/test/unit_test.hpp>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <vector>
#include <map>

// Forge JIT backends
#include <xad-forge/ForgeBackend.hpp>
#include <xad-forge/ForgeBackendAVX.hpp>

using namespace QuantLib;
using namespace boost::unit_test_framework;

BOOST_FIXTURE_TEST_SUITE(QuantLibRisksTests, TopLevelFixture)

BOOST_AUTO_TEST_SUITE(SwaptionJITPipelineTests)

//////////////////////////////////////////////////////////////////////////////
// Helper: Create IborIndex with ZeroCurve (from libormarketmodel.cpp)
//////////////////////////////////////////////////////////////////////////////

namespace {

ext::shared_ptr<IborIndex> makeIndex(std::vector<Date> dates,
                                     const std::vector<Rate>& rates) {
    DayCounter dayCounter = Actual360();
    RelinkableHandle<YieldTermStructure> termStructure;
    ext::shared_ptr<IborIndex> index(new Euribor6M(termStructure));

    Date todaysDate = index->fixingCalendar().adjust(Date(4, September, 2005));
    Settings::instance().evaluationDate() = todaysDate;

    dates[0] = index->fixingCalendar().advance(todaysDate,
                                                index->fixingDays(), Days);

    termStructure.linkTo(ext::shared_ptr<YieldTermStructure>(
        new ZeroCurve(dates, rates, dayCounter)));

    return index;
}

//////////////////////////////////////////////////////////////////////////////
// High-performance chain rule: result = jacobian^T * derivatives
// For hybrid AD workflows: JIT computes dOutput/dIntermediate,
// Tape computes dIntermediate/dInput, chain rule gives dOutput/dInput.
//////////////////////////////////////////////////////////////////////////////

/// Apply chain rule: result[j] = sum_i(derivatives[i] * jacobian[i * numInputs + j])
/// @param jacobian      Row-major flat array [numIntermediates x numInputs]
/// @param derivatives   Vector [numIntermediates] (dOutput/dIntermediate)
/// @param result        Output vector [numInputs] (dOutput/dInput) - zeroed first
/// @param numIntermediates  Number of intermediate variables
/// @param numInputs     Number of input variables
inline void applyChainRule(const double* __restrict jacobian,
                           const double* __restrict derivatives,
                           double* __restrict result,
                           std::size_t numIntermediates,
                           std::size_t numInputs)
{
    // Zero result
    for (std::size_t j = 0; j < numInputs; ++j)
        result[j] = 0.0;

    // Accumulate: result[j] += derivatives[i] * jacobian[i,j]
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

} // anonymous namespace

//////////////////////////////////////////////////////////////////////////////
// Stage 1: Curve Bootstrapping with XAD Tape
// Based on Examples/AdjointMulticurveBootstrapping/AdjointMulticurveBootstrappingXAD.cpp
//////////////////////////////////////////////////////////////////////////////

BOOST_AUTO_TEST_CASE(testStage1_CurveBootstrapping)
{
    BOOST_TEST_MESSAGE("Testing Stage 1: Curve Bootstrapping with XAD Tape...");

    std::cout << "\n";
    std::cout << "=============================================================================\n";
    std::cout << "  STAGE 1: Curve Bootstrapping - Original vs XAD Tape\n";
    std::cout << "=============================================================================\n";
    std::cout << std::endl;

    std::cout << "  Setup: EONIA curve bootstrapping from market quotes\n";
    std::cout << "    - Inputs: 30 market quotes (3 depo + 4 short OIS + 5 dated OIS + 18 long OIS)\n";
    std::cout << "    - Output: Discount factors at various tenors\n";
    std::cout << "    - XAD computes dDF/dQuote via reverse-mode AD\n";
    std::cout << std::endl;

    // Setup
    Calendar calendar = TARGET();
    Date todaysDate(11, December, 2012);
    Settings::instance().evaluationDate() = todaysDate;
    Integer fixingDays = 2;
    Date settlementDate = calendar.adjust(calendar.advance(todaysDate, fixingDays, Days));
    DayCounter termStructureDayCounter = Actual365Fixed();

    // Market Quotes
    std::vector<Real> depos = {0.0004, 0.0004, 0.0004};
    std::vector<Real> shortOis = {0.00070, 0.00069, 0.00078, 0.00074};
    std::vector<Real> datedOIS = {0.000460, 0.000160, -0.000070, -0.000130, -0.000140};
    std::vector<Real> longTermOIS = {
        0.00002, 0.00008, 0.00021, 0.00036, 0.00127, 0.00274,
        0.00456, 0.00647, 0.00827, 0.00996, 0.01147, 0.0128,
        0.01404, 0.01516, 0.01764, 0.01939, 0.02003, 0.02038
    };

    // Store original values for comparison
    std::vector<double> depos_orig(depos.size()), shortOis_orig(shortOis.size());
    std::vector<double> datedOIS_orig(datedOIS.size()), longTermOIS_orig(longTermOIS.size());
    for (size_t k = 0; k < depos.size(); ++k) depos_orig[k] = value(depos[k]);
    for (size_t k = 0; k < shortOis.size(); ++k) shortOis_orig[k] = value(shortOis[k]);
    for (size_t k = 0; k < datedOIS.size(); ++k) datedOIS_orig[k] = value(datedOIS[k]);
    for (size_t k = 0; k < longTermOIS.size(); ++k) longTermOIS_orig[k] = value(longTermOIS[k]);

    // XAD Tape Setup
    using tape_type = Real::tape_type;
    tape_type tape;
    tape.registerInputs(depos);
    tape.registerInputs(shortOis);
    tape.registerInputs(datedOIS);
    tape.registerInputs(longTermOIS);
    tape.newRecording();

    // Build curve helpers
    auto eonia = ext::make_shared<Eonia>();
    std::vector<ext::shared_ptr<RateHelper>> eoniaInstruments;

    DayCounter depositDayCounter = Actual360();
    std::vector<Natural> depoSettlementDays = {0, 1, 2};
    for (size_t i = 0; i < depos.size(); ++i) {
        auto quote = ext::make_shared<SimpleQuote>(depos[i]);
        eoniaInstruments.push_back(ext::make_shared<DepositRateHelper>(
            Handle<Quote>(quote), 1 * Days, depoSettlementDays[i],
            calendar, Following, false, depositDayCounter));
    }

    std::vector<Period> shortOisTenors = {1 * Weeks, 2 * Weeks, 3 * Weeks, 1 * Months};
    for (size_t i = 0; i < shortOis.size(); ++i) {
        auto quote = ext::make_shared<SimpleQuote>(shortOis[i]);
        eoniaInstruments.push_back(ext::make_shared<OISRateHelper>(
            2, shortOisTenors[i], Handle<Quote>(quote), eonia));
    }

    std::vector<std::pair<Date, Date>> datedOisPeriods = {
        {Date(16, January, 2013), Date(13, February, 2013)},
        {Date(13, February, 2013), Date(13, March, 2013)},
        {Date(13, March, 2013), Date(10, April, 2013)},
        {Date(10, April, 2013), Date(8, May, 2013)},
        {Date(8, May, 2013), Date(12, June, 2013)}
    };
    for (size_t i = 0; i < datedOIS.size(); ++i) {
        auto quote = ext::make_shared<SimpleQuote>(datedOIS[i]);
        eoniaInstruments.push_back(ext::make_shared<OISRateHelper>(
            datedOisPeriods[i].first, datedOisPeriods[i].second,
            Handle<Quote>(quote), eonia));
    }

    std::vector<Period> longOisTenors = {
        15 * Months, 18 * Months, 21 * Months, 2 * Years, 3 * Years, 4 * Years,
        5 * Years, 6 * Years, 7 * Years, 8 * Years, 9 * Years, 10 * Years,
        11 * Years, 12 * Years, 15 * Years, 20 * Years, 25 * Years, 30 * Years
    };
    for (size_t i = 0; i < longTermOIS.size(); ++i) {
        auto quote = ext::make_shared<SimpleQuote>(longTermOIS[i]);
        eoniaInstruments.push_back(ext::make_shared<OISRateHelper>(
            2, longOisTenors[i], Handle<Quote>(quote), eonia));
    }

    // Bootstrap curve with XAD
    auto eoniaTermStructure = ext::make_shared<PiecewiseYieldCurve<Discount, Cubic>>(
        todaysDate, eoniaInstruments, termStructureDayCounter);
    eoniaTermStructure->enableExtrapolation();

    // Get XAD discount factors
    std::vector<Period> sampleTenors = {1*Years, 2*Years, 5*Years, 10*Years, 30*Years};
    std::vector<Real> df_xad(sampleTenors.size());
    for (size_t i = 0; i < sampleTenors.size(); ++i) {
        df_xad[i] = eoniaTermStructure->discount(todaysDate + sampleTenors[i]);
    }

    // Compute XAD derivatives for 5Y DF
    Real df5Y = df_xad[2];
    tape.registerOutput(df5Y);
    derivative(df5Y) = 1.0;
    tape.computeAdjoints();

    std::vector<double> derivs_xad = {
        derivative(depos[0]), derivative(shortOis[0]), derivative(longTermOIS[6])
    };

    tape.deactivate();

    // =========================================================================
    // Now compute "Original" (bump-and-reprice for derivatives)
    // =========================================================================
    auto buildCurve = [&](const std::vector<double>& d, const std::vector<double>& s,
                          const std::vector<double>& dt, const std::vector<double>& lt) {
        std::vector<ext::shared_ptr<RateHelper>> instruments;
        for (size_t i = 0; i < d.size(); ++i) {
            auto q = ext::make_shared<SimpleQuote>(d[i]);
            instruments.push_back(ext::make_shared<DepositRateHelper>(
                Handle<Quote>(q), 1 * Days, depoSettlementDays[i],
                calendar, Following, false, depositDayCounter));
        }
        for (size_t i = 0; i < s.size(); ++i) {
            auto q = ext::make_shared<SimpleQuote>(s[i]);
            instruments.push_back(ext::make_shared<OISRateHelper>(
                2, shortOisTenors[i], Handle<Quote>(q), eonia));
        }
        for (size_t i = 0; i < dt.size(); ++i) {
            auto q = ext::make_shared<SimpleQuote>(dt[i]);
            instruments.push_back(ext::make_shared<OISRateHelper>(
                datedOisPeriods[i].first, datedOisPeriods[i].second,
                Handle<Quote>(q), eonia));
        }
        for (size_t i = 0; i < lt.size(); ++i) {
            auto q = ext::make_shared<SimpleQuote>(lt[i]);
            instruments.push_back(ext::make_shared<OISRateHelper>(
                2, longOisTenors[i], Handle<Quote>(q), eonia));
        }
        auto curve = ext::make_shared<PiecewiseYieldCurve<Discount, Cubic>>(
            todaysDate, instruments, termStructureDayCounter);
        curve->enableExtrapolation();
        return curve;
    };

    // Original discount factors
    auto curve_orig = buildCurve(depos_orig, shortOis_orig, datedOIS_orig, longTermOIS_orig);
    std::vector<double> df_orig(sampleTenors.size());
    for (size_t i = 0; i < sampleTenors.size(); ++i) {
        df_orig[i] = value(curve_orig->discount(todaysDate + sampleTenors[i]));
    }

    // Bump-and-reprice for derivatives
    double bump = 1e-6;
    std::vector<double> derivs_bump(3);

    // Bump depos[0]
    auto depos_bump = depos_orig; depos_bump[0] += bump;
    auto curve_bump = buildCurve(depos_bump, shortOis_orig, datedOIS_orig, longTermOIS_orig);
    derivs_bump[0] = (value(curve_bump->discount(todaysDate + 5*Years)) - df_orig[2]) / bump;

    // Bump shortOis[0]
    auto shortOis_bump = shortOis_orig; shortOis_bump[0] += bump;
    curve_bump = buildCurve(depos_orig, shortOis_bump, datedOIS_orig, longTermOIS_orig);
    derivs_bump[1] = (value(curve_bump->discount(todaysDate + 5*Years)) - df_orig[2]) / bump;

    // Bump longTermOIS[6] (5Y OIS)
    auto longTermOIS_bump = longTermOIS_orig; longTermOIS_bump[6] += bump;
    curve_bump = buildCurve(depos_orig, shortOis_orig, datedOIS_orig, longTermOIS_bump);
    derivs_bump[2] = (value(curve_bump->discount(todaysDate + 5*Years)) - df_orig[2]) / bump;

    // =========================================================================
    // TABLE: Discount Factor Comparison
    // =========================================================================
    std::cout << "  " << std::string(60, '=') << "\n";
    std::cout << "  TABLE: DISCOUNT FACTORS\n";
    std::cout << "  " << std::string(60, '=') << "\n";
    std::cout << std::endl;

    std::cout << "  " << std::setw(10) << "Tenor" << " | "
              << std::setw(14) << "Original" << " | "
              << std::setw(14) << "XAD Tape" << " | "
              << std::setw(14) << "Difference" << "\n";
    std::cout << "  " << std::string(10, '-') << "-+-"
              << std::string(14, '-') << "-+-"
              << std::string(14, '-') << "-+-"
              << std::string(14, '-') << "\n";

    bool allMatch = true;
    for (size_t i = 0; i < sampleTenors.size(); ++i) {
        double diff = value(df_xad[i]) - df_orig[i];
        bool match = (std::abs(diff) < 1e-12);
        if (!match) allMatch = false;

        std::ostringstream tenor_str;
        tenor_str << sampleTenors[i];

        std::cout << "  " << std::setw(10) << tenor_str.str() << " | "
                  << std::fixed << std::setprecision(8) << std::setw(14) << df_orig[i] << " | "
                  << std::setw(14) << value(df_xad[i]) << " | "
                  << std::scientific << std::setprecision(2) << std::setw(14) << diff << "\n";
    }
    std::cout << std::endl;

    // =========================================================================
    // TABLE: Derivatives Comparison (5Y DF)
    // =========================================================================
    std::cout << "  " << std::string(70, '=') << "\n";
    std::cout << "  TABLE: DERIVATIVES OF DF(5Y) - Bump h=" << std::scientific << std::setprecision(0) << bump << "\n";
    std::cout << "  " << std::string(70, '=') << "\n";
    std::cout << std::endl;

    std::vector<std::string> derivLabels = {"dDF/dDepo[0]", "dDF/dShortOIS[0]", "dDF/dLongOIS[6]"};

    std::cout << "  " << std::setw(18) << "Sensitivity" << " | "
              << std::setw(14) << "Bump&Reprice" << " | "
              << std::setw(14) << "XAD Tape" << " | "
              << std::setw(12) << "Rel Diff %" << "\n";
    std::cout << "  " << std::string(18, '-') << "-+-"
              << std::string(14, '-') << "-+-"
              << std::string(14, '-') << "-+-"
              << std::string(12, '-') << "\n";

    bool derivsMatch = true;
    double maxRelDiff = 0.0;
    for (size_t i = 0; i < 3; ++i) {
        double diff = derivs_xad[i] - derivs_bump[i];
        // Relative difference as percentage (use bump as reference since it's ground truth for comparison)
        double relDiffPct = (std::abs(derivs_bump[i]) > 1e-15) ? (diff / derivs_bump[i] * 100.0) : 0.0;
        maxRelDiff = std::max(maxRelDiff, std::abs(relDiffPct));
        bool match = (std::abs(relDiffPct) < 0.01);  // 0.01% tolerance for bump vs AD
        if (!match) derivsMatch = false;

        std::cout << "  " << std::setw(18) << derivLabels[i] << " | "
                  << std::scientific << std::setprecision(6) << std::setw(14) << derivs_bump[i] << " | "
                  << std::setw(14) << derivs_xad[i] << " | "
                  << std::fixed << std::setprecision(6) << std::setw(11) << relDiffPct << "%\n";
    }
    std::cout << std::endl;

    std::cout << "  Values Match: " << (allMatch ? "YES" : "NO") << "\n";
    std::cout << "  Derivatives: max rel diff " << std::fixed << std::setprecision(4) << maxRelDiff << "%"
              << (derivsMatch ? " (OK)" : " (CHECK - some > 0.01%)") << "\n";
    std::cout << std::endl;

    std::cout << "  STATUS: [STAGE 1 COMPLETE]\n";
    std::cout << "=============================================================================\n";
    std::cout << std::endl;

    BOOST_CHECK(value(df_xad[2]) > 0.0 && value(df_xad[2]) < 1.0);
    BOOST_CHECK(allMatch);
}


//////////////////////////////////////////////////////////////////////////////
// Stage 2 Comparison: Side-by-side comparison of Original, XAD Tape, and JIT
// Runs all three approaches with SAME random numbers and produces comparison tables
//////////////////////////////////////////////////////////////////////////////

BOOST_AUTO_TEST_CASE(testStage2_Comparison)
{
    BOOST_TEST_MESSAGE("Testing Stage 2 Comparison: Original vs XAD Tape vs JIT...");

    std::cout << "\n";
    std::cout << "=============================================================================\n";
    std::cout << "  STAGE 2 COMPARISON: Original vs XAD Tape vs JIT\n";
    std::cout << "=============================================================================\n";
    std::cout << std::endl;

    std::cout << "  Setup: LMM Monte Carlo swaption pricing\n";
    std::cout << "    - LMM Process: 10 forward rates, exponential correlation (rho=0.5)\n";
    std::cout << "    - Swaption: 1Y option into 1Y swap (indices i=2, j=2)\n";
    std::cout << "    - MC: 10 paths\n";
    std::cout << std::endl;
    std::cout << "  Approaches:\n";
    std::cout << "    - Original (QuantLib): Uses MultiPathGenerator (standard QuantLib)\n";
    std::cout << "    - Original (Re-Route): Custom loop with pre-generated randoms\n";
    std::cout << "    - XAD (QuantLib):      MultiPathGenerator with XAD tape\n";
    std::cout << "    - XAD (Re-Route):      Custom loop with XAD tape\n";
    std::cout << "    - JIT:                 Custom loop with JIT (requires pre-generated randoms)\n";
    std::cout << std::endl;

    // Common Setup
    const Size size = 10;
    const Size steps = 8 * size;
    const Size numZeroRates = 10;  // Number of zero curve input rates

    // Create zero curve with 10 points (covering 5.5 years for LMM)
    // Dates: 0, 6M, 1Y, 1.5Y, 2Y, 2.5Y, 3Y, 3.5Y, 4Y, 5.5Y (extended to cover LMM times)
    Date baseDate(4, September, 2005);
    std::vector<Date> zeroDates = {
        baseDate,
        baseDate + 6 * Months,
        baseDate + 1 * Years,
        baseDate + 18 * Months,
        baseDate + 2 * Years,
        baseDate + 30 * Months,
        baseDate + 3 * Years,
        baseDate + 42 * Months,
        baseDate + 4 * Years,
        baseDate + 66 * Months  // 5.5 years to cover LMM times (max ~5.07 years)
    };

    // Zero rates (upward sloping curve from 4% to 6%)
    std::vector<double> zeroRates_val = {
        0.040, 0.042, 0.044, 0.046, 0.048,
        0.050, 0.052, 0.054, 0.056, 0.060
    };

    std::vector<Rate> rates_ql(numZeroRates);
    for (Size k = 0; k < numZeroRates; ++k) rates_ql[k] = zeroRates_val[k];

    ext::shared_ptr<IborIndex> index = makeIndex(zeroDates, rates_ql);
    ext::shared_ptr<LiborForwardModelProcess> process(
        new LiborForwardModelProcess(size, index));

    ext::shared_ptr<LmCorrelationModel> corrModel(
        new LmExponentialCorrelationModel(size, 0.5));
    ext::shared_ptr<LmVolatilityModel> volaModel(
        new LmLinearExponentialVolatilityModel(process->fixingTimes(),
                                               0.291, 1.483, 0.116, 0.00001));
    process->setCovarParam(ext::shared_ptr<LfmCovarianceParameterization>(
        new LfmCovarianceProxy(volaModel, corrModel)));

    std::vector<Time> fixingTimes = process->fixingTimes();
    TimeGrid grid(fixingTimes.begin(), fixingTimes.end(), steps);

    std::vector<Size> location;
    for (Size idx = 0; idx < fixingTimes.size(); ++idx) {
        location.push_back(
            std::find(grid.begin(), grid.end(), fixingTimes[idx]) - grid.begin());
    }

    Size i = 2, j = 2;
    Calendar calendar = index->fixingCalendar();
    DayCounter dayCounter = index->forwardingTermStructure()->dayCounter();
    BusinessDayConvention convention = index->businessDayConvention();
    Date settlement = index->forwardingTermStructure()->referenceDate();

    Date fwdStart = settlement + Period(6 * i, Months);
    Date fwdMaturity = fwdStart + Period(6 * j, Months);
    Schedule schedule(fwdStart, fwdMaturity, index->tenor(), calendar,
                      convention, convention, DateGeneration::Forward, false);

    Rate swapRate_ql = 0.0404;
    ext::shared_ptr<VanillaSwap> forwardSwap(
        new VanillaSwap(Swap::Receiver, 1.0, schedule, swapRate_ql, dayCounter,
                        schedule, index, 0.0, index->dayCounter()));
    forwardSwap->setPricingEngine(ext::make_shared<DiscountingSwapEngine>(
        index->forwardingTermStructure()));

    double swapRate_double = value(forwardSwap->fairRate());

    Size numFactors = process->factors();
    Size exerciseStep = location[i];
    Size totalRandoms = numFactors * exerciseStep;
    const Size nrTrails = 10;
    Array initRates = process->initialValues();

    // =========================================================================
    // APPROACH 1: Original (QuantLib) - Uses MultiPathGenerator
    // =========================================================================
    typedef PseudoRandom::rsg_type rsg_type;
    rsg_type rsg_ql = PseudoRandom::make_sequence_generator(
        numFactors * (grid.size() - 1), BigNatural(42));

    typedef MultiPathGenerator<rsg_type>::sample_type sample_type;
    MultiPathGenerator<rsg_type> generator_ql(process, grid, rsg_ql, false);

    double mcPrice_orig_ql = 0.0;
    for (Size n = 0; n < nrTrails; ++n) {
        sample_type path = (n % 2) != 0U ? generator_ql.antithetic() : generator_ql.next();

        std::vector<double> mcRates(size);
        for (Size k = 0; k < size; ++k) {
            mcRates[k] = value(path.value[k][location[i]]);
        }

        std::vector<double> dis(size);
        double df = 1.0;
        for (Size k = 0; k < size; ++k) {
            double accrual = value(process->accrualEndTimes()[k]) - value(process->accrualStartTimes()[k]);
            df = df / (1.0 + mcRates[k] * accrual);
            dis[k] = df;
        }

        double npv = 0.0;
        for (Size m = i; m < i + j; ++m) {
            double accrual = value(process->accrualEndTimes()[m]) - value(process->accrualStartTimes()[m]);
            npv += (swapRate_double - mcRates[m]) * accrual * dis[m];
        }
        if (npv > 0.0) mcPrice_orig_ql += npv;
    }
    mcPrice_orig_ql /= static_cast<double>(nrTrails);

    // =========================================================================
    // Pre-generate ALL random numbers for Re-Route approaches
    // Use same dimension as QuantLib MultiPathGenerator to ensure identical RNG sequence
    // =========================================================================
    Size fullGridRandoms = numFactors * (grid.size() - 1);  // Same as QuantLib
    rsg_type rsg = PseudoRandom::make_sequence_generator(fullGridRandoms, BigNatural(42));

    std::vector<std::vector<double>> allRandoms(nrTrails);
    for (Size n = 0; n < nrTrails; ++n) {
        const auto& sequence = (n % 2) != 0U ? rsg.lastSequence() : rsg.nextSequence();
        // Only store up to exerciseStep (we don't need the rest but generate them to keep RNG in sync)
        allRandoms[n].resize(totalRandoms);
        for (Size m = 0; m < totalRandoms; ++m) {
            double rnd = (n % 2) != 0U ? -value(sequence.value[m]) : value(sequence.value[m]);
            allRandoms[n][m] = rnd;
        }
    }

    // =========================================================================
    // APPROACH 2: Original (Re-Route) - Custom loop with pre-generated randoms
    // =========================================================================
    double mcPrice_orig = 0.0;
    for (Size n = 0; n < nrTrails; ++n) {
        std::vector<double> asset(size);
        for (Size k = 0; k < size; ++k) asset[k] = value(initRates[k]);

        for (Size step = 1; step <= exerciseStep; ++step) {
            Size offset = (step - 1) * numFactors;
            Array dw(numFactors);
            for (Size f = 0; f < numFactors; ++f) dw[f] = allRandoms[n][offset + f];

            Array asset_arr(size);
            for (Size k = 0; k < size; ++k) asset_arr[k] = asset[k];
            Array evolved = process->evolve(grid[step - 1], asset_arr, grid.dt(step - 1), dw);
            for (Size k = 0; k < size; ++k) asset[k] = value(evolved[k]);
        }

        std::vector<double> dis(size);
        double df = 1.0;
        for (Size k = 0; k < size; ++k) {
            double accrual = value(process->accrualEndTimes()[k]) - value(process->accrualStartTimes()[k]);
            df = df / (1.0 + asset[k] * accrual);
            dis[k] = df;
        }

        double npv = 0.0;
        for (Size m = i; m < i + j; ++m) {
            double accrual = value(process->accrualEndTimes()[m]) - value(process->accrualStartTimes()[m]);
            npv += (swapRate_double - asset[m]) * accrual * dis[m];
        }
        if (npv > 0.0) mcPrice_orig += npv;
    }
    mcPrice_orig /= static_cast<double>(nrTrails);

    // Helper lambda for bump-and-reprice
    auto computeMCPrice = [&](const std::vector<double>& bumpedInitRates) {
        double price = 0.0;
        for (Size n = 0; n < nrTrails; ++n) {
            std::vector<double> asset_b(size);
            for (Size k = 0; k < size; ++k) asset_b[k] = bumpedInitRates[k];

            for (Size step = 1; step <= exerciseStep; ++step) {
                Size offset = (step - 1) * numFactors;
                Array dw(numFactors);
                for (Size f = 0; f < numFactors; ++f) dw[f] = allRandoms[n][offset + f];

                Array asset_arr(size);
                for (Size k = 0; k < size; ++k) asset_arr[k] = asset_b[k];
                Array evolved = process->evolve(grid[step - 1], asset_arr, grid.dt(step - 1), dw);
                for (Size k = 0; k < size; ++k) asset_b[k] = value(evolved[k]);
            }

            std::vector<double> dis_b(size);
            double df_b = 1.0;
            for (Size k = 0; k < size; ++k) {
                double accrual = value(process->accrualEndTimes()[k]) - value(process->accrualStartTimes()[k]);
                df_b = df_b / (1.0 + asset_b[k] * accrual);
                dis_b[k] = df_b;
            }

            double npv_b = 0.0;
            for (Size m = i; m < i + j; ++m) {
                double accrual = value(process->accrualEndTimes()[m]) - value(process->accrualStartTimes()[m]);
                npv_b += (swapRate_double - asset_b[m]) * accrual * dis_b[m];
            }
            if (npv_b > 0.0) price += npv_b;
        }
        return price / static_cast<double>(nrTrails);
    };

    // Bump-and-reprice derivatives
    double bump = 1e-6;
    std::vector<double> baseInitRates(size);
    for (Size k = 0; k < size; ++k) baseInitRates[k] = value(initRates[k]);

    std::vector<double> dPrice_bump(size);
    for (Size k = 0; k < size; ++k) {
        std::vector<double> bumpedRates = baseInitRates;
        bumpedRates[k] += bump;
        double bumpedPrice = computeMCPrice(bumpedRates);
        dPrice_bump[k] = (bumpedPrice - mcPrice_orig) / bump;
    }

    // =========================================================================
    // APPROACH 3: XAD (QuantLib) - Uses MultiPathGenerator with tape
    // Registers zero curve rates as inputs, derivatives flow through:
    //   zero rates → forward rates (via term structure) → MC paths → price
    // =========================================================================
    using tape_type = Real::tape_type;
    tape_type tape_ql;

    // Register the 10 zero curve rates as tape inputs
    std::vector<Real> rates_xad_ql(numZeroRates);
    for (Size k = 0; k < numZeroRates; ++k) rates_xad_ql[k] = zeroRates_val[k];
    tape_ql.registerInputs(rates_xad_ql);
    tape_ql.newRecording();

    // Build curve and process using tape-registered rates
    ext::shared_ptr<IborIndex> index_xad_ql = makeIndex(zeroDates, rates_xad_ql);
    ext::shared_ptr<LiborForwardModelProcess> process_xad_ql(
        new LiborForwardModelProcess(size, index_xad_ql));

    ext::shared_ptr<LmCorrelationModel> corrModel_xad_ql(
        new LmExponentialCorrelationModel(size, 0.5));
    ext::shared_ptr<LmVolatilityModel> volaModel_xad_ql(
        new LmLinearExponentialVolatilityModel(process_xad_ql->fixingTimes(),
                                               0.291, 1.483, 0.116, 0.00001));
    process_xad_ql->setCovarParam(ext::shared_ptr<LfmCovarianceParameterization>(
        new LfmCovarianceProxy(volaModel_xad_ql, corrModel_xad_ql)));

    // Get location indices for this process
    std::vector<Time> fixingTimes_xad_ql = process_xad_ql->fixingTimes();
    TimeGrid grid_xad_ql(fixingTimes_xad_ql.begin(), fixingTimes_xad_ql.end(), steps);
    std::vector<Size> location_xad_ql;
    for (Size idx = 0; idx < fixingTimes_xad_ql.size(); ++idx) {
        location_xad_ql.push_back(
            std::find(grid_xad_ql.begin(), grid_xad_ql.end(), fixingTimes_xad_ql[idx]) - grid_xad_ql.begin());
    }

    // Reset RNG to same seed as Original QuantLib
    rsg_type rsg_xad_ql = PseudoRandom::make_sequence_generator(
        process_xad_ql->factors() * (grid_xad_ql.size() - 1), BigNatural(42));

    MultiPathGenerator<rsg_type> generator_xad_ql(process_xad_ql, grid_xad_ql, rsg_xad_ql, false);

    // Get swap rate for this process
    Date fwdStart_xad = index_xad_ql->forwardingTermStructure()->referenceDate() + Period(6 * i, Months);
    Date fwdMaturity_xad = fwdStart_xad + Period(6 * j, Months);
    Schedule schedule_xad(fwdStart_xad, fwdMaturity_xad, index_xad_ql->tenor(), calendar,
                          convention, convention, DateGeneration::Forward, false);
    ext::shared_ptr<VanillaSwap> forwardSwap_xad(
        new VanillaSwap(Swap::Receiver, 1.0, schedule_xad, 0.0404, dayCounter,
                        schedule_xad, index_xad_ql, 0.0, index_xad_ql->dayCounter()));
    forwardSwap_xad->setPricingEngine(ext::make_shared<DiscountingSwapEngine>(
        index_xad_ql->forwardingTermStructure()));
    Real swapRate_xad_ql = forwardSwap_xad->fairRate();

    Real mcPrice_xad_ql = 0.0;
    for (Size n = 0; n < nrTrails; ++n) {
        sample_type path = (n % 2) != 0U ? generator_xad_ql.antithetic() : generator_xad_ql.next();

        std::vector<Real> mcRates(size);
        for (Size k = 0; k < size; ++k) {
            mcRates[k] = path.value[k][location_xad_ql[i]];
        }

        std::vector<Real> dis(size);
        Real df = 1.0;
        for (Size k = 0; k < size; ++k) {
            Real accrual = process_xad_ql->accrualEndTimes()[k] - process_xad_ql->accrualStartTimes()[k];
            df = df / (Real(1.0) + mcRates[k] * accrual);
            dis[k] = df;
        }

        Real npv = 0.0;
        for (Size m = i; m < i + j; ++m) {
            Real accrual = process_xad_ql->accrualEndTimes()[m] - process_xad_ql->accrualStartTimes()[m];
            npv = npv + (swapRate_xad_ql - mcRates[m]) * accrual * dis[m];
        }
        if (value(npv) > 0.0) mcPrice_xad_ql += npv;
    }
    mcPrice_xad_ql /= static_cast<Real>(nrTrails);

    tape_ql.registerOutput(mcPrice_xad_ql);
    derivative(mcPrice_xad_ql) = 1.0;
    tape_ql.computeAdjoints();

    // Get derivatives w.r.t. the 10 zero curve rates
    std::vector<double> dPrice_xad_ql(numZeroRates);
    for (Size k = 0; k < numZeroRates; ++k) dPrice_xad_ql[k] = derivative(rates_xad_ql[k]);
    tape_ql.deactivate();

    // =========================================================================
    // APPROACH 4: XAD (Re-Route) - Custom loop with XAD tape
    // Same inputs as XAD QuantLib (zero curve rates) for fair comparison
    // =========================================================================
    tape_type tape_rr;

    // Register the same 10 zero curve rates as inputs (like XAD QuantLib)
    std::vector<Real> rates_xad_rr(numZeroRates);
    for (Size k = 0; k < numZeroRates; ++k) rates_xad_rr[k] = zeroRates_val[k];
    tape_rr.registerInputs(rates_xad_rr);
    tape_rr.newRecording();

    // Build curve and process using tape-registered rates (same as XAD QuantLib)
    ext::shared_ptr<IborIndex> index_xad_rr = makeIndex(zeroDates, rates_xad_rr);
    ext::shared_ptr<LiborForwardModelProcess> process_xad_rr(
        new LiborForwardModelProcess(size, index_xad_rr));

    ext::shared_ptr<LmCorrelationModel> corrModel_xad_rr(
        new LmExponentialCorrelationModel(size, 0.5));
    ext::shared_ptr<LmVolatilityModel> volaModel_xad_rr(
        new LmLinearExponentialVolatilityModel(process_xad_rr->fixingTimes(),
                                               0.291, 1.483, 0.116, 0.00001));
    process_xad_rr->setCovarParam(ext::shared_ptr<LfmCovarianceParameterization>(
        new LfmCovarianceProxy(volaModel_xad_rr, corrModel_xad_rr)));

    // Get location indices and grid for this process
    std::vector<Time> fixingTimes_xad_rr = process_xad_rr->fixingTimes();
    TimeGrid grid_xad_rr(fixingTimes_xad_rr.begin(), fixingTimes_xad_rr.end(), steps);
    std::vector<Size> location_xad_rr;
    for (Size idx = 0; idx < fixingTimes_xad_rr.size(); ++idx) {
        location_xad_rr.push_back(
            std::find(grid_xad_rr.begin(), grid_xad_rr.end(), fixingTimes_xad_rr[idx]) - grid_xad_rr.begin());
    }
    Size exerciseStep_rr = location_xad_rr[i];
    Size numFactors_rr = process_xad_rr->factors();

    // Get initial forward rates from process (these now depend on tape inputs)
    Array initRates_rr = process_xad_rr->initialValues();

    // Get swap rate for this process
    Date fwdStart_rr = index_xad_rr->forwardingTermStructure()->referenceDate() + Period(6 * i, Months);
    Date fwdMaturity_rr = fwdStart_rr + Period(6 * j, Months);
    Schedule schedule_rr(fwdStart_rr, fwdMaturity_rr, index_xad_rr->tenor(), calendar,
                         convention, convention, DateGeneration::Forward, false);
    ext::shared_ptr<VanillaSwap> forwardSwap_rr(
        new VanillaSwap(Swap::Receiver, 1.0, schedule_rr, 0.0404, dayCounter,
                        schedule_rr, index_xad_rr, 0.0, index_xad_rr->dayCounter()));
    forwardSwap_rr->setPricingEngine(ext::make_shared<DiscountingSwapEngine>(
        index_xad_rr->forwardingTermStructure()));
    Real swapRate_rr = forwardSwap_rr->fairRate();

    // Re-Route MC loop with pre-generated randoms
    Real mcPrice_tape = 0.0;
    for (Size n = 0; n < nrTrails; ++n) {
        // Start from initial forward rates (which depend on zero curve rates via tape)
        std::vector<Real> asset_rr(size);
        for (Size k = 0; k < size; ++k) asset_rr[k] = initRates_rr[k];

        for (Size step = 1; step <= exerciseStep_rr; ++step) {
            Size offset = (step - 1) * numFactors_rr;
            Array dw(numFactors_rr);
            for (Size f = 0; f < numFactors_rr; ++f) dw[f] = allRandoms[n][offset + f];

            Array asset_arr(size);
            for (Size k = 0; k < size; ++k) asset_arr[k] = asset_rr[k];
            Array evolved = process_xad_rr->evolve(grid_xad_rr[step - 1], asset_arr, grid_xad_rr.dt(step - 1), dw);
            for (Size k = 0; k < size; ++k) asset_rr[k] = evolved[k];
        }

        std::vector<Real> dis_rr(size);
        Real df_rr = 1.0;
        for (Size k = 0; k < size; ++k) {
            Real accrual = process_xad_rr->accrualEndTimes()[k] - process_xad_rr->accrualStartTimes()[k];
            df_rr = df_rr / (Real(1.0) + asset_rr[k] * accrual);
            dis_rr[k] = df_rr;
        }

        Real npv_rr = 0.0;
        for (Size m = i; m < i + j; ++m) {
            Real accrual = process_xad_rr->accrualEndTimes()[m] - process_xad_rr->accrualStartTimes()[m];
            npv_rr = npv_rr + (swapRate_rr - asset_rr[m]) * accrual * dis_rr[m];
        }
        if (value(npv_rr) > 0.0) mcPrice_tape += npv_rr;
    }
    mcPrice_tape /= static_cast<Real>(nrTrails);

    tape_rr.registerOutput(mcPrice_tape);
    derivative(mcPrice_tape) = 1.0;
    tape_rr.computeAdjoints();

    // Get derivatives w.r.t. the 10 zero curve rates (same as XAD QuantLib)
    std::vector<double> dPrice_xad_rr(numZeroRates);
    for (Size k = 0; k < numZeroRates; ++k) dPrice_xad_rr[k] = derivative(rates_xad_rr[k]);
    tape_rr.deactivate();

    // =========================================================================
    // APPROACH 5: JIT - Custom loop with JIT
    // JIT also uses zero curve rates as inputs for fair comparison
    // =========================================================================
    auto forgeBackend3 = std::make_unique<xad::forge::ForgeBackend<double>>();
    xad::JITCompiler<double> jit(std::move(forgeBackend3));

    // Register 10 zero curve rates as JIT inputs
    std::vector<xad::AD> jit_zeroRates(numZeroRates);
    std::vector<xad::AD> jit_randoms(totalRandoms);

    for (Size k = 0; k < numZeroRates; ++k) {
        jit_zeroRates[k] = xad::AD(zeroRates_val[k]);
        jit.registerInput(jit_zeroRates[k]);
    }
    for (Size m = 0; m < totalRandoms; ++m) {
        jit_randoms[m] = xad::AD(0.0);
        jit.registerInput(jit_randoms[m]);
    }
    jit.newRecording();

    // Build curve and process using JIT-registered rates
    // Note: We need to extract forward rates from the curve for the JIT kernel
    std::vector<xad::AD> jit_zeroRates_vec(numZeroRates);
    for (Size k = 0; k < numZeroRates; ++k) jit_zeroRates_vec[k] = jit_zeroRates[k];

    // For JIT, we build the curve outside JIT and extract forward rates
    // The forward rates become functions of the zero rates through interpolation
    // This is a simplified approach - in practice you'd JIT the curve lookup too
    ext::shared_ptr<IborIndex> index_jit = makeIndex(zeroDates, jit_zeroRates_vec);
    ext::shared_ptr<LiborForwardModelProcess> process_jit(
        new LiborForwardModelProcess(size, index_jit));
    process_jit->setCovarParam(ext::shared_ptr<LfmCovarianceParameterization>(
        new LfmCovarianceProxy(
            ext::make_shared<LmLinearExponentialVolatilityModel>(process_jit->fixingTimes(), 0.291, 1.483, 0.116, 0.00001),
            ext::make_shared<LmExponentialCorrelationModel>(size, 0.5))));

    // Get initial forward rates (these depend on JIT zero rate inputs)
    Array initRates_jit = process_jit->initialValues();
    std::vector<xad::AD> jit_initRates(size);
    for (Size k = 0; k < size; ++k) jit_initRates[k] = initRates_jit[k];

    // Get grid info
    std::vector<Time> fixingTimes_jit = process_jit->fixingTimes();
    TimeGrid grid_jit(fixingTimes_jit.begin(), fixingTimes_jit.end(), steps);
    std::vector<Size> location_jit;
    for (Size idx = 0; idx < fixingTimes_jit.size(); ++idx) {
        location_jit.push_back(
            std::find(grid_jit.begin(), grid_jit.end(), fixingTimes_jit[idx]) - grid_jit.begin());
    }
    Size exerciseStep_jit = location_jit[i];
    Size numFactors_jit = process_jit->factors();

    // Get swap rate
    Date fwdStart_jit = index_jit->forwardingTermStructure()->referenceDate() + Period(6 * i, Months);
    Date fwdMaturity_jit = fwdStart_jit + Period(6 * j, Months);
    Schedule schedule_jit(fwdStart_jit, fwdMaturity_jit, index_jit->tenor(), calendar,
                          convention, convention, DateGeneration::Forward, false);
    ext::shared_ptr<VanillaSwap> forwardSwap_jit(
        new VanillaSwap(Swap::Receiver, 1.0, schedule_jit, 0.0404, dayCounter,
                        schedule_jit, index_jit, 0.0, index_jit->dayCounter()));
    forwardSwap_jit->setPricingEngine(ext::make_shared<DiscountingSwapEngine>(
        index_jit->forwardingTermStructure()));
    xad::AD swapRate_jit = forwardSwap_jit->fairRate();

    // Record path evolution + payoff
    std::vector<xad::AD> asset(size);
    for (Size k = 0; k < size; ++k) asset[k] = jit_initRates[k];

    for (Size step = 1; step <= exerciseStep_jit; ++step) {
        Size offset = (step - 1) * numFactors_jit;
        Array dw(numFactors_jit);
        for (Size f = 0; f < numFactors_jit; ++f) dw[f] = jit_randoms[offset + f];

        Array asset_arr(size);
        for (Size k = 0; k < size; ++k) asset_arr[k] = asset[k];
        Array evolved = process_jit->evolve(grid_jit[step - 1], asset_arr, grid_jit.dt(step - 1), dw);
        for (Size k = 0; k < size; ++k) asset[k] = evolved[k];
    }

    // Compute payoff inside JIT: discount factors + NPV + max(npv,0)
    std::vector<xad::AD> dis(size);
    xad::AD df_jit_val = xad::AD(1.0);
    for (Size k = 0; k < size; ++k) {
        xad::AD accrual = process_jit->accrualEndTimes()[k] - process_jit->accrualStartTimes()[k];
        df_jit_val = df_jit_val / (xad::AD(1.0) + asset[k] * accrual);
        dis[k] = df_jit_val;
    }

    xad::AD jit_npv = xad::AD(0.0);
    for (Size m = i; m < i + j; ++m) {
        xad::AD accrual = process_jit->accrualEndTimes()[m] - process_jit->accrualStartTimes()[m];
        jit_npv = jit_npv + (swapRate_jit - asset[m]) * accrual * dis[m];
    }
    xad::AD jit_payoff = xad::less(jit_npv, xad::AD(0.0)).If(xad::AD(0.0), jit_npv);
    jit.registerOutput(jit_payoff);

    // Compile the JIT graph
    jit.compile();

    // Execute JIT for all paths
    double mcPrice_jit = 0.0;
    for (Size n = 0; n < nrTrails; ++n) {
        // Set zero rates for this path (they're constant, but we need to set them)
        for (Size k = 0; k < numZeroRates; ++k) value(jit_zeroRates[k]) = zeroRates_val[k];
        for (Size m = 0; m < totalRandoms; ++m) value(jit_randoms[m]) = allRandoms[n][m];

        double payoff_value;
        jit.forward(&payoff_value);
        mcPrice_jit += payoff_value;
    }
    mcPrice_jit /= static_cast<double>(nrTrails);

    // =========================================================================
    // TABLE: MC PRICES - All 5 approaches
    // =========================================================================
    std::cout << "  " << std::string(75, '=') << "\n";
    std::cout << "  TABLE: MC PRICES\n";
    std::cout << "  " << std::string(75, '=') << "\n\n";

    std::cout << "  " << std::setw(20) << "Approach" << " | "
              << std::setw(12) << "MC Price" << " | "
              << std::setw(14) << "Diff vs Ref" << " | "
              << "Match\n";
    std::cout << "  " << std::string(20, '-') << "-+-"
              << std::string(12, '-') << "-+-"
              << std::string(14, '-') << "-+-"
              << std::string(5, '-') << "\n";

    // Reference: Original (QuantLib) using MultiPathGenerator
    std::cout << "  " << std::setw(20) << "Orig (QuantLib)" << " | "
              << std::fixed << std::setprecision(6) << std::setw(12) << mcPrice_orig_ql << " | "
              << std::setw(14) << "-" << " |   -  \n";

    // Original (Re-Route) - custom loop
    double diff_orig_rr = mcPrice_orig - mcPrice_orig_ql;
    bool orig_rr_match = (std::abs(diff_orig_rr) < 1e-6);  // may differ slightly due to different RNG sequences
    std::cout << "  " << std::setw(20) << "Orig (Re-Route)" << " | "
              << std::fixed << std::setprecision(6) << std::setw(12) << mcPrice_orig << " | "
              << std::scientific << std::setprecision(2) << std::setw(14) << diff_orig_rr << " | "
              << (orig_rr_match ? "  OK " : "DIFF!") << "\n";

    // XAD (QuantLib) - MultiPathGenerator using XAD types (should match Orig QuantLib)
    double diff_xad_ql = value(mcPrice_xad_ql) - mcPrice_orig_ql;
    bool xad_ql_match = (std::abs(diff_xad_ql) < 1e-10);
    std::cout << "  " << std::setw(20) << "XAD (QuantLib)" << " | "
              << std::fixed << std::setprecision(6) << std::setw(12) << value(mcPrice_xad_ql) << " | "
              << std::scientific << std::setprecision(2) << std::setw(14) << diff_xad_ql << " | "
              << (xad_ql_match ? "  OK " : "DIFF!") << "\n";

    // XAD (Re-Route) - custom loop with tape (compare to Re-Route baseline)
    double diff_tape_price = value(mcPrice_tape) - mcPrice_orig;
    bool tape_match = (std::abs(diff_tape_price) < 1e-10);
    std::cout << "  " << std::setw(20) << "XAD (Re-Route)" << " | "
              << std::fixed << std::setprecision(6) << std::setw(12) << value(mcPrice_tape) << " | "
              << std::scientific << std::setprecision(2) << std::setw(14) << diff_tape_price << " | "
              << (tape_match ? "  OK " : "DIFF!") << "\n";

    // JIT (compare to Re-Route baseline since JIT uses same loop structure)
    double diff_jit_price = mcPrice_jit - mcPrice_orig;
    bool jit_match = (std::abs(diff_jit_price) < 1e-10);
    std::cout << "  " << std::setw(20) << "JIT" << " | "
              << std::fixed << std::setprecision(6) << std::setw(12) << mcPrice_jit << " | "
              << std::scientific << std::setprecision(2) << std::setw(14) << diff_jit_price << " | "
              << (jit_match ? "  OK " : "DIFF!") << "\n";

    std::cout << std::endl;
    std::cout << "  Note: Re-Route approaches use pre-generated randoms (different sequence than QuantLib)\n";
    std::cout << "        XAD/JIT Re-Route should match Orig Re-Route exactly\n";
    std::cout << "        XAD QuantLib should match Orig QuantLib exactly (same RNG, same loop)\n";
    std::cout << std::endl;
    bool pricesMatch = tape_match && jit_match && xad_ql_match;

    // =========================================================================
    // TABLE: DERIVATIVES (w.r.t. 10 zero curve rates) - All AD approaches
    // =========================================================================
    std::cout << "  " << std::string(100, '=') << "\n";
    std::cout << "  TABLE: DERIVATIVES (w.r.t. " << numZeroRates << " zero curve rates)\n";
    std::cout << "  " << std::string(100, '=') << "\n\n";

    // Compute JIT derivatives w.r.t. zero curve rates
    std::vector<double> dPrice_jit(numZeroRates, 0.0);
    const auto& graph = jit.getGraph();
    uint32_t outputSlot = graph.output_ids[0];

    for (Size n = 0; n < nrTrails; ++n) {
        for (Size k = 0; k < numZeroRates; ++k) value(jit_zeroRates[k]) = zeroRates_val[k];
        for (Size m = 0; m < totalRandoms; ++m) value(jit_randoms[m]) = allRandoms[n][m];

        jit.clearDerivatives();
        jit.setDerivative(outputSlot, 1.0);
        jit.computeAdjoints();

        for (Size k = 0; k < numZeroRates; ++k) dPrice_jit[k] += jit.derivative(graph.input_ids[k]);
    }

    for (Size k = 0; k < numZeroRates; ++k) dPrice_jit[k] /= static_cast<double>(nrTrails);

    std::cout << "  " << std::setw(10) << "ZeroRate#" << " | "
              << std::setw(14) << "XAD QuantLib" << " | "
              << std::setw(14) << "XAD Re-Route" << " | "
              << std::setw(14) << "JIT" << " | "
              << std::setw(12) << "QL/RR diff" << " | "
              << std::setw(12) << "RR/JIT diff" << "\n";
    std::cout << "  " << std::string(10, '-') << "-+-"
              << std::string(14, '-') << "-+-"
              << std::string(14, '-') << "-+-"
              << std::string(14, '-') << "-+-"
              << std::string(12, '-') << "-+-"
              << std::string(12, '-') << "\n";

    Size matchingDerivs_ql_rr = 0;
    Size matchingDerivs_rr_jit = 0;
    for (Size k = 0; k < numZeroRates; ++k) {
        double diff_ql_rr = dPrice_xad_ql[k] - dPrice_xad_rr[k];
        double diff_rr_jit = dPrice_xad_rr[k] - dPrice_jit[k];

        bool match_ql_rr = (std::abs(diff_ql_rr) < 1e-10);
        bool match_rr_jit = (std::abs(diff_rr_jit) < 1e-10);
        if (match_ql_rr) matchingDerivs_ql_rr++;
        if (match_rr_jit) matchingDerivs_rr_jit++;

        std::cout << "  " << std::setw(10) << k << " | "
                  << std::scientific << std::setprecision(4) << std::setw(14) << dPrice_xad_ql[k] << " | "
                  << std::setw(14) << dPrice_xad_rr[k] << " | "
                  << std::setw(14) << dPrice_jit[k] << " | "
                  << std::setw(12) << diff_ql_rr << " | "
                  << std::setw(12) << diff_rr_jit << "\n";
    }
    std::cout << std::endl;

    bool derivsMatch = (matchingDerivs_ql_rr == numZeroRates) && (matchingDerivs_rr_jit == numZeroRates);
    std::cout << "  XAD QuantLib vs XAD Re-Route: " << matchingDerivs_ql_rr << "/" << numZeroRates << " exact match\n";
    std::cout << "  XAD Re-Route vs JIT: " << matchingDerivs_rr_jit << "/" << numZeroRates << " exact match\n";
    std::cout << std::endl;

    // Summary
    bool allPass = pricesMatch && derivsMatch;
    std::cout << "  RESULT: " << (allPass ? "[PASS]" : "[CHECK]") << "\n";
    std::cout << "    - Prices: " << (pricesMatch ? "OK" : "DIFF") << "\n";
    std::cout << "    - All derivatives match (QL/RR/JIT): " << (derivsMatch ? "OK" : "DIFF") << "\n";
    std::cout << "\n";
    std::cout << "  STATUS: [STAGE 2 COMPARISON COMPLETE]\n";
    std::cout << "=============================================================================\n";
    std::cout << std::endl;

    BOOST_CHECK(pricesMatch);
    BOOST_CHECK(derivsMatch);
}

//////////////////////////////////////////////////////////////////////////////
// Stage 3: Combined Pipeline Comparison
// Compares: Original (QuantLib/Re-Route) vs XAD (QuantLib/Re-Route) vs JIT
// Full end-to-end: Market Quotes -> Bootstrap -> MC Price -> dPrice/dMarketQuotes
//////////////////////////////////////////////////////////////////////////////

BOOST_AUTO_TEST_CASE(testStage3_CombinedPipeline)
{
    BOOST_TEST_MESSAGE("Testing Stage 3: Combined Bootstrap + MC Pricing Comparison...");

    std::cout << "\n";
    std::cout << "=============================================================================\n";
    std::cout << "  STAGE 3: Combined Pipeline (Bootstrap + MC + Derivatives)\n";
    std::cout << "=============================================================================\n";
    std::cout << std::endl;

    std::cout << "  DATA FLOW:\n";
    std::cout << "    Market Quotes -> Bootstrap -> ZeroRates -> LMM MC -> Price\n";
    std::cout << "         |                                              |\n";
    std::cout << "         +<-------- reverse sweep (XAD Tape or JIT) <---+\n";
    std::cout << std::endl;
    std::cout << "  Approaches:\n";
    std::cout << "    - Original (QuantLib):  MultiPathGenerator (standard QuantLib)\n";
    std::cout << "    - Original (Re-Route):  Custom loop with pre-generated randoms\n";
    std::cout << "    - XAD (QuantLib):       MultiPathGenerator with XAD tape\n";
    std::cout << "    - XAD (Re-Route):       Custom loop with XAD tape\n";
    std::cout << "    - JIT:                  Custom loop with JIT (tape for bootstrap)\n";
    std::cout << std::endl;

    // =========================================================================
    // COMMON PARAMETERS
    // =========================================================================
    Calendar calendar = TARGET();
    Date todaysDate = Date(4, September, 2005);
    Settings::instance().evaluationDate() = todaysDate;

    DayCounter dayCounter = Actual360();
    Integer fixingDays = 2;
    Date settlementDate = calendar.advance(todaysDate, fixingDays, Days);

    const Size size = 10;
    const Size steps = 8 * size;
    const Size nrTrails = 10;  // Reduced for faster testing

    // Market quote values - more realistic curve
    // Deposit rates: ON, 1M, 3M, 6M
    std::vector<Period> depoTenors = {1 * Days, 1 * Months, 3 * Months, 6 * Months};
    std::vector<double> depoRates_val = {0.0350, 0.0365, 0.0380, 0.0400};

    // Swap rates: 1Y, 2Y, 3Y, 4Y, 5Y
    std::vector<Period> swapTenors = {1 * Years, 2 * Years, 3 * Years, 4 * Years, 5 * Years};
    std::vector<double> swapRates_val = {0.0420, 0.0480, 0.0520, 0.0550, 0.0575};

    Size numDeposits = depoTenors.size();
    Size numSwaps = swapTenors.size();
    Size numMarketQuotes = numDeposits + numSwaps;  // 4 + 5 = 9 market inputs

    // Swaption parameters
    Size i_opt = 2;  // Option expiry index
    Size j_opt = 2;  // Swap length index

    std::cout << "  Setup:\n";
    std::cout << "    - Market quotes: " << numDeposits << " deposits + " << numSwaps << " swaps = " << numMarketQuotes << " inputs\n";
    std::cout << "    - Deposit tenors: ON, 1M, 3M, 6M\n";
    std::cout << "    - Swap tenors: 1Y, 2Y, 3Y, 4Y, 5Y\n";
    std::cout << "    - Forward rates: " << size << "\n";
    std::cout << "    - MC paths: " << nrTrails << "\n";
    std::cout << "    - Swaption: " << (i_opt*6) << "M option into " << (j_opt*6) << "M swap\n";
    std::cout << std::endl;

    // =========================================================================
    // Helper: Build curve and LMM process from market quote values
    // =========================================================================
    auto buildCurveAndProcess = [&](const std::vector<double>& depoRates,
                                    const std::vector<double>& swapRates,
                                    std::vector<Date>& curveDates_out,
                                    std::vector<Rate>& zeroRates_out) {
        RelinkableHandle<YieldTermStructure> euriborTS;
        auto euribor6m = ext::make_shared<Euribor6M>(euriborTS);
        euribor6m->addFixing(Date(2, September, 2005), 0.04);

        std::vector<ext::shared_ptr<RateHelper>> instruments;

        // Add deposit rate helpers
        for (Size idx = 0; idx < depoTenors.size(); ++idx) {
            auto depoQuote = ext::make_shared<SimpleQuote>(depoRates[idx]);
            instruments.push_back(ext::make_shared<DepositRateHelper>(
                Handle<Quote>(depoQuote), depoTenors[idx], fixingDays,
                calendar, ModifiedFollowing, true, dayCounter));
        }

        // Add swap rate helpers
        for (Size idx = 0; idx < swapTenors.size(); ++idx) {
            auto swapQuote = ext::make_shared<SimpleQuote>(swapRates[idx]);
            instruments.push_back(ext::make_shared<SwapRateHelper>(
                Handle<Quote>(swapQuote), swapTenors[idx],
                calendar, Annual, Unadjusted, Thirty360(Thirty360::BondBasis), euribor6m));
        }

        auto yieldCurve = ext::make_shared<PiecewiseYieldCurve<ZeroYield, Linear>>(
            settlementDate, instruments, dayCounter);
        yieldCurve->enableExtrapolation();

        curveDates_out.clear();
        zeroRates_out.clear();
        curveDates_out.push_back(settlementDate);
        zeroRates_out.push_back(yieldCurve->zeroRate(settlementDate, dayCounter, Continuous).rate());
        Date endDate = settlementDate + 6 * Years;
        curveDates_out.push_back(endDate);
        zeroRates_out.push_back(yieldCurve->zeroRate(endDate, dayCounter, Continuous).rate());
    };

    // Build base curve to get dimensions
    std::vector<Date> baseCurveDates;
    std::vector<Rate> baseZeroRates;
    buildCurveAndProcess(depoRates_val, swapRates_val, baseCurveDates, baseZeroRates);

    // Build base LMM process
    RelinkableHandle<YieldTermStructure> baseTermStructure;
    ext::shared_ptr<IborIndex> baseIndex(new Euribor6M(baseTermStructure));
    baseIndex->addFixing(Date(2, September, 2005), 0.04);
    baseTermStructure.linkTo(ext::make_shared<ZeroCurve>(baseCurveDates, baseZeroRates, dayCounter));

    ext::shared_ptr<LiborForwardModelProcess> baseProcess(
        new LiborForwardModelProcess(size, baseIndex));
    ext::shared_ptr<LmCorrelationModel> baseCorrModel(
        new LmExponentialCorrelationModel(size, 0.5));
    ext::shared_ptr<LmVolatilityModel> baseVolaModel(
        new LmLinearExponentialVolatilityModel(baseProcess->fixingTimes(),
                                               0.291, 1.483, 0.116, 0.00001));
    baseProcess->setCovarParam(ext::shared_ptr<LfmCovarianceParameterization>(
        new LfmCovarianceProxy(baseVolaModel, baseCorrModel)));

    std::vector<Time> fixingTimes = baseProcess->fixingTimes();
    TimeGrid grid(fixingTimes.begin(), fixingTimes.end(), steps);

    std::vector<Size> location;
    for (Size idx = 0; idx < fixingTimes.size(); ++idx) {
        location.push_back(
            std::find(grid.begin(), grid.end(), fixingTimes[idx]) - grid.begin());
    }

    Size numFactors = baseProcess->factors();
    Size exerciseStep = location[i_opt];
    Size totalRandoms = numFactors * exerciseStep;

    // Get fair swap rate
    BusinessDayConvention convention = baseIndex->businessDayConvention();
    Date settlement = baseIndex->forwardingTermStructure()->referenceDate();
    Date fwdStart = settlement + Period(6 * i_opt, Months);
    Date fwdMaturity = fwdStart + Period(6 * j_opt, Months);

    Schedule schedule(fwdStart, fwdMaturity, baseIndex->tenor(), calendar,
                      convention, convention, DateGeneration::Forward, false);

    ext::shared_ptr<VanillaSwap> forwardSwap(
        new VanillaSwap(Swap::Receiver, 1.0,
                        schedule, 0.05, dayCounter,
                        schedule, baseIndex, 0.0, baseIndex->dayCounter()));
    forwardSwap->setPricingEngine(ext::make_shared<DiscountingSwapEngine>(
        baseIndex->forwardingTermStructure()));

    double swapRate_double = value(forwardSwap->fairRate());

    // Pre-generate random numbers for Re-Route approaches
    // Use same dimension as QuantLib MultiPathGenerator for RNG synchronization
    typedef PseudoRandom::rsg_type rsg_type;
    Size fullGridRandoms = numFactors * (grid.size() - 1);  // Full grid dimension (like MultiPathGenerator)
    rsg_type rsg = PseudoRandom::make_sequence_generator(fullGridRandoms, BigNatural(42));

    std::vector<std::vector<double>> allRandoms(nrTrails);
    for (Size n = 0; n < nrTrails; ++n) {
        const auto& sequence = (n % 2) != 0U ? rsg.lastSequence() : rsg.nextSequence();
        // Store only up to exerciseStep (but generate full dimension to keep RNG in sync)
        allRandoms[n].resize(totalRandoms);
        for (Size m = 0; m < totalRandoms; ++m) {
            double rnd = (n % 2) != 0U ? -value(sequence.value[m]) : value(sequence.value[m]);
            allRandoms[n][m] = rnd;
        }
    }

    // =========================================================================
    // 0. ORIGINAL (QuantLib): Uses MultiPathGenerator
    // =========================================================================
    rsg_type rsg_ql = PseudoRandom::make_sequence_generator(fullGridRandoms, BigNatural(42));
    typedef MultiPathGenerator<rsg_type>::sample_type sample_type;
    MultiPathGenerator<rsg_type> generator_ql(baseProcess, grid, rsg_ql, false);

    double mcPrice_orig_ql = 0.0;
    for (Size n = 0; n < nrTrails; ++n) {
        sample_type path = (n % 2) != 0U ? generator_ql.antithetic() : generator_ql.next();

        std::vector<double> mcRates(size);
        for (Size k = 0; k < size; ++k) {
            mcRates[k] = value(path.value[k][location[i_opt]]);
        }

        std::vector<double> dis(size);
        double df = 1.0;
        for (Size k = 0; k < size; ++k) {
            double accrual = value(baseProcess->accrualEndTimes()[k]) - value(baseProcess->accrualStartTimes()[k]);
            df = df / (1.0 + mcRates[k] * accrual);
            dis[k] = df;
        }

        double npv = 0.0;
        for (Size m = i_opt; m < i_opt + j_opt; ++m) {
            double accrual = value(baseProcess->accrualEndTimes()[m]) - value(baseProcess->accrualStartTimes()[m]);
            npv += (swapRate_double - mcRates[m]) * accrual * dis[m];
        }
        if (npv > 0.0) mcPrice_orig_ql += npv;
    }
    mcPrice_orig_ql /= static_cast<double>(nrTrails);

    // Get base initial rates
    Array baseInitRates = baseProcess->initialValues();
    std::vector<double> baseInitRates_vec(size);
    for (Size k = 0; k < size; ++k) {
        baseInitRates_vec[k] = value(baseInitRates[k]);
    }

    // Get accrual times
    std::vector<double> accrualStart(size), accrualEnd(size);
    for (Size k = 0; k < size; ++k) {
        accrualStart[k] = value(baseProcess->accrualStartTimes()[k]);
        accrualEnd[k] = value(baseProcess->accrualEndTimes()[k]);
    }

    // =========================================================================
    // Helper lambda: Compute MC price from market quotes (for bump-and-reprice)
    // =========================================================================
    auto computeMCPrice = [&](const std::vector<double>& depoRates,
                              const std::vector<double>& swapRates) {
        // Build curve
        std::vector<Date> curveDates;
        std::vector<Rate> zeroRates;
        buildCurveAndProcess(depoRates, swapRates, curveDates, zeroRates);

        // Build process
        RelinkableHandle<YieldTermStructure> termStructure;
        ext::shared_ptr<IborIndex> index(new Euribor6M(termStructure));
        index->addFixing(Date(2, September, 2005), 0.04);
        termStructure.linkTo(ext::make_shared<ZeroCurve>(curveDates, zeroRates, dayCounter));

        ext::shared_ptr<LiborForwardModelProcess> process(
            new LiborForwardModelProcess(size, index));
        ext::shared_ptr<LmCorrelationModel> corrModel(
            new LmExponentialCorrelationModel(size, 0.5));
        ext::shared_ptr<LmVolatilityModel> volaModel(
            new LmLinearExponentialVolatilityModel(process->fixingTimes(),
                                                   0.291, 1.483, 0.116, 0.00001));
        process->setCovarParam(ext::shared_ptr<LfmCovarianceParameterization>(
            new LfmCovarianceProxy(volaModel, corrModel)));

        // Get swap rate
        ext::shared_ptr<VanillaSwap> fwdSwap(
            new VanillaSwap(Swap::Receiver, 1.0,
                            schedule, 0.05, dayCounter,
                            schedule, index, 0.0, index->dayCounter()));
        fwdSwap->setPricingEngine(ext::make_shared<DiscountingSwapEngine>(
            index->forwardingTermStructure()));
        double swapRate = value(fwdSwap->fairRate());

        Array initRates = process->initialValues();

        // MC loop
        double price = 0.0;
        for (Size n = 0; n < nrTrails; ++n) {
            std::vector<double> asset(size);
            for (Size k = 0; k < size; ++k) asset[k] = value(initRates[k]);

            for (Size step = 1; step <= exerciseStep; ++step) {
                Size offset = (step - 1) * numFactors;
                Time t = grid[step - 1];
                Time dt = grid.dt(step - 1);

                Array dw(numFactors);
                for (Size f = 0; f < numFactors; ++f) {
                    dw[f] = allRandoms[n][offset + f];
                }

                Array asset_arr(size);
                for (Size k = 0; k < size; ++k) asset_arr[k] = asset[k];

                Array evolved = process->evolve(t, asset_arr, dt, dw);
                for (Size k = 0; k < size; ++k) asset[k] = value(evolved[k]);
            }

            // Discount factors
            std::vector<double> dis(size);
            double df = 1.0;
            for (Size k = 0; k < size; ++k) {
                double accrual = accrualEnd[k] - accrualStart[k];
                df = df / (1.0 + asset[k] * accrual);
                dis[k] = df;
            }

            // Payoff
            double npv = 0.0;
            for (Size m = i_opt; m < i_opt + j_opt; ++m) {
                double accrual = accrualEnd[m] - accrualStart[m];
                npv += (swapRate - asset[m]) * accrual * dis[m];
            }

            if (npv > 0.0) price += npv;
        }

        return price / static_cast<double>(nrTrails);
    };

    // =========================================================================
    // 1. ORIGINAL (Re-Route): Compute base price (non-XAD, custom loop)
    // =========================================================================
    double mcPrice_orig_rr = computeMCPrice(depoRates_val, swapRates_val);

    // =========================================================================
    // 2. XAD (QuantLib): Uses MultiPathGenerator with XAD tape
    // =========================================================================
    using tape_type = Real::tape_type;
    tape_type tape_ql;

    // Register market quotes as inputs
    std::vector<Real> depositRates_ql(numDeposits);
    std::vector<Real> swapRates_ql(numSwaps);
    for (Size idx = 0; idx < numDeposits; ++idx) depositRates_ql[idx] = depoRates_val[idx];
    for (Size idx = 0; idx < numSwaps; ++idx) swapRates_ql[idx] = swapRates_val[idx];

    tape_ql.registerInputs(depositRates_ql);
    tape_ql.registerInputs(swapRates_ql);
    tape_ql.newRecording();

    // Bootstrap curve on tape
    RelinkableHandle<YieldTermStructure> euriborTS_ql;
    auto euribor6m_ql = ext::make_shared<Euribor6M>(euriborTS_ql);
    euribor6m_ql->addFixing(Date(2, September, 2005), 0.04);

    std::vector<ext::shared_ptr<RateHelper>> instruments_ql;
    for (Size idx = 0; idx < numDeposits; ++idx) {
        auto depoQuote = ext::make_shared<SimpleQuote>(depositRates_ql[idx]);
        instruments_ql.push_back(ext::make_shared<DepositRateHelper>(
            Handle<Quote>(depoQuote), depoTenors[idx], fixingDays,
            calendar, ModifiedFollowing, true, dayCounter));
    }
    for (Size idx = 0; idx < numSwaps; ++idx) {
        auto swapQuote = ext::make_shared<SimpleQuote>(swapRates_ql[idx]);
        instruments_ql.push_back(ext::make_shared<SwapRateHelper>(
            Handle<Quote>(swapQuote), swapTenors[idx],
            calendar, Annual, Unadjusted, Thirty360(Thirty360::BondBasis), euribor6m_ql));
    }

    auto yieldCurve_ql = ext::make_shared<PiecewiseYieldCurve<ZeroYield, Linear>>(
        settlementDate, instruments_ql, dayCounter);
    yieldCurve_ql->enableExtrapolation();

    std::vector<Date> curveDates_ql;
    std::vector<Real> zeroRates_ql;
    curveDates_ql.push_back(settlementDate);
    zeroRates_ql.push_back(yieldCurve_ql->zeroRate(settlementDate, dayCounter, Continuous).rate());
    Date endDate_ql = settlementDate + 6 * Years;
    curveDates_ql.push_back(endDate_ql);
    zeroRates_ql.push_back(yieldCurve_ql->zeroRate(endDate_ql, dayCounter, Continuous).rate());

    // Build LMM process on tape
    RelinkableHandle<YieldTermStructure> termStructure_ql;
    ext::shared_ptr<IborIndex> index_ql(new Euribor6M(termStructure_ql));
    index_ql->addFixing(Date(2, September, 2005), 0.04);
    termStructure_ql.linkTo(ext::make_shared<ZeroCurve>(curveDates_ql, zeroRates_ql, dayCounter));

    ext::shared_ptr<LiborForwardModelProcess> process_ql(
        new LiborForwardModelProcess(size, index_ql));
    process_ql->setCovarParam(ext::shared_ptr<LfmCovarianceParameterization>(
        new LfmCovarianceProxy(
            ext::make_shared<LmLinearExponentialVolatilityModel>(process_ql->fixingTimes(), 0.291, 1.483, 0.116, 0.00001),
            ext::make_shared<LmExponentialCorrelationModel>(size, 0.5))));

    // Get grid and locations for this process
    std::vector<Time> fixingTimes_ql = process_ql->fixingTimes();
    TimeGrid grid_ql(fixingTimes_ql.begin(), fixingTimes_ql.end(), steps);
    std::vector<Size> location_ql;
    for (Size idx = 0; idx < fixingTimes_ql.size(); ++idx) {
        location_ql.push_back(
            std::find(grid_ql.begin(), grid_ql.end(), fixingTimes_ql[idx]) - grid_ql.begin());
    }

    // Get swap rate
    ext::shared_ptr<VanillaSwap> fwdSwap_ql(
        new VanillaSwap(Swap::Receiver, 1.0,
                        schedule, 0.05, dayCounter,
                        schedule, index_ql, 0.0, index_ql->dayCounter()));
    fwdSwap_ql->setPricingEngine(ext::make_shared<DiscountingSwapEngine>(
        index_ql->forwardingTermStructure()));
    Real swapRate_ql = fwdSwap_ql->fairRate();

    // MC with MultiPathGenerator using XAD types
    rsg_type rsg_xad_ql = PseudoRandom::make_sequence_generator(fullGridRandoms, BigNatural(42));
    MultiPathGenerator<rsg_type> generator_xad_ql(process_ql, grid_ql, rsg_xad_ql, false);

    Real mcPrice_xad_ql = 0.0;
    for (Size n = 0; n < nrTrails; ++n) {
        sample_type path = (n % 2) != 0U ? generator_xad_ql.antithetic() : generator_xad_ql.next();

        std::vector<Real> mcRates(size);
        for (Size k = 0; k < size; ++k) {
            mcRates[k] = path.value[k][location_ql[i_opt]];
        }

        std::vector<Real> dis(size);
        Real df = 1.0;
        for (Size k = 0; k < size; ++k) {
            Real accrual = process_ql->accrualEndTimes()[k] - process_ql->accrualStartTimes()[k];
            df = df / (Real(1.0) + mcRates[k] * accrual);
            dis[k] = df;
        }

        Real npv = 0.0;
        for (Size m = i_opt; m < i_opt + j_opt; ++m) {
            Real accrual = process_ql->accrualEndTimes()[m] - process_ql->accrualStartTimes()[m];
            npv = npv + (swapRate_ql - mcRates[m]) * accrual * dis[m];
        }
        if (value(npv) > 0.0) mcPrice_xad_ql += npv;
    }
    mcPrice_xad_ql /= static_cast<Real>(nrTrails);

    tape_ql.registerOutput(mcPrice_xad_ql);
    derivative(mcPrice_xad_ql) = 1.0;
    tape_ql.computeAdjoints();

    std::vector<double> dPrice_xad_ql_depo(numDeposits);
    std::vector<double> dPrice_xad_ql_swap(numSwaps);
    for (Size idx = 0; idx < numDeposits; ++idx) dPrice_xad_ql_depo[idx] = derivative(depositRates_ql[idx]);
    for (Size idx = 0; idx < numSwaps; ++idx) dPrice_xad_ql_swap[idx] = derivative(swapRates_ql[idx]);
    tape_ql.deactivate();

    // =========================================================================
    // 3. XAD (Re-Route): Full pipeline with tape recording (custom loop)
    // =========================================================================
    tape_type tape_rr;

    // Convert to XAD Real vectors
    std::vector<Real> depositRates(numDeposits);
    std::vector<Real> swapRates(numSwaps);
    for (Size idx = 0; idx < numDeposits; ++idx) depositRates[idx] = depoRates_val[idx];
    for (Size idx = 0; idx < numSwaps; ++idx) swapRates[idx] = swapRates_val[idx];

    tape_rr.registerInputs(depositRates);
    tape_rr.registerInputs(swapRates);
    tape_rr.newRecording();

    // Bootstrap curve on tape
    RelinkableHandle<YieldTermStructure> euriborTermStructure;
    auto euribor6m = ext::make_shared<Euribor6M>(euriborTermStructure);
    euribor6m->addFixing(Date(2, September, 2005), 0.04);

    std::vector<ext::shared_ptr<RateHelper>> instruments;

    // Add deposit rate helpers
    for (Size idx = 0; idx < numDeposits; ++idx) {
        auto depoQuote = ext::make_shared<SimpleQuote>(depositRates[idx]);
        instruments.push_back(ext::make_shared<DepositRateHelper>(
            Handle<Quote>(depoQuote), depoTenors[idx], fixingDays,
            calendar, ModifiedFollowing, true, dayCounter));
    }

    // Add swap rate helpers
    for (Size idx = 0; idx < numSwaps; ++idx) {
        auto swapQuote = ext::make_shared<SimpleQuote>(swapRates[idx]);
        instruments.push_back(ext::make_shared<SwapRateHelper>(
            Handle<Quote>(swapQuote), swapTenors[idx],
            calendar, Annual, Unadjusted, Thirty360(Thirty360::BondBasis),
            euribor6m));
    }

    auto yieldCurve = ext::make_shared<PiecewiseYieldCurve<ZeroYield, Linear>>(
        settlementDate, instruments, dayCounter);
    yieldCurve->enableExtrapolation();

    std::vector<Date> curveDates;
    std::vector<Real> zeroRates;
    curveDates.push_back(settlementDate);
    zeroRates.push_back(yieldCurve->zeroRate(settlementDate, dayCounter, Continuous).rate());
    Date endDate = settlementDate + 6 * Years;
    curveDates.push_back(endDate);
    zeroRates.push_back(yieldCurve->zeroRate(endDate, dayCounter, Continuous).rate());

    // Build LMM process on tape
    RelinkableHandle<YieldTermStructure> termStructure;
    ext::shared_ptr<IborIndex> index(new Euribor6M(termStructure));
    index->addFixing(Date(2, September, 2005), 0.04);
    termStructure.linkTo(ext::make_shared<ZeroCurve>(curveDates, zeroRates, dayCounter));

    ext::shared_ptr<LiborForwardModelProcess> process(
        new LiborForwardModelProcess(size, index));
    ext::shared_ptr<LmCorrelationModel> corrModel(
        new LmExponentialCorrelationModel(size, 0.5));
    ext::shared_ptr<LmVolatilityModel> volaModel(
        new LmLinearExponentialVolatilityModel(process->fixingTimes(),
                                               0.291, 1.483, 0.116, 0.00001));
    process->setCovarParam(ext::shared_ptr<LfmCovarianceParameterization>(
        new LfmCovarianceProxy(volaModel, corrModel)));

    // Get swap rate (on tape)
    ext::shared_ptr<VanillaSwap> fwdSwap_tape(
        new VanillaSwap(Swap::Receiver, 1.0,
                        schedule, 0.05, dayCounter,
                        schedule, index, 0.0, index->dayCounter()));
    fwdSwap_tape->setPricingEngine(ext::make_shared<DiscountingSwapEngine>(
        index->forwardingTermStructure()));
    Real swapRate_tape = fwdSwap_tape->fairRate();

    Array initRates_tape = process->initialValues();

    // MC loop on tape
    Real mcPrice_tape = 0.0;
    for (Size n = 0; n < nrTrails; ++n) {
        std::vector<Real> asset(size);
        for (Size k = 0; k < size; ++k) asset[k] = initRates_tape[k];

        for (Size step = 1; step <= exerciseStep; ++step) {
            Size offset = (step - 1) * numFactors;
            Time t = grid[step - 1];
            Time dt = grid.dt(step - 1);

            Array dw(numFactors);
            for (Size f = 0; f < numFactors; ++f) {
                dw[f] = allRandoms[n][offset + f];
            }

            Array asset_arr(size);
            for (Size k = 0; k < size; ++k) asset_arr[k] = asset[k];

            Array evolved = process->evolve(t, asset_arr, dt, dw);
            for (Size k = 0; k < size; ++k) asset[k] = evolved[k];
        }

        // Discount factors
        std::vector<Real> dis(size);
        Real df = 1.0;
        for (Size k = 0; k < size; ++k) {
            double accrual = accrualEnd[k] - accrualStart[k];
            df = df / (Real(1.0) + asset[k] * accrual);
            dis[k] = df;
        }

        // Payoff
        Real npv = 0.0;
        for (Size m = i_opt; m < i_opt + j_opt; ++m) {
            double accrual = accrualEnd[m] - accrualStart[m];
            npv = npv + (swapRate_tape - asset[m]) * accrual * dis[m];
        }

        if (value(npv) > 0.0) mcPrice_tape += npv;
    }
    mcPrice_tape /= static_cast<Real>(nrTrails);

    // Compute XAD derivatives
    tape_rr.registerOutput(mcPrice_tape);
    derivative(mcPrice_tape) = 1.0;
    tape_rr.computeAdjoints();

    std::vector<double> dPrice_xad_rr_depo(numDeposits);
    std::vector<double> dPrice_xad_rr_swap(numSwaps);
    for (Size idx = 0; idx < numDeposits; ++idx) dPrice_xad_rr_depo[idx] = derivative(depositRates[idx]);
    for (Size idx = 0; idx < numSwaps; ++idx) dPrice_xad_rr_swap[idx] = derivative(swapRates[idx]);

    // Deactivate tape for JIT
    tape_rr.deactivate();

    // =========================================================================
    // 4. JIT: Tape for bootstrap, JIT for MC
    // =========================================================================
    // Re-create tape for Jacobian computation only
    tape_type tape2;

    std::vector<Real> depositRates2(numDeposits);
    std::vector<Real> swapRates2(numSwaps);
    for (Size idx = 0; idx < numDeposits; ++idx) depositRates2[idx] = depoRates_val[idx];
    for (Size idx = 0; idx < numSwaps; ++idx) swapRates2[idx] = swapRates_val[idx];

    tape2.registerInputs(depositRates2);
    tape2.registerInputs(swapRates2);
    tape2.newRecording();

    // Bootstrap curve
    RelinkableHandle<YieldTermStructure> euriborTS2;
    auto euribor6m2 = ext::make_shared<Euribor6M>(euriborTS2);
    euribor6m2->addFixing(Date(2, September, 2005), 0.04);

    std::vector<ext::shared_ptr<RateHelper>> instruments2;

    // Add deposit rate helpers
    for (Size idx = 0; idx < numDeposits; ++idx) {
        auto depoQuote2 = ext::make_shared<SimpleQuote>(depositRates2[idx]);
        instruments2.push_back(ext::make_shared<DepositRateHelper>(
            Handle<Quote>(depoQuote2), depoTenors[idx], fixingDays,
            calendar, ModifiedFollowing, true, dayCounter));
    }

    // Add swap rate helpers
    for (Size idx = 0; idx < numSwaps; ++idx) {
        auto swapQuote = ext::make_shared<SimpleQuote>(swapRates2[idx]);
        instruments2.push_back(ext::make_shared<SwapRateHelper>(
            Handle<Quote>(swapQuote), swapTenors[idx],
            calendar, Annual, Unadjusted, Thirty360(Thirty360::BondBasis),
            euribor6m2));
    }

    auto yieldCurve2 = ext::make_shared<PiecewiseYieldCurve<ZeroYield, Linear>>(
        settlementDate, instruments2, dayCounter);
    yieldCurve2->enableExtrapolation();

    std::vector<Date> curveDates2;
    std::vector<Real> zeroRates2;
    curveDates2.push_back(settlementDate);
    zeroRates2.push_back(yieldCurve2->zeroRate(settlementDate, dayCounter, Continuous).rate());
    Date endDate2 = settlementDate + 6 * Years;
    curveDates2.push_back(endDate2);
    zeroRates2.push_back(yieldCurve2->zeroRate(endDate2, dayCounter, Continuous).rate());

    // Build process
    std::vector<Rate> zeroRates2_ql;
    for (const auto& r : zeroRates2) zeroRates2_ql.push_back(r);

    RelinkableHandle<YieldTermStructure> termStructure2;
    ext::shared_ptr<IborIndex> index2(new Euribor6M(termStructure2));
    index2->addFixing(Date(2, September, 2005), 0.04);
    termStructure2.linkTo(ext::make_shared<ZeroCurve>(curveDates2, zeroRates2_ql, dayCounter));

    ext::shared_ptr<LiborForwardModelProcess> process2(
        new LiborForwardModelProcess(size, index2));
    ext::shared_ptr<LmCorrelationModel> corrModel2(
        new LmExponentialCorrelationModel(size, 0.5));
    ext::shared_ptr<LmVolatilityModel> volaModel2(
        new LmLinearExponentialVolatilityModel(process2->fixingTimes(),
                                               0.291, 1.483, 0.116, 0.00001));
    process2->setCovarParam(ext::shared_ptr<LfmCovarianceParameterization>(
        new LfmCovarianceProxy(volaModel2, corrModel2)));

    // Get swap rate (on tape)
    ext::shared_ptr<VanillaSwap> fwdSwap2(
        new VanillaSwap(Swap::Receiver, 1.0,
                        schedule, 0.05, dayCounter,
                        schedule, index2, 0.0, index2->dayCounter()));
    fwdSwap2->setPricingEngine(ext::make_shared<DiscountingSwapEngine>(
        index2->forwardingTermStructure()));
    Real swapRate2_tape = fwdSwap2->fairRate();

    Array initRates2 = process2->initialValues();

    // Compute Jacobian: d(InitRates, SwapRate) / dMarketQuotes
    // numMarketQuotes already defined above (9 = 4 deposits + 5 swaps)
    Size numIntermediates = size + 1;  // 10 init rates + 1 swap rate

    // Flat row-major Jacobian: jacobian[i * numMarketQuotes + j] = d(intermediate_i)/d(input_j)
    std::vector<double> jacobian(numIntermediates * numMarketQuotes, 0.0);

    for (Size k = 0; k < size; ++k) {
        if (initRates2[k].shouldRecord()) {
            tape2.clearDerivatives();
            tape2.registerOutput(initRates2[k]);
            derivative(initRates2[k]) = 1.0;
            tape2.computeAdjoints();

            double* jac_row = jacobian.data() + k * numMarketQuotes;
            for (Size m = 0; m < numDeposits; ++m)
                jac_row[m] = derivative(depositRates2[m]);
            for (Size m = 0; m < numSwaps; ++m)
                jac_row[numDeposits + m] = derivative(swapRates2[m]);
        }
    }

    if (swapRate2_tape.shouldRecord()) {
        tape2.clearDerivatives();
        tape2.registerOutput(swapRate2_tape);
        derivative(swapRate2_tape) = 1.0;
        tape2.computeAdjoints();

        double* jac_row = jacobian.data() + size * numMarketQuotes;
        for (Size m = 0; m < numDeposits; ++m)
            jac_row[m] = derivative(depositRates2[m]);
        for (Size m = 0; m < numSwaps; ++m)
            jac_row[numDeposits + m] = derivative(swapRates2[m]);
    }

    // Deactivate tape for JIT
    tape2.deactivate();

    // JIT setup with ForgeBackend
    auto forgeBackend4 = std::make_unique<xad::forge::ForgeBackend<double>>();
    xad::JITCompiler<double> jit(std::move(forgeBackend4));

    std::vector<xad::AD> jit_initRates(size);
    xad::AD jit_swapRate;
    std::vector<xad::AD> jit_randoms(totalRandoms);

    for (Size k = 0; k < size; ++k) {
        jit_initRates[k] = xad::AD(value(initRates2[k]));
        jit.registerInput(jit_initRates[k]);
    }

    jit_swapRate = xad::AD(value(swapRate2_tape));
    jit.registerInput(jit_swapRate);

    for (Size m = 0; m < totalRandoms; ++m) {
        jit_randoms[m] = xad::AD(0.0);
        jit.registerInput(jit_randoms[m]);
    }

    jit.newRecording();

    // Record path evolution
    std::vector<xad::AD> asset_jit(size);
    for (Size k = 0; k < size; ++k) {
        asset_jit[k] = jit_initRates[k];
    }

    for (Size step = 1; step <= exerciseStep; ++step) {
        Size offset = (step - 1) * numFactors;
        Time t = grid[step - 1];
        Time dt = grid.dt(step - 1);

        Array dw(numFactors);
        for (Size f = 0; f < numFactors; ++f) {
            dw[f] = jit_randoms[offset + f];
        }

        Array asset_arr(size);
        for (Size k = 0; k < size; ++k) asset_arr[k] = asset_jit[k];

        Array evolved = process2->evolve(t, asset_arr, dt, dw);
        for (Size k = 0; k < size; ++k) asset_jit[k] = evolved[k];
    }

    // Compute discount factors
    std::vector<xad::AD> dis_jit(size);
    xad::AD df_jit = xad::AD(1.0);
    for (Size k = 0; k < size; ++k) {
        double accrual = accrualEnd[k] - accrualStart[k];
        df_jit = df_jit / (xad::AD(1.0) + asset_jit[k] * accrual);
        dis_jit[k] = df_jit;
    }

    // Compute payoff
    xad::AD jit_npv = xad::AD(0.0);
    for (Size m = i_opt; m < i_opt + j_opt; ++m) {
        double accrual = accrualEnd[m] - accrualStart[m];
        jit_npv = jit_npv + (jit_swapRate - asset_jit[m]) * accrual * dis_jit[m];
    }

    xad::AD jit_payoff = xad::less(jit_npv, xad::AD(0.0)).If(xad::AD(0.0), jit_npv);
    jit.registerOutput(jit_payoff);

    // Compile the JIT kernel before MC loop
    jit.compile();

    // JIT MC loop
    double mcPrice_jit = 0.0;
    std::vector<double> dPrice_dInitRates(size, 0.0);
    double dPrice_dSwapRate_jit = 0.0;

    const auto& graph = jit.getGraph();
    uint32_t outputSlot = graph.output_ids[0];

    for (Size n = 0; n < nrTrails; ++n) {
        for (Size k = 0; k < size; ++k) {
            value(jit_initRates[k]) = value(initRates2[k]);
        }
        value(jit_swapRate) = value(swapRate2_tape);

        for (Size m = 0; m < totalRandoms; ++m) {
            value(jit_randoms[m]) = allRandoms[n][m];
        }

        double payoff_value;
        jit.forward(&payoff_value);
        mcPrice_jit += payoff_value;

        jit.clearDerivatives();
        jit.setDerivative(outputSlot, 1.0);
        jit.computeAdjoints();

        for (Size k = 0; k < size; ++k) {
            dPrice_dInitRates[k] += jit.derivative(graph.input_ids[k]);
        }
        dPrice_dSwapRate_jit += jit.derivative(graph.input_ids[size]);  // swap rate is at index 'size'
    }

    mcPrice_jit /= static_cast<double>(nrTrails);
    for (Size k = 0; k < size; ++k) {
        dPrice_dInitRates[k] /= static_cast<double>(nrTrails);
    }
    dPrice_dSwapRate_jit /= static_cast<double>(nrTrails);

    // Chain rule: dPrice/dMarketQuotes = jacobian^T * derivatives
    // Build derivatives vector: [dPrice/dInitRate_0, ..., dPrice/dInitRate_9, dPrice/dSwapRate]
    std::vector<double> dPrice_dIntermediates(numIntermediates);
    for (Size k = 0; k < size; ++k)
        dPrice_dIntermediates[k] = dPrice_dInitRates[k];
    dPrice_dIntermediates[size] = dPrice_dSwapRate_jit;

    // Apply chain rule using high-performance function
    std::vector<double> dPrice_jit_market(numMarketQuotes);
    applyChainRule(jacobian.data(), dPrice_dIntermediates.data(), dPrice_jit_market.data(),
                   numIntermediates, numMarketQuotes);

    // Split JIT derivatives into deposits and swaps
    std::vector<double> dPrice_jit_depo(numDeposits);
    std::vector<double> dPrice_jit_swap(numSwaps);
    for (Size idx = 0; idx < numDeposits; ++idx) dPrice_jit_depo[idx] = dPrice_jit_market[idx];
    for (Size idx = 0; idx < numSwaps; ++idx) dPrice_jit_swap[idx] = dPrice_jit_market[numDeposits + idx];

    // =========================================================================
    // 5a. BUMP-AND-REPRICE (QuantLib) - MultiPathGenerator
    // =========================================================================
    double bump = 1e-6;

    // Helper for QuantLib MC pricing using MultiPathGenerator
    auto computeMCPriceQL = [&](const std::vector<double>& depoRates,
                                const std::vector<double>& swapRates) {
        std::vector<Date> curveDates;
        std::vector<Rate> zeroRates;
        buildCurveAndProcess(depoRates, swapRates, curveDates, zeroRates);

        RelinkableHandle<YieldTermStructure> termStructure;
        ext::shared_ptr<IborIndex> index_tmp(new Euribor6M(termStructure));
        index_tmp->addFixing(Date(2, September, 2005), 0.04);
        termStructure.linkTo(ext::make_shared<ZeroCurve>(curveDates, zeroRates, dayCounter));

        ext::shared_ptr<LiborForwardModelProcess> process_tmp(
            new LiborForwardModelProcess(size, index_tmp));
        process_tmp->setCovarParam(ext::shared_ptr<LfmCovarianceParameterization>(
            new LfmCovarianceProxy(
                ext::make_shared<LmLinearExponentialVolatilityModel>(process_tmp->fixingTimes(), 0.291, 1.483, 0.116, 0.00001),
                ext::make_shared<LmExponentialCorrelationModel>(size, 0.5))));

        ext::shared_ptr<VanillaSwap> fwdSwap_tmp(
            new VanillaSwap(Swap::Receiver, 1.0,
                            schedule, 0.05, dayCounter,
                            schedule, index_tmp, 0.0, index_tmp->dayCounter()));
        fwdSwap_tmp->setPricingEngine(ext::make_shared<DiscountingSwapEngine>(
            index_tmp->forwardingTermStructure()));
        double swapRate_tmp = value(fwdSwap_tmp->fairRate());

        // Use same grid dimension as base process
        rsg_type rsg_tmp = PseudoRandom::make_sequence_generator(fullGridRandoms, BigNatural(42));
        MultiPathGenerator<rsg_type> gen_tmp(process_tmp, grid, rsg_tmp, false);

        double price = 0.0;
        for (Size n = 0; n < nrTrails; ++n) {
            sample_type path = (n % 2) != 0U ? gen_tmp.antithetic() : gen_tmp.next();

            std::vector<double> mcRates(size);
            for (Size k = 0; k < size; ++k) {
                mcRates[k] = value(path.value[k][location[i_opt]]);
            }

            std::vector<double> dis(size);
            double df = 1.0;
            for (Size k = 0; k < size; ++k) {
                double accrual = accrualEnd[k] - accrualStart[k];
                df = df / (1.0 + mcRates[k] * accrual);
                dis[k] = df;
            }

            double npv = 0.0;
            for (Size m = i_opt; m < i_opt + j_opt; ++m) {
                double accrual = accrualEnd[m] - accrualStart[m];
                npv += (swapRate_tmp - mcRates[m]) * accrual * dis[m];
            }
            if (npv > 0.0) price += npv;
        }
        return price / static_cast<double>(nrTrails);
    };

    std::vector<double> dPrice_bump_ql_depo(numDeposits);
    std::vector<double> dPrice_bump_ql_swap(numSwaps);

    // Bump each deposit rate (QuantLib)
    for (Size idx = 0; idx < numDeposits; ++idx) {
        std::vector<double> bumpedDepoRates = depoRates_val;
        bumpedDepoRates[idx] += bump;
        double price_bumped = computeMCPriceQL(bumpedDepoRates, swapRates_val);
        dPrice_bump_ql_depo[idx] = (price_bumped - mcPrice_orig_ql) / bump;
    }

    // Bump each swap rate (QuantLib)
    for (Size idx = 0; idx < numSwaps; ++idx) {
        std::vector<double> bumpedSwapRates = swapRates_val;
        bumpedSwapRates[idx] += bump;
        double price_bumped = computeMCPriceQL(depoRates_val, bumpedSwapRates);
        dPrice_bump_ql_swap[idx] = (price_bumped - mcPrice_orig_ql) / bump;
    }

    // =========================================================================
    // 5b. BUMP-AND-REPRICE (Re-Route) - Custom loop
    // =========================================================================
    std::vector<double> dPrice_bump_rr_depo(numDeposits);
    std::vector<double> dPrice_bump_rr_swap(numSwaps);

    // Bump each deposit rate (Re-Route)
    for (Size idx = 0; idx < numDeposits; ++idx) {
        std::vector<double> bumpedDepoRates = depoRates_val;
        bumpedDepoRates[idx] += bump;
        double price_bumped = computeMCPrice(bumpedDepoRates, swapRates_val);
        dPrice_bump_rr_depo[idx] = (price_bumped - mcPrice_orig_rr) / bump;
    }

    // Bump each swap rate (Re-Route)
    for (Size idx = 0; idx < numSwaps; ++idx) {
        std::vector<double> bumpedSwapRates = swapRates_val;
        bumpedSwapRates[idx] += bump;
        double price_bumped = computeMCPrice(depoRates_val, bumpedSwapRates);
        dPrice_bump_rr_swap[idx] = (price_bumped - mcPrice_orig_rr) / bump;
    }

    // =========================================================================
    // COMPARISON TABLES
    // =========================================================================
    std::cout << "  " << std::string(85, '=') << "\n";
    std::cout << "  TABLE 1: MC PRICE COMPARISON\n";
    std::cout << "  " << std::string(85, '=') << "\n";
    std::cout << std::endl;

    std::cout << "  " << std::setw(18) << "Approach" << " | "
              << std::setw(14) << "MC Price" << " | "
              << std::setw(14) << "Diff vs Ref" << " | "
              << "Match\n";
    std::cout << "  " << std::string(18, '-') << "-+-"
              << std::string(14, '-') << "-+-"
              << std::string(14, '-') << "-+-"
              << std::string(5, '-') << "\n";

    // Reference: Original (QuantLib) using MultiPathGenerator
    std::cout << "  " << std::setw(18) << "Orig (QuantLib)" << " | "
              << std::fixed << std::setprecision(6) << std::setw(14) << mcPrice_orig_ql << " | "
              << std::setw(14) << "-" << " |   -  \n";

    // Original (Re-Route) - custom loop
    double diff_orig_rr = mcPrice_orig_rr - mcPrice_orig_ql;
    bool orig_rr_match = (std::abs(diff_orig_rr) < 1e-6);
    std::cout << "  " << std::setw(18) << "Orig (Re-Route)" << " | "
              << std::fixed << std::setprecision(6) << std::setw(14) << mcPrice_orig_rr << " | "
              << std::scientific << std::setprecision(2) << std::setw(14) << diff_orig_rr << " | "
              << (orig_rr_match ? "  OK " : "DIFF!") << "\n";

    // XAD (QuantLib) - MultiPathGenerator with XAD tape
    double diff_xad_ql = value(mcPrice_xad_ql) - mcPrice_orig_ql;
    bool xad_ql_match = (std::abs(diff_xad_ql) < 1e-10);
    std::cout << "  " << std::setw(18) << "XAD (QuantLib)" << " | "
              << std::fixed << std::setprecision(6) << std::setw(14) << value(mcPrice_xad_ql) << " | "
              << std::scientific << std::setprecision(2) << std::setw(14) << diff_xad_ql << " | "
              << (xad_ql_match ? "  OK " : "DIFF!") << "\n";

    // XAD (Re-Route) - custom loop with tape
    double diff_xad_rr = value(mcPrice_tape) - mcPrice_orig_rr;
    bool xad_rr_match = (std::abs(diff_xad_rr) < 1e-10);
    std::cout << "  " << std::setw(18) << "XAD (Re-Route)" << " | "
              << std::fixed << std::setprecision(6) << std::setw(14) << value(mcPrice_tape) << " | "
              << std::scientific << std::setprecision(2) << std::setw(14) << diff_xad_rr << " | "
              << (xad_rr_match ? "  OK " : "DIFF!") << "\n";

    // JIT (compare to Re-Route baseline)
    double diff_jit = mcPrice_jit - mcPrice_orig_rr;
    bool jit_match = (std::abs(diff_jit) < 1e-10);
    std::cout << "  " << std::setw(18) << "JIT" << " | "
              << std::fixed << std::setprecision(6) << std::setw(14) << mcPrice_jit << " | "
              << std::scientific << std::setprecision(2) << std::setw(14) << diff_jit << " | "
              << (jit_match ? "  OK " : "DIFF!") << "\n";

    std::cout << std::endl;
    std::cout << "  Note: Re-Route approaches use pre-generated randoms (same RNG sequence as QuantLib)\n";
    std::cout << "        XAD prices should match their non-XAD counterparts exactly\n";
    std::cout << std::endl;
    bool pricesMatch = xad_ql_match && xad_rr_match && jit_match;

    // Derivatives table
    std::cout << "  " << std::string(150, '=') << "\n";
    std::cout << "  TABLE 2: DERIVATIVE COMPARISON (dPrice/dMarketQuote)\n";
    std::cout << "  " << std::string(150, '=') << "\n";
    std::cout << std::endl;

    std::cout << "  " << std::setw(12) << "Quote" << " | "
              << std::setw(14) << "Bump QuantLib" << " | "
              << std::setw(14) << "Bump Re-Route" << " | "
              << std::setw(14) << "XAD QuantLib" << " | "
              << std::setw(14) << "XAD Re-Route" << " | "
              << std::setw(14) << "JIT" << " | "
              << std::setw(10) << "QL/RR %" << "\n";
    std::cout << "  " << std::string(12, '-') << "-+-"
              << std::string(14, '-') << "-+-"
              << std::string(14, '-') << "-+-"
              << std::string(14, '-') << "-+-"
              << std::string(14, '-') << "-+-"
              << std::string(14, '-') << "-+-"
              << std::string(10, '-') << "\n";

    // Helper for relative diff
    auto relDiff = [](double a, double b) {
        if (std::abs(b) < 1e-12) return 0.0;
        return (a - b) / b * 100.0;
    };

    // Deposit rates
    std::vector<std::string> depoLabels = {"Depo ON", "Depo 1M", "Depo 3M", "Depo 6M"};
    for (Size idx = 0; idx < numDeposits; ++idx) {
        double relDiff_ql_rr = relDiff(dPrice_xad_ql_depo[idx], dPrice_xad_rr_depo[idx]);
        std::cout << "  " << std::setw(12) << depoLabels[idx] << " | "
                  << std::scientific << std::setprecision(4) << std::setw(14) << dPrice_bump_ql_depo[idx] << " | "
                  << std::setw(14) << dPrice_bump_rr_depo[idx] << " | "
                  << std::setw(14) << dPrice_xad_ql_depo[idx] << " | "
                  << std::setw(14) << dPrice_xad_rr_depo[idx] << " | "
                  << std::setw(14) << dPrice_jit_depo[idx] << " | "
                  << std::fixed << std::setprecision(4) << std::setw(9) << relDiff_ql_rr << "%\n";
    }

    // Swap rates
    std::vector<std::string> swapLabels = {"Swap 1Y", "Swap 2Y", "Swap 3Y", "Swap 4Y", "Swap 5Y"};
    for (Size idx = 0; idx < numSwaps; ++idx) {
        double relDiff_ql_rr = relDiff(dPrice_xad_ql_swap[idx], dPrice_xad_rr_swap[idx]);
        std::cout << "  " << std::setw(12) << swapLabels[idx] << " | "
                  << std::scientific << std::setprecision(4) << std::setw(14) << dPrice_bump_ql_swap[idx] << " | "
                  << std::setw(14) << dPrice_bump_rr_swap[idx] << " | "
                  << std::setw(14) << dPrice_xad_ql_swap[idx] << " | "
                  << std::setw(14) << dPrice_xad_rr_swap[idx] << " | "
                  << std::setw(14) << dPrice_jit_swap[idx] << " | "
                  << std::fixed << std::setprecision(4) << std::setw(9) << relDiff_ql_rr << "%\n";
    }

    std::cout << std::endl;

    std::cout << "  STATUS: [STAGE 3 COMPLETE]\n";
    std::cout << "\n";
    std::cout << "=============================================================================\n";
    std::cout << std::endl;

    // Verify
    BOOST_CHECK(mcPrice_orig_ql > 0.0);
    BOOST_CHECK(mcPrice_orig_rr > 0.0);
    BOOST_CHECK(pricesMatch);
    BOOST_CHECK(std::abs(value(mcPrice_tape) - mcPrice_orig_rr) < 1e-10);
    BOOST_CHECK(std::abs(mcPrice_jit - mcPrice_orig_rr) < 1e-6);
}

//////////////////////////////////////////////////////////////////////////////
// Stage 4: Performance Benchmarks
// Same computation as Stage 3 but with timing measurements
//////////////////////////////////////////////////////////////////////////////

BOOST_AUTO_TEST_CASE(testStage4_Benchmarks)
{
    BOOST_TEST_MESSAGE("Testing Stage 4: Performance Benchmarks...");

    std::cout << "\n";
    std::cout << "=============================================================================\n";
    std::cout << "  STAGE 4: Performance Benchmarks\n";
    std::cout << "=============================================================================\n";
    std::cout << std::endl;

    // Benchmark parameters
    const Size warmupIterations = 5;
    const Size benchmarkIterations = 10;

    std::cout << "  Setup:\n";
    std::cout << "    - Warm-up iterations: " << warmupIterations << "\n";
    std::cout << "    - Benchmark iterations: " << benchmarkIterations << "\n";
    std::cout << "    - Market quotes: 4 deposits + 5 swaps = 9 inputs\n";
    std::cout << "    - Forward rates: 10\n";
    std::cout << "    - MC paths: 10\n";
    std::cout << std::endl;

    // =========================================================================
    // COMMON SETUP (same as Stage 3)
    // =========================================================================
    Calendar calendar = TARGET();
    Date todaysDate(4, September, 2005);
    Settings::instance().evaluationDate() = todaysDate;
    Integer fixingDays = 2;
    Date settlementDate = calendar.adjust(calendar.advance(todaysDate, fixingDays, Days));
    DayCounter dayCounter = Actual360();

    // Market quotes
    std::vector<Period> depoTenors = {1 * Days, 1 * Months, 3 * Months, 6 * Months};
    std::vector<double> depoRates_val = {0.0350, 0.0365, 0.0380, 0.0400};

    std::vector<Period> swapTenors = {1 * Years, 2 * Years, 3 * Years, 4 * Years, 5 * Years};
    std::vector<double> swapRates_val = {0.0420, 0.0480, 0.0520, 0.0550, 0.0575};

    Size numDeposits = depoTenors.size();
    Size numSwaps = swapTenors.size();
    Size numMarketQuotes = numDeposits + numSwaps;

    // LMM parameters
    Size size = 10;
    Size i_opt = 2;
    Size j_opt = 2;
    Size nrTrails = 10;
    Size steps = 8;

    // Build base curve for grid/timing setup
    std::vector<Rate> baseZeroRates = {0.0350, 0.0575};
    std::vector<Date> baseDates = {settlementDate, settlementDate + 6 * Years};
    auto baseIndex = makeIndex(baseDates, baseZeroRates);

    ext::shared_ptr<LiborForwardModelProcess> baseProcess(
        new LiborForwardModelProcess(size, baseIndex));
    ext::shared_ptr<LmCorrelationModel> baseCorrModel(
        new LmExponentialCorrelationModel(size, 0.5));
    ext::shared_ptr<LmVolatilityModel> baseVolaModel(
        new LmLinearExponentialVolatilityModel(baseProcess->fixingTimes(),
                                               0.291, 1.483, 0.116, 0.00001));
    baseProcess->setCovarParam(ext::shared_ptr<LfmCovarianceParameterization>(
        new LfmCovarianceProxy(baseVolaModel, baseCorrModel)));

    std::vector<Time> fixingTimes = baseProcess->fixingTimes();
    TimeGrid grid(fixingTimes.begin(), fixingTimes.end(), steps);

    std::vector<Size> location;
    for (Size idx = 0; idx < fixingTimes.size(); ++idx) {
        location.push_back(
            std::find(grid.begin(), grid.end(), fixingTimes[idx]) - grid.begin());
    }

    Size numFactors = baseProcess->factors();
    Size exerciseStep = location[i_opt];
    Size totalRandoms = numFactors * exerciseStep;
    Size fullGridRandoms = numFactors * (grid.size() - 1);  // Full grid for MultiPathGenerator

    // Get fair swap rate
    BusinessDayConvention convention = baseIndex->businessDayConvention();
    Date settlement = baseIndex->forwardingTermStructure()->referenceDate();
    Date fwdStart = settlement + Period(6 * i_opt, Months);
    Date fwdMaturity = fwdStart + Period(6 * j_opt, Months);

    Schedule schedule(fwdStart, fwdMaturity, baseIndex->tenor(), calendar,
                      convention, convention, DateGeneration::Forward, false);

    // Pre-generate random numbers for Re-Route approaches
    // Use same dimension as QuantLib MultiPathGenerator for RNG synchronization
    typedef PseudoRandom::rsg_type rsg_type;
    rsg_type rsg = PseudoRandom::make_sequence_generator(fullGridRandoms, BigNatural(42));

    std::vector<std::vector<double>> allRandoms(nrTrails);
    for (Size n = 0; n < nrTrails; ++n) {
        const auto& sequence = (n % 2) != 0U ? rsg.lastSequence() : rsg.nextSequence();
        // Store only up to exerciseStep (but generate full dimension to keep RNG in sync)
        allRandoms[n].resize(totalRandoms);
        for (Size m = 0; m < totalRandoms; ++m) {
            double rnd = (n % 2) != 0U ? -value(sequence.value[m]) : value(sequence.value[m]);
            allRandoms[n][m] = rnd;
        }
    }

    // Get accrual times
    std::vector<double> accrualStart(size), accrualEnd(size);
    for (Size k = 0; k < size; ++k) {
        accrualStart[k] = value(baseProcess->accrualStartTimes()[k]);
        accrualEnd[k] = value(baseProcess->accrualEndTimes()[k]);
    }

    // Helper: Build curve and get zero rates
    auto buildCurveAndProcess = [&](const std::vector<double>& depoRates,
                                    const std::vector<double>& swapRates,
                                    std::vector<Date>& curveDates_out,
                                    std::vector<Rate>& zeroRates_out) {
        RelinkableHandle<YieldTermStructure> euriborTS;
        auto euribor6m = ext::make_shared<Euribor6M>(euriborTS);
        euribor6m->addFixing(Date(2, September, 2005), 0.04);

        std::vector<ext::shared_ptr<RateHelper>> instruments;

        for (Size idx = 0; idx < numDeposits; ++idx) {
            auto depoQuote = ext::make_shared<SimpleQuote>(depoRates[idx]);
            instruments.push_back(ext::make_shared<DepositRateHelper>(
                Handle<Quote>(depoQuote), depoTenors[idx], fixingDays,
                calendar, ModifiedFollowing, true, dayCounter));
        }

        for (Size idx = 0; idx < numSwaps; ++idx) {
            auto swapQuote = ext::make_shared<SimpleQuote>(swapRates[idx]);
            instruments.push_back(ext::make_shared<SwapRateHelper>(
                Handle<Quote>(swapQuote), swapTenors[idx],
                calendar, Annual, Unadjusted, Thirty360(Thirty360::BondBasis),
                euribor6m));
        }

        auto yieldCurve = ext::make_shared<PiecewiseYieldCurve<ZeroYield, Linear>>(
            settlementDate, instruments, dayCounter);
        yieldCurve->enableExtrapolation();

        curveDates_out.clear();
        zeroRates_out.clear();
        curveDates_out.push_back(settlementDate);
        zeroRates_out.push_back(yieldCurve->zeroRate(settlementDate, dayCounter, Continuous).rate());
        Date endDate = settlementDate + 6 * Years;
        curveDates_out.push_back(endDate);
        zeroRates_out.push_back(yieldCurve->zeroRate(endDate, dayCounter, Continuous).rate());
    };

    // Pre-generate FULL random numbers for slow version (full grid)
    Size fullGridSteps = grid.size() - 1;
    rsg_type rsg_full = PseudoRandom::make_sequence_generator(fullGridRandoms, BigNatural(42));
    std::vector<std::vector<double>> allRandomsFull(nrTrails);
    for (Size n = 0; n < nrTrails; ++n) {
        const auto& sequence = (n % 2) != 0U ? rsg_full.lastSequence() : rsg_full.nextSequence();
        allRandomsFull[n].resize(fullGridRandoms);
        for (Size m = 0; m < fullGridRandoms; ++m) {
            double rnd = (n % 2) != 0U ? -value(sequence.value[m]) : value(sequence.value[m]);
            allRandomsFull[n][m] = rnd;
        }
    }

    // Helper: Compute MC price - SLOW version (full grid, like QuantLib)
    auto computeMCPriceSlow = [&](const std::vector<double>& depoRates,
                                  const std::vector<double>& swapRates) {
        std::vector<Date> curveDates;
        std::vector<Rate> zeroRates;
        buildCurveAndProcess(depoRates, swapRates, curveDates, zeroRates);

        RelinkableHandle<YieldTermStructure> termStructure;
        ext::shared_ptr<IborIndex> index(new Euribor6M(termStructure));
        index->addFixing(Date(2, September, 2005), 0.04);
        termStructure.linkTo(ext::make_shared<ZeroCurve>(curveDates, zeroRates, dayCounter));

        ext::shared_ptr<LiborForwardModelProcess> process(
            new LiborForwardModelProcess(size, index));
        ext::shared_ptr<LmCorrelationModel> corrModel(
            new LmExponentialCorrelationModel(size, 0.5));
        ext::shared_ptr<LmVolatilityModel> volaModel(
            new LmLinearExponentialVolatilityModel(process->fixingTimes(),
                                                   0.291, 1.483, 0.116, 0.00001));
        process->setCovarParam(ext::shared_ptr<LfmCovarianceParameterization>(
            new LfmCovarianceProxy(volaModel, corrModel)));

        ext::shared_ptr<VanillaSwap> fwdSwap(
            new VanillaSwap(Swap::Receiver, 1.0,
                            schedule, 0.05, dayCounter,
                            schedule, index, 0.0, index->dayCounter()));
        fwdSwap->setPricingEngine(ext::make_shared<DiscountingSwapEngine>(
            index->forwardingTermStructure()));
        double swapRate = value(fwdSwap->fairRate());

        Array initRates = process->initialValues();

        double price = 0.0;
        for (Size n = 0; n < nrTrails; ++n) {
            std::vector<double> asset(size);
            std::vector<double> assetAtExercise(size);  // Store rates at exercise
            for (Size k = 0; k < size; ++k) asset[k] = value(initRates[k]);

            // SLOW: Evolve through FULL grid (like MultiPathGenerator)
            for (Size step = 1; step <= fullGridSteps; ++step) {
                Size offset = (step - 1) * numFactors;
                Time t = grid[step - 1];
                Time dt = grid.dt(step - 1);

                Array dw(numFactors);
                for (Size f = 0; f < numFactors; ++f) {
                    dw[f] = allRandomsFull[n][offset + f];
                }

                Array asset_arr(size);
                for (Size k = 0; k < size; ++k) asset_arr[k] = asset[k];

                Array evolved = process->evolve(t, asset_arr, dt, dw);
                for (Size k = 0; k < size; ++k) asset[k] = value(evolved[k]);

                // Store rates at exercise step for payoff calculation
                if (step == exerciseStep) {
                    for (Size k = 0; k < size; ++k) assetAtExercise[k] = asset[k];
                }
            }

            // Use rates at EXERCISE time for payoff (not final rates)
            std::vector<double> dis(size);
            double df = 1.0;
            for (Size k = 0; k < size; ++k) {
                double accrual = accrualEnd[k] - accrualStart[k];
                df = df / (1.0 + assetAtExercise[k] * accrual);
                dis[k] = df;
            }

            double npv = 0.0;
            for (Size m = i_opt; m < i_opt + j_opt; ++m) {
                double accrual = accrualEnd[m] - accrualStart[m];
                npv += (swapRate - assetAtExercise[m]) * accrual * dis[m];
            }

            if (npv > 0.0) price += npv;
        }

        return price / static_cast<double>(nrTrails);
    };

    // =========================================================================
    // Timing storage
    // =========================================================================
    using Clock = std::chrono::high_resolution_clock;
    using Duration = std::chrono::duration<double, std::milli>;

    // Original (QuantLib) timings - MultiPathGenerator (price only)
    std::vector<double> orig_ql_total_times;

    // Original (Re-Route) timings - full grid (price only)
    std::vector<double> orig_rr_slow_total_times;

    // Bump & Reprice (QuantLib) timings - MultiPathGenerator
    std::vector<double> bump_ql_total_times;

    // Bump & Reprice (Re-Route) timings - full grid
    std::vector<double> bump_rr_slow_total_times;

    // XAD (QuantLib) timings - MultiPathGenerator with XAD
    std::vector<double> xad_ql_tape_setup_times;
    std::vector<double> xad_ql_bootstrap_times;
    std::vector<double> xad_ql_mc_times;
    std::vector<double> xad_ql_gradient_times;
    std::vector<double> xad_ql_total_times;

    // XAD (Re-Route) timings - full grid
    std::vector<double> xad_rr_slow_tape_setup_times;
    std::vector<double> xad_rr_slow_bootstrap_times;
    std::vector<double> xad_rr_slow_mc_times;
    std::vector<double> xad_rr_slow_gradient_times;
    std::vector<double> xad_rr_slow_total_times;

    // JIT (Re-Route) timings - full grid
    std::vector<double> jit_slow_tape_setup_times;
    std::vector<double> jit_slow_bootstrap_fwd_times;
    std::vector<double> jit_slow_bootstrap_bwd_times;
    std::vector<double> jit_slow_kernel_record_times;  // JIT graph recording
    std::vector<double> jit_slow_kernel_compile_times; // First forward (triggers compile)
    std::vector<double> jit_slow_set_inputs_times;
    std::vector<double> jit_slow_kernel_exec_times;    // Subsequent forwards (exec only)
    std::vector<double> jit_slow_get_outputs_times;
    std::vector<double> jit_slow_gradient_times;
    std::vector<double> jit_slow_chain_rule_times;
    std::vector<double> jit_slow_total_times;

    // JIT Opt (Re-Route with graph optimizations) timings
    std::vector<double> jit_fast_tape_setup_times;
    std::vector<double> jit_fast_bootstrap_fwd_times;
    std::vector<double> jit_fast_bootstrap_bwd_times;
    std::vector<double> jit_fast_kernel_record_times;
    std::vector<double> jit_fast_kernel_compile_times;
    std::vector<double> jit_fast_set_inputs_times;
    std::vector<double> jit_fast_kernel_exec_times;
    std::vector<double> jit_fast_get_outputs_times;
    std::vector<double> jit_fast_gradient_times;
    std::vector<double> jit_fast_chain_rule_times;
    std::vector<double> jit_fast_total_times;

    // JIT Interpreter (Re-Route) timings - for comparison
    std::vector<double> jit_interp_tape_setup_times;
    std::vector<double> jit_interp_bootstrap_fwd_times;
    std::vector<double> jit_interp_bootstrap_bwd_times;
    std::vector<double> jit_interp_kernel_record_times;
    std::vector<double> jit_interp_kernel_compile_times;
    std::vector<double> jit_interp_set_inputs_times;
    std::vector<double> jit_interp_kernel_exec_times;
    std::vector<double> jit_interp_get_outputs_times;
    std::vector<double> jit_interp_gradient_times;
    std::vector<double> jit_interp_chain_rule_times;
    std::vector<double> jit_interp_total_times;

    // JIT AVX (Re-Route) timings - full grid with AVX2 4-path batching
    std::vector<double> jit_avx_tape_setup_times;
    std::vector<double> jit_avx_bootstrap_fwd_times;
    std::vector<double> jit_avx_bootstrap_bwd_times;
    std::vector<double> jit_avx_kernel_record_times;
    std::vector<double> jit_avx_kernel_compile_times;
    std::vector<double> jit_avx_set_inputs_times;
    std::vector<double> jit_avx_kernel_exec_times;
    std::vector<double> jit_avx_get_outputs_times;
    std::vector<double> jit_avx_gradient_times;
    std::vector<double> jit_avx_chain_rule_times;
    std::vector<double> jit_avx_total_times;

    // Results storage (from last iteration for verification)
    double mcPrice_orig_ql = 0.0;
    double mcPrice_orig_rr_slow = 0.0;
    double mcPrice_bump_ql = 0.0;
    std::vector<double> dPrice_bump_ql_depo(numDeposits);
    std::vector<double> dPrice_bump_ql_swap(numSwaps);
    double mcPrice_bump_rr_slow = 0.0;
    std::vector<double> dPrice_bump_rr_slow_depo(numDeposits);
    std::vector<double> dPrice_bump_rr_slow_swap(numSwaps);

    double mcPrice_xad_ql = 0.0;
    std::vector<double> dPrice_xad_ql_depo(numDeposits);
    std::vector<double> dPrice_xad_ql_swap(numSwaps);

    double mcPrice_xad_rr_slow = 0.0;
    std::vector<double> dPrice_xad_rr_slow_depo(numDeposits);
    std::vector<double> dPrice_xad_rr_slow_swap(numSwaps);

    double mcPrice_jit_slow = 0.0;
    std::vector<double> dPrice_jit_slow_depo(numDeposits);
    std::vector<double> dPrice_jit_slow_swap(numSwaps);

    double mcPrice_jit_fast = 0.0;
    std::vector<double> dPrice_jit_fast_depo(numDeposits);
    std::vector<double> dPrice_jit_fast_swap(numSwaps);

    double mcPrice_jit_interp = 0.0;
    std::vector<double> dPrice_jit_interp_depo(numDeposits);
    std::vector<double> dPrice_jit_interp_swap(numSwaps);

    double mcPrice_jit_avx = 0.0;
    std::vector<double> dPrice_jit_avx_depo(numDeposits);
    std::vector<double> dPrice_jit_avx_swap(numSwaps);

    Size numIntermediates = size + 1;

    // =========================================================================
    // BENCHMARK LOOP
    // =========================================================================
    for (Size iter = 0; iter < warmupIterations + benchmarkIterations; ++iter) {
        bool recordTiming = (iter >= warmupIterations);
        bool isWarmup = (iter < warmupIterations);
        std::string phase = isWarmup ? "Warmup" : "Benchmark";
        Size phaseIter = isWarmup ? (iter + 1) : (iter - warmupIterations + 1);
        Size phaseTotal = isWarmup ? warmupIterations : benchmarkIterations;

        std::cout << "\n  [" << phase << " " << phaseIter << "/" << phaseTotal << "]" << std::flush;

        // =====================================================================
        // 0. ORIGINAL (QuantLib) - MultiPathGenerator (price only, no derivatives)
        // =====================================================================
        std::cout << " Original..." << std::flush;
        {
            auto t_total_start = Clock::now();

            // Build curve and process
            std::vector<Date> curveDates;
            std::vector<Rate> zeroRates;
            buildCurveAndProcess(depoRates_val, swapRates_val, curveDates, zeroRates);

            RelinkableHandle<YieldTermStructure> termStructure;
            ext::shared_ptr<IborIndex> index(new Euribor6M(termStructure));
            index->addFixing(Date(2, September, 2005), 0.04);
            termStructure.linkTo(ext::make_shared<ZeroCurve>(curveDates, zeroRates, dayCounter));

            ext::shared_ptr<LiborForwardModelProcess> process(
                new LiborForwardModelProcess(size, index));
            process->setCovarParam(ext::shared_ptr<LfmCovarianceParameterization>(
                new LfmCovarianceProxy(
                    ext::make_shared<LmLinearExponentialVolatilityModel>(process->fixingTimes(), 0.291, 1.483, 0.116, 0.00001),
                    ext::make_shared<LmExponentialCorrelationModel>(size, 0.5))));

            ext::shared_ptr<VanillaSwap> fwdSwap(
                new VanillaSwap(Swap::Receiver, 1.0,
                                schedule, 0.05, dayCounter,
                                schedule, index, 0.0, index->dayCounter()));
            fwdSwap->setPricingEngine(ext::make_shared<DiscountingSwapEngine>(
                index->forwardingTermStructure()));
            double swapRate = value(fwdSwap->fairRate());

            // MC with MultiPathGenerator
            rsg_type rsg_ql = PseudoRandom::make_sequence_generator(fullGridRandoms, BigNatural(42));
            typedef MultiPathGenerator<rsg_type>::sample_type sample_type;
            MultiPathGenerator<rsg_type> generator_ql(process, grid, rsg_ql, false);

            double price = 0.0;
            for (Size n = 0; n < nrTrails; ++n) {
                sample_type path = (n % 2) != 0U ? generator_ql.antithetic() : generator_ql.next();

                std::vector<double> mcRates(size);
                for (Size k = 0; k < size; ++k) {
                    mcRates[k] = value(path.value[k][location[i_opt]]);
                }

                std::vector<double> dis(size);
                double df = 1.0;
                for (Size k = 0; k < size; ++k) {
                    double accrual = accrualEnd[k] - accrualStart[k];
                    df = df / (1.0 + mcRates[k] * accrual);
                    dis[k] = df;
                }

                double npv = 0.0;
                for (Size m = i_opt; m < i_opt + j_opt; ++m) {
                    double accrual = accrualEnd[m] - accrualStart[m];
                    npv += (swapRate - mcRates[m]) * accrual * dis[m];
                }
                if (npv > 0.0) price += npv;
            }
            mcPrice_orig_ql = price / static_cast<double>(nrTrails);

            auto t_total_end = Clock::now();
            if (recordTiming) {
                orig_ql_total_times.push_back(Duration(t_total_end - t_total_start).count());
            }
        }

        // =====================================================================
        // 0b. ORIGINAL (Re-Route) - Full grid (price only, no derivatives)
        // =====================================================================
        {
            auto t_total_start = Clock::now();
            mcPrice_orig_rr_slow = computeMCPriceSlow(depoRates_val, swapRates_val);
            auto t_total_end = Clock::now();
            if (recordTiming) {
                orig_rr_slow_total_times.push_back(Duration(t_total_end - t_total_start).count());
            }
        }

        // =====================================================================
        // 1a. BUMP & REPRICE (QuantLib) - MultiPathGenerator
        // =====================================================================
        std::cout << " Bump..." << std::flush;
        {
            auto t_total_start = Clock::now();

            // Helper lambda for QuantLib MC pricing
            auto computeMCPriceQL = [&](const std::vector<double>& depoRates,
                                        const std::vector<double>& swapRates) {
                std::vector<Date> curveDates;
                std::vector<Rate> zeroRates;
                buildCurveAndProcess(depoRates, swapRates, curveDates, zeroRates);

                RelinkableHandle<YieldTermStructure> termStructure;
                ext::shared_ptr<IborIndex> index(new Euribor6M(termStructure));
                index->addFixing(Date(2, September, 2005), 0.04);
                termStructure.linkTo(ext::make_shared<ZeroCurve>(curveDates, zeroRates, dayCounter));

                ext::shared_ptr<LiborForwardModelProcess> process(
                    new LiborForwardModelProcess(size, index));
                process->setCovarParam(ext::shared_ptr<LfmCovarianceParameterization>(
                    new LfmCovarianceProxy(
                        ext::make_shared<LmLinearExponentialVolatilityModel>(process->fixingTimes(), 0.291, 1.483, 0.116, 0.00001),
                        ext::make_shared<LmExponentialCorrelationModel>(size, 0.5))));

                ext::shared_ptr<VanillaSwap> fwdSwap(
                    new VanillaSwap(Swap::Receiver, 1.0,
                                    schedule, 0.05, dayCounter,
                                    schedule, index, 0.0, index->dayCounter()));
                fwdSwap->setPricingEngine(ext::make_shared<DiscountingSwapEngine>(
                    index->forwardingTermStructure()));
                double swapRate = value(fwdSwap->fairRate());

                rsg_type rsg_tmp = PseudoRandom::make_sequence_generator(fullGridRandoms, BigNatural(42));
                typedef MultiPathGenerator<rsg_type>::sample_type sample_type;
                MultiPathGenerator<rsg_type> gen_tmp(process, grid, rsg_tmp, false);

                double price = 0.0;
                for (Size n = 0; n < nrTrails; ++n) {
                    sample_type path = (n % 2) != 0U ? gen_tmp.antithetic() : gen_tmp.next();

                    std::vector<double> mcRates(size);
                    for (Size k = 0; k < size; ++k) {
                        mcRates[k] = value(path.value[k][location[i_opt]]);
                    }

                    std::vector<double> dis(size);
                    double df = 1.0;
                    for (Size k = 0; k < size; ++k) {
                        double accrual = accrualEnd[k] - accrualStart[k];
                        df = df / (1.0 + mcRates[k] * accrual);
                        dis[k] = df;
                    }

                    double npv = 0.0;
                    for (Size m = i_opt; m < i_opt + j_opt; ++m) {
                        double accrual = accrualEnd[m] - accrualStart[m];
                        npv += (swapRate - mcRates[m]) * accrual * dis[m];
                    }
                    if (npv > 0.0) price += npv;
                }
                return price / static_cast<double>(nrTrails);
            };

            // Base price
            mcPrice_bump_ql = computeMCPriceQL(depoRates_val, swapRates_val);

            double bump = 1e-6;

            // Bump deposits
            for (Size idx = 0; idx < numDeposits; ++idx) {
                std::vector<double> bumpedDepoRates = depoRates_val;
                bumpedDepoRates[idx] += bump;
                double price_bumped = computeMCPriceQL(bumpedDepoRates, swapRates_val);
                dPrice_bump_ql_depo[idx] = (price_bumped - mcPrice_bump_ql) / bump;
            }

            // Bump swaps
            for (Size idx = 0; idx < numSwaps; ++idx) {
                std::vector<double> bumpedSwapRates = swapRates_val;
                bumpedSwapRates[idx] += bump;
                double price_bumped = computeMCPriceQL(depoRates_val, bumpedSwapRates);
                dPrice_bump_ql_swap[idx] = (price_bumped - mcPrice_bump_ql) / bump;
            }

            auto t_total_end = Clock::now();
            if (recordTiming) {
                bump_ql_total_times.push_back(Duration(t_total_end - t_total_start).count());
            }
        }

        // =====================================================================
        // 1b. BUMP & REPRICE (Re-Route) - Full grid
        // =====================================================================
        {
            auto t_total_start = Clock::now();

            mcPrice_bump_rr_slow = computeMCPriceSlow(depoRates_val, swapRates_val);
            double bump = 1e-6;

            // Bump deposits
            for (Size idx = 0; idx < numDeposits; ++idx) {
                std::vector<double> bumpedDepoRates = depoRates_val;
                bumpedDepoRates[idx] += bump;
                double price_bumped = computeMCPriceSlow(bumpedDepoRates, swapRates_val);
                dPrice_bump_rr_slow_depo[idx] = (price_bumped - mcPrice_bump_rr_slow) / bump;
            }

            // Bump swaps
            for (Size idx = 0; idx < numSwaps; ++idx) {
                std::vector<double> bumpedSwapRates = swapRates_val;
                bumpedSwapRates[idx] += bump;
                double price_bumped = computeMCPriceSlow(depoRates_val, bumpedSwapRates);
                dPrice_bump_rr_slow_swap[idx] = (price_bumped - mcPrice_bump_rr_slow) / bump;
            }

            auto t_total_end = Clock::now();
            if (recordTiming) {
                bump_rr_slow_total_times.push_back(Duration(t_total_end - t_total_start).count());
            }
        }

        // =====================================================================
        // 2. XAD (QuantLib) - MultiPathGenerator with XAD tape
        // =====================================================================
        {
            auto t_total_start = Clock::now();

            // Tape setup
            auto t_tape_setup_start = Clock::now();

            using tape_type = Real::tape_type;
            tape_type tape_ql;

            std::vector<Real> depositRates_ql(numDeposits);
            std::vector<Real> swapRates_ql(numSwaps);
            for (Size idx = 0; idx < numDeposits; ++idx) depositRates_ql[idx] = depoRates_val[idx];
            for (Size idx = 0; idx < numSwaps; ++idx) swapRates_ql[idx] = swapRates_val[idx];

            tape_ql.registerInputs(depositRates_ql);
            tape_ql.registerInputs(swapRates_ql);
            tape_ql.newRecording();

            auto t_tape_setup_end = Clock::now();

            // Bootstrap pass
            auto t_bootstrap_start = Clock::now();

            RelinkableHandle<YieldTermStructure> euriborTS_ql;
            auto euribor6m_ql = ext::make_shared<Euribor6M>(euriborTS_ql);
            euribor6m_ql->addFixing(Date(2, September, 2005), 0.04);

            std::vector<ext::shared_ptr<RateHelper>> instruments_ql;
            for (Size idx = 0; idx < numDeposits; ++idx) {
                auto depoQuote = ext::make_shared<SimpleQuote>(depositRates_ql[idx]);
                instruments_ql.push_back(ext::make_shared<DepositRateHelper>(
                    Handle<Quote>(depoQuote), depoTenors[idx], fixingDays,
                    calendar, ModifiedFollowing, true, dayCounter));
            }
            for (Size idx = 0; idx < numSwaps; ++idx) {
                auto swapQuote = ext::make_shared<SimpleQuote>(swapRates_ql[idx]);
                instruments_ql.push_back(ext::make_shared<SwapRateHelper>(
                    Handle<Quote>(swapQuote), swapTenors[idx],
                    calendar, Annual, Unadjusted, Thirty360(Thirty360::BondBasis), euribor6m_ql));
            }

            auto yieldCurve_ql = ext::make_shared<PiecewiseYieldCurve<ZeroYield, Linear>>(
                settlementDate, instruments_ql, dayCounter);
            yieldCurve_ql->enableExtrapolation();

            std::vector<Date> curveDates_ql;
            std::vector<Real> zeroRates_ql;
            curveDates_ql.push_back(settlementDate);
            zeroRates_ql.push_back(yieldCurve_ql->zeroRate(settlementDate, dayCounter, Continuous).rate());
            Date endDate_ql = settlementDate + 6 * Years;
            curveDates_ql.push_back(endDate_ql);
            zeroRates_ql.push_back(yieldCurve_ql->zeroRate(endDate_ql, dayCounter, Continuous).rate());

            std::vector<Rate> zeroRates_ql_d;
            for (const auto& r : zeroRates_ql) zeroRates_ql_d.push_back(r);

            RelinkableHandle<YieldTermStructure> termStructure_ql;
            ext::shared_ptr<IborIndex> index_ql(new Euribor6M(termStructure_ql));
            index_ql->addFixing(Date(2, September, 2005), 0.04);
            termStructure_ql.linkTo(ext::make_shared<ZeroCurve>(curveDates_ql, zeroRates_ql_d, dayCounter));

            ext::shared_ptr<LiborForwardModelProcess> process_ql(
                new LiborForwardModelProcess(size, index_ql));
            process_ql->setCovarParam(ext::shared_ptr<LfmCovarianceParameterization>(
                new LfmCovarianceProxy(
                    ext::make_shared<LmLinearExponentialVolatilityModel>(process_ql->fixingTimes(), 0.291, 1.483, 0.116, 0.00001),
                    ext::make_shared<LmExponentialCorrelationModel>(size, 0.5))));

            ext::shared_ptr<VanillaSwap> fwdSwap_ql(
                new VanillaSwap(Swap::Receiver, 1.0,
                                schedule, 0.05, dayCounter,
                                schedule, index_ql, 0.0, index_ql->dayCounter()));
            fwdSwap_ql->setPricingEngine(ext::make_shared<DiscountingSwapEngine>(
                index_ql->forwardingTermStructure()));
            Real swapRate_ql = fwdSwap_ql->fairRate();

            auto t_bootstrap_end = Clock::now();

            // MC with MultiPathGenerator using XAD types
            auto t_mc_start = Clock::now();

            rsg_type rsg_xad_ql = PseudoRandom::make_sequence_generator(fullGridRandoms, BigNatural(42));
            typedef MultiPathGenerator<rsg_type>::sample_type sample_type;
            MultiPathGenerator<rsg_type> generator_xad_ql(process_ql, grid, rsg_xad_ql, false);

            Real mcPrice_xad_ql_local = 0.0;
            for (Size n = 0; n < nrTrails; ++n) {
                sample_type path = (n % 2) != 0U ? generator_xad_ql.antithetic() : generator_xad_ql.next();

                std::vector<Real> mcRates(size);
                for (Size k = 0; k < size; ++k) {
                    mcRates[k] = path.value[k][location[i_opt]];
                }

                std::vector<Real> dis(size);
                Real df = 1.0;
                for (Size k = 0; k < size; ++k) {
                    Real accrual = process_ql->accrualEndTimes()[k] - process_ql->accrualStartTimes()[k];
                    df = df / (Real(1.0) + mcRates[k] * accrual);
                    dis[k] = df;
                }

                Real npv = 0.0;
                for (Size m = i_opt; m < i_opt + j_opt; ++m) {
                    Real accrual = process_ql->accrualEndTimes()[m] - process_ql->accrualStartTimes()[m];
                    npv = npv + (swapRate_ql - mcRates[m]) * accrual * dis[m];
                }
                if (value(npv) > 0.0) mcPrice_xad_ql_local += npv;
            }
            mcPrice_xad_ql_local /= static_cast<Real>(nrTrails);

            auto t_mc_end = Clock::now();

            // Gradient pass
            auto t_grad_start = Clock::now();
            tape_ql.registerOutput(mcPrice_xad_ql_local);
            derivative(mcPrice_xad_ql_local) = 1.0;
            tape_ql.computeAdjoints();

            for (Size idx = 0; idx < numDeposits; ++idx) dPrice_xad_ql_depo[idx] = derivative(depositRates_ql[idx]);
            for (Size idx = 0; idx < numSwaps; ++idx) dPrice_xad_ql_swap[idx] = derivative(swapRates_ql[idx]);
            auto t_grad_end = Clock::now();

            mcPrice_xad_ql = value(mcPrice_xad_ql_local);
            tape_ql.deactivate();

            auto t_total_end = Clock::now();

            if (recordTiming) {
                xad_ql_tape_setup_times.push_back(Duration(t_tape_setup_end - t_tape_setup_start).count());
                xad_ql_bootstrap_times.push_back(Duration(t_bootstrap_end - t_bootstrap_start).count());
                xad_ql_mc_times.push_back(Duration(t_mc_end - t_mc_start).count());
                xad_ql_gradient_times.push_back(Duration(t_grad_end - t_grad_start).count());
                xad_ql_total_times.push_back(Duration(t_total_end - t_total_start).count());
            }
        }

        // =====================================================================
        // 3a. XAD (Re-Route) - Full grid with XAD tape
        // =====================================================================
        std::cout << " XAD..." << std::flush;
        {
            auto t_total_start = Clock::now();

            // Tape setup
            auto t_tape_setup_start = Clock::now();

            using tape_type = Real::tape_type;
            tape_type tape;

            std::vector<Real> depositRates(numDeposits);
            std::vector<Real> swapRates(numSwaps);
            for (Size idx = 0; idx < numDeposits; ++idx) depositRates[idx] = depoRates_val[idx];
            for (Size idx = 0; idx < numSwaps; ++idx) swapRates[idx] = swapRates_val[idx];

            tape.registerInputs(depositRates);
            tape.registerInputs(swapRates);
            tape.newRecording();

            auto t_tape_setup_end = Clock::now();

            // Bootstrap pass
            auto t_bootstrap_start = Clock::now();

            RelinkableHandle<YieldTermStructure> euriborTS;
            auto euribor6m = ext::make_shared<Euribor6M>(euriborTS);
            euribor6m->addFixing(Date(2, September, 2005), 0.04);

            std::vector<ext::shared_ptr<RateHelper>> instruments;
            for (Size idx = 0; idx < numDeposits; ++idx) {
                auto depoQuote = ext::make_shared<SimpleQuote>(depositRates[idx]);
                instruments.push_back(ext::make_shared<DepositRateHelper>(
                    Handle<Quote>(depoQuote), depoTenors[idx], fixingDays,
                    calendar, ModifiedFollowing, true, dayCounter));
            }
            for (Size idx = 0; idx < numSwaps; ++idx) {
                auto swapQuote = ext::make_shared<SimpleQuote>(swapRates[idx]);
                instruments.push_back(ext::make_shared<SwapRateHelper>(
                    Handle<Quote>(swapQuote), swapTenors[idx],
                    calendar, Annual, Unadjusted, Thirty360(Thirty360::BondBasis),
                    euribor6m));
            }

            auto yieldCurve = ext::make_shared<PiecewiseYieldCurve<ZeroYield, Linear>>(
                settlementDate, instruments, dayCounter);
            yieldCurve->enableExtrapolation();

            std::vector<Date> curveDates;
            std::vector<Real> zeroRates;
            curveDates.push_back(settlementDate);
            zeroRates.push_back(yieldCurve->zeroRate(settlementDate, dayCounter, Continuous).rate());
            Date endDate = settlementDate + 6 * Years;
            curveDates.push_back(endDate);
            zeroRates.push_back(yieldCurve->zeroRate(endDate, dayCounter, Continuous).rate());

            std::vector<Rate> zeroRates_ql;
            for (const auto& r : zeroRates) zeroRates_ql.push_back(r);

            RelinkableHandle<YieldTermStructure> termStructure;
            ext::shared_ptr<IborIndex> index(new Euribor6M(termStructure));
            index->addFixing(Date(2, September, 2005), 0.04);
            termStructure.linkTo(ext::make_shared<ZeroCurve>(curveDates, zeroRates_ql, dayCounter));

            ext::shared_ptr<LiborForwardModelProcess> process(
                new LiborForwardModelProcess(size, index));
            ext::shared_ptr<LmCorrelationModel> corrModel(
                new LmExponentialCorrelationModel(size, 0.5));
            ext::shared_ptr<LmVolatilityModel> volaModel(
                new LmLinearExponentialVolatilityModel(process->fixingTimes(),
                                                       0.291, 1.483, 0.116, 0.00001));
            process->setCovarParam(ext::shared_ptr<LfmCovarianceParameterization>(
                new LfmCovarianceProxy(volaModel, corrModel)));

            ext::shared_ptr<VanillaSwap> fwdSwap_tape(
                new VanillaSwap(Swap::Receiver, 1.0,
                                schedule, 0.05, dayCounter,
                                schedule, index, 0.0, index->dayCounter()));
            fwdSwap_tape->setPricingEngine(ext::make_shared<DiscountingSwapEngine>(
                index->forwardingTermStructure()));
            Real swapRate_tape = fwdSwap_tape->fairRate();

            Array initRates_tape = process->initialValues();

            auto t_bootstrap_end = Clock::now();

            // MC pass - SLOW: full grid
            auto t_mc_start = Clock::now();

            Real mcPrice_tape = 0.0;
            for (Size n = 0; n < nrTrails; ++n) {
                std::vector<Real> asset(size);
                std::vector<Real> assetAtExercise(size);
                for (Size k = 0; k < size; ++k) asset[k] = initRates_tape[k];

                // SLOW: Evolve through FULL grid
                for (Size step = 1; step <= fullGridSteps; ++step) {
                    Size offset = (step - 1) * numFactors;
                    Time t = grid[step - 1];
                    Time dt = grid.dt(step - 1);

                    Array dw(numFactors);
                    for (Size f = 0; f < numFactors; ++f) {
                        dw[f] = allRandomsFull[n][offset + f];
                    }

                    Array asset_arr(size);
                    for (Size k = 0; k < size; ++k) asset_arr[k] = asset[k];

                    Array evolved = process->evolve(t, asset_arr, dt, dw);
                    for (Size k = 0; k < size; ++k) asset[k] = evolved[k];

                    // Store rates at exercise step
                    if (step == exerciseStep) {
                        for (Size k = 0; k < size; ++k) assetAtExercise[k] = asset[k];
                    }
                }

                // Use rates at EXERCISE time for payoff
                std::vector<Real> dis(size);
                Real df = 1.0;
                for (Size k = 0; k < size; ++k) {
                    double accrual = accrualEnd[k] - accrualStart[k];
                    df = df / (Real(1.0) + assetAtExercise[k] * accrual);
                    dis[k] = df;
                }

                Real npv = 0.0;
                for (Size m = i_opt; m < i_opt + j_opt; ++m) {
                    double accrual = accrualEnd[m] - accrualStart[m];
                    npv = npv + (swapRate_tape - assetAtExercise[m]) * accrual * dis[m];
                }

                if (value(npv) > 0.0) mcPrice_tape += npv;
            }
            mcPrice_tape /= static_cast<Real>(nrTrails);

            auto t_mc_end = Clock::now();

            // Gradient pass
            auto t_grad_start = Clock::now();
            tape.registerOutput(mcPrice_tape);
            derivative(mcPrice_tape) = 1.0;
            tape.computeAdjoints();

            for (Size idx = 0; idx < numDeposits; ++idx) dPrice_xad_rr_slow_depo[idx] = derivative(depositRates[idx]);
            for (Size idx = 0; idx < numSwaps; ++idx) dPrice_xad_rr_slow_swap[idx] = derivative(swapRates[idx]);
            auto t_grad_end = Clock::now();

            mcPrice_xad_rr_slow = value(mcPrice_tape);
            tape.deactivate();

            auto t_total_end = Clock::now();

            if (recordTiming) {
                xad_rr_slow_tape_setup_times.push_back(Duration(t_tape_setup_end - t_tape_setup_start).count());
                xad_rr_slow_bootstrap_times.push_back(Duration(t_bootstrap_end - t_bootstrap_start).count());
                xad_rr_slow_mc_times.push_back(Duration(t_mc_end - t_mc_start).count());
                xad_rr_slow_gradient_times.push_back(Duration(t_grad_end - t_grad_start).count());
                xad_rr_slow_total_times.push_back(Duration(t_total_end - t_total_start).count());
            }
        }

        // =====================================================================
        // 4a. JIT (Re-Route) - Full grid with JIT
        // =====================================================================
        std::cout << " JIT..." << std::flush;
        {
            auto t_total_start = Clock::now();

            // --- Tape setup ---
            auto t_tape_setup_start = Clock::now();

            using tape_type = Real::tape_type;
            tape_type tape2;

            std::vector<Real> depositRates2(numDeposits);
            std::vector<Real> swapRates2(numSwaps);
            for (Size idx = 0; idx < numDeposits; ++idx) depositRates2[idx] = depoRates_val[idx];
            for (Size idx = 0; idx < numSwaps; ++idx) swapRates2[idx] = swapRates_val[idx];

            tape2.registerInputs(depositRates2);
            tape2.registerInputs(swapRates2);
            tape2.newRecording();

            auto t_tape_setup_end = Clock::now();

            // --- Bootstrap forward pass ---
            auto t_boot_fwd_start = Clock::now();

            RelinkableHandle<YieldTermStructure> euriborTS2;
            auto euribor6m2 = ext::make_shared<Euribor6M>(euriborTS2);
            euribor6m2->addFixing(Date(2, September, 2005), 0.04);

            std::vector<ext::shared_ptr<RateHelper>> instruments2;
            for (Size idx = 0; idx < numDeposits; ++idx) {
                auto depoQuote2 = ext::make_shared<SimpleQuote>(depositRates2[idx]);
                instruments2.push_back(ext::make_shared<DepositRateHelper>(
                    Handle<Quote>(depoQuote2), depoTenors[idx], fixingDays,
                    calendar, ModifiedFollowing, true, dayCounter));
            }
            for (Size idx = 0; idx < numSwaps; ++idx) {
                auto swapQuote = ext::make_shared<SimpleQuote>(swapRates2[idx]);
                instruments2.push_back(ext::make_shared<SwapRateHelper>(
                    Handle<Quote>(swapQuote), swapTenors[idx],
                    calendar, Annual, Unadjusted, Thirty360(Thirty360::BondBasis),
                    euribor6m2));
            }

            auto yieldCurve2 = ext::make_shared<PiecewiseYieldCurve<ZeroYield, Linear>>(
                settlementDate, instruments2, dayCounter);
            yieldCurve2->enableExtrapolation();

            std::vector<Date> curveDates2;
            std::vector<Real> zeroRates2;
            curveDates2.push_back(settlementDate);
            zeroRates2.push_back(yieldCurve2->zeroRate(settlementDate, dayCounter, Continuous).rate());
            Date endDate2 = settlementDate + 6 * Years;
            curveDates2.push_back(endDate2);
            zeroRates2.push_back(yieldCurve2->zeroRate(endDate2, dayCounter, Continuous).rate());

            std::vector<Rate> zeroRates2_ql;
            for (const auto& r : zeroRates2) zeroRates2_ql.push_back(r);

            RelinkableHandle<YieldTermStructure> termStructure2;
            ext::shared_ptr<IborIndex> index2(new Euribor6M(termStructure2));
            index2->addFixing(Date(2, September, 2005), 0.04);
            termStructure2.linkTo(ext::make_shared<ZeroCurve>(curveDates2, zeroRates2_ql, dayCounter));

            ext::shared_ptr<LiborForwardModelProcess> process2(
                new LiborForwardModelProcess(size, index2));
            ext::shared_ptr<LmCorrelationModel> corrModel2(
                new LmExponentialCorrelationModel(size, 0.5));
            ext::shared_ptr<LmVolatilityModel> volaModel2(
                new LmLinearExponentialVolatilityModel(process2->fixingTimes(),
                                                       0.291, 1.483, 0.116, 0.00001));
            process2->setCovarParam(ext::shared_ptr<LfmCovarianceParameterization>(
                new LfmCovarianceProxy(volaModel2, corrModel2)));

            ext::shared_ptr<VanillaSwap> fwdSwap2(
                new VanillaSwap(Swap::Receiver, 1.0,
                                schedule, 0.05, dayCounter,
                                schedule, index2, 0.0, index2->dayCounter()));
            fwdSwap2->setPricingEngine(ext::make_shared<DiscountingSwapEngine>(
                index2->forwardingTermStructure()));
            Real swapRate2_tape = fwdSwap2->fairRate();

            Array initRates2 = process2->initialValues();

            auto t_boot_fwd_end = Clock::now();

            // --- Bootstrap backward pass (Jacobian) ---
            auto t_boot_bwd_start = Clock::now();

            std::vector<double> jacobian(numIntermediates * numMarketQuotes, 0.0);

            for (Size k = 0; k < size; ++k) {
                if (initRates2[k].shouldRecord()) {
                    tape2.clearDerivatives();
                    tape2.registerOutput(initRates2[k]);
                    derivative(initRates2[k]) = 1.0;
                    tape2.computeAdjoints();

                    double* jac_row = jacobian.data() + k * numMarketQuotes;
                    for (Size m = 0; m < numDeposits; ++m)
                        jac_row[m] = derivative(depositRates2[m]);
                    for (Size m = 0; m < numSwaps; ++m)
                        jac_row[numDeposits + m] = derivative(swapRates2[m]);
                }
            }

            if (swapRate2_tape.shouldRecord()) {
                tape2.clearDerivatives();
                tape2.registerOutput(swapRate2_tape);
                derivative(swapRate2_tape) = 1.0;
                tape2.computeAdjoints();

                double* jac_row = jacobian.data() + size * numMarketQuotes;
                for (Size m = 0; m < numDeposits; ++m)
                    jac_row[m] = derivative(depositRates2[m]);
                for (Size m = 0; m < numSwaps; ++m)
                    jac_row[numDeposits + m] = derivative(swapRates2[m]);
            }

            tape2.deactivate();

            auto t_boot_bwd_end = Clock::now();

            // --- JIT kernel recording (build JIT graph) ---
            auto t_kernel_record_start = Clock::now();

            // Use ForgeBackend without graph optimizations
            auto forgeBackendSlow = std::make_unique<xad::forge::ForgeBackend<double>>(false);
            xad::JITCompiler<double> jit(std::move(forgeBackendSlow));

            std::vector<xad::AD> jit_initRates(size);
            xad::AD jit_swapRate;
            std::vector<xad::AD> jit_randoms(fullGridRandoms);  // Full grid randoms

            for (Size k = 0; k < size; ++k) {
                jit_initRates[k] = xad::AD(value(initRates2[k]));
                jit.registerInput(jit_initRates[k]);
            }

            jit_swapRate = xad::AD(value(swapRate2_tape));
            jit.registerInput(jit_swapRate);

            for (Size m = 0; m < fullGridRandoms; ++m) {
                jit_randoms[m] = xad::AD(0.0);
                jit.registerInput(jit_randoms[m]);
            }

            jit.newRecording();

            // Record path evolution - SLOW: full grid
            std::vector<xad::AD> asset_jit(size);
            std::vector<xad::AD> assetAtExercise_jit(size);
            for (Size k = 0; k < size; ++k) {
                asset_jit[k] = jit_initRates[k];
            }

            for (Size step = 1; step <= fullGridSteps; ++step) {
                Size offset = (step - 1) * numFactors;
                Time t = grid[step - 1];
                Time dt = grid.dt(step - 1);

                Array dw(numFactors);
                for (Size f = 0; f < numFactors; ++f) {
                    dw[f] = jit_randoms[offset + f];
                }

                Array asset_arr(size);
                for (Size k = 0; k < size; ++k) asset_arr[k] = asset_jit[k];

                Array evolved = process2->evolve(t, asset_arr, dt, dw);
                for (Size k = 0; k < size; ++k) asset_jit[k] = evolved[k];

                // Store rates at exercise step
                if (step == exerciseStep) {
                    for (Size k = 0; k < size; ++k) assetAtExercise_jit[k] = asset_jit[k];
                }
            }

            // Use rates at EXERCISE time for payoff
            std::vector<xad::AD> dis_jit(size);
            xad::AD df_jit = xad::AD(1.0);
            for (Size k = 0; k < size; ++k) {
                double accrual = accrualEnd[k] - accrualStart[k];
                df_jit = df_jit / (xad::AD(1.0) + assetAtExercise_jit[k] * accrual);
                dis_jit[k] = df_jit;
            }

            xad::AD jit_npv = xad::AD(0.0);
            for (Size m = i_opt; m < i_opt + j_opt; ++m) {
                double accrual = accrualEnd[m] - accrualStart[m];
                jit_npv = jit_npv + (jit_swapRate - assetAtExercise_jit[m]) * accrual * dis_jit[m];
            }

            xad::AD jit_payoff = xad::less(jit_npv, xad::AD(0.0)).If(xad::AD(0.0), jit_npv);
            jit.registerOutput(jit_payoff);

            auto t_kernel_record_end = Clock::now();

            // --- JIT kernel compile ---
            auto t_kernel_compile_start = Clock::now();

            jit.compile();

            auto t_kernel_compile_end = Clock::now();

            // --- JIT kernel execution (MC loop) with granular timing ---
            const auto& graph = jit.getGraph();
            uint32_t outputSlot = graph.output_ids[0];

            double mcPrice_jit_local = 0.0;
            std::vector<double> dPrice_dInitRates(size, 0.0);
            double dPrice_dSwapRate_jit = 0.0;

            double set_inputs_time = 0.0;
            double kernel_exec_time = 0.0;
            double get_outputs_time = 0.0;
            double get_gradients_time = 0.0;

            for (Size n = 0; n < nrTrails; ++n) {
                // Set inputs timing
                auto t_set_start = Clock::now();
                for (Size k = 0; k < size; ++k) {
                    value(jit_initRates[k]) = value(initRates2[k]);
                }
                value(jit_swapRate) = value(swapRate2_tape);
                for (Size m = 0; m < fullGridRandoms; ++m) {
                    value(jit_randoms[m]) = allRandomsFull[n][m];
                }
                auto t_set_end = Clock::now();
                set_inputs_time += Duration(t_set_end - t_set_start).count();

                // Kernel exec timing
                auto t_exec_start = Clock::now();
                double payoff_value;
                jit.forward(&payoff_value);
                auto t_exec_end = Clock::now();
                kernel_exec_time += Duration(t_exec_end - t_exec_start).count();

                // Get outputs timing (trivial - just accumulate)
                auto t_get_out_start = Clock::now();
                mcPrice_jit_local += payoff_value;
                auto t_get_out_end = Clock::now();
                get_outputs_time += Duration(t_get_out_end - t_get_out_start).count();

                // Get gradients timing
                auto t_grad_mc_start = Clock::now();
                jit.clearDerivatives();
                jit.setDerivative(outputSlot, 1.0);
                jit.computeAdjoints();
                for (Size k = 0; k < size; ++k) {
                    dPrice_dInitRates[k] += jit.derivative(graph.input_ids[k]);
                }
                dPrice_dSwapRate_jit += jit.derivative(graph.input_ids[size]);
                auto t_grad_mc_end = Clock::now();
                get_gradients_time += Duration(t_grad_mc_end - t_grad_mc_start).count();
            }

            // --- Gradient (chain rule) ---
            auto t_grad_start = Clock::now();

            mcPrice_jit_local /= static_cast<double>(nrTrails);
            for (Size k = 0; k < size; ++k) {
                dPrice_dInitRates[k] /= static_cast<double>(nrTrails);
            }
            dPrice_dSwapRate_jit /= static_cast<double>(nrTrails);

            std::vector<double> dPrice_dIntermediates(numIntermediates);
            for (Size k = 0; k < size; ++k)
                dPrice_dIntermediates[k] = dPrice_dInitRates[k];
            dPrice_dIntermediates[size] = dPrice_dSwapRate_jit;

            std::vector<double> dPrice_jit_market(numMarketQuotes);
            applyChainRule(jacobian.data(), dPrice_dIntermediates.data(), dPrice_jit_market.data(),
                           numIntermediates, numMarketQuotes);

            for (Size idx = 0; idx < numDeposits; ++idx) dPrice_jit_slow_depo[idx] = dPrice_jit_market[idx];
            for (Size idx = 0; idx < numSwaps; ++idx) dPrice_jit_slow_swap[idx] = dPrice_jit_market[numDeposits + idx];

            mcPrice_jit_slow = mcPrice_jit_local;

            auto t_grad_end = Clock::now();
            auto t_total_end = Clock::now();

            if (recordTiming) {
                jit_slow_tape_setup_times.push_back(Duration(t_tape_setup_end - t_tape_setup_start).count());
                jit_slow_bootstrap_fwd_times.push_back(Duration(t_boot_fwd_end - t_boot_fwd_start).count());
                jit_slow_bootstrap_bwd_times.push_back(Duration(t_boot_bwd_end - t_boot_bwd_start).count());
                jit_slow_kernel_record_times.push_back(Duration(t_kernel_record_end - t_kernel_record_start).count());
                jit_slow_kernel_compile_times.push_back(Duration(t_kernel_compile_end - t_kernel_compile_start).count());
                jit_slow_set_inputs_times.push_back(set_inputs_time);
                jit_slow_kernel_exec_times.push_back(kernel_exec_time);
                jit_slow_get_outputs_times.push_back(get_outputs_time);
                jit_slow_gradient_times.push_back(get_gradients_time);
                jit_slow_chain_rule_times.push_back(Duration(t_grad_end - t_grad_start).count());
                jit_slow_total_times.push_back(Duration(t_total_end - t_total_start).count());
            }
        }

        // =====================================================================
        // 4b. JIT Opt (Re-Route) - Full grid with graph optimizations
        // =====================================================================
        std::cout << " JIT-Opt..." << std::flush;
        {
            auto t_total_start = Clock::now();

            // --- Tape setup ---
            auto t_tape_setup_start = Clock::now();

            using tape_type = Real::tape_type;
            tape_type tape2;

            std::vector<Real> depositRates2(numDeposits);
            std::vector<Real> swapRates2(numSwaps);
            for (Size idx = 0; idx < numDeposits; ++idx) depositRates2[idx] = depoRates_val[idx];
            for (Size idx = 0; idx < numSwaps; ++idx) swapRates2[idx] = swapRates_val[idx];

            tape2.registerInputs(depositRates2);
            tape2.registerInputs(swapRates2);
            tape2.newRecording();

            auto t_tape_setup_end = Clock::now();

            // --- Bootstrap forward pass ---
            auto t_boot_fwd_start = Clock::now();

            RelinkableHandle<YieldTermStructure> euriborTS2;
            auto euribor6m2 = ext::make_shared<Euribor6M>(euriborTS2);
            euribor6m2->addFixing(Date(2, September, 2005), 0.04);

            std::vector<ext::shared_ptr<RateHelper>> instruments2;
            for (Size idx = 0; idx < numDeposits; ++idx) {
                auto depoQuote2 = ext::make_shared<SimpleQuote>(depositRates2[idx]);
                instruments2.push_back(ext::make_shared<DepositRateHelper>(
                    Handle<Quote>(depoQuote2), depoTenors[idx], fixingDays,
                    calendar, ModifiedFollowing, true, dayCounter));
            }
            for (Size idx = 0; idx < numSwaps; ++idx) {
                auto swapQuote = ext::make_shared<SimpleQuote>(swapRates2[idx]);
                instruments2.push_back(ext::make_shared<SwapRateHelper>(
                    Handle<Quote>(swapQuote), swapTenors[idx],
                    calendar, Annual, Unadjusted, Thirty360(Thirty360::BondBasis),
                    euribor6m2));
            }

            auto yieldCurve2 = ext::make_shared<PiecewiseYieldCurve<ZeroYield, Linear>>(
                settlementDate, instruments2, dayCounter);
            yieldCurve2->enableExtrapolation();

            std::vector<Date> curveDates2;
            std::vector<Real> zeroRates2;
            curveDates2.push_back(settlementDate);
            zeroRates2.push_back(yieldCurve2->zeroRate(settlementDate, dayCounter, Continuous).rate());
            Date endDate2 = settlementDate + 6 * Years;
            curveDates2.push_back(endDate2);
            zeroRates2.push_back(yieldCurve2->zeroRate(endDate2, dayCounter, Continuous).rate());

            std::vector<Rate> zeroRates2_ql;
            for (const auto& r : zeroRates2) zeroRates2_ql.push_back(r);

            RelinkableHandle<YieldTermStructure> termStructure2;
            ext::shared_ptr<IborIndex> index2(new Euribor6M(termStructure2));
            index2->addFixing(Date(2, September, 2005), 0.04);
            termStructure2.linkTo(ext::make_shared<ZeroCurve>(curveDates2, zeroRates2_ql, dayCounter));

            ext::shared_ptr<LiborForwardModelProcess> process2(
                new LiborForwardModelProcess(size, index2));
            ext::shared_ptr<LmCorrelationModel> corrModel2(
                new LmExponentialCorrelationModel(size, 0.5));
            ext::shared_ptr<LmVolatilityModel> volaModel2(
                new LmLinearExponentialVolatilityModel(process2->fixingTimes(),
                                                       0.291, 1.483, 0.116, 0.00001));
            process2->setCovarParam(ext::shared_ptr<LfmCovarianceParameterization>(
                new LfmCovarianceProxy(volaModel2, corrModel2)));

            ext::shared_ptr<VanillaSwap> fwdSwap2(
                new VanillaSwap(Swap::Receiver, 1.0,
                                schedule, 0.05, dayCounter,
                                schedule, index2, 0.0, index2->dayCounter()));
            fwdSwap2->setPricingEngine(ext::make_shared<DiscountingSwapEngine>(
                index2->forwardingTermStructure()));
            Real swapRate2_tape = fwdSwap2->fairRate();

            Array initRates2 = process2->initialValues();

            auto t_boot_fwd_end = Clock::now();

            // --- Bootstrap backward pass (Jacobian) ---
            auto t_boot_bwd_start = Clock::now();

            std::vector<double> jacobian(numIntermediates * numMarketQuotes, 0.0);

            for (Size k = 0; k < size; ++k) {
                if (initRates2[k].shouldRecord()) {
                    tape2.clearDerivatives();
                    tape2.registerOutput(initRates2[k]);
                    derivative(initRates2[k]) = 1.0;
                    tape2.computeAdjoints();

                    double* jac_row = jacobian.data() + k * numMarketQuotes;
                    for (Size m = 0; m < numDeposits; ++m)
                        jac_row[m] = derivative(depositRates2[m]);
                    for (Size m = 0; m < numSwaps; ++m)
                        jac_row[numDeposits + m] = derivative(swapRates2[m]);
                }
            }

            if (swapRate2_tape.shouldRecord()) {
                tape2.clearDerivatives();
                tape2.registerOutput(swapRate2_tape);
                derivative(swapRate2_tape) = 1.0;
                tape2.computeAdjoints();

                double* jac_row = jacobian.data() + size * numMarketQuotes;
                for (Size m = 0; m < numDeposits; ++m)
                    jac_row[m] = derivative(depositRates2[m]);
                for (Size m = 0; m < numSwaps; ++m)
                    jac_row[numDeposits + m] = derivative(swapRates2[m]);
            }

            tape2.deactivate();

            auto t_boot_bwd_end = Clock::now();

            // --- JIT kernel recording (build JIT graph with Forge Fast config) ---
            auto t_kernel_record_start = Clock::now();

            // Use ForgeBackend with graph optimizations enabled
            auto forgeBackendFast = std::make_unique<xad::forge::ForgeBackend<double>>(true);
            xad::JITCompiler<double> jit(std::move(forgeBackendFast));

            std::vector<xad::AD> jit_initRates(size);
            xad::AD jit_swapRate;
            std::vector<xad::AD> jit_randoms(fullGridRandoms);  // Full grid randoms

            for (Size k = 0; k < size; ++k) {
                jit_initRates[k] = xad::AD(value(initRates2[k]));
                jit.registerInput(jit_initRates[k]);
            }

            jit_swapRate = xad::AD(value(swapRate2_tape));
            jit.registerInput(jit_swapRate);

            for (Size m = 0; m < fullGridRandoms; ++m) {
                jit_randoms[m] = xad::AD(0.0);
                jit.registerInput(jit_randoms[m]);
            }

            jit.newRecording();

            // Record path evolution - Full grid (same as JIT Slow)
            std::vector<xad::AD> asset_jit(size);
            std::vector<xad::AD> assetAtExercise_jit(size);
            for (Size k = 0; k < size; ++k) {
                asset_jit[k] = jit_initRates[k];
            }

            for (Size step = 1; step <= fullGridSteps; ++step) {
                Size offset = (step - 1) * numFactors;
                Time t = grid[step - 1];
                Time dt = grid.dt(step - 1);

                Array dw(numFactors);
                for (Size f = 0; f < numFactors; ++f) {
                    dw[f] = jit_randoms[offset + f];
                }

                Array asset_arr(size);
                for (Size k = 0; k < size; ++k) asset_arr[k] = asset_jit[k];

                Array evolved = process2->evolve(t, asset_arr, dt, dw);
                for (Size k = 0; k < size; ++k) asset_jit[k] = evolved[k];

                // Store rates at exercise step
                if (step == exerciseStep) {
                    for (Size k = 0; k < size; ++k) assetAtExercise_jit[k] = asset_jit[k];
                }
            }

            // Use rates at EXERCISE time for payoff
            std::vector<xad::AD> dis_jit(size);
            xad::AD df_jit = xad::AD(1.0);
            for (Size k = 0; k < size; ++k) {
                double accrual = accrualEnd[k] - accrualStart[k];
                df_jit = df_jit / (xad::AD(1.0) + assetAtExercise_jit[k] * accrual);
                dis_jit[k] = df_jit;
            }

            xad::AD jit_npv = xad::AD(0.0);
            for (Size m = i_opt; m < i_opt + j_opt; ++m) {
                double accrual = accrualEnd[m] - accrualStart[m];
                jit_npv = jit_npv + (jit_swapRate - assetAtExercise_jit[m]) * accrual * dis_jit[m];
            }

            xad::AD jit_payoff = xad::less(jit_npv, xad::AD(0.0)).If(xad::AD(0.0), jit_npv);
            jit.registerOutput(jit_payoff);

            auto t_kernel_record_end = Clock::now();

            // --- JIT kernel compile ---
            auto t_kernel_compile_start = Clock::now();

            jit.compile();

            auto t_kernel_compile_end = Clock::now();

            // --- JIT kernel execution (MC loop) with granular timing ---
            const auto& graph = jit.getGraph();
            uint32_t outputSlot = graph.output_ids[0];

            double mcPrice_jit_local = 0.0;
            std::vector<double> dPrice_dInitRates(size, 0.0);
            double dPrice_dSwapRate_jit = 0.0;

            double set_inputs_time = 0.0;
            double kernel_exec_time = 0.0;
            double get_outputs_time = 0.0;
            double get_gradients_time = 0.0;

            for (Size n = 0; n < nrTrails; ++n) {
                // Set inputs timing
                auto t_set_start = Clock::now();
                for (Size k = 0; k < size; ++k) {
                    value(jit_initRates[k]) = value(initRates2[k]);
                }
                value(jit_swapRate) = value(swapRate2_tape);
                for (Size m = 0; m < fullGridRandoms; ++m) {
                    value(jit_randoms[m]) = allRandomsFull[n][m];
                }
                auto t_set_end = Clock::now();
                set_inputs_time += Duration(t_set_end - t_set_start).count();

                // Kernel exec timing
                auto t_exec_start = Clock::now();
                double payoff_value;
                jit.forward(&payoff_value);
                auto t_exec_end = Clock::now();
                kernel_exec_time += Duration(t_exec_end - t_exec_start).count();

                // Get outputs timing (trivial - just accumulate)
                auto t_get_out_start = Clock::now();
                mcPrice_jit_local += payoff_value;
                auto t_get_out_end = Clock::now();
                get_outputs_time += Duration(t_get_out_end - t_get_out_start).count();

                // Get gradients timing
                auto t_grad_mc_start = Clock::now();
                jit.clearDerivatives();
                jit.setDerivative(outputSlot, 1.0);
                jit.computeAdjoints();
                for (Size k = 0; k < size; ++k) {
                    dPrice_dInitRates[k] += jit.derivative(graph.input_ids[k]);
                }
                dPrice_dSwapRate_jit += jit.derivative(graph.input_ids[size]);
                auto t_grad_mc_end = Clock::now();
                get_gradients_time += Duration(t_grad_mc_end - t_grad_mc_start).count();
            }

            // --- Gradient (chain rule) ---
            auto t_grad_start = Clock::now();

            mcPrice_jit_local /= static_cast<double>(nrTrails);
            for (Size k = 0; k < size; ++k) {
                dPrice_dInitRates[k] /= static_cast<double>(nrTrails);
            }
            dPrice_dSwapRate_jit /= static_cast<double>(nrTrails);

            std::vector<double> dPrice_dIntermediates(numIntermediates);
            for (Size k = 0; k < size; ++k)
                dPrice_dIntermediates[k] = dPrice_dInitRates[k];
            dPrice_dIntermediates[size] = dPrice_dSwapRate_jit;

            std::vector<double> dPrice_jit_market(numMarketQuotes);
            applyChainRule(jacobian.data(), dPrice_dIntermediates.data(), dPrice_jit_market.data(),
                           numIntermediates, numMarketQuotes);

            for (Size idx = 0; idx < numDeposits; ++idx) dPrice_jit_fast_depo[idx] = dPrice_jit_market[idx];
            for (Size idx = 0; idx < numSwaps; ++idx) dPrice_jit_fast_swap[idx] = dPrice_jit_market[numDeposits + idx];

            mcPrice_jit_fast = mcPrice_jit_local;

            auto t_grad_end = Clock::now();
            auto t_total_end = Clock::now();

            if (recordTiming) {
                jit_fast_tape_setup_times.push_back(Duration(t_tape_setup_end - t_tape_setup_start).count());
                jit_fast_bootstrap_fwd_times.push_back(Duration(t_boot_fwd_end - t_boot_fwd_start).count());
                jit_fast_bootstrap_bwd_times.push_back(Duration(t_boot_bwd_end - t_boot_bwd_start).count());
                jit_fast_kernel_record_times.push_back(Duration(t_kernel_record_end - t_kernel_record_start).count());
                jit_fast_kernel_compile_times.push_back(Duration(t_kernel_compile_end - t_kernel_compile_start).count());
                jit_fast_set_inputs_times.push_back(set_inputs_time);
                jit_fast_kernel_exec_times.push_back(kernel_exec_time);
                jit_fast_get_outputs_times.push_back(get_outputs_time);
                jit_fast_gradient_times.push_back(get_gradients_time);
                jit_fast_chain_rule_times.push_back(Duration(t_grad_end - t_grad_start).count());
                jit_fast_total_times.push_back(Duration(t_total_end - t_total_start).count());
            }
        }

        // =====================================================================
        // 4c. JIT Interpreter (Re-Route) - Full grid with interpreter backend
        // =====================================================================
        std::cout << " JIT-Interp..." << std::flush;
        {
            auto t_total_start = Clock::now();

            // --- Tape setup ---
            auto t_tape_setup_start = Clock::now();

            using tape_type = Real::tape_type;
            tape_type tape2;

            std::vector<Real> depositRates2(numDeposits);
            std::vector<Real> swapRates2(numSwaps);
            for (Size idx = 0; idx < numDeposits; ++idx) depositRates2[idx] = depoRates_val[idx];
            for (Size idx = 0; idx < numSwaps; ++idx) swapRates2[idx] = swapRates_val[idx];

            tape2.registerInputs(depositRates2);
            tape2.registerInputs(swapRates2);
            tape2.newRecording();

            auto t_tape_setup_end = Clock::now();

            // --- Bootstrap forward pass ---
            auto t_boot_fwd_start = Clock::now();

            RelinkableHandle<YieldTermStructure> euriborTS2;
            auto euribor6m2 = ext::make_shared<Euribor6M>(euriborTS2);
            euribor6m2->addFixing(Date(2, September, 2005), 0.04);

            std::vector<ext::shared_ptr<RateHelper>> instruments2;
            for (Size idx = 0; idx < numDeposits; ++idx) {
                auto depoQuote2 = ext::make_shared<SimpleQuote>(depositRates2[idx]);
                instruments2.push_back(ext::make_shared<DepositRateHelper>(
                    Handle<Quote>(depoQuote2), depoTenors[idx], fixingDays,
                    calendar, ModifiedFollowing, true, dayCounter));
            }
            for (Size idx = 0; idx < numSwaps; ++idx) {
                auto swapQuote = ext::make_shared<SimpleQuote>(swapRates2[idx]);
                instruments2.push_back(ext::make_shared<SwapRateHelper>(
                    Handle<Quote>(swapQuote), swapTenors[idx],
                    calendar, Annual, Unadjusted, Thirty360(Thirty360::BondBasis),
                    euribor6m2));
            }

            auto yieldCurve2 = ext::make_shared<PiecewiseYieldCurve<ZeroYield, Linear>>(
                settlementDate, instruments2, dayCounter);
            yieldCurve2->enableExtrapolation();

            std::vector<Date> curveDates2;
            std::vector<Real> zeroRates2;
            curveDates2.push_back(settlementDate);
            zeroRates2.push_back(yieldCurve2->zeroRate(settlementDate, dayCounter, Continuous).rate());
            Date endDate2 = settlementDate + 6 * Years;
            curveDates2.push_back(endDate2);
            zeroRates2.push_back(yieldCurve2->zeroRate(endDate2, dayCounter, Continuous).rate());

            std::vector<Rate> zeroRates2_ql;
            for (const auto& r : zeroRates2) zeroRates2_ql.push_back(r);

            RelinkableHandle<YieldTermStructure> termStructure2;
            ext::shared_ptr<IborIndex> index2(new Euribor6M(termStructure2));
            index2->addFixing(Date(2, September, 2005), 0.04);
            termStructure2.linkTo(ext::make_shared<ZeroCurve>(curveDates2, zeroRates2_ql, dayCounter));

            ext::shared_ptr<LiborForwardModelProcess> process2(
                new LiborForwardModelProcess(size, index2));
            ext::shared_ptr<LmCorrelationModel> corrModel2(
                new LmExponentialCorrelationModel(size, 0.5));
            ext::shared_ptr<LmVolatilityModel> volaModel2(
                new LmLinearExponentialVolatilityModel(process2->fixingTimes(),
                                                       0.291, 1.483, 0.116, 0.00001));
            process2->setCovarParam(ext::shared_ptr<LfmCovarianceParameterization>(
                new LfmCovarianceProxy(volaModel2, corrModel2)));

            ext::shared_ptr<VanillaSwap> fwdSwap2(
                new VanillaSwap(Swap::Receiver, 1.0,
                                schedule, 0.05, dayCounter,
                                schedule, index2, 0.0, index2->dayCounter()));
            fwdSwap2->setPricingEngine(ext::make_shared<DiscountingSwapEngine>(
                index2->forwardingTermStructure()));
            Real swapRate2_tape = fwdSwap2->fairRate();

            Array initRates2 = process2->initialValues();

            auto t_boot_fwd_end = Clock::now();

            // --- Bootstrap backward pass (Jacobian) ---
            auto t_boot_bwd_start = Clock::now();

            std::vector<double> jacobian(numIntermediates * numMarketQuotes, 0.0);

            for (Size k = 0; k < size; ++k) {
                if (initRates2[k].shouldRecord()) {
                    tape2.clearDerivatives();
                    tape2.registerOutput(initRates2[k]);
                    derivative(initRates2[k]) = 1.0;
                    tape2.computeAdjoints();

                    double* jac_row = jacobian.data() + k * numMarketQuotes;
                    for (Size m = 0; m < numDeposits; ++m)
                        jac_row[m] = derivative(depositRates2[m]);
                    for (Size m = 0; m < numSwaps; ++m)
                        jac_row[numDeposits + m] = derivative(swapRates2[m]);
                }
            }

            if (swapRate2_tape.shouldRecord()) {
                tape2.clearDerivatives();
                tape2.registerOutput(swapRate2_tape);
                derivative(swapRate2_tape) = 1.0;
                tape2.computeAdjoints();

                double* jac_row = jacobian.data() + size * numMarketQuotes;
                for (Size m = 0; m < numDeposits; ++m)
                    jac_row[m] = derivative(depositRates2[m]);
                for (Size m = 0; m < numSwaps; ++m)
                    jac_row[numDeposits + m] = derivative(swapRates2[m]);
            }

            tape2.deactivate();

            auto t_boot_bwd_end = Clock::now();

            // --- JIT kernel recording (build JIT graph with Interpreter backend) ---
            auto t_kernel_record_start = Clock::now();

            // Use default JITCompiler with JITGraphInterpreter backend
            xad::JITCompiler<double> jit;

            std::vector<xad::AD> jit_initRates(size);
            xad::AD jit_swapRate;
            std::vector<xad::AD> jit_randoms(fullGridRandoms);  // Full grid randoms

            for (Size k = 0; k < size; ++k) {
                jit_initRates[k] = xad::AD(value(initRates2[k]));
                jit.registerInput(jit_initRates[k]);
            }

            jit_swapRate = xad::AD(value(swapRate2_tape));
            jit.registerInput(jit_swapRate);

            for (Size m = 0; m < fullGridRandoms; ++m) {
                jit_randoms[m] = xad::AD(0.0);
                jit.registerInput(jit_randoms[m]);
            }

            jit.newRecording();

            // Record path evolution - Full grid (same as JIT Slow)
            std::vector<xad::AD> asset_jit(size);
            std::vector<xad::AD> assetAtExercise_jit(size);
            for (Size k = 0; k < size; ++k) {
                asset_jit[k] = jit_initRates[k];
            }

            for (Size step = 1; step <= fullGridSteps; ++step) {
                Size offset = (step - 1) * numFactors;
                Time t = grid[step - 1];
                Time dt = grid.dt(step - 1);

                Array dw(numFactors);
                for (Size f = 0; f < numFactors; ++f) {
                    dw[f] = jit_randoms[offset + f];
                }

                Array asset_arr(size);
                for (Size k = 0; k < size; ++k) asset_arr[k] = asset_jit[k];

                Array evolved = process2->evolve(t, asset_arr, dt, dw);
                for (Size k = 0; k < size; ++k) asset_jit[k] = evolved[k];

                // Store rates at exercise step
                if (step == exerciseStep) {
                    for (Size k = 0; k < size; ++k) assetAtExercise_jit[k] = asset_jit[k];
                }
            }

            // Use rates at EXERCISE time for payoff
            std::vector<xad::AD> dis_jit(size);
            xad::AD df_jit = xad::AD(1.0);
            for (Size k = 0; k < size; ++k) {
                double accrual = accrualEnd[k] - accrualStart[k];
                df_jit = df_jit / (xad::AD(1.0) + assetAtExercise_jit[k] * accrual);
                dis_jit[k] = df_jit;
            }

            xad::AD jit_npv = xad::AD(0.0);
            for (Size m = i_opt; m < i_opt + j_opt; ++m) {
                double accrual = accrualEnd[m] - accrualStart[m];
                jit_npv = jit_npv + (jit_swapRate - assetAtExercise_jit[m]) * accrual * dis_jit[m];
            }

            xad::AD jit_payoff = xad::less(jit_npv, xad::AD(0.0)).If(xad::AD(0.0), jit_npv);
            jit.registerOutput(jit_payoff);

            auto t_kernel_record_end = Clock::now();

            // --- JIT kernel compile ---
            auto t_kernel_compile_start = Clock::now();

            jit.compile();

            auto t_kernel_compile_end = Clock::now();

            // --- JIT kernel execution (MC loop) with granular timing ---
            const auto& graph = jit.getGraph();
            uint32_t outputSlot = graph.output_ids[0];

            double mcPrice_jit_local = 0.0;
            std::vector<double> dPrice_dInitRates(size, 0.0);
            double dPrice_dSwapRate_jit = 0.0;

            double set_inputs_time = 0.0;
            double kernel_exec_time = 0.0;
            double get_outputs_time = 0.0;
            double get_gradients_time = 0.0;

            for (Size n = 0; n < nrTrails; ++n) {
                // Set inputs timing
                auto t_set_start = Clock::now();
                for (Size k = 0; k < size; ++k) {
                    value(jit_initRates[k]) = value(initRates2[k]);
                }
                value(jit_swapRate) = value(swapRate2_tape);
                for (Size m = 0; m < fullGridRandoms; ++m) {
                    value(jit_randoms[m]) = allRandomsFull[n][m];
                }
                auto t_set_end = Clock::now();
                set_inputs_time += Duration(t_set_end - t_set_start).count();

                // Kernel exec timing
                auto t_exec_start = Clock::now();
                double payoff_value;
                jit.forward(&payoff_value);
                auto t_exec_end = Clock::now();
                kernel_exec_time += Duration(t_exec_end - t_exec_start).count();

                // Get outputs timing (trivial - just accumulate)
                auto t_get_out_start = Clock::now();
                mcPrice_jit_local += payoff_value;
                auto t_get_out_end = Clock::now();
                get_outputs_time += Duration(t_get_out_end - t_get_out_start).count();

                // Get gradients timing
                auto t_grad_mc_start = Clock::now();
                jit.clearDerivatives();
                jit.setDerivative(outputSlot, 1.0);
                jit.computeAdjoints();
                for (Size k = 0; k < size; ++k) {
                    dPrice_dInitRates[k] += jit.derivative(graph.input_ids[k]);
                }
                dPrice_dSwapRate_jit += jit.derivative(graph.input_ids[size]);
                auto t_grad_mc_end = Clock::now();
                get_gradients_time += Duration(t_grad_mc_end - t_grad_mc_start).count();
            }

            // --- Gradient (chain rule) ---
            auto t_grad_start = Clock::now();

            mcPrice_jit_local /= static_cast<double>(nrTrails);
            for (Size k = 0; k < size; ++k) {
                dPrice_dInitRates[k] /= static_cast<double>(nrTrails);
            }
            dPrice_dSwapRate_jit /= static_cast<double>(nrTrails);

            std::vector<double> dPrice_dIntermediates(numIntermediates);
            for (Size k = 0; k < size; ++k)
                dPrice_dIntermediates[k] = dPrice_dInitRates[k];
            dPrice_dIntermediates[size] = dPrice_dSwapRate_jit;

            std::vector<double> dPrice_jit_market(numMarketQuotes);
            applyChainRule(jacobian.data(), dPrice_dIntermediates.data(), dPrice_jit_market.data(),
                           numIntermediates, numMarketQuotes);

            for (Size idx = 0; idx < numDeposits; ++idx) dPrice_jit_interp_depo[idx] = dPrice_jit_market[idx];
            for (Size idx = 0; idx < numSwaps; ++idx) dPrice_jit_interp_swap[idx] = dPrice_jit_market[numDeposits + idx];

            mcPrice_jit_interp = mcPrice_jit_local;

            auto t_grad_end = Clock::now();
            auto t_total_end = Clock::now();

            if (recordTiming) {
                jit_interp_tape_setup_times.push_back(Duration(t_tape_setup_end - t_tape_setup_start).count());
                jit_interp_bootstrap_fwd_times.push_back(Duration(t_boot_fwd_end - t_boot_fwd_start).count());
                jit_interp_bootstrap_bwd_times.push_back(Duration(t_boot_bwd_end - t_boot_bwd_start).count());
                jit_interp_kernel_record_times.push_back(Duration(t_kernel_record_end - t_kernel_record_start).count());
                jit_interp_kernel_compile_times.push_back(Duration(t_kernel_compile_end - t_kernel_compile_start).count());
                jit_interp_set_inputs_times.push_back(set_inputs_time);
                jit_interp_kernel_exec_times.push_back(kernel_exec_time);
                jit_interp_get_outputs_times.push_back(get_outputs_time);
                jit_interp_gradient_times.push_back(get_gradients_time);
                jit_interp_chain_rule_times.push_back(Duration(t_grad_end - t_grad_start).count());
                jit_interp_total_times.push_back(Duration(t_total_end - t_total_start).count());
            }
        }

        // =====================================================================
        // 4d. JIT AVX (Re-Route) - AVX2 4-path batching
        // =====================================================================
        std::cout << " JIT-AVX..." << std::flush;
        try {
            auto t_total_start = Clock::now();

            // --- Tape setup ---
            auto t_tape_setup_start = Clock::now();

            using tape_type = Real::tape_type;
            tape_type tape2;

            std::vector<Real> depositRates2(numDeposits);
            std::vector<Real> swapRates2(numSwaps);
            for (Size idx = 0; idx < numDeposits; ++idx) depositRates2[idx] = depoRates_val[idx];
            for (Size idx = 0; idx < numSwaps; ++idx) swapRates2[idx] = swapRates_val[idx];

            tape2.registerInputs(depositRates2);
            tape2.registerInputs(swapRates2);
            tape2.newRecording();

            auto t_tape_setup_end = Clock::now();

            // --- Bootstrap forward pass ---
            auto t_boot_fwd_start = Clock::now();

            RelinkableHandle<YieldTermStructure> euriborTS2;
            auto euribor6m2 = ext::make_shared<Euribor6M>(euriborTS2);
            euribor6m2->addFixing(Date(2, September, 2005), 0.04);

            std::vector<ext::shared_ptr<RateHelper>> instruments2;
            for (Size idx = 0; idx < numDeposits; ++idx) {
                auto depoQuote2 = ext::make_shared<SimpleQuote>(depositRates2[idx]);
                instruments2.push_back(ext::make_shared<DepositRateHelper>(
                    Handle<Quote>(depoQuote2), depoTenors[idx], fixingDays,
                    calendar, ModifiedFollowing, true, dayCounter));
            }
            for (Size idx = 0; idx < numSwaps; ++idx) {
                auto swapQuote = ext::make_shared<SimpleQuote>(swapRates2[idx]);
                instruments2.push_back(ext::make_shared<SwapRateHelper>(
                    Handle<Quote>(swapQuote), swapTenors[idx],
                    calendar, Annual, Unadjusted, Thirty360(Thirty360::BondBasis),
                    euribor6m2));
            }

            auto yieldCurve2 = ext::make_shared<PiecewiseYieldCurve<ZeroYield, Linear>>(
                settlementDate, instruments2, dayCounter);
            yieldCurve2->enableExtrapolation();

            std::vector<Date> curveDates2;
            std::vector<Real> zeroRates2;
            curveDates2.push_back(settlementDate);
            zeroRates2.push_back(yieldCurve2->zeroRate(settlementDate, dayCounter, Continuous).rate());
            Date endDate2 = settlementDate + 6 * Years;
            curveDates2.push_back(endDate2);
            zeroRates2.push_back(yieldCurve2->zeroRate(endDate2, dayCounter, Continuous).rate());

            std::vector<Rate> zeroRates2_ql;
            for (const auto& r : zeroRates2) zeroRates2_ql.push_back(r);

            RelinkableHandle<YieldTermStructure> termStructure2;
            ext::shared_ptr<IborIndex> index2(new Euribor6M(termStructure2));
            index2->addFixing(Date(2, September, 2005), 0.04);
            termStructure2.linkTo(ext::make_shared<ZeroCurve>(curveDates2, zeroRates2_ql, dayCounter));

            ext::shared_ptr<LiborForwardModelProcess> process2(
                new LiborForwardModelProcess(size, index2));
            ext::shared_ptr<LmCorrelationModel> corrModel2(
                new LmExponentialCorrelationModel(size, 0.5));
            ext::shared_ptr<LmVolatilityModel> volaModel2(
                new LmLinearExponentialVolatilityModel(process2->fixingTimes(),
                                                       0.291, 1.483, 0.116, 0.00001));
            process2->setCovarParam(ext::shared_ptr<LfmCovarianceParameterization>(
                new LfmCovarianceProxy(volaModel2, corrModel2)));

            ext::shared_ptr<VanillaSwap> fwdSwap2(
                new VanillaSwap(Swap::Receiver, 1.0,
                                schedule, 0.05, dayCounter,
                                schedule, index2, 0.0, index2->dayCounter()));
            fwdSwap2->setPricingEngine(ext::make_shared<DiscountingSwapEngine>(
                index2->forwardingTermStructure()));
            Real swapRate2_tape = fwdSwap2->fairRate();

            Array initRates2 = process2->initialValues();

            auto t_boot_fwd_end = Clock::now();

            // --- Bootstrap backward pass (Jacobian) ---
            auto t_boot_bwd_start = Clock::now();

            std::vector<double> jacobian(numIntermediates * numMarketQuotes, 0.0);

            for (Size k = 0; k < size; ++k) {
                if (initRates2[k].shouldRecord()) {
                    tape2.clearDerivatives();
                    tape2.registerOutput(initRates2[k]);
                    derivative(initRates2[k]) = 1.0;
                    tape2.computeAdjoints();

                    double* jac_row = jacobian.data() + k * numMarketQuotes;
                    for (Size m = 0; m < numDeposits; ++m)
                        jac_row[m] = derivative(depositRates2[m]);
                    for (Size m = 0; m < numSwaps; ++m)
                        jac_row[numDeposits + m] = derivative(swapRates2[m]);
                }
            }

            if (swapRate2_tape.shouldRecord()) {
                tape2.clearDerivatives();
                tape2.registerOutput(swapRate2_tape);
                derivative(swapRate2_tape) = 1.0;
                tape2.computeAdjoints();

                double* jac_row = jacobian.data() + size * numMarketQuotes;
                for (Size m = 0; m < numDeposits; ++m)
                    jac_row[m] = derivative(depositRates2[m]);
                for (Size m = 0; m < numSwaps; ++m)
                    jac_row[numDeposits + m] = derivative(swapRates2[m]);
            }

            tape2.deactivate();

            auto t_boot_bwd_end = Clock::now();

            // --- JIT kernel recording (build JIT graph for AVX compilation) ---
            auto t_kernel_record_start = Clock::now();

            // Build JIT graph for single path (same as JIT Slow)
            auto forgeBackendAVX = std::make_unique<xad::forge::ForgeBackend<double>>(false);
            xad::JITCompiler<double> jit(std::move(forgeBackendAVX));

            std::vector<xad::AD> jit_initRates(size);
            xad::AD jit_swapRate;
            std::vector<xad::AD> jit_randoms(fullGridRandoms);

            for (Size k = 0; k < size; ++k) {
                jit_initRates[k] = xad::AD(value(initRates2[k]));
                jit.registerInput(jit_initRates[k]);
            }

            jit_swapRate = xad::AD(value(swapRate2_tape));
            jit.registerInput(jit_swapRate);

            for (Size m = 0; m < fullGridRandoms; ++m) {
                jit_randoms[m] = xad::AD(0.0);
                jit.registerInput(jit_randoms[m]);
            }

            jit.newRecording();

            // Record path evolution - SLOW: full grid
            std::vector<xad::AD> asset_jit(size);
            std::vector<xad::AD> assetAtExercise_jit(size);
            for (Size k = 0; k < size; ++k) {
                asset_jit[k] = jit_initRates[k];
            }

            for (Size step = 1; step <= fullGridSteps; ++step) {
                Size offset = (step - 1) * numFactors;
                Time t = grid[step - 1];
                Time dt = grid.dt(step - 1);

                Array dw(numFactors);
                for (Size f = 0; f < numFactors; ++f) {
                    dw[f] = jit_randoms[offset + f];
                }

                Array asset_arr(size);
                for (Size k = 0; k < size; ++k) asset_arr[k] = asset_jit[k];

                Array evolved = process2->evolve(t, asset_arr, dt, dw);
                for (Size k = 0; k < size; ++k) asset_jit[k] = evolved[k];

                if (step == exerciseStep) {
                    for (Size k = 0; k < size; ++k) assetAtExercise_jit[k] = asset_jit[k];
                }
            }

            std::vector<xad::AD> dis_jit(size);
            xad::AD df_jit = xad::AD(1.0);
            for (Size k = 0; k < size; ++k) {
                double accrual = accrualEnd[k] - accrualStart[k];
                df_jit = df_jit / (xad::AD(1.0) + assetAtExercise_jit[k] * accrual);
                dis_jit[k] = df_jit;
            }

            xad::AD jit_npv = xad::AD(0.0);
            for (Size m = i_opt; m < i_opt + j_opt; ++m) {
                double accrual = accrualEnd[m] - accrualStart[m];
                jit_npv = jit_npv + (jit_swapRate - assetAtExercise_jit[m]) * accrual * dis_jit[m];
            }

            xad::AD jit_payoff = xad::less(jit_npv, xad::AD(0.0)).If(xad::AD(0.0), jit_npv);
            jit.registerOutput(jit_payoff);

            auto t_kernel_record_end = Clock::now();

            // --- JIT AVX kernel compile (compile to AVX2 native code) ---
            auto t_kernel_compile_start = Clock::now();

            // Get the JIT graph
            const auto& jitGraph = jit.getGraph();

            // Deactivate jit now that we've got the graph
            jit.deactivate();

            // Create AVX backend and compile directly from JITGraph
            xad::forge::ForgeBackendAVX<double> avxBackend(false);
            avxBackend.compile(jitGraph);

            auto t_kernel_compile_end = Clock::now();

            // --- JIT AVX kernel execution (MC loop with 4-path batching) ---
            double mcPrice_jit_local = 0.0;
            std::vector<double> dPrice_dInitRates(size, 0.0);
            double dPrice_dSwapRate_jit = 0.0;

            // Process paths in batches of 4
            constexpr int BATCH_SIZE = xad::forge::ForgeBackendAVX<double>::VECTOR_WIDTH;
            Size numBatches = (nrTrails + BATCH_SIZE - 1) / BATCH_SIZE;

            // Temporary arrays for batch processing
            double inputLanes[BATCH_SIZE];
            double outputLanes[BATCH_SIZE];

            // Granular timing accumulators
            double set_inputs_time = 0.0;
            double kernel_exec_time = 0.0;
            double get_outputs_time = 0.0;
            double get_gradients_time = 0.0;

            for (Size batch = 0; batch < numBatches; ++batch) {
                Size pathStart = batch * BATCH_SIZE;
                Size pathsInBatch = std::min(static_cast<Size>(BATCH_SIZE), nrTrails - pathStart);

                // Set inputs timing
                auto t_set_start = Clock::now();

                // Set initial rates (same for all 4 paths)
                for (Size k = 0; k < size; ++k) {
                    for (int lane = 0; lane < BATCH_SIZE; ++lane) {
                        inputLanes[lane] = value(initRates2[k]);
                    }
                    avxBackend.setInput(k, inputLanes);
                }

                // Set swap rate (same for all 4 paths)
                for (int lane = 0; lane < BATCH_SIZE; ++lane) {
                    inputLanes[lane] = value(swapRate2_tape);
                }
                avxBackend.setInput(size, inputLanes);

                // Set random numbers (different for each path in batch)
                for (Size m = 0; m < fullGridRandoms; ++m) {
                    for (int lane = 0; lane < BATCH_SIZE; ++lane) {
                        Size pathIdx = pathStart + lane;
                        if (pathIdx < nrTrails) {
                            inputLanes[lane] = allRandomsFull[pathIdx][m];
                        } else {
                            inputLanes[lane] = 0.0;  // Padding for incomplete batch
                        }
                    }
                    avxBackend.setInput(size + 1 + m, inputLanes);
                }

                auto t_set_end = Clock::now();
                set_inputs_time += Duration(t_set_end - t_set_start).count();

                // Kernel exec timing (forward + backward pass together)
                auto t_exec_start = Clock::now();

                // Prepare containers for results
                // Need space for: initial rates (size) + swap rate (1) + random numbers (fullGridRandoms)
                std::size_t numInputs = size + 1 + fullGridRandoms;
                std::vector<double> inputGradients(numInputs * BATCH_SIZE);

                // Execute forward + backward in one call
                avxBackend.forwardAndBackward(outputLanes, inputGradients.data());

                auto t_exec_end = Clock::now();
                kernel_exec_time += Duration(t_exec_end - t_exec_start).count();

                // Get outputs timing
                auto t_get_out_start = Clock::now();
                for (Size lane = 0; lane < pathsInBatch; ++lane) {
                    mcPrice_jit_local += outputLanes[lane];
                }
                auto t_get_out_end = Clock::now();
                get_outputs_time += Duration(t_get_out_end - t_get_out_start).count();

                // Get gradients timing
                auto t_grad_mc_start = Clock::now();
                for (Size k = 0; k < size; ++k) {
                    for (Size lane = 0; lane < pathsInBatch; ++lane) {
                        dPrice_dInitRates[k] += inputGradients[k * BATCH_SIZE + lane];
                    }
                }
                for (Size lane = 0; lane < pathsInBatch; ++lane) {
                    dPrice_dSwapRate_jit += inputGradients[size * BATCH_SIZE + lane];
                }
                auto t_grad_mc_end = Clock::now();
                get_gradients_time += Duration(t_grad_mc_end - t_grad_mc_start).count();
            }

            // --- Gradient (chain rule) ---
            auto t_grad_start = Clock::now();

            mcPrice_jit_local /= static_cast<double>(nrTrails);
            for (Size k = 0; k < size; ++k) {
                dPrice_dInitRates[k] /= static_cast<double>(nrTrails);
            }
            dPrice_dSwapRate_jit /= static_cast<double>(nrTrails);

            std::vector<double> dPrice_dIntermediates(numIntermediates);
            for (Size k = 0; k < size; ++k)
                dPrice_dIntermediates[k] = dPrice_dInitRates[k];
            dPrice_dIntermediates[size] = dPrice_dSwapRate_jit;

            std::vector<double> dPrice_jit_market(numMarketQuotes);
            applyChainRule(jacobian.data(), dPrice_dIntermediates.data(), dPrice_jit_market.data(),
                           numIntermediates, numMarketQuotes);

            for (Size idx = 0; idx < numDeposits; ++idx) dPrice_jit_avx_depo[idx] = dPrice_jit_market[idx];
            for (Size idx = 0; idx < numSwaps; ++idx) dPrice_jit_avx_swap[idx] = dPrice_jit_market[numDeposits + idx];

            mcPrice_jit_avx = mcPrice_jit_local;

            auto t_grad_end = Clock::now();
            auto t_total_end = Clock::now();

            if (recordTiming) {
                jit_avx_tape_setup_times.push_back(Duration(t_tape_setup_end - t_tape_setup_start).count());
                jit_avx_bootstrap_fwd_times.push_back(Duration(t_boot_fwd_end - t_boot_fwd_start).count());
                jit_avx_bootstrap_bwd_times.push_back(Duration(t_boot_bwd_end - t_boot_bwd_start).count());
                jit_avx_kernel_record_times.push_back(Duration(t_kernel_record_end - t_kernel_record_start).count());
                jit_avx_kernel_compile_times.push_back(Duration(t_kernel_compile_end - t_kernel_compile_start).count());
                jit_avx_set_inputs_times.push_back(set_inputs_time);
                jit_avx_kernel_exec_times.push_back(kernel_exec_time);
                jit_avx_get_outputs_times.push_back(get_outputs_time);
                jit_avx_gradient_times.push_back(get_gradients_time);
                jit_avx_chain_rule_times.push_back(Duration(t_grad_end - t_grad_start).count());
                jit_avx_total_times.push_back(Duration(t_total_end - t_total_start).count());
            }
        } catch (const std::exception& e) {
            std::cout << "\n  WARNING: JIT-AVX failed: " << e.what() << std::endl;
            std::cout << "  Skipping JIT-AVX benchmarks for this run." << std::endl;
        }

        std::cout << " Done." << std::flush;
    }

    std::cout << "\n\n";

    // =========================================================================
    // RESULTS VERIFICATION (same table as Stage 3)
    // =========================================================================
    std::cout << "  " << std::string(85, '=') << "\n";
    std::cout << "  TABLE 1: MC PRICE COMPARISON (Verification)\n";
    std::cout << "  " << std::string(85, '=') << "\n";
    std::cout << std::endl;

    std::cout << "  " << std::setw(18) << "Approach" << " | "
              << std::setw(14) << "MC Price" << " | "
              << std::setw(14) << "Diff vs Ref" << " | "
              << "Match\n";
    std::cout << "  " << std::string(18, '-') << "-+-"
              << std::string(14, '-') << "-+-"
              << std::string(14, '-') << "-+-"
              << std::string(5, '-') << "\n";

    // Reference: Original (QuantLib)
    std::cout << "  " << std::setw(18) << "Orig (QuantLib)" << " | "
              << std::fixed << std::setprecision(6) << std::setw(14) << mcPrice_orig_ql << " | "
              << std::setw(14) << "-" << " |   -  \n";

    // Original (Re-Route)
    double diff_orig_rr_slow = mcPrice_orig_rr_slow - mcPrice_orig_ql;
    bool orig_rr_slow_match = (std::abs(diff_orig_rr_slow) < 1e-6);
    std::cout << "  " << std::setw(18) << "Orig (RR)" << " | "
              << std::fixed << std::setprecision(6) << std::setw(14) << mcPrice_orig_rr_slow << " | "
              << std::scientific << std::setprecision(2) << std::setw(14) << diff_orig_rr_slow << " | "
              << (orig_rr_slow_match ? "  OK " : "DIFF!") << "\n";

    // Bump & Reprice (QuantLib)
    double diff_bump_ql = mcPrice_bump_ql - mcPrice_orig_ql;
    bool bump_ql_match = (std::abs(diff_bump_ql) < 1e-10);
    std::cout << "  " << std::setw(18) << "Bump (QuantLib)" << " | "
              << std::fixed << std::setprecision(6) << std::setw(14) << mcPrice_bump_ql << " | "
              << std::scientific << std::setprecision(2) << std::setw(14) << diff_bump_ql << " | "
              << (bump_ql_match ? "  OK " : "DIFF!") << "\n";

    // Bump & Reprice (Re-Route)
    double diff_bump_rr_slow = mcPrice_bump_rr_slow - mcPrice_orig_rr_slow;
    bool bump_rr_slow_match = (std::abs(diff_bump_rr_slow) < 1e-10);
    std::cout << "  " << std::setw(18) << "Bump (RR)" << " | "
              << std::fixed << std::setprecision(6) << std::setw(14) << mcPrice_bump_rr_slow << " | "
              << std::scientific << std::setprecision(2) << std::setw(14) << diff_bump_rr_slow << " | "
              << (bump_rr_slow_match ? "  OK " : "DIFF!") << "\n";

    // XAD (QuantLib)
    double diff_xad_ql = mcPrice_xad_ql - mcPrice_orig_ql;
    bool xad_ql_match = (std::abs(diff_xad_ql) < 1e-10);
    std::cout << "  " << std::setw(18) << "XAD (QuantLib)" << " | "
              << std::fixed << std::setprecision(6) << std::setw(14) << mcPrice_xad_ql << " | "
              << std::scientific << std::setprecision(2) << std::setw(14) << diff_xad_ql << " | "
              << (xad_ql_match ? "  OK " : "DIFF!") << "\n";

    // XAD (Re-Route)
    double diff_xad_rr_slow = mcPrice_xad_rr_slow - mcPrice_orig_rr_slow;
    bool xad_rr_slow_match = (std::abs(diff_xad_rr_slow) < 1e-10);
    std::cout << "  " << std::setw(18) << "XAD (RR)" << " | "
              << std::fixed << std::setprecision(6) << std::setw(14) << mcPrice_xad_rr_slow << " | "
              << std::scientific << std::setprecision(2) << std::setw(14) << diff_xad_rr_slow << " | "
              << (xad_rr_slow_match ? "  OK " : "DIFF!") << "\n";

    // JIT (Re-Route)
    double diff_jit_slow = mcPrice_jit_slow - mcPrice_orig_rr_slow;
    bool jit_slow_match = (std::abs(diff_jit_slow) < 1e-10);
    std::cout << "  " << std::setw(18) << "JIT (RR)" << " | "
              << std::fixed << std::setprecision(6) << std::setw(14) << mcPrice_jit_slow << " | "
              << std::scientific << std::setprecision(2) << std::setw(14) << diff_jit_slow << " | "
              << (jit_slow_match ? "  OK " : "DIFF!") << "\n";

    // JIT AVX (Re-Route)
    double diff_jit_avx = mcPrice_jit_avx - mcPrice_orig_rr_slow;
    bool jit_avx_match = (std::abs(diff_jit_avx) < 1e-10);
    std::cout << "  " << std::setw(18) << "JIT AVX (RR)" << " | "
              << std::fixed << std::setprecision(6) << std::setw(14) << mcPrice_jit_avx << " | "
              << std::scientific << std::setprecision(2) << std::setw(14) << diff_jit_avx << " | "
              << (jit_avx_match ? "  OK " : "DIFF!") << "\n";

    // JIT Opt (Re-Route)
    double diff_jit_fast = mcPrice_jit_fast - mcPrice_orig_rr_slow;
    bool jit_fast_match = (std::abs(diff_jit_fast) < 1e-10);
    std::cout << "  " << std::setw(18) << "JIT Opt (RR)" << " | "
              << std::fixed << std::setprecision(6) << std::setw(14) << mcPrice_jit_fast << " | "
              << std::scientific << std::setprecision(2) << std::setw(14) << diff_jit_fast << " | "
              << (jit_fast_match ? "  OK " : "DIFF!") << "\n";

    std::cout << std::endl;

    // Derivatives table
    std::cout << "  " << std::string(170, '=') << "\n";
    std::cout << "  TABLE 2: DERIVATIVE COMPARISON (dPrice/dMarketQuote)\n";
    std::cout << "  " << std::string(170, '=') << "\n";
    std::cout << std::endl;

    std::cout << "  " << std::setw(12) << "Quote" << " | "
              << std::setw(14) << "Bump QuantLib" << " | "
              << std::setw(14) << "XAD QuantLib" << " | "
              << std::setw(14) << "XAD RR" << " | "
              << std::setw(14) << "JIT RR" << " | "
              << std::setw(14) << "JIT AVX" << " | "
              << std::setw(14) << "JIT Opt" << "\n";
    std::cout << "  " << std::string(12, '-') << "-+-"
              << std::string(14, '-') << "-+-"
              << std::string(14, '-') << "-+-"
              << std::string(14, '-') << "-+-"
              << std::string(14, '-') << "-+-"
              << std::string(14, '-') << "-+-"
              << std::string(14, '-') << "\n";

    std::vector<std::string> depoLabels = {"Depo ON", "Depo 1M", "Depo 3M", "Depo 6M"};
    for (Size idx = 0; idx < numDeposits; ++idx) {
        std::cout << "  " << std::setw(12) << depoLabels[idx] << " | "
                  << std::scientific << std::setprecision(4) << std::setw(14) << dPrice_bump_ql_depo[idx] << " | "
                  << std::setw(14) << dPrice_xad_ql_depo[idx] << " | "
                  << std::setw(14) << dPrice_xad_rr_slow_depo[idx] << " | "
                  << std::setw(14) << dPrice_jit_slow_depo[idx] << " | "
                  << std::setw(14) << dPrice_jit_avx_depo[idx] << " | "
                  << std::setw(14) << dPrice_jit_fast_depo[idx] << "\n";
    }

    std::vector<std::string> swapLabels = {"Swap 1Y", "Swap 2Y", "Swap 3Y", "Swap 4Y", "Swap 5Y"};
    for (Size idx = 0; idx < numSwaps; ++idx) {
        std::cout << "  " << std::setw(12) << swapLabels[idx] << " | "
                  << std::scientific << std::setprecision(4) << std::setw(14) << dPrice_bump_ql_swap[idx] << " | "
                  << std::setw(14) << dPrice_xad_ql_swap[idx] << " | "
                  << std::setw(14) << dPrice_xad_rr_slow_swap[idx] << " | "
                  << std::setw(14) << dPrice_jit_slow_swap[idx] << " | "
                  << std::setw(14) << dPrice_jit_avx_swap[idx] << " | "
                  << std::setw(14) << dPrice_jit_fast_swap[idx] << "\n";
    }

    std::cout << std::endl;

    // =========================================================================
    // PERFORMANCE TABLE
    // =========================================================================
    auto avg = [](const std::vector<double>& v) {
        double sum = 0.0;
        for (double x : v) sum += x;
        return v.empty() ? 0.0 : sum / v.size();
    };

    auto stddev = [&avg](const std::vector<double>& v) {
        if (v.size() <= 1) return 0.0;
        double mean = avg(v);
        double sq_sum = 0.0;
        for (double x : v) {
            double diff = x - mean;
            sq_sum += diff * diff;
        }
        return std::sqrt(sq_sum / (v.size() - 1));
    };

    std::cout << "  " << std::string(85, '=') << "\n";
    std::cout << "  TABLE 3: PERFORMANCE ANALYSIS (averaged over " << benchmarkIterations << " iterations)\n";
    std::cout << "  " << std::string(85, '=') << "\n";
    std::cout << std::endl;

    // Original (QuantLib) - price only
    std::cout << "  ORIGINAL (QuantLib) - price only, no derivatives:\n";
    std::cout << "    - TOTAL (avg):            " << std::fixed << std::setprecision(3) << avg(orig_ql_total_times) << " ms\n";
    std::cout << std::endl;

    // Original (Re-Route) - price only, full grid
    std::cout << "  ORIGINAL (Re-Route) - full grid, price only:\n";
    std::cout << "    - TOTAL (avg):            " << std::fixed << std::setprecision(3) << avg(orig_rr_slow_total_times) << " ms\n";
    std::cout << std::endl;

    // Bump & Reprice (QuantLib)
    std::cout << "  BUMP & REPRICE (QuantLib) - MultiPathGenerator:\n";
    std::cout << "    - Evaluations:           " << (1 + numMarketQuotes) << " (1 base + " << numMarketQuotes << " bumps)\n";
    std::cout << "    - TOTAL (avg):           " << std::fixed << std::setprecision(3) << avg(bump_ql_total_times) << " ms\n";
    std::cout << std::endl;

    // Bump & Reprice (Re-Route)
    std::cout << "  BUMP & REPRICE (Re-Route) - full grid:\n";
    std::cout << "    - Evaluations:           " << (1 + numMarketQuotes) << " (1 base + " << numMarketQuotes << " bumps)\n";
    std::cout << "    - TOTAL (avg):           " << std::fixed << std::setprecision(3) << avg(bump_rr_slow_total_times) << " ms\n";
    std::cout << std::endl;

    // XAD (QuantLib)
    std::cout << "  XAD (QuantLib) - MultiPathGenerator:\n";
    std::cout << "    - Tape setup:             " << std::fixed << std::setprecision(3) << avg(xad_ql_tape_setup_times) << " ms\n";
    std::cout << "    - Bootstrap (forward):    " << std::fixed << std::setprecision(3) << avg(xad_ql_bootstrap_times) << " ms\n";
    std::cout << "    - MC Pricing (forward):   " << std::fixed << std::setprecision(3) << avg(xad_ql_mc_times) << " ms\n";
    std::cout << "    - Gradient (adjoints):    " << std::fixed << std::setprecision(3) << avg(xad_ql_gradient_times) << " ms\n";
    std::cout << "    - TOTAL (avg):            " << std::fixed << std::setprecision(3) << avg(xad_ql_total_times) << " ms\n";
    std::cout << std::endl;

    // XAD (Re-Route)
    std::cout << "  XAD (Re-Route) - Full grid:\n";
    std::cout << "    - Tape setup:             " << std::fixed << std::setprecision(3) << avg(xad_rr_slow_tape_setup_times) << " ms\n";
    std::cout << "    - Bootstrap (forward):    " << std::fixed << std::setprecision(3) << avg(xad_rr_slow_bootstrap_times) << " ms\n";
    std::cout << "    - MC Pricing (forward):   " << std::fixed << std::setprecision(3) << avg(xad_rr_slow_mc_times) << " ms\n";
    std::cout << "    - Gradient (adjoints):    " << std::fixed << std::setprecision(3) << avg(xad_rr_slow_gradient_times) << " ms\n";
    std::cout << "    - TOTAL (avg):            " << std::fixed << std::setprecision(3) << avg(xad_rr_slow_total_times) << " ms\n";
    std::cout << std::endl;

    // JIT (Re-Route)
    std::cout << "  JIT (Re-Route) - Full grid:\n";
    std::cout << "    - Tape setup:             " << std::fixed << std::setprecision(3) << avg(jit_slow_tape_setup_times) << " ms\n";
    std::cout << "    - Bootstrap forward:      " << std::fixed << std::setprecision(3) << avg(jit_slow_bootstrap_fwd_times) << " ms\n";
    std::cout << "    - Bootstrap backward:     " << std::fixed << std::setprecision(3) << avg(jit_slow_bootstrap_bwd_times) << " ms\n";
    std::cout << "    - Kernel record:          " << std::fixed << std::setprecision(3) << avg(jit_slow_kernel_record_times) << " ms\n";
    std::cout << "    - Kernel compile:         " << std::fixed << std::setprecision(3) << avg(jit_slow_kernel_compile_times) << " ms\n";
    std::cout << "    - Set inputs:             " << std::fixed << std::setprecision(3) << avg(jit_slow_set_inputs_times) << " ms\n";
    std::cout << "    - Kernel exec:            " << std::fixed << std::setprecision(3) << avg(jit_slow_kernel_exec_times) << " ms\n";
    std::cout << "    - Get outputs:            " << std::fixed << std::setprecision(3) << avg(jit_slow_get_outputs_times) << " ms\n";
    std::cout << "    - Get gradients:          " << std::fixed << std::setprecision(3) << avg(jit_slow_gradient_times) << " ms\n";
    std::cout << "    - Chain rule:             " << std::fixed << std::setprecision(3) << avg(jit_slow_chain_rule_times) << " ms\n";
    std::cout << "    - TOTAL (avg):            " << std::fixed << std::setprecision(3) << avg(jit_slow_total_times) << " ms\n";
    std::cout << std::endl;

    // JIT Opt (Re-Route) - Full grid with graph optimizations
    std::cout << "  JIT Opt (RR) - Full grid + graph optimizations:\n";
    std::cout << "    - Tape setup:             " << std::fixed << std::setprecision(3) << avg(jit_fast_tape_setup_times) << " ms\n";
    std::cout << "    - Bootstrap forward:      " << std::fixed << std::setprecision(3) << avg(jit_fast_bootstrap_fwd_times) << " ms\n";
    std::cout << "    - Bootstrap backward:     " << std::fixed << std::setprecision(3) << avg(jit_fast_bootstrap_bwd_times) << " ms\n";
    std::cout << "    - Kernel record:          " << std::fixed << std::setprecision(3) << avg(jit_fast_kernel_record_times) << " ms\n";
    std::cout << "    - Kernel compile:         " << std::fixed << std::setprecision(3) << avg(jit_fast_kernel_compile_times) << " ms\n";
    std::cout << "    - Set inputs:             " << std::fixed << std::setprecision(3) << avg(jit_fast_set_inputs_times) << " ms\n";
    std::cout << "    - Kernel exec:            " << std::fixed << std::setprecision(3) << avg(jit_fast_kernel_exec_times) << " ms\n";
    std::cout << "    - Get outputs:            " << std::fixed << std::setprecision(3) << avg(jit_fast_get_outputs_times) << " ms\n";
    std::cout << "    - Get gradients:          " << std::fixed << std::setprecision(3) << avg(jit_fast_gradient_times) << " ms\n";
    std::cout << "    - Chain rule:             " << std::fixed << std::setprecision(3) << avg(jit_fast_chain_rule_times) << " ms\n";
    std::cout << "    - TOTAL (avg):            " << std::fixed << std::setprecision(3) << avg(jit_fast_total_times) << " ms\n";
    std::cout << std::endl;

    // JIT Interpreter (Re-Route) - Full grid with interpreter backend
    std::cout << "  JIT Interpreter (RR) - Full grid + interpreter:\n";
    std::cout << "    - Tape setup:             " << std::fixed << std::setprecision(3) << avg(jit_interp_tape_setup_times) << " ms\n";
    std::cout << "    - Bootstrap forward:      " << std::fixed << std::setprecision(3) << avg(jit_interp_bootstrap_fwd_times) << " ms\n";
    std::cout << "    - Bootstrap backward:     " << std::fixed << std::setprecision(3) << avg(jit_interp_bootstrap_bwd_times) << " ms\n";
    std::cout << "    - Kernel record:          " << std::fixed << std::setprecision(3) << avg(jit_interp_kernel_record_times) << " ms\n";
    std::cout << "    - Kernel compile:         " << std::fixed << std::setprecision(3) << avg(jit_interp_kernel_compile_times) << " ms\n";
    std::cout << "    - Set inputs:             " << std::fixed << std::setprecision(3) << avg(jit_interp_set_inputs_times) << " ms\n";
    std::cout << "    - Kernel exec:            " << std::fixed << std::setprecision(3) << avg(jit_interp_kernel_exec_times) << " ms\n";
    std::cout << "    - Get outputs:            " << std::fixed << std::setprecision(3) << avg(jit_interp_get_outputs_times) << " ms\n";
    std::cout << "    - Get gradients:          " << std::fixed << std::setprecision(3) << avg(jit_interp_gradient_times) << " ms\n";
    std::cout << "    - Chain rule:             " << std::fixed << std::setprecision(3) << avg(jit_interp_chain_rule_times) << " ms\n";
    std::cout << "    - TOTAL (avg):            " << std::fixed << std::setprecision(3) << avg(jit_interp_total_times) << " ms\n";
    std::cout << std::endl;

    // JIT AVX (Re-Route) - Full grid with AVX2 4-path batching
    std::cout << "  JIT AVX (RR) - Full grid + AVX2 4-path batching:\n";
    std::cout << "    - Tape setup:             " << std::fixed << std::setprecision(3) << avg(jit_avx_tape_setup_times) << " ms\n";
    std::cout << "    - Bootstrap forward:      " << std::fixed << std::setprecision(3) << avg(jit_avx_bootstrap_fwd_times) << " ms\n";
    std::cout << "    - Bootstrap backward:     " << std::fixed << std::setprecision(3) << avg(jit_avx_bootstrap_bwd_times) << " ms\n";
    std::cout << "    - Kernel record:          " << std::fixed << std::setprecision(3) << avg(jit_avx_kernel_record_times) << " ms\n";
    std::cout << "    - Kernel compile:         " << std::fixed << std::setprecision(3) << avg(jit_avx_kernel_compile_times) << " ms\n";
    std::cout << "    - Set inputs:             " << std::fixed << std::setprecision(3) << avg(jit_avx_set_inputs_times) << " ms\n";
    std::cout << "    - Kernel exec:            " << std::fixed << std::setprecision(3) << avg(jit_avx_kernel_exec_times) << " ms\n";
    std::cout << "    - Get outputs:            " << std::fixed << std::setprecision(3) << avg(jit_avx_get_outputs_times) << " ms\n";
    std::cout << "    - Get gradients:          " << std::fixed << std::setprecision(3) << avg(jit_avx_gradient_times) << " ms\n";
    std::cout << "    - Chain rule:             " << std::fixed << std::setprecision(3) << avg(jit_avx_chain_rule_times) << " ms\n";
    std::cout << "    - TOTAL (avg):            " << std::fixed << std::setprecision(3) << avg(jit_avx_total_times) << " ms\n";
    std::cout << std::endl;

    // Summary
    double orig_ql_avg = avg(orig_ql_total_times);
    double orig_rr_slow_avg = avg(orig_rr_slow_total_times);
    double bump_ql_avg = avg(bump_ql_total_times);
    double bump_rr_slow_avg = avg(bump_rr_slow_total_times);
    double xad_ql_avg = avg(xad_ql_total_times);
    double xad_rr_slow_avg = avg(xad_rr_slow_total_times);
    double jit_slow_avg = avg(jit_slow_total_times);
    double jit_fast_avg = avg(jit_fast_total_times);
    double jit_interp_avg = avg(jit_interp_total_times);
    double jit_avx_avg = avg(jit_avx_total_times);

    double orig_ql_std = stddev(orig_ql_total_times);
    double orig_rr_slow_std = stddev(orig_rr_slow_total_times);
    double bump_ql_std = stddev(bump_ql_total_times);
    double bump_rr_slow_std = stddev(bump_rr_slow_total_times);
    double xad_ql_std = stddev(xad_ql_total_times);
    double xad_rr_slow_std = stddev(xad_rr_slow_total_times);
    double jit_slow_std = stddev(jit_slow_total_times);
    double jit_fast_std = stddev(jit_fast_total_times);
    double jit_interp_std = stddev(jit_interp_total_times);
    double jit_avx_std = stddev(jit_avx_total_times);

    std::cout << "  " << std::string(80, '-') << "\n";
    std::cout << "  SUMMARY (price + all " << numMarketQuotes << " derivatives):\n";
    std::cout << "    Orig (QuantLib):      " << std::fixed << std::setprecision(3) << orig_ql_avg << " ms (" << orig_ql_std << " ms std) (price only)\n";
    std::cout << "    Orig (RR):       " << std::fixed << std::setprecision(3) << orig_rr_slow_avg << " ms (" << orig_rr_slow_std << " ms std) (price only)\n";
    std::cout << "    Bump (QuantLib):      " << std::fixed << std::setprecision(3) << bump_ql_avg << " ms (" << bump_ql_std << " ms std)\n";
    std::cout << "    Bump (RR):       " << std::fixed << std::setprecision(3) << bump_rr_slow_avg << " ms (" << bump_rr_slow_std << " ms std)\n";
    std::cout << "    XAD (QuantLib):       " << std::fixed << std::setprecision(3) << xad_ql_avg << " ms (" << xad_ql_std << " ms std)";
    if (bump_rr_slow_avg > 0) std::cout << " -> " << std::fixed << std::setprecision(1) << (bump_rr_slow_avg / xad_ql_avg) << "x speedup vs BumpSlow";
    std::cout << "\n";
    std::cout << "    XAD (RR):        " << std::fixed << std::setprecision(3) << xad_rr_slow_avg << " ms (" << xad_rr_slow_std << " ms std)";
    if (bump_rr_slow_avg > 0) std::cout << " -> " << std::fixed << std::setprecision(1) << (bump_rr_slow_avg / xad_rr_slow_avg) << "x speedup vs BumpSlow";
    std::cout << "\n";
    std::cout << "    JIT (RR):        " << std::fixed << std::setprecision(3) << jit_slow_avg << " ms (" << jit_slow_std << " ms std)";
    if (bump_rr_slow_avg > 0) std::cout << " -> " << std::fixed << std::setprecision(1) << (bump_rr_slow_avg / jit_slow_avg) << "x speedup vs BumpSlow";
    std::cout << "\n";
    std::cout << "    JIT Opt (RR):    " << std::fixed << std::setprecision(3) << jit_fast_avg << " ms (" << jit_fast_std << " ms std)";
    if (bump_rr_slow_avg > 0) std::cout << " -> " << std::fixed << std::setprecision(1) << (bump_rr_slow_avg / jit_fast_avg) << "x speedup vs BumpSlow";
    std::cout << "\n";
    std::cout << "    JIT Interp (RR): " << std::fixed << std::setprecision(3) << jit_interp_avg << " ms (" << jit_interp_std << " ms std)";
    if (bump_rr_slow_avg > 0) std::cout << " -> " << std::fixed << std::setprecision(1) << (bump_rr_slow_avg / jit_interp_avg) << "x speedup vs BumpSlow";
    std::cout << "\n";
    std::cout << "    JIT AVX (RR):    " << std::fixed << std::setprecision(3) << jit_avx_avg << " ms (" << jit_avx_std << " ms std)";
    if (bump_rr_slow_avg > 0) std::cout << " -> " << std::fixed << std::setprecision(1) << (bump_rr_slow_avg / jit_avx_avg) << "x speedup vs BumpSlow";
    std::cout << "\n";
    std::cout << "  " << std::string(80, '-') << "\n";

    std::cout << std::endl;
    std::cout << "  STATUS: [STAGE 4 BENCHMARKS COMPLETE]\n";
    std::cout << "\n";
    std::cout << "=============================================================================\n";
    std::cout << std::endl;

    // Basic verification
    BOOST_CHECK(mcPrice_orig_ql > 0.0);
    BOOST_CHECK(mcPrice_orig_rr_slow > 0.0);
    BOOST_CHECK(mcPrice_bump_ql > 0.0);
    BOOST_CHECK(mcPrice_bump_rr_slow > 0.0);
    BOOST_CHECK(orig_rr_slow_match);
    BOOST_CHECK(bump_ql_match);
    BOOST_CHECK(bump_rr_slow_match);
    BOOST_CHECK(xad_ql_match);
    BOOST_CHECK(xad_rr_slow_match);
    BOOST_CHECK(jit_slow_match);
    BOOST_CHECK(jit_fast_match);
    BOOST_CHECK(jit_avx_match);
}

//////////////////////////////////////////////////////////////////////////////
// STAGE 5: Scaling Benchmarks (XAD vs JIT on RR)
//////////////////////////////////////////////////////////////////////////////

BOOST_AUTO_TEST_CASE(testStage5_ScalingBenchmarks)
{
    BOOST_TEST_MESSAGE("Testing Stage 5: Scaling Benchmarks (XAD vs JIT)...");

    std::cout << "\n";
    std::cout << "=============================================================================\n";
    std::cout << "  STAGE 5: SCALING BENCHMARKS (XAD vs JIT on RR)\n";
    std::cout << "=============================================================================\n";
    std::cout << std::endl;

    using Clock = std::chrono::high_resolution_clock;
    using Duration = std::chrono::duration<double, std::milli>;

    // Market data setup (same as Stage 4)
    Calendar calendar = TARGET();
    Date todaysDate(4, September, 2005);
    Settings::instance().evaluationDate() = todaysDate;
    Integer fixingDays = 2;
    Date settlementDate = calendar.adjust(calendar.advance(todaysDate, fixingDays, Days));
    DayCounter dayCounter = Actual360();

    Size numDeposits = 4;
    Size numSwaps = 5;
    std::vector<Period> depoTenors = {1 * Days, 1 * Months, 3 * Months, 6 * Months};
    std::vector<Period> swapTenors = {1 * Years, 2 * Years, 3 * Years, 4 * Years, 5 * Years};

    std::vector<double> depoRates_val = {0.0350, 0.0365, 0.0380, 0.0400};
    std::vector<double> swapRates_val = {0.0420, 0.0480, 0.0520, 0.0550, 0.0575};

    Size numMarketQuotes = numDeposits + numSwaps;

    // LMM parameters
    Size size = 10;
    Size i_opt = 2;
    Size j_opt = 2;
    Size steps = 8;

    // Build base curve and process setup
    std::vector<Rate> baseZeroRates = {0.0350, 0.0575};
    std::vector<Date> baseDates = {settlementDate, settlementDate + 6 * Years};
    auto baseIndex = makeIndex(baseDates, baseZeroRates);

    ext::shared_ptr<LiborForwardModelProcess> baseProcess(
        new LiborForwardModelProcess(size, baseIndex));
    ext::shared_ptr<LmCorrelationModel> baseCorrModel(
        new LmExponentialCorrelationModel(size, 0.5));
    ext::shared_ptr<LmVolatilityModel> baseVolaModel(
        new LmLinearExponentialVolatilityModel(baseProcess->fixingTimes(),
                                               0.291, 1.483, 0.116, 0.00001));
    baseProcess->setCovarParam(ext::shared_ptr<LfmCovarianceParameterization>(
        new LfmCovarianceProxy(baseVolaModel, baseCorrModel)));

    // Grid and timing setup
    std::vector<Time> fixingTimes = baseProcess->fixingTimes();
    TimeGrid grid(fixingTimes.begin(), fixingTimes.end(), steps);

    std::vector<Size> location;
    for (Size idx = 0; idx < fixingTimes.size(); ++idx) {
        location.push_back(
            std::find(grid.begin(), grid.end(), fixingTimes[idx]) - grid.begin());
    }

    Size numFactors = baseProcess->factors();
    Size exerciseStep = location[i_opt];
    Size fullGridSteps = grid.size() - 1;
    Size fullGridRandoms = fullGridSteps * numFactors;

    // Types for MC generators
    typedef PseudoRandom::rsg_type rsg_type;
    typedef MultiPathGenerator<rsg_type>::sample_type sample_type;

    // Get fair swap rate (same approach as Stage 4)
    BusinessDayConvention convention = baseIndex->businessDayConvention();
    Date settlement = baseIndex->forwardingTermStructure()->referenceDate();
    Date fwdStart = settlement + Period(6 * i_opt, Months);
    Date fwdMaturity = fwdStart + Period(6 * j_opt, Months);

    Schedule schedule(fwdStart, fwdMaturity, baseIndex->tenor(), calendar,
                      convention, convention, DateGeneration::Forward, false);

    // Accrual periods
    std::vector<double> accrualStart(size), accrualEnd(size);
    for (Size k = 0; k < size; ++k) {
        accrualStart[k] = value(baseProcess->accrualStartTimes()[k]);
        accrualEnd[k] = value(baseProcess->accrualEndTimes()[k]);
    }

    Size numIntermediates = size + 1;

    // Helper lambda for curve building
    auto buildCurveAndProcess = [&](const std::vector<double>& depoRates,
                                    const std::vector<double>& swapRates,
                                    std::vector<Date>& curveDates,
                                    std::vector<Rate>& zeroRates) {
        RelinkableHandle<YieldTermStructure> euriborTS;
        auto euribor6m = ext::make_shared<Euribor6M>(euriborTS);
        euribor6m->addFixing(Date(2, September, 2005), 0.04);

        std::vector<ext::shared_ptr<RateHelper>> instruments;
        for (Size idx = 0; idx < numDeposits; ++idx) {
            auto depoQuote = ext::make_shared<SimpleQuote>(depoRates[idx]);
            instruments.push_back(ext::make_shared<DepositRateHelper>(
                Handle<Quote>(depoQuote), depoTenors[idx], fixingDays,
                calendar, ModifiedFollowing, true, dayCounter));
        }
        for (Size idx = 0; idx < numSwaps; ++idx) {
            auto swapQuote = ext::make_shared<SimpleQuote>(swapRates[idx]);
            instruments.push_back(ext::make_shared<SwapRateHelper>(
                Handle<Quote>(swapQuote), swapTenors[idx],
                calendar, Annual, Unadjusted, Thirty360(Thirty360::BondBasis),
                euribor6m));
        }

        auto yieldCurve = ext::make_shared<PiecewiseYieldCurve<ZeroYield, Linear>>(
            settlementDate, instruments, dayCounter);
        yieldCurve->enableExtrapolation();

        curveDates.clear();
        zeroRates.clear();
        curveDates.push_back(settlementDate);
        zeroRates.push_back(yieldCurve->zeroRate(settlementDate, dayCounter, Continuous).rate());
        Date endDate = settlementDate + 6 * Years;
        curveDates.push_back(endDate);
        zeroRates.push_back(yieldCurve->zeroRate(endDate, dayCounter, Continuous).rate());
    };

    // Test cases: different path counts
    std::vector<Size> pathCounts = {10, 100, 1000, 10000};
    std::vector<std::string> testCaseNames = {"10", "100", "1K", "10K"};

    // Results storage
    struct ScalingResult {
        double xad_ql_total;
        double xad_rrs_total;
        double jit_rrs_total;
        double jit_interp_total;
        double jit_avx_total;
        double xad_ql_std;
        double xad_rrs_std;
        double jit_rrs_std;
        double jit_interp_std;
        double jit_avx_std;
    };
    std::vector<ScalingResult> results(pathCounts.size());

    Size warmupIterations = 5;
    Size benchmarkIterations = 10;

    std::cout << "  Configuration:\n";
    std::cout << "    - Market inputs: " << numMarketQuotes << " (4 deposits + 5 swaps)\n";
    std::cout << "    - Forward rates: " << size << "\n";
    std::cout << "    - Grid steps: " << fullGridSteps << " (full grid)\n";
    std::cout << "    - Warmup iterations: " << warmupIterations << "\n";
    std::cout << "    - Benchmark iterations: " << benchmarkIterations << "\n";
    std::cout << std::endl;

    for (Size tc = 0; tc < pathCounts.size(); ++tc) {
        Size nrTrails = pathCounts[tc];
        std::cout << "  Running test case: " << testCaseNames[tc] << " (" << nrTrails << " paths)..." << std::flush;

        // Pre-generate random numbers for this path count
        rsg_type rsg_base = PseudoRandom::make_sequence_generator(fullGridRandoms, BigNatural(42));
        std::vector<std::vector<double>> allRandoms(nrTrails);
        for (Size n = 0; n < nrTrails; ++n) {
            allRandoms[n].resize(fullGridRandoms);
            const auto& seq = rsg_base.nextSequence();
            for (Size m = 0; m < fullGridRandoms; ++m) {
                allRandoms[n][m] = value(seq.value[m]);
            }
        }

        // Timing accumulators
        std::vector<double> xad_ql_times, xad_rrs_times, jit_rrs_times, jit_interp_times, jit_avx_times;

        for (Size iter = 0; iter < warmupIterations + benchmarkIterations; ++iter) {
            bool recordTiming = (iter >= warmupIterations);

            // =================================================================
            // XAD (QuantLib) - MultiPathGenerator with XAD tape
            // =================================================================
            {
                auto t_start = Clock::now();

                using tape_type = Real::tape_type;
                tape_type tape;

                std::vector<Real> depositRates(numDeposits);
                std::vector<Real> swapRates(numSwaps);
                for (Size idx = 0; idx < numDeposits; ++idx) depositRates[idx] = depoRates_val[idx];
                for (Size idx = 0; idx < numSwaps; ++idx) swapRates[idx] = swapRates_val[idx];

                tape.registerInputs(depositRates);
                tape.registerInputs(swapRates);
                tape.newRecording();

                std::vector<Date> curveDates;
                std::vector<Rate> zeroRates;
                std::vector<double> depoRates_dbl(numDeposits), swapRates_dbl(numSwaps);
                for (Size idx = 0; idx < numDeposits; ++idx) depoRates_dbl[idx] = value(depositRates[idx]);
                for (Size idx = 0; idx < numSwaps; ++idx) swapRates_dbl[idx] = value(swapRates[idx]);
                buildCurveAndProcess(depoRates_dbl, swapRates_dbl, curveDates, zeroRates);

                RelinkableHandle<YieldTermStructure> termStructure;
                ext::shared_ptr<IborIndex> index(new Euribor6M(termStructure));
                index->addFixing(Date(2, September, 2005), 0.04);

                std::vector<Real> zeroRates_real(zeroRates.begin(), zeroRates.end());
                termStructure.linkTo(ext::make_shared<ZeroCurve>(curveDates, zeroRates_real, dayCounter));

                ext::shared_ptr<LiborForwardModelProcess> process(
                    new LiborForwardModelProcess(size, index));
                process->setCovarParam(ext::shared_ptr<LfmCovarianceParameterization>(
                    new LfmCovarianceProxy(
                        ext::make_shared<LmLinearExponentialVolatilityModel>(process->fixingTimes(), 0.291, 1.483, 0.116, 0.00001),
                        ext::make_shared<LmExponentialCorrelationModel>(size, 0.5))));

                ext::shared_ptr<VanillaSwap> fwdSwap(
                    new VanillaSwap(Swap::Receiver, 1.0,
                                    schedule, 0.05, dayCounter,
                                    schedule, index, 0.0, index->dayCounter()));
                fwdSwap->setPricingEngine(ext::make_shared<DiscountingSwapEngine>(
                    index->forwardingTermStructure()));
                Real swapRate = fwdSwap->fairRate();

                rsg_type rsg_ql = PseudoRandom::make_sequence_generator(fullGridRandoms, BigNatural(42));
                MultiPathGenerator<rsg_type> generator_ql(process, grid, rsg_ql, false);

                Real price = Real(0.0);
                for (Size n = 0; n < nrTrails; ++n) {
                    sample_type path = (n % 2) != 0U ? generator_ql.antithetic() : generator_ql.next();

                    Array mcRates(size);
                    for (Size k = 0; k < size; ++k) {
                        mcRates[k] = path.value[k][exerciseStep];
                    }

                    Array dis(size);
                    Real df = Real(1.0);
                    for (Size k = 0; k < size; ++k) {
                        Real accrual = accrualEnd[k] - accrualStart[k];
                        df = df / (Real(1.0) + mcRates[k] * accrual);
                        dis[k] = df;
                    }

                    Real npv = Real(0.0);
                    for (Size m = i_opt; m < i_opt + j_opt; ++m) {
                        Real accrual = accrualEnd[m] - accrualStart[m];
                        npv += (swapRate - mcRates[m]) * accrual * dis[m];
                    }
                    price += max(npv, Real(0.0));
                }
                price /= Real(static_cast<double>(nrTrails));

                tape.registerOutput(price);
                derivative(price) = 1.0;
                tape.computeAdjoints();

                auto t_end = Clock::now();
                if (recordTiming) {
                    xad_ql_times.push_back(Duration(t_end - t_start).count());
                }
                tape.deactivate();
            }

            // =================================================================
            // XAD (RR) - Full grid with XAD tape
            // =================================================================
            {
                auto t_start = Clock::now();

                using tape_type = Real::tape_type;
                tape_type tape;

                std::vector<Real> depositRates(numDeposits);
                std::vector<Real> swapRates(numSwaps);
                for (Size idx = 0; idx < numDeposits; ++idx) depositRates[idx] = depoRates_val[idx];
                for (Size idx = 0; idx < numSwaps; ++idx) swapRates[idx] = swapRates_val[idx];

                tape.registerInputs(depositRates);
                tape.registerInputs(swapRates);
                tape.newRecording();

                RelinkableHandle<YieldTermStructure> euriborTS;
                auto euribor6m = ext::make_shared<Euribor6M>(euriborTS);
                euribor6m->addFixing(Date(2, September, 2005), 0.04);

                std::vector<ext::shared_ptr<RateHelper>> instruments;
                for (Size idx = 0; idx < numDeposits; ++idx) {
                    auto depoQuote = ext::make_shared<SimpleQuote>(depositRates[idx]);
                    instruments.push_back(ext::make_shared<DepositRateHelper>(
                        Handle<Quote>(depoQuote), depoTenors[idx], fixingDays,
                        calendar, ModifiedFollowing, true, dayCounter));
                }
                for (Size idx = 0; idx < numSwaps; ++idx) {
                    auto swapQuote = ext::make_shared<SimpleQuote>(swapRates[idx]);
                    instruments.push_back(ext::make_shared<SwapRateHelper>(
                        Handle<Quote>(swapQuote), swapTenors[idx],
                        calendar, Annual, Unadjusted, Thirty360(Thirty360::BondBasis),
                        euribor6m));
                }

                auto yieldCurve = ext::make_shared<PiecewiseYieldCurve<ZeroYield, Linear>>(
                    settlementDate, instruments, dayCounter);
                yieldCurve->enableExtrapolation();

                std::vector<Date> curveDates;
                std::vector<Real> zeroRates;
                curveDates.push_back(settlementDate);
                zeroRates.push_back(yieldCurve->zeroRate(settlementDate, dayCounter, Continuous).rate());
                Date endDate = settlementDate + 6 * Years;
                curveDates.push_back(endDate);
                zeroRates.push_back(yieldCurve->zeroRate(endDate, dayCounter, Continuous).rate());

                std::vector<Rate> zeroRates_ql;
                for (const auto& r : zeroRates) zeroRates_ql.push_back(r);

                RelinkableHandle<YieldTermStructure> termStructure;
                ext::shared_ptr<IborIndex> index(new Euribor6M(termStructure));
                index->addFixing(Date(2, September, 2005), 0.04);
                termStructure.linkTo(ext::make_shared<ZeroCurve>(curveDates, zeroRates_ql, dayCounter));

                ext::shared_ptr<LiborForwardModelProcess> process(
                    new LiborForwardModelProcess(size, index));
                process->setCovarParam(ext::shared_ptr<LfmCovarianceParameterization>(
                    new LfmCovarianceProxy(
                        ext::make_shared<LmLinearExponentialVolatilityModel>(process->fixingTimes(), 0.291, 1.483, 0.116, 0.00001),
                        ext::make_shared<LmExponentialCorrelationModel>(size, 0.5))));

                ext::shared_ptr<VanillaSwap> fwdSwap(
                    new VanillaSwap(Swap::Receiver, 1.0,
                                    schedule, 0.05, dayCounter,
                                    schedule, index, 0.0, index->dayCounter()));
                fwdSwap->setPricingEngine(ext::make_shared<DiscountingSwapEngine>(
                    index->forwardingTermStructure()));
                Real swapRate = fwdSwap->fairRate();

                Array initRates = process->initialValues();

                Real price = Real(0.0);
                for (Size n = 0; n < nrTrails; ++n) {
                    Array asset(size);
                    for (Size k = 0; k < size; ++k) asset[k] = initRates[k];

                    Array assetAtExercise(size);
                    for (Size step = 1; step <= fullGridSteps; ++step) {
                        Size offset = (step - 1) * numFactors;
                        Time t = grid[step - 1];
                        Time dt = grid.dt(step - 1);

                        Array dw(numFactors);
                        for (Size f = 0; f < numFactors; ++f) {
                            dw[f] = allRandoms[n][offset + f];
                        }

                        asset = process->evolve(t, asset, dt, dw);

                        if (step == exerciseStep) {
                            for (Size k = 0; k < size; ++k) assetAtExercise[k] = asset[k];
                        }
                    }

                    Array dis(size);
                    Real df = Real(1.0);
                    for (Size k = 0; k < size; ++k) {
                        Real accrual = accrualEnd[k] - accrualStart[k];
                        df = df / (Real(1.0) + assetAtExercise[k] * accrual);
                        dis[k] = df;
                    }

                    Real npv = Real(0.0);
                    for (Size m = i_opt; m < i_opt + j_opt; ++m) {
                        Real accrual = accrualEnd[m] - accrualStart[m];
                        npv += (swapRate - assetAtExercise[m]) * accrual * dis[m];
                    }
                    price += max(npv, Real(0.0));
                }
                price /= Real(static_cast<double>(nrTrails));

                tape.registerOutput(price);
                derivative(price) = 1.0;
                tape.computeAdjoints();

                auto t_end = Clock::now();
                if (recordTiming) {
                    xad_rrs_times.push_back(Duration(t_end - t_start).count());
                }
                tape.deactivate();
            }

            // =================================================================
            // JIT (RR) - ForgeBackend with full grid
            // =================================================================
            {
                auto t_start = Clock::now();

                using tape_type = Real::tape_type;
                tape_type tape;

                std::vector<Real> depositRates(numDeposits);
                std::vector<Real> swapRates(numSwaps);
                for (Size idx = 0; idx < numDeposits; ++idx) depositRates[idx] = depoRates_val[idx];
                for (Size idx = 0; idx < numSwaps; ++idx) swapRates[idx] = swapRates_val[idx];

                tape.registerInputs(depositRates);
                tape.registerInputs(swapRates);
                tape.newRecording();

                RelinkableHandle<YieldTermStructure> euriborTS;
                auto euribor6m = ext::make_shared<Euribor6M>(euriborTS);
                euribor6m->addFixing(Date(2, September, 2005), 0.04);

                std::vector<ext::shared_ptr<RateHelper>> instruments;
                for (Size idx = 0; idx < numDeposits; ++idx) {
                    auto depoQuote = ext::make_shared<SimpleQuote>(depositRates[idx]);
                    instruments.push_back(ext::make_shared<DepositRateHelper>(
                        Handle<Quote>(depoQuote), depoTenors[idx], fixingDays,
                        calendar, ModifiedFollowing, true, dayCounter));
                }
                for (Size idx = 0; idx < numSwaps; ++idx) {
                    auto swapQuote = ext::make_shared<SimpleQuote>(swapRates[idx]);
                    instruments.push_back(ext::make_shared<SwapRateHelper>(
                        Handle<Quote>(swapQuote), swapTenors[idx],
                        calendar, Annual, Unadjusted, Thirty360(Thirty360::BondBasis),
                        euribor6m));
                }

                auto yieldCurve = ext::make_shared<PiecewiseYieldCurve<ZeroYield, Linear>>(
                    settlementDate, instruments, dayCounter);
                yieldCurve->enableExtrapolation();

                std::vector<Date> curveDates;
                std::vector<Real> zeroRates;
                curveDates.push_back(settlementDate);
                zeroRates.push_back(yieldCurve->zeroRate(settlementDate, dayCounter, Continuous).rate());
                Date endDate = settlementDate + 6 * Years;
                curveDates.push_back(endDate);
                zeroRates.push_back(yieldCurve->zeroRate(endDate, dayCounter, Continuous).rate());

                std::vector<Rate> zeroRates_ql;
                for (const auto& r : zeroRates) zeroRates_ql.push_back(r);

                RelinkableHandle<YieldTermStructure> termStructure;
                ext::shared_ptr<IborIndex> index(new Euribor6M(termStructure));
                index->addFixing(Date(2, September, 2005), 0.04);
                termStructure.linkTo(ext::make_shared<ZeroCurve>(curveDates, zeroRates_ql, dayCounter));

                ext::shared_ptr<LiborForwardModelProcess> process(
                    new LiborForwardModelProcess(size, index));
                process->setCovarParam(ext::shared_ptr<LfmCovarianceParameterization>(
                    new LfmCovarianceProxy(
                        ext::make_shared<LmLinearExponentialVolatilityModel>(process->fixingTimes(), 0.291, 1.483, 0.116, 0.00001),
                        ext::make_shared<LmExponentialCorrelationModel>(size, 0.5))));

                ext::shared_ptr<VanillaSwap> fwdSwap(
                    new VanillaSwap(Swap::Receiver, 1.0,
                                    schedule, 0.05, dayCounter,
                                    schedule, index, 0.0, index->dayCounter()));
                fwdSwap->setPricingEngine(ext::make_shared<DiscountingSwapEngine>(
                    index->forwardingTermStructure()));
                Real swapRate_tape = fwdSwap->fairRate();

                Array initRates = process->initialValues();

                // Jacobian computation
                std::vector<double> jacobian(numIntermediates * numMarketQuotes, 0.0);
                for (Size k = 0; k < size; ++k) {
                    if (initRates[k].shouldRecord()) {
                        tape.clearDerivatives();
                        tape.registerOutput(initRates[k]);
                        derivative(initRates[k]) = 1.0;
                        tape.computeAdjoints();

                        double* jac_row = jacobian.data() + k * numMarketQuotes;
                        for (Size m = 0; m < numDeposits; ++m)
                            jac_row[m] = derivative(depositRates[m]);
                        for (Size m = 0; m < numSwaps; ++m)
                            jac_row[numDeposits + m] = derivative(swapRates[m]);
                    }
                }
                if (swapRate_tape.shouldRecord()) {
                    tape.clearDerivatives();
                    tape.registerOutput(swapRate_tape);
                    derivative(swapRate_tape) = 1.0;
                    tape.computeAdjoints();

                    double* jac_row = jacobian.data() + size * numMarketQuotes;
                    for (Size m = 0; m < numDeposits; ++m)
                        jac_row[m] = derivative(depositRates[m]);
                    for (Size m = 0; m < numSwaps; ++m)
                        jac_row[numDeposits + m] = derivative(swapRates[m]);
                }
                tape.deactivate();

                // JIT kernel creation
                auto forgeBackend = std::make_unique<xad::forge::ForgeBackend<double>>(false);
                xad::JITCompiler<double> jit(std::move(forgeBackend));

                std::vector<xad::AD> jit_initRates(size);
                xad::AD jit_swapRate;
                std::vector<xad::AD> jit_randoms(fullGridRandoms);

                for (Size k = 0; k < size; ++k) {
                    jit_initRates[k] = xad::AD(value(initRates[k]));
                    jit.registerInput(jit_initRates[k]);
                }
                jit_swapRate = xad::AD(value(swapRate_tape));
                jit.registerInput(jit_swapRate);
                for (Size m = 0; m < fullGridRandoms; ++m) {
                    jit_randoms[m] = xad::AD(0.0);
                    jit.registerInput(jit_randoms[m]);
                }

                jit.newRecording();

                std::vector<xad::AD> asset_jit(size);
                std::vector<xad::AD> assetAtExercise_jit(size);
                for (Size k = 0; k < size; ++k) asset_jit[k] = jit_initRates[k];

                for (Size step = 1; step <= fullGridSteps; ++step) {
                    Size offset = (step - 1) * numFactors;
                    Time t = grid[step - 1];
                    Time dt = grid.dt(step - 1);

                    Array dw(numFactors);
                    for (Size f = 0; f < numFactors; ++f) dw[f] = jit_randoms[offset + f];

                    Array asset_arr(size);
                    for (Size k = 0; k < size; ++k) asset_arr[k] = asset_jit[k];

                    Array evolved = process->evolve(t, asset_arr, dt, dw);
                    for (Size k = 0; k < size; ++k) asset_jit[k] = evolved[k];

                    if (step == exerciseStep) {
                        for (Size k = 0; k < size; ++k) assetAtExercise_jit[k] = asset_jit[k];
                    }
                }

                std::vector<xad::AD> dis_jit(size);
                xad::AD df_jit = xad::AD(1.0);
                for (Size k = 0; k < size; ++k) {
                    double accrual = accrualEnd[k] - accrualStart[k];
                    df_jit = df_jit / (xad::AD(1.0) + assetAtExercise_jit[k] * accrual);
                    dis_jit[k] = df_jit;
                }

                xad::AD jit_npv = xad::AD(0.0);
                for (Size m = i_opt; m < i_opt + j_opt; ++m) {
                    double accrual = accrualEnd[m] - accrualStart[m];
                    jit_npv = jit_npv + (jit_swapRate - assetAtExercise_jit[m]) * accrual * dis_jit[m];
                }

                xad::AD jit_payoff = xad::less(jit_npv, xad::AD(0.0)).If(xad::AD(0.0), jit_npv);
                jit.registerOutput(jit_payoff);

                // Compile the JIT kernel before MC loop
                jit.compile();

                // MC execution
                double mcPrice = 0.0;
                std::vector<double> dPrice_dInitRates(size, 0.0);
                double dPrice_dSwapRate = 0.0;

                const auto& graph = jit.getGraph();
                uint32_t outputSlot = graph.output_ids[0];

                for (Size n = 0; n < nrTrails; ++n) {
                    for (Size k = 0; k < size; ++k) value(jit_initRates[k]) = value(initRates[k]);
                    value(jit_swapRate) = value(swapRate_tape);
                    for (Size m = 0; m < fullGridRandoms; ++m) value(jit_randoms[m]) = allRandoms[n][m];

                    double payoff_value;
                    jit.forward(&payoff_value);
                    mcPrice += payoff_value;

                    jit.clearDerivatives();
                    jit.setDerivative(outputSlot, 1.0);
                    jit.computeAdjoints();

                    for (Size k = 0; k < size; ++k) dPrice_dInitRates[k] += jit.derivative(graph.input_ids[k]);
                    dPrice_dSwapRate += jit.derivative(graph.input_ids[size]);
                }

                mcPrice /= static_cast<double>(nrTrails);
                for (Size k = 0; k < size; ++k) dPrice_dInitRates[k] /= static_cast<double>(nrTrails);
                dPrice_dSwapRate /= static_cast<double>(nrTrails);

                // Chain rule
                std::vector<double> dPrice_dIntermediates(numIntermediates);
                for (Size k = 0; k < size; ++k) dPrice_dIntermediates[k] = dPrice_dInitRates[k];
                dPrice_dIntermediates[size] = dPrice_dSwapRate;

                std::vector<double> dPrice_market(numMarketQuotes);
                applyChainRule(jacobian.data(), dPrice_dIntermediates.data(), dPrice_market.data(),
                               numIntermediates, numMarketQuotes);

                auto t_end = Clock::now();
                if (recordTiming) {
                    jit_rrs_times.push_back(Duration(t_end - t_start).count());
                }
            }

            // =================================================================
            // JIT Interpreter (RR) - JITGraphInterpreter with full grid
            // =================================================================
            {
                auto t_start = Clock::now();

                using tape_type = Real::tape_type;
                tape_type tape;

                std::vector<Real> depositRates(numDeposits);
                std::vector<Real> swapRates(numSwaps);
                for (Size idx = 0; idx < numDeposits; ++idx) depositRates[idx] = depoRates_val[idx];
                for (Size idx = 0; idx < numSwaps; ++idx) swapRates[idx] = swapRates_val[idx];

                tape.registerInputs(depositRates);
                tape.registerInputs(swapRates);
                tape.newRecording();

                RelinkableHandle<YieldTermStructure> euriborTS;
                auto euribor6m = ext::make_shared<Euribor6M>(euriborTS);
                euribor6m->addFixing(Date(2, September, 2005), 0.04);

                std::vector<ext::shared_ptr<RateHelper>> instruments;
                for (Size idx = 0; idx < numDeposits; ++idx) {
                    auto depoQuote = ext::make_shared<SimpleQuote>(depositRates[idx]);
                    instruments.push_back(ext::make_shared<DepositRateHelper>(
                        Handle<Quote>(depoQuote), depoTenors[idx], fixingDays,
                        calendar, ModifiedFollowing, true, dayCounter));
                }
                for (Size idx = 0; idx < numSwaps; ++idx) {
                    auto swapQuote = ext::make_shared<SimpleQuote>(swapRates[idx]);
                    instruments.push_back(ext::make_shared<SwapRateHelper>(
                        Handle<Quote>(swapQuote), swapTenors[idx],
                        calendar, Annual, Unadjusted, Thirty360(Thirty360::BondBasis),
                        euribor6m));
                }

                auto yieldCurve = ext::make_shared<PiecewiseYieldCurve<ZeroYield, Linear>>(
                    settlementDate, instruments, dayCounter);
                yieldCurve->enableExtrapolation();

                std::vector<Date> curveDates;
                std::vector<Real> zeroRates;
                curveDates.push_back(settlementDate);
                zeroRates.push_back(yieldCurve->zeroRate(settlementDate, dayCounter, Continuous).rate());
                Date endDate = settlementDate + 6 * Years;
                curveDates.push_back(endDate);
                zeroRates.push_back(yieldCurve->zeroRate(endDate, dayCounter, Continuous).rate());

                std::vector<Rate> zeroRates_ql;
                for (const auto& r : zeroRates) zeroRates_ql.push_back(r);

                RelinkableHandle<YieldTermStructure> termStructure;
                ext::shared_ptr<IborIndex> index(new Euribor6M(termStructure));
                index->addFixing(Date(2, September, 2005), 0.04);
                termStructure.linkTo(ext::make_shared<ZeroCurve>(curveDates, zeroRates_ql, dayCounter));

                ext::shared_ptr<LiborForwardModelProcess> process(
                    new LiborForwardModelProcess(size, index));
                process->setCovarParam(ext::shared_ptr<LfmCovarianceParameterization>(
                    new LfmCovarianceProxy(
                        ext::make_shared<LmLinearExponentialVolatilityModel>(process->fixingTimes(), 0.291, 1.483, 0.116, 0.00001),
                        ext::make_shared<LmExponentialCorrelationModel>(size, 0.5))));

                ext::shared_ptr<VanillaSwap> fwdSwap(
                    new VanillaSwap(Swap::Receiver, 1.0,
                                    schedule, 0.05, dayCounter,
                                    schedule, index, 0.0, index->dayCounter()));
                fwdSwap->setPricingEngine(ext::make_shared<DiscountingSwapEngine>(
                    index->forwardingTermStructure()));
                Real swapRate_tape = fwdSwap->fairRate();

                Array initRates = process->initialValues();

                // Jacobian computation
                std::vector<double> jacobian(numIntermediates * numMarketQuotes, 0.0);
                for (Size k = 0; k < size; ++k) {
                    if (initRates[k].shouldRecord()) {
                        tape.clearDerivatives();
                        tape.registerOutput(initRates[k]);
                        derivative(initRates[k]) = 1.0;
                        tape.computeAdjoints();

                        double* jac_row = jacobian.data() + k * numMarketQuotes;
                        for (Size m = 0; m < numDeposits; ++m)
                            jac_row[m] = derivative(depositRates[m]);
                        for (Size m = 0; m < numSwaps; ++m)
                            jac_row[numDeposits + m] = derivative(swapRates[m]);
                    }
                }
                if (swapRate_tape.shouldRecord()) {
                    tape.clearDerivatives();
                    tape.registerOutput(swapRate_tape);
                    derivative(swapRate_tape) = 1.0;
                    tape.computeAdjoints();

                    double* jac_row = jacobian.data() + size * numMarketQuotes;
                    for (Size m = 0; m < numDeposits; ++m)
                        jac_row[m] = derivative(depositRates[m]);
                    for (Size m = 0; m < numSwaps; ++m)
                        jac_row[numDeposits + m] = derivative(swapRates[m]);
                }
                tape.deactivate();

                // JIT kernel creation with interpreter
                xad::JITCompiler<double> jit;  // Default: JITGraphInterpreter

                std::vector<xad::AD> jit_initRates(size);
                xad::AD jit_swapRate;
                std::vector<xad::AD> jit_randoms(fullGridRandoms);

                for (Size k = 0; k < size; ++k) {
                    jit_initRates[k] = xad::AD(value(initRates[k]));
                    jit.registerInput(jit_initRates[k]);
                }
                jit_swapRate = xad::AD(value(swapRate_tape));
                jit.registerInput(jit_swapRate);
                for (Size m = 0; m < fullGridRandoms; ++m) {
                    jit_randoms[m] = xad::AD(0.0);
                    jit.registerInput(jit_randoms[m]);
                }

                jit.newRecording();

                std::vector<xad::AD> asset_jit(size);
                std::vector<xad::AD> assetAtExercise_jit(size);
                for (Size k = 0; k < size; ++k) asset_jit[k] = jit_initRates[k];

                for (Size step = 1; step <= fullGridSteps; ++step) {
                    Size offset = (step - 1) * numFactors;
                    Time t = grid[step - 1];
                    Time dt = grid.dt(step - 1);

                    Array dw(numFactors);
                    for (Size f = 0; f < numFactors; ++f) dw[f] = jit_randoms[offset + f];

                    Array asset_arr(size);
                    for (Size k = 0; k < size; ++k) asset_arr[k] = asset_jit[k];

                    Array evolved = process->evolve(t, asset_arr, dt, dw);
                    for (Size k = 0; k < size; ++k) asset_jit[k] = evolved[k];

                    if (step == exerciseStep) {
                        for (Size k = 0; k < size; ++k) assetAtExercise_jit[k] = asset_jit[k];
                    }
                }

                std::vector<xad::AD> dis_jit(size);
                xad::AD df_jit = xad::AD(1.0);
                for (Size k = 0; k < size; ++k) {
                    double accrual = accrualEnd[k] - accrualStart[k];
                    df_jit = df_jit / (xad::AD(1.0) + assetAtExercise_jit[k] * accrual);
                    dis_jit[k] = df_jit;
                }

                xad::AD jit_npv = xad::AD(0.0);
                for (Size m = i_opt; m < i_opt + j_opt; ++m) {
                    double accrual = accrualEnd[m] - accrualStart[m];
                    jit_npv = jit_npv + (jit_swapRate - assetAtExercise_jit[m]) * accrual * dis_jit[m];
                }

                xad::AD jit_payoff = xad::less(jit_npv, xad::AD(0.0)).If(xad::AD(0.0), jit_npv);
                jit.registerOutput(jit_payoff);

                // Compile the JIT kernel before MC loop
                jit.compile();

                // MC execution
                double mcPrice = 0.0;
                std::vector<double> dPrice_dInitRates(size, 0.0);
                double dPrice_dSwapRate = 0.0;

                const auto& graph = jit.getGraph();
                uint32_t outputSlot = graph.output_ids[0];

                for (Size n = 0; n < nrTrails; ++n) {
                    for (Size k = 0; k < size; ++k) value(jit_initRates[k]) = value(initRates[k]);
                    value(jit_swapRate) = value(swapRate_tape);
                    for (Size m = 0; m < fullGridRandoms; ++m) value(jit_randoms[m]) = allRandoms[n][m];

                    double payoff_value;
                    jit.forward(&payoff_value);
                    mcPrice += payoff_value;

                    jit.clearDerivatives();
                    jit.setDerivative(outputSlot, 1.0);
                    jit.computeAdjoints();

                    for (Size k = 0; k < size; ++k) dPrice_dInitRates[k] += jit.derivative(graph.input_ids[k]);
                    dPrice_dSwapRate += jit.derivative(graph.input_ids[size]);
                }

                mcPrice /= static_cast<double>(nrTrails);
                for (Size k = 0; k < size; ++k) dPrice_dInitRates[k] /= static_cast<double>(nrTrails);
                dPrice_dSwapRate /= static_cast<double>(nrTrails);

                // Chain rule
                std::vector<double> dPrice_dIntermediates(numIntermediates);
                for (Size k = 0; k < size; ++k) dPrice_dIntermediates[k] = dPrice_dInitRates[k];
                dPrice_dIntermediates[size] = dPrice_dSwapRate;

                std::vector<double> dPrice_market(numMarketQuotes);
                applyChainRule(jacobian.data(), dPrice_dIntermediates.data(), dPrice_market.data(),
                               numIntermediates, numMarketQuotes);

                auto t_end = Clock::now();
                if (recordTiming) {
                    jit_interp_times.push_back(Duration(t_end - t_start).count());
                }
            }

            // =================================================================
            // JIT AVX (RR) - ForgeBackendAVX with 4-path batching
            // =================================================================
            {
                auto t_start = Clock::now();

                using tape_type = Real::tape_type;
                tape_type tape;

                std::vector<Real> depositRates(numDeposits);
                std::vector<Real> swapRates(numSwaps);
                for (Size idx = 0; idx < numDeposits; ++idx) depositRates[idx] = depoRates_val[idx];
                for (Size idx = 0; idx < numSwaps; ++idx) swapRates[idx] = swapRates_val[idx];

                tape.registerInputs(depositRates);
                tape.registerInputs(swapRates);
                tape.newRecording();

                RelinkableHandle<YieldTermStructure> euriborTS;
                auto euribor6m = ext::make_shared<Euribor6M>(euriborTS);
                euribor6m->addFixing(Date(2, September, 2005), 0.04);

                std::vector<ext::shared_ptr<RateHelper>> instruments;
                for (Size idx = 0; idx < numDeposits; ++idx) {
                    auto depoQuote = ext::make_shared<SimpleQuote>(depositRates[idx]);
                    instruments.push_back(ext::make_shared<DepositRateHelper>(
                        Handle<Quote>(depoQuote), depoTenors[idx], fixingDays,
                        calendar, ModifiedFollowing, true, dayCounter));
                }
                for (Size idx = 0; idx < numSwaps; ++idx) {
                    auto swapQuote = ext::make_shared<SimpleQuote>(swapRates[idx]);
                    instruments.push_back(ext::make_shared<SwapRateHelper>(
                        Handle<Quote>(swapQuote), swapTenors[idx],
                        calendar, Annual, Unadjusted, Thirty360(Thirty360::BondBasis),
                        euribor6m));
                }

                auto yieldCurve = ext::make_shared<PiecewiseYieldCurve<ZeroYield, Linear>>(
                    settlementDate, instruments, dayCounter);
                yieldCurve->enableExtrapolation();

                std::vector<Date> curveDates;
                std::vector<Real> zeroRates;
                curveDates.push_back(settlementDate);
                zeroRates.push_back(yieldCurve->zeroRate(settlementDate, dayCounter, Continuous).rate());
                Date endDate = settlementDate + 6 * Years;
                curveDates.push_back(endDate);
                zeroRates.push_back(yieldCurve->zeroRate(endDate, dayCounter, Continuous).rate());

                std::vector<Rate> zeroRates_ql;
                for (const auto& r : zeroRates) zeroRates_ql.push_back(r);

                RelinkableHandle<YieldTermStructure> termStructure;
                ext::shared_ptr<IborIndex> index(new Euribor6M(termStructure));
                index->addFixing(Date(2, September, 2005), 0.04);
                termStructure.linkTo(ext::make_shared<ZeroCurve>(curveDates, zeroRates_ql, dayCounter));

                ext::shared_ptr<LiborForwardModelProcess> process(
                    new LiborForwardModelProcess(size, index));
                process->setCovarParam(ext::shared_ptr<LfmCovarianceParameterization>(
                    new LfmCovarianceProxy(
                        ext::make_shared<LmLinearExponentialVolatilityModel>(process->fixingTimes(), 0.291, 1.483, 0.116, 0.00001),
                        ext::make_shared<LmExponentialCorrelationModel>(size, 0.5))));

                ext::shared_ptr<VanillaSwap> fwdSwap(
                    new VanillaSwap(Swap::Receiver, 1.0,
                                    schedule, 0.05, dayCounter,
                                    schedule, index, 0.0, index->dayCounter()));
                fwdSwap->setPricingEngine(ext::make_shared<DiscountingSwapEngine>(
                    index->forwardingTermStructure()));
                Real swapRate_tape = fwdSwap->fairRate();

                Array initRates = process->initialValues();

                // Jacobian computation
                std::vector<double> jacobian(numIntermediates * numMarketQuotes, 0.0);
                for (Size k = 0; k < size; ++k) {
                    if (initRates[k].shouldRecord()) {
                        tape.clearDerivatives();
                        tape.registerOutput(initRates[k]);
                        derivative(initRates[k]) = 1.0;
                        tape.computeAdjoints();

                        double* jac_row = jacobian.data() + k * numMarketQuotes;
                        for (Size m = 0; m < numDeposits; ++m)
                            jac_row[m] = derivative(depositRates[m]);
                        for (Size m = 0; m < numSwaps; ++m)
                            jac_row[numDeposits + m] = derivative(swapRates[m]);
                    }
                }
                if (swapRate_tape.shouldRecord()) {
                    tape.clearDerivatives();
                    tape.registerOutput(swapRate_tape);
                    derivative(swapRate_tape) = 1.0;
                    tape.computeAdjoints();

                    double* jac_row = jacobian.data() + size * numMarketQuotes;
                    for (Size m = 0; m < numDeposits; ++m)
                        jac_row[m] = derivative(depositRates[m]);
                    for (Size m = 0; m < numSwaps; ++m)
                        jac_row[numDeposits + m] = derivative(swapRates[m]);
                }
                tape.deactivate();

                // JIT kernel creation for AVX
                xad::JITCompiler<double> jit;

                std::vector<xad::AD> jit_initRates(size);
                xad::AD jit_swapRate;
                std::vector<xad::AD> jit_randoms(fullGridRandoms);

                for (Size k = 0; k < size; ++k) {
                    jit_initRates[k] = xad::AD(value(initRates[k]));
                    jit.registerInput(jit_initRates[k]);
                }
                jit_swapRate = xad::AD(value(swapRate_tape));
                jit.registerInput(jit_swapRate);
                for (Size m = 0; m < fullGridRandoms; ++m) {
                    jit_randoms[m] = xad::AD(0.0);
                    jit.registerInput(jit_randoms[m]);
                }

                jit.newRecording();

                std::vector<xad::AD> asset_jit(size);
                std::vector<xad::AD> assetAtExercise_jit(size);
                for (Size k = 0; k < size; ++k) asset_jit[k] = jit_initRates[k];

                for (Size step = 1; step <= fullGridSteps; ++step) {
                    Size offset = (step - 1) * numFactors;
                    Time t = grid[step - 1];
                    Time dt = grid.dt(step - 1);

                    Array dw(numFactors);
                    for (Size f = 0; f < numFactors; ++f) dw[f] = jit_randoms[offset + f];

                    Array asset_arr(size);
                    for (Size k = 0; k < size; ++k) asset_arr[k] = asset_jit[k];

                    Array evolved = process->evolve(t, asset_arr, dt, dw);
                    for (Size k = 0; k < size; ++k) asset_jit[k] = evolved[k];

                    if (step == exerciseStep) {
                        for (Size k = 0; k < size; ++k) assetAtExercise_jit[k] = asset_jit[k];
                    }
                }

                std::vector<xad::AD> dis_jit(size);
                xad::AD df_jit = xad::AD(1.0);
                for (Size k = 0; k < size; ++k) {
                    double accrual = accrualEnd[k] - accrualStart[k];
                    df_jit = df_jit / (xad::AD(1.0) + assetAtExercise_jit[k] * accrual);
                    dis_jit[k] = df_jit;
                }

                xad::AD jit_npv = xad::AD(0.0);
                for (Size m = i_opt; m < i_opt + j_opt; ++m) {
                    double accrual = accrualEnd[m] - accrualStart[m];
                    jit_npv = jit_npv + (jit_swapRate - assetAtExercise_jit[m]) * accrual * dis_jit[m];
                }

                xad::AD jit_payoff = xad::less(jit_npv, xad::AD(0.0)).If(xad::AD(0.0), jit_npv);
                jit.registerOutput(jit_payoff);

                // Get the JIT graph
                const auto& jitGraph = jit.getGraph();

                // Deactivate jit - we still use it for adjoints but don't need it active
                jit.deactivate();

                // AVX backend with 4-path batching - compiles directly from JITGraph
                xad::forge::ForgeBackendAVX<double> avxBackend(false);
                avxBackend.compile(jitGraph);

                // MC execution with 4-path batching
                double mcPrice = 0.0;
                std::vector<double> dPrice_dInitRates(size, 0.0);
                double dPrice_dSwapRate = 0.0;

                constexpr int BATCH_SIZE = xad::forge::ForgeBackendAVX<double>::VECTOR_WIDTH;
                Size numBatches = (nrTrails + BATCH_SIZE - 1) / BATCH_SIZE;

                std::vector<double> inputBatch(BATCH_SIZE);
                std::vector<double> outputBatch(BATCH_SIZE);

                // Pre-compute input buffer indices for gradient retrieval
                std::vector<size_t> inputBufferIndices;
                inputBufferIndices.reserve(avxBackend.numInputs());
                for (auto inputId : avxBackend.inputIds()) {
                    inputBufferIndices.push_back(avxBackend.buffer()->getBufferIndex(inputId));
                }

                for (Size batch = 0; batch < numBatches; ++batch) {
                    Size batchStart = batch * BATCH_SIZE;
                    Size actualBatchSize = std::min(static_cast<Size>(BATCH_SIZE), nrTrails - batchStart);

                    // Set initRates (same for all paths in batch)
                    for (Size k = 0; k < size; ++k) {
                        for (int lane = 0; lane < BATCH_SIZE; ++lane)
                            inputBatch[lane] = value(initRates[k]);
                        avxBackend.setInput(k, inputBatch.data());
                    }

                    // Set swapRate (same for all paths in batch)
                    for (int lane = 0; lane < BATCH_SIZE; ++lane)
                        inputBatch[lane] = value(swapRate_tape);
                    avxBackend.setInput(size, inputBatch.data());

                    // Set random numbers (different for each path in batch)
                    for (Size m = 0; m < fullGridRandoms; ++m) {
                        for (int lane = 0; lane < BATCH_SIZE; ++lane) {
                            Size pathIdx = batchStart + lane;
                            inputBatch[lane] = (pathIdx < nrTrails) ? allRandoms[pathIdx][m] : 0.0;
                        }
                        avxBackend.setInput(size + 1 + m, inputBatch.data());
                    }

                    // Execute forward + backward in one call
                    // Need space for: initial rates (size) + swap rate (1) + random numbers (fullGridRandoms)
                    std::size_t numInputs = size + 1 + fullGridRandoms;
                    std::vector<double> inputGradients(numInputs * BATCH_SIZE);
                    avxBackend.forwardAndBackward(outputBatch.data(), inputGradients.data());

                    // Accumulate MC price
                    for (Size lane = 0; lane < actualBatchSize; ++lane) {
                        mcPrice += outputBatch[lane];
                    }

                    // Accumulate gradients
                    for (Size k = 0; k < size; ++k) {
                        for (Size lane = 0; lane < actualBatchSize; ++lane) {
                            dPrice_dInitRates[k] += inputGradients[k * BATCH_SIZE + lane];
                        }
                    }

                    // Accumulate gradient for swap rate
                    for (Size lane = 0; lane < actualBatchSize; ++lane) {
                        dPrice_dSwapRate += inputGradients[size * BATCH_SIZE + lane];
                    }
                }

                mcPrice /= static_cast<double>(nrTrails);
                for (Size k = 0; k < size; ++k) dPrice_dInitRates[k] /= static_cast<double>(nrTrails);
                dPrice_dSwapRate /= static_cast<double>(nrTrails);

                // Chain rule
                std::vector<double> dPrice_dIntermediates(numIntermediates);
                for (Size k = 0; k < size; ++k) dPrice_dIntermediates[k] = dPrice_dInitRates[k];
                dPrice_dIntermediates[size] = dPrice_dSwapRate;

                std::vector<double> dPrice_market(numMarketQuotes);
                applyChainRule(jacobian.data(), dPrice_dIntermediates.data(), dPrice_market.data(),
                               numIntermediates, numMarketQuotes);

                auto t_end = Clock::now();
                if (recordTiming) {
                    jit_avx_times.push_back(Duration(t_end - t_start).count());
                }
            }
        }

        // Compute averages
        auto avg = [](const std::vector<double>& v) {
            if (v.empty()) return 0.0;
            double sum = 0.0;
            for (double x : v) sum += x;
            return sum / static_cast<double>(v.size());
        };

        auto stddev = [&avg](const std::vector<double>& v) {
            if (v.size() <= 1) return 0.0;
            double mean = avg(v);
            double sq_sum = 0.0;
            for (double x : v) {
                double diff = x - mean;
                sq_sum += diff * diff;
            }
            return std::sqrt(sq_sum / (v.size() - 1));
        };

        results[tc].xad_ql_total = avg(xad_ql_times);
        results[tc].xad_rrs_total = avg(xad_rrs_times);
        results[tc].jit_rrs_total = avg(jit_rrs_times);
        results[tc].jit_interp_total = avg(jit_interp_times);
        results[tc].jit_avx_total = avg(jit_avx_times);
        results[tc].xad_ql_std = stddev(xad_ql_times);
        results[tc].xad_rrs_std = stddev(xad_rrs_times);
        results[tc].jit_rrs_std = stddev(jit_rrs_times);
        results[tc].jit_interp_std = stddev(jit_interp_times);
        results[tc].jit_avx_std = stddev(jit_avx_times);

        std::cout << " Done." << std::endl;
    }

    // Print results table (compact format)
    std::cout << std::endl;
    std::cout << "  " << std::string(79, '=') << "\n";
    std::cout << "  SCALING RESULTS (ms)\n";
    std::cout << "  " << std::string(79, '=') << "\n";
    std::cout << std::endl;

    std::cout << "   Paths |   XAD(QL) |  XAD(RR) |  JIT(RR) | JIT-Intrp |   JIT-AVX | Speedup\n";
    std::cout << "  -------+-----------+-----------+-----------+-----------+-----------+--------\n";

    for (Size tc = 0; tc < pathCounts.size(); ++tc) {
        double speedup = results[tc].xad_rrs_total / results[tc].jit_rrs_total;
        std::cout << "  " << std::setw(6) << pathCounts[tc] << " |"
                  << std::fixed << std::setprecision(1) << std::setw(10) << results[tc].xad_ql_total << " |"
                  << std::fixed << std::setprecision(1) << std::setw(10) << results[tc].xad_rrs_total << " |"
                  << std::fixed << std::setprecision(1) << std::setw(10) << results[tc].jit_rrs_total << " |"
                  << std::fixed << std::setprecision(1) << std::setw(10) << results[tc].jit_interp_total << " |"
                  << std::fixed << std::setprecision(1) << std::setw(10) << results[tc].jit_avx_total << " |"
                  << std::fixed << std::setprecision(2) << std::setw(7) << speedup << "x\n";
    }

    std::cout << std::endl;
    std::cout << "  Speedup = XAD(RR) / JIT(RR)\n";
    std::cout << std::endl;
    std::cout << "  STATUS: [STAGE 5 SCALING BENCHMARKS COMPLETE]\n";
    std::cout << "\n";
    std::cout << "=============================================================================\n";
    std::cout << std::endl;

    // Basic verification
    BOOST_CHECK(results[0].xad_ql_total > 0.0);
    BOOST_CHECK(results[0].jit_rrs_total > 0.0);
}

//////////////////////////////////////////////////////////////////////////////
// STAGE 6: Production-Like Configuration Benchmarks
//////////////////////////////////////////////////////////////////////////////

BOOST_AUTO_TEST_CASE(testStage6_ProductionLikeBenchmarks)
{
    BOOST_TEST_MESSAGE("Testing Stage 6: Production-Like Configuration Benchmarks...");

    std::cout << "\n";
    std::cout << "=============================================================================\n";
    std::cout << "  STAGE 6: PRODUCTION-LIKE CONFIGURATION BENCHMARKS\n";
    std::cout << "=============================================================================\n";
    std::cout << std::endl;

    using Clock = std::chrono::high_resolution_clock;
    using Duration = std::chrono::duration<double, std::milli>;

    // Market data setup - PRODUCTION-LIKE CONFIGURATION
    Calendar calendar = TARGET();
    Date todaysDate(4, September, 2005);
    Settings::instance().evaluationDate() = todaysDate;
    Integer fixingDays = 2;
    Date settlementDate = calendar.adjust(calendar.advance(todaysDate, fixingDays, Days));
    DayCounter dayCounter = Actual360();

    // Production-like: 4 deposits + 10 swaps = 14 market inputs (up to 10Y)
    Size numDeposits = 4;
    Size numSwaps = 10;
    std::vector<Period> depoTenors = {1 * Days, 1 * Months, 3 * Months, 6 * Months};
    std::vector<Period> swapTenors = {1 * Years, 2 * Years, 3 * Years, 4 * Years, 5 * Years,
                                      6 * Years, 7 * Years, 8 * Years, 9 * Years, 10 * Years};

    // Realistic deposit rates (upward sloping short end)
    std::vector<double> depoRates_val = {0.0320, 0.0335, 0.0355, 0.0375};
    // Realistic swap rates (upward sloping curve to 10Y)
    std::vector<double> swapRates_val = {0.0400, 0.0435, 0.0460, 0.0480, 0.0495,
                                         0.0505, 0.0515, 0.0522, 0.0528, 0.0532};

    Size numMarketQuotes = numDeposits + numSwaps;

    // LMM parameters: 20 forward rates (semi-annual to 10Y)
    // Swaption: 5Y option into 5Y swap (exercises at 5Y, underlying matures at 10Y)
    Size size = 20;      // Semi-annual rates to 10Y
    Size i_opt = 10;     // Option starts at 5Y (10 × 6 months)
    Size j_opt = 10;     // 5Y swap (10 × 6 months)
    Size steps = 20;     // Time steps for 5Y simulation

    // Build base curve and process setup - to 12Y (extra buffer for accrual periods)
    std::vector<Rate> baseZeroRates = {0.0320, 0.0535};
    std::vector<Date> baseDates = {settlementDate, settlementDate + 12 * Years};
    auto baseIndex = makeIndex(baseDates, baseZeroRates);

    ext::shared_ptr<LiborForwardModelProcess> baseProcess(
        new LiborForwardModelProcess(size, baseIndex));
    ext::shared_ptr<LmCorrelationModel> baseCorrModel(
        new LmExponentialCorrelationModel(size, 0.5));
    ext::shared_ptr<LmVolatilityModel> baseVolaModel(
        new LmLinearExponentialVolatilityModel(baseProcess->fixingTimes(),
                                               0.291, 1.483, 0.116, 0.00001));
    baseProcess->setCovarParam(ext::shared_ptr<LfmCovarianceParameterization>(
        new LfmCovarianceProxy(baseVolaModel, baseCorrModel)));

    // Grid and timing setup
    std::vector<Time> fixingTimes = baseProcess->fixingTimes();
    TimeGrid grid(fixingTimes.begin(), fixingTimes.end(), steps);

    std::vector<Size> location;
    for (Size idx = 0; idx < fixingTimes.size(); ++idx) {
        location.push_back(
            std::find(grid.begin(), grid.end(), fixingTimes[idx]) - grid.begin());
    }

    Size numFactors = baseProcess->factors();
    Size exerciseStep = location[i_opt];
    Size fullGridSteps = grid.size() - 1;
    Size fullGridRandoms = fullGridSteps * numFactors;

    // Types for MC generators
    typedef PseudoRandom::rsg_type rsg_type;
    typedef MultiPathGenerator<rsg_type>::sample_type sample_type;

    // Get fair swap rate
    BusinessDayConvention convention = baseIndex->businessDayConvention();
    Date settlement = baseIndex->forwardingTermStructure()->referenceDate();
    Date fwdStart = settlement + Period(6 * i_opt, Months);
    Date fwdMaturity = fwdStart + Period(6 * j_opt, Months);

    Schedule schedule(fwdStart, fwdMaturity, baseIndex->tenor(), calendar,
                      convention, convention, DateGeneration::Forward, false);

    // Accrual periods
    std::vector<double> accrualStart(size), accrualEnd(size);
    for (Size k = 0; k < size; ++k) {
        accrualStart[k] = value(baseProcess->accrualStartTimes()[k]);
        accrualEnd[k] = value(baseProcess->accrualEndTimes()[k]);
    }

    Size numIntermediates = size + 1;

    // Helper lambda for curve building
    auto buildCurveAndProcess = [&](const std::vector<double>& depoRates,
                                    const std::vector<double>& swapRates,
                                    std::vector<Date>& curveDates,
                                    std::vector<Rate>& zeroRates) {
        RelinkableHandle<YieldTermStructure> euriborTS;
        auto euribor6m = ext::make_shared<Euribor6M>(euriborTS);
        euribor6m->addFixing(Date(2, September, 2005), 0.04);

        std::vector<ext::shared_ptr<RateHelper>> instruments;
        for (Size idx = 0; idx < numDeposits; ++idx) {
            auto depoQuote = ext::make_shared<SimpleQuote>(depoRates[idx]);
            instruments.push_back(ext::make_shared<DepositRateHelper>(
                Handle<Quote>(depoQuote), depoTenors[idx], fixingDays,
                calendar, ModifiedFollowing, true, dayCounter));
        }
        for (Size idx = 0; idx < numSwaps; ++idx) {
            auto swapQuote = ext::make_shared<SimpleQuote>(swapRates[idx]);
            instruments.push_back(ext::make_shared<SwapRateHelper>(
                Handle<Quote>(swapQuote), swapTenors[idx],
                calendar, Annual, Unadjusted, Thirty360(Thirty360::BondBasis),
                euribor6m));
        }

        auto yieldCurve = ext::make_shared<PiecewiseYieldCurve<ZeroYield, Linear>>(
            settlementDate, instruments, dayCounter);
        yieldCurve->enableExtrapolation();

        curveDates.clear();
        zeroRates.clear();
        curveDates.push_back(settlementDate);
        zeroRates.push_back(yieldCurve->zeroRate(settlementDate, dayCounter, Continuous).rate());
        Date endDate = settlementDate + 12 * Years;  // Extended for 10Y instrument + buffer
        curveDates.push_back(endDate);
        zeroRates.push_back(yieldCurve->zeroRate(endDate, dayCounter, Continuous).rate());
    };

    // Test configuration - scaling over different path counts
    std::vector<Size> pathCounts = {10, 100, 1000, 10000};
    Size maxPaths = pathCounts.back();
    Size warmupIterations = 2;
    Size benchmarkIterations = 3;

    std::cout << "  Configuration (Production-Like):\n";
    std::cout << "    - Market inputs: " << numMarketQuotes << " (" << numDeposits << " deposits + " << numSwaps << " swaps)\n";
    std::cout << "    - Forward rates: " << size << " (semi-annual to 10Y)\n";
    std::cout << "    - Grid steps: " << fullGridSteps << "\n";
    std::cout << "    - MC paths: 10, 100, 1K, 10K\n";
    std::cout << "    - Swaption: " << (i_opt/2) << "Y into " << (j_opt/2) << "Y swap (matures at " << ((i_opt+j_opt)/2) << "Y)\n";
    std::cout << "    - Warmup iterations: " << warmupIterations << "\n";
    std::cout << "    - Benchmark iterations: " << benchmarkIterations << "\n";
    std::cout << std::endl;

    // Pre-generate random numbers for max paths
    std::cout << "  Generating " << maxPaths << " x " << fullGridRandoms << " random numbers..." << std::flush;
    rsg_type rsg_base = PseudoRandom::make_sequence_generator(fullGridRandoms, BigNatural(42));
    std::vector<std::vector<double>> allRandoms(maxPaths);
    for (Size n = 0; n < maxPaths; ++n) {
        allRandoms[n].resize(fullGridRandoms);
        const auto& seq = rsg_base.nextSequence();
        for (Size m = 0; m < fullGridRandoms; ++m) {
            allRandoms[n][m] = value(seq.value[m]);
        }
    }
    std::cout << " Done." << std::endl;

    // Results storage for scaling table
    struct ScalingResult {
        double xad_time;
        double jit_time;
        double jit_avx_time;
    };
    std::vector<ScalingResult> scalingResults(pathCounts.size());

    // Store final prices/derivs from largest path count
    double xad_price = 0.0, jit_price = 0.0, jit_avx_price = 0.0;
    std::vector<double> xad_derivs, jit_derivs, jit_avx_derivs;

    // Run benchmarks for each path count
    for (Size pathIdx = 0; pathIdx < pathCounts.size(); ++pathIdx) {
        Size nrTrails = pathCounts[pathIdx];
        std::cout << "\n  --- Running with " << nrTrails << " paths ---\n";

        // Timing accumulators for this path count
        std::vector<double> xad_rrs_times, jit_rrs_times, jit_avx_times;

        for (Size iter = 0; iter < warmupIterations + benchmarkIterations; ++iter) {
            bool recordTiming = (iter >= warmupIterations);
            bool isLast = (iter == warmupIterations + benchmarkIterations - 1) &&
                          (pathIdx == pathCounts.size() - 1);  // Only save details for largest path count

            std::cout << "  [" << (iter < warmupIterations ? "Warmup" : "Bench") << " "
                      << (iter < warmupIterations ? iter + 1 : iter - warmupIterations + 1) << "/"
                      << (iter < warmupIterations ? warmupIterations : benchmarkIterations) << "] ";

        // =================================================================
        // XAD (RR) - Full grid with XAD tape (with granular timing)
        // =================================================================
        std::cout << "XAD..." << std::flush;
        {
            auto t_start = Clock::now();
            auto t_phase = Clock::now();

            // Phase timing accumulators (only for last iteration)
            double t_bootstrap_fwd = 0, t_mc_fwd = 0, t_backward = 0;

            using tape_type = Real::tape_type;
            tape_type tape;

            std::vector<Real> depositRates(numDeposits);
            std::vector<Real> swapRates(numSwaps);
            for (Size idx = 0; idx < numDeposits; ++idx) depositRates[idx] = depoRates_val[idx];
            for (Size idx = 0; idx < numSwaps; ++idx) swapRates[idx] = swapRates_val[idx];

            tape.registerInputs(depositRates);
            tape.registerInputs(swapRates);
            tape.newRecording();

            RelinkableHandle<YieldTermStructure> euriborTS;
            auto euribor6m = ext::make_shared<Euribor6M>(euriborTS);
            euribor6m->addFixing(Date(2, September, 2005), 0.04);

            std::vector<ext::shared_ptr<RateHelper>> instruments;
            for (Size idx = 0; idx < numDeposits; ++idx) {
                auto depoQuote = ext::make_shared<SimpleQuote>(depositRates[idx]);
                instruments.push_back(ext::make_shared<DepositRateHelper>(
                    Handle<Quote>(depoQuote), depoTenors[idx], fixingDays,
                    calendar, ModifiedFollowing, true, dayCounter));
            }
            for (Size idx = 0; idx < numSwaps; ++idx) {
                auto swapQuote = ext::make_shared<SimpleQuote>(swapRates[idx]);
                instruments.push_back(ext::make_shared<SwapRateHelper>(
                    Handle<Quote>(swapQuote), swapTenors[idx],
                    calendar, Annual, Unadjusted, Thirty360(Thirty360::BondBasis),
                    euribor6m));
            }

            auto yieldCurve = ext::make_shared<PiecewiseYieldCurve<ZeroYield, Linear>>(
                settlementDate, instruments, dayCounter);
            yieldCurve->enableExtrapolation();

            std::vector<Date> curveDates;
            std::vector<Real> zeroRates;
            curveDates.push_back(settlementDate);
            zeroRates.push_back(yieldCurve->zeroRate(settlementDate, dayCounter, Continuous).rate());
            Date endDate = settlementDate + 12 * Years;  // Extended for 10Y instrument + buffer
            curveDates.push_back(endDate);
            zeroRates.push_back(yieldCurve->zeroRate(endDate, dayCounter, Continuous).rate());

            std::vector<Rate> zeroRates_ql;
            for (const auto& r : zeroRates) zeroRates_ql.push_back(r);

            RelinkableHandle<YieldTermStructure> termStructure;
            ext::shared_ptr<IborIndex> index(new Euribor6M(termStructure));
            index->addFixing(Date(2, September, 2005), 0.04);
            termStructure.linkTo(ext::make_shared<ZeroCurve>(curveDates, zeroRates_ql, dayCounter));

            ext::shared_ptr<LiborForwardModelProcess> process(
                new LiborForwardModelProcess(size, index));
            process->setCovarParam(ext::shared_ptr<LfmCovarianceParameterization>(
                new LfmCovarianceProxy(
                    ext::make_shared<LmLinearExponentialVolatilityModel>(process->fixingTimes(), 0.291, 1.483, 0.116, 0.00001),
                    ext::make_shared<LmExponentialCorrelationModel>(size, 0.5))));

            ext::shared_ptr<VanillaSwap> fwdSwap(
                new VanillaSwap(Swap::Receiver, 1.0,
                                schedule, 0.05, dayCounter,
                                schedule, index, 0.0, index->dayCounter()));
            fwdSwap->setPricingEngine(ext::make_shared<DiscountingSwapEngine>(
                index->forwardingTermStructure()));
            Real swapRate = fwdSwap->fairRate();

            Array initRates = process->initialValues();

            if (isLast) {
                t_bootstrap_fwd = Duration(Clock::now() - t_phase).count();
                t_phase = Clock::now();
            }

            // MC pricing with full grid
            Real price = Real(0.0);
            for (Size n = 0; n < nrTrails; ++n) {
                Array asset(size);
                for (Size k = 0; k < size; ++k) asset[k] = initRates[k];

                Array assetAtExercise(size);
                for (Size step = 1; step <= fullGridSteps; ++step) {
                    Size offset = (step - 1) * numFactors;
                    Time t = grid[step - 1];
                    Time dt = grid.dt(step - 1);

                    Array dw(numFactors);
                    for (Size f = 0; f < numFactors; ++f)
                        dw[f] = Real(allRandoms[n][offset + f]);

                    asset = process->evolve(t, asset, dt, dw);

                    if (step == exerciseStep) {
                        for (Size k = 0; k < size; ++k) assetAtExercise[k] = asset[k];
                    }
                }

                Array dis(size);
                Real df = Real(1.0);
                for (Size k = 0; k < size; ++k) {
                    Real accrual = accrualEnd[k] - accrualStart[k];
                    df = df / (Real(1.0) + assetAtExercise[k] * accrual);
                    dis[k] = df;
                }

                Real npv = Real(0.0);
                for (Size m = i_opt; m < i_opt + j_opt; ++m) {
                    Real accrual = accrualEnd[m] - accrualStart[m];
                    npv += (swapRate - assetAtExercise[m]) * accrual * dis[m];
                }
                price += max(npv, Real(0.0));
            }
            price /= Real(static_cast<double>(nrTrails));

            if (isLast) {
                t_mc_fwd = Duration(Clock::now() - t_phase).count();
                t_phase = Clock::now();
            }

            tape.registerOutput(price);
            derivative(price) = 1.0;
            tape.computeAdjoints();

            if (isLast) {
                t_backward = Duration(Clock::now() - t_phase).count();

                // Report XAD timing breakdown
                std::cout << "\n    [XAD Details: " << tape.getNumStatements() << " tape statements]\n";
                std::cout << "      Bootstrap fwd:   " << std::fixed << std::setprecision(1) << t_bootstrap_fwd << " ms\n";
                std::cout << "      MC fwd (" << nrTrails << " paths): " << std::fixed << std::setprecision(1) << t_mc_fwd << " ms\n";
                std::cout << "      Backward:        " << std::fixed << std::setprecision(1) << t_backward << " ms\n";

                xad_price = value(price);
                xad_derivs.resize(numMarketQuotes);
                for (Size m = 0; m < numDeposits; ++m)
                    xad_derivs[m] = derivative(depositRates[m]);
                for (Size m = 0; m < numSwaps; ++m)
                    xad_derivs[numDeposits + m] = derivative(swapRates[m]);
            }

            auto t_end = Clock::now();
            if (recordTiming) {
                xad_rrs_times.push_back(Duration(t_end - t_start).count());
            }
            tape.deactivate();
        }

        // =================================================================
        // JIT (RR) - Full grid with JIT compilation (with granular timing)
        // =================================================================
        std::cout << " JIT..." << std::flush;
        {
            auto t_start = Clock::now();
            auto t_phase = Clock::now();

            // Phase timing accumulators (only for last iteration to avoid overhead)
            double t_bootstrap_fwd = 0, t_bootstrap_bwd = 0, t_kernel_record = 0;
            double t_kernel_compile = 0, t_mc_exec = 0, t_chain_rule = 0;

            using tape_type = Real::tape_type;
            tape_type tape;

            std::vector<Real> depositRates(numDeposits);
            std::vector<Real> swapRates(numSwaps);
            for (Size idx = 0; idx < numDeposits; ++idx) depositRates[idx] = depoRates_val[idx];
            for (Size idx = 0; idx < numSwaps; ++idx) swapRates[idx] = swapRates_val[idx];

            tape.registerInputs(depositRates);
            tape.registerInputs(swapRates);
            tape.newRecording();

            RelinkableHandle<YieldTermStructure> euriborTS;
            auto euribor6m = ext::make_shared<Euribor6M>(euriborTS);
            euribor6m->addFixing(Date(2, September, 2005), 0.04);

            std::vector<ext::shared_ptr<RateHelper>> instruments;
            for (Size idx = 0; idx < numDeposits; ++idx) {
                auto depoQuote = ext::make_shared<SimpleQuote>(depositRates[idx]);
                instruments.push_back(ext::make_shared<DepositRateHelper>(
                    Handle<Quote>(depoQuote), depoTenors[idx], fixingDays,
                    calendar, ModifiedFollowing, true, dayCounter));
            }
            for (Size idx = 0; idx < numSwaps; ++idx) {
                auto swapQuote = ext::make_shared<SimpleQuote>(swapRates[idx]);
                instruments.push_back(ext::make_shared<SwapRateHelper>(
                    Handle<Quote>(swapQuote), swapTenors[idx],
                    calendar, Annual, Unadjusted, Thirty360(Thirty360::BondBasis),
                    euribor6m));
            }

            auto yieldCurve = ext::make_shared<PiecewiseYieldCurve<ZeroYield, Linear>>(
                settlementDate, instruments, dayCounter);
            yieldCurve->enableExtrapolation();

            std::vector<Date> curveDates;
            std::vector<Real> zeroRates;
            curveDates.push_back(settlementDate);
            zeroRates.push_back(yieldCurve->zeroRate(settlementDate, dayCounter, Continuous).rate());
            Date endDate = settlementDate + 12 * Years;  // Extended for 10Y instrument + buffer
            curveDates.push_back(endDate);
            zeroRates.push_back(yieldCurve->zeroRate(endDate, dayCounter, Continuous).rate());

            std::vector<Rate> zeroRates_ql;
            for (const auto& r : zeroRates) zeroRates_ql.push_back(r);

            RelinkableHandle<YieldTermStructure> termStructure;
            ext::shared_ptr<IborIndex> index(new Euribor6M(termStructure));
            index->addFixing(Date(2, September, 2005), 0.04);
            termStructure.linkTo(ext::make_shared<ZeroCurve>(curveDates, zeroRates_ql, dayCounter));

            ext::shared_ptr<LiborForwardModelProcess> process(
                new LiborForwardModelProcess(size, index));
            process->setCovarParam(ext::shared_ptr<LfmCovarianceParameterization>(
                new LfmCovarianceProxy(
                    ext::make_shared<LmLinearExponentialVolatilityModel>(process->fixingTimes(), 0.291, 1.483, 0.116, 0.00001),
                    ext::make_shared<LmExponentialCorrelationModel>(size, 0.5))));

            ext::shared_ptr<VanillaSwap> fwdSwap(
                new VanillaSwap(Swap::Receiver, 1.0,
                                schedule, 0.05, dayCounter,
                                schedule, index, 0.0, index->dayCounter()));
            fwdSwap->setPricingEngine(ext::make_shared<DiscountingSwapEngine>(
                index->forwardingTermStructure()));
            Real swapRate_tape = fwdSwap->fairRate();

            Array initRates = process->initialValues();

            if (isLast) {
                t_bootstrap_fwd = Duration(Clock::now() - t_phase).count();
                t_phase = Clock::now();
            }

            // Jacobian computation (bootstrap backward)
            std::vector<double> jacobian(numIntermediates * numMarketQuotes, 0.0);
            for (Size k = 0; k < size; ++k) {
                if (initRates[k].shouldRecord()) {
                    tape.clearDerivatives();
                    tape.registerOutput(initRates[k]);
                    derivative(initRates[k]) = 1.0;
                    tape.computeAdjoints();

                    double* jac_row = jacobian.data() + k * numMarketQuotes;
                    for (Size m = 0; m < numDeposits; ++m)
                        jac_row[m] = derivative(depositRates[m]);
                    for (Size m = 0; m < numSwaps; ++m)
                        jac_row[numDeposits + m] = derivative(swapRates[m]);
                }
            }
            if (swapRate_tape.shouldRecord()) {
                tape.clearDerivatives();
                tape.registerOutput(swapRate_tape);
                derivative(swapRate_tape) = 1.0;
                tape.computeAdjoints();

                double* jac_row = jacobian.data() + size * numMarketQuotes;
                for (Size m = 0; m < numDeposits; ++m)
                    jac_row[m] = derivative(depositRates[m]);
                for (Size m = 0; m < numSwaps; ++m)
                    jac_row[numDeposits + m] = derivative(swapRates[m]);
            }
            tape.deactivate();

            if (isLast) {
                t_bootstrap_bwd = Duration(Clock::now() - t_phase).count();
                t_phase = Clock::now();
            }

            // JIT kernel creation (recording)
            auto forgeBackend = std::make_unique<xad::forge::ForgeBackend<double>>(false);
            xad::JITCompiler<double> jit(std::move(forgeBackend));

            std::vector<xad::AD> jit_initRates(size);
            xad::AD jit_swapRate;
            std::vector<xad::AD> jit_randoms(fullGridRandoms);

            for (Size k = 0; k < size; ++k) {
                jit_initRates[k] = xad::AD(value(initRates[k]));
                jit.registerInput(jit_initRates[k]);
            }
            jit_swapRate = xad::AD(value(swapRate_tape));
            jit.registerInput(jit_swapRate);
            for (Size m = 0; m < fullGridRandoms; ++m) {
                jit_randoms[m] = xad::AD(0.0);
                jit.registerInput(jit_randoms[m]);
            }

            jit.newRecording();

            std::vector<xad::AD> asset_jit(size);
            std::vector<xad::AD> assetAtExercise_jit(size);
            for (Size k = 0; k < size; ++k) asset_jit[k] = jit_initRates[k];

            for (Size step = 1; step <= fullGridSteps; ++step) {
                Size offset = (step - 1) * numFactors;
                Time t = grid[step - 1];
                Time dt = grid.dt(step - 1);

                Array dw(numFactors);
                for (Size f = 0; f < numFactors; ++f) dw[f] = jit_randoms[offset + f];

                Array asset_arr(size);
                for (Size k = 0; k < size; ++k) asset_arr[k] = asset_jit[k];

                Array evolved = process->evolve(t, asset_arr, dt, dw);
                for (Size k = 0; k < size; ++k) asset_jit[k] = evolved[k];

                if (step == exerciseStep) {
                    for (Size k = 0; k < size; ++k) assetAtExercise_jit[k] = asset_jit[k];
                }
            }

            std::vector<xad::AD> dis_jit(size);
            xad::AD df_jit = xad::AD(1.0);
            for (Size k = 0; k < size; ++k) {
                double accrual = accrualEnd[k] - accrualStart[k];
                df_jit = df_jit / (xad::AD(1.0) + assetAtExercise_jit[k] * accrual);
                dis_jit[k] = df_jit;
            }

            xad::AD jit_npv = xad::AD(0.0);
            for (Size m = i_opt; m < i_opt + j_opt; ++m) {
                double accrual = accrualEnd[m] - accrualStart[m];
                jit_npv = jit_npv + (jit_swapRate - assetAtExercise_jit[m]) * accrual * dis_jit[m];
            }

            xad::AD jit_payoff = xad::less(jit_npv, xad::AD(0.0)).If(xad::AD(0.0), jit_npv);
            jit.registerOutput(jit_payoff);

            if (isLast) {
                t_kernel_record = Duration(Clock::now() - t_phase).count();
                t_phase = Clock::now();
            }

            // Compile the JIT kernel
            jit.compile();

            if (isLast) {
                t_kernel_compile = Duration(Clock::now() - t_phase).count();
                t_phase = Clock::now();
            }

            // MC execution
            double mcPrice = 0.0;
            std::vector<double> dPrice_dInitRates(size, 0.0);
            double dPrice_dSwapRate = 0.0;

            const auto& graph = jit.getGraph();
            uint32_t outputSlot = graph.output_ids[0];

            for (Size n = 0; n < nrTrails; ++n) {
                for (Size k = 0; k < size; ++k) value(jit_initRates[k]) = value(initRates[k]);
                value(jit_swapRate) = value(swapRate_tape);
                for (Size m = 0; m < fullGridRandoms; ++m) value(jit_randoms[m]) = allRandoms[n][m];

                double payoff_value;
                jit.forward(&payoff_value);
                mcPrice += payoff_value;

                jit.clearDerivatives();
                jit.setDerivative(outputSlot, 1.0);
                jit.computeAdjoints();

                for (Size k = 0; k < size; ++k) dPrice_dInitRates[k] += jit.derivative(graph.input_ids[k]);
                dPrice_dSwapRate += jit.derivative(graph.input_ids[size]);
            }

            mcPrice /= static_cast<double>(nrTrails);
            for (Size k = 0; k < size; ++k) dPrice_dInitRates[k] /= static_cast<double>(nrTrails);
            dPrice_dSwapRate /= static_cast<double>(nrTrails);

            if (isLast) {
                t_mc_exec = Duration(Clock::now() - t_phase).count();
                t_phase = Clock::now();
            }

            // Chain rule
            std::vector<double> dPrice_dIntermediates(numIntermediates);
            for (Size k = 0; k < size; ++k) dPrice_dIntermediates[k] = dPrice_dInitRates[k];
            dPrice_dIntermediates[size] = dPrice_dSwapRate;

            std::vector<double> dPrice_market(numMarketQuotes);
            applyChainRule(jacobian.data(), dPrice_dIntermediates.data(), dPrice_market.data(),
                           numIntermediates, numMarketQuotes);

            if (isLast) {
                t_chain_rule = Duration(Clock::now() - t_phase).count();

                // Report graph size and timing breakdown
                std::cout << "\n    [JIT Details: " << graph.nodeCount() << " nodes, "
                          << graph.input_ids.size() << " inputs]\n";
                std::cout << "      Bootstrap fwd:   " << std::fixed << std::setprecision(1) << t_bootstrap_fwd << " ms\n";
                std::cout << "      Bootstrap bwd:   " << std::fixed << std::setprecision(1) << t_bootstrap_bwd << " ms\n";
                std::cout << "      Kernel record:   " << std::fixed << std::setprecision(1) << t_kernel_record << " ms\n";
                std::cout << "      Kernel compile:  " << std::fixed << std::setprecision(1) << t_kernel_compile << " ms\n";
                std::cout << "      MC exec (" << nrTrails << " paths): " << std::fixed << std::setprecision(1) << t_mc_exec << " ms\n";
                std::cout << "      Chain rule:      " << std::fixed << std::setprecision(1) << t_chain_rule << " ms\n";

                jit_price = mcPrice;
                jit_derivs = dPrice_market;
            }

            auto t_end = Clock::now();
            if (recordTiming) {
                jit_rrs_times.push_back(Duration(t_end - t_start).count());
            }
        }

        // =================================================================
        // JIT AVX (RR) - ForgeBackendAVX with 4-path batching (with granular timing)
        // =================================================================
        std::cout << " JIT-AVX..." << std::flush;
        {
            auto t_start = Clock::now();
            auto t_phase = Clock::now();

            // Phase timing accumulators (only for last iteration)
            double t_bootstrap_fwd = 0, t_bootstrap_bwd = 0, t_kernel_record = 0;
            double t_kernel_compile = 0, t_mc_exec = 0, t_chain_rule = 0;

            using tape_type = Real::tape_type;
            tape_type tape;

            std::vector<Real> depositRates(numDeposits);
            std::vector<Real> swapRates(numSwaps);
            for (Size idx = 0; idx < numDeposits; ++idx) depositRates[idx] = depoRates_val[idx];
            for (Size idx = 0; idx < numSwaps; ++idx) swapRates[idx] = swapRates_val[idx];

            tape.registerInputs(depositRates);
            tape.registerInputs(swapRates);
            tape.newRecording();

            RelinkableHandle<YieldTermStructure> euriborTS;
            auto euribor6m = ext::make_shared<Euribor6M>(euriborTS);
            euribor6m->addFixing(Date(2, September, 2005), 0.04);

            std::vector<ext::shared_ptr<RateHelper>> instruments;
            for (Size idx = 0; idx < numDeposits; ++idx) {
                auto depoQuote = ext::make_shared<SimpleQuote>(depositRates[idx]);
                instruments.push_back(ext::make_shared<DepositRateHelper>(
                    Handle<Quote>(depoQuote), depoTenors[idx], fixingDays,
                    calendar, ModifiedFollowing, true, dayCounter));
            }
            for (Size idx = 0; idx < numSwaps; ++idx) {
                auto swapQuote = ext::make_shared<SimpleQuote>(swapRates[idx]);
                instruments.push_back(ext::make_shared<SwapRateHelper>(
                    Handle<Quote>(swapQuote), swapTenors[idx],
                    calendar, Annual, Unadjusted, Thirty360(Thirty360::BondBasis),
                    euribor6m));
            }

            auto yieldCurve = ext::make_shared<PiecewiseYieldCurve<ZeroYield, Linear>>(
                settlementDate, instruments, dayCounter);
            yieldCurve->enableExtrapolation();

            std::vector<Date> curveDates;
            std::vector<Real> zeroRates;
            curveDates.push_back(settlementDate);
            zeroRates.push_back(yieldCurve->zeroRate(settlementDate, dayCounter, Continuous).rate());
            Date endDate = settlementDate + 12 * Years;  // Extended for 10Y instrument + buffer
            curveDates.push_back(endDate);
            zeroRates.push_back(yieldCurve->zeroRate(endDate, dayCounter, Continuous).rate());

            std::vector<Rate> zeroRates_ql;
            for (const auto& r : zeroRates) zeroRates_ql.push_back(r);

            RelinkableHandle<YieldTermStructure> termStructure;
            ext::shared_ptr<IborIndex> index(new Euribor6M(termStructure));
            index->addFixing(Date(2, September, 2005), 0.04);
            termStructure.linkTo(ext::make_shared<ZeroCurve>(curveDates, zeroRates_ql, dayCounter));

            ext::shared_ptr<LiborForwardModelProcess> process(
                new LiborForwardModelProcess(size, index));
            process->setCovarParam(ext::shared_ptr<LfmCovarianceParameterization>(
                new LfmCovarianceProxy(
                    ext::make_shared<LmLinearExponentialVolatilityModel>(process->fixingTimes(), 0.291, 1.483, 0.116, 0.00001),
                    ext::make_shared<LmExponentialCorrelationModel>(size, 0.5))));

            ext::shared_ptr<VanillaSwap> fwdSwap(
                new VanillaSwap(Swap::Receiver, 1.0,
                                schedule, 0.05, dayCounter,
                                schedule, index, 0.0, index->dayCounter()));
            fwdSwap->setPricingEngine(ext::make_shared<DiscountingSwapEngine>(
                index->forwardingTermStructure()));
            Real swapRate_tape = fwdSwap->fairRate();

            Array initRates = process->initialValues();

            if (isLast) {
                t_bootstrap_fwd = Duration(Clock::now() - t_phase).count();
                t_phase = Clock::now();
            }

            // Jacobian computation
            std::vector<double> jacobian(numIntermediates * numMarketQuotes, 0.0);
            for (Size k = 0; k < size; ++k) {
                if (initRates[k].shouldRecord()) {
                    tape.clearDerivatives();
                    tape.registerOutput(initRates[k]);
                    derivative(initRates[k]) = 1.0;
                    tape.computeAdjoints();

                    double* jac_row = jacobian.data() + k * numMarketQuotes;
                    for (Size m = 0; m < numDeposits; ++m)
                        jac_row[m] = derivative(depositRates[m]);
                    for (Size m = 0; m < numSwaps; ++m)
                        jac_row[numDeposits + m] = derivative(swapRates[m]);
                }
            }
            if (swapRate_tape.shouldRecord()) {
                tape.clearDerivatives();
                tape.registerOutput(swapRate_tape);
                derivative(swapRate_tape) = 1.0;
                tape.computeAdjoints();

                double* jac_row = jacobian.data() + size * numMarketQuotes;
                for (Size m = 0; m < numDeposits; ++m)
                    jac_row[m] = derivative(depositRates[m]);
                for (Size m = 0; m < numSwaps; ++m)
                    jac_row[numDeposits + m] = derivative(swapRates[m]);
            }
            tape.deactivate();

            if (isLast) {
                t_bootstrap_bwd = Duration(Clock::now() - t_phase).count();
                t_phase = Clock::now();
            }

            // JIT kernel creation for AVX
            xad::JITCompiler<double> jit;

            std::vector<xad::AD> jit_initRates(size);
            xad::AD jit_swapRate;
            std::vector<xad::AD> jit_randoms(fullGridRandoms);

            for (Size k = 0; k < size; ++k) {
                jit_initRates[k] = xad::AD(value(initRates[k]));
                jit.registerInput(jit_initRates[k]);
            }
            jit_swapRate = xad::AD(value(swapRate_tape));
            jit.registerInput(jit_swapRate);
            for (Size m = 0; m < fullGridRandoms; ++m) {
                jit_randoms[m] = xad::AD(0.0);
                jit.registerInput(jit_randoms[m]);
            }

            jit.newRecording();

            std::vector<xad::AD> asset_jit(size);
            std::vector<xad::AD> assetAtExercise_jit(size);
            for (Size k = 0; k < size; ++k) asset_jit[k] = jit_initRates[k];

            for (Size step = 1; step <= fullGridSteps; ++step) {
                Size offset = (step - 1) * numFactors;
                Time t = grid[step - 1];
                Time dt = grid.dt(step - 1);

                Array dw(numFactors);
                for (Size f = 0; f < numFactors; ++f) dw[f] = jit_randoms[offset + f];

                Array asset_arr(size);
                for (Size k = 0; k < size; ++k) asset_arr[k] = asset_jit[k];

                Array evolved = process->evolve(t, asset_arr, dt, dw);
                for (Size k = 0; k < size; ++k) asset_jit[k] = evolved[k];

                if (step == exerciseStep) {
                    for (Size k = 0; k < size; ++k) assetAtExercise_jit[k] = asset_jit[k];
                }
            }

            std::vector<xad::AD> dis_jit(size);
            xad::AD df_jit = xad::AD(1.0);
            for (Size k = 0; k < size; ++k) {
                double accrual = accrualEnd[k] - accrualStart[k];
                df_jit = df_jit / (xad::AD(1.0) + assetAtExercise_jit[k] * accrual);
                dis_jit[k] = df_jit;
            }

            xad::AD jit_npv = xad::AD(0.0);
            for (Size m = i_opt; m < i_opt + j_opt; ++m) {
                double accrual = accrualEnd[m] - accrualStart[m];
                jit_npv = jit_npv + (jit_swapRate - assetAtExercise_jit[m]) * accrual * dis_jit[m];
            }

            xad::AD jit_payoff = xad::less(jit_npv, xad::AD(0.0)).If(xad::AD(0.0), jit_npv);
            jit.registerOutput(jit_payoff);

            if (isLast) {
                t_kernel_record = Duration(Clock::now() - t_phase).count();
                t_phase = Clock::now();
            }

            // Get the JIT graph
            const auto& jitGraph = jit.getGraph();
            jit.deactivate();

            // AVX backend with 4-path batching
            xad::forge::ForgeBackendAVX<double> avxBackend(false);
            avxBackend.compile(jitGraph);

            if (isLast) {
                t_kernel_compile = Duration(Clock::now() - t_phase).count();
                t_phase = Clock::now();
            }

            // MC execution with 4-path batching
            double mcPrice = 0.0;
            std::vector<double> dPrice_dInitRates(size, 0.0);
            double dPrice_dSwapRate = 0.0;

            constexpr int BATCH_SIZE = xad::forge::ForgeBackendAVX<double>::VECTOR_WIDTH;
            Size numBatches = (nrTrails + BATCH_SIZE - 1) / BATCH_SIZE;

            std::vector<double> inputBatch(BATCH_SIZE);
            std::vector<double> outputBatch(BATCH_SIZE);

            // Pre-compute input buffer indices
            std::vector<size_t> inputBufferIndices;
            inputBufferIndices.reserve(avxBackend.numInputs());
            for (auto inputId : avxBackend.inputIds()) {
                inputBufferIndices.push_back(avxBackend.buffer()->getBufferIndex(inputId));
            }

            for (Size batch = 0; batch < numBatches; ++batch) {
                Size batchStart = batch * BATCH_SIZE;
                Size actualBatchSize = std::min(static_cast<Size>(BATCH_SIZE), nrTrails - batchStart);

                // Set initRates
                for (Size k = 0; k < size; ++k) {
                    for (int lane = 0; lane < BATCH_SIZE; ++lane)
                        inputBatch[lane] = value(initRates[k]);
                    avxBackend.setInput(k, inputBatch.data());
                }

                // Set swapRate
                for (int lane = 0; lane < BATCH_SIZE; ++lane)
                    inputBatch[lane] = value(swapRate_tape);
                avxBackend.setInput(size, inputBatch.data());

                // Set random numbers
                for (Size m = 0; m < fullGridRandoms; ++m) {
                    for (int lane = 0; lane < BATCH_SIZE; ++lane) {
                        Size pathIdx = batchStart + lane;
                        inputBatch[lane] = (pathIdx < nrTrails) ? allRandoms[pathIdx][m] : 0.0;
                    }
                    avxBackend.setInput(size + 1 + m, inputBatch.data());
                }

                // Execute forward + backward
                std::size_t numInputs = size + 1 + fullGridRandoms;
                std::vector<double> inputGradients(numInputs * BATCH_SIZE);
                avxBackend.forwardAndBackward(outputBatch.data(), inputGradients.data());

                // Accumulate MC price
                for (Size lane = 0; lane < actualBatchSize; ++lane) {
                    mcPrice += outputBatch[lane];
                }

                // Accumulate gradients
                for (Size k = 0; k < size; ++k) {
                    for (Size lane = 0; lane < actualBatchSize; ++lane) {
                        dPrice_dInitRates[k] += inputGradients[k * BATCH_SIZE + lane];
                    }
                }

                for (Size lane = 0; lane < actualBatchSize; ++lane) {
                    dPrice_dSwapRate += inputGradients[size * BATCH_SIZE + lane];
                }
            }

            mcPrice /= static_cast<double>(nrTrails);
            for (Size k = 0; k < size; ++k) dPrice_dInitRates[k] /= static_cast<double>(nrTrails);
            dPrice_dSwapRate /= static_cast<double>(nrTrails);

            if (isLast) {
                t_mc_exec = Duration(Clock::now() - t_phase).count();
                t_phase = Clock::now();
            }

            // Chain rule
            std::vector<double> dPrice_dIntermediates(numIntermediates);
            for (Size k = 0; k < size; ++k) dPrice_dIntermediates[k] = dPrice_dInitRates[k];
            dPrice_dIntermediates[size] = dPrice_dSwapRate;

            std::vector<double> dPrice_market(numMarketQuotes);
            applyChainRule(jacobian.data(), dPrice_dIntermediates.data(), dPrice_market.data(),
                           numIntermediates, numMarketQuotes);

            if (isLast) {
                t_chain_rule = Duration(Clock::now() - t_phase).count();

                // Report timing breakdown for JIT-AVX
                std::cout << "\n    [JIT-AVX Details: " << jitGraph.nodeCount() << " nodes]\n";
                std::cout << "      Bootstrap fwd:   " << std::fixed << std::setprecision(1) << t_bootstrap_fwd << " ms\n";
                std::cout << "      Bootstrap bwd:   " << std::fixed << std::setprecision(1) << t_bootstrap_bwd << " ms\n";
                std::cout << "      Kernel record:   " << std::fixed << std::setprecision(1) << t_kernel_record << " ms\n";
                std::cout << "      Kernel compile:  " << std::fixed << std::setprecision(1) << t_kernel_compile << " ms\n";
                std::cout << "      MC exec (" << nrTrails << " paths): " << std::fixed << std::setprecision(1) << t_mc_exec << " ms\n";
                std::cout << "      Chain rule:      " << std::fixed << std::setprecision(1) << t_chain_rule << " ms\n";

                jit_avx_price = mcPrice;
                jit_avx_derivs = dPrice_market;
            }

            auto t_end = Clock::now();
            if (recordTiming) {
                jit_avx_times.push_back(Duration(t_end - t_start).count());
            }
        }

        std::cout << " Done." << std::endl;
        }  // end iteration loop

        // Compute averages for this path count
        auto avg = [](const std::vector<double>& v) {
            if (v.empty()) return 0.0;
            double sum = 0.0;
            for (double x : v) sum += x;
            return sum / static_cast<double>(v.size());
        };

        scalingResults[pathIdx].xad_time = avg(xad_rrs_times);
        scalingResults[pathIdx].jit_time = avg(jit_rrs_times);
        scalingResults[pathIdx].jit_avx_time = avg(jit_avx_times);

        std::cout << "    Avg times: XAD=" << std::fixed << std::setprecision(1) << scalingResults[pathIdx].xad_time
                  << "ms, JIT=" << scalingResults[pathIdx].jit_time
                  << "ms, JIT-AVX=" << scalingResults[pathIdx].jit_avx_time << "ms\n";
    }  // end path count loop

    // Print scaling table
    std::cout << std::endl;
    std::cout << "  " << std::string(79, '=') << "\n";
    std::cout << "  PRODUCTION-LIKE BENCHMARK RESULTS (5Y into 5Y Swaption, 10Y total)\n";
    std::cout << "  " << std::string(79, '=') << "\n";
    std::cout << std::endl;

    std::cout << "  SCALING TABLE (times in ms):\n";
    std::cout << "    " << std::setw(8) << "Paths" << " | " << std::setw(10) << "XAD"
              << " | " << std::setw(10) << "JIT" << " | " << std::setw(10) << "JIT-AVX"
              << " | " << std::setw(10) << "Speedup" << "\n";
    std::cout << "    " << std::string(58, '-') << "\n";
    for (Size i = 0; i < pathCounts.size(); ++i) {
        std::string pathStr;
        if (pathCounts[i] >= 1000) pathStr = std::to_string(pathCounts[i]/1000) + "K";
        else pathStr = std::to_string(pathCounts[i]);

        double speedup = scalingResults[i].xad_time / scalingResults[i].jit_avx_time;
        std::cout << "    " << std::setw(8) << pathStr << " | "
                  << std::fixed << std::setprecision(1) << std::setw(10) << scalingResults[i].xad_time << " | "
                  << std::fixed << std::setprecision(1) << std::setw(10) << scalingResults[i].jit_time << " | "
                  << std::fixed << std::setprecision(1) << std::setw(10) << scalingResults[i].jit_avx_time << " | "
                  << std::fixed << std::setprecision(2) << std::setw(9) << speedup << "x\n";
    }
    std::cout << std::endl;

    std::cout << "  MC PRICE COMPARISON (10K paths):\n";
    std::cout << "    XAD:     " << std::fixed << std::setprecision(8) << xad_price << "\n";
    std::cout << "    JIT:     " << std::fixed << std::setprecision(8) << jit_price << "\n";
    std::cout << "    JIT-AVX: " << std::fixed << std::setprecision(8) << jit_avx_price << "\n";
    std::cout << std::endl;

    std::cout << "  DERIVATIVE COMPARISON (first 5 market quotes, 10K paths):\n";
    std::cout << "    " << std::setw(12) << "Quote" << " | " << std::setw(14) << "XAD" << " | "
              << std::setw(14) << "JIT" << " | " << std::setw(14) << "JIT-AVX" << "\n";
    std::cout << "    " << std::string(60, '-') << "\n";
    for (Size i = 0; i < std::min(Size(5), numMarketQuotes); ++i) {
        std::string label = (i < numDeposits) ? "Depo " + std::to_string(i) : "Swap " + std::to_string(i - numDeposits);
        std::cout << "    " << std::setw(12) << label << " | "
                  << std::scientific << std::setprecision(4) << std::setw(14) << xad_derivs[i] << " | "
                  << std::scientific << std::setprecision(4) << std::setw(14) << jit_derivs[i] << " | "
                  << std::scientific << std::setprecision(4) << std::setw(14) << jit_avx_derivs[i] << "\n";
    }
    std::cout << std::endl;

    // Final speedup summary
    double final_speedup_jit = scalingResults.back().xad_time / scalingResults.back().jit_time;
    double final_speedup_avx = scalingResults.back().xad_time / scalingResults.back().jit_avx_time;
    std::cout << "  SPEEDUP vs XAD (10K paths):\n";
    std::cout << "    JIT:     " << std::fixed << std::setprecision(2) << final_speedup_jit << "x\n";
    std::cout << "    JIT-AVX: " << std::fixed << std::setprecision(2) << final_speedup_avx << "x\n";
    std::cout << std::endl;

    std::cout << "  STATUS: [STAGE 6 PRODUCTION-LIKE BENCHMARKS COMPLETE]\n";
    std::cout << "\n";
    std::cout << "=============================================================================\n";
    std::cout << std::endl;

    // Verification
    double price_diff = std::abs(xad_price - jit_price);
    BOOST_CHECK_SMALL(price_diff, 1e-10);
    BOOST_CHECK(scalingResults.back().xad_time > 0.0);
    BOOST_CHECK(scalingResults.back().jit_time > 0.0);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE_END()


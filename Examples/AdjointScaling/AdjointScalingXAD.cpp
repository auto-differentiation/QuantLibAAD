/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
 Copyright (C) 2026 Xcelerit Computing Limited

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
Cost of all sensitivities as the number of inputs grows: XAD adjoint mode vs
bumping. A fixed book of 250 European options is priced off spot, dividend
yield, a zero curve and a vol surface growing from 4x5 to 20x25 points
(31 to 511 inputs).

XAD: one recording and one adjoint sweep. QLAAD_DISABLE_AAD=ON: forward
differences, one extra pricing per input. For both side by side, run the
double build with "--csv bumping.csv", then the XAD build with
"--compare bumping.csv".
*/

#include <ql/qldefines.hpp>
#if !defined(BOOST_ALL_NO_LIB) && defined(BOOST_MSVC)
#    include <ql/auto_link.hpp>
#endif
#include <ql/exercise.hpp>
#include <ql/instruments/vanillaoption.hpp>
#include <ql/math/matrix.hpp>
#include <ql/math/randomnumbers/mt19937uniformrng.hpp>
#include <ql/pricingengines/vanilla/analyticeuropeanengine.hpp>
#include <ql/processes/blackscholesprocess.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/volatility/equityfx/blackvariancesurface.hpp>
#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/termstructures/yield/zerocurve.hpp>
#include <ql/time/calendars/target.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>
#include <XAD/XAD.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace QuantLib;

namespace {

    // the market data; every number in here gets a sensitivity
    struct Market {
        Real spot;
        Real dividendYield;
        std::vector<Real> zeroRates; // one per curve date
        Matrix vols;                 // strikes x expiries
    };

    // pointers to all market inputs, in a fixed order
    std::vector<Real*> inputs(Market& m) {
        std::vector<Real*> x = {&m.spot, &m.dividendYield};
        for (auto& r : m.zeroRates)
            x.push_back(&r);
        for (auto it = m.vols.begin(); it != m.vols.end(); ++it)
            x.push_back(&*it);
        return x;
    }

    struct OptionSpec {
        Option::Type type;
        Real strike;
        Date maturity;
    };

    // everything that stays fixed while the market moves
    struct Setup {
        Date today;
        std::vector<Date> curveDates;
        std::vector<Date> expiries;
        std::vector<Real> strikes;
        std::vector<OptionSpec> book;
    };

    Setup makeSetup(const Date& today, Size nExpiries, Size nStrikes, Size bookSize) {
        Setup s;
        s.today = today;
        for (Integer months : {0, 3, 6, 12, 24, 36, 60, 84, 120})
            s.curveDates.push_back(today + months * Months);
        // expiries spread between 1 month and 5 years, strikes between 60 and 140
        for (Size j = 0; j < nExpiries; ++j)
            s.expiries.push_back(today + Integer(1 + j * 59 / (nExpiries - 1)) * Months);
        for (Size i = 0; i < nStrikes; ++i)
            s.strikes.push_back(60.0 + 80.0 * i / (nStrikes - 1));
        // the same book for every surface size
        MersenneTwisterUniformRng rng(42);
        for (Size k = 0; k < bookSize; ++k) {
            Option::Type type = xad::value(rng.nextReal()) < 0.5 ? Option::Call : Option::Put;
            Real strike = 70.0 + 60.0 * xad::value(rng.nextReal());
            Date maturity = today + Integer(30 + xad::value(rng.nextReal()) * 1700) * Days;
            s.book.push_back({type, strike, maturity});
        }
        return s;
    }

    Market makeMarket(const Setup& s) {
        Market m;
        m.spot = 100.0;
        m.dividendYield = 0.015;
        for (Size i = 0; i < s.curveDates.size(); ++i)
            m.zeroRates.push_back(0.025 + 0.0015 * i);
        DayCounter dc = Actual365Fixed();
        m.vols = Matrix(s.strikes.size(), s.expiries.size());
        for (Size i = 0; i < s.strikes.size(); ++i) {
            for (Size j = 0; j < s.expiries.size(); ++j) {
                Real moneyness = std::log(s.strikes[i] / 100.0);
                Time t = dc.yearFraction(s.today, s.expiries[j]);
                m.vols[i][j] = 0.18 - 0.08 * moneyness + 0.15 * moneyness * moneyness +
                               0.03 * std::exp(-t);
            }
        }
        return m;
    }

    // builds the curves and the surface from the market data and prices the book
    Real priceBook(const Setup& s, const Market& m) {
        DayCounter dc = Actual365Fixed();
        Handle<Quote> spot(ext::make_shared<SimpleQuote>(m.spot));
        Handle<YieldTermStructure> dividends(
            ext::make_shared<FlatForward>(s.today, m.dividendYield, dc));
        Handle<YieldTermStructure> rates(
            ext::make_shared<ZeroCurve>(s.curveDates, m.zeroRates, dc));
        Handle<BlackVolTermStructure> vols(ext::make_shared<BlackVarianceSurface>(
            s.today, TARGET(), s.expiries, s.strikes, m.vols, dc,
            BlackVarianceSurface::ConstantExtrapolation,
            BlackVarianceSurface::ConstantExtrapolation));
        auto process = ext::make_shared<BlackScholesMertonProcess>(spot, dividends, rates, vols);
        auto engine = ext::make_shared<AnalyticEuropeanEngine>(process);

        Real value = 0.0;
        for (const auto& spec : s.book) {
            VanillaOption option(ext::make_shared<PlainVanillaPayoff>(spec.type, spec.strike),
                                 ext::make_shared<EuropeanExercise>(spec.maturity));
            option.setPricingEngine(engine);
            value += option.NPV();
        }
        return value;
    }

#ifndef QLAAD_DISABLE_AAD

    using tape_type = Real::tape_type;

    // one recording and one adjoint sweep give all sensitivities
    double priceWithAAD(tape_type& tape,
                        const Setup& s,
                        const Market& market,
                        std::vector<double>& gradient) {
        tape.clearAll();
        Market m = market;
        auto x = inputs(m);
        for (auto* xi : x)
            tape.registerInput(*xi);
        tape.newRecording();

        Real v = priceBook(s, m);

        tape.registerOutput(v);
        derivative(v) = 1.0;
        tape.computeAdjoints();

        gradient.resize(x.size());
        for (Size i = 0; i < x.size(); ++i)
            gradient[i] = derivative(*x[i]);
        return value(v);
    }

    // central differences, for checking the AAD sensitivities
    std::vector<double> centralDifferences(const Setup& s, const Market& market) {
        Market m = market;
        auto x = inputs(m);
        std::vector<double> gradient(x.size());
        for (Size i = 0; i < x.size(); ++i) {
            Real saved = *x[i];
            Real h = 1e-5 * std::max(1.0, value(std::fabs(saved)));
            *x[i] = saved + h;
            Real up = priceBook(s, m);
            *x[i] = saved - h;
            Real down = priceBook(s, m);
            *x[i] = saved;
            gradient[i] = value((up - down) / (2.0 * h));
        }
        return gradient;
    }

#else

    // forward differences: one extra pricing per input
    double priceWithBumping(const Setup& s, const Market& market, std::vector<double>& gradient) {
        Market m = market;
        Real v = priceBook(s, m);
        auto x = inputs(m);
        gradient.resize(x.size());
        for (Size i = 0; i < x.size(); ++i) {
            Real saved = *x[i];
            Real h = 1e-6 * std::max(1.0, std::fabs(saved));
            *x[i] = saved + h;
            gradient[i] = (priceBook(s, m) - v) / h;
            *x[i] = saved;
        }
        return v;
    }

#endif

    // median wall-clock time of repeated runs, in milliseconds
    template <class F>
    double timeMs(F&& f) {
        std::vector<double> times;
        double total = 0.0;
        while (times.size() < 3 || (total < 500.0 && times.size() < 50)) {
            auto start = std::chrono::steady_clock::now();
            f();
            auto end = std::chrono::steady_clock::now();
            times.push_back(std::chrono::duration<double, std::milli>(end - start).count());
            total += times.back();
        }
        std::sort(times.begin(), times.end());
        return times[times.size() / 2];
    }

    struct Row {
        Size inputs;
        double priceMs;
        double sensiMs;
        double tapeMB;
    };

#ifndef QLAAD_DISABLE_AAD

    // timings from the double build, keyed by the number of inputs
    std::map<Size, Row> readCsv(const std::string& file) {
        std::map<Size, Row> rows;
        std::ifstream in(file);
        QL_REQUIRE(in, "cannot open " << file);
        std::string line;
        std::getline(in, line);
        QL_REQUIRE(line == "inputs,price_ms,bumping_ms",
                   file << " is not from the QLAAD_DISABLE_AAD=ON build");
        while (std::getline(in, line)) {
            std::istringstream fields(line);
            Row row = {0, 0.0, 0.0, 0.0};
            char comma;
            if (fields >> row.inputs >> comma >> row.priceMs >> comma >> row.sensiMs)
                rows[row.inputs] = row;
        }
        return rows;
    }

#endif

    void writeCsv(const std::string& file, const std::vector<Row>& rows) {
        std::ofstream out(file);
        QL_REQUIRE(out, "cannot write " << file);
#ifndef QLAAD_DISABLE_AAD
        out << "inputs,price_ms,aad_ms\n";
#else
        out << "inputs,price_ms,bumping_ms\n";
#endif
        for (const auto& row : rows)
            out << row.inputs << "," << row.priceMs << "," << row.sensiMs << "\n";
    }

    std::string ms(double t) {
        std::ostringstream s;
        s << std::fixed << std::setprecision(t < 10.0 ? 2 : 1) << t << " ms";
        return s.str();
    }

    std::string ratio(double f) {
        std::ostringstream s;
        s << std::fixed << std::setprecision(f < 10.0 ? 1 : 0) << f << "x";
        return s.str();
    }

}

int main(int argc, char* argv[]) {
    try {
        std::string csvFile, compareFile;
        for (int i = 1; i + 1 < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--csv")
                csvFile = argv[++i];
            else if (arg == "--compare")
                compareFile = argv[++i];
        }

        Date today(15, May, 2026);
        Settings::instance().evaluationDate() = today;
        const Size bookSize = 250;
        const std::vector<std::pair<Size, Size>> surfaces = {
            {4, 5}, {6, 10}, {10, 15}, {15, 20}, {20, 25}};

        std::cout << "Sensitivities of a book of " << bookSize
                  << " European options to spot, dividend yield,\n"
                  << "a 9-point zero curve and a Black volatility surface of growing size\n\n";

        std::vector<Row> rows;

#ifndef QLAAD_DISABLE_AAD

        std::map<Size, Row> bumping;
        if (!compareFile.empty())
            bumping = readCsv(compareFile);

        tape_type tape;

        // check the AAD sensitivities against central differences on the smallest surface
        {
            Setup s = makeSetup(today, surfaces[0].first, surfaces[0].second, bookSize);
            Market m = makeMarket(s);
            std::vector<double> aad;
            priceWithAAD(tape, s, m, aad);
            std::vector<double> fd = centralDifferences(s, m);
            double maxError = 0.0, maxSensi = 0.0;
            for (Size i = 0; i < aad.size(); ++i) {
                maxError = std::max(maxError, std::fabs(aad[i] - fd[i]));
                maxSensi = std::max(maxSensi, std::fabs(aad[i]));
            }
            std::cout << "AAD vs central differences, " << aad.size()
                      << " inputs: largest difference " << std::scientific
                      << std::setprecision(1) << maxError << " (largest sensitivity "
                      << maxSensi << ")\n\n"
                      << std::defaultfloat;
        }

        for (const auto& surface : surfaces) {
            Setup s = makeSetup(today, surface.first, surface.second, bookSize);
            Market m = makeMarket(s);
            std::vector<double> gradient;
            Row row;
            row.inputs = inputs(m).size();
            // Real is the XAD type here; with nothing registered, the tape records nothing
            row.priceMs = timeMs([&]() { priceBook(s, m); });
            row.sensiMs = timeMs([&]() { priceWithAAD(tape, s, m, gradient); });
            row.tapeMB = static_cast<double>(tape.getMemory()) / (1024.0 * 1024.0);
            rows.push_back(row);
        }

        if (compareFile.empty()) {
            std::cout << " inputs | price (AReal) |    AAD    | AAD/price | tape memory\n"
                      << "--------+---------------+-----------+-----------+------------\n";
            for (const auto& r : rows) {
                std::ostringstream mem;
                mem << std::fixed << std::setprecision(1) << r.tapeMB << " MB";
                std::cout << std::setw(7) << r.inputs << " | " << std::setw(13) << ms(r.priceMs)
                          << " | " << std::setw(9) << ms(r.sensiMs) << " | " << std::setw(9)
                          << ratio(r.sensiMs / r.priceMs) << " | " << std::setw(10) << mem.str()
                          << "\n";
            }
            std::cout << "\nFor bumping times, run the QLAAD_DISABLE_AAD=ON build with "
                         "\"--csv bumping.csv\"\nand pass the file here with "
                         "\"--compare bumping.csv\".\n";
        } else {
            std::cout << " inputs | price (double) | price (AReal) |  bumping  |    AAD    "
                         "| AAD/price | speedup\n"
                      << "--------+----------------+---------------+-----------+-----------"
                         "+-----------+--------\n";
            for (const auto& r : rows) {
                auto b = bumping.find(r.inputs);
                if (b == bumping.end())
                    continue;
                std::cout << std::setw(7) << r.inputs << " | " << std::setw(14)
                          << ms(b->second.priceMs) << " | " << std::setw(13) << ms(r.priceMs)
                          << " | " << std::setw(9) << ms(b->second.sensiMs) << " | "
                          << std::setw(9) << ms(r.sensiMs) << " | " << std::setw(9)
                          << ratio(r.sensiMs / b->second.priceMs) << " | " << std::setw(7)
                          << ratio(b->second.sensiMs / r.sensiMs) << "\n";
            }
            std::cout << "\nAAD/price: AAD time over one plain double pricing.\n"
                         "speedup: bumping time over AAD time.\n";
        }

#else

        for (const auto& surface : surfaces) {
            Setup s = makeSetup(today, surface.first, surface.second, bookSize);
            Market m = makeMarket(s);
            std::vector<double> gradient;
            Row row;
            row.inputs = inputs(m).size();
            row.priceMs = timeMs([&]() { priceBook(s, m); });
            row.sensiMs = timeMs([&]() { priceWithBumping(s, m, gradient); });
            row.tapeMB = 0.0;
            rows.push_back(row);
        }

        std::cout << " inputs | price (double) |  bumping  | bumping/price\n"
                  << "--------+----------------+-----------+--------------\n";
        for (const auto& r : rows)
            std::cout << std::setw(7) << r.inputs << " | " << std::setw(14) << ms(r.priceMs)
                      << " | " << std::setw(9) << ms(r.sensiMs) << " | " << std::setw(13)
                      << ratio(r.sensiMs / r.priceMs) << "\n";

#endif

        if (!csvFile.empty()) {
            writeCsv(csvFile, rows);
            std::cout << "\nTimings written to " << csvFile << "\n";
        }
        return 0;
    } catch (std::exception& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "unknown error" << std::endl;
        return 1;
    }
}

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
Trade-level risk: the sensitivities of every swap in a book to every curve
quote, with XAD.

The curve is recorded once: its node zero rates and their Jacobian to the 45
quotes come from one recording and one adjoint sweep per node. Each of the 200
swaps is then priced off a zero curve through those nodes, which reproduces
the bootstrapped curve, with a small recording of its own, and the chain rule
maps its node sensitivities to the quotes.

QLAAD_DISABLE_AAD=ON: bumping, one bootstrap and book revaluation per quote.
For both side by side, run that build with "--csv bumping.csv", then the XAD
build with "--compare bumping.csv".
*/

#include <ql/qldefines.hpp>
#if !defined(BOOST_ALL_NO_LIB) && defined(BOOST_MSVC)
#    include <ql/auto_link.hpp>
#endif
#include <ql/indexes/ibor/euribor.hpp>
#include <ql/instruments/vanillaswap.hpp>
#include <ql/math/randomnumbers/mt19937uniformrng.hpp>
#include <ql/pricingengines/swap/discountingswapengine.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/yield/piecewiseyieldcurve.hpp>
#include <ql/termstructures/yield/ratehelpers.hpp>
#include <ql/termstructures/yield/zerocurve.hpp>
#include <ql/time/calendars/target.hpp>
#include <ql/time/daycounters/actual360.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>
#include <ql/time/daycounters/thirty360.hpp>
#include <XAD/XAD.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using namespace QuantLib;

namespace {

    const Size depositCount = 10, fraCount = 5, swapCount = 30;
    using Curve = PiecewiseYieldCurve<ZeroYield, Linear>;

    // deposits (on, tn, sn, 1w, 1m to 6m), FRAs (1x7 to 5x11) and swaps (1y to 30y)
    std::vector<double> marketQuotes() {
        std::vector<double> quotes;
        for (Size i = 0; i < depositCount; ++i)
            quotes.push_back(0.0010 + i * 0.0002);
        for (Size i = 0; i < fraCount; ++i)
            quotes.push_back(0.0030 + i * 0.0005);
        for (Size i = 0; i < swapCount; ++i)
            quotes.push_back(0.0060 + i * 0.0001);
        return quotes;
    }

    std::vector<std::string> quoteNames() {
        std::vector<std::string> names = {"deposit ON", "deposit TN", "deposit SN",
                                          "deposit 1W"};
        for (Size i = 1; i <= depositCount - 4; ++i)
            names.push_back("deposit " + std::to_string(i) + "M");
        for (Size i = 1; i <= fraCount; ++i)
            names.push_back("FRA " + std::to_string(i) + "x" + std::to_string(i + 6));
        for (Size i = 1; i <= swapCount; ++i)
            names.push_back("swap " + std::to_string(i) + "Y");
        return names;
    }

    ext::shared_ptr<Curve> bootstrap(const Date& today, const std::vector<Real>& quotes) {
        std::vector<ext::shared_ptr<RateHelper>> helpers;
        auto quote = [&](Size i) {
            return Handle<Quote>(ext::make_shared<SimpleQuote>(quotes[i]));
        };
        for (Size i = 0; i < depositCount; ++i) {
            Period tenor = i < 3 ? 1 * Days : (i == 3 ? 1 * Weeks : Integer(i - 3) * Months);
            Natural fixingDays = i < 2 ? Natural(i) : 2;
            helpers.push_back(ext::make_shared<DepositRateHelper>(
                quote(i), tenor, fixingDays, TARGET(), ModifiedFollowing, false, Actual360()));
        }
        for (Size i = 0; i < fraCount; ++i)
            helpers.push_back(ext::make_shared<FraRateHelper>(quote(depositCount + i), i + 1,
                                                              i + 7, 2, TARGET(),
                                                              ModifiedFollowing, false, Actual360()));
        auto euribor6m = ext::make_shared<Euribor6M>();
        for (Size i = 0; i < swapCount; ++i)
            helpers.push_back(ext::make_shared<SwapRateHelper>(
                quote(depositCount + fraCount + i), Integer(i + 1) * Years, TARGET(), Annual,
                ModifiedFollowing, Thirty360(Thirty360::European), euribor6m));
        auto curve = ext::make_shared<Curve>(today, helpers, Actual365Fixed());
        curve->enableExtrapolation();
        return curve;
    }

    struct Trade {
        Swap::Type type;
        Real nominal;
        Rate fixedRate;
        Integer years;
    };

    std::vector<Trade> makeBook(Size size) {
        MersenneTwisterUniformRng rng(42);
        auto u = [&rng]() { return xad::value(rng.nextReal()); };
        std::vector<Trade> book;
        for (Size k = 0; k < size; ++k) {
            Swap::Type type = u() < 0.5 ? Swap::Payer : Swap::Receiver;
            Real nominal = 1e6 * (1 + Integer(10 * u()));
            Rate fixedRate = 0.002 + 0.008 * u();
            Integer years = 1 + Integer(29.999 * u());
            book.push_back({type, nominal, fixedRate, years});
        }
        return book;
    }

    Real priceTrade(const Trade& t, const Date& start, const Handle<YieldTermStructure>& curve) {
        auto index = ext::make_shared<Euribor6M>(curve);
        Date end = TARGET().advance(start, t.years, Years);
        Schedule fixed(start, end, 1 * Years, TARGET(), ModifiedFollowing, ModifiedFollowing,
                       DateGeneration::Backward, false);
        Schedule floating(start, end, 6 * Months, TARGET(), ModifiedFollowing, ModifiedFollowing,
                          DateGeneration::Backward, false);
        VanillaSwap swap(t.type, t.nominal, fixed, t.fixedRate, Thirty360(Thirty360::European),
                         floating, index, 0.0, Actual360());
        swap.setPricingEngine(ext::make_shared<DiscountingSwapEngine>(curve));
        return swap.NPV();
    }

    // forward differences: one bootstrap and book revaluation per bumped quote
    std::vector<std::vector<double>> bumping(const Date& today,
                                             const Date& start,
                                             const std::vector<double>& quotes,
                                             const std::vector<Trade>& book) {
        auto priceBook = [&](const std::vector<Real>& q) {
            Handle<YieldTermStructure> curve(bootstrap(today, q));
            std::vector<Real> npvs;
            for (const auto& trade : book)
                npvs.push_back(priceTrade(trade, start, curve));
            return npvs;
        };
        std::vector<Real> q(quotes.begin(), quotes.end());
        auto base = priceBook(q);
        std::vector<std::vector<double>> risk(book.size(), std::vector<double>(q.size()));
        const double h = 1e-6;
        for (Size i = 0; i < q.size(); ++i) {
            q[i] += h;
            auto bumped = priceBook(q);
            q[i] -= h;
            for (Size k = 0; k < book.size(); ++k)
                risk[k][i] = double(xad::value((bumped[k] - base[k]) / h));
        }
        return risk;
    }

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

#ifndef QLAAD_DISABLE_AAD

    using tape_type = Real::tape_type;

    struct CurveRisk {
        std::vector<Date> dates;
        std::vector<Real> zeros;
        std::vector<std::vector<double>> jacobian; // nodes x quotes
    };

    // node zero rates and their Jacobian to the quotes: one recording, one sweep per node
    CurveRisk curveRisk(tape_type& tape, const Date& today, const std::vector<double>& quotes) {
        tape.clearAll();
        std::vector<Real> q(quotes.begin(), quotes.end());
        tape.registerInputs(q);
        tape.newRecording();

        auto curve = bootstrap(today, q);
        std::vector<Real> z(curve->data().begin(), curve->data().end());
        for (auto& zj : z)
            tape.registerOutput(zj);

        CurveRisk c;
        c.dates = curve->dates();
        for (auto& zj : z) {
            tape.clearDerivatives();
            derivative(zj) = 1.0;
            tape.computeAdjoints();
            std::vector<double> row;
            for (auto& qi : q)
                row.push_back(derivative(qi));
            c.jacobian.push_back(row);
            c.zeros.push_back(value(zj));
        }
        return c;
    }

    // per trade: node sensitivities from a small recording, mapped to the quotes
    std::vector<std::vector<double>> tradeRisk(tape_type& tape,
                                               const Date& start,
                                               const std::vector<Trade>& book,
                                               const CurveRisk& c) {
        Size nQuotes = c.jacobian.front().size();
        std::vector<std::vector<double>> risk(book.size(), std::vector<double>(nQuotes, 0.0));
        for (Size k = 0; k < book.size(); ++k) {
            tape.clearAll();
            std::vector<Real> z = c.zeros;
            tape.registerInputs(z);
            tape.newRecording();

            Handle<YieldTermStructure> curve(ext::make_shared<ZeroCurve>(c.dates, z, Actual365Fixed()));
            curve->enableExtrapolation();
            Real npv = priceTrade(book[k], start, curve);

            tape.registerOutput(npv);
            derivative(npv) = 1.0;
            tape.computeAdjoints();
            for (Size j = 0; j < z.size(); ++j) {
                double dz = derivative(z[j]);
                if (dz != 0.0)
                    for (Size i = 0; i < nQuotes; ++i)
                        risk[k][i] += dz * c.jacobian[j][i];
            }
        }
        return risk;
    }

    // one trade recorded together with the whole bootstrap, for checking
    std::vector<double> throughBootstrap(tape_type& tape,
                                         const Date& today,
                                         const Date& start,
                                         const std::vector<double>& quotes,
                                         const Trade& trade) {
        tape.clearAll();
        std::vector<Real> q(quotes.begin(), quotes.end());
        tape.registerInputs(q);
        tape.newRecording();
        Real npv = priceTrade(trade, start, Handle<YieldTermStructure>(bootstrap(today, q)));
        tape.registerOutput(npv);
        derivative(npv) = 1.0;
        tape.computeAdjoints();
        std::vector<double> risk;
        for (auto& qi : q)
            risk.push_back(derivative(qi));
        return risk;
    }

    double readCsv(const std::string& file) {
        std::ifstream in(file);
        QL_REQUIRE(in, "cannot open " << file);
        std::string header;
        double ms = 0.0;
        std::getline(in, header);
        QL_REQUIRE(header == "bumping_ms" && (in >> ms),
                   file << " is not from the QLAAD_DISABLE_AAD=ON build");
        return ms;
    }

    double largestDifference(const std::vector<double>& a, const std::vector<double>& b) {
        double d = 0.0;
        for (Size i = 0; i < a.size(); ++i)
            d = std::max(d, std::fabs(a[i] - b[i]));
        return d;
    }

#endif

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

        Date today(2, January, 2015);
        Settings::instance().evaluationDate() = today;
        Date start = TARGET().advance(today, 2, Days);
        auto quotes = marketQuotes();
        auto names = quoteNames();
        auto book = makeBook(200);

        std::cout << "Sensitivities of each of the " << book.size()
                  << " swaps in a book to each of the " << quotes.size() << " curve quotes\n\n";

#ifndef QLAAD_DISABLE_AAD

        double bumpingMs = compareFile.empty() ? 0.0 : readCsv(compareFile);

        tape_type tape;
        std::vector<std::vector<double>> risk;
        double aadMs = timeMs([&]() { risk = tradeRisk(tape, start, book, curveRisk(tape, today, quotes)); });
        std::cout << std::fixed << std::setprecision(1) << "AAD: " << aadMs
                  << " ms, from one recording of the curve and one small recording per swap\n";

        // check the first swap two other ways
        auto direct = throughBootstrap(tape, today, start, quotes, book[0]);
        auto bumped = bumping(today, start, quotes, {book[0]}).front();
        double largest = 0.0;
        for (double r : risk[0])
            largest = std::max(largest, std::fabs(r));
        std::cout << std::scientific << std::setprecision(1)
                  << "  swap 1 against a full recording of curve and swap: largest difference "
                  << largestDifference(risk[0], direct) << "\n"
                  << "  swap 1 by bumping: largest difference "
                  << largestDifference(risk[0], bumped) << " (largest sensitivity " << largest
                  << ")\n";

        if (bumpingMs > 0.0)
            std::cout << std::fixed << std::setprecision(1) << "\nBumping with doubles: "
                      << bumpingMs << " ms, AAD speedup " << std::setprecision(0)
                      << bumpingMs / aadMs << "x\n";
        else
            std::cout << "\nFor bumping times, run the QLAAD_DISABLE_AAD=ON build with "
                         "\"--csv bumping.csv\"\nand pass the file here with "
                         "\"--compare bumping.csv\".\n";

#else

        std::vector<std::vector<double>> risk;
        double bumpingMs = timeMs([&]() { risk = bumping(today, start, quotes, book); });
        std::cout << std::fixed << std::setprecision(1) << "Bumping: " << bumpingMs << " ms, "
                  << quotes.size() + 1 << " bootstraps and book revaluations\n";

        if (!csvFile.empty()) {
            std::ofstream out(csvFile);
            QL_REQUIRE(out, "cannot write " << csvFile);
            out << "bumping_ms\n" << bumpingMs << "\n";
            std::cout << "\nTiming written to " << csvFile << "\n";
        }

#endif

        // the largest sensitivities of the first swap
        const auto& first = risk[0];
        std::vector<Size> order(first.size());
        for (Size i = 0; i < order.size(); ++i)
            order[i] = i;
        std::sort(order.begin(), order.end(),
                  [&](Size a, Size b) { return std::fabs(first[a]) > std::fabs(first[b]); });
        std::cout << "\nSwap 1 (" << (book[0].type == Swap::Payer ? "payer" : "receiver") << ", "
                  << book[0].years << "y), largest sensitivities:\n";
        for (Size k = 0; k < 5; ++k)
            std::cout << std::setw(14) << names[order[k]] << " | " << std::fixed
                      << std::setprecision(0) << std::setw(14) << first[order[k]] << "\n";

        return 0;
    } catch (std::exception& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "unknown error" << std::endl;
        return 1;
    }
}

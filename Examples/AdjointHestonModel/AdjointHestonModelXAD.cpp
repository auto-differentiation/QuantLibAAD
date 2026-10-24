/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
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
Sensitivities through a calibrated model with the implicit function theorem,
without bumping and recalibrating or recording the optimizer.

Heston is calibrated to 104 DAX option prices and prices a forward-start
option. At the calibrated parameters p the objective's gradient in p is zero,
so dV/dm = dV/dm|p - (Hpp^-1 dV/dp)' Hpm, with the Hessian blocks Hpp and Hpm
from central differences of the XAD gradient in p: 11 recordings for all 114
market inputs. Recording the whole calibration instead takes several GB here.

QLAAD_DISABLE_AAD=ON: bump and recalibrate, one calibration per input. For
both side by side, run that build with "--csv bumping.csv", then the XAD
build with "--compare bumping.csv".
*/

#include <ql/qldefines.hpp>
#if !defined(BOOST_ALL_NO_LIB) && defined(BOOST_MSVC)
#    include <ql/auto_link.hpp>
#endif
#include <ql/exercise.hpp>
#include <ql/experimental/forward/analytichestonforwardeuropeanengine.hpp>
#include <ql/instruments/forwardvanillaoption.hpp>
#include <ql/math/optimization/levenbergmarquardt.hpp>
#include <ql/models/equity/hestonmodel.hpp>
#include <ql/models/equity/hestonmodelhelper.hpp>
#include <ql/pricingengines/vanilla/coshestonengine.hpp>
#include <ql/quotes/simplequote.hpp>
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

    // tight, so that bumping and recalibrating moves the parameters reliably;
    // the implicit function theorem doesn't need it
    const Real tolerance = 1e-12;

    // QuantLib's order for the Heston parameters
    const char* parameterNames[] = {"theta", "kappa", "sigma", "rho", "v0"};

    // everything that stays fixed while the market moves
    struct Setup {
        Date today;
        std::vector<Date> curveDates;
        std::vector<Period> expiries;
        std::vector<Real> strikes;
    };

    // the market data; every number in here gets a sensitivity
    struct Market {
        Real spot;
        std::vector<Real> zeroRates; // one per curve date
        std::vector<Real> vols;      // by strike, then expiry
    };

    std::vector<Real*> inputs(Market& m) {
        std::vector<Real*> x = {&m.spot};
        for (auto& r : m.zeroRates)
            x.push_back(&r);
        for (auto& v : m.vols)
            x.push_back(&v);
        return x;
    }

    std::vector<std::string> inputNames(const Setup& s) {
        std::vector<std::string> names = {"spot"};
        for (const auto& d : s.curveDates)
            names.push_back("zero rate " + std::to_string(d - s.today) + "d");
        for (auto strike : s.strikes) {
            for (const auto& expiry : s.expiries) {
                std::ostringstream name;
                name << "vol " << strike << " " << expiry;
                names.push_back(name.str());
            }
        }
        return names;
    }

    // DAX data from A. Sepp, as in QuantLib's Heston model tests
    void makeDaxMarket(Setup& s, Market& m) {
        s.today = Date(5, July, 2002);
        const Integer days[] = {13, 41, 75, 165, 256, 345, 524, 703};
        const Rate rates[] = {0.0357, 0.0349, 0.0341, 0.0355, 0.0359, 0.0368, 0.0386, 0.0401};
        s.curveDates = {s.today};
        m.zeroRates = {0.0357};
        for (Size i = 0; i < 8; ++i) {
            s.curveDates.push_back(s.today + days[i]);
            m.zeroRates.push_back(rates[i]);
            s.expiries.push_back(Period(Integer((days[i] + 3) / 7.0), Weeks));
        }
        s.strikes = {3400, 3600, 3800, 4000, 4200, 4400, 4500,
                     4600, 4800, 5000, 5200, 5400, 5600};
        m.spot = 4468.17;
        m.vols = {0.6625, 0.4875, 0.4204, 0.3667, 0.3431, 0.3267, 0.3121, 0.3121, 0.6007, 0.4543,
                  0.3967, 0.3511, 0.3279, 0.3154, 0.2984, 0.2921, 0.5084, 0.4221, 0.3718, 0.3327,
                  0.3155, 0.3027, 0.2919, 0.2889, 0.4541, 0.3869, 0.3492, 0.3149, 0.2963, 0.2926,
                  0.2819, 0.2800, 0.4060, 0.3607, 0.3330, 0.2999, 0.2887, 0.2811, 0.2751, 0.2775,
                  0.3726, 0.3396, 0.3108, 0.2781, 0.2788, 0.2722, 0.2661, 0.2686, 0.3550, 0.3277,
                  0.3012, 0.2781, 0.2781, 0.2661, 0.2661, 0.2681, 0.3428, 0.3209, 0.2958, 0.2740,
                  0.2688, 0.2627, 0.2580, 0.2620, 0.3302, 0.3062, 0.2799, 0.2631, 0.2573, 0.2533,
                  0.2504, 0.2544, 0.3343, 0.2959, 0.2705, 0.2540, 0.2504, 0.2464, 0.2448, 0.2462,
                  0.3460, 0.2845, 0.2624, 0.2463, 0.2425, 0.2385, 0.2373, 0.2422, 0.3857, 0.2860,
                  0.2578, 0.2399, 0.2357, 0.2327, 0.2312, 0.2351, 0.3976, 0.2860, 0.2607, 0.2356,
                  0.2297, 0.2268, 0.2241, 0.2320};
    }

    struct CalibrationData {
        Handle<Quote> spot;
        Handle<YieldTermStructure> rates, dividends;
        std::vector<ext::shared_ptr<CalibrationHelper>> helpers;
    };

    // builds the curves and the calibration instruments from the market data
    CalibrationData build(const Setup& s, const Market& m) {
        CalibrationData c;
        DayCounter dc = Actual365Fixed();
        c.spot = Handle<Quote>(ext::make_shared<SimpleQuote>(m.spot));
        c.rates = Handle<YieldTermStructure>(ext::make_shared<ZeroCurve>(s.curveDates, m.zeroRates, dc));
        c.dividends = Handle<YieldTermStructure>(ext::make_shared<FlatForward>(s.today, 0.0, dc));
        for (Size i = 0; i < s.strikes.size(); ++i) {
            for (Size j = 0; j < s.expiries.size(); ++j) {
                Handle<Quote> vol(ext::make_shared<SimpleQuote>(m.vols[i * s.expiries.size() + j]));
                c.helpers.push_back(ext::make_shared<HestonModelHelper>(
                    s.expiries[j], TARGET(), c.spot, s.strikes[i], vol, c.rates, c.dividends,
                    BlackCalibrationHelper::PriceError));
            }
        }
        return c;
    }

    ext::shared_ptr<HestonModel> makeModel(const CalibrationData& c, const std::vector<Real>& p) {
        auto process = ext::make_shared<HestonProcess>(c.rates, c.dividends, c.spot, p[4], p[1],
                                                       p[0], p[2], p[3]);
        auto model = ext::make_shared<HestonModel>(process);
        auto engine = ext::make_shared<COSHestonEngine>(model, 12, 75);
        for (const auto& helper : c.helpers)
            ext::dynamic_pointer_cast<BlackCalibrationHelper>(helper)->setPricingEngine(engine);
        return model;
    }

    std::vector<Real> calibrate(const Setup& s, const Market& m, const std::vector<Real>& start) {
        CalibrationData c = build(s, m);
        auto model = makeModel(c, start);
        LevenbergMarquardt optimizer(tolerance, tolerance, tolerance);
        model->calibrate(c.helpers, optimizer,
                         EndCriteria(1000, 100, tolerance, tolerance, tolerance));
        Array p = model->params();
        return std::vector<Real>(p.begin(), p.end());
    }

    // the forward-start call: strike set at the money in 6 months, expiry in 18 months;
    // the payoff's strike is a placeholder, the engine uses the moneyness
    Real price(const Setup& s, const Market& m, const std::vector<Real>& p) {
        CalibrationData c = build(s, m);
        auto process = ext::make_shared<HestonProcess>(c.rates, c.dividends, c.spot, p[4], p[1],
                                                       p[0], p[2], p[3]);
        ForwardVanillaOption option(1.0, s.today + 6 * Months,
                                    ext::make_shared<PlainVanillaPayoff>(Option::Call, 0.0),
                                    ext::make_shared<EuropeanExercise>(s.today + 18 * Months));
        option.setPricingEngine(ext::make_shared<AnalyticHestonForwardEuropeanEngine>(process));
        return option.NPV();
    }

    // bump each input, recalibrate from the calibrated parameters and reprice
    std::vector<double> bumpAndRecalibrate(const Setup& s,
                                           const Market& market,
                                           const std::vector<Real>& p,
                                           const std::vector<Size>& which) {
        Real v = price(s, market, p);
        std::vector<double> sensitivities;
        for (Size i : which) {
            Market m = market;
            Real* x = inputs(m)[i];
            Real h = (i == 0) ? 1.0 : 1e-4; // index 0 is the spot
            *x += h;
            Real bumped = price(s, m, calibrate(s, m, p));
            sensitivities.push_back(double(xad::value((bumped - v) / h)));
        }
        return sensitivities;
    }

    // the largest sensitivities first
    std::vector<Size> largest(const std::vector<double>& sensitivities, Size n) {
        std::vector<Size> order(sensitivities.size());
        for (Size i = 0; i < order.size(); ++i)
            order[i] = i;
        std::sort(order.begin(), order.end(), [&](Size a, Size b) {
            return std::fabs(sensitivities[a]) > std::fabs(sensitivities[b]);
        });
        order.resize(std::min(n, order.size()));
        return order;
    }

    double secondsSince(std::chrono::steady_clock::time_point start) {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }

#ifndef QLAAD_DISABLE_AAD

    using tape_type = Real::tape_type;

    // gradient of f(p, m) with respect to the parameters and the market inputs
    template <class F>
    void gradient(tape_type& tape,
                  F f,
                  const std::vector<Real>& parameters,
                  const Market& market,
                  std::vector<double>& dp,
                  std::vector<double>& dm) {
        tape.clearAll();
        std::vector<Real> p = parameters;
        Market m = market;
        auto x = inputs(m);
        for (auto& pi : p)
            tape.registerInput(pi);
        for (auto* xi : x)
            tape.registerInput(*xi);
        tape.newRecording();

        Real y = f(p, m);

        tape.registerOutput(y);
        derivative(y) = 1.0;
        tape.computeAdjoints();

        dp.resize(p.size());
        for (Size k = 0; k < p.size(); ++k)
            dp[k] = derivative(p[k]);
        dm.resize(x.size());
        for (Size j = 0; j < x.size(); ++j)
            dm[j] = derivative(*x[j]);
    }

    // solves A x = b for a small dense system, with partial pivoting
    std::vector<double> solve(std::vector<std::vector<double>> A, std::vector<double> b) {
        Size n = b.size();
        for (Size c = 0; c < n; ++c) {
            Size pivot = c;
            for (Size r = c + 1; r < n; ++r)
                if (std::fabs(A[r][c]) > std::fabs(A[pivot][c]))
                    pivot = r;
            std::swap(A[c], A[pivot]);
            std::swap(b[c], b[pivot]);
            for (Size r = c + 1; r < n; ++r) {
                double factor = A[r][c] / A[c][c];
                for (Size k = c; k < n; ++k)
                    A[r][k] -= factor * A[c][k];
                b[r] -= factor * b[c];
            }
        }
        std::vector<double> x(n);
        for (Size c = n; c-- > 0;) {
            double sum = b[c];
            for (Size k = c + 1; k < n; ++k)
                sum -= A[c][k] * x[k];
            x[c] = sum / A[c][c];
        }
        return x;
    }

    // sensitivities of the price to the market inputs, through the calibrated parameters
    std::vector<double> implicitFunctionTheorem(tape_type& tape,
                                               const Setup& s,
                                               const Market& m,
                                               const std::vector<Real>& p) {
        // the sum of squared calibration errors, which calibrate() minimizes
        auto objective = [&s](const std::vector<Real>& q, const Market& market) {
            CalibrationData c = build(s, market);
            makeModel(c, q);
            Real f = 0.0;
            for (const auto& helper : c.helpers) {
                Real error = helper->calibrationError();
                f += error * error;
            }
            return f;
        };
        auto target = [&s](const std::vector<Real>& q, const Market& market) {
            return price(s, market, q);
        };

        // Hessian blocks of the objective, from central differences of its gradient
        Market copy = m;
        Size n = p.size(), nm = inputs(copy).size();
        std::vector<std::vector<double>> Hpp(n, std::vector<double>(n));
        std::vector<std::vector<double>> Hmp(nm, std::vector<double>(n));
        for (Size k = 0; k < n; ++k) {
            double h = 1e-4 * std::max(std::fabs(value(p[k])), 0.01);
            std::vector<Real> up = p, down = p;
            up[k] += h;
            down[k] -= h;
            std::vector<double> dpUp, dmUp, dpDown, dmDown;
            gradient(tape, objective, up, m, dpUp, dmUp);
            gradient(tape, objective, down, m, dpDown, dmDown);
            for (Size i = 0; i < n; ++i)
                Hpp[i][k] = (dpUp[i] - dpDown[i]) / (2.0 * h);
            for (Size j = 0; j < nm; ++j)
                Hmp[j][k] = (dmUp[j] - dmDown[j]) / (2.0 * h);
        }

        // dV/dp and the direct dV/dm, with the parameters held fixed
        std::vector<double> dVdp, dVdm;
        gradient(tape, target, p, m, dVdp, dVdm);

        std::vector<double> lambda = solve(Hpp, dVdp);
        for (Size j = 0; j < nm; ++j)
            for (Size k = 0; k < n; ++k)
                dVdm[j] -= Hmp[j][k] * lambda[k];
        return dVdm;
    }

    std::map<std::string, double> readCsv(const std::string& file, double& seconds) {
        std::ifstream in(file);
        QL_REQUIRE(in, "cannot open " << file);
        std::string line;
        std::getline(in, line);
        QL_REQUIRE(line == "input,bump_and_recalibrate",
                   file << " is not from the QLAAD_DISABLE_AAD=ON build");
        std::map<std::string, double> values;
        while (std::getline(in, line)) {
            auto comma = line.rfind(',');
            if (comma != std::string::npos)
                values[line.substr(0, comma)] = std::stod(line.substr(comma + 1));
        }
        QL_REQUIRE(values.count("seconds"), file << " has no timing");
        seconds = values["seconds"];
        return values;
    }

#endif

}

int main(int argc, char* argv[]) {
    try {
        std::string csvFile, compareFile;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--csv" && i + 1 < argc)
                csvFile = argv[++i];
            else if (arg == "--compare" && i + 1 < argc)
                compareFile = argv[++i];
        }

        Setup s;
        Market m;
        makeDaxMarket(s, m);
        Settings::instance().evaluationDate() = s.today;
        auto names = inputNames(s);

        auto start = std::chrono::steady_clock::now();
        std::vector<Real> p = calibrate(s, m, {0.1, 1.0, 0.5, -0.5, 0.1});
        double calibrationSeconds = secondsSince(start);
        Real v = price(s, m, p);

        std::cout << std::fixed << std::setprecision(4)
                  << "Heston model calibrated to " << m.vols.size() << " DAX option prices in "
                  << std::setprecision(2) << calibrationSeconds << " s:\n ";
        for (Size k = 0; k < p.size(); ++k)
            std::cout << " " << parameterNames[k] << " " << std::setprecision(4) << p[k];
        std::cout << "\nForward-start call, strike set in 6 months, expiry in 18 months: "
                  << std::setprecision(2) << v << "\n\n";

#ifndef QLAAD_DISABLE_AAD

        double bumpSeconds = 0.0;
        std::map<std::string, double> bumped;
        if (!compareFile.empty())
            bumped = readCsv(compareFile, bumpSeconds);

        tape_type tape;
        start = std::chrono::steady_clock::now();
        std::vector<double> ift = implicitFunctionTheorem(tape, s, m, p);
        double iftSeconds = secondsSince(start);

        std::cout << "Sensitivities to all " << names.size()
                  << " market inputs with the implicit function theorem: " << std::setprecision(2)
                  << iftSeconds << " s\n\n";

        if (bumped.empty()) {
            // check the largest ones by bumping and recalibrating, with the XAD type
            auto top = largest(ift, 8);
            auto fd = bumpAndRecalibrate(s, m, p, top);
            std::cout << std::setw(20) << "input" << " | " << std::setw(10) << "IFT" << " | "
                      << "bump & recalibrate\n"
                      << std::string(21, '-') << "+" << std::string(12, '-') << "+"
                      << std::string(19, '-') << "\n";
            for (Size k = 0; k < top.size(); ++k)
                std::cout << std::setw(20) << names[top[k]] << " | " << std::setw(10)
                          << std::setprecision(4) << ift[top[k]] << " | " << std::setw(10)
                          << fd[k] << "\n";
            std::cout << "\nFor bump & recalibrate on all inputs with plain doubles, run the "
                         "QLAAD_DISABLE_AAD=ON build\nwith \"--csv bumping.csv\" and pass the file "
                         "here with \"--compare bumping.csv\".\n";
        } else {
            double maxDifference = 0.0, maxSensitivity = 0.0;
            for (Size i = 0; i < names.size(); ++i) {
                auto b = bumped.find(names[i]);
                QL_REQUIRE(b != bumped.end(), "no bumped value for " << names[i]);
                maxDifference = std::max(maxDifference, std::fabs(ift[i] - b->second));
                maxSensitivity = std::max(maxSensitivity, std::fabs(ift[i]));
            }
            std::cout << std::setw(20) << "input" << " | " << std::setw(10) << "IFT" << " | "
                      << "bump & recalibrate\n"
                      << std::string(21, '-') << "+" << std::string(12, '-') << "+"
                      << std::string(19, '-') << "\n";
            for (Size i : largest(ift, 10))
                std::cout << std::setw(20) << names[i] << " | " << std::setw(10)
                          << std::setprecision(4) << ift[i] << " | " << std::setw(10)
                          << bumped[names[i]] << "\n";
            std::cout << "\nLargest difference over all inputs: " << std::setprecision(4)
                      << maxDifference << " (largest sensitivity " << maxSensitivity << ")\n"
                      << "Bump & recalibrate with doubles: " << std::setprecision(2)
                      << bumpSeconds << " s, implicit function theorem with XAD: " << iftSeconds
                      << " s, speedup " << std::setprecision(0) << bumpSeconds / iftSeconds
                      << "x\n";
        }

#else

        std::vector<Size> all(names.size());
        for (Size i = 0; i < all.size(); ++i)
            all[i] = i;
        start = std::chrono::steady_clock::now();
        std::vector<double> fd = bumpAndRecalibrate(s, m, p, all);
        double bumpSeconds = secondsSince(start);

        std::cout << "Sensitivities to all " << names.size()
                  << " market inputs by bumping and recalibrating: " << std::setprecision(2)
                  << bumpSeconds << " s\n\n"
                  << std::setw(20) << "input" << " | " << "bump & recalibrate\n"
                  << std::string(21, '-') << "+" << std::string(19, '-') << "\n";
        for (Size i : largest(fd, 10))
            std::cout << std::setw(20) << names[i] << " | " << std::setw(10)
                      << std::setprecision(4) << fd[i] << "\n";

        if (!csvFile.empty()) {
            std::ofstream out(csvFile);
            QL_REQUIRE(out, "cannot write " << csvFile);
            out << "input,bump_and_recalibrate\n"
                << "seconds," << std::setprecision(6) << bumpSeconds << "\n";
            for (Size i = 0; i < names.size(); ++i)
                out << names[i] << "," << std::setprecision(10) << fd[i] << "\n";
            std::cout << "\nSensitivities written to " << csvFile << "\n";
        }

#endif

        return 0;
    } catch (std::exception& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "unknown error" << std::endl;
        return 1;
    }
}

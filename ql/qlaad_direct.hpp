/*******************************************************************************

   This file is part of QuantLibAAD, an adaptor module to enable using XAD with
   QuantLib. XAD is a fast and comprehensive C++ library for
   automatic differentiation.

   Copyright (C) 2010-2026 Xcelerit Computing Ltd.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU Affero General Public License as published
   by the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.

******************************************************************************/

// Real = xad::ARealDirect<double> (XAD direct mode), selected by QLAAD_DIRECT_MODE=ON

#pragma once

#include <boost/math/special_functions/math_fwd.hpp>
#include <boost/math/tools/promotion.hpp>
#include <boost/type_traits/is_arithmetic.hpp>
#include <boost/type_traits/is_floating_point.hpp>
#include <boost/type_traits/is_pod.hpp>
#include <XAD/Complex.hpp>
#include <XAD/StdCompatibility.hpp>
#include <XAD/XAD.hpp>
#include <type_traits>

#define QL_REAL xad::ARealDirect<double>

// older Boost promotes ARealDirect to a built-in type in its special functions, losing derivatives
static_assert(std::is_same<boost::math::tools::promote_args<xad::ARealDirect<double>>::type,
                           xad::ARealDirect<double>>::value,
              "XAD direct mode needs Boost 1.87 or later");

// Boost specializations
namespace boost {

    namespace math {

        // Boost's rounding declares constexpr locals of arithmetic types; ARealDirect isn't literal
        inline long long lltrunc(const xad::ARealDirect<double>& v) {
            return boost::math::lltrunc(xad::value(v));
        }
        template <class Policy>
        inline long long llround(const xad::ARealDirect<double>& v, const Policy& p) {
            return boost::math::llround(xad::value(v), p);
        }
        template <class Policy>
        inline int itrunc(const xad::ARealDirect<double>& v, const Policy& p) {
            return boost::math::itrunc(xad::value(v), p);
        }
        template <class Policy>
        inline int iround(const xad::ARealDirect<double>& v, const Policy& p) {
            return boost::math::iround(xad::value(v), p);
        }
    }

    // needed by accumulators' max, numeric_cast range checks and ublas element construction
    template <>
    struct is_floating_point<xad::ARealDirect<double>> : public true_type {};
    template <>
    struct is_arithmetic<xad::ARealDirect<double>> : public true_type {};
    template <>
    struct is_pod<xad::ARealDirect<double>> : public false_type {};
}

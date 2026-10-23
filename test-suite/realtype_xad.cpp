/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
 Copyright (C) 2026 Xcelerit Computing Limited

 This file is part of QuantLib / XAD integration module.

 QuantLib is free software: you can redistribute it and/or modify it
 under the terms of the QuantLib license.  You should have received a
 copy of the license along with this program; if not, please email
 <quantlib-dev@lists.sf.net>. The license is also available online at
 <http://quantlib.org/license.shtml>.

 This program is distributed in the hope that it will be useful, but WITHOUT
 ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 FOR A PARTICULAR PURPOSE.  See the license for more details.
*/

#include "toplevelfixture.hpp"
#include "utilities_xad.hpp"
#include <ql/math/functional.hpp>
#include <ql/math/statistics/incrementalstatistics.hpp>
#include <ql/types.hpp>
#include <boost/math/special_functions/fpclassify.hpp>
#include <boost/math/special_functions/round.hpp>
#include <boost/math/special_functions/trunc.hpp>
#include <boost/numeric/conversion/cast.hpp>
#include <boost/type_traits/has_trivial_constructor.hpp>
#include <type_traits>

using namespace QuantLib;
using namespace boost::unit_test_framework;

#ifdef QLAAD_TEST_DIRECT_MODE
static_assert(std::is_same<Real, xad::ARealDirect<double>>::value, "direct mode not selected");
#endif

BOOST_FIXTURE_TEST_SUITE(QuantLibAADTests, TopLevelFixture)

BOOST_AUTO_TEST_SUITE(RealTypeXadTests)


BOOST_AUTO_TEST_CASE(boostRoundingAndClassification) {

    BOOST_TEST_MESSAGE("Testing boost rounding and classification of Real...");

    Real x = 2.6;

    BOOST_CHECK_EQUAL(boost::math::iround(x), 3);
    BOOST_CHECK_EQUAL(boost::math::llround(x), 3LL);
    BOOST_CHECK_EQUAL(boost::math::itrunc(x), 2);
    BOOST_CHECK_EQUAL(boost::math::lltrunc(x), 2LL);
    BOOST_CHECK_EQUAL((boost::math::fpclassify)(x), FP_NORMAL);
    BOOST_CHECK((boost::math::isfinite)(x));
    BOOST_CHECK(!(boost::math::isinf)(x));
}

BOOST_AUTO_TEST_CASE(squaredOfRealAndExpressions) {

    BOOST_TEST_MESSAGE("Testing squared() of Real and of expressions...");

    Real x = 3.0;
    BOOST_CHECK(squared(x) == 9.0);
    BOOST_CHECK(squared(2.0 * x) == 36.0);
    BOOST_CHECK(squared(-x) == 9.0);
}

BOOST_AUTO_TEST_CASE(boostTypeTraits) {

    BOOST_TEST_MESSAGE("Testing boost type traits of Real...");

    IncrementalStatistics s;
    s.add(-2.0);
    s.add(-1.0);
    BOOST_CHECK(s.max() == -1.0);

#ifdef QLAAD_TEST_DIRECT_MODE // numeric_cast doesn't compile for expression templates
    BOOST_CHECK_THROW(boost::numeric_cast<int>(Real(1e20)), boost::numeric::bad_numeric_cast);
#endif

    BOOST_CHECK_EQUAL(boost::has_trivial_constructor<Real>::value,
                      std::is_trivially_default_constructible<Real>::value);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE_END()

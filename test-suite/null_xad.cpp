/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
 Copyright (C) 2024, 2025, 2026 Xcelerit Computing Limited

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

#include "toplevelfixture.hpp"
#include "utilities_xad.hpp"
#include <ql/utilities/null.hpp>


using namespace QuantLib;
using namespace boost::unit_test_framework;

BOOST_FIXTURE_TEST_SUITE(QuantLibAADTests, TopLevelFixture)

BOOST_AUTO_TEST_SUITE(NullXadTests)


BOOST_AUTO_TEST_CASE(nullRealReturnsFloatMax) {

    BOOST_TEST_MESSAGE("Testing null<Real>...");

    Real v = Null<Real>();

    // compare
    QL_CHECK_CLOSE(v, std::numeric_limits<float>::max(), 1e-9);
}

BOOST_AUTO_TEST_CASE(nullIntReturnsIntMax) {

    BOOST_TEST_MESSAGE("Testing null<int>...");

    int v = Null<int>();
    unsigned vu = Null<unsigned>();
    long vl = Null<long>();
    long long vll = Null<long long>();
    unsigned long vul = Null<unsigned long>();
    unsigned long long vull = Null<unsigned long long>();

    // compare
    BOOST_CHECK_EQUAL(v, std::numeric_limits<int>::max());
    BOOST_CHECK_EQUAL(vu, std::numeric_limits<int>::max());
    BOOST_CHECK_EQUAL(vl, std::numeric_limits<int>::max());
    BOOST_CHECK_EQUAL(vll, std::numeric_limits<int>::max());
    BOOST_CHECK_EQUAL(vul, std::numeric_limits<int>::max());
    BOOST_CHECK_EQUAL(vull, std::numeric_limits<int>::max());
}

BOOST_AUTO_TEST_CASE(nullOtherReturnsDefault) {
    BOOST_TEST_MESSAGE("Testing Null<class>...");

    struct Tester {
        Tester() : v_(42) {}
        int v_;
    };

    Tester t = Null<Tester>();

    BOOST_CHECK_EQUAL(t.v_, 42);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE_END()

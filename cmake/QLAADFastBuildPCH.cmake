# Precompiled headers per QuantLib target (tools/pch-select.py on a build without PCH, ninja -t deps):
# only headers every file of the target includes anyway, so the PCH adds no declarations.
# qlaad_pch_<n>: target, headers; qlaad_pch_<n>_skip: its files (relative to QuantLib) that keep no PCH.
set(qlaad_pch_0 "ql_library" <ql/errors.hpp>)
set(qlaad_pch_0_skip
    ql/experimental/commodities/commoditytype.cpp
    ql/experimental/commodities/unitofmeasure.cpp
    ql/experimental/math/piecewiseintegral.cpp
    ql/indexes/region.cpp
    ql/math/errorfunction.cpp
    ql/math/expm1.cpp
    ql/math/optimization/lmdif.cpp
    ql/math/pascaltriangle.cpp
    ql/math/primenumbers.cpp
    ql/math/randomnumbers/knuthuniformrng.cpp
    ql/math/randomnumbers/latticersg.cpp
    ql/math/randomnumbers/lecuyeruniformrng.cpp
    ql/math/randomnumbers/mt19937uniformrng.cpp
    ql/math/randomnumbers/primitivepolynomials.cpp
    ql/math/randomnumbers/seedgenerator.cpp
    ql/math/randomnumbers/xoshiro256starstaruniformrng.cpp
    ql/methods/finitedifferences/operators/fdmlinearoplayout.cpp
    ql/methods/finitedifferences/utilities/squarerootprocessrndcalculator.cpp
    ql/models/marketmodels/models/alphaformconcrete.cpp
    ql/models/marketmodels/products/multistep/multistepnothing.cpp
    ql/time/calendars/islamicholidays.cpp
    ql/utilities/dataformatters.cpp
    ql/version.cpp)
set(qlaad_pch_1 "ql_test" <boost/test/unit_test.hpp> <ql/termstructures/yieldtermstructure.hpp>)
set(qlaad_pch_1_skip
    test-suite/zigguratgaussian.cpp
    test-suite/quantlibglobalfixture.cpp)

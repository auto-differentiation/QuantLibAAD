# Checks that a QuantLib build directory registers each of QuantLib's Boost.Test suites as
# its own CTest test, named QuantLibTests/<Suite>, so that ctest -j runs them in parallel.
#
#   cmake -DBUILD_DIR=<QuantLib build dir> -P tools/check-ctest-per-suite.cmake

if(NOT BUILD_DIR)
    message(FATAL_ERROR "usage: cmake -DBUILD_DIR=<QuantLib build dir> -P ${CMAKE_CURRENT_LIST_FILE}")
endif()

execute_process(COMMAND ${CMAKE_CTEST_COMMAND} -N -R "^QuantLibTests/"
                WORKING_DIRECTORY ${BUILD_DIR}
                OUTPUT_VARIABLE out RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "ctest -N failed (${rc}):\n${out}")
endif()

string(REGEX MATCHALL "Test +#[0-9]+: QuantLibTests/[A-Za-z0-9_]+" tests "${out}")
list(LENGTH tests n)
if(n LESS 100)
    message(FATAL_ERROR "expected more than 100 QuantLibTests/<Suite> ctest tests, found ${n}:\n${out}")
endif()
if(NOT out MATCHES "Test +#[0-9]+: QuantLibTests/AmericanOptionTests\n")
    message(FATAL_ERROR "no ctest test QuantLibTests/AmericanOptionTests:\n${out}")
endif()
message(STATUS "${n} QuantLibTests/<Suite> ctest tests")

# Runs the American option sample and checks its exit code, its value and its greeks.
#   cmake -DEXAMPLE=<executable> -DDISABLE_AAD=<ON|OFF> -P CheckExample.cmake
#
# The value must match the one below within 1e-8 relative in both builds: it is the
# same double computation in both, so only rounding that differs between compilers
# and platforms moves it, while a change to its inputs, engine or grid that shows in
# the 8th digit fails. Each greek must match the XAD value below, relative to that
# value: within 1e-7 with XAD (the print has 10 significant digits), and within 1e-4
# with doubles. The finite-difference price is smooth in every input: central bumps
# with h from 1e-2 to 1e-6 agree with each other, and those with h near 1e-4 are
# within 4e-6 of XAD; 1e-4 also leaves room for a forward difference of that size.
#
# Dividend Rho at the sample's dividend yield of 0 needs QuantLib 1.45 or later
# (lballabio/QuantLib#3057, which keeps the derivative of an implied rate when the
# compound factor is 1); before it, XAD gave 3.08e-5, while bumping gives 9.0464.

set(value 4.486113273) # from the XAD build
set(names "Rho" "Strike Sensitivity" "Vega" "Delta" "Dividend Rho")
set(reference -10.34248692 0.7392268427 10.93683447 -0.6967489009 9.046354776) # from XAD
if(DISABLE_AAD)
    set(tolerance 1e-4)
else()
    set(tolerance 1e-7)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../CheckSample.cmake)
run_example(output)
set(output "\n${output}")

set(printed)
foreach(name IN LISTS names)
    if(output MATCHES "\n${name} *= ([^\n]*)")
        list(APPEND printed ${CMAKE_MATCH_1})
    endif()
endforeach()
list(LENGTH printed count)
list(LENGTH names expected)
if(NOT count EQUAL expected)
    message(FATAL_ERROR "printed ${count} greeks, expected ${expected}")
endif()

math(EXPR last "${count} - 1")
foreach(index RANGE ${last})
    list(GET names ${index} name)
    list(GET printed ${index} greek)
    list(GET reference ${index} stored)
    check_printed_close("${name}" ${greek} ${stored} ${tolerance} 0)
endforeach()

if(NOT output MATCHES "\nAmerican equity option value: ([^\n]*)")
    message(FATAL_ERROR "printed no value")
endif()
check_printed_close("value" ${CMAKE_MATCH_1} ${value} 1e-8 0)
message("checked the value and ${count} greeks")

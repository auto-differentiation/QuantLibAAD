# Runs the CDS sample and checks its exit code, its value and its sensitivities.
#   cmake -DEXAMPLE=<executable> -DDISABLE_AAD=<ON|OFF> -P CheckExample.cmake
#
# The value must be printed exactly as below in both builds. Each sensitivity must
# match the XAD value below, relative to that value: within 1e-7 with XAD (the print
# has 6 significant digits, so this means the same print, up to the rounding of the
# last digit), and within 5e-4 with doubles. Bumping is a forward difference, off by
# about h * T / 2 relative for a step h and times T of up to 2 years here: 1e-5 with
# h = 1e-5 (measured: within 1e-5, the rounding of the print), and 1e-4 with h = 1e-4.
# 5e-4 leaves a margin of 50 and still catches a wrong sign or a shifted entry, off
# by 3% or more. Where XAD gives exactly 0, the value does not depend on that input,
# and bumping it only adds round-off, about 1e-16 * value / h, near 1e-9 here, so
# doubles may differ from 0 by up to 1e-6; that is far above the round-off and below
# 5e-4 of the smallest non-zero sensitivity, the notional's 0.00486724, so it never
# loosens a check that is not of a zero.

set(value 48.6724) # from the XAD build
set(reference # from the XAD build: rhos, hazard rates, recovery, fixed rate, notional
    0 0 0 7.71442 -22.3278 12.2303 -46.1047 -7.2333 0
    0 -215.432 -464.008 -563.438 -1491.45 -1474.51 -1432.88 -354.194 0
    115.967 9854.37 0.00486724)
if(DISABLE_AAD)
    set(tolerance 5e-4)
    set(floor 1e-6)
else()
    set(tolerance 1e-7)
    set(floor 0)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../CheckSample.cmake)
run_example(output)
set(output "\n${output}")

# appends the numbers printed after "<name> = " to the list printed, and their names
# to the list labels; a list like "[0, 1.5, ]" adds one number per entry, as <name>[<i>]
function(collect name)
    if(NOT output MATCHES "\n${name} *= ([^\n]*)")
        return()
    endif()
    set(numbers "${CMAKE_MATCH_1}")
    if(numbers MATCHES "^\\[(.*)\\]$")
        string(REGEX MATCHALL "[^ ,]+" numbers "${CMAKE_MATCH_1}")
        set(index 0)
        foreach(number IN LISTS numbers)
            list(APPEND printed ${number})
            list(APPEND labels "${name}[${index}]")
            math(EXPR index "${index} + 1")
        endforeach()
    else()
        list(APPEND printed ${numbers})
        list(APPEND labels "${name}")
    endif()
    set(printed ${printed} PARENT_SCOPE)
    set(labels ${labels} PARENT_SCOPE)
endfunction()

set(printed)
set(labels)
foreach(name IN ITEMS "Rhos" "Hazard rate sensitivities" "Recovery rate sensitivity"
                      "Fixed rate sensitivity" "Notional sensitivity")
    collect("${name}")
endforeach()
list(LENGTH printed count)
list(LENGTH reference expected)
if(NOT count EQUAL expected)
    message(FATAL_ERROR "printed ${count} sensitivities, expected ${expected}")
endif()

if(NOT output MATCHES "\nCDS value *= ([^\n]*)")
    message(FATAL_ERROR "printed no CDS value")
endif()
if(NOT CMAKE_MATCH_1 STREQUAL value)
    message(FATAL_ERROR "CDS value: ${CMAKE_MATCH_1} differs from ${value}")
endif()

math(EXPR last "${count} - 1")
foreach(index RANGE ${last})
    list(GET labels ${index} label)
    list(GET printed ${index} number)
    list(GET reference ${index} stored)
    check_printed_close("${label}" ${number} ${stored} ${tolerance} ${floor})
endforeach()
message("checked the value and ${count} sensitivities")

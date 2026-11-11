# Runs the trade risk sample and checks its exit code and output.
#   cmake -DEXAMPLE=<executable> -DDISABLE_AAD=<ON|OFF> -P CheckExample.cmake
#
# With XAD:
# - the sensitivities of all swaps to all quotes must match a full recording of curve
#   and swaps within 1e-11 of the largest of them (they agree to about 1e-15). That
#   checks the chain rule from the curve nodes to the quotes, but not the recording
#   of the bootstrap, which both share;
# - those of swap 1 (6Y) and of the longest swap (30Y) must match central differences,
#   which check the bootstrap too, within 1e-5 relative to each sensitivity, or of
#   100 for smaller ones, where rounding in the central differences (1e-5 step, up to
#   2e-4 off) dominates. They agree to 1.7e-6 with that floor, and to 1e-6 above it.
# Both builds print the 5 largest sensitivities of swap 1, which must match the
# XAD values below relative to each value: within 1e-7 with XAD, and within 1e-3
# with doubles (bumping, up to 1e-6 off here), or by 1 in the last digit.

set(largest_of_swap_1 # from the XAD build
    "swap 6Y=47097966" "swap 5Y=120035" "swap 4Y=95807" "swap 3Y=72321" "swap 2Y=47720")

include(${CMAKE_CURRENT_LIST_DIR}/../CheckSample.cmake)
run_example(output)

if(NOT DISABLE_AAD)
    set(sensitivity "largest sensitivity ([^)]+)")
    set(difference "largest difference ([^ \n]+)")
    if(NOT output MATCHES "full recording[^\n]*\\(${sensitivity}\\): ${difference}\n")
        message(FATAL_ERROR "no check against a full recording in the output")
    endif()
    check_relative("all swaps, AAD vs full recording" ${CMAKE_MATCH_2} ${CMAKE_MATCH_1} 11)
    # fails unless the swap's line shows a match within 1e-5
    function(check_central_differences label swap)
        set(relative "largest relative difference ([^ \n]+)\n")
        if(NOT output MATCHES "\n  swap ${swap} by central differences: ${relative}")
            message(FATAL_ERROR "no check of ${label} against central differences")
        endif()
        check_relative("${label}, AAD vs central differences" ${CMAKE_MATCH_1} 1.0e+00 5)
    endfunction()
    check_central_differences("swap 1 (6y)" "1 \\(6y\\)")
    check_central_differences("the longest swap (30y)" "[0-9]+ \\(30y\\)")
endif()

string(REGEX MATCHALL "\n +[^|\n]+ \\| +-?[0-9,]+" rows "${output}")
set(printed "")
foreach(row IN LISTS rows)
    string(REGEX REPLACE "^\n +([^|]*[^ |]) \\| +" "\\1=" row "${row}")
    list(APPEND printed "${row}")
endforeach()
list(LENGTH printed count)
list(LENGTH largest_of_swap_1 expected)
if(NOT count EQUAL expected)
    message(FATAL_ERROR "printed ${count} sensitivities of swap 1, expected ${expected}")
endif()

if(DISABLE_AAD)
    set(tolerance 1e-3)
else()
    set(tolerance 1e-7)
endif()
set(index 0)
foreach(reference IN LISTS largest_of_swap_1)
    list(GET printed ${index} row)
    math(EXPR index "${index} + 1")
    string(REGEX REPLACE "=.*" "" input "${reference}")
    if(NOT row MATCHES "^${input}=(.+)$")
        message(FATAL_ERROR "printed ${row}, expected ${input} in its place")
    endif()
    set(value ${CMAKE_MATCH_1})
    string(REGEX REPLACE ".*=" "" reference "${reference}")
    check_relative_close("swap 1, ${input}" ${value} ${reference} ${tolerance} 1)
endforeach()
message("checked the sensitivities of swap 1")

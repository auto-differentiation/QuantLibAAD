# Runs the scaling sample and checks its exit code and output.
#   cmake -DEXAMPLE=<executable> -DDISABLE_AAD=<ON|OFF> -P CheckExample.cmake
#
# With XAD, the AAD sensitivities on every surface must match central differences
# within 1e-6 relative to each sensitivity, or absolute where it is below 1 (the
# sample prints that largest relative difference): here they agree to about 3e-8,
# the rounding of central differences with their 1e-5 step, so this leaves 30
# times that, and catches a relative error of 1e-6 on any sensitivity of 1 or
# more and an absolute one of 1e-6 on smaller ones.
# Both builds print the sensitivities on the smallest surface, which must match the
# AAD values below relative to each value: within 1e-7 with XAD, and within 2e-4
# with doubles (forward differences with a 1e-6 step, up to 3.8e-5 off here), or by
# 1 in the last digit.
# The doubles build must write its timings with --csv, and the XAD build must read
# such a file with --compare, and reject a line it can't read.

set(surface_sizes 29 69 159 309 509)
set(sensitivities # from the XAD build, in input order
    "spot=30.5118" "dividend yield=-8317.6973" "zero rate 0M=-9.0542" "zero rate 3M=53.7290"
    "zero rate 6M=93.0703" "zero rate 12M=-164.6414" "zero rate 24M=-499.1962"
    "zero rate 36M=-765.4462" "zero rate 60M=695.2588"
    "vol strike 1 expiry 1=1.6368" "vol strike 1 expiry 2=96.8043"
    "vol strike 1 expiry 3=151.0398" "vol strike 1 expiry 4=109.3506"
    "vol strike 2 expiry 1=20.0936" "vol strike 2 expiry 2=761.2400"
    "vol strike 2 expiry 3=1291.7144" "vol strike 2 expiry 4=657.3659"
    "vol strike 3 expiry 1=80.6123" "vol strike 3 expiry 2=1492.7857"
    "vol strike 3 expiry 3=1747.2612" "vol strike 3 expiry 4=981.4651"
    "vol strike 4 expiry 1=26.4961" "vol strike 4 expiry 2=1137.8244"
    "vol strike 4 expiry 3=1627.7945" "vol strike 4 expiry 4=747.0868"
    "vol strike 5 expiry 1=2.0451" "vol strike 5 expiry 2=82.5807"
    "vol strike 5 expiry 3=284.1082" "vol strike 5 expiry 4=132.3599")

include(${CMAKE_CURRENT_LIST_DIR}/../CheckSample.cmake)
run_example(output)

if(DISABLE_AAD)
    set(tolerance 2e-4)
else()
    set(tolerance 1e-7)
    foreach(size IN LISTS surface_sizes)
        set(check "central differences, ${size} inputs: [^\n]*")
        if(NOT output MATCHES "${check}largest relative difference ([^ \n]+)\n")
            message(FATAL_ERROR "no check against central differences for ${size} inputs")
        endif()
        if(NOT CMAKE_MATCH_1 LESS_EQUAL 1e-6)
            message(FATAL_ERROR
                    "${size} inputs, AAD vs central differences: ${CMAKE_MATCH_1} is over 1e-6")
        endif()
    endforeach()
endif()

string(REGEX MATCHALL "\n +[0-9]+ \\|" rows "${output}")
list(LENGTH rows count)
list(LENGTH surface_sizes expected)
if(NOT count EQUAL expected)
    message(FATAL_ERROR "timings for ${count} surface sizes, expected ${expected}")
endif()

string(FIND "${output}" "Sensitivities on the 29-input surface:" start)
if(start EQUAL -1)
    message(FATAL_ERROR "no sensitivities on the smallest surface in the output")
endif()
string(SUBSTRING "${output}" ${start} -1 output)
string(REGEX MATCHALL "\n[a-z][^|\n]* \\| -?[0-9]+\\.[0-9][0-9][0-9][0-9]" printed "${output}")
list(LENGTH printed count)
list(LENGTH sensitivities expected)
if(NOT count EQUAL expected)
    message(FATAL_ERROR "printed ${count} sensitivities, expected ${expected}")
endif()

set(index 0)
foreach(line IN LISTS printed)
    list(GET sensitivities ${index} stored)
    math(EXPR index "${index} + 1")
    string(REGEX MATCH "^(.+)=(.+)$" stored "${stored}")
    set(input "${CMAKE_MATCH_1}")
    set(reference "${CMAKE_MATCH_2}")
    if(NOT line MATCHES "^\n${input} \\| (.+)$")
        message(FATAL_ERROR "printed${line}, expected ${input} in its place")
    endif()
    check_relative_close("${input}" ${CMAKE_MATCH_1} ${reference} ${tolerance} 1)
endforeach()

# runs the sample with arguments; returns its exit code and its output
function(run_with_arguments exit_code output)
    execute_process(COMMAND "${EXAMPLE}" ${ARGN} RESULT_VARIABLE code
                    OUTPUT_VARIABLE out ERROR_VARIABLE errors)
    string(REPLACE "\r" "" out "${out}${errors}")
    set(${exit_code} ${code} PARENT_SCOPE)
    set(${output} "${out}" PARENT_SCOPE)
endfunction()

set(csv "${CMAKE_CURRENT_BINARY_DIR}/AdjointScaling-timings.csv")
file(REMOVE "${csv}")
if(DISABLE_AAD)
    run_with_arguments(code out --csv "${csv}")
    if(NOT code EQUAL 0 OR NOT EXISTS "${csv}")
        message(FATAL_ERROR "--csv exited with ${code} and wrote no file:\n${out}")
    endif()
    file(STRINGS "${csv}" lines)
    list(GET lines 0 header)
    if(NOT header STREQUAL "inputs,price_ms,bumping_ms")
        message(FATAL_ERROR "--csv wrote the header ${header}")
    endif()
    list(REMOVE_AT lines 0)
    set(written "")
    foreach(line IN LISTS lines)
        string(REGEX REPLACE ",.*" "" size "${line}")
        list(APPEND written ${size})
    endforeach()
    if(NOT written STREQUAL surface_sizes)
        message(FATAL_ERROR "--csv wrote rows for ${written}, expected ${surface_sizes}")
    endif()
    message("checked ${count} sensitivities and --csv")
else()
    set(timings "inputs,price_ms,bumping_ms\n")
    foreach(size IN LISTS surface_sizes)
        string(APPEND timings "${size},0.5,${size}\n")
    endforeach()
    file(WRITE "${csv}" "${timings}")
    run_with_arguments(code out --compare "${csv}")
    set(column " \\|[^|\n]+")
    set(row "\n +[0-9]+${column}${column}${column}${column}${column}${column}")
    string(REGEX MATCHALL "${row}" rows "${out}")
    list(LENGTH rows compared)
    list(LENGTH surface_sizes expected)
    if(NOT code EQUAL 0 OR NOT compared EQUAL expected)
        message(FATAL_ERROR "--compare exited with ${code} and printed ${compared} rows, "
                            "expected ${expected}:\n${out}")
    endif()
    file(WRITE "${csv}" "${timings}twenty-nine,0.5,12\n")
    run_with_arguments(code out --compare "${csv}")
    if(code EQUAL 0 OR NOT out MATCHES "cannot read \"twenty-nine,0.5,12\"")
        message(FATAL_ERROR "--compare accepted a line it can't read:\n${out}")
    endif()
    message("checked ${count} sensitivities, the central differences and --compare")
endif()
file(REMOVE "${csv}")

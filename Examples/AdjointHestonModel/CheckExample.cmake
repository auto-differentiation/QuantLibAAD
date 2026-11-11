# Runs the Heston example and checks its exit code, its price and its sensitivities.
#   cmake -DEXAMPLE=<executable> -DDISABLE_AAD=<ON|OFF> -P CheckExample.cmake
#
# The example prints its largest sensitivities: the XAD build 8, from the implicit
# function theorem, each next to bump & recalibrate; the doubles build 10, from
# bump & recalibrate. Both must print exactly the inputs listed below, and:
# - each implicit function theorem value matches the one listed within 0.1%; it is
#   exact up to the calibration tolerance, so platforms agree far more closely;
# - each bump & recalibrate value matches the implicit function theorem within 0.3%:
#   bumping is a one-sided difference, up to 0.09% off here, and recalibrating with
#   XAD or with doubles changes it by up to 0.03%.

set(price_low 480.15) # 480.63 within 0.1%; changing the data or the calibration moves it more
set(price_high 481.11)
set(implicit_function_theorem # from the XAD build, largest first
    "zero rate 524d=2887.9859" "zero rate 165d=-1040.2632" "zero rate 703d=495.4038"
    "zero rate 256d=-446.1408" "vol 4600 49W=401.1669" "vol 4600 37W=390.2413"
    "vol 4800 49W=388.5815" "vol 4500 49W=371.0322" "vol 4500 37W=367.6196"
    "vol 4800 37W=345.5897")
if(DISABLE_AAD)
    set(expected ${implicit_function_theorem})
else()
    list(SUBLIST implicit_function_theorem 0 8 expected)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../CheckSample.cmake)
run_example(output)

if(NOT output MATCHES "expiry in 18 months: ([0-9.]+)\n")
    message(FATAL_ERROR "no price in the output")
endif()
if(CMAKE_MATCH_1 LESS price_low OR CMAKE_MATCH_1 GREATER price_high)
    message(FATAL_ERROR "price ${CMAKE_MATCH_1} is outside [${price_low}, ${price_high}]")
endif()

set(number "-?[0-9]+\\.[0-9][0-9][0-9][0-9]")
string(REPLACE "\n" ";" lines "${output}")
set(printed "")
foreach(line IN LISTS lines)
    if(NOT line MATCHES "^ *([a-z][^|]*[^ |]) \\| +(${number})( \\| +(${number}))?$")
        continue()
    endif()
    set(input "${CMAKE_MATCH_1}")
    set(value "${CMAKE_MATCH_2}")
    set(bumped "${CMAKE_MATCH_4}")
    list(APPEND printed "${input}")

    set(stored "")
    foreach(entry IN LISTS expected)
        if(entry MATCHES "^${input}=(.+)$")
            set(stored "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    if(stored STREQUAL "")
        message(FATAL_ERROR "${input} is not among the expected largest sensitivities")
    endif()

    if(DISABLE_AAD)
        check_relative_close("${input}, bump & recalibrate" ${value} ${stored} 3e-3 0)
    else()
        check_relative_close("${input}, implicit function theorem" ${value} ${stored} 1e-3 0)
        if(bumped STREQUAL "")
            message(FATAL_ERROR "${input}: no bump & recalibrate value next to it")
        endif()
        check_relative_close("${input}, bump & recalibrate" "${bumped}" ${value} 3e-3 0)
    endif()
endforeach()

list(REMOVE_DUPLICATES printed)
list(LENGTH printed rows)
list(LENGTH expected expected_rows)
if(NOT rows EQUAL expected_rows)
    message(FATAL_ERROR "printed ${rows} different sensitivities, expected ${expected_rows}")
endif()
message("checked the price and ${rows} sensitivities")

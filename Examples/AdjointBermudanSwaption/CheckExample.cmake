# Runs the Bermudan swaption sample and checks its exit code, its price and, with
# XAD, its vegas.
#   cmake -DEXAMPLE=<executable> -DDISABLE_AAD=<ON|OFF> -P CheckExample.cmake
#
# The reference is bump & recalibrate in the doubles build, independent of XAD: the
# sample's pricing, with its data, engines and grids unchanged, but the Hull-White
# calibration converged to the least-squares optimum: LevenbergMarquardt(1e-8,
# 1e-15, 1e-15, true), i.e. with the cost function's central-difference Jacobian,
# and EndCriteria(2000, 200, 1e-16, 1e-16, 1e-16), restarted from its result until
# the parameters move by less than 1e-15 (relative). The price below is that
# pricing without bumps, at a = 0.046415256, sigma = 0.0058693096. The default
# Levenberg-Marquardt uses a forward-difference Jacobian and stops with the mean
# reversion 2e-5 (relative) away from the optimum. Each of the 5 calibrated vols
# (vega #4, #8, #12, #16, #20) was bumped on a grid of 81 steps of 5e-5 within
# +-2e-3 and recalibrated each time; the vega is the slope at 0 of a degree-5
# least-squares polynomial through those prices. A fit rather than a single central
# difference, as the price is not smooth in the vols at small steps: it jitters by
# about 1e-6 between neighbouring bumps and jumps by up to 1e-3 at a few of them
# (such points, more than 10 times the median residual off the fit, are dropped),
# while above 1e-3 the vegas change quickly with the step (vega #20 is 17.85 with
# a 4e-3 central difference). Over the windows
# +-1e-3 and +-2e-3 and degrees 3 to 7, the fitted vegas differ from the reference
# by at most 6e-4 (relative), and the implicit function theorem with XAD agrees
# with it within 3e-4. The other 20 vols are not calibrated to, so their vegas are
# exactly 0, as bumping them confirms.
#
# Each vega must match the reference within 2e-3 relative to it, over 3 times the
# reference's own spread; differentiating through the optimizer's iterations
# instead misses vegas #8, #12 and #20 by 0.7% to 1.7% even at a calibration
# tolerance of 1e-12, and by up to 7% at 1e-8. The price must match within 1e-6
# relative: stopping the calibration at 1e-8 moves it by 6e-8. Values printed with
# fewer digits than stored are compared to the reference rounded to them, and may
# differ by one unit of their last digit.

set(price 42.21532351)
set(reference # vega #0 to #24
    0 0 0 0 10.28901 0 0 0 4.71138 0 0 0 3.35763 0 0 0 7.33365 0 0 0 17.14283 0 0 0 0)

include(${CMAKE_CURRENT_LIST_DIR}/../CheckSample.cmake)

# the reference rounded, or padded with zeros, to the decimals of a printed number,
# so that check_relative_close can compare them; shifts by whole digits only, as
# cmake has no floating point
function(to_printed_decimals stored printed result)
    foreach(number IN ITEMS stored printed)
        set(d 0)
        if(${number} MATCHES "\\.([0-9]+)$")
            string(LENGTH "${CMAKE_MATCH_1}" d)
        endif()
        set(${number}_decimals ${d})
    endforeach()
    to_units(${stored} r)
    if(stored_decimals GREATER printed_decimals)
        math(EXPR shift "${stored_decimals} - ${printed_decimals}")
        string(REPEAT 0 ${shift} zeros)
        math(EXPR half "1${zeros} / 2")
        if(r LESS 0)
            math(EXPR r "(${r} - ${half}) / 1${zeros}")
        else()
            math(EXPR r "(${r} + ${half}) / 1${zeros}")
        endif()
    elseif(printed_decimals GREATER stored_decimals)
        math(EXPR shift "${printed_decimals} - ${stored_decimals}")
        string(REPEAT 0 ${shift} zeros)
        math(EXPR r "${r} * 1${zeros}")
    endif()
    set(sign "")
    if(r LESS 0)
        set(sign "-")
    endif()
    absolute(${r} digits)
    if(printed_decimals GREATER 0)
        math(EXPR width "${printed_decimals} + 1")
        string(LENGTH "${digits}" length)
        if(length LESS width)
            math(EXPR missing "${width} - ${length}")
            string(REPEAT 0 ${missing} zeros)
            set(digits "${zeros}${digits}")
        endif()
        string(LENGTH "${digits}" length)
        math(EXPR integer "${length} - ${printed_decimals}")
        string(SUBSTRING "${digits}" 0 ${integer} whole)
        string(SUBSTRING "${digits}" ${integer} -1 fraction)
        set(digits "${whole}.${fraction}")
    endif()
    set(${result} "${sign}${digits}" PARENT_SCOPE)
endfunction()

# fails unless a printed number is within tolerance (like 2e-3) of the reference,
# relative to it, after rounding the reference to the printed decimals, or within
# one unit of the printed last digit where that allows more
function(check_printed label printed stored tolerance)
    to_printed_decimals(${stored} ${printed} rounded)
    check_relative_close("${label}" ${printed} ${rounded} ${tolerance} 1)
endfunction()

run_example(output)
set(output "\n${output}")

if(NOT output MATCHES "\nPrice = (-?[0-9,]+(\\.[0-9]+)?)")
    message(FATAL_ERROR "no price in the output")
endif()
check_printed("Price" ${CMAKE_MATCH_1} ${price} 1e-6)

if(DISABLE_AAD)
    message("checked the price")
    return()
endif()

string(REGEX MATCHALL "\nVega #[0-9]+ = -?[0-9,]+(\\.[0-9]+)?" printed "${output}")
list(LENGTH printed count)
list(LENGTH reference expected)
if(NOT count EQUAL expected)
    message(FATAL_ERROR "printed ${count} vegas, expected ${expected}")
endif()

set(index 0)
foreach(line IN LISTS printed)
    list(GET reference ${index} stored)
    string(REGEX MATCH "Vega #([0-9]+) = (.+)$" line "${line}")
    if(NOT CMAKE_MATCH_1 EQUAL index)
        message(FATAL_ERROR "printed vega #${CMAKE_MATCH_1} where #${index} was expected")
    endif()
    check_printed("Vega #${index}" ${CMAKE_MATCH_2} ${stored} 2e-3)
    math(EXPR index "${index} + 1")
endforeach()
message("checked the price and ${count} vegas")

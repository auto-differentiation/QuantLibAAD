# Helpers for the scripts that run a sample as a test: cmake -DEXAMPLE=<executable> -P <script>

# runs the sample, fails unless it exits with 0, and returns its output
function(run_example output)
    execute_process(COMMAND "${EXAMPLE}" RESULT_VARIABLE exit_code
                    OUTPUT_VARIABLE out ERROR_VARIABLE errors)
    message("${out}${errors}")
    if(NOT exit_code EQUAL 0)
        message(FATAL_ERROR "the example exited with ${exit_code}")
    endif()
    string(REPLACE "\r" "" out "${out}")
    set(${output} "${out}" PARENT_SCOPE)
endfunction()

# a fixed-point number as an integer in units of its last digit, as cmake has no
# floating point; thousands separators are dropped
function(to_units number result)
    string(REGEX REPLACE "[.,]" "" digits "${number}")
    string(REGEX REPLACE "^(-?)0+([0-9])" "\\1\\2" digits "${digits}")
    set(${result} ${digits} PARENT_SCOPE)
endfunction()

# |value| for an integer
function(absolute number result)
    if(number LESS 0)
        math(EXPR number "0 - ${number}")
    endif()
    set(${result} ${number} PARENT_SCOPE)
endfunction()

# fails unless two fixed-point numbers with the same decimals differ by at most
# allowed, in units of their last digit; label says what is checked and the tolerance
function(check_close label value reference allowed)
    to_units(${value} v)
    to_units(${reference} r)
    math(EXPR difference "${v} - ${r}")
    absolute(${difference} difference)
    if(difference GREATER allowed)
        message(FATAL_ERROR "${label}: ${value} differs from ${reference}")
    endif()
endfunction()

# fails unless value is within tolerance (like 5e-4) of reference, relative to the
# reference, or within floor units of the last digit where that allows more
function(check_relative_close label value reference tolerance floor)
    if(NOT tolerance MATCHES "^([0-9]+)e-([0-9]+)$")
        message(FATAL_ERROR "${label}: tolerance ${tolerance} is not like 5e-4")
    endif()
    set(mantissa ${CMAKE_MATCH_1})
    string(REPEAT 0 ${CMAKE_MATCH_2} zeros)
    to_units(${reference} r)
    absolute(${r} r)
    math(EXPR allowed "${r} * ${mantissa} / 1${zeros}")
    if(allowed LESS floor)
        set(allowed ${floor})
    endif()
    check_close("${label} (within ${tolerance})" ${value} ${reference} ${allowed})
endfunction()

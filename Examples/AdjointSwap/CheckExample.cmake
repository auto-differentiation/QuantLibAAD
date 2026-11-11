# Runs the swap sample and checks its exit code and its sensitivities.
#   cmake -DEXAMPLE=<executable> -DDISABLE_AAD=<ON|OFF> -P CheckExample.cmake
#
# Each sensitivity must match the XAD value below, relative to that value: within
# 1e-7 with XAD (rounding and solver tolerances), and within 5e-4 with doubles:
# bumping is a forward difference with a 1e-5 step, up to 1.4e-4 off here. Small
# values, such as those printed as 0.00, may also differ by 0.01, the rounding of
# the last digit.

set(reference # from the XAD build: deposits, FRAs, swaps
    -58221.49 -19407.26 0.00 -0.00 -0.00 -7802.11 2171756.27 8501.32 -0.00 0.00
    -0.00 -24428.33 -707133.23 -617.31 -0.00
    -233519.68 -1246901.61 -1642273.07 -2270914.29 -1697350.11 -2861112.10 -3666586.33
    -4862678.40 -4945499.10 -6680860.84 -2536659.75 -2780965.08 -3697166.99 -5154225.12
    -6253664.09 -12270084.81 -4691675.15 -10814446.54 -11124272.57 -4214894.25
    -6323147.75 -5622294.18 -10905122.94 -10999543.47 -16567826.59 -4068073.30
    -9013964.74 -11412169.23 -6171305.59 -3470525.59 -8892064.62 -10166643.45
    -2905534.40 -7344024.30 -5815124.14 -1371850.24 -4611005.69 -12520918.49
    -8701914.35 -10304303.19)
if(DISABLE_AAD)
    set(tolerance 5e-4)
else()
    set(tolerance 1e-7)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../CheckSample.cmake)
run_example(output)

# the locale may add thousands separators
string(REGEX MATCHALL "dv/d[A-Za-z]+\\[[0-9]+\\] = -?[0-9,]+\\.[0-9][0-9]" printed "${output}")
list(LENGTH printed count)
list(LENGTH reference expected)
if(NOT count EQUAL expected)
    message(FATAL_ERROR "printed ${count} sensitivities, expected ${expected}")
endif()

set(index 0)
foreach(line IN LISTS printed)
    list(GET reference ${index} stored)
    math(EXPR index "${index} + 1")
    string(REGEX MATCH "^([^ ]+) = (.+)$" line "${line}")
    check_relative_close(${CMAKE_MATCH_1} ${CMAKE_MATCH_2} ${stored} ${tolerance} 1)
endforeach()
message("checked ${count} sensitivities")

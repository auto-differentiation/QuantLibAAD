# runs a sample as a test, checked by the CheckExample.cmake next to its source;
# in the C locale on POSIX (on Windows the checks also accept comma grouping),
# as they read the numbers it prints, and stopped after 10 minutes, several
# times the slowest sample's run time
function(add_sample_test name)
    add_test(NAME ${name}
             COMMAND ${CMAKE_COMMAND} -DEXAMPLE=$<TARGET_FILE:${name}>
                     -DDISABLE_AAD=${QLAAD_DISABLE_AAD}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/CheckExample.cmake)
    set_tests_properties(${name} PROPERTIES ENVIRONMENT LC_ALL=C TIMEOUT 600)
endfunction()

add_subdirectory(AdjointAmericanEquityOption)
add_subdirectory(AdjointBermudanSwaption)
add_subdirectory(AdjointCDS)
add_subdirectory(AdjointEuropeanEquityOption)
add_subdirectory(AdjointHestonModel)
add_subdirectory(AdjointMulticurveBootstrapping)
add_subdirectory(AdjointReplication)
add_subdirectory(AdjointScaling)
add_subdirectory(AdjointSwap)



# Checks which of XAD's recommended client-side definitions QLAAD_XAD_RECOMMENDED adds per compiler.
# Script mode only, no build dir or compiler needed:
#
#   cmake [-DMODULE=<path to QLAADXadRecommended.cmake>] -P tools/check-xad-recommended.cmake
#
# XAD_OUTLINE_RECORDING goes to every compiler; XAD_USE_STRONG_INLINE to all but MSVC's cl.exe, where
# it makes QuantLib's AD compile 2.4x slower (clang-cl, ID Clang with the MSVC frontend, keeps it).

if(NOT MODULE)
    set(MODULE "${CMAKE_CURRENT_LIST_DIR}/../cmake/QLAADXadRecommended.cmake")
endif()
if(NOT EXISTS "${MODULE}")
    message(FATAL_ERROR "module not found: ${MODULE}")
endif()
include("${MODULE}")
if(NOT COMMAND qlaad_xad_recommended_definitions)
    message(FATAL_ERROR "${MODULE} does not define qlaad_xad_recommended_definitions()")
endif()

# each case in its own function scope, so the compiler variables don't leak between cases
function(check_case name id frontend)
    set(CMAKE_CXX_COMPILER_ID "${id}")
    set(CMAKE_CXX_COMPILER_FRONTEND_VARIANT "${frontend}")
    set(expected ${ARGN})
    qlaad_xad_recommended_definitions(defs)
    list(SORT defs)
    list(SORT expected)
    if(NOT "${defs}" STREQUAL "${expected}")
        message(FATAL_ERROR "${name} (ID ${id}, frontend ${frontend}): expected '${expected}', got '${defs}'")
    endif()
endfunction()

set(both XAD_USE_STRONG_INLINE XAD_OUTLINE_RECORDING)
check_case("MSVC cl.exe" MSVC MSVC XAD_OUTLINE_RECORDING)
check_case(clang-cl Clang MSVC ${both})
check_case(GCC GNU GNU ${both})
check_case(Clang Clang GNU ${both})
check_case(AppleClang AppleClang GNU ${both})
message(STATUS "XAD recommended definitions: cl.exe without XAD_USE_STRONG_INLINE, others with both")

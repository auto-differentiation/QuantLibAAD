##############################################################################
#
#
#  This file is part of QuantLibAAD, an adaptor module to enable using XAD with
#  QuantLib. XAD is a fast and comprehensive C++ library for
#  automatic differentiation.
#
#  Copyright (C) 2010-2026 Xcelerit Computing Ltd.
#
#  This program is free software: you can redistribute it and/or modify
#  it under the terms of the GNU Affero General Public License as published
#  by the Free Software Foundation, either version 3 of the License, or
#  (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU Affero General Public License for more details.
#
#  You should have received a copy of the GNU Affero General Public License
#  along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
##############################################################################

option(QLAAD_DISABLE_AAD "Disable using XAD for QuantLib's Real, allowing to run samples with double" OFF)
option(QLAAD_ENABLE_CODEGEN "Enable Codegen JIT backend (requires XAD built with XAD_ENABLE_CODEGEN)" OFF)

# Backward compatibility for old QLRISKS_* options
foreach(_opt IN ITEMS
    DISABLE_AAD
)
  if(DEFINED CACHE{QLRISKS_${_opt}})
    set(QLAAD_${_opt} ${QLRISKS_${_opt}} CACHE BOOL "" FORCE)
    message(WARNING
      "CMake option QLRISKS_${_opt} is deprecated. "
      "Please use QLAAD_${_opt} instead."
    )
  endif()
endforeach()

add_subdirectory(ql)
if(MSVC)
	set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
add_subdirectory(Examples)

##############################################################################
# QLAAD-Codegen integration (uses XAD's built-in codegen module)
# NOTE: This must be defined BEFORE test-suite so tests can link to it
##############################################################################

if(QLAAD_ENABLE_CODEGEN)
    message(STATUS "QLAAD-Codegen: Looking for XAD codegen support...")

    # Codegen is now built into XAD when XAD_ENABLE_CODEGEN=ON
    if(TARGET xad_codegen)
        message(STATUS "QLAAD-Codegen: Found xad_codegen target")

        # Create qlaad-codegen as an INTERFACE library
        add_library(qlaad-codegen INTERFACE)
        add_library(QLAAD::codegen ALIAS qlaad-codegen)

        # xad already links xad_codegen, so just link xad
        if(TARGET xad)
            target_link_libraries(qlaad-codegen INTERFACE xad)
        elseif(TARGET XAD::xad)
            target_link_libraries(qlaad-codegen INTERFACE XAD::xad)
        endif()

        target_compile_definitions(qlaad-codegen INTERFACE QLAAD_HAS_CODEGEN=1)

        message(STATUS "QLAAD-Codegen: Configured successfully")
    elseif(XAD_ENABLE_CODEGEN)
        message(STATUS "QLAAD-Codegen: XAD_ENABLE_CODEGEN is ON but xad_codegen target not found")
    else()
        message(STATUS "QLAAD-Codegen: XAD was not built with XAD_ENABLE_CODEGEN=ON")
        message(STATUS "  Codegen JIT backend requires a commercial license.")
        message(STATUS "  See https://www.xcelerit.com/xad-enterprise-support")
    endif()
endif()

##############################################################################
# Test suite (after QLAAD::codegen is defined)
##############################################################################

# Add test-suite if:
# 1. AAD is enabled (for XAD-based tests and benchmarks), OR
# 2. FD benchmark is requested (works with plain double)
if(NOT QLAAD_DISABLE_AAD OR QLAAD_BUILD_BENCHMARK_FD)
    add_subdirectory(test-suite)
endif()

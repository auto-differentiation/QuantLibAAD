##############################################################################
#
#
#  This file is part of QuantLib-Risks, an adaptor module to enable using XAD with
#  QuantLib. XAD is a fast and comprehensive C++ library for
#  automatic differentiation.
#
#  Copyright (C) 2010-2024 Xcelerit Computing Ltd.
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

option(QLRISKS_DISABLE_AAD "Disable using XAD for QuantLib's Real, allowing to run samples with double" OFF)
option(QLRISKS_ENABLE_FORGE "Enable Forge JIT backend via xad-forge" OFF)
option(QLRISKS_USE_FORGE_CAPI "Use Forge C API instead of C++ API for binary compatibility" OFF)

add_subdirectory(ql)
if(MSVC)
	set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
add_subdirectory(Examples)

##############################################################################
# QLRisks-Forge integration via xad-forge
# NOTE: This must be defined BEFORE test-suite so tests can link to it
##############################################################################

if(QLRISKS_ENABLE_FORGE)
    message(STATUS "QLRisks-Forge: Looking for xad-forge...")

    # Pass through the C API option to xad-forge
    set(XAD_FORGE_USE_CAPI ${QLRISKS_USE_FORGE_CAPI} CACHE BOOL "" FORCE)

    # Option 1: Check if xad-forge was added as subdirectory
    if(TARGET xad-forge)
        message(STATUS "QLRisks-Forge: Found xad-forge target (subdirectory mode)")
        set(XAD_FORGE_FOUND TRUE)
    endif()

    # Option 2: Try find_package for pre-built xad-forge
    if(NOT XAD_FORGE_FOUND)
        find_package(xad-forge CONFIG QUIET)
        if(xad-forge_FOUND)
            message(STATUS "QLRisks-Forge: Found xad-forge package (pre-built mode)")
            set(XAD_FORGE_FOUND TRUE)
        endif()
    endif()

    if(XAD_FORGE_FOUND)
        # Create qlrisks-forge as an INTERFACE library wrapping xad-forge
        add_library(qlrisks-forge INTERFACE)
        add_library(QLRisks::forge ALIAS qlrisks-forge)

        target_link_libraries(qlrisks-forge INTERFACE
            XADForge::xad-forge
        )

        target_compile_definitions(qlrisks-forge INTERFACE QLRISKS_HAS_FORGE=1)

        message(STATUS "QLRisks-Forge: Configured with xad-forge")
    else()
        message(STATUS "QLRisks-Forge: xad-forge not found - ForgeBackend will not be available")
        message(STATUS "QLRisks-Forge: To enable, either:")
        message(STATUS "  1. Add xad-forge as subdirectory")
        message(STATUS "  2. Set CMAKE_PREFIX_PATH to xad-forge installation")
    endif()
endif()

##############################################################################
# Test suite (after QLRisks::forge is defined)
##############################################################################

# Add test-suite if:
# 1. AAD is enabled (for XAD-based tests and benchmarks), OR
# 2. FD benchmark is requested (works with plain double)
if(NOT QLRISKS_DISABLE_AAD OR QLRISKS_BUILD_BENCHMARK_FD)
    add_subdirectory(test-suite)
endif()
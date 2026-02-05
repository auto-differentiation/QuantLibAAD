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

set(QLAAD_HEADERS
    qlaad.hpp
)
add_library(QuantLibAAD INTERFACE)
add_library(QuantLib-Risks ALIAS QuantLibAAD)  # for compatibility
target_include_directories(QuantLibAAD INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<INSTALL_INTERFACE:${QL_INSTALL_INCLUDEDIR}>
)
target_compile_features(QuantLibAAD INTERFACE cxx_std_14)
if(NOT QLAAD_DISABLE_AAD)
    target_compile_definitions(QuantLibAAD INTERFACE QL_INCLUDE_FIRST=ql/qlaad.hpp)
else()
    target_compile_definitions(QuantLibAAD INTERFACE QLAAD_DISABLE_AAD=1)
endif()
target_link_libraries(QuantLibAAD INTERFACE XAD::xad)
if(MSVC)
    target_compile_options(QuantLibAAD INTERFACE /bigobj)
endif()
set_target_properties(QuantLibAAD PROPERTIES
    EXPORT_NAME QuantLibAAD
)
install(TARGETS QuantLibAAD EXPORT QuantLibTargets)
foreach(file ${QLAAD_HEADERS})
    install(FILES ${file} DESTINATION "${QL_INSTALL_INCLUDEDIR}/ql")
endforeach()


# Install a convenience config script for QuantLibAAD, which allows users to
# 
# find_package(QuantLibAAD REQUIRED)
# target_link_libraries(myapp PRIVATE QuantLib::QuantLib)
#
# That is, it looks for the XAD dependency before it includes the QuantLibConfig file
#
# Without that, users have to first find_package(XAD) before find_package(QuantLib)
# to achieve the same.
#

set(QLAAD_INSTALL_CMAKEDIR "lib/cmake/QuantLibAAD" CACHE STRING
    "Installation directory for CMake scripts for QuantLib AAD")

include(CMakePackageConfigHelpers)
configure_file("../cmake/QuantLibAADConfig.cmake.in"
    "${PROJECT_BINARY_DIR}/cmake/QuantLibAADConfig.cmake"
    COPYONLY
)
configure_package_config_file("../cmake/QuantLibAADConfig.cmake.in"
    "${PROJECT_BINARY_DIR}/cmake/QuantLibAADConfig.cmake"
    INSTALL_DESTINATION "${QLAAD_INSTALL_CMAKEDIR}"
)
install(FILES "${PROJECT_BINARY_DIR}/cmake/QuantLibAADConfig.cmake"
    DESTINATION "${QLAAD_INSTALL_CMAKEDIR}"
)

# the following is purely for compatibility - it will be removed in the near future
configure_file("../cmake/QuantLibAADConfig.cmake.in"
    "${PROJECT_BINARY_DIR}/cmake/QuantLib-RisksConfig.cmake"
    COPYONLY
)
configure_package_config_file("../cmake/QuantLibAADConfig.cmake.in"
    "${PROJECT_BINARY_DIR}/cmake/QuantLib-RisksConfig.cmake"
    INSTALL_DESTINATION "${QLAAD_INSTALL_CMAKEDIR}"
)
install(FILES "${PROJECT_BINARY_DIR}/cmake/QuantLib-RisksConfig.cmake"
    DESTINATION "${QLAAD_INSTALL_CMAKEDIR}"
)

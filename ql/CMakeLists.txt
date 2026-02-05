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

set(QLRISKS_HEADERS
    qlrisks.hpp
)
add_library(QuantLib-Risks INTERFACE)
target_include_directories(QuantLib-Risks INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<INSTALL_INTERFACE:${QL_INSTALL_INCLUDEDIR}>
)
target_compile_features(QuantLib-Risks INTERFACE cxx_std_14)
if(NOT QLRISKS_DISABLE_AAD)
    target_compile_definitions(QuantLib-Risks INTERFACE QL_INCLUDE_FIRST=ql/qlrisks.hpp)
else()
    target_compile_definitions(QuantLib-Risks INTERFACE QLRISKS_DISABLE_AAD=1)
endif()
target_link_libraries(QuantLib-Risks INTERFACE XAD::xad)
if(MSVC)
    target_compile_options(QuantLib-Risks INTERFACE /bigobj)
endif()
set_target_properties(QuantLib-Risks PROPERTIES
    EXPORT_NAME QuantLib-Risks
)
install(TARGETS QuantLib-Risks EXPORT QuantLibTargets)
foreach(file ${QLRISKS_HEADERS})
    install(FILES ${file} DESTINATION "${QL_INSTALL_INCLUDEDIR}/ql")
endforeach()


# Install a convenience config script for QuantLib-Risks, which allows users to
# 
# find_package(QuantLib-Risks REQUIRED)
# target_link_libraries(myapp PRIVATE QuantLib::QuantLib)
#
# That is, it looks for the XAD dependency before it includes the QuantLibConfig file
#
# Without that, users have to first find_package(XAD) before find_package(QuantLib)
# to achieve the same.
#

set(QLRISKS_INSTALL_CMAKEDIR "lib/cmake/QuantLib-Risks" CACHE STRING
    "Installation directory for CMake scripts for QuantLib XAD")

include(CMakePackageConfigHelpers)
configure_file("../cmake/QuantLib-RisksConfig.cmake.in"
    "${PROJECT_BINARY_DIR}/cmake/QuantLib-RisksConfig.cmake"
    COPYONLY
)
configure_package_config_file("../cmake/QuantLib-RisksConfig.cmake.in"
    "${PROJECT_BINARY_DIR}/cmake/QuantLib-RisksConfig.cmake"
    INSTALL_DESTINATION "${QLRISKS_INSTALL_CMAKEDIR}"
)
install(FILES "${PROJECT_BINARY_DIR}/cmake/QuantLib-RisksConfig.cmake"
    DESTINATION "${QLRISKS_INSTALL_CMAKEDIR}"
)

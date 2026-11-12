cmake_minimum_required(VERSION 3.19)  # policies for cmake -P (IN_LIST, string(JSON))

# Checks that tools/pch-select.py refuses a build with unity sources (it needs one with
# QLAAD_UNITY_BATCH_SIZE=0 and QLAAD_PRECOMPILE_HEADERS=OFF), with a message and no traceback.
#
#   cmake -DBUILD_DIR=<QuantLib build dir with unity sources> -DQL_DIR=<QuantLib sources> -P tools/check-pch-select.cmake

if(NOT BUILD_DIR OR NOT QL_DIR)
    message(FATAL_ERROR "usage: cmake -DBUILD_DIR=<QuantLib build dir> -DQL_DIR=<QuantLib sources> -P ${CMAKE_CURRENT_LIST_FILE}")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(out "${BUILD_DIR}/pch-select-check.cmake")
file(REMOVE ${out})
execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/pch-select.py ${BUILD_DIR} ${QL_DIR} -o ${out}
                RESULT_VARIABLE rc OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(rc EQUAL 0 OR EXISTS ${out})
    message(FATAL_ERROR "pch-select.py accepted a unity build (exit ${rc}):\n${stderr}")
endif()
if(stderr MATCHES "Traceback" OR NOT stderr MATCHES "QLAAD_UNITY_BATCH_SIZE=0 and QLAAD_PRECOMPILE_HEADERS=OFF")
    message(FATAL_ERROR "pch-select.py: expected a message naming both options, got:\n${stderr}")
endif()
message(STATUS "pch-select.py refuses a unity build")

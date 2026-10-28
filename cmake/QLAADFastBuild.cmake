# Faster QuantLib builds: precompiled headers and a unity build (batches of 8) for QuantLib's
# library and test-suite targets, both on by default and switched on from here without touching
# QuantLib. The PCH holds only headers each file includes anyway (QLAADFastBuildPCH.cmake), so it
# adds no declarations; a unity build does merge files (QLAAD_UNITY_BATCH_SIZE=0 turns it off).
# QuantLib adds QL_EXTERNAL_SUBDIRECTORIES before ql/ and test-suite/, so the settings are
# applied by a call deferred to the end of the top-level directory, when the targets exist.

option(QLAAD_FAST_BUILD "Precompiled headers and unity build for QuantLib's library and tests" ON)
option(QLAAD_CTEST_PER_SUITE "Register each QuantLib test suite as its own ctest test" ON)
set(QLAAD_UNITY_BATCH_SIZE 8 CACHE STRING "Unity batch size for QuantLib's targets (0: no unity build)")
# PCH is tested with GCC and Clang; on Windows (MSVC, untested, and sccache does not cache its PCH) it is opt-in.
if(WIN32)
    set(qlaad_pch_default OFF)
else()
    set(qlaad_pch_default ON)
endif()
option(QLAAD_PRECOMPILE_HEADERS "Precompiled headers for QuantLib's targets" ${qlaad_pch_default})

# ccache as compiler launcher when installed and no launcher is set (here and in QuantLib's scope).
option(QLAAD_USE_CCACHE "Use ccache, if installed, when no compiler launcher is set" ON)
if(QLAAD_USE_CCACHE AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
    find_program(QLAAD_CCACHE_PROGRAM ccache)
    if(QLAAD_CCACHE_PROGRAM)
        set(CMAKE_CXX_COMPILER_LAUNCHER ${QLAAD_CCACHE_PROGRAM})
        set(CMAKE_CXX_COMPILER_LAUNCHER ${QLAAD_CCACHE_PROGRAM} PARENT_SCOPE)
    endif()
endif()

# Without these, ccache never hits on a compile that uses a PCH (ccache >= 4.8 takes them on its command line).
if(QLAAD_FAST_BUILD AND QLAAD_PRECOMPILE_HEADERS AND CMAKE_CXX_COMPILER_LAUNCHER)
    list(GET CMAKE_CXX_COMPILER_LAUNCHER 0 qlaad_launcher)
    if(qlaad_launcher MATCHES "(^|[/\\])ccache(\\.exe)?$")
        execute_process(COMMAND ${qlaad_launcher} --version OUTPUT_VARIABLE qlaad_ccache_version ERROR_QUIET)
        if(qlaad_ccache_version MATCHES "ccache version ([0-9.]+)" AND CMAKE_MATCH_1 VERSION_GREATER_EQUAL 4.8)
            set(CMAKE_CXX_COMPILER_LAUNCHER ${CMAKE_CXX_COMPILER_LAUNCHER} sloppiness=pch_defines,time_macros
                PARENT_SCOPE)
        endif()
    endif()
endif()

function(qlaad_fast_build_apply)
    include(${QLAAD_FAST_BUILD_DIR}/QLAADFastBuildPCH.cmake)
    foreach(i RANGE 9)
        if(NOT DEFINED qlaad_pch_${i})
            continue()
        endif()
        list(POP_FRONT qlaad_pch_${i} target)
        if(NOT TARGET ${target})
            continue()
        endif()
        if(QLAAD_UNITY_BATCH_SIZE GREATER 0)
            set_target_properties(${target} PROPERTIES
                UNITY_BUILD ON
                UNITY_BUILD_BATCH_SIZE ${QLAAD_UNITY_BATCH_SIZE})
        endif()
        if(QLAAD_PRECOMPILE_HEADERS)
            target_precompile_headers(${target} PRIVATE ${qlaad_pch_${i}})
            if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
                # no header timestamps in the PCH, so that ccache can cache it
                target_compile_options(${target} PRIVATE "SHELL:-Xclang -fno-pch-timestamp")
            endif()
            foreach(f IN LISTS qlaad_pch_${i}_skip)
                if(EXISTS ${CMAKE_SOURCE_DIR}/${f})
                    set_source_files_properties(${CMAKE_SOURCE_DIR}/${f} TARGET_DIRECTORY ${target}
                                                PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
                endif()
            endforeach()
        endif()
    endforeach()
    message(STATUS "QLAAD_FAST_BUILD: unity batch ${QLAAD_UNITY_BATCH_SIZE}, PCH ${QLAAD_PRECOMPILE_HEADERS}")
endfunction()

if(QLAAD_FAST_BUILD)
    if(CMAKE_VERSION VERSION_LESS 3.19)
        message(STATUS "QLAAD_FAST_BUILD needs CMake >= 3.19, ignored")
    else()
        set(QLAAD_FAST_BUILD_DIR ${CMAKE_CURRENT_LIST_DIR} CACHE INTERNAL "")
        cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL qlaad_fast_build_apply)
    endif()
endif()

# One ctest test per QuantLibTests/<Suite> Boost.Test suite, so that ctest -j runs them in
# parallel; the suites are found in the test-suite sources at configure time.
function(qlaad_ctest_per_suite_apply)
    if(NOT TARGET ql_test_suite OR NOT TARGET ql_test)
        return()
    endif()
    get_target_property(dir ql_test SOURCE_DIR)
    get_target_property(sources ql_test SOURCES)
    get_target_property(workdir ql_test_suite BINARY_DIR)
    set(suites)
    foreach(f IN LISTS sources)
        if(NOT f MATCHES "\\.cpp$")
            continue()
        endif()
        if(NOT IS_ABSOLUTE ${f})
            set(f ${dir}/${f})
        endif()
        file(READ ${f} text)
        string(REGEX MATCHALL "BOOST_(AUTO_TEST_SUITE_END|AUTO_TEST_SUITE|FIXTURE_TEST_SUITE)[ \t]*\\([ \t]*[A-Za-z0-9_]*"
               tokens "${text}")
        set(stack)
        foreach(t IN LISTS tokens)
            if(t MATCHES "_END")
                list(POP_BACK stack)
            else()
                string(REGEX REPLACE ".*\\([ \t]*" "" name "${t}")
                if(stack STREQUAL "QuantLibTests")
                    list(APPEND suites ${name})
                endif()
                list(APPEND stack ${name})
            endif()
        endforeach()
    endforeach()
    list(REMOVE_DUPLICATES suites)
    foreach(s IN LISTS suites)
        add_test(NAME QuantLibTests/${s}
                 COMMAND $<TARGET_FILE:ql_test_suite> --run_test=QuantLibTests/${s} --log_level=message
                 WORKING_DIRECTORY ${workdir})
    endforeach()
    set_property(TEST quantlib_test_suite DIRECTORY ${dir} PROPERTY DISABLED TRUE)
    list(LENGTH suites n)
    message(STATUS "QLAAD_CTEST_PER_SUITE: ${n} QuantLibTests/<Suite> ctest tests")
endfunction()

if(QLAAD_CTEST_PER_SUITE)
    if(CMAKE_VERSION VERSION_LESS 3.28)
        message(STATUS "QLAAD_CTEST_PER_SUITE needs CMake >= 3.28, ignored")
    else()
        cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL qlaad_ctest_per_suite_apply)
    endif()
endif()

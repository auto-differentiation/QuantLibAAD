option(QLAAD_BUILD_TEST_SUITE "Build the QuantLibAAD test suite" OFF)
option(QLAAD_ENABLE_FORGE_TESTS "Enable Forge JIT tests (requires Forge)" OFF)

set(QLAAD_TEST_SOURCES
    americanoption_xad.cpp
    barrieroption_xad.cpp
    batesmodel_xad.cpp
    bermudanswaption_xad.cpp
    bonds_xad.cpp
    creditdefaultswap_xad.cpp
    europeanoption_xad.cpp
    forwardrateagreement_xad.cpp
    hestonmodel_xad.cpp
    swap_xad.cpp

    utilities_xad.cpp
    quantlibtestsuite_xad.cpp
)

# Forge JIT tests - require Forge's native code backend
# Tests QuantLib-specific JIT functionality (swaption pricing pipeline)
set(QLAAD_FORGE_TESTS_ENABLED FALSE)
if(QLAAD_ENABLE_FORGE_TESTS AND TARGET QLAAD::forge)
    message(STATUS "QLAAD test-suite: Adding Forge JIT tests (native code backend)")
    list(APPEND QLAAD_TEST_SOURCES swaption_jit_pipeline_xad.cpp)
    set(QLAAD_FORGE_TESTS_ENABLED TRUE)
elseif(QLAAD_ENABLE_FORGE_TESTS)
    message(WARNING "QLAAD test-suite: QLAAD_ENABLE_FORGE_TESTS=ON but QLAAD::forge not available")
endif()

set(QLAAD_TEST_HEADERS utilities_xad.hpp)

if(QL_BUILD_TEST_SUITE OR QLAAD_BUILD_TEST_SUITE)
    add_executable(QuantLibAAD_test_suite ${QLAAD_TEST_SOURCES} ${QLAAD_TEST_HEADERS})
    set_target_properties(QuantLibAAD_test_suite PROPERTIES OUTPUT_NAME "quantlib-aad-test-suite")
    if (NOT Boost_USE_STATIC_LIBS)
        target_compile_definitions(QuantLibAAD_test_suite PRIVATE BOOST_ALL_DYN_LINK)
    endif()
    target_link_libraries(QuantLibAAD_test_suite PRIVATE
        ql_library
        ${QL_THREAD_LIBRARIES})

    # ONLY link to Forge if Forge tests are actually enabled
    # This is important because linking Forge brings in AVX2-compiled code
    # which can cause ODR violations if not properly isolated
    if(QLAAD_FORGE_TESTS_ENABLED)
        message(STATUS "QLAAD test-suite: Linking QLAAD::forge (Forge tests enabled)")
        target_link_libraries(QuantLibAAD_test_suite PRIVATE QLAAD::forge)
        target_compile_definitions(QuantLibAAD_test_suite PRIVATE QLAAD_HAS_FORGE=1)
    else()
        message(STATUS "QLAAD test-suite: NOT linking Forge (Forge tests disabled)")
    endif()

    if (QL_INSTALL_TEST_SUITE)
        install(TARGETS QuantLibAAD_test_suite RUNTIME DESTINATION ${QL_INSTALL_BINDIR})
    endif()
    add_test(NAME QuantLibAAD_test_suite COMMAND QuantLibAAD_test_suite --log_level=message)
endif()

# =============================================================================
# Benchmark - Split FD/AAD executables for fair comparison
# =============================================================================

# FD benchmark - Finite Differences using plain double QuantLib
# This is built WITHOUT XAD to ensure fair FD vs AAD comparison
option(QLAAD_BUILD_BENCHMARK_FD "Build FD benchmark (plain double, no XAD)" OFF)
if(QLAAD_BUILD_BENCHMARK_FD)
    message(STATUS "QLAAD: Building FD benchmark (plain double)")
    add_executable(benchmark_fd
        benchmark_fd.cpp
        benchmark_common.hpp
        benchmark_pricing.hpp
        PlatformInfo.hpp
    )
    set_target_properties(benchmark_fd PROPERTIES
        OUTPUT_NAME "benchmark-fd")
    target_link_libraries(benchmark_fd PRIVATE
        ql_library
        ${QL_THREAD_LIBRARIES})

    if (QL_INSTALL_TEST_SUITE)
        install(TARGETS benchmark_fd RUNTIME DESTINATION ${QL_INSTALL_BINDIR})
    endif()
endif()

# AAD benchmark - XAD tape-based AAD and optionally Forge JIT
# Built with XAD-enabled QuantLib, optionally links Forge for JIT benchmarks
option(QLAAD_BUILD_BENCHMARK_AAD "Build AAD benchmark (XAD tape, optionally Forge JIT)" OFF)
if(QLAAD_BUILD_BENCHMARK_AAD)
    message(STATUS "QLAAD: Building AAD benchmark")
    add_executable(benchmark_aad
        benchmark_aad.cpp
        benchmark_common.hpp
        benchmark_pricing.hpp
        PlatformInfo.hpp
    )
    set_target_properties(benchmark_aad PROPERTIES
        OUTPUT_NAME "benchmark-aad")
    target_link_libraries(benchmark_aad PRIVATE
        ql_library
        ${QL_THREAD_LIBRARIES})

    # Link Forge if available for JIT benchmarks
    if(TARGET QLAAD::forge)
        message(STATUS "QLAAD AAD benchmark: Linking QLAAD::forge (JIT benchmarks enabled)")
        target_link_libraries(benchmark_aad PRIVATE QLAAD::forge)
        target_compile_definitions(benchmark_aad PRIVATE QLAAD_HAS_FORGE=1)
    else()
        message(STATUS "QLAAD AAD benchmark: Building without Forge (XAD tape only)")
    endif()

    if (QL_INSTALL_TEST_SUITE)
        install(TARGETS benchmark_aad RUNTIME DESTINATION ${QL_INSTALL_BINDIR})
    endif()
endif()
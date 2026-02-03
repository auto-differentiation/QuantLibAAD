option(QLRISKS_BUILD_TEST_SUITE "Build the QuantLib-Risks test suite" OFF)
option(QLRISKS_ENABLE_FORGE_TESTS "Enable Forge JIT tests (requires Forge)" OFF)

set(QLRISKS_TEST_SOURCES
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
set(QLRISKS_FORGE_TESTS_ENABLED FALSE)
if(QLRISKS_ENABLE_FORGE_TESTS AND TARGET QLRisks::forge)
    message(STATUS "QLRisks test-suite: Adding Forge JIT tests (native code backend)")
    list(APPEND QLRISKS_TEST_SOURCES swaption_jit_pipeline_xad.cpp)
    set(QLRISKS_FORGE_TESTS_ENABLED TRUE)
elseif(QLRISKS_ENABLE_FORGE_TESTS)
    message(WARNING "QLRisks test-suite: QLRISKS_ENABLE_FORGE_TESTS=ON but QLRisks::forge not available")
endif()

set(QLRISKS_TEST_HEADERS utilities_xad.hpp)

if(QL_BUILD_TEST_SUITE OR QLRISKS_BUILD_TEST_SUITE)
    add_executable(QuantLib-Risks_test_suite ${QLRISKS_TEST_SOURCES} ${QLRISKS_TEST_HEADERS})
    set_target_properties(QuantLib-Risks_test_suite PROPERTIES OUTPUT_NAME "quantlib-risks-test-suite")
    if (NOT Boost_USE_STATIC_LIBS)
        target_compile_definitions(QuantLib-Risks_test_suite PRIVATE BOOST_ALL_DYN_LINK)
    endif()
    target_link_libraries(QuantLib-Risks_test_suite PRIVATE
        ql_library
        ${QL_THREAD_LIBRARIES})

    # ONLY link to Forge if Forge tests are actually enabled
    # This is important because linking Forge brings in AVX2-compiled code
    # which can cause ODR violations if not properly isolated
    if(QLRISKS_FORGE_TESTS_ENABLED)
        message(STATUS "QLRisks test-suite: Linking QLRisks::forge (Forge tests enabled)")
        target_link_libraries(QuantLib-Risks_test_suite PRIVATE QLRisks::forge)
        target_compile_definitions(QuantLib-Risks_test_suite PRIVATE QLRISKS_HAS_FORGE=1)
    else()
        message(STATUS "QLRisks test-suite: NOT linking Forge (Forge tests disabled)")
    endif()

    if (QL_INSTALL_TEST_SUITE)
        install(TARGETS QuantLib-Risks_test_suite RUNTIME DESTINATION ${QL_INSTALL_BINDIR})
    endif()
    add_test(NAME QuantLib-Risks_test_suite COMMAND QuantLib-Risks_test_suite --log_level=message)
endif()

# =============================================================================
# Benchmark - Split FD/AAD executables for fair comparison
# =============================================================================

# FD benchmark - Finite Differences using plain double QuantLib
# This is built WITHOUT XAD to ensure fair FD vs AAD comparison
option(QLRISKS_BUILD_BENCHMARK_FD "Build FD benchmark (plain double, no XAD)" OFF)
if(QLRISKS_BUILD_BENCHMARK_FD)
    message(STATUS "QLRisks: Building FD benchmark (plain double)")
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
option(QLRISKS_BUILD_BENCHMARK_AAD "Build AAD benchmark (XAD tape, optionally Forge JIT)" OFF)
if(QLRISKS_BUILD_BENCHMARK_AAD)
    message(STATUS "QLRisks: Building AAD benchmark")
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
    if(TARGET QLRisks::forge)
        message(STATUS "QLRisks AAD benchmark: Linking QLRisks::forge (JIT benchmarks enabled)")
        target_link_libraries(benchmark_aad PRIVATE QLRisks::forge)
        target_compile_definitions(benchmark_aad PRIVATE QLRISKS_HAS_FORGE=1)
    else()
        message(STATUS "QLRisks AAD benchmark: Building without Forge (XAD tape only)")
    endif()

    if (QL_INSTALL_TEST_SUITE)
        install(TARGETS benchmark_aad RUNTIME DESTINATION ${QL_INSTALL_BINDIR})
    endif()
endif()
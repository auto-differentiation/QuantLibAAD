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

set(QLAAD_TEST_HEADERS utilities_xad.hpp)

if(QL_BUILD_TEST_SUITE)
    add_executable(QuantLibAAD_test_suite ${QLAAD_TEST_SOURCES} ${QLAAD_TEST_HEADERS})
    set_target_properties(QuantLibAAD_test_suite PROPERTIES OUTPUT_NAME "QuantLibAAD-test-suite")
    if (NOT Boost_USE_STATIC_LIBS)
        target_compile_definitions(QuantLibAAD_test_suite PRIVATE BOOST_ALL_DYN_LINK)
    endif()
    target_link_libraries(QuantLibAAD_test_suite PRIVATE
        ql_library
        ${QL_THREAD_LIBRARIES})
    if (QL_INSTALL_TEST_SUITE)
        install(TARGETS QuantLibAAD_test_suite RUNTIME DESTINATION ${QL_INSTALL_BINDIR})
    endif()
    add_test(NAME QuantLibAAD_test_suite COMMAND QuantLibAAD_test_suite --log_level=message)
endif()

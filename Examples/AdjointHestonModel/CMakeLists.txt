add_executable(
    AdjointHestonModel 
    AdjointHestonModelXAD.cpp)
target_link_libraries(AdjointHestonModel ql_library)
if(QL_INSTALL_EXAMPLES)
    install(TARGETS AdjointHestonModel RUNTIME DESTINATION  ${QL_INSTALL_EXAMPLESDIR})
endif()

add_sample_test(AdjointHestonModel)

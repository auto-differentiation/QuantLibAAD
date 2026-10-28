# XAD's recommended client-side definitions for QLAAD_XAD_RECOMMENDED (guarded by #ifndef in XAD's Config.hpp).
# No strong inlining with MSVC's cl.exe: it makes QuantLib's AD build about 2.4x slower to compile
# (clang-cl has compiler ID Clang and keeps it).

function(qlaad_xad_recommended_definitions out_var)
    set(defs XAD_OUTLINE_RECORDING)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        list(APPEND defs XAD_USE_STRONG_INLINE)
    endif()
    set(${out_var} ${defs} PARENT_SCOPE)
endfunction()

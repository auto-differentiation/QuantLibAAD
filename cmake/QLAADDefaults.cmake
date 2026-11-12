# Build defaults tuned for AD speed; each one can be switched off on the QuantLib command line.

get_directory_property(qlaad_ql_dir PARENT_DIRECTORY)   # QuantLib's source dir
string(TOUPPER "${CMAKE_BUILD_TYPE}" qlaad_build_type)

# QuantLib as a static library, unless BUILD_SHARED_LIBS is set on the command line or QuantLib is
# part of another project, which then decides (its normal variable, else the cache)
if(qlaad_ql_dir STREQUAL CMAKE_SOURCE_DIR AND NOT DEFINED CACHE{BUILD_SHARED_LIBS})
    set(qlaad_shared OFF)
    set(BUILD_SHARED_LIBS OFF PARENT_SCOPE)
else()
    set(qlaad_shared "${BUILD_SHARED_LIBS}")
endif()

# XAD's tape options, fixed in XAD's Config.hpp when XAD configures: QuantLibAAD comes before xad in
# QL_EXTERNAL_SUBDIRECTORIES and sets them as normal variables in QuantLib's scope, which XAD's
# option() takes (CMP0077). So they follow the current settings on every configure, and a value
# in the cache (a -D on the command line) wins.
set(qlaad_xad_on)
# linear slots exclude copy elision, slot reuse and live-slot tracking, which a user may have chosen
if(NOT XAD_COPY_ELISION AND NOT XAD_TAPE_REUSE_SLOTS AND NOT XAD_TAPE_TRACK_LIVE_SLOTS)
    list(APPEND qlaad_xad_on XAD_LINEAR_SLOTS)
endif()
# not with MSVC's cl.exe, where it makes QuantLib's AD build much slower to compile (clang-cl has
# compiler ID Clang and keeps it), and not in single-config Debug builds; XAD's Config.hpp has one
# setting for all configs of a multi-config generator, so these get it in Debug too
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" AND NOT qlaad_build_type STREQUAL "DEBUG")
    list(APPEND qlaad_xad_on XAD_USE_STRONG_INLINE)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
    if(NOT qlaad_shared)   # a shared QuantLib may end up in a module loaded with dlopen
        list(APPEND qlaad_xad_on XAD_TLS_INITIAL_EXEC)
    endif()
    list(APPEND qlaad_xad_on XAD_HUGE_PAGES)
endif()
set(qlaad_xad_options XAD_LINEAR_SLOTS XAD_USE_STRONG_INLINE XAD_TLS_INITIAL_EXEC XAD_HUGE_PAGES)
if(TARGET xad)
    set(qlaad_xad_missing)
    foreach(opt IN LISTS qlaad_xad_on)
        if(NOT ${opt})
            list(APPEND qlaad_xad_missing ${opt})
        endif()
    endforeach()
    list(JOIN qlaad_xad_missing ", " qlaad_xad_missing)
    if(qlaad_xad_missing)
        set(qlaad_xad_missing "these are OFF: ${qlaad_xad_missing}. ")
    endif()
    message(WARNING "XAD was configured first, ${qlaad_xad_missing}"
                    "QuantLibAAD's XAD defaults, LTO and ccache settings don't reach XAD. "
                    "List QuantLibAAD before xad in QL_EXTERNAL_SUBDIRECTORIES for its faster defaults.")
elseif(TARGET XAD::xad)
    message(WARNING "XAD is an installed package: its own Config.hpp decides its tape options, "
                    "QuantLibAAD's XAD defaults don't apply.")
else()
    # A cache entry is the user's when it came from the command line (CMake's help text for a -D;
    # remembered, as XAD's option() then replaces it). Others carry XAD's option() text: in a build
    # dir first configured with xad before QuantLibAAD, XAD wrote them itself, so they are dropped,
    # with a warning, as an old-order -D or an edit in ccmake or cmake-gui looks the same.
    set(qlaad_xad_kept)
    set(qlaad_xad_dropped)
    foreach(opt IN LISTS qlaad_xad_options ITEMS XAD_NO_THREADLOCAL)
        get_property(help CACHE ${opt} PROPERTY HELPSTRING)
        if(help MATCHES "^No help, variable specified on the command line")
            list(APPEND QLAAD_XAD_USER_OPTIONS ${opt})
        elseif(DEFINED CACHE{${opt}} AND NOT opt IN_LIST QLAAD_XAD_USER_OPTIONS)
            # named only when it differs from what then applies
            if(opt IN_LIST qlaad_xad_on)
                set(applies ON)
            else()
                set(applies OFF)
            endif()
            if(NOT ("$CACHE{${opt}}" AND applies) AND ("$CACHE{${opt}}" OR applies))
                list(APPEND qlaad_xad_dropped "${opt}=$CACHE{${opt}}")
            endif()
            unset(${opt} CACHE)
        endif()
    endforeach()
    if(qlaad_xad_dropped)
        list(JOIN qlaad_xad_dropped ", " qlaad_xad_dropped)
        message(WARNING "Dropped XAD's own cached settings from a configure with xad before QuantLibAAD: "
                        "${qlaad_xad_dropped}. QuantLibAAD's defaults apply, pass -D<option>=<value> to keep one.")
    endif()
    list(REMOVE_DUPLICATES QLAAD_XAD_USER_OPTIONS)
    set(QLAAD_XAD_USER_OPTIONS "${QLAAD_XAD_USER_OPTIONS}" CACHE INTERNAL "")
    foreach(opt IN LISTS qlaad_xad_options)
        if(DEFINED CACHE{${opt}} OR DEFINED ${opt})   # the user's, or a parent project's
            list(APPEND qlaad_xad_kept "${opt}=${${opt}}")
        elseif(opt IN_LIST qlaad_xad_on)
            set(${opt} ON PARENT_SCOPE)
        else()
            set(${opt} OFF PARENT_SCOPE)
        endif()
    endforeach()
    if(qlaad_xad_kept)
        list(JOIN qlaad_xad_kept ", " qlaad_xad_kept)
        message(STATUS "QuantLibAAD: XAD options already set: ${qlaad_xad_kept}")
    endif()
endif()

# Link-time optimisation for the targets added after this directory (xad, ql/, Examples/,
# test-suite/) and ours, in the configs other than Debug; build only, not passed on to consumers.
# By default only with GCC, except on Windows (untested there): Clang's LTO objects (thin, IR only)
# would tie installs to the same toolchain. A CMAKE_INTERPROCEDURAL_OPTIMIZATION[_<CONFIG>] in the
# cache, or set by a parent project, wins.
if(NOT WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(qlaad_lto_default ON)
else()
    set(qlaad_lto_default OFF)
endif()
option(QLAAD_LTO "Link-time optimisation for XAD's, QuantLib's and QuantLibAAD's targets (build only, not passed on to consumers)"
       ${qlaad_lto_default})
option(QLAAD_LTO_FAT_OBJECTS "With LTO and GCC, XAD's and QuantLib's libraries also hold machine code, for installs used by other toolchains (slower compile)" OFF)
set(qlaad_lto_configs)
if(DEFINED CACHE{CMAKE_INTERPROCEDURAL_OPTIMIZATION} OR DEFINED CMAKE_INTERPROCEDURAL_OPTIMIZATION)
    if(CMAKE_INTERPROCEDURAL_OPTIMIZATION)
        set(qlaad_lto_configs Debug Release RelWithDebInfo MinSizeRel)
    endif()
elseif(QLAAD_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT qlaad_ipo OUTPUT qlaad_ipo_output LANGUAGES CXX)
    if(NOT qlaad_ipo)
        message(WARNING "QLAAD_LTO: link-time optimisation not supported, ignored: ${qlaad_ipo_output}")
    endif()
    foreach(config Release RelWithDebInfo MinSizeRel)
        string(TOUPPER ${config} c)
        if(DEFINED CACHE{CMAKE_INTERPROCEDURAL_OPTIMIZATION_${c}} OR DEFINED CMAKE_INTERPROCEDURAL_OPTIMIZATION_${c})
            if(CMAKE_INTERPROCEDURAL_OPTIMIZATION_${c})
                list(APPEND qlaad_lto_configs ${config})
            endif()
        elseif(qlaad_ipo)
            set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_${c} ON)
            set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_${c} ON PARENT_SCOPE)
            list(APPEND qlaad_lto_configs ${config})
        endif()
    endforeach()
endif()

# GCC's LTO objects hold only its IR: with QLAAD_LTO_FAT_OBJECTS the installed libraries get machine
# code too, in the same configs, so that they link without LTO and with other toolchains
list(JOIN qlaad_lto_configs "," qlaad_lto_configs)
set(QLAAD_FAT_LTO_CONFIGS "" CACHE INTERNAL "")
if(QLAAD_LTO_FAT_OBJECTS AND qlaad_lto_configs AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if(CMAKE_VERSION VERSION_LESS 3.19)
        message(WARNING "QLAAD_LTO_FAT_OBJECTS needs CMake 3.19, ignored")
    else()
        set(QLAAD_FAT_LTO_CONFIGS ${qlaad_lto_configs} CACHE INTERNAL "")
        cmake_language(DEFER DIRECTORY ${qlaad_ql_dir} CALL qlaad_fat_lto_objects)
    endif()
endif()

function(qlaad_fat_lto_objects)
    foreach(target ql_library xad)
        if(TARGET ${target})
            target_compile_options(${target} PRIVATE "$<$<CONFIG:${QLAAD_FAT_LTO_CONFIGS}>:-ffat-lto-objects>")
        endif()
    endforeach()
endfunction()

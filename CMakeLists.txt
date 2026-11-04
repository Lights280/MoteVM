# ============================================================================
# MoteVM — root build script
#
# Automatic module discovery:
#   src/<module>/<module>.cpp   -> executable target <module>
#   src/<module>/include/       -> module include directory
#   src/include/                -> shared code (headers + implementations)
# Every direct child of src/ is a module (except include); builder is built too.
# src/include is compiled into motevm_core (loader/verifier/disassembler) and
# motevm_tools (assembler/disassembler tooling).
#
# Compiler selection (default: clang++):
#   cmake -S . -B build -G Ninja -DMOTEVM_COMPILER=clang
#   cmake -S . -B build -G Ninja -DMOTEVM_COMPILER=gcc
#   cmake -S . -B build -G Ninja -DMOTEVM_COMPILER=msvc   # inside VS DevShell
#   MSVC via Visual Studio generator: -G "Visual Studio ..." -A x64|Win32
#
# Architecture (default: native):
#   -DMOTEVM_ARCH=x86  /  -DMOTEVM_ARCH=x64
#
# Build type (default: Release for single-config generators;
# Visual Studio: pass --config Release):
#   -DCMAKE_BUILD_TYPE=Release  /  -DCMAKE_BUILD_TYPE=Debug
#
# Module output:
#   <build>/cmake/<toolchain>-<arch>-<config>/bin/<module>[.exe]
# ============================================================================
cmake_minimum_required(VERSION 3.20)

# Capture whether the user explicitly chose a build type before project().
# CMake 4.x with clang on Windows may initialize CMAKE_BUILD_TYPE to Debug.
if(DEFINED CMAKE_BUILD_TYPE AND NOT "${CMAKE_BUILD_TYPE}" STREQUAL "")
    set(MOTEVM_BUILD_TYPE_EXPLICIT ON)
else()
    set(MOTEVM_BUILD_TYPE_EXPLICIT OFF)
endif()

# --- compiler selection (must happen before project()) -----------------------
set(MOTEVM_COMPILER "auto" CACHE STRING "Compiler: auto, clang, gcc, msvc")
set_property(CACHE MOTEVM_COMPILER PROPERTY STRINGS auto clang gcc msvc)
string(TOLOWER "${MOTEVM_COMPILER}" MOTEVM_COMPILER_TAG)

if(NOT DEFINED CMAKE_CXX_COMPILER AND NOT DEFINED ENV{CXX})
    if(MOTEVM_COMPILER_TAG STREQUAL "clang")
        find_program(MOTEVM_CLANGXX NAMES clang++ clang++-20 clang++-19 clang++-18)
        if(NOT MOTEVM_CLANGXX)
            message(FATAL_ERROR "MOTEVM_COMPILER=clang but clang++ was not found")
        endif()
        set(CMAKE_CXX_COMPILER "${MOTEVM_CLANGXX}" CACHE FILEPATH "C++ compiler" FORCE)
    elseif(MOTEVM_COMPILER_TAG STREQUAL "gcc")
        find_program(MOTEVM_GXX NAMES g++ g++-15 g++-14)
        if(NOT MOTEVM_GXX)
            message(FATAL_ERROR "MOTEVM_COMPILER=gcc but g++ was not found")
        endif()
        set(CMAKE_CXX_COMPILER "${MOTEVM_GXX}" CACHE FILEPATH "C++ compiler" FORCE)
    elseif(MOTEVM_COMPILER_TAG STREQUAL "msvc")
        if(NOT CMAKE_GENERATOR MATCHES "Visual Studio")
            find_program(MOTEVM_CL NAMES cl)
            if(NOT MOTEVM_CL)
                message(FATAL_ERROR
                    "MOTEVM_COMPILER=msvc requires a Visual Studio Developer PowerShell "
                    "(Enter-VsDevShell) for Ninja/Makefiles. Alternatively use "
                    "-G \"Visual Studio ...\" with -A x64 or -A Win32.")
            endif()
            set(CMAKE_CXX_COMPILER "${MOTEVM_CL}" CACHE FILEPATH "C++ compiler" FORCE)
        endif()
    else() # auto: prefer clang++ for non-Visual-Studio generators
        if(NOT CMAKE_GENERATOR MATCHES "Visual Studio")
            find_program(MOTEVM_CLANGXX NAMES clang++ clang++-20 clang++-19 clang++-18)
            if(MOTEVM_CLANGXX)
                set(CMAKE_CXX_COMPILER "${MOTEVM_CLANGXX}" CACHE FILEPATH "C++ compiler" FORCE)
                message(STATUS "MoteVM: using clang++ -> ${MOTEVM_CLANGXX}")
            else()
                message(WARNING "MoteVM: clang++ not found; using CMake default C++ compiler")
            endif()
        endif()
    endif()
endif()

if(CMAKE_GENERATOR MATCHES "Visual Studio" AND MOTEVM_COMPILER_TAG MATCHES "clang|gcc")
    message(FATAL_ERROR
        "The Visual Studio generator cannot select ${MOTEVM_COMPILER_TAG}; "
        "use -G Ninja or MinGW Makefiles, or select msvc with -A x64/Win32.")
endif()

project(MoteVM VERSION 0.1.0 LANGUAGES CXX)

# --- validate the selected compiler -----------------------------------------
if(MOTEVM_COMPILER_TAG STREQUAL "clang")
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "MOTEVM_COMPILER=clang but compiler is ${CMAKE_CXX_COMPILER_ID}")
    endif()
elseif(MOTEVM_COMPILER_TAG STREQUAL "gcc")
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        message(FATAL_ERROR "MOTEVM_COMPILER=gcc but compiler is ${CMAKE_CXX_COMPILER_ID}")
    endif()
elseif(MOTEVM_COMPILER_TAG STREQUAL "msvc")
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        message(FATAL_ERROR "MOTEVM_COMPILER=msvc but compiler is ${CMAKE_CXX_COMPILER_ID}")
    endif()
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND
   CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    message(FATAL_ERROR "clang-cl is not supported; use clang++ or MSVC cl")
endif()

# --- default build type: Release --------------------------------------------
if(NOT CMAKE_CONFIGURATION_TYPES)
    if((NOT MOTEVM_BUILD_TYPE_EXPLICIT AND CMAKE_BUILD_TYPE STREQUAL "Debug") OR
       "${CMAKE_BUILD_TYPE}" STREQUAL "")
        set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
    endif()
endif()
# Multi-config generators use their native default; put Release first so
# "cmake --build" without --config prefers it where the generator honors order.
if(CMAKE_CONFIGURATION_TYPES)
    list(REMOVE_ITEM CMAKE_CONFIGURATION_TYPES Release)
    list(INSERT CMAKE_CONFIGURATION_TYPES 0 Release)
    set(CMAKE_CONFIGURATION_TYPES "${CMAKE_CONFIGURATION_TYPES}"
        CACHE STRING "Available configurations" FORCE)
endif()

# --- target architecture -----------------------------------------------------
set(MOTEVM_ARCH "native" CACHE STRING "Target architecture: native, x86, x64")
set_property(CACHE MOTEVM_ARCH PROPERTY STRINGS native x86 x64)
string(TOLOWER "${MOTEVM_ARCH}" MOTEVM_ARCH_TAG)
if(MOTEVM_ARCH_TAG STREQUAL "native")
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(MOTEVM_ARCH_TAG "x64")
    else()
        set(MOTEVM_ARCH_TAG "x86")
    endif()
elseif(NOT MOTEVM_ARCH_TAG MATCHES "^(x86|x64)$")
    message(FATAL_ERROR "MOTEVM_ARCH must be native, x86 or x64")
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    if(MOTEVM_ARCH_TAG STREQUAL "x86" AND NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
        message(FATAL_ERROR
            "MOTEVM_ARCH=x86 with MSVC requires a 32-bit compiler environment. "
            "Use -A Win32 with a Visual Studio generator, or open an x86 DevShell "
            "(Enter-VsDevShell -DevCmdArguments \"-arch=x86 -host_arch=x64\").")
    endif()
    if(MOTEVM_ARCH_TAG STREQUAL "x64" AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR
            "MOTEVM_ARCH=x64 with MSVC requires a 64-bit compiler environment.")
    endif()
endif()

if(MOTEVM_ARCH_TAG STREQUAL "x86" AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -dumpmachine
                    OUTPUT_VARIABLE _motevm_triple OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _motevm_triple MATCHES "i[3-6]86")
        execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -print-multi-lib
                        OUTPUT_VARIABLE _motevm_multilib OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(NOT _motevm_multilib MATCHES "32")
            message(FATAL_ERROR
                "This g++ has no 32-bit multilib (-m32). "
                "Use a MinGW32 toolchain or -DMOTEVM_ARCH=x64.")
        endif()
    endif()
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(MOTEVM_TOOLCHAIN_TAG "msvc")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(MOTEVM_TOOLCHAIN_TAG "clang")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(MOTEVM_TOOLCHAIN_TAG "gcc")
else()
    string(TOLOWER "${CMAKE_CXX_COMPILER_ID}" MOTEVM_TOOLCHAIN_TAG)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Generic outputs (tests/benchmarks); module targets override these below.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# --- 选项（docs/06 §2.1，名字不得改） ---------------------------------------
option(MOTEVM_BUILD_TOOLS      "Build module executables from src/" ON)
option(MOTEVM_BUILD_TESTS      "Build tests (requires tests/)" ON)
option(MOTEVM_BUILD_BENCHMARKS "Build benchmarks (requires benchmarks/)" OFF)
option(MOTEVM_BUILD_FUZZERS    "Build libFuzzer targets (Clang only)" OFF)
option(MOTEVM_SANITIZERS       "Enable ASan/UBSan" OFF)
option(MOTEVM_WERROR           "Treat warnings as errors" OFF)
option(MOTEVM_INSTALL          "Generate install rules" OFF)

# --- 警告集合（docs/06 §2.4） -----------------------------------------------
# 注意：Windows 上 clang++ 的 MSVC 变量同样为 TRUE，因此按 frontend variant 分支，
# 只有 cl / clang-cl 才使用 /W4 风格选项。
function(motevm_apply_warnings target)
    if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC" OR CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
        if(MOTEVM_WERROR)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic
            -Wconversion -Wsign-conversion -Wshadow -Wcast-qual -Wundef
            -Wnon-virtual-dtor -Wold-style-cast
            -Werror=return-type)
        if(MOTEVM_WERROR)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# --- 消毒（docs/06 §2.4） ---------------------------------------------------
function(motevm_apply_sanitizers target)
    if(NOT MOTEVM_SANITIZERS)
        return()
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    elseif(MSVC)
        target_compile_options(${target} PRIVATE /fsanitize=address)
        target_link_options(${target} PRIVATE /fsanitize=address)
    else()
        message(WARNING "MoteVM: MOTEVM_SANITIZERS unsupported by ${CMAKE_CXX_COMPILER_ID}")
    endif()
endfunction()

# --- 架构选择 ---------------------------------------------------------------
function(motevm_apply_arch target)
    if(MOTEVM_ARCH_TAG STREQUAL "x86")
        if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
            # MSVC architecture is fixed by the generator/DevShell; validated above.
        elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND WIN32 AND NOT MINGW)
            target_compile_options(${target} PRIVATE --target=i686-pc-windows-msvc)
            target_link_options(${target} PRIVATE --target=i686-pc-windows-msvc)
        else()
            target_compile_options(${target} PRIVATE -m32)
            target_link_options(${target} PRIVATE -m32)
        endif()
    elseif(MOTEVM_ARCH_TAG STREQUAL "x64")
        if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
            # MSVC architecture is fixed by the generator/DevShell; validated above.
        elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND WIN32 AND NOT MINGW)
            target_compile_options(${target} PRIVATE --target=x86_64-pc-windows-msvc)
            target_link_options(${target} PRIVATE --target=x86_64-pc-windows-msvc)
        else()
            target_compile_options(${target} PRIVATE -m64)
            target_link_options(${target} PRIVATE -m64)
        endif()
    endif()
endfunction()

# --- 输出布局：<build>/cmake/<toolchain>-<arch>-<config>/bin ----------------
function(motevm_set_module_output target)
    if(CMAKE_CONFIGURATION_TYPES)
        foreach(config IN LISTS CMAKE_CONFIGURATION_TYPES)
            string(TOUPPER "${config}" config_property)
            string(TOLOWER "${config}" config_tag)
            set(output_dir
                "${CMAKE_BINARY_DIR}/cmake/${MOTEVM_TOOLCHAIN_TAG}-${MOTEVM_ARCH_TAG}-${config_tag}/bin")
            set_target_properties(${target} PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY_${config_property} "${output_dir}"
                PDB_OUTPUT_DIRECTORY_${config_property} "${output_dir}")
        endforeach()
    else()
        string(TOLOWER "${CMAKE_BUILD_TYPE}" config_tag)
        if(config_tag STREQUAL "")
            set(config_tag "release")
        endif()
        set(output_dir
            "${CMAKE_BINARY_DIR}/cmake/${MOTEVM_TOOLCHAIN_TAG}-${MOTEVM_ARCH_TAG}-${config_tag}/bin")
        set_target_properties(${target} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${output_dir}"
            PDB_OUTPUT_DIRECTORY "${output_dir}")
    endif()
endfunction()


# --- 自动模块发现 -----------------------------------------------------------
set(MOTEVM_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(MOTEVM_MODULE_TARGETS "")
set(MOTEVM_MODULE_DIRS "")
set(MOTEVM_MODULE_NAMES "")

# --- 共享库（不参与模块发现） ----------------------------------------------
# src/include contains every translation unit shared by more than one module:
#   motevm_core  : .mbc loader/verifier/disassembler (parse/verifier/disasm)
#   motevm_tools : assembler/disassembler tooling built on motevm_core
# Both libraries expose src/include as their public include directory.
set(MOTEVM_CORE_SOURCES
    "${MOTEVM_SRC_DIR}/include/parse.cpp"
    "${MOTEVM_SRC_DIR}/include/verifier.cpp"
    "${MOTEVM_SRC_DIR}/include/disasm.cpp")

add_library(motevm_core STATIC ${MOTEVM_CORE_SOURCES})
add_library(motevm::core ALIAS motevm_core)
target_include_directories(motevm_core PUBLIC "${MOTEVM_SRC_DIR}/include")
target_compile_features(motevm_core PUBLIC cxx_std_20)
motevm_apply_warnings(motevm_core)
motevm_apply_sanitizers(motevm_core)
motevm_apply_arch(motevm_core)

set(MOTEVM_TOOL_SOURCES
    "${MOTEVM_SRC_DIR}/include/asm/assembler.cpp"
    "${MOTEVM_SRC_DIR}/include/asm/emitter.cpp"
    "${MOTEVM_SRC_DIR}/include/asm/lexer.cpp"
    "${MOTEVM_SRC_DIR}/include/asm/parser.cpp"
    "${MOTEVM_SRC_DIR}/include/common/diagnostics.cpp"
    "${MOTEVM_SRC_DIR}/include/common/io.cpp"
    "${MOTEVM_SRC_DIR}/include/common/path.cpp"
    "${MOTEVM_SRC_DIR}/include/common/text.cpp"
    "${MOTEVM_SRC_DIR}/include/common/trap.cpp"
    "${MOTEVM_SRC_DIR}/include/dis/disassembler.cpp")

add_library(motevm_tools STATIC ${MOTEVM_TOOL_SOURCES})
target_link_libraries(motevm_tools PUBLIC motevm_core)
target_include_directories(motevm_tools PUBLIC "${MOTEVM_SRC_DIR}/include")
target_compile_features(motevm_tools PUBLIC cxx_std_20)
motevm_apply_warnings(motevm_tools)
motevm_apply_sanitizers(motevm_tools)
motevm_apply_arch(motevm_tools)

if(MOTEVM_BUILD_TOOLS)
    file(GLOB MOTEVM_MODULE_PATHS LIST_DIRECTORIES TRUE CONFIGURE_DEPENDS
        "${MOTEVM_SRC_DIR}/*")
    list(SORT MOTEVM_MODULE_PATHS)

    foreach(module_path IN LISTS MOTEVM_MODULE_PATHS)
        if(NOT IS_DIRECTORY "${module_path}")
            continue()
        endif()

        get_filename_component(module_name "${module_path}" NAME)
        if(module_name STREQUAL "include")
            continue()
        endif()
        if(NOT module_name MATCHES "^[a-z][a-z0-9_]*$")
            message(FATAL_ERROR
                "MoteVM: module directory '${module_path}' must match [a-z][a-z0-9_]*")
        endif()
        if(module_name MATCHES
           "^(all|clean|help|install|test|package|edit_cache|rebuild_cache|list_install_components|depend|preinstall)$")
            message(FATAL_ERROR
                "MoteVM: module name '${module_name}' collides with a CMake target")
        endif()

        set(module_main "${module_path}/${module_name}.cpp")
        if(NOT EXISTS "${module_main}")
            message(FATAL_ERROR
                "MoteVM: module '${module_name}' is missing main file '${module_main}'")
        endif()

        file(GLOB_RECURSE module_sources CONFIGURE_DEPENDS "${module_path}/*.cpp")
        list(FILTER module_sources EXCLUDE REGEX "[/\\\\]include[/\\\\]")
        if(NOT module_sources)
            message(FATAL_ERROR "MoteVM: module '${module_name}' contains no .cpp files")
        endif()

        add_executable(${module_name} ${module_sources})
        target_include_directories(${module_name} PRIVATE
            "${module_path}/include"
            "${MOTEVM_SRC_DIR}/include")
        target_compile_features(${module_name} PRIVATE cxx_std_20)

        motevm_apply_warnings(${module_name})
        motevm_apply_sanitizers(${module_name})
        motevm_apply_arch(${module_name})
        motevm_set_module_output(${module_name})

        if(module_name STREQUAL "motevm")
            target_link_libraries(${module_name} PRIVATE motevm_core)
        elseif(module_name STREQUAL "moteasm" OR module_name STREQUAL "motedis")
            target_link_libraries(${module_name} PRIVATE motevm_tools)
        endif()

        list(APPEND MOTEVM_MODULE_TARGETS ${module_name})
        list(APPEND MOTEVM_MODULE_DIRS "${module_path}")
        list(APPEND MOTEVM_MODULE_NAMES "${module_name}")
        list(LENGTH module_sources _motevm_source_count)
        message(STATUS
            "MoteVM module: ${module_name} (${_motevm_source_count} source(s))")
    endforeach()

    if(NOT MOTEVM_MODULE_TARGETS)
        message(FATAL_ERROR "MoteVM: no modules found under '${MOTEVM_SRC_DIR}'")
    endif()
endif()

if(MOTEVM_BUILD_TESTS)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
        enable_testing()
        add_subdirectory(tests)
    else()
        message(STATUS "MoteVM: MOTEVM_BUILD_TESTS=ON but tests/ is not present; skipping")
    endif()
endif()

if(MOTEVM_BUILD_BENCHMARKS)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/CMakeLists.txt")
        add_subdirectory(benchmarks)
    else()
        message(STATUS "MoteVM: MOTEVM_BUILD_BENCHMARKS=ON but benchmarks/ is not present; skipping")
    endif()
endif()

if(MOTEVM_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(WARNING "MoteVM: MOTEVM_BUILD_FUZZERS requires Clang; skipping")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fuzz/CMakeLists.txt")
        add_subdirectory(fuzz)
    else()
        message(STATUS "MoteVM: MOTEVM_BUILD_FUZZERS=ON but fuzz/ is not present; skipping")
    endif()
endif()

# --- 安装（v1 不承诺 ABI，默认关闭） ----------------------------------------
if(MOTEVM_INSTALL)
    include(GNUInstallDirs)
    if(MOTEVM_MODULE_TARGETS)
        install(TARGETS ${MOTEVM_MODULE_TARGETS}
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    endif()
    if(EXISTS "${MOTEVM_SRC_DIR}/include")
        install(DIRECTORY "${MOTEVM_SRC_DIR}/include/"
            DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
    endif()
    foreach(module_dir module_name IN ZIP_LISTS MOTEVM_MODULE_DIRS MOTEVM_MODULE_NAMES)
        if(EXISTS "${module_dir}/include")
            install(DIRECTORY "${module_dir}/include/"
                DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/${module_name}")
        endif()
    endforeach()
endif()

if(CMAKE_CONFIGURATION_TYPES)
    set(_motevm_config_text "${CMAKE_CONFIGURATION_TYPES}")
else()
    set(_motevm_config_text "${CMAKE_BUILD_TYPE}")
endif()
message(STATUS
    "MoteVM ${PROJECT_VERSION} (MOTEBC 1.0) | CXX=${CMAKE_CXX_COMPILER_ID} "
    "${CMAKE_CXX_COMPILER_VERSION} | ${MOTEVM_TOOLCHAIN_TAG} ${MOTEVM_ARCH_TAG} "
    "${_motevm_config_text} | ${CMAKE_GENERATOR}")
message(STATUS "MoteVM: modules: ${MOTEVM_MODULE_NAMES}")
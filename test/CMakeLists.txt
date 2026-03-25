cmake_minimum_required(VERSION 3.10)
project(XbyakTests)

# Set C++ standard
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable CTest support
enable_testing()

# Xbyak is a header-only library in the parent directory
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/..)
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# ---------------------------------------------------------------------------
# Read compiler warning flags from CFLAGS_WARN.cfg (GCC/Clang only)
# ---------------------------------------------------------------------------
if(NOT MSVC AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/CFLAGS_WARN.cfg")
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/CFLAGS_WARN.cfg" CFLAGS_WARN_STR)
    separate_arguments(CFLAGS_WARN UNIX_COMMAND "${CFLAGS_WARN_STR}")
endif()

# ---------------------------------------------------------------------------
# Platform-specific compile flags
# ---------------------------------------------------------------------------
if(MSVC)
    # MSVC: exceptions on, standard warnings, suppress secure CRT warnings
    set(COMMON_FLAGS /EHsc /W4 /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX)
else()
    # GCC/Clang: optimize + all warnings from CFLAGS_WARN.cfg
    set(COMMON_FLAGS -O2 ${CFLAGS_WARN})
endif()

# ---------------------------------------------------------------------------
# Architecture detection
# ---------------------------------------------------------------------------
set(IS_64BIT FALSE)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(IS_64BIT TRUE)
endif()

# 32-bit binaries are not supported on macOS or MINGW64
set(ONLY_64BIT FALSE)
if(APPLE)
    set(ONLY_64BIT TRUE)
endif()
if(WIN32 AND DEFINED ENV{MSYSTEM})
    string(FIND "$ENV{MSYSTEM}" "MINGW64" MINGW64_POS)
    if(NOT MINGW64_POS EQUAL -1)
        set(ONLY_64BIT TRUE)
    endif()
endif()

# ---------------------------------------------------------------------------
# Option: 32-bit jmp / address tests (require g++-multilib on 64-bit Linux)
# ---------------------------------------------------------------------------
option(BUILD_32BIT_TARGETS
    "Build 32-bit jmp and address binaries (requires g++-multilib on 64-bit Linux)" OFF)

# ---------------------------------------------------------------------------
# Helper function: add a standard test executable to bin/
# Usage: add_test_exe(<name> <source> [extra_compile_flags…])
# ---------------------------------------------------------------------------
function(add_test_exe name source)
    add_executable(${name} ${source})
    target_compile_options(${name} PRIVATE ${COMMON_FLAGS} ${ARGN})
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin")
endfunction()

# ---------------------------------------------------------------------------
# normalize_prefix
#   Shell scripts reference this as ./normalize_prefix (relative to their
#   working directory, which is the source tree).  Build it directly into
#   the source directory so the existing scripts work without modification.
# ---------------------------------------------------------------------------
add_executable(normalize_prefix normalize_prefix.cpp)
target_compile_options(normalize_prefix PRIVATE ${COMMON_FLAGS})
set_target_properties(normalize_prefix PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_CURRENT_SOURCE_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_CURRENT_SOURCE_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}")

# ---------------------------------------------------------------------------
# detect_x32  (C source — detects x32 ABI on Linux)
# ---------------------------------------------------------------------------
add_executable(detect_x32 detect_x32.c)
set_target_properties(detect_x32 PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin")

# ---------------------------------------------------------------------------
# Standalone test binaries (all architectures)
# ---------------------------------------------------------------------------
add_test_exe(bad_address   bad_address.cpp)
add_test_exe(misc          misc.cpp)
add_test_exe(misc32        misc.cpp         -DXBYAK32)
add_test_exe(cvt_test      cvt_test.cpp)
add_test_exe(cvt_test32    cvt_test.cpp     -DXBYAK32)

# noexception: compiled with C++ exceptions disabled
add_executable(noexception noexception.cpp)
target_compile_options(noexception PRIVATE ${COMMON_FLAGS})
if(NOT MSVC)
    target_compile_options(noexception PRIVATE -fno-exceptions)
else()
    target_compile_options(noexception PRIVATE /EHs-)
endif()
set_target_properties(noexception PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin")

# ---------------------------------------------------------------------------
# Optional 32-bit jmp / address  (need -m32, requires multilib on Linux)
# ---------------------------------------------------------------------------
if(BUILD_32BIT_TARGETS AND NOT ONLY_64BIT AND UNIX AND NOT WIN32 AND NOT MSVC)
    message(STATUS "Building 32-bit targets (requires g++-multilib)")

    add_test_exe(jmp jmp.cpp)
    target_compile_options(jmp PRIVATE -m32)
    target_link_options(jmp PRIVATE -m32)

    add_test_exe(address address.cpp -DXBYAK32)
    target_compile_options(address PRIVATE -m32)
    target_link_options(address PRIVATE -m32)
endif()

# ---------------------------------------------------------------------------
# 64-bit-only binaries
# ---------------------------------------------------------------------------
if(IS_64BIT)
    add_test_exe(jmp64             jmp.cpp)
    add_test_exe(address64         address.cpp          -DXBYAK64)
    add_test_exe(apx               apx.cpp)
    add_test_exe(avx10_test        avx10_test.cpp       -DXBYAK64)
    add_test_exe(sf_test           sf_test.cpp          -DXBYAK64)
    add_test_exe(cpumask_test      cpumask_test.cpp)
    add_test_exe(reg_manager_test  reg_manager_test.cpp)
endif()

# ---------------------------------------------------------------------------
# lib_run — library API usage example (builds but is not a CTest test)
# ---------------------------------------------------------------------------
add_executable(lib_run lib_run.cpp lib_test.cpp)
target_compile_options(lib_run PRIVATE ${COMMON_FLAGS})
set_target_properties(lib_run PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin")

# ---------------------------------------------------------------------------
# CTest: Standalone binary tests (no assembler required)
# ---------------------------------------------------------------------------
set(BIN "${CMAKE_BINARY_DIR}/bin")

add_test(NAME bad_address  COMMAND "${BIN}/bad_address")
add_test(NAME misc         COMMAND "${BIN}/misc")
add_test(NAME misc32       COMMAND "${BIN}/misc32")
add_test(NAME cvt_test     COMMAND "${BIN}/cvt_test")
add_test(NAME cvt_test32   COMMAND "${BIN}/cvt_test32")

if(BUILD_32BIT_TARGETS AND NOT ONLY_64BIT AND UNIX AND NOT WIN32 AND NOT MSVC)
    add_test(NAME jmp COMMAND "${BIN}/jmp")
endif()

if(IS_64BIT)
    add_test(NAME jmp64            COMMAND "${BIN}/jmp64")
    add_test(NAME apx              COMMAND "${BIN}/apx")
    add_test(NAME avx10_test       COMMAND "${BIN}/avx10_test")
    add_test(NAME sf_test          COMMAND "${BIN}/sf_test")
    add_test(NAME cpumask_test     COMMAND "${BIN}/cpumask_test")
    add_test(NAME reg_manager_test COMMAND "${BIN}/reg_manager_test")
endif()

# ---------------------------------------------------------------------------
# CTest: Assembler-comparison tests (Linux/macOS, shell scripts)
#
#   These tests compare xbyak's JIT output byte-for-byte against nasm/yasm.
#   Shell scripts recompile their code generator (make_nm / make_512 /
#   address) at test time, so only normalize_prefix needs to be pre-built.
#   WORKING_DIRECTORY is set to the source tree so the scripts can find
#   CFLAGS_WARN.cfg, *.cpp sources, and ./normalize_prefix.
# ---------------------------------------------------------------------------
if(UNIX AND NOT WIN32)
    find_program(NASM_EXECUTABLE nasm)
    find_program(YASM_EXECUTABLE yasm)
    find_program(AWK_EXECUTABLE  awk)

    # Helper macro — registers one shell-script test with CXX forwarded
    macro(add_asm_test test_name script)
        add_test(NAME ${test_name}
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/${script}" ${ARGN}
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
        set_tests_properties(${test_name} PROPERTIES
            ENVIRONMENT "CXX=${CMAKE_CXX_COMPILER}"
            DEPENDS normalize_prefix)
    endmacro()

    # ---- nasm-based tests ----
    if(NASM_EXECUTABLE AND AWK_EXECUTABLE)
        message(STATUS "nasm found: ${NASM_EXECUTABLE} — nasm assembler tests enabled")

        add_asm_test(test_nm_nasm32      test_nm.sh)
        add_asm_test(test_nm_noexcept    test_nm.sh noexcept)
        add_asm_test(test_avx_nasm32     test_avx.sh)
        add_asm_test(test_avx512_nasm32  test_avx512.sh)
        add_asm_test(test_address_32     test_address.sh)

        if(IS_64BIT)
            add_asm_test(test_nm_nasm64       test_nm.sh 64)
            add_asm_test(test_nm_avx512_inst  test_nm.sh avx512)
            add_asm_test(test_avx_nasm64      test_avx.sh 64)
            add_asm_test(test_avx512_nasm64   test_avx512.sh 64)
            add_asm_test(test_address_64      test_address.sh 64)
        endif()
    else()
        message(STATUS "nasm/awk not found — nasm assembler tests skipped")
    endif()

    # ---- yasm-based tests ----
    if(YASM_EXECUTABLE AND AWK_EXECUTABLE)
        message(STATUS "yasm found: ${YASM_EXECUTABLE} — yasm assembler tests enabled")

        add_asm_test(test_nm_yasm32   test_nm.sh Y)
        add_asm_test(test_avx_yasm32  test_avx.sh Y)

        if(IS_64BIT)
            add_asm_test(test_nm_yasm64   test_nm.sh Y64)
            add_asm_test(test_avx_yasm64  test_avx.sh Y64)
        endif()
    else()
        message(STATUS "yasm not found — yasm assembler tests skipped")
    endif()
endif()

# ---------------------------------------------------------------------------
# CTest: XED-based encoding validation tests (optional)
#   Requires: xed tool + python3
#   Each file in dataset/ is tested independently so failures are isolated.
# ---------------------------------------------------------------------------
find_program(XED_EXECUTABLE    xed)
find_program(PYTHON3_EXECUTABLE python3)

if(UNIX AND NOT WIN32 AND XED_EXECUTABLE AND PYTHON3_EXECUTABLE)
    message(STATUS "xed found: ${XED_EXECUTABLE} — XED validation tests enabled")

    file(GLOB XED_DATASETS "${CMAKE_CURRENT_SOURCE_DIR}/dataset/*.txt")
    foreach(DATASET ${XED_DATASETS})
        get_filename_component(DS_NAME ${DATASET} NAME_WE)
        add_test(NAME xed_test_${DS_NAME}
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/test_by_xed.sh" "${DATASET}"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
        set_tests_properties(xed_test_${DS_NAME} PROPERTIES
            ENVIRONMENT
                "CXX=${CMAKE_CXX_COMPILER};XED=${XED_EXECUTABLE};PYTHON=${PYTHON3_EXECUTABLE}")
    endforeach()
else()
    if(UNIX AND NOT WIN32)
        message(STATUS "xed/python3 not found — XED validation tests skipped")
    endif()
endif()

# ---------------------------------------------------------------------------
# Configuration summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "Xbyak Test Configuration:")
message(STATUS "  64-bit build:          ${IS_64BIT}")
message(STATUS "  32-bit targets:        ${BUILD_32BIT_TARGETS}")
message(STATUS "  nasm:                  ${NASM_EXECUTABLE}")
message(STATUS "  yasm:                  ${YASM_EXECUTABLE}")
message(STATUS "  XED:                   ${XED_EXECUTABLE}")
message(STATUS "  python3:               ${PYTHON3_EXECUTABLE}")
message(STATUS "  Executable output:     ${CMAKE_BINARY_DIR}/bin")
message(STATUS "  normalize_prefix at:   ${CMAKE_CURRENT_SOURCE_DIR}/normalize_prefix")
message(STATUS "")

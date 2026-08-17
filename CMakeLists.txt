cmake_minimum_required(VERSION 3.14)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

project(curve_fx_harness VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Production throughput tuning is deliberately opt-in.  The defaults must remain
# safe for binaries deployed across heterogeneous blades.
option(CURVE_FX_ENABLE_IPO
    "Enable compiler-supported interprocedural optimization (portable LTO)"
    OFF)
option(CURVE_FX_NATIVE_TUNING
    "Tune evaluator code for the build host CPU (not portable across blades)"
    OFF)

if(CURVE_FX_ENABLE_IPO)
    include(CheckIPOSupported)
    check_ipo_supported(
        RESULT CURVE_FX_IPO_SUPPORTED
        OUTPUT CURVE_FX_IPO_ERROR
        LANGUAGES CXX
    )
    if(NOT CURVE_FX_IPO_SUPPORTED)
        message(FATAL_ERROR
            "CURVE_FX_ENABLE_IPO requested, but the compiler does not support IPO: "
            "${CURVE_FX_IPO_ERROR}"
        )
    endif()
endif()

if(CURVE_FX_NATIVE_TUNING AND
   NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    message(FATAL_ERROR
        "CURVE_FX_NATIVE_TUNING is only supported by GNU or Clang compilers"
    )
endif()

if(CURVE_FX_ENABLE_IPO)
    set(CURVE_FX_ENABLE_IPO_LOWER "true")
else()
    set(CURVE_FX_ENABLE_IPO_LOWER "false")
endif()
if(CURVE_FX_NATIVE_TUNING)
    set(CURVE_FX_NATIVE_TUNING_LOWER "true")
else()
    set(CURVE_FX_NATIVE_TUNING_LOWER "false")
endif()


# Optimization flags

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(
        -O3
        -Wall
        -Wextra
        -Wpedantic
        -Wno-unused-parameter
    )
endif()

# Dependencies
find_package(Threads REQUIRED)

find_package(OpenMP)
if(OpenMP_CXX_FOUND)
    set(OPENMP_CXX_LIB OpenMP::OpenMP_CXX)
else()
    set(OPENMP_CXX_LIB "")
endif()

# Boost (Headers and Boost.JSON)
find_package(Boost REQUIRED COMPONENTS json)

# SHA-256 implementation
add_library(curve_fx_sha256 INTERFACE)
if(APPLE)
    target_compile_definitions(curve_fx_sha256 INTERFACE ARB_SHA256_COMMONCRYPTO=1)
else()
    find_package(OpenSSL QUIET)
    if(OpenSSL_FOUND)
        target_compile_definitions(curve_fx_sha256 INTERFACE ARB_SHA256_OPENSSL=1)
        target_link_libraries(curve_fx_sha256 INTERFACE OpenSSL::Crypto)
    endif()
endif()

# twocrypto::pool resolution
# Strictly consume installed twocrypto::pool package
find_package(twocrypto_pool REQUIRED CONFIG)
if(NOT TARGET twocrypto::pool)
    message(FATAL_ERROR "Installed twocrypto::pool target not found. Please install twocrypto-cpp first.")
endif()

if(DEFINED twocrypto_pool_VERSION)
    set(RESOLVED_POOL_VERSION "${twocrypto_pool_VERSION}")
else()
    message(FATAL_ERROR "Installed twocrypto_pool exports no version; reinstall twocrypto-cpp.")
endif()

# Policy selection and identity generation
set(POLICY_ID "" CACHE STRING "Optional identifier for compiled policy")
set(POLICY_ABI "twocrypto_policy_v1" CACHE STRING "ABI version of compiled policy")
set(POLICY_HEADER_PATH "" CACHE FILEPATH "Optional path to private compiled policy regular-file header")
set(POLICY_EXPECTED_SHA256 "" CACHE STRING "Optional expected 64-hex SHA-256 hash of compiled policy header")

if(POLICY_HEADER_PATH)
    if(NOT EXISTS "${POLICY_HEADER_PATH}" OR IS_DIRECTORY "${POLICY_HEADER_PATH}")
        message(FATAL_ERROR "POLICY_HEADER_PATH does not name an existing regular file: ${POLICY_HEADER_PATH}")
    endif()

    if(NOT POLICY_EXPECTED_SHA256)
        message(FATAL_ERROR "POLICY_EXPECTED_SHA256 must be provided when POLICY_HEADER_PATH is set.")
    endif()

    string(LENGTH "${POLICY_EXPECTED_SHA256}" POLICY_EXPECTED_SHA256_LEN)
    if(NOT POLICY_EXPECTED_SHA256_LEN EQUAL 64)
        message(FATAL_ERROR "POLICY_EXPECTED_SHA256 must be a 64-character hex SHA-256 hash.")
    endif()

    file(SHA256 "${POLICY_HEADER_PATH}" COMPUTED_POLICY_SHA256)
    if(NOT ("${COMPUTED_POLICY_SHA256}" STREQUAL "${POLICY_EXPECTED_SHA256}"))
        message(FATAL_ERROR "Compiled policy SHA-256 digest mismatch! Expected: ${POLICY_EXPECTED_SHA256}, Actual computed: ${COMPUTED_POLICY_SHA256}")
    endif()

    if(NOT POLICY_ID)
        message(FATAL_ERROR "POLICY_ID is required when POLICY_HEADER_PATH is set.")
    endif()
    if(NOT POLICY_ABI STREQUAL "twocrypto_policy_v1")
        message(FATAL_ERROR "Compiled policies implement exactly twocrypto_policy_v1.")
    endif()
    set(EFFECTIVE_POLICY_SHA256 "${COMPUTED_POLICY_SHA256}")
    set(EFFECTIVE_POLICY_ID "${POLICY_ID}")
    set(HAS_COMPILED_POLICY_LOWER "true")
else()
    if(POLICY_EXPECTED_SHA256)
        message(FATAL_ERROR "POLICY_EXPECTED_SHA256 requires POLICY_HEADER_PATH.")
    endif()
    if(POLICY_ID)
        message(FATAL_ERROR "POLICY_ID requires POLICY_HEADER_PATH.")
    endif()
    set(EFFECTIVE_POLICY_ID "twocrypto_native")
    set(EFFECTIVE_POLICY_SHA256 "none")
    set(HAS_COMPILED_POLICY_LOWER "false")
endif()

# Generate compiled_policy_identity.hpp
set(IDENTITY_HEADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/curve_fx_evaluator")
file(MAKE_DIRECTORY "${IDENTITY_HEADER_DIR}")
set(IDENTITY_HEADER "${IDENTITY_HEADER_DIR}/compiled_policy_identity.hpp")

file(WRITE "${IDENTITY_HEADER}" "// Generated by CMake - DO NOT EDIT\n"
    "#pragma once\n\n"
    "namespace curve_fx {\n"
    "namespace identity {\n"
    "static constexpr const char* HARNESS_VERSION = \"${PROJECT_VERSION}\";\n"
    "static constexpr const char* POOL_VERSION = \"${RESOLVED_POOL_VERSION}\";\n"
    "static constexpr const char* POLICY_ID = \"${EFFECTIVE_POLICY_ID}\";\n"
    "static constexpr const char* POLICY_ABI = \"${POLICY_ABI}\";\n"
    "static constexpr const char* POLICY_SOURCE_SHA256 = \"${EFFECTIVE_POLICY_SHA256}\";\n"
    "static constexpr const char* COMPILER_ID = \"${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}\";\n"
    "static constexpr bool HAS_COMPILED_POLICY = ${HAS_COMPILED_POLICY_LOWER};\n"
    "static constexpr bool ENABLE_IPO = ${CURVE_FX_ENABLE_IPO_LOWER};\n"
    "static constexpr bool NATIVE_TUNING = ${CURVE_FX_NATIVE_TUNING_LOWER};\n"
    "} // namespace identity\n"
    "} // namespace curve_fx\n"
)

# Common evaluator sources
set(CURVE_FX_EVALUATOR_SOURCES
    cpp/src/evaluator_core.cpp
    cpp/src/scenario_store.cpp
    cpp/src/trace.cpp
    cpp/src/events.cpp
    cpp/src/chainlink.cpp
    cpp/src/pool_config_source.cpp
)

# Core Evaluator Library (Production Mode: Long Double)
add_library(curve_fx_evaluator STATIC
    ${CURVE_FX_EVALUATOR_SOURCES}
)

target_include_directories(curve_fx_evaluator PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/cpp/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
    $<INSTALL_INTERFACE:include>
)

target_compile_definitions(curve_fx_evaluator PUBLIC
    ARB_MODE_LD=1
)

target_link_libraries(curve_fx_evaluator PUBLIC
    curve_fx_sha256
    twocrypto::pool
    Boost::json
    Threads::Threads
    ${OPENMP_CXX_LIB}
)

if(POLICY_HEADER_PATH)
    get_filename_component(POLICY_INCLUDE_DIR "${POLICY_HEADER_PATH}" DIRECTORY)
    target_include_directories(curve_fx_evaluator PRIVATE "${POLICY_INCLUDE_DIR}")
    # The evaluator's public headers instantiate pool templates.  Every
    # translation unit that consumes them must select the same policy type or
    # the program violates the C++ one-definition rule.
    target_compile_definitions(curve_fx_evaluator PUBLIC
        TWOCRYPTO_POLICY_HEADER="${POLICY_HEADER_PATH}"
    )
endif()

# Production evaluator executable
add_executable(arb_evaluator_ld
    cpp/src/main.cpp
)

target_compile_definitions(arb_evaluator_ld PRIVATE
    ARB_MODE_LD=1
    BUILD_TARGET_NAME="arb_evaluator_ld"
)

target_link_libraries(arb_evaluator_ld PRIVATE
    curve_fx_evaluator
)
if(CURVE_FX_ENABLE_IPO)
    # Let CMake select the compiler/linker IPO implementation for this toolchain.
    set_property(TARGET curve_fx_evaluator PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    set_property(TARGET arb_evaluator_ld PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

if(CURVE_FX_NATIVE_TUNING)
    # PUBLIC keeps template instantiations in consumers on the same CPU
    # contract as the evaluator library.  This option is intentionally off by
    # default because the resulting binary is not heterogeneous-blade-safe.
    target_compile_options(curve_fx_evaluator PUBLIC -march=native)
endif()


# Tests
option(BUILD_TESTING "Build test suite" ON)
if(BUILD_TESTING)
    enable_testing()

    function(add_harness_test TEST_NAME SOURCE_FILE)
        add_executable(${TEST_NAME} ${SOURCE_FILE})
        target_link_libraries(${TEST_NAME} PRIVATE
            curve_fx_evaluator
        )
        add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
    endfunction()

    add_harness_test(test_trace_arena cpp/tests/test_trace_arena.cpp)
    add_harness_test(test_scenario_store cpp/tests/test_scenario_store.cpp)
    add_harness_test(test_donation cpp/tests/test_donation.cpp)
    add_harness_test(test_yb_2l cpp/tests/test_yb_2l.cpp)
    add_harness_test(test_metric_semantics cpp/tests/test_metric_semantics.cpp)
    add_harness_test(test_io_fastpaths cpp/tests/test_io_fastpaths.cpp)

    # test_balanced_withdrawal uses rejecting_fixed_policy.hpp
    add_executable(test_balanced_withdrawal cpp/tests/test_balanced_withdrawal.cpp)
    target_compile_definitions(test_balanced_withdrawal PRIVATE
        TWOCRYPTO_POLICY_HEADER="${CMAKE_CURRENT_SOURCE_DIR}/cpp/tests/rejecting_fixed_policy.hpp"
    )
    target_link_libraries(test_balanced_withdrawal PRIVATE
        twocrypto::pool
    )
    add_test(NAME test_balanced_withdrawal COMMAND test_balanced_withdrawal)
endif()

# Install rules
include(GNUInstallDirs)
install(TARGETS arb_evaluator_ld
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

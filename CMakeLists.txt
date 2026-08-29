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
option(CURVE_FX_EDGE_GATE_DIAGNOSTICS
    "Collect per-event geometry and fee-gate counters"
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

# Boost (Headers and Boost.JSON)
find_package(Boost REQUIRED COMPONENTS json)

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
# The installed package deliberately exposes no source-control state. Keep
# the description honest without inventing a revision or dirty flag.
set(RESOLVED_POOL_GIT_REVISION "unavailable")
set(RESOLVED_POOL_GIT_DIRTY "unknown")

set(CURVE_FX_HARNESS_REVISION_OVERRIDE "" CACHE STRING
    "Attested harness source revision for builds without Git metadata")
set(CURVE_FX_HARNESS_DIRTY_OVERRIDE "" CACHE STRING
    "Harness source dirty state for builds without Git metadata (true, false, or unknown)")
if(CURVE_FX_HARNESS_REVISION_OVERRIDE)
    set(HARNESS_GIT_REVISION "${CURVE_FX_HARNESS_REVISION_OVERRIDE}")
else()
    execute_process(
        COMMAND git -C "${CMAKE_CURRENT_SOURCE_DIR}" rev-parse HEAD
        OUTPUT_VARIABLE HARNESS_GIT_REVISION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(NOT HARNESS_GIT_REVISION)
        set(HARNESS_GIT_REVISION "unknown")
    endif()
endif()
if(CURVE_FX_HARNESS_DIRTY_OVERRIDE)
    set(HARNESS_GIT_DIRTY "${CURVE_FX_HARNESS_DIRTY_OVERRIDE}")
else()
    execute_process(
        COMMAND git -C "${CMAKE_CURRENT_SOURCE_DIR}" status --porcelain
            --untracked-files=normal
        OUTPUT_VARIABLE HARNESS_GIT_STATUS
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE HARNESS_GIT_STATUS_RESULT
        ERROR_QUIET
    )
    if(NOT HARNESS_GIT_STATUS_RESULT EQUAL 0)
        set(HARNESS_GIT_DIRTY "unknown")
    elseif(HARNESS_GIT_STATUS)
        set(HARNESS_GIT_DIRTY "true")
    else()
        set(HARNESS_GIT_DIRTY "false")
    endif()
endif()

# Policy selection and identity generation
set(POLICY_ID "" CACHE STRING "Optional identifier for compiled policy")
set(POLICY_ABI "twocrypto_policy_v1" CACHE STRING "ABI version of compiled policy")
set(POLICY_HEADER_PATH "" CACHE FILEPATH "Optional path to private compiled policy regular-file header")
if(NOT POLICY_ABI STREQUAL "twocrypto_policy_v1")
    message(FATAL_ERROR "Compiled policies implement exactly twocrypto_policy_v1.")
endif()

if(POLICY_HEADER_PATH)
    if(NOT EXISTS "${POLICY_HEADER_PATH}" OR IS_DIRECTORY "${POLICY_HEADER_PATH}")
        message(FATAL_ERROR "POLICY_HEADER_PATH does not name an existing regular file: ${POLICY_HEADER_PATH}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${POLICY_HEADER_PATH}")

    if(NOT POLICY_ID)
        message(FATAL_ERROR "POLICY_ID is required when POLICY_HEADER_PATH is set.")
    endif()
    set(EFFECTIVE_POLICY_ID "${POLICY_ID}")
    set(HAS_COMPILED_POLICY_LOWER "true")
else()
    if(POLICY_ID)
        message(FATAL_ERROR "POLICY_ID requires POLICY_HEADER_PATH.")
    endif()
    set(EFFECTIVE_POLICY_ID "native_passthrough")
    set(HAS_COMPILED_POLICY_LOWER "true")
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
    "static constexpr const char* HARNESS_GIT_REVISION = \"${HARNESS_GIT_REVISION}\";\n"
    "static constexpr const char* HARNESS_GIT_DIRTY = \"${HARNESS_GIT_DIRTY}\";\n"
    "static constexpr const char* POOL_VERSION = \"${RESOLVED_POOL_VERSION}\";\n"
    "static constexpr const char* POOL_GIT_REVISION = \"${RESOLVED_POOL_GIT_REVISION}\";\n"
    "static constexpr const char* POOL_GIT_DIRTY = \"${RESOLVED_POOL_GIT_DIRTY}\";\n"
    "static constexpr const char* POLICY_ID = \"${EFFECTIVE_POLICY_ID}\";\n"
    "static constexpr const char* POLICY_ABI = \"${POLICY_ABI}\";\n"
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

if(POLICY_HEADER_PATH)
    get_filename_component(POLICY_INCLUDE_DIR "${POLICY_HEADER_PATH}" DIRECTORY)
endif()

function(add_curve_fx_evaluator TARGET_NAME LIBRARY_NAME MODE_DEFINE)
    add_library(${LIBRARY_NAME} STATIC ${CURVE_FX_EVALUATOR_SOURCES})
    target_include_directories(${LIBRARY_NAME} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/cpp/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
        $<INSTALL_INTERFACE:include>
    )
    target_compile_definitions(${LIBRARY_NAME} PUBLIC ${MODE_DEFINE}=1)
    target_link_libraries(${LIBRARY_NAME} PUBLIC
        twocrypto::pool
        Boost::json
        Threads::Threads
    )
    if(CURVE_FX_EDGE_GATE_DIAGNOSTICS)
        target_compile_definitions(${LIBRARY_NAME} PUBLIC
            ARB_EDGE_GATE_DIAGNOSTICS=1
        )
    endif()
    if(POLICY_HEADER_PATH)
        target_include_directories(${LIBRARY_NAME} PRIVATE "${POLICY_INCLUDE_DIR}")
        # Public headers instantiate the selected policy. Consumers must use
        # the same definition or violate the C++ one-definition rule.
        target_compile_definitions(${LIBRARY_NAME} PUBLIC
            TWOCRYPTO_POLICY_HEADER="${POLICY_HEADER_PATH}"
        )
    endif()
    if(CURVE_FX_ENABLE_IPO)
        set_property(TARGET ${LIBRARY_NAME}
            PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
    if(CURVE_FX_NATIVE_TUNING)
        target_compile_options(${LIBRARY_NAME} PUBLIC -march=native)
    endif()

    add_executable(${TARGET_NAME} cpp/src/main.cpp)
    target_compile_definitions(${TARGET_NAME} PRIVATE
        BUILD_TARGET_NAME="${TARGET_NAME}"
        BUILD_TYPE_NAME="$<CONFIG>"
    )
    target_link_libraries(${TARGET_NAME} PRIVATE ${LIBRARY_NAME})
    if(CURVE_FX_ENABLE_IPO)
        set_property(TARGET ${TARGET_NAME}
            PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endfunction()

add_curve_fx_evaluator(
    arb_evaluator_f64 curve_fx_evaluator_f64 ARB_MODE_F64)
add_curve_fx_evaluator(
    arb_evaluator_ld curve_fx_evaluator_ld ARB_MODE_LD)

# Test-only compatibility name. Each executable links exactly one typed core.
add_library(curve_fx_evaluator ALIAS curve_fx_evaluator_ld)


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
    target_include_directories(test_balanced_withdrawal PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/cpp/include"
    )
    target_compile_definitions(test_balanced_withdrawal PRIVATE
        TWOCRYPTO_POLICY_HEADER="${CMAKE_CURRENT_SOURCE_DIR}/cpp/tests/rejecting_fixed_policy.hpp"
    )
    target_link_libraries(test_balanced_withdrawal PRIVATE
        twocrypto::pool
    )
    add_test(NAME test_balanced_withdrawal COMMAND test_balanced_withdrawal)

    # Keep one real protocol gate per arithmetic target.  The existing C++
    # tests intentionally retain the long-double compatibility alias above.
    find_program(CURVE_FX_UV_EXECUTABLE uv)
    if(CURVE_FX_UV_EXECUTABLE)
        function(add_real_evaluator_test TEST_NAME TARGET_NAME EXPECTED_MODE EXPECTED_REAL_TYPE)
            add_test(NAME ${TEST_NAME}
                COMMAND ${CMAKE_COMMAND} -E env
                    "CURVE_FX_EVALUATOR=$<TARGET_FILE:${TARGET_NAME}>"
                    "CURVE_FX_EXPECTED_NUMERIC_MODE=${EXPECTED_MODE}"
                    "CURVE_FX_EXPECTED_REAL_TYPE=${EXPECTED_REAL_TYPE}"
                    "UV_CACHE_DIR=${CMAKE_CURRENT_BINARY_DIR}/.uv-cache"
                    ${CURVE_FX_UV_EXECUTABLE} run
                    --project "${CMAKE_CURRENT_SOURCE_DIR}/python"
                    --frozen --no-sync pytest -q
                    tests/test_numeric_modes.py
            )
            set_tests_properties(${TEST_NAME} PROPERTIES
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/python"
            )
        endfunction()

        add_real_evaluator_test(
            test_real_evaluator_f64 arb_evaluator_f64 double double)
        add_real_evaluator_test(
            test_real_evaluator_ld arb_evaluator_ld longdouble "long double")
    endif()
endif()

# Install rules
include(GNUInstallDirs)
install(TARGETS arb_evaluator_f64 arb_evaluator_ld
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

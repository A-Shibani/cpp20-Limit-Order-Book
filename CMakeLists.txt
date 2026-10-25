
cmake_minimum_required(VERSION 3.20)

project(lob_engine VERSION 0.1.0 LANGUAGES CXX)

option(LOB_BUILD_TESTS "Build the limit order book tests" ON)
# Keep benchmarking optional so ordinary test builds do not need
# to download or compile Google Benchmark.
option(LOB_BUILD_BENCHMARKS "Build throughput benchmarks" OFF)
option(LOB_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

add_library(lob INTERFACE)
target_include_directories(lob INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(lob INTERFACE cxx_std_20)

if(MSVC)
    target_compile_options(lob INTERFACE /W4 /WX /permissive-)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(lob INTERFACE -Wall -Wextra -Werror -Wpedantic)
else()
    message(FATAL_ERROR "Unsupported compiler: ${CMAKE_CXX_COMPILER_ID}")
endif()

if(LOB_ENABLE_SANITIZERS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(lob INTERFACE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
        )
        target_link_options(lob INTERFACE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
        )
    else()
        message(FATAL_ERROR "LOB_ENABLE_SANITIZERS currently requires GCC or Clang")
    endif()
endif()

if(LOB_BUILD_TESTS)
    include(CTest)
    include(FetchContent)

    set(gtest_force_shared_crt ON CACHE BOOL
        "Use the shared MSVC runtime for GoogleTest" FORCE)

    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
        GIT_SHALLOW TRUE
    )

    FetchContent_MakeAvailable(googletest)


    add_executable(
        lob_tests
        tests/types_tests.cpp
        tests/scenarios.cpp
        tests/invariants.cpp
        tests/map_scenarios.cpp
        tests/differential.cpp
        tests/flat_scenarios.cpp
        tests/pool_tests.cpp
        tests/pool_engine_tests.cpp
    )
    target_link_libraries(lob_tests PRIVATE lob GTest::gtest_main)
    set_target_properties(lob_tests PROPERTIES CXX_EXTENSIONS OFF)

    include(GoogleTest)
    gtest_discover_tests(lob_tests)
endif()


if(LOB_BUILD_BENCHMARKS)
    include(FetchContent)

    # We need the benchmark library, not its own test suite.
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL
        "Disable Google Benchmark's tests" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL
        "Disable Google Benchmark's GoogleTest tests" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL
        "Disable Google Benchmark installation targets" FORCE)

    FetchContent_Declare(
        googlebenchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG v1.9.1
        GIT_SHALLOW TRUE
    )

    FetchContent_MakeAvailable(googlebenchmark)

    # Compile the benchmark harness and link it to our engine headers
    # and Google Benchmark.
    add_executable(
        lob_bench
        bench/bench_throughput.cpp
    )

    target_link_libraries(
        lob_bench
        PRIVATE lob benchmark::benchmark
    )

    set_target_properties(
        lob_bench
        PROPERTIES CXX_EXTENSIONS OFF
    )
endif()


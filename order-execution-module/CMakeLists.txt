cmake_minimum_required(VERSION 3.20)
project(order_execution_module LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra -O2)

# --- Dependencies via FetchContent ---
include(FetchContent)

# nlohmann/json
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)

# GoogleTest
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(json googletest)

# --- Main Executable ---
add_executable(order_execution
    src/main.cpp
)
target_include_directories(order_execution PRIVATE src)
target_link_libraries(order_execution PRIVATE
    nlohmann_json::nlohmann_json
    pthread
)

# Copy config to build dir
file(COPY ${CMAKE_SOURCE_DIR}/config DESTINATION ${CMAKE_BINARY_DIR})

# --- Test Executable ---
enable_testing()

add_executable(order_execution_tests
    test/test_order_manager.cpp
    test/test_conflict_resolver.cpp
    test/test_portfolio_guard.cpp
    test/test_arb_coordinator.cpp
)
target_include_directories(order_execution_tests PRIVATE src test)
target_link_libraries(order_execution_tests PRIVATE
    nlohmann_json::nlohmann_json
    GTest::gtest_main
    pthread
)

include(GoogleTest)
gtest_discover_tests(order_execution_tests)

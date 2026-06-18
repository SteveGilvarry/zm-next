cmake_minimum_required(VERSION 3.16)
project(tracker LANGUAGES CXX)

find_package(nlohmann_json REQUIRED)

if(APPLE)
    add_library(tracker SHARED tracker.cpp)
else()
    add_library(tracker MODULE tracker.cpp)
endif()

target_include_directories(tracker PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(tracker PRIVATE
    zmcore
    nlohmann_json::nlohmann_json
)

set_target_properties(tracker PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    OUTPUT_NAME "tracker"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

target_compile_options(tracker PRIVATE "-fvisibility=hidden")

# Unit tests for the pure SORT-style tracking logic (no ABI / EventBus deps).
add_executable(test_tracker_core tests/test_tracker_core.cpp)
target_link_libraries(test_tracker_core PRIVATE GTest::gtest_main)
set_target_properties(test_tracker_core PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME TrackerTest COMMAND $<TARGET_FILE:test_tracker_core>)

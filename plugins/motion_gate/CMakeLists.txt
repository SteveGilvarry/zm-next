cmake_minimum_required(VERSION 3.16)
project(motion_gate LANGUAGES CXX)

find_package(nlohmann_json REQUIRED)

if(APPLE)
    add_library(motion_gate SHARED motion_gate.cpp)
else()
    add_library(motion_gate MODULE motion_gate.cpp)
endif()

target_include_directories(motion_gate PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(motion_gate PRIVATE
    zmcore
    nlohmann_json::nlohmann_json
)

set_target_properties(motion_gate PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    OUTPUT_NAME "motion_gate"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

target_compile_options(motion_gate PRIVATE "-fvisibility=hidden")

# Unit tests for the pure luma-diff helpers (no ABI / deps needed).
add_executable(test_motion_diff tests/test_motion_diff.cpp)
target_link_libraries(test_motion_diff PRIVATE GTest::gtest_main)
set_target_properties(test_motion_diff PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME MotionGateTest COMMAND $<TARGET_FILE:test_motion_diff>)

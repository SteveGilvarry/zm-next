cmake_minimum_required(VERSION 3.16)
project(analytics_rules LANGUAGES CXX)

find_package(nlohmann_json REQUIRED)

if(APPLE)
    add_library(analytics_rules SHARED analytics_rules.cpp)
else()
    add_library(analytics_rules MODULE analytics_rules.cpp)
endif()

target_include_directories(analytics_rules PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(analytics_rules PRIVATE
    zmcore
    nlohmann_json::nlohmann_json
)

set_target_properties(analytics_rules PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    OUTPUT_NAME "analytics_rules"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

target_compile_options(analytics_rules PRIVATE "-fvisibility=hidden")

# Unit tests for the pure geometry / rule-state helpers (no ABI needed).
add_executable(test_geometry tests/test_geometry.cpp)
target_include_directories(test_geometry PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_geometry PRIVATE GTest::gtest_main)
set_target_properties(test_geometry PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME AnalyticsRulesTest COMMAND $<TARGET_FILE:test_geometry>)

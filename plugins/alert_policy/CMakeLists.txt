cmake_minimum_required(VERSION 3.16)
project(alert_policy LANGUAGES CXX)

find_package(nlohmann_json REQUIRED)

if(APPLE)
    add_library(alert_policy SHARED alert_policy.cpp)
else()
    add_library(alert_policy MODULE alert_policy.cpp)
endif()

target_include_directories(alert_policy PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(alert_policy PRIVATE zmcore nlohmann_json::nlohmann_json)

set_target_properties(alert_policy PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    OUTPUT_NAME "alert_policy"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON)

target_compile_options(alert_policy PRIVATE "-fvisibility=hidden")

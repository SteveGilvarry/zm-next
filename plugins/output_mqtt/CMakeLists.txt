cmake_minimum_required(VERSION 3.16)
project(output_mqtt LANGUAGES CXX)

find_package(nlohmann_json REQUIRED)

# Portable libmosquitto discovery (Homebrew / system locations).
find_path(MOSQ_INCLUDE mosquitto.h
    PATHS /opt/homebrew/include /usr/include /usr/local/include REQUIRED)
find_library(MOSQ_LIB NAMES mosquitto
    PATHS /opt/homebrew/lib /usr/lib /usr/local/lib REQUIRED)

if(APPLE)
    add_library(output_mqtt SHARED output_mqtt.cpp)
else()
    add_library(output_mqtt MODULE output_mqtt.cpp)
endif()

target_include_directories(output_mqtt PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MOSQ_INCLUDE}
)

target_link_libraries(output_mqtt PRIVATE
    zmcore
    nlohmann_json::nlohmann_json
    ${MOSQ_LIB}
)

set_target_properties(output_mqtt PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    OUTPUT_NAME "output_mqtt"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

target_compile_options(output_mqtt PRIVATE "-fvisibility=hidden")

# Unit tests for the pure topic-building helper (no ABI / mosquitto needed).
add_executable(test_output_mqtt tests/test_output_mqtt.cpp)
target_include_directories(test_output_mqtt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_output_mqtt PRIVATE GTest::gtest_main)
set_target_properties(test_output_mqtt PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME OutputMqttTest COMMAND $<TARGET_FILE:test_output_mqtt>)

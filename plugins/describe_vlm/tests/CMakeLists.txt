cmake_minimum_required(VERSION 3.16)
project(test_vlm_client LANGUAGES CXX)

find_package(nlohmann_json REQUIRED)

add_executable(test_vlm_client test_vlm_client.cpp)

set_property(TARGET test_vlm_client PROPERTY CXX_STANDARD 17)
set_property(TARGET test_vlm_client PROPERTY CXX_STANDARD_REQUIRED ON)

target_include_directories(test_vlm_client PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(test_vlm_client PRIVATE
    GTest::gtest_main
    nlohmann_json::nlohmann_json
)

add_test(NAME DescribeVlmTest COMMAND $<TARGET_FILE:test_vlm_client>)

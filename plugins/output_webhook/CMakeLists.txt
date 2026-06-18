cmake_minimum_required(VERSION 3.16)
project(output_webhook LANGUAGES CXX)

find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)

if(APPLE)
    add_library(output_webhook SHARED output_webhook.cpp)
else()
    add_library(output_webhook MODULE output_webhook.cpp)
endif()

target_include_directories(output_webhook PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(output_webhook PRIVATE
    zmcore
    nlohmann_json::nlohmann_json
    CURL::libcurl
)

set_target_properties(output_webhook PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    OUTPUT_NAME "output_webhook"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

target_compile_options(output_webhook PRIVATE "-fvisibility=hidden")

# Unit tests for the pure type-filter helper (no ABI / libcurl needed).
add_executable(test_webhook_util tests/test_webhook_util.cpp)
target_include_directories(test_webhook_util PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_webhook_util PRIVATE GTest::gtest_main)
set_target_properties(test_webhook_util PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME OutputWebhookTest COMMAND $<TARGET_FILE:test_webhook_util>)

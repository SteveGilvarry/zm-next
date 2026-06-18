cmake_minimum_required(VERSION 3.16)
project(privacy_mask LANGUAGES CXX)

find_package(nlohmann_json REQUIRED)

if(APPLE)
    add_library(privacy_mask SHARED privacy_mask.cpp)
else()
    add_library(privacy_mask MODULE privacy_mask.cpp)
endif()

target_include_directories(privacy_mask PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(privacy_mask PRIVATE
    zmcore
    nlohmann_json::nlohmann_json
)

set_target_properties(privacy_mask PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    OUTPUT_NAME "privacy_mask"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

target_compile_options(privacy_mask PRIVATE "-fvisibility=hidden")

# Unit tests for the pure geometry / pixel helpers (no ABI / deps needed).
add_executable(test_mask_util tests/test_mask_util.cpp)
target_link_libraries(test_mask_util PRIVATE GTest::gtest_main)
set_target_properties(test_mask_util PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME PrivacyMaskTest COMMAND $<TARGET_FILE:test_mask_util>)

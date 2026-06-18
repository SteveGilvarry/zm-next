cmake_minimum_required(VERSION 3.16)
project(overlay LANGUAGES CXX)

find_package(nlohmann_json REQUIRED)

if(APPLE)
    add_library(overlay SHARED overlay.cpp)
else()
    add_library(overlay MODULE overlay.cpp)
endif()

target_include_directories(overlay PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(overlay PRIVATE
    zmcore
    nlohmann_json::nlohmann_json
)

set_target_properties(overlay PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    OUTPUT_NAME "overlay"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

target_compile_options(overlay PRIVATE "-fvisibility=hidden")

# Unit tests for the pure drawing helpers (no ABI / deps needed).
add_executable(test_draw tests/test_draw.cpp)
target_include_directories(test_draw PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_draw PRIVATE GTest::gtest_main)
set_target_properties(test_draw PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME OverlayTest COMMAND $<TARGET_FILE:test_draw>)

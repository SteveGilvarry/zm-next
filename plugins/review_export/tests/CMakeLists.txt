cmake_minimum_required(VERSION 3.16)

# Unit tests for the pure matte helpers (no ABI / FFmpeg / model needed).
add_executable(test_review_matte test_review_matte.cpp)
target_include_directories(test_review_matte PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_SOURCE_DIR}/plugins/common)
target_link_libraries(test_review_matte PRIVATE GTest::gtest_main)
set_target_properties(test_review_matte PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME ReviewMatteTest COMMAND $<TARGET_FILE:test_review_matte>)

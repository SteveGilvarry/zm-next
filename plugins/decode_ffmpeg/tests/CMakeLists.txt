cmake_minimum_required(VERSION 3.16)
project(test_decode_ffmpeg LANGUAGES CXX)

find_package(Threads REQUIRED)
add_executable(test_decode test_decode.cpp)
target_compile_definitions(test_decode PRIVATE TEST_CMAKE_BINARY_DIR="${CMAKE_BINARY_DIR}")
target_link_libraries(test_decode PRIVATE zmcore GTest::gtest_main Threads::Threads dl)
add_test(NAME DecodeFfmpegTest COMMAND $<TARGET_FILE:test_decode>)

cmake_minimum_required(VERSION 3.16)
project(test_decode_ffmpeg LANGUAGES CXX)

find_package(Threads REQUIRED)
add_executable(test_decode test_decode.cpp)
target_compile_definitions(test_decode PRIVATE
    TEST_CMAKE_BINARY_DIR="${CMAKE_BINARY_DIR}"
    TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
target_link_libraries(test_decode PRIVATE zmcore GTest::gtest_main Threads::Threads)
# test_decode dlopen's the plugin via the dl_compat shim (libdl on POSIX only).
target_include_directories(test_decode PRIVATE ${CMAKE_SOURCE_DIR}/core/include)
if(NOT WIN32)
    target_link_libraries(test_decode PRIVATE ${CMAKE_DL_LIBS})
endif()
add_test(NAME DecodeFfmpegTest COMMAND $<TARGET_FILE:test_decode>)

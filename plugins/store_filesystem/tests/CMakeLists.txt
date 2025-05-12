cmake_minimum_required(VERSION 3.16)
project(test_store LANGUAGES CXX)

add_executable(test_store test_store.cpp)
target_compile_features(test_store PRIVATE cxx_std_20)
target_link_libraries(test_store PRIVATE zmcore gtest_main)
target_compile_definitions(test_store PRIVATE CI_NO_FFMPEG=$ENV{CI_NO_FFMPEG} CMAKE_CURRENT_BINARY_DIR=\"${CMAKE_CURRENT_BINARY_DIR}\")
target_include_directories(test_store PRIVATE ${gtest_SOURCE_DIR}/include)

# WebRTC Output Plugin Tests
cmake_minimum_required(VERSION 3.15)

# Find required packages
find_package(GTest REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# Create test executable
add_executable(test_webrtc test_webrtc.cpp)

# Link libraries
target_link_libraries(test_webrtc 
    PRIVATE 
    output_webrtc
    GTest::gtest 
    GTest::gtest_main
    nlohmann_json::nlohmann_json
)

# Include directories
target_include_directories(test_webrtc 
    PRIVATE 
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# Add to test suite
add_test(
    NAME WebRTCPluginTests
    COMMAND test_webrtc
)

# Set working directory for tests
set_tests_properties(WebRTCPluginTests 
    PROPERTIES 
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

# Add RTSP plugin API test

# Ensure Threads is found before using Threads::Threads
find_package(Threads REQUIRED)

add_executable(test_capture_rtsp test_capture_rtsp.cpp)
# Pass binary directory to tests for plugin lookup
target_compile_definitions(test_capture_rtsp PRIVATE TEST_CMAKE_BINARY_DIR="${CMAKE_BINARY_DIR}")
target_link_libraries(test_capture_rtsp PRIVATE zmcore GTest::gtest_main Threads::Threads dl)




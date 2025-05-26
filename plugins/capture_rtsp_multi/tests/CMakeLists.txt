project(test_capture_rtsp_multi LANGUAGES CXX)

# Create test executable
add_executable(test_capture_rtsp_multi test_capture_rtsp_multi.cpp)

# Set C++ standard
set_property(TARGET test_capture_rtsp_multi PROPERTY CXX_STANDARD 17)
set_property(TARGET test_capture_rtsp_multi PROPERTY CXX_STANDARD_REQUIRED ON)

# Include directories
target_include_directories(test_capture_rtsp_multi PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/plugins/capture_rtsp_multi
)

# Link libraries
target_link_libraries(test_capture_rtsp_multi 
    gtest 
    gtest_main
    ${CMAKE_DL_LIBS}  # For dlopen/dlsym
)

# Add test to CTest
add_test(
    NAME CaptureRtspMultiTest
    COMMAND test_capture_rtsp_multi
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# Make sure the plugin is built before running tests
add_dependencies(test_capture_rtsp_multi capture_rtsp_multi)

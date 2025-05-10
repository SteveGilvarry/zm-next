cmake_minimum_required(VERSION 3.16)

# Test for RTSP plugin
add_executable(test_rtsp_in test_rtsp_in.cpp)

# Find GTest
find_package(GTest REQUIRED)

# Include directories
target_include_directories(test_rtsp_in PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${GTEST_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(test_rtsp_in PRIVATE
    rtsp_in
    ${GTEST_LIBRARIES}
    ${GTEST_MAIN_LIBRARIES}
    pthread
)

# Set C++20 standard for tests
target_compile_features(test_rtsp_in PRIVATE cxx_std_20)

# Add test with environment variable support
add_test(NAME RtspPluginTest COMMAND test_rtsp_in)

# Optional RTSP test server startup
if(RTSP_TEST_SETUP)
    # Try to find ffmpeg to create a test RTSP server
    find_program(FFMPEG_EXECUTABLE ffmpeg)
    if(FFMPEG_EXECUTABLE)
        # Find a sample video file to stream
        file(GLOB SAMPLE_VIDEOS "${CMAKE_SOURCE_DIR}/plugins/capture_rtsp/tests/samples/*.mp4")
        if(SAMPLE_VIDEOS)
            list(GET SAMPLE_VIDEOS 0 SAMPLE_VIDEO)
            
            # Create a custom target to start an RTSP server for testing
            add_custom_target(start_rtsp_server
                COMMAND ${FFMPEG_EXECUTABLE} -re -stream_loop -1 -i "${SAMPLE_VIDEO}" 
                        -c copy -f rtsp -rtsp_transport tcp rtsp://localhost:8554/test
                COMMENT "Starting RTSP test server"
            )
            
            # Set the RTSP_TEST_URL environment variable for the test
            set_tests_properties(RtspPluginTest PROPERTIES
                ENVIRONMENT "RTSP_TEST_URL=rtsp://localhost:8554/test"
            )
        endif()
    endif()
endif()

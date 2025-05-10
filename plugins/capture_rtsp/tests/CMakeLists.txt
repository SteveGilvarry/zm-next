cmake_minimum_required(VERSION 3.16)

# Test for RTSP plugin


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

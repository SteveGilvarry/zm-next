# Add a custom test that runs the local RTSP server and GTest
add_test(
    NAME CaptureRtspWithLocalFfmpeg
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tests
    COMMAND ${CMAKE_COMMAND} -E env SHELL=zsh ./run_gtest_with_local_rtsp.sh
)
# filepath: /Users/stevengilvarry/Code/zm-next/plugins/capture_rtsp/CMakeLists.txt
project(capture_rtsp LANGUAGES CXX)


# Build RTSP capture plugin using exported FFmpeg settings
add_library(capture_rtsp MODULE capture_rtsp.cpp)

target_include_directories(capture_rtsp PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${ZM_FFMPEG_INCLUDES}
)
target_link_directories(capture_rtsp PRIVATE ${ZM_FFMPEG_LIBDIRS})
target_link_libraries(capture_rtsp PRIVATE ${ZM_FFMPEG_LIBS} zmcore)

# Set C++20 standard
target_compile_features(capture_rtsp PRIVATE cxx_std_20)

# Remove 'lib' prefix
set_target_properties(capture_rtsp PROPERTIES PREFIX "")

# macOS rpath for Homebrew FFmpeg dylibs
if(APPLE)
    set_target_properties(capture_rtsp PROPERTIES
        INSTALL_RPATH "@loader_path;/opt/homebrew/lib"
    )
endif()

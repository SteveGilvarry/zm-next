# Add this at the end to build and register plugin tests
add_subdirectory(tests)
# Add a single test that runs the local RTSP server and GTest
add_test(
    NAME CaptureRtspPluginTest
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tests
    COMMAND ${CMAKE_COMMAND} -E env SHELL=zsh ./run_gtest_with_local_rtsp.sh
)
# filepath: /Users/stevengilvarry/Code/zm-next/plugins/capture_rtsp/CMakeLists.txt
project(capture_rtsp LANGUAGES CXX)


# Build RTSP capture plugin using exported FFmpeg settings


# Ensure zm_plugin_init is exported with default visibility
if(APPLE)
    add_library(capture_rtsp SHARED capture_rtsp.cpp)
else()
    add_library(capture_rtsp MODULE capture_rtsp.cpp)
endif()
target_compile_options(capture_rtsp PRIVATE "-fvisibility=hidden")


target_include_directories(capture_rtsp PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${ZM_FFMPEG_INCLUDES}
)
target_link_directories(capture_rtsp PRIVATE ${ZM_FFMPEG_LIBDIRS})
target_link_libraries(capture_rtsp PRIVATE ${ZM_FFMPEG_LIBS} zmcore)

# Set C++20 standard
target_compile_features(capture_rtsp PRIVATE cxx_std_20)


# Remove 'lib' prefix and set visibility for exported symbols
set_target_properties(capture_rtsp PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

# macOS rpath for Homebrew FFmpeg dylibs
if(APPLE)
    set_target_properties(capture_rtsp PROPERTIES
        INSTALL_RPATH "@loader_path;/opt/homebrew/lib"
    )
endif()

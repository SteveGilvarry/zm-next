project(capture_rtsp_multi LANGUAGES CXX)

# Build multi-stream RTSP capture plugin using exported FFmpeg settings

# Ensure zm_plugin_init is exported with default visibility
if(APPLE)
    add_library(capture_rtsp_multi SHARED capture_rtsp_multi.cpp stream_manager.cpp)
else()
    add_library(capture_rtsp_multi MODULE capture_rtsp_multi.cpp stream_manager.cpp)
endif()

# Remove lib prefix to match expected plugin naming convention
set_target_properties(capture_rtsp_multi PROPERTIES PREFIX "")

target_compile_options(capture_rtsp_multi PRIVATE "-fvisibility=hidden")

# Set C++ standard to support modern features used in StreamManager
set_property(TARGET capture_rtsp_multi PROPERTY CXX_STANDARD 17)
set_property(TARGET capture_rtsp_multi PROPERTY CXX_STANDARD_REQUIRED ON)

target_include_directories(capture_rtsp_multi PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${ZM_FFMPEG_INCLUDES}
    ${CMAKE_CURRENT_SOURCE_DIR}  # For stream_manager.hpp
)

target_link_directories(capture_rtsp_multi PRIVATE ${ZM_FFMPEG_LIBDIRS})
target_link_libraries(capture_rtsp_multi PRIVATE ${ZM_FFMPEG_LIBS} zmcore)

# Enable threading support
find_package(Threads REQUIRED)
target_link_libraries(capture_rtsp_multi PRIVATE Threads::Threads)

# Set the output name to match the directory name
set_target_properties(capture_rtsp_multi PROPERTIES OUTPUT_NAME "capture_rtsp_multi")

# Installation rules
install(TARGETS capture_rtsp_multi
    LIBRARY DESTINATION lib/zm/plugins
    RUNTIME DESTINATION lib/zm/plugins
)

# Add tests subdirectory
add_subdirectory(tests)

project(capture_file LANGUAGES CXX)

# File replay INPUT plugin: demuxes a local media file and forwards its
# compressed video packets into the pipeline. A camera-free sibling of
# capture_rtsp_multi for deterministic end-to-end testing.

find_package(nlohmann_json REQUIRED)

# Ensure zm_plugin_init is exported with default visibility.
if(APPLE)
    add_library(capture_file SHARED capture_file.cpp)
else()
    add_library(capture_file MODULE capture_file.cpp)
endif()

# Remove lib prefix to match expected plugin naming convention.
set_target_properties(capture_file PROPERTIES PREFIX "")

target_compile_options(capture_file PRIVATE "-fvisibility=hidden")

set_property(TARGET capture_file PROPERTY CXX_STANDARD 17)
set_property(TARGET capture_file PROPERTY CXX_STANDARD_REQUIRED ON)

target_include_directories(capture_file PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${ZM_FFMPEG_INCLUDES}
)

target_link_directories(capture_file PRIVATE ${ZM_FFMPEG_LIBDIRS})
target_link_libraries(capture_file PRIVATE ${ZM_FFMPEG_LIBS} zmcore nlohmann_json::nlohmann_json)

# Threading support for the replay thread.
find_package(Threads REQUIRED)
target_link_libraries(capture_file PRIVATE Threads::Threads)

set_target_properties(capture_file PROPERTIES OUTPUT_NAME "capture_file")

install(TARGETS capture_file
    LIBRARY DESTINATION lib/zm/plugins
    RUNTIME DESTINATION lib/zm/plugins
)

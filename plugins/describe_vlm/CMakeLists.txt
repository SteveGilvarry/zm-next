cmake_minimum_required(VERSION 3.16)
project(describe_vlm LANGUAGES CXX)

# Dependencies
find_package(nlohmann_json REQUIRED)
find_package(CURL REQUIRED)

# Use FFmpeg settings from parent (cached vars set in the top-level CMakeLists).
include_directories(${ZM_FFMPEG_INCLUDES})
link_directories(${ZM_FFMPEG_LIBDIRS})

# The parent's ZM_FFMPEG_LIBS covers avformat/avcodec/avutil but NOT libswscale,
# which we need for RGB24 -> YUVJ420P before JPEG encoding.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SWSCALE REQUIRED libswscale)

# Build a single plugin with the correct extension for the OS.
if(APPLE)
    add_library(describe_vlm SHARED describe_vlm.cpp)
else()
    add_library(describe_vlm MODULE describe_vlm.cpp)
endif()

target_include_directories(describe_vlm PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_include_directories(describe_vlm PRIVATE ${SWSCALE_INCLUDE_DIRS})
target_link_directories(describe_vlm PRIVATE ${ZM_FFMPEG_LIBDIRS} ${SWSCALE_LIBRARY_DIRS})
target_link_libraries(describe_vlm PRIVATE
    ${ZM_FFMPEG_LIBS}
    ${SWSCALE_LIBRARIES}
    zmcore
    CURL::libcurl
    nlohmann_json::nlohmann_json
)

set_target_properties(describe_vlm PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    OUTPUT_NAME "describe_vlm"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

target_compile_options(describe_vlm PRIVATE "-fvisibility=hidden")

# Unit tests for the pure helpers.
add_subdirectory(tests)

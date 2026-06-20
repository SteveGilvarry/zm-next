cmake_minimum_required(VERSION 3.16)
project(llm_event_review LANGUAGES CXX)

# Dependencies
find_package(nlohmann_json REQUIRED)
find_package(CURL REQUIRED)

# Use FFmpeg settings from parent (cached vars set in the top-level CMakeLists).
include_directories(${ZM_FFMPEG_INCLUDES})
link_directories(${ZM_FFMPEG_LIBDIRS})

# The parent's ZM_FFMPEG_LIBS covers avformat/avcodec/avutil but NOT libswscale,
# which we need for RGB24 -> YUVJ420P (with downscale) before JPEG encoding.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SWSCALE REQUIRED libswscale)

# Build a single plugin with the correct extension for the OS.
if(APPLE)
    add_library(llm_event_review SHARED llm_event_review.cpp)
else()
    add_library(llm_event_review MODULE llm_event_review.cpp)
endif()

target_include_directories(llm_event_review PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_CURRENT_SOURCE_DIR}
    # Reuse describe_vlm's pure HTTP/JSON helpers (vlm_client.hpp) verbatim.
    ${CMAKE_SOURCE_DIR}/plugins/describe_vlm
)

target_include_directories(llm_event_review PRIVATE ${SWSCALE_INCLUDE_DIRS})
target_link_directories(llm_event_review PRIVATE ${ZM_FFMPEG_LIBDIRS} ${SWSCALE_LIBRARY_DIRS})
target_link_libraries(llm_event_review PRIVATE
    ${ZM_FFMPEG_LIBS}
    ${SWSCALE_LIBRARIES}
    zmcore
    CURL::libcurl
    nlohmann_json::nlohmann_json
)

set_target_properties(llm_event_review PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    OUTPUT_NAME "llm_event_review"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

target_compile_options(llm_event_review PRIVATE "-fvisibility=hidden")

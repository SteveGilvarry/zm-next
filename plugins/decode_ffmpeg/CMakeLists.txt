cmake_minimum_required(VERSION 3.16)
project(decode_ffmpeg LANGUAGES CXX)


# Use FFmpeg settings from parent
include_directories(${ZM_FFMPEG_INCLUDES})
link_directories(${ZM_FFMPEG_LIBDIRS})

# Find nlohmann_json (installed via Homebrew)
find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp
    PATHS /opt/homebrew/include /usr/local/include
)
if(NOT NLOHMANN_JSON_INCLUDE_DIR)
    message(FATAL_ERROR "nlohmann/json.hpp not found. Please install nlohmann-json.")
endif()
include_directories(${NLOHMANN_JSON_INCLUDE_DIR})

if(APPLE)
    add_library(decode_ffmpeg SHARED decode_ffmpeg.cpp)
else()
    add_library(decode_ffmpeg MODULE decode_ffmpeg.cpp)
endif()

set_target_properties(decode_ffmpeg PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
target_compile_options(decode_ffmpeg PRIVATE "-fvisibility=hidden")
target_compile_features(decode_ffmpeg PRIVATE cxx_std_20)
target_link_libraries(decode_ffmpeg PRIVATE ${ZM_FFMPEG_LIBS} zmcore swscale)

if(APPLE)
    install(TARGETS decode_ffmpeg DESTINATION ${CMAKE_BINARY_DIR}/plugins/decode_ffmpeg)
else()
    install(TARGETS decode_ffmpeg DESTINATION ${CMAKE_BINARY_DIR}/plugins/decode_ffmpeg)
endif()

add_subdirectory(tests)

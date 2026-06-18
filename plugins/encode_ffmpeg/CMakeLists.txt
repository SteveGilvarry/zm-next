cmake_minimum_required(VERSION 3.16)
project(encode_ffmpeg LANGUAGES CXX)

# Use FFmpeg settings from parent (avcodec / avutil live in ZM_FFMPEG_LIBS)
include_directories(${ZM_FFMPEG_INCLUDES})
link_directories(${ZM_FFMPEG_LIBDIRS})

# libswscale is not in ZM_FFMPEG_LIBS; locate it via pkg-config.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SWSCALE REQUIRED libswscale)
include_directories(${SWSCALE_INCLUDE_DIRS})
link_directories(${SWSCALE_LIBRARY_DIRS})

# nlohmann_json for config parsing.
find_package(nlohmann_json REQUIRED)

if(APPLE)
    add_library(encode_ffmpeg SHARED encode_ffmpeg.cpp)
else()
    add_library(encode_ffmpeg MODULE encode_ffmpeg.cpp)
endif()

set_target_properties(encode_ffmpeg PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
target_compile_options(encode_ffmpeg PRIVATE "-fvisibility=hidden")
target_compile_features(encode_ffmpeg PRIVATE cxx_std_20)
target_link_libraries(encode_ffmpeg PRIVATE
    ${ZM_FFMPEG_LIBS}
    ${SWSCALE_LIBRARIES}
    nlohmann_json::nlohmann_json
    zmcore
)

install(TARGETS encode_ffmpeg DESTINATION ${CMAKE_BINARY_DIR}/plugins/encode_ffmpeg)

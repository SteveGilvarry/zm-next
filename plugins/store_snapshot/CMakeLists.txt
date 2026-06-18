cmake_minimum_required(VERSION 3.16)
project(store_snapshot LANGUAGES CXX)

# Dependencies
find_package(nlohmann_json REQUIRED)

# Use FFmpeg settings from parent (cached vars set in the top-level CMakeLists).
include_directories(${ZM_FFMPEG_INCLUDES})
link_directories(${ZM_FFMPEG_LIBDIRS})

# The parent's ZM_FFMPEG_LIBS covers avcodec/avutil but NOT libswscale, which we
# need for RGB24 -> YUVJ420P before JPEG encoding.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SWSCALE REQUIRED libswscale)

# Build a single plugin with the correct extension for the OS.
if(APPLE)
    add_library(store_snapshot SHARED store_snapshot.cpp)
else()
    add_library(store_snapshot MODULE store_snapshot.cpp)
endif()

target_include_directories(store_snapshot PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_include_directories(store_snapshot PRIVATE ${SWSCALE_INCLUDE_DIRS})
target_link_directories(store_snapshot PRIVATE ${ZM_FFMPEG_LIBDIRS} ${SWSCALE_LIBRARY_DIRS})
target_link_libraries(store_snapshot PRIVATE
    ${ZM_FFMPEG_LIBS}
    ${SWSCALE_LIBRARIES}
    zmcore
    nlohmann_json::nlohmann_json
)

set_target_properties(store_snapshot PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    OUTPUT_NAME "store_snapshot"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

target_compile_options(store_snapshot PRIVATE "-fvisibility=hidden")

# Unit tests for the pure helpers.
add_subdirectory(tests)

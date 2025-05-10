cmake_minimum_required(VERSION 3.16)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
project(zm-next-core LANGUAGES CXX)
# ---------- Require FFmpeg ≥ 7.0 ------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED
    libavformat>=61
    libavcodec>=61
    libavutil>=59
)

# Export for sub-directories
set(ZM_FFMPEG_INCLUDES ${FFMPEG_INCLUDE_DIRS}  CACHE INTERNAL "")
set(ZM_FFMPEG_LIBDIRS  ${FFMPEG_LIBRARY_DIRS}  CACHE INTERNAL "")
set(ZM_FFMPEG_LIBS     ${FFMPEG_LIBRARIES}     CACHE INTERNAL "")

# Include CTest for add_test / enable_testing support
include(CTest)
enable_testing()

# Core library
add_subdirectory(core)

# Plugins
add_subdirectory(plugins/capture_rtsp)
add_subdirectory(plugins/hello)

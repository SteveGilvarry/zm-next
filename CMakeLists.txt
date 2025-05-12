# Main runner for ZM pipelines
add_executable(zm-core src/zm-core.cpp)
target_link_libraries(zm-core PRIVATE zmcore)
target_include_directories(zm-core PRIVATE ${Boost_INCLUDE_DIRS} /opt/homebrew/include)
cmake_minimum_required(VERSION 3.16)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# Option to always enable SIMD
option(ZMP_USE_SIMD "Enable SIMD optimizations (xsimd)" ON)
if(ZMP_USE_SIMD)
  add_definitions(-DZMP_USE_SIMD)
  # Find xsimd headers (Homebrew default and fallback)
  find_path(XSIMD_INCLUDE_DIR xsimd/xsimd.hpp
    PATHS /opt/homebrew/include /usr/local/include
  )
  if(NOT XSIMD_INCLUDE_DIR)
    message(FATAL_ERROR "xsimd/xsimd.hpp not found. Please install xsimd (e.g., brew install xsimd).")
  endif()
  set(ZM_XSIMD_INCLUDES ${XSIMD_INCLUDE_DIR} CACHE INTERNAL "")
endif()

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
add_subdirectory(plugins)

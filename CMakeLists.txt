cmake_minimum_required(VERSION 3.16)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ========================================
# AUTO-DETECT AND CONFIGURE VCPKG
# ========================================
# Check for VCPKG_ROOT environment variable first
if(NOT CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
    message(STATUS "Using vcpkg toolchain from VCPKG_ROOT: ${CMAKE_TOOLCHAIN_FILE}")
endif()

# Fallback: Check common vcpkg installation locations
if(NOT CMAKE_TOOLCHAIN_FILE)
    set(VCPKG_PATHS 
        "$ENV{HOME}/vcpkg"
        "/usr/local/vcpkg"
        "/opt/vcpkg"
        "${CMAKE_SOURCE_DIR}/../vcpkg"
        "${CMAKE_SOURCE_DIR}/vcpkg"
    )
    
    foreach(VCPKG_PATH ${VCPKG_PATHS})
        if(EXISTS "${VCPKG_PATH}/scripts/buildsystems/vcpkg.cmake")
            set(CMAKE_TOOLCHAIN_FILE "${VCPKG_PATH}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
            message(STATUS "Auto-detected vcpkg toolchain at: ${CMAKE_TOOLCHAIN_FILE}")
            break()
        endif()
    endforeach()
endif()

# If vcpkg is found, set it up
if(CMAKE_TOOLCHAIN_FILE AND EXISTS "${CMAKE_TOOLCHAIN_FILE}")
    message(STATUS "vcpkg toolchain configured: ${CMAKE_TOOLCHAIN_FILE}")
else()
    message(WARNING "vcpkg not found. Some dependencies may not be available.")
endif()
project(zm-next-core LANGUAGES CXX)

# ========================================
# SIMD CONFIGURATION
# ========================================
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

# ========================================
# FFMPEG CONFIGURATION
# ========================================
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

# ========================================
# MAIN EXECUTABLE
# ========================================
# Main runner for ZM pipelines
add_executable(zm-core src/zm-core.cpp)
target_link_libraries(zm-core PRIVATE zmcore)
target_include_directories(zm-core PRIVATE ${Boost_INCLUDE_DIRS} /opt/homebrew/include)

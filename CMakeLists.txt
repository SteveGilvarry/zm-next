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
    message(STATUS "vcpkg not found; using system/Homebrew packages (vcpkg is only needed for e.g. a VAAPI FFmpeg on Linux).")
endif()
# One macOS SDK for compiler, linker and find modules; follows xcode-select /
# DEVELOPER_DIR and re-configures when the SDK changes. Must precede project().
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/AppleSdk.cmake)
project(zm-next-core VERSION 0.1.0 LANGUAGES CXX)

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

# ========================================
# OPTIONAL CUDA / NVDEC ZERO-COPY GPU PATH (Linux/NVIDIA)
# ========================================
# OFF by default so macOS / CPU builds are unaffected. Enable on a GPU box with
# -DZM_WITH_CUDA=ON (requires CUDA toolkit incl. NPP, FFmpeg built with --enable-
# cuda/--enable-nvdec, and an onnxruntime built with the CUDA execution provider).
option(ZM_WITH_CUDA "Build NVDEC hw-decode + ONNX Runtime CUDA zero-copy detect path" OFF)
if(ZM_WITH_CUDA)
    enable_language(CUDA)
    find_package(CUDAToolkit REQUIRED)
    add_compile_definitions(ZMP_WITH_CUDA)
    message(STATUS "ZM_WITH_CUDA=ON: building NVDEC + ORT CUDA zero-copy GPU path")
else()
    message(STATUS "ZM_WITH_CUDA=OFF: CPU/software decode + detect (default)")
endif()

# Include CTest for add_test / enable_testing support
include(CTest)
enable_testing()

# Core library (includes the canonical stream socket protocol + WorkerLink)
add_subdirectory(core)

# Plugins
add_subdirectory(plugins)

# ========================================
# MAIN EXECUTABLE
# ========================================
# Main runner for ZM pipelines
find_package(nlohmann_json REQUIRED)
add_executable(zm-core src/zm-core.cpp)
target_link_libraries(zm-core PRIVATE zmcore nlohmann_json::nlohmann_json)

# Facts the worker hello reports. The commit is taken at configure time, so it
# goes stale until CMake re-runs; hardware backends follow the build options.
execute_process(COMMAND git rev-parse --short HEAD WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                OUTPUT_VARIABLE ZM_NEXT_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT ZM_NEXT_COMMIT)
    set(ZM_NEXT_COMMIT "unknown")
endif()
set(ZM_HW_BACKEND_LIST "")
if(ZM_WITH_CUDA)
    list(APPEND ZM_HW_BACKEND_LIST cuda)
endif()
if(APPLE AND ZM_WITH_METAL)
    list(APPEND ZM_HW_BACKEND_LIST metal)
endif()
if(ZM_WITH_VAAPI)
    list(APPEND ZM_HW_BACKEND_LIST vaapi)
endif()
if(ZM_WITH_VULKAN)
    list(APPEND ZM_HW_BACKEND_LIST vulkan)
endif()
if(ZM_WITH_OPENVINO)
    list(APPEND ZM_HW_BACKEND_LIST openvino)
endif()
list(JOIN ZM_HW_BACKEND_LIST "," ZM_HW_BACKENDS)
target_compile_definitions(zm-core PRIVATE
    ZM_NEXT_VERSION="${PROJECT_VERSION}" ZM_NEXT_COMMIT="${ZM_NEXT_COMMIT}" ZM_HW_BACKENDS="${ZM_HW_BACKENDS}")
target_include_directories(zm-core PRIVATE ${Boost_INCLUDE_DIRS} /opt/homebrew/include)

# ========================================
# TOOLS
# ========================================
# wl_dump: live worker-socket probe — connects as a canonical consumer and prints
# every framed message (Hello / Media / Keyframe / Stats / Event / Bye).
add_executable(wl_dump tools/wl_dump.cpp)
target_link_libraries(wl_dump PRIVATE zm_stream_protocol)
target_compile_features(wl_dump PRIVATE cxx_std_20)

# Integration smoke test: drives the built zm-core + plugins through capture_file
# scenarios (decode/record/socket) and asserts end-to-end behavior. Skips itself
# gracefully if ffmpeg is unavailable. Depends on zm-core + wl_dump + plugins.
add_test(NAME IntegrationSmoke
         COMMAND bash ${CMAKE_SOURCE_DIR}/tests/integration/integration_smoke.sh ${CMAKE_BINARY_DIR})
set_tests_properties(IntegrationSmoke PROPERTIES TIMEOUT 120)

# ========================================
# BENCHMARKS (optional)
# ========================================
# Decode/detect CPU-vs-GPU benchmark (bench/). Off by default; turn on with
# -DZM_BUILD_BENCH=ON (needs ONNXRUNTIME_ROOT; CUDA paths need -DZM_WITH_CUDA=ON).
option(ZM_BUILD_BENCH "Build the decode/detect performance benchmark" OFF)
if(ZM_BUILD_BENCH)
    add_subdirectory(bench)
endif()

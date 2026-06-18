cmake_minimum_required(VERSION 3.16)
project(audio_detect LANGUAGES CXX)

# Find required dependencies
find_package(nlohmann_json REQUIRED)

# libavcodec/libavutil come from the cached ZM_FFMPEG_* variables set by the
# parent build; libswresample is discovered separately via pkg-config.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SWRESAMPLE REQUIRED libswresample)

# Locate ONNX Runtime. Default to Homebrew (macOS, CPU/CoreML); on a CUDA box
# pass -DONNXRUNTIME_ROOT=/path/to/onnxruntime-gpu. find_library/find_path keep
# this portable across .dylib/.so and include layouts.
if(NOT ONNXRUNTIME_ROOT)
    execute_process(
        COMMAND brew --prefix onnxruntime
        OUTPUT_VARIABLE ONNXRUNTIME_ROOT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
endif()
if(NOT ONNXRUNTIME_ROOT)
    message(FATAL_ERROR "onnxruntime not found. Install via 'brew install onnxruntime' or pass -DONNXRUNTIME_ROOT=...")
endif()
find_path(ORT_INCLUDE onnxruntime_cxx_api.h
    PATHS ${ONNXRUNTIME_ROOT}/include/onnxruntime ${ONNXRUNTIME_ROOT}/include REQUIRED)
find_library(ORT_LIB NAMES onnxruntime PATHS ${ONNXRUNTIME_ROOT}/lib REQUIRED)

# Build a single plugin with the correct extension for the OS
if(APPLE)
    add_library(audio_detect SHARED audio_detect.cpp)
else()
    add_library(audio_detect MODULE audio_detect.cpp)
endif()

target_include_directories(audio_detect PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${ORT_INCLUDE}
    ${ZM_FFMPEG_INCLUDES}
    ${SWRESAMPLE_INCLUDE_DIRS}
)

target_link_directories(audio_detect PRIVATE
    ${ZM_FFMPEG_LIBDIRS}
)

target_link_libraries(audio_detect PRIVATE
    zmcore
    nlohmann_json::nlohmann_json
    ${ORT_LIB}
    ${ZM_FFMPEG_LIBS}
    ${SWRESAMPLE_LIBRARIES}
)

set_target_properties(audio_detect PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    OUTPUT_NAME "audio_detect"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

target_compile_options(audio_detect PRIVATE "-fvisibility=hidden")

# Unit tests for the pure top-k helper (no ONNX Runtime / FFmpeg needed).
add_executable(test_audio_topk tests/test_audio_topk.cpp)
target_include_directories(test_audio_topk PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_audio_topk PRIVATE GTest::gtest_main)
set_target_properties(test_audio_topk PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
add_test(NAME AudioDetectTest COMMAND $<TARGET_FILE:test_audio_topk>)

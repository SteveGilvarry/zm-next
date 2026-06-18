cmake_minimum_required(VERSION 3.16)
project(detect_seg LANGUAGES CXX)

# Find required dependencies
find_package(nlohmann_json REQUIRED)

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
    add_library(detect_seg SHARED detect_seg.cpp)
else()
    add_library(detect_seg MODULE detect_seg.cpp)
endif()

target_include_directories(detect_seg PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/plugins/detect_onnx
    ${ORT_INCLUDE}
)

target_link_libraries(detect_seg PRIVATE
    zmcore
    nlohmann_json::nlohmann_json
    ${ORT_LIB}
)

set_target_properties(detect_seg PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    OUTPUT_NAME "detect_seg"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

target_compile_options(detect_seg PRIVATE "-fvisibility=hidden")

# Unit tests for the pure post-processing (no ONNX Runtime needed).
add_executable(test_seg_postprocess tests/test_seg_postprocess.cpp)
target_include_directories(test_seg_postprocess PRIVATE
    ${CMAKE_SOURCE_DIR}/plugins/detect_onnx
)
target_link_libraries(test_seg_postprocess PRIVATE GTest::gtest_main)
set_target_properties(test_seg_postprocess PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
add_test(NAME DetectSegTest COMMAND $<TARGET_FILE:test_seg_postprocess>)

cmake_minimum_required(VERSION 3.16)
project(detect_onnx LANGUAGES CXX)

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
    add_library(detect_onnx SHARED detect_onnx.cpp)
else()
    add_library(detect_onnx MODULE detect_onnx.cpp)
endif()

# CUDA zero-copy path (Linux/NVIDIA). ZMP_WITH_CUDA is defined globally by the
# top-level CMake when ZM_WITH_CUDA=ON, reaching both the C++ and CUDA sources.
if(ZM_WITH_CUDA)
    enable_language(CUDA)
    target_sources(detect_onnx PRIVATE detect_cuda.cpp detect_cuda.cu)
    target_link_libraries(detect_onnx PRIVATE CUDA::cudart)
    set_target_properties(detect_onnx PROPERTIES CUDA_STANDARD 17 CUDA_STANDARD_REQUIRED ON)
endif()

target_include_directories(detect_onnx PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${ORT_INCLUDE}
)

target_link_libraries(detect_onnx PRIVATE
    zmcore
    nlohmann_json::nlohmann_json
    ${ORT_LIB}
)

set_target_properties(detect_onnx PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    OUTPUT_NAME "detect_onnx"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

target_compile_options(detect_onnx PRIVATE "-fvisibility=hidden")

# Unit tests for the pure pre/post-processing (no ONNX Runtime needed).
add_executable(test_detect_postprocess tests/test_detect_postprocess.cpp)
target_link_libraries(test_detect_postprocess PRIVATE GTest::gtest_main)
set_target_properties(test_detect_postprocess PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
add_test(NAME DetectOnnxTest COMMAND $<TARGET_FILE:test_detect_postprocess>)

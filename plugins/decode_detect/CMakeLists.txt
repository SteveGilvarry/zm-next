cmake_minimum_required(VERSION 3.16)
project(decode_detect LANGUAGES CXX)

# Fused NVDEC decode + on-device detect. CUDA-only for now (the HwBackend CUDA
# impl + kernels + shared engine are reused from ../detect_onnx).
if(NOT ZM_WITH_CUDA)
    return()
endif()

find_package(nlohmann_json REQUIRED)
find_package(CUDAToolkit REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(DDAV REQUIRED IMPORTED_TARGET libavutil)

set(DET ${CMAKE_SOURCE_DIR}/plugins/detect_onnx)
add_library(decode_detect MODULE
    decode_detect.cpp
    ${DET}/hw_backend.cpp
    ${DET}/hw_backend_cuda.cpp
    ${DET}/detect_cuda.cu
    ${DET}/detect_cuda.cpp
    ${DET}/detect_engine.cpp)

target_include_directories(decode_detect PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${DET}
    ${ONNXRUNTIME_ROOT}/include)
target_link_directories(decode_detect PRIVATE ${ONNXRUNTIME_ROOT}/lib)
target_link_libraries(decode_detect PRIVATE
    zmcore nlohmann_json::nlohmann_json onnxruntime
    CUDA::cudart PkgConfig::DDAV ${CMAKE_DL_LIBS})

set_target_properties(decode_detect PROPERTIES
    PREFIX ""
    OUTPUT_NAME "decode_detect"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CUDA_STANDARD 17
    CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}")

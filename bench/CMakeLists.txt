# Decode/detect benchmarks. Built only when -DZM_BUILD_BENCH=ON.
find_package(PkgConfig REQUIRED)
pkg_check_modules(BFFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale)

# (1) decode + detect-EP + end-to-end CPU-vs-GPU benchmark
add_executable(bench_decode_detect bench_decode_detect.cpp)
target_compile_features(bench_decode_detect PRIVATE cxx_std_17)
target_include_directories(bench_decode_detect PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${ONNXRUNTIME_ROOT}/include)
target_link_directories(bench_decode_detect PRIVATE ${ONNXRUNTIME_ROOT}/lib)
target_link_libraries(bench_decode_detect PRIVATE PkgConfig::BFFMPEG onnxruntime dl)

# (2) motion-gated / ROI-crop cascade prototype (reuses detect_onnx postprocess)
add_executable(bench_roi_cascade bench_roi_cascade.cpp)
target_compile_features(bench_roi_cascade PRIVATE cxx_std_17)
target_include_directories(bench_roi_cascade PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/plugins/detect_onnx
    ${ONNXRUNTIME_ROOT}/include)
target_link_directories(bench_roi_cascade PRIVATE ${ONNXRUNTIME_ROOT}/lib)
target_link_libraries(bench_roi_cascade PRIVATE PkgConfig::BFFMPEG onnxruntime)

if(ZM_WITH_CUDA)
    find_package(CUDAToolkit REQUIRED)
    target_link_libraries(bench_decode_detect PRIVATE CUDA::cudart)
    target_compile_definitions(bench_decode_detect PRIVATE BENCH_WITH_CUDA)
    target_compile_definitions(bench_roi_cascade PRIVATE BENCH_WITH_CUDA)

    # (3) GPU zero-copy ROI cascade — reuses the plugin's detect_cuda kernels
    # (ZMP_WITH_CUDA is defined globally by the top-level CMake when ZM_WITH_CUDA=ON).
    add_executable(bench_gpu_roi
        bench_gpu_roi.cpp
        ${CMAKE_SOURCE_DIR}/plugins/detect_onnx/detect_cuda.cu
        ${CMAKE_SOURCE_DIR}/plugins/detect_onnx/detect_cuda.cpp)
    target_compile_features(bench_gpu_roi PRIVATE cxx_std_17)
    target_include_directories(bench_gpu_roi PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
        ${CMAKE_SOURCE_DIR}/plugins/detect_onnx
        ${ONNXRUNTIME_ROOT}/include)
    target_link_directories(bench_gpu_roi PRIVATE ${ONNXRUNTIME_ROOT}/lib)
    target_link_libraries(bench_gpu_roi PRIVATE PkgConfig::BFFMPEG onnxruntime CUDA::cudart dl)
    target_compile_definitions(bench_gpu_roi PRIVATE BENCH_WITH_CUDA)
    set_target_properties(bench_gpu_roi PROPERTIES CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}")
endif()

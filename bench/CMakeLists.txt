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
endif()

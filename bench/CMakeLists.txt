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

# (3) event dump: drives the REAL decode_ffmpeg -> detect_onnx plugins and writes
# the detection events the detect plugin publishes (the faithful pipeline output).
add_executable(bench_events bench_events.cpp)
target_compile_features(bench_events PRIVATE cxx_std_17)
target_include_directories(bench_events PRIVATE ${CMAKE_SOURCE_DIR}/core/include)
target_link_libraries(bench_events PRIVATE PkgConfig::BFFMPEG dl)

# (4) burned-in overlay: draw the plugin's boxes in C++ and re-encode an MP4 + JPEGs
find_package(nlohmann_json CONFIG REQUIRED)
add_executable(bench_overlay bench_overlay.cpp)
target_compile_features(bench_overlay PRIVATE cxx_std_17)
target_include_directories(bench_overlay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bench_overlay PRIVATE PkgConfig::BFFMPEG nlohmann_json::nlohmann_json)

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

    # (4) shared batched inference engine demo (N producer threads -> 1 session)
    find_package(Threads REQUIRED)
    add_executable(bench_engine bench_engine.cpp
        ${CMAKE_SOURCE_DIR}/plugins/detect_onnx/detect_engine.cpp)
    target_compile_features(bench_engine PRIVATE cxx_std_17)
    target_include_directories(bench_engine PRIVATE
        ${CMAKE_SOURCE_DIR}/plugins/detect_onnx
        ${ONNXRUNTIME_ROOT}/include)
    target_link_directories(bench_engine PRIVATE ${ONNXRUNTIME_ROOT}/lib)
    target_link_libraries(bench_engine PRIVATE onnxruntime CUDA::cudart Threads::Threads)
    target_compile_definitions(bench_engine PRIVATE BENCH_WITH_CUDA)

    # (5) HwBackend demo: drive real NVDEC surfaces through the backend interface
    set(DET ${CMAKE_SOURCE_DIR}/plugins/detect_onnx)
    add_executable(bench_backend bench_backend.cpp
        ${DET}/hw_backend.cpp ${DET}/hw_backend_cuda.cpp ${DET}/detect_cuda.cu ${DET}/detect_cuda.cpp ${DET}/detect_engine.cpp)
    target_compile_features(bench_backend PRIVATE cxx_std_17)
    target_include_directories(bench_backend PRIVATE ${CMAKE_SOURCE_DIR}/core/include ${DET} ${ONNXRUNTIME_ROOT}/include)
    target_link_directories(bench_backend PRIVATE ${ONNXRUNTIME_ROOT}/lib)
    target_link_libraries(bench_backend PRIVATE PkgConfig::BFFMPEG onnxruntime CUDA::cudart Threads::Threads dl)
    set_target_properties(bench_backend PROPERTIES CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}")

    # (6) isolate the letterbox/resample step: GPU kernel vs CPU bilinear
    add_executable(bench_preprocess bench_preprocess.cpp ${DET}/detect_cuda.cu)
    target_compile_features(bench_preprocess PRIVATE cxx_std_17)
    target_include_directories(bench_preprocess PRIVATE ${CMAKE_SOURCE_DIR}/core/include ${DET} ${ONNXRUNTIME_ROOT}/include)
    target_link_libraries(bench_preprocess PRIVATE CUDA::cudart)
    set_target_properties(bench_preprocess PROPERTIES CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}")
endif()

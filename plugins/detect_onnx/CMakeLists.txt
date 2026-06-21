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

# HwBackend factory — ALWAYS compiled. It dispatches make_backend(kind) to the
# per-accelerator TUs below, each guarded by its own macro. With nothing enabled it
# resolves to nullptr so the plugin still links (CPU fallback).
target_sources(detect_onnx PRIVATE hw_backend.cpp)

# CUDA zero-copy path (Linux/NVIDIA). ZMP_WITH_CUDA is defined globally by the
# top-level CMake when ZM_WITH_CUDA=ON, reaching both the C++ and CUDA sources.
if(ZM_WITH_CUDA)
    enable_language(CUDA)
    target_sources(detect_onnx PRIVATE
        hw_backend_cuda.cpp detect_cuda.cpp detect_cuda.cu detect_engine.cpp)
    target_link_libraries(detect_onnx PRIVATE CUDA::cudart)
    set_target_properties(detect_onnx PROPERTIES CUDA_STANDARD 17 CUDA_STANDARD_REQUIRED ON)
endif()

# ── VAAPI path (AMD / Intel iGPU). OFF by default; needs libva + libavfilter. ──────
# Drives VAAPI VPP through the libavfilter scale_vaapi graph (so it only needs the
# FFmpeg headers + libva at link time, not the raw <va/va.h> ABI). The default
# CUDA-only configure never enters this block, so build/ is unaffected.
option(ZM_WITH_VAAPI "Build the VAAPI (AMD/Intel iGPU) HwBackend" OFF)
if(ZM_WITH_VAAPI)
    pkg_check_modules(LIBVA REQUIRED IMPORTED_TARGET libva)
    # avfilter isn't in the core ZM_FFMPEG_LIBS set (avformat/avcodec/avutil); pull it
    # (and its deps) in via pkg-config for the scale_vaapi VPP graph.
    pkg_check_modules(VAAPI_FFMPEG REQUIRED IMPORTED_TARGET
        libavfilter libavutil libavformat libavcodec)
    target_sources(detect_onnx PRIVATE hw_backend_vaapi.cpp)
    target_link_libraries(detect_onnx PRIVATE PkgConfig::LIBVA PkgConfig::VAAPI_FFMPEG)
    target_compile_definitions(detect_onnx PRIVATE ZM_WITH_VAAPI)
    message(STATUS "ZM_WITH_VAAPI=ON: building VAAPI HwBackend (libva + avfilter)")
endif()

# ── OpenVINO path (Intel iGPU / NPU). OFF by default; same wiring shape as VAAPI. ──
# Stub: needs the OpenVINO runtime (find_package(OpenVINO)). Not present on this box,
# so default-OFF keeps the configure clean.
option(ZM_WITH_OPENVINO "Build the OpenVINO (Intel iGPU/NPU) HwBackend" OFF)
if(ZM_WITH_OPENVINO)
    find_package(OpenVINO REQUIRED)
    # OpenVINO decode interop also leans on FFmpeg's hwcontext (VAAPI/QSV surfaces).
    pkg_check_modules(OV_FFMPEG REQUIRED IMPORTED_TARGET libavutil libavformat libavcodec)
    target_sources(detect_onnx PRIVATE hw_backend_openvino.cpp)
    target_link_libraries(detect_onnx PRIVATE openvino::runtime PkgConfig::OV_FFMPEG)
    target_compile_definitions(detect_onnx PRIVATE ZM_WITH_OPENVINO)
    message(STATUS "ZM_WITH_OPENVINO=ON: building OpenVINO HwBackend")
endif()

# ── Vulkan compute path (AMD/Intel iGPU + discrete AMD). OFF by default. ──────────
# The fully-GPU-resident path validated on the RADV iGPU: VAAPI decode -> Vulkan
# dma_buf import -> compute-shader motion + NV12->CHW preprocess -> ncnn-Vulkan YOLO.
# ncnn ships its own bundled Vulkan, so inference is isolated in vulkan_ncnn_infer.cpp
# (the only TU that includes ncnn) while hw_backend_vulkan.cpp uses the system Vulkan.
option(ZM_WITH_VULKAN "Build the Vulkan compute HwBackend (AMD/Intel iGPU + discrete, ncnn-Vulkan)" OFF)
if(ZM_WITH_VULKAN)
    find_package(Vulkan REQUIRED)
    find_package(ncnn CONFIG REQUIRED)
    pkg_check_modules(VK_VA REQUIRED IMPORTED_TARGET libva libva-drm libavutil)
    target_sources(detect_onnx PRIVATE hw_backend_vulkan.cpp vulkan_ncnn_infer.cpp)
    target_link_libraries(detect_onnx PRIVATE Vulkan::Vulkan ncnn PkgConfig::VK_VA)
    target_compile_definitions(detect_onnx PRIVATE ZM_WITH_VULKAN)
    message(STATUS "ZM_WITH_VULKAN=ON: building Vulkan compute HwBackend (ncnn-Vulkan)")
endif()

# ── Metal path (Apple). Objective-C++ .mm — only ever compiled on APPLE so Linux
# never feeds the .mm to the C++ compiler. Gated additionally by ZM_WITH_METAL. ─────
if(APPLE)
    option(ZM_WITH_METAL "Build the Metal/VideoToolbox HwBackend (Apple)" OFF)
    if(ZM_WITH_METAL)
        enable_language(OBJCXX)
        target_sources(detect_onnx PRIVATE hw_backend_metal.mm)
        target_compile_definitions(detect_onnx PRIVATE ZM_WITH_METAL)
        target_link_libraries(detect_onnx PRIVATE
            "-framework Metal" "-framework CoreVideo" "-framework VideoToolbox"
            "-framework CoreMedia" "-framework Foundation" "-framework QuartzCore")
        message(STATUS "ZM_WITH_METAL=ON: building Metal HwBackend (Apple)")
    endif()
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

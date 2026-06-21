cmake_minimum_required(VERSION 3.16)
project(decode_detect LANGUAGES CXX)

# Fused hardware decode + on-device detect in one synchronous stage. Built when a
# GPU/accelerator HwBackend is enabled: CUDA (Linux/NVIDIA) or Metal (Apple). The
# HwBackend impls + postprocess are reused from ../detect_onnx.
if(NOT ZM_WITH_CUDA AND NOT (APPLE AND ZM_WITH_METAL))
    return()
endif()

find_package(nlohmann_json REQUIRED)
find_package(PkgConfig REQUIRED)

# Locate ONNX Runtime (Homebrew on macOS; -DONNXRUNTIME_ROOT on a CUDA box).
if(NOT ONNXRUNTIME_ROOT)
    execute_process(COMMAND brew --prefix onnxruntime
        OUTPUT_VARIABLE ONNXRUNTIME_ROOT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
find_path(DD_ORT_INCLUDE onnxruntime_cxx_api.h
    PATHS ${ONNXRUNTIME_ROOT}/include/onnxruntime ${ONNXRUNTIME_ROOT}/include REQUIRED)
find_library(DD_ORT_LIB NAMES onnxruntime PATHS ${ONNXRUNTIME_ROOT}/lib REQUIRED)

set(DET ${CMAKE_SOURCE_DIR}/plugins/detect_onnx)

# Common sources: the fused stage + the backend factory (dispatches make_backend).
# SHARED on Apple (so the output is decode_detect.dylib, which the loader resolves);
# MODULE elsewhere — matching the other plugins.
if(APPLE)
    add_library(decode_detect SHARED decode_detect.cpp ${DET}/hw_backend.cpp)
else()
    add_library(decode_detect MODULE decode_detect.cpp ${DET}/hw_backend.cpp)
endif()

target_include_directories(decode_detect PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include ${DET} ${DD_ORT_INCLUDE})
target_link_libraries(decode_detect PRIVATE
    zmcore nlohmann_json::nlohmann_json ${DD_ORT_LIB} ${CMAKE_DL_LIBS})

# ── CUDA backend (Linux/NVIDIA) ──────────────────────────────────────────────
if(ZM_WITH_CUDA)
    enable_language(CUDA)
    find_package(CUDAToolkit REQUIRED)
    pkg_check_modules(DDAV REQUIRED IMPORTED_TARGET libavutil)
    target_sources(decode_detect PRIVATE
        ${DET}/hw_backend_cuda.cpp ${DET}/detect_cuda.cu ${DET}/detect_cuda.cpp ${DET}/detect_engine.cpp)
    target_link_libraries(decode_detect PRIVATE CUDA::cudart PkgConfig::DDAV)
    set_target_properties(decode_detect PROPERTIES
        CUDA_STANDARD 17 CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}")
endif()

# ── Metal backend (Apple) ────────────────────────────────────────────────────
if(APPLE AND ZM_WITH_METAL)
    enable_language(OBJCXX)
    target_sources(decode_detect PRIVATE ${DET}/hw_backend_metal.mm)
    set_source_files_properties(${DET}/hw_backend_metal.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_compile_definitions(decode_detect PRIVATE ZM_WITH_METAL)
    # The Metal backend holds the VideoToolbox surface via AVFrame.
    target_include_directories(decode_detect PRIVATE ${ZM_FFMPEG_INCLUDES})
    target_link_directories(decode_detect PRIVATE ${ZM_FFMPEG_LIBDIRS})
    target_link_libraries(decode_detect PRIVATE ${ZM_FFMPEG_LIBS}
        "-framework Metal" "-framework CoreVideo" "-framework VideoToolbox"
        "-framework CoreMedia" "-framework Foundation" "-framework QuartzCore")
endif()

set_target_properties(decode_detect PROPERTIES
    PREFIX ""
    OUTPUT_NAME "decode_detect"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON)

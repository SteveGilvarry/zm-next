# Decode/detect CPU-vs-GPU benchmark. Built only when -DZM_BUILD_BENCH=ON.
find_package(PkgConfig REQUIRED)
pkg_check_modules(BFFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil)

add_executable(bench_decode_detect bench_decode_detect.cpp)
target_compile_features(bench_decode_detect PRIVATE cxx_std_17)
target_include_directories(bench_decode_detect PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${ONNXRUNTIME_ROOT}/include)
target_link_directories(bench_decode_detect PRIVATE ${ONNXRUNTIME_ROOT}/lib)
target_link_libraries(bench_decode_detect PRIVATE PkgConfig::BFFMPEG onnxruntime dl)

if(ZM_WITH_CUDA)
    find_package(CUDAToolkit REQUIRED)
    target_link_libraries(bench_decode_detect PRIVATE CUDA::cudart)
    target_compile_definitions(bench_decode_detect PRIVATE BENCH_WITH_CUDA)
endif()

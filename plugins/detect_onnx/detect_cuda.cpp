// Host side of the CUDA zero-copy inference path: preprocess on-device, run the
// ORT session via IoBinding bound to CUDA memory (no host image readback), decode.
// Compiled only when ZM_WITH_CUDA is enabled. NOT validated on macOS — validate
// on a CUDA box. See docs/GPU_Pipeline.md.

#include "detect_cuda.hpp"

#ifdef ZMP_WITH_CUDA

#include <cuda_runtime.h>
#include <cmath>
#include <stdexcept>

namespace zm::detect {

namespace {
struct DeviceBuffer {
    void* ptr = nullptr;
    explicit DeviceBuffer(size_t bytes) {
        if (cudaMalloc(&ptr, bytes) != cudaSuccess) ptr = nullptr;
    }
    ~DeviceBuffer() { if (ptr) cudaFree(ptr); }
};
}  // namespace

std::vector<Box> cuda_infer_nv12(Ort::Session& session,
                                 const std::string& input_name,
                                 const std::string& output_name,
                                 uint64_t y_ptr, int y_pitch,
                                 uint64_t uv_ptr, int uv_pitch,
                                 int width, int height, int net,
                                 float conf_thr, const std::vector<int>& allow) {
    const Letterbox lb = compute_letterbox(width, height, net);

    // Device input tensor (NCHW float), filled on-device by the fused kernel.
    const size_t n_floats = static_cast<size_t>(3) * net * net;
    DeviceBuffer d_input(n_floats * sizeof(float));
    if (!d_input.ptr) throw std::runtime_error("cudaMalloc(input) failed");

    int kerr = launch_nv12_to_chw(reinterpret_cast<const uint8_t*>(y_ptr), y_pitch,
                                  reinterpret_cast<const uint8_t*>(uv_ptr), uv_pitch,
                                  width, height, lb.scale, lb.pad_x, lb.pad_y, net,
                                  static_cast<float*>(d_input.ptr));
    if (kerr != 0) throw std::runtime_error("nv12_to_chw kernel launch failed");

    // Bind the device buffer directly as the model input — no host copy.
    Ort::MemoryInfo cudaMem("Cuda", OrtArenaAllocator, 0, OrtMemTypeDefault);
    const std::array<int64_t, 4> inShape{1, 3, net, net};
    Ort::Value inTensor = Ort::Value::CreateTensor<float>(
        cudaMem, static_cast<float*>(d_input.ptr), n_floats, inShape.data(), inShape.size());

    Ort::IoBinding binding(session);
    binding.BindInput(input_name.c_str(), inTensor);
    // Let ORT place the (small) output in CPU memory so we can read it directly.
    Ort::MemoryInfo cpuMem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    binding.BindOutput(output_name.c_str(), cpuMem);

    session.Run(Ort::RunOptions{nullptr}, binding);
    cudaDeviceSynchronize();

    auto outputs = binding.GetOutputValues();
    if (outputs.empty()) return {};
    const float* out = outputs[0].GetTensorData<float>();
    const auto shape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
    const int64_t lastDim = shape.empty() ? 0 : shape.back();
    if (lastDim != 6) return {};  // only NMS-free [1,N,6] supported
    int num = 0;
    if (shape.size() == 3)      num = static_cast<int>(shape[1]);
    else if (shape.size() == 2) num = static_cast<int>(shape[0]);

    return decode_nms_free(out, num, lb, conf_thr, allow);
}

}  // namespace zm::detect

#endif  // ZMP_WITH_CUDA

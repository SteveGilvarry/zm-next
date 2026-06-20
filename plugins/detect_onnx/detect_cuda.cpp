// Host side of the CUDA zero-copy inference path: preprocess on-device, run the
// ORT session via IoBinding bound to CUDA memory (no host image readback), decode.
// Compiled only when ZM_WITH_CUDA is enabled. NOT validated on macOS — validate
// on a CUDA box. See docs/GPU_Pipeline.md.

#include "detect_cuda.hpp"

#ifdef ZMP_WITH_CUDA

#include <cuda_runtime.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
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
                                 float conf_thr, const std::vector<int>& allow,
                                 int crop_x, int crop_y, int crop_w, int crop_h) {
    // Default (0s) => whole frame. NV12 chroma is 2x2 subsampled, so keep the
    // crop origin/size even and clamped to the surface.
    if (crop_w <= 0 || crop_h <= 0) { crop_x = 0; crop_y = 0; crop_w = width; crop_h = height; }
    crop_x &= ~1; crop_y &= ~1; crop_w &= ~1; crop_h &= ~1;
    if (crop_x + crop_w > width)  crop_w = (width  - crop_x) & ~1;
    if (crop_y + crop_h > height) crop_h = (height - crop_y) & ~1;
    if (crop_w <= 0 || crop_h <= 0) return {};

    const Letterbox lb = compute_letterbox(crop_w, crop_h, net);

    // Device input tensor (NCHW float), filled on-device by the fused kernel.
    const size_t n_floats = static_cast<size_t>(3) * net * net;
    DeviceBuffer d_input(n_floats * sizeof(float));
    if (!d_input.ptr) throw std::runtime_error("cudaMalloc(input) failed");

    int kerr = launch_nv12_to_chw(reinterpret_cast<const uint8_t*>(y_ptr), y_pitch,
                                  reinterpret_cast<const uint8_t*>(uv_ptr), uv_pitch,
                                  crop_x, crop_y, crop_w, crop_h,
                                  lb.scale, lb.pad_x, lb.pad_y, net,
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

    std::vector<Box> boxes = decode_nms_free(out, num, lb, conf_thr, allow);
    if (crop_x || crop_y)  // map crop-space boxes back to full-surface coordinates
        for (Box& b : boxes) { b.x += crop_x; b.y += crop_y; }
    return boxes;
}

MotionRoi cuda_motion_bbox(uint64_t y_ptr, int y_pitch, int width, int height,
                           std::vector<uint8_t>& prev_grid,
                           int ds, int pix_thr, int min_changed) {
    const int sw = width / ds, sh = height / ds;
    const size_t n = static_cast<size_t>(sw) * sh;
    DeviceBuffer d_grid(n);
    if (!d_grid.ptr) throw std::runtime_error("cudaMalloc(grid) failed");
    if (launch_luma_grid(reinterpret_cast<const uint8_t*>(y_ptr), y_pitch, width, height,
                         ds, sw, sh, static_cast<uint8_t*>(d_grid.ptr)) != 0)
        throw std::runtime_error("luma_grid kernel launch failed");
    std::vector<uint8_t> cur(n);
    cudaMemcpy(cur.data(), d_grid.ptr, n, cudaMemcpyDeviceToHost);  // tiny grid only

    MotionRoi m;
    if (prev_grid.size() == n) {
        int minx = sw, miny = sh, maxx = -1, maxy = -1, cnt = 0;
        for (int j = 0; j < sh; ++j) {
            for (int i = 0; i < sw; ++i) {
                int d = std::abs(static_cast<int>(cur[j * sw + i]) -
                                 static_cast<int>(prev_grid[j * sw + i]));
                if (d > pix_thr) {
                    ++cnt;
                    minx = std::min(minx, i); miny = std::min(miny, j);
                    maxx = std::max(maxx, i); maxy = std::max(maxy, j);
                }
            }
        }
        m.changed = cnt;
        if (cnt >= min_changed && maxx >= minx) {
            m.active = true;
            int x0 = minx * ds, y0 = miny * ds, x1 = (maxx + 1) * ds, y1 = (maxy + 1) * ds;
            const int mx = (x1 - x0) / 5, my = (y1 - y0) / 5;  // 20% margin
            x0 = std::max(0, x0 - mx); y0 = std::max(0, y0 - my);
            x1 = std::min(width, x1 + mx); y1 = std::min(height, y1 + my);
            m.x = x0; m.y = y0; m.w = x1 - x0; m.h = y1 - y0;
        }
    }
    prev_grid.swap(cur);
    return m;
}

}  // namespace zm::detect

#endif  // ZMP_WITH_CUDA

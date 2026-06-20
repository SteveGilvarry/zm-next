// Host side of the CUDA zero-copy inference path: preprocess on-device, run the
// ORT session via IoBinding bound to CUDA memory (no host image readback), decode.
// Compiled only when ZM_WITH_CUDA is enabled. NOT validated on macOS — validate
// on a CUDA box. See docs/GPU_Pipeline.md.

#include "detect_cuda.hpp"

#ifdef ZMP_WITH_CUDA

#include <cuda_runtime.h>
#include <algorithm>
#include <array>
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

std::vector<MotionRoi> cuda_motion_regions(uint64_t y_ptr, int y_pitch, int width, int height,
                                           std::vector<uint8_t>& prev_grid,
                                           int ds, int pix_thr, int min_cells, int max_regions) {
    const int sw = width / ds, sh = height / ds;
    const size_t n = static_cast<size_t>(sw) * sh;
    DeviceBuffer d_grid(n);
    if (!d_grid.ptr) throw std::runtime_error("cudaMalloc(grid) failed");
    if (launch_luma_grid(reinterpret_cast<const uint8_t*>(y_ptr), y_pitch, width, height,
                         ds, sw, sh, static_cast<uint8_t*>(d_grid.ptr)) != 0)
        throw std::runtime_error("luma_grid kernel launch failed");
    std::vector<uint8_t> cur(n);
    cudaMemcpy(cur.data(), d_grid.ptr, n, cudaMemcpyDeviceToHost);

    std::vector<MotionRoi> regions;
    if (prev_grid.size() != n) { prev_grid.swap(cur); return regions; }

    // Binary changed-cell mask (0=unchanged, 1=changed, 2=visited).
    std::vector<uint8_t> mask(n);
    for (size_t k = 0; k < n; ++k)
        mask[k] = (std::abs(static_cast<int>(cur[k]) - static_cast<int>(prev_grid[k])) > pix_thr) ? 1 : 0;
    prev_grid.swap(cur);

    // Connected components (4-connectivity flood fill) -> per-component bbox.
    std::vector<int> stack;
    const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (int j = 0; j < sh; ++j) {
        for (int i = 0; i < sw; ++i) {
            if (mask[j * sw + i] != 1) continue;
            int minx = i, miny = j, maxx = i, maxy = j, cnt = 0;
            stack.clear(); stack.push_back(j * sw + i); mask[j * sw + i] = 2;
            while (!stack.empty()) {
                const int c = stack.back(); stack.pop_back(); ++cnt;
                const int ci = c % sw, cj = c / sw;
                minx = std::min(minx, ci); maxx = std::max(maxx, ci);
                miny = std::min(miny, cj); maxy = std::max(maxy, cj);
                for (auto& d : nb) {
                    const int ni = ci + d[0], nj = cj + d[1];
                    if (ni < 0 || nj < 0 || ni >= sw || nj >= sh) continue;
                    if (mask[nj * sw + ni] == 1) { mask[nj * sw + ni] = 2; stack.push_back(nj * sw + ni); }
                }
            }
            if (cnt < min_cells) continue;
            int x0 = minx * ds, y0 = miny * ds, x1 = (maxx + 1) * ds, y1 = (maxy + 1) * ds;
            const int mx = (x1 - x0) / 5, my = (y1 - y0) / 5;  // 20% margin
            x0 = std::max(0, x0 - mx); y0 = std::max(0, y0 - my);
            x1 = std::min(width, x1 + mx); y1 = std::min(height, y1 + my);
            MotionRoi r; r.active = true; r.changed = cnt;
            r.x = x0; r.y = y0; r.w = x1 - x0; r.h = y1 - y0;
            regions.push_back(r);
        }
    }

    // Merge overlapping boxes so a single object split across components is one ROI.
    auto overlaps = [](const MotionRoi& a, const MotionRoi& b) {
        return !(a.x + a.w <= b.x || b.x + b.w <= a.x || a.y + a.h <= b.y || b.y + b.h <= a.y);
    };
    for (bool merged = true; merged;) {
        merged = false;
        for (size_t i = 0; i < regions.size() && !merged; ++i)
            for (size_t k = i + 1; k < regions.size(); ++k)
                if (overlaps(regions[i], regions[k])) {
                    const int x0 = std::min(regions[i].x, regions[k].x);
                    const int y0 = std::min(regions[i].y, regions[k].y);
                    const int x1 = std::max(regions[i].x + regions[i].w, regions[k].x + regions[k].w);
                    const int y1 = std::max(regions[i].y + regions[i].h, regions[k].y + regions[k].h);
                    regions[i].x = x0; regions[i].y = y0; regions[i].w = x1 - x0; regions[i].h = y1 - y0;
                    regions[i].changed += regions[k].changed;
                    regions.erase(regions.begin() + k); merged = true; break;
                }
    }

    // Cap to the largest max_regions by area.
    if (static_cast<int>(regions.size()) > max_regions) {
        std::sort(regions.begin(), regions.end(), [](const MotionRoi& a, const MotionRoi& b) {
            return static_cast<long>(a.w) * a.h > static_cast<long>(b.w) * b.h;
        });
        regions.resize(max_regions);
    }
    return regions;
}

std::vector<Box> cuda_infer_nv12_batch(Ort::Session& session,
                                       const std::string& input_name,
                                       const std::string& output_name,
                                       uint64_t y_ptr, int y_pitch,
                                       uint64_t uv_ptr, int uv_pitch,
                                       int full_w, int full_h,
                                       const std::vector<MotionRoi>& regions,
                                       int net, float conf_thr,
                                       const std::vector<int>& allow) {
    std::vector<Box> result;
    const int N = static_cast<int>(regions.size());
    if (N == 0) return result;

    const size_t per = static_cast<size_t>(3) * net * net;
    DeviceBuffer d_input(per * N * sizeof(float));
    if (!d_input.ptr) throw std::runtime_error("cudaMalloc(batch) failed");

    std::vector<Letterbox> lbs(N);
    std::vector<std::array<int, 2>> origin(N);
    for (int i = 0; i < N; ++i) {
        int cx = regions[i].x & ~1, cy = regions[i].y & ~1;
        int cw = regions[i].w & ~1, ch = regions[i].h & ~1;
        if (cx + cw > full_w) cw = (full_w - cx) & ~1;
        if (cy + ch > full_h) ch = (full_h - cy) & ~1;
        if (cw <= 0 || ch <= 0) { cx = 0; cy = 0; cw = full_w & ~1; ch = full_h & ~1; }
        lbs[i] = compute_letterbox(cw, ch, net);
        origin[i] = {cx, cy};
        if (launch_nv12_to_chw(reinterpret_cast<const uint8_t*>(y_ptr), y_pitch,
                               reinterpret_cast<const uint8_t*>(uv_ptr), uv_pitch,
                               cx, cy, cw, ch, lbs[i].scale, lbs[i].pad_x, lbs[i].pad_y, net,
                               static_cast<float*>(d_input.ptr) + static_cast<size_t>(i) * per) != 0)
            throw std::runtime_error("batch nv12_to_chw kernel launch failed");
    }

    Ort::MemoryInfo cudaMem("Cuda", OrtArenaAllocator, 0, OrtMemTypeDefault);
    const std::array<int64_t, 4> inShape{N, 3, net, net};
    Ort::Value inTensor = Ort::Value::CreateTensor<float>(
        cudaMem, static_cast<float*>(d_input.ptr), per * N, inShape.data(), inShape.size());
    Ort::IoBinding binding(session);
    binding.BindInput(input_name.c_str(), inTensor);
    binding.BindOutput(output_name.c_str(), Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault));
    session.Run(Ort::RunOptions{nullptr}, binding);
    cudaDeviceSynchronize();

    auto outputs = binding.GetOutputValues();
    if (outputs.empty()) return result;
    const float* out = outputs[0].GetTensorData<float>();
    const auto shape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
    if (shape.size() != 3 || shape.back() != 6) return result;
    const int rows = static_cast<int>(shape[1]);
    for (int i = 0; i < N && i < static_cast<int>(shape[0]); ++i) {
        auto boxes = decode_nms_free(out + static_cast<size_t>(i) * rows * 6, rows, lbs[i], conf_thr, allow);
        for (Box& b : boxes) { b.x += origin[i][0]; b.y += origin[i][1]; result.push_back(b); }
    }
    return result;
}

}  // namespace zm::detect

#endif  // ZMP_WITH_CUDA

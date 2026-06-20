// bench_preprocess — isolate the letterbox/resample step: GPU NV12->CHW kernel on
// a device surface vs CPU bilinear (letterbox_rgb_to_chw) on an RGB frame. Times
// each in isolation (no decode, no inference).
//
//   bench_preprocess [--width 3840] [--height 2160] [--net 640] [--iters 500]

#include "detect_cuda.hpp"          // launch_nv12_to_chw
#include "detect_postprocess.hpp"   // letterbox_rgb_to_chw, compute_letterbox
#include <cuda_runtime.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

using clk = std::chrono::steady_clock;
static double ms_since(clk::time_point t) { return std::chrono::duration<double, std::milli>(clk::now() - t).count(); }

int main(int argc, char** argv) {
    int W = 3840, H = 2160, net = 640, iters = 500;
    for (int i = 1; i < argc; ++i) { std::string a = argv[i]; auto nx = [&]{ return (i + 1 < argc) ? std::atoi(argv[++i]) : 0; };
        if (a == "--width") W = nx(); else if (a == "--height") H = nx(); else if (a == "--net") net = nx(); else if (a == "--iters") iters = nx(); }

    const zm::detect::Letterbox lb = zm::detect::compute_letterbox(W, H, net);

    // ---- GPU: NV12 surface in VRAM ----
    uint8_t *dY = nullptr, *dUV = nullptr; float* dOut = nullptr;
    cudaMalloc(&dY, (size_t)W * H);
    cudaMalloc(&dUV, (size_t)W * (H / 2));
    cudaMalloc(&dOut, (size_t)3 * net * net * sizeof(float));
    cudaMemset(dY, 120, (size_t)W * H); cudaMemset(dUV, 128, (size_t)W * (H / 2));
    for (int i = 0; i < 10; ++i) zm::detect::launch_nv12_to_chw(dY, W, dUV, W, 0, 0, W, H, lb.scale, lb.pad_x, lb.pad_y, net, dOut);
    cudaDeviceSynchronize();
    auto tg = clk::now();
    for (int i = 0; i < iters; ++i) zm::detect::launch_nv12_to_chw(dY, W, dUV, W, 0, 0, W, H, lb.scale, lb.pad_x, lb.pad_y, net, dOut);
    cudaDeviceSynchronize();
    const double gpu_ms = ms_since(tg) / iters;

    // ---- CPU: bilinear on an RGB24 frame ----
    std::vector<uint8_t> rgb((size_t)W * H * 3, 120);
    std::vector<float> out((size_t)3 * net * net);
    zm::detect::letterbox_rgb_to_chw(rgb.data(), lb, out.data());   // warmup
    auto tc = clk::now();
    for (int i = 0; i < iters; ++i) zm::detect::letterbox_rgb_to_chw(rgb.data(), lb, out.data());
    const double cpu_ms = ms_since(tc) / iters;

    printf("preprocess %dx%d -> %d^2 letterbox  (mean of %d)\n", W, H, net, iters);
    printf("  GPU NV12->CHW kernel : %7.3f ms\n", gpu_ms);
    printf("  CPU bilinear (1 core): %7.3f ms\n", cpu_ms);
    printf("  -> GPU is %.1fx faster\n", cpu_ms / gpu_ms);

    cudaFree(dY); cudaFree(dUV); cudaFree(dOut);
    return 0;
}

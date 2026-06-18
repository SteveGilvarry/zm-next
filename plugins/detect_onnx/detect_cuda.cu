// Fused NV12-surface -> letterboxed normalized CHW-float preprocessing kernel.
// Compiled only when ZM_WITH_CUDA is enabled. NOT validated on macOS — validate
// on a CUDA box. See docs/GPU_Pipeline.md.

#include "detect_cuda.hpp"

#ifdef ZMP_WITH_CUDA

#include <cuda_runtime.h>

namespace zm::detect {

namespace {

// One thread per output pixel. Nearest-neighbour sample of the NV12 surface,
// BT.601 (limited range) YUV->RGB, /255, written planar (CHW). Letterbox border
// pixels get 114/255 (matching the CPU letterbox_rgb_to_chw convention).
__global__ void nv12_to_chw_kernel(const uint8_t* __restrict__ y, int y_pitch,
                                   const uint8_t* __restrict__ uv, int uv_pitch,
                                   int src_w, int src_h,
                                   float scale, int pad_x, int pad_y,
                                   int net, float* __restrict__ out) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int yy = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= net || yy >= net) return;

    const int plane = net * net;
    float r = 114.0f / 255.0f, g = 114.0f / 255.0f, b = 114.0f / 255.0f;

    // Map this network-space pixel back to source coordinates.
    const float sx = (x - pad_x + 0.5f) / scale - 0.5f;
    const float sy = (yy - pad_y + 0.5f) / scale - 0.5f;
    if (sx >= 0.0f && sy >= 0.0f && sx < src_w && sy < src_h) {
        int ix = (int)sx; if (ix > src_w - 1) ix = src_w - 1;
        int iy = (int)sy; if (iy > src_h - 1) iy = src_h - 1;

        const float Y = (float)y[iy * y_pitch + ix];
        const int cx = (ix & ~1);          // UV is 2x2 subsampled
        const int cy = iy >> 1;
        const float U = (float)uv[cy * uv_pitch + cx];
        const float V = (float)uv[cy * uv_pitch + cx + 1];

        const float Yf = Y - 16.0f, Uf = U - 128.0f, Vf = V - 128.0f;
        float R = 1.164f * Yf + 1.596f * Vf;
        float G = 1.164f * Yf - 0.392f * Uf - 0.813f * Vf;
        float B = 1.164f * Yf + 2.017f * Uf;
        R = fminf(fmaxf(R, 0.0f), 255.0f);
        G = fminf(fmaxf(G, 0.0f), 255.0f);
        B = fminf(fmaxf(B, 0.0f), 255.0f);
        r = R / 255.0f; g = G / 255.0f; b = B / 255.0f;
    }

    out[0 * plane + yy * net + x] = r;
    out[1 * plane + yy * net + x] = g;
    out[2 * plane + yy * net + x] = b;
}

}  // namespace

int launch_nv12_to_chw(const uint8_t* y_ptr, int y_pitch,
                       const uint8_t* uv_ptr, int uv_pitch,
                       int src_w, int src_h,
                       float scale, int pad_x, int pad_y,
                       int net, float* d_out) {
    const dim3 block(16, 16);
    const dim3 grid((net + block.x - 1) / block.x, (net + block.y - 1) / block.y);
    nv12_to_chw_kernel<<<grid, block>>>(y_ptr, y_pitch, uv_ptr, uv_pitch,
                                        src_w, src_h, scale, pad_x, pad_y, net, d_out);
    return (int)cudaGetLastError();
}

}  // namespace zm::detect

#endif  // ZMP_WITH_CUDA

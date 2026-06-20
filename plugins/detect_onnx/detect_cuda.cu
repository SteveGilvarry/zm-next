// Fused NV12-surface -> letterboxed normalized CHW-float preprocessing kernel.
// Compiled only when ZM_WITH_CUDA is enabled. NOT validated on macOS — validate
// on a CUDA box. See docs/GPU_Pipeline.md.

#include "detect_cuda.hpp"

#ifdef ZMP_WITH_CUDA

#include <cuda_runtime.h>

namespace zm::detect {

namespace {

// One thread per output pixel. Bilinear sample of the NV12 surface (luma at full
// res, chroma at half res), BT.601 (limited range) YUV->RGB, /255, written planar
// (CHW). Letterbox border pixels get 114/255, matching CPU letterbox_rgb_to_chw.
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
        // Bilinear sample luma at full resolution.
        const int x0 = min(max((int)floorf(sx), 0), src_w - 1);
        const int y0 = min(max((int)floorf(sy), 0), src_h - 1);
        const int x1 = min(x0 + 1, src_w - 1);
        const int y1 = min(y0 + 1, src_h - 1);
        const float wx = sx - floorf(sx);
        const float wy = sy - floorf(sy);
        const float y00 = (float)y[y0 * y_pitch + x0];
        const float y01 = (float)y[y0 * y_pitch + x1];
        const float y10 = (float)y[y1 * y_pitch + x0];
        const float y11 = (float)y[y1 * y_pitch + x1];
        const float Y = (y00 * (1.f - wx) + y01 * wx) * (1.f - wy) +
                        (y10 * (1.f - wx) + y11 * wx) * wy;

        // Bilinear sample chroma at half resolution (NV12 UV is 2x2 subsampled,
        // interleaved U,V; chroma grid is src_w/2 x src_h/2).
        const int cw = src_w >> 1, ch = src_h >> 1;
        const float cfx = sx * 0.5f, cfy = sy * 0.5f;
        const int cx0 = min(max((int)floorf(cfx), 0), cw - 1);
        const int cy0 = min(max((int)floorf(cfy), 0), ch - 1);
        const int cx1 = min(cx0 + 1, cw - 1);
        const int cy1 = min(cy0 + 1, ch - 1);
        const float cwx = cfx - floorf(cfx);
        const float cwy = cfy - floorf(cfy);
#define ZM_UV(R, C, O) ((float)uv[(R) * uv_pitch + ((C) << 1) + (O)])
        const float U = (ZM_UV(cy0, cx0, 0) * (1.f - cwx) + ZM_UV(cy0, cx1, 0) * cwx) * (1.f - cwy) +
                        (ZM_UV(cy1, cx0, 0) * (1.f - cwx) + ZM_UV(cy1, cx1, 0) * cwx) * cwy;
        const float V = (ZM_UV(cy0, cx0, 1) * (1.f - cwx) + ZM_UV(cy0, cx1, 1) * cwx) * (1.f - cwy) +
                        (ZM_UV(cy1, cx0, 1) * (1.f - cwx) + ZM_UV(cy1, cx1, 1) * cwx) * cwy;
#undef ZM_UV

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

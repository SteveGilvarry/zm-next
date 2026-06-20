// Fused NV12-surface -> letterboxed normalized CHW-float preprocessing kernel.
// Compiled only when ZM_WITH_CUDA is enabled. NOT validated on macOS — validate
// on a CUDA box. See docs/GPU_Pipeline.md.

#include "detect_cuda.hpp"

#ifdef ZMP_WITH_CUDA

#include <cuda_runtime.h>
#include <cstdlib>

namespace zm::detect {

namespace {

// One thread per output pixel. Bilinear sample of the NV12 surface (luma at full
// res, chroma at half res), BT.601 (limited range) YUV->RGB, /255, written planar
// (CHW). Letterbox border pixels get 114/255, matching CPU letterbox_rgb_to_chw.
__global__ void nv12_to_chw_kernel(const uint8_t* __restrict__ y, int y_pitch,
                                   const uint8_t* __restrict__ uv, int uv_pitch,
                                   int crop_x0, int crop_y0,
                                   int src_w, int src_h,   // region (crop) dims to letterbox
                                   float scale, int pad_x, int pad_y,
                                   int net, float* __restrict__ out) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int yy = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= net || yy >= net) return;

    const int plane = net * net;
    const int cuv_x0 = crop_x0 >> 1, cuv_y0 = crop_y0 >> 1;  // crop origin in chroma grid
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
        const float y00 = (float)y[(crop_y0 + y0) * y_pitch + (crop_x0 + x0)];
        const float y01 = (float)y[(crop_y0 + y0) * y_pitch + (crop_x0 + x1)];
        const float y10 = (float)y[(crop_y0 + y1) * y_pitch + (crop_x0 + x0)];
        const float y11 = (float)y[(crop_y0 + y1) * y_pitch + (crop_x0 + x1)];
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
#define ZM_UV(R, C, O) ((float)uv[(cuv_y0 + (R)) * uv_pitch + ((cuv_x0 + (C)) << 1) + (O)])
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

// Downsample the luma plane onto a small sw x sh grid (one byte per cell) for a
// cheap on-GPU motion diff; only the tiny grid is read back, never the frame.
__global__ void luma_grid_kernel(const uint8_t* __restrict__ y, int y_pitch,
                                 int w, int h, int ds, int sw, int sh,
                                 uint8_t* __restrict__ grid) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    const int j = blockIdx.y * blockDim.y + threadIdx.y;
    if (i >= sw || j >= sh) return;
    int sx = i * ds; if (sx > w - 1) sx = w - 1;
    int sy = j * ds; if (sy > h - 1) sy = h - 1;
    grid[j * sw + i] = y[sy * y_pitch + sx];
}

}  // namespace

// Hardware-bilinear variant: sample the NV12 planes through the GPU texture units
// (cudaFilterModeLinear) instead of computing the 4-tap bilinear by hand. Same
// math, the sampler does the interpolation. Selected by ZM_DETECT_TEX.
//
// A/B (2026-06, RTX 5070 Ti, 4K@640): texture was ~1.6% SLOWER (2.24 vs 2.21
// ms/inf) with identical detections (11889 vs 11888). On per-frame NVDEC surfaces
// the per-call texture-object create + sync + destroy outweighs the sampler, and
// the kernel is bandwidth-bound anyway — so the MANUAL kernel stays the default.
// Kept as an opt-in for other GPUs/workloads; falls back if textures can't be made.
__global__ void nv12_to_chw_tex_kernel(cudaTextureObject_t yTex, cudaTextureObject_t uvTex,
                                       int crop_x0, int crop_y0, int src_w, int src_h,
                                       float scale, int pad_x, int pad_y, int net,
                                       float* __restrict__ out) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int yy = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= net || yy >= net) return;
    const int plane = net * net;
    float r = 114.0f / 255.0f, g = 114.0f / 255.0f, b = 114.0f / 255.0f;
    const float sx = (x - pad_x + 0.5f) / scale - 0.5f;
    const float sy = (yy - pad_y + 0.5f) / scale - 0.5f;
    if (sx >= 0.0f && sy >= 0.0f && sx < src_w && sy < src_h) {
        const float Y = tex2D<float>(yTex, crop_x0 + sx + 0.5f, crop_y0 + sy + 0.5f) * 255.0f;
        const float2 uv = tex2D<float2>(uvTex, crop_x0 * 0.5f + sx * 0.5f + 0.5f,
                                                crop_y0 * 0.5f + sy * 0.5f + 0.5f);
        const float U = uv.x * 255.0f, V = uv.y * 255.0f;
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

static bool tex_enabled() { static const bool e = (std::getenv("ZM_DETECT_TEX") != nullptr); return e; }

// crop_x/crop_y/crop_w/crop_h select the source region to letterbox (origin must
// be even for NV12 chroma alignment); pass the full frame for whole-frame detect.
int launch_nv12_to_chw(const uint8_t* y_ptr, int y_pitch,
                       const uint8_t* uv_ptr, int uv_pitch,
                       int crop_x, int crop_y, int crop_w, int crop_h,
                       float scale, int pad_x, int pad_y,
                       int net, float* d_out) {
    const dim3 block(16, 16);
    const dim3 grid((net + block.x - 1) / block.x, (net + block.y - 1) / block.y);

    if (tex_enabled()) {
        const int yW = crop_x + crop_w, yH = crop_y + crop_h;
        cudaTextureDesc td{};
        td.filterMode = cudaFilterModeLinear; td.readMode = cudaReadModeNormalizedFloat;
        td.addressMode[0] = cudaAddressModeClamp; td.addressMode[1] = cudaAddressModeClamp;
        td.normalizedCoords = 0;
        cudaResourceDesc yr{}; yr.resType = cudaResourceTypePitch2D;
        yr.res.pitch2D.devPtr = const_cast<uint8_t*>(y_ptr); yr.res.pitch2D.pitchInBytes = y_pitch;
        yr.res.pitch2D.width = yW; yr.res.pitch2D.height = yH;
        yr.res.pitch2D.desc = cudaCreateChannelDesc<unsigned char>();
        cudaResourceDesc ur{}; ur.resType = cudaResourceTypePitch2D;
        ur.res.pitch2D.devPtr = const_cast<uint8_t*>(uv_ptr); ur.res.pitch2D.pitchInBytes = uv_pitch;
        ur.res.pitch2D.width = yW / 2; ur.res.pitch2D.height = yH / 2;
        ur.res.pitch2D.desc = cudaCreateChannelDesc<uchar2>();
        cudaTextureObject_t yTex = 0, uvTex = 0;
        if (cudaCreateTextureObject(&yTex, &yr, &td, nullptr) == cudaSuccess &&
            cudaCreateTextureObject(&uvTex, &ur, &td, nullptr) == cudaSuccess) {
            nv12_to_chw_tex_kernel<<<grid, block>>>(yTex, uvTex, crop_x, crop_y, crop_w, crop_h,
                                                    scale, pad_x, pad_y, net, d_out);
            const int err = (int)cudaGetLastError();
            cudaDeviceSynchronize();            // textures must outlive the kernel
            cudaDestroyTextureObject(yTex); cudaDestroyTextureObject(uvTex);
            return err;
        }
        if (yTex) cudaDestroyTextureObject(yTex);
        if (uvTex) cudaDestroyTextureObject(uvTex);
        // creation failed (pitch/alignment) -> fall through to the manual kernel
    }

    nv12_to_chw_kernel<<<grid, block>>>(y_ptr, y_pitch, uv_ptr, uv_pitch,
                                        crop_x, crop_y, crop_w, crop_h,
                                        scale, pad_x, pad_y, net, d_out);
    return (int)cudaGetLastError();
}

int launch_luma_grid(const uint8_t* y_ptr, int y_pitch, int w, int h,
                     int ds, int sw, int sh, uint8_t* d_grid) {
    const dim3 block(16, 16);
    const dim3 grid((sw + block.x - 1) / block.x, (sh + block.y - 1) / block.y);
    luma_grid_kernel<<<grid, block>>>(y_ptr, y_pitch, w, h, ds, sw, sh, d_grid);
    return (int)cudaGetLastError();
}

}  // namespace zm::detect

#endif  // ZMP_WITH_CUDA

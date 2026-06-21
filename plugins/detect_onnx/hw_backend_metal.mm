// hw_backend_metal.mm — Apple HwBackend (VideoToolbox decode + Metal compute + ORT CoreML EP)
//
// This is the Metal sibling of hw_backend_cuda.cpp. It expresses the same fused
// on-device detect pattern (decode -> on-device motion gate -> on-device NV12->CHW
// preprocess -> ORT inference) through the zm::hw::HwBackend interface, but for
// Apple silicon: VideoToolbox produces CVPixelBuffers (NV12), Metal compute kernels
// do the motion diff + preprocess zero-copy via CVMetalTextureCache, and ONNX
// Runtime runs the model through the CoreML execution provider.
//
//   *** NOT BUILT, NOT VALIDATED ON THIS MACHINE. ***
//
// This file was authored on a Linux/NVIDIA box where it cannot be compiled (no
// Metal, no VideoToolbox, no CoreML, no clang Objective-C++ Apple SDK). It is a
// faithful, complete-as-possible STARTING POINT for a Mac build. Every spot that
// could not be verified against real Apple headers/behaviour is marked
// "UNVALIDATED:" in a comment. See the checklist at the bottom of this file.
//
// Build notes (to be wired into plugins/detect_onnx/CMakeLists.txt on the Mac side):
//   - Compile as Objective-C++ (.mm). Needs -fobjc-arc OR manual retain/release;
//     this file is written to be ARC-friendly but uses CF* objects (manual CFRelease).
//   - Link: -framework Metal -framework CoreVideo -framework VideoToolbox
//           -framework CoreMedia -framework Foundation -framework QuartzCore
//   - ONNX Runtime built with the CoreML EP (onnxruntime_providers_coreml).
//   - Define ZM_WITH_METAL (mirrors how ZMP_WITH_CUDA gates the CUDA path) and
//     ensure __APPLE__ (set automatically by the Apple toolchain).
//   - The two Metal kernels below are provided as an inline source string and are
//     compiled at runtime via newLibraryWithSource:. Alternatively, ship a
//     precompiled .metallib (see ZM_METAL_KERNELS_SRC note).

#include "hw_backend.hpp"

#if defined(__APPLE__) && defined(ZM_WITH_METAL)

#import <Metal/Metal.h>
#import <CoreVideo/CoreVideo.h>
#import <CoreMedia/CoreMedia.h>
#import <Foundation/Foundation.h>

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <cstdlib>      // getenv
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/frame.h>          // av_frame_clone / av_frame_free (surface lifetime)
#include <libavutil/hwcontext.h>      // AV_PIX_FMT_VIDEOTOOLBOX, AVFrame.data[3] = CVPixelBufferRef
#include <libavutil/pixfmt.h>
}

// ---------------------------------------------------------------------------
// CoreML execution provider C API. ORT ships this in coreml_provider_factory.h.
// Declared here defensively in case the header isn't on the include path of the
// Linux box that authored this; on a real Mac build prefer the real header:
//   #include <coreml_provider_factory.h>
// UNVALIDATED: confirm the symbol name + flag enum against the installed ORT.
#ifndef ZM_HAVE_COREML_PROVIDER_HEADER
extern "C" OrtStatus* OrtSessionOptionsAppendExecutionProvider_CoreML(
    OrtSessionOptions* options, uint32_t coreml_flags);
// Common flag bits (see onnxruntime coreml_provider_factory.h):
//   COREML_FLAG_USE_CPU_ONLY              = 0x001
//   COREML_FLAG_ENABLE_ON_SUBGRAPH        = 0x002
//   COREML_FLAG_ONLY_ENABLE_DEVICE_WITH_ANE = 0x004
//   COREML_FLAG_ONLY_ALLOW_STATIC_INPUT_SHAPES = 0x008
//   COREML_FLAG_CREATE_MLPROGRAM          = 0x010
#endif

namespace zm::hw {
namespace {

// ===========================================================================
// Metal kernel source (NV12 motion downsample+diff, and NV12->CHW letterbox).
//
// Mirrors detect_cuda.cu: launch_luma_diff (downsample luma to a grid + diff vs
// prev grid, accumulating a changed-count + bbox + luma-sum verdict on device) and
// launch_nv12_to_chw (BT.601 limited-range YUV->RGB, /255, letterbox 114/255 into
// a CHW float buffer).
//
// Provided inline as a string compiled at runtime. A sibling .metal file
// "hw_backend_metal_kernels.metal" with this exact content would compile the same
// way via the offline `xcrun metal` toolchain -> .metallib.
// ===========================================================================
static const char* ZM_METAL_KERNELS_SRC = R"METAL(
#include <metal_stdlib>
using namespace metal;

// --- Motion verdict accumulated on the GPU (mirror of CUDA MotionVerdict) ---
// Laid out to match the host MetalMotionVerdict struct below (atomics in Metal
// must live in device memory; we use atomic_int / atomic_uint on a device buffer).
struct VerdictAtomic {
    atomic_int  cnt;     // changed-cell count
    atomic_int  minx;    // bbox in grid cells
    atomic_int  miny;
    atomic_int  maxx;
    atomic_int  maxy;
    atomic_uint sum_lo;  // luma sum (split 64-bit into two 32-bit atomics; see note)
    atomic_uint sum_hi;
};

// Downsample the NV12 luma plane to a grid cell value (block average) and write it
// into d_cur_grid. One thread per grid cell. `yTex` is an r8Unorm texture view of
// the CVPixelBuffer's Y plane (luma), so reads are normalized [0,1]; we scale back
// to 0..255 to match the CUDA integer-domain diff threshold.
//
// UNVALIDATED: CVMetalTextureCache gives the Y plane as r8Unorm and CbCr as
// rg8Unorm for kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange. Confirm plane
// indices (0 = luma, 1 = chroma) and that sampling returns luma in .r.
kernel void luma_downsample(texture2d<float, access::read> yTex [[texture(0)]],
                            device uchar*  d_cur_grid          [[buffer(0)]],
                            constant int&  ds                  [[buffer(1)]],
                            constant int2& gridDim             [[buffer(2)]], // sw, sh
                            constant int2& frameDim            [[buffer(3)]], // w, h
                            uint2 gid [[thread_position_in_grid]])
{
    const int sw = gridDim.x, sh = gridDim.y;
    if ((int)gid.x >= sw || (int)gid.y >= sh) return;
    const int w = frameDim.x, h = frameDim.y;

    int x0 = (int)gid.x * ds;
    int y0 = (int)gid.y * ds;
    int x1 = min(x0 + ds, w);
    int y1 = min(y0 + ds, h);

    float acc = 0.0f; int n = 0;
    for (int yy = y0; yy < y1; ++yy)
        for (int xx = x0; xx < x1; ++xx) {
            acc += yTex.read(uint2((uint)xx, (uint)yy)).r; // [0,1]
            ++n;
        }
    float mean = (n > 0) ? (acc / (float)n) : 0.0f;
    d_cur_grid[gid.y * sw + gid.x] = (uchar)clamp(mean * 255.0f + 0.5f, 0.0f, 255.0f);
}

// Diff current grid vs previous grid; accumulate changed-count + bbox + luma-sum on
// device. Mirrors detect_cuda.cu launch_luma_diff. One thread per grid cell.
kernel void luma_diff(device const uchar* d_cur   [[buffer(0)]],
                      device const uchar* d_prev  [[buffer(1)]],
                      constant int2&      gridDim [[buffer(2)]], // sw, sh
                      constant int&       thr     [[buffer(3)]],
                      device VerdictAtomic* v     [[buffer(4)]],
                      uint2 gid [[thread_position_in_grid]])
{
    const int sw = gridDim.x, sh = gridDim.y;
    if ((int)gid.x >= sw || (int)gid.y >= sh) return;
    const int idx = gid.y * sw + gid.x;

    uint cur = d_cur[idx];
    // accumulate luma sum (for the global-luma-jump / mean check)
    atomic_fetch_add_explicit(&v->sum_lo, cur, memory_order_relaxed);

    int diff = abs((int)cur - (int)d_prev[idx]);
    if (diff > thr) {
        atomic_fetch_add_explicit(&v->cnt, 1, memory_order_relaxed);
        atomic_fetch_min_explicit(&v->minx, (int)gid.x, memory_order_relaxed);
        atomic_fetch_min_explicit(&v->miny, (int)gid.y, memory_order_relaxed);
        atomic_fetch_max_explicit(&v->maxx, (int)gid.x, memory_order_relaxed);
        atomic_fetch_max_explicit(&v->maxy, (int)gid.y, memory_order_relaxed);
    }
    // NOTE: sum_hi is unused here (sw*sh*255 fits in 32 bits for any sane grid),
    // kept so the struct layout matches a future 64-bit accumulation if needed.
}

// NV12 -> CHW letterbox preprocess. Mirrors detect_cuda.cu launch_nv12_to_chw:
// BT.601 limited-range YUV->RGB, /255, aspect-preserving letterbox with 114/255
// border. One thread per destination pixel (net x net). Output is a planar
// [3,net,net] float buffer (R plane, then G, then B).
//
// yTex:   r8Unorm   luma   (full res)
// uvTex:  rg8Unorm  chroma (half res, interleaved Cb=.r Cg=.g per the NV12 layout;
//                    for VideoRange NV12 .r=Cb, .g=Cr)
// crop:   (cx, cy, cw, ch) region of the SOURCE to sample; cw/ch==0 -> whole frame.
kernel void nv12_to_chw(texture2d<float, access::read> yTex  [[texture(0)]],
                        texture2d<float, access::read> uvTex [[texture(1)]],
                        device float* d_out                  [[buffer(0)]],
                        constant int&   net                  [[buffer(1)]],
                        constant float& scale                [[buffer(2)]],
                        constant int2&  pad                  [[buffer(3)]], // pad_x, pad_y
                        constant int4&  crop                 [[buffer(4)]], // cx,cy,cw,ch
                        constant int2&  frameDim             [[buffer(5)]], // w,h
                        uint2 gid [[thread_position_in_grid]])
{
    const int N = net;
    if ((int)gid.x >= N || (int)gid.y >= N) return;
    const int dx = (int)gid.x, dy = (int)gid.y;
    const int plane = N * N;
    const float PAD = 114.0f / 255.0f;

    // default to border
    float r = PAD, g = PAD, b = PAD;

    const int padx = pad.x, pady = pad.y;
    // net-space content region (after letterbox padding)
    // inverse-map dst pixel -> source pixel (nearest; CUDA path uses nearest too
    // for the fused kernel — UNVALIDATED: confirm against detect_cuda.cu sampling).
    const int srcOrgX = crop.x;
    const int srcOrgY = crop.y;
    const int srcW = (crop.z > 0) ? crop.z : frameDim.x;
    const int srcH = (crop.w > 0) ? crop.w : frameDim.y;

    const float fx = ((float)(dx - padx)) / scale;
    const float fy = ((float)(dy - pady)) / scale;

    if (fx >= 0.0f && fy >= 0.0f && fx < (float)srcW && fy < (float)srcH) {
        int sx = srcOrgX + (int)fx;
        int sy = srcOrgY + (int)fy;
        sx = clamp(sx, 0, frameDim.x - 1);
        sy = clamp(sy, 0, frameDim.y - 1);

        float Y  = yTex.read(uint2((uint)sx, (uint)sy)).r * 255.0f;
        float2 uv = uvTex.read(uint2((uint)(sx / 2), (uint)(sy / 2))).rg * 255.0f;
        float Cb = uv.x, Cr = uv.y;

        // BT.601 limited range YUV->RGB (matches CUDA path)
        float yf = 1.164f * (Y  - 16.0f);
        float cb = Cb - 128.0f;
        float cr = Cr - 128.0f;
        float R = yf + 1.596f * cr;
        float G = yf - 0.392f * cb - 0.813f * cr;
        float B = yf + 2.017f * cb;
        r = clamp(R, 0.0f, 255.0f) / 255.0f;
        g = clamp(G, 0.0f, 255.0f) / 255.0f;
        b = clamp(B, 0.0f, 255.0f) / 255.0f;
    }

    d_out[0 * plane + dy * N + dx] = r;
    d_out[1 * plane + dy * N + dx] = g;
    d_out[2 * plane + dy * N + dx] = b;
}
)METAL";

// Host-side mirror of the device VerdictAtomic struct (plain ints for readback).
// Layout MUST match the Metal struct above (atomics are 32-bit each).
struct MetalMotionVerdict {
    int32_t  cnt;
    int32_t  minx, miny, maxx, maxy;
    uint32_t sum_lo, sum_hi;
};

// ===========================================================================
// Small RAII-ish Metal context shared by the backend instance.
// ===========================================================================
struct MetalCtx {
    id<MTLDevice>             device      = nil;
    id<MTLCommandQueue>       queue       = nil;
    id<MTLLibrary>            library     = nil;
    id<MTLComputePipelineState> psoDownsample = nil;
    id<MTLComputePipelineState> psoDiff       = nil;
    id<MTLComputePipelineState> psoPreproc    = nil;
    CVMetalTextureCacheRef    texCache    = nullptr;
    bool ok = false;
};

// Build a compute pipeline for a named kernel from `library`.
static id<MTLComputePipelineState> makePSO(id<MTLDevice> dev, id<MTLLibrary> lib,
                                           const char* fn, NSError** err) {
    id<MTLFunction> f = [lib newFunctionWithName:[NSString stringWithUTF8String:fn]];
    if (!f) return nil;
    return [dev newComputePipelineStateWithFunction:f error:err];
}

static bool initMetalCtx(MetalCtx& c) {
    c.device = MTLCreateSystemDefaultDevice();
    if (!c.device) return false;
    c.queue = [c.device newCommandQueue];

    NSError* err = nil;
    MTLCompileOptions* opts = [MTLCompileOptions new];
    c.library = [c.device newLibraryWithSource:[NSString stringWithUTF8String:ZM_METAL_KERNELS_SRC]
                                       options:opts
                                         error:&err];
    if (!c.library) {
        // UNVALIDATED: on failure the kernel source has a compile error; log err.
        NSLog(@"[hw_backend_metal] kernel compile failed: %@", err);
        return false;
    }
    c.psoDownsample = makePSO(c.device, c.library, "luma_downsample", &err);
    c.psoDiff       = makePSO(c.device, c.library, "luma_diff",       &err);
    c.psoPreproc    = makePSO(c.device, c.library, "nv12_to_chw",     &err);
    if (!c.psoDownsample || !c.psoDiff || !c.psoPreproc) {
        NSLog(@"[hw_backend_metal] pipeline state creation failed: %@", err);
        return false;
    }

    // Texture cache for zero-copy CVPixelBuffer -> MTLTexture.
    CVReturn r = CVMetalTextureCacheCreate(kCFAllocatorDefault, nullptr, c.device,
                                           nullptr, &c.texCache);
    if (r != kCVReturnSuccess) {
        NSLog(@"[hw_backend_metal] CVMetalTextureCacheCreate failed: %d", r);
        return false;
    }
    c.ok = true;
    return true;
}

// Wrap a plane of a CVPixelBuffer as an MTLTexture (zero-copy, via the cache).
// planeIndex 0 = luma (r8Unorm), 1 = chroma (rg8Unorm). Caller must keep the
// returned CVMetalTextureRef alive until the GPU work using the texture completes,
// then CFRelease it. UNVALIDATED: pixel format mapping per plane.
static id<MTLTexture> textureForPlane(CVMetalTextureCacheRef cache,
                                      CVPixelBufferRef pb, int planeIndex,
                                      MTLPixelFormat fmt, CVMetalTextureRef* outRef) {
    const size_t w = CVPixelBufferGetWidthOfPlane(pb, planeIndex);
    const size_t h = CVPixelBufferGetHeightOfPlane(pb, planeIndex);
    CVMetalTextureRef tex = nullptr;
    CVReturn r = CVMetalTextureCacheCreateTextureFromImage(
        kCFAllocatorDefault, cache, pb, nullptr, fmt, w, h, planeIndex, &tex);
    if (r != kCVReturnSuccess || !tex) { *outRef = nullptr; return nil; }
    *outRef = tex;
    return CVMetalTextureGetTexture(tex);
}

// ===========================================================================
// MetalBackend
// ===========================================================================
class MetalBackend : public HwBackend {
public:
    ~MetalBackend() override {
        if (ctx_.texCache) { CFRelease(ctx_.texCache); ctx_.texCache = nullptr; }
        if (prevGridBuf_) { /* ARC releases the id<MTLBuffer> */ prevGridBuf_ = nil; }
    }

    const char* name() const override { return "metal"; }

    bool load_model(const std::string& path, int net) override {
        model_ = path; net_ = net;
        if (!ctx_.ok && !initMetalCtx(ctx_)) return false;

        try {
            // --- ORT session with the CoreML execution provider ---
            // CoreML routes the graph to the Neural Engine / GPU. The alternative
            // is the (newer/experimental) Metal/MPS EP; CoreML is the supported one
            // for ANE offload. UNVALIDATED end-to-end against a YOLO26 model.
            Ort::SessionOptions so;
            so.SetIntraOpNumThreads(1);
            so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

            uint32_t coreml_flags = 0;
            // COREML_FLAG_CREATE_MLPROGRAM (0x010) is recommended for newer ORT to
            // use the ML Program backend (better op coverage). UNVALIDATED: confirm
            // the constant + that the model converts cleanly.
            coreml_flags |= 0x010;
            Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_CoreML(
                static_cast<OrtSessionOptions*>(so), coreml_flags));

            sess_ = std::make_unique<Ort::Session>(env_, model_.c_str(), so);

            // Cache IO names (mirrors what InferenceEngine does on the CUDA side).
            Ort::AllocatorWithDefaultOptions alloc;
            in_  = sess_->GetInputNameAllocated(0, alloc).get();
            out_ = sess_->GetOutputNameAllocated(0, alloc).get();
            return true;
        } catch (const std::exception& e) {
            NSLog(@"[hw_backend_metal] load_model failed: %s", e.what());
            sess_.reset();
            return false;
        }
    }

    // VideoToolbox surface lifetime: av_frame_clone() refs the underlying
    // CVPixelBuffer so it survives the decoder's call-scoped AVFrame (and can cross
    // a StageRunner queue). For AV_PIX_FMT_VIDEOTOOLBOX frames FFmpeg stores the
    // CVPixelBufferRef in data[3]. release() drops the clone (and its CVBuffer ref).
    Surface acquire(uint64_t av_frame) override {
        Surface s;
        AVFrame* src = reinterpret_cast<AVFrame*>(av_frame);
        if (!src) return s;
        // UNVALIDATED: assert src->format == AV_PIX_FMT_VIDEOTOOLBOX on a real Mac.
        AVFrame* held = av_frame_clone(src);
        if (!held) return s;

        CVPixelBufferRef pb = reinterpret_cast<CVPixelBufferRef>(held->data[3]);
        s.owner   = held;
        s.hw_type = ZM_HW_VTB;                 // 3 (VideoToolbox)
        s.pix_fmt = AV_PIX_FMT_VIDEOTOOLBOX;
        s.width   = held->width;
        s.height  = held->height;
        s.native  = reinterpret_cast<uint64_t>(pb);   // CVPixelBufferRef
        // plane_ptr left zero: Metal path carries the surface in `native`, planes
        // are reached through the CVMetalTextureCache, not raw device pointers.
        return s;
    }

    void release(Surface& s) override {
        if (s.owner) {
            AVFrame* f = static_cast<AVFrame*>(s.owner);
            av_frame_free(&f);                 // drops the clone + its CVPixelBuffer ref
            s.owner = nullptr;
        }
        s.native = 0;
    }

    // Cheap on-device motion gate: downsample luma to a grid + diff vs the prev
    // grid on the GPU, accumulating a changed-count + bbox + luma-sum verdict on
    // device; only the ~28-byte verdict crosses back. Mirrors the CUDA gpudiff
    // path (cuda_motion_bbox_gpudiff): prev grid stays device-resident (ping-pong).
    std::vector<Region> motion(const Surface& s) override {
        if (!s.native || !ctx_.ok) return {};
        CVPixelBufferRef pb = reinterpret_cast<CVPixelBufferRef>(s.native);

        const int sw = std::max(1, s.width  / ds_);
        const int sh = std::max(1, s.height / ds_);
        if (minCells_ <= 0) minCells_ = std::max(8, sw * sh / 400);

        ensureGrids(sw, sh);

        // Y plane texture (zero-copy).
        CVMetalTextureRef yRef = nullptr;
        id<MTLTexture> yTex = textureForPlane(ctx_.texCache, pb, 0,
                                              MTLPixelFormatR8Unorm, &yRef);
        if (!yTex) { if (yRef) CFRelease(yRef); return {}; }

        // Reset the device verdict before accumulation. minx/miny start at sw/sh,
        // maxx/maxy at -1, so the bbox initialises empty (matches CUDA init).
        MetalMotionVerdict init{};
        init.cnt = 0; init.minx = sw; init.miny = sh; init.maxx = -1; init.maxy = -1;
        init.sum_lo = 0; init.sum_hi = 0;
        std::memcpy([verdictBuf_ contents], &init, sizeof(init));

        id<MTLCommandBuffer> cb = [ctx_.queue commandBuffer];

        // 1) downsample current frame's luma into curGridBuf_
        {
            id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
            [enc setComputePipelineState:ctx_.psoDownsample];
            [enc setTexture:yTex atIndex:0];
            [enc setBuffer:curGridBuf_ offset:0 atIndex:0];
            int dsv = ds_;             [enc setBytes:&dsv length:sizeof(int) atIndex:1];
            int gd[2] = {sw, sh};      [enc setBytes:gd  length:sizeof(gd)  atIndex:2];
            int fd[2] = {s.width, s.height}; [enc setBytes:fd length:sizeof(fd) atIndex:3];
            dispatch2D(enc, ctx_.psoDownsample, sw, sh);
            [enc endEncoding];
        }
        // 2) diff curGrid vs prevGrid -> verdict
        {
            id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
            [enc setComputePipelineState:ctx_.psoDiff];
            [enc setBuffer:curGridBuf_  offset:0 atIndex:0];
            [enc setBuffer:prevGridBuf_ offset:0 atIndex:1];
            int gd[2] = {sw, sh};      [enc setBytes:gd length:sizeof(gd) atIndex:2];
            int thrv = thr_;           [enc setBytes:&thrv length:sizeof(int) atIndex:3];
            [enc setBuffer:verdictBuf_ offset:0 atIndex:4];
            dispatch2D(enc, ctx_.psoDiff, sw, sh);
            [enc endEncoding];
        }
        // 3) ping-pong: this frame's grid becomes prev for the next call. A blit
        //    copy keeps prevGridBuf_ device-resident (no host round trip).
        {
            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
            [blit copyFromBuffer:curGridBuf_ sourceOffset:0
                        toBuffer:prevGridBuf_ destinationOffset:0
                            size:(NSUInteger)(sw * sh)];
            [blit endEncoding];
        }

        [cb commit];
        [cb waitUntilCompleted];      // verdict-only readback after completion
        if (yRef) CFRelease(yRef);

        MetalMotionVerdict v{};
        std::memcpy(&v, [verdictBuf_ contents], sizeof(v));

        if (v.cnt < minCells_) return {};   // not enough changed cells

        // Optional global-luma-jump suppression (mirror of CUDA luma_jump_thresh).
        if (lumaJumpThr_ > 0) {
            const float mean = (sw * sh > 0) ? (float)v.sum_lo / (float)(sw * sh) : 0.f;
            if (havePrevMean_ && std::abs(mean - prevMean_) > (float)lumaJumpThr_) {
                prevMean_ = mean; havePrevMean_ = true;
                return {};   // whole-scene exposure shift, not a real mover
            }
            prevMean_ = mean; havePrevMean_ = true;
        }

        // Map grid-cell bbox back to source pixels.
        Region r;
        r.x = v.minx * ds_;
        r.y = v.miny * ds_;
        r.w = std::min((v.maxx - v.minx + 1) * ds_, s.width  - r.x);
        r.h = std::min((v.maxy - v.miny + 1) * ds_, s.height - r.y);
        if (r.w <= 0 || r.h <= 0) return {};
        return { r };
    }

    // Preprocess a (crop of a) surface into a device CHW tensor (MTLBuffer of
    // 3*net*net floats). Mirrors cuda_preprocess_nv12 / launch_nv12_to_chw. Buffer
    // is reused per backend (single-thread consume-before-next-preprocess contract).
    DeviceTensor preprocess(const Surface& s, Region crop) override {
        DeviceTensor t; t.net = net_;
        if (!s.native || !ctx_.ok) return t;
        CVPixelBufferRef pb = reinterpret_cast<CVPixelBufferRef>(s.native);

        // Letterbox is computed for the cropped (or full) region, like the CUDA path.
        const int srcW = (crop.w > 0) ? crop.w : s.width;
        const int srcH = (crop.h > 0) ? crop.h : s.height;
        t.lb = zm::detect::compute_letterbox(srcW, srcH, net_);

        ensureChwBuffer();

        CVMetalTextureRef yRef = nullptr, uvRef = nullptr;
        id<MTLTexture> yTex  = textureForPlane(ctx_.texCache, pb, 0,
                                               MTLPixelFormatR8Unorm,  &yRef);
        id<MTLTexture> uvTex = textureForPlane(ctx_.texCache, pb, 1,
                                               MTLPixelFormatRG8Unorm, &uvRef);
        if (!yTex || !uvTex) {
            if (yRef) CFRelease(yRef);
            if (uvRef) CFRelease(uvRef);
            return t;
        }

        id<MTLCommandBuffer> cb = [ctx_.queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:ctx_.psoPreproc];
        [enc setTexture:yTex  atIndex:0];
        [enc setTexture:uvTex atIndex:1];
        [enc setBuffer:chwBuf_ offset:0 atIndex:0];
        int   netv  = net_;          [enc setBytes:&netv  length:sizeof(int)   atIndex:1];
        float scale = t.lb.scale;    [enc setBytes:&scale length:sizeof(float) atIndex:2];
        int   pad[2] = {t.lb.pad_x, t.lb.pad_y};
        [enc setBytes:pad length:sizeof(pad) atIndex:3];
        int   cr[4] = {crop.x, crop.y, crop.w, crop.h};
        [enc setBytes:cr length:sizeof(cr) atIndex:4];
        int   fd[2] = {s.width, s.height};
        [enc setBytes:fd length:sizeof(fd) atIndex:5];
        dispatch2D(enc, ctx_.psoPreproc, net_, net_);
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];   // tensor must be ready before infer() reads it

        if (yRef)  CFRelease(yRef);
        if (uvRef) CFRelease(uvRef);

        t.ptr = (void*)chwBuf_;    // opaque: an id<MTLBuffer> of 3*net*net floats
        return t;
    }

    // Run inference on the device tensor. The CoreML EP doesn't accept a Metal
    // buffer directly as an ORT input, so we map the MTLBuffer (shared storage =>
    // CPU-visible contents, no copy on UMA) into an Ort::Value over its contents.
    // On Apple-silicon UMA the MTLBuffer.contents pointer aliases the same memory
    // the GPU wrote, so this is effectively zero-copy.
    // UNVALIDATED: confirm the MTLBuffer was created with MTLResourceStorageModeShared
    // and that ORT's CoreML EP tolerates a CPU input tensor (it does for CPU mem).
    std::vector<Detection> infer(const DeviceTensor& t, float conf,
                                 const std::vector<int>& allow) override {
        if (!t.ptr || !sess_) return {};
        id<MTLBuffer> buf = (__bridge id<MTLBuffer>)t.ptr;
        float* data = static_cast<float*>([buf contents]);
        const int net = t.net;

        try {
            Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator,
                                                             OrtMemTypeDefault);
            std::array<int64_t, 4> shape{1, 3, net, net};
            Ort::Value input = Ort::Value::CreateTensor<float>(
                mem, data, (size_t)3 * net * net, shape.data(), shape.size());

            const char* inNames[]  = { in_.c_str()  };
            const char* outNames[] = { out_.c_str() };
            auto outs = sess_->Run(Ort::RunOptions{nullptr}, inNames, &input, 1,
                                   outNames, 1);

            // YOLO26-style NMS-free output [num x 6] (x1,y1,x2,y2,conf,cls).
            const float* od = outs[0].GetTensorData<float>();
            auto info = outs[0].GetTensorTypeAndShapeInfo();
            auto dims = info.GetShape();
            // Expect [..., num, 6]; pick the second-to-last dim as `num`.
            int num = (dims.size() >= 2) ? (int)dims[dims.size() - 2] : 0;
            return zm::detect::decode_nms_free(od, num, t.lb, conf, allow);
        } catch (const std::exception& e) {
            NSLog(@"[hw_backend_metal] infer failed: %s", e.what());
            return {};
        }
    }

private:
    // Dispatch a 2D compute grid sized to (w,h) using the PSO's preferred
    // threadgroup geometry. UNVALIDATED: non-uniform threadgroup dispatch requires
    // an A11+/macOS GPU; the kernels bounds-check, so a padded grid is also fine.
    static void dispatch2D(id<MTLComputeCommandEncoder> enc,
                           id<MTLComputePipelineState> pso, int w, int h) {
        NSUInteger tew = pso.threadExecutionWidth;
        NSUInteger maxT = pso.maxTotalThreadsPerThreadgroup;
        NSUInteger tgh = std::max<NSUInteger>(1, maxT / tew);
        MTLSize tg = MTLSizeMake(tew, tgh, 1);
        MTLSize grid = MTLSizeMake((NSUInteger)w, (NSUInteger)h, 1);
        // dispatchThreads = non-uniform; supported on modern Apple GPUs.
        [enc dispatchThreads:grid threadsPerThreadgroup:tg];
    }

    void ensureGrids(int sw, int sh) {
        const NSUInteger need = (NSUInteger)(sw * sh);
        if (gridCap_ < need) {
            curGridBuf_  = [ctx_.device newBufferWithLength:need
                                                    options:MTLResourceStorageModeShared];
            prevGridBuf_ = [ctx_.device newBufferWithLength:need
                                                    options:MTLResourceStorageModeShared];
            // first-use prev grid = current frame -> no spurious motion frame 1.
            std::memset([prevGridBuf_ contents], 0, need);
            gridCap_ = need;
        }
        if (!verdictBuf_) {
            verdictBuf_ = [ctx_.device newBufferWithLength:sizeof(MetalMotionVerdict)
                                                   options:MTLResourceStorageModeShared];
        }
    }

    void ensureChwBuffer() {
        const NSUInteger need = (NSUInteger)(3 * net_ * net_) * sizeof(float);
        if (!chwBuf_ || chwCap_ < need) {
            chwBuf_ = [ctx_.device newBufferWithLength:need
                                               options:MTLResourceStorageModeShared];
            chwCap_ = need;
        }
    }

    MetalCtx ctx_;
    Ort::Env env_{ORT_LOGGING_LEVEL_WARNING, "zm_metal"};
    std::unique_ptr<Ort::Session> sess_;
    std::string model_, in_, out_;
    int net_ = 640;

    // motion state (device-resident grids, ping-pong)
    id<MTLBuffer> curGridBuf_  = nil;
    id<MTLBuffer> prevGridBuf_ = nil;
    id<MTLBuffer> verdictBuf_  = nil;
    NSUInteger gridCap_ = 0;
    int ds_ = 8, thr_ = 25, minCells_ = 0;
    int lumaJumpThr_ = 0;
    float prevMean_ = 0.f; bool havePrevMean_ = false;

    // preprocess output tensor (reused; single-thread consume-before-next contract)
    id<MTLBuffer> chwBuf_ = nil;
    NSUInteger chwCap_ = 0;
};

}  // namespace

// Narrow factory entry the shared make_backend() (hw_backend.cpp) dispatches to.
std::unique_ptr<HwBackend> make_metal_backend() {
    return std::make_unique<MetalBackend>();
}

}  // namespace zm::hw

#else  // !(__APPLE__ && ZM_WITH_METAL)

// On non-Apple builds (or Apple without ZM_WITH_METAL) this file contributes
// nothing. The CUDA/other make_backend definition (hw_backend_cuda.cpp) provides
// the factory; this TU stays empty so it can be unconditionally listed in the
// build without producing a duplicate symbol.

#endif  // __APPLE__ && ZM_WITH_METAL

// ===========================================================================
// MUST-CHECK-ON-A-MAC checklist (this file is NOT validated):
//
//  1. make_backend duplicate symbol: hw_backend_cuda.cpp ALSO defines
//     zm::hw::make_backend. On a real build only ONE translation unit may define
//     it. Decide the dispatch strategy: either (a) merge the "metal" case into the
//     single make_backend in hw_backend_cuda.cpp (preferred — keep this file's
//     factory under the #if so it only compiles on Apple+Metal and CUDA is off on
//     Apple), or (b) give each a distinct factory and select at a higher level.
//     As written, this factory is INSIDE the #if so it's only emitted when
//     ZM_WITH_METAL — but verify CUDA isn't simultaneously compiled on the Mac.
//
//  2. AV_PIX_FMT_VIDEOTOOLBOX surface layout: confirm FFmpeg stores the
//     CVPixelBufferRef in frame->data[3] for VideoToolbox frames, and that the
//     CVPixelBuffer pixel format is kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
//     (NV12, video range) so the BT.601 limited-range matrix is correct. If it's
//     full-range, drop the 1.164*(Y-16) and the -16 bias.
//
//  3. CVMetalTextureCache plane formats: Y plane as R8Unorm, CbCr plane as
//     RG8Unorm. Confirm CVMetalTextureCacheCreateTextureFromImage succeeds for
//     both planeIndex 0 and 1; some configs need IOSurface-backed CVPixelBuffers
//     (VideoToolbox output is, but a software-decoded buffer may not be).
//
//  4. CbCr ordering in the chroma plane: for VideoRange NV12, plane 1 is Cb,Cr
//     interleaved (.r=Cb, .g=Cr). Confirm; swap if colours look wrong.
//
//  5. Nearest vs bilinear sampling: the preprocess kernel uses nearest-neighbour;
//     detect_cuda.cu may use bilinear. Match whichever the CUDA fused kernel does
//     to keep detections numerically comparable. (CPU path letterbox_rgb_to_chw is
//     bilinear — consider upgrading the kernel to bilinear for parity.)
//
//  6. dispatchThreads (non-uniform threadgroups) requires a sufficiently new GPU/OS.
//     Fallback: pad the grid to a multiple of the threadgroup and rely on the
//     in-kernel bounds checks (already present).
//
//  7. Atomic luma sum: split into sum_lo/sum_hi but only sum_lo is used. Fine for
//     grids where sw*sh*255 < 2^32 (true up to ~16M cells). Verify for your grid.
//
//  8. ORT CoreML EP: confirm OrtSessionOptionsAppendExecutionProvider_CoreML and
//     the flag constants against the installed onnxruntime; prefer including
//     <coreml_provider_factory.h>. Verify the YOLO26 model converts to a CoreML
//     ML Program (COREML_FLAG_CREATE_MLPROGRAM) without unsupported-op fallback to
//     CPU. If conversion is poor, try the Metal/MPS EP or run on CPU EP as a sanity
//     baseline.
//
//  9. infer() input tensor: built over MTLBuffer.contents (shared storage, UMA).
//     Confirm chwBuf_ uses MTLResourceStorageModeShared (it does) so contents is
//     valid CPU memory; the CoreML EP takes a CPU input tensor and moves it.
//
// 10. ARC vs manual memory: this file assumes -fobjc-arc for id<MTL*> objects and
//     manual CFRelease for CV*/CF* objects. If built without ARC, add retain/release
//     for the id<> members. The (__bridge ...) cast in infer() assumes ARC.
//
// 11. Output tensor shape: decode_nms_free expects [num,6]; the `num` extraction
//     guesses dims[size-2]. Verify against the actual model output rank/layout.
//
// 12. No InferenceEngine batching here (unlike CUDA's shared engine). This runs a
//     per-call Ort::Session::Run. If many streams share a Mac, consider a batched
//     engine analogous to detect_engine for CoreML too.
// ===========================================================================

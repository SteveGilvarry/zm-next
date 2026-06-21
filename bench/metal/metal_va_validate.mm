// metal_va_validate.mm — Apple-silicon GPU detection-path validation, milestone
// by milestone, mirroring bench/vk/vk_va_{motion,gate,preproc}.cpp (the proven
// AMD/Vulkan prototypes). Each stage validates against a CPU reference.
//
//   M1  VideoToolbox decode -> CVPixelBuffer (confirm NV12 + range)
//   M2  CVPixelBuffer -> MTLTexture import + GPU luma downsample vs CPU
//   M3  Metal motion gate verdict vs CPU diff over N frames
//   M4  Metal NV12 -> CHW preprocess, dump to PPM for a visual aspect/colour check
//
// Build (standalone, like the vk prototypes):
//   clang++ -std=c++17 -fobjc-arc -O2 metal_va_validate.mm -o metal_va_validate \
//     -framework Metal -framework CoreVideo -framework CoreMedia -framework Foundation \
//     $(pkg-config --cflags --libs libavformat libavcodec libavutil)
//
// Usage: metal_va_validate <clip> [frame=30] [frames=120]

#import <Metal/Metal.h>
#import <CoreVideo/CoreVideo.h>
#import <CoreMedia/CoreMedia.h>
#import <Foundation/Foundation.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

#include "detect_postprocess.hpp"   // zm::detect::compute_letterbox (the real convention)

// ---------------------------------------------------------------------------
// FFmpeg VideoToolbox decode helper
// ---------------------------------------------------------------------------
static enum AVPixelFormat get_vtb(AVCodecContext*, const enum AVPixelFormat* f) {
    for (; *f != AV_PIX_FMT_NONE; f++)
        if (*f == AV_PIX_FMT_VIDEOTOOLBOX) return *f;
    return AV_PIX_FMT_NONE;
}

struct Decoder {
    AVFormatContext* fmt = nullptr;
    AVCodecContext*  dc  = nullptr;
    AVBufferRef*     hw  = nullptr;
    AVPacket*        pkt = nullptr;
    AVFrame*         fr  = nullptr;
    int vs = -1;

    bool open(const char* clip) {
        if (avformat_open_input(&fmt, clip, nullptr, nullptr) < 0) { fprintf(stderr, "open %s\n", clip); return false; }
        avformat_find_stream_info(fmt, nullptr);
        for (unsigned i = 0; i < fmt->nb_streams; i++)
            if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { vs = (int)i; break; }
        if (vs < 0) { fprintf(stderr, "no video stream\n"); return false; }
        const AVCodec* dec = avcodec_find_decoder(fmt->streams[vs]->codecpar->codec_id);
        if (av_hwdevice_ctx_create(&hw, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr, nullptr, 0) < 0) {
            fprintf(stderr, "videotoolbox hwdevice create failed\n"); return false; }
        dc = avcodec_alloc_context3(dec);
        avcodec_parameters_to_context(dc, fmt->streams[vs]->codecpar);
        dc->hw_device_ctx = av_buffer_ref(hw);
        dc->get_format = get_vtb;
        if (avcodec_open2(dc, dec, nullptr) < 0) { fprintf(stderr, "open decoder\n"); return false; }
        pkt = av_packet_alloc(); fr = av_frame_alloc();
        return true;
    }

    // Decode the next VideoToolbox frame into fr. Returns false at EOF.
    bool next() {
        for (;;) {
            int r = avcodec_receive_frame(dc, fr);
            if (r == 0) { if (fr->format == AV_PIX_FMT_VIDEOTOOLBOX) return true; av_frame_unref(fr); continue; }
            if (r != AVERROR(EAGAIN)) return false;
            if (av_read_frame(fmt, pkt) < 0) { avcodec_send_packet(dc, nullptr); continue; }  // flush
            if (pkt->stream_index == vs) avcodec_send_packet(dc, pkt);
            av_packet_unref(pkt);
        }
    }

    void close() {
        if (fr) av_frame_free(&fr);
        if (pkt) av_packet_free(&pkt);
        if (dc) avcodec_free_context(&dc);
        if (hw) av_buffer_unref(&hw);
        if (fmt) avformat_close_input(&fmt);
    }
};

static const char* fourcc_str(OSType t, char out[5]) {
    out[0] = (t >> 24) & 0xff; out[1] = (t >> 16) & 0xff;
    out[2] = (t >> 8) & 0xff;  out[3] = t & 0xff; out[4] = 0;
    return out;
}

// ---------------------------------------------------------------------------
// Metal context + kernels. The downsample/diff math mirrors bench/vk shaders
// exactly (point-sample at i*ds, value = read().r*255+0.5). Preprocess mirrors
// nv12_to_chw.comp (bilinear, inverse letterbox, BT.601 limited range).
// ---------------------------------------------------------------------------
static const char* KERNELS = R"METAL(
#include <metal_stdlib>
using namespace metal;

// M2/M3: point-sample luma downsample to a grid cell (matches motion_downsample_img.comp)
kernel void luma_downsample(texture2d<float, access::read> yTex [[texture(0)]],
                            device uint*  grid [[buffer(0)]],
                            constant int4& P   [[buffer(1)]],   // w, h, ds, gw
                            constant int&  gh  [[buffer(2)]],
                            uint2 gid [[thread_position_in_grid]]) {
    int w = P.x, h = P.y, ds = P.z, gw = P.w;
    if ((int)gid.x >= gw || (int)gid.y >= gh) return;
    int sx = (int)gid.x * ds; if (sx > w - 1) sx = w - 1;
    int sy = (int)gid.y * ds; if (sy > h - 1) sy = h - 1;
    float v = yTex.read(uint2((uint)sx, (uint)sy)).r;        // r8Unorm -> [0,1]
    grid[(uint)gid.y * (uint)gw + gid.x] = (uint)(v * 255.0 + 0.5);
}

// M3: fused downsample + diff vs device-resident prev grid + atomic verdict.
// Mirrors motion_diff_img.comp: one pass, prev[] persists, hasPrev primes frame 1.
struct Verdict { atomic_int cnt; atomic_int minx; atomic_int miny; atomic_int maxx; atomic_int maxy; };
kernel void luma_diff(texture2d<float, access::read> yTex [[texture(0)]],
                      device uint*    prev [[buffer(0)]],
                      device Verdict* v    [[buffer(1)]],
                      constant int4&  P    [[buffer(2)]],   // w, h, ds, gw
                      constant int4&  Q    [[buffer(3)]],   // gh, thr, hasPrev, _
                      uint2 gid [[thread_position_in_grid]]) {
    int w = P.x, h = P.y, ds = P.z, gw = P.w;
    int gh = Q.x, thr = Q.y, hasPrev = Q.z;
    if ((int)gid.x >= gw || (int)gid.y >= gh) return;
    int sx = (int)gid.x * ds; if (sx > w - 1) sx = w - 1;
    int sy = (int)gid.y * ds; if (sy > h - 1) sy = h - 1;
    uint cur = (uint)(yTex.read(uint2((uint)sx, (uint)sy)).r * 255.0 + 0.5);
    uint idx = (uint)gid.y * (uint)gw + gid.x;
    if (hasPrev != 0) {
        int dd = (int)cur - (int)prev[idx]; if (dd < 0) dd = -dd;
        if (dd > thr) {
            atomic_fetch_add_explicit(&v->cnt, 1, memory_order_relaxed);
            atomic_fetch_min_explicit(&v->minx, (int)gid.x, memory_order_relaxed);
            atomic_fetch_min_explicit(&v->miny, (int)gid.y, memory_order_relaxed);
            atomic_fetch_max_explicit(&v->maxx, (int)gid.x, memory_order_relaxed);
            atomic_fetch_max_explicit(&v->maxy, (int)gid.y, memory_order_relaxed);
        }
    }
    prev[idx] = cur;
}

// M4: NV12 -> CHW letterbox preprocess (bilinear). Mirrors nv12_to_chw.comp.
kernel void nv12_to_chw(texture2d<float, access::sample> yTex  [[texture(0)]],
                        texture2d<float, access::sample> uvTex [[texture(1)]],
                        device float* chw [[buffer(0)]],
                        constant int4&   P [[buffer(1)]],   // srcW, srcH, net, padx
                        constant int2&   Q [[buffer(2)]],   // pady, _
                        constant float&  scale [[buffer(3)]],
                        uint2 gid [[thread_position_in_grid]]) {
    int srcW = P.x, srcH = P.y, net = P.z, padx = P.w, pady = Q.x;
    if ((int)gid.x >= net || (int)gid.y >= net) return;
    constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::linear);
    int plane = net * net;
    float r = 114.0/255.0, g = r, b = r;
    float sx = ((float)gid.x - (float)padx + 0.5f) / scale - 0.5f;
    float sy = ((float)gid.y - (float)pady + 0.5f) / scale - 0.5f;
    if (sx >= 0.0f && sy >= 0.0f && sx < (float)srcW && sy < (float)srcH) {
        float2 uvw = float2((sx + 0.5f) / (float)srcW, (sy + 0.5f) / (float)srcH);
        float  Y  = yTex.sample(s, uvw).r * 255.0f;
        float2 UV = uvTex.sample(s, uvw).rg * 255.0f;
        float Yf = Y - 16.0f, U = UV.x - 128.0f, V = UV.y - 128.0f;
        float R = 1.164f*Yf + 1.596f*V;
        float G = 1.164f*Yf - 0.392f*U - 0.813f*V;
        float B = 1.164f*Yf + 2.017f*U;
        r = clamp(R, 0.f, 255.f)/255.f; g = clamp(G, 0.f, 255.f)/255.f; b = clamp(B, 0.f, 255.f)/255.f;
    }
    chw[0*plane + (int)gid.y*net + (int)gid.x] = r;
    chw[1*plane + (int)gid.y*net + (int)gid.x] = g;
    chw[2*plane + (int)gid.y*net + (int)gid.x] = b;
}
)METAL";

struct MetalCtx {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLComputePipelineState> psoDown = nil, psoDiff = nil, psoPre = nil;
    CVMetalTextureCacheRef texCache = nullptr;
    bool init() {
        device = MTLCreateSystemDefaultDevice();
        if (!device) { fprintf(stderr, "no Metal device\n"); return false; }
        queue = [device newCommandQueue];
        NSError* e = nil;
        id<MTLLibrary> lib = [device newLibraryWithSource:[NSString stringWithUTF8String:KERNELS]
                                                  options:nil error:&e];
        if (!lib) { fprintf(stderr, "kernel compile: %s\n", e ? e.description.UTF8String : "?"); return false; }
        auto pso = [&](const char* n) {
            id<MTLFunction> f = [lib newFunctionWithName:[NSString stringWithUTF8String:n]];
            NSError* pe = nil; id<MTLComputePipelineState> p = [device newComputePipelineStateWithFunction:f error:&pe];
            if (!p) fprintf(stderr, "pso %s: %s\n", n, pe ? pe.description.UTF8String : "?");
            return p;
        };
        psoDown = pso("luma_downsample"); psoDiff = pso("luma_diff"); psoPre = pso("nv12_to_chw");
        if (!psoDown || !psoDiff || !psoPre) return false;
        if (CVMetalTextureCacheCreate(kCFAllocatorDefault, nullptr, device, nullptr, &texCache) != kCVReturnSuccess) {
            fprintf(stderr, "CVMetalTextureCacheCreate failed\n"); return false; }
        printf("Metal: %s\n", device.name.UTF8String);
        return true;
    }
};

// Wrap a CVPixelBuffer plane as an MTLTexture (zero-copy via the cache). Keep the
// returned CVMetalTextureRef alive until the GPU work completes, then CFRelease.
static id<MTLTexture> planeTexture(MetalCtx& c, CVPixelBufferRef pb, int plane,
                                   MTLPixelFormat fmt, CVMetalTextureRef* ref) {
    size_t w = CVPixelBufferGetWidthOfPlane(pb, plane), h = CVPixelBufferGetHeightOfPlane(pb, plane);
    *ref = nullptr;
    if (CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, c.texCache, pb, nullptr,
                                                  fmt, w, h, plane, ref) != kCVReturnSuccess || !*ref)
        return nil;
    return CVMetalTextureGetTexture(*ref);
}

static void dispatch2D(id<MTLComputeCommandEncoder> e, id<MTLComputePipelineState> pso, int w, int h) {
    NSUInteger tew = pso.threadExecutionWidth, tgh = std::max<NSUInteger>(1, pso.maxTotalThreadsPerThreadgroup / tew);
    [e dispatchThreads:MTLSizeMake(w, h, 1) threadsPerThreadgroup:MTLSizeMake(tew, tgh, 1)];
}

// ===========================================================================
// M1: VideoToolbox decode -> CVPixelBuffer
// ===========================================================================
static bool m1_decode_inspect(Decoder& d, int wantFrame, CVPixelBufferRef* outFirst) {
    printf("== M1: VideoToolbox decode -> CVPixelBuffer ==\n");
    int idx = 0;
    while (d.next()) {
        if (idx++ < wantFrame) { av_frame_unref(d.fr); continue; }
        CVPixelBufferRef pb = (CVPixelBufferRef)d.fr->data[3];
        if (!pb) { printf("  FAIL: AVFrame.data[3] is null (no CVPixelBuffer)\n"); return false; }

        OSType pf = CVPixelBufferGetPixelFormatType(pb);
        char fc[5]; fourcc_str(pf, fc);
        size_t planes = CVPixelBufferGetPlaneCount(pb);
        size_t w  = CVPixelBufferGetWidth(pb),  h  = CVPixelBufferGetHeight(pb);
        size_t yw = CVPixelBufferGetWidthOfPlane(pb, 0),  yh = CVPixelBufferGetHeightOfPlane(pb, 0);
        size_t cw = planes > 1 ? CVPixelBufferGetWidthOfPlane(pb, 1) : 0;
        size_t ch = planes > 1 ? CVPixelBufferGetHeightOfPlane(pb, 1) : 0;
        bool iosurf = CVPixelBufferGetIOSurface(pb) != nullptr;

        const bool videoRange = (pf == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
        const bool fullRange  = (pf == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange);

        printf("  frame %d: %zux%zu  format='%s' (0x%08x)  planes=%zu  IOSurface=%s\n",
               wantFrame, w, h, fc, (unsigned)pf, planes, iosurf ? "yes" : "no");
        printf("    range: %s\n", videoRange ? "VIDEO (NV12 16-235, BT.601 1.164*(Y-16))"
                                : fullRange  ? "FULL (NV12 0-255)" : "OTHER/unknown");
        printf("    plane0 (Y)   %zux%zu   plane1 (CbCr) %zux%zu\n", yw, yh, cw, ch);

        bool nv12 = (videoRange || fullRange) && planes == 2;
        if (!nv12)   { printf("  FAIL: not bi-planar NV12\n"); av_frame_unref(d.fr); return false; }
        if (!iosurf) { printf("  WARN: not IOSurface-backed (Metal zero-copy import may fail)\n"); }

        if (outFirst) { *outFirst = (CVPixelBufferRef)CVBufferRetain(pb); }
        printf("  PASS: VideoToolbox produced an %s NV12 CVPixelBuffer%s\n",
               videoRange ? "video-range" : "full-range", iosurf ? " (IOSurface-backed, zero-copy ready)" : "");
        av_frame_unref(d.fr);
        return true;
    }
    printf("  FAIL: could not decode frame %d\n", wantFrame);
    return false;
}

// ===========================================================================
// M2: CVPixelBuffer -> MTLTexture import + GPU luma downsample vs CPU readback
// ===========================================================================
static bool m2_import_downsample(MetalCtx& c, CVPixelBufferRef pb) {
    printf("\n== M2: MTLTexture import + GPU downsample vs CPU ==\n");
    const int W = (int)CVPixelBufferGetWidth(pb), H = (int)CVPixelBufferGetHeight(pb);
    const int DS = 8, GW = W / DS, GH = H / DS, NC = GW * GH;

    CVMetalTextureRef yRef = nullptr;
    id<MTLTexture> yTex = planeTexture(c, pb, 0, MTLPixelFormatR8Unorm, &yRef);
    if (!yTex) { printf("  FAIL: Y plane texture import failed\n"); return false; }

    id<MTLBuffer> grid = [c.device newBufferWithLength:NC * sizeof(uint32_t)
                                              options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [c.queue commandBuffer];
    id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e setComputePipelineState:c.psoDown];
    [e setTexture:yTex atIndex:0];
    [e setBuffer:grid offset:0 atIndex:0];
    int P[4] = {W, H, DS, GW}; [e setBytes:P length:sizeof(P) atIndex:1];
    int gh = GH;               [e setBytes:&gh length:sizeof(int) atIndex:2];
    dispatch2D(e, c.psoDown, GW, GH);
    [e endEncoding];
    [cb commit]; [cb waitUntilCompleted];
    CFRelease(yRef);

    // CPU reference: lock the (IOSurface-backed) buffer and point-sample the Y plane.
    CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    const uint8_t* Y = (const uint8_t*)CVPixelBufferGetBaseAddressOfPlane(pb, 0);
    const size_t rb = CVPixelBufferGetBytesPerRowOfPlane(pb, 0);
    const uint32_t* g = (const uint32_t*)grid.contents;
    long mism = 0, bad = 0; int bx = -1, by = -1, bg = 0, bc = 0;
    for (int j = 0; j < GH; j++)
        for (int i = 0; i < GW; i++) {
            int sx = i * DS; if (sx > W - 1) sx = W - 1;
            int sy = j * DS; if (sy > H - 1) sy = H - 1;
            int cpu = Y[sy * rb + sx], gpu = (int)g[j * GW + i];
            if (cpu != gpu) { if (!mism) { bx = i; by = j; bg = gpu; bc = cpu; } ++mism; }
            if (std::abs(cpu - gpu) > 1) ++bad;
        }
    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);

    printf("  grid %dx%d (%d cells): %ld exact mismatches, %ld differ by >1\n", GW, GH, NC, mism, bad);
    if (mism) printf("  first @ (%d,%d): gpu=%d cpu=%d\n", bx, by, bg, bc);
    printf(bad == 0 ? "  PASS: Metal sampled the imported CVPixelBuffer correctly (zero-copy)\n"
                    : "  FAIL: import/sampling mismatch\n");
    return bad == 0;
}

// ===========================================================================
// M3: Metal motion gate verdict vs CPU diff over N frames (mirrors vk_va_gate.cpp)
// ===========================================================================
static bool m3_motion_gate(MetalCtx& c, const char* clip, int MAXF) {
    printf("\n== M3: Metal motion gate verdict vs CPU over %d frames ==\n", MAXF);
    Decoder d; if (!d.open(clip)) return false;
    const int DS = 8, THR = 25;
    int W = 0, H = 0, GW = 0, GH = 0, NC = 0;
    id<MTLBuffer> prev = nil, verdict = nil;
    std::vector<uint8_t> prevCPU;
    bool hasPrev = false; int frames = 0, motion = 0; long mismatch = 0;

    while (frames < MAXF && d.next()) {
        CVPixelBufferRef pb = (CVPixelBufferRef)d.fr->data[3];
        if (!W) {
            W = (int)CVPixelBufferGetWidth(pb); H = (int)CVPixelBufferGetHeight(pb);
            GW = W / DS; GH = H / DS; NC = GW * GH;
            prev = [c.device newBufferWithLength:NC * sizeof(uint32_t) options:MTLResourceStorageModeShared];
            verdict = [c.device newBufferWithLength:5 * sizeof(int32_t) options:MTLResourceStorageModeShared];
            prevCPU.assign(NC, 0);
        }
        CVMetalTextureRef yRef = nullptr;
        id<MTLTexture> yTex = planeTexture(c, pb, 0, MTLPixelFormatR8Unorm, &yRef);
        if (!yTex) { printf("  FAIL: import frame %d\n", frames); d.close(); return false; }

        int32_t vinit[5] = {0, GW, GH, -1, -1};   // cnt, minx, miny, maxx, maxy
        std::memcpy(verdict.contents, vinit, sizeof(vinit));

        id<MTLCommandBuffer> cb = [c.queue commandBuffer];
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e setComputePipelineState:c.psoDiff];
        [e setTexture:yTex atIndex:0];
        [e setBuffer:prev offset:0 atIndex:0];
        [e setBuffer:verdict offset:0 atIndex:1];
        int P[4] = {W, H, DS, GW};                 [e setBytes:P length:sizeof(P) atIndex:2];
        int Q[4] = {GH, THR, hasPrev ? 1 : 0, 0};  [e setBytes:Q length:sizeof(Q) atIndex:3];
        dispatch2D(e, c.psoDiff, GW, GH);
        [e endEncoding];
        [cb commit]; [cb waitUntilCompleted];
        int gcnt = ((const int32_t*)verdict.contents)[0];
        CFRelease(yRef);

        // CPU reference: lock + point-sample downsample + diff vs prevCPU.
        CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
        const uint8_t* Y = (const uint8_t*)CVPixelBufferGetBaseAddressOfPlane(pb, 0);
        const size_t rb = CVPixelBufferGetBytesPerRowOfPlane(pb, 0);
        std::vector<uint8_t> cur(NC);
        for (int j = 0; j < GH; j++)
            for (int i = 0; i < GW; i++) {
                int sx = i * DS; if (sx > W - 1) sx = W - 1;
                int sy = j * DS; if (sy > H - 1) sy = H - 1;
                cur[j * GW + i] = Y[sy * rb + sx];
            }
        CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
        int ccnt = 0;
        if (hasPrev)
            for (int k = 0; k < NC; k++) { int dd = (int)cur[k] - (int)prevCPU[k]; if (dd < 0) dd = -dd; if (dd > THR) ++ccnt; }
        prevCPU = cur;

        if (hasPrev && gcnt != ccnt) { if (mismatch < 3) printf("  frame %d: GPU cnt=%d CPU cnt=%d MISMATCH\n", frames, gcnt, ccnt); ++mismatch; }
        if (hasPrev && gcnt > 0) ++motion;
        hasPrev = true; ++frames;
        av_frame_unref(d.fr);
    }
    d.close();
    printf("  frames=%d  motion=%d (%.0f%%)  GPU-vs-CPU verdict mismatches=%ld\n",
           frames, motion, frames ? 100.0 * motion / frames : 0, mismatch);
    printf(mismatch == 0 ? "  PASS: GPU motion gate matches CPU; only a 20B verdict crossed per frame\n"
                         : "  FAIL: verdict mismatch\n");
    return mismatch == 0 && frames > 1;
}

// ===========================================================================
// M4: Metal NV12 -> CHW preprocess; dump to PPM + assert letterbox bars exist
// (mirrors vk_va_preproc.cpp; the gray bars prove aspect was preserved).
// ===========================================================================
static bool m4_preprocess(MetalCtx& c, const char* clip, int wantFrame, int net) {
    printf("\n== M4: Metal NV12->CHW preprocess (net=%d) ==\n", net);
    Decoder d; if (!d.open(clip)) return false;
    int idx = 0; CVPixelBufferRef pb = nullptr;
    while (d.next()) { if (idx++ >= wantFrame) { pb = (CVPixelBufferRef)d.fr->data[3]; break; } av_frame_unref(d.fr); }
    if (!pb) { printf("  FAIL: no frame %d\n", wantFrame); d.close(); return false; }

    const int W = (int)CVPixelBufferGetWidth(pb), H = (int)CVPixelBufferGetHeight(pb);
    zm::detect::Letterbox lb = zm::detect::compute_letterbox(W, H, net);
    printf("  src %dx%d -> net %d  scale=%.4f pad=(%d,%d)\n", W, H, net, lb.scale, lb.pad_x, lb.pad_y);

    CVMetalTextureRef yRef = nullptr, uvRef = nullptr;
    id<MTLTexture> yTex  = planeTexture(c, pb, 0, MTLPixelFormatR8Unorm,  &yRef);
    id<MTLTexture> uvTex = planeTexture(c, pb, 1, MTLPixelFormatRG8Unorm, &uvRef);
    if (!yTex || !uvTex) { printf("  FAIL: plane import\n"); d.close(); return false; }

    id<MTLBuffer> chw = [c.device newBufferWithLength:3 * net * net * sizeof(float)
                                             options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [c.queue commandBuffer];
    id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e setComputePipelineState:c.psoPre];
    [e setTexture:yTex atIndex:0]; [e setTexture:uvTex atIndex:1];
    [e setBuffer:chw offset:0 atIndex:0];
    int P[4] = {W, H, net, lb.pad_x}; [e setBytes:P length:sizeof(P) atIndex:1];
    int Q[2] = {lb.pad_y, 0};         [e setBytes:Q length:sizeof(Q) atIndex:2];
    float scale = lb.scale;           [e setBytes:&scale length:sizeof(float) atIndex:3];
    dispatch2D(e, c.psoPre, net, net);
    [e endEncoding];
    [cb commit]; [cb waitUntilCompleted];
    CFRelease(yRef); CFRelease(uvRef);

    const float* f = (const float*)chw.contents;
    const int plane = net * net;
    auto px = [&](int x, int y, int ch) { return f[ch * plane + y * net + x]; };

    // Dump an interleaved RGB PPM for eyeballing.
    std::string out = "m4_chw.ppm";
    FILE* fp = fopen(out.c_str(), "wb");
    fprintf(fp, "P6\n%d %d\n255\n", net, net);
    std::vector<uint8_t> row(net * 3);
    for (int y = 0; y < net; y++) {
        for (int x = 0; x < net; x++) {
            row[x*3+0] = (uint8_t)std::clamp(px(x, y, 0) * 255.f + 0.5f, 0.f, 255.f);
            row[x*3+1] = (uint8_t)std::clamp(px(x, y, 1) * 255.f + 0.5f, 0.f, 255.f);
            row[x*3+2] = (uint8_t)std::clamp(px(x, y, 2) * 255.f + 0.5f, 0.f, 255.f);
        }
        fwrite(row.data(), 1, row.size(), fp);
    }
    fclose(fp);
    av_frame_unref(d.fr); d.close();

    // Assert aspect preservation: with pad_y>0 the top/bottom rows must be the
    // 114/255 letterbox border, and the centre must be real content (not border).
    const float PAD = 114.f / 255.f;
    auto isBorder = [&](int x, int y) {
        return std::abs(px(x, y, 0) - PAD) < 0.02f && std::abs(px(x, y, 1) - PAD) < 0.02f
            && std::abs(px(x, y, 2) - PAD) < 0.02f;
    };
    bool ok = true;
    if (lb.pad_y > 2) {
        bool topBar = isBorder(net/2, 2) && isBorder(net/2, lb.pad_y - 2);
        bool botBar = isBorder(net/2, net - 3);
        bool centreContent = !isBorder(net/2, net/2);
        printf("  letterbox check: top-bar=%s bottom-bar=%s centre-content=%s\n",
               topBar?"yes":"no", botBar?"yes":"no", centreContent?"yes":"no");
        ok = topBar && botBar && centreContent;
        if (!ok) printf("  FAIL: bars/content not where aspect-preserving letterbox expects (squashing?)\n");
    } else if (lb.pad_x > 2) {
        bool leftBar = isBorder(2, net/2), rightBar = isBorder(net - 3, net/2);
        bool centreContent = !isBorder(net/2, net/2);
        printf("  letterbox check: left-bar=%s right-bar=%s centre-content=%s\n",
               leftBar?"yes":"no", rightBar?"yes":"no", centreContent?"yes":"no");
        ok = leftBar && rightBar && centreContent;
    }
    printf("  wrote bench/metal/%s (%dx%d) — open it to eyeball colour + aspect\n", out.c_str(), net, net);
    printf(ok ? "  PASS: aspect-preserved letterbox, colour decoded\n" : "  FAIL\n");
    return ok;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <clip> [frame=30] [frames=120]\n", argv[0]); return 2; }
    const char* clip = argv[1];
    const int wantFrame = argc > 2 ? atoi(argv[2]) : 30;
    const int frames = argc > 3 ? atoi(argv[3]) : 120;

    @autoreleasepool {
        MetalCtx mc;
        if (!mc.init()) return 1;

        Decoder d;
        if (!d.open(clip)) return 1;

        CVPixelBufferRef first = nullptr;
        bool ok = m1_decode_inspect(d, wantFrame, &first);
        if (ok && first) ok = m2_import_downsample(mc, first) && ok;
        if (first) CVBufferRelease(first);
        d.close();

        ok = m3_motion_gate(mc, clip, frames) && ok;
        ok = m4_preprocess(mc, clip, wantFrame, 640) && ok;
        printf("\n%s\n", ok ? "ALL OK" : "FAILED");
        return ok ? 0 : 1;
    }
}

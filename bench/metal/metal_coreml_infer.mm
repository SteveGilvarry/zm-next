// metal_coreml_infer.mm — M5: CoreML inference + latency on Apple silicon.
//
// Full GPU/ANE detect path tail: VideoToolbox decode -> Metal NV12->CHW preprocess
// (zero-copy via CVMetalTextureCache, shared MTLBuffer) -> ONNX Runtime CoreML EP
// inference, comparing the Apple Neural Engine vs GPU vs CPU. Mirrors how the CUDA
// path feeds a device CHW tensor into ORT; here ORT's CoreML EP takes the (UMA,
// CPU-visible) MTLBuffer contents. Decodes the NMS-free [1,N,6] head with the same
// decode_nms_free as the rest of the pipeline.
//
// Build:
//   ORT=/opt/homebrew/opt/onnxruntime
//   clang++ -std=c++17 -fobjc-arc -O2 -I../../plugins/detect_onnx -I$ORT/include/onnxruntime \
//     metal_coreml_infer.mm -o metal_coreml_infer \
//     -framework Metal -framework CoreVideo -framework CoreMedia -framework Foundation \
//     -L$ORT/lib -lonnxruntime $(pkg-config --cflags --libs libavformat libavcodec libavutil)
//
// Usage: metal_coreml_infer <model.onnx> <clip> [frame=30] [net=640] [iters=30] [conf=0.25]

#import <Metal/Metal.h>
#import <CoreVideo/CoreVideo.h>
#import <CoreMedia/CoreMedia.h>
#import <Foundation/Foundation.h>

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

#include "detect_postprocess.hpp"   // compute_letterbox, decode_nms_free, Box

// ---- VideoToolbox decode (same as metal_va_validate.mm) ----
static enum AVPixelFormat get_vtb(AVCodecContext*, const enum AVPixelFormat* f) {
    for (; *f != AV_PIX_FMT_NONE; f++) if (*f == AV_PIX_FMT_VIDEOTOOLBOX) return *f;
    return AV_PIX_FMT_NONE;
}
struct Decoder {
    AVFormatContext* fmt=nullptr; AVCodecContext* dc=nullptr; AVBufferRef* hw=nullptr;
    AVPacket* pkt=nullptr; AVFrame* fr=nullptr; int vs=-1;
    bool open(const char* clip) {
        if (avformat_open_input(&fmt, clip, nullptr, nullptr) < 0) return false;
        avformat_find_stream_info(fmt, nullptr);
        for (unsigned i=0;i<fmt->nb_streams;i++) if (fmt->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_VIDEO){vs=(int)i;break;}
        if (vs<0) return false;
        const AVCodec* dec=avcodec_find_decoder(fmt->streams[vs]->codecpar->codec_id);
        if (av_hwdevice_ctx_create(&hw,AV_HWDEVICE_TYPE_VIDEOTOOLBOX,nullptr,nullptr,0)<0) return false;
        dc=avcodec_alloc_context3(dec); avcodec_parameters_to_context(dc,fmt->streams[vs]->codecpar);
        dc->hw_device_ctx=av_buffer_ref(hw); dc->get_format=get_vtb;
        if (avcodec_open2(dc,dec,nullptr)<0) return false;
        pkt=av_packet_alloc(); fr=av_frame_alloc(); return true;
    }
    bool next() {
        for (;;) {
            int r=avcodec_receive_frame(dc,fr);
            if (r==0){ if (fr->format==AV_PIX_FMT_VIDEOTOOLBOX) return true; av_frame_unref(fr); continue; }
            if (r!=AVERROR(EAGAIN)) return false;
            if (av_read_frame(fmt,pkt)<0){ avcodec_send_packet(dc,nullptr); continue; }
            if (pkt->stream_index==vs) avcodec_send_packet(dc,pkt);
            av_packet_unref(pkt);
        }
    }
    void close(){ if(fr)av_frame_free(&fr); if(pkt)av_packet_free(&pkt); if(dc)avcodec_free_context(&dc); if(hw)av_buffer_unref(&hw); if(fmt)avformat_close_input(&fmt); }
};

// ---- Metal NV12->CHW preprocess (same kernel as metal_va_validate.mm) ----
static const char* PRE_KERNEL = R"METAL(
#include <metal_stdlib>
using namespace metal;
kernel void nv12_to_chw(texture2d<float, access::sample> yTex  [[texture(0)]],
                        texture2d<float, access::sample> uvTex [[texture(1)]],
                        device float* chw [[buffer(0)]],
                        constant int4& P [[buffer(1)]], constant int2& Q [[buffer(2)]],
                        constant float& scale [[buffer(3)]], uint2 gid [[thread_position_in_grid]]) {
    int srcW=P.x, srcH=P.y, net=P.z, padx=P.w, pady=Q.x;
    if ((int)gid.x>=net || (int)gid.y>=net) return;
    constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::linear);
    int plane=net*net; float r=114.0/255.0, g=r, b=r;
    float sx=((float)gid.x-(float)padx+0.5f)/scale-0.5f;
    float sy=((float)gid.y-(float)pady+0.5f)/scale-0.5f;
    if (sx>=0.0f && sy>=0.0f && sx<(float)srcW && sy<(float)srcH) {
        float2 uvw=float2((sx+0.5f)/(float)srcW,(sy+0.5f)/(float)srcH);
        float Y=yTex.sample(s,uvw).r*255.0f; float2 UV=uvTex.sample(s,uvw).rg*255.0f;
        float Yf=Y-16.0f, U=UV.x-128.0f, V=UV.y-128.0f;
        float R=1.164f*Yf+1.596f*V, G=1.164f*Yf-0.392f*U-0.813f*V, B=1.164f*Yf+2.017f*U;
        r=clamp(R,0.f,255.f)/255.f; g=clamp(G,0.f,255.f)/255.f; b=clamp(B,0.f,255.f)/255.f;
    }
    chw[0*plane+(int)gid.y*net+(int)gid.x]=r; chw[1*plane+(int)gid.y*net+(int)gid.x]=g; chw[2*plane+(int)gid.y*net+(int)gid.x]=b;
}
)METAL";

struct Metal {
    id<MTLDevice> dev=nil; id<MTLCommandQueue> q=nil; id<MTLComputePipelineState> pso=nil; CVMetalTextureCacheRef cache=nullptr;
    bool init() {
        dev=MTLCreateSystemDefaultDevice(); if(!dev) return false; q=[dev newCommandQueue];
        NSError* e=nil; id<MTLLibrary> lib=[dev newLibraryWithSource:[NSString stringWithUTF8String:PRE_KERNEL] options:nil error:&e];
        if(!lib){ fprintf(stderr,"kernel: %s\n", e?e.description.UTF8String:"?"); return false; }
        pso=[dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"nv12_to_chw"] error:&e];
        if(!pso) return false;
        return CVMetalTextureCacheCreate(kCFAllocatorDefault,nullptr,dev,nullptr,&cache)==kCVReturnSuccess;
    }
};
static id<MTLTexture> planeTex(Metal& m, CVPixelBufferRef pb, int p, MTLPixelFormat f, CVMetalTextureRef* ref) {
    size_t w=CVPixelBufferGetWidthOfPlane(pb,p), h=CVPixelBufferGetHeightOfPlane(pb,p); *ref=nullptr;
    if (CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault,m.cache,pb,nullptr,f,w,h,p,ref)!=kCVReturnSuccess||!*ref) return nil;
    return CVMetalTextureGetTexture(*ref);
}

// Metal preprocess one CVPixelBuffer into a shared (UMA, CPU-visible) CHW MTLBuffer.
static id<MTLBuffer> preprocess(Metal& m, CVPixelBufferRef pb, int net, zm::detect::Letterbox& lb) {
    int W=(int)CVPixelBufferGetWidth(pb), H=(int)CVPixelBufferGetHeight(pb);
    lb = zm::detect::compute_letterbox(W, H, net);
    CVMetalTextureRef yR=nullptr, uvR=nullptr;
    id<MTLTexture> yT=planeTex(m,pb,0,MTLPixelFormatR8Unorm,&yR);
    id<MTLTexture> uvT=planeTex(m,pb,1,MTLPixelFormatRG8Unorm,&uvR);
    if(!yT||!uvT){ if(yR)CFRelease(yR); if(uvR)CFRelease(uvR); return nil; }
    id<MTLBuffer> chw=[m.dev newBufferWithLength:3*net*net*sizeof(float) options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb=[m.q commandBuffer]; id<MTLComputeCommandEncoder> e=[cb computeCommandEncoder];
    [e setComputePipelineState:m.pso]; [e setTexture:yT atIndex:0]; [e setTexture:uvT atIndex:1]; [e setBuffer:chw offset:0 atIndex:0];
    int P[4]={W,H,net,lb.pad_x}; [e setBytes:P length:sizeof(P) atIndex:1];
    int Q[2]={lb.pad_y,0}; [e setBytes:Q length:sizeof(Q) atIndex:2];
    float sc=lb.scale; [e setBytes:&sc length:sizeof(float) atIndex:3];
    NSUInteger tew=m.pso.threadExecutionWidth, tgh=std::max<NSUInteger>(1,m.pso.maxTotalThreadsPerThreadgroup/tew);
    [e dispatchThreads:MTLSizeMake(net,net,1) threadsPerThreadgroup:MTLSizeMake(tew,tgh,1)];
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    CFRelease(yR); CFRelease(uvR);
    return chw;
}

// ---- ORT inference under one EP config; returns median ms + last detections ----
struct EpResult { std::string label; double median_ms=0, min_ms=0; int dets=0; bool ok=false; std::vector<zm::detect::Box> boxes; };

static EpResult run_ep(Ort::Env& env, const char* model, const std::string& label,
                       bool coreml, const char* computeUnits, const float* chw, int net,
                       const zm::detect::Letterbox& lb, float conf, int iters) {
    EpResult R; R.label = label;
    try {
        Ort::SessionOptions so;
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        if (coreml) {
            std::unordered_map<std::string,std::string> opt = {
                {"MLComputeUnits", computeUnits}, {"ModelFormat", "MLProgram"},
                {"RequireStaticInputShapes", "1"},
            };
            so.AppendExecutionProvider("CoreML", opt);
        } else {
            so.SetIntraOpNumThreads(0);   // CPU EP: let ORT pick threads
        }
        Ort::Session sess(env, model, so);
        Ort::AllocatorWithDefaultOptions alloc;
        std::string in = sess.GetInputNameAllocated(0, alloc).get();
        std::string out = sess.GetOutputNameAllocated(0, alloc).get();
        const char* inN[] = {in.c_str()}; const char* outN[] = {out.c_str()};

        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::array<int64_t,4> shp{1,3,net,net};
        auto mkInput = [&]{ return Ort::Value::CreateTensor<float>(mem, const_cast<float*>(chw),
                              (size_t)3*net*net, shp.data(), shp.size()); };

        // warmup (first run compiles/loads the CoreML model — exclude from timing)
        for (int w=0; w<3; ++w) { Ort::Value v=mkInput(); sess.Run(Ort::RunOptions{nullptr}, inN, &v, 1, outN, 1); }

        std::vector<double> ms; ms.reserve(iters);
        Ort::Value last;
        for (int i=0;i<iters;++i) {
            Ort::Value v=mkInput();
            auto t0=std::chrono::high_resolution_clock::now();
            auto outs=sess.Run(Ort::RunOptions{nullptr}, inN, &v, 1, outN, 1);
            auto t1=std::chrono::high_resolution_clock::now();
            ms.push_back(std::chrono::duration<double,std::milli>(t1-t0).count());
            if (i==iters-1) last=std::move(outs[0]);
        }
        std::sort(ms.begin(), ms.end());
        R.median_ms = ms[ms.size()/2]; R.min_ms = ms.front();

        auto info = last.GetTensorTypeAndShapeInfo(); auto dims = info.GetShape();
        const float* od = last.GetTensorData<float>();
        int rows = (dims.size()==3 && dims.back()==6) ? (int)dims[1] : 0;
        if (rows>0) R.boxes = zm::detect::decode_nms_free(od, rows, lb, conf, {});
        R.dets = (int)R.boxes.size();
        R.ok = true;
    } catch (const std::exception& e) {
        fprintf(stderr, "  [%s] failed: %s\n", label.c_str(), e.what());
    }
    return R;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <model.onnx> <clip> [frame=30] [net=640] [iters=30] [conf=0.25]\n", argv[0]); return 2; }
    const char* model=argv[1]; const char* clip=argv[2];
    int wantFrame=argc>3?atoi(argv[3]):30, net=argc>4?atoi(argv[4]):640, iters=argc>5?atoi(argv[5]):30;
    float conf=argc>6?atof(argv[6]):0.25f;

    @autoreleasepool {
        Metal m; if (!m.init()) { fprintf(stderr,"Metal init failed\n"); return 1; }
        printf("Metal: %s   model: %s\n", m.dev.name.UTF8String, model);

        Decoder d; if (!d.open(clip)) { fprintf(stderr,"open clip failed\n"); return 1; }
        int idx=0; CVPixelBufferRef pb=nullptr;
        while (d.next()) { if (idx++>=wantFrame){ pb=(CVPixelBufferRef)d.fr->data[3]; break; } av_frame_unref(d.fr); }
        if (!pb) { fprintf(stderr,"no frame %d\n",wantFrame); return 1; }

        zm::detect::Letterbox lb;
        id<MTLBuffer> chwBuf = preprocess(m, pb, net, lb);
        if (!chwBuf) { fprintf(stderr,"preprocess failed\n"); return 1; }
        const float* chw = (const float*)chwBuf.contents;
        printf("frame %d preprocessed (net=%d, scale=%.3f pad=(%d,%d)); %d timed iters\n\n",
               wantFrame, net, lb.scale, lb.pad_x, lb.pad_y, iters);

        Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "m5");
        std::vector<EpResult> R;
        R.push_back(run_ep(env, model, "CoreML/ANE  (CPUAndNeuralEngine)", true,  "CPUAndNeuralEngine", chw, net, lb, conf, iters));
        R.push_back(run_ep(env, model, "CoreML/GPU  (CPUAndGPU)",          true,  "CPUAndGPU",          chw, net, lb, conf, iters));
        R.push_back(run_ep(env, model, "CoreML/ALL  (CoreML picks)",       true,  "ALL",               chw, net, lb, conf, iters));
        R.push_back(run_ep(env, model, "CPU EP",                           false, "",                                 chw, net, lb, conf, iters));

        av_frame_unref(d.fr); d.close();

        printf("\n=== M5: CoreML inference latency (frame %d, %d iters, median of) ===\n", wantFrame, iters);
        printf("%-34s %10s %10s %8s\n", "execution provider", "median ms", "min ms", "dets");
        double cpu_ms = 0;
        for (auto& r : R) {
            printf("%-34s %10.2f %10.2f %8d%s\n", r.label.c_str(), r.median_ms, r.min_ms, r.dets, r.ok?"":"  (FAILED)");
            if (r.label.rfind("CPU EP",0)==0 && r.ok) cpu_ms = r.median_ms;
        }
        if (cpu_ms>0) for (auto& r:R) if (r.ok && r.median_ms>0 && r.label.rfind("CPU",0)!=0)
            printf("  %-32s %.2fx vs CPU\n", r.label.c_str(), cpu_ms/r.median_ms);

        // Detections from the ANE run (sanity: real objects on the frame).
        const EpResult& ane = R[0];
        if (ane.ok) {
            printf("\nANE detections (conf>=%.2f): %d\n", conf, ane.dets);
            int shown=0; for (auto& b : ane.boxes) { if (shown++>=12) break;
                printf("  class=%d conf=%.2f  box=[%d,%d %dx%d]\n", b.class_id, b.confidence,
                       (int)b.x, (int)b.y, (int)b.w, (int)b.h); }
        }
        bool ok = ane.ok && R.back().ok;   // ANE + CPU baseline both ran
        printf("\n%s\n", ok ? "M5 OK" : "M5 FAILED");
        return ok?0:1;
    }
}

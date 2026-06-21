// metal_backend_test.mm — integration test for the WIRED Metal HwBackend.
//
// Unlike metal_va_validate / metal_coreml_infer (standalone prototypes), this
// drives the ACTUAL plugin code: zm::hw::make_backend("metal") -> the MetalBackend
// in hw_backend_metal.mm, exercising the full HwBackend contract
// (load_model -> acquire -> motion -> preprocess -> infer -> release) on a real
// VideoToolbox-decoded frame. Proves the integrated backend works, not just builds.
//
// Build: compile hw_backend.cpp (C++) + hw_backend_metal.mm (+this, ObjC++/ARC),
// link ORT + Metal/CoreVideo/VideoToolbox/CoreMedia/Foundation + FFmpeg. See the
// run_metal_backend_test.sh next to this file.
//
// Usage: metal_backend_test <model.onnx> <clip> [frame=30] [conf=0.25]

#include "hw_backend.hpp"

#import <CoreVideo/CoreVideo.h>
#include <cstdio>
#include <cstdlib>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

static enum AVPixelFormat get_vtb(AVCodecContext*, const enum AVPixelFormat* f) {
    for (; *f != AV_PIX_FMT_NONE; f++) if (*f == AV_PIX_FMT_VIDEOTOOLBOX) return *f;
    return AV_PIX_FMT_NONE;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <model.onnx> <clip> [frame=30] [conf=0.25]\n", argv[0]); return 2; }
    const char* model = argv[1]; const char* clip = argv[2];
    int wantFrame = argc > 3 ? atoi(argv[3]) : 30;
    float conf = argc > 4 ? atof(argv[4]) : 0.25f;

    // --- make the wired backend via the real factory ---
    auto be = zm::hw::make_backend("metal");
    if (!be) { fprintf(stderr, "make_backend(\"metal\") returned null\n"); return 1; }
    printf("backend: %s\n", be->name());
    if (!be->load_model(model, 640)) { fprintf(stderr, "load_model failed\n"); return 1; }
    printf("model loaded: %s\n", model);

    // --- decode a VideoToolbox frame ---
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, clip, nullptr, nullptr) < 0) { fprintf(stderr, "open clip\n"); return 1; }
    avformat_find_stream_info(fmt, nullptr);
    int vs = -1; for (unsigned i=0;i<fmt->nb_streams;i++) if (fmt->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_VIDEO){vs=i;break;}
    const AVCodec* dec = avcodec_find_decoder(fmt->streams[vs]->codecpar->codec_id);
    AVBufferRef* hw = nullptr; av_hwdevice_ctx_create(&hw, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr, nullptr, 0);
    AVCodecContext* dc = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(dc, fmt->streams[vs]->codecpar);
    dc->hw_device_ctx = av_buffer_ref(hw); dc->get_format = get_vtb;
    avcodec_open2(dc, dec, nullptr);
    AVPacket* pkt = av_packet_alloc(); AVFrame* fr = av_frame_alloc();

    auto nextFrame = [&]() -> bool {
        for (;;) {
            int r = avcodec_receive_frame(dc, fr);
            if (r == 0) { if (fr->format == AV_PIX_FMT_VIDEOTOOLBOX) return true; av_frame_unref(fr); continue; }
            if (r != AVERROR(EAGAIN)) return false;
            if (av_read_frame(fmt, pkt) < 0) { avcodec_send_packet(dc, nullptr); continue; }
            if (pkt->stream_index == vs) avcodec_send_packet(dc, pkt);
            av_packet_unref(pkt);
        }
    };

    int idx = 0; bool got = false;
    while (nextFrame()) { if (idx++ >= wantFrame) { got = true; break; } av_frame_unref(fr); }
    if (!got) { fprintf(stderr, "no frame %d\n", wantFrame); return 1; }

    // --- full backend contract on the decoded frame ---
    zm::hw::Surface s = be->acquire(reinterpret_cast<uint64_t>(fr));
    printf("acquire: %dx%d hw_type=%u native=%s\n", s.width, s.height, s.hw_type, s.native ? "set" : "null");
    if (!s.native) { fprintf(stderr, "acquire produced no surface\n"); return 1; }

    auto regions = be->motion(s);
    printf("motion: %zu region(s)\n", regions.size());
    for (auto& r : regions) printf("   region [%d,%d %dx%d]\n", r.x, r.y, r.w, r.h);

    zm::hw::DeviceTensor t = be->preprocess(s);   // whole frame
    printf("preprocess: tensor %s (net=%d, scale=%.3f pad=(%d,%d))\n",
           t.valid() ? "ready" : "INVALID", t.net, t.lb.scale, t.lb.pad_x, t.lb.pad_y);
    if (!t.valid()) { fprintf(stderr, "preprocess failed\n"); return 1; }

    auto dets = be->infer(t, conf, {});
    printf("infer: %zu detection(s) (conf>=%.2f)\n", dets.size(), conf);
    int shown = 0;
    for (auto& b : dets) { if (shown++ >= 12) break;
        printf("   class=%d conf=%.2f box=[%d,%d %dx%d]\n", b.class_id, b.confidence,
               (int)b.x, (int)b.y, (int)b.w, (int)b.h); }

    be->release(s);
    printf("release: ok\n");

    av_frame_free(&fr); av_packet_free(&pkt); avcodec_free_context(&dc);
    av_buffer_unref(&hw); avformat_close_input(&fmt);

    bool ok = !dets.empty();
    printf("\n%s\n", ok ? "BACKEND TEST OK — wired Metal HwBackend runs end-to-end"
                        : "BACKEND TEST: no detections (check model/frame)");
    return ok ? 0 : 1;
}

// bench_vaapi: smoke-test the VAAPI HwBackend on a real clip. Decodes via VAAPI on
// the AMD/Intel iGPU (surfaces stay on-GPU), then drives the backend's
// acquire -> motion -> preprocess -> infer -> release per frame, exactly as the
// fused pipeline would. Proves the backend works on hardware (not just compiles).
//
// Usage: bench_vaapi <clip> <model.onnx> [device=/dev/dri/renderD129] [max=300]
#include "hw_backend.hpp"
#include <cstdio>
#include <cstdint>
#include <string>
#include <chrono>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

namespace zm::hw { std::unique_ptr<HwBackend> make_vaapi_backend(); }  // defined in hw_backend_vaapi.cpp

static enum AVPixelFormat g_hw_fmt = AV_PIX_FMT_VAAPI;
static enum AVPixelFormat get_hw_format(AVCodecContext*, const enum AVPixelFormat* fmts) {
    for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == g_hw_fmt) return *p;
    fprintf(stderr, "VAAPI surface format not offered by decoder\n");
    return AV_PIX_FMT_NONE;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <clip> <model.onnx> [device] [max]\n", argv[0]); return 1; }
    const char* clip = argv[1];
    const char* model = argv[2];
    const char* dev = argc > 3 ? argv[3] : "/dev/dri/renderD129";
    const int maxFrames = argc > 4 ? atoi(argv[4]) : 300;

    // --- open input + find video stream ---
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, clip, nullptr, nullptr) < 0) { fprintf(stderr, "open failed\n"); return 1; }
    avformat_find_stream_info(fmt, nullptr);
    int vstream = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i)
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { vstream = (int)i; break; }
    if (vstream < 0) { fprintf(stderr, "no video stream\n"); return 1; }
    AVCodecParameters* par = fmt->streams[vstream]->codecpar;
    const AVCodec* dec = avcodec_find_decoder(par->codec_id);

    // --- VAAPI hw device on the iGPU ---
    AVBufferRef* hwdev = nullptr;
    if (av_hwdevice_ctx_create(&hwdev, AV_HWDEVICE_TYPE_VAAPI, dev, nullptr, 0) < 0) {
        fprintf(stderr, "av_hwdevice_ctx_create(VAAPI, %s) failed\n", dev); return 1; }
    AVCodecContext* dctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(dctx, par);
    dctx->hw_device_ctx = av_buffer_ref(hwdev);
    dctx->get_format = get_hw_format;
    if (avcodec_open2(dctx, dec, nullptr) < 0) { fprintf(stderr, "decoder open failed\n"); return 1; }
    printf("decoding %s on %s (%s, %dx%d)\n", clip, dev, avcodec_get_name(par->codec_id), par->width, par->height);

    // --- VAAPI backend ---
    auto be = zm::hw::make_vaapi_backend();
    if (!be) { fprintf(stderr, "make_vaapi_backend() returned null\n"); return 1; }
    if (!be->load_model(model, 640)) { fprintf(stderr, "load_model failed\n"); return 1; }
    printf("backend: %s, model loaded\n", be->name());

    using clk = std::chrono::high_resolution_clock;
    auto ms = [](clk::time_point a, clk::time_point b){ return std::chrono::duration<double,std::milli>(b-a).count(); };

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    int frames = 0, motion = 0, infers = 0; long dets = 0;
    double t_motion = 0, t_pre = 0, t_inf = 0;
    auto t_start = clk::now();

    while (frames < maxFrames && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index != vstream) { av_packet_unref(pkt); continue; }
        if (avcodec_send_packet(dctx, pkt) < 0) { av_packet_unref(pkt); continue; }
        while (avcodec_receive_frame(dctx, frame) >= 0 && frames < maxFrames) {
            if (frame->format != g_hw_fmt) { av_frame_unref(frame); continue; }  // must be a VAAPI surface
            ++frames;
            zm::hw::Surface s = be->acquire(reinterpret_cast<uint64_t>(frame));
            if (!s.owner) { av_frame_unref(frame); continue; }

            auto t0 = clk::now();
            auto regions = be->motion(s);
            auto t1 = clk::now(); t_motion += ms(t0, t1);

            if (!regions.empty()) {
                ++motion;
                for (auto& r : regions) {
                    auto p0 = clk::now();
                    auto tensor = be->preprocess(s, r);
                    auto p1 = clk::now(); t_pre += ms(p0, p1);
                    if (tensor.valid()) {
                        // DEBUG: dump the first preprocessed CHW tensor as a PPM so we
                        // can eyeball whether the model input is a sane letterboxed image.
                        if (infers == 0 && getenv("DUMP_PRE")) {
                            const float* p = static_cast<const float*>(tensor.ptr);
                            int net = tensor.net;
                            FILE* f = fopen("/tmp/vaapi_pre.ppm", "wb");
                            fprintf(f, "P6\n%d %d\n255\n", net, net);
                            for (int y = 0; y < net; ++y) for (int x = 0; x < net; ++x) {
                                for (int c = 0; c < 3; ++c) {
                                    int v = (int)(p[c*net*net + y*net + x] * 255.0f + 0.5f);
                                    unsigned char b = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
                                    fputc(b, f);
                                }
                            }
                            fclose(f);
                            fprintf(stderr, "dumped /tmp/vaapi_pre.ppm (net=%d)\n", net);
                        }
                        auto i0 = clk::now();
                        auto d = be->infer(tensor, 0.25f);
                        auto i1 = clk::now(); t_inf += ms(i0, i1);
                        ++infers; dets += (long)d.size();
                    }
                }
            }
            be->release(s);
            av_frame_unref(frame);
        }
        av_packet_unref(pkt);
    }
    double wall = ms(t_start, clk::now());

    printf("\n=== VAAPI backend smoke test ===\n");
    printf("frames decoded (VAAPI surface): %d\n", frames);
    printf("motion frames               : %d (%.0f%%)\n", motion, frames ? 100.0*motion/frames : 0);
    printf("inferences / detections     : %d / %ld\n", infers, dets);
    printf("avg motion gate  : %.2f ms/frame\n", frames ? t_motion/frames : 0);
    printf("avg preprocess   : %.2f ms/infer\n", infers ? t_pre/infers : 0);
    printf("avg infer (CPU EP): %.2f ms/infer\n", infers ? t_inf/infers : 0);
    printf("end-to-end       : %.0f ms total, %.1f fps\n", wall, frames ? frames*1000.0/wall : 0);

    av_frame_free(&frame); av_packet_free(&pkt);
    avcodec_free_context(&dctx); av_buffer_unref(&hwdev); avformat_close_input(&fmt);
    return 0;
}

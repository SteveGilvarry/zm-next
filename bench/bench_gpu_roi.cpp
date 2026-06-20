// bench_gpu_roi — the GPU zero-copy version of the ROI cascade: NVDEC surface
// stays in VRAM, motion + crop + inference all run on the device.
//
// Drives the real decode_ffmpeg plugin in CUDA mode (emits ZM_HW_CUDA surfaces),
// then per frame: on-GPU luma-diff motion (cuda_motion_bbox), full-frame GPU
// detect, and — when motion — a zero-copy ROI-crop GPU detect (cuda_infer_nv12
// with a crop rect). Nothing but a tiny motion grid + the small output tensor
// ever crosses PCIe; the frame itself never leaves the GPU.
//
// Reuses the plugin's own detect_cuda kernels (compiled into this target).

#include "zm_plugin.h"
#include "detect_cuda.hpp"            // cuda_infer_nv12, cuda_motion_bbox, MotionRoi, Box
#include <onnxruntime_cxx_api.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
}

#include <dlfcn.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using zm::detect::Box;
using zm::detect::MotionRoi;
using clk = std::chrono::steady_clock;
static double ms(clk::time_point t) { return std::chrono::duration<double, std::milli>(clk::now() - t).count(); }

// ---- compact demux to Annex-B packets (file or rtsp://) ----
struct Src { int w = 0, h = 0; std::string codec; std::vector<std::vector<uint8_t>> pkts; };
static bool demux(const std::string& url, int maxf, Src& s) {
    AVFormatContext* f = nullptr; AVDictionary* o = nullptr;
    if (url.rfind("rtsp://", 0) == 0) { av_dict_set(&o, "rtsp_transport", "tcp", 0); av_dict_set(&o, "max_delay", "500000", 0); }
    if (avformat_open_input(&f, url.c_str(), nullptr, &o) < 0) { av_dict_free(&o); return false; }
    av_dict_free(&o);
    avformat_find_stream_info(f, nullptr);
    int v = av_find_best_stream(f, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (v < 0) { avformat_close_input(&f); return false; }
    AVCodecParameters* p = f->streams[v]->codecpar;
    s.w = p->width; s.h = p->height; s.codec = avcodec_get_name(p->codec_id);
    AVBSFContext* bsf = nullptr;
    if (p->extradata && p->extradata_size > 0 && p->extradata[0] == 1) {
        const char* bn = (p->codec_id == AV_CODEC_ID_HEVC) ? "hevc_mp4toannexb" : "h264_mp4toannexb";
        const AVBitStreamFilter* bf = av_bsf_get_by_name(bn);
        if (bf && av_bsf_alloc(bf, &bsf) == 0) { avcodec_parameters_copy(bsf->par_in, p); if (av_bsf_init(bsf) < 0) { av_bsf_free(&bsf); bsf = nullptr; } }
    }
    AVPacket* pkt = av_packet_alloc();
    auto take = [&](AVPacket* q) { s.pkts.emplace_back(q->data, q->data + q->size); };
    while ((int)s.pkts.size() < maxf && av_read_frame(f, pkt) >= 0) {
        if (pkt->stream_index == v) {
            if (bsf) { if (av_bsf_send_packet(bsf, pkt) == 0) { AVPacket* g = av_packet_alloc(); while (av_bsf_receive_packet(bsf, g) == 0) { take(g); av_packet_unref(g); } av_packet_free(&g); } }
            else take(pkt);
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt); if (bsf) av_bsf_free(&bsf); avformat_close_input(&f);
    return !s.pkts.empty();
}

// ---- shared state for the decode host callback ----
struct Ctx {
    Ort::Session* sess = nullptr;
    std::string in, out;
    int net = 640; float conf = 0.25f;
    int ds = 8, thr = 25, minchg = 0;
    std::vector<uint8_t> prev;
    // stats
    int frames = 0, motion = 0, inf_full = 0, inf_gate = 0, inf_roi = 0;
    long det_full = 0, det_gate = 0, det_roi = 0, roi_only = 0, full_only = 0, matched = 0;
    double t_full = 0, t_roi = 0, t_motion = 0;
};

static float iou(const Box& a, const Box& b) {
    float ix = std::max(0.f, std::min(a.x + a.w, b.x + b.w) - std::max(a.x, b.x));
    float iy = std::max(0.f, std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y));
    float in = ix * iy, un = a.w * a.h + b.w * b.h - in; return un > 0 ? in / un : 0.f;
}

static void on_frame(void* vc, const void* buf, size_t size) {
    Ctx* c = static_cast<Ctx*>(vc);
    const auto* hdr = static_cast<const zm_frame_hdr_t*>(buf);
    if (hdr->hw_type != ZM_HW_CUDA || size < sizeof(zm_frame_hdr_t) + sizeof(zm_gpu_frame_t)) return;
    const auto* g = reinterpret_cast<const zm_gpu_frame_t*>(static_cast<const uint8_t*>(buf) + sizeof(zm_frame_hdr_t));
    const uint64_t yp = g->plane_ptr[0], uvp = g->plane_ptr[1];
    const int ypitch = (int)g->linesize[0], uvpitch = (int)g->linesize[1];
    const int w = (int)g->width, h = (int)g->height;
    if (!yp || !uvp) return;

    auto tm = clk::now();
    MotionRoi m = zm::detect::cuda_motion_bbox(yp, ypitch, w, h, c->prev, c->ds, c->thr, c->minchg);
    c->t_motion += ms(tm);

    auto tf = clk::now();
    auto bf = zm::detect::cuda_infer_nv12(*c->sess, c->in, c->out, yp, ypitch, uvp, uvpitch, w, h, c->net, c->conf, {});
    c->t_full += ms(tf);
    ++c->frames; ++c->inf_full; c->det_full += (long)bf.size();

    if (!m.active) return;
    ++c->motion; ++c->inf_gate; c->det_gate += (long)bf.size();

    auto tr = clk::now();
    auto br = zm::detect::cuda_infer_nv12(*c->sess, c->in, c->out, yp, ypitch, uvp, uvpitch, w, h, c->net, c->conf, {}, m.x, m.y, m.w, m.h);
    c->t_roi += ms(tr);
    ++c->inf_roi; c->det_roi += (long)br.size();

    std::vector<char> used(bf.size(), 0);
    for (auto& b : br) {
        int best = -1; float bi = 0.3f;
        for (size_t k = 0; k < bf.size(); ++k)
            if (!used[k] && bf[k].class_id == b.class_id) { float v = iou(b, bf[k]); if (v > bi) { bi = v; best = (int)k; } }
        if (best >= 0) { used[best] = 1; ++c->matched; } else ++c->roi_only;
    }
    for (char u : used) if (!u) ++c->full_only;
}

struct Plugin { void* h = nullptr; void (*init)(zm_plugin_t*) = nullptr;
    bool open(const std::string& p) { h = dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!h) { fprintf(stderr, "dlopen %s: %s\n", p.c_str(), dlerror()); return false; }
        init = (void(*)(zm_plugin_t*))dlsym(h, "zm_plugin_init"); return init; } };

int main(int argc, char** argv) {
    std::string input, model, plugdir = "plugins";
    int maxf = 300, thr = 25, minchg = -1;
    for (int i = 1; i < argc; ++i) { std::string a = argv[i]; auto nx = [&]{ return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "--input") input = nx(); else if (a == "--model") model = nx(); else if (a == "--plugins") plugdir = nx();
        else if (a == "--max-frames") maxf = std::atoi(nx()); else if (a == "--motion-threshold") thr = std::atoi(nx());
        else if (a == "--min-changed") minchg = std::atoi(nx()); }
    if (input.empty() || model.empty()) { fprintf(stderr, "need --input and --model\n"); return 2; }

    Src s;
    fprintf(stderr, "demuxing up to %d packets from %s ...\n", maxf, input.c_str());
    if (!demux(input, maxf, s)) { fprintf(stderr, "demux failed\n"); return 1; }

    Ctx ctx;
    ctx.ds = 8; ctx.thr = thr;
    ctx.minchg = (minchg < 0) ? std::max(40, (s.w / ctx.ds) * (s.h / ctx.ds) / 200) : minchg;

    Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "gpuroi");
    Ort::SessionOptions so; so.SetIntraOpNumThreads(1); so.SetGraphOptimizationLevel(ORT_ENABLE_ALL);
    OrtCUDAProviderOptions cu{}; so.AppendExecutionProvider_CUDA(cu);
    Ort::Session sess(env, model.c_str(), so);
    Ort::AllocatorWithDefaultOptions al;
    ctx.sess = &sess; ctx.in = sess.GetInputNameAllocated(0, al).get(); ctx.out = sess.GetOutputNameAllocated(0, al).get();

    Plugin dec; if (!dec.open(plugdir + "/decode_ffmpeg/decode_ffmpeg.so")) return 1;
    zm_plugin_t p{}; dec.init(&p);
    zm_host_api_t host{}; host.on_frame = on_frame;
    char cfg[256]; std::snprintf(cfg, sizeof(cfg), "{\"hwaccel\":\"cuda\",\"output_format\":\"yuv420p\",\"codec\":\"%s\",\"scale\":\"orig\"}", s.codec.c_str());
    if (p.start(&p, &host, &ctx, cfg) != 0) { fprintf(stderr, "decode start (cuda) failed\n"); return 1; }

    printf("\n=== GPU zero-copy ROI cascade: %dx%d %s | NVDEC->CUDA detect | grid %dx%d min_changed=%d ===\n",
           s.w, s.h, s.codec.c_str(), s.w / ctx.ds, s.h / ctx.ds, ctx.minchg);
    std::vector<uint8_t> buf;
    for (auto& pk : s.pkts) {
        buf.resize(sizeof(zm_frame_hdr_t) + pk.size());
        auto* hd = reinterpret_cast<zm_frame_hdr_t*>(buf.data()); *hd = {};
        hd->hw_type = ZM_FRAME_COMPRESSED; hd->bytes = (uint32_t)pk.size();
        std::memcpy(buf.data() + sizeof(zm_frame_hdr_t), pk.data(), pk.size());
        p.on_frame(&p, buf.data(), buf.size());
    }
    p.stop(&p);

    Ctx& c = ctx;
    double idle = c.frames ? 100.0 * (c.frames - c.motion) / c.frames : 0;
    printf("\nframes: %d   motion frames: %d   idle (skipped): %.1f%%\n", c.frames, c.motion, idle);
    printf("on-GPU motion check: %.3f ms/frame\n", c.frames ? c.t_motion / c.frames : 0);
    printf("\n%-14s %10s %12s %14s\n", "strategy", "inferences", "detections", "avg ms/inf");
    printf("%-14s %10d %12ld %14.2f\n", "full-frame",  c.inf_full, c.det_full, c.inf_full ? c.t_full / c.inf_full : 0);
    printf("%-14s %10d %12ld %14s\n",  "motion-gated", c.inf_gate, c.det_gate, "(=full)");
    printf("%-14s %10d %12ld %14.2f\n", "roi-crop",    c.inf_roi,  c.det_roi,  c.inf_roi ? c.t_roi / c.inf_roi : 0);
    printf("\ngating saves %.1f%% of inferences (%d -> %d).\n", idle, c.inf_full, c.inf_gate);
    printf("ROI vs full-frame on motion frames:  matched %ld | ROI-only %ld (full MISSED) | full-only %ld (ROI blind spot)\n",
           c.matched, c.roi_only, c.full_only);
    printf("note: avg ms/inf here is pure GPU preprocess+infer (no CPU 4K letterbox) — compare to bench_roi_cascade.\n\n");
    return 0;
}

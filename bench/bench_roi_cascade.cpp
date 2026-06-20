// bench_roi_cascade — quantify a Frigate-style motion-gated / ROI-crop detection
// cascade vs naive full-frame detection, on the SAME frames.
//
// Three strategies, evaluated per decoded frame:
//   full-frame   : run YOLO on every frame (baseline)
//   motion-gated : run YOLO (full frame) only when cheap luma-diff sees motion
//   roi-crop     : run YOLO only on the motion region's crop (boxes mapped back)
//
// Reports: % idle frames skipped (gating compute saving), inferences run, total
// detections, and the cross-comparison that matters — boxes ROI finds that the
// full frame MISSES (small/distant moving objects), and boxes full-frame finds
// that ROI misses (static objects outside the motion region = the ROI blind spot).
//
// Decodes via libav directly (CPU) and runs ORT (CUDA EP if built) reusing the
// detect_onnx plugin's own postprocess header — so it measures the algorithm, not
// plumbing. The GPU zero-copy crop kernel is a separate follow-up.

#include "detect_postprocess.hpp"   // plugins/detect_onnx — Letterbox/decode/Box
#include <onnxruntime_cxx_api.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using zm::detect::Box;
using clk = std::chrono::steady_clock;

struct Rect { int x = 0, y = 0, w = 0, h = 0; };

// ----- cheap motion: downsampled luma diff -> bounding box of changed area -----
struct Motion { bool active = false; Rect bbox; int changed = 0; };

static Motion motion_from_luma(const uint8_t* y, int y_pitch, int w, int h,
                               std::vector<uint8_t>& prev, int ds,
                               int pix_thr, int min_changed) {
    const int sw = w / ds, sh = h / ds;
    std::vector<uint8_t> cur((size_t)sw * sh);
    for (int j = 0; j < sh; ++j)
        for (int i = 0; i < sw; ++i)
            cur[(size_t)j * sw + i] = y[(j * ds) * y_pitch + (i * ds)];

    Motion m;
    if (prev.size() == cur.size()) {
        int minx = sw, miny = sh, maxx = -1, maxy = -1, n = 0;
        for (int j = 0; j < sh; ++j) {
            for (int i = 0; i < sw; ++i) {
                int d = std::abs((int)cur[(size_t)j * sw + i] - (int)prev[(size_t)j * sw + i]);
                if (d > pix_thr) {
                    ++n;
                    minx = std::min(minx, i); miny = std::min(miny, j);
                    maxx = std::max(maxx, i); maxy = std::max(maxy, j);
                }
            }
        }
        m.changed = n;
        if (n >= min_changed && maxx >= minx) {
            m.active = true;
            // back to full-res, expand 20% margin, clamp
            int x0 = minx * ds, y0 = miny * ds, x1 = (maxx + 1) * ds, y1 = (maxy + 1) * ds;
            int mx = (x1 - x0) / 5, my = (y1 - y0) / 5;
            x0 = std::max(0, x0 - mx); y0 = std::max(0, y0 - my);
            x1 = std::min(w, x1 + mx); y1 = std::min(h, y1 + my);
            m.bbox = {x0, y0, x1 - x0, y1 - y0};
        }
    }
    prev.swap(cur);
    return m;
}

// ----- ORT detection on a contiguous RGB24 buffer of dims w x h -----
struct Detector {
    Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "roi"};
    Ort::Session sess{nullptr};
    Ort::SessionOptions so;
    std::string in, out;
    int net = 640;
    float conf = 0.25f;

    void load(const std::string& model, bool cuda) {
        so.SetIntraOpNumThreads(1);
        so.SetGraphOptimizationLevel(ORT_ENABLE_ALL);
#ifdef BENCH_WITH_CUDA
        if (cuda) { OrtCUDAProviderOptions o{}; so.AppendExecutionProvider_CUDA(o); }
#endif
        sess = Ort::Session(env, model.c_str(), so);
        Ort::AllocatorWithDefaultOptions a;
        in = sess.GetInputNameAllocated(0, a).get();
        out = sess.GetOutputNameAllocated(0, a).get();
    }
    // returns boxes in the buffer's own pixel coords
    std::vector<Box> run(const uint8_t* rgb, int w, int h) {
        zm::detect::Letterbox lb = zm::detect::compute_letterbox(w, h, net);
        std::vector<float> input((size_t)3 * net * net);
        zm::detect::letterbox_rgb_to_chw(rgb, lb, input.data());
        Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::array<int64_t, 4> shape{1, 3, net, net};
        Ort::Value t = Ort::Value::CreateTensor<float>(mem, input.data(), input.size(),
                                                       shape.data(), shape.size());
        const char* ins[] = {in.c_str()};
        const char* outs[] = {out.c_str()};
        auto o = sess.Run(Ort::RunOptions{nullptr}, ins, &t, 1, outs, 1);
        const float* d = o[0].GetTensorData<float>();
        auto sh = o[0].GetTensorTypeAndShapeInfo().GetShape();
        if (sh.empty() || sh.back() != 6) return {};
        int num = sh.size() == 3 ? (int)sh[1] : (int)sh[0];
        return zm::detect::decode_nms_free(d, num, lb, conf, {});
    }
};

// IoU of two xywh boxes in the same coord space
static float iou(const Box& a, const Box& b) {
    float ax2 = a.x + a.w, ay2 = a.y + a.h, bx2 = b.x + b.w, by2 = b.y + b.h;
    float ix = std::max(0.f, std::min(ax2, bx2) - std::max(a.x, b.x));
    float iy = std::max(0.f, std::min(ay2, by2) - std::max(a.y, b.y));
    float inter = ix * iy, uni = a.w * a.h + b.w * b.h - inter;
    return uni > 0 ? inter / uni : 0.f;
}

int main(int argc, char** argv) {
    std::string input, model;
    int max_frames = 300, ds = 8, pix_thr = 25, min_changed = -1;
    bool cuda = true;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto nx = [&]{ return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "--input") input = nx();
        else if (a == "--model") model = nx();
        else if (a == "--max-frames") max_frames = std::atoi(nx());
        else if (a == "--motion-threshold") pix_thr = std::atoi(nx());
        else if (a == "--min-changed") min_changed = std::atoi(nx());
        else if (a == "--cpu") cuda = false;
    }
    if (input.empty() || model.empty()) { fprintf(stderr, "need --input and --model\n"); return 2; }

    AVFormatContext* fmt = nullptr;
    AVDictionary* opt = nullptr;
    if (input.rfind("rtsp://", 0) == 0) { av_dict_set(&opt, "rtsp_transport", "tcp", 0); av_dict_set(&opt, "max_delay", "500000", 0); }
    if (avformat_open_input(&fmt, input.c_str(), nullptr, &opt) < 0) { fprintf(stderr, "open failed\n"); return 1; }
    av_dict_free(&opt);
    avformat_find_stream_info(fmt, nullptr);
    int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vs < 0) { fprintf(stderr, "no video\n"); return 1; }
    AVCodecParameters* par = fmt->streams[vs]->codecpar;
    const AVCodec* dec = avcodec_find_decoder(par->codec_id);
    AVCodecContext* cc = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(cc, par);
    avcodec_open2(cc, dec, nullptr);
    const int W = par->width, H = par->height;
    // Require a meaningful changed AREA, not scattered sensor noise: default to
    // ~0.5% of the downsampled motion grid (override with --min-changed).
    if (min_changed < 0) min_changed = std::max(40, (W / ds) * (H / ds) / 200);

    Detector det; det.load(model, cuda);
    printf("\n=== ROI cascade: %dx%d %s  | %s EP | model %s ===\n",
           W, H, avcodec_get_name(par->codec_id), cuda ? "CUDA" : "CPU",
           model.substr(model.find_last_of('/') + 1).c_str());
    printf("motion: downscale=%d  pixel_thr=%d  min_changed_cells=%d (grid %dx%d)\n",
           ds, pix_thr, min_changed, W / ds, H / ds);

    SwsContext* sws = nullptr;
    std::vector<uint8_t> rgb((size_t)W * H * 3), crop, prevGray;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* fr = av_frame_alloc();

    // accumulators
    int frames = 0, motion_frames = 0;
    int inf_full = 0, inf_gate = 0, inf_roi = 0;
    long det_full = 0, det_gate = 0, det_roi = 0;
    long roi_only = 0, full_only = 0, matched = 0;
    double t_full = 0, t_roi = 0;

    auto handle = [&](AVFrame* f) {
        if (frames >= max_frames) return;
        // luma-diff motion (uses decoded Y plane directly)
        Motion m = motion_from_luma(f->data[0], f->linesize[0], W, H, prevGray, ds, pix_thr, min_changed);
        // convert to RGB24 for detection
        if (!sws) sws = sws_getContext(W, H, (AVPixelFormat)f->format, W, H, AV_PIX_FMT_RGB24,
                                       SWS_BILINEAR, nullptr, nullptr, nullptr);
        uint8_t* dst[1] = {rgb.data()}; int dsts[1] = {W * 3};
        sws_scale(sws, f->data, f->linesize, 0, H, dst, dsts);
        ++frames;

        // (1) full-frame — baseline, every frame
        auto tf = clk::now();
        auto bf = det.run(rgb.data(), W, H);
        t_full += std::chrono::duration<double, std::milli>(clk::now() - tf).count();
        ++inf_full; det_full += (long)bf.size();

        if (!m.active) return;   // gated & roi skip idle frames
        ++motion_frames;
        ++inf_gate; det_gate += (long)bf.size();   // gated reuses full-frame detection

        // (2) roi-crop — detect only the motion region, map boxes back to full coords
        const Rect& r = m.bbox;
        crop.assign((size_t)r.w * r.h * 3, 0);
        for (int j = 0; j < r.h; ++j)
            std::memcpy(&crop[(size_t)j * r.w * 3], &rgb[((size_t)(r.y + j) * W + r.x) * 3], (size_t)r.w * 3);
        auto tr = clk::now();
        auto br = det.run(crop.data(), r.w, r.h);
        t_roi += std::chrono::duration<double, std::milli>(clk::now() - tr).count();
        ++inf_roi;
        for (auto& b : br) { b.x += r.x; b.y += r.y; }   // crop -> full coords
        det_roi += (long)br.size();

        // cross-compare ROI vs full on this frame (IoU>0.3, same class)
        std::vector<char> usedF(bf.size(), 0);
        for (auto& b : br) {
            int best = -1; float bi = 0.3f;
            for (size_t k = 0; k < bf.size(); ++k)
                if (!usedF[k] && bf[k].class_id == b.class_id) { float v = iou(b, bf[k]); if (v > bi) { bi = v; best = (int)k; } }
            if (best >= 0) { usedF[best] = 1; ++matched; } else ++roi_only;
        }
        for (char u : usedF) if (!u) ++full_only;
    };

    while (frames < max_frames && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == vs && avcodec_send_packet(cc, pkt) == 0)
            while (avcodec_receive_frame(cc, fr) == 0) handle(fr);
        av_packet_unref(pkt);
    }

    auto fps = [](int n, double ms) { return ms > 0 ? n * 1000.0 / ms : 0; };
    const double idle_pct = frames ? 100.0 * (frames - motion_frames) / frames : 0;
    printf("\nframes: %d   motion frames: %d   idle (skipped): %.1f%%\n", frames, motion_frames, idle_pct);
    printf("\n%-14s %10s %12s %14s\n", "strategy", "inferences", "detections", "avg ms/inf");
    printf("%-14s %10d %12ld %14.2f\n", "full-frame",  inf_full, det_full, inf_full ? t_full / inf_full : 0);
    printf("%-14s %10d %12ld %14s\n",  "motion-gated", inf_gate, det_gate, "(=full)");
    printf("%-14s %10d %12ld %14.2f\n", "roi-crop",    inf_roi,  det_roi,  inf_roi ? t_roi / inf_roi : 0);
    printf("\ngating saves %.1f%% of inferences (%d -> %d).\n", idle_pct, inf_full, inf_gate);
    printf("ROI vs full-frame on motion frames:\n");
    printf("  matched (both found): %ld\n", matched);
    printf("  ROI-only  (full-frame MISSED, e.g. small/distant): %ld\n", roi_only);
    printf("  full-only (ROI blind spot: static / outside motion): %ld\n", full_only);
    printf("\n");

    av_frame_free(&fr); av_packet_free(&pkt);
    if (sws) sws_freeContext(sws);
    avcodec_free_context(&cc);
    avformat_close_input(&fmt);
    return 0;
}

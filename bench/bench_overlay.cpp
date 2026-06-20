// bench_overlay — the BURNED-IN overlay path, in C++. Decodes a clip, draws the
// detect_onnx plugin's boxes+labels onto each frame (no external gfx libs), and
// RE-ENCODES an annotated H.264 MP4 (+ sample JPEGs). This is the "bake boxes
// into the pixels" path; it necessarily re-encodes. The alternative (client-side
// overlay from the Event.DETECTION metadata) leaves the stream untouched and does
// NOT encode — see render_samples.py.
//
//   bench_overlay --input clip.mp4 --events dets.jsonl --out-dir samples/
//
// Detections come from bench_events (the real pipeline), so this only draws.

#include "font_glyphs.h"
#include <nlohmann/json.hpp>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using json = nlohmann::json;
struct Det { int x, y, w, h; float conf; std::string label; };

// ---- drawing on a packed RGB24 buffer ----
static inline void px(uint8_t* rgb, int W, int H, int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    uint8_t* p = rgb + (static_cast<size_t>(y) * W + x) * 3;
    p[0] = r; p[1] = g; p[2] = b;
}
static void rect(uint8_t* rgb, int W, int H, int x, int y, int w, int h, int t,
                 uint8_t r, uint8_t g, uint8_t b) {
    for (int i = -t / 2; i <= t / 2; ++i) {
        for (int xx = x; xx < x + w; ++xx) { px(rgb, W, H, xx, y + i, r, g, b); px(rgb, W, H, xx, y + h + i, r, g, b); }
        for (int yy = y; yy < y + h; ++yy) { px(rgb, W, H, x + i, yy, r, g, b); px(rgb, W, H, x + w + i, yy, r, g, b); }
    }
}
static void fill(uint8_t* rgb, int W, int H, int x, int y, int w, int h,
                 uint8_t r, uint8_t g, uint8_t b) {
    for (int yy = y; yy < y + h; ++yy) for (int xx = x; xx < x + w; ++xx) px(rgb, W, H, xx, yy, r, g, b);
}
static void text(uint8_t* rgb, int W, int H, int x, int y, const std::string& s, int sc,
                 uint8_t r, uint8_t g, uint8_t b) {
    int cx = x;
    for (char ch : s) {
        if (ch < FONT_FIRST || ch >= FONT_FIRST + FONT_COUNT) { cx += FONT_W * sc; continue; }
        const uint8_t* gl = FONT[ch - FONT_FIRST];
        for (int row = 0; row < FONT_H; ++row)
            for (int col = 0; col < FONT_W; ++col)
                if (gl[row] & (1 << col))
                    for (int dy = 0; dy < sc; ++dy) for (int dx = 0; dx < sc; ++dx)
                        px(rgb, W, H, cx + col * sc + dx, y + row * sc + dy, r, g, b);
        cx += FONT_W * sc;
    }
}

static void draw_dets(uint8_t* rgb, int W, int H, const std::vector<Det>& dets) {
    const int sc = std::max(2, W / 640);          // scale boxes/text to frame size
    for (const auto& d : dets) {
        rect(rgb, W, H, d.x, d.y, d.w, d.h, sc, 0, 230, 0);
        char buf[64]; std::snprintf(buf, sizeof(buf), "%s %.2f", d.label.c_str(), d.conf);
        const std::string lab = buf;
        const int tw = static_cast<int>(lab.size()) * FONT_W * sc, th = FONT_H * sc;
        int ly = d.y - th - 2; if (ly < 0) ly = d.y + 2;
        fill(rgb, W, H, d.x, ly, tw + 4, th + 2, 0, 230, 0);
        text(rgb, W, H, d.x + 2, ly + 1, lab, sc, 0, 0, 0);
    }
}

// ---- minimal libav encoders ----
static void save_jpeg(const uint8_t* rgb, int W, int H, const std::string& path) {
    const AVCodec* c = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    AVCodecContext* e = avcodec_alloc_context3(c);
    e->width = W; e->height = H; e->pix_fmt = AV_PIX_FMT_YUVJ420P;
    e->time_base = {1, 25};
    avcodec_open2(e, c, nullptr);
    AVFrame* f = av_frame_alloc(); f->format = e->pix_fmt; f->width = W; f->height = H;
    av_frame_get_buffer(f, 32);
    SwsContext* sws = sws_getContext(W, H, AV_PIX_FMT_RGB24, W, H, AV_PIX_FMT_YUVJ420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
    const uint8_t* src[1] = {rgb}; int ss[1] = {W * 3};
    sws_scale(sws, src, ss, 0, H, f->data, f->linesize);
    f->pts = 0;
    AVPacket* p = av_packet_alloc();
    if (avcodec_send_frame(e, f) == 0 && avcodec_receive_packet(e, p) == 0) {
        FILE* fp = std::fopen(path.c_str(), "wb");
        if (fp) { std::fwrite(p->data, 1, p->size, fp); std::fclose(fp); }
    }
    av_packet_free(&p); sws_freeContext(sws); av_frame_free(&f); avcodec_free_context(&e);
}

struct Mp4 {
    AVFormatContext* oc = nullptr; AVCodecContext* enc = nullptr; AVStream* st = nullptr;
    SwsContext* sws = nullptr; AVFrame* yuv = nullptr; int W = 0, H = 0;
    bool open(const std::string& path, int w, int h, AVRational fps) {
        W = w; H = h;
        avformat_alloc_output_context2(&oc, nullptr, nullptr, path.c_str());
        if (!oc) return false;
        const AVCodec* c = avcodec_find_encoder_by_name("libx264");
        if (!c) c = avcodec_find_encoder(AV_CODEC_ID_H264);
        st = avformat_new_stream(oc, nullptr);
        enc = avcodec_alloc_context3(c);
        enc->width = w; enc->height = h; enc->pix_fmt = AV_PIX_FMT_YUV420P;
        enc->time_base = {fps.den, fps.num}; enc->framerate = fps; enc->gop_size = 30;
        av_opt_set(enc->priv_data, "preset", "veryfast", 0);
        av_opt_set(enc->priv_data, "crf", "23", 0);
        if (oc->oformat->flags & AVFMT_GLOBALHEADER) enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (avcodec_open2(enc, c, nullptr) < 0) return false;
        avcodec_parameters_from_context(st->codecpar, enc);
        st->time_base = enc->time_base;
        if (avio_open(&oc->pb, path.c_str(), AVIO_FLAG_WRITE) < 0) return false;
        if (avformat_write_header(oc, nullptr) < 0) return false;
        yuv = av_frame_alloc(); yuv->format = enc->pix_fmt; yuv->width = w; yuv->height = h;
        av_frame_get_buffer(yuv, 32);
        sws = sws_getContext(w, h, AV_PIX_FMT_RGB24, w, h, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
        return true;
    }
    void write(const uint8_t* rgb, int64_t pts) {
        const uint8_t* src[1] = {rgb}; int ss[1] = {W * 3};
        sws_scale(sws, src, ss, 0, H, yuv->data, yuv->linesize);
        yuv->pts = pts;
        AVPacket* p = av_packet_alloc();
        if (avcodec_send_frame(enc, yuv) == 0)
            while (avcodec_receive_packet(enc, p) == 0) {
                av_packet_rescale_ts(p, enc->time_base, st->time_base); p->stream_index = 0;
                av_interleaved_write_frame(oc, p); av_packet_unref(p);
            }
        av_packet_free(&p);
    }
    void close() {
        AVPacket* p = av_packet_alloc();
        avcodec_send_frame(enc, nullptr);
        while (avcodec_receive_packet(enc, p) == 0) {
            av_packet_rescale_ts(p, enc->time_base, st->time_base); p->stream_index = 0;
            av_interleaved_write_frame(oc, p); av_packet_unref(p);
        }
        av_packet_free(&p);
        av_write_trailer(oc);
        avio_closep(&oc->pb);
        sws_freeContext(sws); av_frame_free(&yuv); avcodec_free_context(&enc); avformat_free_context(oc);
    }
};

int main(int argc, char** argv) {
    std::string input, events, outdir = "samples";
    for (int i = 1; i < argc; ++i) { std::string a = argv[i]; auto nx = [&]{ return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "--input") input = nx(); else if (a == "--events") events = nx(); else if (a == "--out-dir") outdir = nx(); }
    if (input.empty() || events.empty()) { fprintf(stderr, "need --input and --events\n"); return 2; }

    // Load detections by frame.
    std::map<int, std::vector<Det>> byframe;
    { FILE* fp = std::fopen(events.c_str(), "r"); if (!fp) { fprintf(stderr, "cannot open %s\n", events.c_str()); return 1; }
      char* line = nullptr; size_t cap = 0; ssize_t n;
      while ((n = getline(&line, &cap, fp)) > 0) {
          try { auto j = json::parse(line); int fr = j["frame"];
              for (auto& d : j["event"]["detections"]) {
                  auto bb = d["bbox"]; byframe[fr].push_back({(int)bb[0], (int)bb[1], (int)bb[2], (int)bb[3],
                      d.value("confidence", 0.0f), d.value("label", std::string("?"))}); }
          } catch (...) {} }
      free(line); std::fclose(fp); }

    // Decode the clip.
    AVFormatContext* in = nullptr;
    if (avformat_open_input(&in, input.c_str(), nullptr, nullptr) < 0) { fprintf(stderr, "open failed\n"); return 1; }
    avformat_find_stream_info(in, nullptr);
    int v = av_find_best_stream(in, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    AVStream* vs = in->streams[v];
    const AVCodec* dec = avcodec_find_decoder(vs->codecpar->codec_id);
    AVCodecContext* dc = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(dc, vs->codecpar);
    avcodec_open2(dc, dec, nullptr);
    const int W = vs->codecpar->width, H = vs->codecpar->height;
    AVRational fps = vs->avg_frame_rate.num ? vs->avg_frame_rate : AVRational{25, 1};

    Mp4 mp4; if (!mp4.open(outdir + "/annotated_cpp.mp4", W, H, fps)) { fprintf(stderr, "encoder open failed\n"); return 1; }
    SwsContext* toRgb = sws_getContext(W, H, dc->pix_fmt, W, H, AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
    std::vector<uint8_t> rgb(static_cast<size_t>(W) * H * 3);

    AVPacket* pkt = av_packet_alloc(); AVFrame* fr = av_frame_alloc();
    int idx = 0, saved = 0, last = -100, best = -1, best_n = 0;
    auto handle = [&](AVFrame* f) {
        uint8_t* dst[1] = {rgb.data()}; int ds[1] = {W * 3};
        sws_scale(toRgb, f->data, f->linesize, 0, H, dst, ds);
        auto it = byframe.find(idx);
        if (it != byframe.end()) {
            draw_dets(rgb.data(), W, H, it->second);
            if ((int)it->second.size() > best_n) { best_n = it->second.size(); best = idx; }
            if (saved < 8 && idx - last >= 8) {
                char nm[256]; std::snprintf(nm, sizeof(nm), "%s/cpp_sample_%02d.jpg", outdir.c_str(), saved);
                save_jpeg(rgb.data(), W, H, nm); ++saved; last = idx;
            }
        }
        mp4.write(rgb.data(), idx);
        ++idx;
    };
    while (av_read_frame(in, pkt) >= 0) {
        if (pkt->stream_index == v && avcodec_send_packet(dc, pkt) == 0)
            while (avcodec_receive_frame(dc, fr) == 0) handle(fr);
        av_packet_unref(pkt);
    }
    avcodec_send_packet(dc, nullptr);
    while (avcodec_receive_frame(dc, fr) == 0) handle(fr);
    mp4.close();

    av_frame_free(&fr); av_packet_free(&pkt); sws_freeContext(toRgb); avcodec_free_context(&dc); avformat_close_input(&in);
    fprintf(stderr, "frames=%d  annotated stills=%d  busiest=frame %d (%d dets)  -> %s/annotated_cpp.mp4\n",
            idx, saved, best, best_n, outdir.c_str());
    return 0;
}

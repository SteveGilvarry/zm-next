// bench_events — run the REAL pipeline (decode_ffmpeg -> detect_onnx plugins) over
// a clip/rtsp source and dump the detection events the detect plugin actually
// publishes, one JSON line per frame: {"frame":N,"event":<Event.DETECTION>}.
//
// This is the faithful detector output (our C++ plugin, our model, our letterbox/
// decode/cascade) — unlike a separate Python inference. A renderer can then draw
// these boxes onto the clip (a viewer's job; the stream itself is untouched).
//
//   bench_events --input <file|rtsp> --model m.onnx --plugins <dir> [--roi] [--out f.jsonl]

#include "zm_plugin.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
}

#include <dlfcn.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

struct Src { int w = 0, h = 0; double fps = 0; std::string codec; std::vector<std::vector<uint8_t>> pkts; };
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
    s.fps = f->streams[v]->avg_frame_rate.num ? av_q2d(f->streams[v]->avg_frame_rate) : 0.0;
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

struct Plugin { void* h = nullptr; void (*init)(zm_plugin_t*) = nullptr;
    bool open(const std::string& p) { h = dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!h) { fprintf(stderr, "dlopen %s: %s\n", p.c_str(), dlerror()); return false; }
        init = (void(*)(zm_plugin_t*))dlsym(h, "zm_plugin_init"); return init; } };

// Shared capture state: decode forwards frames to detect; detect's published
// events are tagged with the current frame index.
struct Cap { int frame = 0; zm_plugin_t* det = nullptr; std::vector<std::pair<int, std::string>> evts; };
static void dec_on_frame(void* c, const void* buf, size_t n) {
    Cap* x = static_cast<Cap*>(c);
    if (x->det && x->det->on_frame) x->det->on_frame(x->det, buf, n);  // -> detect (-> det_publish)
    ++x->frame;
}
static void det_publish(void* c, const char* js) {
    Cap* x = static_cast<Cap*>(c);
    if (js) x->evts.emplace_back(x->frame, std::string(js));
}
static void quiet_log(void*, zm_log_level_t, const char*) {}

int main(int argc, char** argv) {
    std::string input, model, plugdir = "plugins", out = "detections.jsonl", vtt;
    int maxf = 100000; bool roi = false; double fps_override = 0;
    for (int i = 1; i < argc; ++i) { std::string a = argv[i]; auto nx = [&]{ return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "--input") input = nx(); else if (a == "--model") model = nx(); else if (a == "--plugins") plugdir = nx();
        else if (a == "--out") out = nx(); else if (a == "--vtt") vtt = nx(); else if (a == "--fps") fps_override = std::atof(nx());
        else if (a == "--max-frames") maxf = std::atoi(nx()); else if (a == "--roi") roi = true; }
    if (input.empty() || model.empty()) { fprintf(stderr, "need --input and --model\n"); return 2; }

    Src s;
    if (!demux(input, maxf, s)) { fprintf(stderr, "demux failed\n"); return 1; }
    fprintf(stderr, "source %dx%d %s, %zu packets\n", s.w, s.h, s.codec.c_str(), s.pkts.size());

    Plugin decL, detL;
    if (!decL.open(plugdir + "/decode_ffmpeg/decode_ffmpeg.so")) return 1;
    if (!detL.open(plugdir + "/detect_onnx/detect_onnx.so")) return 1;

    Cap cap;
    zm_plugin_t det{}; detL.init(&det); cap.det = &det;
    zm_host_api_t det_host{}; det_host.log = quiet_log; det_host.publish_evt = det_publish;
    char dcfg[512];
    std::snprintf(dcfg, sizeof(dcfg),
        "{\"model_path\":\"%s\",\"ep\":\"cuda\",\"conf_threshold\":0.25%s}",
        model.c_str(), roi ? ",\"roi_motion\":true,\"full_sweep_sec\":1.0" : "");
    if (det.start(&det, &det_host, &cap, dcfg) != 0) { fprintf(stderr, "detect start failed\n"); return 1; }

    zm_plugin_t dec{}; decL.init(&dec);
    zm_host_api_t dec_host{}; dec_host.log = quiet_log; dec_host.on_frame = dec_on_frame;
    char ccfg[256];
    std::snprintf(ccfg, sizeof(ccfg), "{\"hwaccel\":\"cuda\",\"output_format\":\"yuv420p\",\"codec\":\"%s\",\"scale\":\"orig\"}", s.codec.c_str());
    if (dec.start(&dec, &dec_host, &cap, ccfg) != 0) { fprintf(stderr, "decode start failed\n"); return 1; }

    std::vector<uint8_t> buf;
    for (auto& pk : s.pkts) {
        buf.resize(sizeof(zm_frame_hdr_t) + pk.size());
        auto* hd = reinterpret_cast<zm_frame_hdr_t*>(buf.data()); *hd = {};
        hd->hw_type = ZM_FRAME_COMPRESSED; hd->bytes = (uint32_t)pk.size();
        std::memcpy(buf.data() + sizeof(zm_frame_hdr_t), pk.data(), pk.size());
        dec.on_frame(&dec, buf.data(), buf.size());
    }
    dec.stop(&dec); det.stop(&det);

    FILE* fp = std::fopen(out.c_str(), "w");
    if (!fp) { fprintf(stderr, "cannot write %s\n", out.c_str()); return 1; }
    long total = 0;
    for (auto& e : cap.evts) { std::fprintf(fp, "{\"frame\":%d,\"event\":%s}\n", e.first, e.second.c_str()); ++total; }
    std::fclose(fp);
    fprintf(stderr, "frames=%d  detection-events=%ld  mode=%s  -> %s\n",
            cap.frame, total, roi ? "roi_motion cascade" : "full-frame", out.c_str());

    // Optional WebVTT metadata track: one timed cue per frame carrying the
    // Event.DETECTION JSON. A browser <track kind="metadata"> fires cuechange and
    // a canvas draws the boxes over the untouched video — toggleable, no re-encode.
    if (!vtt.empty()) {
        const double fps = fps_override > 0 ? fps_override : (s.fps > 0 ? s.fps : 25.0);
        FILE* vf = std::fopen(vtt.c_str(), "w");
        if (vf) {
            std::fprintf(vf, "WEBVTT\n\n");
            auto stamp = [](double t, char* o) {
                int hh = (int)(t / 3600); t -= hh * 3600;
                int mm = (int)(t / 60); t -= mm * 60;
                int ss = (int)t; int ms = (int)((t - ss) * 1000 + 0.5);
                std::snprintf(o, 16, "%02d:%02d:%02d.%03d", hh, mm, ss, ms);
            };
            char a[16], b[16];
            for (auto& e : cap.evts) {
                stamp(e.first / fps, a);
                stamp((e.first + 1) / fps, b);
                // Wrap with the detection resolution (sw/sh) so a viewer can scale
                // the boxes onto ANY display resolution (e.g. detect@720p, show@4K).
                std::fprintf(vf, "%s --> %s\n{\"sw\":%d,\"sh\":%d,\"event\":%s}\n\n",
                             a, b, s.w, s.h, e.second.c_str());
            }
            std::fclose(vf);
            fprintf(stderr, "wrote WebVTT metadata track (%.2f fps) -> %s\n", fps, vtt.c_str());
        }
    }
    return 0;
}

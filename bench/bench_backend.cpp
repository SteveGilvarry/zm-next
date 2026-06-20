// bench_backend — exercise the HwBackend abstraction end-to-end on real NVDEC
// surfaces. Drives the decode_ffmpeg plugin (CUDA), then for each surface runs the
// fused pattern THROUGH the backend interface: acquire -> motion -> preprocess ->
// infer -> release. Proves the surface lifetime (av_frame ref) and that the CUDA
// backend produces detections, without any CUDA specifics in this file.
//
//   bench_backend --input <file|rtsp> --model dyn.onnx --plugins <dir> [--roi]

#include "zm_plugin.h"
#include "hw_backend.hpp"

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

struct Src { int w = 0, h = 0; std::string codec; std::vector<std::vector<uint8_t>> pkts; };
static bool demux(const std::string& url, int maxf, Src& s) {
    AVFormatContext* f = nullptr; AVDictionary* o = nullptr;
    if (url.rfind("rtsp://", 0) == 0) { av_dict_set(&o, "rtsp_transport", "tcp", 0); av_dict_set(&o, "max_delay", "500000", 0); }
    if (avformat_open_input(&f, url.c_str(), nullptr, &o) < 0) { av_dict_free(&o); return false; }
    av_dict_free(&o); avformat_find_stream_info(f, nullptr);
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

struct Ctx {
    zm::hw::HwBackend* be = nullptr;
    bool roi = false; float conf = 0.25f;
    int frames = 0, motion = 0, dets = 0;
};

static void on_frame(void* vc, const void* buf, size_t size) {
    Ctx* c = static_cast<Ctx*>(vc);
    const auto* hdr = static_cast<const zm_frame_hdr_t*>(buf);
    if (hdr->hw_type != ZM_HW_CUDA || size < sizeof(zm_frame_hdr_t) + sizeof(zm_gpu_frame_t)) return;
    const auto* g = reinterpret_cast<const zm_gpu_frame_t*>(static_cast<const uint8_t*>(buf) + sizeof(zm_frame_hdr_t));

    zm::hw::Surface s = c->be->acquire(g->av_frame);   // ref the surface (survives the call)
    if (!s.owner) return;
    ++c->frames;

    if (c->roi) {
        auto regions = c->be->motion(s);
        if (!regions.empty()) {
            ++c->motion;
            for (const auto& r : regions) {
                auto t = c->be->preprocess(s, r);
                auto d = c->be->infer(t, c->conf, {});
                c->dets += (int)d.size();
            }
        }
    } else {
        auto t = c->be->preprocess(s);                 // whole frame
        auto d = c->be->infer(t, c->conf, {});
        c->dets += (int)d.size();
    }
    c->be->release(s);                                 // drop the ref
}

struct Plugin { void* h = nullptr; void (*init)(zm_plugin_t*) = nullptr;
    bool open(const std::string& p) { h = dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!h) { fprintf(stderr, "dlopen %s: %s\n", p.c_str(), dlerror()); return false; }
        init = (void(*)(zm_plugin_t*))dlsym(h, "zm_plugin_init"); return init; } };

int main(int argc, char** argv) {
    std::string input, model, plugdir = "plugins"; int maxf = 300; bool roi = false;
    for (int i = 1; i < argc; ++i) { std::string a = argv[i]; auto nx = [&]{ return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "--input") input = nx(); else if (a == "--model") model = nx(); else if (a == "--plugins") plugdir = nx();
        else if (a == "--max-frames") maxf = std::atoi(nx()); else if (a == "--roi") roi = true; }
    if (input.empty() || model.empty()) { fprintf(stderr, "need --input and --model\n"); return 2; }

    auto backend = zm::hw::make_backend("cuda");
    if (!backend) { fprintf(stderr, "cuda backend unavailable\n"); return 1; }
    if (!backend->load_model(model, 640)) { fprintf(stderr, "load_model failed\n"); return 1; }

    Src s;
    if (!demux(input, maxf, s)) { fprintf(stderr, "demux failed\n"); return 1; }
    Ctx ctx; ctx.be = backend.get(); ctx.roi = roi;

    Plugin dec; if (!dec.open(plugdir + "/decode_ffmpeg/decode_ffmpeg.so")) return 1;
    zm_plugin_t p{}; dec.init(&p);
    zm_host_api_t host{}; host.on_frame = on_frame;
    char cfg[256]; std::snprintf(cfg, sizeof(cfg), "{\"hwaccel\":\"cuda\",\"output_format\":\"yuv420p\",\"codec\":\"%s\",\"scale\":\"orig\"}", s.codec.c_str());
    if (p.start(&p, &host, &ctx, cfg) != 0) { fprintf(stderr, "decode start (cuda) failed\n"); return 1; }

    std::vector<uint8_t> buf;
    for (auto& pk : s.pkts) {
        buf.resize(sizeof(zm_frame_hdr_t) + pk.size());
        auto* hd = reinterpret_cast<zm_frame_hdr_t*>(buf.data()); *hd = {};
        hd->hw_type = ZM_FRAME_COMPRESSED; hd->bytes = (uint32_t)pk.size();
        std::memcpy(buf.data() + sizeof(zm_frame_hdr_t), pk.data(), pk.size());
        p.on_frame(&p, buf.data(), buf.size());
    }
    p.stop(&p);

    printf("backend=%s  %dx%d %s\n", backend->name(), s.w, s.h, s.codec.c_str());
    printf("frames=%d  motion_frames=%d  detections=%d  (mode=%s)\n",
           ctx.frames, ctx.motion, ctx.dets, roi ? "motion-ROI" : "full-frame");
    return 0;
}

// bench_decode_detect — CPU-vs-GPU decode/detect benchmark for zm-next.
//
// Measures, against a file or an rtsp:// URL:
//   (1) Decode throughput: software vs NVDEC zero-copy vs NVDEC + GPU->CPU download
//   (2) Detect EP latency:  ORT CPU vs CUDA(host input) vs CUDA(device input / IoBinding)
//   (3) End-to-end decode->detect: all-CPU vs full GPU zero-copy, plus a
//       projected "naive hwaccel" pipeline computed from the measured parts.
//
// The point: NVDEC decode is fast, but downloading each surface to system RAM
// (av_hwframe_transfer_data) costs real PCIe time that scales with resolution —
// often erasing the hwaccel win once a CPU stage touches the frame. Keeping the
// surface on the GPU all the way into the CUDA inference EP (zero-copy) avoids
// both the download and the re-upload. This tool quantifies all three.
//
// It dlopens the real decode_ffmpeg / detect_onnx plugins (no reimplementation).

#include "zm_plugin.h"

#include <onnxruntime_cxx_api.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>
}

#ifdef BENCH_WITH_CUDA
#include <cuda_runtime.h>
#endif

#include <dlfcn.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using clk = std::chrono::steady_clock;
static double ms_since(clk::time_point t) {
    return std::chrono::duration<double, std::milli>(clk::now() - t).count();
}

// ----------------------------------------------------------------------------
// Demux: read up to max_frames compressed video packets into memory (Annex-B).
// ----------------------------------------------------------------------------
struct Packet { std::vector<uint8_t> data; bool key; int64_t pts; };
struct Source {
    int w = 0, h = 0;
    std::string codec;          // "h264" / "hevc" / ...
    double fps = 0;
    std::vector<Packet> packets;
    long total_payload = 0;
};

static bool load_source(const std::string& url, int max_frames, Source& out) {
    AVFormatContext* fmt = nullptr;
    AVDictionary* opts = nullptr;
    const bool is_rtsp = url.rfind("rtsp://", 0) == 0;
    if (is_rtsp) {
        av_dict_set(&opts, "rtsp_transport", "tcp", 0);
        av_dict_set(&opts, "stimeout", "5000000", 0);   // 5s connect timeout (us)
        av_dict_set(&opts, "max_delay", "500000", 0);
    }
    if (avformat_open_input(&fmt, url.c_str(), nullptr, &opts) < 0) {
        fprintf(stderr, "load_source: cannot open %s\n", url.c_str());
        av_dict_free(&opts);
        return false;
    }
    av_dict_free(&opts);
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        fprintf(stderr, "load_source: no stream info\n");
        avformat_close_input(&fmt);
        return false;
    }
    int vstream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vstream < 0) { avformat_close_input(&fmt); return false; }
    AVStream* st = fmt->streams[vstream];
    AVCodecParameters* par = st->codecpar;
    out.w = par->width;
    out.h = par->height;
    out.codec = avcodec_get_name(par->codec_id);
    out.fps = st->avg_frame_rate.num ? av_q2d(st->avg_frame_rate) : 0.0;

    // If the bitstream is length-prefixed (AVCC/HVCC, extradata[0]==1), convert
    // to Annex-B so decode_ffmpeg / RTSP both see the same in-band SPS/PPS form.
    AVBSFContext* bsf = nullptr;
    if (par->extradata && par->extradata_size > 0 && par->extradata[0] == 1) {
        const char* bn = (par->codec_id == AV_CODEC_ID_HEVC) ? "hevc_mp4toannexb"
                                                             : "h264_mp4toannexb";
        const AVBitStreamFilter* f = av_bsf_get_by_name(bn);
        if (f && av_bsf_alloc(f, &bsf) == 0) {
            avcodec_parameters_copy(bsf->par_in, par);
            if (av_bsf_init(bsf) < 0) { av_bsf_free(&bsf); bsf = nullptr; }
        }
    }

    AVPacket* pkt = av_packet_alloc();
    auto take = [&](AVPacket* p) {
        Packet bp;
        bp.data.assign(p->data, p->data + p->size);
        bp.key = (p->flags & AV_PKT_FLAG_KEY) != 0;
        bp.pts = p->pts;
        out.total_payload += p->size;
        out.packets.push_back(std::move(bp));
    };
    while ((int)out.packets.size() < max_frames && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == vstream) {
            if (bsf) {
                if (av_bsf_send_packet(bsf, pkt) == 0) {
                    AVPacket* fp = av_packet_alloc();
                    while (av_bsf_receive_packet(bsf, fp) == 0) { take(fp); av_packet_unref(fp); }
                    av_packet_free(&fp);
                }
            } else {
                take(pkt);
            }
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    if (bsf) av_bsf_free(&bsf);
    avformat_close_input(&fmt);
    return !out.packets.empty();
}

// ----------------------------------------------------------------------------
// Plugin loader (dlopen a zm-next plugin .so and grab zm_plugin_init).
// ----------------------------------------------------------------------------
struct PluginLib {
    void* handle = nullptr;
    void (*init)(zm_plugin_t*) = nullptr;
    bool open(const std::string& path) {
        handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle) { fprintf(stderr, "dlopen %s: %s\n", path.c_str(), dlerror()); return false; }
        init = (void(*)(zm_plugin_t*))dlsym(handle, "zm_plugin_init");
        if (!init) { fprintf(stderr, "dlsym zm_plugin_init failed\n"); return false; }
        return true;
    }
};

// ----------------------------------------------------------------------------
// Host harness — captures frames a plugin emits via host->on_frame.
// ----------------------------------------------------------------------------
struct Harness {
    int frames = 0;
    // optional GPU->CPU download timing (the "naive hwaccel" copy cost)
    bool do_download = false;
    double download_ms = 0;
    int downloaded = 0;
    AVFrame* sw = nullptr;
    // optional downstream chain (e2e): forward emitted frames into another plugin
    zm_plugin_t* downstream = nullptr;

    ~Harness() { if (sw) av_frame_free(&sw); }

    static void on_frame(void* ctx, const void* buf, size_t size) {
        auto* h = static_cast<Harness*>(ctx);
        h->frames++;
        const auto* hdr = static_cast<const zm_frame_hdr_t*>(buf);
        if (h->do_download && hdr->hw_type == ZM_HW_CUDA &&
            size >= sizeof(zm_frame_hdr_t) + sizeof(zm_gpu_frame_t)) {
            const auto* g = reinterpret_cast<const zm_gpu_frame_t*>(
                static_cast<const uint8_t*>(buf) + sizeof(zm_frame_hdr_t));
            AVFrame* hw = reinterpret_cast<AVFrame*>(g->av_frame);
            if (hw) {
                if (!h->sw) h->sw = av_frame_alloc();
                auto t = clk::now();
                int r = av_hwframe_transfer_data(h->sw, hw, 0);
                if (r >= 0) { h->download_ms += ms_since(t); h->downloaded++; }
                av_frame_unref(h->sw);
            }
        }
        if (h->downstream && h->downstream->on_frame)
            h->downstream->on_frame(h->downstream, buf, size);
    }
};

struct EventSink {  // counts detection events for e2e
    int detections = 0;
    static void publish(void* ctx, const char* json) {
        auto* s = static_cast<EventSink*>(ctx);
        if (json && std::strstr(json, "\"detection\"")) s->detections++;
    }
};

static void host_log(void*, zm_log_level_t, const char*) {}  // quiet

// ----------------------------------------------------------------------------
// Feed all packets (optionally looped `passes` times) into a started plugin.
// ----------------------------------------------------------------------------
static void feed_packets(zm_plugin_t* p, const Source& src, int passes) {
    std::vector<uint8_t> buf;
    for (int pass = 0; pass < passes; ++pass) {
        for (const auto& pk : src.packets) {
            buf.resize(sizeof(zm_frame_hdr_t) + pk.data.size());
            auto* hdr = reinterpret_cast<zm_frame_hdr_t*>(buf.data());
            *hdr = {};
            hdr->stream_id = 0;
            hdr->hw_type = ZM_FRAME_COMPRESSED;
            hdr->bytes = (uint32_t)pk.data.size();
            hdr->flags = pk.key ? 1u : 0u;
            hdr->pts_usec = (uint64_t)(pk.pts < 0 ? 0 : pk.pts);
            std::memcpy(buf.data() + sizeof(zm_frame_hdr_t), pk.data.data(), pk.data.size());
            p->on_frame(p, buf.data(), buf.size());
        }
    }
}

// ----------------------------------------------------------------------------
// Decode benchmark
// ----------------------------------------------------------------------------
struct DecodeResult { int frames = 0; double ms = 0; double download_ms = 0; int downloaded = 0; };

static DecodeResult bench_decode(PluginLib& lib, const Source& src,
                                 const std::string& hwaccel, const std::string& out_fmt,
                                 bool do_download, int passes) {
    zm_plugin_t plug{};
    lib.init(&plug);
    Harness h;
    h.do_download = do_download;
    zm_host_api_t host{};
    host.log = host_log;
    host.on_frame = Harness::on_frame;
    char cfg[256];
    std::snprintf(cfg, sizeof(cfg),
                  "{\"hwaccel\":\"%s\",\"output_format\":\"%s\",\"codec\":\"%s\",\"scale\":\"orig\"}",
                  hwaccel.c_str(), out_fmt.c_str(), src.codec.c_str());
    if (plug.start(&plug, &host, &h, cfg) != 0) {
        fprintf(stderr, "decode start failed (hwaccel=%s)\n", hwaccel.c_str());
        return {};
    }
    auto t = clk::now();
    feed_packets(&plug, src, passes);
    plug.stop(&plug);
    DecodeResult r;
    r.frames = h.frames;
    r.ms = ms_since(t);
    r.download_ms = h.download_ms;
    r.downloaded = h.downloaded;
    return r;
}

// ----------------------------------------------------------------------------
// Detect EP microbench (ORT direct): CPU vs CUDA(host input) vs CUDA(device).
// Isolates the per-inference HtoD upload cost that the zero-copy path avoids.
// ----------------------------------------------------------------------------
static double median_ms(std::vector<double>& v) {
    std::sort(v.begin(), v.end());
    return v.empty() ? 0 : v[v.size() / 2];
}

static void bench_detect_ep(const std::string& model, int net, int iters) {
    const size_t n = (size_t)3 * net * net;
    std::vector<float> input(n);
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> d(0.f, 1.f);
    for (auto& x : input) x = d(rng);
    const std::array<int64_t, 4> shape{1, 3, net, net};

    Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "bench");

    auto run_session = [&](bool cuda, bool device_input, const char* label) {
        Ort::SessionOptions so;
        so.SetIntraOpNumThreads(1);
        so.SetGraphOptimizationLevel(ORT_ENABLE_ALL);
#ifdef BENCH_WITH_CUDA
        if (cuda) { OrtCUDAProviderOptions o{}; so.AppendExecutionProvider_CUDA(o); }
#endif
        Ort::Session sess(env, model.c_str(), so);
        Ort::AllocatorWithDefaultOptions alloc;
        std::string in = sess.GetInputNameAllocated(0, alloc).get();
        std::string out = sess.GetOutputNameAllocated(0, alloc).get();
        const char* ins[] = {in.c_str()};
        const char* outs[] = {out.c_str()};

        void* dptr = nullptr;
        Ort::IoBinding binding(sess);
#ifdef BENCH_WITH_CUDA
        if (device_input) {
            if (cudaMalloc(&dptr, n * sizeof(float)) != cudaSuccess) { printf("  %-28s cudaMalloc failed\n", label); return; }
            cudaMemcpy(dptr, input.data(), n * sizeof(float), cudaMemcpyHostToDevice);
            Ort::MemoryInfo cudaMem("Cuda", OrtArenaAllocator, 0, OrtMemTypeDefault);
            Ort::Value t = Ort::Value::CreateTensor<float>(cudaMem, (float*)dptr, n, shape.data(), shape.size());
            binding.BindInput(in.c_str(), t);
            binding.BindOutput(out.c_str(), Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault));
        }
#endif
        Ort::MemoryInfo cpuMem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value hostTensor = Ort::Value::CreateTensor<float>(cpuMem, input.data(), n, shape.data(), shape.size());

        // warmup
        for (int i = 0; i < 3; ++i) {
            if (device_input) sess.Run(Ort::RunOptions{nullptr}, binding);
            else sess.Run(Ort::RunOptions{nullptr}, ins, &hostTensor, 1, outs, 1);
        }
        std::vector<double> samples;
        for (int i = 0; i < iters; ++i) {
            auto t0 = clk::now();
            if (device_input) sess.Run(Ort::RunOptions{nullptr}, binding);
            else { auto r = sess.Run(Ort::RunOptions{nullptr}, ins, &hostTensor, 1, outs, 1); (void)r; }
            samples.push_back(ms_since(t0));
        }
        double m = median_ms(samples);
        printf("  %-32s %7.2f ms   %7.1f inf/s\n", label, m, 1000.0 / m);
#ifdef BENCH_WITH_CUDA
        if (dptr) cudaFree(dptr);
#endif
    };

    run_session(false, false, "CPU EP");
#ifdef BENCH_WITH_CUDA
    run_session(true, false, "CUDA EP (host input, uploads)");
    run_session(true, true,  "CUDA EP (device input, IoBinding)");
#endif
}

// ----------------------------------------------------------------------------
// End-to-end decode->detect via the real plugins.
// ----------------------------------------------------------------------------
struct E2EResult { int frames = 0; int detections = 0; double ms = 0; };

static E2EResult bench_e2e(PluginLib& dec, PluginLib& det, const Source& src,
                           const std::string& model, bool gpu, int passes) {
    zm_plugin_t dp{}, tp{};
    dec.init(&dp);
    det.init(&tp);

    EventSink sink;
    zm_host_api_t det_host{};
    det_host.log = host_log;
    det_host.publish_evt = EventSink::publish;
    det_host.on_frame = nullptr;  // detect is the terminal stage here
    char det_cfg[512];
    std::snprintf(det_cfg, sizeof(det_cfg),
        "{\"model_path\":\"%s\",\"input_size\":640,\"ep\":\"%s\",\"frame_width\":%d,\"frame_height\":%d}",
        model.c_str(), gpu ? "cuda" : "cpu", src.w, src.h);
    if (tp.start(&tp, &det_host, &sink, det_cfg) != 0) { fprintf(stderr, "detect start failed\n"); return {}; }

    Harness h;
    h.downstream = &tp;
    zm_host_api_t dec_host{};
    dec_host.log = host_log;
    dec_host.on_frame = Harness::on_frame;
    char dec_cfg[256];
    std::snprintf(dec_cfg, sizeof(dec_cfg),
        "{\"hwaccel\":\"%s\",\"output_format\":\"%s\",\"codec\":\"%s\",\"scale\":\"orig\"}",
        gpu ? "cuda" : "none", gpu ? "yuv420p" : "rgb24", src.codec.c_str());
    if (dp.start(&dp, &dec_host, &h, dec_cfg) != 0) { fprintf(stderr, "decode start failed\n"); return {}; }

    auto t = clk::now();
    feed_packets(&dp, src, passes);
    dp.stop(&dp);
    tp.stop(&tp);

    E2EResult r;
    r.frames = h.frames;
    r.detections = sink.detections;
    r.ms = ms_since(t);
    return r;
}

// ----------------------------------------------------------------------------
int main(int argc, char** argv) {
    std::string input, model, plugdir = "plugins";
    int max_frames = 300, passes = 4, det_iters = 50;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "--input") input = next();
        else if (a == "--model") model = next();
        else if (a == "--plugins") plugdir = next();
        else if (a == "--max-frames") max_frames = std::atoi(next());
        else if (a == "--passes") passes = std::atoi(next());
        else if (a == "--help") {
            printf("usage: bench_decode_detect --input <file|rtsp://> [--model m.onnx]\n"
                   "       [--plugins <dir>] [--max-frames N] [--passes N]\n");
            return 0;
        }
    }
    if (input.empty()) { fprintf(stderr, "need --input <file|rtsp://>\n"); return 2; }

    Source src;
    fprintf(stderr, "loading up to %d packets from %s ...\n", max_frames, input.c_str());
    if (!load_source(input, max_frames, src)) { fprintf(stderr, "failed to load input\n"); return 1; }
    printf("\n=== Source: %dx%d %s  (%zu packets, %.1f fps src) ===\n",
           src.w, src.h, src.codec.c_str(), src.packets.size(), src.fps);
    const int total_frames = (int)src.packets.size() * passes;

    PluginLib decLib, detLib;
    if (!decLib.open(plugdir + "/decode_ffmpeg/decode_ffmpeg.so")) return 1;
    const bool have_det = !model.empty() && detLib.open(plugdir + "/detect_onnx/detect_onnx.so");

    // ---- (1) Decode ----
    printf("\n--- (1) DECODE  (%d frames over %d passes) ---\n", total_frames, passes);
    auto sw  = bench_decode(decLib, src, "none", "yuv420p", false, passes);
    auto cu  = bench_decode(decLib, src, "cuda", "yuv420p", false, passes);
    auto cud = bench_decode(decLib, src, "cuda", "yuv420p", true,  passes);
    auto fps = [](const DecodeResult& r){ return r.frames ? r.frames * 1000.0 / r.ms : 0; };
    printf("  %-34s %6d frames %8.1f ms %8.1f fps\n", "CPU software decode",        sw.frames,  sw.ms,  fps(sw));
    printf("  %-34s %6d frames %8.1f ms %8.1f fps\n", "NVDEC zero-copy (in VRAM)",  cu.frames,  cu.ms,  fps(cu));
    printf("  %-34s %6d frames %8.1f ms %8.1f fps\n", "NVDEC + GPU->CPU download",  cud.frames, cud.ms, fps(cud));
    if (cud.downloaded) {
        double per = cud.download_ms / cud.downloaded;
        double bytes = (double)src.w * src.h * 1.5;  // NV12 bytes/frame
        printf("    -> download: %.2f ms/frame  (%.1f GB/s, %d frames) — this is the hwaccel 'tax'\n",
               per, (bytes / (per / 1000.0)) / 1e9, cud.downloaded);
    }

    // ---- (2) Detect EP ----
    if (have_det) {
        printf("\n--- (2) DETECT execution provider (640x640, median of %d) ---\n", det_iters);
        bench_detect_ep(model, 640, det_iters);
    }

    // ---- (3) End-to-end ----
    if (have_det) {
        printf("\n--- (3) END-TO-END decode->detect (%d frames) ---\n", total_frames);
        auto cpu = bench_e2e(decLib, detLib, src, model, false, passes);
        auto gpu = bench_e2e(decLib, detLib, src, model, true,  passes);
        auto efps = [](const E2EResult& r){ return r.frames ? r.frames * 1000.0 / r.ms : 0; };
        printf("  %-34s %6d frames %8.1f ms %8.1f fps  (%d det)\n", "all-CPU (sw decode + CPU detect)",  cpu.frames, cpu.ms, efps(cpu), cpu.detections);
        printf("  %-34s %6d frames %8.1f ms %8.1f fps  (%d det)\n", "full GPU zero-copy (NVDEC+CUDA)",   gpu.frames, gpu.ms, efps(gpu), gpu.detections);
    } else {
        printf("\n(skipping detect — pass --model <yolo.onnx> to enable)\n");
    }
    printf("\n");
    return 0;
}

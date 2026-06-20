// decode_detect — fused NVDEC decode + on-device detection in ONE synchronous
// stage. It dlopens an internal decode_ffmpeg (CUDA) and, for each decoded surface
// (delivered synchronously on the same thread), runs the HwBackend detect path
// (motion -> preprocess -> infer) and publishes "detection" events. Because decode
// and detect share one call, the GPU surface never crosses a StageRunner queue, so
// the zero-copy path works in the real threaded pipeline. Swap "hw" to target a
// different backend (cuda today; metal/openvino/rocm later).
//
// Config: model_path, input_size(640), conf_threshold(0.25), roi_motion(false),
//         class_filter([ids]), stream_filter([ids]), hw("cuda"),
//         decode_path("plugins/decode_ffmpeg/decode_ffmpeg.so"), codec(optional).

#include "zm_plugin.h"
#include "hw_backend.hpp"
#include <nlohmann/json.hpp>

#include <dlfcn.h>
#include <memory>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {

static const char* kCoco[80] = {
    "person","bicycle","car","motorcycle","airplane","bus","train","truck","boat","traffic light",
    "fire hydrant","stop sign","parking meter","bench","bird","cat","dog","horse","sheep","cow",
    "elephant","bear","zebra","giraffe","backpack","umbrella","handbag","tie","suitcase","frisbee",
    "skis","snowboard","sports ball","kite","baseball bat","baseball glove","skateboard","surfboard",
    "tennis racket","bottle","wine glass","cup","fork","knife","spoon","bowl","banana","apple",
    "sandwich","orange","broccoli","carrot","hot dog","pizza","donut","cake","chair","couch",
    "potted plant","bed","dining table","toilet","tv","laptop","mouse","remote","keyboard","cell phone",
    "microwave","oven","toaster","sink","refrigerator","book","clock","vase","scissors","teddy bear",
    "hair drier","toothbrush"};

struct Ctx {
    zm_host_api_t* host = nullptr; void* hostCtx = nullptr;
    void* decLib = nullptr; zm_plugin_t dec{}; bool decStarted = false;
    zm_host_api_t decHost{};
    std::unique_ptr<zm::hw::HwBackend> be;
    std::string model; int net = 640; float conf = 0.25f;
    bool roiMotion = false;
    std::vector<int> classes, streamFilter;
};

bool streamAllowed(Ctx* c, uint32_t sid) {
    if (c->streamFilter.empty()) return true;
    for (int v : c->streamFilter) if (static_cast<uint32_t>(v) == sid) return true;
    return false;
}
const char* className(int id) { return (id >= 0 && id < 80) ? kCoco[id] : "object"; }

void publish(Ctx* c, const zm_frame_hdr_t* hdr, const std::vector<zm::hw::Detection>& dets) {
    if (dets.empty() || !c->host || !c->host->publish_evt) return;
    json arr = json::array();
    for (const auto& d : dets)
        arr.push_back({{"label", className(d.class_id)}, {"confidence", d.confidence},
                       {"bbox", {d.x, d.y, d.w, d.h}}, {"class_id", d.class_id}});
    json ev;
    ev["type"] = "detection"; ev["stream_id"] = hdr->stream_id; ev["pts_usec"] = hdr->pts_usec;
    ev["detections"] = std::move(arr);
    c->host->publish_evt(c->hostCtx, ev.dump().c_str());
}

// Called by the internal decoder for each decoded surface — SYNCHRONOUS, same
// thread as on_frame(), so the GPU surface is valid for the whole detect.
void on_decoded(void* vc, const void* buf, size_t size) {
    Ctx* c = static_cast<Ctx*>(vc);
    const auto* hdr = static_cast<const zm_frame_hdr_t*>(buf);
    if (!c->be || hdr->hw_type != ZM_HW_CUDA ||
        size < sizeof(zm_frame_hdr_t) + sizeof(zm_gpu_frame_t)) return;
    const auto* g = reinterpret_cast<const zm_gpu_frame_t*>(static_cast<const uint8_t*>(buf) + sizeof(zm_frame_hdr_t));
    auto& be = *c->be;
    zm::hw::Surface s = be.acquire(g->av_frame);
    if (!s.owner) return;
    std::vector<zm::hw::Detection> dets;
    if (c->roiMotion) {
        for (const auto& r : be.motion(s)) {
            auto t = be.preprocess(s, r);
            auto d = be.infer(t, c->conf, c->classes);
            dets.insert(dets.end(), d.begin(), d.end());
        }
    } else {
        auto t = be.preprocess(s);
        dets = be.infer(t, c->conf, c->classes);
    }
    be.release(s);
    publish(c, hdr, dets);
}

// Host-API forwarders for the internal decode instance (host_ctx = Ctx*): pass
// log / event sub-pub through to the real host so the decoder still learns codec
// from StreamMetadata; route its output frames to on_decoded().
void fwd_log(void* hc, zm_log_level_t lvl, const char* m) { Ctx* c = (Ctx*)hc; if (c->host && c->host->log) c->host->log(c->hostCtx, lvl, m); }
void* fwd_sub(void* hc, void (*cb)(void*, const char*), void* u) { Ctx* c = (Ctx*)hc; return (c->host && c->host->subscribe_evt) ? c->host->subscribe_evt(c->hostCtx, cb, u) : nullptr; }
void fwd_unsub(void* hc, void* h) { Ctx* c = (Ctx*)hc; if (c->host && c->host->unsubscribe_evt) c->host->unsubscribe_evt(c->hostCtx, h); }
void fwd_pub(void* hc, const char* j) { Ctx* c = (Ctx*)hc; if (c->host && c->host->publish_evt) c->host->publish_evt(c->hostCtx, j); }

int start(zm_plugin_t* plugin, zm_host_api_t* host, void* host_ctx, const char* json_cfg) {
    auto* c = new Ctx(); c->host = host; c->hostCtx = host_ctx;
    zm_plugin_set_log_context(host, host_ctx);
    std::string hw = "cuda", decPath = "plugins/decode_ffmpeg/decode_ffmpeg.so", codec;
    try {
        auto j = json::parse(json_cfg ? json_cfg : "{}");
        c->model = j.value("model_path", std::string());
        c->net = j.value("input_size", 640);
        c->conf = j.value("conf_threshold", 0.25f);
        c->roiMotion = j.value("roi_motion", false);
        if (j.contains("class_filter") && j["class_filter"].is_array()) c->classes = j["class_filter"].get<std::vector<int>>();
        if (j.contains("stream_filter") && j["stream_filter"].is_array()) c->streamFilter = j["stream_filter"].get<std::vector<int>>();
        hw = j.value("hw", hw); decPath = j.value("decode_path", decPath); codec = j.value("codec", std::string());
    } catch (const std::exception& e) { ZM_LOG_ERROR("decode_detect: config parse failed: %s", e.what()); }

    c->be = zm::hw::make_backend(hw);
    if (!c->be) { ZM_LOG_ERROR("decode_detect: backend '%s' unavailable (pass-through)", hw.c_str()); plugin->instance = c; return 0; }
    if (!c->be->load_model(c->model, c->net)) ZM_LOG_ERROR("decode_detect: load_model('%s') failed", c->model.c_str());

    c->decLib = dlopen(decPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!c->decLib) { ZM_LOG_ERROR("decode_detect: dlopen %s: %s", decPath.c_str(), dlerror()); plugin->instance = c; return 0; }
    auto init = (void(*)(zm_plugin_t*))dlsym(c->decLib, "zm_plugin_init");
    if (!init) { ZM_LOG_ERROR("decode_detect: zm_plugin_init missing in decode"); plugin->instance = c; return 0; }
    init(&c->dec);
    c->decHost.log = fwd_log; c->decHost.on_frame = on_decoded; c->decHost.publish_evt = fwd_pub;
    c->decHost.subscribe_evt = fwd_sub; c->decHost.unsubscribe_evt = fwd_unsub;
    json dcfg; dcfg["hwaccel"] = "cuda"; dcfg["output_format"] = "yuv420p"; dcfg["scale"] = "orig";
    if (!codec.empty()) dcfg["codec"] = codec;
    if (c->dec.start(&c->dec, &c->decHost, c, dcfg.dump().c_str()) == 0) c->decStarted = true;
    else ZM_LOG_ERROR("decode_detect: internal decode start failed");

    ZM_LOG_INFO("decode_detect: fused decode+detect via %s backend (roi_motion=%d)",
                c->be ? c->be->name() : "none", static_cast<int>(c->roiMotion));
    plugin->instance = c;
    return 0;
}

void stop(zm_plugin_t* plugin) {
    if (!plugin || !plugin->instance) return;
    auto* c = static_cast<Ctx*>(plugin->instance);
    if (c->decStarted && c->dec.stop) c->dec.stop(&c->dec);
    if (c->decLib) dlclose(c->decLib);
    delete c; plugin->instance = nullptr;
}

void on_frame(zm_plugin_t* plugin, const void* buf, size_t size) {
    auto* c = plugin ? static_cast<Ctx*>(plugin->instance) : nullptr;
    if (!c) return;
    if (c->host && c->host->on_frame) c->host->on_frame(c->hostCtx, buf, size);  // pass-through
    if (c->decStarted && size >= sizeof(zm_frame_hdr_t)) {
        const auto* hdr = static_cast<const zm_frame_hdr_t*>(buf);
        if (hdr->hw_type == ZM_FRAME_COMPRESSED && streamAllowed(c, hdr->stream_id))
            c->dec.on_frame(&c->dec, buf, size);   // -> on_decoded() synchronously
    }
}

}  // namespace

extern "C" __attribute__((visibility("default"))) void zm_plugin_init(zm_plugin_t* plugin) {
    if (!plugin) return;
    plugin->version = 1; plugin->type = ZM_PLUGIN_PROCESS; plugin->instance = nullptr;
    plugin->start = start; plugin->stop = stop; plugin->on_frame = on_frame;
}

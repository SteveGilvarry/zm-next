//
// describe_vlm plugin
//
// A pass-through PROCESS plugin that performs VLM (vision-language model) scene
// understanding. It periodically sends the latest decoded RGB24 frame to a
// shared, OpenAI-compatible VLM server (e.g. llama.cpp's llama-server or Ollama
// running Moondream / Qwen2.5-VL) and publishes the natural-language description
// as an event.
//
// The heavy model lives in an external server; this plugin is a thin async HTTP
// client. The HTTP call happens on a background thread so the pipeline thread is
// never blocked. Every frame is forwarded downstream unchanged.
//
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include "zm_plugin.h"
#include "vlm_client.hpp"
#include "image_encode.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Plugin context
// ---------------------------------------------------------------------------
// Event-trigger state, shared with the host event callback (as a raw user
// pointer). Leaked on stop so an in-flight callback never dangles (the callback
// only ever touches THIS struct, never the soon-to-be-deleted ctx). When `types`
// is empty the plugin keeps its legacy fixed-interval behaviour.
struct TriggerState {
    std::atomic<bool> running{true};
    std::atomic<bool> fired{false};
    std::mutex mtx;
    std::condition_variable cv;
    std::vector<std::string> types;          // event "type"s that trigger a describe
    std::vector<uint32_t> streamFilter;      // empty = any stream
    std::string lastTrigger;                 // JSON of the event that armed the describe (guarded by mtx)
};

struct DescribeVlmCtx {
    zm_host_api_t* host = nullptr;
    void* hostCtx = nullptr;

    // Configuration
    int frameWidth = 0;
    int frameHeight = 0;
    std::string serverUrl = "http://localhost:8080/v1/chat/completions";
    std::string model = "moondream";
    std::string prompt =
        "Describe what is happening in this security camera image in one sentence.";
    double intervalSec = 10.0;            // in trigger mode: the min gap (cooldown) between describes
    std::vector<uint32_t> streamFilter;  // empty = all streams

    // YOLO->VLM gating: when triggerTypes is non-empty the VLM only describes a
    // frame after a matching event (e.g. "detection") fires, throttled to once
    // per intervalSec. Empty = legacy fixed-interval behaviour. `trig` is leaked.
    std::vector<std::string> triggerTypes;
    TriggerState* trig = nullptr;
    void* trigSub = nullptr;             // host subscription handle
    std::string curTrigger;             // trigger JSON for the in-flight describe (worker thread only)

    // Multi-frame config: send up to `frames` keyframes spaced ~frameIntervalMs
    // so a temporally-aware VLM can reason about motion (loiter / package-drop /
    // approach-then-leave). frames<=1 keeps the original single-frame behaviour.
    int frames = 1;
    int frameIntervalMs = 700;
    bool jsonOutput = false;            // request structured {description,threat_level,confidence}
    int maxTokens = 128;

    // Latest frame snapshot + a sparse ring (one frame per ~frameIntervalMs, last
    // `frames` kept) — all protected by frameMutex.
    std::mutex frameMutex;
    std::vector<uint8_t> latestFrame;
    int latestWidth = 0;
    int latestHeight = 0;
    uint32_t latestStreamId = 0;
    uint64_t latestPtsUsec = 0;
    bool haveFrame = false;
    struct RingFrame { uint64_t pts = 0; int w = 0, h = 0; std::vector<uint8_t> rgb; };
    std::deque<RingFrame> ring;

    // Background worker
    std::thread worker;
    std::atomic<bool> running{false};
    std::mutex cvMutex;
    std::condition_variable cv;
};

// JPEG encoding helper now lives in the shared header (plugins/common); see
// zm::img::encode_rgb24_to_jpeg.

// ---------------------------------------------------------------------------
// HTTP helper (libcurl)
// ---------------------------------------------------------------------------
static size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buf = static_cast<std::string*>(userdata);
    buf->append(ptr, size * nmemb);
    return size * nmemb;
}

// POST the request body to `url`. Returns true and fills `response` on success
// (HTTP 2xx).
static bool http_post_json(const std::string& url, const std::string& body,
                           std::string& response) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    response.clear();
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

    CURLcode rc = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        ZM_LOG_ERROR("describe_vlm: curl error: %s", curl_easy_strerror(rc));
        return false;
    }
    if (httpCode < 200 || httpCode >= 300) {
        ZM_LOG_ERROR("describe_vlm: HTTP %ld from VLM server", httpCode);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Inference cycle
// ---------------------------------------------------------------------------
// JSON schema for structured scene answers (json_output mode).
static const char* kSceneSchema =
    R"({"type":"object","properties":{"description":{"type":"string"},)"
    R"("threat_level":{"type":"string","enum":["LOW","MEDIUM","HIGH"]},)"
    R"("confidence":{"type":"number"}},"required":["description","threat_level"]})";

static void run_inference_cycle(DescribeVlmCtx* ctx) {
    // Gather frame(s): the sampled ring (multi-frame mode) or the latest snapshot.
    DescribeVlmCtx::RingFrame single;
    std::vector<DescribeVlmCtx::RingFrame> picked;
    uint32_t streamId = 0;
    uint64_t ptsUsec = 0;
    {
        std::lock_guard<std::mutex> lk(ctx->frameMutex);
        if (!ctx->haveFrame || ctx->latestFrame.empty()) return;
        streamId = ctx->latestStreamId;
        ptsUsec = ctx->latestPtsUsec;
        if (ctx->frames > 1 && !ctx->ring.empty()) {
            picked.assign(ctx->ring.begin(), ctx->ring.end());  // already spaced, oldest→newest
        } else {
            single.pts = ctx->latestPtsUsec;
            single.w = ctx->latestWidth;
            single.h = ctx->latestHeight;
            single.rgb = ctx->latestFrame;
            picked.push_back(std::move(single));
        }
    }

    const uint64_t newestPts = picked.back().pts;
    std::vector<vlm::ReqFrame> reqs;
    reqs.reserve(picked.size());
    for (const auto& f : picked) {
        if (f.w <= 0 || f.h <= 0 || f.rgb.size() < size_t(f.w) * size_t(f.h) * 3) continue;
        std::vector<uint8_t> jpeg;
        if (!zm::img::encode_rgb24_to_jpeg(f.rgb.data(), f.w, f.h, jpeg)) continue;
        vlm::ReqFrame rf;
        rf.jpeg_base64 = vlm::base64_encode(jpeg.data(), jpeg.size());
        if (picked.size() > 1) {
            // Relative timestamp caption (newest = 0.0s) for temporal grounding.
            const double dt = (static_cast<double>(f.pts) - static_cast<double>(newestPts)) / 1e6;
            char cap[48];
            std::snprintf(cap, sizeof(cap), "Frame at t=%+.1fs:", dt);
            rf.caption = cap;
        }
        reqs.push_back(std::move(rf));
    }
    if (reqs.empty()) {
        ZM_LOG_ERROR("describe_vlm: no encodable frame for inference");
        return;
    }

    const std::string schema = ctx->jsonOutput ? std::string(kSceneSchema) : std::string();
    std::string body = vlm::build_chat_request_json_multi(ctx->model, ctx->prompt, reqs,
                                                          ctx->maxTokens, schema);

    std::string response;
    if (!http_post_json(ctx->serverUrl, body, response)) {
        // http_post_json already logged the error.
        return;
    }

    // Structured (json_output) → pull description/threat_level/confidence; else plain text.
    std::string text, threatLevel;
    double sceneConfidence = -1.0;
    if (ctx->jsonOutput) {
        auto j = vlm::parse_structured_response(response);
        if (j.is_object()) {
            text = j.value("description", std::string());
            threatLevel = j.value("threat_level", std::string());
            if (j.contains("confidence") && j["confidence"].is_number())
                sceneConfidence = j["confidence"].get<double>();
        }
        if (text.empty()) text = vlm::parse_chat_response_text(response);  // fallback
    } else {
        text = vlm::parse_chat_response_text(response);
    }
    if (text.empty()) {
        ZM_LOG_ERROR("describe_vlm: failed to parse VLM response");
        return;
    }

    // Publish the description event.
    json evt;
    evt["type"] = "description";
    evt["text"] = text;
    evt["prompt"] = ctx->prompt;
    evt["model"] = ctx->model;
    evt["stream_id"] = streamId;
    evt["pts_usec"] = ptsUsec;
    evt["frames"] = static_cast<int>(reqs.size());
    // Structured scene fields (json_output mode). `scene_confidence` is kept
    // distinct from the detection `confidence` set from the trigger below.
    if (!threatLevel.empty()) evt["threat_level"] = threatLevel;
    if (sceneConfidence >= 0.0) evt["scene_confidence"] = sceneConfidence;

    // Embed the detection that triggered this describe, so the published event (and
    // thus the MQTT payload) is a single rich alert: VLM text + what was detected.
    if (!ctx->curTrigger.empty()) {
        try {
            auto tj = json::parse(ctx->curTrigger);
            evt["trigger_type"] = tj.value("type", std::string("detection"));
            if (tj.contains("detections") && tj["detections"].is_array()) {
                evt["detections"] = tj["detections"];
                const auto& dets = tj["detections"];
                if (!dets.empty() && dets[0].is_object()) {   // top hit for convenience
                    evt["label"] = dets[0].value("label", std::string());
                    evt["confidence"] = dets[0].value("confidence", 0.0);
                }
            }
        } catch (const std::exception&) { /* leave the description as-is */ }
    }

    if (ctx->host && ctx->host->publish_evt) {
        ctx->host->publish_evt(ctx->hostCtx, evt.dump().c_str());
    }
    ZM_LOG_INFO("describe_vlm: %s", text.c_str());
}

// Host event callback: a detection/motion event from an upstream stage (e.g.
// decode_detect) arms a describe. Only touches the leaked TriggerState.
static void describe_trigger_cb(void* user, const char* json_event) {
    auto* ts = static_cast<TriggerState*>(user);
    if (!ts || !ts->running.load() || !json_event) return;
    try {
        auto j = nlohmann::json::parse(json_event);
        const std::string type = j.value("type", std::string());
        if (std::find(ts->types.begin(), ts->types.end(), type) == ts->types.end()) return;
        if (!ts->streamFilter.empty()) {
            uint32_t sid = j.value("stream_id", 0u);
            if (std::find(ts->streamFilter.begin(), ts->streamFilter.end(), sid) == ts->streamFilter.end())
                return;
        }
        { std::lock_guard<std::mutex> lk(ts->mtx); ts->fired.store(true); ts->lastTrigger = json_event; }
        ts->cv.notify_one();
    } catch (const std::exception&) { /* ignore malformed events */ }
}

// ---------------------------------------------------------------------------
// Background worker loop
// ---------------------------------------------------------------------------
static void worker_loop(DescribeVlmCtx* ctx) {
    const double intervalSec = ctx->intervalSec > 0.0 ? ctx->intervalSec : 10.0;
    const auto cooldown = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(intervalSec));
    TriggerState* ts = ctx->trig;
    const bool gated = ts && !ts->types.empty();
    auto lastDescribe = std::chrono::steady_clock::now() - cooldown;  // allow an immediate first describe

    while (ctx->running.load()) {
        {
            std::unique_lock<std::mutex> lk(ts->mtx);
            // Wake on: shutdown, a fired trigger, or (interval mode) the timeout tick.
            ts->cv.wait_for(lk, cooldown, [&] { return !ctx->running.load() || ts->fired.load(); });
        }
        if (!ctx->running.load()) break;

        const bool fired = ts->fired.exchange(false);
        if (gated && !fired) continue;                       // trigger mode: only describe on a detection
        auto now = std::chrono::steady_clock::now();
        if (now - lastDescribe < cooldown) continue;         // throttle: at most once per intervalSec
        lastDescribe = now;
        // Snapshot the detection that armed this describe, to embed in the event.
        { std::lock_guard<std::mutex> lk(ts->mtx); ctx->curTrigger = fired ? ts->lastTrigger : std::string(); }
        run_inference_cycle(ctx);
    }
}

// ---------------------------------------------------------------------------
// Plugin lifecycle
// ---------------------------------------------------------------------------
extern "C" {

static int describe_vlm_start(zm_plugin_t* plugin, zm_host_api_t* host,
                              void* host_ctx, const char* json_cfg) {
    auto* ctx = new DescribeVlmCtx;
    ctx->host = host;
    ctx->hostCtx = host_ctx;

    zm_plugin_set_log_context(host, host_ctx);

    if (json_cfg) {
        try {
            auto j = json::parse(json_cfg);
            ctx->frames = j.value("frames", 1);
            if (ctx->frames < 1) ctx->frames = 1;
            if (ctx->frames > 8) ctx->frames = 8;
            ctx->frameIntervalMs = j.value("frame_interval_ms", 700);
            ctx->jsonOutput = j.value("json_output", false);
            ctx->maxTokens = j.value("max_tokens", 128);
            ctx->frameWidth = j.value("frame_width", 0);
            ctx->frameHeight = j.value("frame_height", 0);
            ctx->serverUrl = j.value("server_url", ctx->serverUrl);
            ctx->model = j.value("model", ctx->model);
            ctx->prompt = j.value("prompt", ctx->prompt);
            ctx->intervalSec = j.value("interval_sec", ctx->intervalSec);
            if (j.contains("stream_filter") && j["stream_filter"].is_array()) {
                for (const auto& v : j["stream_filter"]) {
                    if (v.is_number_integer() || v.is_number_unsigned()) {
                        ctx->streamFilter.push_back(v.get<uint32_t>());
                    }
                }
            }
            if (j.contains("trigger_types") && j["trigger_types"].is_array()) {
                for (const auto& v : j["trigger_types"])
                    if (v.is_string()) ctx->triggerTypes.push_back(v.get<std::string>());
            }
        } catch (const std::exception& e) {
            ZM_LOG_ERROR("describe_vlm: failed to parse config: %s", e.what());
            // continue with defaults
        }
    }

    plugin->instance = ctx;

    // libcurl global init (idempotent enough for our single-plugin use).
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // Trigger state (always created; the worker waits on its cv). In trigger mode
    // (trigger_types set) subscribe to the host event bus so detections from an
    // upstream YOLO stage arm a describe.
    ctx->trig = new TriggerState;
    ctx->trig->types = ctx->triggerTypes;
    ctx->trig->streamFilter = ctx->streamFilter;
    if (!ctx->triggerTypes.empty() && host && host->subscribe_evt)
        ctx->trigSub = host->subscribe_evt(host_ctx, &describe_trigger_cb, ctx->trig);

    ctx->running.store(true);
    ctx->worker = std::thread(worker_loop, ctx);

    if (ctx->triggerTypes.empty()) {
        ZM_LOG_INFO("describe_vlm started (server=%s, model=%s, interval=%.1fs, mode=fixed-interval)",
                    ctx->serverUrl.c_str(), ctx->model.c_str(), ctx->intervalSec);
    } else {
        std::string ts; for (auto& t : ctx->triggerTypes) ts += (ts.empty()?"":",") + t;
        ZM_LOG_INFO("describe_vlm started (server=%s, model=%s, mode=triggered on [%s], cooldown=%.1fs)",
                    ctx->serverUrl.c_str(), ctx->model.c_str(), ts.c_str(), ctx->intervalSec);
    }
    return 0;
}

static void describe_vlm_stop(zm_plugin_t* plugin) {
    auto* ctx = static_cast<DescribeVlmCtx*>(plugin->instance);
    if (!ctx) return;

    // Stop new triggers first, then wake + join the worker.
    if (ctx->trigSub && ctx->host && ctx->host->unsubscribe_evt)
        ctx->host->unsubscribe_evt(ctx->hostCtx, ctx->trigSub);
    if (ctx->trig) ctx->trig->running.store(false);

    ctx->running.store(false);
    if (ctx->trig) {                       // worker waits on the trigger state's cv
        std::lock_guard<std::mutex> lk(ctx->trig->mtx);
        ctx->trig->cv.notify_all();
    }
    if (ctx->worker.joinable()) {
        ctx->worker.join();
    }

    curl_global_cleanup();

    // ctx->trig is intentionally leaked: a host callback may still be in flight
    // after unsubscribe returns, and it only touches the TriggerState.
    delete ctx;
    plugin->instance = nullptr;
}

static void describe_vlm_on_frame(zm_plugin_t* plugin, const void* buf, size_t size) {
    auto* ctx = static_cast<DescribeVlmCtx*>(plugin->instance);
    if (!ctx || !buf || size < sizeof(zm_frame_hdr_t)) {
        if (ctx && ctx->host && ctx->host->on_frame && buf) {
            ctx->host->on_frame(ctx->hostCtx, buf, size);
        }
        return;
    }

    const zm_frame_hdr_t* hdr = static_cast<const zm_frame_hdr_t*>(buf);
    const uint8_t* payload = static_cast<const uint8_t*>(buf) + sizeof(zm_frame_hdr_t);
    size_t payloadSize = size - sizeof(zm_frame_hdr_t);

    // Only act on RGB24 frames; everything else is forwarded untouched.
    if (hdr->hw_type == ZM_FRAME_RGB24) {
        // Stream filter: if non-empty, only act on listed stream ids.
        bool accept = ctx->streamFilter.empty();
        if (!accept) {
            for (uint32_t sid : ctx->streamFilter) {
                if (sid == hdr->stream_id) { accept = true; break; }
            }
        }

        if (accept) {
            // Determine frame dimensions: prefer configured values, else derive
            // from payload size assuming RGB24.
            int width = ctx->frameWidth;
            int height = ctx->frameHeight;

            // Fast snapshot under a short lock: just a memcpy.
            std::lock_guard<std::mutex> lk(ctx->frameMutex);
            ctx->latestFrame.assign(payload, payload + payloadSize);
            ctx->latestWidth = width;
            ctx->latestHeight = height;
            ctx->latestStreamId = hdr->stream_id;
            ctx->latestPtsUsec = hdr->pts_usec;
            ctx->haveFrame = true;

            // Multi-frame: keep a sparse ring sampled at ~frameIntervalMs, last
            // `frames` kept (bounds memory to `frames` RGB frames, not every frame).
            if (ctx->frames > 1 && width > 0 && height > 0) {
                const uint64_t gap = static_cast<uint64_t>(ctx->frameIntervalMs) * 1000;
                if (ctx->ring.empty() ||
                    hdr->pts_usec - ctx->ring.back().pts >= gap) {
                    DescribeVlmCtx::RingFrame rf;
                    rf.pts = hdr->pts_usec;
                    rf.w = width;
                    rf.h = height;
                    rf.rgb.assign(payload, payload + payloadSize);
                    ctx->ring.push_back(std::move(rf));
                    while (ctx->ring.size() > static_cast<size_t>(ctx->frames))
                        ctx->ring.pop_front();
                }
            }
        }
    }

    // Always forward downstream so the pipeline chain continues.
    if (ctx->host && ctx->host->on_frame) {
        ctx->host->on_frame(ctx->hostCtx, buf, size);
    }
}

#if defined(__GNUC__) || defined(__clang__)
#define ZM_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define ZM_PLUGIN_EXPORT
#endif

ZM_PLUGIN_EXPORT void zm_plugin_init(zm_plugin_t* plugin) {
    plugin->version = 1;
    plugin->type = ZM_PLUGIN_PROCESS;
    plugin->start = describe_vlm_start;
    plugin->stop = describe_vlm_stop;
    plugin->on_frame = describe_vlm_on_frame;
    plugin->instance = nullptr;
}

}  // extern "C"

//
// llm_event_review plugin
//
// A track-gated VLM (vision-language model) event reviewer. Unlike describe_vlm
// (which describes the scene on a dumb timer), this PROCESS plugin fires only
// when the pipeline says something actually happened: it subscribes to "alert"
// (from alert_policy) and optionally "tracked_detection" (from tracker) events
// over the EventBus, snaps the latest decoded frame for that stream, JPEG-encodes
// it (downscaled to a max_pixels cap), asks a local VLM to describe the security
// event, and publishes a "reasoning" event with the natural-language text.
//
// It reuses describe_vlm's proven machinery:
//   - latest-frame-per-stream cache populated from on_frame (RGB24), under mutex,
//   - FFmpeg mjpeg JPEG encode (here with an added downscale to max_pixels),
//   - libcurl + OpenAI-compatible /v1/chat/completions HTTP (via IVisionProvider).
//
// THREADING: on_frame runs on the decode thread; the EventBus callback runs on
// the publisher thread (whoever published the "alert"). The frame cache is
// therefore guarded by a mutex. The VLM HTTP call (slow) is dispatched to a
// background worker thread so neither the decode nor the publisher thread blocks.
//
// LIFETIME: the subscription callback can race plugin teardown, so the State is
// intentionally leaked on stop() (the alert_policy pattern) and the callback
// checks an `active` flag.
//
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "provider.hpp"
#include "zm_plugin.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

using json = nlohmann::json;

namespace {

// ---------------------------------------------------------------------------
// Cached frame (one per stream).
// ---------------------------------------------------------------------------
struct CachedFrame {
    std::vector<uint8_t> rgb;  // RGB24 pixels
    int width = 0;
    int height = 0;
    uint64_t ptsUsec = 0;
    bool have = false;
};

// A queued describe job (handed from the EventBus callback to the worker).
struct ReviewJob {
    uint32_t streamId = 0;
    uint64_t ptsUsec = 0;
    int trackId = 0;
    std::string label;
    std::string triggerEvent;   // "alert" | "tracked_detection"
    std::string triggerReason;  // alert "reason", else ""
    // Frame snapshot taken at enqueue time (decode thread keeps mutating cache).
    std::vector<uint8_t> rgb;
    int width = 0;
    int height = 0;
};

// ---------------------------------------------------------------------------
// Plugin state.
// ---------------------------------------------------------------------------
struct State {
    zm_host_api_t* host = nullptr;
    void* hostCtx = nullptr;

    // --- config ---
    std::string provider = "local";
    std::string serverUrl = "http://localhost:8080/v1/chat/completions";
    // Pure config string forwarded to the OpenAI-compatible /chat/completions
    // call — NOT compiled in. Any VLM the server (vLLM / llama-server / Ollama /
    // a cloud endpoint) exposes can be swapped here with no rebuild: Qwen2.5-VL,
    // Qwen3-VL, MiniCPM-V, InternVL, etc. Default is just a sane local starting point.
    std::string model = "Qwen/Qwen3-VL-8B-Instruct-FP8";
    std::string apiKey;
    int frameWidth = 0;   // RGB24 dims (ABI carries no width/height in the hdr)
    int frameHeight = 0;
    std::string prompt =
        "You are a security camera analyst. Describe the security event in this "
        "image in one sentence, noting anything unusual.";
    long maxPixels = 1003520;       // ~1 MP cap on the longest-edge area
    long timeoutSec = 30;
    bool enableThinking = false;    // chat_template_kwargs.enable_thinking; false =
                                    // caption mode (reasoning models like Qwen3.5
                                    // otherwise burn the token budget on a thought
                                    // preamble). Ignored by non-thinking models.
    std::vector<std::string> classes;       // empty = all
    std::vector<uint32_t> streamFilter;     // empty = all
    std::vector<std::string> triggerEvents{"alert"};  // events we react to
    uint64_t debounceUsec = 10'000'000;     // per-track de-dup window

    // --- provider ---
    std::unique_ptr<llmrev::IVisionProvider> visionProvider;

    // --- latest-frame cache (decode thread writes, worker/cb reads) ---
    std::mutex frameMutex;
    std::unordered_map<uint32_t, CachedFrame> frames;

    // --- per-track de-dup (EventBus-callback thread) ---
    std::mutex dedupMutex;
    // key = (stream_id<<32 | track_id) -> last review wall-clock time
    std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> lastReview;

    // --- background worker ---
    std::thread worker;
    std::atomic<bool> running{false};
    std::mutex jobMutex;
    std::condition_variable jobCv;
    std::deque<ReviewJob> jobs;

    // --- subscription / teardown ---
    void* sub = nullptr;
    std::atomic<bool> active{true};
};

// ---------------------------------------------------------------------------
// JPEG encode with downscale to a max_pixels (area) cap.
// ---------------------------------------------------------------------------
//
// Encodes an RGB24 buffer to an in-memory JPEG. If width*height exceeds
// maxPixels, the image is bilinearly downscaled (aspect preserved) before
// encoding — this caps the visual-token cost on the VLM and reduces PII in the
// bytes that leave the box. Returns true and fills `out` on success.
//
bool encodeRgb24ToJpeg(const uint8_t* rgb, int width, int height, long maxPixels,
                       std::vector<uint8_t>& out) {
    out.clear();
    if (!rgb || width <= 0 || height <= 0) return false;

    // Compute the (possibly downscaled) destination size.
    int dstW = width, dstH = height;
    if (maxPixels > 0 &&
        static_cast<long>(width) * static_cast<long>(height) > maxPixels) {
        const double scale =
            std::sqrt(static_cast<double>(maxPixels) /
                      (static_cast<double>(width) * static_cast<double>(height)));
        dstW = std::max(1, static_cast<int>(width * scale));
        dstH = std::max(1, static_cast<int>(height * scale));
        // mjpeg/yuvj420p prefers even dimensions.
        dstW &= ~1;
        dstH &= ~1;
        if (dstW < 2) dstW = 2;
        if (dstH < 2) dstH = 2;
    }

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!codec) return false;

    AVCodecContext* cctx = avcodec_alloc_context3(codec);
    if (!cctx) return false;

    cctx->pix_fmt = AV_PIX_FMT_YUVJ420P;
    cctx->width = dstW;
    cctx->height = dstH;
    cctx->time_base = AVRational{1, 25};
    cctx->color_range = AVCOL_RANGE_JPEG;

    SwsContext* sws = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    bool ok = false;

    do {
        if (avcodec_open2(cctx, codec, nullptr) < 0) break;

        frame = av_frame_alloc();
        if (!frame) break;
        frame->format = cctx->pix_fmt;
        frame->width = dstW;
        frame->height = dstH;
        if (av_frame_get_buffer(frame, 32) < 0) break;

        // Scale RGB24(src) -> YUVJ420P(dst), resizing in the same pass.
        sws = sws_getContext(width, height, AV_PIX_FMT_RGB24,
                             dstW, dstH, AV_PIX_FMT_YUVJ420P,
                             SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws) break;

        const uint8_t* srcSlice[1] = {rgb};
        int srcStride[1] = {3 * width};
        sws_scale(sws, srcSlice, srcStride, 0, height,
                  frame->data, frame->linesize);

        frame->pts = 0;

        if (avcodec_send_frame(cctx, frame) < 0) break;

        pkt = av_packet_alloc();
        if (!pkt) break;

        int ret = avcodec_receive_packet(cctx, pkt);
        if (ret < 0) break;

        out.assign(pkt->data, pkt->data + pkt->size);
        ok = true;
    } while (false);

    if (pkt) av_packet_free(&pkt);
    if (frame) av_frame_free(&frame);
    if (sws) sws_freeContext(sws);
    if (cctx) avcodec_free_context(&cctx);

    return ok;
}

// ---------------------------------------------------------------------------
// Config helpers.
// ---------------------------------------------------------------------------
bool allowedClass(const State* s, const std::string& l) {
    if (s->classes.empty()) return true;
    for (const auto& c : s->classes) if (c == l) return true;
    return false;
}
bool allowedStream(const State* s, uint32_t sid) {
    if (s->streamFilter.empty()) return true;
    for (uint32_t v : s->streamFilter) if (v == sid) return true;
    return false;
}
bool wantsTrigger(const State* s, const std::string& type) {
    for (const auto& t : s->triggerEvents) if (t == type) return true;
    return false;
}

// Render the prompt with simple [token] substitutions.
std::string renderPrompt(const State* s, const std::string& label,
                         const std::string& reason) {
    std::string p = s->prompt;
    auto sub = [&p](const std::string& tok, const std::string& val) {
        for (size_t pos = p.find(tok); pos != std::string::npos;
             pos = p.find(tok, pos)) {
            p.replace(pos, tok.size(), val);
            pos += val.size();
        }
    };
    sub("[label]", label);
    sub("[reason]", reason);
    sub("[trigger_reason]", reason);
    return p;
}

// ---------------------------------------------------------------------------
// Background worker: pops jobs, encodes JPEG, calls the provider, publishes.
// ---------------------------------------------------------------------------
void processJob(State* s, ReviewJob& job) {
    if (job.rgb.empty() || job.width <= 0 || job.height <= 0) return;
    if (job.rgb.size() <
        static_cast<size_t>(job.width) * static_cast<size_t>(job.height) * 3) {
        ZM_LOG_ERROR("llm_event_review: frame buffer too small for %dx%d RGB24",
                     job.width, job.height);
        return;
    }

    std::vector<uint8_t> jpeg;
    if (!encodeRgb24ToJpeg(job.rgb.data(), job.width, job.height, s->maxPixels,
                           jpeg)) {
        ZM_LOG_ERROR("llm_event_review: JPEG encode failed");
        return;
    }

    const std::string prompt = renderPrompt(s, job.label, job.triggerReason);

    const auto t0 = std::chrono::steady_clock::now();
    const std::string text = s->visionProvider->describe(jpeg, prompt);
    const auto t1 = std::chrono::steady_clock::now();
    const long ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    if (text.empty()) {
        ZM_LOG_ERROR("llm_event_review: empty VLM response (track=%d)",
                     job.trackId);
        return;
    }

    json evt;
    evt["type"] = "reasoning";
    evt["stream_id"] = job.streamId;
    evt["pts_usec"] = job.ptsUsec;
    evt["track_id"] = job.trackId;
    evt["label"] = job.label;
    evt["text"] = text;
    evt["model"] = s->visionProvider->model();
    evt["provider"] = s->visionProvider->provider_id();
    evt["trigger_event"] = job.triggerEvent;
    evt["trigger_reason"] = job.triggerReason;
    evt["inference_time_ms"] = ms;

    if (s->host && s->host->publish_evt) {
        s->host->publish_evt(s->hostCtx, evt.dump().c_str());
    }
    ZM_LOG_INFO("llm_event_review: track=%d %s -> %s", job.trackId,
                job.label.c_str(), text.c_str());
}

void workerLoop(State* s) {
    while (s->running.load()) {
        ReviewJob job;
        {
            std::unique_lock<std::mutex> lk(s->jobMutex);
            s->jobCv.wait(lk, [s] {
                return !s->running.load() || !s->jobs.empty();
            });
            if (!s->running.load() && s->jobs.empty()) break;
            if (s->jobs.empty()) continue;
            job = std::move(s->jobs.front());
            s->jobs.pop_front();
        }
        processJob(s, job);
    }
}

// ---------------------------------------------------------------------------
// EventBus callback: decide whether to review, snapshot the frame, enqueue.
// Runs on the publisher thread.
// ---------------------------------------------------------------------------
void handleEvent(State* s, const std::string& msg) {
    if (!s || !s->active.load()) return;

    json j;
    try { j = json::parse(msg); } catch (...) { return; }

    const std::string type = j.value("type", std::string());
    if (!wantsTrigger(s, type)) return;

    const uint32_t sid = j.value("stream_id", 0u);
    if (!allowedStream(s, sid)) return;
    const uint64_t pts = j.value("pts_usec", 0ull);

    // Normalize the two event shapes into a list of (track_id, label, reason).
    struct Hit { int trackId; std::string label; std::string reason; };
    std::vector<Hit> hits;

    if (type == "alert") {
        const int tid = j.value("track_id", 0);
        const std::string label = j.value("label", std::string());
        if (tid > 0 && allowedClass(s, label))
            hits.push_back({tid, label, j.value("reason", std::string())});
    } else if (type == "tracked_detection") {
        if (j.contains("detections") && j["detections"].is_array()) {
            for (const auto& d : j["detections"]) {
                const int tid = d.value("track_id", 0);
                if (tid <= 0) continue;
                const std::string label = d.value("label", std::string());
                if (!allowedClass(s, label)) continue;
                hits.push_back({tid, label, std::string()});
            }
        }
    }
    if (hits.empty()) return;

    // Snapshot the latest cached frame for this stream once for all hits.
    std::vector<uint8_t> rgb;
    int width = 0, height = 0;
    {
        std::lock_guard<std::mutex> lk(s->frameMutex);
        auto it = s->frames.find(sid);
        if (it == s->frames.end() || !it->second.have ||
            it->second.rgb.empty()) {
            // No frame cached yet for this stream; nothing to review.
            return;
        }
        rgb = it->second.rgb;  // copy out from under the lock
        width = it->second.width;
        height = it->second.height;
    }

    for (auto& h : hits) {
        // Per-track debounce: don't re-review the same track within N seconds.
        const uint64_t key = (static_cast<uint64_t>(sid) << 32) |
                             static_cast<uint32_t>(h.trackId);
        {
            // Debounce on WALL CLOCK, not pts_usec: a stream restart / RTSP
            // timestamp discontinuity must not silently disable de-dup (the whole
            // point of this plugin over describe_vlm is no double-reviewing).
            const auto now = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lk(s->dedupMutex);
            auto it = s->lastReview.find(key);
            if (it != s->lastReview.end() && s->debounceUsec > 0 &&
                now - it->second < std::chrono::microseconds(s->debounceUsec)) {
                continue;  // still inside the debounce window
            }
            s->lastReview[key] = now;
            // Prune tracks not seen for >30s so the map can't grow unbounded.
            for (auto p = s->lastReview.begin(); p != s->lastReview.end();) {
                if (now - p->second > std::chrono::seconds(30)) p = s->lastReview.erase(p);
                else ++p;
            }
        }

        ReviewJob job;
        job.streamId = sid;
        job.ptsUsec = pts;
        job.trackId = h.trackId;
        job.label = h.label;
        job.triggerEvent = type;
        job.triggerReason = h.reason;
        job.rgb = rgb;  // shared frame for every hit this event
        job.width = width;
        job.height = height;

        {
            std::lock_guard<std::mutex> lk(s->jobMutex);
            s->jobs.push_back(std::move(job));
        }
        s->jobCv.notify_one();
    }
}

// ---------------------------------------------------------------------------
// Plugin lifecycle.
// ---------------------------------------------------------------------------
int start(zm_plugin_t* plugin, zm_host_api_t* host, void* host_ctx,
          const char* json_cfg) {
    auto* s = new State();  // leaked on stop() for in-flight callback safety
    s->host = host;
    s->hostCtx = host_ctx;
    zm_plugin_set_log_context(host, host_ctx);

    if (json_cfg) {
        try {
            auto j = json::parse(json_cfg);
            s->provider = j.value("provider", s->provider);
            s->frameWidth = j.value("frame_width", s->frameWidth);
            s->frameHeight = j.value("frame_height", s->frameHeight);
            s->serverUrl = j.value("server_url", s->serverUrl);
            s->model = j.value("model", s->model);
            s->apiKey = j.value("api_key", s->apiKey);
            s->prompt = j.value("prompt", s->prompt);
            s->maxPixels = j.value("max_pixels", s->maxPixels);
            s->timeoutSec = j.value("timeout_sec", s->timeoutSec);
            s->enableThinking = j.value("enable_thinking", s->enableThinking);
            s->debounceUsec = static_cast<uint64_t>(
                j.value("debounce_sec", 10.0) * 1e6);

            if (j.contains("classes") && j["classes"].is_array())
                s->classes = j["classes"].get<std::vector<std::string>>();
            if (j.contains("stream_filter") && j["stream_filter"].is_array()) {
                for (const auto& v : j["stream_filter"]) {
                    if (v.is_number_integer() || v.is_number_unsigned())
                        s->streamFilter.push_back(v.get<uint32_t>());
                }
            }
            if (j.contains("trigger_events") &&
                j["trigger_events"].is_array() &&
                !j["trigger_events"].empty()) {
                s->triggerEvents =
                    j["trigger_events"].get<std::vector<std::string>>();
            }
        } catch (const std::exception& e) {
            ZM_LOG_ERROR("llm_event_review: failed to parse config: %s",
                         e.what());
            // continue with defaults
        }
    }

    s->visionProvider = llmrev::make_provider(
        s->provider, s->serverUrl, s->model, s->apiKey, s->timeoutSec,
        s->enableThinking);
    if (s->provider != "local" && s->provider != "openai")
        ZM_LOG_WARN("llm_event_review: provider '%s' uses the OpenAI-compatible backend; "
                    "native Anthropic/Gemini adapters are Phase 2", s->provider.c_str());
    if (s->frameWidth <= 0 || s->frameHeight <= 0)
        ZM_LOG_WARN("llm_event_review: frame_width/frame_height not set; cannot snapshot "
                    "frames for review (set them to the decoder's output dims)");

    plugin->instance = s;

    // libcurl global init. We do NOT curl_global_cleanup() on stop: it is
    // process-global and NOT refcounted, so tearing it down while another curl
    // plugin (describe_vlm / output_webhook) is still live is undefined behaviour.
    // Leak curl's globals for process lifetime (standard practice).
    curl_global_init(CURL_GLOBAL_DEFAULT);

    s->running.store(true);
    s->worker = std::thread(workerLoop, s);

    if (host && host->subscribe_evt) {
        s->sub = host->subscribe_evt(
            host_ctx,
            [](void* user, const char* js) {
                handleEvent(static_cast<State*>(user), js ? js : "");
            },
            s);
    }

    std::string triggers;
    for (const auto& t : s->triggerEvents)
        triggers += (triggers.empty() ? "" : ",") + t;
    ZM_LOG_INFO(
        "llm_event_review started (provider=%s, server=%s, model=%s, "
        "triggers=[%s], debounce=%.1fs)",
        s->provider.c_str(), s->serverUrl.c_str(), s->model.c_str(),
        triggers.c_str(),
        static_cast<double>(s->debounceUsec) / 1e6);
    return 0;
}

void stop(zm_plugin_t* plugin) {
    if (!plugin || !plugin->instance) return;
    auto* s = static_cast<State*>(plugin->instance);

    // Stop accepting new work; unhook from the bus first so no new callbacks.
    s->active.store(false);
    if (s->host && s->host->unsubscribe_evt && s->sub)
        s->host->unsubscribe_evt(s->hostCtx, s->sub);

    // Drain the worker.
    s->running.store(false);
    s->jobCv.notify_all();
    if (s->worker.joinable()) s->worker.join();

    plugin->instance = nullptr;  // State leaked on purpose: a racing EventBus
                                 // callback may still dereference it.
}

void on_frame(zm_plugin_t* plugin, const void* buf, size_t size) {
    auto* s = plugin ? static_cast<State*>(plugin->instance) : nullptr;
    if (!s || !buf || size < sizeof(zm_frame_hdr_t)) {
        if (s && s->host && s->host->on_frame && buf)
            s->host->on_frame(s->hostCtx, buf, size);
        return;
    }

    const auto* hdr = static_cast<const zm_frame_hdr_t*>(buf);
    const uint8_t* payload =
        static_cast<const uint8_t*>(buf) + sizeof(zm_frame_hdr_t);
    const size_t payloadSize = size - sizeof(zm_frame_hdr_t);

    // Cache the latest RGB24 frame per stream (exactly like describe_vlm).
    //
    // The frame header carries no width/height, so — as in describe_vlm — the
    // RGB24 dimensions come from config (frame_width/frame_height). The worker
    // additionally validates payloadSize >= width*height*3 before encoding, so a
    // misconfigured size is rejected rather than read out of bounds.
    if (hdr->hw_type == ZM_FRAME_RGB24 && allowedStream(s, hdr->stream_id)) {
        std::lock_guard<std::mutex> lk(s->frameMutex);
        auto& cf = s->frames[hdr->stream_id];
        cf.rgb.assign(payload, payload + payloadSize);
        cf.width = s->frameWidth;
        cf.height = s->frameHeight;
        cf.ptsUsec = hdr->pts_usec;
        cf.have = true;
    }

    if (s->host && s->host->on_frame)
        s->host->on_frame(s->hostCtx, buf, size);
}

}  // namespace

extern "C" __attribute__((visibility("default"))) void zm_plugin_init(
    zm_plugin_t* plugin) {
    if (!plugin) return;
    plugin->version = 1;
    plugin->type = ZM_PLUGIN_PROCESS;
    plugin->instance = nullptr;
    plugin->start = start;
    plugin->stop = stop;
    plugin->on_frame = on_frame;
}

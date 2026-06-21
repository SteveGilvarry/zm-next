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
// Per-stream frame ring (time-sampled thumbnails).
// ---------------------------------------------------------------------------
//
// The decode thread feeds frames at full rate; we keep only a thinned ring of
// recent thumbnails (sampled at montage_sample_fps, spanning montage_window_sec)
// so a montage can be assembled at review time without hoarding full-res frames.
//
struct TimedThumb {
    std::vector<uint8_t> rgb;  // downscaled RGB24
    int width = 0;
    int height = 0;
    std::chrono::steady_clock::time_point t;
};
struct StreamRing {
    std::deque<TimedThumb> frames;
    std::chrono::steady_clock::time_point lastSample{};
};

// ---------------------------------------------------------------------------
// Per-track history (the spine of the narrator's story).
// ---------------------------------------------------------------------------
struct TrackHist {
    std::string label;
    std::chrono::steady_clock::time_point t0{};       // first observation
    std::chrono::steady_clock::time_point lastSeen{};
    bool haveT0 = false;
    struct Caption { double tSec; std::string text; };
    std::vector<Caption> captions;                     // from each review
    struct Point { double tSec; float cx, cy; };       // normalized centroid
    std::vector<Point> traj;                            // from tracked_detection bbox
    bool narrated = false;                              // story already emitted?
};

// A queued job handed from the EventBus callback / narrator scan to the worker.
struct ReviewJob {
    enum class Kind { Review, Narrate } kind = Kind::Review;
    uint32_t streamId = 0;
    uint64_t ptsUsec = 0;
    int trackId = 0;
    std::string label;
    std::string triggerEvent;   // "alert" | "tracked_detection"
    std::string triggerReason;  // alert "reason", else ""
    // --- Review: the montage frames selected at enqueue time (shared across the
    //     hits of one event; the decode thread keeps mutating the live ring). ---
    std::shared_ptr<std::vector<TimedThumb>> frames;
    double spanSec = 0.0;
    // --- Narrate: the track's accumulated captions + trajectory. ---
    std::vector<TrackHist::Caption> timeline;
    std::vector<TrackHist::Point> traj;
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
    long maxPixels = 1003520;       // ~1 MP cap for a SINGLE-frame review

    // --- temporal montage (A) ---
    bool montage = true;            // assemble a multi-frame montage per event
    int montageFrames = 6;          // tiles in the montage (laid out cols x rows)
    int montageCols = 3;            // grid columns (rows derived from frames/cols)
    double montageWindowSec = 6.0;  // how far back the ring spans
    double montageSampleFps = 2.0;  // ring sampling rate (frames/sec kept)
    long montageMaxPixels = 400000; // whole-montage area cap (keeps visual tokens
                                    // within the server's max-model-len budget)
    long storeMaxPixels = 230400;   // per-thumbnail store cap (~640x360) -> bounds
                                    // ring memory: ~0.7MB * window*fps per stream
    std::string montagePrompt =
        "You are a security camera analyst reviewing a sequence of [n] frames "
        "from one event, shown in chronological order (left to right, top to "
        "bottom) spanning about [span_sec] seconds. Describe what the [label] "
        "does across the sequence in one or two sentences — focus on movement and "
        "intent (approaching, leaving, loitering, carrying or dropping "
        "something), not a static scene description. Note anything unusual.";

    // --- narrator / story synthesis (B) ---
    bool narrate = true;            // emit a track-close narrative
    double narratorIdleSec = 8.0;   // track silent this long -> synthesize story
    int narratorMinCaptions = 2;    // need at least this many reviews to narrate
    std::string narratorPrompt =
        "You are a security analyst. Below are sequential observations of a "
        "single tracked [label] over time, with its movement path. Write a "
        "2-3 sentence narrative of what this [label] did, emphasising movement "
        "and apparent intent. Do not mention frame numbers or raw coordinates.";

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

    // --- per-stream frame ring (decode thread writes, cb reads/snapshots) ---
    std::mutex frameMutex;
    std::unordered_map<uint32_t, StreamRing> rings;

    // --- per-track de-dup (EventBus-callback thread) ---
    std::mutex dedupMutex;
    // key = (stream_id<<32 | track_id) -> last review wall-clock time
    std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> lastReview;

    // --- per-track history for the narrator (cb writes traj, worker writes
    //     captions + scans for idle tracks). key = (stream_id<<32 | track_id). ---
    std::mutex trackMutex;
    std::unordered_map<uint64_t, TrackHist> trackHist;

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
// Bilinear RGB24 -> RGB24 rescale (aspect preserved, area capped at maxPixels).
// Used both to thumbnail frames into the ring and to size montage tiles.
// ---------------------------------------------------------------------------
bool scaleRgb24(const uint8_t* src, int sw, int sh, int dw, int dh,
                std::vector<uint8_t>& out) {
    out.clear();
    if (!src || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return false;
    SwsContext* sws = sws_getContext(sw, sh, AV_PIX_FMT_RGB24,
                                     dw, dh, AV_PIX_FMT_RGB24,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) return false;
    out.resize(static_cast<size_t>(dw) * dh * 3);
    const uint8_t* srcSlice[1] = {src};
    int srcStride[1] = {3 * sw};
    uint8_t* dstSlice[1] = {out.data()};
    int dstStride[1] = {3 * dw};
    sws_scale(sws, srcSlice, srcStride, 0, sh, dstSlice, dstStride);
    sws_freeContext(sws);
    return true;
}

// Pick a (dw,dh) preserving aspect with dw*dh <= maxPixels, even dims.
void fitDims(int sw, int sh, long maxPixels, int& dw, int& dh) {
    dw = sw; dh = sh;
    if (maxPixels > 0 &&
        static_cast<long>(sw) * sh > maxPixels) {
        const double s = std::sqrt(static_cast<double>(maxPixels) /
                                   (static_cast<double>(sw) * sh));
        dw = std::max(2, static_cast<int>(sw * s));
        dh = std::max(2, static_cast<int>(sh * s));
    }
    dw &= ~1; dh &= ~1;
    if (dw < 2) dw = 2;
    if (dh < 2) dh = 2;
}

// ---------------------------------------------------------------------------
// Montage builder: tile frames into one RGB24 canvas (row-major, a thin gray
// gutter between cells so the model reads them as discrete frames). The grid is
// `cols` wide and as many rows as needed; empty trailing cells stay gray.
// ---------------------------------------------------------------------------
bool buildMontage(const std::vector<TimedThumb>& frames, int cols,
                  long montageMaxPixels, std::vector<uint8_t>& out,
                  int& outW, int& outH) {
    out.clear();
    const int n = static_cast<int>(frames.size());
    if (n == 0 || cols <= 0) return false;
    const int rows = (n + cols - 1) / cols;

    // Source aspect from the first frame.
    const int sw = frames[0].width, sh = frames[0].height;
    if (sw <= 0 || sh <= 0) return false;
    const double aspect = static_cast<double>(sw) / sh;

    // Size a tile so the whole grid lands near montageMaxPixels.
    const double perTile =
        static_cast<double>(montageMaxPixels) / (cols * rows);
    int tileW = std::max(2, static_cast<int>(std::sqrt(perTile * aspect)));
    int tileH = std::max(2, static_cast<int>(tileW / aspect));
    tileW &= ~1; tileH &= ~1;
    if (tileW < 2) tileW = 2;
    if (tileH < 2) tileH = 2;

    const int g = 4;  // gutter px
    outW = cols * tileW + (cols + 1) * g;
    outH = rows * tileH + (rows + 1) * g;
    out.assign(static_cast<size_t>(outW) * outH * 3, 40);  // gray canvas

    for (int i = 0; i < n; ++i) {
        std::vector<uint8_t> tile;
        if (!scaleRgb24(frames[i].rgb.data(), frames[i].width, frames[i].height,
                        tileW, tileH, tile))
            continue;
        const int c = i % cols, r = i / cols;
        const int x0 = g + c * (tileW + g);
        const int y0 = g + r * (tileH + g);
        for (int y = 0; y < tileH; ++y) {
            uint8_t* dst = out.data() +
                (static_cast<size_t>(y0 + y) * outW + x0) * 3;
            const uint8_t* srcRow = tile.data() + static_cast<size_t>(y) * tileW * 3;
            std::memcpy(dst, srcRow, static_cast<size_t>(tileW) * 3);
        }
    }
    return true;
}

// Pick up to `want` frames evenly spaced across the ring (chronological order
// preserved). Fewer-than-want just returns them all.
std::vector<TimedThumb> selectMontageFrames(const std::deque<TimedThumb>& ring,
                                            int want) {
    std::vector<TimedThumb> sel;
    const int m = static_cast<int>(ring.size());
    if (m == 0 || want <= 0) return sel;
    if (m <= want) {
        sel.assign(ring.begin(), ring.end());
        return sel;
    }
    for (int i = 0; i < want; ++i) {
        const int idx = static_cast<int>(
            std::llround(static_cast<double>(i) * (m - 1) / (want - 1)));
        sel.push_back(ring[idx]);
    }
    return sel;
}

inline uint64_t trackKey(uint32_t sid, int tid) {
    return (static_cast<uint64_t>(sid) << 32) | static_cast<uint32_t>(tid);
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

// Montage prompt: the single-frame tokens plus [n] / [span_sec].
std::string renderMontagePrompt(const State* s, const std::string& label,
                                const std::string& reason, int n,
                                double spanSec) {
    std::string p = s->montagePrompt;
    auto sub = [&p](const std::string& tok, const std::string& val) {
        for (size_t pos = p.find(tok); pos != std::string::npos;
             pos = p.find(tok, pos)) {
            p.replace(pos, tok.size(), val);
            pos += val.size();
        }
    };
    char span[32];
    std::snprintf(span, sizeof(span), "%.1f", spanSec);
    sub("[label]", label);
    sub("[reason]", reason);
    sub("[trigger_reason]", reason);
    sub("[n]", std::to_string(n));
    sub("[span_sec]", span);
    return p;
}

// Narrator prompt: instruction + a compact timeline/trajectory data block built
// from the track's accumulated observations.
std::string buildNarratorPrompt(const State* s, const std::string& label,
                                const std::vector<TrackHist::Caption>& timeline,
                                const std::vector<TrackHist::Point>& traj) {
    std::string head = s->narratorPrompt;
    for (size_t pos = head.find("[label]"); pos != std::string::npos;
         pos = head.find("[label]", pos)) {
        head.replace(pos, 7, label);
        pos += label.size();
    }

    std::string block = "\n\nObservations (in order):\n";
    for (const auto& c : timeline) {
        char ts[24];
        std::snprintf(ts, sizeof(ts), "t+%.1fs: ", c.tSec);
        block += "- ";
        block += ts;
        block += c.text;
        block += "\n";
    }
    if (!traj.empty()) {
        block += "Movement path (normalized 0-1, x=right, y=down): ";
        for (size_t i = 0; i < traj.size(); ++i) {
            char pt[40];
            std::snprintf(pt, sizeof(pt), "%s(%.2f,%.2f)",
                          i ? " -> " : "", traj[i].cx, traj[i].cy);
            block += pt;
        }
        block += "\n";
    }
    return head + block;
}

// ---------------------------------------------------------------------------
// Background worker: pops jobs, encodes JPEG, calls the provider, publishes.
// ---------------------------------------------------------------------------
// --- (B) Narrator: text-only synthesis over a track's prior observations. ---
void processNarrate(State* s, ReviewJob& job) {
    if (job.timeline.empty()) return;

    const std::string prompt =
        buildNarratorPrompt(s, job.label, job.timeline, job.traj);

    const auto t0 = std::chrono::steady_clock::now();
    const std::string text = s->visionProvider->narrate(prompt);
    const auto t1 = std::chrono::steady_clock::now();
    const long ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    if (text.empty()) {
        ZM_LOG_ERROR("llm_event_review: empty narrator response (track=%d)",
                     job.trackId);
        return;
    }

    json evt;
    evt["type"] = "reasoning";
    evt["mode"] = "narrative";
    evt["stream_id"] = job.streamId;
    evt["track_id"] = job.trackId;
    evt["label"] = job.label;
    evt["text"] = text;
    evt["model"] = s->visionProvider->model();
    evt["provider"] = s->visionProvider->provider_id();
    evt["caption_count"] = static_cast<int>(job.timeline.size());
    evt["inference_time_ms"] = ms;
    if (s->host && s->host->publish_evt)
        s->host->publish_evt(s->hostCtx, evt.dump().c_str());
    ZM_LOG_INFO("llm_event_review: NARRATIVE track=%d %s -> %s", job.trackId,
                job.label.c_str(), text.c_str());
}

// --- (A) Review: single frame or temporal montage, then describe. ---
void processJob(State* s, ReviewJob& job) {
    if (job.kind == ReviewJob::Kind::Narrate) {
        processNarrate(s, job);
        return;
    }

    if (!job.frames || job.frames->empty()) return;
    const auto& frames = *job.frames;

    std::vector<uint8_t> jpeg;
    bool isMontage = false;
    if (s->montage && frames.size() >= 2) {
        std::vector<uint8_t> canvas;
        int cw = 0, ch = 0;
        if (buildMontage(frames, s->montageCols, s->montageMaxPixels, canvas,
                         cw, ch) &&
            encodeRgb24ToJpeg(canvas.data(), cw, ch, s->montageMaxPixels, jpeg)) {
            isMontage = true;
        }
    }
    if (!isMontage) {
        // Single-frame fallback: the latest (last) frame in the selection.
        const auto& f = frames.back();
        if (f.rgb.size() <
            static_cast<size_t>(f.width) * static_cast<size_t>(f.height) * 3 ||
            !encodeRgb24ToJpeg(f.rgb.data(), f.width, f.height, s->maxPixels,
                               jpeg)) {
            ZM_LOG_ERROR("llm_event_review: JPEG encode failed (track=%d)",
                         job.trackId);
            return;
        }
    }

    const int n = static_cast<int>(frames.size());
    const std::string prompt =
        isMontage ? renderMontagePrompt(s, job.label, job.triggerReason, n,
                                        job.spanSec)
                  : renderPrompt(s, job.label, job.triggerReason);

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
    evt["mode"] = isMontage ? "montage" : "single";
    evt["stream_id"] = job.streamId;
    evt["pts_usec"] = job.ptsUsec;
    evt["track_id"] = job.trackId;
    evt["label"] = job.label;
    evt["text"] = text;
    evt["model"] = s->visionProvider->model();
    evt["provider"] = s->visionProvider->provider_id();
    evt["trigger_event"] = job.triggerEvent;
    evt["trigger_reason"] = job.triggerReason;
    if (isMontage) {
        evt["frame_count"] = n;
        evt["span_sec"] = job.spanSec;
    }
    evt["inference_time_ms"] = ms;

    if (s->host && s->host->publish_evt) {
        s->host->publish_evt(s->hostCtx, evt.dump().c_str());
    }
    ZM_LOG_INFO("llm_event_review: track=%d %s [%s] -> %s", job.trackId,
                job.label.c_str(), isMontage ? "montage" : "single",
                text.c_str());

    // Record this caption into the track's history so the narrator can later
    // stitch it into a story. A fresh caption re-arms narration for the track.
    if (s->narrate && job.trackId > 0) {
        const uint64_t key = trackKey(job.streamId, job.trackId);
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lk(s->trackMutex);
        auto& th = s->trackHist[key];
        if (!th.haveT0) { th.t0 = now; th.haveT0 = true; }
        th.label = job.label;
        th.lastSeen = now;
        th.narrated = false;
        const double tSec =
            std::chrono::duration<double>(now - th.t0).count();
        th.captions.push_back({tSec, text});
    }
}

// Scan the track history for tracks that have gone idle (no detection for
// narratorIdleSec) and have enough captions to be worth narrating. Returns one
// Narrate job per such track and marks it narrated so it fires once. Also prunes
// long-dead tracks. Runs on the worker thread; takes only a brief lock.
std::vector<ReviewJob> collectNarrateJobs(State* s) {
    std::vector<ReviewJob> out;
    if (!s->narrate) return out;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(s->trackMutex);
    for (auto it = s->trackHist.begin(); it != s->trackHist.end();) {
        auto& th = it->second;
        const double idle =
            std::chrono::duration<double>(now - th.lastSeen).count();
        if (!th.narrated &&
            static_cast<int>(th.captions.size()) >= s->narratorMinCaptions &&
            idle >= s->narratorIdleSec) {
            ReviewJob nj;
            nj.kind = ReviewJob::Kind::Narrate;
            nj.streamId = static_cast<uint32_t>(it->first >> 32);
            nj.trackId = static_cast<int>(it->first & 0xffffffffu);
            nj.label = th.label;
            nj.timeline = th.captions;
            nj.traj = th.traj;
            out.push_back(std::move(nj));
            th.narrated = true;
        }
        // Drop tracks dead for >5 min so the maps can't grow unbounded.
        if (idle > 300.0) it = s->trackHist.erase(it);
        else ++it;
    }
    return out;
}

void workerLoop(State* s) {
    while (s->running.load()) {
        ReviewJob job;
        bool haveJob = false;
        {
            std::unique_lock<std::mutex> lk(s->jobMutex);
            // Wake on a job OR every 2s to run the narrator idle scan (events
            // may stop arriving, so we can't wait solely on the queue).
            s->jobCv.wait_for(lk, std::chrono::seconds(2), [s] {
                return !s->running.load() || !s->jobs.empty();
            });
            if (!s->running.load() && s->jobs.empty()) break;
            if (!s->jobs.empty()) {
                job = std::move(s->jobs.front());
                s->jobs.pop_front();
                haveJob = true;
            }
        }
        if (haveJob) processJob(s, job);

        // Narrator scan (skip while draining a busy review backlog to keep
        // event reviews latency-first; we'll catch idle tracks next tick).
        if (!haveJob) {
            for (auto& nj : collectNarrateJobs(s)) {
                if (!s->running.load()) break;
                processJob(s, nj);
            }
        }
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

    // Normalize the two event shapes into a list of hits. bbox (when present,
    // [x,y,w,h] in pixels) feeds the narrator's trajectory.
    struct Hit {
        int trackId; std::string label; std::string reason;
        bool hasBbox = false; float cx = 0, cy = 0;  // normalized centroid
    };
    std::vector<Hit> hits;

    // Centroid from a [x,y,w,h] bbox, normalized by the configured frame dims.
    auto centroid = [&](const json& obj, Hit& h) {
        if (s->frameWidth <= 0 || s->frameHeight <= 0) return;
        if (!obj.contains("bbox") || !obj["bbox"].is_array() ||
            obj["bbox"].size() < 4) return;
        const auto& b = obj["bbox"];
        const double x = b[0].get<double>(), y = b[1].get<double>();
        const double w = b[2].get<double>(), hgt = b[3].get<double>();
        h.cx = static_cast<float>((x + w / 2.0) / s->frameWidth);
        h.cy = static_cast<float>((y + hgt / 2.0) / s->frameHeight);
        h.hasBbox = true;
    };

    if (type == "alert") {
        const int tid = j.value("track_id", 0);
        const std::string label = j.value("label", std::string());
        if (tid > 0 && allowedClass(s, label)) {
            Hit h{tid, label, j.value("reason", std::string())};
            centroid(j, h);
            hits.push_back(std::move(h));
        }
    } else if (type == "tracked_detection") {
        if (j.contains("detections") && j["detections"].is_array()) {
            for (const auto& d : j["detections"]) {
                const int tid = d.value("track_id", 0);
                if (tid <= 0) continue;
                const std::string label = d.value("label", std::string());
                if (!allowedClass(s, label)) continue;
                Hit h{tid, label, std::string()};
                centroid(d, h);
                hits.push_back(std::move(h));
            }
        }
    }
    if (hits.empty()) return;

    // Record trajectory for EVERY hit (denser than reviews, which are debounced)
    // so the narrator gets a real path, not just the reviewed samples.
    if (s->narrate) {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lk(s->trackMutex);
        for (const auto& h : hits) {
            if (!h.hasBbox) continue;
            auto& th = s->trackHist[trackKey(sid, h.trackId)];
            if (!th.haveT0) { th.t0 = now; th.haveT0 = true; }
            th.label = h.label;
            th.lastSeen = now;
            const double tSec =
                std::chrono::duration<double>(now - th.t0).count();
            th.traj.push_back({tSec, h.cx, h.cy});
            if (th.traj.size() > 256)
                th.traj.erase(th.traj.begin(),
                              th.traj.begin() + (th.traj.size() - 256));
        }
    }

    // Snapshot the stream's frame ring once for all hits, then pick the montage
    // frames. Shared (shared_ptr) across every hit of this event so we copy the
    // selected frames out of the live ring exactly once.
    auto frames = std::make_shared<std::vector<TimedThumb>>();
    double spanSec = 0.0;
    {
        std::lock_guard<std::mutex> lk(s->frameMutex);
        auto it = s->rings.find(sid);
        if (it == s->rings.end() || it->second.frames.empty()) {
            // No frames cached yet for this stream; nothing to review.
            return;
        }
        *frames = selectMontageFrames(it->second.frames,
                                      s->montage ? s->montageFrames : 1);
    }
    if (frames->empty()) return;
    if (frames->size() >= 2)
        spanSec = std::chrono::duration<double>(frames->back().t -
                                                frames->front().t).count();

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
        job.kind = ReviewJob::Kind::Review;
        job.streamId = sid;
        job.ptsUsec = pts;
        job.trackId = h.trackId;
        job.label = h.label;
        job.triggerEvent = type;
        job.triggerReason = h.reason;
        job.frames = frames;       // shared montage frames for every hit
        job.spanSec = spanSec;

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

            // temporal montage (A)
            s->montage = j.value("montage", s->montage);
            s->montageFrames = j.value("montage_frames", s->montageFrames);
            s->montageCols = j.value("montage_cols", s->montageCols);
            s->montageWindowSec = j.value("montage_window_sec", s->montageWindowSec);
            s->montageSampleFps = j.value("montage_sample_fps", s->montageSampleFps);
            s->montageMaxPixels = j.value("montage_max_pixels", s->montageMaxPixels);
            s->storeMaxPixels = j.value("store_max_pixels", s->storeMaxPixels);
            s->montagePrompt = j.value("montage_prompt", s->montagePrompt);
            if (s->montageFrames < 1) s->montageFrames = 1;
            if (s->montageCols < 1) s->montageCols = 1;

            // narrator / story synthesis (B)
            s->narrate = j.value("narrate", s->narrate);
            s->narratorIdleSec = j.value("narrator_idle_sec", s->narratorIdleSec);
            s->narratorMinCaptions =
                j.value("narrator_min_captions", s->narratorMinCaptions);
            s->narratorPrompt = j.value("narrator_prompt", s->narratorPrompt);
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
        "triggers=[%s], debounce=%.1fs, montage=%s[%d frames/%.1fs], "
        "narrator=%s[idle=%.1fs])",
        s->provider.c_str(), s->serverUrl.c_str(), s->model.c_str(),
        triggers.c_str(),
        static_cast<double>(s->debounceUsec) / 1e6,
        s->montage ? "on" : "off", s->montageFrames, s->montageWindowSec,
        s->narrate ? "on" : "off", s->narratorIdleSec);
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

    // Feed the per-stream ring (thinned to montage_sample_fps; thumbnails only).
    //
    // The frame header carries no width/height, so — as in describe_vlm — the
    // RGB24 dimensions come from config (frame_width/frame_height). We validate
    // payloadSize >= width*height*3 before reading so a misconfigured size is
    // rejected rather than read out of bounds. Frames are downscaled to
    // store_max_pixels and time-sampled so the ring stays small (~MB/stream).
    if (hdr->hw_type == ZM_FRAME_RGB24 && allowedStream(s, hdr->stream_id) &&
        s->frameWidth > 0 && s->frameHeight > 0 &&
        payloadSize >= static_cast<size_t>(s->frameWidth) * s->frameHeight * 3) {
        const auto now = std::chrono::steady_clock::now();
        const double minInterval =
            s->montageSampleFps > 0 ? 1.0 / s->montageSampleFps : 0.0;

        std::lock_guard<std::mutex> lk(s->frameMutex);
        auto& ring = s->rings[hdr->stream_id];
        const bool due = ring.frames.empty() ||
            std::chrono::duration<double>(now - ring.lastSample).count() >=
                minInterval;
        if (due) {
            int dw, dh;
            fitDims(s->frameWidth, s->frameHeight, s->storeMaxPixels, dw, dh);
            TimedThumb tt;
            tt.t = now;
            if (dw == s->frameWidth && dh == s->frameHeight) {
                tt.rgb.assign(payload, payload + payloadSize);
                tt.width = s->frameWidth;
                tt.height = s->frameHeight;
            } else if (scaleRgb24(payload, s->frameWidth, s->frameHeight, dw, dh,
                                  tt.rgb)) {
                tt.width = dw;
                tt.height = dh;
            }
            if (!tt.rgb.empty()) {
                ring.frames.push_back(std::move(tt));
                ring.lastSample = now;
                // Evict frames older than the montage window (plus a hard cap).
                while (!ring.frames.empty() &&
                       std::chrono::duration<double>(
                           now - ring.frames.front().t).count() >
                           s->montageWindowSec) {
                    ring.frames.pop_front();
                }
                while (ring.frames.size() > 64) ring.frames.pop_front();
            }
        }
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

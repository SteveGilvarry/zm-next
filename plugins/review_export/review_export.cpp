// review_export: motion-synopsis ingredient exporter (PROCESS plugin).
//
// Emits the "tubes" + plate references a video-synopsis renderer (zm-api) needs,
// as a cheap byproduct of the detection pipeline. See docs/Motion_Synopsis.md.
//
// Placement: a PROCESS node downstream of decode_ffmpeg (RGB24) so it receives
// decoded frames via the normal chain; it also subscribes to the EventBus for
//   - "tracked_detection"  (tracker output: per-object bbox + track_id + polygon)
//   - "background_plate"    (motion_pixel_diff: latest plate side-file path)
//   - "RecordingOpening" / "EventClip" (store: the recording lifecycle window)
//
// For each tracked object it samples (<= sample_fps/track) the matching frame
// from a small ring, rasterises the seg polygon to a feathered matte, premultiplies
// the RGB crop (background -> black), downscales it, and MJPEG-encodes a cutout.
// On EventClip it writes the cutouts + chosen plate under {event_dir}/synopsis/
// and publishes a "review_assets" event (mapped to wire EVENT 0x0306, TLV 0x10).
//
// Lifetime: state is leaked on stop() (after unsubscribe + running=false) so an
// in-flight host callback never dangles, matching tracker/store.

#include <zm_plugin.h>
#include <nlohmann/json.hpp>
#include "image_encode.hpp"
#include "review_matte.hpp"
#include "base64.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct Plate {
    std::string path;
    uint64_t wallclock_ms = 0;
    int w = 0, h = 0;
    std::string illum;
};

struct Sample {
    int64_t pts_us = 0;
    uint64_t wallclock_ms = 0;
    std::array<float, 4> bbox{0, 0, 0, 0};            // source coords
    std::vector<std::array<float, 2>> polygon;        // source coords (empty if alpha used)
    std::vector<uint8_t> alpha;                       // soft matte (bbox-local, alpha_w*alpha_h)
    int alpha_w = 0, alpha_h = 0;                      // 0 => no soft mask (use polygon)
    std::vector<uint8_t> jpeg;                         // premultiplied RGB cutout
    int cutout_w = 0, cutout_h = 0;
};

struct Tube {
    std::string label;
    int class_id = -1;
    int64_t t_start_us = 0, t_end_us = 0;
    int64_t last_sample_pts = INT64_MIN;
    std::vector<Sample> samples;
};

struct RingFrame {
    int64_t pts_us = 0;
    std::vector<uint8_t> rgb;   // frame_w * frame_h * 3
};

struct State {
    zm_host_api_t* host = nullptr;
    void* hostCtx = nullptr;
    std::atomic<bool> running{false};
    void* subHandle = nullptr;

    // Config.
    int monitorId = 0;
    int frameW = 0, frameH = 0;          // decoded RGB24 source dims (== detect frame dims)
    int sampleFps = 4;
    int cutoutMaxEdge = 256;
    int feather = 1;                     // box-blur passes on the matte edge
    size_t ringSize = 8;
    int maxSamples = 2000;               // cap buffered tube samples per collection
    int64_t matchTolUs = 100000;         // nearest-frame match window (100ms)
    std::vector<int> streamFilter;

    std::mutex mtx;

    // pts -> wallclock anchor.
    bool haveAnchor = false;
    int64_t ptsAnchor = 0;
    uint64_t wallAnchorMs = 0;

    std::deque<RingFrame> ring;          // recent decoded frames
    std::deque<Plate> plates;            // recent background plates (cap a few)

    // Current recording collection (one in-flight clip; store is single-recording).
    bool collecting = false;
    std::string clipToken;
    int64_t collFirstPts = 0, collLastPts = 0;
    bool haveCollPts = false;
    std::map<uint64_t, Tube> tubes;      // by track_id
    size_t bufferedSamples = 0;          // total samples across tubes this collection
    bool bufferWarned = false;
};

struct Ctx {
    zm_host_api_t* host = nullptr;
    void* hostCtx = nullptr;
    State* state = nullptr;
};

uint64_t wallclock_for(State* st, int64_t pts_us) {
    if (!st->haveAnchor) return 0;
    return st->wallAnchorMs + static_cast<uint64_t>((pts_us - st->ptsAnchor) / 1000);
}

// Build a premultiplied, downscaled, MJPEG-encoded cutout for one detection.
// Returns false if the crop is degenerate or encoding fails.
bool build_cutout(const RingFrame& f, int fw, int fh, const std::array<float, 4>& bbox,
                  const std::vector<std::array<float, 2>>& poly,
                  const std::vector<uint8_t>& alpha, int alphaW, int alphaH,
                  int maxEdge, int feather,
                  std::vector<uint8_t>& jpegOut, int& outW, int& outH) {
    const int ix = std::clamp(static_cast<int>(std::floor(bbox[0])), 0, fw - 1);
    const int iy = std::clamp(static_cast<int>(std::floor(bbox[1])), 0, fh - 1);
    const int iw = std::clamp(static_cast<int>(std::round(bbox[2])), 1, fw - ix);
    const int ih = std::clamp(static_cast<int>(std::round(bbox[3])), 1, fh - iy);
    if (iw < 2 || ih < 2) return false;

    // Crop RGB.
    std::vector<uint8_t> crop(static_cast<size_t>(iw) * ih * 3);
    for (int y = 0; y < ih; ++y) {
        const uint8_t* srow = f.rgb.data() + (static_cast<size_t>(iy + y) * fw + ix) * 3;
        std::memcpy(crop.data() + static_cast<size_t>(y) * iw * 3, srow,
                    static_cast<size_t>(iw) * 3);
    }

    // Matte: prefer the soft alpha (upsampled to the crop) when present, else
    // rasterise the coarse polygon. Feather, then premultiply.
    std::vector<uint8_t> mask;
    if (!alpha.empty() && alphaW > 0 && alphaH > 0) {
        zm::review::upsample_alpha(alpha, alphaW, alphaH, mask, iw, ih);
    } else {
        zm::review::fill_polygon(mask, iw, ih, poly, static_cast<float>(ix),
                                 static_cast<float>(iy));
    }
    for (int i = 0; i < feather; ++i) zm::review::box_blur3(mask, iw, ih);
    zm::review::premultiply_rgb(crop, mask);

    // Downscale (nearest) to <= maxEdge on the long side.
    int dw = iw, dh = ih;
    const int longEdge = std::max(iw, ih);
    if (longEdge > maxEdge) {
        const double s = static_cast<double>(maxEdge) / longEdge;
        dw = std::max(1, static_cast<int>(iw * s));
        dh = std::max(1, static_cast<int>(ih * s));
    }
    const uint8_t* encSrc = crop.data();
    std::vector<uint8_t> scaled;
    if (dw != iw || dh != ih) {
        scaled.resize(static_cast<size_t>(dw) * dh * 3);
        for (int y = 0; y < dh; ++y) {
            const int sy = std::min(ih - 1, y * ih / dh);
            for (int x = 0; x < dw; ++x) {
                const int sx = std::min(iw - 1, x * iw / dw);
                const uint8_t* sp = crop.data() + (static_cast<size_t>(sy) * iw + sx) * 3;
                uint8_t* dp = scaled.data() + (static_cast<size_t>(y) * dw + x) * 3;
                dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2];
            }
        }
        encSrc = scaled.data();
    }

    if (!zm::img::encode_rgb24_to_jpeg(encSrc, dw, dh, jpegOut)) return false;
    outW = dw; outH = dh;
    return true;
}

// Find the ring frame nearest `pts` within tolerance; nullptr if none.
const RingFrame* nearest_frame(State* st, int64_t pts) {
    const RingFrame* best = nullptr;
    int64_t bestd = st->matchTolUs + 1;
    for (const auto& f : st->ring) {
        const int64_t d = std::llabs(f.pts_us - pts);
        if (d < bestd) { bestd = d; best = &f; }
    }
    return (bestd <= st->matchTolUs) ? best : nullptr;
}

void reset_collection(State* st) {
    st->collecting = false;
    st->clipToken.clear();
    st->haveCollPts = false;
    st->tubes.clear();
    st->bufferedSamples = 0;
    st->bufferWarned = false;
}

// Finalize the current collection: write assets under {event_dir}/synopsis/ and
// return the manifest JSON to publish (empty if nothing to emit). Caller holds
// the lock; publish happens AFTER unlocking to avoid re-entrant deadlock.
std::string finalize_locked(State* st, long event_id, const std::string& clip_path,
                            const std::string& cause) {
    if (st->tubes.empty() || clip_path.empty()) return std::string();

    const fs::path eventDir = fs::path(clip_path).parent_path();
    const fs::path synDir = eventDir / "synopsis";
    std::error_code ec;
    fs::create_directories(synDir, ec);
    if (ec) return std::string();

    json manifest;
    manifest["type"] = "review_assets";
    manifest["schema"] = 1;
    manifest["monitor_id"] = st->monitorId;
    manifest["event_id"] = event_id;
    manifest["clip_token"] = st->clipToken;
    manifest["clip_path"] = clip_path;
    manifest["path_base"] = "synopsis";   // relative to dirname(clip_path)
    manifest["t_start_us"] = st->haveCollPts ? st->collFirstPts : 0;
    manifest["t_end_us"] = st->haveCollPts ? st->collLastPts : 0;
    manifest["source_w"] = st->frameW;
    manifest["source_h"] = st->frameH;
    manifest["sample_fps"] = st->sampleFps;
    manifest["cause"] = cause;

    // Plate(s): copy the most recent plate into synopsis/ and reference it.
    json platesJson = json::array();
    if (!st->plates.empty()) {
        const Plate& pl = st->plates.back();
        const std::string base = fs::path(pl.path).filename().string();
        const fs::path dst = synDir / base;
        fs::copy_file(pl.path, dst, fs::copy_options::overwrite_existing, ec);
        if (!ec) {
            platesJson.push_back({{"path", base}, {"wallclock_ms", pl.wallclock_ms},
                                  {"w", pl.w}, {"h", pl.h}, {"illum", pl.illum}});
        }
    }
    manifest["plates"] = std::move(platesJson);

    json tubesJson = json::array();
    for (auto& [tid, tube] : st->tubes) {
        if (tube.samples.empty()) continue;
        const std::string tdir = "t" + std::to_string(tid);
        fs::create_directories(synDir / tdir, ec);
        json samplesJson = json::array();
        int seq = 0;
        for (auto& s : tube.samples) {
            char name[32];
            std::snprintf(name, sizeof(name), "%06d.jpg", seq++);
            const fs::path outPath = synDir / tdir / name;
            std::ofstream f(outPath, std::ios::binary);
            if (!f) continue;
            f.write(reinterpret_cast<const char*>(s.jpeg.data()),
                    static_cast<std::streamsize>(s.jpeg.size()));
            if (!f) continue;
            // Carry the matte zm-api needs for true alpha compositing: the soft
            // alpha (downscaled, base64) when present, else the coarse polygon.
            json mask;
            if (!s.alpha.empty() && s.alpha_w > 0 && s.alpha_h > 0) {
                mask = {{"format", "alpha"}, {"w", s.alpha_w}, {"h", s.alpha_h},
                        {"data", zm::b64::encode(s.alpha)}};
            } else {
                json poly = json::array();
                for (const auto& p : s.polygon) poly.push_back({p[0], p[1]});
                mask = {{"format", "polygon"}, {"points", std::move(poly)}};
            }
            samplesJson.push_back({
                {"pts_us", s.pts_us}, {"wallclock_ms", s.wallclock_ms},
                {"bbox", {s.bbox[0], s.bbox[1], s.bbox[2], s.bbox[3]}},
                {"cutout", tdir + "/" + name},
                {"cutout_w", s.cutout_w}, {"cutout_h", s.cutout_h},
                {"mask", std::move(mask)}});
        }
        if (samplesJson.empty()) continue;
        tubesJson.push_back({{"track_id", tid}, {"label", tube.label},
                             {"class_id", tube.class_id},
                             {"t_start_us", tube.t_start_us},
                             {"t_end_us", tube.t_end_us},
                             {"samples", std::move(samplesJson)}});
    }
    manifest["tubes"] = std::move(tubesJson);

    if (manifest["tubes"].empty()) return std::string();

    // Also drop the manifest on disk for debugging / regeneration.
    const std::string dumped = manifest.dump();
    {
        std::ofstream mf(synDir / "manifest.json", std::ios::binary);
        if (mf) mf << dumped;
    }
    return dumped;
}

// ---- event handling -------------------------------------------------------

void on_tracked_detection(State* st, const json& j) {
    if (!st->collecting) return;
    const int streamId = j.value("stream_id", 0);
    if (!st->streamFilter.empty() &&
        std::find(st->streamFilter.begin(), st->streamFilter.end(), streamId) ==
            st->streamFilter.end())
        return;
    if (!j.contains("detections") || !j["detections"].is_array()) return;
    const int64_t pts = j.value("pts_usec", static_cast<int64_t>(0));

    const RingFrame* frame = nearest_frame(st, pts);
    if (!frame) return;   // sample frame dropped under load — tolerate (skip)

    const int64_t minGap = (st->sampleFps > 0) ? (1000000 / st->sampleFps) : 0;

    for (const auto& d : j["detections"]) {
        // Guard against an unbounded collection (a RecordingOpening that never
        // gets a matching EventClip, or a very long event): stop sampling once the
        // buffer cap is hit; tubes already collected still emit on EventClip.
        if (st->bufferedSamples >= static_cast<size_t>(st->maxSamples)) {
            if (!st->bufferWarned) {
                ZM_LOG_WARN("review_export: sample cap %d reached for clip %s; "
                            "further samples dropped this event",
                            st->maxSamples, st->clipToken.c_str());
                st->bufferWarned = true;
            }
            break;
        }
        const uint64_t tid = d.value("track_id", static_cast<uint64_t>(0));
        if (tid == 0) continue;
        if (!d.contains("bbox") || !d["bbox"].is_array() || d["bbox"].size() < 4) continue;

        Tube& tube = st->tubes[tid];
        if (tube.last_sample_pts != INT64_MIN && (pts - tube.last_sample_pts) < minGap)
            continue;   // throttle per track

        std::array<float, 4> bbox{d["bbox"][0].get<float>(), d["bbox"][1].get<float>(),
                                  d["bbox"][2].get<float>(), d["bbox"][3].get<float>()};
        // Matte: a soft alpha mask (detect_seg P4) supersedes the coarse polygon.
        std::vector<std::array<float, 2>> poly;
        std::vector<uint8_t> alpha;
        int alphaW = 0, alphaH = 0;
        if (d.contains("mask") && d["mask"].is_object() &&
            d["mask"].value("format", std::string()) == "alpha") {
            const auto& m = d["mask"];
            alphaW = m.value("w", 0);
            alphaH = m.value("h", 0);
            alpha = zm::b64::decode(m.value("data", std::string()));
            if (alpha.size() != static_cast<size_t>(alphaW) * alphaH) {
                alpha.clear(); alphaW = alphaH = 0;   // corrupt — fall back to polygon
            }
        }
        if (alpha.empty() && d.contains("polygon") && d["polygon"].is_array()) {
            for (const auto& p : d["polygon"])
                if (p.is_array() && p.size() >= 2)
                    poly.push_back({p[0].get<float>(), p[1].get<float>()});
        }

        Sample s;
        if (!build_cutout(*frame, st->frameW, st->frameH, bbox, poly, alpha, alphaW, alphaH,
                          st->cutoutMaxEdge, st->feather, s.jpeg, s.cutout_w, s.cutout_h))
            continue;
        s.pts_us = pts;
        s.wallclock_ms = wallclock_for(st, pts);
        s.bbox = bbox;
        s.polygon = std::move(poly);
        s.alpha = std::move(alpha);
        s.alpha_w = alphaW;
        s.alpha_h = alphaH;

        if (tube.samples.empty()) {
            tube.label = d.value("label", std::string());
            tube.class_id = d.value("class_id", -1);
            tube.t_start_us = pts;
        }
        tube.t_end_us = pts;
        tube.last_sample_pts = pts;
        tube.samples.push_back(std::move(s));
        ++st->bufferedSamples;
    }
}

void on_background_plate(State* st, const json& j) {
    Plate pl;
    pl.path = j.value("path", std::string());
    pl.wallclock_ms = j.value("wallclock_ms", static_cast<uint64_t>(0));
    pl.w = j.value("w", 0);
    pl.h = j.value("h", 0);
    pl.illum = j.value("illum", std::string());
    if (pl.path.empty()) return;
    st->plates.push_back(std::move(pl));
    while (st->plates.size() > 8) st->plates.pop_front();
}

void handleEvent(State* st, const std::string& msg) {
    if (!st || !st->running.load()) return;
    json j;
    try { j = json::parse(msg); } catch (...) { return; }
    if (!j.is_object()) return;

    const std::string type = j.value("type", std::string());
    const std::string event = j.value("event", std::string());

    // Publish OUTSIDE the lock (publish_evt re-enters subscribers synchronously).
    std::string toPublish;
    {
        std::lock_guard<std::mutex> lk(st->mtx);
        if (type == "tracked_detection") {
            on_tracked_detection(st, j);
        } else if (type == "background_plate") {
            on_background_plate(st, j);
        } else if (event == "RecordingOpening") {
            reset_collection(st);
            st->collecting = true;
            st->clipToken = j.value("clip_token", std::string());
        } else if (event == "EventClip") {
            if (st->collecting) {
                toPublish = finalize_locked(
                    st, static_cast<long>(j.value("event_id", 0)),
                    j.value("path", std::string()), j.value("cause", std::string()));
                reset_collection(st);
            }
        }
    }
    if (!toPublish.empty() && st->host && st->host->publish_evt)
        st->host->publish_evt(st->hostCtx, toPublish.c_str());
}

void forwardFrame(Ctx* ctx, const void* buf, size_t size) {
    if (ctx && ctx->host && ctx->host->on_frame)
        ctx->host->on_frame(ctx->hostCtx, buf, size);
}

int review_export_start(zm_plugin_t* plugin, zm_host_api_t* host, void* host_ctx,
                        const char* json_cfg) {
    auto* ctx = new Ctx();
    ctx->host = host;
    ctx->hostCtx = host_ctx;
    zm_plugin_set_log_context(host, host_ctx);

    auto* st = new State();   // leaked on stop (see Ctx)
    st->host = host;
    st->hostCtx = host_ctx;
    try {
        auto j = json::parse(json_cfg ? json_cfg : "{}");
        st->monitorId = j.value("monitor_id", 0);
        st->frameW = j.value("frame_width", 0);
        st->frameH = j.value("frame_height", 0);
        st->sampleFps = j.value("sample_fps", 4);
        st->cutoutMaxEdge = j.value("cutout_max_edge", 256);
        st->feather = j.value("feather", 1);
        st->ringSize = j.value("ring_size", 8);
        st->maxSamples = j.value("max_samples", 2000);
        if (j.contains("stream_filter") && j["stream_filter"].is_array())
            st->streamFilter = j["stream_filter"].get<std::vector<int>>();
    } catch (const std::exception& e) {
        ZM_LOG_ERROR("review_export: failed to parse config: %s", e.what());
    }
    if (st->ringSize < 1) st->ringSize = 1;
    if (st->ringSize > 64) st->ringSize = 64;   // bound ring memory (full RGB frames)
    if (st->maxSamples < 1) st->maxSamples = 1;

    st->running.store(true);
    ctx->state = st;
    plugin->instance = ctx;

    if (host && host->subscribe_evt) {
        st->subHandle = host->subscribe_evt(
            host_ctx,
            [](void* user, const char* json) {
                handleEvent(static_cast<State*>(user), json ? json : "");
            },
            st);
    }
    ZM_LOG_INFO("review_export: started (monitor=%d %dx%d sample_fps=%d max_edge=%d)",
                st->monitorId, st->frameW, st->frameH, st->sampleFps, st->cutoutMaxEdge);
    return 0;
}

void review_export_stop(zm_plugin_t* plugin) {
    if (!plugin || !plugin->instance) return;
    auto* ctx = static_cast<Ctx*>(plugin->instance);
    if (ctx->state) {
        if (ctx->host && ctx->host->unsubscribe_evt)
            ctx->host->unsubscribe_evt(ctx->hostCtx, ctx->state->subHandle);
        ctx->state->running.store(false);   // state intentionally leaked
    }
    delete ctx;
    plugin->instance = nullptr;
}

void review_export_on_frame(zm_plugin_t* plugin, const void* buf, size_t size) {
    auto* ctx = static_cast<Ctx*>(plugin ? plugin->instance : nullptr);
    if (!ctx || !ctx->state || !buf || size < sizeof(zm_frame_hdr_t)) {
        forwardFrame(ctx, buf, size);
        return;
    }
    State* st = ctx->state;
    const zm_frame_hdr_t* hdr = static_cast<const zm_frame_hdr_t*>(buf);
    const uint8_t* payload = static_cast<const uint8_t*>(buf) + sizeof(zm_frame_hdr_t);

    // Only ring decoded RGB24 frames of the right size (and selected stream).
    const bool wanted =
        hdr->hw_type == ZM_FRAME_RGB24 && st->frameW > 0 && st->frameH > 0 &&
        size >= sizeof(zm_frame_hdr_t) + static_cast<size_t>(st->frameW) * st->frameH * 3 &&
        (st->streamFilter.empty() ||
         std::find(st->streamFilter.begin(), st->streamFilter.end(),
                   static_cast<int>(hdr->stream_id)) != st->streamFilter.end());
    if (wanted) {
        std::lock_guard<std::mutex> lk(st->mtx);
        if (!st->haveAnchor) {
            st->haveAnchor = true;
            st->ptsAnchor = static_cast<int64_t>(hdr->pts_usec);
            st->wallAnchorMs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
        }
        RingFrame rf;
        rf.pts_us = static_cast<int64_t>(hdr->pts_usec);
        rf.rgb.assign(payload, payload + static_cast<size_t>(st->frameW) * st->frameH * 3);
        const int64_t framePts = rf.pts_us;
        st->ring.push_back(std::move(rf));
        while (st->ring.size() > st->ringSize) st->ring.pop_front();
        if (st->collecting) {
            if (!st->haveCollPts) { st->collFirstPts = framePts; st->haveCollPts = true; }
            st->collLastPts = framePts;
        }
    }
    forwardFrame(ctx, buf, size);
}

}  // namespace

extern "C" __attribute__((visibility("default"))) void zm_plugin_init(zm_plugin_t* plugin) {
    if (!plugin) return;
    plugin->version = 1;
    plugin->type = ZM_PLUGIN_PROCESS;
    plugin->instance = nullptr;
    plugin->start = review_export_start;
    plugin->stop = review_export_stop;
    plugin->on_frame = review_export_on_frame;
}

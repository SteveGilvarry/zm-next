// recognize_face: two-stage face recognition plugin (ONNX Runtime C++ API).
//
// Stage 1 — a FACE DETECTOR ONNX model produces face boxes. Stage 2 — a face
// EMBEDDER (ArcFace / MobileFaceNet-style) ONNX model produces a 512-D
// embedding per detected face. Each embedding is matched by cosine similarity
// against a configured gallery of known people; a single "face" recognition
// event is published when one or more faces are present.
//
// This is a pass-through DETECT stage: the frame is ALWAYS forwarded downstream.
// If either model is missing/unloadable, the frame is not RGB24, or the stream
// is filtered out, the plugin simply forwards.
//
// IMPORTANT: the face DETECTOR must be exported NMS-free, exactly like
// detect_onnx expects — i.e. a YOLO26-style output tensor [1, N, 6] of
// (x1, y1, x2, y2, conf, class). We reuse zm::detect::decode_nms_free to turn
// it into source-pixel face boxes.

#include "face_match.hpp"
#include "../detect_onnx/detect_postprocess.hpp"

#include <onnxruntime_cxx_api.h>
#include <coreml_provider_factory.h>

#include <nlohmann/json.hpp>

#include <array>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <algorithm>

#include "zm_plugin.h"

using json = nlohmann::json;

namespace {

struct RecognizeFaceCtx {
    zm_host_api_t* host = nullptr;
    void* hostCtx = nullptr;

    // ONNX Runtime state (shared env + options, one session per stage).
    std::unique_ptr<Ort::Env> env;
    Ort::SessionOptions sessionOptions;

    std::unique_ptr<Ort::Session> detector;
    std::string detInputName;
    std::string detOutputName;

    std::unique_ptr<Ort::Session> embedder;
    std::string embInputName;
    std::string embOutputName;

    // Config.
    std::string detectorModelPath;
    std::string embedderModelPath;
    int net = 640;                 // detector input_size
    int embedSize = 112;           // embedder input (square)
    float confThreshold = 0.5f;    // face-detection confidence
    float matchThreshold = 0.5f;   // cosine match threshold
    float embedMean = 127.5f;      // ArcFace preprocessing: (pixel - mean) / scale
    float embedScale = 128.0f;
    int frameWidth = 0;
    int frameHeight = 0;
    std::string ep = "cpu";
    std::vector<int> streamFilter; // empty = all

    std::vector<zm::face::GalleryEntry> gallery;

    bool warnedUnsupportedShape = false;
};

void forwardFrame(RecognizeFaceCtx* ctx, const void* buf, size_t size) {
    if (ctx && ctx->host && ctx->host->on_frame)
        ctx->host->on_frame(ctx->hostCtx, buf, size);
}

// Create a session from `path`, capturing its first input/output names.
// Returns nullptr (and logs) on failure so the caller can run pass-through.
std::unique_ptr<Ort::Session> makeSession(RecognizeFaceCtx* ctx, const std::string& path,
                                          const char* tag, std::string& inName,
                                          std::string& outName) {
    if (path.empty() || !ctx->env) return nullptr;
    try {
        auto session = std::make_unique<Ort::Session>(*ctx->env, path.c_str(),
                                                      ctx->sessionOptions);
        Ort::AllocatorWithDefaultOptions allocator;
        auto in = session->GetInputNameAllocated(0, allocator);
        auto out = session->GetOutputNameAllocated(0, allocator);
        inName = in.get();
        outName = out.get();
        ZM_LOG_INFO("recognize_face: loaded %s model '%s' (input='%s' output='%s')",
                    tag, path.c_str(), inName.c_str(), outName.c_str());
        return session;
    } catch (const std::exception& e) {
        ZM_LOG_ERROR("recognize_face: failed to load %s model '%s': %s",
                     tag, path.c_str(), e.what());
        return nullptr;
    }
}

// Run the embedder on an embedSize x embedSize RGB24 crop, returning an
// L2-normalized embedding (empty on failure).
std::vector<float> runEmbedder(RecognizeFaceCtx* ctx, const uint8_t* rgb) {
    std::vector<float> emb;
    if (!ctx->embedder) return emb;
    const int es = ctx->embedSize;
    const int plane = es * es;

    // Interleaved RGB24 -> planar CHW float, normalized (pixel - mean) / scale.
    std::vector<float> input(static_cast<size_t>(3) * plane);
    for (int y = 0; y < es; ++y) {
        for (int x = 0; x < es; ++x) {
            for (int c = 0; c < 3; ++c) {
                const float p = rgb[(y * es + x) * 3 + c];
                input[c * plane + y * es + x] = (p - ctx->embedMean) / ctx->embedScale;
            }
        }
    }

    try {
        Ort::MemoryInfo memInfo =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::array<int64_t, 4> inputShape{1, 3, es, es};
        Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
            memInfo, input.data(), input.size(), inputShape.data(), inputShape.size());

        const char* inputNames[] = {ctx->embInputName.c_str()};
        const char* outputNames[] = {ctx->embOutputName.c_str()};

        auto outputs = ctx->embedder->Run(Ort::RunOptions{nullptr}, inputNames,
                                          &inputTensor, 1, outputNames, 1);
        const float* out = outputs[0].GetTensorData<float>();
        auto shape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
        size_t n = 1;
        for (auto d : shape) if (d > 0) n *= static_cast<size_t>(d);
        emb.assign(out, out + n);
        zm::face::l2_normalize(emb);
    } catch (const std::exception& e) {
        ZM_LOG_ERROR("recognize_face: embedder inference error: %s", e.what());
        emb.clear();
    }
    return emb;
}

// Run the detector on a w x h RGB24 frame, returning source-pixel face boxes.
std::vector<zm::detect::Box> runDetector(RecognizeFaceCtx* ctx, const uint8_t* rgb,
                                         int w, int h) {
    std::vector<zm::detect::Box> boxes;
    if (!ctx->detector) return boxes;
    try {
        const int net = ctx->net;
        zm::detect::Letterbox lb = zm::detect::compute_letterbox(w, h, net);

        std::vector<float> input(static_cast<size_t>(3) * net * net);
        zm::detect::letterbox_rgb_to_chw(rgb, lb, input.data());

        Ort::MemoryInfo memInfo =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::array<int64_t, 4> inputShape{1, 3, net, net};
        Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
            memInfo, input.data(), input.size(), inputShape.data(), inputShape.size());

        const char* inputNames[] = {ctx->detInputName.c_str()};
        const char* outputNames[] = {ctx->detOutputName.c_str()};

        auto outputs = ctx->detector->Run(Ort::RunOptions{nullptr}, inputNames,
                                          &inputTensor, 1, outputNames, 1);
        const float* out = outputs[0].GetTensorData<float>();
        auto shape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();

        // Only the NMS-free output [1, N, 6] is supported (matches detect_onnx).
        const int64_t lastDim = shape.empty() ? 0 : shape.back();
        if (lastDim != 6) {
            if (!ctx->warnedUnsupportedShape) {
                ZM_LOG_WARN("recognize_face: unsupported detector output shape; "
                            "only NMS-free [1,N,6] supported (export the face "
                            "detector NMS-free)");
                ctx->warnedUnsupportedShape = true;
            }
            return boxes;
        }

        int num = 0;
        if (shape.size() == 3)      num = static_cast<int>(shape[1]);
        else if (shape.size() == 2) num = static_cast<int>(shape[0]);

        boxes = zm::detect::decode_nms_free(out, num, lb, ctx->confThreshold, {});
    } catch (const std::exception& e) {
        ZM_LOG_ERROR("recognize_face: detector inference error: %s", e.what());
    }
    return boxes;
}

// Load the gallery from config. Each entry is either {name, embedding:[...]} or
// {name, image_path:"..."}. Precomputed embeddings are the primary path; image
// galleries are best-effort and currently unsupported (logged and skipped).
void loadGallery(RecognizeFaceCtx* ctx, const json& arr) {
    if (!arr.is_array()) return;
    for (const auto& e : arr) {
        if (!e.is_object() || !e.contains("name")) continue;
        zm::face::GalleryEntry g;
        g.name = e.value("name", std::string());
        if (g.name.empty()) continue;

        if (e.contains("embedding") && e["embedding"].is_array()) {
            g.emb = e["embedding"].get<std::vector<float>>();
            if (g.emb.empty()) continue;
            zm::face::l2_normalize(g.emb);  // idempotent if already normalized
            ctx->gallery.push_back(std::move(g));
            continue;
        }

        if (e.contains("image_path")) {
            // Computing an embedding from an image requires decoding it (e.g.
            // stb_image) then running detector+embedder. Not wired up in this
            // build — precomputed embeddings are the reliable path.
            ZM_LOG_WARN("recognize_face: image gallery not supported in this build; "
                        "skipping '%s' (provide a precomputed 'embedding' instead)",
                        g.name.c_str());
            continue;
        }
    }
    ZM_LOG_INFO("recognize_face: loaded %zu gallery entr%s",
                ctx->gallery.size(), ctx->gallery.size() == 1 ? "y" : "ies");
}

} // namespace

extern "C" {

static int recognize_face_start(zm_plugin_t* plugin, zm_host_api_t* host, void* host_ctx,
                                const char* json_cfg) {
    auto* ctx = new RecognizeFaceCtx;
    ctx->host = host;
    ctx->hostCtx = host_ctx;
    zm_plugin_set_log_context(host, host_ctx);

    json galleryJson;
    if (json_cfg) {
        try {
            auto j = json::parse(json_cfg);
            ctx->detectorModelPath = j.value("detector_model_path", std::string());
            ctx->embedderModelPath = j.value("embedder_model_path", std::string());
            ctx->net = j.value("input_size", 640);
            ctx->embedSize = j.value("embed_size", 112);
            ctx->confThreshold = j.value("conf_threshold", 0.5f);
            ctx->matchThreshold = j.value("match_threshold", 0.5f);
            ctx->embedMean = j.value("embed_mean", 127.5f);
            ctx->embedScale = j.value("embed_scale", 128.0f);
            ctx->frameWidth = j.value("frame_width", 0);
            ctx->frameHeight = j.value("frame_height", 0);
            ctx->ep = j.value("ep", std::string("cpu"));
            if (j.contains("stream_filter") && j["stream_filter"].is_array())
                ctx->streamFilter = j["stream_filter"].get<std::vector<int>>();
            if (j.contains("gallery"))
                galleryJson = j["gallery"];
        } catch (const std::exception& e) {
            ZM_LOG_ERROR("recognize_face: failed to parse config: %s", e.what());
        }
    }

    if (ctx->embedScale == 0.0f) ctx->embedScale = 128.0f;

    // Create the ONNX Runtime environment and shared session options.
    try {
        ctx->env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "recognize_face");
        ctx->sessionOptions.SetIntraOpNumThreads(1);
        ctx->sessionOptions.SetGraphOptimizationLevel(ORT_ENABLE_ALL);
    } catch (const std::exception& e) {
        ZM_LOG_ERROR("recognize_face: failed to init ORT env: %s", e.what());
    }

    // Optionally append the CoreML execution provider, falling back to CPU.
    if (ctx->ep == "coreml") {
        try {
            uint32_t coreml_flags = 0;
            Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_CoreML(
                static_cast<OrtSessionOptions*>(ctx->sessionOptions), coreml_flags));
            ZM_LOG_INFO("recognize_face: CoreML execution provider enabled");
        } catch (const std::exception& e) {
            ZM_LOG_WARN("recognize_face: CoreML EP unavailable, falling back to CPU: %s",
                        e.what());
        }
    }

    ctx->detector = makeSession(ctx, ctx->detectorModelPath, "detector",
                                ctx->detInputName, ctx->detOutputName);
    ctx->embedder = makeSession(ctx, ctx->embedderModelPath, "embedder",
                                ctx->embInputName, ctx->embOutputName);

    if (!ctx->detector || !ctx->embedder) {
        ZM_LOG_WARN("recognize_face: detector and/or embedder not loaded; "
                    "running as pass-through");
    }

    loadGallery(ctx, galleryJson);

    plugin->instance = ctx;
    return 0;
}

static void recognize_face_stop(zm_plugin_t* plugin) {
    auto* ctx = static_cast<RecognizeFaceCtx*>(plugin->instance);
    if (ctx) {
        delete ctx;
        plugin->instance = nullptr;
    }
}

static void recognize_face_on_frame(zm_plugin_t* plugin, const void* buf, size_t size) {
    auto* ctx = static_cast<RecognizeFaceCtx*>(plugin->instance);
    if (!ctx || !buf || size < sizeof(zm_frame_hdr_t)) {
        forwardFrame(ctx, buf, size);
        return;
    }

    const zm_frame_hdr_t* hdr = static_cast<const zm_frame_hdr_t*>(buf);
    const uint8_t* payload = static_cast<const uint8_t*>(buf) + sizeof(zm_frame_hdr_t);

    // Pass-through unless both models are loaded and the frame is usable RGB24.
    if (!ctx->detector || !ctx->embedder || hdr->hw_type != ZM_FRAME_RGB24) {
        forwardFrame(ctx, buf, size);
        return;
    }
    if (!ctx->streamFilter.empty() &&
        std::find(ctx->streamFilter.begin(), ctx->streamFilter.end(),
                  static_cast<int>(hdr->stream_id)) == ctx->streamFilter.end()) {
        forwardFrame(ctx, buf, size);
        return;
    }

    const int w = ctx->frameWidth;
    const int h = ctx->frameHeight;
    if (w <= 0 || h <= 0 ||
        size < sizeof(zm_frame_hdr_t) + static_cast<size_t>(w) * h * 3) {
        forwardFrame(ctx, buf, size);
        return;
    }

    // Stage 1: detect faces.
    std::vector<zm::detect::Box> faces = runDetector(ctx, payload, w, h);

    if (!faces.empty()) {
        json facesJson = json::array();
        std::vector<uint8_t> crop;
        for (const auto& f : faces) {
            const int fx = static_cast<int>(std::lround(f.x));
            const int fy = static_cast<int>(std::lround(f.y));
            const int fw = static_cast<int>(std::lround(f.w));
            const int fh = static_cast<int>(std::lround(f.h));
            if (fw <= 0 || fh <= 0) continue;

            // Stage 2: crop -> embedder -> match.
            zm::face::crop_resize_rgb(payload, w, h, fx, fy, fw, fh,
                                      ctx->embedSize, ctx->embedSize, crop);
            std::vector<float> emb = runEmbedder(ctx, crop.data());

            zm::face::Match m{"unknown", 0.0f};
            if (!emb.empty())
                m = zm::face::best_match(ctx->gallery, emb, ctx->matchThreshold);

            json fj;
            fj["name"] = m.name;
            fj["similarity"] = m.score;
            fj["bbox"] = {fx, fy, fw, fh};
            facesJson.push_back(std::move(fj));
        }

        if (!facesJson.empty()) {
            json evt;
            evt["type"] = "face";
            evt["stream_id"] = hdr->stream_id;
            evt["pts_usec"] = hdr->pts_usec;
            evt["faces"] = std::move(facesJson);
            if (ctx->host && ctx->host->publish_evt)
                ctx->host->publish_evt(ctx->hostCtx, evt.dump().c_str());
        }
    }

    forwardFrame(ctx, buf, size);
}

#if defined(__GNUC__) || defined(__clang__)
#define ZM_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define ZM_PLUGIN_EXPORT
#endif

ZM_PLUGIN_EXPORT void zm_plugin_init(zm_plugin_t* plugin) {
    plugin->version = 1;
    plugin->type = ZM_PLUGIN_DETECT;
    plugin->start = recognize_face_start;
    plugin->stop = recognize_face_stop;
    plugin->on_frame = recognize_face_on_frame;
    plugin->instance = nullptr;
}

} // extern "C"

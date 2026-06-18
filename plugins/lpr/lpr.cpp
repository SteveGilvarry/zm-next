// lpr: Automatic License Plate Recognition (ALPR/LPR) plugin.
//
// Two-stage ONNX pipeline on decoded RGB24 frames:
//   1. A plate DETECTOR (YOLO26-style, NMS-free [1,N,6]) locates plate bounding
//      boxes in the frame.
//   2. An OCR model reads each cropped plate into text via greedy CTC decode.
//
// This is a pass-through ZM_PLUGIN_DETECT stage: it ALWAYS forwards the frame
// downstream. If either model path is empty/unloadable, or the frame is not
// RGB24, or the stream is filtered out, it forwards without running inference.
//
// IMPORTANT (detector export): the detector model MUST be exported NMS-free,
// producing an output tensor whose last dim is 6 = (x1,y1,x2,y2,conf,class_id),
// already de-duplicated. We reuse zm::detect::decode_nms_free for that.
//
// IMPORTANT (OCR export): the OCR model is expected to output a per-timestep
// class-logit sequence of shape [1, T, C] (or [T, C]) where C = len(charset)+1,
// the extra slot being the CTC blank. We greedy-CTC-decode it to a string.

#include "lpr_decode.hpp"
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

// Default character set: 36 alphanumerics (digits then uppercase letters).
// charset[k] maps OCR class index k to its character.
static const char* kDefaultCharset =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";

struct LprCtx {
    zm_host_api_t* host = nullptr;
    void* hostCtx = nullptr;

    // ONNX Runtime state shared between both sessions.
    std::unique_ptr<Ort::Env> env;
    Ort::SessionOptions sessionOptions;

    // Detector session.
    std::unique_ptr<Ort::Session> detector;
    std::string detInputName;
    std::string detOutputName;

    // OCR session.
    std::unique_ptr<Ort::Session> ocr;
    std::string ocrInputName;
    std::string ocrOutputName;

    // Config.
    std::string detectorPath;
    std::string ocrPath;
    int net = 640;             // detector input_size
    int ocrWidth = 168;
    int ocrHeight = 48;
    bool ocrGrayscale = false;
    float confThreshold = 0.4f;
    int ctcBlank = -1;         // -1 means "use C-1"
    std::string charset = kDefaultCharset;
    int frameWidth = 0;
    int frameHeight = 0;
    std::string ep = "cpu";
    std::vector<int> streamFilter;          // empty = all
    std::vector<std::string> watchlist;     // raw strings; normalized at compare time

    bool warnedUnsupportedDetShape = false;
    bool warnedUnsupportedOcrShape = false;
};

void forwardFrame(LprCtx* ctx, const void* buf, size_t size) {
    if (ctx && ctx->host && ctx->host->on_frame) {
        ctx->host->on_frame(ctx->hostCtx, buf, size);
    }
}

// Build an ORT session for `path`, capturing its first input/output names.
// Returns nullptr (and logs) on failure.
std::unique_ptr<Ort::Session> makeSession(LprCtx* ctx, const std::string& path,
                                          const char* tag,
                                          std::string& inName, std::string& outName) {
    if (path.empty() || !ctx->env) return nullptr;
    try {
        auto session = std::make_unique<Ort::Session>(
            *ctx->env, path.c_str(), ctx->sessionOptions);
        Ort::AllocatorWithDefaultOptions allocator;
        auto in = session->GetInputNameAllocated(0, allocator);
        auto out = session->GetOutputNameAllocated(0, allocator);
        inName = in.get();
        outName = out.get();
        ZM_LOG_INFO("lpr: loaded %s model '%s' (input='%s' output='%s')",
                    tag, path.c_str(), inName.c_str(), outName.c_str());
        return session;
    } catch (const std::exception& e) {
        ZM_LOG_ERROR("lpr: failed to load %s model '%s': %s (pass-through)",
                     tag, path.c_str(), e.what());
        return nullptr;
    }
}

} // namespace

extern "C" {

static int lpr_start(zm_plugin_t* plugin, zm_host_api_t* host, void* host_ctx,
                     const char* json_cfg) {
    auto* ctx = new LprCtx;
    ctx->host = host;
    ctx->hostCtx = host_ctx;
    zm_plugin_set_log_context(host, host_ctx);

    // Parse configuration (all keys optional).
    if (json_cfg) {
        try {
            auto j = json::parse(json_cfg);
            ctx->detectorPath = j.value("detector_model_path", std::string());
            ctx->ocrPath = j.value("ocr_model_path", std::string());
            ctx->net = j.value("input_size", 640);
            ctx->ocrWidth = j.value("ocr_width", 168);
            ctx->ocrHeight = j.value("ocr_height", 48);
            ctx->ocrGrayscale = j.value("ocr_grayscale", false);
            ctx->confThreshold = j.value("conf_threshold", 0.4f);
            ctx->ctcBlank = j.value("ctc_blank", -1);
            ctx->charset = j.value("charset", std::string(kDefaultCharset));
            ctx->frameWidth = j.value("frame_width", 0);
            ctx->frameHeight = j.value("frame_height", 0);
            ctx->ep = j.value("ep", std::string("cpu"));
            if (j.contains("stream_filter") && j["stream_filter"].is_array())
                ctx->streamFilter = j["stream_filter"].get<std::vector<int>>();
            if (j.contains("watchlist") && j["watchlist"].is_array())
                ctx->watchlist = j["watchlist"].get<std::vector<std::string>>();
        } catch (const std::exception& e) {
            ZM_LOG_ERROR("lpr: failed to parse config: %s", e.what());
        }
    }

    // Create the ONNX Runtime environment and session options.
    try {
        ctx->env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "lpr");
        ctx->sessionOptions.SetIntraOpNumThreads(1);
        ctx->sessionOptions.SetGraphOptimizationLevel(ORT_ENABLE_ALL);
    } catch (const std::exception& e) {
        ZM_LOG_ERROR("lpr: failed to init ORT env: %s", e.what());
    }

    // Optionally append the CoreML execution provider, falling back to CPU.
    if (ctx->ep == "coreml") {
        try {
            uint32_t coreml_flags = 0;
            Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_CoreML(
                static_cast<OrtSessionOptions*>(ctx->sessionOptions), coreml_flags));
            ZM_LOG_INFO("lpr: CoreML execution provider enabled");
        } catch (const std::exception& e) {
            ZM_LOG_WARN("lpr: CoreML EP unavailable, falling back to CPU: %s", e.what());
        }
    }

    // Construct both sessions; either missing -> pass-through.
    ctx->detector = makeSession(ctx, ctx->detectorPath, "detector",
                                ctx->detInputName, ctx->detOutputName);
    ctx->ocr = makeSession(ctx, ctx->ocrPath, "ocr",
                           ctx->ocrInputName, ctx->ocrOutputName);

    if (!ctx->detector || !ctx->ocr) {
        ZM_LOG_WARN("lpr: detector and/or OCR model not loaded; running as pass-through");
    }

    plugin->instance = ctx;
    return 0;
}

static void lpr_stop(zm_plugin_t* plugin) {
    auto* ctx = static_cast<LprCtx*>(plugin->instance);
    if (ctx) {
        delete ctx;
        plugin->instance = nullptr;
    }
}

// Run the OCR session on a single plate crop and return the decoded text.
// `confidenceOut` receives an approximate mean per-timestep confidence.
static std::string runOcr(LprCtx* ctx, const uint8_t* rgb, int w, int h,
                          const zm::detect::Box& box, float& confidenceOut) {
    confidenceOut = 0.0f;
    const int channels = ctx->ocrGrayscale ? 1 : 3;
    std::vector<float> input(static_cast<size_t>(channels) * ctx->ocrHeight * ctx->ocrWidth);
    if (ctx->ocrGrayscale) {
        zm::lpr::crop_resize_gray(rgb, w, h, box.x, box.y, box.w, box.h,
                                  ctx->ocrWidth, ctx->ocrHeight, input.data());
    } else {
        zm::lpr::crop_resize_rgb(rgb, w, h, box.x, box.y, box.w, box.h,
                                 ctx->ocrWidth, ctx->ocrHeight, input.data());
    }

    Ort::MemoryInfo memInfo =
        Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::array<int64_t, 4> inputShape{1, channels, ctx->ocrHeight, ctx->ocrWidth};
    Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
        memInfo, input.data(), input.size(), inputShape.data(), inputShape.size());

    const char* inputNames[] = {ctx->ocrInputName.c_str()};
    const char* outputNames[] = {ctx->ocrOutputName.c_str()};

    auto outputs = ctx->ocr->Run(Ort::RunOptions{nullptr}, inputNames,
                                 &inputTensor, 1, outputNames, 1);

    const float* out = outputs[0].GetTensorData<float>();
    auto shape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();

    // Accept [1, T, C] or [T, C].
    int T = 0, C = 0;
    if (shape.size() == 3) {
        T = static_cast<int>(shape[1]);
        C = static_cast<int>(shape[2]);
    } else if (shape.size() == 2) {
        T = static_cast<int>(shape[0]);
        C = static_cast<int>(shape[1]);
    } else {
        if (!ctx->warnedUnsupportedOcrShape) {
            ZM_LOG_WARN("lpr: unsupported OCR output shape; expected [1,T,C] or [T,C]");
            ctx->warnedUnsupportedOcrShape = true;
        }
        return std::string();
    }
    if (T <= 0 || C <= 0) return std::string();

    const int blank = (ctx->ctcBlank >= 0) ? ctx->ctcBlank : (C - 1);
    confidenceOut = zm::lpr::ctc_mean_confidence(out, T, C);
    return zm::lpr::ctc_greedy_decode(out, T, C, ctx->charset, blank);
}

static void lpr_on_frame(zm_plugin_t* plugin, const void* buf, size_t size) {
    auto* ctx = static_cast<LprCtx*>(plugin->instance);
    if (!ctx || !buf || size < sizeof(zm_frame_hdr_t)) {
        forwardFrame(ctx, buf, size);
        return;
    }

    const zm_frame_hdr_t* hdr = static_cast<const zm_frame_hdr_t*>(buf);
    const uint8_t* payload = static_cast<const uint8_t*>(buf) + sizeof(zm_frame_hdr_t);

    // Pass-through when we cannot or should not run inference.
    if (!ctx->detector || !ctx->ocr || hdr->hw_type != ZM_FRAME_RGB24) {
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

    try {
        // Stage 1: plate detection.
        const int net = ctx->net;
        zm::detect::Letterbox lb = zm::detect::compute_letterbox(w, h, net);

        std::vector<float> detInput(static_cast<size_t>(3) * net * net);
        zm::detect::letterbox_rgb_to_chw(payload, lb, detInput.data());

        Ort::MemoryInfo memInfo =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::array<int64_t, 4> detShape{1, 3, net, net};
        Ort::Value detTensor = Ort::Value::CreateTensor<float>(
            memInfo, detInput.data(), detInput.size(), detShape.data(), detShape.size());

        const char* detIn[] = {ctx->detInputName.c_str()};
        const char* detOut[] = {ctx->detOutputName.c_str()};
        auto detOutputs = ctx->detector->Run(Ort::RunOptions{nullptr}, detIn,
                                             &detTensor, 1, detOut, 1);

        const float* dout = detOutputs[0].GetTensorData<float>();
        auto dshape = detOutputs[0].GetTensorTypeAndShapeInfo().GetShape();
        const int64_t lastDim = dshape.empty() ? 0 : dshape.back();
        if (lastDim != 6) {
            if (!ctx->warnedUnsupportedDetShape) {
                ZM_LOG_WARN("lpr: unsupported detector output shape; only NMS-free [1,N,6] supported");
                ctx->warnedUnsupportedDetShape = true;
            }
            forwardFrame(ctx, buf, size);
            return;
        }
        int num = 0;
        if (dshape.size() == 3)      num = static_cast<int>(dshape[1]);
        else if (dshape.size() == 2) num = static_cast<int>(dshape[0]);

        std::vector<zm::detect::Box> plateBoxes =
            zm::detect::decode_nms_free(dout, num, lb, ctx->confThreshold, {});

        // Stage 2: OCR each plate box.
        json plates = json::array();
        for (const auto& box : plateBoxes) {
            float ocrConf = 0.0f;
            std::string text = runOcr(ctx, payload, w, h, box, ocrConf);
            if (text.empty()) continue;
            const std::string norm = zm::lpr::normalize_plate(text);
            json p;
            p["text"] = norm;
            p["confidence"] = ocrConf;
            p["bbox"] = {box.x, box.y, box.w, box.h};
            p["watchlisted"] = zm::lpr::watchlisted(ctx->watchlist, norm);
            plates.push_back(std::move(p));
        }

        if (!plates.empty()) {
            json evt;
            evt["type"] = "lpr";
            evt["stream_id"] = hdr->stream_id;
            evt["pts_usec"] = hdr->pts_usec;
            evt["plates"] = std::move(plates);
            if (ctx->host && ctx->host->publish_evt)
                ctx->host->publish_evt(ctx->hostCtx, evt.dump().c_str());
        }
    } catch (const std::exception& e) {
        ZM_LOG_ERROR("lpr: inference error: %s", e.what());
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
    plugin->start = lpr_start;
    plugin->stop = lpr_stop;
    plugin->on_frame = lpr_on_frame;
    plugin->instance = nullptr;
}

} // extern "C"

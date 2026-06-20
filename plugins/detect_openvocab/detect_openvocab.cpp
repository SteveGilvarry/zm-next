// detect_openvocab: open-vocabulary object-detection plugin (ONNX Runtime C++).
//
// This is detect_onnx specialized for *prompt-defined* classes. Open-vocabulary
// detectors (YOLOE / YOLO-World) let you choose the target classes by TEXT
// PROMPT instead of being fixed to the COCO-80 label set.
//
// ---------------------------------------------------------------------------
// DEPLOYABLE WORKFLOW (how to produce a model for this plugin)
// ---------------------------------------------------------------------------
// The standard, deployable workflow bakes the chosen prompts into the exported
// ONNX so it behaves like an ordinary fixed-class detector at runtime:
//
//   from ultralytics import YOLO
//   model = YOLO("yoloe-v8l-seg.pt")            # or a YOLO-World checkpoint
//   model.set_classes(["delivery van", "ladder", "person with package"])
//   model.export(format="onnx", opset=12)        # prompts are now baked in
//
// After export the model's class indices map 1:1 onto the prompt list passed to
// set_classes(), IN THE SAME ORDER. So at runtime this plugin only needs to map
// class_id -> prompts[class_id]. Configure "prompts" with EXACTLY the same list,
// in the same order, that was given to set_classes() at export time.
//
// FUTURE ENHANCEMENT: fully-dynamic *runtime* prompts (changing target classes
// without re-exporting) require a separate CLIP/text-encoder ONNX that turns
// prompt strings into class embeddings, which are then fed as a second input to
// the YOLOE/YOLO-World visual model. That two-model pipeline is intentionally
// out of scope here; this plugin consumes a single, prompt-baked detector ONNX.
// ---------------------------------------------------------------------------
//
// Behaviour mirrors detect_onnx: it runs the model on decoded RGB24 frames,
// publishes detections as structured "detection" events (tagged
// "source":"openvocab"), and is ALWAYS a pass-through DETECT stage — the frame
// is forwarded downstream regardless of whether inference ran.

#include "../detect_onnx/detect_postprocess.hpp"  // shared pure pre/post-process

#include <onnxruntime_cxx_api.h>
#ifdef __APPLE__
#include <coreml_provider_factory.h>
#endif

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

struct DetectOpenVocabCtx {
    zm_host_api_t* host = nullptr;
    void* hostCtx = nullptr;

    // ONNX Runtime state.
    std::unique_ptr<Ort::Env> env;
    Ort::SessionOptions sessionOptions;
    std::unique_ptr<Ort::Session> session;
    std::string inputName;
    std::string outputName;

    // Config.
    std::string modelPath;
    int net = 640;            // input_size
    float confThreshold = 0.25f;
    int frameWidth = 0;
    int frameHeight = 0;
    std::string ep = "cpu";
    std::vector<int> streamFilter;          // empty = all
    std::vector<std::string> prompts;       // class_id -> prompt string

    bool warnedUnsupportedShape = false;
};

// Map a class id to its configured prompt; fall back to "class_<id>" when the id
// is out of range of the prompt list (or no prompts were configured).
const char* labelFor(const DetectOpenVocabCtx* ctx, int id, std::string& scratch) {
    if (id >= 0 && id < static_cast<int>(ctx->prompts.size()))
        return ctx->prompts[id].c_str();
    scratch = "class_" + std::to_string(id);
    return scratch.c_str();
}

void forwardFrame(DetectOpenVocabCtx* ctx, const void* buf, size_t size) {
    if (ctx && ctx->host && ctx->host->on_frame) {
        ctx->host->on_frame(ctx->hostCtx, buf, size);
    }
}

} // namespace

extern "C" {

static int detect_openvocab_start(zm_plugin_t* plugin, zm_host_api_t* host, void* host_ctx,
                                  const char* json_cfg) {
    auto* ctx = new DetectOpenVocabCtx;
    ctx->host = host;
    ctx->hostCtx = host_ctx;
    zm_plugin_set_log_context(host, host_ctx);

    // Parse configuration (all keys optional).
    if (json_cfg) {
        try {
            auto j = json::parse(json_cfg);
            ctx->modelPath = j.value("model_path", std::string());
            ctx->net = j.value("input_size", 640);
            ctx->confThreshold = j.value("conf_threshold", 0.25f);
            ctx->frameWidth = j.value("frame_width", 0);
            ctx->frameHeight = j.value("frame_height", 0);
            ctx->ep = j.value("ep", std::string("cpu"));
            if (j.contains("stream_filter") && j["stream_filter"].is_array())
                ctx->streamFilter = j["stream_filter"].get<std::vector<int>>();
            if (j.contains("prompts") && j["prompts"].is_array())
                ctx->prompts = j["prompts"].get<std::vector<std::string>>();
        } catch (const std::exception& e) {
            ZM_LOG_ERROR("detect_openvocab: failed to parse config: %s", e.what());
        }
    }

    // Create the ONNX Runtime environment and session options.
    try {
        ctx->env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "detect_openvocab");
        ctx->sessionOptions.SetIntraOpNumThreads(1);
        ctx->sessionOptions.SetGraphOptimizationLevel(ORT_ENABLE_ALL);
    } catch (const std::exception& e) {
        ZM_LOG_ERROR("detect_openvocab: failed to init ORT env: %s", e.what());
    }

    // Optionally append the CoreML execution provider, falling back to CPU.
    if (ctx->ep == "coreml") {
#ifdef __APPLE__
        try {
            uint32_t coreml_flags = 0;
            Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_CoreML(
                static_cast<OrtSessionOptions*>(ctx->sessionOptions), coreml_flags));
            ZM_LOG_INFO("detect_openvocab: CoreML execution provider enabled");
        } catch (const std::exception& e) {
            ZM_LOG_WARN("detect_openvocab: CoreML EP unavailable, falling back to CPU: %s",
                        e.what());
        }
#else
        ZM_LOG_WARN("detect_openvocab: CoreML EP not available on this platform, falling back to CPU");
#endif
    }

    // Construct the session if a model path was given.
    if (!ctx->modelPath.empty() && ctx->env) {
        try {
            ctx->session = std::make_unique<Ort::Session>(
                *ctx->env, ctx->modelPath.c_str(), ctx->sessionOptions);

            Ort::AllocatorWithDefaultOptions allocator;
            auto inName = ctx->session->GetInputNameAllocated(0, allocator);
            auto outName = ctx->session->GetOutputNameAllocated(0, allocator);
            ctx->inputName = inName.get();
            ctx->outputName = outName.get();

            ZM_LOG_INFO("detect_openvocab: loaded model '%s' (input='%s' output='%s' "
                        "net=%d ep=%s prompts=%zu)",
                        ctx->modelPath.c_str(), ctx->inputName.c_str(),
                        ctx->outputName.c_str(), ctx->net, ctx->ep.c_str(),
                        ctx->prompts.size());
            if (ctx->prompts.empty())
                ZM_LOG_WARN("detect_openvocab: no prompts configured; detections will be "
                            "labelled 'class_<id>'");
        } catch (const std::exception& e) {
            ZM_LOG_ERROR("detect_openvocab: failed to load model '%s': %s (running as pass-through)",
                         ctx->modelPath.c_str(), e.what());
            ctx->session.reset();
        }
    } else {
        ZM_LOG_WARN("detect_openvocab: no model_path configured; running as pass-through");
    }

    plugin->instance = ctx;
    return 0;
}

static void detect_openvocab_stop(zm_plugin_t* plugin) {
    auto* ctx = static_cast<DetectOpenVocabCtx*>(plugin->instance);
    if (ctx) {
        delete ctx;
        plugin->instance = nullptr;
    }
}

// Build and publish a "detection" event from decoded boxes (source-pixel coords).
static void publishBoxes(DetectOpenVocabCtx* ctx, const zm_frame_hdr_t* hdr,
                         const std::vector<zm::detect::Box>& boxes) {
    if (boxes.empty()) return;
    json detections = json::array();
    for (const auto& b : boxes) {
        std::string scratch;
        json d;
        d["label"] = labelFor(ctx, b.class_id, scratch);
        d["confidence"] = b.confidence;
        d["bbox"] = {b.x, b.y, b.w, b.h};
        d["class_id"] = b.class_id;
        detections.push_back(std::move(d));
    }
    json evt;
    evt["type"] = "detection";
    evt["source"] = "openvocab";
    evt["stream_id"] = hdr->stream_id;
    evt["pts_usec"] = hdr->pts_usec;
    evt["detections"] = std::move(detections);
    if (ctx->host && ctx->host->publish_evt)
        ctx->host->publish_evt(ctx->hostCtx, evt.dump().c_str());
}

static void detect_openvocab_on_frame(zm_plugin_t* plugin, const void* buf, size_t size) {
    auto* ctx = static_cast<DetectOpenVocabCtx*>(plugin->instance);
    if (!ctx || !buf || size < sizeof(zm_frame_hdr_t)) {
        forwardFrame(ctx, buf, size);
        return;
    }

    const zm_frame_hdr_t* hdr = static_cast<const zm_frame_hdr_t*>(buf);
    const uint8_t* payload = static_cast<const uint8_t*>(buf) + sizeof(zm_frame_hdr_t);

    // Bail out (pass-through) when we cannot or should not run inference.
    if (!ctx->session || hdr->hw_type != ZM_FRAME_RGB24) {
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
        const int net = ctx->net;
        zm::detect::Letterbox lb = zm::detect::compute_letterbox(w, h, net);

        std::vector<float> input(static_cast<size_t>(3) * net * net);
        zm::detect::letterbox_rgb_to_chw(payload, lb, input.data());

        Ort::MemoryInfo memInfo =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::array<int64_t, 4> inputShape{1, 3, net, net};
        Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
            memInfo, input.data(), input.size(), inputShape.data(), inputShape.size());

        const char* inputNames[] = {ctx->inputName.c_str()};
        const char* outputNames[] = {ctx->outputName.c_str()};

        auto outputs = ctx->session->Run(Ort::RunOptions{nullptr}, inputNames,
                                         &inputTensor, 1, outputNames, 1);

        const float* out = outputs[0].GetTensorData<float>();
        auto shape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();

        // Only the YOLO26 NMS-free output [1, N, 6] is supported.
        const int64_t lastDim = shape.empty() ? 0 : shape.back();
        if (lastDim != 6) {
            if (!ctx->warnedUnsupportedShape) {
                ZM_LOG_WARN("detect_openvocab: unsupported output shape; only NMS-free "
                            "[1,N,6] supported");
                ctx->warnedUnsupportedShape = true;
            }
            forwardFrame(ctx, buf, size);
            return;
        }

        int num = 0;
        if (shape.size() == 3)      num = static_cast<int>(shape[1]);
        else if (shape.size() == 2) num = static_cast<int>(shape[0]);

        std::vector<zm::detect::Box> boxes =
            zm::detect::decode_nms_free(out, num, lb, ctx->confThreshold);

        publishBoxes(ctx, hdr, boxes);
    } catch (const std::exception& e) {
        ZM_LOG_ERROR("detect_openvocab: inference error: %s", e.what());
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
    plugin->start = detect_openvocab_start;
    plugin->stop = detect_openvocab_stop;
    plugin->on_frame = detect_openvocab_on_frame;
    plugin->instance = nullptr;
}

} // extern "C"

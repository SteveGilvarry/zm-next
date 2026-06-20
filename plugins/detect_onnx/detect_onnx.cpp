// detect_onnx: YOLO ONNX object-detection plugin (ONNX Runtime C++ API).
//
// Runs a YOLO ONNX model on decoded RGB24 frames and publishes detections as
// structured "detection" events. Always forwards the frame downstream (the
// plugin is a pass-through DETECT stage). If no model is loaded, or the frame
// is not RGB24, or the stream is filtered out, it simply forwards.

#include "detect_postprocess.hpp"
#include "detect_cuda.hpp"   // CUDA zero-copy path (only active when ZM_WITH_CUDA)

#include <onnxruntime_cxx_api.h>
#ifdef __APPLE__
#include <coreml_provider_factory.h>
#endif
#ifdef ZMP_WITH_CUDA
#include <cuda_runtime.h>
#include "detect_engine.hpp"
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

// Standard COCO-80 class names, used when "class_names" is absent from config.
static const std::array<const char*, 80> kCocoNames = {
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train",
    "truck", "boat", "traffic light", "fire hydrant", "stop sign",
    "parking meter", "bench", "bird", "cat", "dog", "horse", "sheep", "cow",
    "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella", "handbag",
    "tie", "suitcase", "frisbee", "skis", "snowboard", "sports ball", "kite",
    "baseball bat", "baseball glove", "skateboard", "surfboard",
    "tennis racket", "bottle", "wine glass", "cup", "fork", "knife", "spoon",
    "bowl", "banana", "apple", "sandwich", "orange", "broccoli", "carrot",
    "hot dog", "pizza", "donut", "cake", "chair", "couch", "potted plant",
    "bed", "dining table", "toilet", "tv", "laptop", "mouse", "remote",
    "keyboard", "cell phone", "microwave", "oven", "toaster", "sink",
    "refrigerator", "book", "clock", "vase", "scissors", "teddy bear",
    "hair drier", "toothbrush"
};

struct DetectOnnxCtx {
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
    std::vector<int> classFilter;       // empty = all
    std::vector<int> streamFilter;      // empty = all
    std::vector<std::string> classNames; // empty = use COCO-80

    // ROI motion cascade (CUDA/GPU surface path only).
    bool roiMotion = false;            // gate inference on motion + detect per-region crops
    int motionDownscale = 8;
    int motionThreshold = 25;
    int motionMinCells = 0;            // per-region min changed cells (0 = auto ~0.25%)
    int maxRegions = 8;
    double fullSweepSec = 2.0;         // periodic full-frame sweep to catch static objects
    std::vector<uint8_t> prevGrid;     // previous downsampled luma grid (motion state)
    uint64_t lastSweepUsec = 0;
    bool sweptOnce = false;
    bool sharedEngine = false;     // route full-frame CUDA detect through the shared engine
    bool engineReady = false;

    bool warnedUnsupportedShape = false;
    bool warnedNoCuda = false;
};

const char* className(const DetectOnnxCtx* ctx, int id, std::string& scratch) {
    if (!ctx->classNames.empty()) {
        if (id >= 0 && id < static_cast<int>(ctx->classNames.size()))
            return ctx->classNames[id].c_str();
    } else if (id >= 0 && id < static_cast<int>(kCocoNames.size())) {
        return kCocoNames[id];
    }
    scratch = std::to_string(id);
    return scratch.c_str();
}

void forwardFrame(DetectOnnxCtx* ctx, const void* buf, size_t size) {
    if (ctx && ctx->host && ctx->host->on_frame) {
        ctx->host->on_frame(ctx->hostCtx, buf, size);
    }
}

} // namespace

extern "C" {

static int detect_onnx_start(zm_plugin_t* plugin, zm_host_api_t* host, void* host_ctx,
                             const char* json_cfg) {
    auto* ctx = new DetectOnnxCtx;
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
            if (j.contains("class_filter") && j["class_filter"].is_array())
                ctx->classFilter = j["class_filter"].get<std::vector<int>>();
            if (j.contains("stream_filter") && j["stream_filter"].is_array())
                ctx->streamFilter = j["stream_filter"].get<std::vector<int>>();
            if (j.contains("class_names") && j["class_names"].is_array())
                ctx->classNames = j["class_names"].get<std::vector<std::string>>();
            ctx->roiMotion = j.value("roi_motion", false);
            ctx->motionDownscale = j.value("motion_downscale", 8);
            ctx->motionThreshold = j.value("motion_threshold", 25);
            ctx->motionMinCells = j.value("motion_min_changed", 0);
            ctx->maxRegions = j.value("max_regions", 8);
            ctx->fullSweepSec = j.value("full_sweep_sec", 2.0);
            ctx->sharedEngine = j.value("shared_engine", false);
        } catch (const std::exception& e) {
            ZM_LOG_ERROR("detect_onnx: failed to parse config: %s", e.what());
        }
    }

    // Create the ONNX Runtime environment and session options.
    try {
        ctx->env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "detect_onnx");
        ctx->sessionOptions.SetIntraOpNumThreads(1);
        ctx->sessionOptions.SetGraphOptimizationLevel(ORT_ENABLE_ALL);
    } catch (const std::exception& e) {
        ZM_LOG_ERROR("detect_onnx: failed to init ORT env: %s", e.what());
    }

    // Optionally append the CoreML execution provider, falling back to CPU.
    if (ctx->ep == "coreml") {
#ifdef __APPLE__
        try {
            uint32_t coreml_flags = 0;
            Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_CoreML(
                static_cast<OrtSessionOptions*>(ctx->sessionOptions), coreml_flags));
            ZM_LOG_INFO("detect_onnx: CoreML execution provider enabled");
        } catch (const std::exception& e) {
            ZM_LOG_WARN("detect_onnx: CoreML EP unavailable, falling back to CPU: %s",
                        e.what());
        }
#else
        ZM_LOG_WARN("detect_onnx: CoreML EP not available on this platform, falling back to CPU");
#endif
    }
#ifdef ZMP_WITH_CUDA
    // CUDA execution provider — required for the zero-copy GPU detect path.
    if (ctx->ep == "cuda") {
        try {
            // Make GPU waits SLEEP the CPU rather than spin-wait (set before the
            // CUDA context is created). Cuts host CPU ~40% at no throughput cost;
            // harmless if a context already exists (another session set it).
            cudaSetDeviceFlags(cudaDeviceScheduleBlockingSync);
            OrtCUDAProviderOptions cuda_opts{};
            ctx->sessionOptions.AppendExecutionProvider_CUDA(cuda_opts);
            ZM_LOG_INFO("detect_onnx: CUDA execution provider enabled (blocking sync)");
        } catch (const std::exception& e) {
            ZM_LOG_WARN("detect_onnx: CUDA EP unavailable, falling back to CPU: %s", e.what());
        }
    }
#endif

#ifdef ZMP_WITH_CUDA
    // Opt-in: route full-frame CUDA detection through the process-wide shared,
    // batched engine (one ORT session + CUDA context for all detect instances).
    if (ctx->sharedEngine && ctx->ep == "cuda" && !ctx->modelPath.empty()) {
        try {
            zm::detect::InferenceEngine::get(ctx->modelPath, ctx->net);   // load shared session now
            ctx->engineReady = true;
            ctx->roiMotion = false;   // engine path is whole-frame only
            ZM_LOG_INFO("detect_onnx: using SHARED batched inference engine (model '%s')",
                        ctx->modelPath.c_str());
        } catch (const std::exception& e) {
            ZM_LOG_ERROR("detect_onnx: shared engine init failed (%s); using per-instance session", e.what());
        }
    }
#endif

    // Construct a per-instance session unless the shared engine owns inference.
    if (!ctx->engineReady && !ctx->modelPath.empty() && ctx->env) {
        try {
            ctx->session = std::make_unique<Ort::Session>(
                *ctx->env, ctx->modelPath.c_str(), ctx->sessionOptions);

            Ort::AllocatorWithDefaultOptions allocator;
            auto inName = ctx->session->GetInputNameAllocated(0, allocator);
            auto outName = ctx->session->GetOutputNameAllocated(0, allocator);
            ctx->inputName = inName.get();
            ctx->outputName = outName.get();

            ZM_LOG_INFO("detect_onnx: loaded model '%s' (input='%s' output='%s' net=%d ep=%s)",
                        ctx->modelPath.c_str(), ctx->inputName.c_str(),
                        ctx->outputName.c_str(), ctx->net, ctx->ep.c_str());
        } catch (const std::exception& e) {
            ZM_LOG_ERROR("detect_onnx: failed to load model '%s': %s (running as pass-through)",
                         ctx->modelPath.c_str(), e.what());
            ctx->session.reset();
        }
    } else if (!ctx->engineReady) {
        ZM_LOG_WARN("detect_onnx: no model_path configured; running as pass-through");
    }

    plugin->instance = ctx;
    return 0;
}

static void detect_onnx_stop(zm_plugin_t* plugin) {
    auto* ctx = static_cast<DetectOnnxCtx*>(plugin->instance);
    if (ctx) {
        delete ctx;
        plugin->instance = nullptr;
    }
}

// Build and publish a "detection" event from decoded boxes (source-pixel coords).
static void publishBoxes(DetectOnnxCtx* ctx, const zm_frame_hdr_t* hdr,
                         const std::vector<zm::detect::Box>& boxes) {
    if (boxes.empty()) return;
    json detections = json::array();
    for (const auto& b : boxes) {
        std::string scratch;
        json d;
        d["label"] = className(ctx, b.class_id, scratch);
        d["confidence"] = b.confidence;
        d["bbox"] = {b.x, b.y, b.w, b.h};
        d["class_id"] = b.class_id;
        detections.push_back(std::move(d));
    }
    json evt;
    evt["type"] = "detection";
    evt["stream_id"] = hdr->stream_id;
    evt["pts_usec"] = hdr->pts_usec;
    evt["detections"] = std::move(detections);
    if (ctx->host && ctx->host->publish_evt)
        ctx->host->publish_evt(ctx->hostCtx, evt.dump().c_str());
}

static void detect_onnx_on_frame(zm_plugin_t* plugin, const void* buf, size_t size) {
    auto* ctx = static_cast<DetectOnnxCtx*>(plugin->instance);
    if (!ctx || !buf || size < sizeof(zm_frame_hdr_t)) {
        forwardFrame(ctx, buf, size);
        return;
    }

    const zm_frame_hdr_t* hdr = static_cast<const zm_frame_hdr_t*>(buf);
    const uint8_t* payload = static_cast<const uint8_t*>(buf) + sizeof(zm_frame_hdr_t);

    // Zero-copy GPU path: a CUDA NV12 surface from NVDEC hardware decode.
    if ((ctx->session || ctx->engineReady) && hdr->hw_type == ZM_HW_CUDA &&
        size >= sizeof(zm_frame_hdr_t) + sizeof(zm_gpu_frame_t)) {
#ifdef ZMP_WITH_CUDA
        const auto* g = reinterpret_cast<const zm_gpu_frame_t*>(payload);
        const int gw = static_cast<int>(g->width), gh = static_cast<int>(g->height);
        const int yp = static_cast<int>(g->linesize[0]), uvp = static_cast<int>(g->linesize[1]);
        try {
            if (ctx->roiMotion) {
                // Motion-gated, per-region ROI cascade with a periodic full sweep.
                const int minCells = ctx->motionMinCells > 0 ? ctx->motionMinCells
                    : std::max(8, (gw / ctx->motionDownscale) * (gh / ctx->motionDownscale) / 400);
                auto regions = zm::detect::cuda_motion_regions(
                    g->plane_ptr[0], yp, gw, gh, ctx->prevGrid,
                    ctx->motionDownscale, ctx->motionThreshold, minCells, ctx->maxRegions);
                const bool sweep = !ctx->sweptOnce ||
                    hdr->pts_usec >= ctx->lastSweepUsec + static_cast<uint64_t>(ctx->fullSweepSec * 1e6);
                std::vector<zm::detect::Box> boxes;
                if (!regions.empty()) {  // detect each mover's crop in one batched pass
                    auto rb = zm::detect::cuda_infer_nv12_batch(
                        *ctx->session, ctx->inputName, ctx->outputName,
                        g->plane_ptr[0], yp, g->plane_ptr[1], uvp, gw, gh,
                        regions, ctx->net, ctx->confThreshold, ctx->classFilter);
                    boxes.insert(boxes.end(), rb.begin(), rb.end());
                }
                if (sweep) {  // occasional whole-frame pass for static objects
                    ctx->lastSweepUsec = hdr->pts_usec;
                    ctx->sweptOnce = true;
                    auto fb = zm::detect::cuda_infer_nv12(
                        *ctx->session, ctx->inputName, ctx->outputName,
                        g->plane_ptr[0], yp, g->plane_ptr[1], uvp, gw, gh,
                        ctx->net, ctx->confThreshold, ctx->classFilter);
                    boxes.insert(boxes.end(), fb.begin(), fb.end());
                }
                if (!boxes.empty()) boxes = zm::detect::merge_overlapping(boxes, 0.5f);
                publishBoxes(ctx, hdr, boxes);
            } else if (ctx->engineReady) {
                // Shared batched engine: preprocess the surface zero-copy, then
                // submit the tensor (coalesced with other streams into one Run).
                zm::detect::Letterbox lb;
                const float* d = zm::detect::cuda_preprocess_nv12(
                    g->plane_ptr[0], yp, g->plane_ptr[1], uvp, gw, gh, ctx->net, lb);
                std::vector<zm::detect::Box> boxes;
                if (d) boxes = zm::detect::InferenceEngine::get(ctx->modelPath, ctx->net)
                                   .infer(d, lb, ctx->confThreshold, ctx->classFilter);
                publishBoxes(ctx, hdr, boxes);
            } else {
                auto boxes = zm::detect::cuda_infer_nv12(
                    *ctx->session, ctx->inputName, ctx->outputName,
                    g->plane_ptr[0], yp, g->plane_ptr[1], uvp, gw, gh, ctx->net,
                    ctx->confThreshold, ctx->classFilter);
                publishBoxes(ctx, hdr, boxes);
            }
        } catch (const std::exception& e) {
            ZM_LOG_ERROR("detect_onnx: CUDA inference error: %s", e.what());
        }
#else
        if (!ctx->warnedNoCuda) {
            ZM_LOG_WARN("detect_onnx: received a CUDA surface but built without CUDA; "
                        "rebuild with -DZM_WITH_CUDA=ON for GPU detect");
            ctx->warnedNoCuda = true;
        }
#endif
        forwardFrame(ctx, buf, size);
        return;
    }

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
                ZM_LOG_WARN("detect_onnx: unsupported output shape; only NMS-free [1,N,6] supported");
                ctx->warnedUnsupportedShape = true;
            }
            forwardFrame(ctx, buf, size);
            return;
        }

        int num = 0;
        if (shape.size() == 3)      num = static_cast<int>(shape[1]);
        else if (shape.size() == 2) num = static_cast<int>(shape[0]);

        std::vector<zm::detect::Box> boxes =
            zm::detect::decode_nms_free(out, num, lb, ctx->confThreshold, ctx->classFilter);

        publishBoxes(ctx, hdr, boxes);
    } catch (const std::exception& e) {
        ZM_LOG_ERROR("detect_onnx: inference error: %s", e.what());
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
    plugin->start = detect_onnx_start;
    plugin->stop = detect_onnx_stop;
    plugin->on_frame = detect_onnx_on_frame;
    plugin->instance = nullptr;
}

} // extern "C"

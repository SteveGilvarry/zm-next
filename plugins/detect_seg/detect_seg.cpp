// detect_seg: YOLO-seg instance-segmentation plugin (ONNX Runtime C++ API).
//
// Runs a YOLO-seg ONNX model (e.g. yolo11n-seg / yolov8-seg) on decoded RGB24
// frames and publishes per-object class + box + segmentation mask (as a coarse
// polygon) in a structured "segmentation" event. Always forwards the frame
// downstream (the plugin is a pass-through DETECT stage). If no model is
// loaded, or the frame is not RGB24, or the stream is filtered out, it simply
// forwards.
//
// YOLO-seg has TWO outputs:
//   detection (rank 3): [1, 4+nc+nm, 8400]  (or transposed [1, 8400, 4+nc+nm])
//   proto     (rank 4): [1, nm, mh, mw]      prototype masks
// For each kept detection: class = argmax of nc class scores, conf = that max,
// box from (cx,cy,w,h); mask = sigmoid(sum_k coeff_k * proto_k), thresholded.

#include "seg_postprocess.hpp"

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

struct DetectSegCtx {
    zm_host_api_t* host = nullptr;
    void* hostCtx = nullptr;

    // ONNX Runtime state.
    std::unique_ptr<Ort::Env> env;
    Ort::SessionOptions sessionOptions;
    std::unique_ptr<Ort::Session> session;
    std::string inputName;
    std::string detName;    // output0 (rank 3) detection
    std::string protoName;  // output1 (rank 4) proto masks

    // Config.
    std::string modelPath;
    int net = 640;               // input_size
    float confThreshold = 0.25f;
    float iouThreshold = 0.45f;
    int numClasses = 0;          // 0 => infer from detection channels
    int maskDim = 32;
    int frameWidth = 0;
    int frameHeight = 0;
    std::string ep = "cpu";
    std::string maskFormat = "polygon";  // "polygon" | "none"
    std::vector<int> classFilter;        // empty = all
    std::vector<int> streamFilter;       // empty = all
    std::vector<std::string> classNames; // empty = use COCO-80

    bool warnedShape = false;
};

const char* className(const DetectSegCtx* ctx, int id, std::string& scratch) {
    if (!ctx->classNames.empty()) {
        if (id >= 0 && id < static_cast<int>(ctx->classNames.size()))
            return ctx->classNames[id].c_str();
    } else if (id >= 0 && id < static_cast<int>(kCocoNames.size())) {
        return kCocoNames[id];
    }
    scratch = std::to_string(id);
    return scratch.c_str();
}

void forwardFrame(DetectSegCtx* ctx, const void* buf, size_t size) {
    if (ctx && ctx->host && ctx->host->on_frame) {
        ctx->host->on_frame(ctx->hostCtx, buf, size);
    }
}

} // namespace

extern "C" {

static int detect_seg_start(zm_plugin_t* plugin, zm_host_api_t* host, void* host_ctx,
                            const char* json_cfg) {
    auto* ctx = new DetectSegCtx;
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
            ctx->iouThreshold = j.value("iou_threshold", 0.45f);
            ctx->numClasses = j.value("num_classes", 0);
            ctx->maskDim = j.value("mask_dim", 32);
            ctx->frameWidth = j.value("frame_width", 0);
            ctx->frameHeight = j.value("frame_height", 0);
            ctx->ep = j.value("ep", std::string("cpu"));
            ctx->maskFormat = j.value("mask_format", std::string("polygon"));
            if (j.contains("class_filter") && j["class_filter"].is_array())
                ctx->classFilter = j["class_filter"].get<std::vector<int>>();
            if (j.contains("stream_filter") && j["stream_filter"].is_array())
                ctx->streamFilter = j["stream_filter"].get<std::vector<int>>();
            if (j.contains("class_names") && j["class_names"].is_array())
                ctx->classNames = j["class_names"].get<std::vector<std::string>>();
        } catch (const std::exception& e) {
            ZM_LOG_ERROR("detect_seg: failed to parse config: %s", e.what());
        }
    }

    // Create the ONNX Runtime environment and session options.
    try {
        ctx->env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "detect_seg");
        ctx->sessionOptions.SetIntraOpNumThreads(1);
        ctx->sessionOptions.SetGraphOptimizationLevel(ORT_ENABLE_ALL);
    } catch (const std::exception& e) {
        ZM_LOG_ERROR("detect_seg: failed to init ORT env: %s", e.what());
    }

    // Optionally append the CoreML execution provider, falling back to CPU.
    if (ctx->ep == "coreml") {
#ifdef __APPLE__
        try {
            uint32_t coreml_flags = 0;
            Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_CoreML(
                static_cast<OrtSessionOptions*>(ctx->sessionOptions), coreml_flags));
            ZM_LOG_INFO("detect_seg: CoreML execution provider enabled");
        } catch (const std::exception& e) {
            ZM_LOG_WARN("detect_seg: CoreML EP unavailable, falling back to CPU: %s",
                        e.what());
        }
#else
        ZM_LOG_WARN("detect_seg: CoreML EP not available on this platform, falling back to CPU");
#endif
    }

    // Construct the session if a model path was given.
    if (!ctx->modelPath.empty() && ctx->env) {
        try {
            ctx->session = std::make_unique<Ort::Session>(
                *ctx->env, ctx->modelPath.c_str(), ctx->sessionOptions);

            Ort::AllocatorWithDefaultOptions allocator;
            auto inName = ctx->session->GetInputNameAllocated(0, allocator);
            ctx->inputName = inName.get();

            // YOLO-seg has two outputs; identify which is proto (rank 4) vs
            // detection (rank 3) by inspecting their shapes.
            const size_t outCount = ctx->session->GetOutputCount();
            if (outCount < 2) {
                ZM_LOG_ERROR("detect_seg: model has %zu output(s); YOLO-seg needs 2 "
                             "(running as pass-through)", outCount);
                ctx->session.reset();
            } else {
                auto n0 = ctx->session->GetOutputNameAllocated(0, allocator);
                auto n1 = ctx->session->GetOutputNameAllocated(1, allocator);
                std::string name0 = n0.get();
                std::string name1 = n1.get();
                auto rank0 = ctx->session->GetOutputTypeInfo(0)
                                 .GetTensorTypeAndShapeInfo().GetShape().size();
                auto rank1 = ctx->session->GetOutputTypeInfo(1)
                                 .GetTensorTypeAndShapeInfo().GetShape().size();
                if (rank0 == 4) { ctx->protoName = name0; ctx->detName = name1; }
                else if (rank1 == 4) { ctx->protoName = name1; ctx->detName = name0; }
                else {
                    // Fall back to ordering: out0 detection, out1 proto.
                    ctx->detName = name0; ctx->protoName = name1;
                }
                ZM_LOG_INFO("detect_seg: loaded model '%s' (input='%s' det='%s' proto='%s' "
                            "net=%d ep=%s)", ctx->modelPath.c_str(), ctx->inputName.c_str(),
                            ctx->detName.c_str(), ctx->protoName.c_str(), ctx->net,
                            ctx->ep.c_str());
            }
        } catch (const std::exception& e) {
            ZM_LOG_ERROR("detect_seg: failed to load model '%s': %s (running as pass-through)",
                         ctx->modelPath.c_str(), e.what());
            ctx->session.reset();
        }
    } else {
        ZM_LOG_WARN("detect_seg: no model_path configured; running as pass-through");
    }

    plugin->instance = ctx;
    return 0;
}

static void detect_seg_stop(zm_plugin_t* plugin) {
    auto* ctx = static_cast<DetectSegCtx*>(plugin->instance);
    if (ctx) {
        delete ctx;
        plugin->instance = nullptr;
    }
}

static void detect_seg_on_frame(zm_plugin_t* plugin, const void* buf, size_t size) {
    auto* ctx = static_cast<DetectSegCtx*>(plugin->instance);
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
        const char* outputNames[] = {ctx->detName.c_str(), ctx->protoName.c_str()};

        auto outputs = ctx->session->Run(Ort::RunOptions{nullptr}, inputNames,
                                         &inputTensor, 1, outputNames, 2);

        // outputs[0] = detection, outputs[1] = proto (per outputNames order).
        const float* det = outputs[0].GetTensorData<float>();
        auto detShape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
        const float* proto = outputs[1].GetTensorData<float>();
        auto protoShape = outputs[1].GetTensorTypeAndShapeInfo().GetShape();

        if (detShape.size() != 3 || protoShape.size() != 4) {
            if (!ctx->warnedShape) {
                ZM_LOG_WARN("detect_seg: unexpected output ranks (det=%zu proto=%zu); "
                            "expected det rank 3, proto rank 4",
                            detShape.size(), protoShape.size());
                ctx->warnedShape = true;
            }
            forwardFrame(ctx, buf, size);
            return;
        }

        // proto: [1, nm, mh, mw]
        const int nm = static_cast<int>(protoShape[1]);
        const int mh = static_cast<int>(protoShape[2]);
        const int mw = static_cast<int>(protoShape[3]);

        // detection: [1, channels, num] (channel-major) or [1, num, channels].
        const int64_t d1 = detShape[1];
        const int64_t d2 = detShape[2];
        // channels = 4 + nc + nm; the larger of the two trailing dims is `num`
        // (8400-ish) and the smaller is `channels`.
        bool channelMajor = d1 < d2;
        const int channels = static_cast<int>(channelMajor ? d1 : d2);
        const int num = static_cast<int>(channelMajor ? d2 : d1);

        const int maskDim = nm;  // proto's nm is authoritative
        int numClasses = ctx->numClasses;
        if (numClasses <= 0) numClasses = channels - 4 - maskDim;
        if (numClasses <= 0) {
            if (!ctx->warnedShape) {
                ZM_LOG_WARN("detect_seg: cannot infer num_classes (channels=%d mask_dim=%d)",
                            channels, maskDim);
                ctx->warnedShape = true;
            }
            forwardFrame(ctx, buf, size);
            return;
        }

        std::vector<zm::seg::SegObj> objs = zm::seg::decode(
            det, num, channels, numClasses, maskDim, channelMajor, lb,
            ctx->confThreshold, ctx->classFilter);
        objs = zm::seg::nms(std::move(objs), ctx->iouThreshold);

        if (objs.empty()) {
            forwardFrame(ctx, buf, size);
            return;
        }

        const bool wantPolygon = (ctx->maskFormat != "none");

        json objsJson = json::array();
        for (auto& o : objs) {
            if (wantPolygon) {
                std::vector<float> mask =
                    zm::seg::build_mask(proto, maskDim, mh, mw, o.coeffs);
                o.polygon = zm::seg::mask_to_polygon(mask, mh, mw, 0.5f, lb, o);
            }
            std::string scratch;
            json od;
            od["label"] = className(ctx, o.class_id, scratch);
            od["confidence"] = o.confidence;
            od["bbox"] = {o.x, o.y, o.w, o.h};
            od["class_id"] = o.class_id;
            if (wantPolygon) {
                json poly = json::array();
                for (const auto& p : o.polygon) poly.push_back({p[0], p[1]});
                od["polygon"] = std::move(poly);
            }
            objsJson.push_back(std::move(od));
        }

        json evt;
        evt["type"] = "segmentation";
        evt["stream_id"] = hdr->stream_id;
        evt["pts_usec"] = hdr->pts_usec;
        evt["objects"] = std::move(objsJson);
        if (ctx->host && ctx->host->publish_evt)
            ctx->host->publish_evt(ctx->hostCtx, evt.dump().c_str());
    } catch (const std::exception& e) {
        ZM_LOG_ERROR("detect_seg: inference error: %s", e.what());
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
    plugin->start = detect_seg_start;
    plugin->stop = detect_seg_stop;
    plugin->on_frame = detect_seg_on_frame;
    plugin->instance = nullptr;
}

} // extern "C"

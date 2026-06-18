// detect_pose: YOLO-pose human pose-estimation plugin (ONNX Runtime C++ API).
//
// Runs a YOLO-pose ONNX model (e.g. yolo11n-pose / yolov8-pose) on decoded
// RGB24 frames and publishes one "pose" event per frame listing each detected
// person and its 17 COCO keypoints. Always forwards the frame downstream (the
// plugin is a pass-through DETECT stage). If no model is loaded, or the frame is
// not RGB24, or the stream is filtered out, it simply forwards.
//
// Unlike detect_onnx's YOLO26 NMS-free [1,N,6] output, the YOLO-pose export is a
// single tensor [1, 56, 8400] (channel-major, needs transpose) or [1, 8400, 56]
// (candidate-major). Each candidate = [cx,cy,w,h,conf, 17*(x,y,vis)]. There is
// one class (person), conf is the person score, and the output is NOT
// pre-deduplicated, so we must confidence-filter then run IoU NMS.

#include "pose_postprocess.hpp"

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

// Default COCO-17 keypoint names, used when "keypoint_names" is absent.
static const std::array<const char*, 17> kCocoKeypoints = {
    "nose", "left_eye", "right_eye", "left_ear", "right_ear",
    "left_shoulder", "right_shoulder", "left_elbow", "right_elbow",
    "left_wrist", "right_wrist", "left_hip", "right_hip",
    "left_knee", "right_knee", "left_ankle", "right_ankle"
};

struct DetectPoseCtx {
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
    int net = 640;             // input_size
    float confThreshold = 0.25f;
    float iouThreshold = 0.45f; // for NMS
    int frameWidth = 0;
    int frameHeight = 0;
    std::string ep = "cpu";
    std::vector<int> streamFilter;             // empty = all
    std::vector<std::string> keypointNames;    // empty = use COCO-17

    bool warnedUnsupportedShape = false;
};

const char* keypointName(const DetectPoseCtx* ctx, int k) {
    if (!ctx->keypointNames.empty()) {
        if (k >= 0 && k < static_cast<int>(ctx->keypointNames.size()))
            return ctx->keypointNames[k].c_str();
    } else if (k >= 0 && k < static_cast<int>(kCocoKeypoints.size())) {
        return kCocoKeypoints[k];
    }
    return "";
}

void forwardFrame(DetectPoseCtx* ctx, const void* buf, size_t size) {
    if (ctx && ctx->host && ctx->host->on_frame) {
        ctx->host->on_frame(ctx->hostCtx, buf, size);
    }
}

} // namespace

extern "C" {

static int detect_pose_start(zm_plugin_t* plugin, zm_host_api_t* host, void* host_ctx,
                             const char* json_cfg) {
    auto* ctx = new DetectPoseCtx;
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
            ctx->frameWidth = j.value("frame_width", 0);
            ctx->frameHeight = j.value("frame_height", 0);
            ctx->ep = j.value("ep", std::string("cpu"));
            if (j.contains("stream_filter") && j["stream_filter"].is_array())
                ctx->streamFilter = j["stream_filter"].get<std::vector<int>>();
            if (j.contains("keypoint_names") && j["keypoint_names"].is_array())
                ctx->keypointNames = j["keypoint_names"].get<std::vector<std::string>>();
        } catch (const std::exception& e) {
            ZM_LOG_ERROR("detect_pose: failed to parse config: %s", e.what());
        }
    }

    // Create the ONNX Runtime environment and session options.
    try {
        ctx->env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "detect_pose");
        ctx->sessionOptions.SetIntraOpNumThreads(1);
        ctx->sessionOptions.SetGraphOptimizationLevel(ORT_ENABLE_ALL);
    } catch (const std::exception& e) {
        ZM_LOG_ERROR("detect_pose: failed to init ORT env: %s", e.what());
    }

    // Optionally append the CoreML execution provider, falling back to CPU.
    if (ctx->ep == "coreml") {
        try {
            uint32_t coreml_flags = 0;
            Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_CoreML(
                static_cast<OrtSessionOptions*>(ctx->sessionOptions), coreml_flags));
            ZM_LOG_INFO("detect_pose: CoreML execution provider enabled");
        } catch (const std::exception& e) {
            ZM_LOG_WARN("detect_pose: CoreML EP unavailable, falling back to CPU: %s",
                        e.what());
        }
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

            ZM_LOG_INFO("detect_pose: loaded model '%s' (input='%s' output='%s' net=%d ep=%s)",
                        ctx->modelPath.c_str(), ctx->inputName.c_str(),
                        ctx->outputName.c_str(), ctx->net, ctx->ep.c_str());
        } catch (const std::exception& e) {
            ZM_LOG_ERROR("detect_pose: failed to load model '%s': %s (running as pass-through)",
                         ctx->modelPath.c_str(), e.what());
            ctx->session.reset();
        }
    } else {
        ZM_LOG_WARN("detect_pose: no model_path configured; running as pass-through");
    }

    plugin->instance = ctx;
    return 0;
}

static void detect_pose_stop(zm_plugin_t* plugin) {
    auto* ctx = static_cast<DetectPoseCtx*>(plugin->instance);
    if (ctx) {
        delete ctx;
        plugin->instance = nullptr;
    }
}

// Build and publish a "pose" event from decoded persons (source-pixel coords).
static void publishPersons(DetectPoseCtx* ctx, const zm_frame_hdr_t* hdr,
                           const std::vector<zm::pose::Person>& persons) {
    if (persons.empty()) return;
    json arr = json::array();
    for (const auto& p : persons) {
        json kpts = json::array();
        for (int k = 0; k < static_cast<int>(p.kpts.size()); ++k) {
            json kp;
            kp["name"] = keypointName(ctx, k);
            kp["x"] = p.kpts[k].x;
            kp["y"] = p.kpts[k].y;
            kp["v"] = p.kpts[k].v;
            kpts.push_back(std::move(kp));
        }
        json person;
        person["confidence"] = p.confidence;
        person["bbox"] = {p.x, p.y, p.w, p.h};
        person["keypoints"] = std::move(kpts);
        arr.push_back(std::move(person));
    }
    json evt;
    evt["type"] = "pose";
    evt["stream_id"] = hdr->stream_id;
    evt["pts_usec"] = hdr->pts_usec;
    evt["persons"] = std::move(arr);
    if (ctx->host && ctx->host->publish_evt)
        ctx->host->publish_evt(ctx->hostCtx, evt.dump().c_str());
}

static void detect_pose_on_frame(zm_plugin_t* plugin, const void* buf, size_t size) {
    auto* ctx = static_cast<DetectPoseCtx*>(plugin->instance);
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

        // Expect a 3-D output [1, A, B] where one of A/B is the per-candidate
        // value count (5 + 17*3 = 56) and the other is the candidate count.
        if (shape.size() != 3) {
            if (!ctx->warnedUnsupportedShape) {
                ZM_LOG_WARN("detect_pose: unsupported output rank %zu; expected [1,56,N] or [1,N,56]",
                            shape.size());
                ctx->warnedUnsupportedShape = true;
            }
            forwardFrame(ctx, buf, size);
            return;
        }

        const int dimA = static_cast<int>(shape[1]);
        const int dimB = static_cast<int>(shape[2]);

        int values = 0, num = 0;
        bool channelMajor = false;
        if (dimA < dimB) {
            // [1, 56, 8400] channel-major: values along dim1, candidates dim2.
            values = dimA;
            num = dimB;
            channelMajor = true;
        } else {
            // [1, 8400, 56] candidate-major: candidates dim1, values dim2.
            values = dimB;
            num = dimA;
            channelMajor = false;
        }

        // values = 5 + num_kpts*3. Derive num_kpts; require it to be >= 1.
        const int numKpts = (values - 5) / 3;
        if (values < 8 || (values - 5) % 3 != 0 || numKpts < 1) {
            if (!ctx->warnedUnsupportedShape) {
                ZM_LOG_WARN("detect_pose: unexpected per-candidate value count %d "
                            "(expected 5 + 3*num_kpts, e.g. 56)", values);
                ctx->warnedUnsupportedShape = true;
            }
            forwardFrame(ctx, buf, size);
            return;
        }

        std::vector<zm::pose::Person> persons =
            zm::pose::decode(out, num, numKpts, channelMajor, lb, ctx->confThreshold);
        persons = zm::pose::nms(std::move(persons), ctx->iouThreshold);

        publishPersons(ctx, hdr, persons);
    } catch (const std::exception& e) {
        ZM_LOG_ERROR("detect_pose: inference error: %s", e.what());
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
    plugin->start = detect_pose_start;
    plugin->stop = detect_pose_stop;
    plugin->on_frame = detect_pose_on_frame;
    plugin->instance = nullptr;
}

} // extern "C"

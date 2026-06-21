// detect_bench: standalone latency + correctness harness for the ONNX YOLO
// detector across execution providers (CPU / DirectML / OpenVINO). It mirrors the
// detect_onnx plugin's ORT session setup and reuses the production pre/post-
// processing (detect_postprocess.hpp), so the numbers reflect the real detect path
// minus the pipeline plumbing.
//
// Build (from a vcvars64 shell), e.g.:
//   cl /nologo /EHsc /std:c++20 /O2 ^
//      /I "%ORT%\include" /I "..\plugins\detect_onnx" ^
//      detect_bench.cpp /Fe:detect_bench.exe ^
//      /link /LIBPATH:"%ORT%\lib" onnxruntime.lib
//
// Usage:
//   detect_bench --model yolo.onnx --image people.jpg --ep cpu|dml|openvino
//                [--device 0] [--ov-device GPU|NPU|CPU] [--iters 50] [--warmup 5]
//                [--size 640] [--conf 0.25]

// Keep <windows.h> (pulled in by the DirectML/D3D12 headers) from defining the
// min/max macros that break std::min/std::max in detect_postprocess.hpp.
#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#endif

#include <onnxruntime_cxx_api.h>
#ifdef ZM_WITH_DIRECTML
#include <dml_provider_factory.h>
#endif

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "detect_postprocess.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <array>
#include <unordered_map>

using namespace zm::detect;
using clk = std::chrono::high_resolution_clock;

static const std::array<const char*, 80> kCoco = {
    "person","bicycle","car","motorcycle","airplane","bus","train","truck","boat",
    "traffic light","fire hydrant","stop sign","parking meter","bench","bird","cat",
    "dog","horse","sheep","cow","elephant","bear","zebra","giraffe","backpack",
    "umbrella","handbag","tie","suitcase","frisbee","skis","snowboard","sports ball",
    "kite","baseball bat","baseball glove","skateboard","surfboard","tennis racket",
    "bottle","wine glass","cup","fork","knife","spoon","bowl","banana","apple",
    "sandwich","orange","broccoli","carrot","hot dog","pizza","donut","cake","chair",
    "couch","potted plant","bed","dining table","toilet","tv","laptop","mouse",
    "remote","keyboard","cell phone","microwave","oven","toaster","sink","refrigerator",
    "book","clock","vase","scissors","teddy bear","hair drier","toothbrush"};

struct Args {
    std::string model, image, ep = "cpu", ov_device = "GPU";
    int device = 0, iters = 50, warmup = 5, size = 640;
    float conf = 0.25f;
};

static Args parse(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        auto val = [&]() { return (i + 1 < argc) ? argv[++i] : ""; };
        if (k == "--model") a.model = val();
        else if (k == "--image") a.image = val();
        else if (k == "--ep") a.ep = val();
        else if (k == "--device") a.device = std::atoi(val());
        else if (k == "--ov-device") a.ov_device = val();
        else if (k == "--iters") a.iters = std::atoi(val());
        else if (k == "--warmup") a.warmup = std::atoi(val());
        else if (k == "--size") a.size = std::atoi(val());
        else if (k == "--conf") a.conf = static_cast<float>(std::atof(val()));
    }
    return a;
}

int main(int argc, char** argv) {
    Args a = parse(argc, argv);
    if (a.model.empty() || a.image.empty()) {
        std::fprintf(stderr, "usage: detect_bench --model M.onnx --image I.jpg "
                             "--ep cpu|dml|openvino [--ov-device GPU|NPU|CPU] "
                             "[--iters N] [--size 640] [--conf 0.25]\n");
        return 2;
    }

    // ── Load image as RGB ────────────────────────────────────────────────────
    int iw = 0, ih = 0, ic = 0;
    unsigned char* img = stbi_load(a.image.c_str(), &iw, &ih, &ic, 3);
    if (!img) { std::fprintf(stderr, "failed to load image %s\n", a.image.c_str()); return 1; }
    std::printf("image: %s  %dx%d\n", a.image.c_str(), iw, ih);

    Letterbox lb = compute_letterbox(iw, ih, a.size);
    std::vector<float> input(static_cast<size_t>(3) * a.size * a.size);
    letterbox_rgb_to_chw(img, lb, input.data());
    stbi_image_free(img);

    // ── ORT session with the requested EP ────────────────────────────────────
    Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "detect_bench");
    Ort::SessionOptions so;
    so.SetIntraOpNumThreads(1);
    so.SetGraphOptimizationLevel(ORT_ENABLE_ALL);

    std::string ep_label = a.ep;
    if (a.ep == "dml" || a.ep == "directml") {
#ifdef ZM_WITH_DIRECTML
        so.DisableMemPattern();
        so.SetExecutionMode(ORT_SEQUENTIAL);
        Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_DML(
            static_cast<OrtSessionOptions*>(so), a.device));
        ep_label = "DirectML(device " + std::to_string(a.device) + ")";
#else
        std::fprintf(stderr, "built without ZM_WITH_DIRECTML\n"); return 3;
#endif
    } else if (a.ep == "openvino") {
        // ORT OpenVINO EP (requires an ORT built with OpenVINO support).
        std::unordered_map<std::string, std::string> ov;
        ov["device_type"] = a.ov_device;   // GPU / NPU / CPU
        so.AppendExecutionProvider("OpenVINO", ov);
        ep_label = "OpenVINO(" + a.ov_device + ")";
    } // else: default CPU EP

    std::wstring wmodel(a.model.begin(), a.model.end());
#ifdef _WIN32
    Ort::Session session(env, wmodel.c_str(), so);
#else
    Ort::Session session(env, a.model.c_str(), so);
#endif

    Ort::AllocatorWithDefaultOptions alloc;
    auto inName = session.GetInputNameAllocated(0, alloc);
    auto outName = session.GetOutputNameAllocated(0, alloc);
    const char* inNames[] = {inName.get()};
    const char* outNames[] = {outName.get()};

    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::array<int64_t, 4> shape{1, 3, a.size, a.size};

    auto run_once = [&](std::vector<Box>* boxes) {
        Ort::Value t = Ort::Value::CreateTensor<float>(mem, input.data(), input.size(),
                                                       shape.data(), shape.size());
        auto out = session.Run(Ort::RunOptions{nullptr}, inNames, &t, 1, outNames, 1);
        if (boxes) {
            const float* o = out[0].GetTensorData<float>();
            auto os = out[0].GetTensorTypeAndShapeInfo().GetShape();
            int num = (os.size() == 3) ? (int)os[1] : (os.size() == 2 ? (int)os[0] : 0);
            *boxes = decode_nms_free(o, num, lb, a.conf);
        }
    };

    // ── Warmup + validate detections ─────────────────────────────────────────
    std::vector<Box> boxes;
    for (int i = 0; i < a.warmup; ++i) run_once(i == a.warmup - 1 ? &boxes : nullptr);

    std::printf("ep: %s\n", ep_label.c_str());
    std::printf("detections (conf>=%.2f): %zu\n", a.conf, boxes.size());
    int people = 0;
    for (const auto& b : boxes) {
        const char* name = (b.class_id >= 0 && b.class_id < 80) ? kCoco[b.class_id] : "?";
        if (b.class_id == 0) ++people;
        std::printf("  %-14s conf=%.3f  bbox=[%.0f,%.0f,%.0f,%.0f]\n",
                    name, b.confidence, b.x, b.y, b.w, b.h);
    }
    std::printf("person count: %d\n", people);

    // ── Timed runs ───────────────────────────────────────────────────────────
    std::vector<double> ms;
    ms.reserve(a.iters);
    for (int i = 0; i < a.iters; ++i) {
        auto t0 = clk::now();
        run_once(nullptr);
        auto t1 = clk::now();
        ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    std::sort(ms.begin(), ms.end());
    double sum = 0; for (double v : ms) sum += v;
    double mean = sum / ms.size();
    std::printf("\nlatency over %d iters (ms): min=%.2f  p50=%.2f  mean=%.2f  p90=%.2f  max=%.2f\n",
                a.iters, ms.front(), ms[ms.size() / 2], mean,
                ms[(size_t)(ms.size() * 0.9)], ms.back());
    std::printf("throughput: %.1f infer/s\n", 1000.0 / mean);
    return 0;
}

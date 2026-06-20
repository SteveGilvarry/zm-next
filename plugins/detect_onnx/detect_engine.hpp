#pragma once
// Shared, batched inference engine: ONE ORT session + ONE CUDA context serving
// many concurrent producers (camera/stream threads). Instead of each thread/
// camera owning its own session (~1 GB RAM + ~0.7 GB VRAM each) and running tiny
// one-frame inferences (full per-Run host overhead each), threads submit their
// already-preprocessed GPU tensor and a dispatcher coalesces concurrent requests
// into a single batched Run. Fixes the two costs we measured: memory (one context)
// and CPU (per-Run overhead amortised over the batch). Requires a dynamic-batch
// model. Compiled only when ZM_WITH_CUDA is enabled.

#ifdef ZMP_WITH_CUDA

#include "detect_postprocess.hpp"        // Box, Letterbox, decode_nms_free
#include <onnxruntime_cxx_api.h>

#include <atomic>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace zm::detect {

class InferenceEngine {
public:
    // model: ONNX path (dynamic batch dim). net: input size. maxBatch: cap per Run.
    // maxWaitUs: linger after the first request to let a batch fill (latency knob).
    InferenceEngine(const std::string& model, int net, int maxBatch, int maxWaitUs);
    ~InferenceEngine();

    // d_chw: device pointer to one [3,net,net] float tensor (caller-owned; stays
    // valid until this returns, which is fine because the call blocks). Returns the
    // decoded boxes for that tensor. Concurrent calls are batched into one Run.
    std::vector<Box> infer(const float* d_chw, const Letterbox& lb, float conf,
                           const std::vector<int>& allow);

    // Process-wide singleton per model path (shared across all threads/plugins).
    static InferenceEngine& get(const std::string& model, int net,
                                int maxBatch = 8, int maxWaitUs = 2000);

    long runs() const { return runs_.load(); }      // number of batched Runs
    long items() const { return items_.load(); }    // total tensors processed

private:
    struct Req {
        const float* src;
        Letterbox lb;
        float conf;
        const std::vector<int>* allow;
        std::promise<std::vector<Box>> prom;
    };
    void loop();

    Ort::Env env_;
    Ort::SessionOptions so_;
    std::unique_ptr<Ort::Session> sess_;
    std::string in_, out_;
    int net_, maxBatch_, maxWaitUs_;
    size_t per_ = 0;
    void* d_batch_ = nullptr;             // [maxBatch,3,net,net] device staging

    std::queue<Req*> q_;
    std::mutex m_;
    std::condition_variable cv_;
    std::thread th_;
    bool stop_ = false;
    std::atomic<long> runs_{0}, items_{0};
};

}  // namespace zm::detect

#endif  // ZMP_WITH_CUDA

#include "detect_engine.hpp"

#ifdef ZMP_WITH_CUDA

#include <cuda_runtime.h>
#include <array>
#include <chrono>
#include <map>

namespace zm::detect {

InferenceEngine::InferenceEngine(const std::string& model, int net, int maxBatch, int maxWaitUs)
    : env_(ORT_LOGGING_LEVEL_ERROR, "engine"), net_(net), maxBatch_(maxBatch), maxWaitUs_(maxWaitUs) {
    cudaSetDeviceFlags(cudaDeviceScheduleBlockingSync);   // CPU sleeps on GPU waits
    so_.SetIntraOpNumThreads(1);
    so_.SetGraphOptimizationLevel(ORT_ENABLE_ALL);
    OrtCUDAProviderOptions o{};
    so_.AppendExecutionProvider_CUDA(o);
    sess_ = std::make_unique<Ort::Session>(env_, model.c_str(), so_);
    Ort::AllocatorWithDefaultOptions a;
    in_ = sess_->GetInputNameAllocated(0, a).get();
    out_ = sess_->GetOutputNameAllocated(0, a).get();
    per_ = static_cast<size_t>(3) * net_ * net_;
    cudaMalloc(&d_batch_, per_ * maxBatch_ * sizeof(float));
    th_ = std::thread(&InferenceEngine::loop, this);
}

InferenceEngine::~InferenceEngine() {
    { std::lock_guard<std::mutex> lk(m_); stop_ = true; }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
    if (d_batch_) cudaFree(d_batch_);
}

std::vector<Box> InferenceEngine::infer(const float* d_chw, const Letterbox& lb, float conf,
                                        const std::vector<int>& allow) {
    Req r; r.src = d_chw; r.lb = lb; r.conf = conf; r.allow = &allow;
    auto fut = r.prom.get_future();
    { std::lock_guard<std::mutex> lk(m_); q_.push(&r); }
    cv_.notify_one();
    return fut.get();
}

void InferenceEngine::loop() {
    while (true) {
        std::vector<Req*> batch;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [&] { return stop_ || !q_.empty(); });
            if (stop_ && q_.empty()) return;
            // Linger briefly so concurrent producers coalesce into one Run.
            if (q_.size() < static_cast<size_t>(maxBatch_) && maxWaitUs_ > 0)
                cv_.wait_for(lk, std::chrono::microseconds(maxWaitUs_),
                             [&] { return stop_ || q_.size() >= static_cast<size_t>(maxBatch_); });
            while (!q_.empty() && batch.size() < static_cast<size_t>(maxBatch_)) {
                batch.push_back(q_.front()); q_.pop();
            }
        }
        const int N = static_cast<int>(batch.size());
        if (N == 0) continue;

        // Assemble a contiguous [N,3,net,net] batch from each request's tensor.
        for (int i = 0; i < N; ++i)
            cudaMemcpy(static_cast<float*>(d_batch_) + static_cast<size_t>(i) * per_,
                       batch[i]->src, per_ * sizeof(float), cudaMemcpyDeviceToDevice);

        Ort::MemoryInfo cudaMem("Cuda", OrtArenaAllocator, 0, OrtMemTypeDefault);
        const std::array<int64_t, 4> shp{N, 3, net_, net_};
        Ort::Value t = Ort::Value::CreateTensor<float>(cudaMem, static_cast<float*>(d_batch_),
                                                       per_ * N, shp.data(), shp.size());
        Ort::IoBinding b(*sess_);
        b.BindInput(in_.c_str(), t);
        b.BindOutput(out_.c_str(), Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault));
        sess_->Run(Ort::RunOptions{nullptr}, b);
        cudaDeviceSynchronize();
        runs_.fetch_add(1); items_.fetch_add(N);

        auto outs = b.GetOutputValues();
        const float* od = outs.empty() ? nullptr : outs[0].GetTensorData<float>();
        const auto shape = outs.empty() ? std::vector<int64_t>{} : outs[0].GetTensorTypeAndShapeInfo().GetShape();
        const int rows = (shape.size() == 3 && shape.back() == 6) ? static_cast<int>(shape[1]) : 0;
        for (int i = 0; i < N; ++i) {
            std::vector<Box> boxes;
            if (od && rows > 0)
                boxes = decode_nms_free(od + static_cast<size_t>(i) * rows * 6, rows,
                                        batch[i]->lb, batch[i]->conf, *batch[i]->allow);
            batch[i]->prom.set_value(std::move(boxes));
        }
    }
}

InferenceEngine& InferenceEngine::get(const std::string& model, int net, int maxBatch, int maxWaitUs) {
    // Leaked on purpose: a process-wide singleton must NOT be destroyed at exit,
    // or its ORT/CUDA teardown races the driver shutdown (CUDA failure 4). The OS
    // reclaims everything at exit anyway.
    static std::mutex mm;
    static auto* reg = new std::map<std::string, InferenceEngine*>();
    std::lock_guard<std::mutex> lk(mm);
    auto& e = (*reg)[model];
    if (!e) e = new InferenceEngine(model, net, maxBatch, maxWaitUs);
    return *e;
}

}  // namespace zm::detect

#endif  // ZMP_WITH_CUDA

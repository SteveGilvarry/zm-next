#include "hw_backend.hpp"

#ifdef ZMP_WITH_CUDA

#include "detect_cuda.hpp"      // cuda_motion_regions, cuda_preprocess_nv12
#include "detect_engine.hpp"    // shared batched InferenceEngine
#include <algorithm>

extern "C" {
#include <libavutil/frame.h>    // av_frame_clone / av_frame_free  (surface lifetime)
}

namespace zm::hw {
namespace {

// CUDA implementation of the fused on-device pattern: NVDEC surface -> on-GPU motion
// -> on-GPU preprocess -> shared batched ORT/CUDA inference. Decode + the kernels +
// the engine were already built and benchmarked; this just expresses them through
// the backend interface so Metal/OpenVINO/ROCm can be added the same way.
class CudaBackend : public HwBackend {
public:
    const char* name() const override { return "cuda"; }

    bool load_model(const std::string& path, int net) override {
        model_ = path; net_ = net;
        try { zm::detect::InferenceEngine::get(model_, net_); return true; }   // loads the shared session
        catch (...) { return false; }
    }

    // av_frame_clone() refs the same GPU surface, so it outlives the decoder's
    // call-scoped AVFrame and could even cross a queue. release() drops the ref.
    Surface acquire(uint64_t av_frame) override {
        Surface s;
        AVFrame* src = reinterpret_cast<AVFrame*>(av_frame);
        if (!src) return s;
        AVFrame* held = av_frame_clone(src);
        if (!held) return s;
        s.owner = held;
        s.hw_type = 1;  // ZM_HW_CUDA
        s.width = held->width; s.height = held->height;
        for (int i = 0; i < 4; ++i) {
            s.plane_ptr[i] = reinterpret_cast<uint64_t>(held->data[i]);
            s.linesize[i] = held->linesize[i];
        }
        s.native = reinterpret_cast<uint64_t>(held);
        return s;
    }

    void release(Surface& s) override {
        if (s.owner) { AVFrame* f = static_cast<AVFrame*>(s.owner); av_frame_free(&f); s.owner = nullptr; }
    }

    std::vector<Region> motion(const Surface& s) override {
        if (!s.plane_ptr[0]) return {};
        if (minCells_ <= 0) minCells_ = std::max(8, (s.width / ds_) * (s.height / ds_) / 400);
        auto regions = zm::detect::cuda_motion_regions(s.plane_ptr[0], s.linesize[0], s.width, s.height,
                                                       prevGrid_, ds_, thr_, minCells_, maxRegions_);
        std::vector<Region> out; out.reserve(regions.size());
        for (auto& m : regions) out.push_back({m.x, m.y, m.w, m.h});
        return out;
    }

    DeviceTensor preprocess(const Surface& s, Region crop) override {
        DeviceTensor t; t.net = net_;
        if (!s.plane_ptr[0] || !s.plane_ptr[1]) return t;
        const float* d = zm::detect::cuda_preprocess_nv12(
            s.plane_ptr[0], s.linesize[0], s.plane_ptr[1], s.linesize[1],
            s.width, s.height, net_, t.lb, crop.x, crop.y, crop.w, crop.h);
        t.ptr = const_cast<float*>(d);
        return t;
    }

    std::vector<Detection> infer(const DeviceTensor& t, float conf,
                                 const std::vector<int>& allow) override {
        if (!t.ptr) return {};
        return zm::detect::InferenceEngine::get(model_, net_)
            .infer(static_cast<const float*>(t.ptr), t.lb, conf, allow);
    }

private:
    std::string model_;
    int net_ = 640;
    std::vector<uint8_t> prevGrid_;            // per-instance motion state
    int ds_ = 8, thr_ = 25, minCells_ = 0, maxRegions_ = 8;
};

}  // namespace

std::unique_ptr<HwBackend> make_backend(const std::string& kind) {
    if (kind == "cuda") return std::make_unique<CudaBackend>();
    return nullptr;   // "metal" / "openvino" / "rocm" / "directml" go here
}

}  // namespace zm::hw

#else  // !ZMP_WITH_CUDA — factory still resolves, returns nullptr for unsupported backends.

namespace zm::hw {
std::unique_ptr<HwBackend> make_backend(const std::string&) { return nullptr; }
}

#endif

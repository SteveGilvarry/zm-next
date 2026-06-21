#include "hw_backend.hpp"

#ifdef ZMP_WITH_CUDA

#include "detect_cuda.hpp"      // cuda_motion_bbox_gpudiff / cuda_motion_regions_cpudiff, cuda_preprocess_nv12
#include "detect_engine.hpp"    // shared batched InferenceEngine
#include <algorithm>
#include <cstdlib>              // getenv (ZM_MOTION_REGIONS opt-in)

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
    ~CudaBackend() override {
        if (gpuDiff_) { zm::detect::gpudiff_state_destroy(gpuDiff_); gpuDiff_ = nullptr; }
    }

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

    // Motion gate. DEFAULT: cuda_motion_bbox_gpudiff — fully GPU-resident (downsample
    // AND diff on the device, prev grid stays device-resident, only a ~24B verdict
    // crosses PCIe), returning ONE merged ROI. Set ZM_MOTION_REGIONS=1 for the
    // opt-in cuda_motion_regions_cpudiff multi-mover path, which copies the full grid
    // back and runs connected-components on the CPU to yield separate per-mover boxes.
    std::vector<Region> motion(const Surface& s) override {
        if (!s.plane_ptr[0]) return {};
        if (minCells_ <= 0) minCells_ = std::max(8, (s.width / ds_) * (s.height / ds_) / 400);

        static const bool useRegions = []{ const char* e = getenv("ZM_MOTION_REGIONS"); return e && *e == '1'; }();
        if (useRegions) {
            auto regions = zm::detect::cuda_motion_regions_cpudiff(s.plane_ptr[0], s.linesize[0], s.width, s.height,
                                                           prevGrid_, ds_, thr_, minCells_, maxRegions_);
            std::vector<Region> out; out.reserve(regions.size());
            for (auto& m : regions) out.push_back({m.x, m.y, m.w, m.h});
            return out;
        }

        if (!gpuDiff_) gpuDiff_ = zm::detect::gpudiff_state_create();
        auto m = zm::detect::cuda_motion_bbox_gpudiff(s.plane_ptr[0], s.linesize[0], s.width, s.height,
                                                      gpuDiff_, ds_, thr_, minCells_);
        if (!m.active) return {};
        return { Region{m.x, m.y, m.w, m.h} };
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
    std::vector<uint8_t> prevGrid_;            // per-instance motion state (regions opt-in path)
    zm::detect::GpuDiffState* gpuDiff_ = nullptr;  // device-resident prev grid (default gpudiff path), lazily created
    int ds_ = 8, thr_ = 25, minCells_ = 0, maxRegions_ = 8;
};

}  // namespace

// Narrow factory entry the shared make_backend() (hw_backend.cpp) dispatches to.
// CudaBackend stays unchanged; only the make_backend() symbol moved out so the
// "metal"/"openvino"/"vaapi" backends can live in their own TUs without colliding.
std::unique_ptr<HwBackend> make_cuda_backend() {
    return std::make_unique<CudaBackend>();
}

}  // namespace zm::hw

#endif  // ZMP_WITH_CUDA — when off, this TU is empty; hw_backend.cpp owns the factory.

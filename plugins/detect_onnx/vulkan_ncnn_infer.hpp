#pragma once
// ncnn-Vulkan YOLO inference, behind a plain interface so the ncnn headers (which
// bundle their own Vulkan loader, ncnn/simplevk.h) never meet the system
// <vulkan/vulkan.h> used by the compute side. hw_backend_vulkan.cpp includes this
// header (pure types only); the implementation TU vulkan_ncnn_infer.cpp is the
// ONLY place that includes ncnn. They communicate via a host CHW float buffer.

#include "detect_postprocess.hpp"   // zm::detect::Box, Letterbox
#include <string>
#include <vector>

namespace zm::hw {

class NcnnYolo {
public:
    NcnnYolo() = default;
    ~NcnnYolo();
    NcnnYolo(const NcnnYolo&) = delete;
    NcnnYolo& operator=(const NcnnYolo&) = delete;

    // Load the converted ncnn head model (.param/.bin) with Vulkan compute on.
    // dev < 0 => ncnn's default GPU; otherwise the given Vulkan GPU index.
    bool load(const std::string& param_path, const std::string& bin_path, int gpu = -1);

    // Run inference on a host CHW float tensor (3*net*net, RGB /255), decode the
    // [84 x 8400] head, and map boxes back to source pixels via lb. NMS is left to
    // the caller/tracker (parity with the detect_onnx flow). `allow` filters classes.
    std::vector<zm::detect::Box> infer(const float* chw, int net,
                                       const zm::detect::Letterbox& lb,
                                       float conf, const std::vector<int>& allow) const;

    bool ok() const { return impl_ != nullptr; }

private:
    void* impl_ = nullptr;   // opaque NcnnImpl (owns the ncnn::Net)
};

}  // namespace zm::hw

// ncnn-Vulkan inference TU. The ONLY file that includes ncnn (whose bundled
// simplevk.h conflicts with system vulkan.h), so it stays isolated from the
// Vulkan compute side. See vulkan_ncnn_infer.hpp.
#if defined(ZM_WITH_VULKAN)

#include "vulkan_ncnn_infer.hpp"
#include <ncnn/net.h>
#include <ncnn/mat.h>
#include <algorithm>

namespace zm::hw {

namespace { struct NcnnImpl { ncnn::Net net; }; }

NcnnYolo::~NcnnYolo() { delete static_cast<NcnnImpl*>(impl_); }

bool NcnnYolo::load(const std::string& param, const std::string& bin, int gpu) {
    auto* p = new NcnnImpl();
    p->net.opt.use_vulkan_compute = true;
    if (gpu >= 0) p->net.set_vulkan_device(gpu);
    if (p->net.load_param(param.c_str()) != 0 || p->net.load_model(bin.c_str()) != 0) {
        delete p; return false;
    }
    impl_ = p;
    return true;
}

std::vector<zm::detect::Box> NcnnYolo::infer(const float* chw, int net,
                                             const zm::detect::Letterbox& lb,
                                             float conf, const std::vector<int>& allow) const {
    std::vector<zm::detect::Box> out;
    if (!impl_) return out;
    ncnn::Net& n = static_cast<NcnnImpl*>(impl_)->net;
    // Wrap the host CHW (RGB planar, /255) as an ncnn Mat (w=net, h=net, c=3).
    ncnn::Mat in(net, net, 3, const_cast<float*>(chw));
    ncnn::Extractor ex = n.create_extractor();
    ex.input("in0", in);
    ncnn::Mat o;                      // head: [w=8400 anchors, h=84 channels]
    if (ex.extract("out0", o) != 0) return out;
    const int A = o.w, C = o.h;       // anchors, channels (4 box + 80 cls)
    for (int a = 0; a < A; ++a) {
        float bestS = 0.f; int bestC = -1;
        for (int c = 4; c < C; ++c) { float s = o.row(c)[a]; if (s > bestS) { bestS = s; bestC = c - 4; } }
        if (bestS < conf) continue;
        if (!allow.empty() && std::find(allow.begin(), allow.end(), bestC) == allow.end()) continue;
        const float cx = o.row(0)[a], cy = o.row(1)[a], w = o.row(2)[a], h = o.row(3)[a];
        zm::detect::Box b;
        b.x = (cx - w * 0.5f - lb.pad_x) / lb.scale;
        b.y = (cy - h * 0.5f - lb.pad_y) / lb.scale;
        b.w = w / lb.scale;
        b.h = h / lb.scale;
        b.confidence = bestS;
        b.class_id = bestC;
        out.push_back(b);
    }
    return out;
}

}  // namespace zm::hw

#endif  // ZM_WITH_VULKAN

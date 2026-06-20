#pragma once
// HwBackend — the "fused on-device" pattern as a swappable backend.
//
// Every accelerated detect stage does the same four things: hardware-decode to a
// device surface, (optionally) a cheap on-device motion check, preprocess the
// surface (or a crop) into a model input tensor, and run inference — all without a
// host round-trip. The vendor-specific part is only the interop glue. This
// interface captures the pattern once; a CUDA / Metal / OpenVINO / ROCm / DirectML
// implementation slots in behind it (chosen like an ONNX Runtime execution provider).
//
// Surfaces are owned across acquire()/release() so they survive crossing a thread
// queue (the call-scoped-surface problem); the fused single-thread case just
// acquire()s and release()s within one on_frame.
//
// (Lives next to detect for now since it reuses the detect postprocess types; it
// could move to core/ when a non-detect on-device plugin needs it too.)

#include "detect_postprocess.hpp"        // zm::detect::Box, Letterbox
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace zm::hw {

// Backend-agnostic decoded video surface. CUDA/HIP fill plane_ptr/linesize (raw
// device pointers); Metal/VAAPI carry the vendor object in `native` (CVPixelBuffer
// / VASurfaceID) and may leave plane_ptr zero. `owner` is a backend-private ref
// kept alive between acquire() and release().
struct Surface {
    uint32_t hw_type = 0;          // ZM_HW_* (CUDA/VAAPI/VTB/...)
    uint32_t pix_fmt = 0;          // native pixel format (e.g. NV12)
    int width = 0, height = 0;
    uint64_t plane_ptr[4] = {0, 0, 0, 0};
    int linesize[4] = {0, 0, 0, 0};
    uint64_t native = 0;           // vendor surface object
    void* owner = nullptr;         // backend-private lifetime ref
};

struct Region { int x = 0, y = 0, w = 0, h = 0; };

using Detection = zm::detect::Box;   // source-pixel xywh + confidence + class_id

// A device-resident, preprocessed CHW input tensor produced by a backend. `ptr` is
// opaque (device pointer / backend handle); `lb` maps the model output back to
// source pixels. Valid until the next preprocess() on the same thread.
struct DeviceTensor {
    void* ptr = nullptr;
    int net = 0;
    zm::detect::Letterbox lb;
    bool valid() const { return ptr != nullptr; }
};

class HwBackend {
public:
    virtual ~HwBackend() = default;
    virtual const char* name() const = 0;

    // Load the detection model once (net = square input size, e.g. 640).
    virtual bool load_model(const std::string& model_path, int net) = 0;

    // Take ownership of a decoded hw frame so it survives until release() (e.g.
    // across a StageRunner queue). For FFmpeg-backed surfaces av_frame is an AVFrame*.
    virtual Surface acquire(uint64_t av_frame) = 0;
    virtual void release(Surface& s) = 0;

    // Cheap on-device motion vs the previous frame: changed regions in source px.
    virtual std::vector<Region> motion(const Surface& s) = 0;

    // Preprocess a (crop of a) surface into a device CHW tensor. crop {0,0,0,0} =
    // whole frame. Consume the tensor (infer) before the next preprocess on this thread.
    virtual DeviceTensor preprocess(const Surface& s, Region crop = {}) = 0;

    // Run inference on a device tensor; boxes are mapped to source pixels.
    virtual std::vector<Detection> infer(const DeviceTensor& t, float conf,
                                         const std::vector<int>& allow = {}) = 0;
};

// Build a backend by name: "cuda" today (when ZM_WITH_CUDA); "metal" / "openvino" /
// "rocm" / "directml" are the planned seams. Returns nullptr if unavailable, so a
// caller can fall back (e.g. to the CPU path).
std::unique_ptr<HwBackend> make_backend(const std::string& kind);

}  // namespace zm::hw

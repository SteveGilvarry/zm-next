#include "hw_backend.hpp"

// VAAPI HwBackend — the "fused on-device" pattern for AMD / Intel iGPUs.
//
// PROVEN on this box (AMD iGPU, /dev/dri/renderD129, Mesa VA-drivers 25.2.8):
//   * H264/HEVC hardware-decode via VAAPI; decoded frames stay on-GPU as
//     AV_PIX_FMT_VAAPI surfaces (the AVFrame carries a VASurfaceID in data[3]).
//   * scale_vaapi (the VPP / video-post-processing engine) downsamples and does
//     CSC entirely on the GPU; only the tiny motion grid needs to cross PCIe.
//   * RADV Vulkan + vaapi/vulkan/drm hwaccels are available, so a future fully
//     device-resident diff (parity with the CUDA gpudiff path) is feasible.
//
// This mirrors hw_backend_cuda.cpp structurally: acquire/release hold the hw
// AVFrame across a queue, motion() is a cheap luma-diff gate, preprocess()
// produces a model input, infer() runs ORT. The only vendor-specific glue is the
// VAAPI VPP scaling + the (host) NV12->letterbox bridge to ORT.
//
// IMPLEMENTATION NOTE on the VPP path:
//   Two ways to drive VAAPI VPP exist:
//     (A) libavfilter "scale_vaapi" graph  — needs only the FFmpeg headers and is
//         what is wired below. This is the portable path and keeps us off the raw
//         libva ABI (which is not even installed as -dev headers on this box).
//     (B) raw libva (vaCreateConfig/vaCreateContext with VAProcPipeline) — lower
//         overhead, lets us reuse one VPP context, but pulls in <va/va.h>. Sketched
//         behind ZM_WITH_VAAPI_VPP so it does not break the syntax check when libva
//         headers are absent. See vpp_scale_raw_va() TODO below.

#if defined(ZM_WITH_VAAPI)

#include "detect_postprocess.hpp"   // zm::detect::Letterbox / Box / decode_nms_free / letterbox_rgb_to_chw
#include <algorithm>
#include <cstdlib>                  // getenv (ZM_MOTION_REGIONS opt-in, parity with CUDA)
#include <cstring>
#include <cmath>
#include <mutex>
#include <vector>

extern "C" {
#include <libavutil/frame.h>            // av_frame_clone / av_frame_free / av_frame_alloc
#include <libavutil/hwcontext.h>        // av_hwframe_transfer_data (hwdownload)
#include <libavutil/pixfmt.h>           // AV_PIX_FMT_VAAPI / AV_PIX_FMT_NV12
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavfilter/avfilter.h>       // VPP via the libavfilter scale_vaapi graph (path A)
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
}

// ONNX Runtime. Realistic EP on AMD/Intel is OpenVINO; we wire the CPU EP here so
// the backend is functional everywhere, and note the OpenVINO seam in infer().
#include <onnxruntime_cxx_api.h>
#include <cpu_provider_factory.h>       // OrtSessionOptionsAppendExecutionProvider_CPU

// ZM_HW_VAAPI surface tag. Kept local to avoid editing the shared ABI header here;
// the value matches the VAAPI slot in zm_frame_hdr_t's hw_type enum (CPU=0, CUDA=1,
// VAAPI=2, VideoToolbox=3, DXVA=4). TODO: pull this from zm_plugin.h once wired.
#ifndef ZM_HW_VAAPI
#define ZM_HW_VAAPI 2u
#endif

namespace zm::hw {
namespace {

// ---------------------------------------------------------------------------
// Convert a packed NV12 host buffer (Y plane + interleaved UV plane) to an
// interleaved RGB24 buffer so we can reuse zm::detect::letterbox_rgb_to_chw.
// BT.601 limited-range, integer math. Used by the honest hwdownload fallback in
// preprocess() (see the zero-copy TODO there).
// ---------------------------------------------------------------------------
void nv12_to_rgb24(const uint8_t* y, int y_stride,
                   const uint8_t* uv, int uv_stride,
                   int w, int h, std::vector<uint8_t>& rgb) {
    rgb.resize(static_cast<size_t>(w) * h * 3);
    for (int j = 0; j < h; ++j) {
        const uint8_t* yr = y + static_cast<size_t>(j) * y_stride;
        const uint8_t* uvr = uv + static_cast<size_t>(j / 2) * uv_stride;
        uint8_t* out = rgb.data() + static_cast<size_t>(j) * w * 3;
        for (int i = 0; i < w; ++i) {
            const int Y = static_cast<int>(yr[i]) - 16;
            const int U = static_cast<int>(uvr[(i / 2) * 2 + 0]) - 128;
            const int V = static_cast<int>(uvr[(i / 2) * 2 + 1]) - 128;
            const int c = 298 * Y;
            int r = (c + 409 * V + 128) >> 8;
            int g = (c - 100 * U - 208 * V + 128) >> 8;
            int b = (c + 516 * U + 128) >> 8;
            out[i * 3 + 0] = static_cast<uint8_t>(std::clamp(r, 0, 255));
            out[i * 3 + 1] = static_cast<uint8_t>(std::clamp(g, 0, 255));
            out[i * 3 + 2] = static_cast<uint8_t>(std::clamp(b, 0, 255));
        }
    }
}

// ---------------------------------------------------------------------------
// A libavfilter graph that runs entirely on the VAAPI device:
//   buffer (vaapi) -> scale_vaapi=W:H[:format=nv12] -> hwdownload -> buffersink
// We keep a small cache keyed on (in_w,in_h,out_w,out_h) so motion() and
// preprocess() can each own a graph sized to their target without rebuilding per
// frame. The input frame's hw_frames_ctx is bound on first push.
// ---------------------------------------------------------------------------
struct VppGraph {
    AVFilterGraph*   graph = nullptr;
    AVFilterContext* src   = nullptr;
    AVFilterContext* sink  = nullptr;
    int in_w = 0, in_h = 0, out_w = 0, out_h = 0;

    ~VppGraph() { if (graph) avfilter_graph_free(&graph); }

    // Build for a given input hw frame and output size. `out_fmt` is the SOFTWARE
    // pixel format produced after hwdownload (NV12). Returns false on any error.
    bool build(const AVFrame* in, int ow, int oh) {
        if (graph) avfilter_graph_free(&graph);
        src = sink = nullptr;
        in_w = in->width; in_h = in->height; out_w = ow; out_h = oh;

        graph = avfilter_graph_alloc();
        if (!graph) return false;

        const AVFilter* bufsrc  = avfilter_get_by_name("buffer");
        const AVFilter* bufsink = avfilter_get_by_name("buffersink");
        if (!bufsrc || !bufsink) return false;

        // Allocate (NOT init) the hw buffer source, then set its parameters —
        // crucially hw_frames_ctx — BEFORE init. FFmpeg 8 rejects a HW pix_fmt at
        // init time when hw_frames_ctx is still null (the crash we hit). So we must
        // not pass pix_fmt via the args string (which would init immediately); we
        // alloc, set params incl. the decoder's frames ctx, then avfilter_init_str.
        src = avfilter_graph_alloc_filter(graph, bufsrc, "in");
        if (!src) return false;
        AVBufferSrcParameters* par = av_buffersrc_parameters_alloc();
        if (!par) return false;
        par->format        = AV_PIX_FMT_VAAPI;
        par->width         = in->width;
        par->height        = in->height;
        par->time_base     = AVRational{1, 1};
        par->hw_frames_ctx = in->hw_frames_ctx;   // carries VADisplay + surface pool
        const int prc = av_buffersrc_parameters_set(src, par);
        av_free(par);
        if (prc < 0) return false;
        if (avfilter_init_str(src, nullptr) < 0) return false;

        if (avfilter_graph_create_filter(&sink, bufsink, "out", nullptr, nullptr, graph) < 0)
            return false;

        // scale_vaapi does the downsample + CSC on the VPP engine; hwdownload pulls
        // the (small) result to a host NV12 frame. For the motion grid this is tiny;
        // for preprocess it is the honest fallback documented at the call site.
        //
        // NOTE: a Vulkan compute shader could replace hwdownload here and diff the
        // grid on-device for full residency — parity with the CUDA gpudiff path.
        char desc[256];
        std::snprintf(desc, sizeof(desc),
                      "scale_vaapi=w=%d:h=%d:format=nv12,hwdownload,format=nv12",
                      ow, oh);

        AVFilterInOut* outputs = avfilter_inout_alloc();
        AVFilterInOut* inputs  = avfilter_inout_alloc();
        if (!outputs || !inputs) {
            if (outputs) avfilter_inout_free(&outputs);
            if (inputs)  avfilter_inout_free(&inputs);
            return false;
        }
        outputs->name = av_strdup("in");  outputs->filter_ctx = src;  outputs->pad_idx = 0; outputs->next = nullptr;
        inputs->name  = av_strdup("out"); inputs->filter_ctx  = sink; inputs->pad_idx  = 0; inputs->next  = nullptr;

        int rc = avfilter_graph_parse_ptr(graph, desc, &inputs, &outputs, nullptr);
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
        if (rc < 0) return false;

        return avfilter_graph_config(graph, nullptr) >= 0;
    }

    // Push one hw frame, pull the downloaded NV12 host frame. Caller frees `out`.
    bool run(AVFrame* in, AVFrame* out) {
        if (av_buffersrc_add_frame_flags(src, in, AV_BUFFERSRC_FLAG_KEEP_REF) < 0)
            return false;
        const int rc = av_buffersink_get_frame(sink, out);
        return rc >= 0;
    }
};

#ifdef ZM_WITH_VAAPI_VPP
// TODO(zero-copy / lower-overhead VPP): drive VAAPI VPP directly through libva
// (path B above) instead of the libavfilter graph. Sketch:
//   * vaInitialize() on the VADisplay backing the decoder's AVHWDeviceContext.
//   * vaCreateConfig(VAProfileNone, VAEntrypointVideoProc) + vaCreateContext().
//   * Per frame: vaCreateSurfaces() for the dst, fill a VAProcPipelineParameterBuffer
//     (src VASurfaceID = (VASurfaceID)(uintptr_t)in->data[3], scaled output_region),
//     vaBeginPicture/vaRenderPicture/vaEndPicture, then either vaDeriveImage+vaMapBuffer
//     to read the small grid, OR export as a dmabuf (vaExportSurfaceHandle) for a
//     Vulkan/OpenCL import — the true zero-copy bridge.
// Guarded out by default because <va/va.h> is NOT installed on this box, so enabling
// it would break the -fsyntax-only check. Left as the documented native VPP seam.
static bool vpp_scale_raw_va(/* VADisplay, src surf, dst size */) { return false; }
#endif

// ---------------------------------------------------------------------------
// CPU mirror of cuda_motion_bbox_cpudiff: given a freshly downloaded small luma
// grid (gw x gh), diff vs the previous grid, count changed cells over `thr`, and
// return ONE merged bbox in grid space (caller scales back to source pixels).
// ---------------------------------------------------------------------------
struct GridMotion { bool active = false; int x0 = 0, y0 = 0, x1 = 0, y1 = 0, changed = 0; };

GridMotion grid_diff_bbox(const uint8_t* cur, int stride, int gw, int gh,
                          std::vector<uint8_t>& prev, int thr, int minCells) {
    GridMotion m;
    if (static_cast<int>(prev.size()) != gw * gh) {
        prev.assign(static_cast<size_t>(gw) * gh, 0);
        for (int j = 0; j < gh; ++j)
            for (int i = 0; i < gw; ++i)
                prev[static_cast<size_t>(j) * gw + i] = cur[static_cast<size_t>(j) * stride + i];
        return m;  // first frame: prime, report no motion
    }
    int minx = gw, miny = gh, maxx = -1, maxy = -1, changed = 0;
    for (int j = 0; j < gh; ++j) {
        for (int i = 0; i < gw; ++i) {
            const int idx = j * gw + i;
            const int c = cur[static_cast<size_t>(j) * stride + i];
            const int p = prev[idx];
            if (std::abs(c - p) > thr) {
                ++changed;
                minx = std::min(minx, i); miny = std::min(miny, j);
                maxx = std::max(maxx, i); maxy = std::max(maxy, j);
            }
            prev[idx] = static_cast<uint8_t>(c);
        }
    }
    m.changed = changed;
    if (changed >= minCells && maxx >= 0) {
        m.active = true; m.x0 = minx; m.y0 = miny; m.x1 = maxx; m.y1 = maxy;
    }
    return m;
}

// ---------------------------------------------------------------------------
class VaapiBackend : public HwBackend {
public:
    VaapiBackend() {
        env_ = Ort::Env(ORT_LOGGING_LEVEL_WARNING, "zm_vaapi");
    }
    ~VaapiBackend() override = default;

    const char* name() const override { return "vaapi"; }

    bool load_model(const std::string& path, int net) override {
        model_ = path; net_ = net;
        try {
            Ort::SessionOptions so;
            so.SetIntraOpNumThreads(1);
            so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            // CPU EP — works on every AMD/Intel box. See infer() for the OpenVINO seam.
            Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_CPU(so, /*use_arena=*/1));
            session_ = std::make_unique<Ort::Session>(env_, model_.c_str(), so);

            Ort::AllocatorWithDefaultOptions alloc;
            in_name_  = session_->GetInputNameAllocated(0, alloc).get();
            out_name_ = session_->GetOutputNameAllocated(0, alloc).get();
            return true;
        } catch (...) {
            session_.reset();
            return false;
        }
    }

    // av_frame_clone() refs the same VASurface, so it outlives the decoder's
    // call-scoped AVFrame and can cross a queue. release() drops the ref.
    Surface acquire(uint64_t av_frame) override {
        Surface s;
        AVFrame* src = reinterpret_cast<AVFrame*>(av_frame);
        if (!src) return s;
        AVFrame* held = av_frame_clone(src);
        if (!held) return s;
        s.owner   = held;
        s.hw_type = ZM_HW_VAAPI;
        s.pix_fmt = static_cast<uint32_t>(AV_PIX_FMT_VAAPI);
        s.width   = held->width;
        s.height  = held->height;
        // For VAAPI the surface id lives in data[3]; planes stay zero (on-GPU).
        s.native  = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(held->data[3]));
        return s;
    }

    void release(Surface& s) override {
        if (s.owner) { AVFrame* f = static_cast<AVFrame*>(s.owner); av_frame_free(&f); s.owner = nullptr; }
    }

    // Motion gate. Downsample luma to a small grid ON the GPU via scale_vaapi (VPP),
    // hwdownload ONLY that small grid, then frame-diff vs a prev grid — mirroring
    // cuda_motion_bbox_cpudiff (changed cells + ONE merged bbox).
    //
    // NOTE (full residency / gpudiff parity): the diff below runs on the host on a
    // few-KB grid. A Vulkan compute shader importing the scaled surface as a dmabuf
    // could do the diff on-device and return only a ~24B verdict (matching the CUDA
    // gpudiff path). Left as a TODO; the grid is tiny so host diff is cheap for now.
    std::vector<Region> motion(const Surface& s) override {
        AVFrame* in = static_cast<AVFrame*>(s.owner);
        if (!in || s.width <= 0 || s.height <= 0) return {};

        const int gw = std::max(1, s.width  / ds_);
        const int gh = std::max(1, s.height / ds_);
        if (minCells_ <= 0) minCells_ = std::max(8, gw * gh / 400);

        if (motionVpp_.in_w != s.width || motionVpp_.in_h != s.height ||
            motionVpp_.out_w != gw || motionVpp_.out_h != gh || !motionVpp_.graph) {
            if (!motionVpp_.build(in, gw, gh)) return {};
        }

        AVFrame* grid = av_frame_alloc();
        if (!grid) return {};
        if (!motionVpp_.run(in, grid)) { av_frame_free(&grid); return {}; }

        // grid is host NV12: data[0] = luma plane, linesize[0] = stride.
        GridMotion m = grid_diff_bbox(grid->data[0], grid->linesize[0], gw, gh,
                                      prevGrid_, thr_, minCells_);
        av_frame_free(&grid);
        if (!m.active) return {};

        // Map grid-cell bbox back to source pixels (inclusive cell -> +1).
        Region r;
        r.x = m.x0 * ds_;
        r.y = m.y0 * ds_;
        r.w = std::min((m.x1 + 1) * ds_, s.width)  - r.x;
        r.h = std::min((m.y1 + 1) * ds_, s.height) - r.y;
        return { r };
    }

    // Preprocess: VPP scale+CSC the surface to NxN for the model.
    //
    // ZERO-COPY TODO: a true zero-copy hand-off to ORT would export the scaled
    // VAAPI output surface as a dmabuf (vaExportSurfaceHandle) and import it into a
    // Vulkan or OpenCL buffer that ORT's EP reads directly. That dmabuf bridge is
    // not finished here, so we do the HONEST path: VPP scale on-GPU -> hwdownload to
    // host NV12 -> NV12->RGB24 -> letterbox into a CHW host tensor (reusing the pure
    // zm::detect helpers). Correct everywhere; just not residency-optimal.
    DeviceTensor preprocess(const Surface& s, Region /*crop*/) override {
        DeviceTensor t; t.net = net_;
        AVFrame* in = static_cast<AVFrame*>(s.owner);
        if (!in || s.width <= 0 || s.height <= 0) return t;

        // ASPECT-PRESERVING letterbox (matches the CPU/CUDA path). compute_letterbox
        // gives the scale + the centred pad; the *content* size is the source scaled
        // by that factor (<= net on each axis). We do the SCALE on the GPU (VPP) to
        // exactly the content size, so we never squash the image and never download
        // anything larger than the model input.
        //
        // TODO(crop): the Region crop (one merged motion bbox) is currently ignored
        // and we letterbox the whole frame — correct, but loses the ROI-cascade win.
        // Pushing the crop onto the VPP needs crop_vaapi (or scale_vaapi crop args);
        // left as a follow-up. Motion still GATES inference, so the cheap path holds.
        const zm::detect::Letterbox lb = zm::detect::compute_letterbox(s.width, s.height, net_);
        const int cw = std::max(1, static_cast<int>(std::lround(s.width  * lb.scale)));
        const int ch = std::max(1, static_cast<int>(std::lround(s.height * lb.scale)));

        if (preVpp_.in_w != s.width || preVpp_.in_h != s.height ||
            preVpp_.out_w != cw || preVpp_.out_h != ch || !preVpp_.graph) {
            if (!preVpp_.build(in, cw, ch)) return t;
        }
        AVFrame* nv12 = av_frame_alloc();
        if (!nv12) return t;
        if (!preVpp_.run(in, nv12)) { av_frame_free(&nv12); return t; }

        // NV12 -> RGB24 (host) on the already-scaled content (small), then place it
        // into the padded net x net CHW. A scale=1 letterbox = identity bilinear, so
        // letterbox_rgb_to_chw just pads + normalizes + reorders to CHW (no re-scale).
        std::vector<uint8_t> rgb;
        nv12_to_rgb24(nv12->data[0], nv12->linesize[0],
                      nv12->data[1], nv12->linesize[1],
                      nv12->width, nv12->height, rgb);
        const int rw = nv12->width, rh = nv12->height;
        av_frame_free(&nv12);

        chw_.assign(static_cast<size_t>(3) * net_ * net_, 114.0f / 255.0f);
        zm::detect::Letterbox place;
        place.net = net_; place.src_w = rw; place.src_h = rh;
        place.scale = 1.0f; place.pad_x = lb.pad_x; place.pad_y = lb.pad_y;
        zm::detect::letterbox_rgb_to_chw(rgb.data(), place, chw_.data());

        t.lb  = lb;            // REAL letterbox -> correct box un-projection in infer()
        t.ptr = chw_.data();   // host CHW (zero-copy-to-ORT is the dmabuf TODO below)
        return t;
    }

    // Inference via ORT. CPU EP wired in load_model().
    //
    // OPENVINO EP SEAM: on Intel (and AMD via the CPU/GPU OpenVINO plugin) the
    // realistic accelerated EP is OpenVINO. Enable by building ORT with
    // --use_openvino and replacing the CPU-EP append in load_model() with
    // OrtSessionOptionsAppendExecutionProvider_OpenVINO_V2(so, {{"device_type","GPU"}}).
    std::vector<Detection> infer(const DeviceTensor& t, float conf,
                                 const std::vector<int>& allow) override {
        if (!t.ptr || !session_) return {};
        try {
            Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            const int64_t shape[4] = {1, 3, net_, net_};
            Ort::Value input = Ort::Value::CreateTensor<float>(
                mem, static_cast<float*>(t.ptr),
                static_cast<size_t>(3) * net_ * net_, shape, 4);

            const char* in_names[]  = { in_name_.c_str() };
            const char* out_names[] = { out_name_.c_str() };
            auto outputs = session_->Run(Ort::RunOptions{nullptr},
                                         in_names, &input, 1, out_names, 1);
            if (outputs.empty()) return {};

            float* out = outputs[0].GetTensorMutableData<float>();
            auto info = outputs[0].GetTensorTypeAndShapeInfo();
            auto dims = info.GetShape();
            // Expect a YOLO26-style NMS-free [1 x num x 6] (or [num x 6]) tensor.
            int num = 0;
            if (dims.size() == 3) num = static_cast<int>(dims[1]);
            else if (dims.size() == 2) num = static_cast<int>(dims[0]);
            if (num <= 0) return {};

            return zm::detect::decode_nms_free(out, num, t.lb, conf, allow);
        } catch (...) {
            return {};
        }
    }

private:
    std::string model_;
    int net_ = 640;

    Ort::Env env_{ORT_LOGGING_LEVEL_WARNING, "zm_vaapi"};
    std::unique_ptr<Ort::Session> session_;
    std::string in_name_, out_name_;

    VppGraph motionVpp_;                 // surface -> small luma grid (VPP)
    VppGraph preVpp_;                    // surface -> net x net NV12 (VPP)
    std::vector<uint8_t> prevGrid_;      // per-instance motion state (host grid)
    std::vector<float>   chw_;           // host CHW model input (see preprocess TODO)

    int ds_ = 8, thr_ = 25, minCells_ = 0;
};

}  // namespace

// NOTE: the real make_backend() (the one that dispatches "cuda"/"vaapi"/...) is
// wired in the factory TU later; this file only provides the VAAPI implementation.
// We expose a narrow constructor the factory can call so we don't redefine the
// shared make_backend symbol here.
std::unique_ptr<HwBackend> make_vaapi_backend() {
    return std::make_unique<VaapiBackend>();
}

}  // namespace zm::hw

#endif  // ZM_WITH_VAAPI

// hw_backend_openvino.cpp — Intel HwBackend (VAAPI / oneVPL decode + OpenVINO inference).
//
// This is the Intel slot of the fused on-device pattern from hw_backend.hpp, the
// structural twin of hw_backend_cuda.cpp: hardware-decode to a device surface
// (Intel: a VAAPI VASurfaceID or a QSV/oneVPL mfxFrameSurface, carried in the
// AVFrame), a cheap on-GPU motion gate, an on-GPU preprocess (VPP scale + CSC),
// then inference — here through OpenVINO Runtime on the Intel iGPU / NPU / CPU.
//
// ── Native OpenVINO API vs. the ONNX Runtime OpenVINO Execution Provider ──────
// There are TWO ways to reach OpenVINO from this codebase:
//
//   (A) The NATIVE OpenVINO Runtime C++ API (ov::Core / ov::CompiledModel /
//       ov::InferRequest) — what THIS file implements. It is the higher-effort
//       path: a second inference runtime alongside the ORT session used by the
//       CPU/CUDA backends, its own tensor/binding types, and — crucially — its
//       own zero-copy interop (ov::intel_gpu::ocl remote tensors wrapping a
//       VASurface) if you want to keep the decoded frame on the GPU all the way
//       into inference. The payoff is the tightest VAAPI↔GPU↔inference fusion and
//       access to NPU + Intel-specific model caching/quantization.
//
//   (B) The ONNX Runtime OpenVINO Execution Provider (OrtOpenVINOProviderOptions
//       / `OpenVINO` EP). This is the LOWER-EFFORT alternative: you keep the
//       existing zm::detect::InferenceEngine / Ort::Session machinery the CUDA
//       backend already shares, and just append the OpenVINO EP at session
//       creation (device_type "GPU"/"NPU"/"CPU", precision FP16/FP32). The model
//       is the same ONNX, the pre/post-processing is identical, and OpenVINO
//       transparently compiles+runs the graph. The trade-off: ORT owns the
//       tensor handoff, so the OpenVINO EP path generally copies the input host
//       buffer in (no VASurface→remote-tensor zero-copy), and NPU/Intel-specific
//       knobs are exposed only through the EP option strings rather than the full
//       ov:: API. For most deployments the EP is the right first move; reach for
//       the native API (this file) only when the host copy is the bottleneck and
//       you need true VAAPI→GPU zero-copy or NPU features the EP doesn't surface.
//
// ── Status ────────────────────────────────────────────────────────────────────
// NOT BUILT / NOT VALIDATED here — no OpenVINO runtime, no Intel GPU, no oneVPL on
// this box. This is a faithful scaffold to compile and validate on Intel hardware.
// Every interop point that needs real-hardware verification is marked TODO. See
// the "VALIDATE ON INTEL HARDWARE" checklist at the bottom of the file.

#include "hw_backend.hpp"

#if defined(ZM_WITH_OPENVINO)

#include "detect_postprocess.hpp"   // zm::detect::Box / Letterbox / decode_nms_free / compute_letterbox
#include <algorithm>
#include <cstdlib>                  // getenv (ZM_OV_DEVICE override)
#include <cstring>
#include <mutex>
#include <stdexcept>

extern "C" {
#include <libavutil/frame.h>        // av_frame_clone / av_frame_free (surface lifetime)
#include <libavutil/pixfmt.h>       // AV_PIX_FMT_VAAPI / AV_PIX_FMT_QSV / AV_PIX_FMT_NV12
#include <libavutil/hwcontext.h>    // AVHWFramesContext (to reach the underlying VASurface)
}

// OpenVINO Runtime. Header layout is stable across 2022.x–2024.x.
#include <openvino/openvino.hpp>
// For true VASurface → GPU remote-tensor zero-copy you additionally need:
//   #include <openvino/runtime/intel_gpu/ocl/va.hpp>   // ov::intel_gpu::ocl::VAContext
// Left out of the default include set because it drags in VA/OpenCL headers; enable
// when wiring the zero-copy path in preprocess()/infer() (see TODO there).

// oneVPL / VAAPI VPP for the on-GPU downscale + CSC. We talk to these through the
// libavutil hwcontext rather than the raw VA/VPL APIs where possible, so the same
// code covers both AV_PIX_FMT_VAAPI (iHD) and AV_PIX_FMT_QSV (oneVPL/QSV) frames.
// TODO(intel): a raw VAAPI VPP (vaCreateContext + VPP pipeline) or oneVPL
// (MFXVideoVPP) path gives the most control over the on-GPU scale; for the scaffold
// we route VPP through FFmpeg's transfer/scale helpers and mark the fast path.

namespace zm::hw {
namespace {

// ── small helpers ──────────────────────────────────────────────────────────────

// Pick the OpenVINO target device. Default "GPU" (Intel iGPU/Arc via the clDNN/
// oneDNN GPU plugin); ZM_OV_DEVICE can force "NPU" or "CPU" (or "AUTO", "HETERO:GPU,CPU").
const char* ov_device() {
    static const char* dev = [] {
        const char* e = getenv("ZM_OV_DEVICE");
        return (e && *e) ? e : "GPU";
    }();
    return dev;
}

// Is this AVFrame an Intel hardware surface we can keep on-device?
inline bool is_intel_hw(const AVFrame* f) {
    return f && (f->format == AV_PIX_FMT_VAAPI || f->format == AV_PIX_FMT_QSV);
}

// ── OpenVINO model wrapper ──────────────────────────────────────────────────────
//
// One ov::Core per process; one compiled model per (path, net, device). Mirrors the
// role of zm::detect::InferenceEngine on the CUDA path, but kept private here so we
// don't entangle the shared ORT engine with the OV runtime.
class OvModel {
public:
    static OvModel& get(const std::string& path, int net) {
        static std::mutex mtx;
        static std::unique_ptr<OvModel> inst;
        std::lock_guard<std::mutex> lk(mtx);
        if (!inst || inst->path_ != path || inst->net_ != net)
            inst = std::unique_ptr<OvModel>(new OvModel(path, net));
        return *inst;
    }

    // Run a CHW float input [1,3,net,net] (host buffer) and decode to source-pixel boxes.
    std::vector<zm::detect::Box> infer(const float* chw, const zm::detect::Letterbox& lb,
                                       float conf, const std::vector<int>& allow) {
        // TODO(intel): for zero-copy, replace this host-tensor create with an
        // ov::RemoteTensor wrapping the VASurface produced by preprocess() (via
        // ov::intel_gpu::ocl::VAContext::create_tensor). Validate the layout/precision
        // the remote tensor reports matches the model's expected input.
        ov::InferRequest req = compiled_.create_infer_request();

        const ov::Shape in_shape{1, 3, static_cast<size_t>(net_), static_cast<size_t>(net_)};
        // The host CHW buffer is owned by the caller (thread-local in preprocess()),
        // and we consume it synchronously below, so wrapping it (no copy) is safe.
        ov::Tensor in(ov::element::f32, in_shape, const_cast<float*>(chw));
        req.set_input_tensor(in);

        req.infer();

        const ov::Tensor out = req.get_output_tensor();
        // Expect a YOLO26-style NMS-free output [1, num, 6] = (x1,y1,x2,y2,conf,cls).
        // TODO(intel): confirm the exported model is NMS-free with this exact layout;
        // some OV-optimized exports transpose to [1,6,num] or keep a raw [1,N,85]
        // YOLO head that still needs NMS. decode_nms_free assumes [num,6] row-major.
        const ov::Shape os = out.get_shape();
        int num = 0;
        if (os.size() == 3 && os[2] == 6) num = static_cast<int>(os[1]);
        else if (os.size() == 2 && os[1] == 6) num = static_cast<int>(os[0]);
        else {
            // Unknown layout — refuse rather than mis-decode.
            // TODO(intel): add a transpose/NMS branch here for the model you ship.
            return {};
        }
        const float* data = out.data<const float>();
        return zm::detect::decode_nms_free(data, num, lb, conf, allow);
    }

private:
    OvModel(const std::string& path, int net) : path_(path), net_(net) {
        // read_model on the ONNX directly — OpenVINO has a native ONNX front-end, so
        // no separate IR (.xml/.bin) conversion is required.
        std::shared_ptr<ov::Model> model = core_.read_model(path);
        // TODO(intel): if the model has a dynamic input, reshape to {1,3,net,net} here
        // (model->reshape(...)) so the GPU plugin can compile a static-shape kernel.
        ov::AnyMap cfg{
            // FP16 on the iGPU is the sweet spot; OV picks it by default on GPU but
            // we make it explicit. Drop/adjust for NPU or CPU as needed.
            {ov::hint::inference_precision.name(), ov::element::f16},
            // Cache compiled blobs so subsequent loads skip recompilation.
            // TODO(intel): point this at a writable, per-host cache dir.
            {ov::cache_dir.name(), std::string("/tmp/zm_ov_cache")},
        };
        compiled_ = core_.compile_model(model, ov_device(), cfg);
    }

    std::string path_;
    int net_;
    ov::Core core_;
    ov::CompiledModel compiled_;
};

// ── The Intel backend ───────────────────────────────────────────────────────────
class OpenvinoBackend : public HwBackend {
public:
    ~OpenvinoBackend() override = default;

    const char* name() const override { return "openvino"; }

    bool load_model(const std::string& path, int net) override {
        model_ = path; net_ = net;
        try { OvModel::get(model_, net_); return true; }   // compiles the model up-front
        catch (const std::exception&) { return false; }
        catch (...) { return false; }
    }

    // av_frame_clone() refs the same VAAPI/QSV surface, so it outlives the decoder's
    // call-scoped AVFrame (and can cross a StageRunner queue). release() drops the ref.
    Surface acquire(uint64_t av_frame) override {
        Surface s;
        AVFrame* src = reinterpret_cast<AVFrame*>(av_frame);
        if (!src) return s;
        AVFrame* held = av_frame_clone(src);
        if (!held) return s;

        s.owner = held;
        s.hw_type = 2;   // ZM_HW_VAAPI (covers both VAAPI and QSV here)
        s.width = held->width;
        s.height = held->height;
        s.pix_fmt = static_cast<uint32_t>(held->format);  // AV_PIX_FMT_VAAPI / _QSV / sw NV12

        if (is_intel_hw(held)) {
            // For a hardware surface, data[3] is the VASurfaceID (VAAPI) or the
            // mfxFrameSurface ptr (QSV); the AVFrame itself is the lifetime handle.
            // We do NOT fill plane_ptr (no host pointers) — motion()/preprocess()
            // run VPP on the surface. `native` carries the AVFrame for that VPP.
            s.native = reinterpret_cast<uint64_t>(held);
            // TODO(intel): expose the raw VASurfaceID in plane_ptr[3] if a downstream
            // raw-VAAPI VPP path wants it directly:
            //   s.plane_ptr[3] = (uint64_t)(uintptr_t)held->data[3];
        } else {
            // Software fallback (e.g. decoder gave us NV12 in system memory): treat
            // like the CPU/CUDA layout so motion/preprocess can read planes directly.
            for (int i = 0; i < 4; ++i) {
                s.plane_ptr[i] = reinterpret_cast<uint64_t>(held->data[i]);
                s.linesize[i] = held->linesize[i];
            }
            s.native = reinterpret_cast<uint64_t>(held);
        }
        return s;
    }

    void release(Surface& s) override {
        if (s.owner) { AVFrame* f = static_cast<AVFrame*>(s.owner); av_frame_free(&f); s.owner = nullptr; }
    }

    // Cheap luma-diff motion gate, mirroring cuda_motion_bbox_cpudiff: downscale the
    // frame to a small (sw x sh) luma grid, diff it against the previous frame's grid,
    // and return one merged bbox (in source pixels) over the changed cells.
    //
    // ON-DEVICE NOTE: the downscale SHOULD be a VAAPI/oneVPL VPP pass on the GPU so the
    // full frame never crosses to the host — only the tiny grid does (exactly the PCIe
    // story of the CUDA cpudiff path). The diff itself is currently on the host; an
    // OpenVINO/OpenCL kernel (or an ov:: preprocessing op) could move the diff
    // on-device too — see the OPENCL TODO below — but for a coarse grid the host diff
    // is negligible, so the scaffold keeps it on the CPU.
    std::vector<Region> motion(const Surface& s) override {
        const int sw = std::max(1, s.width / ds_);
        const int sh = std::max(1, s.height / ds_);
        if (minCells_ <= 0) minCells_ = std::max(8, sw * sh / 400);

        std::vector<uint8_t> grid(static_cast<size_t>(sw) * sh, 0);
        if (!downscale_luma_grid(s, sw, sh, grid)) return {};

        // First frame: seed prev grid, report no motion.
        if (prevGrid_.size() != grid.size()) { prevGrid_ = grid; return {}; }

        // Host diff over the small grid (connected bbox of changed cells).
        // TODO(intel/opencl): to keep the diff on-device, run a tiny OpenCL/oneAPI
        // kernel (or an ov::Model preprocessing graph) over two device-resident grids
        // and read back only a ~24-byte verdict, exactly like cuda_motion_bbox_gpudiff.
        int minx = sw, miny = sh, maxx = -1, maxy = -1, changed = 0;
        for (int gy = 0; gy < sh; ++gy) {
            for (int gx = 0; gx < sw; ++gx) {
                const int idx = gy * sw + gx;
                const int d = std::abs(int(grid[idx]) - int(prevGrid_[idx]));
                if (d >= thr_) {
                    ++changed;
                    minx = std::min(minx, gx); maxx = std::max(maxx, gx);
                    miny = std::min(miny, gy); maxy = std::max(maxy, gy);
                }
            }
        }
        prevGrid_ = std::move(grid);

        if (changed < minCells_ || maxx < 0) return {};

        // Grid cells -> source pixels (each cell spans ds_ px). Clamp to frame.
        Region r;
        r.x = std::clamp(minx * ds_, 0, s.width);
        r.y = std::clamp(miny * ds_, 0, s.height);
        const int x2 = std::clamp((maxx + 1) * ds_, 0, s.width);
        const int y2 = std::clamp((maxy + 1) * ds_, 0, s.height);
        r.w = std::max(0, x2 - r.x);
        r.h = std::max(0, y2 - r.y);
        if (r.w <= 0 || r.h <= 0) return {};
        return { r };
    }

    // Preprocess a (crop of a) surface into a model-input CHW tensor. On Intel this is
    // a VPP scale + colour-space convert (NV12 -> RGB) to net x net, letterboxed.
    // For the scaffold we produce a HOST CHW buffer that infer() wraps; the true
    // zero-copy path keeps it as a GPU surface / ov::RemoteTensor (see TODO).
    DeviceTensor preprocess(const Surface& s, Region crop) override {
        DeviceTensor t; t.net = net_;

        const int cx = crop.w > 0 ? crop.x : 0;
        const int cy = crop.h > 0 ? crop.y : 0;
        const int cw = crop.w > 0 ? crop.w : s.width;
        const int ch = crop.h > 0 ? crop.h : s.height;
        if (cw <= 0 || ch <= 0) return t;

        t.lb = zm::detect::compute_letterbox(cw, ch, net_);

        // Thread-local CHW scratch so the pointer stays valid until the next
        // preprocess() on this thread (matches the DeviceTensor contract).
        static thread_local std::vector<float> chw;
        chw.assign(static_cast<size_t>(3) * net_ * net_, 114.0f / 255.0f);

        // TODO(intel): replace this with a real VAAPI/oneVPL VPP pass:
        //   1. vaCreateSurfaces / oneVPL VPP target at net x net, RGBP/RGB24.
        //   2. VPP scale (the crop region) + CSC NV12->RGB on the GPU.
        //   3. Either map the result to a host RGB24 buffer and letterbox_rgb_to_chw()
        //      it here, OR (fast path) keep it as a GPU surface and build an
        //      ov::RemoteTensor in infer() — no host copy at all.
        // For now: transfer the (crop of the) NV12 surface to host and CPU-letterbox,
        // so the rest of the pipeline is exercisable without VPP wired up.
        if (!vpp_to_chw_host(s, cx, cy, cw, ch, t.lb, chw.data())) return t;  // t.ptr stays null

        t.ptr = chw.data();
        return t;
    }

    // Run inference on the preprocessed tensor via OpenVINO; boxes come back in
    // source pixels via the letterbox carried on the tensor.
    std::vector<Detection> infer(const DeviceTensor& t, float conf,
                                 const std::vector<int>& allow) override {
        if (!t.ptr) return {};
        // TODO(intel): when preprocess() yields a GPU surface, pass an ov::RemoteTensor
        // here instead of the host CHW pointer (see OvModel::infer TODO).
        return OvModel::get(model_, net_)
            .infer(static_cast<const float*>(t.ptr), t.lb, conf, allow);
    }

private:
    // Downscale the surface's luma to an (sw x sh) grid. Real impl: VAAPI/oneVPL VPP
    // scale to sw x sh, NV12, then read back just the Y plane (the grid). Scaffold:
    // transfer the hw surface to host NV12 once and box-average on the CPU.
    bool downscale_luma_grid(const Surface& s, int sw, int sh, std::vector<uint8_t>& grid) {
        // TODO(intel): VPP downscale on the GPU; read back only sw*sh bytes.
        std::vector<uint8_t> y; int ystride = 0;
        if (!fetch_luma_host(s, y, ystride)) return false;
        if (y.empty()) return false;
        // Box-average source luma into the grid (coarse; good enough for a motion gate).
        for (int gy = 0; gy < sh; ++gy) {
            const int sy0 = gy * ds_, sy1 = std::min(sy0 + ds_, s.height);
            for (int gx = 0; gx < sw; ++gx) {
                const int sx0 = gx * ds_, sx1 = std::min(sx0 + ds_, s.width);
                uint32_t acc = 0; int n = 0;
                for (int yy = sy0; yy < sy1; ++yy)
                    for (int xx = sx0; xx < sx1; ++xx) { acc += y[size_t(yy) * ystride + xx]; ++n; }
                grid[size_t(gy) * sw + gx] = n ? uint8_t(acc / n) : 0;
            }
        }
        return true;
    }

    // Fetch the full-res luma plane to host. For a hw surface this is an
    // av_hwframe_transfer_data() to a system-memory NV12 frame.
    bool fetch_luma_host(const Surface& s, std::vector<uint8_t>& y, int& ystride) {
        AVFrame* held = static_cast<AVFrame*>(s.owner);
        if (!held) return false;

        if (s.plane_ptr[0]) {
            // Software surface — luma is already in host memory.
            ystride = s.linesize[0];
            y.resize(size_t(ystride) * s.height);
            std::memcpy(y.data(), reinterpret_cast<const void*>(s.plane_ptr[0]), y.size());
            return true;
        }

        // Hardware surface: transfer to a system-memory frame.
        // TODO(intel): this is the SLOW path (full-frame readback). The whole point of
        // the on-device gate is to AVOID this — implement the VPP-downscale-on-GPU path
        // in downscale_luma_grid() so only the tiny grid is read back. Kept here so the
        // scaffold runs end-to-end before the VPP path is wired.
        AVFrame* sw = av_frame_alloc();
        if (!sw) return false;
        bool ok = false;
        if (av_hwframe_transfer_data(sw, held, 0) >= 0 && sw->data[0]) {
            ystride = sw->linesize[0];
            y.resize(size_t(ystride) * sw->height);
            std::memcpy(y.data(), sw->data[0], y.size());
            ok = true;
        }
        av_frame_free(&sw);
        return ok;
    }

    // VPP (or host-fallback) NV12 -> letterboxed CHW. Scaffold: transfer to host NV12,
    // convert the crop to RGB24, then letterbox_rgb_to_chw().
    bool vpp_to_chw_host(const Surface& s, int cx, int cy, int cw, int ch,
                         const zm::detect::Letterbox& lb, float* chw_out) {
        AVFrame* held = static_cast<AVFrame*>(s.owner);
        if (!held) return false;

        // Get host NV12 (planes 0 = Y, 1 = interleaved UV).
        const uint8_t* yp; const uint8_t* uvp; int ystride, uvstride;
        AVFrame* sw = nullptr;
        if (s.plane_ptr[0] && s.plane_ptr[1]) {
            yp  = reinterpret_cast<const uint8_t*>(s.plane_ptr[0]); ystride  = s.linesize[0];
            uvp = reinterpret_cast<const uint8_t*>(s.plane_ptr[1]); uvstride = s.linesize[1];
        } else {
            sw = av_frame_alloc();
            if (!sw) return false;
            if (av_hwframe_transfer_data(sw, held, 0) < 0 || !sw->data[0]) { av_frame_free(&sw); return false; }
            // Expect NV12 from the transfer (Intel default). TODO(intel): if the
            // transfer yields a different sw format, request NV12 on the hwframes ctx.
            yp  = sw->data[0]; ystride  = sw->linesize[0];
            uvp = sw->data[1]; uvstride = sw->linesize[1];
        }

        // Convert the crop NV12 -> RGB24 (BT.601, same convention as the CUDA kernel),
        // then reuse the pure CPU letterbox. This is the host-fallback CSC; the GPU
        // path does the CSC inside VPP and skips this entirely.
        std::vector<uint8_t> rgb(size_t(cw) * ch * 3);
        for (int j = 0; j < ch; ++j) {
            const int sy = std::clamp(cy + j, 0, s.height - 1);
            for (int i = 0; i < cw; ++i) {
                const int sx = std::clamp(cx + i, 0, s.width - 1);
                const int Y = yp[size_t(sy) * ystride + sx];
                const int uvIdx = (sy / 2) * uvstride + (sx / 2) * 2;
                const int U = uvp[uvIdx] - 128;
                const int V = uvp[uvIdx + 1] - 128;
                const int c = Y - 16;
                auto cl = [](int v) { return uint8_t(std::clamp(v, 0, 255)); };
                uint8_t* px = &rgb[(size_t(j) * cw + i) * 3];
                px[0] = cl((298 * c + 409 * V + 128) >> 8);             // R
                px[1] = cl((298 * c - 100 * U - 208 * V + 128) >> 8);   // G
                px[2] = cl((298 * c + 516 * U + 128) >> 8);             // B
            }
        }
        if (sw) av_frame_free(&sw);

        // lb was computed for (cw x ch); letterbox the crop into the CHW tensor.
        zm::detect::letterbox_rgb_to_chw(rgb.data(), lb, chw_out);
        return true;
    }

    std::string model_;
    int net_ = 640;
    std::vector<uint8_t> prevGrid_;            // per-instance motion state (host grid)
    int ds_ = 8, thr_ = 25, minCells_ = 0;     // downscale factor, per-cell diff thr, min changed cells
};

}  // namespace

// Narrow factory entry the shared make_backend() (hw_backend.cpp) dispatches to.
std::unique_ptr<HwBackend> make_openvino_backend() {
    return std::make_unique<OpenvinoBackend>();
}

}  // namespace zm::hw

#endif  // ZM_WITH_OPENVINO

// ── VALIDATE ON INTEL HARDWARE ──────────────────────────────────────────────────
// Build: add this TU + the OpenVINO include/lib dirs under an `if(ZM_WITH_OPENVINO)`
// block in plugins/detect_onnx/CMakeLists.txt (find_package(OpenVINO REQUIRED),
// target_link_libraries(... openvino::runtime), and compile_definitions
// ZM_WITH_OPENVINO). Note: the existing make_backend() in hw_backend_cuda.cpp also
// defines this symbol — only ONE backend TU can be compiled into the plugin at a
// time (or merge the factories) to avoid a duplicate-symbol link error.
//
// Then verify on an Intel box (iGPU/Arc + iHD driver, or NPU):
//  1. Decode path: FFmpeg actually delivers AV_PIX_FMT_VAAPI (vaapi hwaccel, iHD
//     driver) or AV_PIX_FMT_QSV (qsv) frames; av_frame_clone keeps the VASurface
//     alive across release(); no surface-pool exhaustion under load.
//  2. OvModel: ov::Core::read_model parses the exact ONNX you ship; compile_model
//     succeeds on "GPU" (and "NPU"/"CPU" via ZM_OV_DEVICE); the cache_dir is writable.
//  3. Output layout: the model is NMS-free [1,num,6]=(x1,y1,x2,y2,conf,cls). If it's
//     [1,6,num] or a raw YOLO head, add the transpose/NMS branch in OvModel::infer.
//  4. CSC correctness: BT.601 vs BT.709 — compare a few decoded boxes against the
//     CPU/CUDA path on the same frame; adjust coefficients if colours are off.
//  5. Motion gate: replace fetch_luma_host's full-frame readback with a real
//     VAAPI/oneVPL VPP downscale so only the sw*sh grid crosses to host; tune ds_/thr_.
//  6. Zero-copy (perf): wire ov::intel_gpu::ocl::VAContext so preprocess() yields a
//     GPU surface and infer() binds an ov::RemoteTensor — eliminates the host CHW copy.
//  7. Decide native-API (this file) vs the lower-effort ORT OpenVINO EP (see header
//     comment) for your deployment; the EP may be enough and reuses InferenceEngine.

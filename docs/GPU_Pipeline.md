# Zero-copy GPU pipeline (NVDEC → CUDA → ORT, gated)

Status (2026-06): **implemented behind `-DZM_WITH_CUDA=ON` (OFF by default).**
Built and compile-checked on macOS with the option OFF (so the default CPU build
is unaffected) and the FFmpeg decode side compile-checked with the portable API.
The CUDA-specific code (kernel, NPP-free fused preprocess, ORT CUDA IoBinding) has
**NOT been run** — validate on a Linux/NVIDIA box. See "Validate" below.

## What is implemented

- **ABI:** `zm_gpu_frame_t` (`core/include/zm_plugin.h`) describes a GPU surface —
  per-plane device pointers + pitches, dims, native pix_fmt, and the owning
  `AVFrame*` (valid for the synchronous on_frame call). When a frame's `hw_type`
  is a GPU type, the on_frame payload is this descriptor, not pixel bytes.
- **decode_ffmpeg:** `"hwaccel":"cuda"` config creates a CUDA `hw_device_ctx`,
  selects `AV_PIX_FMT_CUDA` via `get_format`, and emits the decoded NV12 surface
  as a `zm_gpu_frame_t` (no CPU download). Falls back to software decode if no
  CUDA device is available — so the same plugin runs everywhere. (FFmpeg must be
  built with `--enable-cuda --enable-nvdec`.)
- **detect_onnx:** with a model and `"ep":"cuda"`, a CUDA `hw_type` frame is run
  zero-copy: a fused CUDA kernel (`detect_cuda.cu`) samples the NV12 surface into a
  letterboxed, normalized CHW float tensor **on the device**, which is bound as the
  ORT input via `IoBinding` (CUDA memory — no host image readback); only the small
  output tensor returns to the CPU for NMS-free decode. Without `ZM_WITH_CUDA`, a
  CUDA frame is logged (throttled) and passed through.
- **Chain:** the synchronous stage-to-stage routing (see capture→decode→detect)
  bounds the surface lifetime to the on_frame call, so the descriptor's `AVFrame*`
  stays valid through detect without refcount juggling.

## Known caveats (validate / tune on GPU)

- The fused kernel uses **nearest-neighbour** sampling and **BT.601 limited-range**
  YUV→RGB. Fine for detection; revisit (bilinear / BT.709 / full-range) if accuracy
  needs it or the camera signals a different matrix.
- CPU consumers of a GPU frame (motion, describe_vlm) are **not** yet wired to a
  download-on-demand helper — today only `detect_onnx` consumes CUDA frames. Add an
  `av_hwframe_transfer_data` helper before mixing CPU stages after a GPU decode.
- The previous CPU reality still applies when `hwaccel` != `cuda`: software decode →
  `sws_scale` → CPU RGB/gray; `motion_pixel_diff` CPU; `ShmRing` CPU bytes.

## Build & validate (Linux/NVIDIA)

```
cmake -B build -DZM_WITH_CUDA=ON -DONNXRUNTIME_ROOT=/opt/onnxruntime-gpu ..
```
Requires the CUDA toolkit, FFmpeg with NVDEC, and an onnxruntime built with the
CUDA execution provider. Then run a pipeline with `decode_ffmpeg` cfg
`"hwaccel":"cuda"` feeding `detect_onnx` cfg `"ep":"cuda"`, and confirm detections
with `nvidia-smi` showing decode+compute on the GPU and no per-frame host copies.

## Goal

Decode on the GPU and keep frames there; run detection/motion on the surface;
**download to CPU only when a CPU consumer needs it** (CPU motion, VLM JPEG, or no
GPU present). The worker_link media path sends compressed packets, so it never
needs decoded frames — it's already free of decoded-frame copies.

## Design

- **HW decode in `decode_ffmpeg`:** set up `hw_device_ctx` per backend, implement
  the `get_format` callback to select the hw pixfmt, and keep the AVFrame in
  `hw_frames_ctx` (do **not** call `av_hwframe_transfer_data`). Emit a frame whose
  `hw_type` is the GPU type and whose `handle` references the surface.
- **In-process GPU fan-out (not ShmRing):** GPU surface handles are process-local
  and can't go through Boost.Interprocess shared memory. Keep surfaces in a GPU
  frame pool and fan out the *handle* in the header to in-process GPU consumers;
  the `ShmRing` stays the CPU transport. (This is a second, parallel path in
  `CaptureThread`/`PluginManager`, selected by `hw_type`.)
- **GPU consumers:**
  - Detection (embedded ORT): CUDA EP via `IoBinding` with a device pointer (zero
    readback); CoreML/Metal on Apple; map the FFmpeg surface to the EP's memory.
  - Motion: a GPU kernel (CUDA/Metal) or VAAPI/CL — later.
- **Download-on-demand:** a helper that materializes a CPU `RGB24`/`GRAYSCALE`
  buffer from a surface, called lazily only by CPU consumers.

## Per-backend

- macOS: VideoToolbox decode → `CVPixelBuffer`/`IOSurface` → Metal/CoreML.
- Linux NVIDIA: NVDEC → CUDA frames → ORT CUDA/TensorRT EP (CUDA interop).
- Linux Intel/AMD: VAAPI surfaces → VAAPI/OpenCL or download.

## Sequencing

Best done after the embedded detection tier lands (so there's a GPU consumer to
justify it), and it should be designed jointly with that plugin's `IoBinding`
input path. Until then, the CPU path is correct and portable; GPU is a throughput
optimization for many-camera / high-resolution installs.

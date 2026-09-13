# GPU pipeline: decode, motion and detection without leaving the GPU

Status (2026-09-13): **implemented and validated on NVIDIA (CUDA), Apple silicon (VideoToolbox +
Metal + CoreML/ANE) and AMD/Intel (VAAPI, Vulkan prototypes).** The fused stage is the
`decode_detect` plugin. The measured numbers below come from runs on this branch; the June
validation of the CUDA and VAAPI paths is recorded in the commit messages for `07c0790`,
`94cd302` and `docs/Mixed_GPU_Findings.md`.

## What runs where

`decode_detect` is a PROCESS plugin that dlopens `decode_ffmpeg` internally and, for every decoded
surface, runs a `HwBackend` (`plugins/detect_onnx/hw_backend.hpp`) in the same synchronous call:

    acquire(surface) -> motion(surface) -> preprocess(surface, region) -> infer(tensor) -> release

Because decode and detect share one call, the surface never crosses a `StageRunner` queue and never
touches CPU memory. What comes back to the host is a ~28-byte motion verdict (changed-cell count,
bbox, luma sum) and the detection tensor. With `roi_motion: true` the gate runs first and inference
only happens for frames with enough changed cells.

| Backend (`hw`) | Decode | Motion on GPU | Preprocess + inference | Build flag | Validated |
|---|---|---|---|---|---|
| `cuda` | NVDEC (`hwaccel: cuda`) | `gpudiff` kernel, previous grid stays device-resident | CUDA kernel -> ORT CUDA EP, IoBinding | `-DZM_WITH_CUDA=ON` | RTX 50-series. Motion diff byte-identical to the host diff (8/8). Default gate on CUDA. |
| `metal` | VideoToolbox (`hwaccel: videotoolbox`), CVPixelBuffer imported with `CVMetalTextureCache` | Metal downsample + diff kernels, ping-pong grids on device | Metal NV12->CHW letterbox -> ORT CoreML EP (Neural Engine) | `-DZM_WITH_METAL=ON` (Apple only) | M4 Pro. Bench: 0 mismatches vs CPU over 120 frames. Pipeline: this document. |
| `vaapi` | VAAPI | `scale_vaapi` VPP downsample | ORT | `-DZM_WITH_VAAPI=ON` | Ryzen iGPU, 196 detections in `bench/bench_vaapi.cpp`. Needs vcpkg FFmpeg with VAAPI. |
| `vulkan` | VAAPI -> dma_buf import | Vulkan compute, 0 mismatches, 0.13 ms at 720p | Vulkan preprocess -> ncnn-Vulkan | `-DZM_WITH_VULKAN=ON` | Prototypes in `bench/vk/`; backend compiles, not run end to end in a pipeline. |
| `openvino` | -- | -- | OpenVINO EP | `-DZM_WITH_OPENVINO=ON` | Compiles. Never run. |

`hw: "auto"` (the default) tries `metal` on Apple and `cuda, vaapi, vulkan, openvino` elsewhere until
one is compiled in and available, then derives the inner decoder's `hwaccel` from the choice.

## Measured: the Apple motion gate in a real pipeline

Run on 2026-09-13, M4 Pro, `capture_file -> decode_detect(hw: auto)`, YOLO26n fp16, 4K 25 fps
H.264 night-street CCTV clip, 30 s realtime, socket consumer attached. Counters are the plugin's own
(`decode_detect stats` log lines, every 250 frames and on stop).

| | `roi_motion: true` | `roi_motion: false` |
|---|---|---|
| Frames decoded | 726 | 723 |
| Frames dropped by the gate | 563 (78%) | 0 |
| Inferences | 163 | 723 |
| Raw detections | 1024 | 4284 |
| `zm-core` CPU at 30 s | **5.6%** | 33.5% |
| RSS at 30 s | 157 MB | 155 MB |

The gate's inference count follows the clip's real motion. ffmpeg's own per-frame luma difference
(`signalstats` YDIF, mean per 50-frame block) peaks at frames 150-350 (0.6-1.2) and flatlines at 0.04
from frame 400 on. The gate ran 101 inferences in frames 1-250, 62 in 251-500 and 0 in 501-726.
Parked cars are still there in every frame (the ungated run finds ~6 per frame) but nothing moves, so
nothing is inferred.

Static control: a 12 s clip made by looping one frame of the same footage (`ffmpeg -loop 1`) gave
287 frames, 286 gated, 1 inference, 7 detections. The one inference is the first frame; after that the
previous-grid comparison holds.

Repro from `build/`:

    ./zm-core --pipeline ../pipelines/apple_metal_detect.json --socket /tmp/zm.sock --monitor-id 1
    ./wl_dump /tmp/zm.sock 27      # EVENT code=0x301 per frame with detections

Copy `pipelines/apple_metal_detect.template.json` to a `.json` with real paths. `wl_dump` prints the
first 160 characters of each event's JSON, so count detections from the plugin stats, not from its
output.

## Apple specifics

- VideoToolbox emits `420v` NV12, video range, so the kernels use BT.601 `1.164*(Y-16)`.
- The ANE only helps with an **fp16** model. fp32 falls back to about CPU speed (13.9 ms vs 1.95 ms
  for yolo26n at 640). Export with `half=True`. `ZM_COREML_UNITS` overrides the compute-unit choice
  (`ALL`, `CPUAndNeuralEngine`, `CPUAndGPU`, `CPUOnly`).
- Metal kernels compile at runtime from source; no offline Metal toolchain is needed.
- `decode_detect` dlopens `plugins/decode_ffmpeg/decode_ffmpeg.dylib` from disk. After changing
  `decode_ffmpeg`, rebuild its dylib or the stale one silently downloads VTB frames to CPU and
  `on_decoded` sees no GPU surface (0 detections, no error).

## Known gaps

- **GPU motion exists only inside `decode_detect`.** `motion_gate`, `motion_pixel_diff` and `zones`
  are CPU plugins that never see a GPU frame. There is no standalone GPU motion stage.
- **GPU surfaces don't leave `decode_detect`.** Everything after it in the pipeline is CPU.
  A standalone `decode_ffmpeg` with `hwaccel` decodes on the GPU and downloads each frame to CPU
  (`av_hwframe_transfer_data`), so CPU plugins after it work. Until 2026-09-13 it emitted GPU
  descriptors instead: CPU children silently got no pixels (on the M4 Pro, detect_onnx after a
  VideoToolbox decode produced 0 detection events vs 123 in software), and a GPU child read an
  `AVFrame` already freed by the time it left the queue. `gpu_output: true` restores descriptors
  and is only for a synchronous consumer. Sharing one GPU surface across several plugins would need
  refcounted surfaces through the queues (`av_frame_clone` + release on consume); not built.
- **Zones are not applied on the GPU path.** The gate works on a whole-frame grid. Zone geometry
  only applies through `analytics_rules` on tracker output.
- **Gate tunables** live under `motion` in the `decode_detect` config and apply to every backend
  (`zm::hw::MotionParams`): `downsample` (cell size in px, default 8), `pixel_threshold` (per-cell
  luma diff, default 25, Vulkan 18), `min_cells` (default `max(8, cells/400)`), `luma_jump`
  (exposure-change suppression, default off; Metal only for now), `max_regions` (CUDA multi-region
  path, default 8). Omit a key to keep the backend default. CUDA's multi-region path is still
  selected by `ZM_MOTION_REGIONS=1` in the environment, not by config.
- **Don't mix two GPU stacks in one process.** Running the AMD VAAPI/Vulkan stack and ORT-CUDA in
  the same process slows CUDA inference from 1.8 ms to 30 ms. Splitting decode and inference across
  GPUs only pays with separate processes. See `docs/Mixed_GPU_Findings.md`.
- **Per-monitor sessions don't scale past ~8 cameras on a 16 GB card.** Each worker owns a CUDA
  context and ORT session (~1.2-1.5 GB host, ~0.7 GB VRAM). The per-GPU `zm-infer` daemon design is
  in `zm-api/docs/ZMNEXT_SHARED_INFERENCE_PLAN.md`; not built.
- OpenVINO and the Vulkan backend have not been run in a pipeline.

## ABI

`zm_gpu_frame_t` (`core/include/zm_plugin.h`) describes a GPU surface: per-plane device pointers and
pitches, dims, native pix_fmt, and the owning `AVFrame*`, valid for the synchronous `on_frame` call.
When a frame's `hw_type` is `ZM_HW_CUDA` or `ZM_HW_VTB`, the `on_frame` payload is this descriptor,
not pixel bytes. `decode_ffmpeg` emits it only with `gpu_output: true` and `hwaccel` `cuda` or
`videotoolbox`; the `AVFrame` is unref'd when that `on_frame` returns, so only a consumer that
acquires it inside the call (`decode_detect`'s `on_decoded` → `HwBackend::acquire`) may use it.
`StageRunner` queues copy the descriptor bytes, not the surface, so a descriptor must never cross a
stage boundary. GPU surface handles are process-local and cannot cross the `ShmRing`; the ring stays
the CPU transport.

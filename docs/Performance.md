# Performance measurements

Every number here was measured, with the date, commit, hardware and where the method lives. Numbers
before 2026-06-27 predate the capture packet-leak fix (committed as `b1c635a`), and everything before
2026-09-13 predates the `decode_ffmpeg` hardware-frame fix (`4cbae2f`); treat those as indicative
and re-run before quoting them.

**Not measured yet:** zm-next against ZoneMinder's `zmc`/`zma` (memory or CPU), and any CPU-only
Linux pipeline. See "Wanted" at the end.

Hardware used below:

| Name | Hardware |
|---|---|
| CUDA box | Linux, NVIDIA RTX 5070 Ti, AMD Ryzen with iGPU |
| M4 Pro | Apple MacBook Pro, M4 Pro, macOS |
| zmdev | Ubuntu 24.04 VM, 4 vCPU, 15 GB, no GPU (192.168.0.45) |

## Detection

### Shared batched engine vs one inference session per camera

2026-06-20, `26feeae`, CUDA box. Method: `bench/bench_engine.cpp` (`--per-thread` models the
per-camera session), `bench/bench_engine_resources.py` samples CPU/GPU/VRAM/RAM.

| Cameras | Per-camera sessions | Shared engine |
|---|---|---|
| 8 | CPU 241%, VRAM 1,827 MB, RAM 1,601 MB | CPU ~30%, RAM ~1.1 GB (flat) |
| 1 → 8 | CPU 64% → 241%, VRAM 432 → 1,827 MB | flat |

Throughput is the same (~1,250 inferences/s at 8 cameras). Per-camera sessions only win at 1–2
cameras, where the batch linger adds latency. These are threads in one process sharing a CUDA
context; separate worker processes per camera widen the gap.

### Preprocess: NV12 → 640² letterboxed tensor

2026-06-20, `51396ee`, CUDA box. Method: `bench/bench_preprocess.cpp`.

| Path | Time per frame |
|---|---|
| CUDA kernel | ~0.005 ms |
| CPU bilinear, one core | ~2.1 ms |

The CPU cost is set by the 640² output, so it is the same for 4K and 720p sources.

### Motion-gated and ROI-crop detection

2026-06-20, `d8dad5e`, live 4K camera. Method: `bench/bench_roi_cascade.cpp`.

- The motion gate skipped ~64% of inferences with no real events lost (idle frames are static).
- ROI crops found 26 small or distant detections the full-frame 4K → 640 downscale missed, and lost
  10 static objects outside the motion region.

### GPU zero-copy cascade

2026-06-20, `2e9460c` / `452a784`, CUDA box, live 4K camera. Method: `bench/bench_gpu_roi.cpp`.

| Step | CPU path | GPU path |
|---|---|---|
| Inference including preprocess | ~9.4 ms | ~3.0 ms |
| Motion check | — | ~0.4 ms per frame |

### Single GPU, whole pipeline

2026-06-21, `1c25af5`, CUDA box (5070 Ti only). Method and discussion: `docs/Mixed_GPU_Findings.md`.

| Source | NVDEC decode | CUDA YOLO | Full pipeline | GPU util |
|---|---|---|---|---|
| 4K | 505 fps | 1.8 ms | 413 fps | 1–3% |
| 720p | 2,065 fps | 1.81 ms | 404 fps | 1–3% |

Detection-bound at ~400 fps either way. A shared batch-1 session saturates at ~540 inferences/s.

### Mixed GPUs: AMD iGPU front half, NVIDIA inference

2026-06-21, `1c25af5`, CUDA box. `docs/Mixed_GPU_Findings.md`.

| Source | iGPU motion | iGPU preprocess | Front-half throughput |
|---|---|---|---|
| 4K | 1.89 ms | 0.84 ms | 261 fps |
| 720p | 0.13 ms | 0.43 ms | 865 fps |

In one process, sharing it with the AMD VAAPI/Vulkan stack slows ORT-CUDA YOLO from 1.8 ms to 30 ms
(31 fps end to end vs 404 single-GPU). With separate processes the split reaches ~553 fps at 720p.

### Apple Neural Engine inference

2026-06-21, `1ec1383`, M4 Pro. YOLO26n at 640, median of 30. Method: `bench/metal/metal_coreml_infer.mm`.

| Model | Neural Engine | GPU | CPU |
|---|---|---|---|
| fp16 | **1.95 ms** | 4.8 ms | 16.3 ms |
| fp32 | 13.9 ms (falls back) | 4.3 ms | 13.8 ms |

The Neural Engine only helps with an fp16 export.

### Apple GPU motion gate in a real pipeline

2026-09-13, `ec20e88`, M4 Pro. 4K 25 fps night-street clip, 30 s, `capture_file → decode_detect`
(VideoToolbox → Metal → CoreML). Details: `docs/GPU_Pipeline.md`.

| | Gate on | Gate off |
|---|---|---|
| Frames dropped by the gate | 563 / 726 (78%) | 0 |
| Inferences | 163 | 723 |
| zm-core CPU | 5.6% | 33.5% |

## Memory

### Worker memory with a CUDA session

2026-06-27, CUDA box, 4 cameras. Source: zm-api `docs/ZMNEXT_SHARED_INFERENCE_PLAN.md`.

- ~1.2–1.5 GB host RSS and ~0.7 GB VRAM per camera, mostly the ORT/CUDA arena and libraries.
- Before the packet-leak fix: RSS climbed ~40 MB/min per 4K camera without bound.
- After the fix: flat. The two 4K workers plateaued at ~1.38 GB; 4 cameras ~5.1 GB total.
- The shared memory ring is a fixed 122 MB per worker.

### zm-core with Metal + CoreML detection

2026-09-13, `ec20e88`, M4 Pro, 4K clip through `decode_detect` (VideoToolbox decode, Metal motion, YOLO26n on CoreML): ~155–157 MB RSS at 30 s, gate on or off. macOS RSS; not comparable with Linux numbers.

## Reliability

### Camera authentication backoff

2026-09-14, `13d706a`, M4 Pro, mediamtx camera with a wrong password.

| | Before | After |
|---|---|---|
| Login attempts in 126 s | one every 1–30 s | 2 (62 s apart, logged by the camera) |
| Stop during a retry wait | up to the wait (30 s) | 0.2 s |

## Wanted

| Measurement | Why | Method |
|---|---|---|
| zmc vs zm-core memory and CPU, same cameras | zm-next replaces zmc per monitor; it must not cost more | `bench/memory/` (live-camera version needs a root step on zmdev) |
| CPU-only Linux pipeline (decode, motion, record; + CPU YOLO) | zmdev and many ZoneMinder installs have no GPU | `bench/memory/` plus CPU sampling |
| Re-run of the June CUDA numbers | Before the leak and decode fixes | the bench scripts above, on the CUDA box |
| Shared ring size vs stream resolution | The fixed 122 MB ring may dominate at low resolutions | per-stream ring sizing, then `bench/memory/` |

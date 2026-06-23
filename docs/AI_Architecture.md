# AI Architecture (Phase 3)

This document captures the state-of-the-art survey (mid-2026) behind zm-next's AI
plugins and the recommended, tiered build plan. The goal is the capability that
actually differentiates a next-gen ZoneMinder core from every existing NVR:
moving from "pixels changed" (motion) to **"a person is at the door"** (detection)
to **"someone left a package and walked away"** (scene understanding).

The detection metadata contract rides the canonical worker stream-socket EVENT
channel (event code `0x0301` detection, JSON detail
`{objects:[{label,confidence,x,y,w,h,track_id,zone_id}], frame_pts_us}`), so AI results ride the same per-monitor worker
socket as media and lifecycle events — no new transport.

## Runtime decision: ONNX Runtime

**We use ONNX Runtime (C++ API) as the single inference runtime, with selectable
execution providers (EPs).** Rationale from the survey:

- ORT is the portability champion: one model + one code path runs on **CPU**
  (baseline), **CoreML** (macOS dev / Apple Silicon), **CUDA / TensorRT** (Linux
  GPU prod), plus DirectML / OpenVINO / QNN / NNAPI. Hardware acceleration is a
  config switch, not a rewrite — which exactly matches zm-next's cross-platform
  plugin model and the `hw_type` abstraction already in the frame ABI.
- TensorRT is faster on NVIDIA but is vendor lock-in; the right way to get it is
  ORT's **TensorRT EP**, keeping one codebase.
- So: **yes, we need ONNX** — it is the abstraction layer that lets one detector
  plugin run everywhere. Each plugin picks an EP from config and falls back to CPU.

Install: `brew install onnxruntime` (1.26.x; CPU EP on the brew bottle). CoreML/
CUDA/TensorRT EPs come from a platform build or vendor package; the plugin
requests an EP and degrades gracefully to CPU if it is unavailable.

## Model survey (mid-2026)

Real-time detectors (COCO AP / edge-friendliness):
- **YOLO26** (Ultralytics) — NMS-free, end-to-end, DFL removed. Exports a clean
  single output `[1, 300, 6]` (xyxy, conf, class_id); **postprocessing is just a
  confidence threshold** — no NMS, no anchor decode. Best deployment story for a
  C++ plugin, strong on edge/CPU. **Primary Tier-1 model.**
- **YOLO11 / classic anchor-free** — output `[1, 84, 8400]`; needs transpose +
  NMS. Supported as a secondary decode path for existing model zoos.
- **RT-DETRv4 / D-FINE / DEIMv2** — highest AP, transformer-heavy, less CPU/edge
  friendly. Optional high-accuracy EP on GPU boxes.

Open-vocabulary (detect classes named in plain English, **no retraining**):
- **YOLOE** ("real-time seeing anything": text / visual / prompt-free) and
  **YOLO-World** — real-time, edge-deployable. This is a UX revolution for
  surveillance: the user types "delivery van", "ladder against the wall", "person
  with a package" and gets detections, instead of being limited to 80 COCO classes.
- Grounding DINO / OWLv2 / DINO-X — higher accuracy, transformer-heavy, not
  real-time; reserve for offline/forensic use.

Video / scene understanding ("what is happening on screen") — small VLMs:
- **Qwen2.5-VL (7B)** — long-horizon video analysis, second-level event
  localization over multi-hour video; explicitly targets surveillance. ~6 GB,
  local-capable on a decent GPU.
- **SmolVLM (2B)** / **Moondream (~2B)** — tiny, edge-deployable scene description
  (Jetson / Pi / small VRAM). Great for "describe this clip" on a trigger.

## Tiered build plan

Each tier is a self-contained DETECT/PROCESS plugin emitting the existing
detection/event schema. Build order is foundation → differentiation.

### Tier 1 — `detect_onnx` (object detection)  ← building now
The foundation: shared ORT integration + the bread-and-butter "person / car /
animal / package" detection that replaces dumb motion as the alarm trigger.
- Input: decoded frames (`ZM_FRAME_RGB24`) from `decode_ffmpeg`.
- Pipeline: letterbox resize → NCHW float → ORT inference → decode `[1,N,6]`
  (NMS-free) → confidence + class filter → emit `detection` events.
- Reuse: the `zones` plugin (Boost.Geometry R-tree) to gate/label detections by
  zone (`zone_id`), so "person in driveway" is distinguishable from "person on
  street". Pair with `motion_pixel_diff` as a cheap pre-filter (only run
  inference when motion is present) to save compute.

### Tier 2 — `detect_openvocab` (YOLOE / YOLO-World)
User-defined classes by text prompt, no training. The big UX differentiator.
Same plugin shape as Tier 1; adds a text-prompt config and the embedding head.

### Tier 3 — `describe_vlm` (scene understanding)  ← the revolution
A small VLM (Moondream / SmolVLM, or Qwen2.5-VL on GPU) triggered **on events**
(not per-frame) to produce natural-language descriptions and answer questions
about a clip: "what is happening", "is anyone holding a weapon", "did a package
get left". Output becomes searchable event metadata → "show me when someone
approached the door with a package". This is what no existing open NVR does well.
- Runs occasionally (on motion/detection trigger or N-second cadence), so cost is
  bounded even with a larger model.
- Emits a new `Event` variant (description / Q&A) carried on the worker socket.

## Compute-budget strategy (cascade)  ← `motion_gate` implemented

Run cheap stages first, expensive stages only when warranted:
`motion_gate` (always, ~free) → `detect_onnx` (only on motion) → `describe_vlm`
(on a confirmed detection / alarm). This keeps a multi-camera install affordable
while still delivering scene understanding where it matters.

**`motion_gate`** (built) is the lightweight pre-filter: a PROCESS plugin that
diffs downsampled luma and, with `"gate":true` (default), only forwards frames
downstream while motion is active plus a cooldown — so a static scene never
reaches YOLO. Pipeline shape: `decode_ffmpeg (rgb24) → motion_gate → detect_onnx`.
It accepts RGB24/GRAY/YUV420P (computes luma cheaply), passes GPU-surface and
compressed frames through untouched, and publishes `motion` events. Cfg keys:
`frame_width`/`frame_height`, `downscale` (4), `pixel_threshold` (20),
`min_changed_pixels` (50), `cooldown_frames` (15), `gate` (true), `stream_filter`.
It is distinct from `motion_pixel_diff`, which remains the heavier zones+blobs
analyzer; use `motion_gate` purely to throttle inference.

## Downstream (zm-api) — documented, not built here

zm-next stays a worker; these are tasks for the Rust `zm-api` repo (recorded here
so they aren't lost — **not** to be implemented from this project):
- Extend the binary stream-socket consumer (`src/streaming/source/protocol.rs`)
  to handle EVENT (0x06) — detection (0x0301) / description (0x0302) — and ingest
  them into the event model.
- Persist detection metadata (labels, bboxes, track ids, zone ids) and VLM
  descriptions as queryable event fields; expose natural-language event search in
  the API → dashboard / mobile / zmNinjaNg.
- Model/asset management: ship/download ONNX models, expose per-monitor AI config
  (model, EP, classes/prompts, thresholds) that the daemon renders into the
  pipeline JSON it pushes to each worker.
- **Camera PTZ control** — issue ONVIF PTZ SOAP (ContinuousMove / Stop /
  GotoPreset) directly to the camera's ONVIF service endpoint. This is a
  control-plane action with NO media-pipeline involvement (no frames flow through
  it) and ONVIF PTZ is a separate service from the RTSP stream, so the API (which
  holds camera IP + credentials) talks to the camera directly — it must NOT be a
  zm-next pipeline plugin. (Contrast two-way audio, which rides the camera's RTSP
  backchannel and therefore does need the worker — see docs/Two_Way_Audio.md.)

## Running the plugins (implemented)

Both AI plugins are built and unit-tested (`DetectOnnxTest`, `DescribeVlmTest`).

### detect_onnx (object detection)
Sits after `decode_ffmpeg` (consumes `ZM_FRAME_RGB24`). Emits one event per frame
with >=1 detection:
`{"type":"detection","stream_id":N,"pts_usec":…,"detections":[{"label","confidence","bbox":[x,y,w,h],"class_id"}]}`
which `WorkerLink` maps to `Event.DETECTION`. Pipeline node cfg keys: `model_path`
(YOLO26 ONNX; empty → pass-through), `input_size` (640), `conf_threshold` (0.25),
`frame_width`/`frame_height` (decoder output dims), `ep` ("cpu"|"coreml"),
`class_filter` (ids), `class_names` (default COCO-80), `stream_filter`. Export a
model with `yolo export model=yolo26n.pt format=onnx` (NMS-free `[1,300,6]`).

### describe_vlm (scene understanding)
Pass-through PROCESS node; on a cadence it JPEG-encodes the latest frame and POSTs
to an OpenAI-compatible VLM server, publishing
`{"type":"description","text":…,"prompt":…,"model":…}` → `Event.DESCRIPTION`.
Cfg keys: `server_url` (default `http://localhost:8080/v1/chat/completions`),
`model` (default `moondream`), `prompt`, `interval_sec` (10), `frame_width`/
`frame_height`, `stream_filter`. The plugin is a thin client — run the shared VLM
server separately, e.g.:
- **llama.cpp:** `llama-server -hf ggml-org/SmolVLM-Instruct-GGUF --port 8080` (or
  a Qwen2.5-VL / Moondream GGUF + its `--mmproj`).
- **Ollama:** `ollama serve` then `ollama pull moondream`; point `server_url` at
  `http://localhost:11434/v1/chat/completions`.
One server is shared by all monitors; only `describe_vlm` nodes call it, on their
interval, so cost stays bounded.

## Sources

- Best object detection models 2026 — https://www.ultralytics.com/blog/the-best-object-detection-models-of-2025
- YOLO26 NMS-free / docs — https://docs.ultralytics.com/models/yolo26 ; https://learnopencv.com/yolo26-nms-free-inference/
- YOLO26 ONNX export — https://docs.ultralytics.com/integrations/onnx
- RT-DETRv4 — https://arxiv.org/pdf/2510.25257 ; D-FINE — https://arxiv.org/pdf/2410.13842
- ML inference runtimes 2026 — https://medium.com/@digvijay17july/ml-inference-runtimes-in-2026-an-architects-guide-to-choosing-the-right-engine-d3989a87d052
- ONNX Runtime EPs / CoreML — https://onnxruntime.ai/docs/execution-providers/CoreML-ExecutionProvider.html ; https://onnxruntime.ai/docs/build/eps.html
- YOLOE open-vocab — https://learnopencv.com/yoloe-tutorial-real-time-open-vocabulary-detection/ ; YOLO-World — https://arxiv.org/abs/2401.17270
- SmolVLM — https://arxiv.org/pdf/2504.05299 ; Qwen2.5-VL — https://www.emergentmind.com/topics/qwen2-5-vl

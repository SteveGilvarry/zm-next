# Research & Design: Motion Gating + LLM Event Review

**Status:** Design / Build Plan (decisive)
**Audience:** zm-next core + plugin authors
**Scope:** Two workstreams — (1) the cheap motion gate, (2) a local-first, pluggable LLM event-review + natural-language query subsystem.

---

## TL;DR

1. **Motion gate — KEEP the GPU downsampled-luma frame-diff. Do not replace it.** The gate's job is *cheap, high-recall wakeup*, not precision; the downstream YOLO + tracker is the precision stage and is explicitly designed to reject the gate's false positives. A gate false-positive costs one inference; a false-negative is unrecoverable. MOG2/ViBe/SuBSENSE/optical-flow/learned change-nets buy precision we already get downstream, at real cost, and still fail on the cases that actually hurt (headlights, auto-exposure, swaying trees). **Invest in tuning the existing `cuda_motion_regions` path** (more downsample, larger min-blob, N-of-M temporal persistence, static masks, global-luma-jump suppression) and prove it with `bench_gpu_roi` + `bench_resources`. The *one* optional add-on worth prototyping is a **cheap optical-flow / motion-coherence confirmation stage** (NVIDIA HW flow on Blackwell, ~2-3 ms) gated behind diff hits, for treed/weather-exposed scenes only.

2. **LLM event review — build it, and trigger it RIGHT.** Add a new **`llm_event_review`** PROCESS plugin that fires on `alert` / `tracked_detection` EventBus events (NOT a dumb timer like today's `describe_vlm`), snaps the relevant frame(s) for the track, and calls a **local Qwen3-VL-8B-Instruct-FP8 on vLLM** (fallback: GGUF Q4_K_M on `llama-server`) over the OpenAI-compatible `/v1/chat/completions` shape. Put a **pluggable provider interface** (`IVisionProvider`) behind it so Claude / OpenAI / Gemini are drop-in cloud providers with key handling and **cloud→local fallback**. Store every description in a single **SQLite + `sqlite-vec`** store and answer natural-language questions with a **local Qwen function-calling router** that routes counting to SQL and "what/when" to **hybrid RAG (SQL pre-filter + embeddings)**, grounded by verifiable `event_id`s. This is the right half of `ha-llmvision` (providers, timeline, NL query, importance tiering) bolted onto zm-next's superior in-pipeline `decode→motion→detect→track` trigger model.

---

## Phased Roadmap (what to build first, and why)

| Phase | Workstream | Deliverable | Why this order |
|---|---|---|---|
| **0** | Motion | A/B harness + tuning recipe for the existing `cuda_motion_regions` gate (downsample, min-blob, N-of-M persistence, masks, global-luma suppression). Capture baseline recall/FP-wake/GPU-cost numbers with `bench_gpu_roi` + `bench_resources`. | Cheapest, highest-leverage. Settles "is a heavier method worth it?" with data before we touch any model code. Establishes the benchmark baseline everything else is measured against. |
| **1** | LLM | `llm_event_review` plugin (MVP): subscribe to `alert`+`tracked_detection`, **snap latest frame** (Approach A), call **local Qwen3-VL via `IVisionProvider`/OpenAI-compat local provider only**, publish `"reasoning"` event. Add a `reasoning` EVENT code (0x0305) to the stream-socket protocol. | Replaces the broken timer trigger with a track-gated one immediately, using infra that already exists (`describe_vlm`'s HTTP client + frame-snap pattern). Local-only first = no key/privacy surface to get wrong. |
| **2** | LLM | **Provider abstraction hardening**: `OpenAICompatProvider` / `AnthropicProvider` / `GeminiProvider` adapters + `Retry`/`RateLimit`/`Fallback` decorators + secret-ref key handling + per-camera local-only routing. Cloud→local fallback wired. | Adds cloud quality + resilience once the local path is proven. Decorator composition keeps adapters dumb. |
| **3** | LLM | **NL query subsystem**: capability-detected vector store (MariaDB-native `VECTOR` 11.8+ → `sqlite-vec` fallback, feature-flagged; see *Vector store*), embed-at-ingest (`bge-small`/`nomic`/`Qwen3-Embedding-0.6B`), Qwen function-calling router with `count_events()` (SQL) + `search_events()` (hybrid), strict-citation grounded answers. Surface via zm-api endpoint. | Needs a corpus of stored descriptions (Phases 1-2) before query is meaningful. Highest complexity, lowest urgency. |
| **4** | Motion (optional) | Optical-flow / motion-coherence confirmation stage behind diff hits (NVIDIA HW flow on Blackwell), enabled per-zone for treed/weather scenes. Clip-based (Approach B) frame retrieval from `store_event` pre-roll for richer LLM temporal context. | Pure refinement. Only justified after Phase 0 data shows residual weather/tree false-wakes, and after Phase 1 shows single-frame context is insufficient. |

---

## System Component / Data-Flow Diagram

```
 CAMERA (RTSP)
     │
┌────▼─────────────┐   ZM_HW_CUDA surface stays in VRAM (zero-copy)
│ decode_ffmpeg    │────────────────────────────────────────────────┐
└────┬─────────────┘                                                 │
     │ luma (on GPU)                                                 │
┌────▼──────────────────────────────┐                               │
│ MOTION GATE                        │  cuda_motion_regions(y_ptr…)  │
│  downsampled-luma frame-diff       │  → [MotionRoi{x,y,w,h}, …]    │
│  (+N-of-M persistence, masks,      │  only the tiny grid crosses   │
│   global-luma suppression)         │  PCIe — frame never leaves GPU│
│  [opt Phase 4: HW optical-flow     │                               │
│   coherence confirm]               │                               │
└────┬───────────────────────────────┘                              │
     │ motion ROIs (cheap wake)                                      │
┌────▼─────────────┐  cuda_infer_nv12_batch on ROI crops            │
│ detect_onnx      │  → "detection" {label,bbox,conf}               │
└────┬─────────────┘                                                 │
┌────▼─────────────┐                                                 │
│ tracker          │  → "tracked_detection" {track_id,label,bbox}   │
└────┬─────────────┘                                                 │
┌────▼─────────────┐                                                 │
│ alert_policy     │  → "alert" {track_id,label,reason:new|moving}  │
└────┬─────────────┘                                                 │
     │  EventBus (host->publish_evt / subscribe_evt)                 │
     ├──────────────┬───────────────────────────────────┐           │
┌────▼────────┐ ┌───▼──────────────────────────────┐    │           │
│ store_event │ │ llm_event_review (NEW)            │◄───┘ snaps     │
│ pre/post    │ │  on_frame: snap latest RGB24      │      frame for │
│ roll .mkv   │ │  on alert/tracked_detection:      │      track     │
└─────────────┘ │   build DescribeRequest(frames)   │                │
                │   IVisionProvider.describe() ──────┼──┐            │
                │   publish "reasoning" event        │  │            │
                └───────────────┬────────────────────┘  │            │
                                │                        │           │
   ┌────────────────────────────┼────────────────────────┘           │
   │  IVisionProvider chain (Phase 2)                                 │
   │  Fallback{ RateLimited{Retry{Anthropic}},                       │
   │            RateLimited{Retry{Gemini}},                          │
   │            Local(OpenAI-compat) }                               │
   │     cloud first (quality) → LOCAL last (offline/privacy)        │
   │                                                                 │
   │   ┌──────────────────────────────────────────────┐             │
   └──►│ LOCAL VLM SERVER (RTX 5070 Ti 16GB)           │             │
       │  vLLM ≥0.11.1  Qwen3-VL-8B-Instruct-FP8       │             │
       │  /v1/chat/completions (OpenAI-compatible)     │             │
       │  fallback: llama-server Qwen3-VL-8B Q4_K_M    │             │
       └──────────────────────────────────────────────┘             │
                                │ "reasoning" {description,          │
                                │   threat_level, track_id}          │
                ┌───────────────▼──────────────────────────────┐    │
                │ EventBus → worker socket (Event.REASONING)    │    │
                │ → zm-api (Rust)                               │    │
                └───────────────┬──────────────────────────────┘    │
                                │ embed-at-ingest                    │
                ┌───────────────▼──────────────────────────────┐    │
                │ SQLite (single file)                          │    │
                │  events / detections tables  +  sqlite-vec    │    │
                │  embed_text = "cam ts | labels | description" │    │
                └───────────────┬──────────────────────────────┘    │
   NL question                  │                                    │
   "how many cars today?" ──►┌──▼───────────────────────────────┐   │
   "when did someone bring   │ Qwen (local, function-calling)    │   │
    a package last week?"    │ classify intent + extract slots   │   │
                             │  COUNT → count_events()  [SQL]    │   │
                             │  WHAT/WHEN → search_events()      │   │
                             │     pre-filter(ts,cam,class)      │   │
                             │     → sqlite-vec ANN + BM25       │   │
                             │  strict-citation answer w/event_id│   │
                             └───────────────────────────────────┘   │
                                                                     │
   (cuda kernels reused by bench_gpu_roi / bench_resources) ◄────────┘
```

---

# Workstream 1 — Motion Gating

## Decision: KEEP the GPU downsampled-luma frame-diff

zm-next already implements the textbook-correct NVR pattern: a **cheap motion gate → AI classifier**. This is exactly what Frigate (frame-diff luma + threshold + contour grouping + masks → YOLO) and Blue Iris (pixel-zone motion → CodeProject.AI/DeepStack) do, and what Hikvision AcuSense / Dahua SMD do on-camera (cheap pixel trigger → small classifier CNN). The gate's design goal is **high recall at low cost**: never miss real motion, tolerate false positives because the downstream object detector + tracker filters them.

This inverts the usual CDnet-style benchmark ranking, which weights precision heavily. Methods that win on CDnet F-measure (SuBSENSE, PAWCS, FgSegNet, BSUV-Net) are **overkill for a gate** — their precision gains are *redundant* sitting in front of a real object detector, and their cost (CPU-heavy single-threaded BG models, or a second GPU net) works directly against the "most efficient AND accurate NVR" goal.

### Why not the named alternatives

- **MOG2 / KNN / GMM** — adds per-pixel adaptivity + shadow handling over plain diff. Genuinely better against gradual lighting, modest cost — but it is the precision we *already get downstream*. It still ghosts and still trips on the cases that matter most (global luminance jumps from headlights / AGC / IR-cut), because those are global luma shifts, not things a per-pixel Gaussian rejects cleanly.
- **ViBe** — sample-based, very low compute/memory, detects from frame 2, embeds well. The strongest "cheap upgrade" candidate, but same redundancy argument: it buys robustness we don't need in front of YOLO.
- **SuBSENSE / PAWCS** — top the *non-learned* CDnet rankings on dynamic backgrounds (swaying trees, rippling water) via LBSP texture + local adaptive sensitivity, but they are CPU-heavy, single-threaded, and themselves sensitive to global illumination shifts. Their precision advantage is wasted on a gate.
- **Learned change-nets (FgSegNet F≈0.977, BSUV-Net)** — highest accuracy, but require a GPU net and don't generalize cheaply to unseen cameras (BSUV-Net authors note it isn't real-time). Putting a heavy net upstream of our real detector defeats the entire purpose of a gate.
- **Optical flow (Lucas-Kanade / Farneback / RAFT)** — uniquely able to *distinguish coherent translation (a walking person) from incoherent jitter (leaves, rain)*. This is the only family that addresses a real residual failure mode. But as a **primary gate** it is expensive (sparse LK >20 ms, dense Farneback ~8 ms, RAFT heavy). As a **confirmation stage** it is attractive — NVIDIA HW optical flow (Turing+, and our Blackwell 5070 Ti) runs ~2-3 ms/frame at near-zero GPU load. **This is the one optional add-on (Phase 4).**

**Verdict:** Don't swap the gate. The failure modes that actually hurt (headlights, auto-exposure, swaying trees) are not reliably fixed by MOG2/ViBe/SuBSENSE, and optical flow as a primary gate is too costly. Tune the existing path; add optical-flow *confirmation* only where data justifies it.

## Tuning the existing gate (noise / weather) — the real work

The gate lives in two places that share the same algorithm:
- CPU/host path: `plugins/motion_gate/motion_gate.cpp` (config keys `frame_width`, `frame_height`, `downscale`, `pixel_threshold`, `min_changed_pixels`, `cooldown_frames`, `gate`) + `plugins/motion_gate/motion_diff.hpp` (`zm::motiongate::downsample_luma`).
- On-GPU zero-copy path: `cuda_motion_regions(y_ptr, y_pitch, w, h, prev_grid, ds, pix_thr, min_cells, max_regions)` and `cuda_motion_bbox(...)` in `plugins/detect_onnx/detect_cuda.hpp` — only the tiny changed-cell grid crosses PCIe; the frame never leaves the GPU.

Tuning recipe (each maps to an existing or small-new config knob):

1. **Downsample harder before differencing** (`downscale` / `ds`). Coarser grids kill per-pixel sensor noise and rain/leaf speckle for free, and shrink the grid that crosses PCIe.
2. **Raise the min-blob threshold** (`min_changed_pixels` / `min_cells`). Speckle from leaves and rain is small and scattered; a real person/vehicle is a large contiguous blob. This is the single biggest weather win.
3. **N-of-M temporal persistence (NEW knob).** Require a region to be active in N of the last M frames before it counts as a wake. A continuous trajectory beats scattered noise; rain/leaf flicker is incoherent frame-to-frame. Add `persist_n` / `persist_m` to both paths.
4. **Static motion masks (NEW knob).** Per-zone mask grid over trees / timestamp overlays / flags / known-moving water. Cheap (AND against the changed-cell grid before connected components). Reuse the existing `zones`/`privacy_mask` plugin geometry where possible.
5. **Global-luma-jump suppression (NEW knob).** Track the frame mean luma (already computed on GPU); when it jumps beyond `global_luma_delta`, suppress or briefly debounce gate wakes for `K` frames. This is the principled answer to headlights, auto-exposure, and IR-cut — the cases MOG2 *doesn't* fix.
6. **(Phase 4) Optical-flow coherence confirm.** On a diff hit in a flow-enabled zone, run NVIDIA HW flow over the ROI; require coherent (low-divergence) motion before waking the detector. Rejects swaying-tree/rain/shake jitter that survives 1-5.

## A/B methodology with the existing benchmark harness

The repo already has the right tools. **Establish a baseline, then change one knob at a time.**

- **`bench/bench_gpu_roi.cpp`** drives the *real* `decode_ffmpeg` in CUDA mode and runs on-GPU luma-diff motion → full-frame GPU detect → zero-copy ROI-crop GPU detect, reusing the plugin's own `detect_cuda` kernels. This is the harness for **gate quality + ROI behavior**: feed labeled clips (clear, rainy, windy/treed, night-with-headlights, IR-cutover) and measure, per knob setting:
  - **Recall** (real events that produced a gate wake) — must stay ~1.0.
  - **False-wake rate** (gate wakes with no ground-truth object) — minimize.
  - **Wakes/sec → downstream inferences/sec** — the cost the gate imposes.
- **`bench/bench_resources.py`** (and `bench_engine_resources.py`) measure GPU/CPU/PCIe cost. Confirm tuning changes (more downsample, masks, persistence) **reduce** GPU load via fewer ROI inferences and don't regress decode throughput.
- **`bench/bench_roi_cascade.cpp`** for the CPU/host comparison point.

A/B loop: for each candidate config (downsample 4→6→8; min_cells; persist N-of-M; mask on/off; global-luma suppression on/off; Phase 4 flow on/off), run the labeled-clip suite through `bench_gpu_roi`, log (recall, false-wake/min, inferences/sec) and through `bench_resources` for (GPU%, PCIe MB/s). **Accept a change only if it cuts false-wakes/GPU-load with zero recall loss.** Optical flow (Phase 4) is accepted only if it removes residual treed/weather false-wakes that knobs 1-5 cannot, at < the cost of the inferences it saves.

---

# Workstream 2 — LLM Event Review

## What's wrong with `describe_vlm` today, and what we keep from `ha-llmvision`

Today's `plugins/describe_vlm/describe_vlm.cpp` is **timer-driven**: `run_inference_cycle()` every `interval_sec` (default 10s) POSTs the latest frame to an OpenAI-compatible server and publishes a `"description"` event. That is the same fundamental defect as `ha-llmvision`: it describes the scene on a clock, not when something happened, with no notion of *which object* or *which track* caused an event.

`ha-llmvision` is an excellent **consumer/UX layer** (multi-provider abstraction with local-first options, SSIM keyframe de-dup before paying for inference, importance tiering `passive`/`time-sensitive`/`critical`, structured extraction, a retention-bounded queryable timeline with thumbnails, two-phase notify) bolted onto a **weak, borrowed trigger model**: its "detection" is whatever Home Assistant hands it — a `binary_sensor` going on or a camera flipping to `recording` — and its frame selection is whole-frame SSIM. Concrete defects: no per-object gating (a swaying tree fires it and burns a call); poor temporal precision (SSIM picks the most pixel-different frames, not the decisive moment); event fragmentation (stateless fires de-duped only by a blunt 10-minute cooldown, with no track identity, so one person is recounted and back-to-back people are collapsed); latency/waste (records *after* the event begins).

**zm-next does not have to borrow its trigger.** Detection state lives *inside* our pipeline (`decode→motion→detect→track`). So the VLM becomes a **last-mile describer of well-formed tracks**, not the thing deciding what happened:

- **Trigger on tracks, not binaries** — fire on a `track_id` the moment `alert_policy` says it satisfies policy (class, dwell/zone/line-cross, confidence).
- **Per-object gating in-pipeline** — trees/rain never reach the VLM because they never become a tracked, classified object.
- **Track-driven keyframe selection** — pick frames from the object's crop trajectory (sharpest/largest/most-frontal), not whole-frame SSIM. Nails temporal precision and slashes tokens (tight crops + one context frame).
- **Event identity = no double-counting** — one track = one event = one timeline entry; de-dup by track continuity, not a fixed cooldown.
- **Keep the good parts** — provider abstraction (local-first), importance tiering, structured extraction, two-phase notify, retention-bounded timeline — but back the timeline with **embeddings/vector search**, not LLM-over-SQLite-rows.

## The local model: exact recommendation

**Primary: `Qwen/Qwen3-VL-8B-Instruct-FP8` served by vLLM (≥0.11.1)**, exposing the built-in OpenAI-compatible `/v1/chat/completions`. FP8 weights ≈ 9 GB, accuracy ≈ BF16, fits 16 GB with room for KV + image tokens. Qwen3-VL-8B is the quality sweet spot for temporal/relational event captioning ("a person left a package and walked away").

```
vllm serve Qwen/Qwen3-VL-8B-Instruct-FP8 \
  --max-model-len 32768 \
  --limit-mm-per-prompt.video 0 \
  --gpu-memory-utilization 0.85 \
  --mm-processor-kwargs '{"max_pixels": 1003520}'
# CUDA 12.8+/sm_120 build (Blackwell); vLLM >= 0.11.1
```

**Fallback (simplest, lowest VRAM): `Qwen3-VL-8B-Instruct` GGUF Q4_K_M + mmproj on `llama-server`** (~5-7 GB, CUDA-12.8+ build for Blackwell sm_120). Same OpenAI-compatible wire shape.

```
llama-server -m Qwen3VL-8B-Instruct-Q4_K_M.gguf \
  --mmproj mmproj-Qwen3VL-8B-Instruct-F16.gguf \
  -c 16384 -ngl 99 --host 0.0.0.0 --port 8080
```

**Critical practical guidance (the part people get wrong):**
- **Quantization is mandatory** on 16 GB — full BF16 7B/8B VLMs OOM even on a 24 GB 4090 once image preprocessing + KV cache are added. FP8 or INT4/Q4 fit the 8B with margin.
- **Cap image tokens.** Visual tokens ≈ (H×W)/(28×28). The shipped `max_pixels` default is ~16,384 tokens/image — ~12× the trained regime; clamp to ~1 MP (~1000 tokens). For surveillance, packages/people are large objects; you do not need OCR-grade resolution.
- **Pre-sample 4-8 keyframes from the track, feed JPEGs**, disable video decode (`--limit-mm-per-prompt.video 0`). Multiplying frames is the main VRAM/latency driver. Our C++ side already has the frames.
- **Realistic latency on the 5070 Ti:** prefill (vision encode + frame tokens) dominates; end-to-end ≈ **1.5-3 s per event** on vLLM/FP8 (3-6 s on llama.cpp Q4). For event-triggered (not per-frame) captioning, well within budget.
- **Blackwell (sm_120) is the single biggest setup risk.** CUDA 12.8+ is the first with full sm_120 support; vLLM/FlashInfer may need recent wheels or source builds. With CUDA 12.9 this is largely resolved in current releases, but **budget environment-setup time**. If setup pain is unacceptable, llama.cpp/Ollama get you running fastest. **Avoid Ollama beyond prototyping** (single-user, weak concurrency/tuning).

Strong alternatives if needed: **Qwen3-VL-4B-FP8** (more headroom/concurrency, terser), **MiniCPM-V 2.6** (great native video-clip alternative), **InternVL3/3.5**, **SmolVLM2-2.2B** (<4 GB, many parallel streams).

## New plugin: `llm_event_review` (PROCESS plugin)

Grounded on the real zm-next ABI:

| ABI element | File:line | Use |
|---|---|---|
| Plugin type `ZM_PLUGIN_PROCESS` | `core/include/zm_plugin.h:27-33` | not INPUT/DETECT/OUTPUT/STORE |
| `start()` | `plugins/alert_policy/alert_policy.cpp:133-150` | parse cfg; `host->subscribe_evt(host_ctx, event_cb, ctx)` |
| Event subscribe handle | `core/src/PluginManager.cpp:32-38` | opaque handle, freed in `stop()` |
| `on_frame()` snap RGB24 | `plugins/describe_vlm/describe_vlm.cpp:55-62, 338-382` | latest-frame snapshot under mutex when `hw_type==ZM_FRAME_RGB24`; forward downstream |
| Event publish | `plugins/alert_policy/alert_policy.cpp:66-74`, `tracker.cpp:144-145` | `host->publish_evt(host_ctx, json.dump().c_str())` |
| `stop()` | `plugins/alert_policy/alert_policy.cpp:151-165` | `host->unsubscribe_evt`, stop worker |
| Frame header | `core/include/zm_plugin.h:74-81` | `zm_frame_hdr_t` → `stream_id`, `pts_usec`, `hw_type` |
| Logging | `core/include/zm_plugin.h:239-243` | `ZM_LOG_INFO/ERROR` |
| HTTP/JPEG client to reuse | `plugins/describe_vlm/vlm_client.hpp` | fork for richer prompt + `IVisionProvider` |

**Subscribed events** (via EventBus):

| Event | Source | Payload |
|---|---|---|
| `tracked_detection` | `tracker` (`tracker.cpp`) | `stream_id`, `pts_usec`, `detections[].{track_id,label,confidence,bbox}` |
| `alert` | `alert_policy` (`alert_policy.cpp:76-131`) | `stream_id`, `pts_usec`, `track_id`, `label`, `bbox`, `reason` ("new"\|"moving_again") |

**Behavior:** on a qualifying `alert`/`tracked_detection`, build a `DescribeRequest` from the track's frame(s) (MVP = latest snap, Approach A; Phase 4 = clip from `store_event` pre-roll, Approach B), call `IVisionProvider.describe()`, and publish a new `"reasoning"` event:

```json
{
  "type": "reasoning",
  "stream_id": 3, "pts_usec": 172839, "track_id": 42,
  "label": "person", "trigger_event": "alert", "trigger_reason": "new",
  "llm_response": { "description": "...", "threat_level": "low|medium|high",
                    "details": "...", "confidence": 0.0 },
  "model": "qwen3-vl-8b-fp8", "provider_id": "local", "inference_time_ms": 1820
}
```

**Config keys:**

```json
{
  "id": "llm_review", "kind": "llm_event_review",
  "cfg": {
    "trigger_events": ["alert", "tracked_detection"],
    "confidence_threshold": 0.5,
    "frame_width": 1920, "frame_height": 1080,
    "keyframes": 4, "stream_filter": [], "timeout_sec": 30,
    "importance": { "enabled": true },
    "prompt_template": "Object: [label] at [bbox]. Event: [trigger_reason]. Describe the security event in one sentence; note anomalies; assess threat (low/medium/high).",
    "vlm": {
      "default_chain": ["local"],                 // Phase 1: local only
      "request_defaults": { "detail": "low", "max_output_tokens": 256,
                            "timeout_ms": 20000, "max_long_edge_px": 1280 },
      "providers": {
        "local": { "kind": "openai_compatible",
                   "base_url": "http://127.0.0.1:8000/v1",
                   "model": "Qwen/Qwen3-VL-8B-Instruct-FP8",
                   "api_key": "", "send_detail_param": false,
                   "image_mode": "base64_only" }
      }
    }
  }
}
```

## Pluggable provider interface (Phase 2)

One canonical OpenAI-shaped in-memory request/response; per-provider adapters translate to the wire format. The **OpenAI-compatible `/v1/chat/completions` content shape is the portable format** — spoken by llama.cpp/`llama-server`, Ollama, vLLM, LM Studio, *and* OpenAI itself — so one serializer covers OpenAI + every local server. Only **Anthropic** (`source` block: `{"type":"image","source":{"type":"base64","media_type":...,"data":...}}`) and **Gemini** (`{"inline_data":{"mime_type":...,"data":...}}`) need small native adapters.

```cpp
class IVisionProvider {
public:
  virtual std::expected<DescribeResponse, ProviderError>
    describe(const DescribeRequest&, std::stop_token = {}) = 0;
  virtual bool supports_multi_image() const = 0;
  virtual uint32_t max_images() const = 0;
  virtual std::string provider_id() const = 0;   // "anthropic"|"openai"|"gemini"|"local"
  virtual bool is_local() const = 0;
  virtual bool healthy() const = 0;              // gates fallback
};
```

Adapters: `OpenAICompatProvider` (covers OpenAI + llama.cpp/Ollama/vLLM/LM Studio, just a different `base_url`), `AnthropicProvider`, `GeminiProvider`. **Resilience is composed as decorators, not baked per-adapter:** `RetryingProvider` (exp backoff + full jitter on 429/5xx/network/timeout, honoring `Retry-After`), `RateLimitedProvider` (token bucket to stay under RPM/TPM), `FallbackProvider` (try chain in order, circuit-break unhealthy, **cloud→local fallback**).

Typical wiring (Phase 2): `Fallback{ RateLimited{Retry{Anthropic}}, RateLimited{Retry{Gemini}}, Local }` — cloud first for quality, **local last so the NVR keeps working offline / under rate-limit / for sensitive cameras.**

**Provider facts that shape config/cost:**
- **Anthropic** — up to 100 images/request (200k models), ≤10 MB/image, long edge resized ≤1568px; ~$0.02-0.06 per 4-frame event; inputs not trained on, auto-deleted 7 days, **ZDR available**. Header: `x-api-key` + `anthropic-version`.
- **OpenAI** — `detail:"low"` ≈ flat ~85 tokens (right NVR default); escalate to `high` only for plates/faces; not trained on API data since Mar 2023, abuse logs ≤30 days, ZDR available. Header: `Authorization: Bearer`.
- **Gemini** — typically cheapest; ≤384px = 258 tokens, tiled 768×768 at 258/tile; inline ≤20 MB total else File API. **Paid tier ONLY for footage** (free tier may train on prompts). Header: `x-goog-api-key`.

**Cross-cutting for security footage:** default `detail:"low"` + downscale to ≤1280-1568px long edge before send (cuts cost *and* PII exposure); stateless one-event-per-request; **per-camera local-only routing** for sensitive cameras (tag camera → select local-only chain).

**Key handling:** never plaintext — resolve via `secret_ref` indirection (`env:ANTHROPIC_API_KEY`, `file:/run/secrets/...`, OS keychain); per-provider header injection in the adapter; **never log frame bytes** — log only `request_id`, provider, token usage, latency.

```yaml
vlm:                                   # Phase 2 full chain
  default_chain: [cloud_primary, cloud_secondary, local]
  request_defaults: { detail: low, max_output_tokens: 256, timeout_ms: 20000, max_long_edge_px: 1280 }
  providers:
    cloud_primary:   { kind: anthropic, base_url: https://api.anthropic.com,
                       model: claude-sonnet-4-6, api_key: ${ANTHROPIC_API_KEY},
                       anthropic_version: "2023-06-01", zero_data_retention: true,
                       rate_limit: { rpm: 50, tpm: 100000 },
                       retry: { max_attempts: 4, base_ms: 500, max_ms: 8000, jitter: full } }
    cloud_secondary: { kind: gemini, model: gemini-2.5-flash, api_key: ${GEMINI_API_KEY},
                       paid_tier: true,        # REQUIRED for footage
                       rate_limit: { rpm: 60, tpm: 1000000 } }
    local:           { kind: openai_compatible, base_url: http://127.0.0.1:8000/v1,
                       model: Qwen/Qwen3-VL-8B-Instruct-FP8, api_key: "",
                       send_detail_param: false, image_mode: base64_only }
  circuit_breaker: { failure_threshold: 5, cooldown_ms: 30000 }
```

## Event → description → index → query data flow (Phase 3)

**Index (at ingest, zm-api Rust consumer of `Event.REASONING`):** one event = one chunk = one row. Build an enriched embed string folding metadata into the text so semantic search "knows" about camera/labels, while keeping raw fields as filterable columns:

```
embed_text = "front_door_cam 2026-06-18 14:32 | person, package |
A person in dark clothing approached the front door carrying a parcel and set it down."
```

Store `event_id`, `ts` (epoch int, indexed), `monitor_id`, `labels[]`, `track_ids[]`, `description`, `threat_level`, plus the vector — colocated with the metadata so structured filters and vector search run in the *same* SQL query (decisive for selective filters). The physical store is backend-dependent (see *Vector store* below): MariaDB-native `VECTOR` in the operational DB when available, else a single SQLite file with `sqlite-vec` (`vec0`).

**Embedding model (local, compute-once-per-event):** `bge-small`/`bge-base` or `nomic-embed-text-v1.5` (CPU-friendly, MIT/Apache); step up to `Qwen3-Embedding-0.6B` to stay in one model family with the generator.

**Vector store: a pluggable, capability-detected backend (feature-flagged).** The vector index is a
**derived, rebuildable** artifact (truth lives in the operational DB + media/sidecars), so the store
backend is chosen at runtime behind one abstraction rather than hard-committed. zm-next stays
**DB-less** throughout — it only emits the description (and, optionally, the image/CLIP embedding as
an event field, computed on the GPU it already runs); *which* store receives it is entirely zm-api's
decision.

- **Abstraction:** a `VectorStore` trait in zm-api — `ensure_schema` / `upsert(event_id,kind,vec,meta)`
  / `search(q,filter,k)` / `rebuild()` — with impls `MariaDbVectorStore`, `SqliteVecStore`,
  `PgVectorStore` (later), and `NullVectorStore` (feature off). The ingest path and the search API are
  identical across backends; the engine-specific vector SQL (MariaDB `VEC_DISTANCE_COSINE`, pgvector
  `<=>`) is hidden inside each impl (so it's raw SQL, not ORM-portable — fine behind the trait).
- **Feature flag** (zm-api settings):
  ```toml
  [search]
  enabled = "auto"   # auto | on | off
  backend = "auto"   # auto | mariadb | sqlite | postgres | none
  ```
- **Capability detection ("only if the DB supports it"):** at startup, probe the *already-connected*
  DB functionally, not by version string alone (forks/distro patches make version-sniffing fragile).
  MariaDB: version ≥ 11.8 **and** a tiny `VECTOR(3)` temp-table probe succeeds → native available
  (Community 11.8 LTS ships `VECTOR` + HNSW `VEC_DISTANCE_*`, GPLv2, no extension). Postgres:
  `CREATE EXTENSION IF NOT EXISTS vector`. Cache + log the chosen backend loudly.
- **Selection (`auto`):** prefer **MariaDB-native** (same engine ZoneMinder already runs → in-DB
  hybrid: metadata pre-filter + FTS + vector ANN in one transactional query over the same rows) →
  else **`sqlite-vec`** as the **universal floor** (embeddable single file, no server-version
  dependency, so search still works on older 10.x MariaDB / MySQL) → `PgVectorStore` slots into the
  same chain later. An operator who refuses a second store can set `backend = "mariadb"` (strict:
  disabled when native is absent, no sqlite fallback) — the flag supports both graceful-degrade
  (default) and native-or-nothing.
- **Rebuild-on-upgrade = near-zero lock-in:** because the index is rebuildable from descriptions,
  switching backends is a `rebuild()` (re-embed), not a data migration. A user on 10.x runs sqlite-vec
  today; after a MariaDB 11.8 upgrade, `auto` detects native vectors, rebuilds into the in-DB store,
  and drops the sqlite file. Adding `PgVectorStore` later is one impl + one probe branch, no API/ingest
  change.

Whichever backend is active, the requirement is the same and it's what `sqlite-vec` first motivated:
**pre-filtering** (metadata/time/class applied *before* the ANN). sqlite-vec does this via in-DB joins;
MariaDB-native and pgvector ≥ 0.8 (iterative index scans) do it in-engine — avoid any store that can
only post-filter, which starves selective queries like "camera=front_door AND last 1h".

**Embeddings table is zm-api-owned and additive** — a `zmnext_event_vectors`-style table (or the
separate sqlite-vec file), never a change to ZoneMinder's shared schema; zm-api owns its own
migrations for it.

**Query — this is a *router* problem, not pure RAG.** A local **Qwen with function calling** classifies intent and extracts slots (`time_range`→epoch bounds resolved *in code*, `monitor_id`, `classes`), then routes:

- `count_events(time_range, monitor_id, classes, group_by)` → **direct SQL `COUNT ... GROUP BY`** for "how many cars today" — never let the LLM tally a context window (it miscounts; top-k truncation silently undercounts).
- `search_events(time_range, monitor_id, classes, query_text, k)` → **hybrid retrieval** for "when did someone bring a package": SQL **pre-filter** (ts/monitor/class) → `sqlite-vec` ANN + BM25, top-k, threshold.

**Generation:** strict-citation closed-book prompt — "Answer **only** using the events below; every claim must cite the `event_id`; if not present, say so; counts come only from the count tool." Then **verify every cited `event_id` exists** in the result set before returning (drop/flag unverifiable IDs). Returning `event_id`s lets the UI deep-link to the clip/snapshot — verifiable grounding.

**Pitfalls baked into the design:** counting → SQL (never LLM tally); temporal → resolve relative dates to epoch bounds in code + indexed `ts` + SQL pre-filter + `ORDER BY ts`; Text2SQL errors → parameterized tool functions with whitelisted columns, app-side date math; hallucination → strict-citation + existence-verify; post-filter recall loss → `sqlite-vec` pre-filtering.

## Transport changes

The canonical worker stream-socket EVENT channel (`core/include/zm/stream_socket_protocol.hpp`)
already has detection (`0x0301`) and description (`0x0302`) event codes carrying a JSON detail
TLV (`0x10`). **Add:**
- A `reasoning` event code (`0x0305`, additive — version stays 1, skip-on-unknown) whose JSON
  detail is `{description, threat_level: LOW|MEDIUM|HIGH, confidence, track_id, trigger_reason,
  model, provider_id}`.
- zm-api ingests the reasoning event, persists + embeds it, and exposes `POST /api/events/search`
  for the NL query subsystem.

---

## Key Risks & Mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| Blackwell sm_120 build pain (vLLM/FlashInfer) | Local VLM won't start | CUDA 12.8+/12.9; use recent wheels; **llama.cpp Q4 fallback** gets you running; budget setup time in Phase 1 |
| VRAM OOM from uncapped image/video tokens | Crashes under load | `max_pixels~1e6`, `--max-model-len 32768`, `--limit-mm-per-prompt.video 0`, 4-8 pre-sampled keyframes, FP8/Q4 only |
| Over-tuning the motion gate hurts recall | Missed real events (unrecoverable) | A/B gate via `bench_gpu_roi`; **accept a knob only at zero recall loss**; recall is the hard constraint |
| Cloud provider leaks surveillance PII | Privacy/legal | Paid-tier + ZDR; `detail:low` + downscale before send; **per-camera local-only routing**; stateless requests; never log frame bytes |
| Cloud rate-limit / outage | No descriptions | `Retry`+`RateLimited` decorators; `Fallback` chain with **local last** = always-available backstop |
| RAG miscounts / hallucinates over events | Wrong answers | Route counting to SQL; strict-citation + `event_id` existence-verify; resolve time to epoch bounds in code; `sqlite-vec` pre-filter |
| Inter-plugin clip retrieval not in ABI | Approach B blocked | Ship Approach A (frame snap) in Phase 1; add `store_event` pre-roll query to ABI in Phase 4 only if single-frame context proves insufficient |
| Two competing detectors (like Frigate+ha-llmvision) | Duplicated work | N/A by design — single in-pipeline `detect→track`; VLM is a pure describer of tracks, never a second detector |

---

## Sources

**Motion / change detection**
- https://docs.frigate.video/configuration/motion_detection/
- https://deepwiki.com/blakeblackshear/frigate/4.3-motion-detection
- https://github.com/vandroogenbroeckmarc/vibe
- https://www.semanticscholar.org/paper/SuBSENSE:-A-Universal-Change-Detection-Method-With-St-Charles-Bilodeau/aab873234597bc5d97ef31317d8bbf7d7e8ae5e2
- https://arxiv.org/pdf/1811.05255
- https://arxiv.org/pdf/1907.11371
- https://developer.nvidia.com/blog/opencv-optical-flow-algorithms-with-nvidia-turing-gpus/
- https://www.wundertech.net/how-to-set-up-codeprojectai-on-blue-iris/
- https://cctvplanner.io/compare/hikvision-vs-dahua
- https://arxiv.org/html/2411.02582v1
- https://hometechops.com/cameras/frigate-false-detections
- https://link.springer.com/article/10.1007/s11554-023-01288-6
- https://www.cambridge.org/core/journals/apsipa-transactions-on-signal-and-information-processing/article/moving-object-detection-in-the-h264avc-compressed-domain/6E33AB0ABBD364B1B8627007E8DAC9EB

**Local Qwen-VL on 16 GB / serving**
- https://github.com/QwenLM/Qwen3-VL
- https://arxiv.org/abs/2511.21631
- https://huggingface.co/Qwen/Qwen3-VL-8B-Instruct-GGUF
- https://huggingface.co/Qwen/Qwen3-VL-8B-Instruct-FP8
- https://huggingface.co/Qwen/Qwen3-VL-4B-Instruct
- https://docs.vllm.ai/projects/recipes/en/latest/Qwen/Qwen3-VL.html
- https://github.com/vllm-project/vllm/issues/21239
- https://github.com/vllm-project/vllm/issues/24728
- https://gigagpu.com/rtx-4090-24gb-for-qwen-25-7b/
- https://gigagpu.com/fix-vllm-out-of-memory-kv-cache/
- https://huggingface.co/Qwen/Qwen2.5-VL-7B-Instruct/discussions/18
- https://huggingface.co/Qwen/Qwen2.5-VL-7B-Instruct-AWQ
- https://docs.vllm.ai/projects/recipes/en/latest/Qwen/Qwen2.5-VL.html
- https://github.com/Blaizzy/mlx-vlm/issues/1175
- https://pub.towardsai.net/qwen2-5-vl-a-hands-on-code-walkthrough-5fba8a34e7d7
- https://uv020.medium.com/qwen2-5-vl-ai-at-the-intersection-of-vision-language-569fe85bf1bf
- https://tensorfoundry.io/blog/llm-inference-servers-compared
- https://www.glukhov.org/llm-performance/benchmarks/choosing-best-llm-for-ollama-on-16gb-vram-gpu/
- https://deepwiki.com/ggml-org/llama.cpp/6.5-multimodal-support-(libmtmd)
- https://github.com/ggml-org/llama.cpp/issues/22696
- https://forums.developer.nvidia.com/t/software-migration-guide-for-nvidia-blackwell-rtx-gpus-a-guide-to-cuda-12-8-pytorch-tensorrt-and-llama-cpp/321330
- https://blog.roboflow.com/local-vision-language-models/
- https://arxiv.org/html/2504.05299v1
- https://huggingface.co/openbmb/MiniCPM-V-2_6
- https://willitrunai.com/models/minicpm-v-2.6-8b

**Third-party provider APIs**
- https://platform.claude.com/docs/en/build-with-claude/vision
- https://platform.claude.com/docs/en/manage-claude/api-and-data-retention
- https://privacy.claude.com/en/articles/8956058-i-have-a-zero-data-retention-agreement-with-anthropic-what-products-does-it-apply-to
- https://developers.openai.com/api/docs/guides/images-vision
- https://developers.openai.com/api/docs/guides/your-data
- https://ai.google.dev/gemini-api/docs/image-understanding
- https://ai.google.dev/gemini-api/docs/pricing
- https://ai.google.dev/gemini-api/docs/billing
- https://docs.ollama.com/api/openai-compatibility
- https://github.com/ggml-org/llama.cpp/blob/master/docs/multimodal.md
- https://docs.vllm.ai/en/v0.18.0/serving/openai_compatible_server/
- https://hackernoon.com/openais-rate-limit-a-guide-to-exponential-backoff-for-llm-evaluation
- https://werun.dev/blog/how-to-handle-llm-api-rate-limits-in-production
- https://www.spoold.com/tools/vision-tokens

**NL query / RAG / hybrid retrieval**
- https://www.mindstudio.ai/blog/rag-vs-knowledge-graphs-vs-tabular-models-agent-memory
- https://arxiv.org/pdf/2510.09106
- https://app.ailog.fr/en/blog/guides/choosing-embedding-models
- https://www.bentoml.com/blog/a-guide-to-open-source-embedding-models
- https://www.morphllm.com/ollama-embedding-models
- https://arxiv.org/pdf/2402.01613
- https://github.com/asg017/sqlite-vec
- https://github.com/asg017/sqlite-vss
- https://mariadb.org/projects/mariadb-vector/  (MariaDB native VECTOR + HNSW, Community 11.8 LTS, GPLv2)
- https://mariadb.com/docs/server/reference/sql-structure/vectors/vector-overview
- https://github.com/pgvector/pgvector  (pgvector ≥ 0.8 iterative index scans for filtered search)
- https://dev.to/aairom/embedded-intelligence-how-sqlite-vec-delivers-fast-local-vector-search-for-ai-3dpb
- https://www.instaclustr.com/education/vector-database/pgvector-performance-benchmark-results-and-5-ways-to-boost-performance/
- https://arxiv.org/pdf/2403.04871
- https://dev.to/yaruyng/retrieval-strategy-design-vector-keyword-and-hybrid-search-53j3
- https://towardsdatascience.com/hybrid-search-and-re-ranking-in-production-rag/
- https://www.myscale.com/blog/filtered-vector-search-in-myscale/
- https://arxiv.org/pdf/2510.27141
- https://redis.io/blog/hybrid-search-benefits-rag-systems/
- https://blog.gopenai.com/power-up-rag-chatbot-with-hybrid-search-and-filtering-47386bade934
- https://zilliz.com/blog/metadata-filtering-hybrid-search-or-agent-in-rag-applications
- https://arxiv.org/pdf/2510.24402
- https://qwen.readthedocs.io/en/latest/framework/function_call.html
- https://github.com/QwenLM/Qwen-Agent
- https://deepwiki.com/QwenLM/Qwen2.5/2.2-function-calling-and-tool-use
- https://arxiv.org/html/2512.12117v1
- https://www.elastic.co/search-labs/blog/grounding-rag
- https://www.mdpi.com/2076-3417/16/6/3013
- https://www.longato.ch/llm-grounding-prompts/
- https://medium.com/@myscale/why-sql-for-retrieval-augmented-generation-rag-system-ade8bb989306
- https://arxiv.org/pdf/2601.09523
- https://www.k2view.com/blog/rag-hallucination/
- https://arxiv.org/html/2510.24476v1

**ha-llmvision teardown**
- https://github.com/valentinfrlch/ha-llmvision
- https://github.com/valentinfrlch/ha-llmvision/blob/main/custom_components/llmvision/media_handlers.py
- https://deepwiki.com/valentinfrlch/ha-llmvision/4-blueprints
- https://deepwiki.com/valentinfrlch/ha-llmvision/4.1-event-summary
- https://deepwiki.com/valentinfrlch/ha-llmvision/5.2-timeline-setup
- https://llmvision.gitbook.io/getting-started/usage/stream-analyzer
- https://github.com/valentinfrlch/llmvision-card

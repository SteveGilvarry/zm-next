# Mixed-GPU & AMD full-GPU path — findings

**Question:** zm-next's detect pipeline (decode → motion gate → preprocess → YOLO)
can run entirely on one GPU, or be **split across GPUs** — e.g. an integrated GPU
doing the always-on decode + motion gate, a discrete GPU doing YOLO (and the LLM).
When is the split worth it?

**Box used:** RTX 5070 Ti (16 GB, NVDEC/CUDA) + AMD Ryzen 7 9700X integrated Radeon
(RADV iGPU, VCN decode). Detection runs on the **720p substream** (detect-on-low-res,
overlay-on-4K), so 720p is the number that matters; 4K is shown for contrast.

---

## TL;DR
1. **A single strong GPU wins solo and has huge headroom.** On the 5070 Ti one 4K
   stream is 1–3 % GPU; you'd need ~18 flat (or far more gated) streams to saturate
   it.
2. **The AMD iGPU *can* do the whole front-half on-GPU**, validated end-to-end
   (decode + Vulkan motion gate + Vulkan preprocess). At 720p it's cheap: motion
   **0.13 ms**, preprocess **0.43 ms**, **865 fps** front-half.
3. **Naive in-process mixing is a trap.** Running the AMD VAAPI/Vulkan stack and
   ORT-CUDA in the *same process* collapses CUDA YOLO from **1.8 ms → 30 ms** (it
   degrades to ~CPU-EP speed). The naive mixed pipeline is **13× worse** than
   single-GPU (31 vs 404 fps).
4. **Done right (process isolation), mixed *can* win at 720p** — ~553 fps detect-bound
   vs 404 fps single-GPU — but only with separate processes + a shared-memory handoff
   + pipelining. Big engineering cost.
5. **Rule for the auto-selector:** default single-GPU; split only when the inference
   GPU is the bottleneck (many cameras, weak/old GPU, or LLM-pinned) **and** you can
   isolate the two stacks in separate processes.

---

## Measured numbers

### Single-GPU NVIDIA (everything on the 5070 Ti)
| Stream | NVDEC decode | CUDA YOLO | Full pipeline | GPU util |
|---|---|---|---|---|
| 4K | 505 fps | 1.8 ms | 413 fps | 1–3 % |
| **720p** | **2065 fps** | **1.81 ms** | **404 fps** | 1–3 % |

Detect-bound at ~400 fps either way (YOLO is always 640×640 regardless of source res).
Concurrent detect saturation (shared CUDA session, batch-1): plateaus at **~540 inf/s**
at 70–93 % util (the session, not the card, is the limit without batching).

### iGPU front-half (AMD, VAAPI decode + Vulkan motion + Vulkan preprocess)
| Source | Motion (Vulkan) | Preprocess | Front-half throughput | iGPU / NVIDIA util |
|---|---|---|---|---|
| 4K | 1.89 ms | 0.84 ms | 261 fps (~8 cam) | 15 % / 1 % |
| **720p** | **0.13 ms** | **0.43 ms** | **865 fps (~28 cam)** | 1 % / ~0 % |

The motion gate is **fully GPU-resident** on the iGPU — a 20-byte verdict is read
back per frame, validated byte-identical to a CPU reference over 120 frames
(0 mismatches). Handoff payload to the discrete GPU is the CHW: **4.9 MB/frame**.

### End-to-end mixed pipeline (iGPU → NVIDIA YOLO), one process, 720p
| Stage | Cost |
|---|---|
| iGPU motion | 0.13 ms/frame |
| iGPU preprocess | 0.43 ms/frame |
| CHW handoff | 4.9 MB/frame |
| **NVIDIA YOLO (ORT-CUDA)** | **30 ms/frame** ❌ (vs 1.8 ms native) |
| **End-to-end** | **31 fps** (vs 404 single-GPU) |

---

## The key finding: naive in-process mixing breaks CUDA

The iGPU stages behave exactly as measured standalone. But the **ORT-CUDA YOLO
collapses from 1.8 ms to 30 ms** the moment it shares a process with the AMD
VAAPI/Vulkan stack. Ruled out:
- **vLLM contention** — same 30 ms with the LLM stopped.
- **NVIDIA's Vulkan ICD loading** — same 30 ms with Vulkan restricted to the RADV
  (AMD) ICD only.

The same model + same ORT runs at **1.98 ms in a clean process** (`bench_decode_detect`),
and NVIDIA util sat at ~5 % during the mixed run — the signature of the **CUDA EP
falling back to ~CPU-EP performance** (30 ms ≈ CPU-EP with the NMS-free head) when
coexisting with the AMD driver stack in one address space.

**Implication:** a production mixed-GPU pipeline must use **process isolation** —
an iGPU process (decode + motion + preprocess) handing CHW tensors over shared
memory to a separate NVIDIA process (YOLO). zm-next's `ShmRing` already provides
the cross-process transport. In-process mixing is not viable.

---

## When does the split actually pay off?

| Capacity (720p, 4K@30, ~23 % gated) | Value | Cameras |
|---|---|---|
| NVIDIA detect | ~540 inf/s | ~80 gated / ~18 flat |
| iGPU front-half | 865 fps | ~28 |

- **Strong inference GPU (5070 Ti):** wins solo. One stream barely touches it
  (1–3 %); it saturates well after the iGPU's offload ceiling. **Don't split.**
- **Mixed wins** only when the inference GPU's detect capacity drops below the
  iGPU's ~28-camera offload — i.e. a **weak/old discrete GPU**, **many cameras
  saturating a mid GPU**, or a **GPU pinned by a co-resident LLM** (we separately
  measured detect latency going 1.8 ms → 15 ms under LLM generation — exactly the
  work the iGPU keeps unaffected).
- And even then, **only with process isolation** — naive mixing is catastrophically
  slow (31 fps).

### Auto-selector heuristic
Probe devices at startup (CUDA props, Vulkan/ncnn r-score, VAAPI/QSV decoders, VRAM),
then:
- **Default:** single-GPU — assign decode+motion+detect to the strongest GPU.
- **Split** (iGPU front-half → discrete YOLO, *separate processes*) only when the
  inference GPU is weak, saturated by stream count, or shared with the LLM.
- JSON policy: `"hw_policy": "auto"` (probe + decide) or explicit per-stage device
  pins (`decode`/`motion`/`detect`/`llm`).

---

## Background: the AMD full-GPU path (what enabled this test)

The mixed test needed the iGPU to do real GPU work. That path was built and
validated (prototypes in `bench/vk/`, backend in `plugins/detect_onnx/`):

| Stage | Implementation | Validation |
|---|---|---|
| Decode | VAAPI (VCN) → VA surface | runs on the iGPU |
| Import | `vaExportSurfaceHandle` → dma_buf → Vulkan `VK_EXT_image_drm_format_modifier` | 0 mismatches vs CPU (tiled surface) |
| Motion gate | Vulkan compute (downsample + diff, device-resident prev grid) | 0 verdict mismatches / 120 frames, 20 B readback |
| Preprocess | Vulkan compute (letterbox + BT.601 YUV→RGB + normalize → CHW) | correct letterboxed frame |
| Inference | ncnn-Vulkan YOLO on the iGPU | works; 31 ms on this iGPU (3.2 ms on the 5070 Ti, same Vulkan code) |

Wired into `hw_backend_vulkan.cpp` (ncnn isolated in `vulkan_ncnn_infer.cpp` to dodge
its bundled-Vulkan header clash), `ZM_WITH_VULKAN` build option. **Because Vulkan is
vendor-agnostic, this same path covers AMD (iGPU + discrete), Intel, and even NVIDIA**
— a portability win that ROCm (no iGPU support) can't match.

Note: on *this* AMD desktop iGPU, ncnn-Vulkan inference (31 ms) is slightly *slower*
than the CPU EP (24 ms) — the chip is a 2-CU part (ncnn r-score 11 vs the 5070 Ti's
84). The full-GPU inference win is real on bigger iGPUs (Intel Arc + NPU, AMD Strix)
and discrete; on a tiny desktop iGPU its value is CPU offload, not raw speed.

---

## Benches
- `bench/vk/vk_va_igpu.cpp` — iGPU front-half (decode+motion+preprocess), throughput.
- `bench/vk/vk_va_mixed.cpp` — end-to-end mixed (iGPU front-half → ORT-CUDA YOLO).
- `bench/vk/vk_va_gate.cpp` / `vk_va_preproc.cpp` — motion / preprocess validation.
- `bench/bench_decode_detect.cpp` — single-GPU NVIDIA baseline.

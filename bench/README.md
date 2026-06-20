# Decode/Detect CPU-vs-GPU benchmark

Quantifies where the GPU actually helps in the `capture → decode → detect` path,
and — specifically — the cost of moving frames between GPU and system memory.

It `dlopen`s the real `decode_ffmpeg` and `detect_onnx` plugins and demuxes a
**file or an `rtsp://` URL** via libavformat, then measures:

1. **Decode** — CPU software vs NVDEC zero-copy vs *NVDEC + GPU→CPU download*
   (`av_hwframe_transfer_data`). The download line is the "hwaccel tax": fast
   NVDEC decode can be eaten by the per-frame copy back to system RAM, and that
   copy scales with resolution.
2. **Detect EP** — ORT `CPU` vs `CUDA(host input)` vs `CUDA(device input / IoBinding)`.
   The host-vs-device pair isolates the per-inference HtoD upload the zero-copy
   path avoids.
3. **End-to-end** — all-CPU (sw decode + CPU detect) vs full GPU zero-copy
   (NVDEC surface → CUDA kernel → ORT IoBinding, never leaving VRAM).

## Build

Requires the CUDA build of zm-next (see the top-level build recipe). Add the
benchmark flag:

```bash
cmake -S . -B build <...usual flags...> -DZM_WITH_CUDA=ON -DZM_BUILD_BENCH=ON
cmake --build build --target bench_decode_detect -j$(nproc)
```

## Run

```bash
bench/run_bench.sh           # all clips in bench/clips/ + camera if configured
```

Or directly:

```bash
build/bench/bench_decode_detect \
  --input bench/clips/4k_h264.mp4 \
  --model bench/models/yolo26n.onnx \
  --plugins build/plugins [--max-frames 300] [--passes 4]
```

`run_bench.sh` sets `LD_LIBRARY_PATH` (vcpkg dynamic libs + onnxruntime + CUDA).

## Camera (live RTSP)

Put the URL in **`bench/camera.local`** (git-ignored, mode 600 — never commit):

```bash
ZM_RTSP_URL='rtsp://user:pass@host:554/Streaming/Channels/101'
```

`run_bench.sh` sources it automatically. Prefer `transportmode=unicast` for a
stable single-client pull.

## Artifacts

`bench/models/` (exported `.onnx`) and `bench/clips/` (generated video) are
git-ignored. Regenerate clips with ffmpeg `testsrc2`; export the model with
`yolo export model=yolo26n.pt format=onnx nms=True imgsz=640` (output `[1,300,6]`).

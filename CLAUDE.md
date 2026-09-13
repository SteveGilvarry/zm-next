# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

ZM-Next is a plugin-based video pipeline engine (a next-generation ZoneMinder core). The `zm-core`
executable loads a pipeline described in JSON, dynamically loads plugins (`dlopen`), wires them into a
capture → decode → detect → output graph, and runs each in its own thread. Plugins are independent
shared libraries communicating through a stable C ABI.

## Build & Test

The project uses CMake and depends on Homebrew packages (FFmpeg ≥ 7.0, onnxruntime, xsimd,
nlohmann-json, Boost, mosquitto). vcpkg is optional: the build uses it when `VCPKG_ROOT` is set or
`~/vcpkg` exists (on Linux it supplies an FFmpeg built with VAAPI for the VAAPI backend).
After an Xcode or Homebrew FFmpeg upgrade, a stale `build/CMakeCache.txt` can pin old SDK or library
paths; `./build.sh clean` fixes it.

```bash
./build.sh            # configure (Debug) + build into build/
./build.sh clean      # wipe build/ and rebuild
./build.sh test       # build then run ctest
```

Manual equivalent:

```bash
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Debug ..
make -j$(sysctl -n hw.ncpu)
ctest --output-on-failure          # all tests
ctest -R ShmRingTest --output-on-failure   # single test by name
```

Core test names (registered via `add_test`): `ShmRingTest`, `EventBusTest`, `PluginManagerTest`,
`PipelineLoaderTest`, `MonitorTest`. Plugins register their own gtest executables under their `tests/`
subdirs. Tests use GoogleTest, fetched automatically via CMake `FetchContent`.

### Running the engine

```bash
# Run from the build/ directory — plugin paths are resolved relative to CWD as
# plugins/<kind>/<kind>.dylib (see path resolution note below).
cd build
./zm-core --pipeline ../pipelines/e2e_file_cascade.template.json --socket /tmp/zm.sock --monitor-id 1
./wl_dump /tmp/zm.sock 7                      # watch Hello/Media/Event on the worker socket
./zm-core --pipelines-dir ../pipelines        # picks first .json found
```

## Architecture

### Plugin ABI (`core/include/zm_plugin.h`)

The contract between core and every plugin. A plugin is a shared library exporting a single C symbol
`zm_plugin_init` (`ZM_PLUGIN_EXPORT_SYMBOL`) that fills a `zm_plugin_t`:

- `type` — one of `ZM_PLUGIN_INPUT / PROCESS / DETECT / OUTPUT / STORE`.
- `start(plugin, host, host_ctx, json_cfg)` / `stop(plugin)` — lifecycle.
- `on_frame(plugin, buf, size)` — receives a frame as a single buffer `[zm_frame_hdr_t][payload]`.
- `instance` — plugin-private state pointer.

The host passes a `zm_host_api_t` table (log, `publish_evt`, `on_frame`) so plugins can log, emit
metadata events, and forward frames downstream. `zm_frame_hdr_t` carries `stream_id`, `hw_type`
(CPU/CUDA/VAAPI/VideoToolbox/DXVA), a `handle` (CPU packet pointer or GPU surface id), `bytes`,
keyframe `flags`, and `pts_usec`. Compressed vs. uncompressed formats are signalled via the high-valued
`hw_type` enum values (`ZM_FRAME_COMPRESSED`, `ZM_FRAME_RGB24`, `ZM_FRAME_GRAYSCALE`, `ZM_FRAME_YUV420P`).

Plugin logging/event helpers (`zm_plugin_log_info`, `ZM_LOG_*` C++ macros, `zm_plugin_publish_event`,
`zm_plugin_publish_stats`) are declared here and implemented in `core/src/plugin_utils.cpp`. Call
`zm_plugin_set_log_context(host, host_ctx)` in your plugin's `start` before using them. See
`docs/Plugin_Logging_Standards.md`.

### Core library (`core/`, builds `libzmcore.a`)

- **PipelineLoader** — parses a pipeline from a JSON file (pushed by the orchestrating daemon; zm-next
  has no DB connection). JSON uses a recursive
  tree of plugin nodes, each with `id`, `kind` (or explicit `path`), `cfg` (or `config`), and `children`;
  the loader **flattens** this tree into an ordered `vector<PluginConfig>`. When `kind` is given (not an
  explicit `path`), the `.so`/`.dylib` is resolved as `plugins/<kind>/<kind><ext>` **relative to the
  working directory** — hence run `zm-core` from `build/`.
- **PluginManager** — `dlopen`s each plugin, calls `zm_plugin_init`, owns the `ShmRing` and
  `CaptureThread`, and drives `startAll()` / `stopAll()`.
- **CaptureThread** — runs the input plugin, pushes captured frames into a `ShmRing`, and fans them out
  to output plugins.
- **ShmRing** — lock-free shared-memory ring buffer (Boost.Interprocess, named `zm_shmring`) for
  cross-process/thread frame transport.
- **EventBus** — thread-safe in-process singleton pub/sub (`EventBus::instance()`) for metadata events.
- **WorkerLink** (`core/src/WorkerLink.cpp`) — the per-monitor Unix-socket server speaking the canonical
  stream-socket protocol (`core/src/stream_socket_protocol.cpp`): media + EVENT push, optional control.

### Plugins (`plugins/`, each builds a `SHARED`/`MODULE` lib with `PREFIX ""`)

Registered in `plugins/CMakeLists.txt`. Capture/decode/output plugins consume the parent's FFmpeg
settings via the cached `ZM_FFMPEG_INCLUDES` / `ZM_FFMPEG_LIBDIRS` / `ZM_FFMPEG_LIBS` variables.

- `capture_rtsp_multi` — multi-stream RTSP input (`stream_manager`).
- `decode_ffmpeg` — FFmpeg-based decode to rgb24/etc.
- `zones` — zone definitions + Boost.Geometry R-tree spatial indexing (ZoneMinder-format compatible).
- `motion_pixel_diff` — SIMD (xsimd) pixel-difference motion detection; reads zones output.
- `motion_hybrid` — older combined motion plugin (see its `MIGRATION_GUIDE.md`).
- `decode_detect` — fused hardware decode + on-GPU motion gate + detect (CUDA / Metal+ANE /
  VAAPI / Vulkan backends in `plugins/detect_onnx/hw_backend_*`); see `docs/GPU_Pipeline.md`.
- `detect_onnx`, `detect_pose`, `detect_seg`, `detect_openvocab`, `recognize_face`, `lpr`,
  `audio_detect`, `tracker`, `analytics_rules` (incl. pose-based fall), `alert_policy`,
  `describe_vlm`, `llm_event_review` — the AI tier; config keys in `docs/Plugin_Config_Reference.md`.
- `privacy_mask` — static polygons and dynamic person/face/plate masking from detection events.
- `output_mqtt`, `output_webhook` — event outputs. There is no live-video output plugin: media goes
  over the worker socket to zm-api, which serves WebRTC / HLS / MSE. (`output_webrtc`, `output_mse`
  and the Node `signaling/` bridge were removed 2026-09-13.)
- `store` — unified recorder; `mode` = `continuous` (time-rotated segments) | `event`
  (triggered pre/post-roll clips) | `both`. Writes media to disk and runs the zm-api
  event-id assignment handshake (recording_opening → assign_recording → EventClip).
  Replaces the former `store_filesystem` + `store_event`.
- `hello` — minimal reference plugin / `PluginManager` test fixture.

The modular motion design (`zones` → `motion_pixel_diff` → output, replacing monolithic motion plugins)
is documented in `docs/Motion_Architecture.md`.

### Pipelines (`pipelines/*.json`)

Declarative pipeline graphs. Note `pipelines/` is gitignored except `*.template.json` (configs may
contain camera credentials in `rtsp://` URLs). `e2e_file_cascade.template.json` is the camera-free
reference (file → decode → motion → detect + recording, observed on the worker socket); the others
cover GPU detect, fall alerts, person alerts and production layouts.

## Conventions

- Core builds with C++20 and `-Wall -Werror`; plugins generally C++17/20. Release adds
  `-O3 -march=native -flto`.
- Plugins set `PREFIX ""` (so output is `name.dylib`, not `libname.dylib`) and hide symbols
  (`-fvisibility=hidden`) except the exported `zm_plugin_init`.
- On Apple, plugins build as `SHARED`; on Linux as `MODULE`.
- SIMD is gated by the `ZMP_USE_SIMD` option (default ON, defines `-DZMP_USE_SIMD`, requires xsimd).

# CLAUDE.md - AI Assistant Guide for zm-next

## Project Overview

zm-next is a next-generation modular video surveillance system inspired by ZoneMinder. It uses a plugin-based pipeline architecture for real-time video capture, decoding, motion detection, and streaming (WebRTC/MSE). Written primarily in C++20 with a C plugin ABI.

## Repository Structure

```
zm-next/
├── core/                   # Core framework library (libzmcore.a)
│   ├── include/zm/         # Public C++ headers (namespace zm)
│   ├── include/zm_plugin.h # C plugin interface (ABI boundary)
│   ├── src/                # Core implementations
│   └── tests/              # Unit tests (Google Test)
├── plugins/                # Plugin ecosystem (shared libraries)
│   ├── hello/              # Example/template plugin
│   ├── capture_rtsp_multi/ # INPUT: Multi-stream RTSP capture
│   ├── decode_ffmpeg/      # PROCESS: FFmpeg video decoding
│   ├── zones/              # PROCESS: Spatial zone management
│   ├── motion_pixel_diff/  # DETECT: SIMD pixel-diff motion detection
│   ├── motion_hybrid/      # DETECT: Combined motion (deprecated)
│   ├── output_webrtc/      # OUTPUT: WebRTC streaming
│   ├── output_mse/         # OUTPUT: Media Source Extensions streaming
│   └── store_filesystem/   # STORE: Disk persistence
├── src/zm-core.cpp         # Main executable entry point
├── docs/                   # Architecture and integration docs
├── tests/                  # Integration and manual test files
├── CMakeLists.txt          # Root CMake build
└── build.sh                # Build helper script
```

## Build System

**Requirements**: CMake 3.16+, vcpkg, C++20 compiler, FFmpeg 7.0+, Boost 1.70+

### Quick Build

```bash
./build.sh          # Configure and build (auto-detects vcpkg)
./build.sh clean    # Clean rebuild
./build.sh test     # Build and run all tests
```

### Manual Build

```bash
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Debug ..
make -j$(nproc)
```

### Build Outputs

- `build/zm-core` - Main executable
- `build/libzmcore.a` - Core static library
- `build/plugins/*.so` (Linux) / `*.dylib` (macOS) - Plugin shared libraries

### Key CMake Options

- `-DZMP_USE_SIMD=ON` (default ON) - Enable xsimd SIMD optimizations
- `-DCMAKE_BUILD_TYPE=Debug|Release` - Debug: `-g -O0`, Release: `-O3 -march=native -flto`

### Dependencies (via vcpkg)

- FFmpeg 7.0+ (libavformat>=61, libavcodec>=61, libavutil>=59)
- Boost (Asio, Geometry, Thread)
- nlohmann_json
- SQLite3
- Google Test 1.12.0 (fetched via FetchContent)
- xsimd (header-only SIMD)
- LibDataChannel (WebRTC)

## Running Tests

```bash
# From build directory
ctest --output-on-failure

# Individual tests
./build/test_shmring
./build/test_eventbus
./build/test_plugin_manager
./build/test_pipeloader
./build/test_monitor
```

### Test Names (ctest)

- `ShmRingTest` - Shared memory ring buffer
- `EventBusTest` - Pub/sub event system
- `PluginManagerTest` - Dynamic plugin loading
- `PipelineLoaderTest` - JSON/DB pipeline parsing
- `MonitorTest` - Monitor instance management

## Architecture

### Data Flow

```
RTSP Camera → capture_rtsp_multi (INPUT)
    → decode_ffmpeg (PROCESS)
    → zones (PROCESS)
    → motion_pixel_diff (DETECT)
    → output_webrtc / output_mse / store_filesystem (OUTPUT)
```

### Core Components

| Component | File | Purpose |
|-----------|------|---------|
| PluginManager | `core/src/PluginManager.cpp` | Dynamic plugin loading via dlopen, pipeline lifecycle |
| PipelineLoader | `core/src/PipelineLoader.cpp` | JSON or SQLite pipeline config parsing |
| EventBus | `core/src/EventBus.cpp` | Thread-safe singleton pub/sub for plugin events |
| ShmRing | `core/src/ShmRing.cpp` | Shared memory ring buffer for frame passing |
| CaptureThread | `core/src/CaptureThread.cpp` | Frame capture coordination |

### Plugin Types

Defined in `zm_plugin.h` as `zm_plugin_type_t`:
- `ZM_PLUGIN_INPUT` - Frame sources (cameras, files)
- `ZM_PLUGIN_PROCESS` - Frame transformation (decode, zone filtering)
- `ZM_PLUGIN_DETECT` - Analysis (motion detection)
- `ZM_PLUGIN_OUTPUT` - Streaming outputs (WebRTC, MSE)
- `ZM_PLUGIN_STORE` - Persistence (filesystem, database)

### Frame Format

Frames are passed as `[zm_frame_hdr_t][payload]` buffers via `on_frame()`:

```c
typedef struct zm_frame_hdr_s {
    uint32_t stream_id;   // Stream identifier
    uint32_t hw_type;     // ZM_HW_CPU, ZM_HW_CUDA, ZM_HW_VAAPI, etc.
    uint64_t handle;      // CPU: data pointer; GPU: surface ID
    uint32_t bytes;       // Payload size
    uint32_t flags;       // Keyframe flags
    uint64_t pts_usec;    // Presentation timestamp (microseconds)
} zm_frame_hdr_t;
```

## Code Conventions

### Naming

- **Namespace**: All core C++ code in `namespace zm`
- **Classes**: PascalCase (`PluginManager`, `EventBus`, `PipelineLoader`)
- **Member variables**: trailing underscore (`mutex_`, `subscribers_`, `pipeline_`)
- **C API**: `zm_` prefix, snake_case (`zm_plugin_init`, `zm_frame_hdr_t`, `zm_host_api_t`)
- **Enums**: `ZM_` prefix, UPPER_SNAKE_CASE (`ZM_LOG_INFO`, `ZM_PLUGIN_INPUT`)
- **Files**: snake_case for plugins (`capture_rtsp_multi.cpp`), PascalCase for core (`PluginManager.cpp`)

### Headers

- Use `#pragma once` for include guards
- Public core headers in `core/include/zm/` (`.hpp`)
- Plugin C interface in `core/include/zm_plugin.h` (`.h`)

### Plugin Development

Every plugin must export `zm_plugin_init` (see `hello/hello.cpp` for template):

```cpp
extern "C" void zm_plugin_init(zm_plugin_t* plugin) {
    plugin->type = ZM_PLUGIN_OUTPUT;  // set type
    plugin->instance = /* context */;
    plugin->start = my_start;         // lifecycle
    plugin->stop = my_stop;
    plugin->on_frame = my_on_frame;   // frame handler
}
```

### Logging

Plugins use the standardized logging API from `zm_plugin.h`:

```cpp
// In start(): set up logging context
zm_plugin_set_log_context(host_api, host_ctx);

// Then use throughout:
zm_plugin_log_info("Processing stream %u", stream_id);
zm_plugin_log_error("Failed: %s", error_msg);

// Or C++ macros:
ZM_LOG_INFO("message");
ZM_LOG_WARN("message");
```

### Compiler Settings

- C++20 standard (`cxx_std_20`)
- `-Wall -Werror` on core library (warnings are errors)
- Tests link against `GTest::gtest_main`

## Key Files to Know

| File | Why It Matters |
|------|---------------|
| `core/include/zm_plugin.h` | Plugin ABI contract - the interface all plugins implement |
| `core/include/zm/PluginManager.hpp` | How plugins are loaded and managed |
| `core/include/zm/PipelineLoader.hpp` | Pipeline configuration loading |
| `core/include/zm/EventBus.hpp` | Inter-plugin communication |
| `plugins/hello/hello.cpp` | Reference plugin implementation |
| `src/zm-core.cpp` | Application entry point |
| `CMakeLists.txt` | Root build config with vcpkg/FFmpeg/SIMD setup |
| `build.sh` | Build automation script |

## Common Tasks

### Adding a New Plugin

1. Create `plugins/<name>/` with `<name>.cpp` and `CMakeLists.txt`
2. Implement `zm_plugin_init()` exporting the `zm_plugin_t` struct
3. Add `add_subdirectory(<name>)` to `plugins/CMakeLists.txt`
4. Plugin CMake should build a `MODULE` library linking against `zmcore`

### Adding a Core Test

1. Add `tests/test_<component>.cpp` in `core/tests/`
2. Use Google Test framework with `TEST()` macros
3. Register in `core/CMakeLists.txt` with `add_executable` + `add_test`
4. Link against `zmcore GTest::gtest_main Threads::Threads`

## Documentation

- `docs/Motion_Architecture.md` - Modular motion detection design
- `docs/Plugin_Logging_Standards.md` - Unified logging API guide
- `docs/WebRTC_FFI_API_Guide.md` - Rust FFI for WebRTC
- `docs/Rust_WebRTC_FFI_Instructions.md` - Rust integration guide
- `MSE_IPC_Integration_Guide.md` - TCP IPC protocol for MSE streaming
- Plugin-specific READMEs in each plugin directory

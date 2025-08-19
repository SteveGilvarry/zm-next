# MSE (Media Source Extensions) Output Plugin

This plugin provides a Media Source Extensions compatible output for H.264 video streams from the zm-next pipeline. It's designed to work with web-based frontends that need to stream video data to browsers using MSE.

## Features

- **Multi-stream support**: Handle multiple camera streams in a single plugin instance
- **Thread-safe buffering**: Concurrent access from pipeline and external APIs
- **Buffer management**: Automatic dropping of old segments to prevent memory overflow
- **Statistics tracking**: Monitor buffer status, dropped frames, and throughput
- **C API**: Clean C interface for integration with Rust frontends and other languages

## Architecture

The plugin consists of:

1. **MSESegmentBuffer**: Thread-safe queue for media segments per stream
2. **MSEService**: Singleton service managing multiple camera streams
3. **C API**: External interface for Rust/other language integration
4. **Plugin Interface**: Standard zm-core plugin lifecycle management

## Usage

### Pipeline Integration

The plugin automatically receives frames from the zm-core pipeline when configured as an output plugin in your pipeline JSON:

```json
{
  "outputs": [
    {
      "plugin": "output_mse",
      "config": {
        "camera_id": 1,
        "stream_id": 0,
        "codec": "h264"
      }
    }
  ]
}
```

### External API Usage (Rust/C)

Include the header file and link against the plugin:

```c
#include "mse_api.h"

// Streams are automatically registered when first H.264 frame is received
// Dimensions are auto-detected from H.264 SPS

// Pull segments for streaming
uint8_t buffer[65536];
size_t size = zm_mse_try_pop_segment(1, buffer, sizeof(buffer));
if (size > 0) {
    // Send buffer to browser via MSE
    send_to_browser(buffer, size);
}

// Get statistics
uint64_t total_segments, dropped_segments;
size_t current_buffer_size = zm_mse_get_buffer_stats(1, &total_segments, &dropped_segments);
```

## Stream Management

### Registration

Streams are automatically registered when the first H.264 frame is received. The plugin auto-detects stream dimensions from the H.264 SPS (Sequence Parameter Set). Manual registration is also supported for advanced use cases:

```c
void zm_mse_register_stream(uint32_t camera_id, uint32_t stream_id, const char* codec, int width, int height);
```

- `camera_id`: Unique identifier for the camera (1, 2, 3, ...)
- `stream_id`: Stream identifier (0 for main stream, 1+ for additional streams)
- `codec`: Codec type ("h264", "aac", etc.)
- `width`/`height`: Video dimensions (0 for auto-detection from stream)

### Unregistration

When done with a stream:

```c
void zm_mse_unregister_stream(uint32_t camera_id, uint32_t stream_id);
```

## Segment Access

### Blocking Pop

Wait for the next segment (blocks if buffer is empty):

```c
size_t zm_mse_pop_segment(uint32_t camera_id, uint8_t* out, size_t max_size);
```

### Non-blocking Pop

Try to get a segment immediately:

```c
size_t zm_mse_try_pop_segment(uint32_t camera_id, uint8_t* out, size_t max_size);
```

Returns 0 if no data is available.

## Buffer Management

The plugin maintains a circular buffer of up to 100 segments per stream. When the buffer is full, the oldest segments are automatically dropped to make room for new ones.

### Buffer Statistics

```c
// Get current buffer size
size_t size = zm_mse_get_buffer_size(camera_id);

// Get detailed statistics
uint64_t total, dropped;
size_t current = zm_mse_get_buffer_stats(camera_id, &total, &dropped);

// Get throughput statistics
uint64_t bytes = zm_mse_get_bytes_received(camera_id);
uint64_t frames = zm_mse_get_frame_count(camera_id);
```

## Integration Examples

### Rust FFI

```rust
extern "C" {
    fn zm_mse_try_pop_segment(camera_id: u32, out: *mut u8, max_size: usize) -> usize;
    fn zm_mse_get_buffer_size(camera_id: u32) -> usize;
}

// Usage - streams auto-register when first frame is received
unsafe {
    let mut buffer = vec![0u8; 65536];
    let size = zm_mse_try_pop_segment(1, buffer.as_mut_ptr(), buffer.len());
    if size > 0 {
        buffer.truncate(size);
        // Send to WebRTC or MSE
    }
}
```

### JavaScript (via WebAssembly/Native Modules)

The C API can be exposed to JavaScript through WebAssembly or native Node.js modules for direct browser integration.

## Notes

- The plugin auto-detects stream dimensions from H.264 SPS (Sequence Parameter Set)
- Segments are properly fragmented into MP4 format for MSE compatibility
- Keyframe-based segmentation ensures clean segment boundaries
- The plugin assumes H.264 NAL units from the capture pipeline
- Buffer overflow protection prevents memory leaks in long-running scenarios
- Multiple cameras/streams are supported with independent buffers

## Building

The plugin is built automatically as part of the zm-next project:

```bash
mkdir -p build && cd build
cmake .. && make -j
```

The resulting `output_mse.dylib` (macOS) or `output_mse.so` (Linux) can be loaded by zm-core.

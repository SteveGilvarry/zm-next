# Multi-Stream RTSP Capture Plugin

This plugin provides multi-stream RTSP capture capabilities for the ZoneMinder-style video recording system. It can simultaneously capture from multiple IP cameras with RTSP streams while maintaining individual stream management and automatic reconnection.

## Features

- **Multi-stream support**: Capture from unlimited RTSP streams simultaneously
- **Individual stream management**: Each stream has its own configuration and state
- **Automatic reconnection**: Configurable retry logic for handling network issues
- **Hardware acceleration**: Optional hardware decoding support per stream
- **Dynamic stream management**: Add/remove streams at runtime
- **Comprehensive logging**: Per-stream logging with detailed status information

## Configuration

The plugin accepts JSON configuration in two formats:

### Single Stream (Backward Compatibility)
```json
{
    "url": "rtsp://192.168.1.100/stream1",
    "transport": "tcp",
    "hw_decode": true
}
```

### Multi-Stream Configuration
```json
{
    "streams": [
        {
            "stream_id": 0,
            "url": "rtsp://192.168.1.100/stream1",
            "transport": "tcp",
            "hw_decode": true,
            "max_retry_attempts": 5,
            "retry_delay_ms": 2000
        },
        {
            "stream_id": 1,
            "url": "rtsp://192.168.1.101/stream1",
            "transport": "udp",
            "hw_decode": false,
            "max_retry_attempts": -1,
            "retry_delay_ms": 1000
        }
    ]
}
```

### Configuration Parameters

- **stream_id**: Unique identifier for the stream (auto-assigned if not specified)
- **url**: RTSP URL of the IP camera
- **transport**: Transport protocol - "tcp" (default) or "udp"
- **hw_decode**: Enable hardware decoding (default: false)
- **max_retry_attempts**: Maximum reconnection attempts (-1 for infinite, default: 5)
- **retry_delay_ms**: Delay between retry attempts in milliseconds (default: 2000)

## Hardware Acceleration

The plugin supports hardware-accelerated decoding on supported platforms:

- **macOS**: VideoToolbox
- **Linux**: VA-API
- **Windows**: DirectX Video Acceleration (DXVA)

Hardware acceleration is automatically detected and configured when `hw_decode` is enabled for a stream.

## Stream Management

### StreamManager Class

The core of the plugin is the `StreamManager` class which handles:

- **Stream lifecycle**: Setup, connection, capture, and cleanup
- **Thread management**: Individual capture threads per stream
- **Error handling**: Automatic reconnection with configurable retry logic
- **Statistics tracking**: Frame counts, connection status, uptime

### Stream States

Each stream can be in one of several states:

- **Disconnected**: Not connected, attempting to connect
- **Connected**: Successfully connected and capturing frames
- **Retrying**: Connection failed, waiting for retry
- **Stopped**: Manually stopped or max retries reached

## Output

The plugin publishes frames using the ZoneMinder frame format:

```c
typedef struct zm_frame_hdr_s {
    uint32_t stream_id;        // Stream identifier
    uint32_t hw_type;          // Hardware surface type
    uint64_t handle;           // Packet data pointer
    uint32_t bytes;            // Packet size
    uint32_t flags;            // Keyframe flag
    uint64_t pts_usec;         // Timestamp in microseconds
} zm_frame_hdr_t;
```

## Architecture

```
capture_rtsp_multi.cpp
├── Plugin Interface (zm_plugin_init)
├── FFmpeg Integration
└── StreamManager
    ├── Stream Configuration Parser
    ├── Per-Stream Capture Threads
    ├── Hardware Acceleration Setup
    ├── Automatic Reconnection Logic
    └── Frame Publishing Pipeline
```

## Building

The plugin is built as part of the main zm-next project:

```bash
mkdir -p build && cd build
cmake .. && make -j
```

The plugin will be built as `libcapture_rtsp_multi.so` (Linux) or `libcapture_rtsp_multi.dylib` (macOS).

## Usage Example

For a complete pipeline example, see `/pipelines/multi_rtsp_to_filesystem.json`.

```cpp
// Load plugin
zm_plugin_t plugin;
zm_plugin_init(&plugin);

// Configuration for two cameras
const char* config = R"({
    "streams": [
        {
            "stream_id": 0,
            "url": "rtsp://camera1.local/stream1",
            "transport": "tcp",
            "hw_decode": true
        },
        {
            "stream_id": 1,
            "url": "rtsp://camera2.local/stream1", 
            "transport": "tcp",
            "hw_decode": true
        }
    ]
})";

// Start plugin
plugin.start(&plugin, &host_api, host_ctx, config);

// Plugin will now capture from both streams simultaneously
// Frames will be delivered via host_api.on_frame callback

// Stop plugin
plugin.stop(&plugin);
```

## Monitoring

The StreamManager provides comprehensive statistics for monitoring:

- **Connection status** per stream
- **Frame capture counts**
- **Retry attempts and failures**
- **Stream uptime**
- **Hardware acceleration status**

## Differences from Single-Stream Plugin

- **Scalability**: Supports unlimited streams vs single stream
- **Management**: Centralized stream management with StreamManager class
- **Configuration**: Flexible JSON configuration supporting both single and multi-stream
- **Threading**: Individual threads per stream for better isolation
- **Statistics**: Comprehensive per-stream statistics and monitoring
- **Robustness**: Enhanced error handling and reconnection logic

## Future Enhancements

- **Dynamic reconfiguration**: Hot-reload configuration changes
- **Load balancing**: Distribute streams across multiple threads
- **Stream prioritization**: Quality of service controls
- **Advanced statistics**: Bandwidth monitoring, frame rate analysis
- **Web interface**: Real-time monitoring dashboard

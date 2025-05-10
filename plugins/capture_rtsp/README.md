# RTSP Input Plugin for ZoneMinder Next

This plugin provides RTSP input capabilities for ZoneMinder Next, using FFmpeg 7.x libraries to efficiently capture streams from IP cameras and other RTSP sources.

## Features

- Connects to RTSP URLs with H.264/H.265/AV1 video and optional AAC/G.711 audio
- Hardware acceleration support (CUDA, VA-API, VideoToolbox) when available
- Direct packet passthrough (zero-copy) for efficient processing
- Automatic reconnection with exponential backoff
- Robust error handling and logging

## Configuration

The plugin accepts a JSON configuration with the following parameters:

```json
{
  "url": "rtsp://username:password@camera-ip:port/stream",
  "transport": "tcp",
  "max_streams": 2,
  "hw_decode": true
}
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| url | string | (required) | RTSP URL to connect to |
| transport | string | "tcp" | RTSP transport protocol: "tcp" or "udp" |
| max_streams | integer | 2 | Maximum number of streams to process |
| hw_decode | boolean | true | Try to use hardware acceleration if available |

## Build Requirements

- FFmpeg 7.x+ with development headers
- C++20 compatible compiler
- CMake 3.16+

## Implementation Details

The plugin follows the ZoneMinder Next plugin architecture:

1. Streams raw packets from the camera using FFmpeg's networking layers
2. Captures compressed packets without decoding when possible
3. Uses hardware acceleration for video packet processing when available
4. Publishes frames to the host via callback functions
5. Handles reconnection with backoff when a camera goes offline

## Testing

A unit test suite is provided that can be run with either a real camera or a mock RTSP server for CI testing.

To run with a real camera:

```bash
export RTSP_TEST_URL="rtsp://username:password@camera-ip:port/stream"
cd build
ctest -R RtspPluginTest
```

To use a mock RTSP server with FFmpeg:

```bash
ffmpeg -re -stream_loop -1 -i sample.mp4 -c copy -f rtsp -rtsp_transport tcp rtsp://localhost:8554/test &
export RTSP_TEST_URL="rtsp://localhost:8554/test"
cd build
ctest -R RtspPluginTest
```

## Performance

The plugin is designed for minimum CPU usage:
- ≤ 2% CPU at 4K 30fps with software decoding
- Near-zero CPU usage with hardware decoding
- Low memory footprint

## Error Handling

The plugin logs various events to help with debugging:
- Connection successes and failures
- Stream information
- Frame statistics
- Hardware acceleration status

## License

This plugin is licensed under the same terms as ZoneMinder Next.

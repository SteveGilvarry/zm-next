# WebRTC Output Plugin

A WebRTC output plugin for ZoneMinder-style video systems that enables real-time streaming of video to web browsers and WebRTC clients using libdatachannel.

## Features

- **Real-time WebRTC streaming**: Stream H.264 video directly to web browsers
- **Multi-client support**: Handle multiple concurrent WebRTC connections
- **Stream filtering**: Support for multi-stream input with configurable filtering
- **ICE server configuration**: Configurable STUN/TURN servers for NAT traversal
- **Automatic cleanup**: Disconnect detection and client timeout handling
- **Performance monitoring**: Built-in statistics and event publishing
- **Thread-safe**: Concurrent frame processing and client management

## Configuration

```json
{
  "bind_address": "0.0.0.0",
  "port": 8080,
  "ice_servers": "[{\"urls\":\"stun:stun.l.google.com:19302\"}]",
  "stream_filter": [0, 1],
  "max_clients": 10,
  "client_timeout_seconds": 30,
  "enable_simulcast": false
}
```

### Configuration Options

- **bind_address** (string): Network interface to bind to (default: "0.0.0.0")
- **port** (integer): Port for WebRTC signaling (default: 8080)
- **ice_servers** (string): JSON array of ICE servers for NAT traversal
- **stream_filter** (array): Stream IDs to accept (empty = accept all)
- **max_clients** (integer): Maximum concurrent clients (default: 10)
- **client_timeout_seconds** (integer): Client timeout in seconds (default: 30)
- **enable_simulcast** (boolean): Enable simulcast support (default: false)

## Usage

### Basic Configuration

```json
{
  "bind_address": "127.0.0.1",
  "port": 8080
}
```

### With STUN/TURN Servers

```json
{
  "bind_address": "0.0.0.0",
  "port": 8080,
  "ice_servers": "[{\"urls\":\"stun:stun.l.google.com:19302\"},{\"urls\":\"turn:turnserver.example.com:3478\",\"username\":\"user\",\"credential\":\"pass\"}]"
}
```

### Multi-Stream Filtering

```json
{
  "bind_address": "0.0.0.0", 
  "port": 8080,
  "stream_filter": [0, 2],
  "max_clients": 5
}
```

## Pipeline Integration

The WebRTC output plugin can be integrated into pipelines to stream video in real-time:

```json
{
  "plugins": {
    "cap": {
      "kind": "capture_rtsp",
      "cfg": {
        "url": "rtsp://camera.local/stream1"
      }
    },
    "webrtc": {
      "kind": "output_webrtc", 
      "cfg": {
        "bind_address": "0.0.0.0",
        "port": 8080,
        "max_clients": 10
      }
    }
  },
  "pipeline": [
    {"from": "cap", "to": "webrtc"}
  ]
}
```

## Client Connection

WebRTC clients can connect using standard WebRTC APIs. The plugin automatically:

1. Handles WebRTC peer connection establishment
2. Manages ICE candidate exchange
3. Sets up RTP streaming for H.264 video
4. Monitors connection health and timeouts

## Events Published

The plugin publishes events via the host API:

### WebRTCStarted
```json
{
  "event": "WebRTCStarted",
  "bind_address": "0.0.0.0",
  "port": 8080,
  "max_clients": 10
}
```

### WebRTCStats (on shutdown)
```json
{
  "event": "WebRTCStats", 
  "frames_sent": 15420,
  "bytes_sent": 52428800,
  "clients_connected": 5,
  "clients_disconnected": 2
}
```

## Technical Details

### Video Codec Support
- **Primary**: H.264 (AVC)
- **Formats**: AVCC format preferred for WebRTC compatibility
- **Hardware acceleration**: Inherits from input stream

### Network Protocol
- **Transport**: WebRTC over DTLS/SRTP
- **Packetization**: RTP with H.264 payload format
- **NAT traversal**: ICE with configurable STUN/TURN servers

### Performance Characteristics
- **Low latency**: Sub-second latency for real-time monitoring
- **Scalable**: Multi-client support with efficient frame distribution
- **Memory efficient**: Bounded frame queues and automatic cleanup
- **Thread-safe**: Separate processing thread for non-blocking operation

## Dependencies

- **libdatachannel**: WebRTC implementation
- **FFmpeg**: Video codec parameter handling
- **nlohmann/json**: Configuration parsing
- **pthread**: Thread management

## Build Requirements

The plugin requires libdatachannel to be installed:

```bash
# macOS with Homebrew
brew install libdatachannel

# Ubuntu/Debian
sudo apt-get install libdatachannel-dev

# Build from source
git clone https://github.com/paullouisageneau/libdatachannel.git
cd libdatachannel
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
sudo make install
```

## Testing

Run the test suite:

```bash
cd build/plugins/output_webrtc
./test_webrtc
```

Tests cover:
- Plugin initialization and configuration
- Stream filtering functionality
- Metadata and video frame processing
- Error handling and edge cases
- Client connection management

## Troubleshooting

### Common Issues

1. **No video output**: Ensure metadata is received before video frames
2. **Connection failures**: Check ICE server configuration and network connectivity
3. **High latency**: Verify network conditions and consider local TURN server
4. **Memory usage**: Monitor frame queue depth and client connection counts

### Debug Logging

Enable debug logging to troubleshoot issues:
- Plugin logs connection events, frame processing, and errors
- Check for metadata reception before video frames
- Monitor client connection state changes

### Network Configuration

For production deployments:
- Configure appropriate TURN servers for NAT traversal
- Ensure firewall rules allow WebRTC traffic
- Consider bandwidth limitations for multiple clients
- Monitor network quality and connection stability

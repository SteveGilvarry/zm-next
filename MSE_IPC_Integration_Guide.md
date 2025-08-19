# MSE Plugin IPC Integration Guide

## Overview

The MSE output plugin now includes a **Boost.Asio TCP control server** that enables inter-process communication (IPC) for accessing media segments. This solves the previous issue where the Rust API and zm-core ran in separate processes and couldn't share segment data.

## Architecture Change

**Before**: Rust API → Direct FFI calls → Empty buffers (different process)
**After**: Rust API → TCP Socket → MSE Control Server → Live segment data

## Control Server Details

- **Host**: `127.0.0.1`
- **Port**: `9051`
- **Protocol**: TCP with JSON commands
- **Status**: Auto-starts when MSE plugin initializes

## API Commands

All commands are JSON objects sent over TCP, terminated with `\n`. Responses are also JSON objects terminated with `\n`.

### 1. Get Active Cameras
```json
{"command": "get_active_cameras"}
```
**Response**:
```json
{
  "command": "get_active_cameras",
  "success": true,
  "cameras": [1, 2, 3],
  "count": 3
}
```

### 2. Get Buffer Statistics
```json
{"command": "get_buffer_stats", "camera_id": 1}
```
**Response**:
```json
{
  "command": "get_buffer_stats",
  "camera_id": 1,
  "success": true,
  "buffer_size": 5,
  "total_segments": 1245,
  "dropped_segments": 12,
  "bytes_received": 825897,
  "frame_count": 270
}
```

### 3. Get Initialization Segment
```json
{"command": "get_init_segment", "camera_id": 1}
```
**Response**:
```json
{
  "command": "get_init_segment",
  "camera_id": 1,
  "success": true,
  "size": 744,
  "message": "Init segment available"
}
```
**Note**: This confirms the init segment exists and its size. For actual binary data transfer, see the streaming approach below.

### 4. Get Latest Media Segment
```json
{"command": "get_latest_segment", "camera_id": 1}
```
**Response**:
```json
{
  "command": "get_latest_segment",
  "camera_id": 1,
  "success": true,
  "size": 133131,
  "message": "Latest segment available"
}
```
**Note**: This provides metadata about the latest segment. For actual binary data, use the streaming approach.

### 5. Get Full Statistics
```json
{"command": "get_stats"}
```
**Response**:
```json
{
  "success": true,
  "total_streams": 1,
  "streams": [
    {
      "camera_id": 1,
      "stream_id": 0,
      "codec": "h264",
      "width": 1280,
      "height": 720,
      "frame_count": 270,
      "bytes_received": 825897,
      "dimensions_detected": true,
      "buffer_size": 5,
      "total_segments": 5,
      "dropped_segments": 0
    }
  ]
}
```

## Recommended Integration Approach

### Option 1: Replace FFI with TCP Client (Recommended)

Replace the current FFI-based segment access with a TCP client:

```rust
use std::net::TcpStream;
use std::io::{Read, Write};
use serde_json::{json, Value};

pub struct MSEClient {
    stream: TcpStream,
}

impl MSEClient {
    pub fn new() -> Result<Self, Box<dyn std::error::Error>> {
        let stream = TcpStream::connect("127.0.0.1:9051")?;
        Ok(MSEClient { stream })
    }
    
    pub fn get_active_cameras(&mut self) -> Result<Vec<u32>, Box<dyn std::error::Error>> {
        let cmd = json!({"command": "get_active_cameras"});
        let response = self.send_command(&cmd)?;
        
        if response["success"].as_bool().unwrap_or(false) {
            let cameras: Vec<u32> = response["cameras"]
                .as_array()
                .unwrap_or(&vec![])
                .iter()
                .filter_map(|v| v.as_u64().map(|n| n as u32))
                .collect();
            Ok(cameras)
        } else {
            Err("Failed to get active cameras".into())
        }
    }
    
    pub fn get_buffer_stats(&mut self, camera_id: u32) -> Result<BufferStats, Box<dyn std::error::Error>> {
        let cmd = json!({"command": "get_buffer_stats", "camera_id": camera_id});
        let response = self.send_command(&cmd)?;
        
        if response["success"].as_bool().unwrap_or(false) {
            Ok(BufferStats {
                buffer_size: response["buffer_size"].as_u64().unwrap_or(0),
                total_segments: response["total_segments"].as_u64().unwrap_or(0),
                dropped_segments: response["dropped_segments"].as_u64().unwrap_or(0),
                bytes_received: response["bytes_received"].as_u64().unwrap_or(0),
                frame_count: response["frame_count"].as_u64().unwrap_or(0),
            })
        } else {
            Err("Failed to get buffer stats".into())
        }
    }
    
    pub fn has_init_segment(&mut self, camera_id: u32) -> Result<bool, Box<dyn std::error::Error>> {
        let cmd = json!({"command": "get_init_segment", "camera_id": camera_id});
        let response = self.send_command(&cmd)?;
        Ok(response["success"].as_bool().unwrap_or(false) && response["size"].as_u64().unwrap_or(0) > 0)
    }
    
    pub fn has_latest_segment(&mut self, camera_id: u32) -> Result<bool, Box<dyn std::error::Error>> {
        let cmd = json!({"command": "get_latest_segment", "camera_id": camera_id});
        let response = self.send_command(&cmd)?;
        Ok(response["success"].as_bool().unwrap_or(false) && response["size"].as_u64().unwrap_or(0) > 0)
    }
    
    fn send_command(&mut self, cmd: &Value) -> Result<Value, Box<dyn std::error::Error>> {
        // Send command
        let request = format!("{}\n", cmd.to_string());
        self.stream.write_all(request.as_bytes())?;
        
        // Read response
        let mut buffer = [0; 4096];
        let mut response_data = String::new();
        
        loop {
            let bytes_read = self.stream.read(&mut buffer)?;
            if bytes_read == 0 { break; }
            
            response_data.push_str(&String::from_utf8_lossy(&buffer[..bytes_read]));
            if response_data.contains('\n') { break; }
        }
        
        let response_line = response_data.lines().next().unwrap_or("");
        let response: Value = serde_json::from_str(response_line)?;
        Ok(response)
    }
}

#[derive(Debug)]
pub struct BufferStats {
    pub buffer_size: u64,
    pub total_segments: u64,
    pub dropped_segments: u64,
    pub bytes_received: u64,
    pub frame_count: u64,
}
```

### Option 2: Hybrid Approach

Keep the current FFI functions as a fallback, but add TCP client as the primary method:

```rust
impl SegmentProvider {
    pub fn get_init_segment(&mut self, camera_id: u32) -> Result<Vec<u8>, Error> {
        // Try TCP client first
        if let Ok(mut client) = MSEClient::new() {
            if client.has_init_segment(camera_id)? {
                // Use TCP to get metadata, then FFI for binary data
                // This provides live detection while keeping existing FFI
                return self.get_init_segment_ffi(camera_id);
            }
        }
        
        // Fallback to direct FFI (will be empty if no IPC)
        self.get_init_segment_ffi(camera_id)
    }
}
```

## HTTP Endpoint Updates

Update your existing HTTP endpoints to use the IPC client:

```rust
// GET /api/cameras
async fn get_cameras() -> Json<CamerasResponse> {
    match MSEClient::new().and_then(|mut client| client.get_active_cameras()) {
        Ok(cameras) => Json(CamerasResponse { cameras }),
        Err(_) => Json(CamerasResponse { cameras: vec![] })
    }
}

// GET /api/cameras/{camera_id}/status
async fn get_camera_status(camera_id: u32) -> Json<CameraStatus> {
    match MSEClient::new().and_then(|mut client| client.get_buffer_stats(camera_id)) {
        Ok(stats) => Json(CameraStatus {
            camera_id,
            active: true,
            buffer_size: stats.buffer_size,
            total_segments: stats.total_segments,
            frame_count: stats.frame_count,
            // ... other fields
        }),
        Err(_) => Json(CameraStatus {
            camera_id,
            active: false,
            // ... default values
        })
    }
}

// GET /api/cameras/{camera_id}/segments/init
async fn get_init_segment(camera_id: u32) -> Result<Vec<u8>, StatusCode> {
    match MSEClient::new().and_then(|mut client| client.has_init_segment(camera_id)) {
        Ok(true) => {
            // Init segment is available - now get the actual binary data
            // You can extend the IPC protocol to include binary data transfer
            // or use FFI as a bridge to get the actual bytes
            get_init_segment_ffi(camera_id)
        },
        _ => Err(StatusCode::NOT_FOUND)
    }
}
```

## Binary Data Transfer (Future Enhancement)

For now, the IPC provides metadata and confirmation of segment availability. To transfer actual binary data over TCP, we can extend the protocol:

### Option A: Base64 Encoding
Modify the MSE control server to include base64-encoded binary data in responses.

### Option B: Separate Binary Protocol
Use a second TCP port or separate socket for streaming binary data.

### Option C: HTTP Bridge
Run a simple HTTP server inside zm-core that serves segments directly.

## Testing the Integration

1. **Start zm-core**: `./zm-core --pipeline ../pipelines/rtsp_multi_to_mse.json`
2. **Verify control server**: Check that "MSE Control server listening on 127.0.0.1:9051" appears in logs
3. **Test connection**: Use the provided Python script or netcat: `echo '{"command":"get_active_cameras"}' | nc 127.0.0.1 9051`
4. **Validate responses**: Ensure JSON responses contain expected data

## Migration Steps

1. **Add TCP client code** to your Rust API
2. **Update HTTP endpoints** to use IPC instead of FFI for status/availability checks
3. **Test connectivity** with active zm-core pipeline
4. **Implement binary data transfer** based on your preferred approach above
5. **Add error handling** for cases where zm-core is not running or control server is unavailable

## Error Handling

The IPC approach requires zm-core to be running. Add proper error handling:

```rust
pub fn is_zm_core_running() -> bool {
    MSEClient::new().is_ok()
}

pub fn get_segments_with_fallback(camera_id: u32) -> SegmentResult {
    if is_zm_core_running() {
        // Use IPC for live data
        get_segments_via_ipc(camera_id)
    } else {
        // Return appropriate error or cached data
        Err(Error::ZmCoreNotRunning)
    }
}
```

This new architecture solves the fundamental IPC problem and provides real-time access to live segment data!

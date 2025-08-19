# Rust API Instructions for WebRTC FFI Interface

## Overview
The WebRTC service is a centralized C++ plugin (`output_webrtc.dylib`) that manages multiple camera streams through a single service instance. It exposes an FFI interface for external interaction, particularly designed for Rust API integration.

## Current Service Status
**IMPORTANT**: There is currently a WebRTC service running with:
- **Service Port**: 8080 (localhost)
- **Active Camera**: Camera ID 0 (4K RTSP stream from 192.168.0.235)
- **Library Path**: `/Users/stevengilvarry/Code/zm-next/build/plugins/output_webrtc/output_webrtc.dylib`
- **Process**: Multiple zm-core instances running the service

## FFI Interface Functions

### 1. Service Management

#### Discovery and Status
```rust
// Get list of all active camera streams
extern "C" fn zm_webrtc_list_camera_streams() -> *mut c_char;

// Get overall service statistics
extern "C" fn zm_webrtc_get_stats(
    total_frames: *mut u64,
    total_bytes: *mut u64, 
    clients_connected: *mut u64,
    clients_disconnected: *mut u64
);
```

#### Service Lifecycle
```rust
// Initialize service for a camera (usually called internally)
extern "C" fn zm_webrtc_init_service(camera_id: u32) -> c_int;

// Shutdown service gracefully
extern "C" fn zm_webrtc_shutdown_service(camera_id: u32) -> c_int;
```

### 2. Stream Management

#### Stream Registration
```rust
// Register a new camera stream
extern "C" fn zm_webrtc_register_stream(camera_id: u32) -> c_int;

// Unregister a camera stream  
extern "C" fn zm_webrtc_unregister_stream(camera_id: u32) -> c_int;

// Push frame data to a camera stream
extern "C" fn zm_webrtc_push_frame(
    camera_id: u32,
    frame_data: *const u8,
    size: usize
) -> c_int;
```

### 3. Client Management (WebRTC Signaling)

#### Client Lifecycle
```rust
// Create new WebRTC client for specific camera
extern "C" fn zm_webrtc_create_client(
    client_id: *const c_char,
    camera_id: u32
) -> *mut c_void;

// Remove WebRTC client
extern "C" fn zm_webrtc_remove_client(client_id: *const c_char) -> c_int;

// Get WebRTC connection state
extern "C" fn zm_webrtc_get_connection_state(client_id: *const c_char) -> c_int;
```

#### WebRTC Signaling
```rust
// Set SDP offer from client, returns SDP answer
extern "C" fn zm_webrtc_set_offer(
    client_id: *const c_char,
    offer_sdp: *const c_char
) -> *mut c_char;

// Add ICE candidate from client
extern "C" fn zm_webrtc_add_ice_candidate(
    client_id: *const c_char,
    candidate_str: *const c_char,
    sdp_mid: *const c_char
) -> c_int;
```

### 4. Memory Management
```rust
// Free strings returned by FFI functions
extern "C" fn zm_webrtc_free_string(str: *mut c_char);
```

## Rust Implementation Example

### Step 1: Dynamic Library Loading
```rust
use libloading::{Library, Symbol};
use std::ffi::{CString, CStr};
use std::os::raw::{c_char, c_int, c_void};
use serde_json::Value;

pub struct WebRTCService {
    lib: Library,
}

impl WebRTCService {
    pub fn new() -> Result<Self, Box<dyn std::error::Error>> {
        let lib = unsafe {
            Library::new("/Users/stevengilvarry/Code/zm-next/build/plugins/output_webrtc/output_webrtc.dylib")?
        };
        Ok(WebRTCService { lib })
    }
}
```

### Step 2: Stream Discovery
```rust
impl WebRTCService {
    pub fn discover_streams(&self) -> Result<Vec<CameraStream>, Box<dyn std::error::Error>> {
        let list_streams: Symbol<unsafe extern "C" fn() -> *mut c_char> = 
            unsafe { self.lib.get(b"zm_webrtc_list_camera_streams")? };
        
        let json_ptr = unsafe { list_streams() };
        if json_ptr.is_null() {
            return Ok(Vec::new());
        }
        
        let json_str = unsafe { CStr::from_ptr(json_ptr).to_str()? };
        let streams: Value = serde_json::from_str(json_str)?;
        
        // Free the C string
        let free_string: Symbol<unsafe extern "C" fn(*mut c_char)> = 
            unsafe { self.lib.get(b"zm_webrtc_free_string")? };
        unsafe { free_string(json_ptr); }
        
        let mut camera_streams = Vec::new();
        if let Value::Array(stream_array) = streams {
            for stream in stream_array {
                if let Some(camera_id) = stream["camera_id"].as_u64() {
                    camera_streams.push(CameraStream {
                        camera_id: camera_id as u32,
                        frames_sent: stream["frames_sent"].as_u64().unwrap_or(0),
                        bytes_sent: stream["bytes_sent"].as_u64().unwrap_or(0),
                        has_metadata: stream["has_metadata"].as_bool().unwrap_or(false),
                    });
                }
            }
        }
        
        Ok(camera_streams)
    }
}

#[derive(Debug)]
pub struct CameraStream {
    pub camera_id: u32,
    pub frames_sent: u64,
    pub bytes_sent: u64,
    pub has_metadata: bool,
}
```

### Step 3: Service Statistics
```rust
impl WebRTCService {
    pub fn get_service_stats(&self) -> Result<ServiceStats, Box<dyn std::error::Error>> {
        let get_stats: Symbol<unsafe extern "C" fn(*mut u64, *mut u64, *mut u64, *mut u64)> = 
            unsafe { self.lib.get(b"zm_webrtc_get_stats")? };
        
        let mut total_frames = 0u64;
        let mut total_bytes = 0u64;
        let mut clients_connected = 0u64;
        let mut clients_disconnected = 0u64;
        
        unsafe {
            get_stats(
                &mut total_frames,
                &mut total_bytes,
                &mut clients_connected,
                &mut clients_disconnected,
            );
        }
        
        Ok(ServiceStats {
            total_frames,
            total_bytes,
            clients_connected,
            clients_disconnected,
        })
    }
}

#[derive(Debug)]
pub struct ServiceStats {
    pub total_frames: u64,
    pub total_bytes: u64,
    pub clients_connected: u64,
    pub clients_disconnected: u64,
}
```

### Step 4: WebRTC Client Management
```rust
impl WebRTCService {
    pub fn create_webrtc_client(&self, client_id: &str, camera_id: u32) -> Result<WebRTCClient, Box<dyn std::error::Error>> {
        let create_client: Symbol<unsafe extern "C" fn(*const c_char, u32) -> *mut c_void> = 
            unsafe { self.lib.get(b"zm_webrtc_create_client")? };
        
        let client_id_c = CString::new(client_id)?;
        let client_ptr = unsafe { create_client(client_id_c.as_ptr(), camera_id) };
        
        if client_ptr.is_null() {
            return Err("Failed to create WebRTC client".into());
        }
        
        Ok(WebRTCClient {
            id: client_id.to_string(),
            camera_id,
            _ptr: client_ptr,
        })
    }
    
    pub fn handle_webrtc_offer(&self, client_id: &str, offer_sdp: &str) -> Result<String, Box<dyn std::error::Error>> {
        let set_offer: Symbol<unsafe extern "C" fn(*const c_char, *const c_char) -> *mut c_char> = 
            unsafe { self.lib.get(b"zm_webrtc_set_offer")? };
        
        let client_id_c = CString::new(client_id)?;
        let offer_sdp_c = CString::new(offer_sdp)?;
        
        let answer_ptr = unsafe { set_offer(client_id_c.as_ptr(), offer_sdp_c.as_ptr()) };
        if answer_ptr.is_null() {
            return Err("Failed to process WebRTC offer".into());
        }
        
        let answer_str = unsafe { CStr::from_ptr(answer_ptr).to_str()? };
        let answer = answer_str.to_string();
        
        // Free the C string
        let free_string: Symbol<unsafe extern "C" fn(*mut c_char)> = 
            unsafe { self.lib.get(b"zm_webrtc_free_string")? };
        unsafe { free_string(answer_ptr); }
        
        Ok(answer)
    }
    
    pub fn add_ice_candidate(&self, client_id: &str, candidate: &str, sdp_mid: Option<&str>) -> Result<(), Box<dyn std::error::Error>> {
        let add_ice: Symbol<unsafe extern "C" fn(*const c_char, *const c_char, *const c_char) -> c_int> = 
            unsafe { self.lib.get(b"zm_webrtc_add_ice_candidate")? };
        
        let client_id_c = CString::new(client_id)?;
        let candidate_c = CString::new(candidate)?;
        let sdp_mid_c = sdp_mid.map(|s| CString::new(s)).transpose()?;
        
        let result = unsafe {
            add_ice(
                client_id_c.as_ptr(),
                candidate_c.as_ptr(),
                sdp_mid_c.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
            )
        };
        
        if result < 0 {
            return Err("Failed to add ICE candidate".into());
        }
        
        Ok(())
    }
}

pub struct WebRTCClient {
    pub id: String,
    pub camera_id: u32,
    _ptr: *mut c_void,
}
```

## Current Service Interaction

### Immediate Testing Steps

1. **Discover Current Camera Stream**:
```rust
let service = WebRTCService::new()?;
let streams = service.discover_streams()?;
println!("Current streams: {:?}", streams);
// Expected: Camera ID 0 with active frame processing
```

2. **Get Service Statistics**:
```rust
let stats = service.get_service_stats()?;
println!("Service stats: {:?}", stats);
// Expected: >9000 frames processed, multiple MB of data
```

3. **Create WebRTC Client for Camera 0**:
```rust
let client = service.create_webrtc_client("rust_test_client", 0)?;
println!("Created client for camera {}", client.camera_id);
```

### Expected Current State
Based on the running service, you should see:
- **Camera ID**: 0
- **Stream URL**: RTSP from 192.168.0.235 (4K H.264)
- **Frame Count**: 9000+ frames processed
- **Frame Rate**: ~30 FPS
- **Resolution**: 3840x2160 (4K)
- **Codec**: H.264
- **Frame Size**: ~800KB for keyframes

### Multi-Camera Preparation

To test multiple cameras, create additional pipeline configurations:
```json
{
  "name": "rtsp_to_webrtc_multi",
  "root": true,
  "plugins": [
    {
      "id": "capture_multi", 
      "kind": "capture_rtsp_multi",
      "cfg": {
        "streams": [
          {
            "stream_id": 0,
            "url": "rtsp://camera1_url",
            // ... camera 0 config
          },
          {
            "stream_id": 1,
            "url": "rtsp://camera2_url",
            // ... camera 1 config
          }
        ]
      },
      "children": [
        {
          "id": "webrtc_service",
          "kind": "output_webrtc",
          "cfg": {
            "bind_address": "0.0.0.0",
            "port": 8080,
            "camera_id": 0  // Service handles multiple cameras
          }
        }
      ]
    }
  ]
}
```

## Integration Architecture

### HTTP Endpoints (To Implement)
Your Rust API should expose:
- `GET /streams` - List active camera streams
- `GET /stats` - Service statistics
- `POST /webrtc/offer` - Handle WebRTC offers
- `POST /webrtc/ice` - Handle ICE candidates
- `WebSocket /signaling/{camera_id}` - Real-time signaling

### Error Handling
- All FFI functions return negative values on error
- Null pointers indicate failure or no data
- Always free C strings with `zm_webrtc_free_string`
- Check connection states regularly

### Performance Considerations
- The service handles frame queuing internally (max 10 frames per camera)
- Frame processing is threaded per camera
- Client management is thread-safe with mutexes
- Memory management is handled by the C++ service

This FFI interface provides complete programmatic control over the WebRTC service, allowing your Rust API to discover streams, manage clients, and handle WebRTC signaling while the C++ service handles the heavy lifting of frame processing and WebRTC streaming.

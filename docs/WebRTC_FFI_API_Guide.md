# WebRTC FFI API Guide for Rust Integration

## Overview

The zm-next WebRTC service provides a centralized FFI (Foreign Function Interface) for managing multiple camera streams through a single WebRTC service on port 8080. This guide explains how to interact with the FFI from Rust.

## Current Service Status & Testing

**Service Status**: ✅ RUNNING
- **Address**: `localhost:8080`
- **Active Camera**: Camera ID `0`
- **Stream Source**: `rtsp://xxxxxxx:xxxxxxxx@192.168.0.235:554/Streaming/Channels/101?transportmode=mcast&profile=Profile_1`
- **Resolution**: 3840x2160 (4K)
- **Codec**: H.264
- **Frame Processing**: Active (processing keyframes ~844KB each)

### Verifying Service Availability

Before calling FFI functions, verify the service is running:

```bash
# Test basic connectivity
curl -s http://localhost:8080/ 
# Should return "No matched path found" (indicates service is responding)

# Check if the zm-core process is running with WebRTC
ps aux | grep zm-core
# Should show the pipeline process
```

### Live Service Testing

You can test against the currently running service:

```rust
// This will work with the current running camera 0
let streams = discover_camera_streams().unwrap();
assert_eq!(streams.len(), 1);
assert_eq!(streams[0].camera_id, 0);
println!("Camera 0 stats: {} frames sent", streams[0].frames_sent);
```

## Practical Testing With Current Service

### Step 1: Verify Service Response

```bash
# Service is responding on port 8080
curl -s http://localhost:8080/
# Expected: "No matched path found" (means service is up)
```

### Step 2: Test Camera Discovery

Expected JSON response from `zm_webrtc_list_camera_streams()`:

```json
[
  {
    "camera_id": 0,
    "frames_sent": 1234,  // Incremental counter
    "bytes_sent": 98765432,  // Total bytes processed
    "has_metadata": true
  }
]
```

### Step 3: WebRTC Client Creation

```rust
// Create client for the active camera 0
let ffi = WebRTCFFI::new()?;
let client_ptr = ffi.create_client("test_client_001", 0)?;
println!("Client created: {:p}", client_ptr);
```

### Step 4: Connection Flow

1. **Create Client**: `zm_webrtc_create_client("client_001", 0)`
2. **Set Offer**: `zm_webrtc_set_offer("client_001", offer_sdp)` → Returns answer SDP
3. **Exchange ICE**: `zm_webrtc_add_ice_candidate("client_001", candidate, mid)`
4. **Monitor State**: `zm_webrtc_get_connection_state("client_001")`
5. **Clean Up**: `zm_webrtc_remove_client("client_001")`

### Expected Service Logs

When FFI functions are called, you should see logs like:
```
[PLUGIN][INFO] WebRTCService: Created client test_client_001 for camera 0
[PLUGIN][INFO] WebRTCService: Created answer for client test_client_001
[PLUGIN][DEBUG] WebRTCService: Added ICE candidate for client test_client_001
```

### Frame Processing Verification

The service is actively processing frames. Check the stats:

```rust
let stats = ffi.get_stats()?;
println!("Frames processed: {}", stats.total_frames);
// Should show incrementing numbers as frames are processed
```

### Multi-Camera Preparation

To add camera 1, create a new pipeline config:

```json
{
  "name": "rtsp_to_webrtc_camera1",
  "plugins": [{
    "id": "webrtc_service_cam1",
    "kind": "output_webrtc", 
    "cfg": {
      "camera_id": 1,
      "port": 8080  // Same service, different camera
    }
  }]
}
```

Both cameras will share the same service instance on port 8080.

## FFI Function Signatures

### Service Discovery & Management

```c
// Discover all active camera streams
char* zm_webrtc_list_camera_streams();

// Service management (called by plugins, not typically by external API)
int zm_webrtc_init_service(uint32_t camera_id);
int zm_webrtc_shutdown_service(uint32_t camera_id);

// Get service statistics
void zm_webrtc_get_stats(uint64_t* total_frames, uint64_t* total_bytes, 
                        uint64_t* clients_connected, uint64_t* clients_disconnected);

// Free allocated strings
void zm_webrtc_free_string(char* str);
```

### Stream Management

```c
// Register/unregister camera streams (for external stream injection)
int zm_webrtc_register_stream(uint32_t camera_id);
int zm_webrtc_unregister_stream(uint32_t camera_id);

// Push frame data to a camera stream (for external sources)
int zm_webrtc_push_frame(uint32_t camera_id, const uint8_t* frame_data, size_t size);
```

### WebRTC Client Management

```c
// Create a new WebRTC client for a specific camera
void* zm_webrtc_create_client(const char* client_id, uint32_t camera_id);

// WebRTC signaling
char* zm_webrtc_set_offer(const char* client_id, const char* offer_sdp);
int zm_webrtc_add_ice_candidate(const char* client_id, const char* candidate_str, const char* sdp_mid);

// Client lifecycle
int zm_webrtc_remove_client(const char* client_id);
int zm_webrtc_get_connection_state(const char* client_id);
```

## Rust FFI Bindings Example

### 1. Basic FFI Declarations

```rust
use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int, c_void};
use serde::{Deserialize, Serialize};

#[link(name = "output_webrtc")]
extern "C" {
    fn zm_webrtc_list_camera_streams() -> *mut c_char;
    fn zm_webrtc_create_client(client_id: *const c_char, camera_id: u32) -> *mut c_void;
    fn zm_webrtc_set_offer(client_id: *const c_char, offer_sdp: *const c_char) -> *mut c_char;
    fn zm_webrtc_add_ice_candidate(client_id: *const c_char, candidate: *const c_char, sdp_mid: *const c_char) -> c_int;
    fn zm_webrtc_remove_client(client_id: *const c_char) -> c_int;
    fn zm_webrtc_get_connection_state(client_id: *const c_char) -> c_int;
    fn zm_webrtc_get_stats(total_frames: *mut u64, total_bytes: *mut u64, 
                          clients_connected: *mut u64, clients_disconnected: *mut u64);
    fn zm_webrtc_free_string(str: *mut c_char);
}

#[derive(Serialize, Deserialize, Debug)]
pub struct CameraStream {
    pub camera_id: u32,
    pub frames_sent: u64,
    pub bytes_sent: u64,
    pub has_metadata: bool,
}

#[derive(Debug)]
pub struct WebRTCStats {
    pub total_frames: u64,
    pub total_bytes: u64,
    pub clients_connected: u64,
    pub clients_disconnected: u64,
}
```

### 2. Stream Discovery

```rust
pub fn discover_camera_streams() -> Result<Vec<CameraStream>, Box<dyn std::error::Error>> {
    unsafe {
        let streams_ptr = zm_webrtc_list_camera_streams();
        if streams_ptr.is_null() {
            return Ok(Vec::new());
        }
        
        let streams_cstr = CStr::from_ptr(streams_ptr);
        let streams_json = streams_cstr.to_str()?;
        let streams: Vec<CameraStream> = serde_json::from_str(streams_json)?;
        
        zm_webrtc_free_string(streams_ptr);
        Ok(streams)
    }
}

// Usage example
match discover_camera_streams() {
    Ok(streams) => {
        println!("Found {} active camera streams:", streams.len());
        for stream in streams {
            println!("  Camera {}: {} frames sent, {} bytes", 
                    stream.camera_id, stream.frames_sent, stream.bytes_sent);
        }
    }
    Err(e) => eprintln!("Error discovering streams: {}", e),
}
```

### 3. WebRTC Client Management

```rust
pub struct WebRTCClient {
    client_id: String,
    camera_id: u32,
    client_ptr: *mut c_void,
}

impl WebRTCClient {
    pub fn new(client_id: &str, camera_id: u32) -> Option<Self> {
        let client_id_cstr = CString::new(client_id).ok()?;
        
        unsafe {
            let client_ptr = zm_webrtc_create_client(client_id_cstr.as_ptr(), camera_id);
            if client_ptr.is_null() {
                return None;
            }
            
            Some(WebRTCClient {
                client_id: client_id.to_string(),
                camera_id,
                client_ptr,
            })
        }
    }
    
    pub fn set_offer(&self, offer_sdp: &str) -> Result<String, Box<dyn std::error::Error>> {
        let client_id_cstr = CString::new(&self.client_id)?;
        let offer_cstr = CString::new(offer_sdp)?;
        
        unsafe {
            let answer_ptr = zm_webrtc_set_offer(client_id_cstr.as_ptr(), offer_cstr.as_ptr());
            if answer_ptr.is_null() {
                return Err("Failed to create answer".into());
            }
            
            let answer_cstr = CStr::from_ptr(answer_ptr);
            let answer = answer_cstr.to_str()?.to_string();
            zm_webrtc_free_string(answer_ptr);
            
            Ok(answer)
        }
    }
    
    pub fn add_ice_candidate(&self, candidate: &str, sdp_mid: Option<&str>) -> Result<(), Box<dyn std::error::Error>> {
        let client_id_cstr = CString::new(&self.client_id)?;
        let candidate_cstr = CString::new(candidate)?;
        let sdp_mid_cstr = sdp_mid.map(|s| CString::new(s)).transpose()?;
        
        unsafe {
            let result = zm_webrtc_add_ice_candidate(
                client_id_cstr.as_ptr(),
                candidate_cstr.as_ptr(),
                sdp_mid_cstr.as_ref().map_or(std::ptr::null(), |s| s.as_ptr())
            );
            
            if result == 0 {
                Ok(())
            } else {
                Err("Failed to add ICE candidate".into())
            }
        }
    }
    
    pub fn get_connection_state(&self) -> i32 {
        let client_id_cstr = CString::new(&self.client_id).unwrap();
        unsafe {
            zm_webrtc_get_connection_state(client_id_cstr.as_ptr())
        }
    }
}

impl Drop for WebRTCClient {
    fn drop(&mut self) {
        let client_id_cstr = CString::new(&self.client_id).unwrap();
        unsafe {
            zm_webrtc_remove_client(client_id_cstr.as_ptr());
        }
    }
}
```

### 4. Service Statistics

```rust
pub fn get_webrtc_stats() -> WebRTCStats {
    let mut total_frames = 0u64;
    let mut total_bytes = 0u64;
    let mut clients_connected = 0u64;
    let mut clients_disconnected = 0u64;
    
    unsafe {
        zm_webrtc_get_stats(
            &mut total_frames,
            &mut total_bytes,
            &mut clients_connected,
            &mut clients_disconnected,
        );
    }
    
    WebRTCStats {
        total_frames,
        total_bytes,
        clients_connected,
        clients_disconnected,
    }
}
```

## Usage Examples for Current Running Service

### Discover Active Camera (Currently Camera 0)

```rust
// This will return camera ID 0 that's currently streaming
let streams = discover_camera_streams().unwrap();
println!("Active cameras: {:?}", streams);
// Expected output: Camera 0 with frame/byte statistics
```

### Create WebRTC Client for Camera 0

```rust
// Connect to the currently running camera stream
let client = WebRTCClient::new("client_001", 0).expect("Failed to create client");

// Handle WebRTC signaling (from browser/client)
let offer_sdp = "v=0\r\no=- 123456789 2 IN IP4 127.0.0.1\r\n..."; // Browser's offer
let answer_sdp = client.set_offer(offer_sdp).expect("Failed to create answer");

// Send answer back to browser, then handle ICE candidates
client.add_ice_candidate("candidate:1 1 UDP 2130706431 192.168.1.100 54400 typ host", Some("video")).unwrap();
```

### Monitor Service Health

```rust
let stats = get_webrtc_stats();
println!("Service stats: {} frames, {} bytes, {} clients", 
         stats.total_frames, stats.total_bytes, stats.clients_connected);
```

## HTTP Endpoints (Alternative Access)

The service also runs an HTTP server on port 8080:

- `GET /streams` - List camera streams (JSON)
- `POST /client` - Create WebRTC client
- WebRTC signaling endpoints

## Error Handling

- Functions returning `int`: 0 = success, -1 = error
- Functions returning `char*`: NULL = error, otherwise valid string (must call `zm_webrtc_free_string`)
- Functions returning `void*`: NULL = error, otherwise valid client pointer

## Memory Management

**Important**: Always call `zm_webrtc_free_string()` on any string returned by the FFI to prevent memory leaks.

## Multi-Camera Support

The service supports multiple cameras with different IDs. Each camera gets its own stream processing thread but shares the same WebRTC service instance on port 8080.

```rust
// Example: Connect to multiple cameras
let client_cam0 = WebRTCClient::new("viewer1_cam0", 0)?;  // Current camera
let client_cam1 = WebRTCClient::new("viewer1_cam1", 1)?;  // Future camera
```

## Connection States

WebRTC connection states returned by `get_connection_state()`:
- 0: New
- 1: Connecting  
- 2: Connected
- 3: Disconnected
- 4: Failed
- 5: Closed

This FFI interface provides full access to the centralized WebRTC service for building external APIs, web interfaces, and client applications.

## Compilation & Linking Setup

### Dynamic Loading Approach (Recommended)

Instead of linking directly, use dynamic loading in Rust for more flexibility:

```rust
use libloading::{Library, Symbol};
use std::ffi::{CStr, CString};

pub struct WebRTCFFI {
    #[allow(dead_code)]
    lib: Library,
}

impl WebRTCFFI {
    pub fn new() -> Result<Self, Box<dyn std::error::Error>> {
        // Load the plugin library
        let lib = unsafe {
            Library::new("./build/plugins/output_webrtc/output_webrtc.dylib")?
        };
        
        Ok(WebRTCFFI { lib })
    }
    
    pub fn list_camera_streams(&self) -> Result<Vec<CameraStream>, Box<dyn std::error::Error>> {
        unsafe {
            let func: Symbol<unsafe extern "C" fn() -> *mut std::os::raw::c_char> = 
                self.lib.get(b"zm_webrtc_list_camera_streams")?;
            
            let streams_ptr = func();
            if streams_ptr.is_null() {
                return Ok(Vec::new());
            }
            
            let streams_cstr = CStr::from_ptr(streams_ptr);
            let streams_json = streams_cstr.to_str()?;
            let streams: Vec<CameraStream> = serde_json::from_str(streams_json)?;
            
            // Free the string
            let free_func: Symbol<unsafe extern "C" fn(*mut std::os::raw::c_char)> = 
                self.lib.get(b"zm_webrtc_free_string")?;
            free_func(streams_ptr);
            
            Ok(streams)
        }
    }
    
    pub fn create_client(&self, client_id: &str, camera_id: u32) -> Result<*mut std::ffi::c_void, Box<dyn std::error::Error>> {
        unsafe {
            let func: Symbol<unsafe extern "C" fn(*const std::os::raw::c_char, u32) -> *mut std::ffi::c_void> = 
                self.lib.get(b"zm_webrtc_create_client")?;
            
            let client_id_cstr = CString::new(client_id)?;
            let client_ptr = func(client_id_cstr.as_ptr(), camera_id);
            
            if client_ptr.is_null() {
                Err("Failed to create client".into())
            } else {
                Ok(client_ptr)
            }
        }
    }
}
```

### Cargo.toml Dependencies

```toml
[dependencies]
libloading = "0.8"
serde = { version = "1.0", features = ["derive"] }
serde_json = "1.0"
```

### Plugin Location

The plugin is built at:
```
./build/plugins/output_webrtc/output_webrtc.dylib
```

Make sure your Rust application can find this path, or copy it to a standard library location.

#!/usr/bin/env python3
"""
Example implementation showing how to get actual binary segment data 
from the MSE Control Server via a hybrid IPC + FFI approach.

This demonstrates the pattern the Rust API should follow:
1. Use IPC to check segment availability 
2. Use FFI to get the actual binary data when available
"""

import socket
import json
import ctypes
import time
from pathlib import Path

class MSEIPCClient:
    """IPC client for MSE Control Server."""
    
    def __init__(self, host='127.0.0.1', port=9051):
        self.host = host
        self.port = port
        self.sock = None
    
    def connect(self):
        """Connect to MSE control server."""
        try:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.sock.connect((self.host, self.port))
            return True
        except Exception as e:
            print(f"Failed to connect to MSE control server: {e}")
            return False
    
    def send_command(self, command):
        """Send a command and get response."""
        if not self.sock:
            return None
            
        try:
            request = json.dumps(command) + "\n"
            self.sock.send(request.encode('utf-8'))
            
            response_data = ""
            while True:
                chunk = self.sock.recv(4096).decode('utf-8')
                response_data += chunk
                if '\n' in response_data:
                    break
            
            response_line = response_data.split('\n')[0]
            return json.loads(response_line)
        except Exception as e:
            print(f"Command failed: {e}")
            return None
    
    def get_active_cameras(self):
        """Get list of active cameras."""
        response = self.send_command({"command": "get_active_cameras"})
        if response and response.get("success"):
            return response.get("cameras", [])
        return []
    
    def get_buffer_stats(self, camera_id):
        """Get buffer statistics for a camera."""
        response = self.send_command({
            "command": "get_buffer_stats", 
            "camera_id": camera_id
        })
        if response and response.get("success"):
            return {
                'buffer_size': response.get('buffer_size', 0),
                'total_segments': response.get('total_segments', 0),
                'dropped_segments': response.get('dropped_segments', 0),
                'bytes_received': response.get('bytes_received', 0),
                'frame_count': response.get('frame_count', 0)
            }
        return None
    
    def has_init_segment(self, camera_id):
        """Check if init segment is available."""
        response = self.send_command({
            "command": "get_init_segment",
            "camera_id": camera_id
        })
        return (response and 
                response.get("success") and 
                response.get("size", 0) > 0)
    
    def has_latest_segment(self, camera_id):
        """Check if latest segment is available."""
        response = self.send_command({
            "command": "get_latest_segment",
            "camera_id": camera_id
        })
        return (response and 
                response.get("success") and 
                response.get("size", 0) > 0)
    
    def close(self):
        """Close connection."""
        if self.sock:
            self.sock.close()
            self.sock = None

class MSEFFIClient:
    """FFI client for getting actual binary data."""
    
    def __init__(self, lib_path="./build/plugins/output_mse/output_mse.dylib"):
        self.lib = None
        self.lib_path = lib_path
        self._load_library()
    
    def _load_library(self):
        """Load the MSE plugin library."""
        try:
            if Path(self.lib_path).exists():
                self.lib = ctypes.CDLL(self.lib_path)
                
                # Define function signatures
                self.lib.zm_mse_get_init_segment.argtypes = [
                    ctypes.c_uint32,  # camera_id
                    ctypes.POINTER(ctypes.c_uint8),  # out
                    ctypes.c_size_t   # max_size
                ]
                self.lib.zm_mse_get_init_segment.restype = ctypes.c_size_t
                
                self.lib.zm_mse_get_latest_segment.argtypes = [
                    ctypes.c_uint32,  # camera_id
                    ctypes.POINTER(ctypes.c_uint8),  # out
                    ctypes.c_size_t   # max_size
                ]
                self.lib.zm_mse_get_latest_segment.restype = ctypes.c_size_t
                
                print(f"Loaded MSE FFI library: {self.lib_path}")
            else:
                print(f"MSE library not found: {self.lib_path}")
        except Exception as e:
            print(f"Failed to load MSE library: {e}")
    
    def get_init_segment(self, camera_id):
        """Get initialization segment binary data."""
        if not self.lib:
            return None
            
        try:
            # Allocate buffer (init segments are typically small)
            buffer_size = 64 * 1024  # 64KB
            buffer = (ctypes.c_uint8 * buffer_size)()
            
            # Call FFI function
            actual_size = self.lib.zm_mse_get_init_segment(
                camera_id, 
                ctypes.cast(buffer, ctypes.POINTER(ctypes.c_uint8)),
                buffer_size
            )
            
            if actual_size > 0:
                # Convert to bytes
                return bytes(buffer[:actual_size])
            
        except Exception as e:
            print(f"FFI get_init_segment failed: {e}")
        
        return None
    
    def get_latest_segment(self, camera_id):
        """Get latest media segment binary data."""
        if not self.lib:
            return None
            
        try:
            # Allocate larger buffer for media segments
            buffer_size = 1024 * 1024  # 1MB
            buffer = (ctypes.c_uint8 * buffer_size)()
            
            # Call FFI function
            actual_size = self.lib.zm_mse_get_latest_segment(
                camera_id,
                ctypes.cast(buffer, ctypes.POINTER(ctypes.c_uint8)),
                buffer_size
            )
            
            if actual_size > 0:
                # Convert to bytes
                return bytes(buffer[:actual_size])
                
        except Exception as e:
            print(f"FFI get_latest_segment failed: {e}")
        
        return None

class MSEHybridClient:
    """
    Hybrid client that uses IPC for availability checks and FFI for binary data.
    This is the recommended approach for the Rust API.
    """
    
    def __init__(self):
        self.ipc = MSEIPCClient()
        self.ffi = MSEFFIClient()
    
    def connect(self):
        """Connect to services."""
        return self.ipc.connect()
    
    def get_active_cameras(self):
        """Get active cameras via IPC."""
        return self.ipc.get_active_cameras()
    
    def get_camera_status(self, camera_id):
        """Get comprehensive camera status."""
        stats = self.ipc.get_buffer_stats(camera_id)
        if stats:
            return {
                'camera_id': camera_id,
                'active': True,
                'has_init_segment': self.ipc.has_init_segment(camera_id),
                'has_latest_segment': self.ipc.has_latest_segment(camera_id),
                **stats
            }
        return {
            'camera_id': camera_id,
            'active': False
        }
    
    def get_init_segment_if_available(self, camera_id):
        """Get init segment only if available (non-blocking)."""
        # First check availability via IPC
        if self.ipc.has_init_segment(camera_id):
            # Then get binary data via FFI
            return self.ffi.get_init_segment(camera_id)
        return None
    
    def get_latest_segment_if_available(self, camera_id):
        """Get latest segment only if available (non-blocking)."""
        # First check availability via IPC
        if self.ipc.has_latest_segment(camera_id):
            # Then get binary data via FFI
            return self.ffi.get_latest_segment(camera_id)
        return None
    
    def close(self):
        """Close connections."""
        self.ipc.close()

def demonstrate_hybrid_approach():
    """Demonstrate the hybrid IPC + FFI approach."""
    
    print("=== MSE Hybrid Client Demo ===")
    print("This shows how Rust API should integrate with the new IPC interface.\n")
    
    client = MSEHybridClient()
    
    # Connect to IPC server
    if not client.connect():
        print("❌ Could not connect to MSE control server.")
        print("   Make sure zm-core is running with MSE pipeline.")
        return
    
    print("✅ Connected to MSE control server")
    
    try:
        # Get active cameras
        cameras = client.get_active_cameras()
        print(f"📹 Active cameras: {cameras}")
        
        if not cameras:
            print("❌ No active cameras found")
            return
        
        # Test each camera
        for camera_id in cameras:
            print(f"\n--- Testing Camera {camera_id} ---")
            
            # Get status
            status = client.get_camera_status(camera_id)
            print(f"📊 Status: {json.dumps(status, indent=2)}")
            
            # Try to get init segment
            init_segment = client.get_init_segment_if_available(camera_id)
            if init_segment:
                print(f"🎬 Init segment: {len(init_segment)} bytes")
                print(f"   First 32 bytes: {init_segment[:32].hex()}")
                
                # Save to file for inspection
                with open(f"camera_{camera_id}_init.mp4", "wb") as f:
                    f.write(init_segment)
                print(f"   💾 Saved as camera_{camera_id}_init.mp4")
            else:
                print("❌ No init segment available")
            
            # Try to get latest segment
            latest_segment = client.get_latest_segment_if_available(camera_id)
            if latest_segment:
                print(f"🎞️  Latest segment: {len(latest_segment)} bytes")
                print(f"   First 32 bytes: {latest_segment[:32].hex()}")
                
                # Save to file for inspection
                with open(f"camera_{camera_id}_latest.m4s", "wb") as f:
                    f.write(latest_segment)
                print(f"   💾 Saved as camera_{camera_id}_latest.m4s")
            else:
                print("❌ No latest segment available")
        
    finally:
        client.close()
        print("\n✅ Demo completed")

if __name__ == "__main__":
    demonstrate_hybrid_approach()

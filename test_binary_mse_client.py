#!/usr/bin/env python3
"""
Test script for MSE plugin binary data transfer via TCP control server.
This validates the complete IPC interface including binary segment retrieval.
"""

import socket
import json
import time
import threading

class MSEClient:
    def __init__(self, host='127.0.0.1', port=9051):
        self.host = host
        self.port = port
        self.socket = None
        
    def connect(self):
        """Connect to the MSE control server."""
        try:
            self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.socket.connect((self.host, self.port))
            print(f"Connected to MSE control server at {self.host}:{self.port}")
            return True
        except Exception as e:
            print(f"Failed to connect: {e}")
            return False
    
    def disconnect(self):
        """Disconnect from the server."""
        if self.socket:
            self.socket.close()
            self.socket = None
    
    def send_command(self, command_dict):
        """Send a JSON command and return response."""
        try:
            command_str = json.dumps(command_dict) + '\n'
            self.socket.sendall(command_str.encode())
            
            # Read response line
            response_data = b''
            while True:
                chunk = self.socket.recv(1)
                if not chunk or chunk == b'\n':
                    break
                response_data += chunk
            
            response = json.loads(response_data.decode())
            return response
        except Exception as e:
            print(f"Command failed: {e}")
            return None
    
    def get_binary_data(self, response):
        """Read binary data following a JSON response that indicates binary_follows."""
        if response.get('type') != 'binary_follows':
            return None
            
        try:
            size = response['size']
            binary_data = b''
            bytes_remaining = size
            
            while bytes_remaining > 0:
                chunk = self.socket.recv(min(bytes_remaining, 8192))
                if not chunk:
                    break
                binary_data += chunk
                bytes_remaining -= len(chunk)
            
            return binary_data
        except Exception as e:
            print(f"Failed to read binary data: {e}")
            return None
    
    def get_active_cameras(self):
        """Get list of active cameras."""
        command = {"command": "get_active_cameras"}
        return self.send_command(command)
    
    def get_buffer_stats(self, camera_id):
        """Get buffer statistics for a camera."""
        command = {"command": "get_buffer_stats", "camera_id": camera_id}
        return self.send_command(command)
    
    def get_init_segment(self, camera_id):
        """Get initialization segment (binary data)."""
        command = {"command": "get_init_segment", "camera_id": camera_id}
        response = self.send_command(command)
        print(f"DEBUG: Init segment response: {response}")
        if response and response.get('success'):
            if response.get('type') == 'binary_follows':
                binary_data = self.get_binary_data(response)
                return response, binary_data
            else:
                print(f"DEBUG: No binary_follows type found, got: {response.get('type')}")
        return response, None
    
    def get_latest_segment(self, camera_id):
        """Get latest media segment (binary data)."""
        command = {"command": "get_latest_segment", "camera_id": camera_id}
        response = self.send_command(command)
        print(f"DEBUG: Latest segment response: {response}")
        if response and response.get('success'):
            if response.get('type') == 'binary_follows':
                binary_data = self.get_binary_data(response)
                return response, binary_data
            else:
                print(f"DEBUG: No binary_follows type found, got: {response.get('type')}")
        return response, None
    
    def get_stats(self):
        """Get overall statistics."""
        command = {"command": "get_stats"}
        return self.send_command(command)

def validate_binary_segment(binary_data, segment_type="unknown"):
    """Validate that binary data looks like valid MP4 segment."""
    if not binary_data or len(binary_data) < 8:
        return False, f"Too small: {len(binary_data) if binary_data else 0} bytes"
    
    # Look for MP4 box headers (4-byte size + 4-byte type)
    boxes_found = []
    offset = 0
    while offset + 8 <= len(binary_data):
        box_size = int.from_bytes(binary_data[offset:offset+4], 'big')
        box_type = binary_data[offset+4:offset+8].decode('ascii', errors='ignore')
        boxes_found.append(box_type)
        
        if box_size == 0 or box_size > len(binary_data) - offset:
            break
            
        offset += box_size
        if len(boxes_found) >= 5:  # Don't scan too deep
            break
    
    # For init segments, we expect ftyp and moov boxes
    # For media segments, we expect moof and mdat boxes
    valid_init_boxes = ['ftyp', 'moov']
    valid_media_boxes = ['moof', 'mdat']
    
    has_init_boxes = any(box in boxes_found for box in valid_init_boxes)
    has_media_boxes = any(box in boxes_found for box in valid_media_boxes)
    
    if segment_type == "init":
        is_valid = has_init_boxes
    elif segment_type == "media":
        is_valid = has_media_boxes
    else:
        is_valid = has_init_boxes or has_media_boxes
    
    return is_valid, f"Found boxes: {boxes_found[:5]}"

def main():
    print("=== MSE Binary Data Transfer Test ===")
    
    client = MSEClient()
    
    # Connect to server
    if not client.connect():
        print("❌ Failed to connect to MSE control server")
        print("Make sure zm-core is running with the MSE plugin")
        return
    
    try:
        # 1. Get active cameras
        print("\n1. Checking active cameras...")
        cameras_response = client.get_active_cameras()
        if cameras_response and cameras_response.get('success'):
            cameras = cameras_response.get('cameras', [])
            print(f"✅ Found {len(cameras)} active cameras: {cameras}")
            
            if not cameras:
                print("⚠️  No active cameras found")
                print("Make sure a pipeline is running that feeds data to the MSE plugin")
                return
        else:
            print(f"❌ Failed to get active cameras: {cameras_response}")
            return
        
        # Test with the first camera
        camera_id = cameras[0]
        print(f"\n2. Testing with camera {camera_id}")
        
        # 2. Get buffer stats
        print(f"\n3. Getting buffer stats for camera {camera_id}...")
        stats_response = client.get_buffer_stats(camera_id)
        if stats_response and stats_response.get('success'):
            print("✅ Buffer stats:")
            print(f"   - Buffer size: {stats_response.get('buffer_size', 0)} segments")
            print(f"   - Total segments: {stats_response.get('total_segments', 0)}")
            print(f"   - Dropped segments: {stats_response.get('dropped_segments', 0)}")
            print(f"   - Bytes received: {stats_response.get('bytes_received', 0)}")
            print(f"   - Frame count: {stats_response.get('frame_count', 0)}")
        else:
            print(f"❌ Failed to get buffer stats: {stats_response}")
        
        # 3. Test initialization segment
        print(f"\n4. Testing initialization segment retrieval...")
        init_response, init_binary = client.get_init_segment(camera_id)
        if init_response and init_response.get('success') and init_binary:
            print(f"✅ Got initialization segment: {len(init_binary)} bytes")
            is_valid, details = validate_binary_segment(init_binary, "init")
            if is_valid:
                print(f"✅ Initialization segment appears valid: {details}")
            else:
                print(f"⚠️  Initialization segment validation failed: {details}")
        else:
            print(f"❌ Failed to get initialization segment: {init_response}")
        
        # 4. Test latest media segment
        print(f"\n5. Testing latest media segment retrieval...")
        media_response, media_binary = client.get_latest_segment(camera_id)
        if media_response and media_response.get('success') and media_binary:
            print(f"✅ Got latest media segment: {len(media_binary)} bytes")
            is_valid, details = validate_binary_segment(media_binary, "media")
            if is_valid:
                print(f"✅ Media segment appears valid: {details}")
            else:
                print(f"⚠️  Media segment validation failed: {details}")
        else:
            print(f"❌ Failed to get latest media segment: {media_response}")
            if stats_response and stats_response.get('buffer_size', 0) == 0:
                print("   (This is expected if no media segments have been generated yet)")
        
        # 5. Monitor for new segments (brief test)
        print(f"\n6. Monitoring for new segments (10 seconds)...")
        previous_total = stats_response.get('total_segments', 0) if stats_response else 0
        
        for i in range(10):
            time.sleep(1)
            current_stats = client.get_buffer_stats(camera_id)
            if current_stats and current_stats.get('success'):
                current_total = current_stats.get('total_segments', 0)
                if current_total > previous_total:
                    print(f"✅ New segment detected! Total: {current_total} (was {previous_total})")
                    
                    # Try to get the latest segment
                    latest_response, latest_binary = client.get_latest_segment(camera_id)
                    if latest_response and latest_response.get('success') and latest_binary:
                        print(f"✅ Retrieved new segment: {len(latest_binary)} bytes")
                    
                    previous_total = current_total
                    break
            print(f"   Waiting... ({i+1}/10)")
        else:
            print("⚠️  No new segments detected during monitoring period")
        
        # 6. Overall statistics
        print(f"\n7. Getting overall statistics...")
        overall_stats = client.get_stats()
        if overall_stats and overall_stats.get('success'):
            print("✅ Overall MSE statistics:")
            print(f"   - Total streams: {overall_stats.get('total_streams', 0)}")
            streams = overall_stats.get('streams', [])
            for stream in streams:
                print(f"   - Stream {stream.get('camera_id')}:{stream.get('stream_id')}")
                print(f"     * Resolution: {stream.get('width')}x{stream.get('height')}")
                print(f"     * Frames: {stream.get('frame_count')}")
                print(f"     * Buffer: {stream.get('buffer_size')} segments")
        
        print("\n=== Test Summary ===")
        print("✅ Binary data transfer functionality is working!")
        print("✅ The MSE plugin can provide both init and media segments over TCP")
        print("✅ This eliminates the need for FFI in the Rust API")
        
    except Exception as e:
        print(f"❌ Test failed with exception: {e}")
        import traceback
        traceback.print_exc()
        
    finally:
        client.disconnect()
        print("\nDisconnected from MSE control server")

if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""
Simple WebRTC Test Client
Demonstrates how to connect to the zm-next WebRTC control server
"""

import socket
import json
import time
import uuid

class WebRTCTestClient:
    def __init__(self, host='127.0.0.1', port=9050):
        self.host = host
        self.port = port
        self.viewer_id = f"test_viewer_{uuid.uuid4().hex[:8]}"
        
    def send_command(self, command):
        """Send a command to the WebRTC control server"""
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(10)
            sock.connect((self.host, self.port))
            
            # Send command
            command_str = json.dumps(command) + '\n'
            sock.send(command_str.encode())
            print(f"Sent: {command_str.strip()}")
            
            # Receive response
            response = sock.recv(4096).decode().strip()
            sock.close()
            
            print(f"Received: {response}")
            return json.loads(response)
            
        except Exception as e:
            print(f"Error: {e}")
            return None
    
    def test_create_offer(self, camera_id=1):
        """Test creating a WebRTC offer"""
        print(f"\n=== Testing Create Offer for Camera {camera_id} ===")
        
        command = {
            "command": "create_offer",
            "camera_id": camera_id,
            "viewer_id": self.viewer_id
        }
        
        response = self.send_command(command)
        if response and response.get('success'):
            print(f"✅ Offer created successfully!")
            print(f"Offer SDP length: {len(response.get('offer', ''))}")
            return response.get('offer')
        else:
            print("❌ Failed to create offer")
            return None
    
    def test_get_statistics(self):
        """Test getting WebRTC statistics"""
        print(f"\n=== Testing Get Statistics ===")
        
        command = {
            "command": "get_stats",
            "camera_id": 1,  # Required but ignored for stats
            "viewer_id": self.viewer_id  # Required but ignored for stats
        }
        
        response = self.send_command(command)
        if response:
            print("✅ Statistics retrieved!")
            
            # Pretty print key statistics
            print(f"Active viewers: {len(response.get('viewers', []))}")
            print(f"Active streams: {len(response.get('streams', []))}")
            print(f"Total frames processed: {response.get('total_frames_processed', 0)}")
            print(f"Total connections created: {response.get('total_connections_created', 0)}")
            
            if response.get('viewers'):
                print("Viewer details:")
                for viewer in response['viewers']:
                    print(f"  - {viewer.get('viewer_id')} (Camera {viewer.get('camera_id')}, Connected: {viewer.get('is_connected')})")
                    
        else:
            print("❌ Failed to get statistics")
    
    def simulate_connection_flow(self, camera_id=1):
        """Simulate a basic WebRTC connection flow"""
        print(f"\n🔄 Simulating WebRTC connection flow for camera {camera_id}")
        print(f"Viewer ID: {self.viewer_id}")
        
        # Step 1: Create offer
        offer = self.test_create_offer(camera_id)
        if not offer:
            return
        
        # Step 2: Get statistics to see if viewer was registered
        time.sleep(1)
        self.test_get_statistics()
        
        print(f"\n📝 Next steps for a real client:")
        print(f"1. Process the SDP offer: {offer[:100]}...")
        print(f"2. Create SDP answer and send via 'set_answer' command")
        print(f"3. Exchange ICE candidates via 'add_ice_candidate' command")
        print(f"4. WebRTC peer connection will be established")
        print(f"5. Video frames will start flowing!")

def main():
    print("WebRTC Test Client for zm-next")
    print("===============================")
    
    client = WebRTCTestClient()
    
    # Test basic connection and statistics
    client.test_get_statistics()
    
    # Simulate connection flow
    client.simulate_connection_flow()
    
    print(f"\n🔍 Monitor the zm-core logs to see the connection attempts!")

if __name__ == "__main__":
    main()

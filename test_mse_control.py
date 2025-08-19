#!/usr/bin/env python3
"""Test script to connect to MSE Control Server and test IPC communication."""

import socket
import json
import time

def test_mse_control_server():
    """Test the MSE control server IPC interface."""
    
    try:
        # Connect to MSE control server
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect(('127.0.0.1', 9051))
        print("Connected to MSE Control Server on port 9051")
        
        # Test commands
        commands = [
            {"command": "get_active_cameras"},
            {"command": "get_buffer_stats", "camera_id": 1},
            {"command": "get_init_segment", "camera_id": 1},
            {"command": "get_latest_segment", "camera_id": 1},
            {"command": "get_stats"}
        ]
        
        for cmd in commands:
            print(f"\n--- Testing command: {cmd['command']} ---")
            
            # Send command
            request = json.dumps(cmd) + "\n"
            sock.send(request.encode('utf-8'))
            
            # Read response
            response_data = ""
            while True:
                chunk = sock.recv(1024).decode('utf-8')
                response_data += chunk
                if '\n' in response_data:
                    break
            
            # Parse and display response
            response_line = response_data.split('\n')[0]
            try:
                response = json.loads(response_line)
                print(f"Response: {json.dumps(response, indent=2)}")
            except json.JSONDecodeError as e:
                print(f"JSON decode error: {e}")
                print(f"Raw response: {response_line}")
            
            time.sleep(0.5)  # Small delay between commands
        
        sock.close()
        print("\nTest completed successfully!")
        
    except Exception as e:
        print(f"Error: {e}")

if __name__ == "__main__":
    print("Testing MSE Control Server IPC...")
    test_mse_control_server()

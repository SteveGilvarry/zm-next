#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

// FFI function declarations (extern "C" functions from the plugin)
extern "C" {
    char* zm_webrtc_list_camera_streams();
    void* zm_webrtc_create_client(const char* client_id, uint32_t camera_id);
    int zm_webrtc_remove_client(const char* client_id);
    void zm_webrtc_get_stats(uint64_t* total_frames, uint64_t* total_bytes, 
                            uint64_t* clients_connected, uint64_t* clients_disconnected);
    void zm_webrtc_free_string(char* str);
}

int main() {
    printf("Testing WebRTC FFI Interface\n");
    printf("=============================\n\n");
    
    // Test 1: Discover camera streams
    printf("1. Discovering camera streams...\n");
    char* streams_json = zm_webrtc_list_camera_streams();
    if (streams_json) {
        printf("   Active streams: %s\n", streams_json);
        zm_webrtc_free_string(streams_json);
    } else {
        printf("   No streams found or service not running\n");
    }
    
    // Test 2: Get service statistics
    printf("\n2. Getting service statistics...\n");
    uint64_t total_frames = 0, total_bytes = 0, clients_connected = 0, clients_disconnected = 0;
    zm_webrtc_get_stats(&total_frames, &total_bytes, &clients_connected, &clients_disconnected);
    printf("   Total frames: %llu\n", total_frames);
    printf("   Total bytes: %llu\n", total_bytes);
    printf("   Clients connected: %llu\n", clients_connected);
    printf("   Clients disconnected: %llu\n", clients_disconnected);
    
    // Test 3: Create a test client for camera 0
    printf("\n3. Creating test client for camera 0...\n");
    void* client = zm_webrtc_create_client("test_client_001", 0);
    if (client) {
        printf("   Client created successfully: %p\n", client);
        
        // Clean up
        int result = zm_webrtc_remove_client("test_client_001");
        printf("   Client removed: %s\n", result == 0 ? "success" : "failed");
    } else {
        printf("   Failed to create client (service may not be running)\n");
    }
    
    printf("\nFFI test completed.\n");
    return 0;
}

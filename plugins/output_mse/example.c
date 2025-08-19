#include "mse_api.h"
#include <stdio.h>
#include <unistd.h>
#include <string.h>

/**
 * Example usage of the MSE Plugin C API
 * 
 * This demonstrates how to register streams, push test data,
 * and pull segments for MSE streaming.
 */

int main() {
    printf("MSE Plugin API Example\n");
    printf("======================\n\n");

    // Register a camera stream
    printf("1. Registering camera stream...\n");
    zm_mse_register_stream(1, 0, "h264", 1920, 1080);
    
    // Simulate pushing some segment data (normally done by pipeline)
    printf("2. Pushing test segments...\n");
    const char* test_data = "fake_h264_segment_data_here";
    for (int i = 0; i < 5; i++) {
        zm_mse_push_segment(1, (const uint8_t*)test_data, strlen(test_data));
        printf("   Pushed segment %d\n", i + 1);
    }
    
    // Check buffer status
    printf("\n3. Checking buffer status...\n");
    size_t buffer_size = zm_mse_get_buffer_size(1);
    printf("   Current buffer size: %zu segments\n", buffer_size);
    
    uint64_t total_segments, dropped_segments;
    size_t current_size = zm_mse_get_buffer_stats(1, &total_segments, &dropped_segments);
    printf("   Total segments received: %llu\n", total_segments);
    printf("   Dropped segments: %llu\n", dropped_segments);
    printf("   Current buffer size: %zu\n", current_size);
    
    uint64_t bytes_received = zm_mse_get_bytes_received(1);
    uint64_t frame_count = zm_mse_get_frame_count(1);
    printf("   Bytes received: %llu\n", bytes_received);
    printf("   Frame count: %llu\n", frame_count);
    
    // Pull segments
    printf("\n4. Pulling segments...\n");
    uint8_t buffer[1024];
    while (buffer_size > 0) {
        size_t segment_size = zm_mse_try_pop_segment(1, buffer, sizeof(buffer));
        if (segment_size > 0) {
            printf("   Retrieved segment: %zu bytes\n", segment_size);
            // In a real application, you would send this to the browser via MSE
        }
        buffer_size = zm_mse_get_buffer_size(1);
    }
    
    printf("   No more segments available\n");
    
    // Unregister the stream
    printf("\n5. Unregistering stream...\n");
    zm_mse_unregister_stream(1, 0);
    
    printf("\nExample completed successfully!\n");
    return 0;
}

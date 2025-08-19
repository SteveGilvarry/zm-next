#include <iostream>
#include <vector>
#include <cstring>
#include <dlfcn.h>

// Function pointers for MSE plugin
typedef void (*zm_mse_register_stream_t)(uint32_t, uint32_t, const char*, int, int);
typedef void (*zm_mse_push_segment_t)(uint32_t, const uint8_t*, size_t);
typedef size_t (*zm_mse_try_pop_segment_t)(uint32_t, uint8_t*, size_t);
typedef size_t (*zm_mse_get_buffer_size_t)(uint32_t);
typedef size_t (*zm_mse_get_init_segment_t)(uint32_t, uint8_t*, size_t);

int main() {
    // Load the MSE plugin
    void* handle = dlopen("./plugins/output_mse/output_mse.dylib", RTLD_LAZY);
    if (!handle) {
        std::cerr << "Error loading plugin: " << dlerror() << std::endl;
        return 1;
    }

    // Get function pointers
    auto register_stream = (zm_mse_register_stream_t)dlsym(handle, "zm_mse_register_stream");
    auto push_segment = (zm_mse_push_segment_t)dlsym(handle, "zm_mse_push_segment");
    auto try_pop_segment = (zm_mse_try_pop_segment_t)dlsym(handle, "zm_mse_try_pop_segment");
    auto get_buffer_size = (zm_mse_get_buffer_size_t)dlsym(handle, "zm_mse_get_buffer_size");
    auto get_init_segment = (zm_mse_get_init_segment_t)dlsym(handle, "zm_mse_get_init_segment");

    if (!register_stream || !push_segment || !try_pop_segment || !get_buffer_size || !get_init_segment) {
        std::cerr << "Error: Failed to load plugin functions" << std::endl;
        dlclose(handle);
        return 1;
    }

    std::cout << "=== MSE Plugin Direct Test ===" << std::endl;

    // Register a test stream
    std::cout << "1. Registering stream (camera_id=1, stream_id=0, codec=h264, 1280x720)" << std::endl;
    register_stream(1, 0, "h264", 1280, 720);

    // Check initial state
    std::cout << "2. Initial buffer size: " << get_buffer_size(1) << std::endl;

    // Get initialization segment
    uint8_t init_buffer[4096];
    size_t init_size = get_init_segment(1, init_buffer, sizeof(init_buffer));
    std::cout << "3. Initialization segment size: " << init_size << " bytes" << std::endl;

    // Create sample H.264 data (SPS + PPS + I-frame)
    // This is a minimal valid H.264 stream header for 1280x720
    std::vector<uint8_t> sample_sps = {
        0x00, 0x00, 0x00, 0x01,  // Start code
        0x67,                     // NALU header (SPS, type 7)
        0x64, 0x00, 0x1f,        // Profile/level
        0xac, 0x2b, 0x50, 0x50, 0x05, 0x0a, 0x02, 0x02, 0x02, 0x80, 
        0x00, 0x00, 0x03, 0x00, 0x80, 0x00, 0x00, 0x1e, 0x07, 0x8c, 0x18, 0x60
    };

    std::vector<uint8_t> sample_pps = {
        0x00, 0x00, 0x00, 0x01,  // Start code  
        0x68,                     // NALU header (PPS, type 8)
        0xee, 0x3c, 0x80
    };

    std::vector<uint8_t> sample_iframe = {
        0x00, 0x00, 0x00, 0x01,  // Start code
        0x65,                     // NALU header (I-frame, type 5)
        0x88, 0x84, 0x00, 0x33, 0xff, 0xe4, 0x00, 0x00, 0x6c, 0x00, 0x02, 0x70, 0x00, 0x04, 0xe0
    };

    // Push sample H.264 data
    std::cout << "4. Pushing sample SPS data (" << sample_sps.size() << " bytes)" << std::endl;
    push_segment(1, sample_sps.data(), sample_sps.size());

    std::cout << "5. Pushing sample PPS data (" << sample_pps.size() << " bytes)" << std::endl;
    push_segment(1, sample_pps.data(), sample_pps.size());

    std::cout << "6. Pushing sample I-frame data (" << sample_iframe.size() << " bytes)" << std::endl;
    push_segment(1, sample_iframe.data(), sample_iframe.size());

    // Check buffer size after pushing data
    std::cout << "7. Buffer size after pushing data: " << get_buffer_size(1) << std::endl;

    // Try to pop a segment
    uint8_t segment_buffer[8192];
    size_t segment_size = try_pop_segment(1, segment_buffer, sizeof(segment_buffer));
    std::cout << "8. Popped segment size: " << segment_size << " bytes" << std::endl;

    if (segment_size > 0) {
        std::cout << "✅ SUCCESS: Plugin generated MP4 segment!" << std::endl;
        std::cout << "First 16 bytes: ";
        for (int i = 0; i < std::min(16, (int)segment_size); i++) {
            printf("%02x ", segment_buffer[i]);
        }
        std::cout << std::endl;
    } else {
        std::cout << "❌ No segment generated" << std::endl;
    }

    dlclose(handle);
    return 0;
}

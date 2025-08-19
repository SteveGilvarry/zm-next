#include <iostream>
#include <dlfcn.h>

extern "C" {
    size_t zm_mse_get_active_cameras(uint32_t* camera_ids, size_t max_cameras);
    uint64_t zm_mse_get_frame_count(uint32_t camera_id);
    uint64_t zm_mse_get_bytes_received(uint32_t camera_id);
    size_t zm_mse_get_buffer_size(uint32_t camera_id);
}

int main() {
    // Load the plugin
    void* handle = dlopen("./plugins/output_mse/output_mse.dylib", RTLD_LAZY);
    if (!handle) {
        std::cerr << "Error loading plugin: " << dlerror() << std::endl;
        return 1;
    }

    auto get_active_cameras = (size_t(*)(uint32_t*, size_t))dlsym(handle, "zm_mse_get_active_cameras");
    auto get_frame_count = (uint64_t(*)(uint32_t))dlsym(handle, "zm_mse_get_frame_count");
    auto get_bytes_received = (uint64_t(*)(uint32_t))dlsym(handle, "zm_mse_get_bytes_received");
    auto get_buffer_size = (size_t(*)(uint32_t))dlsym(handle, "zm_mse_get_buffer_size");

    if (!get_active_cameras || !get_frame_count || !get_bytes_received || !get_buffer_size) {
        std::cerr << "Error loading functions" << std::endl;
        dlclose(handle);
        return 1;
    }

    std::cout << "=== MSE Plugin Debug Info ===" << std::endl;

    // Get active cameras
    uint32_t camera_ids[10];
    size_t count = get_active_cameras(camera_ids, 10);
    
    std::cout << "Active cameras: " << count << std::endl;
    for (size_t i = 0; i < count; i++) {
        uint32_t camera_id = camera_ids[i];
        uint64_t frames = get_frame_count(camera_id);
        uint64_t bytes = get_bytes_received(camera_id);
        size_t buffer_size = get_buffer_size(camera_id);
        
        std::cout << "Camera " << camera_id << ":" << std::endl;
        std::cout << "  Frames: " << frames << std::endl;
        std::cout << "  Bytes: " << bytes << std::endl;
        std::cout << "  Buffer size: " << buffer_size << std::endl;
    }

    dlclose(handle);
    return 0;
}

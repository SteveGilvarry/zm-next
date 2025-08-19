# Plugin Logging Standards for zm-next

## Overview

This document outlines the standardized logging approach for zm-next plugins. Logging utilities are now **integrated directly int### Implementation Status

### Completed
✅ **Integrated logging utilities directly into `zm_plugin.h`**  
✅ Implemented core logging functions in `plugin_utils.cpp`  
✅ Added C++ convenience classes and macros  
✅ Integrated with core CMake build system  
✅ Updated MSE plugin to use new standardized API  
✅ **All builds and tests pass**  

### Next Steps
1. **Migrate Existing Plugins**: Update output_webrtc, capture_rtsp_multi, etc.
2. **Documentation**: Update plugin development guide
3. **Enhancement**: Add more advanced features (structured logging, advanced rate limiting)

## Key Architectural Decision

**Why integrate into `zm_plugin.h` instead of separate files?**

1. **Single Include**: Plugins only need `#include <zm_plugin.h>`
2. **Core Functionality**: Logging is fundamental plugin infrastructure, not optional
3. **Consistency**: All plugin APIs in one place
4. **Simplicity**: No separate utility headers to manage
5. **Maintenance**: Updates affect all plugins automatically`**, eliminating the need for separate headers and ensuring consistent logging across all plugins.

## Current Problem

Each plugin previously implemented its own logging helpers, leading to:
- **Code Duplication**: Every plugin recreated the same logging pattern
- **Inconsistency**: Different logging styles across plugins
- **Maintenance Burden**: Updates must be made in multiple places
- **Plugin Complexity**: Plugins spend time on logging infrastructure instead of core functionality

## Solution: Integrated Plugin Logging

### Core Integration

Logging utilities are now **part of the core plugin API** in `zm_plugin.h`:

```c
#include <zm_plugin.h>  // Everything included in one header!

// In your plugin start function:
zm_plugin_set_log_context(host_api, host_ctx);

// Use standardized logging:
zm_plugin_log_info("Plugin initialized with %d streams", stream_count);
zm_plugin_log_error("Failed to process frame: %s", error_msg);

// Use rate-limited logging for high-frequency events:
zm_plugin_log_debug_throttled(5, "Processing frame %llu", frame_count);
```

### C++ Convenience Classes

For C++ plugins, use the RAII logger class:

```cpp
#include <zm_plugin.h>  // All utilities included

static int plugin_start(zm_plugin_t* plugin, zm_host_api_t* host, void* host_ctx, const char* json_cfg) {
    // Initialize logging context
    ZmPluginLogger logger(host, host_ctx);
    
    // Use convenient macros
    ZM_LOG_INFO("Plugin started with config: %s", json_cfg);
    ZM_LOG_ERROR("Failed to connect: %s", error_msg);
    
    return 0;
}
```

## API Reference

### Core Logging Functions

```c
void zm_plugin_set_log_context(zm_host_api_t* host_api, void* host_ctx);
void zm_plugin_log_debug(const char* format, ...);
void zm_plugin_log_info(const char* format, ...);
void zm_plugin_log_warn(const char* format, ...);
void zm_plugin_log_error(const char* format, ...);
```

### Throttled Logging (Rate-Limited)

```c
// Log at most once per interval_sec seconds
void zm_plugin_log_debug_throttled(int interval_sec, const char* format, ...);
void zm_plugin_log_info_throttled(int interval_sec, const char* format, ...);
```

### Prefixed Logging

```c
// Add custom prefix to log messages
void zm_plugin_log_with_prefix(zm_log_level_t level, const char* prefix, const char* format, ...);

// Example:
zm_plugin_log_with_prefix(ZM_LOG_INFO, "RTSP", "Connected to %s", url);
// Output: [RTSP] Connected to rtsp://example.com/stream
```

### Event Publishing

```c
// Publish structured events
void zm_plugin_publish_event(const char* event_type, const char* json_data);
void zm_plugin_publish_simple_event(const char* event_type, const char* key, const char* value);

// Example:
zm_plugin_publish_simple_event("camera_status", "status", "connected");
```

### Statistics Publishing

```c
// Standardized plugin statistics
typedef struct zm_plugin_stats_s {
    uint64_t frames_processed;
    uint64_t bytes_processed;
    uint64_t errors_count;
    uint64_t warnings_count;
    const char* plugin_name;
    const char* plugin_version;
} zm_plugin_stats_t;

void zm_plugin_publish_stats(const zm_plugin_stats_t* stats);
```

### C++ Convenience Macros

```cpp
#define ZM_LOG_DEBUG(...) ZmPluginLogger::debug(__VA_ARGS__)
#define ZM_LOG_INFO(...) ZmPluginLogger::info(__VA_ARGS__)
#define ZM_LOG_WARN(...) ZmPluginLogger::warn(__VA_ARGS__)
#define ZM_LOG_ERROR(...) ZmPluginLogger::error(__VA_ARGS__)
```

## Migration Guide

### Before (Current Pattern)

```cpp
// Old pattern in output_webrtc.cpp
static zm_host_api_t* g_host_api = nullptr;
static void* g_host_ctx = nullptr;

static void log_info(const char* format, ...) {
    if (!g_host_api || !g_host_api->log) return;
    
    va_list args;
    va_start(args, format);
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    g_host_api->log(g_host_ctx, ZM_LOG_INFO, buffer);
}

static int webrtc_start(zm_plugin_t* plugin, zm_host_api_t* host, void* host_ctx, const char* json_cfg) {
    g_host_api = host;
    g_host_ctx = host_ctx;
    
    log_info("WebRTC plugin started");
    return 0;
}
```

### After (Standardized Pattern)

```cpp
// New pattern using utilities
#include <zm_plugin_utils.h>

static int webrtc_start(zm_plugin_t* plugin, zm_host_api_t* host, void* host_ctx, const char* json_cfg) {
    ZmPluginLogger logger(host, host_ctx);
    
    ZM_LOG_INFO("WebRTC plugin started");
    return 0;
}
```

## Benefits

### For Plugin Developers
- **Reduced Boilerplate**: No need to implement logging helpers
- **Consistent Interface**: Same logging API across all plugins
- **Enhanced Features**: Built-in rate limiting, prefixing, event publishing
- **Less Maintenance**: Core team maintains logging infrastructure

### For Core Team
- **Single Point of Control**: All plugin logging goes through one implementation
- **Easier Debugging**: Consistent log format and behavior
- **Feature Enhancement**: Add new logging features once, all plugins benefit
- **Quality Assurance**: Standardized error handling and validation

### For Users
- **Consistent Logs**: Uniform format and behavior across all plugins
- **Better Debugging**: More structured and predictable log output
- **Enhanced Monitoring**: Standardized events and statistics

## Implementation Status

### Completed
✅ Created `zm_plugin_utils.h` header with full API  
✅ Implemented `zm_plugin_utils.c` with core functionality  
✅ Added C++ convenience classes and macros  
✅ Integrated with core CMake build system  
✅ Created refactored MSE plugin example  

### Next Steps
1. **Migrate Existing Plugins**: Update output_webrtc, capture_rtsp_multi, etc.
2. **Documentation**: Update plugin development guide
3. **Testing**: Verify logging works correctly across all plugins
4. **Enhancement**: Add more advanced features (structured logging, log levels)

## Examples

### Basic Logging
```cpp
ZM_LOG_INFO("Plugin initialized for camera %u", camera_id);
ZM_LOG_WARN("Buffer almost full: %zu/%zu", current_size, max_size);
ZM_LOG_ERROR("Failed to connect to %s: %s", url, strerror(errno));
```

### Rate-Limited Logging
```cpp
// Log at most once every 5 seconds
zm_plugin_log_info_throttled(5, "Processing frame rate: %.2f fps", current_fps);
```

### Event Publishing
```cpp
// Publish connection status
zm_plugin_publish_simple_event("connection_status", "state", "connected");

// Publish complex statistics
char stats_json[512];
snprintf(stats_json, sizeof(stats_json), 
         "{\"fps\":%.2f,\"bitrate\":%llu}", fps, bitrate);
zm_plugin_publish_event("performance_stats", stats_json);
```

### Structured Statistics
```cpp
zm_plugin_stats_t stats = {0};
stats.frames_processed = total_frames;
stats.bytes_processed = total_bytes;
stats.errors_count = error_count;
stats.plugin_name = "output_webrtc";
stats.plugin_version = "1.0.0";

zm_plugin_publish_stats(&stats);
```

## Conclusion

This standardized logging approach significantly improves the zm-next plugin ecosystem by:
- Eliminating redundant code across plugins
- Providing consistent, feature-rich logging capabilities
- Simplifying plugin development and maintenance
- Enabling better monitoring and debugging

All new plugins should use these utilities, and existing plugins should be migrated to this standard.

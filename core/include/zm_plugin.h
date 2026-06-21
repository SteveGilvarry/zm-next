#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// ABI version of the plugin contract. Plugins should set zm_plugin_t.version
// to this value in their zm_plugin_init; the host warns on a mismatch.
#define ZM_PLUGIN_ABI_VERSION 1u

// Simple JSON library for configuration
typedef struct zm_json_s zm_json_t;

// Logging levels
typedef enum {
    ZM_LOG_DEBUG,
    ZM_LOG_INFO,
    ZM_LOG_WARN,
    ZM_LOG_ERROR
} zm_log_level_t;

// Unified Plugin interface type
typedef enum zm_plugin_type_e {
    ZM_PLUGIN_INPUT,
    ZM_PLUGIN_PROCESS,
    ZM_PLUGIN_DETECT,
    ZM_PLUGIN_OUTPUT,
    ZM_PLUGIN_STORE
} zm_plugin_type_t;

// Hardware surface types
typedef enum zm_hw_type_e {
    ZM_HW_CPU = 0,
    ZM_HW_CUDA = 1,
    ZM_HW_VAAPI = 2,
    ZM_HW_VTB = 3,   // VideoToolbox (Apple)
    ZM_HW_DXVA = 4,  // DirectX Video Acceleration
    
    // Frame format types (using high values to avoid conflicts)
    ZM_FRAME_COMPRESSED = 100,  // Compressed video (H.264, etc.)
    ZM_FRAME_RGB24 = 101,       // Uncompressed RGB24
    ZM_FRAME_GRAYSCALE = 102,   // Uncompressed grayscale
    ZM_FRAME_YUV420P = 103,     // Uncompressed YUV420P
    ZM_FRAME_COMPRESSED_AUDIO = 104  // Compressed audio (AAC, Opus, G.711, ...)
} zm_hw_type_t;

// Host API for plugins to call
typedef struct zm_host_api_s {
    // Logger with different severity levels
    void (*log)(void* host_ctx, zm_log_level_t level, const char* msg);
    // Event publishing for metadata events
    void (*publish_evt)(void* host_ctx, const char* json_event);
    // Frame callback for input plugins to forward frames to pipeline
    void (*on_frame)(void* host_ctx, const void* frame_hdr, size_t frame_size);
    // Subscribe to metadata events. `cb(user, json_event)` is invoked for each
    // event published by any plugin; returns an opaque handle for unsubscribe_evt.
    // ALWAYS subscribe via this host call rather than touching an in-process bus
    // directly: plugins are dlopen'd shared libraries and do NOT share the host's
    // event-bus singleton across the library boundary.
    void* (*subscribe_evt)(void* host_ctx,
                           void (*cb)(void* user, const char* json_event),
                           void* user);
    // Remove a subscription created with subscribe_evt.
    void (*unsubscribe_evt)(void* host_ctx, void* handle);
    // Reserved for future extensions of the API
    void* reserved[2];
} zm_host_api_t;

// Frame header prefixed to each media packet/frame
typedef struct zm_frame_hdr_s {
    uint32_t stream_id;        // Stream identifier (0=first video, 1=second video, etc.)
    uint32_t hw_type;          // 0=CPU, 1=CUDA, 2=VAAPI, 3=VTB
    uint64_t handle;           // CPU: packet data pointer; GPU: surface ID
    uint32_t bytes;            // For CPU: size of packet following this header
    uint32_t flags;            // Keyframe flag, etc.
    uint64_t pts_usec;         // Presentation timestamp in microseconds
} zm_frame_hdr_t;

// Descriptor for a frame that lives on a GPU/accelerator surface (zero-copy
// path). When a frame's hw_type is a GPU type (ZM_HW_CUDA / ZM_HW_VAAPI /
// ZM_HW_VTB), the on_frame payload after the zm_frame_hdr_t is THIS struct, not
// pixel bytes — the actual surface stays on the device. `av_frame` is the opaque
// AVFrame* that owns the surface; it is guaranteed valid only for the duration
// of the (synchronous) on_frame call, so a consumer must use or download it
// before returning. A CPU consumer calls a download helper; a GPU consumer
// (e.g. ORT CUDA EP) uses plane_ptr/linesize directly without a host copy.
typedef struct zm_gpu_frame_s {
    uint32_t hw_type;        // ZM_HW_CUDA / ZM_HW_VAAPI / ZM_HW_VTB
    uint32_t pix_fmt;        // native surface format (AVPixelFormat, e.g. NV12)
    uint32_t width;
    uint32_t height;
    uint64_t plane_ptr[4];   // per-plane device pointers / surface handles
    uint32_t linesize[4];    // per-plane strides (bytes)
    uint64_t av_frame;       // opaque AVFrame* owning the surface (call-scoped)
    uint64_t device_ctx;     // opaque hw device context (AVBufferRef*), optional
} zm_gpu_frame_t;

// Plugin definition
typedef struct zm_plugin_s {
    uint32_t version;          // API version, currently 1
    zm_plugin_type_t type;     // Plugin type: input, process, detect, etc.
    // Plugin lifecycle callbacks
    int (*start)(struct zm_plugin_s* plugin, zm_host_api_t* host, void* host_ctx, const char* json_cfg);
    void (*stop)(struct zm_plugin_s* plugin);
    // Direct frame passing: standardized single buffer [zm_frame_hdr_t][payload]
    void (*on_frame)(struct zm_plugin_s* plugin, const void* buf, size_t size);
    // Plugin instance data
    void* instance;            // Plugin-specific context
    void* reserved[2];         // Reserved for future use
} zm_plugin_t;

// Export this symbol from your plugin
#define ZM_PLUGIN_EXPORT_SYMBOL "zm_plugin_init"
typedef void (*zm_plugin_init_fn)(zm_plugin_t*);

// Cross-platform symbol-export decoration for the plugin entry point.
// MSVC needs __declspec(dllexport) to place zm_plugin_init in the DLL export
// table (so the host's GetProcAddress finds it); GCC/Clang use the visibility
// attribute (the TUs are built with -fvisibility=hidden).
#ifndef ZM_PLUGIN_EXPORT
#  if defined(_WIN32)
#    define ZM_PLUGIN_EXPORT __declspec(dllexport)
#  elif defined(__GNUC__) || defined(__clang__)
#    define ZM_PLUGIN_EXPORT __attribute__((visibility("default")))
#  else
#    define ZM_PLUGIN_EXPORT
#  endif
#endif

// =============================================================================
// PLUGIN LOGGING UTILITIES
// =============================================================================

/**
 * Global plugin logging context - set during plugin start
 */
typedef struct zm_plugin_log_ctx_s {
    zm_host_api_t* host_api;
    void* host_ctx;
} zm_plugin_log_ctx_t;

/**
 * Set the global logging context for the plugin
 * Call this in your plugin's start function
 */
void zm_plugin_set_log_context(zm_host_api_t* host_api, void* host_ctx);

/**
 * Standardized logging functions for plugins
 * These automatically format messages and route to the host API
 */
void zm_plugin_log_debug(const char* format, ...);
void zm_plugin_log_info(const char* format, ...);
void zm_plugin_log_warn(const char* format, ...);
void zm_plugin_log_error(const char* format, ...);

/**
 * Conditional logging with rate limiting
 * Useful for high-frequency events to avoid log spam
 */
void zm_plugin_log_debug_throttled(int interval_sec, const char* format, ...);
void zm_plugin_log_info_throttled(int interval_sec, const char* format, ...);

/**
 * Log with custom prefix for better organization
 */
void zm_plugin_log_with_prefix(zm_log_level_t level, const char* prefix, const char* format, ...);

// =============================================================================
// PLUGIN EVENT AND STATISTICS UTILITIES
// =============================================================================

/**
 * Simplified event publishing helpers
 */
void zm_plugin_publish_event(const char* event_type, const char* json_data);
void zm_plugin_publish_simple_event(const char* event_type, const char* key, const char* value);

/**
 * Common statistics reporting structure
 */
typedef struct zm_plugin_stats_s {
    uint64_t frames_processed;
    uint64_t bytes_processed;
    uint64_t errors_count;
    uint64_t warnings_count;
    const char* plugin_name;
    const char* plugin_version;
} zm_plugin_stats_t;

/**
 * Publish standardized plugin statistics
 */
void zm_plugin_publish_stats(const zm_plugin_stats_t* stats);

#ifdef __cplusplus
}

// =============================================================================
// C++ CONVENIENCE WRAPPERS
// =============================================================================

/**
 * C++ RAII logging context manager
 */
class ZmPluginLogger {
public:
    ZmPluginLogger(zm_host_api_t* host_api, void* host_ctx) {
        zm_plugin_set_log_context(host_api, host_ctx);
    }
    
    static void debug(const char* format, ...) {
        va_list args;
        va_start(args, format);
        char buffer[1024];
        vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);
        zm_plugin_log_debug("%s", buffer);
    }
    
    static void info(const char* format, ...) {
        va_list args;
        va_start(args, format);
        char buffer[1024];
        vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);
        zm_plugin_log_info("%s", buffer);
    }
    
    static void warn(const char* format, ...) {
        va_list args;
        va_start(args, format);
        char buffer[1024];
        vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);
        zm_plugin_log_warn("%s", buffer);
    }
    
    static void error(const char* format, ...) {
        va_list args;
        va_start(args, format);
        char buffer[1024];
        vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);
        zm_plugin_log_error("%s", buffer);
    }
};

// Convenience macros for C++
#define ZM_LOG_DEBUG(...) ZmPluginLogger::debug(__VA_ARGS__)
#define ZM_LOG_INFO(...) ZmPluginLogger::info(__VA_ARGS__)
#define ZM_LOG_WARN(...) ZmPluginLogger::warn(__VA_ARGS__)
#define ZM_LOG_ERROR(...) ZmPluginLogger::error(__VA_ARGS__)

#endif

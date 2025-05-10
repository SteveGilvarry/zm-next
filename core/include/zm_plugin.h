#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

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
    ZM_HW_DXVA = 4   // DirectX Video Acceleration
} zm_hw_type_t;

// Host API for plugins to call
typedef struct zm_host_api_s {
    // Logger with different severity levels
    void (*log)(void* host_ctx, zm_log_level_t level, const char* msg);
    // Event publishing for metadata events
    void (*publish_evt)(void* host_ctx, const char* json_event);
    // Frame callback for input plugins to forward frames to pipeline
    void (*on_frame)(void* host_ctx, const void* frame_hdr, size_t frame_size);
    // Reserved for future extensions of the API
    void* reserved[4];
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

// Plugin definition
typedef struct zm_plugin_s {
    uint32_t version;          // API version, currently 1
    zm_plugin_type_t type;     // Plugin type: input, process, detect, etc.
    // Plugin lifecycle callbacks
    int (*start)(struct zm_plugin_s* plugin, zm_host_api_t* host, void* host_ctx, const char* json_cfg);
    void (*stop)(struct zm_plugin_s* plugin);
    // Direct frame passing (not used by INPUT plugins)
    void (*on_frame)(struct zm_plugin_s* plugin, const zm_frame_hdr_t* hdr, const void* payload);
    // Plugin instance data
    void* instance;            // Plugin-specific context
    void* reserved[2];         // Reserved for future use
} zm_plugin_t;

// Export this symbol from your plugin
#define ZM_PLUGIN_EXPORT_SYMBOL "zm_plugin_init"
typedef void (*zm_plugin_init_fn)(zm_plugin_t*);

#ifdef __cplusplus
}
#endif

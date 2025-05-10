#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum zm_plugin_type_e {
    ZM_PLUGIN_INPUT,
    ZM_PLUGIN_PROCESS,
    ZM_PLUGIN_DETECT,
    ZM_PLUGIN_OUTPUT,
    ZM_PLUGIN_STORE
} zm_plugin_type_t;

// Plugin interface
typedef struct zm_plugin_s zm_plugin_t;

// Frame header
typedef struct zm_frame_hdr_s {
    uint32_t width;
    uint32_t height;
    uint64_t timestamp;
    uint32_t hw_type; // 0=CPU,1=CUDA,...
} zm_frame_hdr_t;

// Plugin callbacks
typedef void (*zm_plugin_start_fn)(zm_plugin_t *plugin);
typedef void (*zm_plugin_stop_fn)(zm_plugin_t *plugin);
typedef void (*zm_plugin_on_frame_fn)(zm_plugin_t *plugin, const zm_frame_hdr_t *hdr, const void *payload, size_t payload_size);

struct zm_plugin_s {
    zm_plugin_type_t type;
    void *instance; // plugin-specific context
    zm_plugin_start_fn start;
    zm_plugin_stop_fn stop;
    zm_plugin_on_frame_fn on_frame;
};

// Host API functions available to plugins
// Implemented by the host and callable by plugins
void host_log(const char *msg);
void publish_event(const char *json);

#ifdef __cplusplus
}
#endif

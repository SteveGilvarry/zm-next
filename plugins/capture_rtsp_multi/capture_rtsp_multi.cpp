#include "zm_plugin.h"
#include "stream_manager.hpp"

#include <memory>
#include <cstring>

// Global pointers for FFmpeg to ZoneMinder log callback
static zm_host_api_t* g_host_api = nullptr;
static void* g_host_ctx = nullptr;

// FFmpeg log callback that routes through ZoneMinder logging
static void ffmpeg_log_callback(void* avcl, int level, const char* fmt, va_list args) {
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, args);
    
    // Only forward FFmpeg logs at INFO or higher
    if (level > AV_LOG_INFO) return;
    
    zm_log_level_t zm_level;
    if (level <= AV_LOG_ERROR) zm_level = ZM_LOG_ERROR;
    else if (level <= AV_LOG_WARNING) zm_level = ZM_LOG_WARN;
    else zm_level = ZM_LOG_INFO;
    
    if (g_host_api && g_host_api->log) {
        g_host_api->log(g_host_ctx, zm_level, buf);
    }
}

// Plugin context structure
struct RtspMultiContext {
    std::unique_ptr<StreamManager> stream_manager;
    zm_host_api_t* host_api;
    void* host_ctx;
    
    RtspMultiContext() : stream_manager(nullptr), host_api(nullptr), host_ctx(nullptr) {}
    
    void log(zm_log_level_t level, const char* msg) {
        if (host_api && host_api->log) {
            host_api->log(host_ctx, level, msg);
        }
    }
};

// Plugin implementation functions
static int rtsp_multi_start(zm_plugin_t* plugin, zm_host_api_t* host, void* host_ctx, const char* json_cfg) {
    if (!plugin || !host || !json_cfg) {
        return -1;
    }
    
    // Store global references for FFmpeg logging
    g_host_api = host;
    g_host_ctx = host_ctx;
    
    // Setup FFmpeg logging
    av_log_set_callback(ffmpeg_log_callback);
    av_log_set_level(AV_LOG_INFO);
    
    // Create plugin context
    auto* ctx = new RtspMultiContext();
    ctx->host_api = host;
    ctx->host_ctx = host_ctx;
    plugin->instance = ctx;
    
    ctx->log(ZM_LOG_INFO, "Starting multi-stream RTSP capture plugin");
    
    // Create and initialize stream manager
    ctx->stream_manager = std::make_unique<StreamManager>();
    if (!ctx->stream_manager->initialize(host, host_ctx, json_cfg)) {
        ctx->log(ZM_LOG_ERROR, "Failed to initialize stream manager");
        delete ctx;
        plugin->instance = nullptr;
        return -1;
    }
    
    // Start all configured streams
    if (!ctx->stream_manager->start_all_streams()) {
        ctx->log(ZM_LOG_ERROR, "Failed to start all streams");
        delete ctx;
        plugin->instance = nullptr;
        return -1;
    }
    
    ctx->log(ZM_LOG_INFO, "Multi-stream RTSP capture plugin started successfully");
    return 0;
}

static void rtsp_multi_stop(zm_plugin_t* plugin) {
    if (!plugin || !plugin->instance) {
        return;
    }
    
    auto* ctx = static_cast<RtspMultiContext*>(plugin->instance);
    ctx->log(ZM_LOG_INFO, "Stopping multi-stream RTSP capture plugin");
    
    // Stop all streams
    if (ctx->stream_manager) {
        ctx->stream_manager->stop_all_streams();
        ctx->stream_manager.reset();
    }
    
    ctx->log(ZM_LOG_INFO, "Multi-stream RTSP capture plugin stopped");
    
    delete ctx;
    plugin->instance = nullptr;
    
    // Clear global references
    g_host_api = nullptr;
    g_host_ctx = nullptr;
}

static void rtsp_multi_on_frame(zm_plugin_t* plugin, const void* buf, size_t size) {
    // This is an input plugin, so it doesn't receive frames from upstream
    // Instead, it generates frames and sends them via the host API
    (void)plugin;
    (void)buf;
    (void)size;
}

// Plugin initialization
extern "C" {

__attribute__((visibility("default")))
void zm_plugin_init(zm_plugin_t* plugin) {
    if (!plugin) {
        return;
    }
    
    // Initialize plugin structure
    std::memset(plugin, 0, sizeof(zm_plugin_t));
    
    plugin->version = 1;
    plugin->type = ZM_PLUGIN_INPUT;
    plugin->start = rtsp_multi_start;
    plugin->stop = rtsp_multi_stop;
    plugin->on_frame = rtsp_multi_on_frame;
    plugin->instance = nullptr;
}

// Compatibility alias
__attribute__((visibility("default")))
void init_plugin(zm_plugin_t* plugin) {
    zm_plugin_init(plugin);
}

} // extern "C"

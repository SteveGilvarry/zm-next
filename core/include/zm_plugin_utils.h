#pragma once

#include "zm_plugin.h"
#include <stdarg.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// PLUGIN LOGGING UTILITIES
// =============================================================================

/**
 * Global plugin logging context - must be set during plugin start
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
// PLUGIN EVENT UTILITIES
// =============================================================================

/**
 * Simplified event publishing helpers
 */
void zm_plugin_publish_event(const char* event_type, const char* json_data);
void zm_plugin_publish_simple_event(const char* event_type, const char* key, const char* value);

// =============================================================================
// PLUGIN STATISTICS HELPERS
// =============================================================================

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
#endif

// =============================================================================
// C++ CONVENIENCE MACROS AND WRAPPERS
// =============================================================================

#ifdef __cplusplus

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

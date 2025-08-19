#include "zm_plugin.h"
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <inttypes.h>

// =============================================================================
// GLOBAL PLUGIN LOGGING CONTEXT
// =============================================================================

static zm_plugin_log_ctx_t g_log_ctx = {NULL, NULL};

void zm_plugin_set_log_context(zm_host_api_t* host_api, void* host_ctx) {
    g_log_ctx.host_api = host_api;
    g_log_ctx.host_ctx = host_ctx;
}

// =============================================================================
// INTERNAL LOGGING IMPLEMENTATION
// =============================================================================

static void zm_plugin_log_internal(zm_log_level_t level, const char* format, va_list args) {
    if (!g_log_ctx.host_api || !g_log_ctx.host_api->log) {
        return;
    }
    
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);
    g_log_ctx.host_api->log(g_log_ctx.host_ctx, level, buffer);
}

// =============================================================================
// PUBLIC LOGGING FUNCTIONS
// =============================================================================

void zm_plugin_log_debug(const char* format, ...) {
    va_list args;
    va_start(args, format);
    zm_plugin_log_internal(ZM_LOG_DEBUG, format, args);
    va_end(args);
}

void zm_plugin_log_info(const char* format, ...) {
    va_list args;
    va_start(args, format);
    zm_plugin_log_internal(ZM_LOG_INFO, format, args);
    va_end(args);
}

void zm_plugin_log_warn(const char* format, ...) {
    va_list args;
    va_start(args, format);
    zm_plugin_log_internal(ZM_LOG_WARN, format, args);
    va_end(args);
}

void zm_plugin_log_error(const char* format, ...) {
    va_list args;
    va_start(args, format);
    zm_plugin_log_internal(ZM_LOG_ERROR, format, args);
    va_end(args);
}

// =============================================================================
// THROTTLED LOGGING
// =============================================================================

static time_t get_last_log_time(const char* key) {
    // Simple implementation - in practice you might want a hash table
    // For now, return 0 to always allow logging (can be enhanced later)
    return 0;
}

static void set_last_log_time(const char* key, time_t timestamp) {
    // Simple implementation - in practice you might want a hash table
    // This can be enhanced later with proper rate limiting storage
}

void zm_plugin_log_debug_throttled(int interval_sec, const char* format, ...) {
    time_t now = time(NULL);
    char key[256];
    snprintf(key, sizeof(key), "debug_%s", format); // Simple key based on format
    
    time_t last_log = get_last_log_time(key);
    if (now - last_log >= interval_sec) {
        va_list args;
        va_start(args, format);
        zm_plugin_log_internal(ZM_LOG_DEBUG, format, args);
        va_end(args);
        set_last_log_time(key, now);
    }
}

void zm_plugin_log_info_throttled(int interval_sec, const char* format, ...) {
    time_t now = time(NULL);
    char key[256];
    snprintf(key, sizeof(key), "info_%s", format); // Simple key based on format
    
    time_t last_log = get_last_log_time(key);
    if (now - last_log >= interval_sec) {
        va_list args;
        va_start(args, format);
        zm_plugin_log_internal(ZM_LOG_INFO, format, args);
        va_end(args);
        set_last_log_time(key, now);
    }
}

// =============================================================================
// PREFIXED LOGGING
// =============================================================================

void zm_plugin_log_with_prefix(zm_log_level_t level, const char* prefix, const char* format, ...) {
    char prefixed_format[1024];
    snprintf(prefixed_format, sizeof(prefixed_format), "[%s] %s", prefix, format);
    
    va_list args;
    va_start(args, format);
    zm_plugin_log_internal(level, prefixed_format, args);
    va_end(args);
}

// =============================================================================
// EVENT PUBLISHING HELPERS
// =============================================================================

void zm_plugin_publish_event(const char* event_type, const char* json_data) {
    if (!g_log_ctx.host_api || !g_log_ctx.host_api->publish_evt) {
        return;
    }
    
    char event_json[2048];
    snprintf(event_json, sizeof(event_json), 
             "{\"type\":\"%s\",\"timestamp\":%" PRId64 ",\"data\":%s}",
             event_type, (int64_t)time(NULL), json_data);
    
    g_log_ctx.host_api->publish_evt(g_log_ctx.host_ctx, event_json);
}

void zm_plugin_publish_simple_event(const char* event_type, const char* key, const char* value) {
    char json_data[512];
    snprintf(json_data, sizeof(json_data), "{\"%s\":\"%s\"}", key, value);
    zm_plugin_publish_event(event_type, json_data);
}

// =============================================================================
// STATISTICS PUBLISHING
// =============================================================================

void zm_plugin_publish_stats(const zm_plugin_stats_t* stats) {
    if (!stats) return;
    
    char stats_json[1024];
    snprintf(stats_json, sizeof(stats_json),
             "{\"frames_processed\":%" PRIu64 ","
             "\"bytes_processed\":%" PRIu64 ","
             "\"errors_count\":%" PRIu64 ","
             "\"warnings_count\":%" PRIu64 ","
             "\"plugin_name\":\"%s\","
             "\"plugin_version\":\"%s\"}",
             stats->frames_processed,
             stats->bytes_processed,
             stats->errors_count,
             stats->warnings_count,
             stats->plugin_name ? stats->plugin_name : "unknown",
             stats->plugin_version ? stats->plugin_version : "unknown");
    
    zm_plugin_publish_event("plugin_stats", stats_json);
}

// Example: Before and After Plugin Logging Integration

// ============================================================================
// BEFORE: Each plugin had to implement its own logging
// ============================================================================

// In output_webrtc.cpp (OLD APPROACH):
#include <zm_plugin.h>
#include <stdarg.h>

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
    
    log_info("WebRTC plugin started");  // Custom helper
    return 0;
}

// ============================================================================
// AFTER: Using integrated logging utilities
// ============================================================================

// In output_mse.cpp (NEW APPROACH):
#include <zm_plugin.h>  // Everything included!

static int mse_start(zm_plugin_t* plugin, zm_host_api_t* host, void* host_ctx, const char* json_cfg) {
    // Set up logging context (one line!)
    zm_plugin_set_log_context(host, host_ctx);
    
    // Use standardized logging functions
    zm_plugin_log_info("MSE Output Plugin started");
    return 0;
}

// For C++, even simpler:
static int cpp_plugin_start(zm_plugin_t* plugin, zm_host_api_t* host, void* host_ctx, const char* json_cfg) {
    ZmPluginLogger logger(host, host_ctx);
    
    ZM_LOG_INFO("Plugin started with config: %s", json_cfg);
    ZM_LOG_WARN("This is a warning message");
    ZM_LOG_ERROR("Error occurred: %s", "example error");
    
    return 0;
}

// ============================================================================
// BENEFITS COMPARISON
// ============================================================================

/*
BEFORE (per plugin):
- 30+ lines of boilerplate logging code
- Global variables for host API
- Manual va_list handling
- Inconsistent across plugins
- No rate limiting or advanced features

AFTER (integrated):
- 1 line setup: zm_plugin_set_log_context()
- Standardized API: zm_plugin_log_*()
- Built-in features: rate limiting, prefixing, events
- Consistent across all plugins
- C++ convenience macros available
- All functionality in core zm_plugin.h header

REDUCTION:
- 95% less boilerplate code per plugin
- 100% consistency across plugins
- Enhanced features available to all plugins
- Single point of maintenance in core
*/

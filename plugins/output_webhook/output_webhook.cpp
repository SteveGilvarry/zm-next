// output_webhook: an OUTPUT sink that POSTs each pipeline event as JSON to a
// configured HTTP(S) webhook URL (for notifications / automations / Home
// Assistant). It mirrors output_mqtt but forwards events over HTTP via libcurl.
//
// It does NOT touch frames (on_frame is a no-op). Instead it subscribes to the
// host event stream (via host->subscribe_evt) and POSTs each event payload to
// the configured URL. An optional "event_types" allow-list filters which events
// are forwarded.
//
// Lifetime note: events are delivered through the HOST API across the dlopen
// boundary. All mutable state lives in a raw, intentionally-leaked WebhookState
// struct passed as the `user` pointer. An atomic `running` flag gates the
// callback. stop() unsubscribes via host->unsubscribe_evt, flips running to
// false, tears down the curl handle, and leaks the state struct so an in-flight
// callback never touches freed memory.

#include "webhook_util.hpp"

#include <zm_plugin.h>
#include <nlohmann/json.hpp>
#include <curl/curl.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {

struct WebhookState {
    // Config.
    std::string url;                       // required; empty => POST nothing
    long timeoutMs = 2000;                 // CURLOPT_TIMEOUT_MS
    std::string authHeader;                // optional, e.g. "Authorization: Bearer X"
    bool haveAuthHeader = false;
    std::vector<std::string> eventTypes;   // optional allow-list (empty = all)

    // Runtime.
    CURL* curl = nullptr;
    struct curl_slist* headers = nullptr;  // owned header list for the easy handle
    std::atomic<bool> running{false};
    std::mutex postMutex;                   // serialize POSTs / teardown (easy handle not thread-safe)
    void* subHandle = nullptr;
};

// `state` is intentionally leaked on stop (after unsubscribe) so an in-flight
// host callback never touches freed memory.
struct WebhookPluginCtx {
    zm_host_api_t* host = nullptr;
    void* hostCtx = nullptr;
    WebhookState* state = nullptr;
};

// Discard any response body so it isn't written to stdout.
size_t discardBody(char* /*ptr*/, size_t size, size_t nmemb, void* /*userdata*/) {
    return size * nmemb;
}

void handleEvent(WebhookState* st, const std::string& payload) {
    if (!st || !st->running.load(std::memory_order_acquire))
        return;
    if (st->url.empty())
        return;

    // Optional type filter (derived from the event's "type" field).
    if (!st->eventTypes.empty()) {
        std::string type;
        try {
            auto j = json::parse(payload);
            if (j.contains("type") && j["type"].is_string())
                type = j["type"].get<std::string>();
        } catch (const std::exception&) {
            type.clear();
        }
        if (!zm::outputwebhook::type_allowed(st->eventTypes, type))
            return;
    }

    std::lock_guard<std::mutex> lock(st->postMutex);
    if (!st->running.load(std::memory_order_acquire) || !st->curl)
        return;

    curl_easy_setopt(st->curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(st->curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));

    const CURLcode rc = curl_easy_perform(st->curl);
    if (rc != CURLE_OK) {
        ZM_LOG_WARN("output_webhook: POST to '%s' failed: %s",
                    st->url.c_str(), curl_easy_strerror(rc));
        return;
    }

    long httpCode = 0;
    curl_easy_getinfo(st->curl, CURLINFO_RESPONSE_CODE, &httpCode);
    if (httpCode < 200 || httpCode >= 300) {
        ZM_LOG_WARN("output_webhook: POST to '%s' returned HTTP %ld",
                    st->url.c_str(), httpCode);
    }
}

int output_webhook_start(zm_plugin_t* plugin, zm_host_api_t* host, void* host_ctx,
                         const char* json_cfg) {
    zm_plugin_set_log_context(host, host_ctx);

    auto* state = new WebhookState();  // leaked on stop (see WebhookPluginCtx)
    try {
        auto j = json::parse(json_cfg ? json_cfg : "{}");
        state->url = j.value("url", state->url);
        state->timeoutMs = j.value("timeout_ms", state->timeoutMs);
        if (j.contains("auth_header") && j["auth_header"].is_string()) {
            state->authHeader = j["auth_header"].get<std::string>();
            state->haveAuthHeader = !state->authHeader.empty();
        }
        if (j.contains("event_types") && j["event_types"].is_array()) {
            for (const auto& e : j["event_types"]) {
                if (e.is_string())
                    state->eventTypes.push_back(e.get<std::string>());
            }
        }
    } catch (const std::exception& e) {
        ZM_LOG_ERROR("output_webhook: failed to parse config: %s", e.what());
    }

    if (state->url.empty()) {
        ZM_LOG_WARN("output_webhook: no 'url' configured; events will NOT be POSTed");
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    state->curl = curl_easy_init();
    if (!state->curl) {
        ZM_LOG_ERROR("output_webhook: curl_easy_init failed");
        curl_global_cleanup();
        return -1;
    }

    // Reusable easy handle: configure once, vary only the POST body per event.
    state->headers = curl_slist_append(state->headers, "Content-Type: application/json");
    if (state->haveAuthHeader)
        state->headers = curl_slist_append(state->headers, state->authHeader.c_str());

    curl_easy_setopt(state->curl, CURLOPT_URL, state->url.c_str());
    curl_easy_setopt(state->curl, CURLOPT_POST, 1L);
    curl_easy_setopt(state->curl, CURLOPT_HTTPHEADER, state->headers);
    curl_easy_setopt(state->curl, CURLOPT_TIMEOUT_MS, state->timeoutMs);
    curl_easy_setopt(state->curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(state->curl, CURLOPT_WRITEFUNCTION, discardBody);

    state->running.store(true, std::memory_order_release);

    // Subscribe via the HOST so events reach us across the dlopen boundary. The
    // `state` user pointer is leaked on stop so an in-flight callback is safe.
    if (host && host->subscribe_evt) {
        state->subHandle = host->subscribe_evt(
            host_ctx,
            [](void* user, const char* json_event) {
                handleEvent(static_cast<WebhookState*>(user), json_event ? json_event : "");
            },
            state);
    }

    auto* ctx = new WebhookPluginCtx{host, host_ctx, state};
    plugin->instance = ctx;

    ZM_LOG_INFO("output_webhook: started url='%s' timeout_ms=%ld auth=%s event_types=%zu",
                state->url.c_str(), state->timeoutMs,
                state->haveAuthHeader ? "yes" : "no", state->eventTypes.size());
    return 0;
}

void output_webhook_stop(zm_plugin_t* plugin) {
    if (!plugin || !plugin->instance)
        return;
    auto* ctx = static_cast<WebhookPluginCtx*>(plugin->instance);
    WebhookState* state = ctx->state;

    if (state) {
        // Unsubscribe via the host (no more callbacks), then disarm any in-flight.
        if (ctx->host && ctx->host->unsubscribe_evt)
            ctx->host->unsubscribe_evt(ctx->hostCtx, state->subHandle);
        state->running.store(false, std::memory_order_release);

        std::lock_guard<std::mutex> lock(state->postMutex);
        if (state->curl) {
            curl_easy_cleanup(state->curl);
            state->curl = nullptr;
        }
        if (state->headers) {
            curl_slist_free_all(state->headers);
            state->headers = nullptr;
        }
        curl_global_cleanup();
    }

    // `state` is intentionally leaked so an in-flight callback can't dangle.
    delete ctx;
    plugin->instance = nullptr;
}

// Sink: we have no downstream children, so frames are ignored.
void output_webhook_on_frame(zm_plugin_t* /*plugin*/, const void* /*buf*/,
                             size_t /*size*/) {
    // no-op
}

}  // namespace

extern "C" __attribute__((visibility("default"))) void zm_plugin_init(zm_plugin_t* plugin) {
    if (!plugin) return;
    plugin->version = ZM_PLUGIN_ABI_VERSION;
    plugin->type = ZM_PLUGIN_OUTPUT;
    plugin->instance = nullptr;
    plugin->start = output_webhook_start;
    plugin->stop = output_webhook_stop;
    plugin->on_frame = output_webhook_on_frame;
}

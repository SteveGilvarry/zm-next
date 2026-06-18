// output_mqtt: an OUTPUT sink that republishes pipeline events to an MQTT broker.
//
// It does NOT touch frames (on_frame is a no-op). Instead it subscribes to the
// in-process EventBus "plugin_event" channel and forwards each event payload to
// MQTT, deriving the topic from the event's "type" field. This lets
// detections / motion / descriptions drive Home Assistant or other automations.
//
// Lifetime note: EventBus has no unsubscribe and the callback can fire after
// stop(). All mutable state lives in a std::shared_ptr captured by the callback
// lambda, and an atomic `running` flag gates the callback. stop() flips running
// to false and tears down the mosquitto handle; the shared_ptr keeps the state
// object alive as long as the bus might still call back.

#include "mqtt_topic.hpp"

#include <zm_plugin.h>
#include <nlohmann/json.hpp>
#include <mosquitto.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

using json = nlohmann::json;

namespace {

struct MqttState {
    // Config.
    std::string host = "localhost";
    int port = 1883;
    std::string baseTopic = "zm-next";
    std::string clientId = "zm-next";
    std::string username;
    std::string password;
    bool haveUsername = false;
    bool havePassword = false;
    int qos = 0;

    // Runtime.
    struct mosquitto* mosq = nullptr;
    std::atomic<bool> running{false};
    std::mutex pubMutex;  // serialize publishes / teardown
    void* subHandle = nullptr;
};

// `state` is intentionally leaked on stop (after unsubscribe) so an in-flight
// host callback never touches freed memory.
struct MqttPluginCtx {
    zm_host_api_t* host = nullptr;
    void* hostCtx = nullptr;
    MqttState* state = nullptr;
};

void handleEvent(MqttState* st, const std::string& payload) {
    if (!st || !st->running.load(std::memory_order_acquire))
        return;

    std::string type;
    try {
        auto j = json::parse(payload);
        if (j.contains("type") && j["type"].is_string())
            type = j["type"].get<std::string>();
    } catch (const std::exception&) {
        // Not JSON or no usable type; fall back to the default topic.
        type.clear();
    }

    const std::string topic = zm::outputmqtt::topic_for(st->baseTopic, type);

    std::lock_guard<std::mutex> lock(st->pubMutex);
    if (!st->running.load(std::memory_order_acquire) || !st->mosq)
        return;

    const int rc = mosquitto_publish(
        st->mosq, nullptr, topic.c_str(),
        static_cast<int>(payload.size()), payload.data(),
        st->qos, /*retain=*/false);
    if (rc != MOSQ_ERR_SUCCESS) {
        ZM_LOG_WARN("output_mqtt: publish to '%s' failed: %s",
                    topic.c_str(), mosquitto_strerror(rc));
    }
}

int output_mqtt_start(zm_plugin_t* plugin, zm_host_api_t* host, void* host_ctx,
                      const char* json_cfg) {
    zm_plugin_set_log_context(host, host_ctx);

    auto* state = new MqttState();  // leaked on stop (see MqttPluginCtx)
    try {
        auto j = json::parse(json_cfg ? json_cfg : "{}");
        state->host = j.value("host", state->host);
        state->port = j.value("port", state->port);
        state->baseTopic = j.value("base_topic", state->baseTopic);
        state->clientId = j.value("client_id", state->clientId);
        state->qos = j.value("qos", state->qos);
        if (j.contains("username") && j["username"].is_string()) {
            state->username = j["username"].get<std::string>();
            state->haveUsername = true;
        }
        if (j.contains("password") && j["password"].is_string()) {
            state->password = j["password"].get<std::string>();
            state->havePassword = true;
        }
    } catch (const std::exception& e) {
        ZM_LOG_ERROR("output_mqtt: failed to parse config: %s", e.what());
    }

    mosquitto_lib_init();

    state->mosq = mosquitto_new(state->clientId.c_str(), /*clean_session=*/true,
                                /*obj=*/nullptr);
    if (!state->mosq) {
        ZM_LOG_ERROR("output_mqtt: mosquitto_new failed (client_id='%s')",
                     state->clientId.c_str());
        mosquitto_lib_cleanup();
        return -1;
    }

    if (state->haveUsername) {
        const char* pw = state->havePassword ? state->password.c_str() : nullptr;
        const int rc = mosquitto_username_pw_set(state->mosq,
                                                 state->username.c_str(), pw);
        if (rc != MOSQ_ERR_SUCCESS)
            ZM_LOG_WARN("output_mqtt: username_pw_set failed: %s",
                        mosquitto_strerror(rc));
    }

    const int crc = mosquitto_connect(state->mosq, state->host.c_str(),
                                      state->port, /*keepalive=*/60);
    if (crc != MOSQ_ERR_SUCCESS) {
        ZM_LOG_ERROR("output_mqtt: connect to %s:%d failed: %s",
                     state->host.c_str(), state->port, mosquitto_strerror(crc));
        mosquitto_destroy(state->mosq);
        state->mosq = nullptr;
        mosquitto_lib_cleanup();
        return -1;
    }

    const int lrc = mosquitto_loop_start(state->mosq);
    if (lrc != MOSQ_ERR_SUCCESS) {
        ZM_LOG_ERROR("output_mqtt: loop_start failed: %s", mosquitto_strerror(lrc));
        mosquitto_disconnect(state->mosq);
        mosquitto_destroy(state->mosq);
        state->mosq = nullptr;
        mosquitto_lib_cleanup();
        return -1;
    }

    state->running.store(true, std::memory_order_release);

    // Subscribe via the HOST so events reach us across the dlopen boundary. The
    // `state` user pointer is leaked on stop so an in-flight callback is safe.
    if (host && host->subscribe_evt) {
        state->subHandle = host->subscribe_evt(
            host_ctx,
            [](void* user, const char* json) {
                handleEvent(static_cast<MqttState*>(user), json ? json : "");
            },
            state);
    }

    auto* ctx = new MqttPluginCtx{host, host_ctx, state};
    plugin->instance = ctx;

    ZM_LOG_INFO("output_mqtt: connected to %s:%d base_topic='%s' client_id='%s' qos=%d",
                state->host.c_str(), state->port, state->baseTopic.c_str(),
                state->clientId.c_str(), state->qos);
    return 0;
}

void output_mqtt_stop(zm_plugin_t* plugin) {
    if (!plugin || !plugin->instance)
        return;
    auto* ctx = static_cast<MqttPluginCtx*>(plugin->instance);
    MqttState* state = ctx->state;

    if (state) {
        // Unsubscribe via the host (no more callbacks), then disarm any in-flight.
        if (ctx->host && ctx->host->unsubscribe_evt)
            ctx->host->unsubscribe_evt(ctx->hostCtx, state->subHandle);
        state->running.store(false, std::memory_order_release);

        std::lock_guard<std::mutex> lock(state->pubMutex);
        if (state->mosq) {
            mosquitto_loop_stop(state->mosq, /*force=*/true);
            mosquitto_disconnect(state->mosq);
            mosquitto_destroy(state->mosq);
            state->mosq = nullptr;
            mosquitto_lib_cleanup();
        }
    }

    // `state` is intentionally leaked so an in-flight callback can't dangle.
    delete ctx;
    plugin->instance = nullptr;
}

// Sink: we have no downstream children, so frames are ignored.
void output_mqtt_on_frame(zm_plugin_t* /*plugin*/, const void* /*buf*/,
                          size_t /*size*/) {
    // no-op
}

}  // namespace

extern "C" __attribute__((visibility("default"))) void zm_plugin_init(zm_plugin_t* plugin) {
    if (!plugin) return;
    plugin->version = ZM_PLUGIN_ABI_VERSION;
    plugin->type = ZM_PLUGIN_OUTPUT;
    plugin->instance = nullptr;
    plugin->start = output_mqtt_start;
    plugin->stop = output_mqtt_stop;
    plugin->on_frame = output_mqtt_on_frame;
}

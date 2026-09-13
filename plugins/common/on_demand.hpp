#pragma once
// on_demand.hpp — shared handling for on-demand worker commands.
//
// zm-api sends a Command over the worker socket, e.g.
//   {"cmd":"snapshot_now","request_id":42,"stream_id":0}
//   {"cmd":"describe_now","request_id":43,"prompt":"Is the gate open?"}
// zm-core answers the Command at once with {"ok":true,"message":"dispatched"} and
// re-publishes the JSON on the host event bus. The plugin that owns the command
// does the work and publishes a result event tagged with the same request_id,
// which travels back over the socket as an EVENT. Every command gets exactly one
// result event, success or failure, so the caller never waits on silence.
//
// Header-only, no ABI; plugins/common/tests covers it.

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace zm::ondemand {

struct Request {
    std::string cmd;
    uint64_t request_id = 0;
    bool has_stream = false;   // the caller named a stream
    uint32_t stream_id = 0;
    nlohmann::json args;       // the whole command object, for command-specific keys
};

// True when `j` is the command `name`; fills `out`.
inline bool parse(const nlohmann::json& j, const std::string& name, Request& out) {
    if (!j.is_object()) return false;
    const auto it = j.find("cmd");
    if (it == j.end() || !it->is_string() || it->get<std::string>() != name) return false;
    out.cmd = name;
    out.request_id = 0;
    if (j.contains("request_id") && j["request_id"].is_number_unsigned())
        out.request_id = j["request_id"].get<uint64_t>();
    else if (j.contains("request_id") && j["request_id"].is_number_integer() &&
             j["request_id"].get<int64_t>() >= 0)
        out.request_id = static_cast<uint64_t>(j["request_id"].get<int64_t>());
    out.has_stream = j.contains("stream_id") && j["stream_id"].is_number_integer() &&
                     j["stream_id"].get<int64_t>() >= 0;
    out.stream_id = out.has_stream ? static_cast<uint32_t>(j["stream_id"].get<int64_t>()) : 0;
    out.args = j;
    return true;
}

// Whether this plugin instance should answer: a command that names a stream is
// only for instances whose stream filter admits it; one that names no stream is
// for every instance.
inline bool for_this_instance(const Request& r, const std::vector<uint32_t>& stream_filter) {
    if (!r.has_stream || stream_filter.empty()) return true;
    return std::find(stream_filter.begin(), stream_filter.end(), r.stream_id) != stream_filter.end();
}

// Tag a result event so the caller can match it to its command.
inline void tag(nlohmann::json& evt, const Request& r, bool ok, const std::string& error = {}) {
    evt["request_id"] = r.request_id;
    evt["on_demand"] = true;
    evt["ok"] = ok;
    if (!ok) evt["error"] = error;
}

}  // namespace zm::ondemand

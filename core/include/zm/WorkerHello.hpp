#pragma once
// WorkerHello — what a zm-next worker tells each peer that connects to its
// socket (message type 0x14, docs/Worker_Control_Protocol.md "Worker hello").
//
// Built from three inputs: the plugin catalog of this build (manifest.json plus
// each plugin's <kind>.schema.json), the active pipeline JSON, and static facts
// (version, monitor id, hardware backends compiled in).
//
// Secrets never appear in the hello. pipeline_hash covers the pipeline with
// secret values replaced, so it is safe to show any peer. Whether the secrets
// changed is reported separately, only to control peers, as
// secrets_fingerprint = sha256(salt + secrets); zm-api holds the secrets anyway.
// A peer that could see the fingerprint unsalted could brute-force weak
// passwords, which is why observers never get it.

#include <nlohmann/json.hpp>

#include <set>
#include <string>
#include <vector>

namespace zm::worker {

struct PluginInfo {
    std::string kind;
    std::string version;                 // schema x-plugin-version, "" without a schema
    std::string schema_sha256;           // "" without a schema
    nlohmann::json schema;               // null without a schema
    std::set<std::string> secret_keys;   // property names marked x-secret anywhere in the schema
};

struct Catalog {
    std::vector<PluginInfo> plugins;     // manifest order
    std::string error;                   // non-empty if the manifest couldn't be read
    const PluginInfo* find(const std::string& kind) const;
};

// Key names treated as secret for plugins without a schema (or in addition to
// x-secret ones): values of these are never hashed into pipeline_hash.
const std::set<std::string>& default_secret_keys();

// Read <plugins_dir>/manifest.json ({"plugins":[{"kind":"..."}, ...]}) and each
// <plugins_dir>/<kind>/<kind>.schema.json that exists.
Catalog load_catalog(const std::string& plugins_dir);

// Copy of `pipeline` (the {"plugins":[...]} tree) with every secret value under
// a node's cfg/config replaced by "<secret>"; the removed values are appended to
// `secrets` as {path, value} pairs in a stable order.
nlohmann::json redact_pipeline(const nlohmann::json& pipeline, const Catalog& catalog,
                               std::vector<std::pair<std::string, std::string>>* secrets = nullptr);

// "sha256:<hex>" of the redacted pipeline (keys sorted, compact).
std::string pipeline_hash(const nlohmann::json& pipeline, const Catalog& catalog);

// "sha256:<hex>" of salt + the pipeline's secrets in stable order; "" when the
// pipeline has none.
std::string secrets_fingerprint(const nlohmann::json& pipeline, const Catalog& catalog,
                                const std::string& salt);

struct HelloFacts {
    std::string version;
    std::string commit;
    uint32_t plugin_abi = 0;
    int64_t monitor_id = 0;
    std::string state;                   // unconfigured | configuring | running | stopping
    std::vector<std::string> hw_backends;
};

// Hello fields every peer may see.
nlohmann::json hello_public(const HelloFacts& facts, const Catalog& catalog,
                            const nlohmann::json* pipeline);

// Fields added for control peers only (currently secrets_fingerprint).
nlohmann::json hello_control_extra(const Catalog& catalog, const nlohmann::json* pipeline,
                                   const std::string& salt);

// describe_plugins command result: {"<kind>": {"version":..., "schema": {...}}}.
// Empty `kinds` = every plugin with a schema. Unknown kinds map to null.
nlohmann::json describe_plugins(const Catalog& catalog, const std::vector<std::string>& kinds);

}  // namespace zm::worker

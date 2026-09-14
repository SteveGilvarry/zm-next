#pragma once
// WorkerConfig — checks and prepares the pipeline sent by the `configure`
// command (docs/Worker_Control_Protocol.md "Configure").
//
// validate_pipeline() checks the {"plugins":[...]} tree against the plugin
// catalog: every node's kind must be built into this worker, its cfg must match
// the plugin's JSON Schema, every {"$secret": name} must name an entry of the
// secrets map, and every x-secret property must be given as such a reference.
// The validator implements the schema keywords the plugin schemas use: type,
// enum, minimum, maximum, exclusiveMinimum, exclusiveMaximum, properties,
// required, additionalProperties and items. Unknown keywords are ignored.
//
// Error messages name the path and the rule, never the value, so a secret put
// in the wrong place is not echoed back.

#include "zm/WorkerHello.hpp"

#include <nlohmann/json.hpp>

#include <set>
#include <string>
#include <vector>

namespace zm::worker {

struct ConfigError {
    std::string path;      // e.g. plugins[0].children[1].cfg.iou_threshold
    std::string message;   // e.g. must be <= 1
};

// Configure-time checks on the pipeline and its secrets map.
std::vector<ConfigError> validate_pipeline(const nlohmann::json& pipeline, const Catalog& catalog,
                                           const nlohmann::json& secrets);

// Check one value against a schema (exposed for tests). `secrets` resolves
// $secret references; pass an empty object when none are allowed.
void validate_value(const nlohmann::json& value, const nlohmann::json& schema, const std::string& path,
                    const nlohmann::json& secrets, std::vector<ConfigError>& errors);

nlohmann::json errors_json(const std::vector<ConfigError>& errors);

struct ResolvedPipeline {
    nlohmann::json pipeline;              // references replaced by their values (contains secrets)
    std::set<std::string> secret_paths;   // JSON pointers of the replaced references
    std::vector<std::string> secret_values;
};

// Replace every {"$secret": name} with secrets[name]. Call after validation;
// unknown names become empty strings.
ResolvedPipeline resolve_secrets(const nlohmann::json& pipeline, const nlohmann::json& secrets);

}  // namespace zm::worker

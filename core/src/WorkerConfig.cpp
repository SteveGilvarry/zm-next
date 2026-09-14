#include "zm/WorkerConfig.hpp"

#include <cmath>

namespace zm::worker {

using json = nlohmann::json;

namespace {

// {"$secret": "name"} and nothing else.
bool is_secret_ref(const json& v) {
    return v.is_object() && v.size() == 1 && v.contains("$secret") && v["$secret"].is_string();
}

std::string json_pointer_token(const std::string& key) {
    std::string out;
    for (char c : key) {
        if (c == '~') out += "~0";
        else if (c == '/') out += "~1";
        else out += c;
    }
    return out;
}

std::string number_text(const json& n) {
    if (n.is_number_integer()) return n.dump();
    double d = n.get<double>();
    if (std::floor(d) == d && std::fabs(d) < 1e15) return std::to_string(static_cast<long long>(d));
    return n.dump();
}

bool matches_type(const json& v, const std::string& type) {
    if (type == "object") return v.is_object();
    if (type == "array") return v.is_array();
    if (type == "string") return v.is_string();
    if (type == "boolean") return v.is_boolean();
    if (type == "null") return v.is_null();
    if (type == "number") return v.is_number();
    if (type == "integer") {
        if (v.is_number_integer()) return true;
        return v.is_number_float() && std::floor(v.get<double>()) == v.get<double>();
    }
    return true;  // unknown type names don't reject
}

// References may appear anywhere in cfg, including in plugins without a schema.
void check_refs(const json& value, const std::string& path, const json& secrets,
                std::vector<ConfigError>& errors) {
    if (value.is_object()) {
        if (value.contains("$secret")) {
            if (!is_secret_ref(value))
                errors.push_back({path, "a $secret reference is {\"$secret\": \"<name>\"} with no other keys"});
            else if (!secrets.contains(value["$secret"].get<std::string>()))
                errors.push_back({path, "unknown secret '" + value["$secret"].get<std::string>() + "'"});
            return;
        }
        for (const auto& [k, sub] : value.items()) check_refs(sub, path + "." + k, secrets, errors);
    } else if (value.is_array()) {
        for (size_t i = 0; i < value.size(); ++i)
            check_refs(value[i], path + "[" + std::to_string(i) + "]", secrets, errors);
    }
}

void validate_node(const json& node, const std::string& path, const Catalog& catalog,
                   const json& secrets, std::vector<ConfigError>& errors) {
    static const std::set<std::string> kNodeKeys = {"id", "kind", "cfg", "config", "children", "queue_depth"};
    if (!node.is_object()) {
        errors.push_back({path, "must be an object"});
        return;
    }
    for (const auto& [k, v] : node.items()) {
        (void)v;
        if (k == "path")
            errors.push_back({path + ".path", "not accepted in configure; name the plugin with kind"});
        else if (!kNodeKeys.count(k))
            errors.push_back({path + "." + k, "unknown key"});
    }
    const PluginInfo* info = nullptr;
    if (!node.contains("kind") || !node["kind"].is_string()) {
        errors.push_back({path + ".kind", "is required and must be a string"});
    } else {
        const std::string kind = node["kind"].get<std::string>();
        info = catalog.find(kind);
        if (!info) errors.push_back({path + ".kind", "plugin '" + kind + "' is not built into this worker"});
    }
    if (node.contains("id") && !node["id"].is_string())
        errors.push_back({path + ".id", "must be a string"});
    if (node.contains("queue_depth") &&
        (!matches_type(node["queue_depth"], "integer") || node["queue_depth"].get<double>() < 1))
        errors.push_back({path + ".queue_depth", "must be an integer >= 1"});
    if (node.contains("cfg") && node.contains("config"))
        errors.push_back({path, "give cfg or config, not both"});

    for (const char* cfgKey : {"cfg", "config"}) {
        if (!node.contains(cfgKey)) continue;
        const std::string cfgPath = path + "." + cfgKey;
        if (info && info->schema.is_object()) {
            validate_value(node[cfgKey], info->schema, cfgPath, secrets, errors);
        } else {
            if (!node[cfgKey].is_object()) errors.push_back({cfgPath, "must be an object"});
            check_refs(node[cfgKey], cfgPath, secrets, errors);
        }
    }

    if (node.contains("children")) {
        if (!node["children"].is_array()) {
            errors.push_back({path + ".children", "must be an array"});
        } else {
            for (size_t i = 0; i < node["children"].size(); ++i)
                validate_node(node["children"][i], path + ".children[" + std::to_string(i) + "]",
                              catalog, secrets, errors);
        }
    }
}

void resolve(json& value, const std::string& pointer, const json& secrets, ResolvedPipeline& out) {
    if (is_secret_ref(value)) {
        const std::string name = value["$secret"].get<std::string>();
        std::string secret;
        if (secrets.contains(name) && secrets[name].is_string()) secret = secrets[name].get<std::string>();
        out.secret_paths.insert(pointer);
        out.secret_values.push_back(secret);
        value = secret;
    } else if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it)
            resolve(*it, pointer + "/" + json_pointer_token(it.key()), secrets, out);
    } else if (value.is_array()) {
        for (size_t i = 0; i < value.size(); ++i)
            resolve(value[i], pointer + "/" + std::to_string(i), secrets, out);
    }
}

}  // namespace

void validate_value(const json& value, const json& schema, const std::string& path, const json& secrets,
                    std::vector<ConfigError>& errors) {
    if (!schema.is_object()) return;

    if (value.is_object() && value.contains("$secret")) {
        const size_t before = errors.size();
        check_refs(value, path, secrets, errors);
        if (errors.size() != before) return;
        bool stringOk = !schema.contains("type") || schema["type"] == "string";
        if (schema.contains("type") && schema["type"].is_array())
            for (const auto& t : schema["type"]) stringOk = stringOk || t == "string";
        if (!stringOk) errors.push_back({path, "a $secret reference can only replace a string"});
        return;
    }
    if (schema.value("x-secret", false) && value.is_string() && !value.get<std::string>().empty()) {
        errors.push_back({path, "secret must be a $secret reference"});
        return;
    }

    if (schema.contains("type")) {
        const json& t = schema["type"];
        bool ok = false;
        std::string want;
        if (t.is_string()) {
            ok = matches_type(value, t.get<std::string>());
            want = t.get<std::string>();
        } else if (t.is_array()) {
            for (const auto& one : t) {
                if (!one.is_string()) continue;
                ok = ok || matches_type(value, one.get<std::string>());
                want += (want.empty() ? "" : " or ") + one.get<std::string>();
            }
        } else {
            ok = true;
        }
        if (!ok) {
            errors.push_back({path, "must be " + (want == "integer" || want == "array" || want == "object"
                                                      ? std::string("an ") : std::string("a ")) + want});
            return;
        }
    }

    if (schema.contains("enum") && schema["enum"].is_array()) {
        bool found = false;
        for (const auto& e : schema["enum"])
            if (e == value || (e.is_number() && value.is_number() && e.get<double>() == value.get<double>()))
                found = true;
        if (!found) errors.push_back({path, "must be one of " + schema["enum"].dump()});
    }

    if (value.is_number()) {
        const double v = value.get<double>();
        struct Bound { const char* key; const char* op; bool (*fails)(double v, double b); };
        static const Bound bounds[] = {
            {"minimum", ">=", [](double v, double b) { return v < b; }},
            {"maximum", "<=", [](double v, double b) { return v > b; }},
            {"exclusiveMinimum", ">", [](double v, double b) { return v <= b; }},
            {"exclusiveMaximum", "<", [](double v, double b) { return v >= b; }},
        };
        for (const auto& b : bounds)
            if (schema.contains(b.key) && schema[b.key].is_number() && b.fails(v, schema[b.key].get<double>()))
                errors.push_back({path, std::string("must be ") + b.op + " " + number_text(schema[b.key])});
    }

    if (value.is_object()) {
        const json empty = json::object();
        const json& props = (schema.contains("properties") && schema["properties"].is_object())
                                ? schema["properties"] : empty;
        if (schema.contains("required") && schema["required"].is_array())
            for (const auto& r : schema["required"])
                if (r.is_string() && !value.contains(r.get<std::string>()))
                    errors.push_back({path + "." + r.get<std::string>(), "is required"});
        for (const auto& [k, sub] : value.items()) {
            const std::string childPath = path + "." + k;
            if (props.contains(k)) {
                validate_value(sub, props[k], childPath, secrets, errors);
            } else if (schema.contains("additionalProperties")) {
                const json& ap = schema["additionalProperties"];
                if (ap.is_boolean() && !ap.get<bool>()) errors.push_back({childPath, "unknown key"});
                else if (ap.is_object()) validate_value(sub, ap, childPath, secrets, errors);
                else check_refs(sub, childPath, secrets, errors);
            } else {
                check_refs(sub, childPath, secrets, errors);
            }
        }
    }

    if (value.is_array()) {
        for (size_t i = 0; i < value.size(); ++i) {
            const std::string childPath = path + "[" + std::to_string(i) + "]";
            if (schema.contains("items") && schema["items"].is_object())
                validate_value(value[i], schema["items"], childPath, secrets, errors);
            else
                check_refs(value[i], childPath, secrets, errors);
        }
    }
}

std::vector<ConfigError> validate_pipeline(const json& pipeline, const Catalog& catalog, const json& secrets) {
    std::vector<ConfigError> errors;
    if (!secrets.is_object()) {
        errors.push_back({"secrets", "must be an object of strings"});
        return errors;
    }
    for (const auto& [name, v] : secrets.items())
        if (!v.is_string()) errors.push_back({"secrets." + name, "must be a string"});
    if (!pipeline.is_object()) {
        errors.push_back({"pipeline", "must be an object"});
        return errors;
    }
    if (!pipeline.contains("plugins") || !pipeline["plugins"].is_array() || pipeline["plugins"].empty()) {
        errors.push_back({"plugins", "must be a non-empty array"});
        return errors;
    }
    for (size_t i = 0; i < pipeline["plugins"].size(); ++i)
        validate_node(pipeline["plugins"][i], "plugins[" + std::to_string(i) + "]", catalog, secrets, errors);
    return errors;
}

json errors_json(const std::vector<ConfigError>& errors) {
    json out = json::array();
    for (const auto& e : errors) out.push_back({{"path", e.path}, {"message", e.message}});
    return out;
}

ResolvedPipeline resolve_secrets(const json& pipeline, const json& secrets) {
    ResolvedPipeline out;
    out.pipeline = pipeline;
    resolve(out.pipeline, "", secrets.is_object() ? secrets : json::object(), out);
    return out;
}

}  // namespace zm::worker

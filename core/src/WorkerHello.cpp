#include "zm/WorkerHello.hpp"
#include "zm/sha256.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace zm::worker {

using json = nlohmann::json;

const PluginInfo* Catalog::find(const std::string& kind) const {
    for (const auto& p : plugins)
        if (p.kind == kind) return &p;
    return nullptr;
}

const std::set<std::string>& default_secret_keys() {
    static const std::set<std::string> keys = {"password", "username", "auth_header",
                                               "api_key", "token", "secret"};
    return keys;
}

namespace {

bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

void collect_secret_keys(const json& node, std::set<std::string>& out) {
    if (!node.is_object()) return;
    if (node.contains("properties") && node["properties"].is_object()) {
        for (const auto& [name, sub] : node["properties"].items()) {
            if (sub.is_object() && sub.value("x-secret", false)) out.insert(name);
            collect_secret_keys(sub, out);
        }
    }
    for (const char* k : {"items", "additionalProperties"})
        if (node.contains(k)) collect_secret_keys(node[k], out);
    for (const char* k : {"oneOf", "anyOf", "allOf"})
        if (node.contains(k) && node[k].is_array())
            for (const auto& sub : node[k]) collect_secret_keys(sub, out);
}

// Replace secret values inside one plugin's cfg, recording them with their path.
void redact_cfg(json& value, const std::set<std::string>& secret, const std::string& path,
                std::vector<std::pair<std::string, std::string>>* secrets) {
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            const std::string child = path + "/" + it.key();
            if (secret.count(it.key()) && (it->is_string() || it->is_number())) {
                if (secrets) secrets->emplace_back(child, it->is_string() ? it->get<std::string>() : it->dump());
                *it = "<secret>";
            } else {
                redact_cfg(*it, secret, child, secrets);
            }
        }
    } else if (value.is_array()) {
        for (size_t i = 0; i < value.size(); ++i)
            redact_cfg(value[i], secret, path + "/" + std::to_string(i), secrets);
    }
}

void redact_nodes(json& nodes, const Catalog& catalog, const std::string& path,
                  std::vector<std::pair<std::string, std::string>>* secrets) {
    if (!nodes.is_array()) return;
    for (size_t i = 0; i < nodes.size(); ++i) {
        json& node = nodes[i];
        if (!node.is_object()) continue;
        const std::string here = path + "/" + std::to_string(i);
        std::set<std::string> secret = default_secret_keys();
        if (node.contains("kind") && node["kind"].is_string())
            if (const auto* p = catalog.find(node["kind"].get<std::string>()))
                secret.insert(p->secret_keys.begin(), p->secret_keys.end());
        for (const char* cfgKey : {"cfg", "config"})
            if (node.contains(cfgKey)) redact_cfg(node[cfgKey], secret, here + "/" + cfgKey, secrets);
        if (node.contains("children")) redact_nodes(node["children"], catalog, here + "/children", secrets);
    }
}

}  // namespace

Catalog load_catalog(const std::string& plugins_dir) {
    Catalog cat;
    std::string text;
    if (!read_file(plugins_dir + "/manifest.json", text)) {
        cat.error = "no manifest at " + plugins_dir + "/manifest.json";
        return cat;
    }
    json manifest = json::parse(text, nullptr, false);
    if (!manifest.is_object() || !manifest.contains("plugins") || !manifest["plugins"].is_array()) {
        cat.error = "manifest.json is not {\"plugins\":[...]}";
        return cat;
    }
    for (const auto& entry : manifest["plugins"]) {
        if (!entry.is_object() || !entry.contains("kind") || !entry["kind"].is_string()) continue;
        PluginInfo info;
        info.kind = entry["kind"].get<std::string>();
        std::string schemaText;
        if (read_file(plugins_dir + "/" + info.kind + "/" + info.kind + ".schema.json", schemaText)) {
            info.schema = json::parse(schemaText, nullptr, false);
            if (info.schema.is_discarded()) {
                info.schema = nullptr;
            } else {
                info.schema_sha256 = "sha256:" + Sha256::hex(schemaText);
                info.version = info.schema.value("x-plugin-version", std::string());
                collect_secret_keys(info.schema, info.secret_keys);
            }
        }
        cat.plugins.push_back(std::move(info));
    }
    return cat;
}

json redact_pipeline(const json& pipeline, const Catalog& catalog,
                     std::vector<std::pair<std::string, std::string>>* secrets, const SecretPaths* extra) {
    json copy = pipeline;
    if (copy.is_object() && copy.contains("plugins")) redact_nodes(copy["plugins"], catalog, "/plugins", secrets);
    if (extra) {
        for (const auto& pointer : *extra) {
            const json::json_pointer ptr(pointer);
            if (!copy.contains(ptr)) continue;
            json& v = copy[ptr];
            if (v.is_string() && v.get<std::string>() == "<secret>") continue;  // already taken by key name
            if (secrets) secrets->emplace_back(pointer, v.is_string() ? v.get<std::string>() : v.dump());
            v = "<secret>";
        }
    }
    return copy;
}

std::string pipeline_hash(const json& pipeline, const Catalog& catalog, const SecretPaths* extra) {
    return "sha256:" + Sha256::hex(redact_pipeline(pipeline, catalog, nullptr, extra).dump());
}

std::string secrets_fingerprint(const json& pipeline, const Catalog& catalog, const std::string& salt,
                                const SecretPaths* extra) {
    std::vector<std::pair<std::string, std::string>> secrets;
    redact_pipeline(pipeline, catalog, &secrets, extra);
    if (secrets.empty()) return "";
    std::sort(secrets.begin(), secrets.end());
    json list = json::array();
    for (const auto& [path, value] : secrets) list.push_back({path, value});
    return "sha256:" + Sha256::hex(salt + list.dump());
}

json hello_public(const HelloFacts& facts, const Catalog& catalog, const json* pipeline,
                  const SecretPaths* extra) {
    json plugins = json::array();
    for (const auto& p : catalog.plugins) {
        json entry = {{"kind", p.kind}};
        entry["version"] = p.version.empty() ? json(nullptr) : json(p.version);
        entry["schema_sha256"] = p.schema_sha256.empty() ? json(nullptr) : json(p.schema_sha256);
        plugins.push_back(std::move(entry));
    }
    json hello = {
        {"protocol", {{"canonical", 1}, {"control", 1}}},
        {"zm_next", {{"version", facts.version}, {"commit", facts.commit}, {"plugin_abi", facts.plugin_abi}}},
        {"monitor_id", facts.monitor_id},
        {"state", facts.state},
        {"plugins", std::move(plugins)},
        {"hw", {{"backends", facts.hw_backends}}},
    };
    hello["pipeline_hash"] = pipeline ? json(pipeline_hash(*pipeline, catalog, extra)) : json(nullptr);
    if (!catalog.error.empty()) hello["catalog_error"] = catalog.error;
    return hello;
}

json hello_control_extra(const Catalog& catalog, const json* pipeline, const std::string& salt,
                         const SecretPaths* extra) {
    json out = json::object();
    if (pipeline) {
        const std::string fp = secrets_fingerprint(*pipeline, catalog, salt, extra);
        out["secrets_fingerprint"] = fp.empty() ? json(nullptr) : json(fp);
    }
    return out;
}

json describe_plugins(const Catalog& catalog, const std::vector<std::string>& kinds) {
    json out = json::object();
    if (kinds.empty()) {
        for (const auto& p : catalog.plugins)
            if (!p.schema.is_null()) out[p.kind] = {{"version", p.version}, {"schema", p.schema}};
        return out;
    }
    for (const auto& k : kinds) {
        const auto* p = catalog.find(k);
        if (!p || p->schema.is_null()) out[k] = nullptr;
        else out[k] = {{"version", p->version}, {"schema", p->schema}};
    }
    return out;
}

}  // namespace zm::worker

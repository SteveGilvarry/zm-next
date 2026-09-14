#include "zm/Redactor.hpp"
#include "zm/WorkerConfig.hpp"
#include "zm/WorkerHello.hpp"

#include <gtest/gtest.h>

#include <algorithm>

using json = nlohmann::json;
using namespace zm::worker;

namespace {

// A catalog built in memory: one plugin with a schema, one without.
Catalog test_catalog() {
    Catalog cat;
    PluginInfo cap;
    cap.kind = "capture_rtsp_multi";
    cap.schema = json::parse(R"({
        "type":"object","additionalProperties":false,
        "properties":{"streams":{"type":"array","items":{"type":"object","additionalProperties":false,
            "required":["url"],
            "properties":{
                "url":{"type":"string"},
                "transport":{"type":"string","enum":["tcp","udp"]},
                "max_retry_attempts":{"type":"integer","minimum":-1},
                "username":{"type":"string","x-secret":true},
                "password":{"type":"string","x-secret":true}}}}}})");
    cap.secret_keys = {"username", "password"};
    cat.plugins.push_back(cap);
    PluginInfo trk;
    trk.kind = "tracker";
    trk.schema = json::parse(R"({"type":"object","additionalProperties":false,
        "properties":{"iou_threshold":{"type":"number","exclusiveMinimum":0,"maximum":1}}})");
    cat.plugins.push_back(trk);
    PluginInfo noSchema;
    noSchema.kind = "capture_file";
    cat.plugins.push_back(noSchema);
    return cat;
}

json pipeline_with(const json& streamCfg, const json& children = json::array()) {
    return {{"plugins", json::array({{{"id", "cap"}, {"kind", "capture_rtsp_multi"},
                                      {"cfg", {{"streams", json::array({streamCfg})}}},
                                      {"children", children}}})}};
}

bool has_error(const std::vector<ConfigError>& errors, const std::string& path, const std::string& msgPart) {
    return std::any_of(errors.begin(), errors.end(), [&](const ConfigError& e) {
        return e.path == path && e.message.find(msgPart) != std::string::npos;
    });
}

std::string dump(const std::vector<ConfigError>& errors) { return errors_json(errors).dump(); }

}  // namespace

TEST(WorkerConfig, ValidPipelineWithReferencesPasses) {
    const auto cat = test_catalog();
    const json p = pipeline_with({{"url", "rtsp://10.0.0.5/102"},
                                  {"username", {{"$secret", "cam.user"}}},
                                  {"password", {{"$secret", "cam.pass"}}}},
                                 json::array({{{"kind", "tracker"}, {"cfg", {{"iou_threshold", 0.3}}},
                                               {"queue_depth", 4}}}));
    const auto errors = validate_pipeline(p, cat, {{"cam.user", "admin"}, {"cam.pass", "p@ss:w/d"}});
    EXPECT_TRUE(errors.empty()) << dump(errors);
}

TEST(WorkerConfig, SchemaRulesReportPathAndRule) {
    const auto cat = test_catalog();
    const json p = pipeline_with({{"transport", "http"}, {"max_retry_attempts", 2.5}, {"bogus", 1}},
                                 json::array({{{"kind", "tracker"}, {"cfg", {{"iou_threshold", 1.5}}}},
                                              {{"kind", "tracker"}, {"cfg", {{"iou_threshold", 0}}}}}));
    const auto errors = validate_pipeline(p, cat, json::object());
    EXPECT_TRUE(has_error(errors, "plugins[0].cfg.streams[0].url", "is required")) << dump(errors);
    EXPECT_TRUE(has_error(errors, "plugins[0].cfg.streams[0].transport", "must be one of [\"tcp\",\"udp\"]"));
    EXPECT_TRUE(has_error(errors, "plugins[0].cfg.streams[0].max_retry_attempts", "must be an integer"));
    EXPECT_TRUE(has_error(errors, "plugins[0].cfg.streams[0].bogus", "unknown key"));
    EXPECT_TRUE(has_error(errors, "plugins[0].children[0].cfg.iou_threshold", "must be <= 1"));
    EXPECT_TRUE(has_error(errors, "plugins[0].children[1].cfg.iou_threshold", "must be > 0"));
    EXPECT_EQ(errors.size(), 6u) << dump(errors);
}

TEST(WorkerConfig, IntegralFloatCountsAsInteger) {
    const auto cat = test_catalog();
    const auto errors = validate_pipeline(pipeline_with({{"url", "rtsp://h/"}, {"max_retry_attempts", 5.0}}),
                                          cat, json::object());
    EXPECT_TRUE(errors.empty()) << dump(errors);
}

TEST(WorkerConfig, SecretsMustBeReferencesAndExist) {
    const auto cat = test_catalog();
    const json p = pipeline_with({{"url", "rtsp://h/"},
                                  {"username", "admin"},
                                  {"password", {{"$secret", "missing"}}}});
    const auto errors = validate_pipeline(p, cat, {{"other", "x"}});
    EXPECT_TRUE(has_error(errors, "plugins[0].cfg.streams[0].username", "must be a $secret reference"));
    EXPECT_TRUE(has_error(errors, "plugins[0].cfg.streams[0].password", "unknown secret 'missing'"));
    // The literal value is never echoed back.
    EXPECT_EQ(dump(errors).find("admin"), std::string::npos);
}

TEST(WorkerConfig, ReferenceShapeAndTypeChecked) {
    const auto cat = test_catalog();
    json p = pipeline_with({{"url", "rtsp://h/"}, {"password", {{"$secret", "a"}, {"extra", 1}}}},
                           json::array({{{"kind", "tracker"}, {"cfg", {{"iou_threshold", {{"$secret", "a"}}}}}},
                                        {{"kind", "capture_file"}, {"cfg", {{"token", {{"$secret", "nope"}}}}}}}));
    const auto errors = validate_pipeline(p, cat, {{"a", "secret-a"}});
    EXPECT_TRUE(has_error(errors, "plugins[0].cfg.streams[0].password", "no other keys")) << dump(errors);
    EXPECT_TRUE(has_error(errors, "plugins[0].children[0].cfg.iou_threshold", "can only replace a string"));
    // References are checked in plugins without a schema too.
    EXPECT_TRUE(has_error(errors, "plugins[0].children[1].cfg.token", "unknown secret 'nope'"));
}

TEST(WorkerConfig, NodeRules) {
    const auto cat = test_catalog();
    const json p = {{"plugins", json::array({
        {{"kind", "capture_rtsp_multi"}, {"path", "/tmp/evil.so"}, {"cfg", json::object()}, {"config", json::object()}},
        {{"kind", "not_built"}, {"queue_depth", 0}, {"colour", "red"}},
        {{"id", 7}},
    })}};
    const auto errors = validate_pipeline(p, cat, json::object());
    EXPECT_TRUE(has_error(errors, "plugins[0].path", "not accepted in configure")) << dump(errors);
    EXPECT_TRUE(has_error(errors, "plugins[0]", "not both"));
    EXPECT_TRUE(has_error(errors, "plugins[1].kind", "not built into this worker"));
    EXPECT_TRUE(has_error(errors, "plugins[1].queue_depth", ">= 1"));
    EXPECT_TRUE(has_error(errors, "plugins[1].colour", "unknown key"));
    EXPECT_TRUE(has_error(errors, "plugins[2].kind", "is required"));
    EXPECT_TRUE(has_error(errors, "plugins[2].id", "must be a string"));

    EXPECT_TRUE(has_error(validate_pipeline({{"plugins", json::array()}}, cat, json::object()), "plugins", "non-empty"));
    EXPECT_TRUE(has_error(validate_pipeline(p, cat, {{"k", 5}}), "secrets.k", "must be a string"));
}

TEST(WorkerConfig, ResolveReplacesReferencesAndRecordsPointers) {
    const json p = pipeline_with({{"url", "rtsp://h/"},
                                  {"username", {{"$secret", "u"}}},
                                  {"password", {{"$secret", "p"}}}},
                                 json::array({{{"kind", "capture_file"}, {"cfg", {{"a/b", {{"$secret", "p"}}}}}}}));
    const auto r = resolve_secrets(p, {{"u", "admin"}, {"p", "p@ss:w/d"}});
    EXPECT_EQ(r.pipeline["plugins"][0]["cfg"]["streams"][0]["username"], "admin");
    EXPECT_EQ(r.pipeline["plugins"][0]["cfg"]["streams"][0]["password"], "p@ss:w/d");
    EXPECT_EQ(r.pipeline["plugins"][0]["children"][0]["cfg"]["a/b"], "p@ss:w/d");
    EXPECT_EQ(r.secret_paths, (std::set<std::string>{"/plugins/0/cfg/streams/0/username",
                                                     "/plugins/0/cfg/streams/0/password",
                                                     "/plugins/0/children/0/cfg/a~1b"}));
    EXPECT_EQ(r.secret_values.size(), 3u);
}

TEST(WorkerConfig, ConfiguredAndLiteralPipelinesHashTheSame) {
    const auto cat = test_catalog();
    const json refs = pipeline_with({{"url", "rtsp://h/"}, {"password", {{"$secret", "p"}}}},
                                    json::array({{{"kind", "capture_file"}, {"cfg", {{"key", {{"$secret", "k"}}}}}}}));
    const auto r1 = resolve_secrets(refs, {{"p", "one"}, {"k", "key-one"}});
    const auto r2 = resolve_secrets(refs, {{"p", "two"}, {"k", "key-two"}});
    const json literal = pipeline_with({{"url", "rtsp://h/"}, {"password", "three"}},
                                       json::array({{{"kind", "capture_file"}, {"cfg", {{"key", "<secret>"}}}}}));

    const std::string h1 = pipeline_hash(r1.pipeline, cat, &r1.secret_paths);
    EXPECT_EQ(h1, pipeline_hash(r2.pipeline, cat, &r2.secret_paths));
    EXPECT_EQ(h1, pipeline_hash(literal, cat));
    // A secret at a key not marked secret (cfg.key) still stays out of the hash...
    EXPECT_NE(pipeline_hash(r1.pipeline, cat), h1);
    // ...and counts towards the fingerprint.
    EXPECT_NE(secrets_fingerprint(r1.pipeline, cat, "salt", &r1.secret_paths),
              secrets_fingerprint(r1.pipeline, cat, "salt"));
    EXPECT_NE(secrets_fingerprint(r1.pipeline, cat, "salt", &r1.secret_paths),
              secrets_fingerprint(r2.pipeline, cat, "salt", &r2.secret_paths));
}

TEST(Redactor, ReplacesSecretFormsAndUrlUserinfo) {
    auto& r = zm::Redactor::instance();
    r.setSecrets({"p@ss:w/d", "quo\"te\\x", "abc", ""});
    // p@ss:w/d: raw + percent-encoded (its JSON form is the raw one);
    // quo"te\x: raw + percent-encoded + JSON-escaped; "abc" is too short
    EXPECT_EQ(r.patternCount(), 5u);
    EXPECT_EQ(r.apply("login with p@ss:w/d failed"), "login with *** failed");
    EXPECT_EQ(r.apply("GET rtsp://admin:p%40ss%3Aw%2Fd@10.0.0.5/102"), "GET rtsp://***@10.0.0.5/102");
    EXPECT_EQ(r.apply(R"({"error":"bad key quo\"te\\x"})"), R"({"error":"bad key ***"})");
    EXPECT_EQ(r.apply("abc rtsp://u:other@h/"), "abc rtsp://***@h/");
    EXPECT_EQ(r.apply("p@ss:w/dp@ss:w/d"), "******");

    r.setSecrets({});
    EXPECT_EQ(r.patternCount(), 0u);
    EXPECT_EQ(r.apply("p@ss:w/d"), "p@ss:w/d");
}

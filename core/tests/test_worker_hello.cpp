#include "zm/WorkerHello.hpp"
#include "zm/sha256.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <unistd.h>

using json = nlohmann::json;
namespace fs = std::filesystem;
using namespace zm::worker;

// ---------------------------------------------------------------------------
// SHA-256, FIPS 180-4 examples
// ---------------------------------------------------------------------------
TEST(Sha256, StandardVectors) {
    EXPECT_EQ(zm::Sha256::hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(zm::Sha256::hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(zm::Sha256::hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    EXPECT_EQ(zm::Sha256::hex(std::string(1000000, 'a')),
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Sha256, IncrementalMatchesOneShotAcrossBlockBoundaries) {
    const std::string msg(200, 'x');
    for (size_t split : {1u, 55u, 56u, 63u, 64u, 65u, 119u}) {
        zm::Sha256 h;
        h.update(msg.substr(0, split));
        h.update(msg.substr(split));
        const auto d = h.digest();
        std::string hex;
        char buf[3];
        for (auto b : d) { std::snprintf(buf, sizeof(buf), "%02x", b); hex += buf; }
        EXPECT_EQ(hex, zm::Sha256::hex(msg)) << "split " << split;
    }
}

// ---------------------------------------------------------------------------
// Catalog from a temporary plugins dir
// ---------------------------------------------------------------------------
class WorkerHelloTest : public ::testing::Test {
protected:
    fs::path dir;
    void SetUp() override {
        dir = fs::temp_directory_path() / ("zm_hello_" + std::to_string(::getpid()));
        fs::create_directories(dir / "capture_rtsp_multi");
        fs::create_directories(dir / "output_mqtt");
        fs::create_directories(dir / "tracker");
        write(dir / "manifest.json",
              R"({"plugins":[{"kind":"capture_rtsp_multi"},{"kind":"output_mqtt"},{"kind":"tracker"}]})");
        write(dir / "capture_rtsp_multi" / "capture_rtsp_multi.schema.json", R"({
            "x-plugin-kind":"capture_rtsp_multi","x-plugin-version":"1.2.0","type":"object",
            "properties":{"streams":{"type":"array","items":{"type":"object","properties":{
                "url":{"type":"string"},
                "login":{"type":"string","x-secret":true},
                "password":{"type":"string","x-secret":true}}}}}})");
        write(dir / "output_mqtt" / "output_mqtt.schema.json", R"({
            "x-plugin-kind":"output_mqtt","x-plugin-version":"1.0.0","type":"object",
            "properties":{"host":{"type":"string"},"password":{"type":"string","x-secret":true}}})");
        // tracker has no schema
    }
    void TearDown() override { fs::remove_all(dir); }
    static void write(const fs::path& p, const std::string& s) { std::ofstream(p) << s; }

    static json pipeline(const std::string& pass, const std::string& mqttPass = "m1") {
        return json::parse(R"({"plugins":[{"kind":"capture_rtsp_multi","cfg":{"streams":[
            {"url":"rtsp://10.0.0.5/live","login":"admin","password":")" + pass + R"("}]},
            "children":[{"kind":"tracker","cfg":{"iou_threshold":0.3}},
                        {"kind":"output_mqtt","cfg":{"host":"broker","password":")" + mqttPass + R"("}}]}]})");
    }
};

TEST_F(WorkerHelloTest, CatalogReadsManifestAndSchemas) {
    const Catalog cat = load_catalog(dir.string());
    ASSERT_TRUE(cat.error.empty()) << cat.error;
    ASSERT_EQ(cat.plugins.size(), 3u);
    const auto* cap = cat.find("capture_rtsp_multi");
    ASSERT_NE(cap, nullptr);
    EXPECT_EQ(cap->version, "1.2.0");
    EXPECT_EQ(cap->schema_sha256.rfind("sha256:", 0), 0u);
    EXPECT_EQ(cap->secret_keys, (std::set<std::string>{"login", "password"}));
    const auto* trk = cat.find("tracker");
    ASSERT_NE(trk, nullptr);
    EXPECT_TRUE(trk->schema.is_null());
    EXPECT_TRUE(trk->version.empty());
}

TEST_F(WorkerHelloTest, MissingManifestIsReportedNotThrown) {
    const Catalog cat = load_catalog((dir / "nope").string());
    EXPECT_FALSE(cat.error.empty());
    EXPECT_TRUE(cat.plugins.empty());
}

TEST_F(WorkerHelloTest, RedactionRemovesSchemaAndDefaultSecretsAtAnyDepth) {
    const Catalog cat = load_catalog(dir.string());
    std::vector<std::pair<std::string, std::string>> secrets;
    const json red = redact_pipeline(pipeline("hunter2"), cat, &secrets);
    const std::string text = red.dump();
    EXPECT_EQ(text.find("hunter2"), std::string::npos);
    EXPECT_EQ(text.find("admin"), std::string::npos) << "schema x-secret 'login'";
    EXPECT_EQ(text.find("m1"), std::string::npos);
    EXPECT_NE(text.find("rtsp://10.0.0.5/live"), std::string::npos) << "non-secret kept";
    EXPECT_NE(text.find("0.3"), std::string::npos);
    EXPECT_EQ(secrets.size(), 3u);
}

TEST_F(WorkerHelloTest, PipelineHashIgnoresSecretsButSeesEverythingElse) {
    const Catalog cat = load_catalog(dir.string());
    EXPECT_EQ(pipeline_hash(pipeline("a"), cat), pipeline_hash(pipeline("b"), cat));
    json changed = pipeline("a");
    changed["plugins"][0]["children"][0]["cfg"]["iou_threshold"] = 0.4;
    EXPECT_NE(pipeline_hash(pipeline("a"), cat), pipeline_hash(changed, cat));
}

TEST_F(WorkerHelloTest, SecretsFingerprintChangesWithSecretsAndSalt) {
    const Catalog cat = load_catalog(dir.string());
    const std::string a = secrets_fingerprint(pipeline("a"), cat, "salt1");
    EXPECT_NE(a, secrets_fingerprint(pipeline("b"), cat, "salt1"));
    EXPECT_NE(a, secrets_fingerprint(pipeline("a", "m2"), cat, "salt1"));
    EXPECT_NE(a, secrets_fingerprint(pipeline("a"), cat, "salt2"));
    EXPECT_EQ(a, secrets_fingerprint(pipeline("a"), cat, "salt1"));
    EXPECT_EQ(secrets_fingerprint(json::parse(R"({"plugins":[{"kind":"tracker","cfg":{}}]})"), cat, "s"), "");
}

TEST_F(WorkerHelloTest, HelloHasNoSecretsAndControlExtraIsSeparate) {
    const Catalog cat = load_catalog(dir.string());
    HelloFacts facts{"0.1.0", "abc1234", 1, 3, "running", {"metal"}};
    const json p = pipeline("hunter2");
    const json pub = hello_public(facts, cat, &p);
    const std::string text = pub.dump();
    EXPECT_EQ(text.find("hunter2"), std::string::npos);
    EXPECT_EQ(pub["state"], "running");
    EXPECT_EQ(pub["monitor_id"], 3);
    EXPECT_EQ(pub["zm_next"]["plugin_abi"], 1);
    EXPECT_EQ(pub["plugins"].size(), 3u);
    EXPECT_TRUE(pub["plugins"][2]["schema_sha256"].is_null());
    EXPECT_FALSE(pub.contains("secrets_fingerprint"));
    const json extra = hello_control_extra(cat, &p, "");
    EXPECT_EQ(extra["secrets_fingerprint"].get<std::string>().rfind("sha256:", 0), 0u);

    const json unconfigured = hello_public(facts, cat, nullptr);
    EXPECT_TRUE(unconfigured["pipeline_hash"].is_null());
}

TEST_F(WorkerHelloTest, DescribePluginsReturnsSchemas) {
    const Catalog cat = load_catalog(dir.string());
    const json all = describe_plugins(cat, {});
    EXPECT_TRUE(all.contains("capture_rtsp_multi"));
    EXPECT_TRUE(all.contains("output_mqtt"));
    EXPECT_FALSE(all.contains("tracker")) << "no schema, not described";
    const json some = describe_plugins(cat, {"output_mqtt", "tracker", "nope"});
    EXPECT_EQ(some["output_mqtt"]["version"], "1.0.0");
    EXPECT_TRUE(some["tracker"].is_null());
    EXPECT_TRUE(some["nope"].is_null());
}

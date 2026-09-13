#include "on_demand.hpp"

#include <gtest/gtest.h>

using nlohmann::json;
using namespace zm::ondemand;

TEST(OnDemand, ParsesMatchingCommand) {
    Request r;
    ASSERT_TRUE(parse(json::parse(R"({"cmd":"snapshot_now","request_id":42,"stream_id":1,"inline":true})"),
                      "snapshot_now", r));
    EXPECT_EQ(r.cmd, "snapshot_now");
    EXPECT_EQ(r.request_id, 42u);
    EXPECT_TRUE(r.has_stream);
    EXPECT_EQ(r.stream_id, 1u);
    EXPECT_TRUE(r.args.value("inline", false));
}

TEST(OnDemand, IgnoresOtherCommandsAndEvents) {
    Request r;
    EXPECT_FALSE(parse(json::parse(R"({"cmd":"describe_now","request_id":1})"), "snapshot_now", r));
    EXPECT_FALSE(parse(json::parse(R"({"type":"detection","stream_id":0})"), "snapshot_now", r));
    EXPECT_FALSE(parse(json::parse(R"({"cmd":7})"), "snapshot_now", r));
    EXPECT_FALSE(parse(json::parse(R"([1,2])"), "snapshot_now", r));
}

TEST(OnDemand, MissingOrBadFieldsDefault) {
    Request r;
    ASSERT_TRUE(parse(json::parse(R"({"cmd":"describe_now"})"), "describe_now", r));
    EXPECT_EQ(r.request_id, 0u);
    EXPECT_FALSE(r.has_stream);

    ASSERT_TRUE(parse(json::parse(R"({"cmd":"describe_now","request_id":-3,"stream_id":-1})"), "describe_now", r));
    EXPECT_EQ(r.request_id, 0u) << "negative request_id is not a valid id";
    EXPECT_FALSE(r.has_stream) << "negative stream_id is not a stream";

    ASSERT_TRUE(parse(json::parse(R"({"cmd":"describe_now","request_id":"9","stream_id":"0"})"), "describe_now", r));
    EXPECT_EQ(r.request_id, 0u) << "strings are not coerced";
    EXPECT_FALSE(r.has_stream);
}

TEST(OnDemand, ParseResetsAPreviouslyFilledRequest) {
    Request r;
    ASSERT_TRUE(parse(json::parse(R"({"cmd":"snapshot_now","request_id":5,"stream_id":2})"), "snapshot_now", r));
    ASSERT_TRUE(parse(json::parse(R"({"cmd":"snapshot_now"})"), "snapshot_now", r));
    EXPECT_EQ(r.request_id, 0u);
    EXPECT_FALSE(r.has_stream);
}

TEST(OnDemand, InstanceSelectionByStream) {
    Request any, s1;
    parse(json::parse(R"({"cmd":"snapshot_now"})"), "snapshot_now", any);
    parse(json::parse(R"({"cmd":"snapshot_now","stream_id":1})"), "snapshot_now", s1);
    EXPECT_TRUE(for_this_instance(any, {}));
    EXPECT_TRUE(for_this_instance(any, {0}));
    EXPECT_TRUE(for_this_instance(s1, {}));
    EXPECT_TRUE(for_this_instance(s1, {0, 1}));
    EXPECT_FALSE(for_this_instance(s1, {0}));
}

TEST(OnDemand, TagMarksResult) {
    Request r;
    parse(json::parse(R"({"cmd":"snapshot_now","request_id":7})"), "snapshot_now", r);
    json ok = {{"event", "EventSnapshot"}};
    tag(ok, r, true);
    EXPECT_EQ(ok["request_id"], 7);
    EXPECT_EQ(ok["on_demand"], true);
    EXPECT_EQ(ok["ok"], true);
    EXPECT_FALSE(ok.contains("error"));

    json bad = {{"event", "EventSnapshot"}};
    tag(bad, r, false, "no frame");
    EXPECT_EQ(bad["ok"], false);
    EXPECT_EQ(bad["error"], "no frame");
}

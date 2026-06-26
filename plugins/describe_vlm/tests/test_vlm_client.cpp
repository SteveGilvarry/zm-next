#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <cstdint>

#include "vlm_client.hpp"

TEST(VlmClient, Base64EncodeKnownBytes) {
    // "Man" -> "TWFu"
    const std::string in = "Man";
    EXPECT_EQ(vlm::base64_encode(reinterpret_cast<const uint8_t*>(in.data()),
                                 in.size()),
              "TWFu");

    // "M" -> "TQ==" (one byte, two padding chars)
    EXPECT_EQ(vlm::base64_encode(reinterpret_cast<const uint8_t*>("M"), 1),
              "TQ==");

    // "Ma" -> "TWE=" (two bytes, one padding char)
    EXPECT_EQ(vlm::base64_encode(reinterpret_cast<const uint8_t*>("Ma"), 2),
              "TWE=");

    // Empty input -> empty string
    EXPECT_EQ(vlm::base64_encode(nullptr, 0), "");
}

TEST(VlmClient, BuildChatRequestJson) {
    const std::string model = "moondream";
    const std::string prompt = "Describe the scene.";
    const std::string b64 = "QUJDRA==";  // arbitrary

    std::string body = vlm::build_chat_request_json(model, prompt, b64);

    // Must be valid JSON.
    auto j = nlohmann::json::parse(body);

    EXPECT_EQ(j["model"], model);
    ASSERT_TRUE(j["messages"].is_array());
    ASSERT_FALSE(j["messages"].empty());

    const auto& content = j["messages"][0]["content"];
    ASSERT_TRUE(content.is_array());
    ASSERT_EQ(content.size(), 2u);

    EXPECT_EQ(content[0]["type"], "text");
    EXPECT_EQ(content[0]["text"], prompt);

    EXPECT_EQ(content[1]["type"], "image_url");
    std::string url = content[1]["image_url"]["url"].get<std::string>();
    EXPECT_EQ(url, "data:image/jpeg;base64," + b64);
    EXPECT_NE(url.find("data:image/jpeg;base64,"), std::string::npos);

    EXPECT_EQ(j["max_tokens"], 128);
}

TEST(VlmClient, ParseChatResponseText) {
    const std::string resp = R"({
        "id": "chatcmpl-1",
        "choices": [
            {
                "index": 0,
                "message": {
                    "role": "assistant",
                    "content": "A person is walking past a parked car."
                },
                "finish_reason": "stop"
            }
        ]
    })";

    EXPECT_EQ(vlm::parse_chat_response_text(resp),
              "A person is walking past a parked car.");
}

TEST(VlmClient, BuildMultiFrameRequestInterleavesCaptionsAndImages) {
    std::vector<vlm::ReqFrame> frames = {{"AAAA", "Frame at t=-1.0s:"},
                                         {"BBBB", "Frame at t=+0.0s:"}};
    auto body = nlohmann::json::parse(
        vlm::build_chat_request_json_multi("qwen", "Describe the sequence.", frames, 64));
    const auto& content = body["messages"][0]["content"];
    // prompt + (caption,image) x2 = 5 parts.
    ASSERT_EQ(content.size(), 5u);
    EXPECT_EQ(content[0]["type"], "text");
    EXPECT_EQ(content[1]["text"], "Frame at t=-1.0s:");
    EXPECT_EQ(content[2]["type"], "image_url");
    EXPECT_NE(content[2]["image_url"]["url"].get<std::string>().find("AAAA"),
              std::string::npos);
    EXPECT_EQ(content[4]["image_url"]["url"], "data:image/jpeg;base64,BBBB");
    EXPECT_EQ(body["max_tokens"], 64);
    EXPECT_FALSE(body.contains("response_format"));
}

TEST(VlmClient, BuildRequestWithJsonSchema) {
    const char* schema = R"({"type":"object","properties":{"description":{"type":"string"}}})";
    auto body = nlohmann::json::parse(
        vlm::build_chat_request_json_multi("qwen", "p", {{"AAAA", ""}}, 128, schema));
    ASSERT_TRUE(body.contains("response_format"));
    EXPECT_EQ(body["response_format"]["type"], "json_schema");
    EXPECT_EQ(body["response_format"]["json_schema"]["strict"], true);
    EXPECT_TRUE(body["response_format"]["json_schema"]["schema"].contains("properties"));
}

TEST(VlmClient, BuildRequestBadSchemaFallsBackToJsonObject) {
    auto body = nlohmann::json::parse(
        vlm::build_chat_request_json_multi("qwen", "p", {{"AAAA", ""}}, 128, "not a schema"));
    EXPECT_EQ(body["response_format"]["type"], "json_object");
}

TEST(VlmClient, SingleImageBuilderStillWorks) {
    auto body = nlohmann::json::parse(vlm::build_chat_request_json("m", "p", "ZZZZ"));
    const auto& content = body["messages"][0]["content"];
    ASSERT_EQ(content.size(), 2u);   // prompt + 1 image, no caption
    EXPECT_EQ(content[1]["image_url"]["url"], "data:image/jpeg;base64,ZZZZ");
}

TEST(VlmClient, ParseStructuredResponseFromFencedJson) {
    // Model wraps JSON in a ```json fence inside the chat content.
    const std::string resp = R"({"choices":[{"message":{"content":
        "```json\n{\"description\":\"a person at the door\",\"threat_level\":\"LOW\"}\n```"}}]})";
    auto j = vlm::parse_structured_response(resp);
    ASSERT_TRUE(j.is_object());
    EXPECT_EQ(j["description"], "a person at the door");
    EXPECT_EQ(j["threat_level"], "LOW");
}

TEST(VlmClient, ParseStructuredResponsePlainObject) {
    const std::string resp =
        R"({"choices":[{"message":{"content":"{\"description\":\"car\",\"threat_level\":\"MEDIUM\"}"}}]})";
    auto j = vlm::parse_structured_response(resp);
    ASSERT_TRUE(j.is_object());
    EXPECT_EQ(j["threat_level"], "MEDIUM");
}

TEST(VlmClient, ParseChatResponseMalformed) {
    // Not JSON at all.
    EXPECT_EQ(vlm::parse_chat_response_text("not json {{{"), "");

    // Valid JSON but no choices.
    EXPECT_EQ(vlm::parse_chat_response_text(R"({"foo":"bar"})"), "");

    // Empty choices array.
    EXPECT_EQ(vlm::parse_chat_response_text(R"({"choices":[]})"), "");

    // Choice without message.content.
    EXPECT_EQ(vlm::parse_chat_response_text(R"({"choices":[{"index":0}]})"), "");

    // content not a string.
    EXPECT_EQ(vlm::parse_chat_response_text(
                  R"({"choices":[{"message":{"content":123}}]})"),
              "");
}

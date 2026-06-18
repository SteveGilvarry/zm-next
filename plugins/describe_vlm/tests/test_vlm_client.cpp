#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <string>
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

#pragma once
//
// Pure (dependency-free) helpers for the describe_vlm plugin.
//
// These functions have no dependency on libcurl, FFmpeg, or the zm plugin ABI
// so that they can be unit-tested in isolation. The only dependency is the
// header-only nlohmann::json library.
//
#include <cstdint>
#include <cstddef>
#include <string>
#include <nlohmann/json.hpp>

namespace vlm {

// Standard base64 alphabet.
inline std::string base64_encode(const uint8_t* data, size_t len) {
    static const char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    if (!data || len == 0) return out;
    out.reserve(((len + 2) / 3) * 4);

    size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        uint32_t n = (uint32_t(data[i]) << 16) |
                     (uint32_t(data[i + 1]) << 8) |
                     (uint32_t(data[i + 2]));
        out.push_back(kTable[(n >> 18) & 0x3F]);
        out.push_back(kTable[(n >> 12) & 0x3F]);
        out.push_back(kTable[(n >> 6) & 0x3F]);
        out.push_back(kTable[n & 0x3F]);
    }

    size_t rem = len - i;
    if (rem == 1) {
        uint32_t n = uint32_t(data[i]) << 16;
        out.push_back(kTable[(n >> 18) & 0x3F]);
        out.push_back(kTable[(n >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (rem == 2) {
        uint32_t n = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
        out.push_back(kTable[(n >> 18) & 0x3F]);
        out.push_back(kTable[(n >> 12) & 0x3F]);
        out.push_back(kTable[(n >> 6) & 0x3F]);
        out.push_back('=');
    }

    return out;
}

// Build an OpenAI-compatible chat completions request body containing a text
// prompt plus an inline base64 JPEG image. Returns the serialized JSON string.
inline std::string build_chat_request_json(const std::string& model,
                                           const std::string& prompt,
                                           const std::string& jpeg_base64) {
    nlohmann::json body;
    body["model"] = model;

    nlohmann::json text_part;
    text_part["type"] = "text";
    text_part["text"] = prompt;

    nlohmann::json image_part;
    image_part["type"] = "image_url";
    image_part["image_url"]["url"] =
        "data:image/jpeg;base64," + jpeg_base64;

    nlohmann::json msg;
    msg["role"] = "user";
    msg["content"] = nlohmann::json::array({text_part, image_part});

    body["messages"] = nlohmann::json::array({msg});
    body["max_tokens"] = 128;

    return body.dump();
}

// Extract choices[0].message.content from an OpenAI-style chat completion
// response. Returns "" on any parse error or missing field.
inline std::string parse_chat_response_text(const std::string& response_json) {
    try {
        auto j = nlohmann::json::parse(response_json);
        if (!j.contains("choices") || !j["choices"].is_array() ||
            j["choices"].empty()) {
            return "";
        }
        const auto& choice = j["choices"][0];
        if (!choice.contains("message")) return "";
        const auto& message = choice["message"];
        if (!message.contains("content") || !message["content"].is_string()) {
            return "";
        }
        return message["content"].get<std::string>();
    } catch (const std::exception&) {
        return "";
    }
}

}  // namespace vlm

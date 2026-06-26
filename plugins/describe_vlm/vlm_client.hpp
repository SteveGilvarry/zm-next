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
#include <vector>
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
// One frame for a multi-image request: the base64 JPEG plus an optional caption
// (e.g. a relative timestamp "t=+1.2s") interleaved before the image so a
// temporally-aware VLM (Qwen3-VL) can reason about the sequence.
struct ReqFrame {
    std::string jpeg_base64;
    std::string caption;  // optional per-frame text; empty = no caption part
};

// Build an OpenAI-compatible chat request with a text prompt followed by N images
// (each optionally preceded by its caption). When `response_schema` is a non-empty
// JSON Schema string it is sent as response_format=json_schema (strict); if it
// won't parse, falls back to response_format=json_object. max_tokens caps output.
inline std::string build_chat_request_json_multi(
        const std::string& model, const std::string& prompt,
        const std::vector<ReqFrame>& frames, int max_tokens = 128,
        const std::string& response_schema = "") {
    nlohmann::json content = nlohmann::json::array();
    nlohmann::json text_part;
    text_part["type"] = "text";
    text_part["text"] = prompt;
    content.push_back(text_part);
    for (const auto& f : frames) {
        if (!f.caption.empty()) {
            nlohmann::json cap;
            cap["type"] = "text";
            cap["text"] = f.caption;
            content.push_back(cap);
        }
        nlohmann::json image_part;
        image_part["type"] = "image_url";
        image_part["image_url"]["url"] = "data:image/jpeg;base64," + f.jpeg_base64;
        content.push_back(image_part);
    }

    nlohmann::json msg;
    msg["role"] = "user";
    msg["content"] = std::move(content);

    nlohmann::json body;
    body["model"] = model;
    body["messages"] = nlohmann::json::array({msg});
    body["max_tokens"] = max_tokens;

    if (!response_schema.empty()) {
        auto schema = nlohmann::json::parse(response_schema, nullptr, /*allow_exc=*/false);
        if (schema.is_object()) {
            body["response_format"] = {
                {"type", "json_schema"},
                {"json_schema", {{"name", "scene"}, {"schema", schema}, {"strict", true}}}};
        } else {
            body["response_format"] = {{"type", "json_object"}};
        }
    }
    return body.dump();
}

// Build an OpenAI-compatible chat completions request body containing a text
// prompt plus a single inline base64 JPEG image. (Thin wrapper over the
// multi-image builder; kept for the single-frame default path.)
inline std::string build_chat_request_json(const std::string& model,
                                           const std::string& prompt,
                                           const std::string& jpeg_base64) {
    return build_chat_request_json_multi(model, prompt, {{jpeg_base64, ""}});
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

// Parse a structured (JSON) VLM answer out of the response content. Tolerant of
// ```json … ``` code fences and surrounding prose: returns the first JSON object
// found, or a null json on failure. Used when a json schema was requested.
inline nlohmann::json parse_structured_response(const std::string& response_json) {
    std::string content = parse_chat_response_text(response_json);
    if (content.empty()) return nlohmann::json();
    // Strip a leading ```json / ``` fence if present.
    const std::size_t fence = content.find("```");
    if (fence != std::string::npos) {
        std::size_t start = content.find('\n', fence);
        std::size_t end = content.rfind("```");
        if (start != std::string::npos && end != std::string::npos && end > start)
            content = content.substr(start + 1, end - start - 1);
    }
    // Fall back to the substring between the first '{' and last '}'.
    auto j = nlohmann::json::parse(content, nullptr, /*allow_exc=*/false);
    if (!j.is_object()) {
        const std::size_t b = content.find('{'), e = content.rfind('}');
        if (b != std::string::npos && e != std::string::npos && e > b)
            j = nlohmann::json::parse(content.substr(b, e - b + 1), nullptr, false);
    }
    return j.is_object() ? j : nlohmann::json();
}

}  // namespace vlm

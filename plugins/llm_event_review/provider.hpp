#pragma once
//
// IVisionProvider — pluggable vision-language provider interface for the
// llm_event_review plugin.
//
// Phase 1 ships ONE implementation, LocalOpenAICompat, which talks to a local
// OpenAI-compatible /v1/chat/completions endpoint (llama-server / vLLM / Ollama
// / LM Studio) using the exact HTTP + JSON mechanism already proven in the
// describe_vlm plugin (libcurl + nlohmann::json + an inline base64 image_url).
//
// The interface is intentionally minimal for Phase 1: a single describe() call
// that takes an already-JPEG-encoded image plus a text prompt and returns the
// model's natural-language text (empty string on failure). Phases 2+ will widen
// this to a richer DescribeRequest/DescribeResponse and add cloud adapters; the
// seams for that are marked below.
//
// The HTTP transport here is a private copy of describe_vlm's libcurl helper so
// this header stays self-contained (one translation unit, no extra link deps
// beyond CURL + nlohmann_json, which the plugin already links). The pure JSON /
// base64 helpers are reused verbatim from describe_vlm's vlm_client.hpp.
//
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include "vlm_client.hpp"  // vlm::base64_encode / build_chat_request_json / parse_chat_response_text
#include "zm_plugin.h"     // ZM_LOG_*

namespace llmrev {

// ---------------------------------------------------------------------------
// IVisionProvider — the abstraction every provider implements.
// ---------------------------------------------------------------------------
//
// describe() takes a JPEG-encoded image and a prompt and returns the model's
// text response. Returns "" on any error (the implementation logs the cause).
//
class IVisionProvider {
public:
    virtual ~IVisionProvider() = default;

    // Run a single-image describe call. `jpeg` is a complete JPEG byte stream.
    virtual std::string describe(const std::vector<uint8_t>& jpeg,
                                 const std::string& prompt) = 0;

    // Identifier surfaced in the published "reasoning" event ("local", and in
    // Phase 2 "anthropic" | "openai" | "gemini").
    virtual std::string provider_id() const = 0;

    // The model name surfaced in the published "reasoning" event.
    virtual std::string model() const = 0;
};

// ---------------------------------------------------------------------------
// LocalOpenAICompat — Phase 1 provider.
// ---------------------------------------------------------------------------
//
// POSTs an OpenAI-compatible /v1/chat/completions request carrying the prompt
// and an inline base64 JPEG image_url, then extracts choices[0].message.content.
// This is the portable wire shape spoken by llama-server, vLLM, Ollama and
// LM Studio, so the only thing that changes between local backends is base_url
// and model.
//
class LocalOpenAICompat : public IVisionProvider {
public:
    LocalOpenAICompat(std::string server_url, std::string model,
                      std::string api_key, long timeout_sec)
        : serverUrl_(std::move(server_url)),
          model_(std::move(model)),
          apiKey_(std::move(api_key)),
          timeoutSec_(timeout_sec > 0 ? timeout_sec : 60L) {}

    std::string provider_id() const override { return "local"; }
    std::string model() const override { return model_; }

    std::string describe(const std::vector<uint8_t>& jpeg,
                         const std::string& prompt) override {
        if (jpeg.empty()) return "";

        const std::string b64 = vlm::base64_encode(jpeg.data(), jpeg.size());
        const std::string body =
            vlm::build_chat_request_json(model_, prompt, b64);

        std::string response;
        if (!httpPostJson(serverUrl_, body, apiKey_, timeoutSec_, response)) {
            // httpPostJson already logged the cause.
            return "";
        }

        return vlm::parse_chat_response_text(response);
    }

private:
    // --- libcurl transport (copied from describe_vlm::http_post_json) ---------
    static size_t writeCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
        auto* buf = static_cast<std::string*>(userdata);
        buf->append(ptr, size * nmemb);
        return size * nmemb;
    }

    // POST `body` to `url`. Returns true and fills `response` on HTTP 2xx.
    static bool httpPostJson(const std::string& url, const std::string& body,
                             const std::string& apiKey, long timeoutSec,
                             std::string& response) {
        CURL* curl = curl_easy_init();
        if (!curl) return false;

        response.clear();
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        // Optional bearer auth (local servers usually ignore it; harmless empty).
        std::string authHeader;
        if (!apiKey.empty()) {
            authHeader = "Authorization: Bearer " + apiKey;
            headers = curl_slist_append(headers, authHeader.c_str());
        }

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSec);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

        CURLcode rc = curl_easy_perform(curl);
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (rc != CURLE_OK) {
            ZM_LOG_ERROR("llm_event_review: curl error: %s",
                         curl_easy_strerror(rc));
            return false;
        }
        if (httpCode < 200 || httpCode >= 300) {
            ZM_LOG_ERROR("llm_event_review: HTTP %ld from VLM server", httpCode);
            return false;
        }
        return true;
    }

    std::string serverUrl_;
    std::string model_;
    std::string apiKey_;
    long timeoutSec_;
};

// ---------------------------------------------------------------------------
// Factory — by provider name.
// ---------------------------------------------------------------------------
//
// Phase 1: only "local" is wired. Phase 2 seams below add cloud adapters that
// translate the canonical request to each vendor's native wire shape:
//
//   // Phase 2: AnthropicProvider  — provider == "anthropic"
//   //   POST https://api.anthropic.com/v1/messages
//   //   image block: {"type":"image","source":{"type":"base64",
//   //                 "media_type":"image/jpeg","data":<b64>}}
//   //   headers: x-api-key + anthropic-version
//   //
//   // Phase 2: OpenAIProvider     — provider == "openai"
//   //   identical /v1/chat/completions shape as local; just base_url +
//   //   "Authorization: Bearer <key>" and detail:"low".
//   //
//   // Phase 2: GeminiProvider     — provider == "gemini"
//   //   POST .../v1beta/models/<model>:generateContent
//   //   image block: {"inline_data":{"mime_type":"image/jpeg","data":<b64>}}
//   //   header: x-goog-api-key
//   //
//   // Phase 2: also wrap the chosen provider in Retry / RateLimit / Fallback
//   //          decorators (cloud-first, local-last) per the design doc.
//
inline std::unique_ptr<IVisionProvider> make_provider(
    const std::string& provider, const std::string& server_url,
    const std::string& model, const std::string& api_key, long timeout_sec) {
    // Phase 1: every provider name falls back to the local OpenAI-compatible
    // path. Cloud names are accepted but routed local until Phase 2 lands.
    (void)provider;
    return std::make_unique<LocalOpenAICompat>(server_url, model, api_key,
                                               timeout_sec);
}

}  // namespace llmrev

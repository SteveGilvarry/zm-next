#include "zm/Redactor.hpp"
#include "zm/url_credentials.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace zm {

Redactor& Redactor::instance() {
    static Redactor r;
    return r;
}

void Redactor::setSecrets(const std::vector<std::string>& values) {
    std::vector<std::string> patterns;
    auto add = [&](const std::string& p) {
        if (p.size() >= kMinSecretLength && std::find(patterns.begin(), patterns.end(), p) == patterns.end())
            patterns.push_back(p);
    };
    for (const auto& v : values) {
        if (v.size() < kMinSecretLength) continue;
        add(v);
        add(capture::encode_userinfo(v));
        const std::string quoted = nlohmann::json(v).dump();  // "..." with JSON escapes
        add(quoted.substr(1, quoted.size() - 2));
    }
    // Longest first, so a secret containing another is replaced whole.
    std::sort(patterns.begin(), patterns.end(),
              [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
    auto next = std::make_shared<const std::vector<std::string>>(std::move(patterns));
    std::lock_guard<std::mutex> lock(mutex_);
    patterns_ = std::move(next);
}

size_t Redactor::patternCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return patterns_->size();
}

std::string Redactor::apply(const std::string& text) const {
    std::shared_ptr<const std::vector<std::string>> patterns;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        patterns = patterns_;
    }
    std::string out = text;
    for (const auto& p : *patterns) {
        for (size_t pos = out.find(p); pos != std::string::npos; pos = out.find(p, pos + 3))
            out.replace(pos, p.size(), "***");
    }
    return out.find("://") == std::string::npos ? out : capture::redact_text(out);
}

}  // namespace zm

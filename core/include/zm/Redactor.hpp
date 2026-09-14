#pragma once
// Redactor — one filter on every path a plugin's text leaves the worker by: the
// host log function, publish_evt and WorkerLink::publishEventJson
// (docs/Worker_Control_Protocol.md "Secret redaction").
//
// It replaces
//   - each registered secret value, in its raw, percent-encoded and JSON-escaped
//     forms, with "***", and
//   - any scheme://user:pass@ userinfo (zm::capture::redact_text).
//
// Secrets shorter than kMinSecretLength are not matched by value: replacing
// every "1" or "ab" would corrupt events and logs while hiding little. They are
// still removed from URLs and never appear in the hello.
//
// Plugins writing to stdout/stderr directly bypass this; plugins should log
// through the host.

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace zm {

class Redactor {
public:
    static constexpr size_t kMinSecretLength = 4;

    static Redactor& instance();

    // Replace the registered set (e.g. after configure). Empty values are ignored.
    void setSecrets(const std::vector<std::string>& values);

    // `text` with secrets and URL userinfo replaced.
    std::string apply(const std::string& text) const;

    // Number of match patterns currently registered (for tests).
    size_t patternCount() const;

private:
    Redactor() = default;
    mutable std::mutex mutex_;
    std::shared_ptr<const std::vector<std::string>> patterns_ =
        std::make_shared<const std::vector<std::string>>();
};

}  // namespace zm

// Pure helper for filtering which event types get POSTed to the webhook.
// Header-only so it can be unit-tested without linking libcurl.
#pragma once

#include <string>
#include <vector>

namespace zm::outputwebhook {

// Returns true if `type` should be POSTed given the allow-list `allow`.
// An empty allow-list means "allow all types". Matching is case-sensitive.
inline bool type_allowed(const std::vector<std::string>& allow,
                         const std::string& type) {
    if (allow.empty())
        return true;
    for (const auto& t : allow) {
        if (t == type)
            return true;
    }
    return false;
}

}  // namespace zm::outputwebhook

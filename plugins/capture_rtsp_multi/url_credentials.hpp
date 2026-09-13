#pragma once
// url_credentials.hpp — keep camera credentials out of logs and events.
//
// zm-api passes a camera's URL and its username/password as separate fields
// (credentials are never persisted in the pipeline). The capture plugin joins
// them only at avformat_open_input, and every URL it logs or publishes goes
// through redact(), including URLs that arrived with credentials embedded.
//
// Pure header, no FFmpeg; tests/test_url_credentials.cpp covers it.

#include <cctype>
#include <cstddef>
#include <string>

namespace zm::capture {

// Percent-encode for the userinfo part of a URL (RFC 3986): keep unreserved
// characters, encode everything else, so ':' '@' '/' in a password can't
// change how the URL parses.
inline std::string encode_userinfo(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

// Offsets of the authority section: [begin, end) after "scheme://" up to the
// first '/', '?' or '#'. Returns false for strings without "://".
inline bool authority_span(const std::string& url, size_t& begin, size_t& end) {
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) return false;
    begin = scheme + 3;
    end = url.find_first_of("/?#", begin);
    if (end == std::string::npos) end = url.size();
    return true;
}

// Position of the '@' ending the userinfo, or npos when there is none. The last
// '@' in the authority wins, so an unencoded '@' in a password still redacts.
inline size_t userinfo_end(const std::string& url) {
    size_t b = 0, e = 0;
    if (!authority_span(url, b, e)) return std::string::npos;
    const size_t at = url.rfind('@', e == 0 ? 0 : e - 1);
    return (at != std::string::npos && at >= b) ? at : std::string::npos;
}

// The URL with any userinfo replaced by "***", for logs and events.
inline std::string redact(const std::string& url) {
    const size_t at = userinfo_end(url);
    if (at == std::string::npos) return url;
    size_t b = 0, e = 0;
    authority_span(url, b, e);
    return url.substr(0, b) + "***" + url.substr(at);
}

// Redact every URL userinfo inside free text, e.g. an FFmpeg log line that
// quotes the URL it failed to open. A URL ends at whitespace or a quote.
inline std::string redact_text(const std::string& text) {
    std::string out;
    size_t pos = 0;
    while (true) {
        const size_t scheme = text.find("://", pos);
        if (scheme == std::string::npos) { out.append(text, pos, std::string::npos); break; }
        size_t start = scheme;
        while (start > pos && (std::isalnum(static_cast<unsigned char>(text[start - 1])) ||
                               text[start - 1] == '+' || text[start - 1] == '-' || text[start - 1] == '.'))
            --start;
        size_t end = text.find_first_of(" \t\r\n\"'<>", scheme + 3);
        if (end == std::string::npos) end = text.size();
        out.append(text, pos, start - pos);
        out += redact(text.substr(start, end - start));
        pos = end;
    }
    return out;
}

// The URL to open: explicit username/password win over any embedded userinfo.
// With no username the URL is returned unchanged.
inline std::string with_credentials(const std::string& url, const std::string& username,
                                    const std::string& password) {
    if (username.empty()) return url;
    size_t b = 0, e = 0;
    if (!authority_span(url, b, e)) return url;
    const size_t at = userinfo_end(url);
    const size_t host = (at == std::string::npos) ? b : at + 1;
    std::string userinfo = encode_userinfo(username);
    if (!password.empty()) userinfo += ":" + encode_userinfo(password);
    return url.substr(0, b) + userinfo + "@" + url.substr(host);
}

}  // namespace zm::capture

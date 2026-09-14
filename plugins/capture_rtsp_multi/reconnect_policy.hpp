#pragma once
// reconnect_policy.hpp — when a camera stream retries, and what health it reports.
//
// Two backoff tracks:
//   - network failures (refused, timeout, stream dropped): 1 s doubling to 30 s,
//     as before;
//   - authentication failures (the camera answered 401/403): 60 s doubling to
//     15 min. Retrying a wrong password every few seconds locks many camera
//     accounts, so a rejected login slows right down until it succeeds.
// max_retry_attempts (> 0) still gives up after that many consecutive failures of
// either kind; -1 retries forever.
//
// Health events follow the canonical stream-socket lifecycle codes, published on
// transitions rather than per attempt so a dead camera doesn't flood the socket:
//   connection_failed    (0x0101) never connected, or reconnecting after a failed
//                                 reconnect; repeated at most once a minute while
//                                 it keeps failing
//   connection_restored  (0x0102) connected after connection_failed / auth failure
//   capture_failed       (0x0105) a streaming connection dropped
//   capture_resumed      (0x0106) reconnected after capture_failed
//   stream_auth_failed   (0x0402) every rejected login (they are >= 60 s apart)
// See docs/Worker_Control_Protocol.md, "Status".
//
// Pure header: no FFmpeg, no clock. The caller passes whether the error was an
// auth rejection and the current time in ms; tests/test_reconnect_policy.cpp
// drives it.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace zm::capture {

struct HealthEvent {
    std::string type;       // connection_failed | connection_restored | capture_failed |
                            // capture_resumed | stream_auth_failed
    int attempt = 0;        // consecutive failures so far (0 for recoveries)
    int64_t retry_in_ms = 0;
};

class ReconnectPolicy {
public:
    static constexpr int64_t kNetMinMs = 1000;
    static constexpr int64_t kNetMaxMs = 30000;
    static constexpr int64_t kAuthMinMs = 60000;
    static constexpr int64_t kAuthMaxMs = 15 * 60 * 1000;
    static constexpr int64_t kRepeatFailedEveryMs = 60000;

    explicit ReconnectPolicy(int max_retry_attempts = -1) : max_retries_(max_retry_attempts) {}

    struct Decision {
        bool give_up = false;
        int64_t delay_ms = 0;               // before the next attempt (caller adds jitter)
        std::vector<HealthEvent> events;
    };

    // A connection attempt failed. `auth` = the camera rejected the credentials.
    Decision on_connect_failed(bool auth, int64_t now_ms) {
        Decision d;
        ++attempts_;
        if (auth) {
            d.delay_ms = auth_delay_;
            auth_delay_ = std::min(auth_delay_ * 2, kAuthMaxMs);
            net_delay_ = kNetMinMs;
            d.events.push_back({"stream_auth_failed", attempts_, d.delay_ms});
            fault_ = Fault::Auth;
        } else {
            d.delay_ms = net_delay_;
            net_delay_ = std::min(net_delay_ * 2, kNetMaxMs);
            auth_delay_ = kAuthMinMs;
            if (fault_ == Fault::None || fault_ == Fault::Auth) {
                // A new network outage (or the password now works but the network doesn't).
                d.events.push_back({"connection_failed", attempts_, d.delay_ms});
                last_failed_emit_ms_ = now_ms;
                fault_ = Fault::Connect;
            } else if (now_ms - last_failed_emit_ms_ >= kRepeatFailedEveryMs) {
                // Same outage, still failing: a reminder at most once a minute. The
                // outage keeps its kind, so a drop still recovers as capture_resumed.
                d.events.push_back({"connection_failed", attempts_, d.delay_ms});
                last_failed_emit_ms_ = now_ms;
            }
        }
        if (max_retries_ > 0 && attempts_ >= max_retries_) d.give_up = true;
        return d;
    }

    // A connection attempt succeeded.
    std::vector<HealthEvent> on_connected() {
        std::vector<HealthEvent> out;
        if (fault_ == Fault::Capture) out.push_back({"capture_resumed", 0, 0});
        else if (fault_ == Fault::Connect || fault_ == Fault::Auth) out.push_back({"connection_restored", 0, 0});
        fault_ = Fault::None;
        attempts_ = 0;
        net_delay_ = kNetMinMs;
        auth_delay_ = kAuthMinMs;
        return out;
    }

    // A streaming connection dropped (read error or EOF).
    std::vector<HealthEvent> on_stream_dropped(int64_t now_ms) {
        fault_ = Fault::Capture;
        last_failed_emit_ms_ = now_ms;
        return {{"capture_failed", 0, 0}};
    }

    int attempts() const { return attempts_; }

private:
    enum class Fault { None, Connect, Capture, Auth };
    int max_retries_;
    int attempts_ = 0;
    int64_t net_delay_ = kNetMinMs;
    int64_t auth_delay_ = kAuthMinMs;
    int64_t last_failed_emit_ms_ = 0;
    Fault fault_ = Fault::None;
};

}  // namespace zm::capture

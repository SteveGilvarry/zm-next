#include "reconnect_policy.hpp"

#include <gtest/gtest.h>

using namespace zm::capture;

namespace {
std::vector<std::string> types(const std::vector<HealthEvent>& ev) {
    std::vector<std::string> t;
    for (const auto& e : ev) t.push_back(e.type);
    return t;
}
using V = std::vector<std::string>;
}  // namespace

TEST(ReconnectPolicy, NetworkBackoffDoublesToThirtySeconds) {
    ReconnectPolicy p;
    const int64_t expected[] = {1000, 2000, 4000, 8000, 16000, 30000, 30000};
    int64_t t = 0;
    for (int64_t e : expected) {
        auto d = p.on_connect_failed(false, t);
        EXPECT_EQ(d.delay_ms, e);
        EXPECT_FALSE(d.give_up);
        t += d.delay_ms;
    }
}

TEST(ReconnectPolicy, AuthBackoffStartsAtAMinuteAndCapsAtFifteen) {
    ReconnectPolicy p;
    const int64_t expected[] = {60000, 120000, 240000, 480000, 900000, 900000};
    int64_t t = 0;
    for (int64_t e : expected) {
        auto d = p.on_connect_failed(true, t);
        EXPECT_EQ(d.delay_ms, e);
        ASSERT_EQ(types(d.events), V{"stream_auth_failed"});
        EXPECT_EQ(d.events[0].retry_in_ms, e);
        t += d.delay_ms;
    }
}

TEST(ReconnectPolicy, WrongPasswordMakesFewAttemptsInAnHour) {
    // The point of the auth track: a bad password must not hammer the camera.
    ReconnectPolicy p;
    int attempts = 0;
    for (int64_t t = 0; t < 3600 * 1000;) {
        t += p.on_connect_failed(true, t).delay_ms;
        ++attempts;
    }
    EXPECT_LE(attempts, 8);
}

TEST(ReconnectPolicy, ConnectionFailedOnceThenAtMostEveryMinute) {
    ReconnectPolicy p;
    auto d = p.on_connect_failed(false, 0);
    EXPECT_EQ(types(d.events), V{"connection_failed"});
    EXPECT_TRUE(p.on_connect_failed(false, 1000).events.empty());
    EXPECT_TRUE(p.on_connect_failed(false, 30000).events.empty());
    EXPECT_EQ(types(p.on_connect_failed(false, 60000).events), V{"connection_failed"});
    EXPECT_TRUE(p.on_connect_failed(false, 90000).events.empty());
}

TEST(ReconnectPolicy, RecoveryAfterConnectFailureIsConnectionRestored) {
    ReconnectPolicy p;
    p.on_connect_failed(false, 0);
    p.on_connect_failed(false, 1000);
    EXPECT_EQ(types(p.on_connected()), V{"connection_restored"});
    EXPECT_EQ(p.attempts(), 0);
    // Backoff resets.
    EXPECT_EQ(p.on_connect_failed(false, 5000).delay_ms, 1000);
}

TEST(ReconnectPolicy, DropThenReconnectIsCaptureFailedThenResumed) {
    ReconnectPolicy p;
    EXPECT_TRUE(p.on_connected().empty()) << "first connect reports nothing";
    EXPECT_EQ(types(p.on_stream_dropped(10000)), V{"capture_failed"});
    // Reconnect attempts right after the drop don't add connection_failed...
    EXPECT_TRUE(p.on_connect_failed(false, 11000).events.empty());
    EXPECT_TRUE(p.on_connect_failed(false, 13000).events.empty());
    // ...until the outage passes a minute.
    EXPECT_EQ(types(p.on_connect_failed(false, 70000).events), V{"connection_failed"});
    EXPECT_EQ(types(p.on_connected()), V{"capture_resumed"});
}

TEST(ReconnectPolicy, AuthRecoveryIsConnectionRestored) {
    ReconnectPolicy p;
    p.on_connect_failed(true, 0);
    EXPECT_EQ(types(p.on_connected()), V{"connection_restored"});
}

TEST(ReconnectPolicy, SwitchingBetweenAuthAndNetworkResetsTheOtherTrack) {
    ReconnectPolicy p;
    p.on_connect_failed(true, 0);                       // auth 60 s, next would be 120 s
    auto net = p.on_connect_failed(false, 60000);       // camera now unreachable
    EXPECT_EQ(net.delay_ms, 1000);
    EXPECT_EQ(types(net.events), V{"connection_failed"});
    auto auth = p.on_connect_failed(true, 61000);       // reachable again, still wrong password
    EXPECT_EQ(auth.delay_ms, 60000) << "auth track restarts after a network failure";
}

TEST(ReconnectPolicy, MaxRetryAttemptsGivesUpOnEitherKind) {
    ReconnectPolicy p(3);
    EXPECT_FALSE(p.on_connect_failed(false, 0).give_up);
    EXPECT_FALSE(p.on_connect_failed(true, 1000).give_up);
    EXPECT_TRUE(p.on_connect_failed(false, 61000).give_up);

    ReconnectPolicy forever(-1);
    for (int i = 0; i < 1000; ++i) ASSERT_FALSE(forever.on_connect_failed(i % 2, i * 1000).give_up);
}

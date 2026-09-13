#include "url_credentials.hpp"

#include <gtest/gtest.h>

using namespace zm::capture;

TEST(UrlCredentials, RedactsEmbeddedUserinfo) {
    EXPECT_EQ(redact("rtsp://admin:hunter2@10.0.0.5:554/Streaming/Channels/101"),
              "rtsp://***@10.0.0.5:554/Streaming/Channels/101");
    EXPECT_EQ(redact("rtsp://admin@cam.local/live"), "rtsp://***@cam.local/live");
}

TEST(UrlCredentials, RedactLeavesPlainUrlsAlone) {
    EXPECT_EQ(redact("rtsp://10.0.0.5:554/live"), "rtsp://10.0.0.5:554/live");
    EXPECT_EQ(redact("/var/clips/a.mp4"), "/var/clips/a.mp4");
    EXPECT_EQ(redact(""), "");
}

TEST(UrlCredentials, RedactHandlesAtSignsInPasswordAndPath) {
    // Unencoded '@' in the password: the last '@' before the path ends userinfo.
    EXPECT_EQ(redact("rtsp://admin:p@ss@10.0.0.5/live"), "rtsp://***@10.0.0.5/live");
    // '@' only in the path or query is not userinfo.
    EXPECT_EQ(redact("rtsp://10.0.0.5/live?user=a@b"), "rtsp://10.0.0.5/live?user=a@b");
    EXPECT_EQ(redact("rtsp://10.0.0.5/a@b/live"), "rtsp://10.0.0.5/a@b/live");
}

TEST(UrlCredentials, WithCredentialsInsertsEncodedUserinfo) {
    EXPECT_EQ(with_credentials("rtsp://10.0.0.5:554/live", "admin", "hunter2"),
              "rtsp://admin:hunter2@10.0.0.5:554/live");
    EXPECT_EQ(with_credentials("rtsp://10.0.0.5/live", "admin", "p@ss:w/rd#1"),
              "rtsp://admin:p%40ss%3Aw%2Frd%231@10.0.0.5/live");
    EXPECT_EQ(with_credentials("rtsp://10.0.0.5/live", "viewer", ""), "rtsp://viewer@10.0.0.5/live");
}

TEST(UrlCredentials, ExplicitCredentialsReplaceEmbeddedOnes) {
    EXPECT_EQ(with_credentials("rtsp://old:old@10.0.0.5/live", "new", "pw"), "rtsp://new:pw@10.0.0.5/live");
}

TEST(UrlCredentials, NoUsernameReturnsUrlUnchanged) {
    EXPECT_EQ(with_credentials("rtsp://a:b@10.0.0.5/live", "", "ignored"), "rtsp://a:b@10.0.0.5/live");
    EXPECT_EQ(with_credentials("/var/clips/a.mp4", "admin", "x"), "/var/clips/a.mp4");
}

TEST(UrlCredentials, RedactTextScrubsUrlsInsideLogLines) {
    EXPECT_EQ(redact_text("[rtsp @ 0x1] method DESCRIBE failed for rtsp://admin:pw@10.0.0.5/live: 401"),
              "[rtsp @ 0x1] method DESCRIBE failed for rtsp://***@10.0.0.5/live: 401");
    EXPECT_EQ(redact_text("a rtsp://u:p@h/x and http://v:q@k/y end"),
              "a rtsp://***@h/x and http://***@k/y end");
    EXPECT_EQ(redact_text("\"rtsp://u:p@h/x\""), "\"rtsp://***@h/x\"");
    EXPECT_EQ(redact_text("no urls here @ all"), "no urls here @ all");
    EXPECT_EQ(redact_text("tcp://10.0.0.5:554 refused"), "tcp://10.0.0.5:554 refused");
}

TEST(UrlCredentials, RedactOfTheOpenedUrlShowsNoSecret) {
    const std::string opened = with_credentials("rtsp://10.0.0.5/live", "admin", "s3cr3t@!");
    const std::string shown = redact(opened);
    EXPECT_EQ(shown.find("admin"), std::string::npos);
    EXPECT_EQ(shown.find("s3cr3t"), std::string::npos);
    EXPECT_EQ(shown, "rtsp://***@10.0.0.5/live");
}

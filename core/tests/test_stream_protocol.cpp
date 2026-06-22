// Unit tests for the canonical per-monitor stream socket wire protocol. Ported
// from ZoneMinder's tests/zm_stream_socket_protocol.cpp and adapted to zm-next's
// FFmpeg-free BuildHello plus the analysis/AI EVENT extensions.

#include "zm/stream_socket_protocol.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace ss = zm::stream_socket;

namespace {

ss::Header make_header(ss::MessageType type, ss::StreamId stream, uint8_t flags,
                       uint32_t sequence, uint32_t generation, uint64_t pts_us,
                       uint32_t payload_size) {
    ss::Header h{};
    h.length = ss::kHeaderLengthBytes + payload_size;
    h.version = ss::kProtocolVersion;
    h.type = static_cast<uint8_t>(type);
    h.stream = static_cast<uint8_t>(stream);
    h.flags = flags;
    h.sequence = sequence;
    h.generation = generation;
    h.pts_us = pts_us;
    return h;
}

} // namespace

TEST(StreamProtocol, HeaderRoundTrip) {
    ss::Header in = make_header(ss::MessageType::Media, ss::StreamId::Video,
                                ss::kFlagKeyframe, 7, 2, 1234567, 42);
    uint8_t buf[ss::kHeaderSize];
    ss::SerializeHeader(in, buf);

    // Little-endian on the wire.
    EXPECT_EQ(buf[4], ss::kProtocolVersion);
    EXPECT_EQ(buf[5], static_cast<uint8_t>(ss::MessageType::Media));
    EXPECT_EQ(buf[6], static_cast<uint8_t>(ss::StreamId::Video));
    EXPECT_EQ(buf[7], ss::kFlagKeyframe);

    ss::Header out{};
    ASSERT_TRUE(ss::ParseHeader(buf, out));
    EXPECT_EQ(out.length, in.length);
    EXPECT_EQ(out.type, in.type);
    EXPECT_EQ(out.stream, in.stream);
    EXPECT_EQ(out.flags, in.flags);
    EXPECT_EQ(out.sequence, 7u);
    EXPECT_EQ(out.generation, 2u);
    EXPECT_EQ(out.pts_us, 1234567u);
    EXPECT_EQ(out.payload_size(), 42u);
}

TEST(StreamProtocol, HeaderRejectsBadVersionAndLength) {
    ss::Header h = make_header(ss::MessageType::Hello, ss::StreamId::Video, 0, 0, 0, 0, 0);
    uint8_t buf[ss::kHeaderSize];
    ss::SerializeHeader(h, buf);

    uint8_t bad_version[ss::kHeaderSize];
    std::memcpy(bad_version, buf, ss::kHeaderSize);
    bad_version[4] = 9;
    ss::Header out{};
    EXPECT_FALSE(ss::ParseHeader(bad_version, out));

    uint8_t bad_len[ss::kHeaderSize];
    std::memcpy(bad_len, buf, ss::kHeaderSize);
    bad_len[0] = 5; bad_len[1] = bad_len[2] = bad_len[3] = 0;  // below header remainder
    EXPECT_FALSE(ss::ParseHeader(bad_len, out));
}

TEST(StreamProtocol, HelloVideoRoundTrip) {
    const uint8_t extradata[] = {0x00, 0x00, 0x00, 0x01, 0x67};
    std::vector<uint8_t> body = ss::BuildHello(/*codec_id=*/27, extradata, sizeof(extradata),
                                               /*w=*/1920, /*h=*/1080, /*fps_num=*/30, /*fps_den=*/1,
                                               /*sample_rate=*/0, /*channels=*/0);
    ss::HelloInfo info;
    ASSERT_TRUE(ss::ParseHello(body.data(), body.size(), info));
    EXPECT_EQ(info.codec_id, 27u);
    EXPECT_EQ(info.width, 1920u);
    EXPECT_EQ(info.height, 1080u);
    EXPECT_EQ(info.fps_num, 30u);
    EXPECT_EQ(info.fps_den, 1u);
    EXPECT_EQ(info.sample_rate, 0u);
    ASSERT_EQ(info.extradata.size(), sizeof(extradata));
    EXPECT_EQ(info.extradata[4], 0x67);
}

TEST(StreamProtocol, HelloAudioRoundTrip) {
    std::vector<uint8_t> body = ss::BuildHello(/*codec_id=*/86018, nullptr, 0,
                                               /*w=*/0, /*h=*/0, /*fps_num=*/0, /*fps_den=*/0,
                                               /*sample_rate=*/16000, /*channels=*/1);
    ss::HelloInfo info;
    ASSERT_TRUE(ss::ParseHello(body.data(), body.size(), info));
    EXPECT_EQ(info.codec_id, 86018u);
    EXPECT_EQ(info.sample_rate, 16000u);
    EXPECT_EQ(info.channels, 1u);
    EXPECT_EQ(info.width, 0u);
}

TEST(StreamProtocol, HelloSkipsUnknownTagAndRequiresCodecId) {
    // Build a valid hello, then append an unknown TLV tag — must be skipped.
    std::vector<uint8_t> body = ss::BuildHello(27, nullptr, 0, 640, 480, 0, 0, 0, 0);
    body.push_back(0x7F);             // unknown tag
    body.push_back(0x02); body.push_back(0x00);  // len = 2
    body.push_back(0xAA); body.push_back(0xBB);
    ss::HelloInfo info;
    ASSERT_TRUE(ss::ParseHello(body.data(), body.size(), info));
    EXPECT_EQ(info.codec_id, 27u);
    EXPECT_EQ(info.width, 640u);

    // No codec id at all → rejected.
    std::vector<uint8_t> no_codec;  // empty payload
    EXPECT_FALSE(ss::ParseHello(no_codec.data(), no_codec.size(), info));
}

TEST(StreamProtocol, HelloRejectsTruncatedTlv) {
    std::vector<uint8_t> body = {0x01, 0xFF, 0x00, 0xAA};  // tag 1 claims 255 bytes, has 1
    ss::HelloInfo info;
    EXPECT_FALSE(ss::ParseHello(body.data(), body.size(), info));
}

TEST(StreamProtocol, StatsRoundTrip) {
    std::vector<uint8_t> body = ss::BuildStats(1000, 42);
    uint64_t sent = 0, dropped = 0;
    ASSERT_TRUE(ss::ParseStats(body.data(), body.size(), sent, dropped));
    EXPECT_EQ(sent, 1000u);
    EXPECT_EQ(dropped, 42u);
    EXPECT_FALSE(ss::ParseStats(body.data(), 8, sent, dropped));  // too short
}

TEST(StreamProtocol, EventLifecycleRoundTrip) {
    ss::MonitorEvent in;
    in.code = ss::kEventStateChanged;
    in.wall_clock_us = 1700000000000000ull; in.has_wall_clock = true;
    in.message = "alarm";
    in.state_id = 3; in.has_state_id = true;
    in.prev_state_id = 1; in.has_prev_state_id = true;
    in.state_name = "ALARM";
    in.health_code = 0; in.has_health_code = true;

    std::vector<uint8_t> body = ss::BuildEvent(in);
    ss::MonitorEvent out;
    ASSERT_TRUE(ss::ParseEvent(body.data(), body.size(), out));
    EXPECT_EQ(out.code, ss::kEventStateChanged);
    EXPECT_TRUE(out.has_wall_clock);
    EXPECT_EQ(out.wall_clock_us, in.wall_clock_us);
    EXPECT_EQ(out.message, "alarm");
    EXPECT_TRUE(out.has_state_id);
    EXPECT_EQ(out.state_id, 3u);
    EXPECT_EQ(out.prev_state_id, 1u);
    EXPECT_EQ(out.state_name, "ALARM");
    EXPECT_TRUE(out.has_health_code);
}

TEST(StreamProtocol, EventJsonDetailExtensionRoundTrip) {
    ss::MonitorEvent in;
    in.code = ss::kEventDetection;
    in.wall_clock_us = 123; in.has_wall_clock = true;
    in.json_detail = R"({"objects":[{"label":"person","confidence":0.9}]})";

    std::vector<uint8_t> body = ss::BuildEvent(in);
    ss::MonitorEvent out;
    ASSERT_TRUE(ss::ParseEvent(body.data(), body.size(), out));
    EXPECT_EQ(out.code, ss::kEventDetection);
    EXPECT_EQ(out.json_detail, in.json_detail);
    EXPECT_TRUE(out.message.empty());
}

TEST(StreamProtocol, EventSkipsUnknownTag) {
    std::vector<uint8_t> body;
    body.push_back(0x01); body.push_back(0x03);  // code 0x0301 (detection)
    body.push_back(0x7E);                          // unknown tag
    body.push_back(0x01); body.push_back(0x00);    // len 1
    body.push_back(0xCC);
    ss::MonitorEvent out;
    ASSERT_TRUE(ss::ParseEvent(body.data(), body.size(), out));
    EXPECT_EQ(out.code, ss::kEventDetection);
}

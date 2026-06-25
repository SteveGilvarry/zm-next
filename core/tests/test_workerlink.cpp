#include "zm/WorkerLink.hpp"
#include "zm/stream_socket_protocol.hpp"
#include "zm/EventBus.hpp"

#include <nlohmann/json.hpp>
#include <gtest/gtest.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <cstdint>

namespace ss = zm::stream_socket;
using json = nlohmann::json;

namespace {

int connect_client(const std::string& path) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    EXPECT_GE(fd, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    for (int i = 0; i < 100; ++i) {
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0)
            return fd;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ADD_FAILURE() << "could not connect to " << path;
    ::close(fd);
    return -1;
}

bool wait_readable(int fd, int timeout_ms) {
    pollfd p{fd, POLLIN, 0};
    return ::poll(&p, 1, timeout_ms) > 0 && (p.revents & POLLIN);
}

bool read_n(int fd, void* buf, size_t n) {
    size_t off = 0;
    auto* p = static_cast<uint8_t*>(buf);
    while (off < n) {
        ssize_t r = ::read(fd, p + off, n - off);
        if (r <= 0) return false;
        off += static_cast<size_t>(r);
    }
    return true;
}

// Read one canonical wire unit: a 24-byte header followed by payload_size bytes.
bool read_msg(int fd, ss::Header& h, std::vector<uint8_t>* payload = nullptr) {
    uint8_t hdr[ss::kHeaderSize];
    if (!read_n(fd, hdr, ss::kHeaderSize)) return false;
    if (!ss::ParseHeader(hdr, h)) return false;
    const size_t plen = h.payload_size();
    std::vector<uint8_t> body(plen);
    if (plen && !read_n(fd, body.data(), plen)) return false;
    if (payload) *payload = std::move(body);
    return true;
}

void write_msg(int fd, ss::MessageType type, ss::StreamId stream, int64_t pts,
               const std::string& body) {
    ss::Header h{};
    h.length = ss::kHeaderLengthBytes + static_cast<uint32_t>(body.size());
    h.version = ss::kProtocolVersion;
    h.type = static_cast<uint8_t>(type);
    h.stream = static_cast<uint8_t>(stream);
    h.flags = 0;
    h.sequence = 0;
    h.generation = 0;
    h.pts_us = static_cast<uint64_t>(pts);
    uint8_t hb[ss::kHeaderSize];
    ss::SerializeHeader(h, hb);
    ASSERT_EQ(::write(fd, hb, ss::kHeaderSize), static_cast<ssize_t>(ss::kHeaderSize));
    if (!body.empty())
        ASSERT_EQ(::write(fd, body.data(), body.size()), static_cast<ssize_t>(body.size()));
}

void send_subscribe(int fd, bool video, bool audio = false, bool events = true) {
    json s = {{"video", video}, {"audio", audio}, {"events", events}};
    write_msg(fd, ss::MessageType::Subscribe, ss::StreamId::Monitor, 0, s.dump());
}

std::string temp_socket_path(int n = -1) {
    std::string p = std::string("/tmp/zm_wl_test_") + std::to_string(::getpid());
    if (n >= 0) p += "_" + std::to_string(n);
    return p + ".sock";
}

} // namespace

TEST(WorkerLinkTest, SnapshotOnConnectThenEvent) {
    const std::string path = temp_socket_path();
    zm::WorkerLink link(/*monitor_id=*/7, path);
    link.setSnapshotJson(R"({"type":"state_changed","state":"IDLE"})");
    ASSERT_TRUE(link.start());

    int client = connect_client(path);
    ASSERT_GE(client, 0);

    // The snapshot is replayed to every consumer on connect.
    ss::Header h;
    std::vector<uint8_t> body;
    ASSERT_TRUE(read_msg(client, h, &body));
    EXPECT_EQ(h.type, static_cast<uint8_t>(ss::MessageType::Event));
    ss::MonitorEvent ev;
    ASSERT_TRUE(ss::ParseEvent(body.data(), body.size(), ev));
    EXPECT_EQ(ev.code, ss::kEventSnapshot);

    // A subsequently published motion event maps to a DETECTION event frame.
    link.publishEventJson(R"({"type":"motion","pixels":1234})");
    ASSERT_TRUE(read_msg(client, h, &body));
    EXPECT_EQ(h.type, static_cast<uint8_t>(ss::MessageType::Event));
    ASSERT_TRUE(ss::ParseEvent(body.data(), body.size(), ev));
    EXPECT_EQ(ev.code, ss::kEventDetection);
    EXPECT_NE(ev.json_detail.find("1234"), std::string::npos);

    ::close(client);
    link.stop();
}

TEST(WorkerLinkTest, CommandRequestResponse) {
    const std::string path = temp_socket_path();
    zm::WorkerLink link(/*monitor_id=*/1, path);
    link.setCommandHandler([](const std::string& name, const std::string& args) {
        zm::WorkerLink::CommandResult r;
        if (name == "status") { r.ok = true; r.message = "ok"; r.data_json = R"({"plugins":3})"; }
        else { r.ok = false; r.message = "unknown_command"; }
        (void)args;
        return r;
    });
    ASSERT_TRUE(link.start());

    int client = connect_client(path);
    ASSERT_GE(client, 0);

    json cmd = {{"request_id", 42}, {"name", "status"}};
    write_msg(client, ss::MessageType::Command, ss::StreamId::Monitor, 0, cmd.dump());

    ss::Header h;
    std::vector<uint8_t> body;
    ASSERT_TRUE(read_msg(client, h, &body));
    EXPECT_EQ(h.type, static_cast<uint8_t>(ss::MessageType::Response));
    json resp = json::parse(body.begin(), body.end());
    EXPECT_EQ(resp["request_id"], 42u);
    EXPECT_TRUE(resp["ok"]);
    EXPECT_NE(resp["data"].get<std::string>().find("3"), std::string::npos);

    ::close(client);
    link.stop();
}

TEST(WorkerLinkTest, EventsOnlyConsumerGetsNoMedia) {
    const std::string path = temp_socket_path();
    zm::WorkerLink link(/*monitor_id=*/2, path);
    ASSERT_TRUE(link.start());

    int client = connect_client(path);
    ASSERT_GE(client, 0);

    // Subscribe to events only (record-only / no-view consumer).
    send_subscribe(client, /*video=*/false, /*audio=*/false, /*events=*/true);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Media must NOT reach an events-only consumer...
    auto payload = std::make_shared<std::vector<uint8_t>>(std::vector<uint8_t>{1, 2, 3, 4});
    link.sendMedia(/*stream=*/1, /*keyframe=*/true, /*pts=*/100, payload);
    // ...but an event must.
    link.publishEventJson(R"({"type":"capture_failed"})");

    ss::Header h;
    std::vector<uint8_t> body;
    ASSERT_TRUE(read_msg(client, h, &body));
    EXPECT_EQ(h.type, static_cast<uint8_t>(ss::MessageType::Event));
    ss::MonitorEvent ev;
    ASSERT_TRUE(ss::ParseEvent(body.data(), body.size(), ev));
    EXPECT_EQ(ev.code, ss::kEventCaptureFailed);

    ::close(client);
    link.stop();
}

TEST(WorkerLinkTest, VideoSubscriberReceivesMediaPayload) {
    const std::string path = temp_socket_path();
    zm::WorkerLink link(/*monitor_id=*/3, path);
    ASSERT_TRUE(link.start());

    int client = connect_client(path);
    ASSERT_GE(client, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::vector<uint8_t> au{0, 0, 0, 1, 9, 8, 7, 6, 5};  // pretend access unit
    auto payload = std::make_shared<std::vector<uint8_t>>(au);
    link.sendMedia(/*stream=*/1, /*keyframe=*/true, /*pts=*/4242, payload);

    ss::Header h;
    std::vector<uint8_t> body;
    ASSERT_TRUE(read_msg(client, h, &body));
    EXPECT_EQ(h.type, static_cast<uint8_t>(ss::MessageType::Media));
    EXPECT_EQ(h.stream, static_cast<uint8_t>(ss::StreamId::Video));
    EXPECT_TRUE(h.flags & ss::kFlagKeyframe);
    EXPECT_EQ(h.pts_us, 4242u);
    EXPECT_EQ(body, au);  // payload arrived intact

    ::close(client);
    link.stop();
}

TEST(WorkerLinkTest, StreamMetadataBecomesHello) {
    const std::string path = temp_socket_path();
    zm::WorkerLink link(/*monitor_id=*/4, path);
    ASSERT_TRUE(link.start());

    int client = connect_client(path);
    ASSERT_GE(client, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // "AAECAw==" is base64 for bytes {0,1,2,3} (stand-in for SPS/PPS extradata).
    link.publishEventJson(
        R"({"event":"StreamMetadata","stream_id":0,"codec_id":27,"width":1920,)"
        R"("height":1080,"pix_fmt":0,"profile":100,"level":40,"extradata":"AAECAw=="})");

    ss::Header h;
    std::vector<uint8_t> body;
    ASSERT_TRUE(read_msg(client, h, &body));
    EXPECT_EQ(h.type, static_cast<uint8_t>(ss::MessageType::Hello));
    ss::HelloInfo info;
    ASSERT_TRUE(ss::ParseHello(body.data(), body.size(), info));
    EXPECT_EQ(info.codec_id, 27u);
    EXPECT_EQ(info.width, 1920u);
    EXPECT_EQ(info.height, 1080u);
    ASSERT_EQ(info.extradata.size(), 4u);
    EXPECT_EQ(info.extradata[3], 3);

    ::close(client);
    link.stop();
}

// A consumer that connects AFTER stream params were established must still get a
// Hello on connect (cached + replayed), so it can initialize its decoder without
// waiting for the next generation bump.
TEST(WorkerLinkTest, HelloReplayedToLateConsumer) {
    const std::string path = temp_socket_path(900);
    zm::WorkerLink link(/*monitor_id=*/9, path);
    ASSERT_TRUE(link.start());

    // Stream params arrive BEFORE any consumer connects.
    link.publishEventJson(
        R"({"event":"StreamMetadata","stream_id":0,"codec_id":27,"width":1280,)"
        R"("height":720,"pix_fmt":0,"profile":100,"level":40,"extradata":"AAECAw=="})");
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    int client = connect_client(path);
    ASSERT_GE(client, 0);

    // The very first frame the late consumer receives must be the replayed Hello.
    ss::Header h;
    std::vector<uint8_t> body;
    ASSERT_TRUE(read_msg(client, h, &body));
    EXPECT_EQ(h.type, static_cast<uint8_t>(ss::MessageType::Hello));
    ss::HelloInfo info;
    ASSERT_TRUE(ss::ParseHello(body.data(), body.size(), info));
    EXPECT_EQ(info.codec_id, 27u);
    EXPECT_EQ(info.width, 1280u);

    ::close(client);
    link.stop();
}

// A consumer that disconnects and reconnects must get the Hello again.
TEST(WorkerLinkTest, ReconnectReceivesHelloAgain) {
    const std::string path = temp_socket_path(960);
    zm::WorkerLink link(/*monitor_id=*/12, path);
    ASSERT_TRUE(link.start());
    link.publishEventJson(
        R"({"event":"StreamMetadata","stream_id":0,"codec_id":27,"width":800,)"
        R"("height":600,"pix_fmt":0,"profile":100,"level":40,"extradata":"AAECAw=="})");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    for (int attempt = 0; attempt < 2; ++attempt) {
        int client = connect_client(path);
        ASSERT_GE(client, 0) << "attempt " << attempt;
        ss::Header h;
        std::vector<uint8_t> body;
        ASSERT_TRUE(read_msg(client, h, &body)) << "attempt " << attempt;
        ASSERT_EQ(h.type, static_cast<uint8_t>(ss::MessageType::Hello)) << "attempt " << attempt;
        ss::HelloInfo info;
        ASSERT_TRUE(ss::ParseHello(body.data(), body.size(), info));
        EXPECT_EQ(info.width, 800u);
        ::close(client);  // disconnect; the worker must reap us and serve the next
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    link.stop();
}

// Cold-start stress: repeatedly connect + stream, asserting the consumer always
// begins receiving media within a bounded window.
TEST(WorkerLinkTest, ColdStartAlwaysReceivesMedia) {
    const int kTrials = 25;
    for (int t = 0; t < kTrials; ++t) {
        const std::string path = temp_socket_path(1000 + t);
        zm::WorkerLink link(/*monitor_id=*/10, path);
        ASSERT_TRUE(link.start()) << "trial " << t;

        int client = connect_client(path);
        ASSERT_GE(client, 0) << "trial " << t;
        send_subscribe(client, /*video=*/true, /*audio=*/false, /*events=*/false);

        bool gotMedia = false;
        for (int i = 0; i < 200 && !gotMedia; ++i) {
            auto payload = std::make_shared<std::vector<uint8_t>>(
                std::vector<uint8_t>{0, 0, 0, 1, static_cast<uint8_t>(i)});
            link.sendMedia(/*stream=*/1, /*keyframe=*/i == 0, /*pts=*/i * 1000, payload);
            while (wait_readable(client, 5)) {
                ss::Header h;
                std::vector<uint8_t> body;
                if (!read_msg(client, h, &body)) { gotMedia = false; break; }
                if (h.type == static_cast<uint8_t>(ss::MessageType::Media)) { gotMedia = true; break; }
            }
        }
        EXPECT_TRUE(gotMedia) << "trial " << t << ": consumer never received media";

        ::close(client);
        link.stop();
    }
}

// A health/lifecycle event published before a consumer connects must be replayed
// as that consumer's connect snapshot; detection events must NOT become it.
TEST(WorkerLinkTest, HealthEventRefreshesSnapshot) {
    const std::string path = temp_socket_path(950);
    zm::WorkerLink link(/*monitor_id=*/11, path);
    ASSERT_TRUE(link.start());

    // A detection arrives first (must NOT become the snapshot)...
    link.publishEventJson(R"({"type":"detection","detections":[]})");
    // ...then a health event (this IS current status).
    link.publishEventJson(R"({"type":"connection_failed","message":"dead input"})");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    int client = connect_client(path);
    ASSERT_GE(client, 0);
    ss::Header h;
    std::vector<uint8_t> body;
    ASSERT_TRUE(read_msg(client, h, &body));
    EXPECT_EQ(h.type, static_cast<uint8_t>(ss::MessageType::Event));
    ss::MonitorEvent ev;
    ASSERT_TRUE(ss::ParseEvent(body.data(), body.size(), ev));
    EXPECT_EQ(ev.code, ss::kEventConnectionFailed);
    EXPECT_NE(ev.message.find("dead input"), std::string::npos);

    ::close(client);
    link.stop();
}

// A recording-saved (EventClip) event maps to the RECORDING_SAVED extension code
// and carries its metadata in the JSON detail TLV.
TEST(WorkerLinkTest, RecordingSavedEvent) {
    const std::string path = temp_socket_path(970);
    zm::WorkerLink link(/*monitor_id=*/13, path);
    ASSERT_TRUE(link.start());

    int client = connect_client(path);
    ASSERT_GE(client, 0);

    link.publishEventJson(R"({"event":"EventClip","path":"/data/ev/1.mp4","duration":15})");

    ss::Header h;
    std::vector<uint8_t> body;
    ASSERT_TRUE(read_msg(client, h, &body));
    EXPECT_EQ(h.type, static_cast<uint8_t>(ss::MessageType::Event));
    ss::MonitorEvent ev;
    ASSERT_TRUE(ss::ParseEvent(body.data(), body.size(), ev));
    EXPECT_EQ(ev.code, ss::kEventRecordingSaved);
    EXPECT_NE(ev.json_detail.find("1.mp4"), std::string::npos);

    ::close(client);
    link.stop();
}

// store_event's segment-open request maps to the recording_opening extension
// code (0x0304) and carries clip_token in the JSON detail TLV.
TEST(WorkerLinkTest, RecordingOpeningEvent) {
    const std::string path = temp_socket_path(980);
    zm::WorkerLink link(/*monitor_id=*/14, path);
    ASSERT_TRUE(link.start());

    int client = connect_client(path);
    ASSERT_GE(client, 0);

    link.publishEventJson(
        R"({"event":"RecordingOpening","clip_token":"14-7-1","trigger":"detection"})");

    ss::Header h;
    std::vector<uint8_t> body;
    ASSERT_TRUE(read_msg(client, h, &body));
    EXPECT_EQ(h.type, static_cast<uint8_t>(ss::MessageType::Event));
    ss::MonitorEvent ev;
    ASSERT_TRUE(ss::ParseEvent(body.data(), body.size(), ev));
    EXPECT_EQ(ev.code, ss::kEventRecordingOpening);
    EXPECT_NE(ev.json_detail.find("14-7-1"), std::string::npos);
    EXPECT_NE(ev.json_detail.find("detection"), std::string::npos);

    ::close(client);
    link.stop();
}

// A client→server cmd-style Command (assign_recording) is delivered to the
// command handler with the cmd name + full payload; when the handler dispatches
// it onto the event bus (as zm-core does), a bus subscriber receives it — with
// no deadlock despite the dispatch happening under WorkerLink's lock.
TEST(WorkerLinkTest, CmdStyleCommandDispatchedToBus) {
    const std::string path = temp_socket_path(990);
    zm::WorkerLink link(/*monitor_id=*/15, path);

    std::string dispatched;
    auto sub = zm::EventBus::instance().subscribe(
        "plugin_event", [&](const std::string& m) { dispatched = m; });

    link.setCommandHandler([](const std::string& name, const std::string& args) {
        zm::WorkerLink::CommandResult r;
        if (name == "assign_recording") {
            zm::EventBus::instance().publish("plugin_event", args);  // mimic zm-core
            r.ok = true; r.message = "dispatched";
        } else {
            r.ok = false; r.message = "unknown";
        }
        return r;
    });
    ASSERT_TRUE(link.start());

    int client = connect_client(path);
    ASSERT_GE(client, 0);

    const char* cmd = R"({"cmd":"assign_recording","clip_token":"15-7-1",)"
                      R"("event_id":512,"dir":"/data/3/512","video_name":"512-video.mp4"})";
    write_msg(client, ss::MessageType::Command, ss::StreamId::Monitor, 0, cmd);

    ss::Header h;
    std::vector<uint8_t> body;
    ASSERT_TRUE(read_msg(client, h, &body));
    EXPECT_EQ(h.type, static_cast<uint8_t>(ss::MessageType::Response));
    json resp = json::parse(body.begin(), body.end());
    EXPECT_TRUE(resp["ok"]);

    // The full command JSON (with event_id + clip_token) reached the bus.
    ASSERT_FALSE(dispatched.empty());
    json d = json::parse(dispatched);
    EXPECT_EQ(d["cmd"], "assign_recording");
    EXPECT_EQ(d["event_id"], 512);
    EXPECT_EQ(d["clip_token"], "15-7-1");

    zm::EventBus::instance().unsubscribe("plugin_event", sub);
    ::close(client);
    link.stop();
}

// A review_assets event (motion synopsis tube manifest) maps to the 0x0306
// extension code and its JSON rides in the json_detail TLV (proving the
// map_event_code + is_ai_code edits, without which it would land in the message
// tag and zm-api's ingest would break).
TEST(WorkerLinkTest, ReviewAssetsEvent) {
    const std::string path = temp_socket_path(975);
    zm::WorkerLink link(/*monitor_id=*/16, path);
    ASSERT_TRUE(link.start());

    int client = connect_client(path);
    ASSERT_GE(client, 0);

    link.publishEventJson(
        R"({"type":"review_assets","event_id":512,"clip_token":"16-7-1",)"
        R"("tubes":[{"track_id":3}]})");

    ss::Header h;
    std::vector<uint8_t> body;
    ASSERT_TRUE(read_msg(client, h, &body));
    EXPECT_EQ(h.type, static_cast<uint8_t>(ss::MessageType::Event));
    ss::MonitorEvent ev;
    ASSERT_TRUE(ss::ParseEvent(body.data(), body.size(), ev));
    EXPECT_EQ(ev.code, ss::kEventReviewAssets);
    EXPECT_FALSE(ev.json_detail.empty());          // rode in TLV 0x10, not the message tag
    EXPECT_TRUE(ev.message.empty());
    EXPECT_NE(ev.json_detail.find("16-7-1"), std::string::npos);

    ::close(client);
    link.stop();
}

TEST(WorkerLinkTest, PeriodicStats) {
    const std::string path = temp_socket_path();
    zm::WorkerLink::Config cfg;
    cfg.stats_interval = std::chrono::milliseconds(100);
    zm::WorkerLink link(/*monitor_id=*/5, path, cfg);
    ASSERT_TRUE(link.start());

    int client = connect_client(path);
    ASSERT_GE(client, 0);

    // No snapshot and no events, so the first frame the consumer sees is a
    // periodic Stats frame emitted on the configured interval.
    ss::Header h;
    std::vector<uint8_t> body;
    ASSERT_TRUE(read_msg(client, h, &body));
    EXPECT_EQ(h.type, static_cast<uint8_t>(ss::MessageType::Stats));
    uint64_t sent = 0, dropped = 0;
    EXPECT_TRUE(ss::ParseStats(body.data(), body.size(), sent, dropped));

    ::close(client);
    link.stop();
}

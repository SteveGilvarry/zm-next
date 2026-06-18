#include "zm/WorkerLink.hpp"
#include "worker_link.pb.h"

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

namespace wlp = zm::worker::v1;

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

// Wait up to timeout_ms for the fd to become readable. Returns true if readable.
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

// Read one wire unit (u32 pb_len, u32 payload_len, [Frame], [payload]) and parse
// the Frame. Payload bytes (if any) are returned in `payload`.
bool read_frame(int fd, wlp::Frame& frame, std::vector<uint8_t>* payload = nullptr) {
    uint8_t hdr[8];
    if (!read_n(fd, hdr, 8)) return false;
    uint32_t pb_len = hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) | (uint32_t(hdr[3]) << 24);
    uint32_t payload_len = hdr[4] | (hdr[5] << 8) | (hdr[6] << 16) | (uint32_t(hdr[7]) << 24);
    std::vector<uint8_t> pbuf(pb_len);
    if (!read_n(fd, pbuf.data(), pb_len)) return false;
    if (!frame.ParseFromArray(pbuf.data(), static_cast<int>(pb_len))) return false;
    if (payload_len) {
        std::vector<uint8_t> body(payload_len);
        if (!read_n(fd, body.data(), payload_len)) return false;
        if (payload) *payload = std::move(body);
    }
    return true;
}

void write_frame(int fd, const wlp::Frame& frame) {
    uint32_t pb_len = static_cast<uint32_t>(frame.ByteSizeLong());
    std::vector<uint8_t> buf(8 + pb_len);
    auto put = [&](size_t at, uint32_t v) {
        buf[at] = v & 0xff; buf[at+1] = (v >> 8) & 0xff;
        buf[at+2] = (v >> 16) & 0xff; buf[at+3] = (v >> 24) & 0xff;
    };
    put(0, pb_len);
    put(4, 0);
    frame.SerializeToArray(buf.data() + 8, static_cast<int>(pb_len));
    ASSERT_EQ(::write(fd, buf.data(), buf.size()), static_cast<ssize_t>(buf.size()));
}

std::string temp_socket_path() {
    return std::string("/tmp/zm_wl_test_") + std::to_string(::getpid()) + ".sock";
}

std::string temp_socket_path(int n) {
    return std::string("/tmp/zm_wl_test_") + std::to_string(::getpid()) + "_" +
           std::to_string(n) + ".sock";
}

void pb_subscribe(int fd, bool video, bool audio = false, bool events = true) {
    wlp::Frame sub;
    auto* s = sub.mutable_subscribe();
    s->set_video(video);
    s->set_audio(audio);
    s->set_events(events);
    write_frame(fd, sub);
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
    wlp::Frame snap;
    ASSERT_TRUE(read_frame(client, snap));
    EXPECT_EQ(snap.monitor_id(), 7u);
    ASSERT_EQ(snap.payload_case(), wlp::Frame::kEvent);
    EXPECT_EQ(snap.event().code(), wlp::Event::SNAPSHOT);

    // A subsequently published motion event maps to a DETECTION event frame.
    link.publishEventJson(R"({"type":"motion","pixels":1234})");
    wlp::Frame ev;
    ASSERT_TRUE(read_frame(client, ev));
    ASSERT_EQ(ev.payload_case(), wlp::Frame::kEvent);
    EXPECT_EQ(ev.event().code(), wlp::Event::DETECTION);
    EXPECT_NE(ev.event().message().find("1234"), std::string::npos);

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

    wlp::Frame req;
    req.set_monitor_id(1);
    auto* cmd = req.mutable_command();
    cmd->set_request_id(42);
    cmd->set_name("status");
    write_frame(client, req);

    wlp::Frame resp;
    ASSERT_TRUE(read_frame(client, resp));
    ASSERT_EQ(resp.payload_case(), wlp::Frame::kResponse);
    EXPECT_EQ(resp.response().request_id(), 42u);
    EXPECT_TRUE(resp.response().ok());
    EXPECT_NE(resp.response().data_json().find("3"), std::string::npos);

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
    wlp::Frame sub;
    sub.set_monitor_id(2);
    auto* s = sub.mutable_subscribe();
    s->set_video(false);
    s->set_audio(false);
    s->set_events(true);
    write_frame(client, sub);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Media must NOT reach an events-only consumer...
    auto payload = std::make_shared<std::vector<uint8_t>>(std::vector<uint8_t>{1, 2, 3, 4});
    link.sendMedia(/*stream=*/wlp::STREAM_KIND_VIDEO, /*keyframe=*/true, /*pts=*/100, payload);
    // ...but an event must.
    link.publishEventJson(R"({"type":"capture_failed"})");

    wlp::Frame f;
    ASSERT_TRUE(read_frame(client, f));
    ASSERT_EQ(f.payload_case(), wlp::Frame::kEvent);
    EXPECT_EQ(f.event().code(), wlp::Event::CAPTURE_FAILED);

    ::close(client);
    link.stop();
}

TEST(WorkerLinkTest, VideoSubscriberReceivesMediaPayload) {
    const std::string path = temp_socket_path();
    zm::WorkerLink link(/*monitor_id=*/3, path);
    ASSERT_TRUE(link.start());

    int client = connect_client(path);
    ASSERT_GE(client, 0);

    pb_subscribe(client, /*video=*/true);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::vector<uint8_t> au{0, 0, 0, 1, 9, 8, 7, 6, 5};  // pretend access unit
    auto payload = std::make_shared<std::vector<uint8_t>>(au);
    link.sendMedia(/*stream=*/wlp::STREAM_KIND_VIDEO, /*keyframe=*/true, /*pts=*/4242, payload);

    wlp::Frame f;
    std::vector<uint8_t> body;
    ASSERT_TRUE(read_frame(client, f, &body));
    ASSERT_EQ(f.payload_case(), wlp::Frame::kMedia);
    EXPECT_EQ(f.media().stream(), wlp::STREAM_KIND_VIDEO);
    EXPECT_TRUE(f.media().keyframe());
    EXPECT_EQ(f.media().pts_us(), 4242);
    EXPECT_EQ(body, au);  // payload arrived intact, alongside the protobuf

    ::close(client);
    link.stop();
}

TEST(WorkerLinkTest, StreamMetadataBecomesHello) {
    const std::string path = temp_socket_path();
    zm::WorkerLink link(/*monitor_id=*/4, path);
    ASSERT_TRUE(link.start());

    int client = connect_client(path);
    ASSERT_GE(client, 0);
    pb_subscribe(client, /*video=*/true);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // "AAECAw==" is base64 for bytes {0,1,2,3} (stand-in for SPS/PPS extradata).
    link.publishEventJson(
        R"({"event":"StreamMetadata","stream_id":0,"codec_id":27,"width":1920,)"
        R"("height":1080,"pix_fmt":0,"profile":100,"level":40,"extradata":"AAECAw=="})");

    wlp::Frame f;
    ASSERT_TRUE(read_frame(client, f));
    ASSERT_EQ(f.payload_case(), wlp::Frame::kHello);
    EXPECT_EQ(f.hello().codec_id(), 27u);
    EXPECT_EQ(f.hello().width(), 1920u);
    EXPECT_EQ(f.hello().height(), 1080u);
    ASSERT_EQ(f.hello().extradata().size(), 4u);
    EXPECT_EQ(static_cast<uint8_t>(f.hello().extradata()[3]), 3);

    ::close(client);
    link.stop();
}

// A consumer that connects AFTER stream params were established must still get a
// Hello on subscribe (cached + replayed), so it can initialize its decoder
// without waiting for the next generation bump. Regression guard for the fix
// where Hello was sent only once at startup.
TEST(WorkerLinkTest, HelloReplayedToLateSubscriber) {
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
    pb_subscribe(client, /*video=*/true);

    // The very first frame the late subscriber receives must be the replayed Hello.
    wlp::Frame f;
    ASSERT_TRUE(read_frame(client, f));
    ASSERT_EQ(f.payload_case(), wlp::Frame::kHello);
    EXPECT_EQ(f.hello().codec_id(), 27u);
    EXPECT_EQ(f.hello().width(), 1280u);

    ::close(client);
    link.stop();
}

// Cold-start stress: repeatedly connect + subscribe + stream, asserting the
// consumer always begins receiving media within a bounded window. Targets the
// intermittent "media=0 for the whole session" race seen under load.
TEST(WorkerLinkTest, ColdStartSubscribeAlwaysReceivesMedia) {
    const int kTrials = 25;
    for (int t = 0; t < kTrials; ++t) {
        const std::string path = temp_socket_path(1000 + t);
        zm::WorkerLink link(/*monitor_id=*/10, path);
        ASSERT_TRUE(link.start()) << "trial " << t;

        int client = connect_client(path);
        ASSERT_GE(client, 0) << "trial " << t;
        pb_subscribe(client, /*video=*/true, /*audio=*/false, /*events=*/false);

        // Stream like a real camera (~continuous), no pre-sleep, while reading.
        // The consumer must receive at least one Media frame well before we give up.
        bool gotMedia = false;
        for (int i = 0; i < 200 && !gotMedia; ++i) {
            auto payload = std::make_shared<std::vector<uint8_t>>(
                std::vector<uint8_t>{0, 0, 0, 1, static_cast<uint8_t>(i)});
            link.sendMedia(wlp::STREAM_KIND_VIDEO, /*keyframe=*/i == 0, /*pts=*/i * 1000, payload);
            // Drain whatever is readable without blocking long.
            while (wait_readable(client, 5)) {
                wlp::Frame f;
                std::vector<uint8_t> body;
                if (!read_frame(client, f, &body)) { gotMedia = false; break; }
                if (f.payload_case() == wlp::Frame::kMedia) { gotMedia = true; break; }
            }
        }
        EXPECT_TRUE(gotMedia) << "trial " << t << ": consumer never received media";

        ::close(client);
        link.stop();
    }
}

// A health/lifecycle event published before a consumer connects must be replayed
// as that consumer's connect snapshot, so a late subscriber learns current status
// (e.g. a dead input) instead of seeing nothing. Detection events must NOT
// become the snapshot.
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
    wlp::Frame f;
    ASSERT_TRUE(read_frame(client, f));
    ASSERT_EQ(f.payload_case(), wlp::Frame::kEvent);
    EXPECT_EQ(f.event().code(), wlp::Event::CONNECTION_FAILED);
    EXPECT_NE(f.event().message().find("dead input"), std::string::npos);

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
    wlp::Frame f;
    ASSERT_TRUE(read_frame(client, f));
    EXPECT_EQ(f.payload_case(), wlp::Frame::kStats);

    ::close(client);
    link.stop();
}

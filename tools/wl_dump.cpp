// wl_dump: connect to a zm-core worker socket, subscribe, and print the framed
// protobuf messages (Hello / Media / Event / Stats / Bye). A live end-to-end
// probe for the per-monitor worker interface. Usage: wl_dump <socket> [seconds]

#include "worker_link.pb.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <iostream>

namespace wl = zm::worker::v1;

static void put_u32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(v & 0xff); b.push_back((v >> 8) & 0xff);
    b.push_back((v >> 16) & 0xff); b.push_back((v >> 24) & 0xff);
}
static uint32_t get_u32(const uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24);
}

int main(int argc, char** argv) {
    if (argc < 2) { std::cerr << "usage: wl_dump <socket> [seconds]\n"; return 2; }
    const std::string path = argv[1];
    const int seconds = argc > 2 ? atoi(argv[2]) : 5;

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    for (int i = 0; i < 200; ++i) {
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) break;
        struct timespec ts{0, 10'000'000}; nanosleep(&ts, nullptr);
        if (i == 199) { std::cerr << "could not connect to " << path << "\n"; return 1; }
    }
    std::cout << "connected to " << path << "\n";

    // Subscribe to everything so the worker pushes media + events to us.
    wl::Frame sub;
    auto* s = sub.mutable_subscribe();
    s->set_video(true); s->set_audio(true); s->set_events(true);
    std::string body = sub.SerializeAsString();
    std::vector<uint8_t> out;
    put_u32(out, (uint32_t)body.size()); put_u32(out, 0);
    out.insert(out.end(), body.begin(), body.end());
    (void)!::write(fd, out.data(), out.size());

    std::string buf;
    int hello = 0, media = 0, events = 0, stats = 0, other = 0;
    long mediaBytes = 0;
    time_t end = time(nullptr) + seconds;
    while (time(nullptr) < end) {
        pollfd pfd{fd, POLLIN, 0};
        if (::poll(&pfd, 1, 200) <= 0) continue;
        char tmp[65536];
        ssize_t n = ::read(fd, tmp, sizeof(tmp));
        if (n <= 0) break;
        buf.append(tmp, n);
        while (buf.size() >= 8) {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(buf.data());
            uint32_t pblen = get_u32(p), paylen = get_u32(p + 4);
            if (buf.size() < 8 + pblen + paylen) break;
            wl::Frame f;
            if (f.ParseFromArray(buf.data() + 8, pblen)) {
                switch (f.payload_case()) {
                    case wl::Frame::kHello:
                        ++hello;
                        std::cout << "HELLO stream=" << f.hello().stream()
                                  << " codec_id=" << f.hello().codec_id()
                                  << " " << f.hello().width() << "x" << f.hello().height()
                                  << " sr=" << f.hello().sample_rate() << "\n";
                        break;
                    case wl::Frame::kMedia:
                        ++media; mediaBytes += paylen;
                        if (media <= 3 || media % 25 == 0)
                            std::cout << "MEDIA #" << media << " stream=" << f.media().stream()
                                      << " key=" << f.media().keyframe()
                                      << " pts=" << f.media().pts_us()
                                      << " bytes=" << paylen << "\n";
                        break;
                    case wl::Frame::kEvent: {
                        ++events;
                        const auto& ev = f.event();
                        if (ev.code() == wl::Event::DESCRIPTION)
                            std::cout << "DESCRIPTION: " << ev.description().text() << "\n";
                        else
                            std::cout << "EVENT code=" << ev.code()
                                      << " msg=" << ev.message().substr(0, 120) << "\n";
                        break;
                    }
                    case wl::Frame::kStats:
                        ++stats;
                        std::cout << "STATS sent=" << f.stats().messages_sent()
                                  << " dropped=" << f.stats().messages_dropped() << "\n";
                        break;
                    default: ++other; break;
                }
            }
            buf.erase(0, 8 + pblen + paylen);
        }
    }
    std::cout << "--- summary: hello=" << hello << " media=" << media
              << " (" << mediaBytes << " bytes) events=" << events
              << " stats=" << stats << " other=" << other << " ---\n";
    ::close(fd);
    return 0;
}

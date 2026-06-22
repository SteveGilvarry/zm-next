// wl_dump: connect to a zm-core worker socket as a canonical consumer and print
// every framed message (Hello / Media / Keyframe / Stats / Event / Bye). A live
// end-to-end probe for the per-monitor worker interface, speaking the same wire
// protocol as the ZoneMinder zmc producer and the zm-api consumer.
// Usage: wl_dump <socket> [seconds]

#include "zm/stream_socket_protocol.hpp"

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

namespace ss = zm::stream_socket;

int main(int argc, char** argv) {
    // Line-buffer stdout so each printed frame/event is emitted immediately when
    // piped (not held in a block buffer and flushed in bursts) — keeps a live feed
    // through `wl_dump | grep ...` real-time.
    setvbuf(stdout, nullptr, _IOLBF, 0);
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
    // No Subscribe needed: a canonical consumer receives everything by default.

    std::string buf;
    int hello = 0, media = 0, keyframe = 0, events = 0, stats = 0, bye = 0, other = 0;
    long mediaBytes = 0;
    time_t end = time(nullptr) + seconds;
    while (time(nullptr) < end) {
        pollfd pfd{fd, POLLIN, 0};
        if (::poll(&pfd, 1, 200) <= 0) continue;
        char tmp[65536];
        ssize_t n = ::read(fd, tmp, sizeof(tmp));
        if (n <= 0) break;
        buf.append(tmp, n);
        while (buf.size() >= ss::kHeaderSize) {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(buf.data());
            ss::Header h{};
            if (!ss::ParseHeader(p, h)) { std::cout << "BAD HEADER, resync\n"; buf.clear(); break; }
            const size_t payload_len = h.payload_size();
            if (buf.size() < ss::kHeaderSize + payload_len) break;
            const uint8_t* body = p + ss::kHeaderSize;

            switch (static_cast<ss::MessageType>(h.type)) {
                case ss::MessageType::Hello: {
                    ++hello;
                    ss::HelloInfo info;
                    if (ss::ParseHello(body, payload_len, info))
                        std::cout << "HELLO stream=" << int(h.stream)
                                  << " codec_id=" << info.codec_id
                                  << " " << info.width << "x" << info.height
                                  << " sr=" << info.sample_rate
                                  << " extradata=" << info.extradata.size()
                                  << " gen=" << h.generation << "\n";
                    else
                        std::cout << "HELLO (unparsable, " << payload_len << " bytes)\n";
                    break;
                }
                case ss::MessageType::Media:
                    ++media; mediaBytes += payload_len;
                    if (media <= 3 || media % 25 == 0)
                        std::cout << "MEDIA #" << media << " stream=" << int(h.stream)
                                  << " key=" << ((h.flags & ss::kFlagKeyframe) ? 1 : 0)
                                  << " seq=" << h.sequence << " pts=" << h.pts_us
                                  << " bytes=" << payload_len << "\n";
                    break;
                case ss::MessageType::Keyframe:
                    ++keyframe;
                    std::cout << "KEYFRAME stream=" << int(h.stream)
                              << " pts=" << h.pts_us << " bytes=" << payload_len << "\n";
                    break;
                case ss::MessageType::Stats: {
                    ++stats;
                    uint64_t sent = 0, dropped = 0;
                    ss::ParseStats(body, payload_len, sent, dropped);
                    std::cout << "STATS sent=" << sent << " dropped=" << dropped << "\n";
                    break;
                }
                case ss::MessageType::Event: {
                    ++events;
                    ss::MonitorEvent ev;
                    if (ss::ParseEvent(body, payload_len, ev)) {
                        std::cout << "EVENT code=0x" << std::hex << ev.code << std::dec;
                        if (!ev.json_detail.empty())
                            std::cout << " detail=" << ev.json_detail.substr(0, 160);
                        else if (!ev.message.empty())
                            std::cout << " msg=" << ev.message.substr(0, 120);
                        std::cout << "\n";
                    } else {
                        std::cout << "EVENT (unparsable, " << payload_len << " bytes)\n";
                    }
                    break;
                }
                case ss::MessageType::Bye:
                    ++bye;
                    std::cout << "BYE\n";
                    break;
                default:
                    ++other;
                    break;
            }
            buf.erase(0, ss::kHeaderSize + payload_len);
        }
    }
    std::cout << "--- summary: hello=" << hello << " media=" << media
              << " (" << mediaBytes << " bytes) keyframe=" << keyframe
              << " events=" << events << " stats=" << stats
              << " bye=" << bye << " other=" << other << " ---\n";
    ::close(fd);
    return 0;
}

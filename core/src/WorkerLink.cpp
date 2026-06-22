#include "zm/WorkerLink.hpp"
#include "zm/stream_socket_protocol.hpp"

#include <nlohmann/json.hpp>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <csignal>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <chrono>
#include <iostream>

namespace zm {

namespace ss = zm::stream_socket;
using json = nlohmann::json;

namespace {

int64_t now_usec() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

uint32_t get_u32_le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// zm-next internal stream kind (1 = video, 2 = audio; matches the proto
// STREAM_KIND_* values the producers pass) → canonical wire StreamId.
uint8_t wire_stream(uint32_t internal) {
    return internal == 2u ? static_cast<uint8_t>(ss::StreamId::Audio)
                          : static_cast<uint8_t>(ss::StreamId::Video);
}

// Minimal base64 decoder: the capture plugin base64-encodes codec extradata in
// its StreamMetadata event; core is FFmpeg-free so we decode it here.
std::vector<uint8_t> base64_decode(const std::string& in) {
    static const auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<uint8_t> out;
    int buf = 0, bits = 0;
    for (char c : in) {
        if (c == '=') break;
        int v = val(c);
        if (v < 0) continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buf >> bits) & 0xff));
        }
    }
    return out;
}

// Map a plugin EventBus event onto a canonical/extension EVENT code. Lifecycle
// "type"s map to their canonical codes; motion/zone/detection → DETECTION;
// description → DESCRIPTION; the EventClip "event" → RECORDING_SAVED. Anything
// else stays 0 (unspecified) and is still forwarded with its raw JSON.
uint16_t map_event_code(const std::string& type, const std::string& event) {
    if (type == "connection_failed")      return ss::kEventConnectionFailed;
    if (type == "connection_restored")    return ss::kEventConnectionRestored;
    if (type == "prime_capture_failed")   return ss::kEventPrimeCaptureFailed;
    if (type == "prime_capture_restored") return ss::kEventPrimeCaptureRestored;
    if (type == "capture_failed")         return ss::kEventCaptureFailed;
    if (type == "capture_resumed")        return ss::kEventCaptureResumed;
    if (type == "state_changed")          return ss::kEventStateChanged;
    if (type == "motion" || type == "zone_motion" || type == "detection")
        return ss::kEventDetection;
    if (type == "description")            return ss::kEventDescription;
    if (event == "EventClip")             return ss::kEventRecordingSaved;
    if (event == "RecordingOpening")      return ss::kEventRecordingOpening;
    return 0;
}

bool is_ai_code(uint16_t code) {
    return code == ss::kEventDetection || code == ss::kEventDescription ||
           code == ss::kEventRecordingSaved || code == ss::kEventRecordingOpening;
}

} // namespace

WorkerLink::WorkerLink(uint32_t monitor_id, std::string socket_path)
    : monitor_id_(monitor_id), socket_path_(std::move(socket_path)) {}

WorkerLink::WorkerLink(uint32_t monitor_id, std::string socket_path, Config cfg)
    : monitor_id_(monitor_id), socket_path_(std::move(socket_path)), cfg_(std::move(cfg)) {}

WorkerLink::~WorkerLink() {
    stop();
}

void WorkerLink::setCommandHandler(CommandHandler handler) {
    handler_ = std::move(handler);
}

void WorkerLink::setTalkbackHandler(TalkbackHandler handler) {
    talkbackHandler_ = std::move(handler);
}

bool WorkerLink::start() {
    std::signal(SIGPIPE, SIG_IGN);

    if (socket_path_.size() >= sizeof(sockaddr_un{}.sun_path)) {
        std::cerr << "[WorkerLink] socket path too long: " << socket_path_ << std::endl;
        return false;
    }

    listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        std::cerr << "[WorkerLink] socket() failed: " << std::strerror(errno) << std::endl;
        return false;
    }
    ::unlink(socket_path_.c_str());

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "[WorkerLink] bind(" << socket_path_ << ") failed: "
                  << std::strerror(errno) << std::endl;
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    ::chmod(socket_path_.c_str(), 0660);
    if (::listen(listen_fd_, 8) < 0) {
        std::cerr << "[WorkerLink] listen() failed: " << std::strerror(errno) << std::endl;
        ::close(listen_fd_);
        listen_fd_ = -1;
        ::unlink(socket_path_.c_str());
        return false;
    }

    last_stats_ = std::chrono::steady_clock::now();
    running_.store(true);
    thread_ = std::thread(&WorkerLink::runLoop, this);
    std::cerr << "[WorkerLink] listening on " << socket_path_
              << " (monitor " << monitor_id_ << ")" << std::endl;
    return true;
}

void WorkerLink::stop() {
    if (!running_.exchange(false)) {
        if (listen_fd_ >= 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            ::unlink(socket_path_.c_str());
        }
        return;
    }

    // Best-effort BYE to every connected consumer before tearing down. We're
    // closing everything next, so write directly and ignore errors (no reaping).
    {
        MessagePtr bye = makeControl(static_cast<uint8_t>(ss::MessageType::Bye),
                                     static_cast<uint8_t>(ss::StreamId::Video),
                                     /*flags=*/0, /*sequence=*/0, /*pts_us=*/0,
                                     /*body=*/{});
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [fd, c] : clients_) {
            (void)c;
            ::write(fd, bye->prefix.data(), bye->prefix.size());
        }
    }

    if (thread_.joinable())
        thread_.join();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [fd, c] : clients_)
            ::close(fd);
        clients_.clear();
    }
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    ::unlink(socket_path_.c_str());
}

WorkerLink::MessagePtr WorkerLink::makeControl(uint8_t type, uint8_t stream, uint8_t flags,
                                              uint32_t sequence, int64_t pts_us,
                                              std::vector<uint8_t> body, bool control) {
    auto msg = std::make_shared<Message>();
    ss::Header h{};
    h.length = ss::kHeaderLengthBytes + static_cast<uint32_t>(body.size());
    h.version = ss::kProtocolVersion;
    h.type = type;
    h.stream = stream;
    h.flags = flags;
    h.sequence = sequence;
    h.generation = generation_;
    h.pts_us = static_cast<uint64_t>(pts_us);
    msg->prefix.resize(ss::kHeaderSize + body.size());
    ss::SerializeHeader(h, msg->prefix.data());
    if (!body.empty())
        std::memcpy(msg->prefix.data() + ss::kHeaderSize, body.data(), body.size());
    msg->payload = nullptr;
    msg->control = control;
    return msg;
}

WorkerLink::MessagePtr WorkerLink::makeMedia(uint8_t type, uint8_t stream, uint8_t flags,
                                            uint32_t sequence, int64_t pts_us,
                                            std::shared_ptr<const std::vector<uint8_t>> payload) {
    auto msg = std::make_shared<Message>();
    const uint32_t payload_len = payload ? static_cast<uint32_t>(payload->size()) : 0;
    ss::Header h{};
    h.length = ss::kHeaderLengthBytes + payload_len;
    h.version = ss::kProtocolVersion;
    h.type = type;
    h.stream = stream;
    h.flags = flags;
    h.sequence = sequence;
    h.generation = generation_;
    h.pts_us = static_cast<uint64_t>(pts_us);
    msg->prefix.resize(ss::kHeaderSize);
    ss::SerializeHeader(h, msg->prefix.data());
    msg->payload = std::move(payload);
    msg->control = false;
    return msg;
}

void WorkerLink::enqueue(const MessagePtr& msg, bool video, bool audio, bool events) {
    // Caller holds mutex_.
    for (auto& [fd, c] : clients_) {
        if (c.dead) continue;
        bool wanted = (video && c.want_video) || (audio && c.want_audio) ||
                      (events && c.want_events) || msg->control;
        if (!wanted) continue;

        // Bounded queue: drop oldest non-control messages on overflow.
        while ((c.queue.size() >= cfg_.queue_max_msgs ||
                c.queued_bytes + msg->wire_size() > cfg_.queue_max_bytes) &&
               !c.queue.empty()) {
            // Never drop a control message, and never drop the partially-written
            // front message.
            auto& front = c.queue.front();
            if (front->control || c.front_offset > 0) break;
            c.queued_bytes -= front->wire_size();
            c.queue.pop_front();
            ++c.dropped;
        }
        c.queue.push_back(msg);
        c.queued_bytes += msg->wire_size();
        onClientWritable(c);
    }
    reapDead();
}

void WorkerLink::runLoop() {
    while (running_.load()) {
        std::vector<pollfd> pfds;
        pfds.push_back({listen_fd_, POLLIN, 0});
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& [fd, c] : clients_) {
                short ev = POLLIN;
                if (!c.queue.empty()) ev |= POLLOUT;
                pfds.push_back({fd, ev, 0});
            }
        }

        int rc = ::poll(pfds.data(), pfds.size(), 200);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (rc > 0 && (pfds[0].revents & POLLIN))
            acceptClient();

        std::lock_guard<std::mutex> lock(mutex_);
        if (rc > 0) {
            for (size_t i = 1; i < pfds.size(); ++i) {
                int fd = pfds[i].fd;
                auto it = clients_.find(fd);
                if (it == clients_.end()) continue;
                Client& c = it->second;  // stable: we never erase mid-loop, only mark dead
                if (pfds[i].revents & (POLLHUP | POLLERR)) { c.dead = true; continue; }
                if (pfds[i].revents & POLLIN)  onClientReadable(c);
                if (!c.dead && (pfds[i].revents & POLLOUT)) onClientWritable(c);
            }
        }

        // Periodic per-consumer Stats so the daemon can observe liveness + drops.
        // Runs every iteration (including poll timeouts) so an idle consumer still
        // gets stats.
        auto now = std::chrono::steady_clock::now();
        if (now - last_stats_ >= cfg_.stats_interval) {
            last_stats_ = now;
            for (auto& [fd, c] : clients_) {
                if (c.dead) continue;
                MessagePtr st = makeControl(static_cast<uint8_t>(ss::MessageType::Stats),
                                            static_cast<uint8_t>(ss::StreamId::Video),
                                            /*flags=*/0, /*sequence=*/0, /*pts_us=*/0,
                                            ss::BuildStats(c.sent, c.dropped),
                                            /*control=*/false);
                c.queue.push_back(st);
                c.queued_bytes += st->wire_size();
                onClientWritable(c);
            }
        }
        reapDead();
    }
}

void WorkerLink::acceptClient() {
    int cfd = ::accept(listen_fd_, nullptr, nullptr);
    if (cfd < 0) return;

    std::lock_guard<std::mutex> lock(mutex_);
    if (clients_.size() >= cfg_.max_clients) {
        ::close(cfd);
        return;
    }
    Client c;
    c.fd = cfd;
    // Canonical consumers (zm-api, zmc-style) send no client->server messages, so
    // fan out everything by default; the optional Subscribe extension can narrow
    // it later. New consumers get HELLO(s), then the current-status snapshot, then
    // the cached keyframe so they can initialize and render immediately.
    c.want_video = true;
    c.want_audio = true;
    c.want_events = true;
    auto push = [&c](const MessagePtr& m) {
        if (!m) return;
        c.queue.push_back(m);
        c.queued_bytes += m->wire_size();
    };
    push(hello_video_);
    push(hello_audio_);
    push(snapshot_);
    push(keyframe_);
    auto [it, _] = clients_.emplace(cfd, std::move(c));
    onClientWritable(it->second);
}

void WorkerLink::onClientReadable(Client& c) {
    char buf[4096];
    ssize_t n = ::read(c.fd, buf, sizeof(buf));
    if (n <= 0) { c.dead = true; return; }
    c.inbuf.append(buf, static_cast<size_t>(n));

    // Parse as many complete wire units as are buffered. Inbound frames use the
    // canonical 24-byte header with zm-next control extension message types.
    while (c.inbuf.size() >= ss::kHeaderSize) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(c.inbuf.data());
        ss::Header h{};
        if (!ss::ParseHeader(p, h)) { c.dead = true; return; }  // corrupt stream
        const size_t payload_len = h.payload_size();
        const size_t need = ss::kHeaderSize + payload_len;
        if (c.inbuf.size() < need) break;

        const char* body = c.inbuf.data() + ss::kHeaderSize;
        switch (static_cast<ss::MessageType>(h.type)) {
            case ss::MessageType::Subscribe: {
                json j = json::parse(body, body + payload_len, nullptr, false);
                if (j.is_object()) {
                    c.want_video = j.value("video", true);
                    c.want_audio = j.value("audio", true);
                    c.want_events = j.value("events", true);
                    // Replay the cached Hello(s) for the just-subscribed streams.
                    auto push = [&c](const MessagePtr& m) {
                        if (!m) return;
                        c.queue.push_back(m);
                        c.queued_bytes += m->wire_size();
                    };
                    if (c.want_video) push(hello_video_);
                    if (c.want_audio) push(hello_audio_);
                    onClientWritable(c);
                }
                break;
            }
            case ss::MessageType::Command: {
                json j = json::parse(body, body + payload_len, nullptr, false);
                CommandResult r{false, "bad command", ""};
                std::string name;
                uint64_t request_id = 0;
                if (j.is_object()) {
                    // Accept either {"cmd":...} (zm-api plugin-targeted commands) or
                    // {"name":...,"args":...} (core control). Pass the FULL payload
                    // JSON to the handler so plugin commands keep every field.
                    name = j.value("cmd", j.value("name", std::string{}));
                    request_id = j.value("request_id", 0ull);
                    std::string raw(body, payload_len);
                    if (handler_) r = handler_(name, raw);
                    else r = {false, "no command handler", ""};
                }
                json resp = {{"request_id", request_id}, {"ok", r.ok},
                             {"message", r.message}, {"data", r.data_json}};
                std::string rs = resp.dump();
                MessagePtr out = makeControl(static_cast<uint8_t>(ss::MessageType::Response),
                                             static_cast<uint8_t>(ss::StreamId::Monitor),
                                             /*flags=*/0, /*sequence=*/0, /*pts_us=*/0,
                                             std::vector<uint8_t>(rs.begin(), rs.end()));
                c.queue.push_back(out);
                c.queued_bytes += out->wire_size();
                onClientWritable(c);
                break;
            }
            case ss::MessageType::Talkback: {
                // Payload = [u32 codec_le][raw audio]; pts in the header.
                if (talkbackHandler_ && payload_len >= 4) {
                    uint32_t codec = get_u32_le(reinterpret_cast<const uint8_t*>(body));
                    talkbackHandler_(codec, static_cast<int64_t>(h.pts_us),
                                     std::string(body + 4, payload_len - 4));
                }
                break;
            }
            default:
                break;  // ignore other / unknown inbound types
        }
        c.inbuf.erase(0, need);
    }
}

void WorkerLink::onClientWritable(Client& c) {
    while (!c.queue.empty()) {
        const MessagePtr& msg = c.queue.front();
        const size_t prefix_sz = msg->prefix.size();
        const size_t payload_sz = msg->payload ? msg->payload->size() : 0;
        const size_t total = prefix_sz + payload_sz;

        // Assemble up to two iovecs for the not-yet-written tail of this message.
        iovec iov[2];
        int iovcnt = 0;
        size_t off = c.front_offset;
        if (off < prefix_sz) {
            iov[iovcnt].iov_base = const_cast<uint8_t*>(msg->prefix.data() + off);
            iov[iovcnt].iov_len = prefix_sz - off;
            ++iovcnt;
            if (payload_sz) {
                iov[iovcnt].iov_base = const_cast<uint8_t*>(msg->payload->data());
                iov[iovcnt].iov_len = payload_sz;
                ++iovcnt;
            }
        } else {
            size_t poff = off - prefix_sz;
            iov[iovcnt].iov_base = const_cast<uint8_t*>(msg->payload->data() + poff);
            iov[iovcnt].iov_len = payload_sz - poff;
            ++iovcnt;
        }

        ssize_t w = ::writev(c.fd, iov, iovcnt);
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return; // try again on next POLLOUT
            if (errno == EINTR) continue;
            c.dead = true;
            return;
        }
        c.front_offset += static_cast<size_t>(w);
        if (c.front_offset >= total) {
            c.front_offset = 0;
            c.queued_bytes -= msg->wire_size();
            ++c.sent;
            c.queue.pop_front();
        } else {
            return; // partial write; resume on next POLLOUT
        }
    }
}

void WorkerLink::reapDead() {
    // Caller holds mutex_. Erase clients marked dead by an I/O failure or hangup.
    for (auto it = clients_.begin(); it != clients_.end();) {
        if (it->second.dead) {
            ::close(it->first);
            it = clients_.erase(it);
        } else {
            ++it;
        }
    }
}

void WorkerLink::publishEventJson(const std::string& raw_event_json) {
    if (!running_.load()) return;

    json j = json::parse(raw_event_json, nullptr, /*allow_exceptions=*/false);
    const bool obj = !j.is_discarded() && j.is_object();

    // The capture plugin's StreamMetadata event carries codec parameters; turn it
    // into a Hello (the media handshake) rather than a generic Event.
    if (obj && j.value("event", std::string{}) == "StreamMetadata") {
        std::vector<uint8_t> extradata;
        if (j.contains("extradata") && j["extradata"].is_string())
            extradata = base64_decode(j["extradata"].get<std::string>());
        const bool isAudio = (j.value("media", std::string("video")) == "audio");
        setStreamParams(/*stream=*/isAudio ? 2u : 1u,
                        static_cast<uint32_t>(j.value("codec_id", 0)),
                        extradata.data(), extradata.size(),
                        static_cast<uint32_t>(j.value("width", 0)),
                        static_cast<uint32_t>(j.value("height", 0)),
                        /*fps_num=*/0, /*fps_den=*/0,
                        static_cast<uint32_t>(j.value("sample_rate", 0)),
                        static_cast<uint32_t>(j.value("channels", 0)));
        return;
    }

    const std::string type = (obj && j.contains("type") && j["type"].is_string())
                                 ? j["type"].get<std::string>() : "";
    const std::string event = (obj && j.contains("event") && j["event"].is_string())
                                  ? j["event"].get<std::string>() : "";

    ss::MonitorEvent ev;
    ev.code = map_event_code(type, event);
    ev.wall_clock_us = static_cast<uint64_t>(now_usec());
    ev.has_wall_clock = true;
    // Lifecycle events surface human-readable detail in `message`; analysis/AI
    // events carry their structured payload in the JSON detail TLV.
    if (is_ai_code(ev.code))
        ev.json_detail = raw_event_json;
    else
        ev.message = raw_event_json;

    // Lifecycle/health events represent current monitor status, so cache the
    // latest as the connect-time snapshot. Detection/description/recording are
    // per-event streams (not "status") and must not clobber the snapshot.
    const bool is_health = ev.code != 0 && ev.code != ss::kEventSnapshot &&
                           !is_ai_code(ev.code);

    std::lock_guard<std::mutex> lock(mutex_);
    MessagePtr msg = makeControl(static_cast<uint8_t>(ss::MessageType::Event),
                                 static_cast<uint8_t>(ss::StreamId::Monitor),
                                 /*flags=*/0, event_sequence_++, /*pts_us=*/0,
                                 ss::BuildEvent(ev));
    enqueue(msg, /*video=*/false, /*audio=*/false, /*events=*/true);
    if (is_health) snapshot_ = msg;
}

void WorkerLink::setSnapshotJson(const std::string& raw_event_json) {
    ss::MonitorEvent ev;
    ev.code = ss::kEventSnapshot;
    ev.wall_clock_us = static_cast<uint64_t>(now_usec());
    ev.has_wall_clock = true;
    ev.message = raw_event_json;

    std::lock_guard<std::mutex> lock(mutex_);
    // Tagged with the current event sequence as a baseline; cached, not broadcast.
    snapshot_ = makeControl(static_cast<uint8_t>(ss::MessageType::Event),
                            static_cast<uint8_t>(ss::StreamId::Monitor),
                            /*flags=*/0, event_sequence_, /*pts_us=*/0,
                            ss::BuildEvent(ev));
}

void WorkerLink::setStreamParams(uint32_t stream, uint32_t codec_id,
                                 const uint8_t* extradata, size_t extradata_len,
                                 uint32_t width, uint32_t height,
                                 uint32_t fps_num, uint32_t fps_den,
                                 uint32_t sample_rate, uint32_t channels) {
    std::vector<uint8_t> body = ss::BuildHello(codec_id, extradata, extradata_len,
                                               width, height, fps_num, fps_den,
                                               sample_rate, channels);
    const uint8_t wstream = wire_stream(stream);
    const bool isAudio = (wstream == static_cast<uint8_t>(ss::StreamId::Audio));

    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<uint8_t>& cached = isAudio ? hello_audio_body_ : hello_video_body_;
    if (body == cached) return;  // no change: don't bump generation / re-broadcast

    const bool reconfigure = !cached.empty();
    if (reconfigure) {
        ++generation_;
        sequence_[0] = sequence_[1] = 0;
        keyframe_.reset();
    }
    cached = body;
    MessagePtr hello = makeControl(static_cast<uint8_t>(ss::MessageType::Hello),
                                   wstream, /*flags=*/0, /*sequence=*/0, /*pts_us=*/0,
                                   std::vector<uint8_t>(body));
    (isAudio ? hello_audio_ : hello_video_) = hello;
    enqueue(hello, /*video=*/!isAudio, /*audio=*/isAudio, /*events=*/false);

    // Re-issue the other stream's HELLO so both carry the new generation.
    if (reconfigure) {
        if (isAudio && !hello_video_body_.empty()) {
            hello_video_ = makeControl(static_cast<uint8_t>(ss::MessageType::Hello),
                                       static_cast<uint8_t>(ss::StreamId::Video),
                                       0, 0, 0, std::vector<uint8_t>(hello_video_body_));
            enqueue(hello_video_, /*video=*/true, /*audio=*/false, /*events=*/false);
        } else if (!isAudio && !hello_audio_body_.empty()) {
            hello_audio_ = makeControl(static_cast<uint8_t>(ss::MessageType::Hello),
                                       static_cast<uint8_t>(ss::StreamId::Audio),
                                       0, 0, 0, std::vector<uint8_t>(hello_audio_body_));
            enqueue(hello_audio_, /*video=*/false, /*audio=*/true, /*events=*/false);
        }
    }
}

void WorkerLink::sendMedia(uint32_t stream, bool keyframe, int64_t pts_us,
                           std::shared_ptr<const std::vector<uint8_t>> payload) {
    if (!running_.load() || !payload || payload->empty()) return;
    const uint8_t wstream = wire_stream(stream);
    const bool is_video = (wstream == static_cast<uint8_t>(ss::StreamId::Video));
    const bool video_keyframe = keyframe && is_video;
    const uint8_t flags = video_keyframe ? ss::kFlagKeyframe : 0;

    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t seq = sequence_[wstream]++;  // per-stream counter; gaps => drops

    // Cache the keyframe for fast-start of late joiners (same refcounted buffer).
    if (video_keyframe) {
        keyframe_ = makeMedia(static_cast<uint8_t>(ss::MessageType::Keyframe),
                              wstream, flags, seq, pts_us, payload);
    }
    if (clients_.empty()) return;  // sequence still advanced + keyframe cached

    MessagePtr msg = makeMedia(static_cast<uint8_t>(ss::MessageType::Media),
                               wstream, flags, seq, pts_us, std::move(payload));
    enqueue(msg, /*video=*/is_video, /*audio=*/!is_video, /*events=*/false);
}

} // namespace zm

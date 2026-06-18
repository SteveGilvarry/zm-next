#include "zm/WorkerLink.hpp"

#include "worker_link.pb.h"
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

namespace pb = zm::worker::v1;
using json = nlohmann::json;

namespace {

int64_t now_usec() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

void put_u32_le(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xff);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xff);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xff);
}

uint32_t get_u32_le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// Map a plugin EventBus JSON payload onto an Event code. Plugins currently emit
// ad-hoc {"type":...} objects; lifecycle types map to their codes, motion/zone
// types to DETECTION, everything else to UNSPECIFIED. The raw JSON is preserved
// in `message` until plugins emit fully-structured events (Phase 3).
// Minimal base64 decoder (the capture plugin base64-encodes codec extradata in
// its StreamMetadata event; core is FFmpeg-free so we decode it here).
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
        if (c == '=' ) break;
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

pb::Event::Code map_event_code(const std::string& type) {
    if (type == "connection_failed")      return pb::Event::CONNECTION_FAILED;
    if (type == "connection_restored")    return pb::Event::CONNECTION_RESTORED;
    if (type == "prime_capture_failed")   return pb::Event::PRIME_CAPTURE_FAILED;
    if (type == "prime_capture_restored") return pb::Event::PRIME_CAPTURE_RESTORED;
    if (type == "capture_failed")         return pb::Event::CAPTURE_FAILED;
    if (type == "capture_resumed")        return pb::Event::CAPTURE_RESUMED;
    if (type == "state_changed")          return pb::Event::STATE_CHANGED;
    if (type == "motion" || type == "zone_motion" || type == "detection")
        return pb::Event::DETECTION;
    if (type == "description")            return pb::Event::DESCRIPTION;
    return pb::Event::CODE_UNSPECIFIED;
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
        pb::Frame frame;
        frame.set_monitor_id(monitor_id_);
        frame.mutable_bye()->set_reason("worker shutting down");
        MessagePtr bye = buildFrameMessage(frame, nullptr, /*control=*/true);
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

WorkerLink::MessagePtr WorkerLink::buildFrameMessage(
    const pb::Frame& frame, std::shared_ptr<const std::vector<uint8_t>> payload, bool control) {
    auto msg = std::make_shared<Message>();
    const uint32_t pb_len = static_cast<uint32_t>(frame.ByteSizeLong());
    const uint32_t payload_len = payload ? static_cast<uint32_t>(payload->size()) : 0;
    msg->prefix.resize(8 + pb_len);
    put_u32_le(msg->prefix.data(), pb_len);
    put_u32_le(msg->prefix.data() + 4, payload_len);
    frame.SerializeToArray(msg->prefix.data() + 8, static_cast<int>(pb_len));
    msg->payload = std::move(payload);
    msg->control = control;
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
                pb::Frame f;
                f.set_monitor_id(monitor_id_);
                auto* st = f.mutable_stats();
                st->set_messages_sent(c.sent);
                st->set_messages_dropped(c.dropped);
                c.queue.push_back(buildFrameMessage(f, nullptr, /*control=*/true));
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
    // Replay the cached snapshot so a late subscriber learns current status
    // without waiting for the next transition (events analogue of a keyframe).
    if (snapshot_) {
        c.queue.push_back(snapshot_);
        c.queued_bytes += snapshot_->wire_size();
    }
    auto [it, _] = clients_.emplace(cfd, std::move(c));
    onClientWritable(it->second);
}

void WorkerLink::onClientReadable(Client& c) {
    char buf[4096];
    ssize_t n = ::read(c.fd, buf, sizeof(buf));
    if (n <= 0) { c.dead = true; return; }
    c.inbuf.append(buf, static_cast<size_t>(n));

    // Parse as many complete wire units as are buffered.
    while (c.inbuf.size() >= 8) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(c.inbuf.data());
        uint32_t pb_len = get_u32_le(p);
        uint32_t payload_len = get_u32_le(p + 4);
        size_t need = 8 + pb_len + payload_len;
        if (c.inbuf.size() < need) break;

        pb::Frame frame;
        if (frame.ParseFromArray(c.inbuf.data() + 8, static_cast<int>(pb_len))) {
            switch (frame.payload_case()) {
                case pb::Frame::kSubscribe: {
                    const auto& s = frame.subscribe();
                    c.want_video = s.video();
                    c.want_audio = s.audio();
                    c.want_events = s.events();
                    // Replay the cached Hello(s) for the just-subscribed streams so
                    // a late consumer can initialize its decoder immediately.
                    for (const auto& [stream, hello] : helloByStream_) {
                        const bool isAudio = (stream == pb::STREAM_KIND_AUDIO);
                        if ((isAudio && c.want_audio) || (!isAudio && c.want_video)) {
                            c.queue.push_back(hello);
                            c.queued_bytes += hello->wire_size();
                        }
                    }
                    onClientWritable(c);
                    break;
                }
                case pb::Frame::kCommand: {
                    const auto& cmd = frame.command();
                    CommandResult r{false, "no command handler", ""};
                    if (handler_) r = handler_(cmd.name(), cmd.args_json());
                    pb::Frame out;
                    out.set_monitor_id(monitor_id_);
                    auto* resp = out.mutable_response();
                    resp->set_request_id(cmd.request_id());
                    resp->set_ok(r.ok);
                    resp->set_message(r.message);
                    resp->set_data_json(r.data_json);
                    c.queue.push_back(buildFrameMessage(out, nullptr, /*control=*/true));
                    onClientWritable(c);
                    break;
                }
                case pb::Frame::kTalkback: {
                    // Two-way audio: relay to the camera backchannel handler.
                    if (talkbackHandler_) {
                        const auto& tb = frame.talkback();
                        talkbackHandler_(tb.codec(), tb.pts_us(), tb.data());
                    }
                    break;
                }
                default:
                    break; // ignore other inbound types
            }
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

    // The capture plugin's StreamMetadata event carries codec parameters; turn it
    // into a Hello (the media handshake) rather than a generic Event.
    if (!j.is_discarded() && j.is_object() &&
        j.value("event", std::string{}) == "StreamMetadata") {
        std::vector<uint8_t> extradata;
        if (j.contains("extradata") && j["extradata"].is_string())
            extradata = base64_decode(j["extradata"].get<std::string>());
        const bool isAudio = (j.value("media", std::string("video")) == "audio");
        setStreamParams(static_cast<uint32_t>(isAudio ? pb::STREAM_KIND_AUDIO
                                                      : pb::STREAM_KIND_VIDEO),
                        static_cast<uint32_t>(j.value("codec_id", 0)),
                        extradata.data(), extradata.size(),
                        static_cast<uint32_t>(j.value("width", 0)),
                        static_cast<uint32_t>(j.value("height", 0)),
                        /*fps_num=*/0, /*fps_den=*/0,
                        static_cast<uint32_t>(j.value("sample_rate", 0)),
                        static_cast<uint32_t>(j.value("channels", 0)));
        return;
    }

    pb::Frame frame;
    frame.set_monitor_id(monitor_id_);
    auto* ev = frame.mutable_event();

    std::string type = (!j.is_discarded() && j.is_object() && j.contains("type") &&
                        j["type"].is_string())
                           ? j["type"].get<std::string>()
                           : "";
    ev->set_code(map_event_code(type));
    ev->set_wall_clock_us(now_usec());
    ev->set_message(raw_event_json);

    // Populate the structured Description for VLM scene-understanding events.
    if (type == "description" && j.is_object()) {
        auto* d = ev->mutable_description();
        d->set_text(j.value("text", std::string{}));
        d->set_prompt(j.value("prompt", std::string{}));
        d->set_model(j.value("model", std::string{}));
    }

    const pb::Event::Code code = ev->code();
    // Lifecycle/health events represent current monitor status, so cache the
    // latest as the connect-time snapshot — a consumer that connects later learns
    // the current state immediately. Detection/description are per-event streams
    // (too frequent, not "status") and must not clobber the snapshot.
    const bool is_health = code != pb::Event::CODE_UNSPECIFIED &&
                           code != pb::Event::SNAPSHOT &&
                           code != pb::Event::DETECTION &&
                           code != pb::Event::DESCRIPTION;

    MessagePtr msg;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        frame.set_sequence(event_sequence_++);
        frame.set_generation(generation_);
        msg = buildFrameMessage(frame, nullptr, /*control=*/true);
        enqueue(msg, /*video=*/false, /*audio=*/false, /*events=*/true);
        if (is_health) snapshot_ = msg;
    }
}

void WorkerLink::setSnapshotJson(const std::string& raw_event_json) {
    pb::Frame frame;
    frame.set_monitor_id(monitor_id_);
    auto* ev = frame.mutable_event();
    ev->set_code(pb::Event::SNAPSHOT);
    ev->set_wall_clock_us(now_usec());
    ev->set_message(raw_event_json);

    std::lock_guard<std::mutex> lock(mutex_);
    frame.set_generation(generation_);
    snapshot_ = buildFrameMessage(frame, nullptr, /*control=*/true);
}

void WorkerLink::setStreamParams(uint32_t stream, uint32_t codec_id,
                                 const uint8_t* extradata, size_t extradata_len,
                                 uint32_t width, uint32_t height,
                                 uint32_t fps_num, uint32_t fps_den,
                                 uint32_t sample_rate, uint32_t channels) {
    pb::Frame frame;
    frame.set_monitor_id(monitor_id_);
    auto* h = frame.mutable_hello();
    h->set_stream(static_cast<pb::StreamKind>(stream));
    h->set_codec_id(codec_id);
    if (extradata && extradata_len)
        h->set_extradata(extradata, extradata_len);
    h->set_width(width);
    h->set_height(height);
    h->set_fps_num(fps_num);
    h->set_fps_den(fps_den);
    h->set_sample_rate(sample_rate);
    h->set_channels(channels);

    std::lock_guard<std::mutex> lock(mutex_);
    ++generation_;
    frame.set_generation(generation_);
    MessagePtr msg = buildFrameMessage(frame, nullptr, /*control=*/true);
    // Cache the latest Hello per stream kind so a client that subscribes after
    // startup still receives it (see the Subscribe handler).
    helloByStream_[stream] = msg;
    enqueue(msg, /*video=*/true, /*audio=*/true, /*events=*/false);
}

void WorkerLink::sendMedia(uint32_t stream, bool keyframe, int64_t pts_us,
                           std::shared_ptr<const std::vector<uint8_t>> payload) {
    if (!running_.load()) return;
    pb::Frame frame;
    frame.set_monitor_id(monitor_id_);
    auto* m = frame.mutable_media();
    m->set_stream(static_cast<pb::StreamKind>(stream));
    m->set_keyframe(keyframe);
    m->set_pts_us(pts_us);

    const bool is_video = (static_cast<pb::StreamKind>(stream) == pb::STREAM_KIND_VIDEO);
    std::lock_guard<std::mutex> lock(mutex_);
    frame.set_sequence(media_sequence_++);  // per-stream counter; gaps => drops
    frame.set_generation(generation_);
    MessagePtr msg = buildFrameMessage(frame, std::move(payload), /*control=*/false);
    enqueue(msg, /*video=*/is_video, /*audio=*/!is_video, /*events=*/false);
}

} // namespace zm

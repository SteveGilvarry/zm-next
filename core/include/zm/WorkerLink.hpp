#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <deque>
#include <unordered_map>
#include <memory>
#include <vector>
#include <functional>
#include <chrono>
#include <cstdint>

namespace zm {

// The per-monitor worker link: one Unix domain socket
// (${ZM_PATH_SOCKS}/stream_{monitor_id}.sock) serving the canonical stream
// socket protocol (zm/stream_socket_protocol.hpp) to multiple consumers at once.
// Byte-compatible with the ZoneMinder zmc producer and the zm-api consumer.
//   push  (server->client, canonical): Hello / Media / Keyframe / Stats / Event / Bye
//   pull  (client->server, zm-next extension): Subscribe / Command -> Response, Talkback
//
// Wire unit: [24-byte canonical header][payload]. For control frames
// (Hello/Event/Stats/Bye) the header is followed by a small TLV/blob body kept
// in `prefix`; for Media/Keyframe the header is `prefix` and the access unit is
// the refcounted `payload` — one buffer shared across every consumer queue and
// written with writev(), so the payload is never copied per-consumer.
//
// The producer never blocks on consumers: each consumer has a bounded queue; on
// overflow the oldest non-control messages are dropped (observable as Frame
// sequence gaps and in Stats). Control messages (Hello/Event/Bye/Response) are
// never dropped.
class WorkerLink {
public:
    struct Config {
        std::string group;                 // socket group for chmod 0660; empty = leave default
        std::vector<uint32_t> allowed_uids;// empty = allow all (filesystem perms still apply)
        size_t max_clients = 8;
        size_t queue_max_bytes = 8 * 1024 * 1024;
        size_t queue_max_msgs = 256;
        std::chrono::milliseconds stall_timeout = std::chrono::seconds(10);
        std::chrono::milliseconds stats_interval = std::chrono::seconds(5);
    };

    // Answer to a client Command. data_json is an optional structured result.
    struct CommandResult {
        bool ok = false;
        std::string message;
        std::string data_json;
    };
    // Handles a client Command (name + JSON args) and returns the Response body.
    using CommandHandler =
        std::function<CommandResult(const std::string& name, const std::string& args_json)>;

    // Handles inbound two-way-audio (talkback) chunks: (AVCodecID, pts_us, bytes).
    // The handler relays them to the camera's audio backchannel.
    using TalkbackHandler =
        std::function<void(uint32_t codec, int64_t pts_us, const std::string& data)>;

    WorkerLink(uint32_t monitor_id, std::string socket_path);
    WorkerLink(uint32_t monitor_id, std::string socket_path, Config cfg);
    ~WorkerLink();

    WorkerLink(const WorkerLink&) = delete;
    WorkerLink& operator=(const WorkerLink&) = delete;

    bool start();   // unlink stale path, bind, apply perms, listen, spawn thread
    void stop();    // broadcast Bye, close everything, unlink

    void setCommandHandler(CommandHandler handler);
    void setTalkbackHandler(TalkbackHandler handler);

    // --- events ---------------------------------------------------------------
    // Publish a lifecycle/detection event from the in-process EventBus. For the
    // first slice we accept the raw plugin JSON and map it onto an Event frame;
    // as plugins emit structured events this gains typed overloads. Broadcast to
    // every consumer subscribed to events. Never dropped.
    void publishEventJson(const std::string& raw_event_json);

    // Cache the current-status snapshot replayed to each new consumer on connect
    // (the events analogue of the cached keyframe). Caching only; no broadcast.
    void setSnapshotJson(const std::string& raw_event_json);

    // --- media (wired in the media+Hello task) --------------------------------
    // Set/replace the codec parameters for a stream; caches and broadcasts a
    // Hello, bumping generation if they changed. Plain values so core stays
    // FFmpeg-free (the capture plugin supplies them via the host API).
    void setStreamParams(uint32_t stream, uint32_t codec_id,
                         const uint8_t* extradata, size_t extradata_len,
                         uint32_t width, uint32_t height,
                         uint32_t fps_num, uint32_t fps_den,
                         uint32_t sample_rate = 0, uint32_t channels = 0);

    // Queue one access unit to every subscribed consumer. The payload is shared
    // by refcount, not copied. Never blocks.
    void sendMedia(uint32_t stream, bool keyframe, int64_t pts_us,
                   std::shared_ptr<const std::vector<uint8_t>> payload);

private:
    // One serialized message shared across all consumer queues. `prefix` holds
    // the 24-byte canonical header plus any TLV/blob body (built once); `payload`
    // holds the refcounted media bytes (null for control/event frames).
    struct Message {
        std::vector<uint8_t> prefix;
        std::shared_ptr<const std::vector<uint8_t>> payload;
        bool control = false;
        size_t wire_size() const {
            return prefix.size() + (payload ? payload->size() : 0);
        }
    };
    using MessagePtr = std::shared_ptr<const Message>;

    struct Client {
        int fd = -1;
        std::deque<MessagePtr> queue;
        size_t queued_bytes = 0;
        size_t front_offset = 0;     // bytes of queue.front() already written
        uint64_t sent = 0;
        uint64_t dropped = 0;
        bool want_video = false;
        bool want_audio = false;
        bool want_events = true;     // events on by default until a Subscribe arrives
        bool dead = false;           // marked on fatal I/O; reaped after iteration
        std::string inbuf;           // partial inbound wire unit
        uint32_t uid = 0;
        uint32_t pid = 0;
    };

    void runLoop();
    void acceptClient();
    void onClientReadable(Client& c);
    void onClientWritable(Client& c);
    void enqueue(const MessagePtr& msg, bool video, bool audio, bool events);
    void reapDead();             // close + erase clients marked dead (mutex held)
    // Build a control message (Hello/Event/Stats/Bye/Response): prefix = 24-byte
    // canonical header + `body`, no media payload, never dropped.
    MessagePtr makeControl(uint8_t type, uint8_t stream, uint8_t flags,
                           uint32_t sequence, int64_t pts_us,
                           std::vector<uint8_t> body, bool control = true);
    // Build a media message (Media/Keyframe): prefix = 24-byte canonical header,
    // `payload` is the refcounted access unit shared across consumer queues.
    MessagePtr makeMedia(uint8_t type, uint8_t stream, uint8_t flags,
                         uint32_t sequence, int64_t pts_us,
                         std::shared_ptr<const std::vector<uint8_t>> payload);

    uint32_t monitor_id_;
    std::string socket_path_;
    Config cfg_;
    int listen_fd_{-1};
    std::thread thread_;
    std::atomic<bool> running_{false};

    std::mutex mutex_;
    std::unordered_map<int, Client> clients_;
    CommandHandler handler_;
    TalkbackHandler talkbackHandler_;

    uint32_t event_sequence_{0};
    uint32_t sequence_[2]{0, 0};     // per-stream media counter, indexed by wire StreamId (Video, Audio)
    uint32_t generation_{0};
    MessagePtr snapshot_;            // current-status EVENT, replayed on connect
    // Cached HELLOs (per stream) + the most recent keyframe, replayed to each
    // new consumer on connect so it can init its decoder and render immediately
    // without waiting for the next generation bump / GOP.
    MessagePtr hello_video_;
    MessagePtr hello_audio_;
    MessagePtr keyframe_;
    std::vector<uint8_t> hello_video_body_;  // for change detection (skip no-op generation bumps)
    std::vector<uint8_t> hello_audio_body_;
    std::chrono::steady_clock::time_point last_stats_;
};

} // namespace zm

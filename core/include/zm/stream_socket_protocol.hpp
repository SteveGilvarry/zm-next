// Per-monitor media stream socket wire protocol.
//
// Ported from the ZoneMinder project's stream socket protocol
// (src/zm_stream_socket_protocol.{h,cpp} on feature/stream-socket-events,
// GPLv2-or-later) and adapted for zm-next: the FFmpeg dependency is removed so
// zm-next's FFmpeg-free core can encode/decode it, and the EVENT channel is
// extended with zm-next analysis/AI event codes (detection / description /
// recording) plus a JSON detail TLV. The wire format is byte-compatible with
// the ZoneMinder producer (zmc) and the zm-api consumer so all three speak one
// protocol; new codes/tags are additive and skip-on-unknown (version stays 1).
//
// Wire layout (all integers little-endian). Every message:
//   u32  length      bytes following this field (fixed header remainder + payload)
//   u8   version     protocol version, currently 1
//   u8   type        MessageType
//   u8   stream      StreamId (0 video, 1 audio, 2 monitor)
//   u8   flags       bit 0: keyframe (video); other bits reserved, must be 0
//   u32  sequence    per-stream, counts every message produced (drops = gaps)
//   u32  generation  stream epoch; a bump means re-init the decoder from a HELLO
//   u64  pts_us      microseconds, AV_TIME_BASE_Q, shared clock for both streams
//   [payload]
//
// HELLO and EVENT payloads are TLV lists (u8 tag, u16 length, value); unknown
// tags must be skipped by consumers.

#ifndef ZM_STREAM_SOCKET_PROTOCOL_HPP
#define ZM_STREAM_SOCKET_PROTOCOL_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace zm {
namespace stream_socket {

constexpr uint8_t kProtocolVersion = 1;

// Total serialized header size, including the leading length field.
constexpr size_t kHeaderSize = 24;
// Bytes of fixed header counted by the length field (kHeaderSize minus the
// length field itself).
constexpr uint32_t kHeaderLengthBytes = kHeaderSize - sizeof(uint32_t);
// Sanity cap on the length field; larger values mean a corrupt or hostile peer.
constexpr uint32_t kMaxMessageLength = 32 * 1024 * 1024;

// codec_id sentinel for "not present" (== FFmpeg AV_CODEC_ID_NONE).
constexpr uint32_t kCodecIdNone = 0;

enum class MessageType : uint8_t {
  Hello    = 0x01,
  Media    = 0x02,
  Keyframe = 0x03,
  Stats    = 0x04,
  Bye      = 0x05,
  Event    = 0x06,

  // zm-next client->server control extension (additive; canonical consumers
  // never send these and the producer ignores unknown inbound types).
  Subscribe = 0x10,
  Command   = 0x11,
  Response  = 0x12,
  Talkback  = 0x13,
};

enum class StreamId : uint8_t {
  Video   = 0,
  Audio   = 1,
  Monitor = 2,  // EVENT frames: monitor lifecycle, neither video nor audio
};

constexpr uint8_t kFlagKeyframe = 0x01;

// HELLO TLV tags
constexpr uint8_t kTlvCodecId    = 0x01;  // u32, AVCodecID value
constexpr uint8_t kTlvExtradata  = 0x02;  // raw codecpar->extradata
constexpr uint8_t kTlvWidth      = 0x03;  // u32
constexpr uint8_t kTlvHeight     = 0x04;  // u32
constexpr uint8_t kTlvFpsNum     = 0x05;  // u32
constexpr uint8_t kTlvFpsDen     = 0x06;  // u32
constexpr uint8_t kTlvSampleRate = 0x07;  // u32
constexpr uint8_t kTlvChannels   = 0x08;  // u32
constexpr uint8_t kTlvProfile    = 0x09;  // u32
constexpr uint8_t kTlvLevel      = 0x0A;  // u32

struct Header {
  uint32_t length = 0;   // bytes after the length field == kHeaderLengthBytes + payload size
  uint8_t  version = 0;
  uint8_t  type = 0;
  uint8_t  stream = 0;
  uint8_t  flags = 0;
  uint32_t sequence = 0;
  uint32_t generation = 0;
  uint64_t pts_us = 0;

  uint32_t payload_size() const { return length - kHeaderLengthBytes; }
};

void SerializeHeader(const Header &header, uint8_t out[kHeaderSize]);

// Returns false if the version is unsupported or the length field is impossible
// (shorter than the fixed header remainder or above the cap).
bool ParseHeader(const uint8_t in[kHeaderSize], Header &header);

// Decoded HELLO parameters. Zero means "not present on the wire" for every
// field except codec_id, which is required.
struct HelloInfo {
  uint32_t codec_id = kCodecIdNone;
  std::vector<uint8_t> extradata;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t fps_num = 0;
  uint32_t fps_den = 0;
  uint32_t sample_rate = 0;
  uint32_t channels = 0;
  uint32_t profile = 0;
  uint32_t level = 0;
};

// Builds the HELLO payload TLV list from plain codec parameters (FFmpeg-free).
// A field is emitted only when non-zero, which naturally yields video-only
// (width/height/fps) or audio-only (sample_rate/channels) parameter sets,
// matching the ZoneMinder producer's output for the same stream.
std::vector<uint8_t> BuildHello(uint32_t codec_id,
                                const uint8_t *extradata, size_t extradata_len,
                                uint32_t width, uint32_t height,
                                uint32_t fps_num, uint32_t fps_den,
                                uint32_t sample_rate, uint32_t channels);

// Parses a HELLO payload, skipping unknown tags. Returns false on a truncated
// TLV or a missing codec id.
bool ParseHello(const uint8_t *data, size_t len, HelloInfo &info);

// STATS payload: u64 messages_sent, u64 messages_dropped_for_this_client.
std::vector<uint8_t> BuildStats(uint64_t sent, uint64_t dropped);
bool ParseStats(const uint8_t *data, size_t len, uint64_t &sent, uint64_t &dropped);

// EVENT frame (MessageType::Event, StreamId::Monitor). Payload is a u16
// event_code followed by a TLV tail (u8 tag, u16 length, value), unknown tags
// skipped. Lifecycle codes (canonical):
constexpr uint16_t kEventSnapshot             = 0x0001;  // current health+state, on connect
constexpr uint16_t kEventConnectionFailed     = 0x0101;
constexpr uint16_t kEventConnectionRestored   = 0x0102;
constexpr uint16_t kEventPrimeCaptureFailed   = 0x0103;
constexpr uint16_t kEventPrimeCaptureRestored = 0x0104;
constexpr uint16_t kEventCaptureFailed        = 0x0105;
constexpr uint16_t kEventCaptureResumed       = 0x0106;
constexpr uint16_t kEventStateChanged         = 0x0201;
// zm-next analysis/AI extension codes (additive, reserved 0x03xx range):
constexpr uint16_t kEventDetection            = 0x0301;  // motion / object detection
constexpr uint16_t kEventDescription          = 0x0302;  // VLM scene description
constexpr uint16_t kEventRecordingSaved       = 0x0303;  // a clip was written to storage
constexpr uint16_t kEventRecordingOpening     = 0x0304;  // a clip segment opened; requests an event-id assignment

// EVENT TLV tags
constexpr uint8_t kTlvWallClockUs = 0x01;  // u64, unix-epoch microseconds
constexpr uint8_t kTlvMessage     = 0x02;  // utf8, human-readable detail
constexpr uint8_t kTlvStateId     = 0x03;  // u32, current monitor state
constexpr uint8_t kTlvPrevStateId = 0x04;  // u32, previous state (state_changed)
constexpr uint8_t kTlvDetail      = 0x05;  // u32, errno / ffmpeg error code
constexpr uint8_t kTlvStateName   = 0x06;  // utf8, "Idle"/"Alarm"/...
constexpr uint8_t kTlvHealthCode  = 0x07;  // u16, active fault code in a snapshot (0 = healthy)
// zm-next extension TLV: structured analysis/AI detail as a UTF-8 JSON document
// (detection object list, description text, recording metadata).
constexpr uint8_t kTlvJsonDetail  = 0x10;

// Decoded EVENT payload. A field is "present" only when its has_* flag is set
// (or its string is non-empty); the wire omits fields that do not apply.
struct MonitorEvent {
  uint16_t code = 0;
  uint64_t wall_clock_us = 0;  bool has_wall_clock = false;
  std::string message;
  uint32_t state_id = 0;       bool has_state_id = false;
  uint32_t prev_state_id = 0;  bool has_prev_state_id = false;
  uint32_t detail = 0;         bool has_detail = false;
  std::string state_name;
  uint16_t health_code = 0;    bool has_health_code = false;
  std::string json_detail;     // zm-next extension: structured analysis/AI JSON
};

// Builds an EVENT payload (u16 code + TLV tail) from the populated fields of ev.
// Header framing (sequence/generation/pts) is added by the caller.
std::vector<uint8_t> BuildEvent(const MonitorEvent &ev);

// Parses an EVENT payload, skipping unknown tags. Returns false on a truncated
// fixed code or TLV.
bool ParseEvent(const uint8_t *data, size_t len, MonitorEvent &ev);

}  // namespace stream_socket
}  // namespace zm

#endif  // ZM_STREAM_SOCKET_PROTOCOL_HPP

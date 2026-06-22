// Per-monitor media stream socket wire protocol (see header for provenance).
// Ported FFmpeg-free from ZoneMinder's zm_stream_socket_protocol and extended
// with zm-next analysis/AI EVENT codes + a JSON detail TLV.

#include "zm/stream_socket_protocol.hpp"

namespace zm {
namespace stream_socket {

namespace {

void put_u16(uint8_t *out, uint16_t value) {
  out[0] = value & 0xff;
  out[1] = (value >> 8) & 0xff;
}

void put_u32(uint8_t *out, uint32_t value) {
  out[0] = value & 0xff;
  out[1] = (value >> 8) & 0xff;
  out[2] = (value >> 16) & 0xff;
  out[3] = (value >> 24) & 0xff;
}

void put_u64(uint8_t *out, uint64_t value) {
  put_u32(out, static_cast<uint32_t>(value & 0xffffffff));
  put_u32(out + 4, static_cast<uint32_t>(value >> 32));
}

uint16_t get_u16(const uint8_t *in) {
  return static_cast<uint16_t>(in[0]) | (static_cast<uint16_t>(in[1]) << 8);
}

uint32_t get_u32(const uint8_t *in) {
  return static_cast<uint32_t>(in[0]) |
         (static_cast<uint32_t>(in[1]) << 8) |
         (static_cast<uint32_t>(in[2]) << 16) |
         (static_cast<uint32_t>(in[3]) << 24);
}

uint64_t get_u64(const uint8_t *in) {
  return static_cast<uint64_t>(get_u32(in)) |
         (static_cast<uint64_t>(get_u32(in + 4)) << 32);
}

void append_tlv(std::vector<uint8_t> &out, uint8_t tag, const uint8_t *value, uint16_t len) {
  out.push_back(tag);
  uint8_t len_le[2];
  put_u16(len_le, len);
  out.insert(out.end(), len_le, len_le + 2);
  out.insert(out.end(), value, value + len);
}

void append_tlv_u32(std::vector<uint8_t> &out, uint8_t tag, uint32_t value) {
  uint8_t value_le[4];
  put_u32(value_le, value);
  append_tlv(out, tag, value_le, sizeof(value_le));
}

void append_tlv_u64(std::vector<uint8_t> &out, uint8_t tag, uint64_t value) {
  uint8_t value_le[8];
  put_u64(value_le, value);
  append_tlv(out, tag, value_le, sizeof(value_le));
}

void append_tlv_str(std::vector<uint8_t> &out, uint8_t tag, const std::string &value) {
  // TLV length is u16; clamp pathologically long strings rather than overflow.
  uint16_t len = value.size() > 0xffff ? 0xffff : static_cast<uint16_t>(value.size());
  append_tlv(out, tag, reinterpret_cast<const uint8_t *>(value.data()), len);
}

}  // namespace

void SerializeHeader(const Header &header, uint8_t out[kHeaderSize]) {
  put_u32(out, header.length);
  out[4] = header.version;
  out[5] = header.type;
  out[6] = header.stream;
  out[7] = header.flags;
  put_u32(out + 8, header.sequence);
  put_u32(out + 12, header.generation);
  put_u64(out + 16, header.pts_us);
}

bool ParseHeader(const uint8_t in[kHeaderSize], Header &header) {
  header.length = get_u32(in);
  header.version = in[4];
  header.type = in[5];
  header.stream = in[6];
  header.flags = in[7];
  header.sequence = get_u32(in + 8);
  header.generation = get_u32(in + 12);
  header.pts_us = get_u64(in + 16);

  if (header.version != kProtocolVersion)
    return false;
  if (header.length < kHeaderLengthBytes || header.length > kMaxMessageLength)
    return false;
  return true;
}

std::vector<uint8_t> BuildHello(uint32_t codec_id,
                                const uint8_t *extradata, size_t extradata_len,
                                uint32_t width, uint32_t height,
                                uint32_t fps_num, uint32_t fps_den,
                                uint32_t sample_rate, uint32_t channels) {
  std::vector<uint8_t> out;
  out.reserve(64 + extradata_len);

  append_tlv_u32(out, kTlvCodecId, codec_id);
  if (extradata && extradata_len > 0) {
    uint16_t len = extradata_len > 0xffff ? 0xffff : static_cast<uint16_t>(extradata_len);
    append_tlv(out, kTlvExtradata, extradata, len);
  }
  if (width > 0) append_tlv_u32(out, kTlvWidth, width);
  if (height > 0) append_tlv_u32(out, kTlvHeight, height);
  if (fps_num > 0 && fps_den > 0) {
    append_tlv_u32(out, kTlvFpsNum, fps_num);
    append_tlv_u32(out, kTlvFpsDen, fps_den);
  }
  if (sample_rate > 0) append_tlv_u32(out, kTlvSampleRate, sample_rate);
  if (channels > 0) append_tlv_u32(out, kTlvChannels, channels);

  return out;
}

bool ParseHello(const uint8_t *data, size_t len, HelloInfo &info) {
  info = HelloInfo();
  size_t pos = 0;
  while (pos < len) {
    if (len - pos < 3)
      return false;
    uint8_t tag = data[pos];
    uint16_t value_len = get_u16(data + pos + 1);
    pos += 3;
    if (len - pos < value_len)
      return false;
    const uint8_t *value = data + pos;
    pos += value_len;

    bool known_numeric = tag >= kTlvCodecId && tag <= kTlvLevel && tag != kTlvExtradata;
    if (known_numeric && value_len != 4)
      return false;

    switch (tag) {
      case kTlvCodecId:    info.codec_id = get_u32(value); break;
      case kTlvExtradata:  info.extradata.assign(value, value + value_len); break;
      case kTlvWidth:      info.width = get_u32(value); break;
      case kTlvHeight:     info.height = get_u32(value); break;
      case kTlvFpsNum:     info.fps_num = get_u32(value); break;
      case kTlvFpsDen:     info.fps_den = get_u32(value); break;
      case kTlvSampleRate: info.sample_rate = get_u32(value); break;
      case kTlvChannels:   info.channels = get_u32(value); break;
      case kTlvProfile:    info.profile = get_u32(value); break;
      case kTlvLevel:      info.level = get_u32(value); break;
      default: break;  // unknown tag: skip
    }
  }
  return info.codec_id != kCodecIdNone;
}

std::vector<uint8_t> BuildStats(uint64_t sent, uint64_t dropped) {
  std::vector<uint8_t> out(16);
  put_u64(out.data(), sent);
  put_u64(out.data() + 8, dropped);
  return out;
}

bool ParseStats(const uint8_t *data, size_t len, uint64_t &sent, uint64_t &dropped) {
  if (len < 16)
    return false;
  sent = get_u64(data);
  dropped = get_u64(data + 8);
  return true;
}

std::vector<uint8_t> BuildEvent(const MonitorEvent &ev) {
  std::vector<uint8_t> out;
  out.reserve(16 + ev.message.size() + ev.state_name.size() + ev.json_detail.size());

  uint8_t code_le[2];
  put_u16(code_le, ev.code);
  out.insert(out.end(), code_le, code_le + 2);

  if (ev.has_wall_clock)      append_tlv_u64(out, kTlvWallClockUs, ev.wall_clock_us);
  if (!ev.message.empty())    append_tlv_str(out, kTlvMessage, ev.message);
  if (ev.has_state_id)        append_tlv_u32(out, kTlvStateId, ev.state_id);
  if (ev.has_prev_state_id)   append_tlv_u32(out, kTlvPrevStateId, ev.prev_state_id);
  if (ev.has_detail)          append_tlv_u32(out, kTlvDetail, ev.detail);
  if (!ev.state_name.empty()) append_tlv_str(out, kTlvStateName, ev.state_name);
  if (ev.has_health_code) {
    uint8_t hc_le[2];
    put_u16(hc_le, ev.health_code);
    append_tlv(out, kTlvHealthCode, hc_le, sizeof(hc_le));
  }
  if (!ev.json_detail.empty()) append_tlv_str(out, kTlvJsonDetail, ev.json_detail);

  return out;
}

bool ParseEvent(const uint8_t *data, size_t len, MonitorEvent &ev) {
  ev = MonitorEvent();
  if (len < 2)
    return false;
  ev.code = get_u16(data);

  size_t pos = 2;
  while (pos < len) {
    if (len - pos < 3)
      return false;
    uint8_t tag = data[pos];
    uint16_t value_len = get_u16(data + pos + 1);
    pos += 3;
    if (len - pos < value_len)
      return false;
    const uint8_t *value = data + pos;
    pos += value_len;

    switch (tag) {
      case kTlvWallClockUs:
        if (value_len != 8) return false;
        ev.wall_clock_us = get_u64(value);
        ev.has_wall_clock = true;
        break;
      case kTlvMessage:
        ev.message.assign(reinterpret_cast<const char *>(value), value_len);
        break;
      case kTlvStateId:
        if (value_len != 4) return false;
        ev.state_id = get_u32(value);
        ev.has_state_id = true;
        break;
      case kTlvPrevStateId:
        if (value_len != 4) return false;
        ev.prev_state_id = get_u32(value);
        ev.has_prev_state_id = true;
        break;
      case kTlvDetail:
        if (value_len != 4) return false;
        ev.detail = get_u32(value);
        ev.has_detail = true;
        break;
      case kTlvStateName:
        ev.state_name.assign(reinterpret_cast<const char *>(value), value_len);
        break;
      case kTlvHealthCode:
        if (value_len != 2) return false;
        ev.health_code = get_u16(value);
        ev.has_health_code = true;
        break;
      case kTlvJsonDetail:
        ev.json_detail.assign(reinterpret_cast<const char *>(value), value_len);
        break;
      default:
        break;  // unknown tag: skip
    }
  }
  return true;
}

}  // namespace stream_socket
}  // namespace zm

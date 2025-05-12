
#include <zm_plugin.h>
#include <nlohmann/json.hpp>
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/time.h>
#include <libavutil/avutil.h>
#include <libavcodec/avcodec.h>
}
#include <filesystem>
#include <fstream>
#include <chrono>
#include <ctime>
#include <string>
#include <vector>
#include <memory>
#include <mutex>

using json = nlohmann::json;
namespace fs = std::filesystem;

struct StoreInstance {
    std::string root;
    int monitor_id = 0;
    int max_secs = 300;
    int flags = 0;
    int hw_encode = 0;
    std::string cur_dir;
    std::string cur_path;
    std::unique_ptr<AVFormatContext, decltype(&avformat_free_context)> fmt_ctx{nullptr, avformat_free_context};
    int64_t start_ts = 0;
    int64_t last_pts = 0;
    bool file_open = false;
    bool warned_gpu = false;
    std::mutex mtx;
    bool header_written = false;
    AVStream* video_stream = nullptr;
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
    std::vector<std::vector<uint8_t>> pending_packets;
    zm_host_api_t* host = nullptr;
    void* host_ctx = nullptr;
};

static std::string get_default_root() {
#ifdef __APPLE__
    return "/Shared/zm/media";
#elif defined(_WIN32)
    return "C:/ZM/media";
#else
    return "/lib/zm/media";
#endif
}

static std::string make_path(const std::string& root, int monitor_id, std::time_t t) {
    std::tm tm = *std::localtime(&t);
    char buf[128];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d/Monitor-%d/%H-%M-%S.mkv", &tm);
    std::string path = root + "/" + buf;
    size_t pos = path.find("%d");
    if (pos != std::string::npos) path.replace(pos, 2, std::to_string(monitor_id));
    return path;
}

static void log(StoreInstance* inst, zm_log_level_t level, const char* fmt, ...) {
    if (!inst || !inst->host || !inst->host->log) return;
    va_list ap;
    va_start(ap, fmt);
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    inst->host->log(inst->host_ctx, level, buf);
}

static void close_file(StoreInstance* inst) {
    if (!inst->file_open) return;
    std::lock_guard<std::mutex> lock(inst->mtx);
    av_write_trailer(inst->fmt_ctx.get());
    avio_closep(&inst->fmt_ctx->pb);
    int64_t duration = inst->last_pts - inst->start_ts;
    json ev = { {"path", inst->cur_path}, {"duration", duration} };
    if (inst->host && inst->host->publish_evt)
        inst->host->publish_evt(inst->host_ctx, ev.dump().c_str());
    log(inst, ZM_LOG_INFO, "Closed file: %s (duration=%ld)", inst->cur_path.c_str(), duration);
    inst->fmt_ctx.reset();
    inst->file_open = false;
}

static bool open_file(StoreInstance* inst, std::time_t t) {
    inst->cur_path = make_path(inst->root, inst->monitor_id, t);
    fs::create_directories(fs::path(inst->cur_path).parent_path());
    AVFormatContext* ctx = nullptr;
    if (avformat_alloc_output_context2(&ctx, nullptr, "matroska", inst->cur_path.c_str()) < 0 || !ctx) {
        log(inst, ZM_LOG_ERROR, "Failed to alloc output context for %s", inst->cur_path.c_str());
        return false;
    }
    inst->fmt_ctx.reset(ctx);
    if (avio_open(&ctx->pb, inst->cur_path.c_str(), AVIO_FLAG_WRITE) < 0) {
        log(inst, ZM_LOG_ERROR, "Failed to open file %s", inst->cur_path.c_str());
        return false;
    }
    inst->start_ts = 0;
    inst->last_pts = 0;
    inst->file_open = true;
    inst->warned_gpu = false;
    inst->header_written = false;
    inst->video_stream = nullptr;
    log(inst, ZM_LOG_INFO, "Opened file: %s", inst->cur_path.c_str());
    return true;
}

extern "C" __attribute__((visibility("default"))) void zm_plugin_init(zm_plugin_t* plug) {
    plug->version = 1;
    plug->type = ZM_PLUGIN_STORE;
    plug->instance = nullptr;
    plug->start = [](zm_plugin_t* plugin, zm_host_api_t* host, void* host_ctx, const char* json_cfg) -> int {
        auto inst = new StoreInstance;
        inst->host = host;
        inst->host_ctx = host_ctx;
        try {
            auto j = json::parse(json_cfg);
            inst->root = j.value("root", get_default_root());
            inst->max_secs = j.value("max_secs", 300);
            inst->flags = j.value("flags", 0);
            inst->hw_encode = j.value("hw_encode", 0);
            inst->monitor_id = j.value("monitor_id", 0);
        } catch (...) {
            log(inst, ZM_LOG_ERROR, "Invalid config JSON");
            delete inst;
            return -1;
        }
        std::time_t now = std::time(nullptr);
        if (!open_file(inst, now)) {
            delete inst;
            return -1;
        }
        plugin->instance = inst;
        return 0;
    };
    // Standardized single-buffer on_frame: buf = [zm_frame_hdr_t][payload]
    plug->on_frame = [](zm_plugin_t* plugin, const void* buf, size_t size) {
        auto inst = static_cast<StoreInstance*>(plugin->instance);
        log(inst, ZM_LOG_DEBUG, "on_frame: inst=%p, buf=%p, size=%zu", inst, buf, size);
        if (!inst) return;
        if (!buf || size < sizeof(zm_frame_hdr_t)) {
            log(inst, ZM_LOG_DEBUG, "on_frame: buffer too small or null (size=%zu)", size);
            return;
        }
        const zm_frame_hdr_t* hdr = (const zm_frame_hdr_t*)buf;
        log(inst, ZM_LOG_DEBUG, "on_frame: stream_id=%u, bytes=%u, pts_usec=%" PRId64 ", flags=0x%x, hw_type=%d", hdr->stream_id, hdr->bytes, hdr->pts_usec, hdr->flags, hdr->hw_type);
        if (hdr->hw_type != ZM_HW_CPU) {
            if (!inst->warned_gpu) {
                log(inst, ZM_LOG_WARN, "Skipping GPU frame");
                inst->warned_gpu = true;
            }
            return;
        }
        if (size < sizeof(zm_frame_hdr_t) + hdr->bytes) {
            log(inst, ZM_LOG_ERROR, "Frame buffer too small: got %zu, need %zu", size, sizeof(zm_frame_hdr_t) + (size_t)hdr->bytes);
            return;
        }
        const uint8_t* payload = (const uint8_t*)buf + sizeof(zm_frame_hdr_t);
        std::lock_guard<std::mutex> lock(inst->mtx);
        if (!inst->file_open) {
            log(inst, ZM_LOG_DEBUG, "on_frame: file not open, skipping");
            return;
        }

        // Helper: extract SPS/PPS from H264 bytestream (handles 3- and 4-byte start codes)
        auto extract_sps_pps = [](const uint8_t* data, size_t len, std::vector<uint8_t>& sps, std::vector<uint8_t>& pps, StoreInstance* inst) {
            size_t i = 0;
            while (i + 3 < len) {
                size_t start = 0;
                if (data[i] == 0 && data[i+1] == 0 && data[i+2] == 1) {
                    start = 3;
                } else if (i + 4 < len && data[i] == 0 && data[i+1] == 0 && data[i+2] == 0 && data[i+3] == 1) {
                    start = 4;
                } else {
                    ++i;
                    continue;
                }
                size_t nal_start = i + start;
                if (nal_start >= len) break;
                uint8_t nal_type = data[nal_start] & 0x1F;
                size_t nal_end = nal_start;
                size_t j = nal_start;
                while (j + 3 < len) {
                    if ((data[j] == 0 && data[j+1] == 0 && data[j+2] == 1) ||
                        (j + 4 < len && data[j] == 0 && data[j+1] == 0 && data[j+2] == 0 && data[j+3] == 1)) {
                        break;
                    }
                    ++j;
                }
                nal_end = (j < len) ? j : len;
                if (nal_type == 7 && sps.empty()) {
                    sps.assign(data + nal_start, data + nal_end);
                    log(inst, ZM_LOG_INFO, "Detected SPS (len=%zu)", sps.size());
                }
                if (nal_type == 8 && pps.empty()) {
                    pps.assign(data + nal_start, data + nal_end);
                    log(inst, ZM_LOG_INFO, "Detected PPS (len=%zu)", pps.size());
                }
                i = nal_end;
            }
        };

        // If header not written, try to extract SPS/PPS and buffer packets
        if (!inst->header_written) {
            extract_sps_pps(payload, hdr->bytes, inst->sps, inst->pps, inst);
            // Buffer this packet for later (up to 50 packets)
            if (inst->pending_packets.size() < 50)
                inst->pending_packets.emplace_back(payload, payload + hdr->bytes);
            // Only proceed if we have both SPS and PPS
            if (!inst->sps.empty() && !inst->pps.empty()) {
                AVFormatContext* ctx = inst->fmt_ctx.get();
                AVStream* st = avformat_new_stream(ctx, nullptr);
                if (!st) {
                    log(inst, ZM_LOG_ERROR, "Failed to create new stream");
                    return;
                }
                inst->video_stream = st;
                st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
                st->codecpar->codec_id = AV_CODEC_ID_H264;
                st->codecpar->format = 0;
                st->codecpar->width = hdr->stream_id;
                st->codecpar->height = hdr->flags;
                st->time_base = AVRational{1, 1000000};
                // Compose extradata (Annex B -> AVCC)
                std::vector<uint8_t> extradata;
                // AVCC header
                extradata.push_back(1); // version
                extradata.push_back(inst->sps[1]); // profile
                extradata.push_back(inst->sps[2]); // compat
                extradata.push_back(inst->sps[3]); // level
                extradata.push_back(0xFC | 3); // reserved (6 bits) + nalu length size - 1 (2 bits)
                extradata.push_back(0xE0 | 1); // reserved (3 bits) + num of SPS (5 bits)
                extradata.push_back((inst->sps.size() >> 8) & 0xFF);
                extradata.push_back(inst->sps.size() & 0xFF);
                extradata.insert(extradata.end(), inst->sps.begin(), inst->sps.end());
                extradata.push_back(1); // num of PPS
                extradata.push_back((inst->pps.size() >> 8) & 0xFF);
                extradata.push_back(inst->pps.size() & 0xFF);
                extradata.insert(extradata.end(), inst->pps.begin(), inst->pps.end());
                st->codecpar->extradata_size = extradata.size();
                st->codecpar->extradata = (uint8_t*)av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE);
                memcpy(st->codecpar->extradata, extradata.data(), extradata.size());
                int ret = avformat_write_header(ctx, nullptr);
                if (ret < 0) {
                    log(inst, ZM_LOG_ERROR, "Failed to write header for %s (ret=%d)", inst->cur_path.c_str(), ret);
                    return;
                }
                inst->header_written = true;
                log(inst, ZM_LOG_INFO, "Header written, writing %zu buffered packets", inst->pending_packets.size());
                // Write all buffered packets
                for (const auto& pktbuf : inst->pending_packets) {
                    AVPacket pkt;
                    av_init_packet(&pkt);
                    pkt.data = (uint8_t*)pktbuf.data();
                    pkt.size = pktbuf.size();
                    pkt.pts = hdr->pts_usec;
                    pkt.dts = hdr->pts_usec;
                    pkt.stream_index = 0;
                    pkt.flags = (hdr->flags & 1) ? AV_PKT_FLAG_KEY : 0;
                    if (inst->start_ts == 0) inst->start_ts = pkt.pts;
                    inst->last_pts = pkt.pts;
                    log(inst, ZM_LOG_DEBUG, "Writing buffered frame: pts=%ld, size=%d", (long)pkt.pts, pkt.size);
                    int ret2 = av_write_frame(inst->fmt_ctx.get(), &pkt);
                    if (ret2 < 0) {
                        log(inst, ZM_LOG_ERROR, "Failed to write frame (buffered), ret=%d", ret2);
                    }
                }
                inst->pending_packets.clear();
            }
            if (inst->pending_packets.size() >= 50) {
                log(inst, ZM_LOG_ERROR, "SPS/PPS not found after 50 packets, giving up on this segment");
                inst->pending_packets.clear();
            }
            return;
        }

        // Normal write after header
        AVPacket pkt;
        av_init_packet(&pkt);
        pkt.data = (uint8_t*)payload;
        pkt.size = hdr->bytes;
        pkt.pts = hdr->pts_usec;
        pkt.dts = hdr->pts_usec;
        pkt.stream_index = 0;
        pkt.flags = (hdr->flags & 1) ? AV_PKT_FLAG_KEY : 0;
        if (inst->start_ts == 0) inst->start_ts = pkt.pts;
        inst->last_pts = pkt.pts;
        log(inst, ZM_LOG_DEBUG, "Writing frame: pts=%ld, size=%d", (long)pkt.pts, pkt.size);
        int ret3 = av_write_frame(inst->fmt_ctx.get(), &pkt);
        if (ret3 < 0) {
            log(inst, ZM_LOG_ERROR, "Failed to write frame, ret=%d", ret3);
        }
        int64_t elapsed = (pkt.pts - inst->start_ts) / 1000000;
        std::time_t now = std::time(nullptr);
        std::tm tm = *std::localtime(&now);
        if (elapsed >= inst->max_secs || (tm.tm_hour == 0 && tm.tm_min == 0 && tm.tm_sec < 2)) {
            log(inst, ZM_LOG_INFO, "Segment duration reached or midnight, closing and opening new file");
            close_file(inst);
            open_file(inst, now);
        }
    };
    plug->stop = [](zm_plugin_t* plugin) {
        auto inst = static_cast<StoreInstance*>(plugin->instance);
        if (inst) {
            close_file(inst);
            delete inst;
            plugin->instance = nullptr;
        }
    };
}


#include <zm_plugin.h>
#include <nlohmann/json.hpp>
#ifdef __cplusplus
extern "C" {
#endif
#include <libavformat/avformat.h>
#include <libavutil/time.h>
#include <libavutil/avutil.h>
#include <libavcodec/avcodec.h>
#include <libavutil/base64.h>
#ifdef __cplusplus
}
#endif
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
    // metadata from input plugin
    AVCodecParameters* metadata_codecpar = nullptr;
    // buffer raw packets until header is written
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
        if (!inst || !buf) return;
        // Detect JSON metadata event (buffer starts with '{')
        if (size > 0 && static_cast<const char*>(buf)[0] == '{') {
            try {
                std::string js(static_cast<const char*>(buf), size);
                auto j = json::parse(js);
                if (j.value("event", "") == "StreamMetadata") {
                    if (inst->metadata_codecpar)
                        avcodec_parameters_free(&inst->metadata_codecpar);
                    inst->metadata_codecpar = avcodec_parameters_alloc();
                    inst->metadata_codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
                    inst->metadata_codecpar->codec_id   = j["codec_id"];
                    inst->metadata_codecpar->width      = j["width"];
                    inst->metadata_codecpar->height     = j["height"];
                    inst->metadata_codecpar->format     = j["pix_fmt"];
                    inst->metadata_codecpar->profile    = j["profile"];
                    inst->metadata_codecpar->level      = j["level"];
                    auto ed_b64 = j["extradata"].get<std::string>();
                    int ed_len = av_base64_decode(nullptr, ed_b64.c_str(), 0);
                    if (ed_len > 0) {
                        std::vector<uint8_t> ed(ed_len);
                        av_base64_decode(ed.data(), ed_b64.c_str(), ed_len);
                        inst->metadata_codecpar->extradata = (uint8_t*)av_mallocz(ed_len + AV_INPUT_BUFFER_PADDING_SIZE);
                        if (inst->metadata_codecpar->extradata) {
                            memcpy(inst->metadata_codecpar->extradata, ed.data(), ed_len);
                            // Padding is already zeroed by av_mallocz
                            inst->metadata_codecpar->extradata_size = ed_len;
                        } else {
                            inst->metadata_codecpar->extradata_size = 0;
                        }
                    } else {
                        inst->metadata_codecpar->extradata = nullptr;
                        inst->metadata_codecpar->extradata_size = 0;
                    }
                    log(inst, ZM_LOG_INFO, "Received StreamMetadata, codec %d %dx%d",
                        inst->metadata_codecpar->codec_id,
                        inst->metadata_codecpar->width,
                        inst->metadata_codecpar->height);
                }
            } catch (...) {
                log(inst, ZM_LOG_WARN, "Failed to parse JSON event in on_frame");
            }
            return;
        }
        // Must have a full frame header
        if (size < sizeof(zm_frame_hdr_t)) {
            log(inst, ZM_LOG_ERROR, "on_frame: invalid data size %zu", size);
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

        // If header not written but we have metadata, initialize stream and write header
        if (!inst->header_written && inst->metadata_codecpar) {
            // H.264 requires extradata (SPS/PPS) for muxing
            if (inst->metadata_codecpar->codec_id == AV_CODEC_ID_H264 &&
                (inst->metadata_codecpar->extradata == nullptr || inst->metadata_codecpar->extradata_size == 0)) {
                log(inst, ZM_LOG_ERROR, "H.264 extradata (SPS/PPS) missing or empty! Cannot write header.");
                return;
            }
            // Fix deprecated YUVJ420P to YUV420P for muxer compatibility
            if (inst->metadata_codecpar->format == AV_PIX_FMT_YUVJ420P) {
                log(inst, ZM_LOG_WARN, "YUVJ420P is deprecated, forcing to YUV420P for muxer");
                inst->metadata_codecpar->format = AV_PIX_FMT_YUV420P;
            }
            // Diagnostic: log all AVCodecParameters fields before copying
            log(inst, ZM_LOG_DEBUG,
                "metadata_codecpar: codec_type=%d, codec_id=%d, width=%d, height=%d, format=%d, profile=%d, level=%d, extradata_size=%d, extradata_ptr=%p",
                inst->metadata_codecpar->codec_type,
                inst->metadata_codecpar->codec_id,
                inst->metadata_codecpar->width,
                inst->metadata_codecpar->height,
                inst->metadata_codecpar->format,
                inst->metadata_codecpar->profile,
                inst->metadata_codecpar->level,
                inst->metadata_codecpar->extradata_size,
                inst->metadata_codecpar->extradata);
            AVFormatContext* oc = inst->fmt_ctx.get();
            // find encoder for the stream codec, to initialize muxer correctly
            const AVCodec* enc = avcodec_find_encoder(inst->metadata_codecpar->codec_id);
            if (!enc) {
                log(inst, ZM_LOG_WARN, "No encoder found for codec id %d, using default stream creation", inst->metadata_codecpar->codec_id);
            }
            AVStream* st = avformat_new_stream(oc, enc);
            if (!st) {
                log(inst, ZM_LOG_ERROR, "Failed to create new stream for metadata");
                return;
            }
            inst->video_stream = st;
            // copy full codec parameters from metadata
            avcodec_parameters_copy(st->codecpar, inst->metadata_codecpar);
            // ensure video stream type
            st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
            st->time_base = AVRational{1, 1000000};
            oc->streams[st->index]->time_base = st->time_base;
            // diagnostic: log codec parameters before header
            log(inst, ZM_LOG_DEBUG,
                "Writing header: codec_id=%d, width=%d, height=%d, pix_fmt=%d, extradata_size=%d, extradata_ptr=%p",
                st->codecpar->codec_id,
                st->codecpar->width,
                st->codecpar->height,
                st->codecpar->format,
                st->codecpar->extradata_size,
                st->codecpar->extradata);
            int ret = avformat_write_header(oc, nullptr);
            if (ret < 0) {
                log(inst, ZM_LOG_ERROR, "Failed to write header for %s (ret=%d)", inst->cur_path.c_str(), ret);
                return;
            }
            inst->header_written = true;
            // Prevent double-free: clear extradata pointer before freeing metadata_codecpar
            if (inst->metadata_codecpar) {
                inst->metadata_codecpar->extradata = nullptr;
                inst->metadata_codecpar->extradata_size = 0;
            }
            avcodec_parameters_free(&inst->metadata_codecpar);
            log(inst, ZM_LOG_INFO, "Header written via metadata, dropping %zu buffered packets", inst->pending_packets.size());
            // drop buffered packets (no timestamps available)
            inst->pending_packets.clear();
            return;
        }

        // Normal write after header
        // write packet using stack AVPacket
        {
            AVPacket pkt;
            av_init_packet(&pkt);
            pkt.data = (uint8_t*)payload;
            pkt.size = hdr->bytes;
            pkt.pts = hdr->pts_usec;
            pkt.dts = hdr->pts_usec;
            pkt.stream_index = inst->video_stream ? inst->video_stream->index : 0;
            pkt.flags = (hdr->flags & 1) ? AV_PKT_FLAG_KEY : 0;
            if (inst->start_ts == 0) inst->start_ts = pkt.pts;
            inst->last_pts = pkt.pts;
            log(inst, ZM_LOG_DEBUG, "Writing frame: pts=%ld, size=%d, stream=%d", (long)pkt.pts, pkt.size, pkt.stream_index);
            int ret3 = av_interleaved_write_frame(inst->fmt_ctx.get(), &pkt);
            if (ret3 < 0) {
                log(inst, ZM_LOG_ERROR, "Failed to write frame, ret=%d", ret3);
            }
        }
        // compute elapsed based on recorded timestamps
        int64_t elapsed = (inst->last_pts - inst->start_ts) / 1000000;
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

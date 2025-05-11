
#include <zm_plugin.h>
#include <nlohmann/json.hpp>
#include <libavformat/avformat.h>
#include <libavutil/time.h>
#include <libavutil/avutil.h>
#include <libavcodec/avcodec.h>
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
    plug->on_frame = [](zm_plugin_t* plugin, const zm_frame_hdr_t* hdr, const void* payload) {
        auto inst = static_cast<StoreInstance*>(plugin->instance);
        if (!inst || !hdr) return;
        if (hdr->hw_type != ZM_HW_CPU) {
            if (!inst->warned_gpu) {
                log(inst, ZM_LOG_WARN, "Skipping GPU frame");
                inst->warned_gpu = true;
            }
            return;
        }
        std::lock_guard<std::mutex> lock(inst->mtx);
        if (!inst->file_open) return;
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
        if (av_write_frame(inst->fmt_ctx.get(), &pkt) < 0) {
            log(inst, ZM_LOG_ERROR, "Failed to write frame");
        }
        int64_t elapsed = (pkt.pts - inst->start_ts) / 1000000;
        std::time_t now = std::time(nullptr);
        std::tm tm = *std::localtime(&now);
        if (elapsed >= inst->max_secs || (tm.tm_hour == 0 && tm.tm_min == 0 && tm.tm_sec < 2)) {
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

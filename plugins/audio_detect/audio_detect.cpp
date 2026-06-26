// audio_detect: ONNX audio-event classifier plugin (ONNX Runtime C++ API).
//
// Classifies the audio stream into events (glass break, scream, dog bark,
// alarm, gunshot, ...) using a YAMNet-style ONNX waveform classifier.
//
// Receives COMPRESSED AUDIO frames (hw_type == ZM_FRAME_COMPRESSED) on the
// configured `audio_stream_id`, decodes them with FFmpeg, resamples to mono
// FLT PCM at `sample_rate`, accumulates a rolling sample buffer, and runs the
// ONNX model over sliding windows of `window_sec` advanced by `hop_sec`.
//
// This is a pass-through DETECT stage: every frame is forwarded downstream via
// host->on_frame at the end of on_frame, regardless of whether it was audio.
//
// NOTE: the pipeline today carries only video; audio frames will be delivered
// by a later capture change. Until then this plugin sees no frames matching
// `audio_stream_id` and simply forwards everything (a no-op classifier) — that
// is expected and correct.

#include "audio_topk.hpp"
#include "logmel.hpp"

#include <onnxruntime_cxx_api.h>

#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "zm_plugin.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

using json = nlohmann::json;

namespace {

struct AudioDetectCtx {
    zm_host_api_t* host = nullptr;
    void* hostCtx = nullptr;

    // Config.
    int audioStreamId = 1;
    std::string codec = "aac";       // fallback codec if not auto-detected
    bool autoCodec = true;           // auto-detect codec from audio StreamMetadata
    std::string modelPath;
    int sampleRate = 16000;
    double windowSec = 1.0;
    double hopSec = 0.5;
    float confThreshold = 0.4f;
    int topK = 3;
    std::vector<std::string> labels; // class index -> label; empty -> class_<id>

    // Input front-end: "waveform" (YAMNet-style, raw samples) or "logmel"
    // (CED / EfficientAT — a log-mel spectrogram). For logmel, `mel` is built in
    // start() from the mel_* config and runWindow() feeds [.., n_mels, n_frames].
    bool useLogMel = false;
    zm::audio::MelConfig melCfg;
    std::unique_ptr<zm::audio::MelExtractor> mel;

    // Derived window sizes (samples).
    size_t windowSamples = 0;
    size_t hopSamples = 0;

    // Auto-detect state (host-event subscription); meta leaked on stop.
    struct AudioMeta* meta = nullptr;
    void* metaSub = nullptr;

    // FFmpeg decode + resample state.
    const AVCodec* avCodec = nullptr;
    AVCodecContext* codecCtx = nullptr;
    SwrContext* swr = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    int swrInRate = 0;          // last configured input rate (re-init on change)
    AVSampleFormat swrInFmt = AV_SAMPLE_FMT_NONE;
    int swrInChannels = 0;

    // ONNX Runtime state.
    std::unique_ptr<Ort::Env> env;
    Ort::SessionOptions sessionOptions;
    std::unique_ptr<Ort::Session> session;
    std::string inputName;
    std::string outputName;
    int64_t inputRank = 0;      // 1 -> [N], 2 -> [1, N]

    // Rolling mono float PCM buffer at sampleRate.
    std::vector<float> samples;

    bool warnedRun = false;
};

const char* labelName(const AudioDetectCtx* ctx, int id, std::string& scratch) {
    if (id >= 0 && id < static_cast<int>(ctx->labels.size())) {
        return ctx->labels[id].c_str();
    }
    scratch = "class_" + std::to_string(id);
    return scratch.c_str();
}

void forwardFrame(AudioDetectCtx* ctx, const void* buf, size_t size) {
    if (ctx && ctx->host && ctx->host->on_frame) {
        ctx->host->on_frame(ctx->hostCtx, buf, size);
    }
}

// Find an FFmpeg decoder for the configured codec string. Try by name first
// (handles "opus", "libopus", "pcm_s16le", etc.) then fall back to a couple of
// well-known codec IDs.
const AVCodec* findAudioDecoder(const std::string& name) {
    const AVCodec* c = avcodec_find_decoder_by_name(name.c_str());
    if (c) return c;
    if (name == "aac")  return avcodec_find_decoder(AV_CODEC_ID_AAC);
    if (name == "opus") return avcodec_find_decoder(AV_CODEC_ID_OPUS);
    if (name == "mp3")  return avcodec_find_decoder(AV_CODEC_ID_MP3);
    if (name == "pcm" || name == "pcm_s16le")
        return avcodec_find_decoder(AV_CODEC_ID_PCM_S16LE);
    return nullptr;
}

// Auto-detect state shared with the host event callback. Leaked on stop so an
// in-flight callback is always safe.
struct AudioMeta {
    std::atomic<int> codec_id{(int)AV_CODEC_ID_NONE};
    std::atomic<bool> running{true};
};

// Host event callback: learn the audio codec id from the capture plugin's audio
// StreamMetadata event so the decoder is auto-detected (AAC/Opus/G.711/...).
static void audio_meta_cb(void* user, const char* json_event) {
    auto* m = static_cast<AudioMeta*>(user);
    if (!m || !m->running.load() || !json_event) return;
    try {
        auto j = json::parse(json_event);
        if (j.value("event", std::string()) != "StreamMetadata") return;
        if (j.value("media", std::string()) != "audio") return;
        m->codec_id.store(j.value("codec_id", (int)AV_CODEC_ID_NONE));
    } catch (...) {
        // ignore malformed events
    }
}

// Lazily create the audio decoder, preferring the auto-detected codec id, else
// the configured fallback. Returns true once a decoder is open.
static bool ensureAudioDecoder(AudioDetectCtx* ctx) {
    if (ctx->codecCtx) return true;
    const AVCodec* dec = nullptr;
    if (ctx->autoCodec && ctx->meta) {
        auto cid = (enum AVCodecID)ctx->meta->codec_id.load();
        if (cid != AV_CODEC_ID_NONE) dec = avcodec_find_decoder(cid);
    }
    if (!dec) dec = findAudioDecoder(ctx->codec);  // fallback to configured codec
    if (!dec) return false;                        // no metadata yet, no fallback

    ctx->avCodec = dec;
    ctx->codecCtx = avcodec_alloc_context3(dec);
    if (!ctx->codecCtx) return false;
    ctx->codecCtx->sample_rate = ctx->sampleRate;
    if (avcodec_open2(ctx->codecCtx, dec, nullptr) < 0) {
        avcodec_free_context(&ctx->codecCtx);
        ctx->codecCtx = nullptr;
        return false;
    }
    ZM_LOG_INFO("audio_detect: decoder ready codec=%s (%s)",
                dec->name ? dec->name : "?", ctx->autoCodec ? "auto" : "configured");
    if (ctx->host && ctx->host->unsubscribe_evt && ctx->metaSub) {
        ctx->host->unsubscribe_evt(ctx->hostCtx, ctx->metaSub);
        ctx->metaSub = nullptr;
        if (ctx->meta) ctx->meta->running.store(false);
    }
    return true;
}

// (Re)initialise the SwrContext to convert from the decoded frame's layout to
// mono / FLT / sampleRate. Returns true on success.
bool ensureSwr(AudioDetectCtx* ctx, const AVFrame* f) {
    int inRate = f->sample_rate > 0 ? f->sample_rate : ctx->codecCtx->sample_rate;
    AVSampleFormat inFmt = static_cast<AVSampleFormat>(f->format);
    int inChannels = f->ch_layout.nb_channels;
    if (inChannels <= 0) inChannels = 1;

    if (ctx->swr && inRate == ctx->swrInRate && inFmt == ctx->swrInFmt &&
        inChannels == ctx->swrInChannels) {
        return true; // already configured for this input shape
    }

    if (ctx->swr) {
        swr_free(&ctx->swr);
        ctx->swr = nullptr;
    }

    AVChannelLayout inLayout;
    av_channel_layout_default(&inLayout, inChannels);
    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, 1); // mono

    int ret = swr_alloc_set_opts2(&ctx->swr,
                                  &outLayout, AV_SAMPLE_FMT_FLT, ctx->sampleRate,
                                  &inLayout, inFmt, inRate,
                                  0, nullptr);
    av_channel_layout_uninit(&inLayout);
    av_channel_layout_uninit(&outLayout);

    if (ret < 0 || !ctx->swr) {
        ZM_LOG_ERROR("audio_detect: swr_alloc_set_opts2 failed (%d)", ret);
        return false;
    }
    if (swr_init(ctx->swr) < 0) {
        ZM_LOG_ERROR("audio_detect: swr_init failed");
        swr_free(&ctx->swr);
        ctx->swr = nullptr;
        return false;
    }
    ctx->swrInRate = inRate;
    ctx->swrInFmt = inFmt;
    ctx->swrInChannels = inChannels;
    return true;
}

// Resample one decoded AVFrame to mono FLT @ sampleRate and append to ctx->samples.
void appendResampled(AudioDetectCtx* ctx, const AVFrame* f) {
    if (!ensureSwr(ctx, f)) return;

    // Upper bound on output samples for this input frame.
    int64_t maxOut = swr_get_out_samples(ctx->swr, f->nb_samples);
    if (maxOut <= 0) return;

    size_t oldSize = ctx->samples.size();
    ctx->samples.resize(oldSize + static_cast<size_t>(maxOut));

    uint8_t* outPtr = reinterpret_cast<uint8_t*>(ctx->samples.data() + oldSize);
    const uint8_t** inData = const_cast<const uint8_t**>(f->extended_data);

    int produced = swr_convert(ctx->swr, &outPtr, static_cast<int>(maxOut),
                               inData, f->nb_samples);
    if (produced < 0) {
        ctx->samples.resize(oldSize);
        return;
    }
    ctx->samples.resize(oldSize + static_cast<size_t>(produced));
}

// Run the model over the front window of ctx->samples and emit an event.
void runWindow(AudioDetectCtx* ctx, const zm_frame_hdr_t* hdr) {
    if (!ctx->session || ctx->samples.size() < ctx->windowSamples) return;

    try {
        const size_t n = ctx->windowSamples;
        Ort::MemoryInfo memInfo =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        // Input tensor: either a raw waveform (YAMNet-style) or a log-mel
        // spectrogram (CED/EfficientAT). The mel buffer must outlive the Run().
        std::vector<int64_t> inputShape;
        std::vector<float> melBuf;
        const float* data = ctx->samples.data();
        size_t dataLen = n;

        if (ctx->useLogMel && ctx->mel) {
            int frames = 0;
            melBuf = ctx->mel->extract(ctx->samples.data(), n, frames);
            if (frames <= 0 || melBuf.empty()) return;
            const int64_t M = ctx->mel->n_mels();
            const int64_t F = frames;
            // Match the model's rank: [1,1,M,F] / [1,M,F] / [M,F].
            if (ctx->inputRank >= 4)      inputShape = {1, 1, M, F};
            else if (ctx->inputRank == 3) inputShape = {1, M, F};
            else                          inputShape = {M, F};
            data = melBuf.data();
            dataLen = melBuf.size();
        } else {
            // Raw waveform: [1, N] or [N].
            if (ctx->inputRank == 2) inputShape = {1, static_cast<int64_t>(n)};
            else                     inputShape = {static_cast<int64_t>(n)};
        }

        Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
            memInfo, const_cast<float*>(data), dataLen,
            inputShape.data(), inputShape.size());

        const char* inputNames[] = {ctx->inputName.c_str()};
        const char* outputNames[] = {ctx->outputName.c_str()};

        auto outputs = ctx->session->Run(Ort::RunOptions{nullptr}, inputNames,
                                         &inputTensor, 1, outputNames, 1);

        const float* out = outputs[0].GetTensorData<float>();
        auto shape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();

        // Number of classes is the last dim (handles [num_classes] and
        // [1, num_classes]).
        size_t numClasses = 0;
        if (!shape.empty() && shape.back() > 0) {
            numClasses = static_cast<size_t>(shape.back());
        }
        if (numClasses == 0) return;

        auto top = zm::audio::top_k_above_threshold(out, numClasses,
                                                    ctx->confThreshold, ctx->topK);
        if (!top.empty()) {
            json events = json::array();
            for (const auto& [idx, score] : top) {
                std::string scratch;
                json e;
                e["label"] = labelName(ctx, idx, scratch);
                e["confidence"] = score;
                events.push_back(std::move(e));
            }
            json evt;
            evt["type"] = "audio_event";
            evt["stream_id"] = hdr->stream_id;
            evt["pts_usec"] = hdr->pts_usec;
            evt["events"] = std::move(events);
            if (ctx->host && ctx->host->publish_evt)
                ctx->host->publish_evt(ctx->hostCtx, evt.dump().c_str());
        }
    } catch (const std::exception& e) {
        if (!ctx->warnedRun) {
            ZM_LOG_ERROR("audio_detect: inference error: %s", e.what());
            ctx->warnedRun = true;
        }
    }

    // Slide the window forward by hopSamples.
    size_t erase = ctx->hopSamples > 0 ? ctx->hopSamples : ctx->windowSamples;
    if (erase > ctx->samples.size()) erase = ctx->samples.size();
    ctx->samples.erase(ctx->samples.begin(),
                       ctx->samples.begin() + static_cast<std::ptrdiff_t>(erase));
}

} // namespace

extern "C" {

static int audio_detect_start(zm_plugin_t* plugin, zm_host_api_t* host,
                              void* host_ctx, const char* json_cfg) {
    auto* ctx = new AudioDetectCtx;
    ctx->host = host;
    ctx->hostCtx = host_ctx;
    zm_plugin_set_log_context(host, host_ctx);

    // Parse configuration (all keys optional).
    if (json_cfg) {
        try {
            auto j = json::parse(json_cfg);
            ctx->audioStreamId = j.value("audio_stream_id", -1);  // -1 = any audio stream
            // "codec" is an OPTIONAL override; without it the codec is auto-detected
            // from the capture plugin's audio StreamMetadata.
            if (j.contains("codec")) {
                ctx->codec = j["codec"].get<std::string>();
                ctx->autoCodec = false;
            }
            ctx->modelPath = j.value("model_path", std::string());
            ctx->sampleRate = j.value("sample_rate", 16000);
            ctx->windowSec = j.value("window_sec", 1.0);
            ctx->hopSec = j.value("hop_sec", 0.5);
            ctx->confThreshold = j.value("conf_threshold", 0.4f);
            ctx->topK = j.value("top_k", 3);
            if (j.contains("labels") && j["labels"].is_array())
                ctx->labels = j["labels"].get<std::vector<std::string>>();
            ctx->useLogMel = (j.value("input_type", std::string("waveform")) == "logmel");
            ctx->melCfg.n_fft = j.value("n_fft", 512);
            ctx->melCfg.hop_length = j.value("hop_length", 160);
            ctx->melCfg.n_mels = j.value("n_mels", 64);
            ctx->melCfg.fmin = j.value("fmin", 0.f);
            ctx->melCfg.fmax = j.value("fmax", 0.f);
            ctx->melCfg.log_offset = j.value("mel_log_offset", 1e-6f);
            ctx->melCfg.log10 = j.value("mel_log10", false);
            ctx->melCfg.slaney = j.value("mel_slaney", false);
        } catch (const std::exception& e) {
            ZM_LOG_ERROR("audio_detect: failed to parse config: %s", e.what());
        }
    }

    if (ctx->sampleRate <= 0) ctx->sampleRate = 16000;
    if (ctx->windowSec <= 0.0) ctx->windowSec = 1.0;
    if (ctx->hopSec <= 0.0) ctx->hopSec = ctx->windowSec;
    ctx->windowSamples =
        static_cast<size_t>(ctx->windowSec * ctx->sampleRate);
    ctx->hopSamples = static_cast<size_t>(ctx->hopSec * ctx->sampleRate);
    if (ctx->windowSamples == 0) ctx->windowSamples = 1;

    // Build the log-mel front-end (CED/EfficientAT). Uses the model's sample rate.
    if (ctx->useLogMel) {
        ctx->melCfg.sample_rate = ctx->sampleRate;
        ctx->mel = std::make_unique<zm::audio::MelExtractor>(ctx->melCfg);
        if (!ctx->mel->valid()) {
            ZM_LOG_ERROR("audio_detect: invalid log-mel config (n_fft must be power of 2); "
                         "falling back to waveform input");
            ctx->useLogMel = false;
            ctx->mel.reset();
        } else {
            ZM_LOG_INFO("audio_detect: log-mel front-end (n_fft=%d hop=%d n_mels=%d sr=%d)",
                        ctx->melCfg.n_fft, ctx->melCfg.hop_length, ctx->melCfg.n_mels,
                        ctx->sampleRate);
        }
    }

    // The audio decoder is created lazily (in on_frame) once the codec is known —
    // auto-detected from the audio StreamMetadata, or the configured fallback.
    // Subscribe via the host to learn the codec id.
    ctx->meta = new AudioMeta();
    if (host && host->subscribe_evt)
        ctx->metaSub = host->subscribe_evt(host_ctx, &audio_meta_cb, ctx->meta);

    ctx->pkt = av_packet_alloc();
    ctx->frame = av_frame_alloc();

    // Create the ONNX Runtime environment and session options.
    try {
        ctx->env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING,
                                              "audio_detect");
        ctx->sessionOptions.SetIntraOpNumThreads(1);
        ctx->sessionOptions.SetGraphOptimizationLevel(ORT_ENABLE_ALL);
    } catch (const std::exception& e) {
        ZM_LOG_ERROR("audio_detect: failed to init ORT env: %s", e.what());
    }

    // Construct the session if a model path was given.
    if (!ctx->modelPath.empty() && ctx->env) {
        try {
            ctx->session = std::make_unique<Ort::Session>(
                *ctx->env, ctx->modelPath.c_str(), ctx->sessionOptions);

            Ort::AllocatorWithDefaultOptions allocator;
            auto inName = ctx->session->GetInputNameAllocated(0, allocator);
            auto outName = ctx->session->GetOutputNameAllocated(0, allocator);
            ctx->inputName = inName.get();
            ctx->outputName = outName.get();

            // Determine input rank ([N] vs [1, N]) from the model.
            auto typeInfo = ctx->session->GetInputTypeInfo(0);
            auto tensorInfo = typeInfo.GetTensorTypeAndShapeInfo();
            ctx->inputRank = static_cast<int64_t>(tensorInfo.GetShape().size());
            if (ctx->inputRank != 2) ctx->inputRank = 1;

            ZM_LOG_INFO("audio_detect: loaded model '%s' (input='%s' output='%s' "
                        "rank=%lld sr=%d win=%zu hop=%zu)",
                        ctx->modelPath.c_str(), ctx->inputName.c_str(),
                        ctx->outputName.c_str(),
                        static_cast<long long>(ctx->inputRank), ctx->sampleRate,
                        ctx->windowSamples, ctx->hopSamples);
        } catch (const std::exception& e) {
            ZM_LOG_ERROR("audio_detect: failed to load model '%s': %s "
                         "(running as pass-through)",
                         ctx->modelPath.c_str(), e.what());
            ctx->session.reset();
        }
    } else {
        ZM_LOG_WARN("audio_detect: no model_path configured; running as pass-through");
    }

    plugin->instance = ctx;
    return 0;
}

static void audio_detect_stop(zm_plugin_t* plugin) {
    auto* ctx = static_cast<AudioDetectCtx*>(plugin->instance);
    if (!ctx) return;
    // Stop metadata deliveries; AudioMeta is intentionally leaked so an in-flight
    // callback can't dereference freed memory.
    if (ctx->host && ctx->host->unsubscribe_evt && ctx->metaSub)
        ctx->host->unsubscribe_evt(ctx->hostCtx, ctx->metaSub);
    if (ctx->meta) ctx->meta->running.store(false);
    if (ctx->swr) swr_free(&ctx->swr);
    if (ctx->codecCtx) avcodec_free_context(&ctx->codecCtx);
    if (ctx->frame) av_frame_free(&ctx->frame);
    if (ctx->pkt) av_packet_free(&ctx->pkt);
    ctx->session.reset();
    ctx->env.reset();
    ctx->samples.clear();
    delete ctx;
    plugin->instance = nullptr;
}

static void audio_detect_on_frame(zm_plugin_t* plugin, const void* buf, size_t size) {
    auto* ctx = static_cast<AudioDetectCtx*>(plugin->instance);
    if (!ctx || !buf || size < sizeof(zm_frame_hdr_t)) {
        forwardFrame(ctx, buf, size);
        return;
    }

    const zm_frame_hdr_t* hdr = static_cast<const zm_frame_hdr_t*>(buf);
    const uint8_t* payload = static_cast<const uint8_t*>(buf) + sizeof(zm_frame_hdr_t);
    const size_t payloadSize = size - sizeof(zm_frame_hdr_t);

    // Not a compressed-audio frame (or wrong stream, or we can't run) -> forward.
    // audioStreamId < 0 means "any audio stream".
    if (hdr->hw_type != ZM_FRAME_COMPRESSED_AUDIO ||
        (ctx->audioStreamId >= 0 && static_cast<int>(hdr->stream_id) != ctx->audioStreamId) ||
        !ctx->session || !ctx->pkt || !ctx->frame || payloadSize == 0) {
        forwardFrame(ctx, buf, size);
        return;
    }
    // Create the decoder on first audio frame (codec auto-detected by now).
    if (!ensureAudioDecoder(ctx)) {
        forwardFrame(ctx, buf, size);
        return;
    }

    // Wrap the compressed payload in an AVPacket (no ownership transfer; the
    // decoder copies what it needs during send_packet).
    av_packet_unref(ctx->pkt);
    ctx->pkt->data = const_cast<uint8_t*>(payload);
    ctx->pkt->size = static_cast<int>(payloadSize);

    int ret = avcodec_send_packet(ctx->codecCtx, ctx->pkt);
    if (ret >= 0 || ret == AVERROR(EAGAIN)) {
        while (true) {
            ret = avcodec_receive_frame(ctx->codecCtx, ctx->frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) break;
            appendResampled(ctx, ctx->frame);
            av_frame_unref(ctx->frame);
        }
    }
    // Detach borrowed buffer so av_packet_unref doesn't touch caller memory.
    ctx->pkt->data = nullptr;
    ctx->pkt->size = 0;

    // Drain any full windows.
    while (ctx->samples.size() >= ctx->windowSamples) {
        size_t before = ctx->samples.size();
        runWindow(ctx, hdr);
        if (ctx->samples.size() >= before) break; // no progress -> avoid spin
    }

    forwardFrame(ctx, buf, size);
}

#if defined(__GNUC__) || defined(__clang__)
#define ZM_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define ZM_PLUGIN_EXPORT
#endif

ZM_PLUGIN_EXPORT void zm_plugin_init(zm_plugin_t* plugin) {
    plugin->version = 1;
    plugin->type = ZM_PLUGIN_DETECT;
    plugin->start = audio_detect_start;
    plugin->stop = audio_detect_stop;
    plugin->on_frame = audio_detect_on_frame;
    plugin->instance = nullptr;
}

} // extern "C"

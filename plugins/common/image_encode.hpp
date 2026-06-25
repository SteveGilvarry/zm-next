#pragma once

// Shared, header-only image encoders (FFmpeg MJPEG). Header-only so each plugin
// .so compiles its own copy; consumers must link avcodec/avutil + libswscale
// (ZM_FFMPEG_LIBS + SWSCALE) and add this dir to their include path.
//
// Promoted from describe_vlm's file-static encoder so describe_vlm, review_export
// (cutouts) and motion_pixel_diff (background plates) share one implementation.

#include <cstdint>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace zm {
namespace img {

// Encode a tightly-packed pixel buffer of `src_fmt` to an in-memory JPEG.
// Internal helper; callers use the typed wrappers below.
inline bool encode_to_jpeg(const uint8_t* pixels, int width, int height,
                           AVPixelFormat src_fmt, int src_stride,
                           std::vector<uint8_t>& out) {
    out.clear();
    if (!pixels || width <= 0 || height <= 0) return false;

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!codec) return false;

    AVCodecContext* cctx = avcodec_alloc_context3(codec);
    if (!cctx) return false;

    cctx->pix_fmt = AV_PIX_FMT_YUVJ420P;
    cctx->width = width;
    cctx->height = height;
    cctx->time_base = AVRational{1, 25};
    cctx->color_range = AVCOL_RANGE_JPEG;

    SwsContext* sws = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    bool ok = false;

    do {
        if (avcodec_open2(cctx, codec, nullptr) < 0) break;

        frame = av_frame_alloc();
        if (!frame) break;
        frame->format = cctx->pix_fmt;
        frame->width = width;
        frame->height = height;
        if (av_frame_get_buffer(frame, 32) < 0) break;

        sws = sws_getContext(width, height, src_fmt,
                             width, height, AV_PIX_FMT_YUVJ420P,
                             SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws) break;

        const uint8_t* srcSlice[1] = {pixels};
        int srcStride[1] = {src_stride};
        sws_scale(sws, srcSlice, srcStride, 0, height,
                  frame->data, frame->linesize);

        frame->pts = 0;
        if (avcodec_send_frame(cctx, frame) < 0) break;

        pkt = av_packet_alloc();
        if (!pkt) break;
        if (avcodec_receive_packet(cctx, pkt) < 0) break;

        out.assign(pkt->data, pkt->data + pkt->size);
        ok = true;
    } while (false);

    if (pkt) av_packet_free(&pkt);
    if (frame) av_frame_free(&frame);
    if (sws) sws_freeContext(sws);
    if (cctx) avcodec_free_context(&cctx);
    return ok;
}

// Encode a packed RGB24 (R,G,B per pixel) buffer to JPEG.
inline bool encode_rgb24_to_jpeg(const uint8_t* rgb, int width, int height,
                                 std::vector<uint8_t>& out) {
    return encode_to_jpeg(rgb, width, height, AV_PIX_FMT_RGB24, 3 * width, out);
}

// Encode a packed 8-bit grayscale buffer to JPEG (used for background plates).
inline bool encode_gray8_to_jpeg(const uint8_t* gray, int width, int height,
                                 std::vector<uint8_t>& out) {
    return encode_to_jpeg(gray, width, height, AV_PIX_FMT_GRAY8, width, out);
}

} // namespace img
} // namespace zm

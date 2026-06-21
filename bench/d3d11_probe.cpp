// d3d11_probe: load the built decode_ffmpeg plugin, configure D3D11VA hardware
// decode, feed an H.264 elementary stream, and report whether the Intel media
// engine produced hardware surfaces (the plugin logs the surface pixel format).
//
// Build (vcvars64 shell), e.g.:
//   cl /nologo /EHsc /std:c++20 /I ..\core\include d3d11_probe.cpp /Fe:d3d11_probe.exe
// Run with the plugin dir + vcpkg debug bin on PATH so the FFmpeg DLLs resolve.

#include "zm_plugin.h"
#include "zm/dl_compat.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <atomic>

static std::atomic<int> g_frames{0};

static void probe_log(void*, zm_log_level_t lvl, const char* msg) {
    std::printf("[plugin][%d] %s\n", (int)lvl, msg ? msg : "(null)");
}
static void probe_on_frame(void*, const void* buf, size_t sz) {
    if (sz < sizeof(zm_frame_hdr_t)) return;
    g_frames++;
}

static std::vector<std::vector<uint8_t>> load_annexb(const std::string& path) {
    std::vector<std::vector<uint8_t>> pkts;
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return pkts; }
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), {});
    size_t i = 0;
    while (i + 4 < b.size()) {
        if (b[i] == 0 && b[i+1] == 0 && b[i+2] == 0 && b[i+3] == 1) {
            size_t j = i + 4;
            while (j + 4 < b.size() && !(b[j]==0&&b[j+1]==0&&b[j+2]==0&&b[j+3]==1)) ++j;
            pkts.emplace_back(b.begin()+i, b.begin()+(j < b.size() ? j : b.size()));
            i = j;
        } else ++i;
    }
    return pkts;
}

int main(int argc, char** argv) {
    const char* plugin = argc > 1 ? argv[1] : "decode_ffmpeg.dll";
    const char* clip   = argc > 2 ? argv[2] : "packet.h264";
    const char* hwaccel = argc > 3 ? argv[3] : "d3d11va";

    void* h = zm_dlopen(plugin);
    if (!h) { std::fprintf(stderr, "load %s failed: %s\n", plugin, zm_dlerror().c_str()); return 1; }
    auto init = (void(*)(zm_plugin_t*))zm_dlsym(h, "zm_plugin_init");
    if (!init) { std::fprintf(stderr, "no zm_plugin_init\n"); return 1; }

    zm_plugin_t p; std::memset(&p, 0, sizeof(p));
    init(&p);

    zm_host_api_t host; std::memset(&host, 0, sizeof(host));
    host.log = probe_log;
    host.on_frame = probe_on_frame;

    std::string cfg = std::string("{\"threads\":1,\"scale\":\"orig\",\"hw_decode\":true,"
                                  "\"codec\":\"h264\",\"output_format\":\"yuv420p\",\"hwaccel\":\"")
                    + hwaccel + "\"}";
    std::printf("starting decode_ffmpeg with %s\n", cfg.c_str());
    if (p.start(&p, &host, nullptr, cfg.c_str()) != 0) {
        std::fprintf(stderr, "plugin start failed\n"); return 2;
    }

    auto pkts = load_annexb(clip);
    std::printf("feeding %zu H.264 NAL packets...\n", pkts.size());
    for (const auto& pk : pkts) {
        std::vector<uint8_t> buf(sizeof(zm_frame_hdr_t) + pk.size());
        auto* hdr = reinterpret_cast<zm_frame_hdr_t*>(buf.data());
        std::memset(hdr, 0, sizeof(zm_frame_hdr_t));
        hdr->hw_type = ZM_FRAME_COMPRESSED;   // mark as compressed H.264 so it decodes
        hdr->bytes = (uint32_t)pk.size();
        std::memcpy(buf.data() + sizeof(zm_frame_hdr_t), pk.data(), pk.size());
        p.on_frame(&p, buf.data(), buf.size());
    }
    if (p.stop) p.stop(&p);
    std::printf("decoded frames forwarded downstream: %d\n", g_frames.load());
    zm_dlclose(h);
    return 0;
}

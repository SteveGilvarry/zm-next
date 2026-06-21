// PluginManager implements dynamic loading of C plugins via C ABI

// filepath: /Users/stevengilvarry/Code/zm-next/core/src/PluginManager.cpp
#include "zm/PluginManager.hpp"
#include "zm_plugin.h"
#include "zm/EventBus.hpp"
#include "zm/StageRunner.hpp"
#include <iostream>
#include <cstring>
#include <string>

// ── Cross-platform dynamic-loader shim ────────────────────────────────────────
// POSIX uses dlopen/dlsym/dlclose/dlerror (libdl); Windows uses the Win32
// LoadLibrary/GetProcAddress/FreeLibrary family. The plugin "handle" is a
// void* on both (HMODULE is a pointer), so PluginManager stores it unchanged.
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
namespace {
inline void* zm_dlopen(const char* path) {
    return reinterpret_cast<void*>(::LoadLibraryA(path));
}
inline void* zm_dlsym(void* handle, const char* sym) {
    return reinterpret_cast<void*>(
        ::GetProcAddress(reinterpret_cast<HMODULE>(handle), sym));
}
inline void zm_dlclose(void* handle) {
    ::FreeLibrary(reinterpret_cast<HMODULE>(handle));
}
inline std::string zm_dlerror() {
    DWORD e = ::GetLastError();
    if (!e) return "(no error)";
    char* msg = nullptr;
    ::FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                         FORMAT_MESSAGE_IGNORE_INSERTS,
                     nullptr, e, 0, reinterpret_cast<char*>(&msg), 0, nullptr);
    std::string s = msg ? msg : "(unknown error)";
    if (msg) ::LocalFree(msg);
    return s;
}
} // namespace
#else
#include <dlfcn.h>
namespace {
inline void* zm_dlopen(const char* path) { return ::dlopen(path, RTLD_NOW); }
inline void* zm_dlsym(void* handle, const char* sym) { return ::dlsym(handle, sym); }
inline void zm_dlclose(void* handle) { ::dlclose(handle); }
inline std::string zm_dlerror() { const char* e = ::dlerror(); return e ? e : "(no error)"; }
} // namespace
#endif

// Global host API for v1 plugins
// Route plugin logs to stdout so VS Code Debug Console can capture them
extern "C" void host_log(void* /*host_ctx*/, zm_log_level_t level, const char* msg) {
    const char* lvl = "INFO";
    if (level == ZM_LOG_DEBUG) lvl = "DEBUG";
    else if (level == ZM_LOG_WARN) lvl = "WARN";
    else if (level == ZM_LOG_ERROR) lvl = "ERROR";
    std::cout << "[PLUGIN][" << lvl << "] " << (msg ? msg : "(null)") << std::endl;
}
// Routes a plugin's output frame to its downstream stages. host_ctx is the
// plugin's StageRunner; forwarding copies the frame into each child stage's
// bounded queue (which runs on its own thread), so stages are decoupled.
extern "C" void chain_on_frame(void* host_ctx, const void* buf, size_t size) {
    if (!host_ctx) return;
    static_cast<zm::StageRunner*>(host_ctx)->forwardToChildren(buf, size);
}

// Host-backed event subscription so plugins reliably reach the host's single
// EventBus instance across the dlopen boundary (a plugin calling
// EventBus::instance() in its own .dylib would get a separate instance).
extern "C" void* host_subscribe_evt(void* /*host_ctx*/,
                                    void (*cb)(void* user, const char* json_event),
                                    void* user) {
    auto id = zm::EventBus::instance().subscribe(
        "plugin_event",
        [cb, user](const std::string& m) { cb(user, m.c_str()); });
    return reinterpret_cast<void*>(static_cast<uintptr_t>(id));
}
extern "C" void host_unsubscribe_evt(void* /*host_ctx*/, void* handle) {
    zm::EventBus::instance().unsubscribe(
        "plugin_event",
        static_cast<zm::EventBus::SubscriptionId>(reinterpret_cast<uintptr_t>(handle)));
}

zm_host_api_t gHost = {
    /* log */ host_log,
    /* publish_evt */ [](void* host_ctx, const char* json_event) -> void {
        zm::EventBus::instance().publish("plugin_event", json_event);
    },
    /* on_frame        */ chain_on_frame,
    /* subscribe_evt   */ host_subscribe_evt,
    /* unsubscribe_evt */ host_unsubscribe_evt,
    /* reserved        */ {nullptr, nullptr}
};

namespace zm {

PluginManager::PluginManager() {
    // Set up global host API for plugins (log and on_frame can be set elsewhere)
    gHost.publish_evt = [](void* host_ctx, const char* json_event) -> void {
        zm::EventBus::instance().publish("plugin_event", json_event);
    };
}

PluginManager::~PluginManager() {
    for (auto &handle : handles_) {
        zm_dlclose(handle);
    }
}


// Legacy: load a single plugin (for tests)
bool PluginManager::loadPlugin(const std::string &path) {
    void *handle = zm_dlopen(path.c_str());
    if (!handle) {
        std::cerr << "Failed to load plugin: " << zm_dlerror() << std::endl;
        return false;
    }
    handles_.push_back(handle);
    return true;
}

// Load a pipeline: vector of PluginConfig (path, config_json)
bool PluginManager::loadPipeline(const std::vector<PluginConfig>& pipeline) {
    pipeline_.clear();
    for (const auto& pcfg : pipeline) {
        void* handle = zm_dlopen(pcfg.path.c_str());
        if (!handle) {
            std::cerr << "Failed to load plugin: " << pcfg.path << ": " << zm_dlerror() << std::endl;
            return false;
        }
        using init_fn_t = void(*)(zm_plugin_t*);
        auto init_fn = (init_fn_t)zm_dlsym(handle, "zm_plugin_init");
        if (!init_fn) {
            std::cerr << "zm_plugin_init not found in " << pcfg.path << std::endl;
            zm_dlclose(handle);
            return false;
        }
        PluginInstance inst;
        inst.handle = handle;
        std::memset(&inst.plugin, 0, sizeof(zm_plugin_t));
        init_fn(&inst.plugin);
        if (inst.plugin.version != ZM_PLUGIN_ABI_VERSION) {
            std::cerr << "[PluginManager] WARN: " << pcfg.path << " reports ABI version "
                      << inst.plugin.version << ", expected " << ZM_PLUGIN_ABI_VERSION
                      << "; loading anyway." << std::endl;
        }
        inst.config = pcfg;
        pipeline_.push_back(inst);
    }
    return true;
}

void PluginManager::startAll() {
    if (pipeline_.empty()) return;

    // Identify input plugin (first with type == ZM_PLUGIN_INPUT)
    size_t inputIdx = 0;
    for (; inputIdx < pipeline_.size(); ++inputIdx) {
        if (pipeline_[inputIdx].plugin.type == ZM_PLUGIN_INPUT)
            break;
    }
    if (inputIdx == pipeline_.size()) {
        std::cerr << "PluginManager::startAll: No input plugin found in pipeline." << std::endl;
        return;
    }

    // One StageRunner (thread + bounded drop-queue) per non-input plugin,
    // index-aligned with pipeline_ (the input slot stays null).
    runners_.clear();
    runners_.resize(pipeline_.size());
    for (size_t i = 0; i < pipeline_.size(); ++i) {
        if (i == inputIdx) continue;
        const int depth = pipeline_[i].config.queue_depth > 0 ? pipeline_[i].config.queue_depth : 16;
        runners_[i] = std::make_unique<StageRunner>(&pipeline_[i].plugin, static_cast<size_t>(depth));
    }
    // Resolve a node's downstream child runners from the tree topology.
    auto childRunnersOf = [&](size_t i) {
        std::vector<StageRunner*> kids;
        for (int ci : pipeline_[i].config.children)
            if (ci >= 0 && ci < static_cast<int>(pipeline_.size()) && runners_[ci])
                kids.push_back(runners_[ci].get());
        return kids;
    };
    for (size_t i = 0; i < pipeline_.size(); ++i)
        if (runners_[i]) runners_[i]->setChildren(childRunnersOf(i));

    // Start each non-input plugin with its StageRunner as host_ctx, so that
    // host->on_frame (chain_on_frame) routes its output into the children's
    // queues. Then start the stage threads.
    for (size_t i = 0; i < pipeline_.size(); ++i) {
        if (i == inputIdx) continue;
        auto& inst = pipeline_[i];
        if (inst.plugin.start)
            inst.plugin.start(&inst.plugin, &gHost, runners_[i].get(), inst.config.config_json.c_str());
    }
    for (size_t i = 0; i < pipeline_.size(); ++i)
        if (runners_[i]) runners_[i]->start();

    // Ring + capture thread, delivering captured frames to the input's children.
    ring_ = std::make_unique<ShmRing>(256, 1024*1024, ringName_);
    captureThread_ = std::make_unique<CaptureThread>(&pipeline_[inputIdx].plugin, *ring_,
                                                     childRunnersOf(inputIdx),
                                                     pipeline_[inputIdx].config.config_json, link_);
    captureThread_->start();
}

void PluginManager::stopAll() {
    // Stop the frame pump first: this cancels the ring's blocking pop and joins
    // the capture thread, whose run() stops the input plugin on exit.
    if (captureThread_) {
        captureThread_->stop();
        captureThread_.reset();
    }
    // Stop the stage threads (join) so no plugin on_frame is in flight, then stop
    // the plugins. The input plugin was already stopped by the capture thread.
    for (auto& r : runners_)
        if (r) r->stop();
    for (auto& inst : pipeline_) {
        if (inst.plugin.type == ZM_PLUGIN_INPUT) continue;
        if (inst.plugin.stop) {
            inst.plugin.stop(&inst.plugin);
        }
    }
    runners_.clear();
}


size_t PluginManager::pluginCount() const {
    return pipeline_.size();
}


void* PluginManager::getHandle(size_t index) const {
    if (index < pipeline_.size())
        return pipeline_[index].handle;
    return nullptr;
}

} // namespace zm


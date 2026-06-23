// zm-core.cpp - Main runner for ZoneMinder-Next plugin pipelines
#include "zm/PipelineLoader.hpp"
#include "zm/PluginManager.hpp"
#include "zm/EventBus.hpp"
#include "zm/WorkerLink.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <filesystem>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>

using namespace zm;
namespace fs = std::filesystem;

// Set by the "stop" control command or a termination signal to break the main
// loop for a clean shutdown.
static std::atomic<bool> g_shutdown{false};

// Async-signal-safe: only touch the atomic flag. SIGTERM/SIGINT request a clean
// stop; SIGHUP is reserved for reload/logrotate and is a no-op for now (so the
// supervising daemon's logrot signal doesn't kill the worker).
extern "C" void handle_signal(int sig) {
    if (sig == SIGHUP) return;
    g_shutdown.store(true);
}

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " --pipeline <pipeline.json>\n";
    std::cout << "       or: " << prog << " --pipelines-dir <dir>\n";
    std::cout << "Options:\n";
    std::cout << "  --socket <path>      Unix socket for the worker link (media+events+control)\n";
    std::cout << "  --monitor-id <id>    Monitor id for this worker (per-monitor socket)\n";
}

int main(int argc, char** argv) {
    std::string pipelineFile;
    std::string pipelinesDir;
    std::string socketPath;
    int64_t monitorId = 0;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--pipeline" && i + 1 < argc) pipelineFile = argv[++i];
        else if (arg == "--pipelines-dir" && i + 1 < argc) pipelinesDir = argv[++i];
        else if (arg == "--socket" && i + 1 < argc) socketPath = argv[++i];
        else if (arg == "--monitor-id" && i + 1 < argc) monitorId = std::stoll(argv[++i]);
        else if (arg == "-h" || arg == "--help") { print_usage(argv[0]); return 0; }
    }
    if (pipelineFile.empty() && pipelinesDir.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    // Install signal handlers so the supervising daemon can stop us cleanly.
    std::signal(SIGTERM, handle_signal);
    std::signal(SIGINT, handle_signal);
    std::signal(SIGHUP, handle_signal);

    // Find pipeline file if only directory is given
    if (pipelineFile.empty() && !pipelinesDir.empty()) {
        for (const auto& entry : fs::directory_iterator(pipelinesDir)) {
            if (entry.path().extension() == ".json") {
                pipelineFile = entry.path();
                std::cout << "Using pipeline: " << pipelineFile << std::endl;
                break;
            }
        }
        if (pipelineFile.empty()) {
            std::cerr << "No pipeline JSON found in " << pipelinesDir << std::endl;
            return 2;
        }
    }

    // Pipeline config is a JSON file pushed by the orchestrating daemon (zm-api);
    // zm-next has no DB connection.
    PipelineLoader loader(pipelineFile);
    if (!loader.load()) {
        std::cerr << "Failed to load pipeline: " << pipelineFile << std::endl;
        return 3;
    }
    loader.printProgress();

    PluginManager pm;
    // Use new API: pass vector<PluginConfig> from loader
    if (!pm.loadPipeline(loader.getPipeline())) {
        std::cerr << "Failed to load plugins for pipeline." << std::endl;
        return 4;
    }

    // Optional worker link: one per-monitor Unix socket carrying media + events
    // (push) and control (pull) to the orchestrating local zm-api.
    std::unique_ptr<WorkerLink> link;
    if (!socketPath.empty()) {
        link = std::make_unique<WorkerLink>(static_cast<uint32_t>(monitorId), socketPath);
        link->setCommandHandler([&pm](const std::string& name, const std::string& args)
                                    -> WorkerLink::CommandResult {
            (void)args;
            WorkerLink::CommandResult r;
            if (name == "stop" || name == "shutdown") {
                g_shutdown.store(true);
                r.ok = true; r.message = "stopping";
            } else if (name == "status") {
                r.ok = true; r.message = "status";
                r.data_json = "{\"plugins\":" + std::to_string(pm.pluginCount()) +
                              ",\"running\":" + (g_shutdown.load() ? "false" : "true") + "}";
            } else if (name == "reload") {
                // Hot reload is Phase 2 — daemon should restart the process for now.
                r.ok = false; r.message = "not_implemented";
            } else if (name == "assign_recording") {
                // Plugin-targeted command: dispatch the full command JSON onto the
                // in-process event bus so the store plugin (subscribed via the host
                // API) can match it by clip_token. `args` is the raw command JSON.
                EventBus::instance().publish("plugin_event", args);
                r.ok = true; r.message = "dispatched";
            } else {
                r.ok = false; r.message = "unknown_command: " + name;
            }
            return r;
        });

        // Two-way audio (talkback): inbound audio from the API destined for the
        // camera speaker. Routing to the camera's ONVIF/RTSP audio backchannel is
        // owned by the capture plugin (see docs/Two_Way_Audio.md); for now we log
        // receipt so the contract is exercised end-to-end.
        link->setTalkbackHandler([](uint32_t codec, int64_t pts_us, const std::string& data) {
            std::cout << "[zm-core] talkback audio: codec=" << codec
                      << " pts=" << pts_us << " bytes=" << data.size()
                      << " (camera backchannel relay not yet implemented)" << std::endl;
        });

        // Bridge in-process telemetry to the link. Plugins publish JSON events on
        // the "plugin_event" channel (see PluginManager/CaptureThread host API);
        // WorkerLink maps them onto canonical stream-socket EVENT frames.
        WorkerLink* wl = link.get();
        EventBus::instance().subscribe("plugin_event", [wl](const std::string& evt) {
            // Inbound plugin-targeted commands are re-published on this same bus to
            // reach the plugins (see the command handler above). Don't echo those
            // back out to socket consumers — and never call back into WorkerLink
            // here (this runs under WorkerLink's lock during command dispatch).
            if (evt.find("\"cmd\"") != std::string::npos) {
                auto j = nlohmann::json::parse(evt, nullptr, /*allow_exceptions=*/false);
                if (j.is_object() && j.contains("cmd")) return;
            }
            wl->publishEventJson(evt);
        });

        if (!link->start()) {
            std::cerr << "Failed to start worker link at " << socketPath << std::endl;
            return 5;
        }
        // CaptureThread taps compressed media into the link; must be set pre-start.
        pm.setWorkerLink(link.get());
    }

    // Per-instance shared-memory segment name so concurrent monitors don't clash.
    pm.setRingName("zm_shmring_" + std::to_string(monitorId));

    pm.startAll();
    std::cout << "[zm-core] Pipeline running. Press Ctrl+C to exit." << std::endl;
    // Main loop: plugins run in their own threads; wait for a stop command.
    while (!g_shutdown.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "[zm-core] Shutting down..." << std::endl;
    pm.stopAll();
    if (link) link->stop();
    return 0;
}

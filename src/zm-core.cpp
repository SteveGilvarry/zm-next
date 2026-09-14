// zm-core.cpp - Main runner for ZoneMinder-Next plugin pipelines
#include "zm/PipelineLoader.hpp"
#include "zm/PluginManager.hpp"
#include "zm/EventBus.hpp"
#include "zm/Redactor.hpp"
#include "zm/WorkerConfig.hpp"
#include "zm/WorkerHello.hpp"
#include "zm/WorkerLink.hpp"
#include "zm/platform.hpp"
#include "zm_plugin.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>
#include <unistd.h>
#include <vector>

using namespace zm;
using json = nlohmann::json;
namespace fs = std::filesystem;

// Exit codes a supervisor can act on (docs/Worker_Control_Protocol.md "Exit codes").
constexpr int kExitStopped = 0;    // stop command or signal
constexpr int kExitUsage = 64;     // bad command line or --pipeline that doesn't load
constexpr int kExitSocket = 69;    // worker socket unusable
constexpr int kExitInternal = 70;  // unexpected error

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
    std::cout << "Usage: " << prog << " --socket <path> [--pipeline <pipeline.json | ->]   (- = read JSON from stdin)\n";
    std::cout << "       or: " << prog << " --pipeline <pipeline.json | -> | --pipelines-dir <dir>\n";
    std::cout << "Without a pipeline the worker starts unconfigured and waits for a configure command\n"
                 "on its socket.\n";
    std::cout << "Options:\n";
    std::cout << "  --socket <path>      Unix socket for the worker link (media+events+control)\n";
    std::cout << "  --monitor-id <id>    Monitor id for this worker (per-monitor socket)\n";
    std::cout << "  --control-uid <uid>  Also accept commands from this uid (repeatable; e.g. zm-api's\n"
                 "                       service user). This process's own uid is always accepted;\n"
                 "                       other peers may read media and events only.\n";
}

namespace {

// The worker's one pipeline and what the hello says about it. Commands run on
// the worker link's thread; `mu` keeps a configure from racing the final stop.
struct Worker {
    std::mutex mu;
    const worker::Catalog* catalog = nullptr;
    WorkerLink* link = nullptr;          // null without --socket
    int64_t monitorId = 0;
    worker::HelloFacts facts;
    std::unique_ptr<PluginManager> pm;
    json pipeline;                       // resolved (holds secrets); null when unconfigured
    worker::SecretPaths secretPaths;     // where references were resolved
    std::string salt;

    const json* pipelinePtr() const { return pipeline.is_object() ? &pipeline : nullptr; }

    std::string pipelineHash() const {
        return pipelinePtr() ? worker::pipeline_hash(pipeline, *catalog, &secretPaths) : "";
    }

    // Every secret in the pipeline (by schema key name or reference) goes to
    // the redactor before any plugin starts.
    void registerSecrets() {
        std::vector<std::pair<std::string, std::string>> found;
        if (pipelinePtr()) worker::redact_pipeline(pipeline, *catalog, &found, &secretPaths);
        std::vector<std::string> values;
        for (auto& f : found) values.push_back(std::move(f.second));
        Redactor::instance().setSecrets(values);
    }

    void setState(const std::string& state, const std::string& reason) {
        facts.state = state;
        if (!link) return;
        link->setWorkerHello(worker::hello_public(facts, *catalog, pipelinePtr(), &secretPaths).dump(),
                             worker::hello_control_extra(*catalog, pipelinePtr(), salt, &secretPaths).dump());
        json ev = {{"type", "worker_state"}, {"state", state}, {"reason", reason}};
        const std::string hash = pipelineHash();
        ev["pipeline_hash"] = hash.empty() ? json(nullptr) : json(hash);
        link->publishEventJson(ev.dump());
    }

    // Load and start `pipeline`. On failure `error` says why.
    bool start(std::string& error) {
        PipelineLoader loader("configure");
        if (!loader.loadText(pipeline.dump())) {
            error = "pipeline has no loadable plugins";
            return false;
        }
        pm = std::make_unique<PluginManager>();
        if (link) pm->setWorkerLink(link);
        pm->setRingName("zm_shmring_" + std::to_string(monitorId));
        if (!pm->loadPipeline(loader.getPipeline())) {
            pm.reset();
            error = "a plugin library failed to load (see the worker log)";
            return false;
        }
        pm->startAll();
        return true;
    }

    void stop() {
        if (!pm) return;
        pm->stopAll();
        pm.reset();
    }

    // {"cmd":"configure","pipeline":{...},"secrets":{...},"secrets_salt":"...","apply":"restart"}
    WorkerLink::CommandResult configure(const std::string& args) {
        WorkerLink::CommandResult r;
        const json cmd = json::parse(args, nullptr, false);
        if (!cmd.is_object()) {
            r.message = "invalid_config";
            r.data_json = json({{"errors", {{{"path", ""}, {"message", "command is not a JSON object"}}}}}).dump();
            return r;
        }
        std::vector<worker::ConfigError> errors;
        const json secrets = cmd.contains("secrets") ? cmd["secrets"] : json::object();
        const std::string apply = cmd.value("apply", std::string("restart"));
        if (apply != "restart") errors.push_back({"apply", "must be \"restart\""});
        if (cmd.contains("secrets_salt") && !cmd["secrets_salt"].is_string())
            errors.push_back({"secrets_salt", "must be a string"});
        if (!cmd.contains("pipeline")) {
            errors.push_back({"pipeline", "is required"});
        } else {
            const auto more = worker::validate_pipeline(cmd["pipeline"], *catalog, secrets);
            errors.insert(errors.end(), more.begin(), more.end());
        }
        if (!errors.empty()) {
            r.message = "invalid_config";
            r.data_json = json({{"errors", worker::errors_json(errors)}}).dump();
            return r;
        }

        std::lock_guard<std::mutex> lock(mu);
        if (g_shutdown.load()) {
            r.message = "stopping";
            return r;
        }
        auto resolved = worker::resolve_secrets(cmd["pipeline"], secrets);
        setState("configuring", "configure");
        stop();
        pipeline = std::move(resolved.pipeline);
        secretPaths = std::move(resolved.secret_paths);
        salt = cmd.value("secrets_salt", std::string());
        // Register the new secrets before starting plugins. The old pipeline's
        // are dropped: its plugins are stopped, so nothing logs them any more.
        registerSecrets();
        std::string error;
        if (!start(error)) {
            pipeline = nullptr;
            secretPaths.clear();
            setState("failed", error);
            r.message = "apply_failed";
            r.data_json = json({{"error", error}}).dump();
            return r;
        }
        setState("running", "configured");
        r.ok = true;
        r.message = "configured";
        r.data_json = json({{"pipeline_hash", pipelineHash()}}).dump();
        return r;
    }
};

int run(int argc, char** argv) {
    std::string pipelineFile;
    std::string pipelinesDir;
    std::string socketPath;
    int64_t monitorId = 0;
    std::vector<uint32_t> controlUids;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--pipeline" && i + 1 < argc) pipelineFile = argv[++i];
        else if (arg == "--pipelines-dir" && i + 1 < argc) pipelinesDir = argv[++i];
        else if (arg == "--socket" && i + 1 < argc) socketPath = argv[++i];
        else if (arg == "--monitor-id" && i + 1 < argc) {
            const std::string v = argv[++i];
            if (v.empty() || v.find_first_not_of("0123456789") != std::string::npos) {
                std::cerr << "--monitor-id needs a number, got '" << v << "'" << std::endl;
                return kExitUsage;
            }
            monitorId = std::stoll(v);
        }
        else if (arg == "--control-uid" && i + 1 < argc) {
            const std::string v = argv[++i];
            if (v.empty() || v.find_first_not_of("0123456789") != std::string::npos) {
                std::cerr << "--control-uid needs a numeric uid, got '" << v << "'" << std::endl;
                return kExitUsage;
            }
            controlUids.push_back(static_cast<uint32_t>(std::stoul(v)));
        }
        else if (arg == "-h" || arg == "--help") { print_usage(argv[0]); return kExitStopped; }
        else {
            std::cerr << "unknown or incomplete argument '" << arg << "'" << std::endl;
            print_usage(argv[0]);
            return kExitUsage;
        }
    }
    if (pipelineFile.empty() && pipelinesDir.empty() && socketPath.empty()) {
        print_usage(argv[0]);
        return kExitUsage;
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
            return kExitUsage;
        }
    }

    // This build's plugin catalog, from the same directory as the plugin
    // libraries: the hello's plugin list, describe_plugins and configure validation.
    const worker::Catalog catalog = worker::load_catalog(plugins_dir());
    if (!catalog.error.empty())
        std::cerr << "[zm-core] plugin catalog: " << catalog.error
                  << " (hello lists no plugins; configure rejects every kind)" << std::endl;

    Worker w;
    w.catalog = &catalog;
    w.monitorId = monitorId;
    w.facts.version = ZM_NEXT_VERSION;
    w.facts.commit = ZM_NEXT_COMMIT;
    w.facts.plugin_abi = ZM_PLUGIN_ABI_VERSION;
    w.facts.monitor_id = monitorId;
    w.facts.state = "unconfigured";
    for (std::string b, all = ZM_HW_BACKENDS; !all.empty();) {
        const auto comma = all.find(',');
        b = all.substr(0, comma);
        all = comma == std::string::npos ? "" : all.substr(comma + 1);
        if (!b.empty()) w.facts.hw_backends.push_back(b);
    }

    // A pipeline from the command line (the pre-configure path, still used by
    // zm-api's stdin delivery) is parsed before the socket opens, so a bad one
    // exits with a usage error.
    if (!pipelineFile.empty()) {
        // Pipeline config is a JSON file pushed by the orchestrating daemon (zm-api);
        // zm-next has no DB connection.
        PipelineLoader loader(pipelineFile);
        if (!loader.load()) {
            std::cerr << "Failed to load pipeline: " << pipelineFile << std::endl;
            return kExitUsage;
        }
        loader.printProgress();
        w.pipeline = json::parse(loader.rawJson(), nullptr, false);
        w.registerSecrets();
    }

    // Optional worker link: one per-monitor Unix socket carrying media + events
    // (push) and control (pull) to the orchestrating local zm-api.
    std::unique_ptr<WorkerLink> link;
    if (!socketPath.empty()) {
        WorkerLink::Config linkCfg;
        if (!controlUids.empty()) {
            linkCfg.control_uids = controlUids;
            linkCfg.control_uids.push_back(static_cast<uint32_t>(::geteuid()));
        }
        link = std::make_unique<WorkerLink>(static_cast<uint32_t>(monitorId), socketPath, linkCfg);
        w.link = link.get();
        link->setWorkerHello(worker::hello_public(w.facts, catalog, w.pipelinePtr()).dump(),
                             worker::hello_control_extra(catalog, w.pipelinePtr(), "").dump());
        link->setCommandHandler([&w, &catalog](const std::string& name, const std::string& args)
                                    -> WorkerLink::CommandResult {
            WorkerLink::CommandResult r;
            if (name == "stop" || name == "shutdown") {
                g_shutdown.store(true);
                r.ok = true; r.message = "stopping";
            } else if (name == "status") {
                r.ok = true; r.message = "status";
                std::lock_guard<std::mutex> lock(w.mu);
                json data = {{"state", w.facts.state},
                             {"plugins", w.pm ? w.pm->pluginCount() : 0},
                             {"running", !g_shutdown.load() && w.pm != nullptr}};
                const std::string hash = w.pipelineHash();
                data["pipeline_hash"] = hash.empty() ? json(nullptr) : json(hash);
                r.data_json = data.dump();
            } else if (name == "configure") {
                r = w.configure(args);
            } else if (name == "describe_plugins") {
                // {"cmd":"describe_plugins","kinds":["tracker",...]} (omit kinds = all
                // plugins with a schema) -> data {"<kind>": {"version", "schema"} | null}.
                std::vector<std::string> kinds;
                const auto j = json::parse(args, nullptr, false);
                if (j.is_object() && j.contains("kinds") && j["kinds"].is_array())
                    for (const auto& k : j["kinds"])
                        if (k.is_string()) kinds.push_back(k.get<std::string>());
                r.ok = true; r.message = "plugins";
                r.data_json = worker::describe_plugins(catalog, kinds).dump();
            } else if (name == "reload") {
                // Replaced by configure; hot reload is Phase 3.
                r.ok = false; r.message = "not_implemented";
            } else if (name == "assign_recording" || name == "snapshot_now" ||
                       name == "describe_now") {
                // Plugin-targeted command: dispatch the full command JSON onto the
                // in-process event bus for the plugin that owns it (store matches
                // assign_recording by clip_token; store_snapshot and describe_vlm
                // answer snapshot_now / describe_now with an EVENT carrying the
                // command's request_id). `args` is the raw command JSON.
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
            // back out to socket consumers. A plugin may answer a command inline, so
            // this can run inside the command handler; WorkerLink calls handlers
            // without its lock held, so publishing the answer here is safe.
            if (evt.find("\"cmd\"") != std::string::npos) {
                auto j = json::parse(evt, nullptr, /*allow_exceptions=*/false);
                if (j.is_object() && j.contains("cmd")) return;
            }
            wl->publishEventJson(evt);
        });

        if (!link->start()) {
            std::cerr << "Failed to start worker link at " << socketPath << std::endl;
            return kExitSocket;
        }
    }

    {
        std::lock_guard<std::mutex> lock(w.mu);
        if (w.pipelinePtr()) {
            std::string error;
            if (!w.start(error)) {
                std::cerr << "Failed to load plugins for pipeline: " << error << std::endl;
                if (link) link->stop();
                return kExitUsage;
            }
            w.setState("running", "command_line");
            std::cout << "[zm-core] Pipeline running. Press Ctrl+C to exit." << std::endl;
        } else {
            w.setState("unconfigured", "started");
            std::cout << "[zm-core] Unconfigured; waiting for configure on " << socketPath << std::endl;
        }
    }

    // Main loop: plugins run in their own threads; wait for a stop command.
    while (!g_shutdown.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "[zm-core] Shutting down..." << std::endl;
    {
        std::lock_guard<std::mutex> lock(w.mu);
        w.stop();
    }
    if (link) link->stop();
    return kExitStopped;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "[zm-core] fatal: " << Redactor::instance().apply(e.what()) << std::endl;
    } catch (...) {
        std::cerr << "[zm-core] fatal: unknown exception" << std::endl;
    }
    return kExitInternal;
}

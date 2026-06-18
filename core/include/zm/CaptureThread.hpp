#pragma once

#include <thread>
#include <atomic>
#include <memory>
#include "zm_plugin.h"
#include "zm/ShmRing.hpp"

namespace zm {

class WorkerLink;   // optional media sink (per-monitor worker socket)
class StageRunner;  // downstream stage threads (input plugin's children)

// Manages a capture input plugin, pushes frames into a ShmRing and delivers them
// to the input plugin's downstream stage runners (each on its own thread).
class CaptureThread {
public:
    CaptureThread(zm_plugin_t* inputPlugin,
                  ShmRing& ring,
                  const std::vector<StageRunner*>& outputs,
                  const std::string& inputConfig,
                  WorkerLink* link = nullptr);
    ~CaptureThread();

    // Start capture loop
    void start();
    // Stop capture loop
    void stop();

private:
    void run();

    zm_plugin_t* inputPlugin_;
    ShmRing& ring_;
    std::vector<StageRunner*> outputs_;
    std::string inputConfig_;
    WorkerLink* link_;

    std::thread thread_;
    std::atomic<bool> running_{false};
};

} // namespace zm

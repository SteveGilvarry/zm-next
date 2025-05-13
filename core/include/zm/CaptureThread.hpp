#pragma once

#include <thread>
#include <atomic>
#include <memory>
#include "zm_plugin.h"
#include "zm/ShmRing.hpp"

namespace zm {

// Manages a capture input plugin, pushes frames into a ShmRing and fans out to output plugins
class CaptureThread {
public:
    CaptureThread(zm_plugin_t* inputPlugin,
                  ShmRing& ring,
                  const std::vector<zm_plugin_t*>& outputs,
                  const std::string& inputConfig);
    ~CaptureThread();

    // Start capture loop
    void start();
    // Stop capture loop
    void stop();

private:
    void run();

    zm_plugin_t* inputPlugin_;
    ShmRing& ring_;
    std::vector<zm_plugin_t*> outputs_;
    std::string inputConfig_;

    std::thread thread_;
    std::atomic<bool> running_{false};
};

} // namespace zm

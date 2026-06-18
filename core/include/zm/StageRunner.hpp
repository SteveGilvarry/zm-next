#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "zm_plugin.h"

namespace zm {

// Runs one pipeline stage (plugin) on its own thread with a bounded, drop-oldest
// input queue. Decouples stages so a slow stage (e.g. a heavy detector) drops its
// own backlog instead of stalling capture, recording, or sibling branches.
//
// Frames are delivered as owned copies of the [zm_frame_hdr_t][payload] buffer.
// A plugin's on_frame runs on this runner's thread; when it forwards downstream
// via host->on_frame, the host routes that to forwardToChildren(), which copies
// into each child runner's queue.
class StageRunner {
public:
    StageRunner(zm_plugin_t* plugin, size_t max_depth);
    ~StageRunner();

    StageRunner(const StageRunner&) = delete;
    StageRunner& operator=(const StageRunner&) = delete;

    void setChildren(std::vector<StageRunner*> children) { children_ = std::move(children); }

    void start();
    void stop();

    // Enqueue a copy of the frame for this stage; drops the oldest queued frame
    // if the queue is full. Thread-safe; never blocks the caller.
    void deliver(const void* buf, size_t size);

    // Forward a produced frame to every downstream child (copies into their
    // queues). Called from the chain host->on_frame hook.
    void forwardToChildren(const void* buf, size_t size);

    uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    uint64_t processed() const { return processed_.load(std::memory_order_relaxed); }

private:
    void run();

    zm_plugin_t* plugin_;
    size_t max_depth_;
    std::vector<StageRunner*> children_;

    std::deque<std::vector<uint8_t>> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> dropped_{0};
    std::atomic<uint64_t> processed_{0};
};

} // namespace zm

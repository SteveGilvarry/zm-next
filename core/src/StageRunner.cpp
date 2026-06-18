#include "zm/StageRunner.hpp"

#include <iostream>

namespace zm {

StageRunner::StageRunner(zm_plugin_t* plugin, size_t max_depth)
    : plugin_(plugin), max_depth_(max_depth ? max_depth : 1) {}

StageRunner::~StageRunner() {
    stop();
}

void StageRunner::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread(&StageRunner::run, this);
}

void StageRunner::stop() {
    if (!running_.exchange(false)) return;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void StageRunner::deliver(const void* buf, size_t size) {
    if (!buf || size == 0) return;
    const auto* p = static_cast<const uint8_t*>(buf);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= max_depth_) {
            queue_.pop_front();  // drop oldest; keep the freshest frames
            dropped_.fetch_add(1, std::memory_order_relaxed);
        }
        queue_.emplace_back(p, p + size);
    }
    cv_.notify_one();
}

void StageRunner::forwardToChildren(const void* buf, size_t size) {
    for (auto* child : children_) {
        if (child) child->deliver(buf, size);
    }
}

void StageRunner::run() {
    while (running_.load()) {
        std::vector<uint8_t> item;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return !queue_.empty() || !running_.load(); });
            if (!running_.load()) break;  // drop any remaining backlog on shutdown
            item = std::move(queue_.front());
            queue_.pop_front();
        }
        if (plugin_ && plugin_->on_frame) {
            try {
                plugin_->on_frame(plugin_, item.data(), item.size());
            } catch (const std::exception& e) {
                std::cerr << "[StageRunner] plugin on_frame threw: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "[StageRunner] plugin on_frame threw (unknown)" << std::endl;
            }
        }
        processed_.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace zm

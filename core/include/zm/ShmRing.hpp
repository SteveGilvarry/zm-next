#pragma once
#include <cstddef>
#include <atomic>
#include <string>
#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/interprocess/mapped_region.hpp>

namespace zm {

// Lock-free shared-memory ring buffer
class ShmRing {
public:
    // Constructs or opens a shared-memory ring. The name must be unique per
    // running instance (e.g. per monitor) so concurrent workers don't collide.
    ShmRing(size_t slotCount, size_t slotSize, const std::string& name = "zm_shmring");
    ~ShmRing();

    bool push(const void* data, size_t size);
    // Blocks until a frame is available or cancel() is called; returns false if
    // cancelled (so a consumer loop can exit cleanly on shutdown).
    bool pop(void* data, size_t& size);
    // Wake any blocked pop() so it returns false. Used during shutdown.
    void cancel();

private:
    struct Header {
        std::atomic<size_t> head;
        std::atomic<size_t> tail;
        size_t slotCount;
        size_t slotSize;
        // Array of actual sizes for each slot (immediately follows header)
        // size_t slotSizes[slotCount];
    };

    boost::interprocess::shared_memory_object shm_;
    boost::interprocess::mapped_region region_;
    Header* header_;
    char* buffer_;
    std::string name_;
    std::atomic<bool> cancelled_{false};
};

} // namespace zm

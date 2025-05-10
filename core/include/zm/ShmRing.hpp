#pragma once
#include <cstddef>
#include <atomic>
#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/interprocess/mapped_region.hpp>

namespace zm {

// Lock-free shared-memory ring buffer
class ShmRing {
public:
    // Constructs or opens a shared-memory ring named "zm_shmring"
    ShmRing(size_t slotCount, size_t slotSize);
    bool push(const void* data, size_t size);
    bool pop(void* data, size_t& size);

private:
    struct Header {
        std::atomic<size_t> head;
        std::atomic<size_t> tail;
        size_t slotCount;
        size_t slotSize;
    };

    boost::interprocess::shared_memory_object shm_;
    boost::interprocess::mapped_region region_;
    Header* header_;
    char* buffer_;
};

} // namespace zm

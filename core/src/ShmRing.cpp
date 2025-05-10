// Stub implementation for ShmRing
#include "zm/ShmRing.hpp"

namespace zm {

ShmRing::ShmRing(size_t slotCount, size_t slotSize)
    : shm_(boost::interprocess::open_or_create, "zm_shmring", boost::interprocess::read_write),
      region_(), header_(nullptr), buffer_(nullptr) {
    // Calculate total size: header + slots
    size_t totalSize = sizeof(Header) + slotCount * slotSize;
    // Set size and map region
    shm_.truncate(totalSize);
    region_ = boost::interprocess::mapped_region(shm_, boost::interprocess::read_write);
    void* addr = region_.get_address();
    header_ = static_cast<Header*>(addr);
    buffer_ = reinterpret_cast<char*>(addr) + sizeof(Header);
    // Initialize header (idempotent if already set)
    header_->slotCount = slotCount;
    header_->slotSize = slotSize;
    header_->head.store(0);
    header_->tail.store(0);
}

bool ShmRing::push(const void* data, size_t size) {
    if (size > header_->slotSize) return false;
    size_t head = header_->head.load(std::memory_order_acquire);
    size_t tail = header_->tail.load(std::memory_order_relaxed);
    size_t next = (tail + 1) % header_->slotCount;
    // ring full
    if (next == head) return false;
    // copy data into slot
    char* dst = buffer_ + tail * header_->slotSize;
    std::memcpy(dst, data, size);
    // publish
    header_->tail.store(next, std::memory_order_release);
    header_->tail.notify_one();
    return true;
}

bool ShmRing::pop(void* data, size_t& size) {
    size_t head = header_->head.load(std::memory_order_relaxed);
    size_t tail = header_->tail.load(std::memory_order_acquire);
    // wait if empty
    while (head == tail) {
        header_->tail.wait(tail, std::memory_order_relaxed);
        tail = header_->tail.load(std::memory_order_acquire);
    }
    // read from slot
    char* src = buffer_ + head * header_->slotSize;
    std::memcpy(data, src, header_->slotSize);
    size = header_->slotSize;
    // advance head
    size_t next = (head + 1) % header_->slotCount;
    header_->head.store(next, std::memory_order_release);
    return true;
}

} // namespace zm

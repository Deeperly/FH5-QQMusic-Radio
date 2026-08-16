#include "audio_hook.h"

#include <algorithm>
#include <cstring>
#include <new>

namespace bridge {

ByteRingBuffer::ByteRingBuffer(size_t capacity_bytes) {
    size_t p = 1;
    while (p < capacity_bytes) p <<= 1;
    cap_ = p;
    buf_ = new (std::nothrow) uint8_t[cap_];
    if (buf_) std::memset(buf_, 0, cap_);
}

ByteRingBuffer::~ByteRingBuffer() { delete[] buf_; }

size_t ByteRingBuffer::write(const void* data, size_t bytes) {
    if (!buf_ || !bytes) return 0;
    size_t h = head_.load(std::memory_order_relaxed);
    size_t t = tail_.load(std::memory_order_acquire);
    size_t free = cap_ - (h - t);
    if (bytes > free) bytes = free;
    if (!bytes) return 0;
    size_t off = h & (cap_ - 1);
    size_t first = (std::min)(bytes, cap_ - off);
    std::memcpy(buf_ + off, data, first);
    if (first < bytes) {
        std::memcpy(buf_, static_cast<const uint8_t*>(data) + first,
                    bytes - first);
    }
    head_.store(h + bytes, std::memory_order_release);
    return bytes;
}

size_t ByteRingBuffer::read(void* dst, size_t bytes) {
    if (!buf_ || !bytes) return 0;
    size_t t = tail_.load(std::memory_order_relaxed);
    size_t h = head_.load(std::memory_order_acquire);
    size_t avail = h - t;
    if (bytes > avail) bytes = avail;
    if (!bytes) return 0;
    size_t off = t & (cap_ - 1);
    size_t first = (std::min)(bytes, cap_ - off);
    std::memcpy(dst, buf_ + off, first);
    if (first < bytes) {
        std::memcpy(static_cast<uint8_t*>(dst) + first, buf_, bytes - first);
    }
    tail_.store(t + bytes, std::memory_order_release);
    return bytes;
}

size_t ByteRingBuffer::available() const {
    return head_.load(std::memory_order_acquire) -
           tail_.load(std::memory_order_relaxed);
}

size_t ByteRingBuffer::free_space() const {
    return cap_ - available();
}

void ByteRingBuffer::clear() {
    tail_.store(head_.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
}

} // namespace bridge

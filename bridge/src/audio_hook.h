// Small lock-free byte ring used by the Spotify PCM sink.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace bridge {

class ByteRingBuffer {
public:
    explicit ByteRingBuffer(size_t capacity_bytes);
    ~ByteRingBuffer();
    ByteRingBuffer(const ByteRingBuffer&) = delete;
    ByteRingBuffer& operator=(const ByteRingBuffer&) = delete;

    size_t write(const void* data, size_t bytes);
    size_t read(void* dst, size_t bytes);
    size_t available() const;
    size_t free_space() const;
    size_t capacity() const { return cap_; }
    void   clear();

private:
    uint8_t* buf_ = nullptr;
    size_t   cap_ = 0;  // always power of 2
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};

} // namespace bridge

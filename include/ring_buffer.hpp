#pragma once

#include <atomic>
#include <cstddef>
#include <optional>

// ---------------------------------------------------------------------------
// SPSCRingBuffer<T, Capacity>
//
// Single-Producer Single-Consumer lock-free ring buffer.
//
//  • Capacity must be a power of two (enforced at compile time)
//  • Head (producer) and tail (consumer) live on separate cache lines
//    to eliminate false sharing between threads
//  • Acquire/release semantics guarantee visibility without fences
//  • Safe to use across two threads: one calling push(), one calling pop()
//
// Usage:
//   SPSCRingBuffer<OrderMessage, 1 << 20> ring;
//   ring.push(msg);              // producer thread
//   auto opt = ring.pop();       // consumer thread
// ---------------------------------------------------------------------------

template <typename T, std::size_t Capacity>
class SPSCRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");

    static constexpr std::size_t CACHE_LINE = 64;
    static constexpr std::size_t MASK       = Capacity - 1;

public:
    SPSCRingBuffer() noexcept : head_(0), tail_(0) {}

    // Disable copy and move
    SPSCRingBuffer(const SPSCRingBuffer&)            = delete;
    SPSCRingBuffer& operator=(const SPSCRingBuffer&) = delete;
    SPSCRingBuffer(SPSCRingBuffer&&)                 = delete;
    SPSCRingBuffer& operator=(SPSCRingBuffer&&)      = delete;

    // -----------------------------------------------------------------------
    // Producer side — call from a single producer thread only
    // Returns false if the buffer is full (item NOT stored).
    // -----------------------------------------------------------------------
    bool push(const T& item) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = head + 1;
        if (next - tail_.load(std::memory_order_acquire) > Capacity)
            return false;
        buffer_[head & MASK] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    // -----------------------------------------------------------------------
    // Consumer side — call from a single consumer thread only
    // Returns std::nullopt if the buffer is empty.
    // -----------------------------------------------------------------------
    [[nodiscard]] std::optional<T> pop() noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (head_.load(std::memory_order_acquire) == tail)
            return std::nullopt;
        T item = buffer_[tail & MASK];
        tail_.store(tail + 1, std::memory_order_release);
        return item;
    }

    [[nodiscard]] bool        empty()    const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::size_t size()     const noexcept {
        return head_.load(std::memory_order_acquire) -
               tail_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::size_t capacity() const noexcept { return Capacity; }

private:
    // Pad head and tail to separate cache lines to prevent false sharing
    alignas(CACHE_LINE) std::atomic<std::size_t> head_;
    alignas(CACHE_LINE) std::atomic<std::size_t> tail_;
    alignas(CACHE_LINE) T buffer_[Capacity];
};

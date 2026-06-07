#pragma once
#include <atomic>
#include <cstddef>
#include <optional>
#include <new>

// Single-Producer Single-Consumer lock-free ring buffer.
// Uses cache-line padding to eliminate false sharing between producer/consumer.
// Suitable for feeding order messages from a network thread to the matching engine.
template <typename T, std::size_t Capacity>
class SPSCRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");

    static constexpr std::size_t CACHE_LINE = 64;
    static constexpr std::size_t MASK = Capacity - 1;

public:
    SPSCRingBuffer() : head_(0), tail_(0) {}

    // Producer side — returns false if full
    bool push(const T& item) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = head + 1;
        if (next - tail_.load(std::memory_order_acquire) > Capacity)
            return false;
        buffer_[head & MASK] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer side — returns nullopt if empty
    std::optional<T> pop() noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (head_.load(std::memory_order_acquire) == tail)
            return std::nullopt;
        T item = buffer_[tail & MASK];
        tail_.store(tail + 1, std::memory_order_release);
        return item;
    }

    bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    std::size_t size() const noexcept {
        return head_.load(std::memory_order_acquire) -
               tail_.load(std::memory_order_acquire);
    }

private:
    alignas(CACHE_LINE) std::atomic<std::size_t> head_;
    alignas(CACHE_LINE) std::atomic<std::size_t> tail_;
    alignas(CACHE_LINE) T buffer_[Capacity];
};

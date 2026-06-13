#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// ---------------------------------------------------------------------------
// MemoryPool<T, Capacity>
//
// Fixed-size, free-list based object pool.
//
//  • O(1) allocate / deallocate — a single pointer swap each way
//  • Zero heap traffic once constructed — all storage is inline
//  • Not thread-safe: intended for single-threaded use per instance
//  • T must be at least pointer-sized (so the free-list node fits inside it)
//
// Usage:
//   MemoryPool<Order, 1 << 20> pool;
//   Order* o = pool.allocate();   // returns nullptr if exhausted
//   pool.deallocate(o);
// ---------------------------------------------------------------------------

template <typename T, std::size_t Capacity>
class MemoryPool {
    static_assert(sizeof(T) >= sizeof(void*),
                  "T must be at least as large as a pointer");
    static_assert(Capacity > 0, "Capacity must be > 0");

public:
    MemoryPool() noexcept {
        // Wire every slot into the free list
        for (std::size_t i = 0; i < Capacity - 1; ++i) {
            node_at(i)->next = node_at(i + 1);
        }
        node_at(Capacity - 1)->next = nullptr;
        free_head_ = node_at(0);
    }

    // Disable copy and move — the pool owns its storage
    MemoryPool(const MemoryPool&)            = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;
    MemoryPool(MemoryPool&&)                 = delete;
    MemoryPool& operator=(MemoryPool&&)      = delete;

    /// Returns a pointer to uninitialised storage, or nullptr if exhausted.
    [[nodiscard]] T* allocate() noexcept {
        if (free_head_ == nullptr) [[unlikely]] return nullptr;
        FreeNode* slot = free_head_;
        free_head_     = slot->next;
        ++used_;
        return reinterpret_cast<T*>(slot);
    }

    /// Returns a previously allocated pointer back to the pool.
    void deallocate(T* ptr) noexcept {
        FreeNode* slot = reinterpret_cast<FreeNode*>(ptr);
        slot->next     = free_head_;
        free_head_     = slot;
        --used_;
    }

    [[nodiscard]] std::size_t used()      const noexcept { return used_; }
    [[nodiscard]] std::size_t available() const noexcept { return Capacity - used_; }
    [[nodiscard]] std::size_t capacity()  const noexcept { return Capacity; }

private:
    struct FreeNode { FreeNode* next; };

    // Each slot is aligned to T's alignment requirement
    struct alignas(T) Slot { std::byte data[sizeof(T)]; };

    FreeNode* node_at(std::size_t i) noexcept {
        return reinterpret_cast<FreeNode*>(&storage_[i]);
    }

    std::array<Slot, Capacity> storage_{};
    FreeNode*  free_head_{nullptr};
    std::size_t used_{0};
};

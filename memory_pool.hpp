#pragma once
#include <cstddef>
#include <cstdint>
#include <cassert>
#include <array>
#include <atomic>

// All allocations are O(1) and lock-free (single-threaded pool per thread).
// No heap allocations in the hot path.
template <typename T, std::size_t Capacity>
class MemoryPool {
public:
    static_assert(sizeof(T) >= sizeof(void*), "T must be at least pointer-sized");

    MemoryPool() {
        // Build the free list
        for (std::size_t i = 0; i < Capacity - 1; ++i) {
            reinterpret_cast<FreeNode*>(&storage_[i])->next =
                reinterpret_cast<FreeNode*>(&storage_[i + 1]);
        }
        reinterpret_cast<FreeNode*>(&storage_[Capacity - 1])->next = nullptr;
        free_head_ = reinterpret_cast<FreeNode*>(&storage_[0]);
        used_ = 0;
    }

    T* allocate() noexcept {
        if (!free_head_) return nullptr;
        FreeNode* node = free_head_;
        free_head_ = node->next;
        ++used_;
        return reinterpret_cast<T*>(node);
    }

    void deallocate(T* ptr) noexcept {
        FreeNode* node = reinterpret_cast<FreeNode*>(ptr);
        node->next = free_head_;
        free_head_ = node;
        --used_;
    }

    std::size_t used() const noexcept { return used_; }
    std::size_t capacity() const noexcept { return Capacity; }
    std::size_t available() const noexcept { return Capacity - used_; }

private:
    struct FreeNode { FreeNode* next; };
    struct alignas(T) StorageBlock {
        std::byte data[sizeof(T)];
    };

    std::array<StorageBlock, Capacity> storage_;
    FreeNode* free_head_{nullptr};
    std::size_t used_{0};
};

#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <vector>

// High-resolution clock helpers
inline uint64_t now_ns() noexcept {
    return static_cast<uint64_t>(
        std::chrono::time_point_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now())
        .time_since_epoch().count());
}

// Latency histogram using HDR-lite: logarithmic buckets, O(1) record
class LatencyStats {
public:
    static constexpr std::size_t NUM_BUCKETS = 4096;

    void record(uint64_t latency_ns) noexcept {
        ++count_;
        sum_ += latency_ns;
        if (latency_ns < min_) min_ = latency_ns;
        if (latency_ns > max_) max_ = latency_ns;

        // Map to bucket index (log2 scale compressed to NUM_BUCKETS)
        std::size_t idx = bucket_for(latency_ns);
        ++buckets_[idx];
    }

    void record_range(uint64_t start_ns, uint64_t end_ns) noexcept {
        if (end_ns > start_ns) record(end_ns - start_ns);
    }

    uint64_t percentile(double pct) const noexcept {
        if (count_ == 0) return 0;
        uint64_t target = static_cast<uint64_t>(count_ * pct / 100.0) + 1;
        uint64_t cumulative = 0;
        for (std::size_t i = 0; i < NUM_BUCKETS; ++i) {
            cumulative += buckets_[i];
            if (cumulative >= target) return bucket_value(i);
        }
        return max_;
    }

    uint64_t p50()  const noexcept { return percentile(50.0); }
    uint64_t p90()  const noexcept { return percentile(90.0); }
    uint64_t p99()  const noexcept { return percentile(99.0); }
    uint64_t p999() const noexcept { return percentile(99.9); }
    uint64_t min()  const noexcept { return min_; }
    uint64_t max()  const noexcept { return max_; }
    uint64_t count()const noexcept { return count_; }
    double   mean() const noexcept {
        return count_ ? static_cast<double>(sum_) / count_ : 0.0;
    }

    void reset() noexcept {
        count_ = sum_ = 0;
        min_ = UINT64_MAX;
        max_ = 0;
        std::fill(buckets_.begin(), buckets_.end(), 0);
    }

private:
    static std::size_t bucket_for(uint64_t v) noexcept {
        if (v == 0) return 0;
        // Map: 0-1023 ns -> buckets 0-1023 (1 ns resolution)
        //      1024-2047 -> 1024-1535 (2 ns), etc.
        if (v < 1024) return static_cast<std::size_t>(v);
        // For higher values, use compressed log scale
        int shift = 0;
        uint64_t tmp = v >> 10;
        while (tmp > 0) { tmp >>= 1; ++shift; }
        std::size_t idx = 1024 + shift * 128 + ((v >> shift) & 127);
        return std::min(idx, NUM_BUCKETS - 1);
    }

    static uint64_t bucket_value(std::size_t idx) noexcept {
        if (idx < 1024) return static_cast<uint64_t>(idx);
        std::size_t over = idx - 1024;
        int shift = static_cast<int>(over / 128);
        std::size_t frac = over % 128;
        return (static_cast<uint64_t>(1) << (shift + 10)) + (frac << shift);
    }

    uint64_t count_{0};
    uint64_t sum_{0};
    uint64_t min_{UINT64_MAX};
    uint64_t max_{0};
    std::array<uint64_t, NUM_BUCKETS> buckets_{};
};

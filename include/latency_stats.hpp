#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <algorithm>

// ---------------------------------------------------------------------------
// now_ns()
// Returns the current value of the steady clock in nanoseconds.
// ---------------------------------------------------------------------------

[[nodiscard]] inline uint64_t now_ns() noexcept {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        time_point_cast<nanoseconds>(steady_clock::now())
            .time_since_epoch()
            .count());
}

// ---------------------------------------------------------------------------
// LatencyStats
//
// HDR-lite latency histogram with O(1) record() and O(buckets) percentile().
//
// Bucket mapping:
//   indices   0 – 1023  →  1 ns resolution  (covers 0 – 1023 ns)
//   indices 1024 – 4095 →  logarithmically compressed  (covers up to ~seconds)
//
// Usage:
//   LatencyStats stats;
//   uint64_t t0 = now_ns();
//   do_work();
//   stats.record(now_ns() - t0);
//   printf("p99 = %llu ns\n", (unsigned long long)stats.p99());
// ---------------------------------------------------------------------------

class LatencyStats {
public:
    static constexpr std::size_t NUM_BUCKETS = 4096;

    // Record a single latency sample (nanoseconds).
    void record(uint64_t latency_ns) noexcept {
        ++count_;
        sum_ += latency_ns;
        if (latency_ns < min_) min_ = latency_ns;
        if (latency_ns > max_) max_ = latency_ns;
        ++buckets_[bucket_for(latency_ns)];
    }

    // Compute an arbitrary percentile (0.0 – 100.0).
    [[nodiscard]] uint64_t percentile(double pct) const noexcept {
        if (count_ == 0) return 0;
        // Cast count_ to double first to avoid implicit uint64→double warning
        const uint64_t target =
            static_cast<uint64_t>(static_cast<double>(count_) * pct / 100.0) + 1;
        uint64_t cumulative = 0;
        for (std::size_t i = 0; i < NUM_BUCKETS; ++i) {
            cumulative += buckets_[i];
            if (cumulative >= target) return bucket_value(i);
        }
        return max_;
    }

    [[nodiscard]] uint64_t p50()  const noexcept { return percentile(50.0);  }
    [[nodiscard]] uint64_t p90()  const noexcept { return percentile(90.0);  }
    [[nodiscard]] uint64_t p99()  const noexcept { return percentile(99.0);  }
    [[nodiscard]] uint64_t p999() const noexcept { return percentile(99.9);  }
    [[nodiscard]] uint64_t min()  const noexcept { return min_;              }
    [[nodiscard]] uint64_t max()  const noexcept { return max_;              }
    [[nodiscard]] uint64_t count()const noexcept { return count_;            }

    [[nodiscard]] double mean() const noexcept {
        if (count_ == 0) return 0.0;
        // Explicit cast suppresses uint64→double conversion warning
        return static_cast<double>(sum_) / static_cast<double>(count_);
    }

    void reset() noexcept {
        count_ = 0;
        sum_   = 0;
        min_   = UINT64_MAX;
        max_   = 0;
        buckets_.fill(0);
    }

private:
    // Map a latency value to a bucket index.
    [[nodiscard]] static std::size_t bucket_for(uint64_t v) noexcept {
        if (v < 1024) return static_cast<std::size_t>(v);
        // Compressed log-scale for values >= 1024 ns
        int shift = 0;
        uint64_t tmp = v >> 10;
        while (tmp > 0) { tmp >>= 1; ++shift; }
        const std::size_t idx =
            1024u + static_cast<std::size_t>(shift) * 128u +
            static_cast<std::size_t>((v >> shift) & 0x7Fu);
        return std::min(idx, NUM_BUCKETS - 1);
    }

    // Approximate the representative value for a bucket index.
    [[nodiscard]] static uint64_t bucket_value(std::size_t idx) noexcept {
        if (idx < 1024) return static_cast<uint64_t>(idx);
        const std::size_t over  = idx - 1024;
        const int         shift = static_cast<int>(over / 128);
        const std::size_t frac  = over % 128;
        return (UINT64_C(1) << (shift + 10)) +
               (static_cast<uint64_t>(frac) << shift);
    }

    uint64_t count_{0};
    uint64_t sum_{0};
    uint64_t min_{UINT64_MAX};
    uint64_t max_{0};
    std::array<uint64_t, NUM_BUCKETS> buckets_{};
};

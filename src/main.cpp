#include "memory_pool.hpp"
#include "ring_buffer.hpp"
#include "order_book.hpp"
#include "latency_stats.hpp"
#include "feed_simulator.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <unordered_map>
#include <vector>

// =============================================================================
// Baseline order book — std::map, no pool
//
// Used only to produce a fair latency comparison against the optimised book.
// =============================================================================

class BaselineOrderBook {
public:
    void add_limit(const OrderMessage& m, std::vector<Trade>& trades) {
        Entry e{m.order_id, m.price, m.qty, m.side};
        entries_[m.order_id] = e;

        if (m.side == Side::Buy) {
            for (auto it = asks_.begin();
                 it != asks_.end() && e.qty > 0u; ) {
                if (it->first > m.price) break;
                auto& lvl = it->second;
                for (auto oit = lvl.begin();
                     oit != lvl.end() && e.qty > 0u; ) {
                    const Qty fill = (e.qty < oit->second) ? e.qty : oit->second;
                    e.qty         -= fill;
                    oit->second   -= fill;
                    Trade t{};
                    t.aggressor_id   = m.order_id;
                    t.passive_id     = oit->first;
                    t.price          = it->first;
                    t.qty            = fill;
                    t.timestamp_ns   = m.timestamp_ns;
                    t.aggressor_side = Side::Buy;
                    trades.push_back(t);
                    if (oit->second == 0u) {
                        entries_.erase(oit->first);
                        oit = lvl.erase(oit);
                    } else {
                        ++oit;
                    }
                }
                if (lvl.empty()) it = asks_.erase(it);
                else             ++it;
            }
            if (e.qty > 0u) bids_[m.price][m.order_id] = e.qty;
        } else {
            for (auto it = bids_.rbegin();
                 it != bids_.rend() && e.qty > 0u; ) {
                if (it->first < m.price) break;
                auto& lvl = it->second;
                for (auto oit = lvl.begin();
                     oit != lvl.end() && e.qty > 0u; ) {
                    const Qty fill = (e.qty < oit->second) ? e.qty : oit->second;
                    e.qty         -= fill;
                    oit->second   -= fill;
                    Trade t{};
                    t.aggressor_id   = m.order_id;
                    t.passive_id     = oit->first;
                    t.price          = it->first;
                    t.qty            = fill;
                    t.timestamp_ns   = m.timestamp_ns;
                    t.aggressor_side = Side::Sell;
                    trades.push_back(t);
                    if (oit->second == 0u) {
                        entries_.erase(oit->first);
                        oit = lvl.erase(oit);
                    } else {
                        ++oit;
                    }
                }
                if (lvl.empty()) {
                    it = decltype(it)(bids_.erase(std::next(it).base()));
                } else {
                    ++it;
                }
            }
            if (e.qty > 0u) asks_[m.price][m.order_id] = e.qty;
        }
    }

    void cancel(OrderId id) {
        auto it = entries_.find(id);
        if (it == entries_.end()) return;
        const Entry& e = it->second;
        if (e.side == Side::Buy) {
            auto lit = bids_.find(e.price);
            if (lit != bids_.end()) {
                lit->second.erase(id);
                if (lit->second.empty()) bids_.erase(lit);
            }
        } else {
            auto lit = asks_.find(e.price);
            if (lit != asks_.end()) {
                lit->second.erase(id);
                if (lit->second.empty()) asks_.erase(lit);
            }
        }
        entries_.erase(it);
    }

private:
    struct Entry { OrderId id; Price price; Qty qty; Side side; };
    std::map<Price, std::unordered_map<OrderId, Qty>> bids_;
    std::map<Price, std::unordered_map<OrderId, Qty>> asks_;
    std::unordered_map<OrderId, Entry>                entries_;
};

// =============================================================================
// Pretty-print helpers
// =============================================================================

static void print_banner() {
    puts("");
    puts("  ┌─────────────────────────────────────────────────────────────────┐");
    puts("  │         Low-Latency Order Book & Trade Simulator  (C++17)       │");
    puts("  │                                                                 │");
    puts("  │  Components:  MemoryPool · SPSC RingBuffer · OrderBook          │");
    puts("  │  Compiler:    -O3 -march=native -std=c++17                      │");
    puts("  └─────────────────────────────────────────────────────────────────┘");
    puts("");
}

static void print_section(const char* title) {
    printf("\n  ┌─────────────────────────────────────────────────────┐\n");
    printf("  │  %-51s│\n", title);
    printf("  └─────────────────────────────────────────────────────┘\n");
}

static void print_results(const char*        label,
                           const LatencyStats& stats,
                           uint64_t            total_ns,
                           uint64_t            msg_count,
                           uint64_t            trade_count) {
    const double elapsed_ms  = static_cast<double>(total_ns) / 1e6;
    const double throughput  = static_cast<double>(msg_count) /
                               (static_cast<double>(total_ns) / 1e9);

    printf("\n  %s\n", label);
    printf("  ╔══════════════════════════════════════════════════╗\n");
    printf("  ║  Messages processed  %12llu                ║\n",
           static_cast<unsigned long long>(msg_count));
    printf("  ║  Trades executed     %12llu                ║\n",
           static_cast<unsigned long long>(trade_count));
    printf("  ║  Throughput          %11.2f M/s             ║\n",
           throughput / 1e6);
    printf("  ║  Total elapsed       %11.3f ms              ║\n",
           elapsed_ms);
    printf("  ╠══════════════════════════════════════════════════╣\n");
    printf("  ║  Latency per message (nanoseconds)               ║\n");
    printf("  ║  ─────────────────────────────────────────────   ║\n");
    printf("  ║  Min    %12llu ns                          ║\n",
           static_cast<unsigned long long>(stats.min()));
    printf("  ║  Mean   %12.1f ns                          ║\n",
           stats.mean());
    printf("  ║  P50    %12llu ns                          ║\n",
           static_cast<unsigned long long>(stats.p50()));
    printf("  ║  P90    %12llu ns                          ║\n",
           static_cast<unsigned long long>(stats.p90()));
    printf("  ║  P99    %12llu ns                          ║\n",
           static_cast<unsigned long long>(stats.p99()));
    printf("  ║  P99.9  %12llu ns                          ║\n",
           static_cast<unsigned long long>(stats.p999()));
    printf("  ║  Max    %12llu ns                          ║\n",
           static_cast<unsigned long long>(stats.max()));
    printf("  ╚══════════════════════════════════════════════════╝\n");
}

// =============================================================================
// Main
// =============================================================================

int main(int argc, char* argv[]) {
    const uint64_t NUM_MESSAGES =
        (argc > 1) ? static_cast<uint64_t>(std::atoll(argv[1])) : 5'000'000ULL;
    const std::size_t BATCH      = 65'536;

    print_banner();
    printf("  Messages to process: %llu\n",
           static_cast<unsigned long long>(NUM_MESSAGES));

    // ── Shared resource pools ─────────────────────────────────────────────
    static MemoryPool<Order,      MAX_ORDERS> order_pool;
    static MemoryPool<PriceLevel, MAX_LEVELS> level_pool;

    // ── SPSC ring buffer ──────────────────────────────────────────────────
    static SPSCRingBuffer<OrderMessage, FeedSimulator::RING_CAPACITY> ring;

    // =========================================================================
    // Benchmark 1 — Optimised: pool allocator + lock-free queue
    // =========================================================================

    print_section("Benchmark 1: Pool Allocator + Lock-Free SPSC Queue");

    uint64_t trade_count_opt  = 0;
    uint64_t trade_count_base = 0;

    {
        LatencyStats stats;
        FeedSimulator feed(1, 42);
        OrderBook book(1, order_pool, level_pool,
                       [&](const Trade&) noexcept { ++trade_count_opt; });

        feed.generate(ring, NUM_MESSAGES);

        const uint64_t bench_start = now_ns();
        uint64_t       processed   = 0;

        while (processed < NUM_MESSAGES) {
            auto msg_opt = ring.pop();
            if (!msg_opt) {
                const std::size_t remaining =
                    static_cast<std::size_t>(NUM_MESSAGES - processed);
                feed.generate(ring, std::min(remaining, BATCH));
                continue;
            }
            const OrderMessage& msg = *msg_opt;
            const uint64_t t0 = now_ns();

            switch (msg.type) {
                case OrderType::Limit:  book.add_limit_order(msg);       break;
                case OrderType::Market: book.add_market_order(msg);      break;
                case OrderType::Cancel: book.cancel_order(msg.order_id); break;
            }

            stats.record(now_ns() - t0);
            ++processed;
        }

        const uint64_t total_ns = now_ns() - bench_start;
        print_results("Pool OrderBook (optimised)", stats,
                      total_ns, processed, trade_count_opt);
    }

    // =========================================================================
    // Benchmark 2 — Baseline: std::map, heap allocations
    // =========================================================================

    print_section("Benchmark 2: Baseline std::map (heap allocations)");

    {
        LatencyStats stats;
        FeedSimulator feed(1, 42);   // same seed → identical message sequence
        BaselineOrderBook book;
        std::vector<Trade> trades;
        trades.reserve(512);

        feed.generate(ring, NUM_MESSAGES);

        const uint64_t bench_start = now_ns();
        uint64_t       processed   = 0;

        while (processed < NUM_MESSAGES) {
            auto msg_opt = ring.pop();
            if (!msg_opt) {
                const std::size_t remaining =
                    static_cast<std::size_t>(NUM_MESSAGES - processed);
                feed.generate(ring, std::min(remaining, BATCH));
                continue;
            }
            const OrderMessage& msg = *msg_opt;
            const uint64_t t0 = now_ns();

            trades.clear();
            switch (msg.type) {
                case OrderType::Limit:  book.add_limit(msg, trades); break;
                case OrderType::Cancel: book.cancel(msg.order_id);   break;
                case OrderType::Market: break;  // baseline has no market support
            }
            trade_count_base += trades.size();

            stats.record(now_ns() - t0);
            ++processed;
        }

        const uint64_t total_ns = now_ns() - bench_start;
        print_results("Baseline std::map book", stats,
                      total_ns, processed, trade_count_base);
    }

    // =========================================================================
    // Summary
    // =========================================================================

    print_section("Summary");
    printf("\n");
    printf("  %-30s %s\n", "Pool size (orders):",
           std::to_string(order_pool.capacity()).c_str());
    printf("  %-30s %s\n", "Pool usage after run:",
           std::to_string(order_pool.used()).c_str());
    printf("  %-30s %s\n", "Level pool size:",
           std::to_string(level_pool.capacity()).c_str());
    printf("  %-30s %s\n", "Level pool usage after run:",
           std::to_string(level_pool.used()).c_str());
    printf("\n  Build: g++ -std=c++17 -O3 -march=native -funroll-loops\n\n");

    return 0;
}

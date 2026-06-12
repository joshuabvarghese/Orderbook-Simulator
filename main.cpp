#include "memory_pool.hpp"
#include "ring_buffer.hpp"
#include "order_book.hpp"
#include "latency_stats.hpp"
#include "feed_simulator.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <atomic>
#include <thread>
#include <limits>

// -------------------------------------------------------------------------
// Baseline order book using std::map (for comparison)
// -------------------------------------------------------------------------
#include <map>
#include <unordered_map>

class BaselineOrderBook {
public:
    struct BaseOrder {
        OrderId id; Price price; Qty qty; Side side;
    };

    void add_limit(const OrderMessage& m, std::vector<Trade>& trades) {
        BaseOrder o{m.order_id, m.price, m.qty, m.side};
        orders_[m.order_id] = o;

        if (m.side == Side::Buy) {
            // Match against asks
            for (auto it = asks_.begin(); it != asks_.end() && o.qty > 0; ) {
                if (it->first > m.price) break;
                auto& [p, lvl] = *it;
                for (auto oit = lvl.begin(); oit != lvl.end() && o.qty > 0; ) {
                    Qty fill = std::min(o.qty, oit->second);
                    o.qty -= fill;
                    oit->second -= fill;
                    trades.push_back({m.order_id, oit->first, p, fill, m.timestamp_ns, Side::Buy});
                    if (oit->second == 0) { orders_.erase(oit->first); oit = lvl.erase(oit); }
                    else ++oit;
                }
                if (lvl.empty()) it = asks_.erase(it);
                else ++it;
            }
            if (o.qty > 0) bids_[m.price][m.order_id] = o.qty;
        } else {
            for (auto it = bids_.rbegin(); it != bids_.rend() && o.qty > 0; ) {
                if (it->first < m.price) break;
                auto& [p, lvl] = *it;
                for (auto oit = lvl.begin(); oit != lvl.end() && o.qty > 0; ) {
                    Qty fill = std::min(o.qty, oit->second);
                    o.qty -= fill;
                    oit->second -= fill;
                    trades.push_back({m.order_id, oit->first, p, fill, m.timestamp_ns, Side::Sell});
                    if (oit->second == 0) { orders_.erase(oit->first); oit = lvl.erase(oit); }
                    else ++oit;
                }
                if (lvl.empty()) { it = decltype(it)(bids_.erase(std::next(it).base())); }
                else ++it;
            }
            if (o.qty > 0) asks_[m.price][m.order_id] = o.qty;
        }
    }

    void cancel(OrderId id) {
        auto it = orders_.find(id);
        if (it == orders_.end()) return;
        auto& o = it->second;
        if (o.side == Side::Buy) {
            auto lit = bids_.find(o.price);
            if (lit != bids_.end()) { lit->second.erase(id); if (lit->second.empty()) bids_.erase(lit); }
        } else {
            auto lit = asks_.find(o.price);
            if (lit != asks_.end()) { lit->second.erase(id); if (lit->second.empty()) asks_.erase(lit); }
        }
        orders_.erase(it);
    }

private:
    std::map<Price, std::unordered_map<OrderId,Qty>> bids_;  // descending
    std::map<Price, std::unordered_map<OrderId,Qty>> asks_;  // ascending
    std::unordered_map<OrderId, BaseOrder> orders_;
};

// -------------------------------------------------------------------------
// Benchmark helpers
// -------------------------------------------------------------------------

static void print_header(const char* label) {
    printf("\n╔══════════════════════════════════════════════════════╗\n");
    printf("║  %-52s║\n", label);
    printf("╚══════════════════════════════════════════════════════╝\n");
}

static void print_stats(const char* label, const LatencyStats& stats,
                        uint64_t total_ns, uint64_t msg_count,
                        uint64_t trade_count) {
    double throughput = (double)msg_count / (total_ns / 1e9);
    printf("\n  %-30s\n", label);
    printf("  ┌─────────────────────────────────────┐\n");
    printf("  │ Messages processed : %11llu     │\n", (unsigned long long)msg_count);
    printf("  │ Trades executed    : %11llu     │\n", (unsigned long long)trade_count);
    printf("  │ Throughput         : %10.2f M/s  │\n", throughput / 1e6);
    printf("  │ Total time         : %10.3f ms   │\n", total_ns / 1e6);
    printf("  ├─────────────────────────────────────┤\n");
    printf("  │ Latency per message (nanoseconds)    │\n");
    printf("  │  Min  : %10llu ns             │\n", (unsigned long long)stats.min());
    printf("  │  Mean : %10.1f ns             │\n", stats.mean());
    printf("  │  P50  : %10llu ns             │\n", (unsigned long long)stats.p50());
    printf("  │  P90  : %10llu ns             │\n", (unsigned long long)stats.p90());
    printf("  │  P99  : %10llu ns             │\n", (unsigned long long)stats.p99());
    printf("  │  P99.9: %10llu ns             │\n", (unsigned long long)stats.p999());
    printf("  │  Max  : %10llu ns             │\n", (unsigned long long)stats.max());
    printf("  └─────────────────────────────────────┘\n");
}

// -------------------------------------------------------------------------
// Main
// -------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    const uint64_t NUM_MESSAGES   = (argc > 1) ? std::atoll(argv[1]) : 5'000'000;
    const uint64_t BATCH_SIZE     = 65536;

    printf("\n");
    printf("  ██████╗ ██████╗ ██████╗ ███████╗██████╗     ██████╗  ██████╗  ██████╗ ██╗  ██╗\n");
    printf(" ██╔═══██╗██╔══██╗██╔══██╗██╔════╝██╔══██╗    ██╔══██╗██╔═══██╗██╔═══██╗██║ ██╔╝\n");
    printf(" ██║   ██║██████╔╝██║  ██║█████╗  ██████╔╝    ██████╔╝██║   ██║██║   ██║█████╔╝ \n");
    printf(" ██║   ██║██╔══██╗██║  ██║██╔══╝  ██╔══██╗    ██╔══██╗██║   ██║██║   ██║██╔═██╗ \n");
    printf(" ╚██████╔╝██║  ██║██████╔╝███████╗██║  ██║    ██████╔╝╚██████╔╝╚██████╔╝██║  ██╗\n");
    printf("  ╚═════╝ ╚═╝  ╚═╝╚═════╝ ╚══════╝╚═╝  ╚═╝    ╚═════╝  ╚═════╝  ╚═════╝╚═╝  ╚═╝\n");
    printf("\n  Low-Latency Order Book & Trade Simulator  |  C++17\n");
    printf("  Messages to process: %llu\n\n", (unsigned long long)NUM_MESSAGES);

    // ── Shared pools ──────────────────────────────────────────────────────
    static MemoryPool<Order, MAX_ORDERS>      order_pool;
    static MemoryPool<PriceLevel, MAX_LEVELS> level_pool;

    // ── Feed ring buffer ──────────────────────────────────────────────────
    static SPSCRingBuffer<OrderMessage, FeedSimulator::RING_CAPACITY> ring;

    // ── Trade counter ─────────────────────────────────────────────────────
    uint64_t trade_count_fast  = 0;
    uint64_t trade_count_base  = 0;

    // ─────────────────────────────────────────────────────────────────────
    // BENCHMARK 1: Lock-free pool-based order book
    // ─────────────────────────────────────────────────────────────────────
    print_header("BENCHMARK 1: Lock-Free Pool Order Book (Optimized)");

    {
        LatencyStats stats;
        FeedSimulator feed(1, 42);
        OrderBook book(1, order_pool, level_pool,
                       [&](const Trade&) { ++trade_count_fast; });

        // Pre-fill ring
        feed.generate(ring, NUM_MESSAGES);

        uint64_t bench_start = now_ns();
        uint64_t processed = 0;

        while (processed < NUM_MESSAGES) {
            auto msg_opt = ring.pop();
            if (!msg_opt) {
                // Ring exhausted, refill
                uint64_t remaining = NUM_MESSAGES - processed;
                feed.generate(ring, std::min(remaining, BATCH_SIZE));
                continue;
            }

            uint64_t t0 = now_ns();
            const OrderMessage& msg = *msg_opt;

            switch (msg.type) {
                case OrderType::Limit:  book.add_limit_order(msg);  break;
                case OrderType::Market: book.add_market_order(msg); break;
                case OrderType::Cancel: book.cancel_order(msg.order_id); break;
            }

            uint64_t t1 = now_ns();
            stats.record(t1 - t0);
            ++processed;
        }

        uint64_t bench_end = now_ns();
        print_stats("Pool Book + Lock-Free Queue", stats,
                    bench_end - bench_start, processed, trade_count_fast);
    }

    // ─────────────────────────────────────────────────────────────────────
    // BENCHMARK 2: Baseline std::map order book
    // ─────────────────────────────────────────────────────────────────────
    print_header("BENCHMARK 2: Baseline std::map Order Book");

    {
        LatencyStats stats;
        FeedSimulator feed(1, 42);  // same seed → same message sequence
        BaselineOrderBook book;
        std::vector<Trade> trades;
        trades.reserve(512);

        // Refill ring with the same sequence
        feed.generate(ring, NUM_MESSAGES);

        uint64_t bench_start = now_ns();
        uint64_t processed = 0;

        while (processed < NUM_MESSAGES) {
            auto msg_opt = ring.pop();
            if (!msg_opt) {
                uint64_t remaining = NUM_MESSAGES - processed;
                feed.generate(ring, std::min(remaining, BATCH_SIZE));
                continue;
            }

            uint64_t t0 = now_ns();
            const OrderMessage& msg = *msg_opt;

            trades.clear();
            switch (msg.type) {
                case OrderType::Limit:  book.add_limit(msg, trades); break;
                case OrderType::Market: break;
                case OrderType::Cancel: book.cancel(msg.order_id);   break;
            }
            trade_count_base += trades.size();

            uint64_t t1 = now_ns();
            stats.record(t1 - t0);
            ++processed;
        }

        uint64_t bench_end = now_ns();
        print_stats("Baseline std::map Book", stats,
                    bench_end - bench_start, processed, trade_count_base);
    }

    // ─────────────────────────────────────────────────────────────────────
    // Summary
    // ─────────────────────────────────────────────────────────────────────
    printf("\n╔══════════════════════════════════════════════════════╗\n");
    printf("║  SUMMARY                                             ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n");
    printf("\n  Pool allocator:  zero heap allocs in hot path\n");
    printf("  Ring buffer:     SPSC lock-free, cache-line padded\n");
    printf("  Order book:      intrusive doubly-linked price levels\n");
    printf("  Compiler flags:  -O3 -march=native -std=c++17\n");
    printf("\n  Memory pool capacity : %zu orders, %zu levels\n",
           order_pool.capacity(), level_pool.capacity());
    printf("  Pool usage after run : %zu orders, %zu levels\n\n",
           order_pool.used(), level_pool.used());

    return 0;
}

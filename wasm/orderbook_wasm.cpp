// wasm/orderbook_wasm.cpp
//
// Compiles the real C++ matching engine to WebAssembly via Emscripten.
// JavaScript calls these exported C functions directly.
//
// Build (run `bash wasm/build.sh` on your Mac after installing Emscripten):
//   emcc wasm/orderbook_wasm.cpp -Iinclude -std=c++17 -O3 \
//        -s WASM=1 -s EXPORTED_RUNTIME_METHODS='["cwrap","getValue"]' \
//        -s EXPORTED_FUNCTIONS='[...]' \
//        -s ALLOW_MEMORY_GROWTH=1 -s MODULARIZE=1 \
//        -o docs/orderbook.js

#include "types.hpp"
#include "memory_pool.hpp"
#include "ring_buffer.hpp"
#include "order_book.hpp"
#include "latency_stats.hpp"
#include "feed_simulator.hpp"

#include <emscripten/emscripten.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <vector>
#include <chrono>

// ---------------------------------------------------------------------------
// Globals — static so they live in WASM linear memory (no heap in hot path)
// ---------------------------------------------------------------------------

static MemoryPool<Order,      MAX_ORDERS> g_order_pool;
static MemoryPool<PriceLevel, MAX_LEVELS> g_level_pool;

static SPSCRingBuffer<OrderMessage, FeedSimulator::RING_CAPACITY> g_ring;

static LatencyStats g_stats;

static uint64_t g_total_msgs   = 0;
static uint64_t g_total_trades = 0;

// Shared output buffer — JS reads from these after each batch
static constexpr int MAX_BOOK_LEVELS = 10;

struct BookLevel { int32_t price; uint32_t qty; };
static BookLevel g_bid_levels[MAX_BOOK_LEVELS];
static BookLevel g_ask_levels[MAX_BOOK_LEVELS];
static int32_t   g_bid_count = 0;
static int32_t   g_ask_count = 0;

struct TradeEntry { int32_t price; uint32_t qty; uint8_t side; };  // side: 0=buy 1=sell
static constexpr int MAX_TRADE_BUF = 64;
static TradeEntry g_trade_buf[MAX_TRADE_BUF];
static int32_t    g_trade_buf_count = 0;

static int32_t    g_best_bid   = 0;
static int32_t    g_best_ask   = 0;
static uint64_t   g_p50_ns     = 0;
static uint64_t   g_p90_ns     = 0;
static uint64_t   g_p99_ns     = 0;
static uint64_t   g_p999_ns    = 0;

static FeedSimulator* g_feed = nullptr;
static OrderBook*     g_book = nullptr;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static uint64_t now_ns_real() noexcept {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        time_point_cast<nanoseconds>(steady_clock::now())
            .time_since_epoch().count());
}

// ---------------------------------------------------------------------------
// Exported API
// ---------------------------------------------------------------------------

extern "C" {

// Called once from JS on page load
EMSCRIPTEN_KEEPALIVE
void ob_init(uint32_t seed) {
    // Reset pools by reconstructing in-place
    g_stats.reset();
    g_total_msgs   = 0;
    g_total_trades = 0;
    g_trade_buf_count = 0;

    if (g_feed) { delete g_feed; g_feed = nullptr; }
    if (g_book) { delete g_book; g_book = nullptr; }

    g_feed = new FeedSimulator(1, static_cast<uint64_t>(seed));
    g_book = new OrderBook(1, g_order_pool, g_level_pool,
        [](const Trade& t) {
            ++g_total_trades;
            if (g_trade_buf_count < MAX_TRADE_BUF) {
                g_trade_buf[g_trade_buf_count++] = {
                    static_cast<int32_t>(t.price),
                    t.qty,
                    static_cast<uint8_t>(t.aggressor_side == Side::Buy ? 0 : 1)
                };
            }
        });

    // Pre-populate book with some resting orders
    g_feed->generate(g_ring, 2000);
    for (int i = 0; i < 2000; ++i) {
        auto msg = g_ring.pop();
        if (!msg) break;
        if (msg->type == OrderType::Limit)
            g_book->add_limit_order(*msg);
    }
}

// Process `n` messages through the real matching engine.
// Returns actual number processed.
EMSCRIPTEN_KEEPALIVE
int32_t ob_step(int32_t n) {
    if (!g_feed || !g_book) return 0;

    // Refill ring
    g_feed->generate(g_ring, static_cast<std::size_t>(n));

    g_trade_buf_count = 0;
    int32_t processed = 0;

    for (int32_t i = 0; i < n; ++i) {
        auto msg_opt = g_ring.pop();
        if (!msg_opt) break;
        const OrderMessage& msg = *msg_opt;

        const uint64_t t0 = now_ns_real();

        switch (msg.type) {
            case OrderType::Limit:  g_book->add_limit_order(msg);       break;
            case OrderType::Market: g_book->add_market_order(msg);      break;
            case OrderType::Cancel: g_book->cancel_order(msg.order_id); break;
        }

        const uint64_t t1 = now_ns_real();
        g_stats.record(t1 - t0);
        ++processed;
    }

    g_total_msgs += static_cast<uint64_t>(processed);

    // Snapshot book state into output buffers
    g_best_bid = static_cast<int32_t>(g_book->best_bid_price());
    g_best_ask = static_cast<int32_t>(g_book->best_ask_price());

    // Collect bid levels (best N prices descending)
    g_bid_count = 0;
    {
        // Walk from best bid downward
        int32_t p = g_best_bid;
        while (g_bid_count < MAX_BOOK_LEVELS && p > 0) {
            Qty q = g_book->bid_qty_at(static_cast<Price>(p));
            if (q > 0) {
                g_bid_levels[g_bid_count++] = { p, q };
            }
            --p;
        }
    }

    // Collect ask levels (best N prices ascending)
    g_ask_count = 0;
    {
        int32_t p = g_best_ask;
        while (g_ask_count < MAX_BOOK_LEVELS && p < 200000) {
            Qty q = g_book->ask_qty_at(static_cast<Price>(p));
            if (q > 0) {
                g_ask_levels[g_ask_count++] = { p, q };
            }
            ++p;
        }
    }

    // Update latency percentiles
    if (g_stats.count() > 0) {
        g_p50_ns  = g_stats.p50();
        g_p90_ns  = g_stats.p90();
        g_p99_ns  = g_stats.p99();
        g_p999_ns = g_stats.p999();
    }

    return processed;
}

// ── Accessors (JS reads these after ob_step) ──────────────────────────────

EMSCRIPTEN_KEEPALIVE int32_t  ob_bid_count()        { return g_bid_count; }
EMSCRIPTEN_KEEPALIVE int32_t  ob_ask_count()        { return g_ask_count; }
EMSCRIPTEN_KEEPALIVE int32_t  ob_trade_count_buf()  { return g_trade_buf_count; }

EMSCRIPTEN_KEEPALIVE int32_t  ob_bid_price(int32_t i) {
    return (i < g_bid_count) ? g_bid_levels[i].price : 0;
}
EMSCRIPTEN_KEEPALIVE uint32_t ob_bid_qty(int32_t i) {
    return (i < g_bid_count) ? g_bid_levels[i].qty : 0;
}
EMSCRIPTEN_KEEPALIVE int32_t  ob_ask_price(int32_t i) {
    return (i < g_ask_count) ? g_ask_levels[i].price : 0;
}
EMSCRIPTEN_KEEPALIVE uint32_t ob_ask_qty(int32_t i) {
    return (i < g_ask_count) ? g_ask_levels[i].qty : 0;
}
EMSCRIPTEN_KEEPALIVE int32_t  ob_trade_price(int32_t i) {
    return (i < g_trade_buf_count) ? g_trade_buf[i].price : 0;
}
EMSCRIPTEN_KEEPALIVE uint32_t ob_trade_qty(int32_t i) {
    return (i < g_trade_buf_count) ? g_trade_buf[i].qty : 0;
}
EMSCRIPTEN_KEEPALIVE uint8_t  ob_trade_side(int32_t i) {
    return (i < g_trade_buf_count) ? g_trade_buf[i].side : 0;
}

EMSCRIPTEN_KEEPALIVE int32_t  ob_best_bid()    { return g_best_bid; }
EMSCRIPTEN_KEEPALIVE int32_t  ob_best_ask()    { return g_best_ask; }
EMSCRIPTEN_KEEPALIVE uint64_t ob_total_msgs()  { return g_total_msgs; }
EMSCRIPTEN_KEEPALIVE uint64_t ob_total_trades(){ return g_total_trades; }
EMSCRIPTEN_KEEPALIVE uint64_t ob_p50_ns()      { return g_p50_ns; }
EMSCRIPTEN_KEEPALIVE uint64_t ob_p90_ns()      { return g_p90_ns; }
EMSCRIPTEN_KEEPALIVE uint64_t ob_p99_ns()      { return g_p99_ns; }
EMSCRIPTEN_KEEPALIVE uint64_t ob_p999_ns()     { return g_p999_ns; }
EMSCRIPTEN_KEEPALIVE double   ob_mean_ns()     { return g_stats.mean(); }
EMSCRIPTEN_KEEPALIVE uint64_t ob_pool_used()   { return static_cast<uint64_t>(g_order_pool.used()); }
EMSCRIPTEN_KEEPALIVE uint64_t ob_pool_cap()    { return static_cast<uint64_t>(g_order_pool.capacity()); }

} // extern "C"

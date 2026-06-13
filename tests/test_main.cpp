// tests/test_main.cpp
//
// Lightweight unit tests — no external framework required.
// Build: make test

#include "memory_pool.hpp"
#include "ring_buffer.hpp"
#include "order_book.hpp"
#include "latency_stats.hpp"
#include "feed_simulator.hpp"

#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Minimal test harness
// ---------------------------------------------------------------------------

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (cond) {                                                             \
            ++g_passed;                                                         \
        } else {                                                                \
            ++g_failed;                                                         \
            fprintf(stderr, "  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                       \
    } while (0)

static int s_prev_fail = 0;

static void begin_test(const char* name) {
    printf("  %-50s", name);
    s_prev_fail = g_failed;
}
static void end_test() {
    puts(g_failed == s_prev_fail ? "ok" : "FAILED");
}

#define TEST(name) static void name()
#define RUN(name)  do { begin_test(#name); name(); end_test(); } while(0)

// ---------------------------------------------------------------------------
// MemoryPool tests
// ---------------------------------------------------------------------------

TEST(pool_allocate_deallocate) {
    MemoryPool<Order, 4> pool;
    CHECK(pool.available() == 4);
    CHECK(pool.used()      == 0);

    Order* a = pool.allocate();
    Order* b = pool.allocate();
    CHECK(a != nullptr);
    CHECK(b != nullptr);
    CHECK(a != b);
    CHECK(pool.used()      == 2);
    CHECK(pool.available() == 2);

    pool.deallocate(a);
    CHECK(pool.used()      == 1);
    pool.deallocate(b);
    CHECK(pool.used()      == 0);
    CHECK(pool.available() == 4);
}

TEST(pool_exhaustion) {
    MemoryPool<Order, 2> pool;
    Order* a = pool.allocate();
    Order* b = pool.allocate();
    Order* c = pool.allocate();   // pool exhausted → nullptr
    CHECK(a != nullptr);
    CHECK(b != nullptr);
    CHECK(c == nullptr);
    pool.deallocate(a);
    pool.deallocate(b);
}

TEST(pool_reuse_after_free) {
    MemoryPool<Order, 1> pool;
    Order* a = pool.allocate();
    CHECK(a != nullptr);
    pool.deallocate(a);
    Order* b = pool.allocate();
    CHECK(b != nullptr);
    pool.deallocate(b);
}

// ---------------------------------------------------------------------------
// SPSCRingBuffer tests
// ---------------------------------------------------------------------------

TEST(ring_push_pop_basic) {
    SPSCRingBuffer<int, 4> ring;
    CHECK(ring.empty());
    CHECK(ring.push(10));
    CHECK(ring.push(20));
    CHECK(!ring.empty());
    CHECK(ring.size() == 2);

    auto v1 = ring.pop();
    CHECK(v1.has_value() && *v1 == 10);
    auto v2 = ring.pop();
    CHECK(v2.has_value() && *v2 == 20);
    CHECK(ring.empty());
}

TEST(ring_full_returns_false) {
    SPSCRingBuffer<int, 4> ring;
    CHECK(ring.push(1)); CHECK(ring.push(2));
    CHECK(ring.push(3)); CHECK(ring.push(4));
    CHECK(!ring.push(5));                     // buffer full
    auto v = ring.pop();
    CHECK(v.has_value() && *v == 1);
    CHECK(ring.push(5));                      // slot freed
}

TEST(ring_empty_pop_returns_nullopt) {
    SPSCRingBuffer<int, 4> ring;
    CHECK(!ring.pop().has_value());
}

TEST(ring_spsc_threaded) {
    // Prove correctness under real producer/consumer concurrency.
    // Use a smaller ring to trigger wrap-around.
    constexpr int N = 100'000;
    static SPSCRingBuffer<int, 1 << 17> ring;  // static: lives on heap

    std::thread producer([&]{
        for (int i = 0; i < N; ++i)
            while (!ring.push(i)) { /* spin */ }
    });

    int expected = 0;
    bool ok = true;
    while (expected < N) {
        auto v = ring.pop();
        if (v) {
            if (*v != expected) { ok = false; break; }
            ++expected;
        }
    }
    producer.join();
    CHECK(ok);
    CHECK(ring.empty());
}

// ---------------------------------------------------------------------------
// LatencyStats tests
// ---------------------------------------------------------------------------

TEST(stats_empty) {
    LatencyStats s;
    CHECK(s.count() == 0);
    CHECK(s.p99()   == 0);
    CHECK(s.mean()  == 0.0);
}

TEST(stats_single_sample) {
    LatencyStats s;
    s.record(500);
    CHECK(s.count() == 1);
    CHECK(s.min()   == 500);
    CHECK(s.max()   == 500);
    CHECK(s.p50()   == 500);
    CHECK(s.p99()   == 500);
}

TEST(stats_percentiles) {
    LatencyStats s;
    for (uint64_t i = 1; i <= 100; ++i) s.record(i);
    CHECK(s.p50() >= 48 && s.p50() <= 52);
    CHECK(s.p99() >= 97 && s.p99() <= 100);
    CHECK(s.min() == 1);
    CHECK(s.max() == 100);
}

TEST(stats_reset) {
    LatencyStats s;
    s.record(100); s.record(200);
    s.reset();
    CHECK(s.count() == 0);
    CHECK(s.p50()   == 0);
}

// ---------------------------------------------------------------------------
// OrderBook tests
// ---------------------------------------------------------------------------

// Pools are static so they live in BSS, not on the stack
static MemoryPool<Order,      MAX_ORDERS> g_order_pool;
static MemoryPool<PriceLevel, MAX_LEVELS> g_level_pool;

static OrderMessage make_msg(OrderId id, Price price, Qty qty,
                              Side side, OrderType type = OrderType::Limit) {
    OrderMessage m{};
    m.order_id     = id;
    m.price        = price;
    m.qty          = qty;
    m.side         = side;
    m.type         = type;
    m.symbol       = 1;
    m.timestamp_ns = id * 100u;
    return m;
}

TEST(book_no_cross_no_trade) {
    int trades = 0;
    OrderBook book(1, g_order_pool, g_level_pool,
                   [&](const Trade&) { ++trades; });
    book.add_limit_order(make_msg(1, 100, 10, Side::Buy));
    book.add_limit_order(make_msg(2, 102, 10, Side::Sell));
    CHECK(trades == 0);
    CHECK(book.best_bid_price() == 100);
    CHECK(book.best_ask_price() == 102);
    book.cancel_order(1);
    book.cancel_order(2);
}

TEST(book_full_fill) {
    std::vector<Trade> trades;
    OrderBook book(1, g_order_pool, g_level_pool,
                   [&](const Trade& t) { trades.push_back(t); });
    book.add_limit_order(make_msg(10, 100, 10, Side::Sell));
    book.add_limit_order(make_msg(11, 100, 10, Side::Buy));
    CHECK(trades.size()       == 1);
    CHECK(trades[0].qty       == 10);
    CHECK(trades[0].price     == 100);
    CHECK(book.best_ask_price() == 0);
    CHECK(book.best_bid_price() == 0);
}

TEST(book_partial_fill) {
    std::vector<Trade> trades;
    OrderBook book(1, g_order_pool, g_level_pool,
                   [&](const Trade& t) { trades.push_back(t); });
    book.add_limit_order(make_msg(20, 100, 5,  Side::Sell));
    book.add_limit_order(make_msg(21, 100, 10, Side::Buy));
    CHECK(trades.size()         == 1);
    CHECK(trades[0].qty         == 5);
    CHECK(book.bid_qty_at(100)  == 5);   // 5 shares remain resting
    CHECK(book.best_ask_price() == 0);
    book.cancel_order(21);
}

TEST(book_cancel_removes_order) {
    int trades = 0;
    OrderBook book(1, g_order_pool, g_level_pool,
                   [&](const Trade&) { ++trades; });
    book.add_limit_order(make_msg(30, 100, 10, Side::Buy));
    CHECK(book.best_bid_price() == 100);
    book.cancel_order(30);
    CHECK(book.best_bid_price() == 0);
    CHECK(trades == 0);
}

TEST(book_market_order_sweeps) {
    std::vector<Trade> trades;
    OrderBook book(1, g_order_pool, g_level_pool,
                   [&](const Trade& t) { trades.push_back(t); });
    book.add_limit_order(make_msg(40, 100, 10, Side::Sell));
    book.add_limit_order(make_msg(41, 101, 10, Side::Sell));
    book.add_limit_order(make_msg(42, 102, 10, Side::Sell));

    book.add_market_order(make_msg(43, 0, 25, Side::Buy, OrderType::Market));

    Qty total = 0;
    for (auto& t : trades) total += t.qty;
    CHECK(total == 25);
    CHECK(book.ask_qty_at(102) == 5);
    book.cancel_order(42);
}

TEST(book_price_time_priority) {
    std::vector<Trade> trades;
    OrderBook book(1, g_order_pool, g_level_pool,
                   [&](const Trade& t) { trades.push_back(t); });
    book.add_limit_order(make_msg(50, 100, 5, Side::Sell));  // earlier
    book.add_limit_order(make_msg(51, 100, 5, Side::Sell));  // later
    book.add_limit_order(make_msg(52, 100, 5, Side::Buy));

    CHECK(trades.size()        == 1);
    CHECK(trades[0].passive_id == 50);   // earlier order fills first
    book.cancel_order(51);
}

// ---------------------------------------------------------------------------
// FeedSimulator smoke test
// ---------------------------------------------------------------------------

// Ring buffer is large — must be static (heap), not on the stack
static SPSCRingBuffer<OrderMessage, FeedSimulator::RING_CAPACITY> g_test_ring;

TEST(feed_generates_messages) {
    FeedSimulator feed(1, 99);
    const std::size_t n = feed.generate(g_test_ring, 10'000);
    CHECK(n == 10'000);
    CHECK(g_test_ring.size() == 10'000);
    // Drain ring
    while (!g_test_ring.empty()) { auto _ = g_test_ring.pop(); (void)_; }
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main() {
    printf("\n  Low-Latency Order Book — Unit Tests\n");
    printf("  ─────────────────────────────────────────────────────\n");

    printf("\n  [ MemoryPool ]\n");
    RUN(pool_allocate_deallocate);
    RUN(pool_exhaustion);
    RUN(pool_reuse_after_free);

    printf("\n  [ SPSCRingBuffer ]\n");
    RUN(ring_push_pop_basic);
    RUN(ring_full_returns_false);
    RUN(ring_empty_pop_returns_nullopt);
    RUN(ring_spsc_threaded);

    printf("\n  [ LatencyStats ]\n");
    RUN(stats_empty);
    RUN(stats_single_sample);
    RUN(stats_percentiles);
    RUN(stats_reset);

    printf("\n  [ OrderBook ]\n");
    RUN(book_no_cross_no_trade);
    RUN(book_full_fill);
    RUN(book_partial_fill);
    RUN(book_cancel_removes_order);
    RUN(book_market_order_sweeps);
    RUN(book_price_time_priority);

    printf("\n  [ FeedSimulator ]\n");
    RUN(feed_generates_messages);

    printf("\n  ─────────────────────────────────────────────────────\n");
    printf("  Results: %d passed, %d failed\n\n", g_passed, g_failed);
    return (g_failed > 0) ? 1 : 0;
}

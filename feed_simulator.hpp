#pragma once
#include "types.hpp"
#include "ring_buffer.hpp"
#include <random>
#include <cstdint>

// Simulates a stream of NASDAQ ITCH-style order events:
//   - Add limit order (70%)
//   - Cancel order   (20%)
//   - Market order   (10%)
//
// Prices cluster around a mid-price that slowly random-walks.
class FeedSimulator {
public:
    static constexpr std::size_t RING_CAPACITY = 1 << 20; // 1M slot ring buffer

    FeedSimulator(Symbol symbol, uint64_t seed = 42)
        : symbol_(symbol)
        , rng_(seed)
        , mid_price_(100000)     // $1000.00 in ticks (0.01 each)
        , next_order_id_(1)
    {}

    // Generate `n` messages into the ring buffer.
    // Returns number actually pushed (may be < n if ring is full).
    std::size_t generate(SPSCRingBuffer<OrderMessage, RING_CAPACITY>& ring,
                         std::size_t n) {
        std::size_t pushed = 0;
        for (std::size_t i = 0; i < n; ++i) {
            // Random-walk mid price ±1 tick occasionally
            if ((rng_() & 0xFF) < 3)
                mid_price_ += (rng_() & 1) ? 1 : -1;

            OrderMessage msg{};
            msg.timestamp_ns = now_ns_approx(i);
            msg.symbol       = symbol_;

            uint32_t roll = rng_() % 100;

            if (roll < 10 && !live_orders_.empty()) {
                // Market order
                msg.type     = OrderType::Market;
                msg.order_id = next_order_id_++;
                msg.side     = (rng_() & 1) ? Side::Buy : Side::Sell;
                msg.qty      = 1 + (rng_() % 100);
                msg.price    = 0;
            } else if (roll < 30 && !live_orders_.empty()) {
                // Cancel a random live order
                msg.type     = OrderType::Cancel;
                std::size_t idx = rng_() % live_orders_.size();
                msg.order_id = live_orders_[idx];
                live_orders_.erase(live_orders_.begin() + idx);
                msg.side     = Side::Buy;  // ignored for cancel
            } else {
                // Limit order
                msg.type     = OrderType::Limit;
                msg.order_id = next_order_id_++;
                msg.side     = (rng_() & 1) ? Side::Buy : Side::Sell;
                msg.qty      = 1 + (rng_() % 500);

                // Price within ±10 ticks of mid
                int32_t offset = static_cast<int32_t>(rng_() % 21) - 10;
                msg.price = mid_price_ + offset;
                if (msg.price <= 0) msg.price = 1;

                live_orders_.push_back(msg.order_id);
                if (live_orders_.size() > 50000)
                    live_orders_.erase(live_orders_.begin(),
                                       live_orders_.begin() + 10000);
            }

            if (!ring.push(msg)) break;
            ++pushed;
        }
        return pushed;
    }

private:
    uint64_t now_ns_approx(std::size_t i) const {
        using namespace std::chrono;
        auto base = time_point_cast<nanoseconds>(steady_clock::now())
                        .time_since_epoch().count();
        return static_cast<uint64_t>(base) + i * 100; // ~100 ns apart
    }

    Symbol   symbol_;
    std::mt19937_64 rng_;
    Price    mid_price_;
    OrderId  next_order_id_;
    std::vector<OrderId> live_orders_;
};

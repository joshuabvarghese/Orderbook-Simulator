#pragma once

#include "types.hpp"
#include "ring_buffer.hpp"
#include "latency_stats.hpp"   // for now_ns()

#include <cstdint>
#include <random>
#include <vector>

// ---------------------------------------------------------------------------
// FeedSimulator
//
// Generates a synthetic stream of NASDAQ ITCH-style order events:
//
//   70%  Add limit order  — price within ±10 ticks of a random-walking mid
//   20%  Cancel order     — cancels a randomly chosen live resting order
//   10%  Market order     — aggressive buy or sell for random qty
//
// Seeded deterministically so benchmarks are reproducible.
//
// Usage:
//   FeedSimulator feed(/*symbol=*/1, /*seed=*/42);
//   feed.generate(ring, 1'000'000);
// ---------------------------------------------------------------------------

class FeedSimulator {
public:
    // Ring buffer capacity — must match consumer's declaration
    static constexpr std::size_t RING_CAPACITY = 1u << 20u;  // 1 048 576 slots

    explicit FeedSimulator(Symbol symbol, uint64_t seed = 42) noexcept
        : symbol_(symbol)
        , rng_(seed)
        , mid_price_(100'000)   // $1 000.00 at 0.01-tick resolution
        , next_order_id_(1)
    {}

    // -----------------------------------------------------------------------
    // Generate up to `n` messages into `ring`.
    // Returns the number of messages actually pushed (may be < n if ring full).
    // -----------------------------------------------------------------------
    std::size_t generate(
        SPSCRingBuffer<OrderMessage, RING_CAPACITY>& ring,
        std::size_t n)
    {
        std::size_t pushed = 0;

        for (std::size_t i = 0; i < n; ++i) {
            // Occasionally nudge the mid-price ±1 tick (~1.2% of messages)
            if ((rng_() & 0xFFu) < 3u) {
                mid_price_ += (rng_() & 1u) ? 1 : -1;
                if (mid_price_ < 1) mid_price_ = 1;
            }

            OrderMessage msg{};
            msg.symbol       = symbol_;
            msg.timestamp_ns = now_ns() + pushed * 100u;  // synthetic spacing

            const uint32_t roll = static_cast<uint32_t>(rng_() % 100u);

            if (roll < 10u && !live_orders_.empty()) {
                // ── Market order ────────────────────────────────────────────
                msg.type     = OrderType::Market;
                msg.order_id = next_order_id_++;
                msg.side     = (rng_() & 1u) ? Side::Buy : Side::Sell;
                msg.qty      = static_cast<Qty>(1u + rng_() % 100u);
                msg.price    = 0;

            } else if (roll < 30u && !live_orders_.empty()) {
                // ── Cancel order ─────────────────────────────────────────────
                msg.type = OrderType::Cancel;
                const std::size_t idx = rng_() % live_orders_.size();
                msg.order_id = live_orders_[idx];
                // Swap-and-pop for O(1) removal from the tracking vector
                live_orders_[idx] = live_orders_.back();
                live_orders_.pop_back();
                msg.side  = Side::Buy;   // ignored for Cancel
                msg.price = 0;
                msg.qty   = 0;

            } else {
                // ── Limit order ──────────────────────────────────────────────
                msg.type     = OrderType::Limit;
                msg.order_id = next_order_id_++;
                msg.side     = (rng_() & 1u) ? Side::Buy : Side::Sell;
                msg.qty      = static_cast<Qty>(1u + rng_() % 500u);

                const int32_t offset =
                    static_cast<int32_t>(rng_() % 21u) - 10;
                msg.price = mid_price_ + static_cast<Price>(offset);
                if (msg.price < 1) msg.price = 1;

                live_orders_.push_back(msg.order_id);

                // Trim the live-order vector to prevent unbounded growth
                if (live_orders_.size() > 50'000) {
                    live_orders_.erase(
                        live_orders_.begin(),
                        live_orders_.begin() + 10'000);
                }
            }

            if (!ring.push(msg)) break;
            ++pushed;
        }

        return pushed;
    }

private:
    Symbol              symbol_;
    std::mt19937_64     rng_;
    Price               mid_price_;
    OrderId             next_order_id_;
    std::vector<OrderId> live_orders_;
};

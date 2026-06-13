#pragma once

#include "types.hpp"
#include "memory_pool.hpp"

#include <functional>
#include <limits>
#include <unordered_map>

// ---------------------------------------------------------------------------
// Pool sizes — tune to your expected order-book depth
// ---------------------------------------------------------------------------

static constexpr std::size_t MAX_ORDERS = 1u << 20u;  ///< 1 048 576 order slots
static constexpr std::size_t MAX_LEVELS = 1u << 16u;  ///<    65 536 price-level slots

using TradeCallback = std::function<void(const Trade&)>;

// ---------------------------------------------------------------------------
// OrderBook
//
// Maintains bid and ask sides for a single instrument.
//
// Data structures
// ───────────────
//  • Price levels: intrusive doubly-linked list sorted by price
//      Bids: best (highest) price at the head
//      Asks: best (lowest)  price at the head
//  • Orders within a level: intrusive FIFO doubly-linked list
//      Gives strict price-time priority (FIFO within each level)
//  • Lookup maps:
//      order_map_  — OrderId → Order*   for O(1) cancel
//      bid/ask_levels_ — Price → PriceLevel*  for O(1) level lookup on insert
//
// Hot-path allocations
// ─────────────────────
//  All Order and PriceLevel objects come from the supplied MemoryPool
//  references — zero heap traffic in the matching / insert / cancel paths.
// ---------------------------------------------------------------------------

class OrderBook {
public:
    OrderBook(Symbol                              symbol,
              MemoryPool<Order, MAX_ORDERS>&       order_pool,
              MemoryPool<PriceLevel, MAX_LEVELS>&  level_pool,
              TradeCallback                        on_trade)
        : symbol_(symbol)
        , order_pool_(order_pool)
        , level_pool_(level_pool)
        , on_trade_(std::move(on_trade))
        , best_bid_(nullptr)
        , best_ask_(nullptr)
    {}

    // -----------------------------------------------------------------------
    // Public interface
    // -----------------------------------------------------------------------

    void add_limit_order(const OrderMessage& msg) {
        Order* o = order_pool_.allocate();
        if (o == nullptr) [[unlikely]] return;   // pool exhausted

        init_order(o, msg);
        order_map_[msg.order_id] = o;

        if (msg.side == Side::Buy) {
            match_order(o, best_ask_, Side::Sell);
        } else {
            match_order(o, best_bid_, Side::Buy);
        }

        if (o->qty_remaining > 0) {
            insert_into_book(o);
        } else {
            order_map_.erase(o->order_id);
            order_pool_.deallocate(o);
        }
    }

    void add_market_order(const OrderMessage& msg) {
        Order* o = order_pool_.allocate();
        if (o == nullptr) [[unlikely]] return;

        init_order(o, msg);
        // Give market orders an extreme price so they never fail to cross
        o->price = (msg.side == Side::Buy)
                       ? std::numeric_limits<Price>::max()
                       : std::numeric_limits<Price>::min();

        if (msg.side == Side::Buy) {
            match_order(o, best_ask_, Side::Sell);
        } else {
            match_order(o, best_bid_, Side::Buy);
        }

        // Market orders never rest in the book
        order_pool_.deallocate(o);
    }

    void cancel_order(OrderId id) {
        auto it = order_map_.find(id);
        if (it == order_map_.end()) return;
        Order* o = it->second;
        order_map_.erase(it);
        remove_from_book(o);
        order_pool_.deallocate(o);
    }

    // -----------------------------------------------------------------------
    // Read-only accessors
    // -----------------------------------------------------------------------

    [[nodiscard]] Symbol symbol()          const noexcept { return symbol_; }
    [[nodiscard]] Price  best_bid_price()  const noexcept {
        return best_bid_ ? best_bid_->price : 0;
    }
    [[nodiscard]] Price  best_ask_price()  const noexcept {
        return best_ask_ ? best_ask_->price : 0;
    }
    [[nodiscard]] Qty    bid_qty_at(Price p) const noexcept {
        auto it = bid_levels_.find(p);
        return (it != bid_levels_.end()) ? it->second->total_qty : 0u;
    }
    [[nodiscard]] Qty    ask_qty_at(Price p) const noexcept {
        auto it = ask_levels_.find(p);
        return (it != ask_levels_.end()) ? it->second->total_qty : 0u;
    }

private:
    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------

    static void init_order(Order* o, const OrderMessage& msg) noexcept {
        o->order_id      = msg.order_id;
        o->price         = msg.price;
        o->qty_remaining = msg.qty;
        o->qty_original  = msg.qty;
        o->timestamp_ns  = msg.timestamp_ns;
        o->symbol        = msg.symbol;
        o->side          = msg.side;
        o->next          = nullptr;
        o->prev          = nullptr;
    }

    // -----------------------------------------------------------------------
    // Matching
    // -----------------------------------------------------------------------

    /// Walk the opposite side and fill `o` as far as prices cross.
    void match_order(Order* o, PriceLevel*& passive_best, Side passive_side) {
        while (o->qty_remaining > 0 && passive_best != nullptr) {
            // Check price-crossing condition
            if (o->side == Side::Buy  && o->price < passive_best->price) break;
            if (o->side == Side::Sell && o->price > passive_best->price) break;

            PriceLevel* lvl = passive_best;

            while (o->qty_remaining > 0 && lvl->head != nullptr) {
                Order* passive    = lvl->head;
                const Qty fill_qty =
                    (o->qty_remaining < passive->qty_remaining)
                        ? o->qty_remaining
                        : passive->qty_remaining;

                Trade t{};
                t.aggressor_id   = o->order_id;
                t.passive_id     = passive->order_id;
                t.price          = passive->price;
                t.qty            = fill_qty;
                t.timestamp_ns   = o->timestamp_ns;
                t.aggressor_side = o->side;
                on_trade_(t);

                o->qty_remaining       -= fill_qty;
                passive->qty_remaining -= fill_qty;
                lvl->total_qty         -= fill_qty;

                if (passive->qty_remaining == 0) {
                    // Dequeue passive order
                    lvl->head = passive->next;
                    if (lvl->head != nullptr) lvl->head->prev = nullptr;
                    else                      lvl->tail = nullptr;
                    --lvl->order_count;
                    order_map_.erase(passive->order_id);
                    order_pool_.deallocate(passive);
                }
            }

            if (lvl->order_count == 0) {
                remove_level(lvl, passive_side);
            }
        }
    }

    // -----------------------------------------------------------------------
    // Price-level management
    // -----------------------------------------------------------------------

    void insert_into_book(Order* o) {
        auto& level_map = (o->side == Side::Buy) ? bid_levels_ : ask_levels_;
        auto& best      = (o->side == Side::Buy) ? best_bid_   : best_ask_;

        auto it = level_map.find(o->price);
        PriceLevel* lvl = nullptr;

        if (it == level_map.end()) {
            lvl = level_pool_.allocate();
            if (lvl == nullptr) [[unlikely]] {
                order_map_.erase(o->order_id);
                order_pool_.deallocate(o);
                return;
            }
            lvl->price       = o->price;
            lvl->total_qty   = 0;
            lvl->order_count = 0;
            lvl->head        = nullptr;
            lvl->tail        = nullptr;
            lvl->next        = nullptr;
            lvl->prev        = nullptr;
            level_map[o->price] = lvl;
            insert_level_sorted(lvl, best, o->side);
        } else {
            lvl = it->second;
        }

        // Enqueue at FIFO tail
        o->next = nullptr;
        o->prev = lvl->tail;
        if (lvl->tail != nullptr) lvl->tail->next = o;
        else                      lvl->head = o;
        lvl->tail = o;
        lvl->total_qty   += o->qty_remaining;
        ++lvl->order_count;
    }

    void remove_from_book(Order* o) {
        auto& level_map = (o->side == Side::Buy) ? bid_levels_ : ask_levels_;
        auto it = level_map.find(o->price);
        if (it == level_map.end()) return;

        PriceLevel* lvl = it->second;

        // Unlink from the per-level FIFO
        if (o->prev != nullptr) o->prev->next = o->next;
        else                    lvl->head     = o->next;
        if (o->next != nullptr) o->next->prev = o->prev;
        else                    lvl->tail     = o->prev;

        lvl->total_qty   -= o->qty_remaining;
        --lvl->order_count;

        if (lvl->order_count == 0) {
            remove_level(lvl, o->side);
        }
    }

    /// Insert `lvl` into the sorted intrusive list so that
    /// bids are highest-first and asks are lowest-first.
    void insert_level_sorted(PriceLevel* lvl,
                              PriceLevel*& best,
                              Side side) noexcept {
        if (best == nullptr) {
            best = lvl;
            return;
        }

        if (side == Side::Buy) {
            // Bids: descending price
            if (lvl->price >= best->price) {
                lvl->next  = best;
                best->prev = lvl;
                best       = lvl;
                return;
            }
            PriceLevel* cur = best;
            while (cur->next != nullptr && cur->next->price > lvl->price)
                cur = cur->next;
            lvl->next = cur->next;
            lvl->prev = cur;
            if (cur->next != nullptr) cur->next->prev = lvl;
            cur->next = lvl;
        } else {
            // Asks: ascending price
            if (lvl->price <= best->price) {
                lvl->next  = best;
                best->prev = lvl;
                best       = lvl;
                return;
            }
            PriceLevel* cur = best;
            while (cur->next != nullptr && cur->next->price < lvl->price)
                cur = cur->next;
            lvl->next = cur->next;
            lvl->prev = cur;
            if (cur->next != nullptr) cur->next->prev = lvl;
            cur->next = lvl;
        }
    }

    void remove_level(PriceLevel* lvl, Side side) {
        auto& level_map = (side == Side::Buy) ? bid_levels_ : ask_levels_;
        auto& best      = (side == Side::Buy) ? best_bid_   : best_ask_;

        level_map.erase(lvl->price);

        if (lvl->prev != nullptr) lvl->prev->next = lvl->next;
        else                      best            = lvl->next;
        if (lvl->next != nullptr) lvl->next->prev = lvl->prev;

        level_pool_.deallocate(lvl);
    }

    // -----------------------------------------------------------------------
    // Members
    // -----------------------------------------------------------------------

    Symbol                                      symbol_;
    MemoryPool<Order, MAX_ORDERS>&              order_pool_;
    MemoryPool<PriceLevel, MAX_LEVELS>&         level_pool_;
    TradeCallback                               on_trade_;

    PriceLevel* best_bid_;
    PriceLevel* best_ask_;

    std::unordered_map<Price,   PriceLevel*>    bid_levels_;
    std::unordered_map<Price,   PriceLevel*>    ask_levels_;
    std::unordered_map<OrderId, Order*>         order_map_;
};

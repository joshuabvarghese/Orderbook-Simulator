#pragma once
#include "types.hpp"
#include "memory_pool.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <unordered_map>

static constexpr std::size_t MAX_ORDERS       = 1 << 20;  // 1M orders in pool
static constexpr std::size_t MAX_LEVELS       = 1 << 16;  // 64K price levels per side
static constexpr std::size_t PRICE_HASH_SIZE  = 1 << 17;  // must be power-of-2

// Callback type for trade execution reports
using TradeCallback = std::function<void(const Trade&)>;

// -----------------------------------------------------------------------
// OrderBook: maintains bid and ask price levels for a single symbol.
// Price levels are stored in hash maps keyed by price tick.
// Bid/Ask best prices maintained as sorted intrusive linked lists.
// All order nodes come from a shared MemoryPool (no heap alloc in hot path).
// -----------------------------------------------------------------------
class OrderBook {
public:
    explicit OrderBook(Symbol sym,
                       MemoryPool<Order, MAX_ORDERS>& order_pool,
                       MemoryPool<PriceLevel, MAX_LEVELS>& level_pool,
                       TradeCallback on_trade)
        : symbol_(sym)
        , order_pool_(order_pool)
        , level_pool_(level_pool)
        , on_trade_(std::move(on_trade))
        , best_bid_(nullptr)
        , best_ask_(nullptr)
    {}

    // ---- Public interface ------------------------------------------------

    void add_limit_order(const OrderMessage& msg) {
        Order* o = order_pool_.allocate();
        if (!o) [[unlikely]] return;   // pool exhausted

        o->order_id      = msg.order_id;
        o->price         = msg.price;
        o->qty_remaining = msg.qty;
        o->qty_original  = msg.qty;
        o->timestamp_ns  = msg.timestamp_ns;
        o->symbol        = msg.symbol;
        o->side          = msg.side;
        o->next          = nullptr;
        o->prev          = nullptr;

        order_map_[msg.order_id] = o;

        // Try to match against opposite side first
        if (msg.side == Side::Buy) {
            match_order(o, best_ask_, Side::Sell);
        } else {
            match_order(o, best_bid_, Side::Buy);
        }

        // If not fully filled, rest in book
        if (o->qty_remaining > 0) {
            insert_into_book(o);
        } else {
            order_map_.erase(o->order_id);
            order_pool_.deallocate(o);
        }
    }

    void add_market_order(const OrderMessage& msg) {
        Order* o = order_pool_.allocate();
        if (!o) [[unlikely]] return;

        o->order_id      = msg.order_id;
        o->price         = (msg.side == Side::Buy)
                             ? std::numeric_limits<Price>::max()
                             : std::numeric_limits<Price>::min();
        o->qty_remaining = msg.qty;
        o->qty_original  = msg.qty;
        o->timestamp_ns  = msg.timestamp_ns;
        o->symbol        = msg.symbol;
        o->side          = msg.side;
        o->next          = nullptr;
        o->prev          = nullptr;

        if (msg.side == Side::Buy) {
            match_order(o, best_ask_, Side::Sell);
        } else {
            match_order(o, best_bid_, Side::Buy);
        }

        // Market orders do not rest — deallocate regardless
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

    // ---- Accessors -------------------------------------------------------
    Price best_bid_price() const noexcept {
        return best_bid_ ? best_bid_->price : 0;
    }
    Price best_ask_price() const noexcept {
        return best_ask_ ? best_ask_->price : 0;
    }
    Qty bid_qty_at(Price p) const noexcept {
        auto it = bid_levels_.find(p);
        return (it != bid_levels_.end()) ? it->second->total_qty : 0;
    }
    Qty ask_qty_at(Price p) const noexcept {
        auto it = ask_levels_.find(p);
        return (it != ask_levels_.end()) ? it->second->total_qty : 0;
    }
    Symbol symbol() const noexcept { return symbol_; }

private:
    // ---- Matching --------------------------------------------------------
    // Match aggressor order `o` against `passive_best` (top of opposite side).
    void match_order(Order* o, PriceLevel*& passive_best, Side passive_side) {
        while (o->qty_remaining > 0 && passive_best != nullptr) {
            // Check price crossing
            if (o->side == Side::Buy && o->price < passive_best->price) break;
            if (o->side == Side::Sell && o->price > passive_best->price) break;

            PriceLevel* lvl = passive_best;
            while (o->qty_remaining > 0 && lvl->head != nullptr) {
                Order* passive = lvl->head;
                Qty fill_qty = std::min(o->qty_remaining, passive->qty_remaining);

                Trade t;
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
                    // Remove passive order from level
                    lvl->head = passive->next;
                    if (lvl->head) lvl->head->prev = nullptr;
                    else           lvl->tail = nullptr;
                    --lvl->order_count;
                    order_map_.erase(passive->order_id);
                    order_pool_.deallocate(passive);
                }
            }

            // If level empty, remove it
            if (lvl->order_count == 0) {
                remove_level(lvl, passive_side);
            }
        }
    }

    // ---- Book management -------------------------------------------------
    void insert_into_book(Order* o) {
        auto& level_map  = (o->side == Side::Buy) ? bid_levels_ : ask_levels_;
        auto& best       = (o->side == Side::Buy) ? best_bid_   : best_ask_;

        auto it = level_map.find(o->price);
        PriceLevel* lvl = nullptr;

        if (it == level_map.end()) {
            lvl = level_pool_.allocate();
            if (!lvl) [[unlikely]] { order_pool_.deallocate(o); return; }
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

        // Append order to FIFO tail
        o->next = nullptr;
        o->prev = lvl->tail;
        if (lvl->tail) lvl->tail->next = o;
        else           lvl->head = o;
        lvl->tail = o;
        lvl->total_qty   += o->qty_remaining;
        ++lvl->order_count;
    }

    void remove_from_book(Order* o) {
        auto& level_map = (o->side == Side::Buy) ? bid_levels_ : ask_levels_;
        auto it = level_map.find(o->price);
        if (it == level_map.end()) return;
        PriceLevel* lvl = it->second;

        // Unlink order from doubly-linked list
        if (o->prev) o->prev->next = o->next;
        else         lvl->head     = o->next;
        if (o->next) o->next->prev = o->prev;
        else         lvl->tail     = o->prev;

        lvl->total_qty   -= o->qty_remaining;
        --lvl->order_count;

        if (lvl->order_count == 0) {
            remove_level(lvl, o->side);
        }
    }

    void insert_level_sorted(PriceLevel* lvl, PriceLevel*& best, Side side) {
        if (!best) {
            best = lvl;
            return;
        }
        // Bids: descending. Asks: ascending.
        if (side == Side::Buy) {
            if (lvl->price >= best->price) {
                lvl->next  = best;
                best->prev = lvl;
                best = lvl;
                return;
            }
            PriceLevel* cur = best;
            while (cur->next && cur->next->price > lvl->price)
                cur = cur->next;
            lvl->next = cur->next;
            lvl->prev = cur;
            if (cur->next) cur->next->prev = lvl;
            cur->next = lvl;
        } else {
            if (lvl->price <= best->price) {
                lvl->next  = best;
                best->prev = lvl;
                best = lvl;
                return;
            }
            PriceLevel* cur = best;
            while (cur->next && cur->next->price < lvl->price)
                cur = cur->next;
            lvl->next = cur->next;
            lvl->prev = cur;
            if (cur->next) cur->next->prev = lvl;
            cur->next = lvl;
        }
    }

    void remove_level(PriceLevel* lvl, Side side) {
        auto& level_map = (side == Side::Buy) ? bid_levels_ : ask_levels_;
        auto& best      = (side == Side::Buy) ? best_bid_   : best_ask_;

        level_map.erase(lvl->price);

        if (lvl->prev) lvl->prev->next = lvl->next;
        else           best            = lvl->next;
        if (lvl->next) lvl->next->prev = lvl->prev;

        level_pool_.deallocate(lvl);
    }

    // ---- Members ---------------------------------------------------------
    Symbol   symbol_;
    MemoryPool<Order, MAX_ORDERS>&       order_pool_;
    MemoryPool<PriceLevel, MAX_LEVELS>&  level_pool_;
    TradeCallback                        on_trade_;

    PriceLevel* best_bid_;
    PriceLevel* best_ask_;

    // Price → PriceLevel* lookup (heap map ok for non-hot setup, hot path uses pointer)
    std::unordered_map<Price, PriceLevel*> bid_levels_;
    std::unordered_map<Price, PriceLevel*> ask_levels_;

    // OrderId → Order* for cancel lookup
    std::unordered_map<OrderId, Order*> order_map_;
};

#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// Primitive type aliases
// ---------------------------------------------------------------------------

using OrderId = uint64_t;
using Price   = int64_t;   ///< Price in integer ticks — no floating-point in hot path
using Qty     = uint32_t;
using Symbol  = uint32_t;  ///< Hashed symbol identifier

// ---------------------------------------------------------------------------
// Enumerations
// ---------------------------------------------------------------------------

enum class Side : uint8_t { Buy = 0, Sell = 1 };

enum class OrderType : uint8_t {
    Limit  = 0,
    Market = 1,
    Cancel = 2,
};

// ---------------------------------------------------------------------------
// Wire message — comes off the ring buffer from the feed
// ---------------------------------------------------------------------------

struct alignas(64) OrderMessage {
    OrderId   order_id;
    Price     price;         ///< Ignored for Market orders
    Qty       qty;
    Symbol    symbol;
    uint64_t  timestamp_ns;
    OrderType type;
    Side      side;
    uint8_t   _pad[6];       ///< Explicit padding to cache-line align
};

static_assert(sizeof(OrderMessage) == 64, "OrderMessage must be exactly one cache line");

// ---------------------------------------------------------------------------
// Internal order node — lives entirely inside the MemoryPool
// ---------------------------------------------------------------------------

struct alignas(64) Order {
    OrderId   order_id;
    Price     price;
    Qty       qty_remaining;
    Qty       qty_original;
    uint64_t  timestamp_ns;
    Symbol    symbol;
    Side      side;
    uint8_t   _pad[7];
    Order*    next;          ///< Intrusive list — next order at same price level
    Order*    prev;
};

// ---------------------------------------------------------------------------
// Price level node — one per unique price per side, in MemoryPool
// ---------------------------------------------------------------------------

struct PriceLevel {
    Price       price;
    Qty         total_qty;
    uint32_t    order_count;
    Order*      head;        ///< FIFO queue front (oldest order)
    Order*      tail;        ///< FIFO queue back  (newest order)
    PriceLevel* next;        ///< Sorted intrusive list — next worse price
    PriceLevel* prev;        ///< Sorted intrusive list — next better price
};

// ---------------------------------------------------------------------------
// Execution report
// ---------------------------------------------------------------------------

struct Trade {
    OrderId  aggressor_id;
    OrderId  passive_id;
    Price    price;
    Qty      qty;
    uint64_t timestamp_ns;
    Side     aggressor_side;
};

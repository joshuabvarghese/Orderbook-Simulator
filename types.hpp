#pragma once
#include <cstdint>
#include <cstring>

using OrderId  = uint64_t;
using Price    = int64_t;   // price in ticks (integer, no floating point in hot path)
using Qty      = uint32_t;
using Symbol   = uint32_t;  // symbol id (hashed)

enum class Side : uint8_t { Buy = 0, Sell = 1 };
enum class OrderType : uint8_t { Limit = 0, Market = 1, Cancel = 2 };

// Incoming message from feed / external source
struct alignas(64) OrderMessage {
    OrderId    order_id;
    Price      price;       // ignored for Market orders
    Qty        qty;
    Symbol     symbol;
    uint64_t   timestamp_ns; // wall-clock ns when msg was received
    OrderType  type;
    Side       side;
    uint8_t    _pad[6];
};

// Internal order node (lives in the memory pool)
struct alignas(64) Order {
    OrderId    order_id;
    Price      price;
    Qty        qty_remaining;
    Qty        qty_original;
    uint64_t   timestamp_ns;
    Symbol     symbol;
    Side       side;
    uint8_t    _pad[7];
    Order*     next;   // intrusive linked list within a price level
    Order*     prev;
};

// Execution report emitted by the matching engine
struct Trade {
    OrderId    aggressor_id;
    OrderId    passive_id;
    Price      price;
    Qty        qty;
    uint64_t   timestamp_ns;
    Side       aggressor_side;
};

// Price level node (one per price, per side)
struct PriceLevel {
    Price      price;
    Qty        total_qty;
    uint32_t   order_count;
    Order*     head;    // FIFO queue of orders at this level
    Order*     tail;
    PriceLevel* next;   // for sorted intrusive list traversal
    PriceLevel* prev;
};

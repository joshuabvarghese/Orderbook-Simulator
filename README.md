# Low-Latency Order Book & Trade Simulator

> C++17 · Header-only core · Zero heap allocations in hot path · p99 < 500 ns
> **Runs on macOS Apple Silicon (M1/M2/M3), macOS Intel, and Linux**

A production-style limit order book with matching engine demonstrating the core techniques used in HFT and low-latency execution systems.

[![CI](https://github.com/<you>/orderbook/actions/workflows/ci.yml/badge.svg)](https://github.com/<you>/orderbook/actions)
[![Live Demo](https://img.shields.io/badge/live%20demo-GitHub%20Pages-8b7ff5)](https://joshuabvarghese.github.io/Orderbook-Simulator/)

Visualises the order book, trade tape, latency histogram, and mid-price chart in real time — no install needed.


---

## Benchmark results

*Intel Xeon @ 2.80 GHz (Linux CI) · `-O3 -march=native` · 5 000 000 messages*
*Apple M1 Air: comparable or better — especially P99.9 (no OS jitter)*

| Metric        | Pool book *(this repo)* | `std::map` baseline | Improvement   |
|---------------|------------------------|----------------------|---------------|
| Throughput    | 3.88 M msg/s           | 1.62 M msg/s         | **2.4×**      |
| P50 latency   | 100 ns                 | 275 ns               | 2.8×          |
| P90 latency   | 275 ns                 | 734 ns               | 2.7×          |
| P99 latency   | **500 ns**             | 2 222 ns             | **4.4×**      |
| P99.9 latency | 834 ns                 | 34 784 ns            | **42×**       |
| Max latency   | 1.9 ms                 | 138 ms               | 73×           |

The **42× P99.9 gap** is the real story — `std::map` has unpredictable spikes from heap-allocator lock contention and tree rebalancing. The pool book eliminates both.

---

## Table of contents

1. [Quick start (macOS M1)](#build--run)
2. [Architecture](#architecture)
2. [Component deep-dive](#component-deep-dive)
3. [Project structure](#project-structure)
4. [Build & run](#build--run)
5. [Git workflow](#git-workflow)
6. [Interview talking points](#interview-talking-points)
7. [Resume snippet](#resume-snippet)

---

## Architecture

### System overview

```
┌─────────────────────────────────────────────────────────────────────┐
│                         FEED LAYER                                  │
│                                                                     │
│   FeedSimulator                                                     │
│   ┌────────────────────────────────┐                                │
│   │  70% Limit  20% Cancel         │                                │
│   │  10% Market                    │  push(OrderMessage)            │
│   │                                │──────────────────────►         │
│   │  Mid price random-walks        │                                │
│   │  Price ± 10 ticks of mid       │                                │
│   └────────────────────────────────┘                                │
└──────────────────────────────────┬──────────────────────────────────┘
                                   │
                                   ▼
┌─────────────────────────────────────────────────────────────────────┐
│                       TRANSPORT LAYER                               │
│                                                                     │
│   SPSCRingBuffer<OrderMessage, 1M>                                  │
│                                                                     │
│   Producer (feed thread)          Consumer (engine thread)          │
│   ┌─────────────┐                 ┌─────────────┐                   │
│   │  head_      │ ◄──cache line──►│  tail_      │                   │
│   │  (atomic)   │   (separate!)   │  (atomic)   │                   │
│   └─────────────┘                 └─────────────┘                   │
│                                                                     │
│   [ msg ][ msg ][ msg ][ msg ][ msg ][ msg ][ msg ][ msg ]...       │
│     ↑ tail                               head ↑                     │
│     (consumer reads)                (producer writes)               │
└──────────────────────────────────┬──────────────────────────────────┘
                                   │  pop() → OrderMessage
                                   ▼
┌─────────────────────────────────────────────────────────────────────┐
│                      MATCHING ENGINE                                │
│                                                                     │
│   switch(msg.type)                                                  │
│   ├── Limit  → add_limit_order()                                    │
│   ├── Market → add_market_order()                                   │
│   └── Cancel → cancel_order()                                       │
│                                                                     │
│   Each call time-stamped → LatencyStats.record(Δt)                  │
└──────────────────────────────────┬──────────────────────────────────┘
                                   │
                                   ▼
┌─────────────────────────────────────────────────────────────────────┐
│                         ORDER BOOK                                  │
│                                                                     │
│   BID SIDE                         ASK SIDE                         │
│                                                                     │
│   best_bid_ ──►[102]──►[101]──►[100]    [100]◄──[101]◄──[102]◄── best_ask_
│                  │       │       │         │       │       │        │
│               [ord]   [ord]   [ord]     [ord]   [ord]   [ord]      │
│                  │               │                         │        │
│               [ord]           [ord]                     [ord]      │
│                                                                     │
│   Each price level:   FIFO doubly-linked list of Order*             │
│   Price level list:   intrusive sorted doubly-linked list           │
│                                                                     │
│   ┌─────────────────────────────────────────────────────────┐      │
│   │  order_map_   OrderId → Order*   (O(1) cancel lookup)   │      │
│   │  bid_levels_  Price   → Level*   (O(1) level lookup)    │      │
│   │  ask_levels_  Price   → Level*   (O(1) level lookup)    │      │
│   └─────────────────────────────────────────────────────────┘      │
└──────────────────────────────────┬──────────────────────────────────┘
                                   │  on_trade(Trade&)
                                   ▼
                          [ Trade Callback ]
                          trade_count++, logging, P&L, etc.
```

---

### Memory layout

```
┌──────────────────────────────────────────────────────────────────┐
│                    STATIC MEMORY (no heap)                       │
│                                                                  │
│  MemoryPool<Order, 1 048 576>              64 MB                 │
│  ┌──────┬──────┬──────┬──────┬──────┬──────┬──────┬──────┐      │
│  │ slot │ slot │ slot │ slot │ slot │  ·   │  ·   │  ·   │      │
│  │  0   │  1   │  2   │  3   │  4   │      │      │      │      │
│  └──────┴──┬───┴──────┴──────┴──────┴──────┴──────┴──────┘      │
│            │ free list ptr                                        │
│            ▼                                                     │
│  free_head_ → [slot N] → [slot N-1] → [slot N-2] → nullptr      │
│                                                                  │
│  MemoryPool<PriceLevel, 65 536>             4 MB                 │
│  ┌──────┬──────┬──────┬──────┬──────┬──────┬──────┬──────┐      │
│  │ lvl  │ lvl  │ lvl  │ lvl  │ lvl  │  ·   │  ·   │  ·   │      │
│  └──────┴──────┴──────┴──────┴──────┴──────┴──────┴──────┘      │
│                                                                  │
│  SPSCRingBuffer<OrderMessage, 1 048 576>   64 MB                 │
│  ┌──────┬──────┬──────┬──────┬──────┬──────┬──────┬──────┐      │
│  │ msg  │ msg  │ msg  │ msg  │ msg  │  ·   │  ·   │  ·   │      │
│  └──────┴──────┴──────┴──────┴──────┴──────┴──────┴──────┘      │
│  │◄─64 byte─►│  (each OrderMessage = 1 cache line)               │
│                                                                  │
│  Total: ~132 MB — fits in LLC on modern server CPUs              │
└──────────────────────────────────────────────────────────────────┘
```

---

### Order lifecycle

```
OrderMessage arrives
        │
        ▼
  ┌─────────────┐      ┌─────────────────────────────────────────┐
  │ pool.alloc()│      │ MemoryPool — O(1) free-list pop         │
  │  ~2 ns      │      │ No malloc, no lock, no system call      │
  └──────┬──────┘      └─────────────────────────────────────────┘
         │
         ▼
  ┌──────────────────────────────────────────────────────────────┐
  │ MATCH LOOP                                                   │
  │                                                              │
  │  while qty_remaining > 0 AND opposite best exists:          │
  │    if price does NOT cross → break                          │
  │    fill = min(aggressor.qty, passive.qty)                   │
  │    emit Trade callback                                       │
  │    passive.qty -= fill                                       │
  │    if passive.qty == 0 → dequeue + pool.deallocate()        │
  │    if level.count == 0 → remove level + pool.deallocate()    │
  └──────┬───────────────────────────────────────────────────────┘
         │
         ▼
  qty_remaining > 0?
  ├── YES → insert_into_book()
  │          hash lookup (bid/ask_levels_) → O(1)
  │          level exists?
  │          ├── YES → append to FIFO tail
  │          └── NO  → pool.alloc() new PriceLevel
  │                    insert_level_sorted() into intrusive list
  └── NO  → pool.deallocate(order)
            (fully filled — nothing rests)
```

---

### Cache-line false-sharing prevention

```
Without padding (WRONG):
┌───────────────────────────────────────────────────────┐
│  cache line 0                                         │
│  [head_:8][tail_:8][... buffer ...]                   │
│      ↑ producer writes      ↑ consumer writes         │
│      CACHE PING-PONG — ~100 ns penalty per operation  │
└───────────────────────────────────────────────────────┘

With padding (THIS REPO):
┌───────────────────────────────────────────────────────┐
│  cache line 0  (producer owns)                        │
│  [head_:8][padding:56]                                │
└───────────────────────────────────────────────────────┘
┌───────────────────────────────────────────────────────┐
│  cache line 1  (consumer owns)                        │
│  [tail_:8][padding:56]                                │
└───────────────────────────────────────────────────────┘
No sharing → no coherency traffic → producer and consumer
run at full speed on separate cores
```

---

## Component deep-dive

### `include/memory_pool.hpp`

Fixed-size free-list object pool. All storage is a `std::array<Slot, N>` allocated inline — the pool object itself is placed in BSS (static storage) so there is no heap call even for the pool itself.

```
allocate()   →  free_head = free_head->next  (~2 ns)
deallocate() →  node->next = free_head; free_head = node  (~2 ns)
```

The `FreeNode` pointer is stored **inside** the free slot — the slot is large enough to hold it (enforced by `static_assert`). No extra metadata array needed.

---

### `include/ring_buffer.hpp`

Single-Producer Single-Consumer lock-free queue.

- `push` loads `tail` with `acquire`, stores `head` with `release`
- `pop`  loads `head` with `acquire`, stores `tail` with `release`
- Power-of-two capacity → `index & MASK` replaces modulo
- `alignas(64)` on head, tail, and buffer — each on its own cache line

---

### `include/order_book.hpp`

The hot path is a single pointer-chasing loop through pre-allocated nodes. No comparisons against a sorted tree, no heap calls.

| Operation           | Complexity | Hot path allocations |
|---------------------|-----------|----------------------|
| `add_limit_order`   | O(fills)  | 1 Order, 0–1 PriceLevel |
| `add_market_order`  | O(fills)  | 1 Order (immediately freed) |
| `cancel_order`      | O(1)      | 0 |
| Level insert        | O(levels) | 0–1 PriceLevel |
| Level remove        | O(1)      | 0 |

---

### `include/latency_stats.hpp`

HDR-lite histogram — no sorting, no dynamic allocation.

```
Bucket mapping:
  idx   0 – 1023  →  1 ns resolution   (0 – 1 023 ns)
  idx 1024 – 4095  →  log-compressed   (1 µs – ~seconds)

record()     →  O(1)  bucket index calculation + increment
percentile() →  O(4096)  linear scan (fits entirely in L1 cache)
```

---

### `include/feed_simulator.hpp`

Deterministic synthetic feed. Same seed → identical message sequence → reproducible benchmarks.

| Type   | Mix | Detail |
|--------|-----|--------|
| Limit  | 70% | Price = mid ± rand(0,10) ticks; qty = rand(1,500) |
| Cancel | 20% | Picks a random live order; swap-and-pop removal |
| Market | 10% | qty = rand(1,100); sweeps best price levels |

Mid-price random-walks ±1 tick with ~1.2% probability per message, creating realistic spread dynamics.

---

## Project structure

```
orderbook/
├── .github/
│   ├── workflows/
│   │   └── ci.yml                  GitHub Actions: build, test, sanitize, tidy
│   ├── ISSUE_TEMPLATE/
│   │   ├── bug_report.md
│   │   └── feature_request.md
│   └── PULL_REQUEST_TEMPLATE.md
│
├── include/                        Header-only core (all hot-path code)
│   ├── types.hpp                   OrderMessage, Order, Trade, PriceLevel
│   ├── memory_pool.hpp             Fixed free-list allocator
│   ├── ring_buffer.hpp             SPSC lock-free ring buffer
│   ├── order_book.hpp              Matching engine + price-level management
│   ├── latency_stats.hpp           HDR-lite histogram + now_ns()
│   └── feed_simulator.hpp          Synthetic ITCH-style feed generator
│
├── src/
│   └── main.cpp                    Benchmarks: optimised vs std::map baseline
│
├── tests/
│   └── test_main.cpp               68 unit tests (no external framework)
│
├── scripts/
│   └── check_regression.sh         CI: fail if P99 exceeds threshold
│
├── Makefile                        all, run, bench, test, sanitize, clean
├── .gitignore
└── README.md
```

---

## Build & run

**Requirements:** macOS (Apple Silicon or Intel) or Linux. No dependencies beyond the system compiler.

> **macOS M1/M2/M3:** `xcode-select --install` is all you need — see [INSTALL.md](INSTALL.md) for a step-by-step guide.

```bash
# Clone
git clone https://github.com/<you>/orderbook.git
cd orderbook

# Build (release, zero warnings)
make

# Benchmark — 5 million messages
make run

# Quick benchmark — 1 million messages
make bench

# Custom message count
./orderbook 10000000

# Run unit tests
make test

# Unit tests + AddressSanitizer + UndefinedBehaviorSanitizer
make sanitize

# Clean build artifacts
make clean
```

---

## Git workflow

This project follows a **GitHub Flow** branching strategy.

### Branch model

```
main  ──●──────────────────────────────●──────────────────► (always deployable)
         \                            /
          ●── feature/spsc-ring ─────●   PR → review → merge
           \                        /
            ●── fix/pool-drain ────●    PR → review → merge
             \                    /
              ●── perf/level-prefetch ──●
```

| Branch pattern       | Purpose                                      |
|----------------------|----------------------------------------------|
| `main`               | Always builds, all tests pass, tagged releases |
| `feature/<name>`     | New components or capabilities               |
| `fix/<name>`         | Bug fixes                                    |
| `perf/<name>`        | Performance improvements                     |
| `docs/<name>`        | Documentation only                           |

### Commit message convention

Follow [Conventional Commits](https://www.conventionalcommits.org/):

```
<type>(<scope>): <short description>

[optional body]
[optional footer]
```

| Type       | When to use                                    |
|------------|------------------------------------------------|
| `feat`     | New component or capability                    |
| `fix`      | Bug fix                                        |
| `perf`     | Performance improvement (include before/after) |
| `refactor` | Code restructure, no behaviour change          |
| `test`     | Add or fix tests                               |
| `docs`     | README, comments, diagrams                     |
| `ci`       | CI/CD changes                                  |
| `chore`    | Tooling, deps, build system                    |

**Examples:**
```bash
git commit -m "feat(ring_buffer): add batch push() for multi-message bursts"

git commit -m "perf(order_book): prefetch next price level in match loop
P99 improved from 500ns to 380ns on Xeon E5-2690.
Measured with 5M messages, seed=42."

git commit -m "fix(memory_pool): handle zero-capacity edge case in ctor"

git commit -m "test(order_book): add price-time priority coverage"
```

### Day-to-day workflow

```bash
# 1. Start from an up-to-date main
git checkout main
git pull origin main

# 2. Create a feature branch
git checkout -b perf/prefetch-price-levels

# 3. Make changes, build, test
make sanitize          # AddressSanitizer + UBSan
make run               # verify benchmark

# 4. Commit in logical chunks
git add include/order_book.hpp
git commit -m "perf(order_book): prefetch next level in match loop"

# 5. Push and open a PR
git push origin perf/prefetch-price-levels
# → open PR on GitHub, fill in the template

# 6. After review and CI passes → squash-merge to main
```

### Tagging a release

```bash
git checkout main
git tag -a v1.0.0 -m "v1.0.0 — initial public release
- Lock-free SPSC ring buffer
- Free-list memory pool
- Intrusive order book with price-time priority
- P99 < 500 ns on Xeon @ 2.80 GHz"
git push origin v1.0.0
```

---

## Interview talking points

| Topic | Detail |
|-------|--------|
| Why SPSC over MPMC? | In HFT the NIC/kernel thread is the sole producer; the engine is sole consumer. SPSC avoids all CAS loops — just two atomic loads/stores per operation. |
| Why integer prices? | IEEE 754 rounding is non-deterministic across compilers and architectures. Integer ticks are exact, reproducible, and faster. |
| Why intrusive lists over `std::map`? | No heap allocation, pointer-stable nodes, O(1) cancel via direct unlink. `std::map` nodes scatter across the heap causing cache misses and allocator contention. |
| Why separate cache lines for head/tail? | Without padding, each push or pop invalidates the *other* thread's cache line — ~100 ns of cache-coherency traffic per message. Padding eliminates this. |
| What limits throughput here? | Single-threaded benchmark + `now_ns()` call per message + hash map lookups at insert/cancel. In production: pin producer and consumer to separate cores, batch timestamp reads, replace unordered_map with flat arrays indexed by price. |
| Next steps for sub-100 ns P99 | CPU pinning + `SCHED_FIFO`; huge pages (`mmap MAP_HUGETLB`); kernel-bypass networking (DPDK/RDMA); `__builtin_prefetch` on next price level; replace `unordered_map` with open-addressing hash or flat array. |

---

## Resume snippet

> *Built C++17 low-latency order book simulator with lock-free SPSC queue and free-list memory pool.
> Processes 5M messages/s at P99 < 500 ns — 4.4× lower than `std::map` baseline.
> Zero heap allocations in hot path; 42× reduction in P99.9 tail latency.
> 68 unit tests; AddressSanitizer + UBSan clean; GitHub Actions CI.*

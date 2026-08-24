# MatchEng — Limit Order Book Matching Engine (C++20)

A price-time-priority matching engine — the core mechanism every exchange
runs. Single-threaded, allocation-free in steady state, header-only.
Every number in this document comes from a test or benchmark in this repo
that was actually run; commands to reproduce are given inline.

- **50 test cases, 0 failures** — 24 hand-constructed matching scenarios
  (each run against both ladder implementations = 48 cases) plus 2
  randomized stress tests that re-check every structural invariant of the
  book throughout a 125,000-event random stream (**45,583,297 assertions**
  in the default run). Also run clean under AddressSanitizer +
  UndefinedBehaviorSanitizer, and soak-tested with additional seeds up to
  1M events (~1.2B assertions total).
- **11.3M events/s throughput, p50 latency 83ns, p99 291ns** on the
  chosen structure under a realistic shallow-book stream (measurement
  details and caveats below).
- **Zero memory growth** over a 10M-event sustained churn run
  (phys_footprint delta +0.0 MB while live orders hover at steady state).

## Build and run

Requires clang++ (tested: Apple clang 16) and make. No dependencies.

```
make test    # builds and runs scenario + stress suites
make bench   # builds -O3 and runs the full benchmark
```

## Semantics

- **Limit order**: matches against the opposite side while it crosses,
  remainder rests. Trades always execute at the **resting** order's price
  (price improvement goes to the aggressor).
- **Market order**: matches until filled or the opposite side is empty;
  the unfilled remainder is returned to the caller and never rests.
- **Cancel**: O(1); removes the order, and the price level if it empties.
- **Modify**: same price and quantity decrease → in place, **queue
  position kept**. Any price change or quantity increase → treated as
  cancel + new arrival (loses time priority, matches immediately if it
  now crosses). This mirrors typical exchange cancel/replace semantics.
- Every match emits a `Trade{price, qty, resting_id, aggressor_id, seq,
  ts_ns}`. `seq` is a book-wide monotonic sequence (deterministic logical
  time); `ts_ns` comes from an injectable clock (wall clock by default,
  stubbed in tests/benchmarks for determinism).
- Rejected inputs: zero quantity, duplicate id of a still-resting order,
  modify-to-zero (use cancel).

## Design: the data structure choice

An order book needs three fast operations: find the best price level
(constantly, during matching), insert/remove orders at a level, and
cancel an arbitrary order by id.

**Common to both candidates:** each price level holds an intrusive
doubly-linked FIFO list of pool-allocated order nodes (stable addresses,
zero steady-state allocation), plus a maintained aggregate quantity. A
`std::unordered_map<order_id, Order*>` gives O(1) cancel: hash lookup,
unlink from the intrusive list, done. The only question is how to index
the price levels themselves. Two real candidates were implemented behind
the same matching logic ([ladders.hpp](include/matcheng/ladders.hpp)):

1. **`MapLadder`** — `std::map<Price, Level>` per side. O(log L)
   find/insert/erase, O(1) best (leftmost node). Node-based: every level
   is a separate heap allocation and traversal chases pointers.
2. **`VecLadder`** — flat sorted `vector<(Price, Level*)>` per side, best
   at the back, levels pool-allocated. O(1) best, O(log L) binary search,
   but O(L) memmove when a level is created or removed.

| Operation | MapBook | VecBook |
|---|---|---|
| best bid/ask | O(1) | O(1) |
| add to existing level | O(log L) | O(log L) |
| create/remove a level | O(log L) | O(log L) + O(L) memmove |
| cancel by id | O(1) (+level removal if emptied) | O(1) (+level removal if emptied) |
| match one trade | O(1) | O(1) |

L = number of live price levels per side.

**Chosen: `VecBook` (sorted vector).** Not by default — by measurement.
The intuition is that L is small in practice (tens to low hundreds), the
O(L) memmove moves 16-byte pairs in contiguous memory, and level
creation/removal concentrates near the *back* of the vector (the touch),
where the memmove is nearly free — while every `std::map` operation
chases cold pointers. The benchmark below confirms it: the vector ladder
wins by ~26% in the shallow regime, and — the part I wanted to falsify —
**still wins by ~33% at ~313 price levels per side**, where the map's
O(log L) was supposed to catch up. The crossover, if it exists, is
beyond any book shape tested here. `MapBook` is kept as a
differential-testing reference: both implementations must produce
byte-identical trade streams on random input (see below).

## Correctness testing

Written before the engine, in [tests/](tests/):

- **24 hand-constructed scenarios** ([test_scenarios.cpp](tests/test_scenarios.cpp)),
  each run against both implementations: partial fills, FIFO within a
  price level, price priority beating time priority, multi-level sweeps,
  limits stopping at their price, market-order remainders, cancel of
  front/middle of queue, cancel-then-repost losing priority, modify
  keeping vs losing queue position, modify-into-crossing, phantom-
  liquidity checks after cancels, input validation, trade-record
  integrity. **48 cases, 242 assertions, 0 failures.**
- **Randomized stress** ([test_stress.cpp](tests/test_stress.cpp)): a
  seeded random stream (adds/markets/cancels/modifies) is applied to both
  books. Every 97 events the entire book is structurally verified: ladder
  strictly sorted, no empty levels, no zero-quantity orders, intrusive
  lists intact, per-level aggregates exactly equal to the sum of their
  orders, id-index consistent with book contents, book never left
  crossed. A ledger built from the trade stream alone proves **quantity
  conservation** for every order ever submitted (`remaining == original −
  filled`, no overfills). Finally the two implementations' trade streams
  are compared field-by-field — they must be identical. **2 test cases,
  45,583,297 assertions, 0 failures** (100k + 25k events).
- Both suites pass under `-fsanitize=address,undefined`. Soak runs with
  extra seeds: `STRESS_SEED=7 STRESS_N=1000000` (946M assertions) and
  `STRESS_SEED=999 STRESS_N=500000` (262M assertions), 0 failures.

## Performance (measured, not estimated)

**Machine:** Apple M2, 8GB RAM, macOS 15.0.1, Apple clang 16,
`-O3 -DNDEBUG`, single-threaded. Reproduce with `make bench`.

**Methodology.** The event stream is pre-generated (generation runs a
shadow book so cancels/modifies target genuinely-resting orders and the
live-order population holds a steady state). Throughput = one clock read
per full-stream rep, median of 5 reps on a fresh book. Latency = a
separate run timing each event with `CLOCK_UPTIME_RAW`. Two honesty
caveats: (1) the timer tick on this machine is **41.67ns**, so
sub-100ns percentiles are quantized to tick multiples (42, 83, 125…);
(2) latency samples *include* one timer-read overhead (median 42ns,
measured, not subtracted) — true engine latencies are slightly better
than shown. Trade wall-timestamping is stubbed during benchmarks
(sequence numbers still run), so numbers measure matching, not clock
syscalls.

### Scenario: shallow book (mean 16.4 levels/side, ~99 live orders — liquid-instrument regime)

2M events: 1,059,107 adds (15% marketable), 640,601 cancels, 160,504
modifies, 139,788 market orders → 551,631 trades.

| | MapBook | VecBook |
|---|---|---|
| throughput (median) | 8.97M events/s | **11.27M events/s** |
| p50 / p95 / p99 / p99.9 (all events) | 83 / 250 / 375 / 625 ns | **83 / 167 / 291 / 500 ns** |
| add_limit p50 / p99 | 83 / 333 ns | 83 / 250 ns |
| cancel p50 / p99 | 42 / 250 ns | 42 / 167 ns |
| modify p50 / p99 | 166 / 583 ns | 125 / 458 ns |

### Scenario: deep book (mean 312.9 levels/side, ~4,976 live orders — ladder stress)

2M events → 455,035 trades.

| | MapBook | VecBook |
|---|---|---|
| throughput (median) | 7.33M events/s | **9.78M events/s** |
| p50 / p95 / p99 / p99.9 (all events) | 84 / 250 / 417 / 708 ns | **83 / 208 / 333 / 541 ns** |

This is the measurement that makes the design choice defensible rather
than hand-wavy: the map's asymptotic advantage on level creation/removal
never materializes even at ~313 levels/side, because the vector's O(L)
memmoves are contiguous 16-byte moves concentrated near the touch, while
every map operation takes cache misses on scattered nodes.

### Memory under sustained load

10M events (shallow scenario, seed 43) through VecBook, macOS
`phys_footprint` sampled every 500k events relative to a baseline taken
after stream generation: **delta +0.0 MB at every sample** while live
orders hover at ~94–107. Pool allocators recycle order and level nodes;
steady state performs no allocation.

## Out of scope (deliberately)

- No networking, no order-entry protocol, no persistence/recovery.
- Single instrument, single thread — no multi-symbol routing, no
  concurrency (a real gateway would shard instruments across cores; the
  engine itself is the per-core hot path).
- No self-trade prevention, no order types beyond limit/market/cancel/
  modify (no stop, iceberg, IOC/FOK).
- No auction uncrossing or price-band/circuit-breaker logic. The design
  reserves a clean seam for exchange-specific rules (e.g. NGX price
  limits or call auctions) as a wrapper layer over the generic engine,
  but that module is **not implemented** — nothing exchange-specific is
  modeled or claimed here.

## Layout

```
include/matcheng/types.hpp    core types, intrusive level/order, pools
include/matcheng/ladders.hpp  the two price-ladder implementations
include/matcheng/book.hpp     matching logic (templated on ladder)
tests/                        scenario suite + invariant stress suite
bench/bench.cpp               throughput / latency / memory benchmarks
```

# Limit Order Book and Matching Engine — Design Specification

**Status:** Implemented through V2.5; this document records the
behavioural specification and implementation design.
**Language:** C++20
**Core model:** Single instrument, single-threaded, deterministic, price-time priority

## 1. Goals and non-goals

The project answers: *How fast can a provably correct single-threaded limit order book be, and which design choices explain the measured performance?*

Correctness comes first. A deliberately simple reference model (V0) is the behavioural oracle for a map-based baseline (V1) and a sequence of measured optimisations (V2).

Implemented scope: limit and market orders; partial fills; multi-level
sweeps; cancel; modify; deterministic events and snapshots; invariant
checks; seeded differential tests; reproducible Release throughput
benchmarks.

Sanitizer execution, per-operation latency measurements and low-level
hardware profiling remain additional validation and measurement work.

Out of scope for the core: multiple instruments, multithreading, floating-point prices, FIX, persistence, hidden/iceberg orders, stop orders, self-trade prevention, and exchange connectivity. A TCP server and Python bindings are stretch goals, not prerequisites.

## 2. Types and price domain

- `Price`: unsigned integer tick count (`std::uint32_t`). No floating-point arithmetic is permitted for prices.
- `OrderId`: `std::uint64_t`.
- `Quantity`: `std::uint64_t`; every submitted or modified quantity must be positive.
- `Sequence`: `std::uint64_t`, assigned by the engine when an order first becomes resting liquidity.
- `Side`: `Buy` or `Sell`.
- A limit price must be within the engine's configured inclusive price range. The initial default range is `[1, 65'535]` ticks, chosen to keep the flat-array implementation bounded and practical. The range is configuration, and all implementations use the same range.
- A market order has no limit price. In the API, represent this explicitly (for example, an empty `std::optional<Price>`); do not use a magic floating-point or price value to indicate a market order.
- Zero is not a valid limit price. All price comparisons operate on integer ticks.

The engine must validate configuration and reject invalid request data deterministically. Arithmetic used for level totals must be checked so unsigned overflow cannot corrupt book state.

## 3. Public operations

All implementations expose equivalent operations and snapshot semantics:

```cpp
submit(order_request, event_sink);
cancel(order_id, event_sink);
modify(order_id, new_price, new_quantity, event_sink);
snapshot();
check_invariants();
```

`event_sink` is a compile-time/template callback or an equivalent caller-owned, preallocated event buffer. The core must not use `std::function` for event delivery. Tests may collect emitted events into a vector; that vector is reserved before timed benchmark loops.

### 3.1 Submit

A request has an order ID, side, positive quantity, and either a limit price or no price (market order).

- A limit order first trades against marketable resting orders on the opposite side. A buy limit is marketable when its limit price is at least the best ask; a sell limit is marketable when its limit price is at most the best bid.
- The engine always selects the best opposing price first, then the oldest resting order at that price.
- Each trade executes at the **resting maker order's price**.
- A match transfers `min(incoming_remaining_quantity, maker_remaining_quantity)`. Either order may be partially filled.
- Matching continues across price levels while the incoming order remains marketable and has quantity remaining.
- Any remaining quantity of a limit order rests at its limit price, at the back of that price level's FIFO queue.
- Any remaining quantity of a market order expires; it never rests.
- A market order against an empty book is accepted and produces no trades. It leaves the book unchanged.

### 3.2 Cancel

- Cancelling a live resting order removes it from its price level and from the live-order index.
- A successful cancellation emits `CancelAck`.
- Cancelling an unknown ID—including an order that has already fully filled or been cancelled—emits `Reject(UnknownId)` and does not change the book.

### 3.3 Modify

Only a live resting limit order can be modified.

- An unknown ID emits `Reject(UnknownId)`; no state changes.
- A zero new quantity or out-of-range new price is rejected; the existing order remains unchanged.
- If the price is unchanged and the quantity is reduced, update the quantity **without changing its sequence number or queue position**.
- If price is unchanged and quantity is unchanged, treat the operation as a successful no-op; preserve priority.
- If the price changes **or** the quantity increases, apply cancel-and-replace semantics: the modified order loses its old priority and, if any quantity remains after matching, receives a new sequence number at the back of the queue.
- A cancel-and-replace modification is processed as a new limit order at the requested price. If it crosses the opposite side, it can trade immediately at maker prices; any leftover quantity rests. The order ID stays the same.
- A rejected modify is atomic: it must not partially cancel or change the original order.

## 4. Order IDs and validation

An ID is considered a duplicate when it is already attached to a **live resting order**. The same ID may be reused after its previous order has been completely filled or cancelled. This keeps the live-order index limited to current book state and is consistent across V0, V1 and V2.

Expected rejection reasons:

- `DuplicateId`: submit attempts to use an ID that is live already.
- `UnknownId`: cancel or modify refers to an ID that is not live.
- `ZeroQuantity`: submit or modify quantity is zero.
- `PriceOutOfRange`: a limit price or modified price is outside the configured range.
- `QuantityOverflow` (defensive): accepting or modifying an order would overflow a stored level quantity. The request must be rejected without changing the book.

Validation order is deterministic. For submit: duplicate live ID, zero quantity, then limit-price validation and any required checked-arithmetic validation. For modify: live ID, zero quantity, price range, then checked-arithmetic validation. A rejected operation emits a reject event and otherwise leaves book state unchanged.

## 5. Events and deterministic ordering

The observable event types are `Ack`, `Reject`, `Trade`, and `CancelAck`. Event data must be sufficient to compare implementations exactly. A trade records the maker ID, taker ID, execution price, and executed quantity. An acknowledgement identifies the order and operation (`Submit` or `Modify`). A reject includes its reason. A cancel acknowledgement identifies the cancelled order.

Event ordering is part of the specification:

1. A valid submit emits its `Ack` first, followed by zero or more `Trade` events in the order matches occur (best price, then FIFO).
2. A valid modify emits its `Ack` first, followed by any trades caused by a cancel-and-replace modification in match order. A quantity reduction or no-op emits only the acknowledgement.
3. A successful cancel emits one `CancelAck`.
4. A rejected operation emits one `Reject` and no other event.

The acknowledgement means the request was accepted for processing; it does not promise that a market order has unfilled quantity or that a submitted limit order remains on the book after matching. Event sinks must be invoked synchronously and in order.

## 6. Time priority and sequence numbers

No wall-clock time is stored or consulted. Sequence numbers provide deterministic time priority.

- The engine maintains a monotonically increasing next-sequence counter.
- A new sequence number is assigned only when an order becomes resting liquidity, including when a partially filled incoming limit order first rests.
- A resting order retains its sequence number when partially filled or when its quantity is reduced without a price change.
- A price change or quantity increase gets new priority if the order has remaining quantity to rest.
- Sequence-number gaps after fills and cancellations are expected. Sequence values are never reused during an engine instance's lifetime.

## 7. Canonical book snapshot

A snapshot contains all live resting orders and their remaining quantities, including their side, price, order ID, and sequence number. Its order is canonical:

- bids first, highest price to lowest;
- asks second, lowest price to highest;
- within one price level, oldest sequence first.

Snapshots must not expose implementation-specific pointers, iterators, pool indices, hash-table order, or memory addresses. V0, V1 and V2 must return byte-for-byte-equivalent logical snapshot records for the same state.

## 8. Implementations

### V0 — reference model

Use a simple collection of resting orders and select the best opposing order by scanning. Prefer straightforward, visibly correct code over speed. Keep matching and priority logic structurally different from V1 where practical so the same bug is less likely to appear in both.

### V1 — map-based baseline

Use an ordered price map per side, a FIFO queue of orders at each level, and an `unordered_map<OrderId, handle>` for live-order lookup and cancellation. Price priority comes from the map; time priority comes from FIFO order.

### V2 — measured optimisation ladder

Each implementation preserves the matching semantics specified above.

- **V2.2 — Flat price arrays and occupancy bitmaps:** Replace ordered
  price maps with bounded flat price-level storage and bitmaps for
  locating occupied levels.

- **V2.3 — Preallocated order pool:** Replace separately allocated
  order nodes with a preallocated pool, intrusive FIFO links and a
  free list. Retain the standard unordered live-order index.

- **V2.4 — Custom order-ID map:** Replace the standard live-order
  index with a fixed-capacity, open-addressed `OrderIdMap`.

- **V2.5 — Generation-aware handles:** Associate pool handles with
  generation counters so that a stale handle cannot refer to a
  different order after its pool slot is reused. A slot is retired
  rather than allowing its generation counter to wrap.

Each stage must preserve the matching contract and pass scenario,
invariant and differential tests. Benchmark results must identify
the exact implementation being measured.

## 9. Correctness requirements

### Hand-written scenarios

At minimum test:

- FIFO execution for multiple orders at one price;
- partial fill of a resting order;
- partial fill of the incoming order;
- sweep across multiple price levels;
- maker-price execution;
- buy and sell price priority;
- market order with an empty book;
- market order with insufficient liquidity (remainder expires);
- limit remainder resting after matching;
- cancellation of a live order;
- cancellation of a fully filled or already-cancelled order;
- duplicate live ID and ID reuse after termination;
- quantity reduction preserves FIFO priority;
- quantity increase loses priority;
- price change loses priority;
- modified order that crosses and trades;
- invalid modify leaves the original order untouched;
- zero quantity and out-of-range price rejection;
- empty book and one-sided book cases.
- pool allocation and release;
- generation increments when a released pool slot is reused;
- stale handles cannot release or modify a newer allocation;
- exhausted pool behaviour;
- generation-aware order-ID lookup and queue consistency.

### Differential tests

Use deterministic seeded random operation streams. The normal
differential test runs 500 seeds with 200 operations per seed,
giving 100,000 generated operations.

The same requests are replayed against V0, V1, V2.2 and the current
V2.5 BookPool implementation. After every operation, compare the
full emitted event sequence and canonical snapshot against V0.
Check internal invariants in every implementation.

The operation distribution is approximately 60% limit submit,
20% cancel, 10% modify and 10% market submit. Generate valid and
invalid operations intentionally, including missing IDs, duplicate
live IDs, price boundaries, empty books, partial fills and repeated
ID lifecycles. Failure output must identify the seed and first
mismatching operation.

Longer stress runs may be added separately; they are not part of
the normal test-suite requirement.

### Invariants after every operation

- The book is not crossed: if both sides are non-empty, best bid is strictly less than best ask.
- Each level's stored aggregate quantity equals the sum of remaining quantities of its live orders.
- Every live-order index entry points to exactly one live order, and every live order has exactly one index entry where that index exists.
- There are no zero-quantity resting orders.
- Order IDs are unique among live orders.
- FIFO sequence order is increasing within every price level.
- Sequence numbers are unique among live resting orders and originate from a monotonically increasing counter.
- Every live pool-backed order handle must refer to the current
  generation of its allocated pool slot.

## 10. Benchmarking and profiling

Benchmark only Release builds, never sanitizer builds. The workload generator and event collection must not dominate the measured engine operation. Pre-generate operations and reserve output/event storage before timing.

**Current measurement coverage:** Google Benchmark throughput
measurements have been collected for balanced, crossing-heavy and
cancel-heavy workloads in Release configurations. Repeated-run
statistics are recorded in `docs/results.md`.

The x64 BookPool benchmark was built from the V2.5 codebase. Its
registrations retain historical `BM_V2_4_*` names; this distinction
is documented in the results file.

Per-operation latency percentiles, allocation counts and hardware
performance-counter profiling remain future measurement extensions.

Workloads:

- **Balanced:** random walk around a configurable mid, geometric price-distance distribution, moderate crossing rate, balanced sides.
- **Crossing-heavy:** higher probability that incoming orders cross several resting levels.
- **Cancel-heavy:** stable resting liquidity with frequent cancellation and replacement.

Report:

- throughput in operations per second;
- per-operation latency p50, p99, p99.9 and maximum;
- repeated-run median and spread;
- allocation count or allocations per operation where measurable;
- hardware, OS, compiler version, build type and exact compiler flags.

Warm up before measurement, pin to one core where supported, and account for timer overhead when collecting per-operation latency. Keep the latency sample buffer preallocated and calculate percentiles after the timed run. Use Google Benchmark for throughput. Use `perf stat` for cycles, instructions, IPC, cache misses and branch misses where available; use callgrind/cachegrind as an alternative when `perf` is impractical. Profile before changing code.

The optimisation table records every step, the workload, repeated-run statistics, and the observed delta versus the immediately preceding version. A speedup target is not a correctness criterion; investigate regressions instead of hiding them.

## 11. Build and repository hygiene

- CMake 3.20 or newer; C++20 required.
- GoogleTest and Google Benchmark may be acquired with CMake `FetchContent`.
- Windows development and testing use MSVC with CMake.
- The CI workflow is configured for Ubuntu/GCC and sanitizer builds.
  A successful remote CI run has not yet been recorded.
- AddressSanitizer and UndefinedBehaviorSanitizer are supported by the
  GCC/Clang CMake configuration, but sanitizer execution remains
  outstanding.
- Keep sanitizer and benchmark configurations separate.
- Add deterministic tests and keep random seeds in failure output.
- Keep performance results and commands reproducible in `docs/results.md` and the README.

## 12. Completion criteria

The core matching-engine implementation is complete through V2.5.

Completed:
- V0 reference and V1 map-based implementations.
- V2.2 flat/bitmap implementation.
- V2.3 preallocated order pool and intrusive FIFO.
- V2.4 custom fixed-capacity order-ID map.
- V2.5 generation-aware handles.
- Hand-written scenarios and invariant tests.
- 500-seed differential testing across four implementations.
- Repeated Release throughput measurements and documented results.
- Windows Debug and x64 Release test suites, with 66 tests passing.

Outstanding extensions:
- Local or remote sanitizer validation.
- Per-operation latency measurements.
- Allocation and hardware-counter profiling.
- Additional performance isolation experiments.

Tier 3 (TCP server, Python bindings and BTC-calibrated workload) remains
deferred until after applications are underway.

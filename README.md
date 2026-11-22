# C++20 Limit Order Book and Matching Engine

A correctness-first, single-instrument limit order book and matching engine, implemented in C++20.

The project explores how different data structures affect the performance of a single-threaded matching engine. It begins with a simple reference implementation, establishes alternative engine designs, and uses deterministic workloads and differential testing to evaluate correctness and throughput.

## Project status

**Core implementation: complete through V2.5.**

- [x] Initial design specification
- [x] Integer-tick price and quantity types
- [x] Deterministic matching events and canonical book snapshots
- [x] V0 reference matching engine
- [x] Hand-written matching scenarios and invariant tests
- [x] V1 map-based matching engine
- [x] Differential testing between implementations
- [x] V2.2 flat price arrays and occupancy bitmaps
- [x] V2.4 preallocated order pool, intrusive FIFO queues and custom order-ID map
- [x] V2.5 generation-aware handles to protect against stale pool-slot references
- [x] Google Benchmark throughput harness
- [x] x86 and x64 Release benchmark measurements

**Testing:** 66/66 tests passed following V2.5 implementation, including the x64 Release build.

Performance measurements are preliminary and are documented alongside their hardware, compiler, benchmark configuration and variability.

## Key design decisions

### Matching semantics

The engine implements:

- Limit and market orders.
- Price-time priority.
- FIFO ordering within each price level.
- Partial fills and sweeps across multiple price levels.
- Execution at the resting maker order's price.
- Cancellation and modification.
- Deterministic event generation.
- Validation of invalid operations without unintended book mutation.

Modification priority follows explicit rules: reducing quantity preserves priority, while increasing quantity or changing price loses priority.

Prices and quantities use integer representations rather than floating-point values.

### Implementation progression

| Version | Implementation | Main focus |
|---|---|---|
| V0 | Reference engine | Simple, understandable correctness baseline |
| V1 | `std::map` price levels and FIFO queues | Ordered price lookup and baseline performance |
| V2.2 | Flat price arrays and occupancy bitmaps | Reduce price-level lookup overhead |
| V2.4 | Preallocated order pool, intrusive FIFO queues and custom fixed-capacity `OrderIdMap` | Reduce allocation and lookup overhead |
| V2.5 | Generation-aware pool handles | Prevent stale references when pool slots are reused |

The versions provide different implementation trade-offs. Comparisons between complete versions measure the combined effect of their structural changes; they do not necessarily isolate the contribution of one data structure.

## Correctness and testing

Correctness is treated as a prerequisite for performance optimisation.

The test suite includes:

- Hand-written matching scenarios covering submissions, fills, partial fills, sweeps, cancellations and modifications.
- Invariant checks for book consistency.
- Differential tests that compare engine behaviour against the reference implementation.
- Comparisons of generated events and canonical book snapshots.
- Order-pool tests covering allocation, release, slot reuse and generation validation.

The current suite contains 66 passing tests.

The design and correctness requirements are documented in [`docs/design.md`](docs/design.md).

## Performance

Throughput is measured using Google Benchmark with deterministic synthetic workloads:

- Balanced.
- Crossing-heavy.
- Cancel-heavy.

Workload generation, engine construction and initial book loading are excluded from timed replay.

The x64 Release measurements produced the following median throughput:

| Implementation | Balanced | Crossing-heavy | Cancel-heavy |
|---|---:|---:|---:|
| V0 | 1.51M ops/s | 4.71M ops/s | 2.65M ops/s |
| V1 | 5.64M ops/s | 6.03M ops/s | 6.36M ops/s |
| V2.2 | 8.65M ops/s | 6.07M ops/s | 10.66M ops/s |
| BookPool (V2.5 code) | 15.96M ops/s | 7.42M ops/s | 19.85M ops/s |

The BookPool measurements retain the historical `BM_V2_4` benchmark names but were collected using the V2.5 codebase, including generation-aware handles.

These are preliminary throughput measurements, not latency percentile measurements or guaranteed performance figures. Some workloads show noticeable run-to-run variation.

Detailed methodology, historical comparisons and raw measurements:

- [`docs/results.md`](docs/results.md)
- [`docs/benchmark_x64_2026-10-25.json`](docs/benchmark_x64_2026-10-25.json)

## Build and test

Requirements:

- CMake 3.20 or later.
- A C++20-compatible compiler.
- Git.
- GoogleTest (downloaded through CMake FetchContent during configuration).

### Windows — MSVC

Run from the repository root in PowerShell:

```powershell
cmake -S . -B build
cmake --build build --config Debug --parallel 2
ctest --test-dir build -C Debug --output-on-failure
```

To build and test in Release mode:

```powershell
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure
```

### Linux — GCC or Clang

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

### Optional sanitizers

AddressSanitizer and UndefinedBehaviorSanitizer are supported through GCC or Clang:

```bash
cmake -S . -B build-sanitize \
  -DCMAKE_BUILD_TYPE=Debug \
  -DLOB_ENABLE_SANITIZERS=ON

cmake --build build-sanitize --parallel 2
ctest --test-dir build-sanitize --output-on-failure
```

Sanitizer builds are intended for correctness and memory-safety checks, not performance measurements.

## Running the benchmarks

Benchmarking is optional and disabled by default.

Enable Google Benchmark when configuring a separate build:

```powershell
cmake -S . -B build-bench -DLOB_BUILD_BENCHMARKS=ON
```

Build the benchmark executable:

```powershell
cmake --build build-bench --config Release --target lob_bench --parallel 2
```

For a Visual Studio generator, the executable is normally located at:

```text
build-bench/Release/lob_bench.exe
```

Run the benchmarks with repeated measurements and JSON output:

```powershell
.\build-bench\Release\lob_bench.exe `
    --benchmark_min_time=1s `
    --benchmark_repetitions=10 `
    --benchmark_report_aggregates_only=true `
    --benchmark_out=docs\benchmark-results.json `
    --benchmark_out_format=json
```

For single-configuration generators, the executable may instead be located directly inside `build-bench`.

Do not compare measurements from different architectures or build configurations as though they were directly interchangeable.

## Technical report

[Read the full C++20 Limit Order Book technical report](docs/technical-report.pdf)

The report covers the architecture, matching semantics, optimisation
progression, correctness testing and benchmark results.

## Repository structure

```text
.
├── .github/
│   └── workflows/
│       └── ci.yml
├── bench/
│   ├── bench_throughput.cpp
│   └── gen_workload.hpp
├── docs/
│   ├── benchmark_x64_2026-10-25.json
│   ├── design.md
│   └── results.md
├── include/
│   └── lob/
├── tests/
├── CMakeLists.txt
└── README.md
```

The public design and implementation details are described in `docs/design.md`.

## Scope and non-goals

This project deliberately focuses on a deterministic, single-threaded matching engine for one instrument.

It does not currently aim to implement:

- Multiple instruments or a multi-threaded matching core.
- Floating-point price representations.
- FIX connectivity or exchange protocol compatibility.
- Persistence or recovery.
- Advanced order types.
- A production network server.

The priority is to establish correctness, explore meaningful data-structure alternatives, and measure performance with reproducible experiments before expanding the scope.

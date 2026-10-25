
# Benchmark Results

## Status

The throughput harness has been added. Results are pending the first
Release benchmark run on the development machine.

No performance claims should be made until measurements are collected.

## Environment

Record these details for every published result:

- CPU: Intel Core i7-10510U, 4 cores / 8 logical processors
- OS: Windows 10 Enterprise, build 26200 (as reported)
- Compiler: MSVC 19.51.36260, x86 target
- CMake: 4.4.4
- Build type: Release
- Architecture: x86 (32-bit)
- Build type: Release
- Compiler flags:
- Benchmark command:
- Date:

## Initial throughput baseline

**Status:** Initial results; further repeatability checks required.

- Build type: Release
- Benchmark repetitions: 5
- Operations per batch: 2,000
- Workload generation: outside timed replay
- Initial book setup: outside timed replay
- Event sink: non-allocating event counter

| Workload | V0 median ops/sec | V1 median ops/sec | Speedup |
|---|---:|---:|---:|
| Balanced | 1.213M | 4.571M | 3.77x |
| Crossing-heavy | 4.248M | 4.411M | 1.04x |
| Cancel-heavy | 2.294M | 5.094M | 2.22x |

### Initial interpretation

V1 substantially improves the balanced and cancel-heavy synthetic workloads.
The crossing-heavy result shows little difference, so no meaningful speedup
is claimed for that workload yet.

These results are specific to the recorded machine, compiler, build settings
and workload generator. They are preliminary and should be repeated before
being used in a CV.

The benchmark measures operation replay, including matching, state updates
and event callbacks. It excludes workload generation and initial book setup.

## Methodology notes

- The operation sequence is generated before timed replay.
- Both engines use the same seed and operation sequence for each workload.
- Book construction and initial liquidity setup are outside the timed section.
- The timed section includes matching, state updates and event callbacks.
- The workload is synthetic. Its operation mix and crossing rate are controls,
  not claims about a real exchange.
- The reference implementation helps generate valid cancellation and
  modification targets. Differential tests remain the correctness check.
- The result depends on the CPU, compiler, build flags and workload.

## Optimisation ladder

This section will be expanded after we establish the V1 baseline.

| Version | Change | Workload | Throughput delta | Notes |
|---|---|---|---:|---|
| V0 | Vector-based reference model | Pending | Pending | Baseline |
| V1 | Ordered price maps, FIFO lists, ID index | Pending | Pending | To be measured |

## V2.1 — Flat price-level array (18 October 2026)

### Environment

- CPU: Intel Core i7-10510U, 4 cores / 8 logical processors
- OS: Windows 10 Enterprise, reported build 26200
- Compiler: MSVC 19.51.36260, x86 target
- CMake: 4.4.4
- Build type: Release
- Repetitions: 5
- Operations per batch: 2,000

### Median throughput

| Workload | V0 (ops/s) | V1 (ops/s) | V2.1 (ops/s) | V2.1 / V1 |
|---|---:|---:|---:|---:|
| Balanced | 1.348M | 4.588M | 6.368M | 1.39x |
| Crossing-heavy | 4.245M | 4.495M | 0.250M | 0.056x |
| Cancel-heavy | 2.607M | 5.213M | 8.000M | 1.53x |

### Interpretation

The flat array improves throughput on the balanced and cancel-heavy
synthetic workloads but produces a severe regression on the crossing-heavy
workload.

Initial code inspection identifies linear scanning of empty price-array
slots as a likely cause. The next implementation will use a bitmap to locate
occupied levels without checking every intervening tick.

This diagnosis has not yet been confirmed with a profiler.

The results are preliminary. The crossing-heavy regression is clear, but
some V2.1 repetitions show substantial variability, so the benchmark should
be rerun after the bitmap change with a longer measurement duration.

Do not use these numbers as universal engine-performance claims. They describe
this implementation and the recorded x86 Release environment.


## V2.2 — Flat price array with occupancy bitmap

### Method

- Build type: Release
- Architecture: x86
- Operations per batch: 2,000
- Repetitions: 5
- Workloads: balanced, crossing-heavy, cancel-heavy
- Measurement: median throughput from Google Benchmark

### Results

| Workload | V1 median (ops/s) | V2.2 median (ops/s) | Speedup |
|---|---:|---:|---:|
| Balanced | 4.776M | 7.111M | 1.49x |
| Crossing-heavy | 4.507M | 4.923M | 1.09x |
| Cancel-heavy | 5.213M | 7.529M | 1.44x |

### Interpretation

The flat array and bitmap combination shows promising throughput gains
on balanced and cancel-heavy synthetic workloads. The crossing-heavy
regression observed in V2.1 is no longer present in this run.

However, V2.2's coefficient of variation is high across all three workloads.
The results are therefore preliminary and require further repeatability
checks before being used as definitive performance claims.

The bitmap lets the engine skip groups of empty price slots when locating
the next occupied level. The experiment changes price-level search while
retaining list-based order storage and the unordered ID index.


## V2.3 — Preallocated order pool and intrusive FIFO

### Method

- CPU: Intel Core i7-10510U, 4 cores / 8 logical processors
- OS: Windows 10 Enterprise, reported build 26200
- Compiler: MSVC 19.51.36260, x86 target
- Build type: Release
- Operations per batch: 2,000
- Repetitions: 3
- Engine: flat price array + occupancy bitmap + order pool
- Pool capacity in benchmark: 4,096 slots

### Results

| Workload | V2.2 median (ops/s) | V2.3 median (ops/s) | Relative change |
|---|---:|---:|---:|
| Balanced | 7.646M | 10.596M | +38.6% |
| Crossing-heavy | 5.158M | 7.654M | +48.4% |
| Cancel-heavy | 8.862M | 7.369M | -16.8% |

### Interpretation

Replacing std::list order nodes with preallocated pool slots and intrusive
FIFO links improved the balanced and crossing-heavy results in this run,
but reduced throughput in the cancel-heavy workload.

The coefficients of variation were high, so these results are preliminary.
Repeat measurements before using the results in a CV or making definitive
performance claims.

The pool avoids separate list-node allocations during replay, but the
unordered ID index can still allocate entries. The experiment therefore
measures the pool and intrusive-list change, not a completely allocation-free
engine.

| V2.4 | Flat open-addressing ID map | Pending | Pending | Not implemented |
| V2.5 | Generation counters on handles | Pending | Pending | Not implemented |

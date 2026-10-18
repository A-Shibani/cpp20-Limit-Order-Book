
# Benchmark Results

## Status

The throughput harness has been added. Results are pending the first
Release benchmark run on the development machine.

No performance claims should be made until measurements are collected.

## Environment

Record these details for every published result:

- CPU:
- CPU core count:
- RAM:
- OS version:
- Compiler and version:
- CMake version:
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
| V2.1 | Flat array of price levels | Pending | Pending | Not implemented |
| V2.2 | Preallocated order pool and intrusive lists | Pending | Pending | Not implemented |
| V2.3 | Bitmap for non-empty levels | Pending | Pending | Not implemented |
| V2.4 | Flat open-addressing ID map | Pending | Pending | Not implemented |
| V2.5 | Generation counters on handles | Pending | Pending | Not implemented |

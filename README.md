
# C++20 Limit Order Book and Matching Engine

A correctness-first, single-instrument limit order book and matching engine. The project starts with a deliberately simple reference model, establishes a map-based baseline, and then measures each optimisation independently.

## Current status

- [x] Initial design specification
- [x] C++20 type and event definitions
- [x] CMake build and starter GoogleTest suite
- [ ] V0 reference matching engine
- [ ] Hand-written matching scenarios and invariants
- [ ] V1 map-based engine and differential tests
- [ ] Reproducible benchmarks and profiling
- [ ] V2 optimisation ladder

No throughput or latency claims will be made until measured on documented hardware.

## Build and test

Requirements: CMake 3.20+, a C++20 compiler, and Git.
CMake downloads GoogleTest on the first configure.

### Windows — Visual Studio 2022 with C++ support

Run these commands in PowerShell from the repository root:

```powershell
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

### GCC or Clang on Linux / WSL2

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Sanitizer build (GCC or Clang):

```bash
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug -DLOB_ENABLE_SANITIZERS=ON
cmake --build build-sanitize --parallel
ctest --test-dir build-sanitize --output-on-failure
```

Do not use sanitizer builds for performance measurements.

## Design

Read [`docs/design.md`](docs/design.md) before implementing matching behaviour.

The public types currently live in `include/lob/types.hpp`.
Event structures live in `include/lob/events.hpp`.

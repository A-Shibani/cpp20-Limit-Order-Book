
#include "lob/book_map.hpp"
#include "lob/book_ref.hpp"
#include "gen_workload.hpp"
#include "lob/book_flat.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>

namespace {

// Replay exactly the same pre-generated workload through whichever engine
// type is supplied as the template argument.
template <typename Book>
void run_throughput_benchmark(
    benchmark::State& state,
    lob::bench::WorkloadKind kind,
    std::uint64_t seed
) {
    const auto operation_count =
        static_cast<std::size_t>(state.range(0));

    // Workload generation uses V0 as a model, but happens before the timer
    // starts. Both engines get the same deterministic operation sequence.
    const lob::bench::Workload workload =
        lob::bench::generate_workload(
            kind,
            operation_count,
            seed
        );

    std::uint64_t total_event_count = 0;

    for (auto _ : state) {
        (void)_;

        // Setup is deliberately excluded from the measured replay:
        // create a clean engine and load the identical initial orders.
        state.PauseTiming();

        {
            Book book;
            std::uint64_t event_count = 0;

            // Count emitted events without allocating or storing them.
            // Event delivery remains part of the measured operation.
            const auto event_sink = [&event_count](const lob::Event&) {
                ++event_count;
            };

            for (const auto& order : workload.initial_orders) {
                book.submit(order, event_sink);
            }

            // Do not count the initial book's setup acknowledgements.
            event_count = 0;

            state.ResumeTiming();

            // This is the timed section: only replay the fixed operation list.
            for (const auto& operation : workload.operations) {
                lob::bench::apply_workload_operation(
                    book,
                    operation,
                    event_sink
                );
            }

            // Prevent the compiler from treating the result as unused.
            benchmark::DoNotOptimize(book);
            benchmark::DoNotOptimize(event_count);
            benchmark::ClobberMemory();

            // Pause before aggregating counters and destroying the book.
            state.PauseTiming();
            total_event_count += event_count;
        }

        // The book has now been destroyed while timing was paused.
        // Resume for the benchmark framework's next iteration.
        state.ResumeTiming();
    }

    const std::int64_t operations_per_batch =
        static_cast<std::int64_t>(workload.operations.size());

    const std::int64_t total_operations =
        state.iterations() * operations_per_batch;

    // Google Benchmark uses this to report operations per second.
    state.SetItemsProcessed(total_operations);

    // A useful diagnostic that helps us see whether a workload is producing
    // realistic activity or mostly producing rejected operations.
    if (total_operations > 0) {
        state.counters["events_per_operation"] =
            static_cast<double>(total_event_count)
            / static_cast<double>(total_operations);
    }
}

// V0 reference-model benchmarks.
static void BM_V0_Balanced(benchmark::State& state) {
    run_throughput_benchmark<lob::BookRef>(
        state, lob::bench::WorkloadKind::Balanced, 1001
    );
}

static void BM_V0_CrossingHeavy(benchmark::State& state) {
    run_throughput_benchmark<lob::BookRef>(
        state, lob::bench::WorkloadKind::CrossingHeavy, 1002
    );
}

static void BM_V0_CancelHeavy(benchmark::State& state) {
    run_throughput_benchmark<lob::BookRef>(
        state, lob::bench::WorkloadKind::CancelHeavy, 1003
    );
}

// V1 map-based benchmarks. The same workload kind and seed are used
// as V0 so the operation trace is identical for each comparison.
static void BM_V1_Balanced(benchmark::State& state) {
    run_throughput_benchmark<lob::BookMap>(
        state, lob::bench::WorkloadKind::Balanced, 1001
    );
}

static void BM_V1_CrossingHeavy(benchmark::State& state) {
    run_throughput_benchmark<lob::BookMap>(
        state, lob::bench::WorkloadKind::CrossingHeavy, 1002
    );
}

static void BM_V1_CancelHeavy(benchmark::State& state) {
    run_throughput_benchmark<lob::BookMap>(
        state, lob::bench::WorkloadKind::CancelHeavy, 1003
    );
}


static void BM_V2_2_Balanced(benchmark::State& state) {
    // Use the same seed and workload kind as V0 and V1.
    run_throughput_benchmark<lob::BookFlat>(
        state, lob::bench::WorkloadKind::Balanced, 1001
    );
}

static void BM_V2_2_CrossingHeavy(benchmark::State& state) {
    run_throughput_benchmark<lob::BookFlat>(
        state, lob::bench::WorkloadKind::CrossingHeavy, 1002
    );
}

static void BM_V2_2_CancelHeavy(benchmark::State& state) {
    run_throughput_benchmark<lob::BookFlat>(
        state, lob::bench::WorkloadKind::CancelHeavy, 1003
    );
}

// Each batch replays 2,000 operations. We will start small, then increase
// the workload size once the initial benchmark is working correctly.
BENCHMARK(BM_V0_Balanced)->Arg(2000)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_V0_CrossingHeavy)->Arg(2000)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_V0_CancelHeavy)->Arg(2000)->Unit(benchmark::kMillisecond);

BENCHMARK(BM_V1_Balanced)->Arg(2000)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_V1_CrossingHeavy)->Arg(2000)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_V1_CancelHeavy)->Arg(2000)->Unit(benchmark::kMillisecond);

BENCHMARK(BM_V2_2_Balanced)->Arg(2000)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_V2_2_CrossingHeavy)->Arg(2000)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_V2_2_CancelHeavy)->Arg(2000)->Unit(benchmark::kMillisecond);

}  // namespace

BENCHMARK_MAIN();

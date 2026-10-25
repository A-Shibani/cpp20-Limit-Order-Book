#include "lob/book_map.hpp"
#include "lob/book_ref.hpp"
#include "gen_workload.hpp"
#include "lob/book_flat.hpp"
#include "lob/book_pool.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace {

// Construct each engine in the same way the benchmark expects.
// BookPool has a fixed-capacity order pool, so give it an explicit capacity.
template <typename Book>
Book make_benchmark_book() {
    // The generated batches contain 2,000 operations, plus up to 128 initial
    // orders. A capacity of 4,096 leaves headroom for this benchmark workload.
    if constexpr (std::is_same_v<Book, lob::BookPool>) {
        return Book(4096);
    } else {
        // V0, V1 and V2.2 use their default constructors.
        return Book{};
    }
}

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

    // Generate the workload before timing starts. All implementations receive
    // the same deterministic operation sequence for a given kind and seed.
    const lob::bench::Workload workload =
        lob::bench::generate_workload(
            kind,
            operation_count,
            seed
        );

    std::uint64_t total_event_count = 0;

    for (auto _ : state) {
        (void)_;

        // Exclude engine construction and initial-book loading from the timed
        // section. This isolates the cost of replaying the operation sequence.
        state.PauseTiming();

        {
            Book book = make_benchmark_book<Book>();
            std::uint64_t event_count = 0;

            // Count emitted events without storing them or allocating a vector.
            // Event delivery itself remains part of the timed operation.
            const auto event_sink = [&event_count](const lob::Event&) {
                ++event_count;
            };

            for (const auto& order : workload.initial_orders) {
                book.submit(order, event_sink);
            }

            // Do not count acknowledgements produced while setting up the book.
            event_count = 0;

            state.ResumeTiming();

            // This is the measured section: replay the fixed list of operations.
            for (const auto& operation : workload.operations) {
                lob::bench::apply_workload_operation(
                    book,
                    operation,
                    event_sink
                );
            }

            // Make the result observable to the compiler so useful work is not
            // optimised away. ClobberMemory also acts as a compiler barrier.
            benchmark::DoNotOptimize(book);
            benchmark::DoNotOptimize(event_count);
            benchmark::ClobberMemory();

            // Pause before adding counters and destroying the engine object.
            state.PauseTiming();
            total_event_count += event_count;
        }

        // Resume the framework's timer for its next iteration.
        state.ResumeTiming();
    }

    const std::int64_t operations_per_batch =
        static_cast<std::int64_t>(workload.operations.size());

    const std::int64_t total_operations =
        state.iterations() * operations_per_batch;

    // Google Benchmark reports this as throughput (operations per second).
    state.SetItemsProcessed(total_operations);

    // This diagnostic helps identify workloads that emit unusually few or many
    // events; it is not itself a performance metric.
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

// V1 map-based benchmarks. They use the same workload kinds and seeds as V0.
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

// V2.2: flat price arrays plus occupancy bitmaps and the existing ID map.
static void BM_V2_2_Balanced(benchmark::State& state) {
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

// V2.4: BookPool now uses the custom fixed-capacity OrderIdMap instead of
// std::unordered_map. These benchmark names intentionally say V2_4 so that
// the measurements are not confused with the earlier V2.3 implementation.
static void BM_V2_4_Balanced(benchmark::State& state) {
    run_throughput_benchmark<lob::BookPool>(
        state, lob::bench::WorkloadKind::Balanced, 1001
    );
}

static void BM_V2_4_CrossingHeavy(benchmark::State& state) {
    run_throughput_benchmark<lob::BookPool>(
        state, lob::bench::WorkloadKind::CrossingHeavy, 1002
    );
}

static void BM_V2_4_CancelHeavy(benchmark::State& state) {
    run_throughput_benchmark<lob::BookPool>(
        state, lob::bench::WorkloadKind::CancelHeavy, 1003
    );
}

// Each batch replays 2,000 operations. Increase this later once the baseline
// measurements and correctness checks are stable.
BENCHMARK(BM_V0_Balanced)->Arg(2000)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_V0_CrossingHeavy)->Arg(2000)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_V0_CancelHeavy)->Arg(2000)->Unit(benchmark::kMillisecond);

BENCHMARK(BM_V1_Balanced)->Arg(2000)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_V1_CrossingHeavy)->Arg(2000)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_V1_CancelHeavy)->Arg(2000)->Unit(benchmark::kMillisecond);

BENCHMARK(BM_V2_2_Balanced)->Arg(2000)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_V2_2_CrossingHeavy)->Arg(2000)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_V2_2_CancelHeavy)->Arg(2000)->Unit(benchmark::kMillisecond);

BENCHMARK(BM_V2_4_Balanced)->Arg(2000)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_V2_4_CrossingHeavy)->Arg(2000)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_V2_4_CancelHeavy)->Arg(2000)->Unit(benchmark::kMillisecond);

}  // namespace

BENCHMARK_MAIN();

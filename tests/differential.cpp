
#include "lob/book_map.hpp"
#include "lob/book_ref.hpp"
#include "lob/book_flat.hpp"

#include <cstdint>
#include <random>
#include <vector>

#include <gtest/gtest.h>

namespace lob {
namespace {

// Describe one input operation so exactly the same request can be
// replayed against both implementations.
enum class Action {
    LimitSubmit,
    MarketSubmit,
    Cancel,
    Modify
};

struct TestOperation {
    Action action{};
    OrderId id{};
    Side side{Side::Buy};
    Quantity quantity{};
    Price price{};
};

// Run an operation and collect its events. The template allows this
// helper to work with both BookRef and BookMap without a base class.
template <typename Book>
void apply_operation(
    Book& book,
    const TestOperation& operation,
    std::vector<Event>& events
) {
    const auto collect = [&events](const Event& event) {
        events.push_back(event);
    };

    switch (operation.action) {
    case Action::LimitSubmit:
        book.submit(
            OrderRequest::limit_order(
                operation.id,
                operation.side,
                operation.quantity,
                operation.price
            ),
            collect
        );
        break;

    case Action::MarketSubmit:
        book.submit(
            OrderRequest::market_order(
                operation.id,
                operation.side,
                operation.quantity
            ),
            collect
        );
        break;

    case Action::Cancel:
        book.cancel(operation.id, collect);
        break;

    case Action::Modify:
        book.modify(
            operation.id,
            operation.price,
            operation.quantity,
            collect
        );
        break;
    }
}

// Compare two engines after every operation, not merely at the end.
// This localises a divergence to its first operation.
TEST(DifferentialTests, BookMapMatchesReferenceOverSeededStreams) {
    constexpr std::uint64_t seed_count = 500;
    constexpr int operations_per_seed = 200;

    for (std::uint64_t seed = 0; seed < seed_count; ++seed) {
        std::mt19937_64 rng(seed);

        std::uniform_int_distribution<int> action_dist(0, 99);
        std::uniform_int_distribution<int> id_dist(1, 48);
        std::uniform_int_distribution<int> side_dist(0, 1);
        std::uniform_int_distribution<int> quantity_dist(0, 25);
        std::uniform_int_distribution<int> normal_price_dist(95, 105);
        std::uniform_int_distribution<int> price_case_dist(0, 199);

        BookRef reference;
        BookMap candidate;
        BookFlat flat_candidate;

        std::vector<Event> reference_events;
        std::vector<Event> candidate_events;
        std::vector<Event> flat_events;
        flat_events.reserve(64);

        // Reserve event storage before the operation loop, avoiding
        // repeated reallocations while collecting a single operation.
        reference_events.reserve(64);
        candidate_events.reserve(64);

        for (int step = 0; step < operations_per_seed; ++step) {
            const int action_number = action_dist(rng);

            Action action;
            if (action_number < 60) {
                action = Action::LimitSubmit;
            } else if (action_number < 80) {
                action = Action::Cancel;
            } else if (action_number < 90) {
                action = Action::Modify;
            } else {
                action = Action::MarketSubmit;
            }

            const OrderId id =
                static_cast<OrderId>(id_dist(rng));

            const Side side = side_dist(rng) == 0
                ? Side::Buy
                : Side::Sell;

            const Quantity quantity =
                static_cast<Quantity>(quantity_dist(rng));

            // Most prices are ordinary in-range prices. Occasionally
            // generate invalid boundaries to test deterministic rejects.
            const int price_case = price_case_dist(rng);
            Price price{};

            if (price_case == 0) {
                price = 0;
            } else if (price_case == 1) {
                price = 65'536;
            } else {
                price = static_cast<Price>(normal_price_dist(rng));
            }

            const TestOperation operation{
                action, id, side, quantity, price
            };

            reference_events.clear();
            candidate_events.clear();
            flat_events.clear();

            apply_operation(reference, operation, reference_events);
            apply_operation(candidate, operation, candidate_events);
            // Apply the identical input to V2.1. All engines receive the same
            // operation, so any difference in events or state is significant.
            apply_operation(flat_candidate, operation, flat_events);

            // Events must match exactly, including order and contents.
            if (reference_events != candidate_events) {
                FAIL()
                    << "Event mismatch: seed=" << seed
                    << ", step=" << step
                    << ", id=" << id
                    << ", price=" << price
                    << ", quantity=" << quantity;
            }

            // V2.1 must emit the same ordered events as the reference engine.
            if (reference_events != flat_events) {
                FAIL()
                    << "V0/V2.1 event mismatch: seed=" << seed
                    << ", step=" << step
                    << ", id=" << id
                    << ", price=" << price
                    << ", quantity=" << quantity;
            }

            // Both implementations must expose identical logical state.
            if (reference.snapshot() != candidate.snapshot()) {
                FAIL()
                    << "Snapshot mismatch: seed=" << seed
                    << ", step=" << step
                    << ", id=" << id
                    << ", price=" << price
                    << ", quantity=" << quantity;
            }

            // Snapshot comparison catches differences in resting quantities,
            // order priority, side, price, IDs and sequence numbers.
            if (reference.snapshot() != flat_candidate.snapshot()) {
                FAIL()
                    << "V0/V2.1 snapshot mismatch: seed=" << seed
                    << ", step=" << step
                    << ", id=" << id
                    << ", price=" << price
                    << ", quantity=" << quantity;
            }

            // Check internal invariants in each implementation too.
            if (!reference.check_invariants()) {
                FAIL()
                    << "V0 invariant failure: seed=" << seed
                    << ", step=" << step;
            }

            if (!candidate.check_invariants()) {
                FAIL()
                    << "V1 invariant failure: seed=" << seed
                    << ", step=" << step;
            }

            if (!flat_candidate.check_invariants()) {
                FAIL()
                    << "V2.1 invariant failure: seed=" << seed
                    << ", step=" << step;
            }
        }
    }
}

}  // namespace
}  // namespace lob

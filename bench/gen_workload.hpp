
#pragma once

#include "lob/book_ref.hpp"
#include "lob/types.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace lob::bench {

// Different workload styles help us understand where each design performs
// well. They are synthetic workloads, not claims about a real exchange.
enum class WorkloadKind {
    Balanced,
    CrossingHeavy,
    CancelHeavy
};

// The input operation to replay. This is separate from lob::Operation,
// which describes the operation associated with an emitted event.
enum class WorkloadAction {
    LimitSubmit,
    MarketSubmit,
    Cancel,
    Modify
};

struct WorkloadOperation {
    WorkloadAction action{WorkloadAction::LimitSubmit};
    OrderId id{};
    Side side{Side::Buy};
    Quantity quantity{};
    Price price{};
};

struct Workload {
    // These orders establish the same initial book for every benchmark run.
    // They are loaded while benchmark timing is paused.
    std::vector<OrderRequest> initial_orders;

    // This operation sequence is generated once and replayed repeatedly.
    std::vector<WorkloadOperation> operations;
};

// Apply one generated operation to any engine implementing our common API.
// This template lets the exact same workload drive V0 and V1.
template <typename Book, typename EventSink>
void apply_workload_operation(
    Book& book,
    const WorkloadOperation& operation,
    EventSink& event_sink
) {
    switch (operation.action) {
    case WorkloadAction::LimitSubmit:
        book.submit(
            OrderRequest::limit_order(
                operation.id,
                operation.side,
                operation.quantity,
                operation.price
            ),
            event_sink
        );
        break;

    case WorkloadAction::MarketSubmit:
        book.submit(
            OrderRequest::market_order(
                operation.id,
                operation.side,
                operation.quantity
            ),
            event_sink
        );
        break;

    case WorkloadAction::Cancel:
        book.cancel(operation.id, event_sink);
        break;

    case WorkloadAction::Modify:
        book.modify(
            operation.id,
            operation.price,
            operation.quantity,
            event_sink
        );
        break;
    }
}

// Generate deterministic inputs using a seed.
//
// crossing_rate controls how often generated limit orders are deliberately
// aggressive relative to a moving synthetic midpoint. It is an approximate
// control: the actual crossing rate also depends on the current book.
inline Workload generate_workload(
    WorkloadKind kind,
    std::size_t operation_count,
    std::uint64_t seed
) {
    Workload workload;
    workload.operations.reserve(operation_count);

    std::mt19937_64 rng(seed);

    // These distributions define the synthetic order flow.
    std::uniform_int_distribution<int> initial_quantity_dist(5, 25);
    std::uniform_int_distribution<int> quantity_dist(1, 20);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<int> action_dist(0, 99);
    std::uniform_int_distribution<int> midpoint_step_dist(-1, 1);
    std::uniform_int_distribution<int> keep_price_dist(0, 3);

    // A geometric distribution concentrates prices near the midpoint while
    // still occasionally generating orders farther away.
    std::geometric_distribution<int> distance_dist(0.25);

    double crossing_rate = 0.20;

    if (kind == WorkloadKind::CrossingHeavy) {
        crossing_rate = 0.75;
    } else if (kind == WorkloadKind::CancelHeavy) {
        crossing_rate = 0.02;
    }

    std::bernoulli_distribution aggressive_dist(crossing_rate);

    // Start with liquidity on both sides, separated around the midpoint.
    // The default midpoint sits comfortably inside our configured price range.
    constexpr int starting_mid = 32'768;
    constexpr int initial_levels_per_side = 64;

    OrderId next_id = 1;

    for (int distance = 1;
         distance <= initial_levels_per_side;
         ++distance) {
        const auto quantity = static_cast<Quantity>(
            initial_quantity_dist(rng)
        );

        // Bids below the midpoint and asks above it cannot cross each other.
        workload.initial_orders.push_back(
            OrderRequest::limit_order(
                next_id++,
                Side::Buy,
                quantity,
                static_cast<Price>(starting_mid - distance)
            )
        );

        workload.initial_orders.push_back(
            OrderRequest::limit_order(
                next_id++,
                Side::Sell,
                quantity,
                static_cast<Price>(starting_mid + distance)
            )
        );
    }

    // Use V0 outside the timed benchmark to maintain a realistic, reproducible
    // sequence of IDs for cancellations and modifications. This also means
    // many cancel/modify operations target orders known to be resting.
    BookRef model;

    const auto discard_event = [](const Event&) {};

    for (const auto& order : workload.initial_orders) {
        model.submit(order, discard_event);
    }

    int midpoint = starting_mid;

    // Choose an in-range price around the current synthetic midpoint.
    // Buy aggression raises the limit; sell aggression lowers it.
    const auto generate_price = [&](Side side) -> Price {
        const int distance = std::min(
            distance_dist(rng),
            63
        ) + 1;

        const bool aggressive = aggressive_dist(rng);

        int generated_price = midpoint;

        if (side == Side::Buy) {
            generated_price += aggressive ? distance : -distance;
        } else {
            generated_price += aggressive ? -distance : distance;
        }

        return static_cast<Price>(generated_price);
    };

    for (std::size_t i = 0; i < operation_count; ++i) {
        // Move the synthetic midpoint by at most one tick per operation.
        // This creates a simple random walk rather than a fixed price.
        midpoint = std::clamp(
            midpoint + midpoint_step_dist(rng),
            32'000,
            33'500
        );

        // Select the operation mix for this workload.
        const int roll = action_dist(rng);
        WorkloadAction action = WorkloadAction::LimitSubmit;

        if (kind == WorkloadKind::Balanced) {
            if (roll < 60) {
                action = WorkloadAction::LimitSubmit;
            } else if (roll < 80) {
                action = WorkloadAction::Cancel;
            } else if (roll < 90) {
                action = WorkloadAction::Modify;
            } else {
                action = WorkloadAction::MarketSubmit;
            }
        } else if (kind == WorkloadKind::CrossingHeavy) {
            if (roll < 55) {
                action = WorkloadAction::LimitSubmit;
            } else if (roll < 65) {
                action = WorkloadAction::Cancel;
            } else if (roll < 75) {
                action = WorkloadAction::Modify;
            } else {
                action = WorkloadAction::MarketSubmit;
            }
        } else {
            // Cancel-heavy: 45% submit, 50% cancel, 5% modify.
            if (roll < 45) {
                action = WorkloadAction::LimitSubmit;
            } else if (roll < 95) {
                action = WorkloadAction::Cancel;
            } else {
                action = WorkloadAction::Modify;
            }
        }

        // Select a currently resting order when cancel or modify is needed.
        // Because matching can fill orders, we refresh this list from the
        // reference engine instead of relying on stale IDs.
        std::vector<RestingOrderSnapshot> resting_orders;
        RestingOrderSnapshot target{};

        if (action == WorkloadAction::Cancel
            || action == WorkloadAction::Modify) {
            resting_orders = model.snapshot();

            // Keep a small minimum book in the cancel-heavy workload.
            // When the book is too small, replace that cancel with a submit.
            const bool book_too_small =
                kind == WorkloadKind::CancelHeavy
                && action == WorkloadAction::Cancel
                && resting_orders.size() <= 32U;

            if (resting_orders.empty() || book_too_small) {
                action = WorkloadAction::LimitSubmit;
            } else {
                std::uniform_int_distribution<std::size_t> target_dist(
                    0,
                    resting_orders.size() - 1
                );

                target = resting_orders[target_dist(rng)];
            }
        }

        WorkloadOperation operation{};
        operation.action = action;

        switch (action) {
        case WorkloadAction::LimitSubmit: {
            // Submitted IDs are unique. Modified orders retain their ID.
            const Side side = side_dist(rng) == 0
                ? Side::Buy
                : Side::Sell;

            operation.id = next_id++;
            operation.side = side;
            operation.quantity =
                static_cast<Quantity>(quantity_dist(rng));
            operation.price = generate_price(side);
            break;
        }

        case WorkloadAction::MarketSubmit: {
            const Side side = side_dist(rng) == 0
                ? Side::Buy
                : Side::Sell;

            operation.id = next_id++;
            operation.side = side;
            operation.quantity =
                static_cast<Quantity>(quantity_dist(rng));

            // The price field is ignored for a market order.
            operation.price = 0;
            break;
        }

        case WorkloadAction::Cancel:
            // The selected ID is currently resting in the model.
            operation.id = target.id;
            operation.side = target.side;
            break;

        case WorkloadAction::Modify:
            // Modification cannot change the order's side or ID.
            operation.id = target.id;
            operation.side = target.side;
            operation.quantity =
                static_cast<Quantity>(quantity_dist(rng));

            // Sometimes keep the price to generate quantity-only changes.
            // Otherwise, generate a new price that may cross the book.
            if (keep_price_dist(rng) == 0) {
                operation.price = target.price;
            } else {
                operation.price = generate_price(target.side);
            }
            break;
        }

        // Save the input first, then apply it to the model. V0's resulting
        // state guides the next operation, but its generation time is not
        // part of the measured engine throughput.
        workload.operations.push_back(operation);

        apply_workload_operation(
            model,
            workload.operations.back(),
            discard_event
        );
    }

    return workload;
}

}  // namespace lob::bench


#pragma once

#include "lob/events.hpp"
#include "lob/types.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <list>
#include <map>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lob {

// V1: map-based limit order book.
//
// Compared with V0:
// - Price levels are stored in ordered maps.
// - Each price level contains a FIFO list of resting orders.
// - An ID index lets us locate an order for cancellation or modification.
//
// The matching rules and public operations remain the same as V0.
class BookMap {
private:
    // One resting order in the book.
    struct RestingOrder {
        OrderId id{};
        Side side{Side::Buy};
        Price price{};
        Quantity remaining_quantity{};
        Sequence sequence{};
    };

    // A list preserves the order of its elements.
    // Orders are appended to the back and matched from the front.
    using OrderQueue = std::list<RestingOrder>;

    // A price level holds all orders at one price.
    struct Level {
        Quantity total_quantity{0};
        OrderQueue orders;
    };

    // std::map keeps prices sorted from low to high.
    // We use reverse iteration to find the highest bid.
    using LevelMap = std::map<Price, Level>;

    // A handle identifies the exact resting order without a scan.
    //
    // std::list iterators remain valid when other list elements are
    // inserted or erased, except for an iterator to an erased element.
    struct OrderHandle {
        Side side;
        Price price;
        OrderQueue::iterator order_it;
    };

    // The index contains only currently resting orders.
    using OrderIndex = std::unordered_map<OrderId, OrderHandle>;

public:
    explicit BookMap(PriceRange range = {}) : range_(range) {
        if (!range_.valid()) {
            throw std::invalid_argument("Invalid price range");
        }
    }

    // Submit a new limit or market order.
    //
    // The event sink is called synchronously. It must not re-enter
    // this book while an operation is in progress.
    template <typename EventSink>
    void submit(
        const OrderRequest& request,
        EventSink&& event_sink
    ) {
        // An ID may be reused after its old order is no longer resting.
        // While an order is live, however, the ID must be unique.
        if (order_index_.find(request.id) != order_index_.end()) {
            event_sink(Event{RejectEvent{
                request.id,
                Operation::Submit,
                RejectReason::DuplicateId
            }});
            return;
        }

        if (request.quantity == 0) {
            event_sink(Event{RejectEvent{
                request.id,
                Operation::Submit,
                RejectReason::ZeroQuantity
            }});
            return;
        }

        if (request.limit_price.has_value()
            && !range_.contains(*request.limit_price)) {
            event_sink(Event{RejectEvent{
                request.id,
                Operation::Submit,
                RejectReason::PriceOutOfRange
            }});
            return;
        }

        // Validate overflow before acknowledging or executing trades.
        // Rejection must not partially change the book.
        if (would_overflow_if_resting(request)) {
            event_sink(Event{RejectEvent{
                request.id,
                Operation::Submit,
                RejectReason::QuantityOverflow
            }});
            return;
        }

        // Accepted requests emit Ack before any Trade events.
        event_sink(Event{AckEvent{
            request.id,
            Operation::Submit
        }});

        // Match against the opposite side, then rest any eligible
        // limit-order remainder.
        match_and_rest(request, request.quantity, event_sink);
    }

    // Cancel a resting order by ID.
    //
    // Looking up the ID is average O(1) in the hash index.
    // Removing the order from its list is O(1) once its handle is found.
    template <typename EventSink>
    void cancel(OrderId id, EventSink&& event_sink) {
        const auto index_it = order_index_.find(id);

        if (index_it == order_index_.end()) {
            event_sink(Event{RejectEvent{
                id,
                Operation::Cancel,
                RejectReason::UnknownId
            }});
            return;
        }

        // Remove the order from the level and ID index before
        // confirming that the cancellation succeeded.
        remove_resting_order(index_it);

        event_sink(Event{CancelAckEvent{id}});
    }

    // Modify a currently resting limit order.
    //
    // Reducing quantity at the same price preserves queue priority.
    // Increasing quantity or changing price loses that priority:
    // we remove the original order and process a replacement.
    template <typename EventSink>
    void modify(
        OrderId id,
        Price new_price,
        Quantity new_quantity,
        EventSink&& event_sink
    ) {
        auto index_it = order_index_.find(id);

        if (index_it == order_index_.end()) {
            event_sink(Event{RejectEvent{
                id,
                Operation::Modify,
                RejectReason::UnknownId
            }});
            return;
        }

        // Copy the original data before any removal or mutation.
        const RestingOrder original = *index_it->second.order_it;

        if (new_quantity == 0) {
            event_sink(Event{RejectEvent{
                id,
                Operation::Modify,
                RejectReason::ZeroQuantity
            }});
            return;
        }

        if (!range_.contains(new_price)) {
            event_sink(Event{RejectEvent{
                id,
                Operation::Modify,
                RejectReason::PriceOutOfRange
            }});
            return;
        }

        // Case 1: the price is unchanged and quantity does not increase.
        // A reduction preserves both sequence number and list position.
        // An unchanged request is simply acknowledged as a no-op.
        if (new_price == original.price
            && new_quantity <= original.remaining_quantity) {
            if (new_quantity < original.remaining_quantity) {
                LevelMap& levels = levels_for(original.side);
                auto level_it = levels.find(original.price);

                // The removed quantity must also be subtracted from
                // the aggregate quantity stored on this price level.
                const Quantity reduction =
                    original.remaining_quantity - new_quantity;

                level_it->second.total_quantity -= reduction;

                // Updating the existing list element preserves its
                // iterator and therefore its queue position.
                index_it->second.order_it->remaining_quantity =
                    new_quantity;
            }

            event_sink(Event{AckEvent{
                id,
                Operation::Modify
            }});
            return;
        }

        // Case 2: price changed or quantity increased.
        // Treat it as a new incoming limit order, but check it first.
        const OrderRequest replacement =
            OrderRequest::limit_order(
                id,
                original.side,
                new_quantity,
                new_price
            );

        // Exclude the original order from the destination-level total:
        // it will be removed before its replacement is inserted.
        if (would_overflow_if_resting(replacement, id)) {
            event_sink(Event{RejectEvent{
                id,
                Operation::Modify,
                RejectReason::QuantityOverflow
            }});
            return;
        }

        // Cancel-and-replace: the original must disappear before the
        // replacement is processed, so its old priority is lost.
        remove_resting_order(index_it);

        event_sink(Event{AckEvent{
            id,
            Operation::Modify
        }});

        // A replacement can trade immediately. Any remainder that
        // rests receives a fresh sequence number.
        match_and_rest(replacement, new_quantity, event_sink);
    }

    // Return the logical book in a canonical order:
    // bids high-to-low, then asks low-to-high, FIFO within each level.
    //
    // Because the maps and lists already have these ordering properties,
    // we can construct the snapshot without sorting all the orders.
    [[nodiscard]]
    std::vector<RestingOrderSnapshot> snapshot() const {
        std::vector<RestingOrderSnapshot> result;
        result.reserve(order_index_.size());

        // Bids: std::map is ascending, so reverse iteration starts
        // with the highest bid.
        for (auto level_it = bids_.rbegin();
             level_it != bids_.rend();
             ++level_it) {
            for (const auto& order : level_it->second.orders) {
                result.push_back(to_snapshot_record(order));
            }
        }

        // Asks: ordinary iteration starts with the lowest ask.
        for (const auto& [price, level] : asks_) {
            (void)price;

            for (const auto& order : level.orders) {
                result.push_back(to_snapshot_record(order));
            }
        }

        return result;
    }

    // Check structural properties of the complete book.
    //
    // This function is intended for tests and diagnostics, not for
    // the timed matching path.
    [[nodiscard]]
    bool check_invariants() const {
        std::unordered_set<OrderId> seen_ids;
        std::unordered_set<Sequence> seen_sequences;

        seen_ids.reserve(order_index_.size());
        seen_sequences.reserve(order_index_.size());

        std::size_t order_count = 0;

        if (!check_levels(
                bids_, Side::Buy,
                seen_ids, seen_sequences, order_count)) {
            return false;
        }

        if (!check_levels(
                asks_, Side::Sell,
                seen_ids, seen_sequences, order_count)) {
            return false;
        }

        // Every resting order must have exactly one index entry,
        // and no index entry may refer to a missing order.
        if (order_count != order_index_.size()) {
            return false;
        }

        // A valid completed book cannot contain a matchable pair.
        if (!bids_.empty() && !asks_.empty()) {
            const Price best_bid = bids_.rbegin()->first;
            const Price best_ask = asks_.begin()->first;

            if (best_bid >= best_ask) {
                return false;
            }
        }

        return true;
    }

private:
    PriceRange range_;

    // Buy prices and sell prices are maintained separately.
    LevelMap bids_;
    LevelMap asks_;

    // Gives direct access to a live order's list node.
    OrderIndex order_index_;

    // Only orders that actually become resting liquidity receive
    // a sequence number. The counter is independent of wall-clock time.
    Sequence next_sequence_{1};

    // Return the side's price map. This overload allows mutations.
    LevelMap& levels_for(Side side) noexcept {
        return side == Side::Buy ? bids_ : asks_;
    }

    // The const overload is used by read-only queries.
    const LevelMap& levels_for(Side side) const noexcept {
        return side == Side::Buy ? bids_ : asks_;
    }

    // Return the best opposing price, but only if it crosses the
    // incoming limit. A market order has no price restriction.
    [[nodiscard]]
    std::optional<Price> best_opposing_price(
        const OrderRequest& incoming
    ) const {
        if (incoming.side == Side::Buy) {
            // An incoming buy takes the cheapest resting ask.
            if (asks_.empty()) {
                return std::nullopt;
            }

            const Price best_ask = asks_.begin()->first;

            if (incoming.limit_price.has_value()
                && best_ask > *incoming.limit_price) {
                return std::nullopt;
            }

            return best_ask;
        }

        // An incoming sell takes the highest resting bid.
        if (bids_.empty()) {
            return std::nullopt;
        }

        const Price best_bid = bids_.rbegin()->first;

        if (incoming.limit_price.has_value()
            && best_bid < *incoming.limit_price) {
            return std::nullopt;
        }

        return best_bid;
    }

    // Find and execute as many trades as possible for an already
    // validated request. The caller emits the Ack event first.
    template <typename EventSink>
    void match_and_rest(
        const OrderRequest& incoming,
        Quantity remaining,
        EventSink& event_sink
    ) {
        while (remaining > 0) {
            const auto maker_price = best_opposing_price(incoming);

            // No eligible opposing price remains.
            if (!maker_price.has_value()) {
                break;
            }

            // If the incoming side is Buy, the maker rests in asks_;
            // if incoming is Sell, the maker rests in bids_.
            const Side maker_side = incoming.side == Side::Buy
                ? Side::Sell
                : Side::Buy;

            LevelMap& opposing_levels = levels_for(maker_side);
            auto level_it = opposing_levels.find(*maker_price);

            Level& level = level_it->second;

            // FIFO: the order at the front of the list is the oldest
            // resting order at this price.
            RestingOrder& maker_at_front = level.orders.front();

            // Copy the maker record so its ID and price remain available
            // even if this trade fully removes the list node.
            const RestingOrder maker = maker_at_front;

            // Execute only the quantity both sides have available.
            const Quantity executed = std::min(
                remaining,
                maker.remaining_quantity
            );

            remaining -= executed;
            maker_at_front.remaining_quantity -= executed;

            // The level total includes all remaining quantities at this
            // price, so a trade reduces that total by the same amount.
            level.total_quantity -= executed;

            if (maker_at_front.remaining_quantity == 0) {
                // A fully filled order is no longer live or cancellable.
                order_index_.erase(maker.id);
                level.orders.pop_front();
            }

            // If no orders remain at this price, remove the price level.
            // Removing the level invalidates level_it, so do not use it
            // again after this point.
            if (level.orders.empty()) {
                opposing_levels.erase(level_it);
            }

            // Execute at the resting maker's price, not the taker's.
            event_sink(Event{TradeEvent{
                maker.id,
                incoming.id,
                maker.price,
                executed
            }});
        }

        // Only limit orders can leave resting liquidity.
        // A market order's unfilled remainder is discarded.
        if (remaining > 0 && incoming.limit_price.has_value()) {
            add_resting_order(
                incoming.id,
                incoming.side,
                *incoming.limit_price,
                remaining
            );
        }
    }

    // Add an order that has already been validated and has a positive
    // remaining quantity that is safe to add to its price level.
    void add_resting_order(
        OrderId id,
        Side side,
        Price price,
        Quantity quantity
    ) {
        LevelMap& levels = levels_for(side);

        // Create this price level if it doesn't already exist.
        auto level_it = levels.try_emplace(price).first;
        Level& level = level_it->second;

        const Sequence sequence = next_sequence_;
        ++next_sequence_;

        // Append to the back: new resting orders join the end of FIFO.
        auto order_it = level.orders.emplace(
            level.orders.end(),
            RestingOrder{
                id, side, price, quantity, sequence
            }
        );

        // Update the aggregate quantity for this level.
        // Overflow was checked before the order was processed.
        level.total_quantity += quantity;

        // Store the list iterator so cancel and modify can locate this
        // order without scanning every price level or order.
        order_index_.emplace(
            id,
            OrderHandle{side, price, order_it}
        );
    }

    // Remove an already-found live order from both its level and index.
    // The caller must not use index_it after this function returns.
    void remove_resting_order(OrderIndex::iterator index_it) {
        const OrderHandle handle = index_it->second;

        LevelMap& levels = levels_for(handle.side);
        auto level_it = levels.find(handle.price);
        Level& level = level_it->second;

        // Removing an order must update the level's aggregate quantity.
        level.total_quantity -= handle.order_it->remaining_quantity;

        // list::erase removes this node without moving other list nodes.
        level.orders.erase(handle.order_it);

        // Remove the ID index entry as well, so the ID is no longer live.
        order_index_.erase(index_it);

        // Empty price levels should not remain in either map.
        if (level.orders.empty()) {
            levels.erase(level_it);
        }
    }

    // Predict whether an incoming limit order would leave a remainder
    // whose addition would overflow the destination level.
    //
    // The simulation does not mutate the book. It only calculates how
    // much would trade first and how much would remain resting.
    //
    // excluded_id is used by modify(): the original order is removed
    // before its replacement is inserted, so do not count that old
    // order in the destination-level quantity.
    [[nodiscard]]
    bool would_overflow_if_resting(
        const OrderRequest& request,
        std::optional<OrderId> excluded_id = std::nullopt
    ) const {
        // A market order never rests, so it cannot increase a level.
        if (!request.limit_price.has_value()) {
            return false;
        }

        Quantity remaining = request.quantity;
        const Price limit = *request.limit_price;

        // Simulate matches against the opposite side in price-time order.
        if (request.side == Side::Buy) {
            // Asks are naturally ordered from lowest to highest.
            for (const auto& [price, level] : asks_) {
                // More expensive asks cannot be reached by this limit.
                if (price > limit) {
                    break;
                }

                for (const auto& maker : level.orders) {
                    if (maker.remaining_quantity >= remaining) {
                        // This maker fills the incoming quantity.
                        // Nothing is left to rest.
                        return false;
                    }

                    remaining -= maker.remaining_quantity;
                }
            }
        } else {
            // Bids are visited from highest to lowest.
            for (auto level_it = bids_.rbegin();
                 level_it != bids_.rend();
                 ++level_it) {
                if (level_it->first < limit) {
                    break;
                }

                for (const auto& maker : level_it->second.orders) {
                    if (maker.remaining_quantity >= remaining) {
                        return false;
                    }

                    remaining -= maker.remaining_quantity;
                }
            }
        }

        // If the order was fully matched, no destination level grows.
        if (remaining == 0) {
            return false;
        }

        // Calculate the existing quantity at the resting destination
        // price, skipping the old order for a cancel-and-replace modify.
        const LevelMap& same_side_levels = levels_for(request.side);

        Quantity level_total = 0;
        const Quantity maximum =
            std::numeric_limits<Quantity>::max();

        const auto destination = same_side_levels.find(limit);

        if (destination != same_side_levels.end()) {
            for (const auto& existing : destination->second.orders) {
                if (excluded_id.has_value()
                    && existing.id == *excluded_id) {
                    continue;
                }

                // Guard the calculation itself against overflow.
                if (existing.remaining_quantity > maximum - level_total) {
                    return true;
                }

                level_total += existing.remaining_quantity;
            }
        }

        // Safe only if the remainder fits alongside existing quantity.
        return remaining > maximum - level_total;
    }

    // Validate every order and aggregate total in one side's price map.
    // The ID and sequence sets are shared across both sides so duplicates
    // anywhere in the full book are detected.
    [[nodiscard]]
    bool check_levels(
        const LevelMap& levels,
        Side expected_side,
        std::unordered_set<OrderId>& seen_ids,
        std::unordered_set<Sequence>& seen_sequences,
        std::size_t& order_count
    ) const {
        const Quantity maximum =
            std::numeric_limits<Quantity>::max();

        for (const auto& [price, level] : levels) {
            if (!range_.contains(price)) {
                return false;
            }

            // Empty levels should be erased after fills or cancellations.
            if (level.orders.empty()) {
                return false;
            }

            Quantity calculated_total = 0;
            Sequence previous_sequence = 0;

            for (const auto& order : level.orders) {
                if (order.side != expected_side || order.price != price) {
                    return false;
                }

                if (order.remaining_quantity == 0) {
                    return false;
                }

                // Sequence zero is reserved. Every resting order must
                // have been assigned a sequence by this engine instance.
                if (order.sequence == 0
                    || order.sequence >= next_sequence_) {
                    return false;
                }

                // A level's FIFO list must progress from older to newer.
                if (order.sequence <= previous_sequence) {
                    return false;
                }

                previous_sequence = order.sequence;

                if (!seen_ids.insert(order.id).second) {
                    return false;
                }

                if (!seen_sequences.insert(order.sequence).second) {
                    return false;
                }

                // Checked summation: avoid overflow while validating.
                if (order.remaining_quantity > maximum - calculated_total) {
                    return false;
                }

                calculated_total += order.remaining_quantity;

                // Every resting order must have a matching ID-index entry.
                const auto index_it = order_index_.find(order.id);

                if (index_it == order_index_.end()) {
                    return false;
                }

                const OrderHandle& handle = index_it->second;

                if (handle.side != expected_side
                    || handle.price != price
                    || handle.order_it->id != order.id) {
                    return false;
                }

                ++order_count;
            }

            // The stored total must agree with the sum of its orders.
            if (calculated_total != level.total_quantity) {
                return false;
            }
        }

        return true;
    }

    // Convert a private resting order to the public snapshot record.
    // Keeping this conversion in one place avoids duplicating field order.
    [[nodiscard]]
    static RestingOrderSnapshot to_snapshot_record(
        const RestingOrder& order
    ) {
        return RestingOrderSnapshot{
            order.id,
            order.side,
            order.price,
            order.remaining_quantity,
            order.sequence
        };
    }
};

}  // namespace lob

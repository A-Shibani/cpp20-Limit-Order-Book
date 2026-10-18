
#pragma once

#include "lob/events.hpp"
#include "lob/types.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <list>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lob {

// V2.1: flat price-level storage.
//
// What changed from V1:
//   - V1: bids_ and asks_ are ordered std::map objects.
//   - V2.1: bids_ and asks_ are vectors indexed directly by price.
//
// What has NOT changed yet:
//   - Orders within each level still use std::list for FIFO.
//   - Live orders are still located through std::unordered_map.
//   - When a best level empties, finding the next best level scans
//     the relevant part of the price array. A bitmap comes later.
//
// This separation allows us to measure the effect of changing price-level
// storage without simultaneously changing every other data structure.
class BookFlat {
private:
    // One live order resting at a price level.
    struct RestingOrder {
        OrderId id{};
        Side side{Side::Buy};
        Price price{};
        Quantity remaining_quantity{};
        Sequence sequence{};
    };

    // The oldest order is at the front; new orders join the back.
    using OrderQueue = std::list<RestingOrder>;

    // One price level.
    //
    // total_quantity is the sum of every resting order's remaining quantity
    // at this price and side. active is true exactly when the level is
    // expected to contain resting orders.
    struct Level {
        Quantity total_quantity{0};
        OrderQueue orders;
        bool active{false};
    };

    // The handle lets us find one exact order node without scanning all
    // price levels or all orders within the level.
    struct OrderHandle {
        Side side;
        Price price;
        OrderQueue::iterator order_it;
    };

    using OrderIndex = std::unordered_map<OrderId, OrderHandle>;

public:
    // Allocate every level in the configured price range once.
    //
    // For the default range [1, 65535], each side has 65,535 Level
    // objects. The vectors do not grow as orders arrive; only the lists
    // and ID index allocate dynamically in this version.
    explicit BookFlat(PriceRange range = {})
        : range_(range),
          bids_(checked_level_count(range)),
          asks_(checked_level_count(range)) {
    }

    // Submit either a limit order or a market order.
    //
    // The event sink receives output synchronously. It must not call
    // back into this same book while an operation is in progress.
    template <typename EventSink>
    void submit(
        const OrderRequest& request,
        EventSink&& event_sink
    ) {
        // Only resting orders occupy the live-ID index. An ID can be
        // reused after its previous order is filled or cancelled.
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

        // Market orders have no limit price to validate.
        if (request.limit_price.has_value()
            && !range_.contains(*request.limit_price)) {
            event_sink(Event{RejectEvent{
                request.id,
                Operation::Submit,
                RejectReason::PriceOutOfRange
            }});
            return;
        }

        // Check quantity overflow before emitting an acknowledgement or
        // executing any trades. Rejection must leave the book unchanged.
        if (would_overflow_if_resting(request)) {
            event_sink(Event{RejectEvent{
                request.id,
                Operation::Submit,
                RejectReason::QuantityOverflow
            }});
            return;
        }

        // The specified event order is Ack first, then Trade events.
        event_sink(Event{AckEvent{
            request.id,
            Operation::Submit
        }});

        // Match against the opposing side and rest any eligible limit
        // remainder. An unfilled market remainder will not be stored.
        match_and_rest(request, request.quantity, event_sink);
    }

    // Cancel a currently resting order.
    template <typename EventSink>
    void cancel(OrderId id, EventSink&& event_sink) {
        // The hash index finds the order handle without a full-book scan.
        auto index_it = order_index_.find(id);

        if (index_it == order_index_.end()) {
            event_sink(Event{RejectEvent{
                id,
                Operation::Cancel,
                RejectReason::UnknownId
            }});
            return;
        }

        // Remove the order from its level and from the index before
        // telling the caller that cancellation succeeded.
        remove_resting_order(index_it);

        event_sink(Event{CancelAckEvent{id}});
    }

    // Modify a currently resting order.
    //
    // Rules:
    //   - Same price + smaller quantity: retain queue priority.
    //   - Same price + same quantity: acknowledge a no-op.
    //   - Price change or quantity increase: cancel-and-replace.
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

        // Keep a copy before any mutation invalidates the original handle.
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

        // Case 1: only a quantity reduction, or an exact no-op.
        // Updating the existing node preserves its position in the FIFO
        // list and preserves its original sequence number.
        if (new_price == original.price
            && new_quantity <= original.remaining_quantity) {
            if (new_quantity < original.remaining_quantity) {
                Level& level =
                    levels_for(original.side)[index_for(original.price)];

                // The level aggregate must be reduced by exactly the same
                // amount as the resting order's remaining quantity.
                const Quantity reduction =
                    original.remaining_quantity - new_quantity;

                level.total_quantity -= reduction;
                index_it->second.order_it->remaining_quantity =
                    new_quantity;
            }

            event_sink(Event{AckEvent{
                id,
                Operation::Modify
            }});
            return;
        }

        // Case 2: changing price or increasing quantity loses priority.
        // Validate the replacement before removing the original order.
        const OrderRequest replacement =
            OrderRequest::limit_order(
                id,
                original.side,
                new_quantity,
                new_price
            );

        // Exclude the original ID from the destination price total because
        // it will be removed before the replacement is processed.
        if (would_overflow_if_resting(replacement, id)) {
            event_sink(Event{RejectEvent{
                id,
                Operation::Modify,
                RejectReason::QuantityOverflow
            }});
            return;
        }

        // The old order is now removed. Do not use index_it after this call:
        // remove_resting_order erases that entry from the hash index.
        remove_resting_order(index_it);

        event_sink(Event{AckEvent{
            id,
            Operation::Modify
        }});

        // The replacement can immediately trade against opposing liquidity.
        // Any remaining quantity receives a fresh sequence number if it rests.
        match_and_rest(replacement, new_quantity, event_sink);
    }

    // Return a canonical snapshot independent of the internal data layout.
    //
    // The ID index lets us enumerate all resting orders without scanning
    // every possible price tick. We sort the copied snapshot afterward so
    // callers see the same logical order from V0, V1 and V2.1.
    [[nodiscard]]
    std::vector<RestingOrderSnapshot> snapshot() const {
        std::vector<RestingOrderSnapshot> result;
        result.reserve(order_index_.size());

        for (const auto& [id, handle] : order_index_) {
            (void)id;

            // The handle points to this order's stable list node.
            const RestingOrder& order = *handle.order_it;

            result.push_back(RestingOrderSnapshot{
                order.id,
                order.side,
                order.price,
                order.remaining_quantity,
                order.sequence
            });
        }

        // Canonical order:
        //   1. Bids before asks.
        //   2. Highest bid to lowest bid.
        //   3. Lowest ask to highest ask.
        //   4. FIFO sequence within the same price.
        std::sort(
            result.begin(),
            result.end(),
            [](const RestingOrderSnapshot& lhs,
               const RestingOrderSnapshot& rhs) {
                if (lhs.side != rhs.side) {
                    return lhs.side == Side::Buy;
                }

                if (lhs.price != rhs.price) {
                    if (lhs.side == Side::Buy) {
                        return lhs.price > rhs.price;
                    }

                    return lhs.price < rhs.price;
                }

                return lhs.sequence < rhs.sequence;
            }
        );

        return result;
    }

    // Check structural and bookkeeping invariants.
    //
    // This method is for tests and diagnostics, not the timed matching path.
    // It visits occupied levels through the ID index rather than scanning
    // all 65,535 possible prices on every test operation.
    [[nodiscard]]
    bool check_invariants() const {
        std::unordered_set<OrderId> seen_ids;
        std::unordered_set<Sequence> seen_sequences;

        // A level key encodes both side and price-array index. This lets us
        // inspect each occupied level once even though many orders can share
        // a level.
        std::unordered_set<std::uint64_t> visited_level_keys;

        seen_ids.reserve(order_index_.size());
        seen_sequences.reserve(order_index_.size());
        visited_level_keys.reserve(active_level_count_);

        std::size_t counted_orders = 0;

        std::optional<Price> calculated_best_bid;
        std::optional<Price> calculated_best_ask;

        // Start from every live ID entry and check the order handle.
        for (const auto& [id, handle] : order_index_) {
            if (!range_.contains(handle.price)) {
                return false;
            }

            const std::vector<Level>& levels = levels_for(handle.side);
            const Level& level = levels[index_for(handle.price)];

            // Every indexed order must point into a non-empty active level.
            if (!level.active || level.orders.empty()) {
                return false;
            }

            if (handle.order_it == level.orders.end()) {
                return false;
            }

            const RestingOrder& pointed_order = *handle.order_it;

            // The ID index must point to the requested ID, side and price.
            if (pointed_order.id != id
                || pointed_order.side != handle.side
                || pointed_order.price != handle.price) {
                return false;
            }

            // Track the best prices observed through the live-order index.
            if (handle.side == Side::Buy) {
                if (!calculated_best_bid.has_value()
                    || handle.price > *calculated_best_bid) {
                    calculated_best_bid = handle.price;
                }
            } else {
                if (!calculated_best_ask.has_value()
                    || handle.price < *calculated_best_ask) {
                    calculated_best_ask = handle.price;
                }
            }

            // Encode the side as one bit of distinction so a bid level and
            // ask level at the same numerical price count as two levels.
            const std::uint64_t level_key =
                static_cast<std::uint64_t>(index_for(handle.price)) * 2U
                + (handle.side == Side::Buy ? 0U : 1U);

            // Only sum a level once, even when several orders rest there.
            if (!visited_level_keys.insert(level_key).second) {
                continue;
            }

            Quantity calculated_total = 0;
            Sequence previous_sequence = 0;

            const Quantity maximum =
                std::numeric_limits<Quantity>::max();

            // Verify every order in this level and its reverse ID index.
            for (auto order_it = level.orders.cbegin();
                 order_it != level.orders.cend();
                 ++order_it) {
                const RestingOrder& order = *order_it;

                if (order.side != handle.side
                    || order.price != handle.price) {
                    return false;
                }

                if (order.remaining_quantity == 0) {
                    return false;
                }

                if (order.sequence == 0
                    || order.sequence >= next_sequence_) {
                    return false;
                }

                // FIFO sequence numbers must increase from front to back.
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

                // Checked summation avoids overflow in the invariant checker.
                if (order.remaining_quantity > maximum - calculated_total) {
                    return false;
                }
                calculated_total += order.remaining_quantity;

                // Every list node must have an ID-index entry pointing back
                // to this exact node, side and price.
                const auto reverse_it = order_index_.find(order.id);

                if (reverse_it == order_index_.end()) {
                    return false;
                }

                const OrderHandle& reverse_handle = reverse_it->second;

                if (reverse_handle.side != order.side
                    || reverse_handle.price != order.price
                    || reverse_handle.order_it != order_it) {
                    return false;
                }

                ++counted_orders;
            }

            // Stored level total and the independently computed total agree.
            if (calculated_total != level.total_quantity) {
                return false;
            }
        }

        // These checks detect missing index entries, extra index entries,
        // or occupied levels that aren't accounted for by the index.
        if (counted_orders != order_index_.size()
            || counted_orders != resting_order_count_) {
            return false;
        }

        if (visited_level_keys.size() != active_level_count_) {
            return false;
        }

        // Cached best prices must agree with the live orders.
        if (calculated_best_bid != best_bid_
            || calculated_best_ask != best_ask_) {
            return false;
        }

        // A completed order-book operation must not leave a crossed book.
        if (best_bid_.has_value() && best_ask_.has_value()
            && *best_bid_ >= *best_ask_) {
            return false;
        }

        return true;
    }

private:
    // The number of array slots required for an inclusive price range.
    // Validate the range before allocating either side.
    [[nodiscard]]
    static std::size_t checked_level_count(PriceRange range) {
        if (!range.valid()) {
            throw std::invalid_argument("Invalid price range");
        }

        return static_cast<std::size_t>(range.maximum)
            - static_cast<std::size_t>(range.minimum)
            + 1U;
    }

    PriceRange range_;

    // Each vector is a fixed flat array once the constructor completes.
    // A price maps directly to one Level using index_for(price).
    std::vector<Level> bids_;
    std::vector<Level> asks_;

    // Average O(1) ID lookup under normal hash-table conditions.
    OrderIndex order_index_;

    // Cached best prices avoid scanning the array on every matching attempt.
    // We refresh a cache when its best level becomes empty.
    std::optional<Price> best_bid_;
    std::optional<Price> best_ask_;

    // These counters help validate that level storage and the ID index agree.
    Sequence next_sequence_{1};
    std::size_t resting_order_count_{0};
    std::size_t active_level_count_{0};

    // Convert a valid price into a zero-based array index.
    [[nodiscard]]
    std::size_t index_for(Price price) const noexcept {
        return static_cast<std::size_t>(price)
            - static_cast<std::size_t>(range_.minimum);
    }

    // Convert an array index back into its price.
    [[nodiscard]]
    Price price_for_index(std::size_t index) const noexcept {
        return static_cast<Price>(
            static_cast<std::size_t>(range_.minimum) + index
        );
    }

    // Mutable access to one side's fixed level array.
    std::vector<Level>& levels_for(Side side) noexcept {
        return side == Side::Buy ? bids_ : asks_;
    }

    // Read-only access for validation and query operations.
    const std::vector<Level>& levels_for(Side side) const noexcept {
        return side == Side::Buy ? bids_ : asks_;
    }

    // Return the current best eligible opposing price, if one exists.
    // A market order has no price restriction.
    [[nodiscard]]
    std::optional<Price> best_opposing_price(
        const OrderRequest& incoming
    ) const {
        if (incoming.side == Side::Buy) {
            if (!best_ask_.has_value()) {
                return std::nullopt;
            }

            const Price best_ask = *best_ask_;

            if (incoming.limit_price.has_value()
                && best_ask > *incoming.limit_price) {
                return std::nullopt;
            }

            return best_ask;
        }

        if (!best_bid_.has_value()) {
            return std::nullopt;
        }

        const Price best_bid = *best_bid_;

        if (incoming.limit_price.has_value()
            && best_bid < *incoming.limit_price) {
            return std::nullopt;
        }

        return best_bid;
    }

    // Find the next active price strictly beyond current_price in priority
    // order for a particular side.
    //
    // This version scans array slots. Later we will replace this search with
    // a bitmap, so we can measure the bitmap improvement separately.
    [[nodiscard]]
    std::optional<Price> next_non_empty_price(
        Side side,
        Price current_price
    ) const {
        const std::vector<Level>& levels = levels_for(side);
        const std::size_t current_index = index_for(current_price);

        if (side == Side::Sell) {
            // Asks: search toward higher prices.
            for (std::size_t index = current_index + 1U;
                 index < levels.size();
                 ++index) {
                if (levels[index].active
                    && !levels[index].orders.empty()) {
                    return price_for_index(index);
                }
            }
        } else {
            // Bids: search toward lower prices. The guard avoids unsigned
            // underflow when current_index reaches zero.
            std::size_t index = current_index;

            while (index > 0U) {
                --index;

                if (levels[index].active
                    && !levels[index].orders.empty()) {
                    return price_for_index(index);
                }
            }
        }

        return std::nullopt;
    }

    // Refresh the cached best price after its former best level becomes empty.
    //
    // The price range is bounded. We scan outward from the emptied level
    // until another occupied level is found. If none exists, the side is empty.
    void refresh_best_after_empty(
        Side side,
        Price emptied_price
    ) {
        if (side == Side::Buy) {
            // Removing a non-best bid does not affect the best bid cache.
            if (!best_bid_.has_value()
                || *best_bid_ != emptied_price) {
                return;
            }

            best_bid_.reset();

            const std::size_t emptied_index = index_for(emptied_price);

            // Lower bids have lower array indices.
            std::size_t index = emptied_index;

            while (index > 0U) {
                --index;

                if (bids_[index].active
                    && !bids_[index].orders.empty()) {
                    best_bid_ = price_for_index(index);
                    return;
                }
            }

            return;
        }

        // Removing a non-best ask does not affect the best ask cache.
        if (!best_ask_.has_value()
            || *best_ask_ != emptied_price) {
            return;
        }

        best_ask_.reset();

        const std::size_t emptied_index = index_for(emptied_price);

        // Higher asks have higher array indices.
        for (std::size_t index = emptied_index + 1U;
             index < asks_.size();
             ++index) {
            if (asks_[index].active
                && !asks_[index].orders.empty()) {
                best_ask_ = price_for_index(index);
                return;
            }
        }
    }

    // Remove an existing order from its level and the ID index.
    //
    // The iterator passed in refers to the ID-index entry. It becomes invalid
    // when order_index_.erase() runs, so the caller must not use it afterward.
    void remove_resting_order(OrderIndex::iterator index_it) {
        const OrderHandle handle = index_it->second;

        std::vector<Level>& levels = levels_for(handle.side);
        Level& level = levels[index_for(handle.price)];

        // Remove this order's contribution to the level aggregate.
        level.total_quantity -=
            handle.order_it->remaining_quantity;

        // Erasing a list node preserves iterators to the other orders.
        level.orders.erase(handle.order_it);

        // Remove the order's ID lookup entry and update the live-order count.
        order_index_.erase(index_it);
        --resting_order_count_;

        // If this was the last order at the price, that level is now inactive.
        if (level.orders.empty()) {
            level.active = false;
            --active_level_count_;

            // If the emptied level was the best price, find its replacement.
            refresh_best_after_empty(handle.side, handle.price);
        }
    }

    // Add leftover limit quantity as a new resting order.
    //
    // The caller must already have validated the request and checked level
    // quantity overflow. This helper is only called when quantity is positive.
    void add_resting_order(
        OrderId id,
        Side side,
        Price price,
        Quantity quantity
    ) {
        std::vector<Level>& levels = levels_for(side);
        Level& level = levels[index_for(price)];

        // Remember whether this is a new occupied price level.
        const bool level_was_empty = level.orders.empty();

        // A fresh sequence number puts the new order at the back of FIFO.
        const Sequence sequence = next_sequence_;

        auto order_it = level.orders.emplace(
            level.orders.end(),
            RestingOrder{
                id,
                side,
                price,
                quantity,
                sequence
            }
        );

        // Update the level's aggregate quantity.
        // The overflow check was completed before matching began.
        level.total_quantity += quantity;

        // An empty level becomes active the first time an order rests there.
        if (level_was_empty) {
            level.active = true;
            ++active_level_count_;
        }

        // Maintain the cached best price when adding a new level.
        if (side == Side::Buy) {
            if (!best_bid_.has_value() || price > *best_bid_) {
                best_bid_ = price;
            }
        } else {
            if (!best_ask_.has_value() || price < *best_ask_) {
                best_ask_ = price;
            }
        }

        // Save the new order's list iterator so it can be found by ID.
        order_index_.emplace(
            id,
            OrderHandle{side, price, order_it}
        );

        ++resting_order_count_;
        ++next_sequence_;
    }

    // Match an accepted incoming order, then rest its limit remainder.
    template <typename EventSink>
    void match_and_rest(
        const OrderRequest& incoming,
        Quantity remaining,
        EventSink& event_sink
    ) {
        while (remaining > 0U) {
            const auto maker_price = best_opposing_price(incoming);

            // No opposing price is available within the incoming limit.
            if (!maker_price.has_value()) {
                break;
            }

            // A buy matches against asks; a sell matches against bids.
            const Side maker_side = incoming.side == Side::Buy
                ? Side::Sell
                : Side::Buy;

            std::vector<Level>& opposing_levels =
                levels_for(maker_side);

            const std::size_t maker_index = index_for(*maker_price);
            Level& level = opposing_levels[maker_index];

            // FIFO means the oldest order at this price is at the front.
            RestingOrder& maker_at_front = level.orders.front();

            // Copy maker details before erasing a fully filled list node.
            const RestingOrder maker = maker_at_front;

            // The trade can never be larger than either order's remainder.
            const Quantity executed = std::min(
                remaining,
                maker_at_front.remaining_quantity
            );

            remaining -= executed;
            maker_at_front.remaining_quantity -= executed;

            // Keep the per-price aggregate equal to its live order quantities.
            level.total_quantity -= executed;

            if (maker_at_front.remaining_quantity == 0U) {
                // A fully filled maker is no longer live or cancellable.
                order_index_.erase(maker.id);
                level.orders.pop_front();
                --resting_order_count_;

                // If this was the final order at the price, mark the level
                // inactive and refresh the best-price cache if necessary.
                if (level.orders.empty()) {
                    level.active = false;
                    --active_level_count_;

                    refresh_best_after_empty(maker_side, maker.price);
                }
            }

            // Trades always execute at the resting maker's price.
            event_sink(Event{TradeEvent{
                maker.id,
                incoming.id,
                maker.price,
                executed
            }});
        }

        // A limit order's unfilled remainder can rest at its limit price.
        // A market order has no limit price and therefore never rests.
        if (remaining > 0U && incoming.limit_price.has_value()) {
            add_resting_order(
                incoming.id,
                incoming.side,
                *incoming.limit_price,
                remaining
            );
        }
    }

    // Calculate how much of a request would remain after matching, then
    // check whether that remainder could overflow its destination level.
    //
    // This is a read-only simulation: it does not erase orders or emit trades.
    // For each eligible price, the level aggregate is enough to determine
    // whether the incoming quantity would be fully filled at that level.
    //
    // excluded_id is used by modify(). The original order is removed before
    // its replacement is processed, so its quantity must not be counted twice.
    [[nodiscard]]
    bool would_overflow_if_resting(
        const OrderRequest& request,
        std::optional<OrderId> excluded_id = std::nullopt
    ) const {
        // A market order's remainder always expires instead of resting.
        if (!request.limit_price.has_value()) {
            return false;
        }

        Quantity remaining = request.quantity;

        const Side maker_side = request.side == Side::Buy
            ? Side::Sell
            : Side::Buy;

        auto current_price = best_opposing_price(request);

        while (current_price.has_value() && remaining > 0U) {
            const Price price = *current_price;

            // Stop once subsequent prices are outside a limit order's
            // permitted range. best_opposing_price checks the first price;
            // this condition checks every later price reached by the scan.
            if (request.limit_price.has_value()) {
                if (request.side == Side::Buy
                    && price > *request.limit_price) {
                    break;
                }

                if (request.side == Side::Sell
                    && price < *request.limit_price) {
                    break;
                }
            }

            const Level& maker_level =
                levels_for(maker_side)[index_for(price)];

            // If this level alone can fill the incoming remainder, nothing
            // would be left to rest, so destination-level overflow is impossible.
            if (maker_level.total_quantity >= remaining) {
                return false;
            }

            // Otherwise the incoming order would consume this entire level.
            remaining -= maker_level.total_quantity;

            // Proceed to the next occupied level in price priority order.
            current_price = next_non_empty_price(maker_side, price);
        }

        if (remaining == 0U) {
            return false;
        }

        // Read the current quantity at the incoming order's destination level.
        const Price destination_price = *request.limit_price;
        const Level& destination_level =
            levels_for(request.side)[index_for(destination_price)];

        Quantity existing_quantity = destination_level.total_quantity;

        // For cancel-and-replace, subtract the old order's contribution when
        // the old and new orders have the same side and destination price.
        if (excluded_id.has_value()) {
            const auto old_it = order_index_.find(*excluded_id);

            if (old_it != order_index_.end()
                && old_it->second.side == request.side
                && old_it->second.price == destination_price) {
                const Quantity old_quantity =
                    old_it->second.order_it->remaining_quantity;

                if (old_quantity > existing_quantity) {
                    // Indicates inconsistent level bookkeeping.
                    return true;
                }

                existing_quantity -= old_quantity;
            }
        }

        // Safe only if the new resting remainder fits alongside the existing
        // destination quantity without exceeding the integer maximum.
        const Quantity maximum =
            std::numeric_limits<Quantity>::max();

        return remaining > maximum - existing_quantity;
    }
};

}  // namespace lob

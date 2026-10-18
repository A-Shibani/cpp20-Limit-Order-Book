
#pragma once

#include "lob/events.hpp"
#include "lob/types.hpp"

#include <limits>
#include <algorithm>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <vector>

namespace lob {

// V0: deliberately simple reference implementation.
//
// Orders are kept in a vector. To find a match, we scan the vector
// and select the best opposing order by price and then sequence.
// This favours clarity and auditability over performance.
class BookRef {
public:
    explicit BookRef(PriceRange range = {}) : range_(range) {
        if (!range_.valid()) {
            throw std::invalid_argument("Invalid price range");
        }
    }

    // The event sink is called synchronously.
    // It must not re-enter this book while an operation is in progress.
    template <typename EventSink>
    void submit(
        const OrderRequest& request,
        EventSink&& event_sink
    ) {
        // Reject an ID that is already attached to a resting order.
        if (contains_live_order(request.id)) {
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

        if (would_overflow_if_resting(request)) { event_sink(Event{RejectEvent{
        request.id,
        Operation::Submit,
        RejectReason::QuantityOverflow
            }});
            return;
        }

        // A valid request is acknowledged before its trade events.
        event_sink(Event{AckEvent{
            request.id,
            Operation::Submit
        }});

        Quantity remaining = request.quantity;

        // Repeatedly find the best eligible resting order.
        while (remaining > 0) {
            const auto maker_index = find_best_match(request);

            if (!maker_index.has_value()) {
                break;
            }

            // Copy the maker details before changing or erasing the order.
            const RestingOrder maker = orders_[*maker_index];

            const Quantity executed = std::min(
                remaining,
                maker.remaining_quantity
            );

            remaining -= executed;
            orders_[*maker_index].remaining_quantity -= executed;

            if (orders_[*maker_index].remaining_quantity == 0) {
                const auto position = orders_.begin()
                    + static_cast<std::ptrdiff_t>(*maker_index);

                orders_.erase(position);
            }

            // Trades execute at the resting maker's price.
            event_sink(Event{TradeEvent{
                maker.id,
                request.id,
                maker.price,
                executed
            }});
        }

        // An unfilled limit remainder becomes resting liquidity.
        // Market orders never rest.
        if (remaining > 0 && request.limit_price.has_value()) {
            orders_.push_back(RestingOrder{
                request.id,
                request.side,
                *request.limit_price,
                remaining,
                next_sequence_
            });

            ++next_sequence_;
        }
    }

    
    template <typename EventSink>
    void cancel(
        OrderId id,
        EventSink&& event_sink
    ) {
        const auto it = std::find_if(
            orders_.begin(),
            orders_.end(),
            [id](const RestingOrder& order) {
                return order.id == id;
            }
        );

        if (it == orders_.end()) {
            event_sink(Event{RejectEvent{
                id,
                Operation::Cancel,
                RejectReason::UnknownId
            }});
            return;
        }

        // Remove the order before notifying the caller.
        orders_.erase(it);

        event_sink(Event{CancelAckEvent{id}});
    }

    
    template <typename EventSink>
    void modify(
        OrderId id,
        Price new_price,
        Quantity new_quantity,
        EventSink&& event_sink
    ) {
        const auto it = std::find_if(
            orders_.begin(),
            orders_.end(),
            [id](const RestingOrder& order) {
                return order.id == id;
            }
        );

        // 1. Validate the order ID.
        if (it == orders_.end()) {
            event_sink(Event{RejectEvent{
                id,
                Operation::Modify,
                RejectReason::UnknownId
            }});
            return;
        }

        // 2. Validate quantity.
        if (new_quantity == 0) {
            event_sink(Event{RejectEvent{
                id,
                Operation::Modify,
                RejectReason::ZeroQuantity
            }});
            return;
        }

        // 3. Validate price.
        if (!range_.contains(new_price)) {
            event_sink(Event{RejectEvent{
                id,
                Operation::Modify,
                RejectReason::PriceOutOfRange
            }});
            return;
        }

        const RestingOrder original = *it;

        // A reduction at the same price preserves queue priority.
        // An unchanged price and quantity is a successful no-op.
        if (new_price == original.price
            && new_quantity <= original.remaining_quantity) {
            if (new_quantity < original.remaining_quantity) {
                it->remaining_quantity = new_quantity;
            }

            event_sink(Event{AckEvent{
                id,
                Operation::Modify
            }});
            return;
        }

        const OrderRequest replacement = OrderRequest::limit_order(
            id,
            original.side,
            new_quantity,
            new_price
            );

        if (would_overflow_if_resting(replacement, id)) {
            event_sink(Event{RejectEvent{
                id,
                Operation::Modify,
                RejectReason::QuantityOverflow
            }});
            return;
        }

        // A price change or quantity increase loses queue priority.
        // Remove the old order before processing its replacement.
        orders_.erase(it);

        event_sink(Event{AckEvent{
            id,
            Operation::Modify
        }});

        Quantity remaining = new_quantity;

        // Process the replacement as an incoming limit order.
        while (remaining > 0) {
            const auto maker_index = find_best_match(replacement);

            if (!maker_index.has_value()) {
                break;
            }

            const RestingOrder maker = orders_[*maker_index];

            const Quantity executed = std::min(
                remaining,
                maker.remaining_quantity
            );

            remaining -= executed;
            orders_[*maker_index].remaining_quantity -= executed;

            if (orders_[*maker_index].remaining_quantity == 0) {
                const auto position = orders_.begin()
                    + static_cast<std::ptrdiff_t>(*maker_index);

                orders_.erase(position);
            }

            event_sink(Event{TradeEvent{
                maker.id,
                id,
                maker.price,
                executed
            }});
        }

        // A replacement remainder rests at the new price with
        // a fresh sequence number. A fully executed replacement
        // does not receive a sequence number.
        if (remaining > 0) {
            orders_.push_back(RestingOrder{
                id,
                original.side,
                new_price,
                remaining,
                next_sequence_
            });

            ++next_sequence_;
        }
    }


    // Return a canonical snapshot:
    // bids from highest to lowest, then asks from lowest to highest;
    // orders at the same price are sorted by sequence.
    [[nodiscard]]
    std::vector<RestingOrderSnapshot> snapshot() const {
        std::vector<RestingOrderSnapshot> result;
        result.reserve(orders_.size());

        for (const auto& order : orders_) {
            result.push_back(RestingOrderSnapshot{
                order.id,
                order.side,
                order.price,
                order.remaining_quantity,
                order.sequence
            });
        }

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

    // Check invariants that apply to this initial implementation.
    [[nodiscard]]
    bool check_invariants() const {
        bool has_bid = false;
        bool has_ask = false;
        Price best_bid = 0;
        Price best_ask = 0;

        for (std::size_t i = 0; i < orders_.size(); ++i) {
            const auto& order = orders_[i];

            if (order.remaining_quantity == 0) {
                return false;
            }

            if (!range_.contains(order.price)) {
                return false;
            }

            if (order.sequence == 0
                || order.sequence >= next_sequence_) {
                return false;
            }

            if (order.side == Side::Buy) {
                if (!has_bid || order.price > best_bid) {
                    best_bid = order.price;
                }
                has_bid = true;
            } else {
                if (!has_ask || order.price < best_ask) {
                    best_ask = order.price;
                }
                has_ask = true;
            }

            for (std::size_t j = i + 1; j < orders_.size(); ++j) {
                const auto& other = orders_[j];

                // Live order IDs and resting sequence numbers are unique.
                if (order.id == other.id
                    || order.sequence == other.sequence) {
                    return false;
                }

                // The vector preserves insertion order. Within the
                // same price level, sequence numbers must increase.
                if (order.side == other.side
                    && order.price == other.price
                    && order.sequence >= other.sequence) {
                    return false;
                }
            }
        }

        // A valid book cannot have a bid at or above its best ask.
        if (has_bid && has_ask && best_bid >= best_ask) {
            return false;
        }

        return true;
    }

private:
    struct RestingOrder {
        OrderId id{};
        Side side{Side::Buy};
        Price price{};
        Quantity remaining_quantity{};
        Sequence sequence{};
    };

    PriceRange range_;
    std::vector<RestingOrder> orders_;
    Sequence next_sequence_{1};

    [[nodiscard]]
    bool contains_live_order(OrderId id) const {
        return std::any_of(
            orders_.begin(),
            orders_.end(),
            [id](const RestingOrder& order) {
                return order.id == id;
            }
        );
    }

    [[nodiscard]]
    bool crosses(
        const OrderRequest& incoming,
        const RestingOrder& maker
    ) const {
        if (maker.side == incoming.side) {
            return false;
        }

        // A market order has no price limit.
        if (incoming.is_market()) {
            return true;
        }

        const Price limit = *incoming.limit_price;

        if (incoming.side == Side::Buy) {
            // A buy limit can execute at prices at or below its limit.
            return maker.price <= limit;
        }

        // A sell limit can execute at prices at or above its limit.
        return maker.price >= limit;
    }

    
    [[nodiscard]]
    bool would_overflow_if_resting(
        const OrderRequest& request,
        std::optional<OrderId> excluded_id = std::nullopt
    ) const {
        // Market orders never rest, so they cannot overflow a level.
        if (request.is_market()) {
            return false;
        }

        // Simulate matching without mutating the book.
        // This determines how much quantity would actually rest.
        Quantity remaining = request.quantity;

        std::vector<const RestingOrder*> candidates;
        candidates.reserve(orders_.size());

        for (const auto& order : orders_) {
            if (crosses(request, order)) {
                candidates.push_back(&order);
            }
        }

        std::sort(
            candidates.begin(),
            candidates.end(),
            [&request](
                const RestingOrder* lhs,
                const RestingOrder* rhs
            ) {
                if (lhs->price != rhs->price) {
                    if (request.side == Side::Buy) {
                        return lhs->price < rhs->price;
                    }
                    return lhs->price > rhs->price;
                }

                return lhs->sequence < rhs->sequence;
            }
        );

        for (const auto* maker : candidates) {
            if (maker->remaining_quantity >= remaining) {
                remaining = 0;
                break;
            }

            remaining -= maker->remaining_quantity;
        }

        // Nothing rests, so there is no resting-level overflow.
        if (remaining == 0) {
            return false;
        }

        const Quantity maximum =
            std::numeric_limits<Quantity>::max();

        // Calculate existing quantity at the destination level.
        // For modify, exclude the original order because it will be
        // removed before its replacement is processed.
        Quantity level_total = 0;

        for (const auto& order : orders_) {
            if (order.side != request.side
                || order.price != *request.limit_price) {
                continue;
            }

            if (excluded_id.has_value()
                && order.id == *excluded_id) {
                continue;
            }

            // A pre-existing overflow would also be unsafe.
            if (order.remaining_quantity > maximum - level_total) {
                return true;
            }

            level_total += order.remaining_quantity;
        }

        return remaining > maximum - level_total;
    }


    [[nodiscard]]
    std::optional<std::size_t> find_best_match(
        const OrderRequest& incoming
    ) const {
        std::optional<std::size_t> best_index;

        for (std::size_t i = 0; i < orders_.size(); ++i) {
            const auto& candidate = orders_[i];

            if (!crosses(incoming, candidate)) {
                continue;
            }

            if (!best_index.has_value()) {
                best_index = i;
                continue;
            }

            const auto& current_best = orders_[*best_index];

            bool better_price = false;

            if (incoming.side == Side::Buy) {
                // A buyer takes the lowest available ask first.
                better_price = candidate.price < current_best.price;
            } else {
                // A seller takes the highest available bid first.
                better_price = candidate.price > current_best.price;
            }

            const bool same_price = (
                candidate.price == current_best.price
            );

            const bool earlier_in_queue = (
                candidate.sequence < current_best.sequence
            );

            if (better_price
                || (same_price && earlier_in_queue)) {
                best_index = i;
            }
        }

        return best_index;
    }
};

}  // namespace lob


#pragma once

#include "lob/events.hpp"
#include "lob/pool.hpp"
#include "lob/types.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lob {

// V2.3: flat price levels + bitmap + preallocated order pool.
//
// Compared with V2.2, the price arrays and bitmap are retained, but
// std::list nodes are replaced by pool slots linked through integer indices.
//
// A live order appears in three places:
//   1. A slot in OrderPool.
//   2. A FIFO chain at its price level.
//   3. order_index_, which maps its ID to the slot.
//
// Every operation must keep those representations consistent.
class BookPool {
private:
    using Index = OrderPool::Index;

    static constexpr Index no_slot = OrderPool::no_slot;

    // A price level maintains the head and tail of an intrusive FIFO chain.
    //
    // head is the oldest order; tail is the newest order. The chain links
    // themselves live inside OrderPool::OrderSlot::prev and ::next.
    struct Level {
        Quantity total_quantity{0};
        Index head{no_slot};
        Index tail{no_slot};
        bool active{false};
    };

    // The ID index points directly to a slot rather than to a list iterator.
    struct OrderHandle {
        Side side{Side::Buy};
        Price price{};
        Index slot{no_slot};
    };

    using OrderIndex = std::unordered_map<OrderId, OrderHandle>;

public:
    // order_capacity is the maximum number of simultaneously resting orders.
    //
    // The pool and fixed price arrays are allocated at construction time.
    // We reserve hash-table buckets here too, reducing rehashing as orders
    // arrive. The unordered_map can still allocate individual entries.
    explicit BookPool(
        std::size_t order_capacity = 65'536,
        PriceRange range = {}
    )
        : range_(range),
          bids_(checked_level_count(range)),
          asks_(checked_level_count(range)),
          bid_bitmap_(
              checked_bitmap_word_count(range),
              std::uint64_t{0}
          ),
          ask_bitmap_(
              checked_bitmap_word_count(range),
              std::uint64_t{0}
          ),
          pool_(order_capacity) {
        order_index_.reserve(order_capacity);
    }

    // Submit a limit or market order.
    template <typename EventSink>
    void submit(
        const OrderRequest& request,
        EventSink&& event_sink
    ) {
        // Only currently resting orders occupy an ID. Filled or cancelled
        // IDs can be reused, but duplicate live IDs must be rejected.
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

        // Reject before trading if the eventual resting remainder would
        // overflow its destination level. This simulation does not mutate
        // the book or emit trades.
        if (would_overflow_if_resting(request)) {
            event_sink(Event{RejectEvent{
                request.id,
                Operation::Submit,
                RejectReason::QuantityOverflow
            }});
            return;
        }

        // With no free slot, an order that cannot match needs to be rejected
        // before its Ack. If it can match, fully consumed makers release slots;
        // if it fills completely, it never needs a resting slot.
        //
        // Thus an eligible match can proceed safely even when the pool starts
        // full. A market order never needs a new resting slot.
        if (request.limit_price.has_value()
            && pool_.available() == 0U
            && !best_opposing_price(request).has_value()) {
            event_sink(Event{RejectEvent{
                request.id,
                Operation::Submit,
                RejectReason::OrderPoolExhausted
            }});
            return;
        }

        // Event ordering is part of the public specification: Ack first,
        // followed by the Trade events produced by matching.
        event_sink(Event{AckEvent{
            request.id,
            Operation::Submit
        }});

        match_and_rest(request, request.quantity, event_sink);
    }

    // Cancel a live resting order.
    template <typename EventSink>
    void cancel(OrderId id, EventSink&& event_sink) {
        auto index_it = order_index_.find(id);

        if (index_it == order_index_.end()) {
            event_sink(Event{RejectEvent{
                id,
                Operation::Cancel,
                RejectReason::UnknownId
            }});
            return;
        }

        // This helper removes the order from its FIFO chain, ID index,
        // price-level total, bitmap if the level empties, and order pool.
        remove_resting_order(index_it);

        event_sink(Event{CancelAckEvent{id}});
    }

    // Modify an existing resting order without changing its ID or side.
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

        // Copy the original before any operation releases its slot.
        const OrderPool::OrderSlot original =
            pool_[index_it->second.slot];

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

        // Case 1: quantity reduction at the same price preserves priority.
        // A quantity no-op is acknowledged without changing the book.
        if (new_price == original.price
            && new_quantity <= original.remaining_quantity) {
            if (new_quantity < original.remaining_quantity) {
                Level& level =
                    levels_for(original.side)[index_for(original.price)];

                // Subtract exactly the quantity removed from this order.
                level.total_quantity -=
                    original.remaining_quantity - new_quantity;

                // The slot and its prev/next links stay untouched, so FIFO
                // position and sequence number are preserved.
                pool_[index_it->second.slot].remaining_quantity =
                    new_quantity;
            }

            event_sink(Event{AckEvent{
                id,
                Operation::Modify
            }});
            return;
        }

        // Case 2: price changes or quantity increases, so priority is lost.
        const OrderRequest replacement =
            OrderRequest::limit_order(
                id,
                original.side,
                new_quantity,
                new_price
            );

        // Validate before removing the original order. If rejected, the
        // original order must remain completely unchanged.
        if (would_overflow_if_resting(replacement, id)) {
            event_sink(Event{RejectEvent{
                id,
                Operation::Modify,
                RejectReason::QuantityOverflow
            }});
            return;
        }

        // The old slot is released here. The replacement can use that free
        // slot if some of its quantity must rest after matching.
        remove_resting_order(index_it);

        event_sink(Event{AckEvent{
            id,
            Operation::Modify
        }});

        match_and_rest(replacement, new_quantity, event_sink);
    }

    // Build a canonical snapshot for comparisons with V0, V1 and V2.2.
    //
    // We enumerate live orders through the ID index, not through all 65,535
    // price slots. Sorting the copy hides the internal layout from callers.
    [[nodiscard]]
    std::vector<RestingOrderSnapshot> snapshot() const {
        std::vector<RestingOrderSnapshot> result;
        result.reserve(order_index_.size());

        for (const auto& [id, handle] : order_index_) {
            (void)id;

            const auto& order = pool_[handle.slot];

            result.push_back(RestingOrderSnapshot{
                order.id,
                order.side,
                order.price,
                order.remaining_quantity,
                order.sequence
            });
        }

        // Match the canonical order used by the other implementations:
        // bids descending, asks ascending, FIFO sequence within each level.
        std::sort(
            result.begin(),
            result.end(),
            [](const RestingOrderSnapshot& lhs,
               const RestingOrderSnapshot& rhs) {
                if (lhs.side != rhs.side) {
                    return lhs.side == Side::Buy;
                }

                if (lhs.price != rhs.price) {
                    return lhs.side == Side::Buy
                        ? lhs.price > rhs.price
                        : lhs.price < rhs.price;
                }

                return lhs.sequence < rhs.sequence;
            }
        );

        return result;
    }

    // Check cross-structure invariants without scanning every possible price
    // slot on every call. Tests use this; the timed matching path does not.
    [[nodiscard]]
    bool check_invariants() const {
        std::unordered_set<OrderId> seen_ids;
        std::unordered_set<Sequence> seen_sequences;
        std::unordered_set<Index> seen_slots;
        std::unordered_set<std::uint64_t> visited_levels;

        seen_ids.reserve(order_index_.size());
        seen_sequences.reserve(order_index_.size());
        seen_slots.reserve(order_index_.size());
        visited_levels.reserve(active_level_count_);

        std::size_t counted_orders = 0;
        std::optional<Price> calculated_best_bid;
        std::optional<Price> calculated_best_ask;

        const Quantity maximum =
            std::numeric_limits<Quantity>::max();

        // Start from each live ID entry and check that it leads to a valid
        // active pool slot and a valid occupied level.
        for (const auto& [id, handle] : order_index_) {
            if (handle.slot >= pool_.capacity()
                || !pool_.is_active(handle.slot)
                || !range_.contains(handle.price)) {
                return false;
            }

            const auto& order = pool_[handle.slot];

            if (order.id != id
                || order.side != handle.side
                || order.price != handle.price
                || order.remaining_quantity == 0) {
                return false;
            }

            const auto& levels = levels_for(handle.side);
            const std::size_t level_index = index_for(handle.price);
            const Level& level = levels[level_index];

            if (!level.active
                || level.head == no_slot
                || level.tail == no_slot
                || !level_bit_is_set(handle.side, level_index)) {
                return false;
            }

            // Compute the best prices independently of the cached values.
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

            // Encode side and array index together, so bids and asks at the
            // same numerical price count as distinct price levels.
            const std::uint64_t level_key =
                static_cast<std::uint64_t>(level_index) * 2U
                + (handle.side == Side::Buy ? 0U : 1U);

            // Several ID entries may point into one level; validate that
            // level's complete intrusive list only once.
            if (!visited_levels.insert(level_key).second) {
                continue;
            }

            Quantity calculated_total = 0;
            Sequence previous_sequence = 0;
            Index previous_slot = no_slot;
            Index cursor = level.head;

            // Walk the FIFO chain from oldest to newest.
            // The seen-slots set also detects a corrupt cyclic list.
            while (cursor != no_slot) {
                if (cursor >= pool_.capacity()
                    || !pool_.is_active(cursor)
                    || !seen_slots.insert(cursor).second) {
                    return false;
                }

                const auto& current = pool_[cursor];

                if (current.side != handle.side
                    || current.price != handle.price
                    || current.remaining_quantity == 0
                    || current.prev != previous_slot) {
                    return false;
                }

                if (current.sequence == 0
                    || current.sequence >= next_sequence_
                    || current.sequence <= previous_sequence) {
                    return false;
                }

                if (!seen_ids.insert(current.id).second
                    || !seen_sequences.insert(current.sequence).second) {
                    return false;
                }

                // Each linked order must point back from the ID index to
                // the exact pool slot, not just to the same ID or price.
                const auto reverse_it = order_index_.find(current.id);

                if (reverse_it == order_index_.end()
                    || reverse_it->second.slot != cursor
                    || reverse_it->second.side != current.side
                    || reverse_it->second.price != current.price) {
                    return false;
                }

                if (current.remaining_quantity > maximum - calculated_total) {
                    return false;
                }

                calculated_total += current.remaining_quantity;
                ++counted_orders;

                previous_sequence = current.sequence;
                previous_slot = cursor;
                cursor = current.next;
            }

            // The final visited node must be the stored tail, and the
            // independently calculated total must match the level aggregate.
            if (previous_slot != level.tail
                || calculated_total != level.total_quantity) {
                return false;
            }
        }

        // The linked queues, ID index and pool must agree on live-order count.
        if (counted_orders != order_index_.size()
            || counted_orders != resting_order_count_
            || pool_.in_use() != resting_order_count_
            || pool_.available() + pool_.in_use() != pool_.capacity()) {
            return false;
        }

        if (visited_levels.size() != active_level_count_) {
            return false;
        }

        // Every set bit represents an occupied level and vice versa.
        std::size_t bitmap_count = 0;

        for (const auto word : bid_bitmap_) {
            bitmap_count += static_cast<std::size_t>(std::popcount(word));
        }

        for (const auto word : ask_bitmap_) {
            bitmap_count += static_cast<std::size_t>(std::popcount(word));
        }

        if (bitmap_count != active_level_count_) {
            return false;
        }

        if (calculated_best_bid != best_bid_
            || calculated_best_ask != best_ask_) {
            return false;
        }

        // A completed operation must never leave crossed resting liquidity.
        if (best_bid_.has_value() && best_ask_.has_value()
            && *best_bid_ >= *best_ask_) {
            return false;
        }

        return true;
    }

private:
    // Validate the price range before allocating the level arrays.
    [[nodiscard]]
    static std::size_t checked_level_count(PriceRange range) {
        if (!range.valid()) {
            throw std::invalid_argument("Invalid price range");
        }

        return static_cast<std::size_t>(range.maximum)
            - static_cast<std::size_t>(range.minimum)
            + 1U;
    }

    // Round the level count up to the number of 64-bit bitmap words.
    [[nodiscard]]
    static std::size_t checked_bitmap_word_count(PriceRange range) {
        const std::size_t count = checked_level_count(range);

        return count / 64U + (count % 64U == 0U ? 0U : 1U);
    }

    PriceRange range_;

    // Fixed arrays: a price's index is calculated directly from its tick.
    std::vector<Level> bids_;
    std::vector<Level> asks_;

    // A set bitmap bit means the corresponding price level is occupied.
    std::vector<std::uint64_t> bid_bitmap_;
    std::vector<std::uint64_t> ask_bitmap_;

    // Resting order data and its intrusive prev/next links are stored here.
    OrderPool pool_;

    // The index maps IDs to pool slot indices.
    OrderIndex order_index_;

    Sequence next_sequence_{1};
    std::size_t resting_order_count_{0};
    std::size_t active_level_count_{0};

    std::optional<Price> best_bid_;
    std::optional<Price> best_ask_;

    [[nodiscard]]
    std::size_t index_for(Price price) const noexcept {
        return static_cast<std::size_t>(price)
            - static_cast<std::size_t>(range_.minimum);
    }

    [[nodiscard]]
    Price price_for_index(std::size_t index) const noexcept {
        return static_cast<Price>(
            static_cast<std::size_t>(range_.minimum) + index
        );
    }

    std::vector<Level>& levels_for(Side side) noexcept {
        return side == Side::Buy ? bids_ : asks_;
    }

    const std::vector<Level>& levels_for(Side side) const noexcept {
        return side == Side::Buy ? bids_ : asks_;
    }

    std::vector<std::uint64_t>& bitmap_for(Side side) noexcept {
        return side == Side::Buy ? bid_bitmap_ : ask_bitmap_;
    }

    const std::vector<std::uint64_t>& bitmap_for(
        Side side
    ) const noexcept {
        return side == Side::Buy ? bid_bitmap_ : ask_bitmap_;
    }

    // Set or clear one occupancy bit for an array index.
    void set_level_bit(
        Side side,
        std::size_t level_index,
        bool occupied
    ) noexcept {
        auto& bitmap = bitmap_for(side);

        const std::size_t word_index = level_index / 64U;
        const unsigned bit_offset =
            static_cast<unsigned>(level_index % 64U);

        const std::uint64_t mask =
            std::uint64_t{1} << bit_offset;

        if (occupied) {
            bitmap[word_index] |= mask;
        } else {
            bitmap[word_index] &= ~mask;
        }
    }

    [[nodiscard]]
    bool level_bit_is_set(
        Side side,
        std::size_t level_index
    ) const noexcept {
        const auto& bitmap = bitmap_for(side);

        const std::size_t word_index = level_index / 64U;
        const unsigned bit_offset =
            static_cast<unsigned>(level_index % 64U);

        const std::uint64_t mask =
            std::uint64_t{1} << bit_offset;

        return (bitmap[word_index] & mask) != 0U;
    }

    // Return the best eligible opposing price if one exists.
    [[nodiscard]]
    std::optional<Price> best_opposing_price(
        const OrderRequest& incoming
    ) const {
        if (incoming.side == Side::Buy) {
            if (!best_ask_.has_value()) {
                return std::nullopt;
            }

            if (incoming.limit_price.has_value()
                && *best_ask_ > *incoming.limit_price) {
                return std::nullopt;
            }

            return best_ask_;
        }

        if (!best_bid_.has_value()) {
            return std::nullopt;
        }

        if (incoming.limit_price.has_value()
            && *best_bid_ < *incoming.limit_price) {
            return std::nullopt;
        }

        return best_bid_;
    }

    // Search bitmap words for the next occupied price beyond current_price.
    [[nodiscard]]
    std::optional<Price> next_non_empty_price(
        Side side,
        Price current_price
    ) const {
        const std::size_t current_index = index_for(current_price);
        const std::size_t level_count = bids_.size();

        const auto& bitmap = bitmap_for(side);

        constexpr std::uint64_t all_bits =
            std::numeric_limits<std::uint64_t>::max();

        if (side == Side::Sell) {
            // Asks: scan upward, locating the lowest set bit in each word.
            const std::size_t start_index = current_index + 1U;

            if (start_index >= level_count) {
                return std::nullopt;
            }

            std::size_t word_index = start_index / 64U;
            const unsigned first_bit =
                static_cast<unsigned>(start_index % 64U);

            // Ignore any occupied levels below the starting price.
            std::uint64_t word =
                bitmap[word_index] & (all_bits << first_bit);

            while (true) {
                if (word != 0U) {
                    const std::size_t found_index =
                        word_index * 64U
                        + static_cast<std::size_t>(
                            std::countr_zero(word)
                        );

                    if (found_index < level_count) {
                        return price_for_index(found_index);
                    }
                }

                ++word_index;

                if (word_index >= bitmap.size()) {
                    break;
                }

                word = bitmap[word_index];
            }

            return std::nullopt;
        }

        // Bids: scan downward, locating the highest set bit in each word.
        if (current_index == 0U) {
            return std::nullopt;
        }

        const std::size_t start_index = current_index - 1U;
        std::size_t word_index = start_index / 64U;

        const unsigned last_bit =
            static_cast<unsigned>(start_index % 64U);

        const std::uint64_t mask = last_bit == 63U
            ? all_bits
            : ((std::uint64_t{1} << (last_bit + 1U)) - 1U);

        std::uint64_t word = bitmap[word_index] & mask;

        while (true) {
            if (word != 0U) {
                const unsigned highest_bit =
                    63U - static_cast<unsigned>(
                        std::countl_zero(word)
                    );

                const std::size_t found_index =
                    word_index * 64U + highest_bit;

                if (found_index < level_count) {
                    return price_for_index(found_index);
                }
            }

            if (word_index == 0U) {
                break;
            }

            --word_index;
            word = bitmap[word_index];
        }

        return std::nullopt;
    }

    // When the best level empties, find the next occupied price via the bitmap.
    void refresh_best_after_empty(
        Side side,
        Price emptied_price
    ) {
        if (side == Side::Buy) {
            if (best_bid_.has_value()
                && *best_bid_ == emptied_price) {
                best_bid_ =
                    next_non_empty_price(Side::Buy, emptied_price);
            }
            return;
        }

        if (best_ask_.has_value()
            && *best_ask_ == emptied_price) {
            best_ask_ =
                next_non_empty_price(Side::Sell, emptied_price);
        }
    }

    // Unlink an order from its intrusive FIFO list.
    //
    // The ID-index iterator becomes invalid when its entry is erased.
    // The pool slot is released only after its links are no longer needed.
    void remove_resting_order(OrderIndex::iterator index_it) {
        const OrderHandle handle = index_it->second;
        const Index slot_index = handle.slot;

        auto& levels = levels_for(handle.side);
        Level& level = levels[index_for(handle.price)];

        auto& slot = pool_[slot_index];

        // Update the previous order, or the level head if this was first.
        if (slot.prev == no_slot) {
            level.head = slot.next;
        } else {
            pool_[slot.prev].next = slot.next;
        }

        // Update the next order, or the level tail if this was last.
        if (slot.next == no_slot) {
            level.tail = slot.prev;
        } else {
            pool_[slot.next].prev = slot.prev;
        }

        level.total_quantity -= slot.remaining_quantity;

        // The ID is no longer live after the node is removed.
        order_index_.erase(index_it);
        --resting_order_count_;

        if (level.head == no_slot) {
            // An empty chain must have both endpoints unset.
            level.tail = no_slot;
            level.active = false;
            --active_level_count_;

            set_level_bit(handle.side, index_for(handle.price), false);
            refresh_best_after_empty(handle.side, handle.price);
        }

        // The slot is now detached from the level and can be reused.
        // Treat failure as a logic error rather than silently corrupting
        // the free list and live-order count.
        if (!pool_.release(slot_index)) {
            throw std::logic_error("Failed to release a live order slot");
        }
    }

    // Allocate a pool slot and append it to the FIFO queue at this price.
    void add_resting_order(
        OrderId id,
        Side side,
        Price price,
        Quantity quantity
    ) {
        const Index slot_index = pool_.acquire();

        // submit() checks capacity before matching. A successful match that
        // leaves a remainder frees at least one maker slot; a modify frees
        // its original slot. Reaching this case indicates a bookkeeping bug.
        if (slot_index == no_slot) {
            throw std::logic_error(
                "Order pool exhausted after capacity validation"
            );
        }

        auto& levels = levels_for(side);
        const std::size_t level_index = index_for(price);
        Level& level = levels[level_index];

        const bool was_empty = !level.active;

        // Initialise the acquired slot's order data and queue links.
        auto& slot = pool_[slot_index];
        slot.id = id;
        slot.side = side;
        slot.price = price;
        slot.remaining_quantity = quantity;
        slot.sequence = next_sequence_;
        slot.prev = level.tail;
        slot.next = no_slot;

        // Append to the tail, preserving FIFO priority.
        if (level.tail == no_slot) {
            level.head = slot_index;
        } else {
            pool_[level.tail].next = slot_index;
        }
        level.tail = slot_index;

        level.total_quantity += quantity;

        if (was_empty) {
            level.active = true;
            ++active_level_count_;

            // Mark this price as occupied for bitmap searches.
            set_level_bit(side, level_index, true);
        }

        // Update cached best price when this new level improves the best.
        if (side == Side::Buy) {
            if (!best_bid_.has_value() || price > *best_bid_) {
                best_bid_ = price;
            }
        } else {
            if (!best_ask_.has_value() || price < *best_ask_) {
                best_ask_ = price;
            }
        }

        const auto [index_it, inserted] = order_index_.emplace(
            id,
            OrderHandle{side, price, slot_index}
        );
        (void)index_it;

        if (!inserted) {
            throw std::logic_error(
                "Duplicate ID reached add_resting_order"
            );
        }

        ++resting_order_count_;
        ++next_sequence_;
    }

    // Match an accepted request, then rest any eligible limit remainder.
    template <typename EventSink>
    void match_and_rest(
        const OrderRequest& incoming,
        Quantity remaining,
        EventSink& event_sink
    ) {
        while (remaining > 0U) {
            const auto maker_price = best_opposing_price(incoming);

            if (!maker_price.has_value()) {
                break;
            }

            const Side maker_side = incoming.side == Side::Buy
                ? Side::Sell
                : Side::Buy;

            const std::size_t level_index = index_for(*maker_price);
            Level& level = levels_for(maker_side)[level_index];

            // FIFO: the head slot is the oldest resting order at this price.
            const Index maker_index = level.head;
            auto& maker_slot = pool_[maker_index];

            // Copy the maker details before releasing its slot if filled.
            const OrderPool::OrderSlot maker = maker_slot;

            const Quantity executed = std::min(
                remaining,
                maker_slot.remaining_quantity
            );

            remaining -= executed;
            maker_slot.remaining_quantity -= executed;
            level.total_quantity -= executed;

            if (maker_slot.remaining_quantity == 0U) {
                // Remove the now-filled maker from the head of the queue.
                const Index next_index = maker_slot.next;
                level.head = next_index;

                if (next_index == no_slot) {
                    level.tail = no_slot;
                } else {
                    pool_[next_index].prev = no_slot;
                }

                order_index_.erase(maker.id);
                --resting_order_count_;

                if (level.head == no_slot) {
                    level.active = false;
                    --active_level_count_;

                    set_level_bit(maker_side, level_index, false);
                    refresh_best_after_empty(maker_side, maker.price);
                }

                // The maker no longer exists in the book, so its pool slot
                // can be returned to the free list.
                if (!pool_.release(maker_index)) {
                    throw std::logic_error(
                        "Failed to release a filled order slot"
                    );
                }
            }

            // The resting maker's price is the execution price.
            event_sink(Event{TradeEvent{
                maker.id,
                incoming.id,
                maker.price,
                executed
            }});
        }

        // A limit remainder rests; a market remainder expires.
        if (remaining > 0U && incoming.limit_price.has_value()) {
            add_resting_order(
                incoming.id,
                incoming.side,
                *incoming.limit_price,
                remaining
            );
        }
    }

    // Check whether the incoming limit remainder would overflow its target
    // level, simulating matches without mutating any live orders.
    [[nodiscard]]
    bool would_overflow_if_resting(
        const OrderRequest& request,
        std::optional<OrderId> excluded_id = std::nullopt
    ) const {
        if (!request.limit_price.has_value()) {
            return false;
        }

        Quantity remaining = request.quantity;

        const Side maker_side = request.side == Side::Buy
            ? Side::Sell
            : Side::Buy;

        auto price = best_opposing_price(request);

        while (price.has_value() && remaining > 0U) {
            const Price maker_price = *price;

            // Do not simulate trades beyond the incoming limit price.
            // best_opposing_price() checks the first level, but subsequent bitmap
            // searches may find levels outside the limit and must stop here.
            if (request.side == Side::Buy
                && maker_price > *request.limit_price) {
                break;
            }

            if (request.side == Side::Sell
                && maker_price < *request.limit_price) {
                break;
            }

            const Level& maker_level =
                levels_for(maker_side)[index_for(maker_price)];

            // This level can fill the remaining incoming quantity completely.
            if (maker_level.total_quantity >= remaining) {
                return false;
            }

            remaining -= maker_level.total_quantity;

            // Search the next occupied price in matching priority order.
            price = next_non_empty_price(maker_side, maker_price);
        }

        if (remaining == 0U) {
            return false;
        }

        const Price destination_price = *request.limit_price;
        const Level& destination_level =
            levels_for(request.side)[index_for(destination_price)];

        Quantity existing_quantity = destination_level.total_quantity;

        // For a cancel-and-replace modification, do not count the old order's
        // quantity if the replacement returns to the same price and side.
        if (excluded_id.has_value()) {
            const auto old_it = order_index_.find(*excluded_id);

            if (old_it != order_index_.end()
                && old_it->second.side == request.side
                && old_it->second.price == destination_price) {
                const Quantity old_quantity =
                    pool_[old_it->second.slot].remaining_quantity;

                if (old_quantity > existing_quantity) {
                    return true;
                }

                existing_quantity -= old_quantity;
            }
        }

        const Quantity maximum =
            std::numeric_limits<Quantity>::max();

        return remaining > maximum - existing_quantity;
    }
};

}  // namespace lob

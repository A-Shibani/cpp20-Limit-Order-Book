
#pragma once

#include "lob/types.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace lob {

// A fixed-capacity pool for resting orders.
//
// Instead of allocating a separate linked-list node every time an order
// rests, we allocate all OrderSlot objects up front. Each live order gets
// one slot, identified by a small integer index.
//
// The pool has two different kinds of links:
//
//   prev / next:
//       Connect live order slots into the FIFO queue at a price level.
//
//   free_next:
//       Connect unused slots into the pool's free list.
//
// Keeping these links separate is important: a slot belongs either to
// a live price-level queue or to the free list, never both.
//
// This first version deliberately does not use generation counters.
// We will add that as a separate, measured safety step later.
class OrderPool {
public:
    // Slot indices are smaller and cheaper to store than pointers.
    using Index = std::uint32_t;

    // A special index meaning "there is no slot here".
    // This value is reserved and never used for a valid slot index.
    static constexpr Index no_slot =
        std::numeric_limits<Index>::max();

    // All information for one possible order lives in one pool slot.
    struct OrderSlot {
        OrderId id{};
        Side side{Side::Buy};
        Price price{};
        Quantity remaining_quantity{};
        Sequence sequence{};

        // These two links form the intrusive FIFO list while the slot is live.
        Index prev{no_slot};
        Index next{no_slot};

        // This link is used only while the slot is free.
        Index free_next{no_slot};

        // active tells us whether this slot currently holds a live order.
        bool active{false};
    };

    // Allocate the pool once, with the chosen maximum number of live orders.
    //
    // The engine can then reuse these slots without allocating a new order
    // node for each submit/cancel cycle. The vector itself never grows.
    explicit OrderPool(std::size_t capacity)
        : slots_(checked_capacity(capacity)),
          free_head_(capacity == 0U ? no_slot : Index{0}),
          free_count_(capacity) {
        // Build the initial free list: 0 -> 1 -> 2 -> ... -> no_slot.
        //
        // Because the slots vector is already allocated, these links can
        // be established once in the constructor.
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            if (i + 1U < slots_.size()) {
                slots_[i].free_next = static_cast<Index>(i + 1U);
            } else {
                // The final free slot marks the end of the free list.
                slots_[i].free_next = no_slot;
            }
        }
    }

    // Take one unused slot from the front of the free list.
    //
    // Returns no_slot if the pool is full. Returning a sentinel rather than
    // throwing lets the engine turn pool exhaustion into a normal rejection.
    [[nodiscard]]
    Index acquire() noexcept {
        if (free_head_ == no_slot) {
            return no_slot;
        }

        // Save the slot we are taking, then advance the free-list head.
        const Index acquired_index = free_head_;
        OrderSlot& acquired_slot = slots_[acquired_index];

        free_head_ = acquired_slot.free_next;
        --free_count_;

        // Clear any values left by the slot's previous owner.
        // The slot is no longer linked into the free list.
        acquired_slot = OrderSlot{};
        acquired_slot.active = true;

        return acquired_index;
    }

    // Return a live slot to the free list.
    //
    // Returns false for an invalid index or a slot that is already free.
    // This check makes accidental double release detectable in tests.
    [[nodiscard]]
    bool release(Index index) noexcept {
        if (index >= slots_.size()) {
            return false;
        }

        OrderSlot& released_slot = slots_[index];

        if (!released_slot.active) {
            return false;
        }

        // Clear the order data and unlink it from any FIFO list.
        // The caller must unlink it from its price level BEFORE releasing it.
        released_slot = OrderSlot{};
        released_slot.active = false;

        // Push the slot onto the front of the free list.
        released_slot.free_next = free_head_;
        free_head_ = index;
        ++free_count_;

        return true;
    }

    // Access a slot by its index. Callers must use only valid indices.
    //
    // We expose the slot so the matching engine can update its order fields
    // and intrusive prev/next links without allocating a separate node.
    OrderSlot& operator[](Index index) noexcept {
        return slots_[index];
    }

    // Read-only slot access for diagnostics and queries.
    const OrderSlot& operator[](Index index) const noexcept {
        return slots_[index];
    }

    // Total fixed capacity; this never increases after construction.
    [[nodiscard]]
    std::size_t capacity() const noexcept {
        return slots_.size();
    }

    // Number of slots currently on the free list.
    [[nodiscard]]
    std::size_t available() const noexcept {
        return free_count_;
    }

    // Number of slots currently assigned to orders.
    [[nodiscard]]
    std::size_t in_use() const noexcept {
        return slots_.size() - free_count_;
    }

    // Check whether an index represents a live slot.
    [[nodiscard]]
    bool is_active(Index index) const noexcept {
        return index < slots_.size() && slots_[index].active;
    }

private:
    // The sentinel must remain outside the range of valid slot indices.
    // This also avoids silently narrowing a large capacity to Index.
    [[nodiscard]]
    static std::size_t checked_capacity(std::size_t capacity) {
        if (capacity > static_cast<std::size_t>(no_slot)) {
            throw std::length_error(
                "OrderPool capacity exceeds its index type"
            );
        }

        return capacity;
    }

    // All slot storage is allocated in this vector at construction.
    // We never push_back new slots as orders arrive.
    std::vector<OrderSlot> slots_;

    // The index of the first unused slot, or no_slot if the pool is full.
    Index free_head_{no_slot};

    // Keeping this count lets the engine check pool availability in O(1).
    std::size_t free_count_{0};
};

}  // namespace lob

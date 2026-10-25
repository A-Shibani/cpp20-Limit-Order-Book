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
// V2.5 adds a generation counter to each slot. A handle now identifies both
// the slot index and the particular lifetime of the order stored there. If a
// slot is released and later reused, the new order gets a different generation,
// so an old handle cannot accidentally refer to the new order.
class OrderPool {
public:
    // Slot indices are smaller and cheaper to store than pointers.
    using Index = std::uint32_t;

    // Each successful use of a slot gets a new generation number.
    // uint64_t makes exhaustion extraordinarily unlikely; if a slot reaches
    // the maximum generation, it is retired rather than allowed to wrap around.
    using Generation = std::uint64_t;

    // A special index meaning "there is no slot here".
    // This value is reserved and never used for a valid slot index.
    static constexpr Index no_slot =
        std::numeric_limits<Index>::max();

    // A safe reference to one specific lifetime of a pool slot.
    // The index alone is not sufficient because the pool reuses slot indices.
    struct Handle {
        Index index{no_slot};
        Generation generation{0};
    };

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

        // Generation survives release/reset and advances on each acquisition.
        // It is metadata for the slot's lifetime, not an order's business data.
        Generation generation{0};

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
    // Existing engine code can keep using the index-returning API; code that
    // needs stale-handle protection can immediately call handle_for(index),
    // or use acquire_handle() below.
    [[nodiscard]]
    Index acquire() noexcept {
        while (free_head_ != no_slot) {
            // Remove the first free slot from the linked free list.
            const Index acquired_index = free_head_;
            OrderSlot& acquired_slot = slots_[acquired_index];
            free_head_ = acquired_slot.free_next;
            --free_count_;

            // A maximum-generation slot should already have been retired by
            // release(). Keep this defensive check so a corrupted/free-list
            // edge case still cannot cause generation wraparound (the ABA bug).
            if (acquired_slot.generation
                == std::numeric_limits<Generation>::max()) {
                acquired_slot = OrderSlot{};
                acquired_slot.generation =
                    std::numeric_limits<Generation>::max();
                ++retired_count_;
                continue;
            }

            // Save the new lifetime number before clearing fields from the
            // previous order. Default-resetting the whole slot would otherwise
            // reset generation and allow an old handle to become valid again.
            const Generation next_generation = acquired_slot.generation + 1U;
            acquired_slot = OrderSlot{};
            acquired_slot.generation = next_generation;
            acquired_slot.active = true;

            ++in_use_count_;
            return acquired_index;
        }

        return no_slot;
    }

    // Acquire a slot and return its generation-aware handle directly.
    // An invalid handle (index == no_slot) indicates that the pool is full.
    [[nodiscard]]
    Handle acquire_handle() noexcept {
        const Index index = acquire();
        if (index == no_slot) {
            return Handle{};
        }

        return Handle{index, slots_[index].generation};
    }

    // Build a handle for a currently active slot. If index is invalid or the
    // slot is free, return an invalid handle. This is useful when older code
    // acquired an index but now needs a safe handle for a map entry.
    [[nodiscard]]
    Handle handle_for(Index index) const noexcept {
        if (!is_active(index)) {
            return Handle{};
        }

        return Handle{index, slots_[index].generation};
    }

    // A handle is valid only while the slot is active and its generation still
    // matches. Both checks matter: release invalidates it immediately, and a
    // later reuse produces a different generation.
    [[nodiscard]]
    bool is_valid(Handle handle) const noexcept {
        return handle.index < slots_.size()
            && slots_[handle.index].active
            && handle.generation != 0U
            && slots_[handle.index].generation == handle.generation;
    }

    // Return a live slot to the free list using its raw index.
    //
    // Returns false for an invalid index or a slot that is already free.
    // This overload remains for existing engine code; generation-aware callers
    // should prefer release(Handle) below.
    [[nodiscard]]
    bool release(Index index) noexcept {
        if (index >= slots_.size()) {
            return false;
        }

        OrderSlot& released_slot = slots_[index];
        if (!released_slot.active) {
            return false;
        }

        // Save generation across the reset. A slot's generation must never
        // move backwards when the order fields are cleared.
        const Generation current_generation = released_slot.generation;
        released_slot = OrderSlot{};
        released_slot.generation = current_generation;
        released_slot.active = false;
        --in_use_count_;

        // Do not let a generation wrap from max back to zero. Such a slot is
        // permanently retired; its old handles can never match a future order.
        if (current_generation == std::numeric_limits<Generation>::max()) {
            ++retired_count_;
            return true;
        }

        // Push the slot onto the front of the free list for reuse.
        released_slot.free_next = free_head_;
        free_head_ = index;
        ++free_count_;

        return true;
    }

    // Release only if the handle still names the current lifetime of the slot.
    // This safely rejects stale handles and accidental double releases.
    [[nodiscard]]
    bool release(Handle handle) noexcept {
        if (!is_valid(handle)) {
            return false;
        }

        return release(handle.index);
    }

    // Access a slot by its index. Callers must use only valid indices.
    //
    // We expose the slot so the matching engine can update its order fields
    // and intrusive prev/next links without allocating a separate node.
    // The caller should first validate any ID-map handle before dereferencing.
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

    // Number of slots currently assigned to live orders.
    [[nodiscard]]
    std::size_t in_use() const noexcept {
        return in_use_count_;
    }

    // Number of slots retired to prevent generation-counter wraparound.
    // This normally remains zero for the entire lifetime of a program.
    [[nodiscard]]
    std::size_t retired() const noexcept {
        return retired_count_;
    }

    // Check whether an index represents a live slot.
    [[nodiscard]]
    bool is_active(Index index) const noexcept {
        return index < slots_.size() && slots_[index].active;
    }

    // Return the generation for an active slot, or zero for an invalid/free slot.
    [[nodiscard]]
    Generation generation(Index index) const noexcept {
        return is_active(index) ? slots_[index].generation : Generation{0};
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

    // The index of the first unused slot, or no_slot if none are available.
    Index free_head_{no_slot};

    // These counters keep allocation and invariant checks O(1). Retired slots
    // are neither active nor free, so all three counts sum to capacity().
    std::size_t free_count_{0};
    std::size_t in_use_count_{0};
    std::size_t retired_count_{0};
};

}  // namespace lob

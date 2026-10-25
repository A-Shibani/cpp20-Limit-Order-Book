#pragma once

#include "lob/types.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace lob {

// A fixed-capacity hash map from OrderId to Value.
//
// Why implement this ourselves?
// - The table allocates its storage once, in the constructor.
// - Insertions and deletions do not resize or rehash the table.
// - Collisions are handled by linear probing.
// - Tombstones allow entries to be deleted without breaking lookups.
//
// The template lets us store the same OrderHandle type that BookPool
// currently stores in its std::unordered_map.
template <typename Value>
class OrderIdMap {
public:
    // max_entries is the maximum number of live entries, not the number
    // of buckets. We allocate at least twice as many buckets as entries
    // to keep the table from becoming too crowded.
    explicit OrderIdMap(std::size_t max_entries)
        : max_entries_(max_entries),
          slots_(bucket_count_for(max_entries)) {}

    // Insert a new key/value pair.
    //
    // Returns true if insertion succeeds.
    // Returns false if the key already exists or the live-entry limit
    // has been reached.
    //
    // V is a forwarding reference so callers can pass either an existing
    // Value or a temporary Value without an unnecessary extra copy.
    template <typename V>
    bool insert(OrderId key, V&& value) {
        // The live-entry limit is a hard capacity limit. This map never
        // grows, even if more orders are submitted later.
        if (size_ >= max_entries_) {
            return false;
        }

        const std::size_t mask = slots_.size() - 1;
        std::size_t index = bucket_index(key);

        // Remember the first deleted slot we encounter.
        //
        // We cannot immediately insert into a tombstone: the same key
        // might already exist later in the probe sequence.
        std::size_t first_tombstone = npos;

        // A power-of-two bucket count lets us wrap around the table
        // using a bit mask instead of a modulo operation.
        for (std::size_t probes = 0; probes < slots_.size(); ++probes) {
            Slot& slot = slots_[index];

            if (slot.state == State::Occupied) {
                // A duplicate key must not create a second entry.
                if (slot.key == key) {
                    return false;
                }
            } else if (slot.state == State::Tombstone) {
                // Reuse the earliest tombstone if we eventually find
                // an empty bucket.
                if (first_tombstone == npos) {
                    first_tombstone = index;
                }
            } else {
                // An Empty bucket means this key is not present later
                // in the probe sequence, so insertion can finish.
                const std::size_t target =
                    (first_tombstone != npos) ? first_tombstone : index;

                Slot& destination = slots_[target];
                destination.key = key;
                destination.value.emplace(std::forward<V>(value));
                destination.state = State::Occupied;

                ++size_;
                return true;
            }

            // Linear probing: try the next bucket, wrapping at the end.
            index = (index + 1) & mask;
        }

        // Defensive fallback: if every bucket was visited and we found
        // a tombstone, it is still a valid place to insert.
        //
        // With our allocation policy, this should not normally be needed,
        // but it makes the probing logic robust.
        if (first_tombstone != npos) {
            Slot& destination = slots_[first_tombstone];
            destination.key = key;
            destination.value.emplace(std::forward<V>(value));
            destination.state = State::Occupied;

            ++size_;
            return true;
        }

        return false;
    }

    // Find a key and return a pointer to its stored value.
    //
    // A null pointer means that the key does not exist.
    // Returning a pointer avoids copying Value, and lets BookPool
    // inspect or update the stored OrderHandle directly.
    Value* find(OrderId key) {
        const std::size_t index = find_index(key);

        if (index == npos) {
            return nullptr;
        }

        return &*slots_[index].value;
    }

    // Read-only version of find(), for const OrderIdMap objects.
    const Value* find(OrderId key) const {
        const std::size_t index = find_index(key);

        if (index == npos) {
            return nullptr;
        }

        return &*slots_[index].value;
    }

    // Remove a key if it exists.
    //
    // Important: the slot becomes a Tombstone, NOT Empty.
    // Making it Empty could cause a later lookup to stop too early and
    // incorrectly report that a key further along the probe sequence
    // does not exist.
    bool erase(OrderId key) {
        const std::size_t index = find_index(key);

        if (index == npos) {
            return false;
        }

        Slot& slot = slots_[index];

        // Destroy the stored value and mark the bucket as deleted.
        slot.value.reset();
        slot.state = State::Tombstone;

        --size_;
        return true;
    }

    // Remove every entry and reset all tombstones.
    void clear() {
        for (Slot& slot : slots_) {
            slot.value.reset();
            slot.key = OrderId{};
            slot.state = State::Empty;
        }

        size_ = 0;
    }

    // Number of currently stored entries (tombstones do not count).
    [[nodiscard]] std::size_t size() const {
        return size_;
    }

    [[nodiscard]] bool empty() const {
        return size_ == 0;
    }

    // Maximum number of live entries this map can hold.
    [[nodiscard]] std::size_t max_size() const {
        return max_entries_;
    }

    // Physical number of hash-table buckets.
    // Exposed mainly for tests and benchmarks.
    [[nodiscard]] std::size_t bucket_count() const {
        return slots_.size();
    }

    // Visit every live key/value pair.
    //
    // This is useful for invariant checks in BookPool. The callback is
    // called only for Occupied slots, never for empty buckets or tombstones.
    template <typename Function>
    void for_each(Function&& function) {
        for (Slot& slot : slots_) {
            if (slot.state == State::Occupied) {
                function(slot.key, *slot.value);
            }
        }
    }

    // Read-only version, for invariant checks on const books.
    template <typename Function>
    void for_each(Function&& function) const {
        for (const Slot& slot : slots_) {
            if (slot.state == State::Occupied) {
                function(slot.key, *slot.value);
            }
        }
    }

private:
    // These are the three possible states of a bucket:
    //
    // Empty     - never occupied since the last clear; lookup can stop here.
    // Occupied  - contains a live key/value pair.
    // Tombstone - was deleted; lookup must continue past it.
    enum class State : std::uint8_t {
        Empty,
        Occupied,
        Tombstone
    };

    struct Slot {
        OrderId key{};
        std::optional<Value> value{};
        State state{State::Empty};
    };

    // Sentinel used to mean "no valid bucket index".
    static constexpr std::size_t npos =
        std::numeric_limits<std::size_t>::max();

    std::size_t max_entries_{0};
    std::size_t size_{0};
    std::vector<Slot> slots_;

    // Return the bucket count needed for max_entries.
    //
    // The bucket count is always a power of two, which makes the
    // wraparound calculation in insert() and find_index() inexpensive.
    static std::size_t bucket_count_for(std::size_t max_entries) {
        const std::size_t maximum =
            std::numeric_limits<std::size_t>::max();

        // Multiplying by two must not overflow size_t.
        if (max_entries > maximum / 2) {
            throw std::length_error("OrderIdMap capacity is too large");
        }

        // Small tables still get at least eight buckets.
        const std::size_t wanted =
            (max_entries < 4) ? 8 : max_entries * 2;

        std::size_t bucket_count = 1;

        // Round up to the next power of two.
        while (bucket_count < wanted) {
            if (bucket_count > maximum / 2) {
                throw std::length_error("OrderIdMap bucket count is too large");
            }

            bucket_count *= 2;
        }

        return bucket_count;
    }

    // Mix the bits of the ID before selecting a bucket.
    //
    // Order IDs may have patterns, so using the raw ID directly could
    // cluster keys into a small part of the table. This integer-mixing
    // function spreads similar input values more evenly.
    [[nodiscard]] std::size_t bucket_index(OrderId key) const {
        std::uint64_t mixed = static_cast<std::uint64_t>(key);

        mixed ^= mixed >> 30;
        mixed *= 0xbf58476d1ce4e5b9ULL;
        mixed ^= mixed >> 27;
        mixed *= 0x94d049bb133111ebULL;
        mixed ^= mixed >> 31;

        // slots_.size() is a power of two, so size - 1 is a bit mask.
        return static_cast<std::size_t>(mixed) & (slots_.size() - 1);
    }

    // Find the bucket for key, or return npos when it is absent.
    [[nodiscard]] std::size_t find_index(OrderId key) const {
        const std::size_t mask = slots_.size() - 1;
        std::size_t index = bucket_index(key);

        // Check at most every bucket. This bound also prevents an
        // infinite loop if the table has experienced many deletions.
        for (std::size_t probes = 0; probes < slots_.size(); ++probes) {
            const Slot& slot = slots_[index];

            if (slot.state == State::Empty) {
                // An Empty bucket ends the probe sequence: a matching
                // key cannot be located beyond it.
                return npos;
            }

            if (slot.state == State::Occupied && slot.key == key) {
                return index;
            }

            // Tombstones do not terminate lookup. Keep probing.
            index = (index + 1) & mask;
        }

        return npos;
    }
};

}  // namespace lob
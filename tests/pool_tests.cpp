
#include "lob/pool.hpp"

#include <cstdint>

#include <gtest/gtest.h>

namespace lob {
namespace {

TEST(OrderPoolTests, StartsWithAllSlotsAvailable) {
    OrderPool pool(4);

    EXPECT_EQ(pool.capacity(), 4U);
    EXPECT_EQ(pool.available(), 4U);
    EXPECT_EQ(pool.in_use(), 0U);
}

TEST(OrderPoolTests, AcquiresDistinctSlotsUntilExhausted) {
    OrderPool pool(3);

    const auto first = pool.acquire();
    const auto second = pool.acquire();
    const auto third = pool.acquire();

    // The first three acquisitions must all succeed and return unique slots.
    ASSERT_NE(first, OrderPool::no_slot);
    ASSERT_NE(second, OrderPool::no_slot);
    ASSERT_NE(third, OrderPool::no_slot);

    EXPECT_NE(first, second);
    EXPECT_NE(first, third);
    EXPECT_NE(second, third);

    // A fourth acquisition fails normally rather than allocating more memory.
    EXPECT_EQ(pool.acquire(), OrderPool::no_slot);

    EXPECT_EQ(pool.available(), 0U);
    EXPECT_EQ(pool.in_use(), 3U);
}

TEST(OrderPoolTests, ReleasedSlotCanBeReused) {
    OrderPool pool(2);

    const auto first = pool.acquire();
    const auto second = pool.acquire();

    ASSERT_NE(first, OrderPool::no_slot);
    ASSERT_NE(second, OrderPool::no_slot);

    // Fill some fields to model a live order occupying this slot.
    pool[first].id = 42;
    pool[first].side = Side::Buy;
    pool[first].price = 100;
    pool[first].remaining_quantity = 5;
    pool[first].active = true;

    ASSERT_TRUE(pool.release(first));

    EXPECT_EQ(pool.available(), 1U);
    EXPECT_EQ(pool.in_use(), 1U);
    EXPECT_FALSE(pool.is_active(first));

    // release() pushes onto the free-list head, so the next acquisition
    // should immediately reuse the slot we just released.
    const auto reused = pool.acquire();

    EXPECT_EQ(reused, first);
    EXPECT_TRUE(pool.is_active(reused));

    // Reinitialising a slot prevents the new owner from seeing old order data.
    EXPECT_EQ(pool[reused].id, 0U);
    EXPECT_EQ(pool[reused].remaining_quantity, 0U);
    EXPECT_EQ(pool[reused].prev, OrderPool::no_slot);
    EXPECT_EQ(pool[reused].next, OrderPool::no_slot);

    EXPECT_EQ(pool.available(), 0U);
    EXPECT_EQ(pool.in_use(), 2U);
}

TEST(OrderPoolTests, RejectsDoubleRelease) {
    OrderPool pool(1);

    const auto slot = pool.acquire();
    ASSERT_NE(slot, OrderPool::no_slot);

    EXPECT_TRUE(pool.release(slot));

    // A second release of the same inactive slot must not corrupt
    // the free list or increase the available count past the capacity.
    EXPECT_FALSE(pool.release(slot));
    EXPECT_EQ(pool.available(), 1U);
}

TEST(OrderPoolTests, IntrusiveLinksCanConnectLiveSlots) {
    OrderPool pool(3);

    const auto first = pool.acquire();
    const auto second = pool.acquire();

    ASSERT_NE(first, OrderPool::no_slot);
    ASSERT_NE(second, OrderPool::no_slot);

    // Model two adjacent orders in one price level's FIFO list:
    //
    // first <--> second
    //
    // These links live inside each slot; no separate list node is allocated.
    pool[first].next = second;
    pool[second].prev = first;

    EXPECT_EQ(pool[first].next, second);
    EXPECT_EQ(pool[second].prev, first);

    // The free-list count remains unchanged by queue-link operations.
    EXPECT_EQ(pool.available(), 1U);
    EXPECT_EQ(pool.in_use(), 2U);
}

TEST(OrderPoolTests, ZeroCapacityPoolFailsGracefully) {
    OrderPool pool(0);

    EXPECT_EQ(pool.capacity(), 0U);
    EXPECT_EQ(pool.available(), 0U);
    EXPECT_EQ(pool.acquire(), OrderPool::no_slot);
}

// -----------------------------------------------------------------------------
// V2.5: Generation-aware handle tests
// -----------------------------------------------------------------------------
//
// A pool slot can be reused many times. A generation-aware handle identifies
// one particular use of that slot, not just the slot's numerical index.
//
// These tests verify that a handle from an old order cannot accidentally be
// treated as a handle to a newer order that reuses the same slot.

TEST(OrderPoolGenerationTests, AcquireHandleReturnsValidGeneration) {
    // A capacity of one makes it easy to observe exactly which slot is reused.
    lob::OrderPool pool(1);

    const lob::OrderPool::Handle handle = pool.acquire_handle();

    // A successful handle has a real slot index and nonzero generation.
    ASSERT_NE(handle.index, lob::OrderPool::no_slot);
    EXPECT_NE(handle.generation, 0U);

    EXPECT_TRUE(pool.is_valid(handle));
    EXPECT_TRUE(pool.is_active(handle.index));
    EXPECT_EQ(pool.generation(handle.index), handle.generation);

    EXPECT_EQ(pool.available(), 0U);
    EXPECT_EQ(pool.in_use(), 1U);
}

TEST(OrderPoolGenerationTests, ReleaseHandleInvalidatesIt) {
    lob::OrderPool pool(1);

    const auto handle = pool.acquire_handle();
    ASSERT_TRUE(pool.is_valid(handle));

    // Releasing the handle returns its slot to the free list.
    EXPECT_TRUE(pool.release(handle));

    // A handle must become invalid as soon as its order is released.
    EXPECT_FALSE(pool.is_valid(handle));
    EXPECT_FALSE(pool.is_active(handle.index));

    EXPECT_EQ(pool.available(), 1U);
    EXPECT_EQ(pool.in_use(), 0U);

    // Releasing the same handle a second time must fail.
    EXPECT_FALSE(pool.release(handle));
}

TEST(OrderPoolGenerationTests, ReuseIncrementsGeneration) {
    lob::OrderPool pool(1);

    const auto first_handle = pool.acquire_handle();
    ASSERT_TRUE(pool.is_valid(first_handle));

    ASSERT_TRUE(pool.release(first_handle));

    // The capacity-one pool must reuse the same numerical slot.
    const auto second_handle = pool.acquire_handle();

    ASSERT_NE(second_handle.index, lob::OrderPool::no_slot);
    EXPECT_EQ(second_handle.index, first_handle.index);

    // The slot is the same, but this is a different lifetime.
    EXPECT_GT(second_handle.generation, first_handle.generation);

    EXPECT_FALSE(pool.is_valid(first_handle));
    EXPECT_TRUE(pool.is_valid(second_handle));
}

TEST(OrderPoolGenerationTests, StaleHandleCannotReleaseReusedSlot) {
    lob::OrderPool pool(1);

    const auto old_handle = pool.acquire_handle();
    ASSERT_TRUE(pool.release(old_handle));

    const auto current_handle = pool.acquire_handle();
    ASSERT_TRUE(pool.is_valid(current_handle));

    ASSERT_EQ(current_handle.index, old_handle.index);

    // The old handle refers to the previous lifetime of this slot.
    // It must not release the current order.
    EXPECT_FALSE(pool.release(old_handle));

    EXPECT_TRUE(pool.is_valid(current_handle));
    EXPECT_TRUE(pool.is_active(current_handle.index));
    EXPECT_EQ(pool.in_use(), 1U);
    EXPECT_EQ(pool.available(), 0U);

    // The current handle is still allowed to release its own slot.
    EXPECT_TRUE(pool.release(current_handle));
    EXPECT_EQ(pool.in_use(), 0U);
    EXPECT_EQ(pool.available(), 1U);
}

TEST(OrderPoolGenerationTests, HandleForSupportsIndexBasedAcquisition) {
    lob::OrderPool pool(2);

    // Existing BookPool code acquires slots by index. handle_for() allows
    // that code to construct a generation-aware handle without changing
    // the allocation API all at once.
    const lob::OrderPool::Index index = pool.acquire();

    ASSERT_NE(index, lob::OrderPool::no_slot);

    const auto handle = pool.handle_for(index);

    EXPECT_EQ(handle.index, index);
    EXPECT_NE(handle.generation, 0U);
    EXPECT_TRUE(pool.is_valid(handle));

    // Releasing through the legacy index-based interface must also invalidate
    // the generation-aware handle.
    EXPECT_TRUE(pool.release(index));
    EXPECT_FALSE(pool.is_valid(handle));
}

TEST(OrderPoolGenerationTests, ExhaustedPoolReturnsInvalidHandle) {
    lob::OrderPool pool(1);

    const auto first_handle = pool.acquire_handle();
    ASSERT_TRUE(pool.is_valid(first_handle));

    // There are no free slots, so another acquisition must fail safely.
    const auto failed_handle = pool.acquire_handle();

    EXPECT_EQ(failed_handle.index, lob::OrderPool::no_slot);
    EXPECT_EQ(failed_handle.generation, 0U);
    EXPECT_FALSE(pool.is_valid(failed_handle));

    // Failed allocation must not disturb the live order.
    EXPECT_TRUE(pool.is_valid(first_handle));
    EXPECT_EQ(pool.in_use(), 1U);
    EXPECT_EQ(pool.available(), 0U);
}

}  // namespace
}  // namespace lob


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

}  // namespace
}  // namespace lob

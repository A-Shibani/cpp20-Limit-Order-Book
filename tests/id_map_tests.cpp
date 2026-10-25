#include "lob/id_map.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace {

// A small stand-in for BookPool's OrderHandle.
// We only need a value that can be stored and retrieved.
struct TestValue {
    int payload;
};

// Replicates OrderIdMap's bucket calculation solely to construct
// keys that deliberately collide in the tests.
//
// This lets us test deletion in the middle of a collision chain
// deterministically, rather than hoping random IDs will collide.
std::size_t test_bucket_index(
    lob::OrderId key,
    std::size_t bucket_count
) {
    std::uint64_t mixed = static_cast<std::uint64_t>(key);

    mixed ^= mixed >> 30;
    mixed *= 0xbf58476d1ce4e5b9ULL;
    mixed ^= mixed >> 27;
    mixed *= 0x94d049bb133111ebULL;
    mixed ^= mixed >> 31;

    return static_cast<std::size_t>(mixed) & (bucket_count - 1);
}

// Find distinct keys that have the same initial bucket.
// Linear probing must then place them in different buckets.
std::vector<lob::OrderId> find_colliding_keys(
    std::size_t bucket_count,
    std::size_t required
) {
    std::unordered_map<std::size_t, std::vector<lob::OrderId>> by_bucket;

    for (lob::OrderId key = 1; key < 1'000'000; ++key) {
        const std::size_t bucket = test_bucket_index(key, bucket_count);
        auto& keys = by_bucket[bucket];

        keys.push_back(key);

        if (keys.size() == required) {
            return keys;
        }
    }

    // The chosen table size should make collisions easy to find.
    // An empty result makes the test fail clearly if that assumption
    // ever stops being true.
    return {};
}

}  // namespace

TEST(OrderIdMapTests, StartsEmptyAndReportsCapacity) {
    lob::OrderIdMap<TestValue> map(8);

    EXPECT_TRUE(map.empty());
    EXPECT_EQ(map.size(), 0U);
    EXPECT_EQ(map.max_size(), 8U);

    // Bucket count is deliberately larger than the live-entry limit.
    EXPECT_GE(map.bucket_count(), map.max_size());

    // The implementation uses a power-of-two bucket count.
    const std::size_t buckets = map.bucket_count();
    EXPECT_GT(buckets, 0U);
    EXPECT_EQ(buckets & (buckets - 1), 0U);
}

TEST(OrderIdMapTests, InsertsAndFindsValues) {
    lob::OrderIdMap<TestValue> map(8);

    EXPECT_TRUE(map.insert(101, TestValue{42}));
    EXPECT_EQ(map.size(), 1U);
    EXPECT_FALSE(map.empty());

    // A mutable lookup returns a pointer to the stored value.
    TestValue* found = map.find(101);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->payload, 42);

    // Looking up an absent ID returns nullptr.
    EXPECT_EQ(map.find(999), nullptr);

    // Check the read-only overload as well.
    const auto& read_only_map = map;
    const TestValue* const_found = read_only_map.find(101);
    ASSERT_NE(const_found, nullptr);
    EXPECT_EQ(const_found->payload, 42);
}

TEST(OrderIdMapTests, RejectsDuplicateWithoutOverwritingExistingValue) {
    lob::OrderIdMap<TestValue> map(4);

    ASSERT_TRUE(map.insert(7, TestValue{10}));

    // Inserting a duplicate must not silently replace the old value.
    EXPECT_FALSE(map.insert(7, TestValue{99}));

    EXPECT_EQ(map.size(), 1U);

    const TestValue* found = map.find(7);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->payload, 10);
}

TEST(OrderIdMapTests, DeletingFirstCollisionDoesNotBreakLaterLookups) {
    lob::OrderIdMap<TestValue> map(8);

    const auto keys = find_colliding_keys(map.bucket_count(), 3);
    ASSERT_EQ(keys.size(), 3U);

    ASSERT_TRUE(map.insert(keys[0], TestValue{10}));
    ASSERT_TRUE(map.insert(keys[1], TestValue{20}));
    ASSERT_TRUE(map.insert(keys[2], TestValue{30}));

    // All three IDs share the same initial bucket, so linear probing
    // places them in a collision chain.
    //
    // Delete the first entry. The remaining keys must still be found.
    ASSERT_TRUE(map.erase(keys[0]));

    EXPECT_EQ(map.find(keys[0]), nullptr);

    const TestValue* second = map.find(keys[1]);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->payload, 20);

    const TestValue* third = map.find(keys[2]);
    ASSERT_NE(third, nullptr);
    EXPECT_EQ(third->payload, 30);

    EXPECT_EQ(map.size(), 2U);
}

TEST(OrderIdMapTests, ReusesTombstoneWithoutAllowingDuplicateKeys) {
    lob::OrderIdMap<TestValue> map(8);

    const auto keys = find_colliding_keys(map.bucket_count(), 3);
    ASSERT_EQ(keys.size(), 3U);

    ASSERT_TRUE(map.insert(keys[0], TestValue{10}));
    ASSERT_TRUE(map.insert(keys[1], TestValue{20}));

    // Deleting the first key leaves a tombstone at the start of the
    // probe chain.
    ASSERT_TRUE(map.erase(keys[0]));

    // The new, distinct key can reuse that tombstone.
    EXPECT_TRUE(map.insert(keys[2], TestValue{30}));

    // A duplicate further along the collision chain must still be found.
    // The map must not insert another copy into the earlier tombstone.
    EXPECT_FALSE(map.insert(keys[1], TestValue{999}));

    EXPECT_EQ(map.size(), 2U);

    const TestValue* second = map.find(keys[1]);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->payload, 20);

    const TestValue* third = map.find(keys[2]);
    ASSERT_NE(third, nullptr);
    EXPECT_EQ(third->payload, 30);
}

TEST(OrderIdMapTests, CapacityIsBoundedAndRecoversAfterErase) {
    lob::OrderIdMap<TestValue> map(2);

    ASSERT_TRUE(map.insert(1, TestValue{10}));
    ASSERT_TRUE(map.insert(2, TestValue{20}));

    // The map has reached its configured maximum number of live entries.
    EXPECT_FALSE(map.insert(3, TestValue{30}));
    EXPECT_EQ(map.size(), 2U);

    // Deleting an entry frees live-entry capacity.
    ASSERT_TRUE(map.erase(1));
    EXPECT_EQ(map.size(), 1U);

    EXPECT_TRUE(map.insert(3, TestValue{30}));
    EXPECT_EQ(map.size(), 2U);

    EXPECT_EQ(map.find(1), nullptr);
    EXPECT_NE(map.find(2), nullptr);
    EXPECT_NE(map.find(3), nullptr);
}

TEST(OrderIdMapTests, EraseOfMissingKeyReturnsFalse) {
    lob::OrderIdMap<TestValue> map(4);

    EXPECT_FALSE(map.erase(123));
    EXPECT_EQ(map.size(), 0U);

    ASSERT_TRUE(map.insert(123, TestValue{55}));

    // Erasing the same ID twice should succeed only the first time.
    EXPECT_TRUE(map.erase(123));
    EXPECT_FALSE(map.erase(123));

    EXPECT_TRUE(map.empty());
}

TEST(OrderIdMapTests, ClearRemovesEntriesAndTombstones) {
    lob::OrderIdMap<TestValue> map(8);

    ASSERT_TRUE(map.insert(10, TestValue{100}));
    ASSERT_TRUE(map.insert(20, TestValue{200}));
    ASSERT_TRUE(map.erase(10));

    // Clear must remove both live entries and deleted-slot markers.
    map.clear();

    EXPECT_TRUE(map.empty());
    EXPECT_EQ(map.size(), 0U);
    EXPECT_EQ(map.find(10), nullptr);
    EXPECT_EQ(map.find(20), nullptr);

    // The table remains usable after clearing.
    EXPECT_TRUE(map.insert(30, TestValue{300}));

    const TestValue* found = map.find(30);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->payload, 300);
}

TEST(OrderIdMapTests, ForEachVisitsOnlyLiveEntries) {
    lob::OrderIdMap<TestValue> map(8);

    ASSERT_TRUE(map.insert(1, TestValue{10}));
    ASSERT_TRUE(map.insert(2, TestValue{20}));
    ASSERT_TRUE(map.insert(3, TestValue{30}));
    ASSERT_TRUE(map.erase(2));

    std::size_t visited = 0;
    int payload_sum = 0;

    map.for_each([&](lob::OrderId id, TestValue& value) {
        ++visited;
        payload_sum += value.payload;

        // Every visited key should still be present.
        EXPECT_NE(map.find(id), nullptr);
    });

    EXPECT_EQ(visited, 2U);
    EXPECT_EQ(payload_sum, 40);

    // Confirm that the const overload also visits only live entries.
    const auto& read_only_map = map;
    std::size_t const_visited = 0;

    read_only_map.for_each(
        [&](lob::OrderId, const TestValue&) {
            ++const_visited;
        }
    );

    EXPECT_EQ(const_visited, 2U);
}
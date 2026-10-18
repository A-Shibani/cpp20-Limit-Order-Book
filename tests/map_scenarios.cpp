
#include "lob/book_map.hpp"

#include <limits>
#include <vector>

#include <gtest/gtest.h>

namespace lob {
namespace {

// Helper used by the tests to collect emitted events in order.
using Events = std::vector<Event>;

TEST(BookMapScenarios, MatchesBestPriceAndOldestOrderFirst) {
    BookMap book;
    Events events;

    const auto collect = [&events](const Event& event) {
        events.push_back(event);
    };

    // Create two orders at the same ask price and one at a worse price.
    book.submit(OrderRequest::limit_order(1, Side::Sell, 2, 101), collect);
    book.submit(OrderRequest::limit_order(2, Side::Sell, 3, 101), collect);
    book.submit(OrderRequest::limit_order(3, Side::Sell, 4, 102), collect);

    events.clear();

    // This buy can trade at both 101 and 102, but needs only four units.
    book.submit(OrderRequest::limit_order(10, Side::Buy, 4, 102), collect);

    ASSERT_EQ(events.size(), 3U);

    EXPECT_EQ(
        events[0],
        (Event{AckEvent{10, Operation::Submit}})
    );

    // The oldest order at 101 fills first.
    EXPECT_EQ(events[1], (Event{TradeEvent{1, 10, 101, 2}}));

    // The remaining two units come from the next order at 101.
    EXPECT_EQ(events[2], (Event{TradeEvent{2, 10, 101, 2}}));

    const auto resting = book.snapshot();

    ASSERT_EQ(resting.size(), 2U);
    EXPECT_EQ(
        resting[0],
        (RestingOrderSnapshot{2, Side::Sell, 101, 1, 2})
    );
    EXPECT_EQ(
        resting[1],
        (RestingOrderSnapshot{3, Side::Sell, 102, 4, 3})
    );

    EXPECT_TRUE(book.check_invariants());
}

TEST(BookMapScenarios, CancellationRemovesOrderAndUpdatesLevelTotal) {
    BookMap book;
    Events events;

    const auto collect = [&events](const Event& event) {
        events.push_back(event);
    };

    book.submit(OrderRequest::limit_order(1, Side::Buy, 5, 100), collect);
    book.submit(OrderRequest::limit_order(2, Side::Buy, 7, 100), collect);

    events.clear();
    book.cancel(1, collect);

    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0], (Event{CancelAckEvent{1}}));

    const auto resting = book.snapshot();

    ASSERT_EQ(resting.size(), 1U);
    EXPECT_EQ(
        resting[0],
        (RestingOrderSnapshot{2, Side::Buy, 100, 7, 2})
    );

    // This also checks that the aggregate quantity is now exactly seven.
    EXPECT_TRUE(book.check_invariants());
}

TEST(BookMapScenarios, QuantityReductionPreservesFifoPriority) {
    BookMap book;
    Events events;

    const auto collect = [&events](const Event& event) {
        events.push_back(event);
    };

    book.submit(OrderRequest::limit_order(1, Side::Sell, 3, 101), collect);
    book.submit(OrderRequest::limit_order(2, Side::Sell, 3, 101), collect);

    events.clear();
    book.modify(1, 101, 2, collect);

    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0], (Event{AckEvent{1, Operation::Modify}}));

    // The first order retains priority after the quantity reduction.
    events.clear();
    book.submit(OrderRequest::market_order(3, Side::Buy, 3), collect);

    ASSERT_EQ(events.size(), 3U);
    EXPECT_EQ(events[1], (Event{TradeEvent{1, 3, 101, 2}}));
    EXPECT_EQ(events[2], (Event{TradeEvent{2, 3, 101, 1}}));

    EXPECT_TRUE(book.check_invariants());
}

TEST(BookMapScenarios, RejectsOverflowWithoutChangingOriginalBook) {
    BookMap book;
    Events events;

    const auto collect = [&events](const Event& event) {
        events.push_back(event);
    };

    constexpr Quantity maximum =
        std::numeric_limits<Quantity>::max();

    book.submit(
        OrderRequest::limit_order(1, Side::Sell, maximum, 101),
        collect
    );

    const auto before = book.snapshot();
    events.clear();

    // Adding one more unit at the same level would overflow its total.
    book.submit(
        OrderRequest::limit_order(2, Side::Sell, 1, 101),
        collect
    );

    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(
        events[0],
        (Event{RejectEvent{
            2, Operation::Submit, RejectReason::QuantityOverflow
        }})
    );

    EXPECT_EQ(book.snapshot(), before);
    EXPECT_TRUE(book.check_invariants());
}

}  // namespace
}  // namespace lob

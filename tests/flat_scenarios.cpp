
#include "lob/book_flat.hpp"

#include <limits>
#include <vector>

#include <gtest/gtest.h>

namespace lob {
namespace {

// Collect events in order so tests can check the exact observable behaviour.
using Events = std::vector<Event>;

TEST(BookFlatScenarios, MatchesBestPriceAndOldestOrderFirst) {
    BookFlat book;
    Events events;

    const auto collect = [&events](const Event& event) {
        events.push_back(event);
    };

    // Three resting asks: two at 101, one at the worse price of 102.
    book.submit(OrderRequest::limit_order(1, Side::Sell, 2, 101), collect);
    book.submit(OrderRequest::limit_order(2, Side::Sell, 3, 101), collect);
    book.submit(OrderRequest::limit_order(3, Side::Sell, 4, 102), collect);

    events.clear();

    // This buy can execute at 101 and 102, but needs only four units.
    book.submit(OrderRequest::limit_order(10, Side::Buy, 4, 102), collect);

    ASSERT_EQ(events.size(), 3U);
    EXPECT_EQ(events[0], (Event{AckEvent{10, Operation::Submit}}));

    // The older order at the best price must execute first.
    EXPECT_EQ(events[1], (Event{TradeEvent{1, 10, 101, 2}}));
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

TEST(BookFlatScenarios, RefreshesBestAskAfterLevelBecomesEmpty) {
    BookFlat book;
    Events events;

    const auto collect = [&events](const Event& event) {
        events.push_back(event);
    };

    // The best ask is 101; the next ask is 103.
    book.submit(OrderRequest::limit_order(1, Side::Sell, 2, 101), collect);
    book.submit(OrderRequest::limit_order(2, Side::Sell, 3, 103), collect);

    events.clear();

    // This market buy completely empties the 101 level.
    book.submit(OrderRequest::market_order(10, Side::Buy, 2), collect);

    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[1], (Event{TradeEvent{1, 10, 101, 2}}));

    // The cached best ask must now point to 103.
    events.clear();
    book.submit(OrderRequest::market_order(11, Side::Buy, 1), collect);

    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[1], (Event{TradeEvent{2, 11, 103, 1}}));

    const auto resting = book.snapshot();

    ASSERT_EQ(resting.size(), 1U);
    EXPECT_EQ(
        resting[0],
        (RestingOrderSnapshot{2, Side::Sell, 103, 2, 2})
    );

    EXPECT_TRUE(book.check_invariants());
}

TEST(BookFlatScenarios, RefreshesBestBidAfterCancellation) {
    BookFlat book;
    Events events;

    const auto collect = [&events](const Event& event) {
        events.push_back(event);
    };

    // There are bids at 101 and 100.
    book.submit(OrderRequest::limit_order(1, Side::Buy, 2, 101), collect);
    book.submit(OrderRequest::limit_order(2, Side::Buy, 3, 100), collect);

    events.clear();
    book.cancel(1, collect);

    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0], (Event{CancelAckEvent{1}}));

    // After removing the 101 bid, a market sell must hit the 100 bid.
    events.clear();
    book.submit(OrderRequest::market_order(10, Side::Sell, 1), collect);

    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[1], (Event{TradeEvent{2, 10, 100, 1}}));

    const auto resting = book.snapshot();

    ASSERT_EQ(resting.size(), 1U);
    EXPECT_EQ(
        resting[0],
        (RestingOrderSnapshot{2, Side::Buy, 100, 2, 2})
    );

    EXPECT_TRUE(book.check_invariants());
}

TEST(BookFlatScenarios, QuantityReductionPreservesFifoPriority) {
    BookFlat book;
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

    // Reducing the first order must not put it behind the second one.
    events.clear();
    book.submit(OrderRequest::market_order(3, Side::Buy, 3), collect);

    ASSERT_EQ(events.size(), 3U);
    EXPECT_EQ(events[1], (Event{TradeEvent{1, 3, 101, 2}}));
    EXPECT_EQ(events[2], (Event{TradeEvent{2, 3, 101, 1}}));

    EXPECT_TRUE(book.check_invariants());
}

TEST(BookFlatScenarios, RejectsOverflowWithoutChangingBook) {
    BookFlat book;
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

    // Adding one more unit at this price would overflow the level total.
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


TEST(BookFlatScenarios, FindsNextAskAcrossBitmapWordBoundary) {
    BookFlat book;
    Events events;

    const auto collect = [&events](const Event& event) {
        events.push_back(event);
    };

    // With the default range, price 101 and price 300 fall in different
    // bitmap words. Emptying 101 must find 300 without scanning every tick.
    book.submit(
        OrderRequest::limit_order(1, Side::Sell, 2, 101),
        collect
    );
    book.submit(
        OrderRequest::limit_order(2, Side::Sell, 3, 300),
        collect
    );

    events.clear();
    book.submit(
        OrderRequest::market_order(10, Side::Buy, 2),
        collect
    );

    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[1], (Event{TradeEvent{1, 10, 101, 2}}));

    // The next market buy must locate the ask at 300.
    events.clear();
    book.submit(
        OrderRequest::market_order(11, Side::Buy, 1),
        collect
    );

    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[1], (Event{TradeEvent{2, 11, 300, 1}}));

    EXPECT_TRUE(book.check_invariants());
}

TEST(BookFlatScenarios, FindsNextBidAcrossBitmapWordBoundary) {
    BookFlat book;
    Events events;

    const auto collect = [&events](const Event& event) {
        events.push_back(event);
    };

    // Price 300 and price 101 occupy different bitmap words.
    book.submit(
        OrderRequest::limit_order(1, Side::Buy, 2, 300),
        collect
    );
    book.submit(
        OrderRequest::limit_order(2, Side::Buy, 3, 101),
        collect
    );

    events.clear();
    book.cancel(1, collect);

    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0], (Event{CancelAckEvent{1}}));

    // Once bid 300 disappears, the bitmap must find bid 101.
    events.clear();
    book.submit(
        OrderRequest::market_order(10, Side::Sell, 1),
        collect
    );

    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[1], (Event{TradeEvent{2, 10, 101, 1}}));

    EXPECT_TRUE(book.check_invariants());
}


}  // namespace
}  // namespace lob

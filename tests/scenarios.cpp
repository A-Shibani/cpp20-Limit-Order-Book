#include "lob/book_ref.hpp"

#include <vector>

#include <gtest/gtest.h>

#include <limits>

namespace lob {
namespace {

TEST(BookRefScenarios, ExecutesAtRestingPriceAndPartiallyFillsMaker) {
    BookRef book;
    std::vector<Event> events;

    const auto collect = [&events](const Event& event) {
        events.push_back(event);
    };

    // First, rest a buy order for 10 units at 100.
    book.submit(
        OrderRequest::limit_order(1, Side::Buy, 10, 100),
        collect
    );

    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(
        events.front(),
        (Event{AckEvent{1, Operation::Submit}})
    );

    events.clear();

    // Sell 4 units at a limit of 99. The resting buy at 100 crosses.
    book.submit(
        OrderRequest::limit_order(2, Side::Sell, 4, 99),
        collect
    );

    ASSERT_EQ(events.size(), 2U);

    // Event ordering: acknowledgement first, then the trade.
    EXPECT_EQ(
        events[0],
        (Event{AckEvent{2, Operation::Submit}})
    );

    EXPECT_EQ(
        events[1],
        (Event{TradeEvent{1, 2, 100, 4}})
    );

    // The resting buy has 6 units left and retains its original sequence.
    const auto resting = book.snapshot();

    ASSERT_EQ(resting.size(), 1U);
    EXPECT_EQ(
        resting.front(),
        (RestingOrderSnapshot{1, Side::Buy, 100, 6, 1})
    );

    EXPECT_TRUE(book.check_invariants());
}

TEST(BookRefScenarios, UnfilledLimitRemainderRests) {
    BookRef book;
    std::vector<Event> events;

    const auto collect = [&events](const Event& event) {
        events.push_back(event);
    };

    // Rest 5 units on the bid.
    book.submit(
        OrderRequest::limit_order(1, Side::Buy, 5, 100),
        collect
    );

    events.clear();

    // The sell executes 5 units and has 3 units left.
    book.submit(
        OrderRequest::limit_order(2, Side::Sell, 8, 99),
        collect
    );

    ASSERT_EQ(events.size(), 2U);

    EXPECT_EQ(
        events[0],
        (Event{AckEvent{2, Operation::Submit}})
    );

    EXPECT_EQ(
        events[1],
        (Event{TradeEvent{1, 2, 100, 5}})
    );

    // The incoming sell remainder now rests at its own limit price.
    const auto resting = book.snapshot();

    ASSERT_EQ(resting.size(), 1U);
    EXPECT_EQ(
        resting.front(),
        (RestingOrderSnapshot{2, Side::Sell, 99, 3, 2})
    );

    EXPECT_TRUE(book.check_invariants());
}

TEST(BookRefScenarios, MarketOrderOnEmptyBookNeverRests) {
    BookRef book;
    std::vector<Event> events;

    const auto collect = [&events](const Event& event) {
        events.push_back(event);
    };

    book.submit(
        OrderRequest::market_order(7, Side::Buy, 3),
        collect
    );

    ASSERT_EQ(events.size(), 1U);

    EXPECT_EQ(
        events.front(),
        (Event{AckEvent{7, Operation::Submit}})
    );

    EXPECT_TRUE(book.snapshot().empty());
    EXPECT_TRUE(book.check_invariants());
}


TEST(BookRefScenarios, MatchesOldestOrderFirstAtSamePrice) {
    BookRef book;
    std::vector<Event> events;

    const auto collect = [&events](const Event& event) {
        events.push_back(event);
    };

    // Two sell orders rest at the same price.
    book.submit(
        OrderRequest::limit_order(10, Side::Sell, 3, 101),
        collect
    );
    book.submit(
        OrderRequest::limit_order(11, Side::Sell, 4, 101),
        collect
    );

    events.clear();

    // The incoming buy needs 5 units.
    book.submit(
        OrderRequest::limit_order(20, Side::Buy, 5, 101),
        collect
    );

    ASSERT_EQ(events.size(), 3U);

    EXPECT_EQ(
        events[0],
        (Event{AckEvent{20, Operation::Submit}})
    );
    EXPECT_EQ(
        events[1],
        (Event{TradeEvent{10, 20, 101, 3}})
    );
    EXPECT_EQ(
        events[2],
        (Event{TradeEvent{11, 20, 101, 2}})
    );

    const auto resting = book.snapshot();

    ASSERT_EQ(resting.size(), 1U);
    EXPECT_EQ(
        resting[0],
        (RestingOrderSnapshot{11, Side::Sell, 101, 2, 2})
    );

    EXPECT_TRUE(book.check_invariants());
}

TEST(BookRefScenarios, SweepsMultiplePriceLevelsInPricePriorityOrder) {
    BookRef book;
    std::vector<Event> events;

    const auto collect = [&events](const Event& event) {
        events.push_back(event);
    };

    book.submit(
        OrderRequest::limit_order(1, Side::Sell, 2, 101),
        collect
    );
    book.submit(
        OrderRequest::limit_order(2, Side::Sell, 3, 102),
        collect
    );
    book.submit(
        OrderRequest::limit_order(3, Side::Sell, 4, 104),
        collect
    );

    events.clear();

    // Buy 4 units, sweeping the first two ask levels.
    book.submit(
        OrderRequest::limit_order(10, Side::Buy, 4, 102),
        collect
    );

    ASSERT_EQ(events.size(), 3U);

    EXPECT_EQ(
        events[0],
        (Event{AckEvent{10, Operation::Submit}})
    );
    EXPECT_EQ(
        events[1],
        (Event{TradeEvent{1, 10, 101, 2}})
    );
    EXPECT_EQ(
        events[2],
        (Event{TradeEvent{2, 10, 102, 2}})
    );

    // The third level must remain untouched.
    const auto resting = book.snapshot();

    ASSERT_EQ(resting.size(), 2U);
    EXPECT_EQ(
        resting[0],
        (RestingOrderSnapshot{2, Side::Sell, 102, 1, 2})
    );
    EXPECT_EQ(
        resting[1],
        (RestingOrderSnapshot{3, Side::Sell, 104, 4, 3})
    );

    EXPECT_TRUE(book.check_invariants());
}

TEST(BookRefScenarios, MarketOrderDiscardsUnfilledRemainder) {
    BookRef book;
    std::vector<Event> events;

    const auto collect = [&events](const Event& event) {
        events.push_back(event);
    };

    book.submit(
        OrderRequest::limit_order(1, Side::Sell, 2, 101),
        collect
    );

    events.clear();

    // Only 2 units are available, although the market buy requests 5.
    book.submit(
        OrderRequest::market_order(2, Side::Buy, 5),
        collect
    );

    ASSERT_EQ(events.size(), 2U);

    EXPECT_EQ(
        events[0],
        (Event{AckEvent{2, Operation::Submit}})
    );
    EXPECT_EQ(
        events[1],
        (Event{TradeEvent{1, 2, 101, 2}})
    );

    // Market orders never leave a resting remainder.
    EXPECT_TRUE(book.snapshot().empty());
    EXPECT_TRUE(book.check_invariants());
}


TEST(BookRefScenarios, CancelsRestingOrder) {
    BookRef book;
    std::vector<Event> events;

    const auto collect = [&events](const Event& event) {
        events.push_back(event);
    };

    book.submit(
        OrderRequest::limit_order(1, Side::Buy, 5, 100),
        collect
    );

    events.clear();
    book.cancel(1, collect);

    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(
        events[0],
        (Event{CancelAckEvent{1}})
    );

    EXPECT_TRUE(book.snapshot().empty());
    EXPECT_TRUE(book.check_invariants());
}

TEST(BookRefScenarios, RejectsCancelForUnknownOrFilledOrder) {
    BookRef book;
    std::vector<Event> events;

    const auto collect = [&events](const Event& event) {
        events.push_back(event);
    };

    // An ID that was never in the book.
    book.cancel(99, collect);

    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(
        events[0],
        (Event{RejectEvent{
            99, Operation::Cancel, RejectReason::UnknownId
        }})
    );

    // Rest an order and then fill it completely.
    events.clear();

    book.submit(
        OrderRequest::limit_order(1, Side::Sell, 2, 101),
        collect
    );

    events.clear();

    book.submit(
        OrderRequest::market_order(2, Side::Buy, 2),
        collect
    );

    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(
        events[1],
        (Event{TradeEvent{1, 2, 101, 2}})
    );

    // A fully filled order is no longer cancellable.
    events.clear();
    book.cancel(1, collect);

    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(
        events[0],
        (Event{RejectEvent{
            1, Operation::Cancel, RejectReason::UnknownId
        }})
    );

    EXPECT_TRUE(book.snapshot().empty());
    EXPECT_TRUE(book.check_invariants());
}

TEST(BookRefScenarios, RejectsInvalidSubmissionsWithoutChangingBook) {
    BookRef book;
    std::vector<Event> events;

    const auto collect = [&events](const Event& event) {
        events.push_back(event);
    };

    // Establish one live order.
    book.submit(
        OrderRequest::limit_order(1, Side::Sell, 5, 101),
        collect
    );

    const auto before = book.snapshot();
    events.clear();

    // Duplicate ID.
    book.submit(
        OrderRequest::limit_order(1, Side::Buy, 1, 100),
        collect
    );

    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(
        events[0],
        (Event{RejectEvent{
            1, Operation::Submit, RejectReason::DuplicateId
        }})
    );
    EXPECT_EQ(book.snapshot(), before);

    // Zero quantity.
    events.clear();

    book.submit(
        OrderRequest::limit_order(2, Side::Buy, 0, 100),
        collect
    );

    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(
        events[0],
        (Event{RejectEvent{
            2, Operation::Submit, RejectReason::ZeroQuantity
        }})
    );
    EXPECT_EQ(book.snapshot(), before);

    // Price above the configured maximum of 65,535.
    events.clear();

    book.submit(
        OrderRequest::limit_order(3, Side::Buy, 1, 65'536),
        collect
    );

    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(
        events[0],
        (Event{RejectEvent{
            3, Operation::Submit, RejectReason::PriceOutOfRange
        }})
    );

    // Rejected requests must leave existing state untouched.
    EXPECT_EQ(book.snapshot(), before);
    EXPECT_TRUE(book.check_invariants());
}


TEST(BookRefScenarios, AllowsIdReuseAfterPreviousOrderIsFilled) {
    BookRef book;
    std::vector<Event> events;

    const auto collect = [&events](const Event& event) {
        events.push_back(event);
    };

    book.submit(
        OrderRequest::limit_order(1, Side::Buy, 2, 100),
        collect
    );

    events.clear();

    book.submit(
        OrderRequest::limit_order(2, Side::Sell, 2, 100),
        collect
    );

    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(
        events[1],
        (Event{TradeEvent{1, 2, 100, 2}})
    );

    // ID 1 is now free because its original order was filled.
    events.clear();

    book.submit(
        OrderRequest::limit_order(1, Side::Sell, 3, 102),
        collect
    );

    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(
        events[0],
        (Event{AckEvent{1, Operation::Submit}})
    );

    const auto resting = book.snapshot();

    ASSERT_EQ(resting.size(), 1U);
    EXPECT_EQ(
        resting[0],
        (RestingOrderSnapshot{1, Side::Sell, 102, 3, 2})
    );

    EXPECT_TRUE(book.check_invariants());
}


TEST(BookRefScenarios, QuantityReductionPreservesQueuePriority) {
    BookRef book;
    std::vector<Event> events;

    const auto collect = [&events](const Event& event) {
        events.push_back(event);
    };

    book.submit(
        OrderRequest::limit_order(1, Side::Sell, 3, 101),
        collect
    );
    book.submit(
        OrderRequest::limit_order(2, Side::Sell, 4, 101),
        collect
    );

    events.clear();
    book.modify(1, 101, 2, collect);

    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(
        events[0],
        (Event{AckEvent{1, Operation::Modify}})
    );

    // Consume 3 units. Order 1 should still execute first.
    events.clear();
    book.submit(
        OrderRequest::market_order(3, Side::Buy, 3),
        collect
    );

    ASSERT_EQ(events.size(), 3U);
    EXPECT_EQ(
        events[1],
        (Event{TradeEvent{1, 3, 101, 2}})
    );
    EXPECT_EQ(
        events[2],
        (Event{TradeEvent{2, 3, 101, 1}})
    );

    const auto resting = book.snapshot();

    ASSERT_EQ(resting.size(), 1U);
    EXPECT_EQ(
        resting[0],
        (RestingOrderSnapshot{2, Side::Sell, 101, 3, 2})
    );

    EXPECT_TRUE(book.check_invariants());
}

TEST(BookRefScenarios, UnchangedModifyIsNoOp) {
    BookRef book;
    std::vector<Event> events;

    const auto collect = [&events](const Event& event) {
        events.push_back(event);
    };

    book.submit(
        OrderRequest::limit_order(1, Side::Buy, 5, 100),
        collect
    );

    const auto before = book.snapshot();

    events.clear();
    book.modify(1, 100, 5, collect);

    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(
        events[0],
        (Event{AckEvent{1, Operation::Modify}})
    );
    EXPECT_EQ(book.snapshot(), before);

    EXPECT_TRUE(book.check_invariants());
}

TEST(BookRefScenarios, QuantityIncreaseLosesQueuePriority) {
    BookRef book;
    std::vector<Event> events;

    const auto collect = [&events](const Event& event) {
        events.push_back(event);
    };

    book.submit(
        OrderRequest::limit_order(1, Side::Sell, 3, 101),
        collect
    );
    book.submit(
        OrderRequest::limit_order(2, Side::Sell, 3, 101),
        collect
    );

    events.clear();
    book.modify(1, 101, 4, collect);

    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(
        events[0],
        (Event{AckEvent{1, Operation::Modify}})
    );

    // Order 2 retains priority because order 1 was replaced.
    events.clear();
    book.submit(
        OrderRequest::market_order(3, Side::Buy, 4),
        collect
    );

    ASSERT_EQ(events.size(), 3U);
    EXPECT_EQ(
        events[1],
        (Event{TradeEvent{2, 3, 101, 3}})
    );
    EXPECT_EQ(
        events[2],
        (Event{TradeEvent{1, 3, 101, 1}})
    );

    const auto resting = book.snapshot();

    ASSERT_EQ(resting.size(), 1U);
    EXPECT_EQ(
        resting[0],
        (RestingOrderSnapshot{1, Side::Sell, 101, 3, 3})
    );

    EXPECT_TRUE(book.check_invariants());
}

TEST(BookRefScenarios, PriceChangeLosesQueuePriorityAtNewLevel) {
    BookRef book;
    std::vector<Event> events;

    const auto collect = [&events](const Event& event) {
        events.push_back(event);
    };

    book.submit(
        OrderRequest::limit_order(1, Side::Sell, 2, 101),
        collect
    );
    book.submit(
        OrderRequest::limit_order(2, Side::Sell, 2, 102),
        collect
    );

    events.clear();
    book.modify(1, 102, 2, collect);

    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(
        events[0],
        (Event{AckEvent{1, Operation::Modify}})
    );

    // Both orders now have price 102. Order 2 is older at that level.
    events.clear();
    book.submit(
        OrderRequest::limit_order(3, Side::Buy, 1, 102),
        collect
    );

    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(
        events[1],
        (Event{TradeEvent{2, 3, 102, 1}})
    );

    const auto resting = book.snapshot();

    ASSERT_EQ(resting.size(), 2U);
    EXPECT_EQ(
        resting[0],
        (RestingOrderSnapshot{2, Side::Sell, 102, 1, 2})
    );
    EXPECT_EQ(
        resting[1],
        (RestingOrderSnapshot{1, Side::Sell, 102, 2, 3})
    );

    EXPECT_TRUE(book.check_invariants());
}

TEST(BookRefScenarios, ModifyCanTradeAndRestRemainder) {
    BookRef book;
    std::vector<Event> events;

    const auto collect = [&events](const Event& event) {
        events.push_back(event);
    };

    book.submit(
        OrderRequest::limit_order(1, Side::Sell, 2, 101),
        collect
    );
    book.submit(
        OrderRequest::limit_order(2, Side::Sell, 3, 103),
        collect
    );
    book.submit(
        OrderRequest::limit_order(3, Side::Buy, 5, 99),
        collect
    );

    events.clear();

    // The replacement buys at up to 102, so it takes the 101 ask.
    book.modify(3, 102, 4, collect);

    ASSERT_EQ(events.size(), 2U);

    EXPECT_EQ(
        events[0],
        (Event{AckEvent{3, Operation::Modify}})
    );
    EXPECT_EQ(
        events[1],
        (Event{TradeEvent{1, 3, 101, 2}})
    );

    // Two units rest at 102; the ask at 103 is untouched.
    const auto resting = book.snapshot();

    ASSERT_EQ(resting.size(), 2U);
    EXPECT_EQ(
        resting[0],
        (RestingOrderSnapshot{3, Side::Buy, 102, 2, 4})
    );
    EXPECT_EQ(
        resting[1],
        (RestingOrderSnapshot{2, Side::Sell, 103, 3, 2})
    );

    EXPECT_TRUE(book.check_invariants());
}

TEST(BookRefScenarios, RejectsInvalidModificationsWithoutChangingBook) {
    BookRef book;
    std::vector<Event> events;

    const auto collect = [&events](const Event& event) {
        events.push_back(event);
    };

    book.submit(
        OrderRequest::limit_order(1, Side::Sell, 5, 101),
        collect
    );

    const auto before = book.snapshot();

    // Unknown order ID.
    events.clear();
    book.modify(99, 100, 1, collect);

    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(
        events[0],
        (Event{RejectEvent{
            99, Operation::Modify, RejectReason::UnknownId
        }})
    );
    EXPECT_EQ(book.snapshot(), before);

    // Zero quantity.
    events.clear();
    book.modify(1, 101, 0, collect);

    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(
        events[0],
        (Event{RejectEvent{
            1, Operation::Modify, RejectReason::ZeroQuantity
        }})
    );
    EXPECT_EQ(book.snapshot(), before);

    // Invalid price.
    events.clear();
    book.modify(1, 65'536, 5, collect);

    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(
        events[0],
        (Event{RejectEvent{
            1, Operation::Modify, RejectReason::PriceOutOfRange
        }})
    );
    EXPECT_EQ(book.snapshot(), before);

    EXPECT_TRUE(book.check_invariants());
}


TEST(BookRefScenarios, RejectsSubmissionThatOverflowsLevelQuantity) {
    BookRef book;
    std::vector<Event> events;

    const auto collect = [&events](const Event& event) {
        events.push_back(event);
    };

    constexpr Quantity maximum =
        std::numeric_limits<Quantity>::max();

    // A single order can consume the full representable quantity.
    book.submit(
        OrderRequest::limit_order(1, Side::Sell, maximum, 101),
        collect
    );

    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(
        events[0],
        (Event{AckEvent{1, Operation::Submit}})
    );

    const auto before = book.snapshot();
    events.clear();

    // One additional unit at this level would overflow its total.
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

TEST(BookRefScenarios, OverflowingModifyLeavesOriginalOrderUntouched) {
    BookRef book;
    std::vector<Event> events;

    const auto collect = [&events](const Event& event) {
        events.push_back(event);
    };

    constexpr Quantity maximum =
        std::numeric_limits<Quantity>::max();

    book.submit(
        OrderRequest::limit_order(1, Side::Sell, maximum, 101),
        collect
    );
    book.submit(
        OrderRequest::limit_order(2, Side::Sell, 1, 102),
        collect
    );

    const auto before = book.snapshot();
    events.clear();

    // Moving order 2 to 101 would overflow that level.
    book.modify(2, 101, 1, collect);

    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(
        events[0],
        (Event{RejectEvent{
            2, Operation::Modify, RejectReason::QuantityOverflow
        }})
    );

    // Rejection must not remove or alter the original order.
    EXPECT_EQ(book.snapshot(), before);
    EXPECT_TRUE(book.check_invariants());
}


}  // namespace
}  // namespace lob

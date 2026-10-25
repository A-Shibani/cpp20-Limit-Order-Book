
#include "lob/book_pool.hpp"

#include <limits>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

namespace lob {
namespace {

using Events = std::vector<Event>;

TEST(BookPoolScenarios, MatchesAtMakerPriceAndPreservesFifo) {
    // This small pool is enough for the three initial sell orders.
    BookPool book(8);
    Events events;

    const auto collect = [&events](const Event& event) {
        events.push_back(event);
    };

    book.submit(OrderRequest::limit_order(1, Side::Sell, 2, 101), collect);
    book.submit(OrderRequest::limit_order(2, Side::Sell, 3, 101), collect);
    book.submit(OrderRequest::limit_order(3, Side::Sell, 4, 102), collect);

    events.clear();

    book.submit(OrderRequest::limit_order(10, Side::Buy, 4, 102), collect);

    ASSERT_EQ(events.size(), 3U);
    EXPECT_EQ(events[0], (Event{AckEvent{10, Operation::Submit}}));
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

TEST(BookPoolScenarios, CancellationReleasesSlotForReuse) {
    // Capacity one lets us verify that cancellation returns a slot to the pool.
    BookPool book(1);
    Events events;

    const auto collect = [&events](const Event& event) {
        events.push_back(event);
    };

    book.submit(OrderRequest::limit_order(1, Side::Buy, 5, 100), collect);

    ASSERT_EQ(book.snapshot().size(), 1U);
    events.clear();

    book.cancel(1, collect);

    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0], (Event{CancelAckEvent{1}}));
    EXPECT_TRUE(book.snapshot().empty());
    EXPECT_TRUE(book.check_invariants());

    // The same physical pool slot can be reused for a different order ID.
    events.clear();
    book.submit(OrderRequest::limit_order(2, Side::Sell, 3, 105), collect);

    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0], (Event{AckEvent{2, Operation::Submit}}));

    const auto resting = book.snapshot();
    ASSERT_EQ(resting.size(), 1U);
    EXPECT_EQ(
        resting[0],
        (RestingOrderSnapshot{2, Side::Sell, 105, 3, 2})
    );

    EXPECT_TRUE(book.check_invariants());
}

TEST(BookPoolScenarios, RejectsWhenPoolIsFullAndOrderWouldNeedToRest) {
    BookPool book(1);
    Events events;

    const auto collect = [&events](const Event& event) {
        events.push_back(event);
    };

    book.submit(OrderRequest::limit_order(1, Side::Buy, 5, 100), collect);

    const auto before = book.snapshot();
    events.clear();

    // This buy cannot match the resting buy and has no free pool slot.
    book.submit(OrderRequest::limit_order(2, Side::Buy, 1, 99), collect);

    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(
        events[0],
        (Event{RejectEvent{
            2, Operation::Submit, RejectReason::OrderPoolExhausted
        }})
    );

    EXPECT_EQ(book.snapshot(), before);
    EXPECT_TRUE(book.check_invariants());
}

TEST(BookPoolScenarios, FullPoolStillAllowsOrderThatTradesCompletely) {
    BookPool book(1);
    Events events;

    const auto collect = [&events](const Event& event) {
        events.push_back(event);
    };

    book.submit(OrderRequest::limit_order(1, Side::Buy, 5, 100), collect);

    events.clear();

    // Even though the pool is full, the incoming sell will fully execute.
    // It doesn't require a new slot, so it must not be rejected.
    book.submit(OrderRequest::limit_order(2, Side::Sell, 5, 100), collect);

    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0], (Event{AckEvent{2, Operation::Submit}}));
    EXPECT_EQ(events[1], (Event{TradeEvent{1, 2, 100, 5}}));

    EXPECT_TRUE(book.snapshot().empty());
    EXPECT_TRUE(book.check_invariants());
}

TEST(BookPoolScenarios, QuantityReductionPreservesQueuePriority) {
    BookPool book(8);
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

    events.clear();
    book.submit(OrderRequest::market_order(3, Side::Buy, 3), collect);

    ASSERT_EQ(events.size(), 3U);
    EXPECT_EQ(events[1], (Event{TradeEvent{1, 3, 101, 2}}));
    EXPECT_EQ(events[2], (Event{TradeEvent{2, 3, 101, 1}}));

    EXPECT_TRUE(book.check_invariants());
}

TEST(BookPoolScenarios, RejectsOverflowingModificationAtomically) {
    BookPool book(4);
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
    book.submit(
        OrderRequest::limit_order(2, Side::Sell, 1, 102),
        collect
    );

    const auto before = book.snapshot();
    events.clear();

    book.modify(2, 101, 1, collect);

    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(
        events[0],
        (Event{RejectEvent{
            2, Operation::Modify, RejectReason::QuantityOverflow
        }})
    );

    EXPECT_EQ(book.snapshot(), before);
    EXPECT_TRUE(book.check_invariants());
}


TEST(BookPoolScenarios, CancellingMiddleOrderPreservesIntrusiveQueue) {
    BookPool book(8);
    Events events;

    const auto collect = [&events](const Event& event) {
        events.push_back(event);
    };

    // These orders form the queue: 1 <-> 2 <-> 3.
    book.submit(
        OrderRequest::limit_order(1, Side::Sell, 2, 101),
        collect
    );
    book.submit(
        OrderRequest::limit_order(2, Side::Sell, 2, 101),
        collect
    );
    book.submit(
        OrderRequest::limit_order(3, Side::Sell, 2, 101),
        collect
    );

    // Remove order 2. Orders 1 and 3 must remain linked in FIFO order.
    events.clear();
    book.cancel(2, collect);

    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0], (Event{CancelAckEvent{2}}));
    EXPECT_TRUE(book.check_invariants());

    // Buy 3 units. The engine must fill order 1, then order 3.
    events.clear();
    book.submit(
        OrderRequest::market_order(10, Side::Buy, 3),
        collect
    );

    ASSERT_EQ(events.size(), 3U);
    EXPECT_EQ(events[0], (Event{AckEvent{10, Operation::Submit}}));
    EXPECT_EQ(events[1], (Event{TradeEvent{1, 10, 101, 2}}));
    EXPECT_EQ(events[2], (Event{TradeEvent{3, 10, 101, 1}}));

    // Order 3 has one unit left. Its queue links and pool slot remain valid.
    const auto resting = book.snapshot();

    ASSERT_EQ(resting.size(), 1U);
    EXPECT_EQ(
        resting[0],
        (RestingOrderSnapshot{3, Side::Sell, 101, 1, 3})
    );

    EXPECT_TRUE(book.check_invariants());
}

// V2.5: After a pool slot is reused, an operation using the old order ID
// must not affect the new order occupying that slot.
TEST(BookPoolGenerationTests, ReusedSlotCannotBeCancelledUsingOldOrderId) {
    // Capacity one guarantees the second resting order reuses the only slot.
    lob::BookPool book(1);

    std::vector<lob::Event> events;

    // Collect events so we can inspect the response to cancelling the old ID.
    const auto event_sink = [&events](const lob::Event& event) {
        events.push_back(event);
    };

    // First order occupies the only slot.
    book.submit(
        lob::OrderRequest::limit_order(1, lob::Side::Buy, 10, 100),
        event_sink
    );

    ASSERT_TRUE(book.check_invariants());
    ASSERT_EQ(book.snapshot().size(), 1U);
    EXPECT_EQ(book.snapshot()[0].id, 1U);

    // Cancel it, returning its slot to the pool.
    book.cancel(1, event_sink);

    ASSERT_TRUE(book.check_invariants());
    ASSERT_TRUE(book.snapshot().empty());

    // A different order now occupies the same pool slot.
    book.submit(
        lob::OrderRequest::limit_order(2, lob::Side::Buy, 20, 101),
        event_sink
    );

    ASSERT_TRUE(book.check_invariants());

    auto snapshot = book.snapshot();
    ASSERT_EQ(snapshot.size(), 1U);
    EXPECT_EQ(snapshot[0].id, 2U);
    EXPECT_EQ(snapshot[0].remaining_quantity, 20U);
    EXPECT_EQ(snapshot[0].price, 101U);

    // Ignore the successful events above; inspect only the next operation.
    events.clear();

    // ID 1 is no longer live. This must be rejected, without cancelling ID 2.
    book.cancel(1, event_sink);

    ASSERT_EQ(events.size(), 1U);

    const auto* rejection = std::get_if<lob::RejectEvent>(&events[0]);
    ASSERT_NE(rejection, nullptr);
    EXPECT_EQ(rejection->order_id, 1U);
    EXPECT_EQ(rejection->reason, lob::RejectReason::UnknownId);

    // The new order must remain live and unchanged.
    snapshot = book.snapshot();
    ASSERT_EQ(snapshot.size(), 1U);
    EXPECT_EQ(snapshot[0].id, 2U);
    EXPECT_EQ(snapshot[0].remaining_quantity, 20U);
    EXPECT_EQ(snapshot[0].price, 101U);

    EXPECT_TRUE(book.check_invariants());
}

}  // namespace
}  // namespace lob

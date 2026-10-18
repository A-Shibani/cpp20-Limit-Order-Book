
#include "lob/events.hpp"
#include "lob/types.hpp"

#include <cstdint>
#include <limits>
#include <type_traits>
#include <variant>

#include <gtest/gtest.h>

namespace lob {
namespace {

static_assert(std::is_same_v<Price, std::uint32_t>);
static_assert(std::is_same_v<OrderId, std::uint64_t>);
static_assert(std::is_same_v<Quantity, std::uint64_t>);
static_assert(std::is_same_v<Sequence, std::uint64_t>);

TEST(PriceRangeTests, IncludesConfiguredBoundaries) {
    constexpr PriceRange range{};

    EXPECT_TRUE(range.valid());
    EXPECT_TRUE(range.contains(1));
    EXPECT_TRUE(range.contains(65'535));
    EXPECT_FALSE(range.contains(0));
    EXPECT_FALSE(range.contains(65'536));
}

TEST(PriceRangeTests, RejectsInvalidConfiguration) {
    constexpr PriceRange zero_minimum{0, 100};
    constexpr PriceRange inverted{100, 10};

    EXPECT_FALSE(zero_minimum.valid());
    EXPECT_FALSE(zero_minimum.contains(10));
    EXPECT_FALSE(inverted.valid());
    EXPECT_FALSE(inverted.contains(50));
}

TEST(OrderRequestTests, LimitOrderHasAnExplicitPrice) {
    constexpr auto request =
        OrderRequest::limit_order(42, Side::Buy, 7, 123);

    EXPECT_EQ(request.id, 42);
    EXPECT_EQ(request.side, Side::Buy);
    EXPECT_EQ(request.quantity, 7);
    ASSERT_TRUE(request.limit_price.has_value());
    EXPECT_EQ(*request.limit_price, 123);
    EXPECT_FALSE(request.is_market());
}

TEST(OrderRequestTests, MarketOrderHasNoPrice) {
    constexpr auto request =
        OrderRequest::market_order(99, Side::Sell, 3);

    EXPECT_EQ(request.id, 99);
    EXPECT_EQ(request.side, Side::Sell);
    EXPECT_EQ(request.quantity, 3);
    EXPECT_FALSE(request.limit_price.has_value());
    EXPECT_TRUE(request.is_market());
}

TEST(EventTests, EventVariantStoresEveryPublicEventShape) {
    const Event ack = AckEvent{42, Operation::Submit};
    const Event reject = RejectEvent{
        42, Operation::Cancel, RejectReason::UnknownId
    };
    const Event trade = TradeEvent{10, 42, 123, 5};
    const Event cancel_ack = CancelAckEvent{10};

    ASSERT_TRUE(std::holds_alternative<AckEvent>(ack));
    ASSERT_TRUE(std::holds_alternative<RejectEvent>(reject));
    ASSERT_TRUE(std::holds_alternative<TradeEvent>(trade));
    ASSERT_TRUE(std::holds_alternative<CancelAckEvent>(cancel_ack));

    EXPECT_EQ(
        std::get<AckEvent>(ack),
        (AckEvent{42, Operation::Submit})
    );

    EXPECT_EQ(
        std::get<RejectEvent>(reject),
        (RejectEvent{
            42, Operation::Cancel, RejectReason::UnknownId
        })
    );

    EXPECT_EQ(
        std::get<TradeEvent>(trade),
        (TradeEvent{10, 42, 123, 5})
    );

    EXPECT_EQ(
        std::get<CancelAckEvent>(cancel_ack),
        (CancelAckEvent{10})
    );
}

TEST(TypeLimitsTests, MaximumRepresentableIdsAndQuantitiesAreIntegerTypes) {
    constexpr auto max_id = std::numeric_limits<OrderId>::max();
    constexpr auto max_quantity =
        std::numeric_limits<Quantity>::max();

    EXPECT_GT(max_id, 0U);
    EXPECT_GT(max_quantity, 0U);
}

}  // namespace
}  // namespace lob

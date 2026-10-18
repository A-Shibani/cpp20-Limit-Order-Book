
#pragma once

#include <cstdint>
#include <optional>

namespace lob {

using Price = std::uint32_t;
using OrderId = std::uint64_t;
using Quantity = std::uint64_t;
using Sequence = std::uint64_t;

enum class Side : std::uint8_t {
    Buy,
    Sell,
};

// The configured price range is inclusive.
// The bounded range will support the future flat-array engine.
struct PriceRange {
    Price minimum{1};
    Price maximum{65'535};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return minimum > 0 && minimum <= maximum;
    }

    [[nodiscard]] constexpr bool contains(Price price) const noexcept {
        return valid() && price >= minimum && price <= maximum;
    }
};

// A limit order has a price.
// A market order has no price.
struct OrderRequest {
    OrderId id{};
    Side side{Side::Buy};
    Quantity quantity{};
    std::optional<Price> limit_price{};

    [[nodiscard]] static constexpr OrderRequest limit_order(
        OrderId order_id,
        Side order_side,
        Quantity order_quantity,
        Price price) noexcept {
        return OrderRequest{
            order_id,
            order_side,
            order_quantity,
            std::optional<Price>{price}
        };
    }

    [[nodiscard]] static constexpr OrderRequest market_order(
        OrderId order_id,
        Side order_side,
        Quantity order_quantity) noexcept {
        return OrderRequest{
            order_id,
            order_side,
            order_quantity,
            std::nullopt
        };
    }

    [[nodiscard]] constexpr bool is_market() const noexcept {
        return !limit_price.has_value();
    }
};

struct RestingOrderSnapshot {
    OrderId id{};
    Side side{Side::Buy};
    Price price{};
    Quantity remaining_quantity{};
    Sequence sequence{};

    friend constexpr bool operator==(
        const RestingOrderSnapshot&,
        const RestingOrderSnapshot&) = default;
};

}  // namespace lob

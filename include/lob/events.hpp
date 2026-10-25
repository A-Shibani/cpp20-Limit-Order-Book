
#pragma once

#include "lob/types.hpp"

#include <variant>

namespace lob {

enum class Operation : std::uint8_t {
    Submit,
    Cancel,
    Modify,
};

enum class RejectReason : std::uint8_t {
    DuplicateId,
    UnknownId,
    ZeroQuantity,
    PriceOutOfRange,
    QuantityOverflow,
    OrderPoolExhausted
};

struct AckEvent {
    OrderId order_id{};
    Operation operation{Operation::Submit};

    friend constexpr bool operator==(
        const AckEvent&, const AckEvent&) = default;
};

struct RejectEvent {
    OrderId order_id{};
    Operation operation{Operation::Submit};
    RejectReason reason{RejectReason::UnknownId};

    friend constexpr bool operator==(
        const RejectEvent&, const RejectEvent&) = default;
};

struct TradeEvent {
    OrderId maker_id{};
    OrderId taker_id{};
    Price price{};
    Quantity quantity{};

    friend constexpr bool operator==(
        const TradeEvent&, const TradeEvent&) = default;
};

struct CancelAckEvent {
    OrderId order_id{};

    friend constexpr bool operator==(
        const CancelAckEvent&, const CancelAckEvent&) = default;
};

// The event is one of these four types.
using Event = std::variant<
    AckEvent,
    RejectEvent,
    TradeEvent,
    CancelAckEvent
>;

}  // namespace lob

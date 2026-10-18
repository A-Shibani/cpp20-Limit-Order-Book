
#include "lob/book_ref.hpp"

#include <cstdint>
#include <random>
#include <vector>

#include <gtest/gtest.h>

namespace lob {
namespace {

TEST(BookRefInvariants, RandomOperationStreamsPreserveBookInvariants) {
    constexpr std::uint64_t seed_count = 100;
    constexpr int operations_per_seed = 500;

    for (std::uint64_t seed = 0; seed < seed_count; ++seed) {
        std::mt19937_64 rng(seed);

        std::uniform_int_distribution<int> action_dist(0, 99);
        std::uniform_int_distribution<int> id_dist(1, 64);
        std::uniform_int_distribution<int> price_dist(95, 105);
        std::uniform_int_distribution<int> quantity_dist(1, 20);
        std::uniform_int_distribution<int> side_dist(0, 1);

        BookRef book;
        std::vector<Event> events;
        events.reserve(32);

        const auto collect = [&events](const Event& event) {
            events.push_back(event);
        };

        for (int step = 0; step < operations_per_seed; ++step) {
            const int action = action_dist(rng);
            const OrderId id =
                static_cast<OrderId>(id_dist(rng));
            const Price price =
                static_cast<Price>(price_dist(rng));
            const Quantity quantity =
                static_cast<Quantity>(quantity_dist(rng));
            const Side side = side_dist(rng) == 0
                ? Side::Buy
                : Side::Sell;

            events.clear();

            // 60% limit submit, 20% cancel,
            // 10% modify, 10% market submit.
            if (action < 60) {
                book.submit(
                    OrderRequest::limit_order(
                        id, side, quantity, price
                    ),
                    collect
                );
            } else if (action < 80) {
                book.cancel(id, collect);
            } else if (action < 90) {
                book.modify(id, price, quantity, collect);
            } else {
                book.submit(
                    OrderRequest::market_order(
                        id, side, quantity
                    ),
                    collect
                );
            }

            SCOPED_TRACE(
                ::testing::Message()
                << "seed=" << seed
                << ", step=" << step
                << ", action=" << action
                << ", order_id=" << id
                << ", price=" << price
                << ", quantity=" << quantity
            );

            ASSERT_TRUE(book.check_invariants());
        }
    }
}

}  // namespace
}  // namespace lob

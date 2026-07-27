#include "bitmap_exchange.hpp"
#include "reference_model.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <set>
#include <utility>
#include <vector>

namespace bx = bitmap_exchange;
namespace bxt = bitmap_exchange::test;

namespace {

[[nodiscard]] std::vector<std::uint16_t> sparse_prices(
    const std::uint16_t low,
    const std::uint16_t high,
    const std::size_t count) {
    std::vector<std::uint16_t> prices;
    prices.reserve(count);
    const auto span = static_cast<std::uint32_t>(high) - low;
    for (std::size_t index = 0; index < count; ++index) {
        const auto base = static_cast<std::uint32_t>(low)
                        + static_cast<std::uint32_t>((static_cast<std::uint64_t>(index) * span) / count);
        prices.push_back(static_cast<std::uint16_t>(base + (index % 3)));
    }
    std::sort(prices.begin(), prices.end());
    prices.erase(std::unique(prices.begin(), prices.end()), prices.end());
    for (std::uint32_t candidate = low; prices.size() < count && candidate <= high; ++candidate) {
        const auto price = static_cast<std::uint16_t>(candidate);
        if (!std::binary_search(prices.begin(), prices.end(), price)) {
            prices.push_back(price);
            std::sort(prices.begin(), prices.end());
        }
    }
    BX_REQUIRE(prices.size() == count);
    return prices;
}

void bitmap_empty_and_idempotence() {
    bx::HierarchicalBitmap bitmap;
    BX_REQUIRE(bitmap.empty());
    BX_REQUIRE(bitmap.first() == bx::kNoAsk);
    BX_REQUIRE(bitmap.last() == bx::kNoBid);
    BX_REQUIRE(bitmap.next(0) == bx::kNoAsk);
    BX_REQUIRE(bitmap.previous(0) == bx::kNoBid);
    BX_REQUIRE(bitmap.validate());

    bitmap.reset(0);
    bitmap.reset(65'535);
    BX_REQUIRE(bitmap.validate());

    bitmap.set(100);
    bitmap.set(100);
    BX_REQUIRE(bitmap.first() == 100);
    BX_REQUIRE(bitmap.last() == 100);
    bitmap.reset(100);
    bitmap.reset(100);
    BX_REQUIRE(bitmap.empty());
    BX_REQUIRE(bitmap.validate());
}

void bitmap_all_singletons() {
    bx::HierarchicalBitmap bitmap;
    for (std::uint32_t value = 0; value < bx::kPriceCount; ++value) {
        const auto price = static_cast<std::uint16_t>(value);
        bitmap.set(price);
        BX_REQUIRE(bitmap.test(price));
        BX_REQUIRE(bitmap.first() == static_cast<std::int32_t>(value));
        BX_REQUIRE(bitmap.last() == static_cast<std::int32_t>(value));
        BX_REQUIRE(bitmap.previous(price) == bx::kNoBid);
        BX_REQUIRE(bitmap.next(price) == bx::kNoAsk);
        BX_REQUIRE(bitmap.validate());
        bitmap.reset(price);
    }
    BX_REQUIRE(bitmap.empty());
}

void bitmap_hierarchy_boundaries() {
    bx::HierarchicalBitmap bitmap;
    constexpr std::array<std::uint16_t, 14> prices{
        0, 1, 62, 63, 64, 65, 4'094, 4'095, 4'096, 4'097, 65'470, 65'471, 65'534, 65'535,
    };
    for (const auto price : prices) {
        bitmap.set(price);
    }
    BX_REQUIRE(bitmap.validate());
    BX_REQUIRE(bitmap.first() == 0);
    BX_REQUIRE(bitmap.last() == 65'535);
    for (std::size_t index = 0; index < prices.size(); ++index) {
        const auto expected_previous = index == 0 ? bx::kNoBid : static_cast<std::int32_t>(prices[index - 1]);
        const auto expected_next = index + 1 == prices.size()
            ? bx::kNoAsk
            : static_cast<std::int32_t>(prices[index + 1]);
        BX_REQUIRE(bitmap.previous(prices[index]) == expected_previous);
        BX_REQUIRE(bitmap.next(prices[index]) == expected_next);
    }

    for (const auto price : prices) {
        bitmap.reset(price);
        BX_REQUIRE(bitmap.validate());
    }
    BX_REQUIRE(bitmap.empty());
}

void bitmap_full_domain_and_queries() {
    bx::HierarchicalBitmap bitmap;
    for (std::uint32_t value = 0; value < bx::kPriceCount; ++value) {
        bitmap.set(static_cast<std::uint16_t>(value));
    }
    BX_REQUIRE(bitmap.validate());
    BX_REQUIRE(bitmap.first() == 0);
    BX_REQUIRE(bitmap.last() == 65'535);
    for (std::uint32_t value = 0; value < bx::kPriceCount; ++value) {
        const auto price = static_cast<std::uint16_t>(value);
        const auto expected_previous = value == 0 ? bx::kNoBid : static_cast<std::int32_t>(value - 1);
        const auto expected_next = value == 65'535 ? bx::kNoAsk : static_cast<std::int32_t>(value + 1);
        BX_REQUIRE(bitmap.previous(price) == expected_previous);
        BX_REQUIRE(bitmap.next(price) == expected_next);
    }
    for (std::uint32_t value = 0; value < bx::kPriceCount; value += 2) {
        bitmap.reset(static_cast<std::uint16_t>(value));
    }
    BX_REQUIRE(bitmap.validate());
    BX_REQUIRE(bitmap.first() == 1);
    BX_REQUIRE(bitmap.last() == 65'535);
}

void bitmap_random_differential() {
    bx::HierarchicalBitmap bitmap;
    std::set<std::uint16_t> reference;
    bxt::Rng rng(0xa174'5f9c'ca62'7123ULL);

    for (std::size_t operation = 0; operation < 500'000; ++operation) {
        const auto random = rng.next();
        const auto price = static_cast<std::uint16_t>(random);
        if (((random >> 16) & 1U) == 0) {
            bitmap.set(price);
            reference.insert(price);
        } else {
            bitmap.reset(price);
            reference.erase(price);
        }

        if ((operation & 255U) == 255U) {
            BX_REQUIRE(bitmap.validate());
            BX_REQUIRE(bitmap.empty() == reference.empty());
            const auto expected_first = reference.empty() ? bx::kNoAsk : static_cast<std::int32_t>(*reference.begin());
            const auto expected_last = reference.empty() ? bx::kNoBid : static_cast<std::int32_t>(*reference.rbegin());
            BX_REQUIRE(bitmap.first() == expected_first);
            BX_REQUIRE(bitmap.last() == expected_last);

            for (std::size_t query_index = 0; query_index < 64; ++query_index) {
                const auto query = static_cast<std::uint16_t>(rng.next());
                const auto next_iterator = reference.upper_bound(query);
                const auto expected_next = next_iterator == reference.end()
                    ? bx::kNoAsk
                    : static_cast<std::int32_t>(*next_iterator);
                const auto lower = reference.lower_bound(query);
                const auto expected_previous = lower == reference.begin()
                    ? bx::kNoBid
                    : static_cast<std::int32_t>(*std::prev(lower));
                BX_REQUIRE(bitmap.next(query) == expected_next);
                BX_REQUIRE(bitmap.previous(query) == expected_previous);
            }
        }
    }
}

void l2_empty_boundaries_and_totals() {
    bx::L2Book book;
    BX_REQUIRE(book.best_bid() == bx::kNoBid);
    BX_REQUIRE(book.best_ask() == bx::kNoAsk);
    BX_REQUIRE(book.total_quantity(bx::Side::Bid) == 0);
    BX_REQUIRE(book.total_quantity(bx::Side::Ask) == 0);
    BX_REQUIRE(book.validate());

    book.set_level(bx::Side::Bid, 0, std::numeric_limits<std::uint32_t>::max());
    book.set_level(bx::Side::Bid, 65'535, 7);
    book.set_level(bx::Side::Ask, 0, 11);
    book.set_level(bx::Side::Ask, 65'535, std::numeric_limits<std::uint32_t>::max());
    BX_REQUIRE(book.best_bid() == 65'535);
    BX_REQUIRE(book.best_ask() == 0);
    BX_REQUIRE(book.total_quantity(bx::Side::Bid)
        == static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 7);
    BX_REQUIRE(book.total_quantity(bx::Side::Ask)
        == static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 11);
    BX_REQUIRE(book.validate());

    book.set_level(bx::Side::Bid, 65'535, 0);
    book.set_level(bx::Side::Ask, 0, 0);
    BX_REQUIRE(book.best_bid() == 0);
    BX_REQUIRE(book.best_ask() == 65'535);
    book.clear();
    BX_REQUIRE(book.validate());
}

void l2_top_order_and_limit() {
    bx::L2Book book;
    // Literals written at their own width. Left as plain `int`, the conversion
    // inside `std::pair`'s constructor is one GCC and Clang accept silently and
    // MSVC reports -- and this build treats warnings as errors, so the C++ port
    // did not compile on MSVC at all until these were typed.
    for (const auto& [price, quantity] : std::array<std::pair<std::uint16_t, std::uint32_t>, 4>{
             {{std::uint16_t{100}, std::uint32_t{1}},
              {std::uint16_t{90}, std::uint32_t{2}},
              {std::uint16_t{110}, std::uint32_t{3}},
              {std::uint16_t{95}, std::uint32_t{4}}}}) {
        book.set_level(bx::Side::Bid, price, quantity);
        book.set_level(bx::Side::Ask, price, quantity + 10U);
    }

    std::vector<std::uint16_t> bids;
    BX_REQUIRE(book.for_each_top(bx::Side::Bid, 0, [&](const auto, const auto) {}) == 0);
    BX_REQUIRE(book.for_each_top(bx::Side::Bid, 3, [&](const auto price, const auto) {
        bids.push_back(price);
    }) == 3);
    BX_REQUIRE((bids == std::vector<std::uint16_t>{110, 100, 95}));

    std::vector<std::uint16_t> asks;
    BX_REQUIRE(book.for_each_top(bx::Side::Ask, 10, [&](const auto price, const auto) {
        asks.push_back(price);
    }) == 4);
    BX_REQUIRE((asks == std::vector<std::uint16_t>{90, 95, 100, 110}));
}

void l2_sweep_zero_partial_exact_and_shortfall() {
    bx::L2Book book;
    book.set_level(bx::Side::Ask, 100, 10);
    book.set_level(bx::Side::Ask, 105, 20);
    book.set_level(bx::Side::Ask, 200, 30);

    const auto zero = book.sweep(bx::Side::Ask, 0);
    BX_REQUIRE(zero.filled_quantity == 0);
    BX_REQUIRE(zero.levels_visited == 0);

    const auto partial = book.sweep(bx::Side::Ask, 5);
    BX_REQUIRE(partial.filled_quantity == 5);
    BX_REQUIRE(partial.notional_ticks == 500);
    BX_REQUIRE(partial.levels_visited == 1);

    const auto exact = book.sweep(bx::Side::Ask, 30);
    BX_REQUIRE(exact.filled_quantity == 30);
    BX_REQUIRE(exact.notional_ticks == 3'100);
    BX_REQUIRE(exact.levels_visited == 2);

    const auto shortfall = book.sweep(bx::Side::Ask, 100);
    BX_REQUIRE(shortfall.filled_quantity == 60);
    BX_REQUIRE(shortfall.notional_ticks == 9'100);
    BX_REQUIRE(shortfall.levels_visited == 3);
}

void l2_sparse_top_1000() {
    bx::L2Book book;
    const auto bids = sparse_prices(512, 32'000, 1'000);
    const auto asks = sparse_prices(33'536, 65'000, 1'000);
    for (std::size_t index = 0; index < 1'000; ++index) {
        book.set_level(bx::Side::Bid, bids[index], static_cast<std::uint32_t>(index + 1));
        book.set_level(bx::Side::Ask, asks[index], static_cast<std::uint32_t>(index + 1));
    }

    std::vector<std::uint16_t> actual_bids;
    std::vector<std::uint16_t> actual_asks;
    BX_REQUIRE(book.for_each_top(bx::Side::Bid, 1'000, [&](const auto price, const auto) {
        actual_bids.push_back(price);
    }) == 1'000);
    BX_REQUIRE(book.for_each_top(bx::Side::Ask, 1'000, [&](const auto price, const auto) {
        actual_asks.push_back(price);
    }) == 1'000);

    auto expected_bids = bids;
    std::reverse(expected_bids.begin(), expected_bids.end());
    BX_REQUIRE(actual_bids == expected_bids);
    BX_REQUIRE(actual_asks == asks);

    const auto ask_sweep = book.sweep(bx::Side::Ask, book.total_quantity(bx::Side::Ask));
    BX_REQUIRE(ask_sweep.filled_quantity == book.total_quantity(bx::Side::Ask));
    BX_REQUIRE(ask_sweep.levels_visited == 1'000);
    BX_REQUIRE(book.validate());
}

void l2_random_differential() {
    bx::L2Book fast;
    bxt::ReferenceL2 reference;
    bxt::Rng rng(0x54dd'19a6'e87f'1201ULL);

    for (std::size_t operation = 0; operation < 600'000; ++operation) {
        const auto random = rng.next();
        const auto side = ((random >> 8) & 1U) == 0 ? bx::Side::Bid : bx::Side::Ask;
        const auto price = static_cast<std::uint16_t>(random >> 16);
        const auto quantity = ((random >> 32) % 7) == 0
            ? 0U
            : static_cast<std::uint32_t>(1 + ((random >> 40) % 1'000'000));
        fast.set_level(side, price, quantity);
        reference.set_level(side, price, quantity);

        if ((operation & 2047U) == 2047U) {
            BX_REQUIRE(fast.validate());
            BX_REQUIRE(fast.best_bid() == reference.best_bid());
            BX_REQUIRE(fast.best_ask() == reference.best_ask());
            BX_REQUIRE(fast.total_quantity(bx::Side::Bid) == reference.total_quantity(bx::Side::Bid));
            BX_REQUIRE(fast.total_quantity(bx::Side::Ask) == reference.total_quantity(bx::Side::Ask));
            BX_REQUIRE(fast.state_hash() == reference.state_hash());

            for (const auto query_side : {bx::Side::Bid, bx::Side::Ask}) {
                std::vector<std::pair<std::uint16_t, std::uint32_t>> actual;
                fast.for_each_top(query_side, 1'000, [&](const auto level_price, const auto level_quantity) {
                    actual.emplace_back(level_price, level_quantity);
                });
                BX_REQUIRE(actual == reference.top(query_side, 1'000));
                const auto target = rng.next() % (reference.total_quantity(query_side) + 1);
                const auto left = fast.sweep(query_side, target);
                const auto right = reference.sweep(query_side, target);
                BX_REQUIRE(left.requested_quantity == right.requested_quantity);
                BX_REQUIRE(left.filled_quantity == right.filled_quantity);
                BX_REQUIRE(left.notional_ticks == right.notional_ticks);
                BX_REQUIRE(left.levels_visited == right.levels_visited);
            }
        }
    }
}

} // namespace

int main() {
    bxt::Runner runner;
    runner.run("bitmap empty state and idempotence", bitmap_empty_and_idempotence);
    runner.run("bitmap every singleton price", bitmap_all_singletons);
    runner.run("bitmap word and hierarchy boundaries", bitmap_hierarchy_boundaries);
    runner.run("bitmap full-domain next/previous", bitmap_full_domain_and_queries);
    runner.run("bitmap randomized differential model", bitmap_random_differential);
    runner.run("L2 empty, boundary prices, and 64-bit totals", l2_empty_boundaries_and_totals);
    runner.run("L2 top-N ordering and limits", l2_top_order_and_limit);
    runner.run("L2 sweep zero, partial, exact, and shortfall", l2_sweep_zero_partial_exact_and_shortfall);
    runner.run("L2 sparse top-1000 traversal and VWAP", l2_sparse_top_1000);
    runner.run("L2 randomized differential model", l2_random_differential);
    return runner.finish();
}

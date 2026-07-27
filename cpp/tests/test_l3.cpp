#include "bitmap_exchange.hpp"
#include "test_support.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace bx = bitmap_exchange;
namespace bxt = bitmap_exchange::test;

namespace {

[[nodiscard]] std::vector<bx::OrderView> queue(
    const bx::L3Book& book,
    const bx::Side side,
    const std::uint16_t price) {
    std::vector<bx::OrderView> result;
    book.for_each_order_at_level(side, price, [&](const bx::OrderView order) {
        result.push_back(order);
    });
    return result;
}

void require_queue(
    const bx::L3Book& book,
    const bx::Side side,
    const std::uint16_t price,
    const std::initializer_list<std::pair<std::uint32_t, std::uint32_t>> expected) {
    const auto actual = queue(book, side, price);
    BX_REQUIRE(actual.size() == expected.size());
    auto actual_iterator = actual.begin();
    for (const auto& [id, quantity] : expected) {
        BX_REQUIRE(actual_iterator->id == id);
        BX_REQUIRE(actual_iterator->side == side);
        BX_REQUIRE(actual_iterator->price == price);
        BX_REQUIRE(actual_iterator->quantity == quantity);
        ++actual_iterator;
    }
}

void compact_layout_and_empty_state() {
    static_assert(sizeof(bx::OrderSlot) == 24);
    static_assert(sizeof(bx::PriceLevel) == 16);

    bx::L3Book book(0, 0);
    BX_REQUIRE(book.capacity() == 0);
    BX_REQUIRE(book.order_id_capacity() == 0);
    BX_REQUIRE(book.live_orders() == 0);
    BX_REQUIRE(book.best_bid() == bx::kNoBid);
    BX_REQUIRE(book.best_ask() == bx::kNoAsk);
    BX_REQUIRE(book.validate());
    BX_REQUIRE(book.add_resting(0, bx::Side::Bid, 100, 1) == bx::RejectReason::OrderIdOutOfRange);
}


void invalid_capacity_is_rejected_before_allocation() {
    bool rejected = false;
    try {
        const bx::L3Book book(static_cast<std::size_t>(bx::kInvalidIndex), 1);
        (void)book;
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    BX_REQUIRE(rejected);
}

void zero_capacity_and_empty_book_time_in_force() {
    bx::L3Book book(0, 8);
    const auto before = book.state_hash();

    const auto gtc = book.submit_limit(1, bx::Side::Bid, 100, 7, bx::TimeInForce::GoodTillCancel);
    BX_REQUIRE(gtc.reject_reason == bx::RejectReason::CapacityExceeded);
    const auto post_only = book.submit_limit(1, bx::Side::Bid, 100, 7, bx::TimeInForce::PostOnly);
    BX_REQUIRE(post_only.reject_reason == bx::RejectReason::CapacityExceeded);

    const auto ioc = book.submit_limit(1, bx::Side::Bid, 100, 7, bx::TimeInForce::ImmediateOrCancel);
    BX_REQUIRE(ioc.accepted());
    BX_REQUIRE(ioc.traded_quantity == 0);
    BX_REQUIRE(ioc.canceled_quantity == 7);

    const auto fok = book.submit_limit(1, bx::Side::Bid, 100, 7, bx::TimeInForce::FillOrKill);
    BX_REQUIRE(fok.reject_reason == bx::RejectReason::InsufficientLiquidity);

    const auto market_ioc = book.submit_market(2, bx::Side::Ask, 9, bx::TimeInForce::ImmediateOrCancel);
    BX_REQUIRE(market_ioc.accepted());
    BX_REQUIRE(market_ioc.canceled_quantity == 9);
    BX_REQUIRE(book.submit_market(2, bx::Side::Ask, 9, bx::TimeInForce::FillOrKill).reject_reason
        == bx::RejectReason::InsufficientLiquidity);
    BX_REQUIRE(book.submit_market(2, bx::Side::Ask, 9, bx::TimeInForce::PostOnly).reject_reason
        == bx::RejectReason::UnsupportedTimeInForce);

    BX_REQUIRE(book.state_hash() == before);
    BX_REQUIRE(book.validate());
}

void limit_price_stops_matching_and_fok_preview() {
    bx::L3Book book(8, 16);
    BX_REQUIRE(bx::succeeded(book.add_resting(1, bx::Side::Ask, 100, 2)));
    BX_REQUIRE(bx::succeeded(book.add_resting(2, bx::Side::Ask, 101, 3)));
    const auto before = book.state_hash();

    const auto rejected = book.submit_limit(10, bx::Side::Bid, 100, 3, bx::TimeInForce::FillOrKill);
    BX_REQUIRE(rejected.reject_reason == bx::RejectReason::InsufficientLiquidity);
    BX_REQUIRE(book.state_hash() == before);

    std::vector<bx::Fill> fills;
    const auto ioc = book.submit_limit(
        10,
        bx::Side::Bid,
        100,
        4,
        bx::TimeInForce::ImmediateOrCancel,
        [&](const bx::Fill fill) { fills.push_back(fill); });
    BX_REQUIRE(ioc.accepted());
    BX_REQUIRE(ioc.traded_quantity == 2);
    BX_REQUIRE(ioc.canceled_quantity == 2);
    BX_REQUIRE((fills == std::vector<bx::Fill>{{1, bx::Side::Ask, 100, 2}}));
    require_queue(book, bx::Side::Ask, 101, {{2, 3}});
    BX_REQUIRE(book.best_ask() == 101);
    BX_REQUIRE(book.validate());
}

void level_iteration_and_order_lookup() {
    bx::L3Book book(16, 32);
    BX_REQUIRE(bx::succeeded(book.add_resting(1, bx::Side::Bid, 98, 1)));
    BX_REQUIRE(bx::succeeded(book.add_resting(2, bx::Side::Bid, 100, 2)));
    BX_REQUIRE(bx::succeeded(book.add_resting(3, bx::Side::Bid, 99, 3)));
    BX_REQUIRE(bx::succeeded(book.add_resting(4, bx::Side::Bid, 100, 4)));
    BX_REQUIRE(bx::succeeded(book.add_resting(5, bx::Side::Ask, 102, 5)));
    BX_REQUIRE(bx::succeeded(book.add_resting(6, bx::Side::Ask, 101, 6)));

    std::vector<std::pair<std::uint16_t, std::uint64_t>> bid_levels;
    BX_REQUIRE(book.for_each_level(bx::Side::Bid, 2, [&](const auto price, const auto quantity) {
        bid_levels.emplace_back(price, quantity);
    }) == 2);
    BX_REQUIRE(
        (bid_levels
         == std::vector<std::pair<std::uint16_t, std::uint64_t>>{
             {std::uint16_t{100}, std::uint64_t{6}}, {std::uint16_t{99}, std::uint64_t{3}}}));

    std::vector<std::uint16_t> ask_prices;
    BX_REQUIRE(book.for_each_level(bx::Side::Ask, 8, [&](const auto price, const auto) {
        ask_prices.push_back(price);
    }) == 2);
    BX_REQUIRE((ask_prices == std::vector<std::uint16_t>{101, 102}));
    BX_REQUIRE(book.for_each_level(bx::Side::Ask, 0, [&](const auto, const auto) {}) == 0);

    const auto order = book.order(4);
    BX_REQUIRE(order.has_value());
    BX_REQUIRE(order->id == 4);
    BX_REQUIRE(order->side == bx::Side::Bid);
    BX_REQUIRE(order->price == 100);
    BX_REQUIRE(order->quantity == 4);
    BX_REQUIRE(!book.order(31).has_value());
    require_queue(book, bx::Side::Bid, 100, {{2, 2}, {4, 4}});
    BX_REQUIRE(book.validate());
}

void replacement_can_cross_then_rest_remainder() {
    bx::L3Book book(2, 16);
    BX_REQUIRE(bx::succeeded(book.add_resting(1, bx::Side::Ask, 100, 5)));
    BX_REQUIRE(bx::succeeded(book.add_resting(2, bx::Side::Bid, 99, 10)));

    std::vector<bx::Fill> fills;
    const auto result = book.replace_order(
        2,
        3,
        100,
        12,
        [&](const bx::Fill fill) { fills.push_back(fill); });
    BX_REQUIRE(result.accepted());
    BX_REQUIRE(result.traded_quantity == 5);
    BX_REQUIRE(result.rested_quantity == 7);
    BX_REQUIRE((fills == std::vector<bx::Fill>{{1, bx::Side::Ask, 100, 5}}));
    BX_REQUIRE(!book.contains(1));
    BX_REQUIRE(!book.contains(2));
    require_queue(book, bx::Side::Bid, 100, {{3, 7}});
    BX_REQUIRE(book.best_bid() == 100);
    BX_REQUIRE(book.best_ask() == bx::kNoAsk);
    BX_REQUIRE(book.validate());
}

void aggregate_quantity_uses_64_bits() {
    bx::L3Book book(4, 8);
    constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
    BX_REQUIRE(bx::succeeded(book.add_resting(1, bx::Side::Ask, 65'535, maximum)));
    BX_REQUIRE(bx::succeeded(book.add_resting(2, bx::Side::Ask, 65'535, maximum)));
    BX_REQUIRE(book.level_quantity(bx::Side::Ask, 65'535) == 2ULL * maximum);

    const auto result = book.submit_market(3, bx::Side::Bid, maximum, bx::TimeInForce::FillOrKill);
    BX_REQUIRE(result.accepted());
    BX_REQUIRE(result.traded_quantity == maximum);
    BX_REQUIRE(result.notional_ticks == static_cast<std::uint64_t>(65'535) * maximum);
    require_queue(book, bx::Side::Ask, 65'535, {{2, maximum}});
    BX_REQUIRE(book.level_quantity(bx::Side::Ask, 65'535) == maximum);
    BX_REQUIRE(book.validate());
}

void market_sell_side_symmetry() {
    bx::L3Book book(8, 16);
    BX_REQUIRE(bx::succeeded(book.add_resting(1, bx::Side::Bid, 10, 2)));
    BX_REQUIRE(bx::succeeded(book.add_resting(2, bx::Side::Bid, 65'535, 3)));
    std::vector<bx::Fill> fills;
    const auto result = book.submit_market(
        10,
        bx::Side::Ask,
        4,
        bx::TimeInForce::ImmediateOrCancel,
        [&](const bx::Fill fill) { fills.push_back(fill); });
    BX_REQUIRE(result.accepted());
    BX_REQUIRE(result.traded_quantity == 4);
    BX_REQUIRE((fills == std::vector<bx::Fill>{
        {2, bx::Side::Bid, 65'535, 3},
        {1, bx::Side::Bid, 10, 1},
    }));
    require_queue(book, bx::Side::Bid, 10, {{1, 1}});
    BX_REQUIRE(book.best_bid() == 10);
    BX_REQUIRE(book.validate());
}

void add_validation_and_cross_prevention() {
    bx::L3Book book(3, 4);
    BX_REQUIRE(book.add_resting(0, bx::Side::Bid, 100, 0) == bx::RejectReason::QuantityZero);
    BX_REQUIRE(book.add_resting(4, bx::Side::Bid, 100, 1) == bx::RejectReason::OrderIdOutOfRange);
    BX_REQUIRE(book.add_resting(0, bx::Side::Bid, 100, 10) == bx::RejectReason::None);
    BX_REQUIRE(book.add_resting(0, bx::Side::Bid, 99, 10) == bx::RejectReason::DuplicateOrderId);
    BX_REQUIRE(book.add_resting(1, bx::Side::Ask, 100, 10) == bx::RejectReason::WouldCross);
    BX_REQUIRE(book.add_resting(1, bx::Side::Ask, 101, 10) == bx::RejectReason::None);
    BX_REQUIRE(book.add_resting(2, bx::Side::Bid, 99, 10) == bx::RejectReason::None);
    BX_REQUIRE(book.add_resting(3, bx::Side::Ask, 102, 10) == bx::RejectReason::CapacityExceeded);
    BX_REQUIRE(book.live_orders() == 3);
    BX_REQUIRE(book.best_bid() == 100);
    BX_REQUIRE(book.best_ask() == 101);
    BX_REQUIRE(book.validate());
}

void boundary_prices_and_best_updates() {
    bx::L3Book book(8, 16);
    BX_REQUIRE(bx::succeeded(book.add_resting(1, bx::Side::Bid, 0, 1)));
    BX_REQUIRE(bx::succeeded(book.add_resting(2, bx::Side::Bid, 65'534, 2)));
    BX_REQUIRE(bx::succeeded(book.add_resting(3, bx::Side::Ask, 65'535, 3)));
    BX_REQUIRE(book.best_bid() == 65'534);
    BX_REQUIRE(book.best_ask() == 65'535);
    BX_REQUIRE(book.cancel(2) == bx::RejectReason::None);
    BX_REQUIRE(book.best_bid() == 0);
    BX_REQUIRE(book.cancel(1) == bx::RejectReason::None);
    BX_REQUIRE(book.best_bid() == bx::kNoBid);
    BX_REQUIRE(book.cancel(3) == bx::RejectReason::None);
    BX_REQUIRE(book.best_ask() == bx::kNoAsk);
    BX_REQUIRE(book.validate());
}

void strict_fifo_and_maker_price() {
    bx::L3Book book(16, 32);
    BX_REQUIRE(bx::succeeded(book.add_resting(1, bx::Side::Ask, 100, 10)));
    BX_REQUIRE(bx::succeeded(book.add_resting(2, bx::Side::Ask, 100, 20)));
    BX_REQUIRE(bx::succeeded(book.add_resting(3, bx::Side::Ask, 100, 30)));

    std::vector<bx::Fill> fills;
    const auto result = book.submit_limit(
        10,
        bx::Side::Bid,
        120,
        35,
        bx::TimeInForce::ImmediateOrCancel,
        [&](const bx::Fill fill) { fills.push_back(fill); });

    BX_REQUIRE(result.accepted());
    BX_REQUIRE(result.fills == 3);
    BX_REQUIRE(result.traded_quantity == 35);
    BX_REQUIRE(result.notional_ticks == 3'500);
    BX_REQUIRE(result.rested_quantity == 0);
    BX_REQUIRE(result.canceled_quantity == 0);
    BX_REQUIRE((fills == std::vector<bx::Fill>{
        {1, bx::Side::Ask, 100, 10},
        {2, bx::Side::Ask, 100, 20},
        {3, bx::Side::Ask, 100, 5},
    }));
    require_queue(book, bx::Side::Ask, 100, {{3, 25}});
    BX_REQUIRE(book.validate());
}

void throwing_fill_callback_preserves_book_invariants() {
    bx::L3Book book(8, 16);
    BX_REQUIRE(bx::succeeded(book.add_resting(1, bx::Side::Ask, 100, 3)));
    BX_REQUIRE(bx::succeeded(book.add_resting(2, bx::Side::Ask, 101, 4)));

    bool callback_threw = false;
    try {
        [[maybe_unused]] const auto result = book.submit_limit(
            10,
            bx::Side::Bid,
            101,
            7,
            bx::TimeInForce::ImmediateOrCancel,
            [](const bx::Fill&) { throw std::runtime_error("test callback failure"); });
    } catch (const std::runtime_error&) {
        callback_threw = true;
    }

    BX_REQUIRE(callback_threw);
    BX_REQUIRE(!book.contains(1));
    BX_REQUIRE(book.contains(2));
    BX_REQUIRE(!book.contains(10));
    BX_REQUIRE(book.live_orders() == 1);
    BX_REQUIRE(book.best_ask() == 101);
    require_queue(book, bx::Side::Ask, 101, {{2, 4}});
    BX_REQUIRE(book.validate());
}

void price_priority_and_multi_level_matching() {
    bx::L3Book book(32, 64);
    BX_REQUIRE(bx::succeeded(book.add_resting(1, bx::Side::Ask, 102, 7)));
    BX_REQUIRE(bx::succeeded(book.add_resting(2, bx::Side::Ask, 100, 5)));
    BX_REQUIRE(bx::succeeded(book.add_resting(3, bx::Side::Ask, 101, 6)));
    BX_REQUIRE(bx::succeeded(book.add_resting(4, bx::Side::Ask, 100, 4)));

    std::vector<bx::Fill> fills;
    const auto result = book.submit_limit(
        20,
        bx::Side::Bid,
        101,
        13,
        bx::TimeInForce::ImmediateOrCancel,
        [&](const bx::Fill fill) { fills.push_back(fill); });

    BX_REQUIRE(result.accepted());
    BX_REQUIRE((fills == std::vector<bx::Fill>{
        {2, bx::Side::Ask, 100, 5},
        {4, bx::Side::Ask, 100, 4},
        {3, bx::Side::Ask, 101, 4},
    }));
    BX_REQUIRE(result.notional_ticks == 5 * 100 + 4 * 100 + 4 * 101);
    require_queue(book, bx::Side::Ask, 101, {{3, 2}});
    require_queue(book, bx::Side::Ask, 102, {{1, 7}});
    BX_REQUIRE(book.best_ask() == 101);
    BX_REQUIRE(book.validate());
}

void sell_side_symmetry() {
    bx::L3Book book(16, 32);
    BX_REQUIRE(bx::succeeded(book.add_resting(1, bx::Side::Bid, 100, 3)));
    BX_REQUIRE(bx::succeeded(book.add_resting(2, bx::Side::Bid, 102, 4)));
    BX_REQUIRE(bx::succeeded(book.add_resting(3, bx::Side::Bid, 101, 5)));

    std::vector<bx::Fill> fills;
    const auto result = book.submit_limit(
        10,
        bx::Side::Ask,
        100,
        8,
        bx::TimeInForce::ImmediateOrCancel,
        [&](const bx::Fill fill) { fills.push_back(fill); });
    BX_REQUIRE(result.accepted());
    BX_REQUIRE((fills == std::vector<bx::Fill>{
        {2, bx::Side::Bid, 102, 4},
        {3, bx::Side::Bid, 101, 4},
    }));
    require_queue(book, bx::Side::Bid, 101, {{3, 1}});
    require_queue(book, bx::Side::Bid, 100, {{1, 3}});
    BX_REQUIRE(book.best_bid() == 101);
    BX_REQUIRE(book.validate());
}

void gtc_rests_and_crosses_then_rests() {
    bx::L3Book book(16, 32);
    const auto passive = book.submit_limit(1, bx::Side::Bid, 99, 10, bx::TimeInForce::GoodTillCancel);
    BX_REQUIRE(passive.accepted());
    BX_REQUIRE(passive.rested_quantity == 10);
    require_queue(book, bx::Side::Bid, 99, {{1, 10}});

    BX_REQUIRE(bx::succeeded(book.add_resting(2, bx::Side::Ask, 101, 5)));
    const auto crossing = book.submit_limit(3, bx::Side::Bid, 101, 12, bx::TimeInForce::GoodTillCancel);
    BX_REQUIRE(crossing.accepted());
    BX_REQUIRE(crossing.traded_quantity == 5);
    BX_REQUIRE(crossing.rested_quantity == 7);
    BX_REQUIRE(!book.contains(2));
    require_queue(book, bx::Side::Bid, 101, {{3, 7}});
    BX_REQUIRE(book.best_bid() == 101);
    BX_REQUIRE(book.best_ask() == bx::kNoAsk);
    BX_REQUIRE(book.validate());
}

void ioc_semantics() {
    bx::L3Book book(8, 16);
    const auto empty = book.submit_limit(1, bx::Side::Bid, 100, 10, bx::TimeInForce::ImmediateOrCancel);
    BX_REQUIRE(empty.accepted());
    BX_REQUIRE(empty.traded_quantity == 0);
    BX_REQUIRE(empty.canceled_quantity == 10);
    BX_REQUIRE(!book.contains(1));

    BX_REQUIRE(bx::succeeded(book.add_resting(2, bx::Side::Ask, 100, 4)));
    const auto partial = book.submit_limit(3, bx::Side::Bid, 100, 10, bx::TimeInForce::ImmediateOrCancel);
    BX_REQUIRE(partial.accepted());
    BX_REQUIRE(partial.traded_quantity == 4);
    BX_REQUIRE(partial.canceled_quantity == 6);
    BX_REQUIRE(book.live_orders() == 0);
    BX_REQUIRE(book.validate());
}

void fok_is_atomic() {
    bx::L3Book book(16, 32);
    BX_REQUIRE(bx::succeeded(book.add_resting(1, bx::Side::Ask, 100, 4)));
    BX_REQUIRE(bx::succeeded(book.add_resting(2, bx::Side::Ask, 101, 6)));
    const auto before = book.state_hash();
    std::vector<bx::Fill> rejected_fills;
    const auto rejected = book.submit_limit(
        10,
        bx::Side::Bid,
        101,
        11,
        bx::TimeInForce::FillOrKill,
        [&](const bx::Fill fill) { rejected_fills.push_back(fill); });
    BX_REQUIRE(!rejected.accepted());
    BX_REQUIRE(rejected.reject_reason == bx::RejectReason::InsufficientLiquidity);
    BX_REQUIRE(rejected_fills.empty());
    BX_REQUIRE(book.state_hash() == before);

    std::vector<bx::Fill> fills;
    const auto filled = book.submit_limit(
        11,
        bx::Side::Bid,
        101,
        10,
        bx::TimeInForce::FillOrKill,
        [&](const bx::Fill fill) { fills.push_back(fill); });
    BX_REQUIRE(filled.accepted());
    BX_REQUIRE(filled.traded_quantity == 10);
    BX_REQUIRE(filled.canceled_quantity == 0);
    BX_REQUIRE(filled.rested_quantity == 0);
    BX_REQUIRE((fills == std::vector<bx::Fill>{
        {1, bx::Side::Ask, 100, 4},
        {2, bx::Side::Ask, 101, 6},
    }));
    BX_REQUIRE(book.live_orders() == 0);
    BX_REQUIRE(book.validate());
}

void post_only_is_atomic() {
    bx::L3Book book(4, 8);
    BX_REQUIRE(bx::succeeded(book.add_resting(1, bx::Side::Ask, 100, 5)));
    const auto before = book.state_hash();
    const auto rejected = book.submit_limit(2, bx::Side::Bid, 100, 3, bx::TimeInForce::PostOnly);
    BX_REQUIRE(!rejected.accepted());
    BX_REQUIRE(rejected.reject_reason == bx::RejectReason::WouldCross);
    BX_REQUIRE(book.state_hash() == before);

    const auto accepted = book.submit_limit(2, bx::Side::Bid, 99, 3, bx::TimeInForce::PostOnly);
    BX_REQUIRE(accepted.accepted());
    BX_REQUIRE(accepted.rested_quantity == 3);
    require_queue(book, bx::Side::Bid, 99, {{2, 3}});
    BX_REQUIRE(book.validate());
}

void cancel_head_middle_tail_and_only_order() {
    bx::L3Book book(16, 32);
    for (std::uint32_t id = 1; id <= 4; ++id) {
        BX_REQUIRE(bx::succeeded(book.add_resting(id, bx::Side::Ask, 100, id * 10)));
    }
    BX_REQUIRE(book.cancel(1) == bx::RejectReason::None);
    require_queue(book, bx::Side::Ask, 100, {{2, 20}, {3, 30}, {4, 40}});
    BX_REQUIRE(book.cancel(3) == bx::RejectReason::None);
    require_queue(book, bx::Side::Ask, 100, {{2, 20}, {4, 40}});
    BX_REQUIRE(book.cancel(4) == bx::RejectReason::None);
    require_queue(book, bx::Side::Ask, 100, {{2, 20}});
    BX_REQUIRE(book.cancel(2) == bx::RejectReason::None);
    BX_REQUIRE(book.best_ask() == bx::kNoAsk);
    BX_REQUIRE(book.cancel(2) == bx::RejectReason::UnknownOrderId);
    BX_REQUIRE(book.cancel(99) == bx::RejectReason::OrderIdOutOfRange);
    BX_REQUIRE(book.validate());
}

void reduce_preserves_priority() {
    bx::L3Book book(16, 32);
    BX_REQUIRE(bx::succeeded(book.add_resting(1, bx::Side::Ask, 100, 10)));
    BX_REQUIRE(bx::succeeded(book.add_resting(2, bx::Side::Ask, 100, 20)));
    BX_REQUIRE(bx::succeeded(book.add_resting(3, bx::Side::Ask, 100, 30)));

    BX_REQUIRE(book.reduce_quantity(2, 15) == bx::RejectReason::None);
    require_queue(book, bx::Side::Ask, 100, {{1, 10}, {2, 15}, {3, 30}});
    BX_REQUIRE(book.level_quantity(bx::Side::Ask, 100) == 55);
    BX_REQUIRE(book.reduce_quantity(2, 15) == bx::RejectReason::None);
    BX_REQUIRE(book.reduce_quantity(2, 16) == bx::RejectReason::QuantityIncreaseNotAllowed);
    BX_REQUIRE(book.reduce_quantity(2, 0) == bx::RejectReason::None);
    require_queue(book, bx::Side::Ask, 100, {{1, 10}, {3, 30}});
    BX_REQUIRE(book.reduce_quantity(2, 0) == bx::RejectReason::UnknownOrderId);
    BX_REQUIRE(book.reduce_quantity(99, 0) == bx::RejectReason::OrderIdOutOfRange);
    BX_REQUIRE(book.validate());
}

void replace_gets_new_id_and_new_priority() {
    bx::L3Book book(16, 64);
    BX_REQUIRE(bx::succeeded(book.add_resting(1, bx::Side::Ask, 100, 10)));
    BX_REQUIRE(bx::succeeded(book.add_resting(2, bx::Side::Ask, 100, 20)));
    BX_REQUIRE(bx::succeeded(book.add_resting(3, bx::Side::Ask, 100, 30)));

    const auto same_price = book.replace_order(2, 20, 100, 25);
    BX_REQUIRE(same_price.accepted());
    BX_REQUIRE(!book.contains(2));
    BX_REQUIRE(book.contains(20));
    require_queue(book, bx::Side::Ask, 100, {{1, 10}, {3, 30}, {20, 25}});

    BX_REQUIRE(bx::succeeded(book.add_resting(4, bx::Side::Bid, 99, 7)));
    std::vector<bx::Fill> fills;
    const auto crossing = book.replace_order(
        4,
        21,
        100,
        12,
        [&](const bx::Fill fill) { fills.push_back(fill); });
    BX_REQUIRE(crossing.accepted());
    BX_REQUIRE(crossing.traded_quantity == 12);
    BX_REQUIRE((fills == std::vector<bx::Fill>{
        {1, bx::Side::Ask, 100, 10},
        {3, bx::Side::Ask, 100, 2},
    }));
    BX_REQUIRE(!book.contains(4));
    BX_REQUIRE(!book.contains(21));
    require_queue(book, bx::Side::Ask, 100, {{3, 28}, {20, 25}});
    BX_REQUIRE(book.validate());
}

void invalid_replace_is_atomic() {
    bx::L3Book book(8, 16);
    BX_REQUIRE(bx::succeeded(book.add_resting(1, bx::Side::Bid, 100, 10)));
    BX_REQUIRE(bx::succeeded(book.add_resting(2, bx::Side::Bid, 100, 20)));
    const auto before = book.state_hash();

    BX_REQUIRE(book.replace_order(9, 3, 101, 1).reject_reason == bx::RejectReason::UnknownOrderId);
    BX_REQUIRE(book.replace_order(1, 1, 101, 1).reject_reason == bx::RejectReason::ReplacementIdMustDiffer);
    BX_REQUIRE(book.replace_order(1, 2, 101, 1).reject_reason == bx::RejectReason::DuplicateOrderId);
    BX_REQUIRE(book.replace_order(1, 20, 101, 1).reject_reason == bx::RejectReason::OrderIdOutOfRange);
    BX_REQUIRE(book.replace_order(1, 3, 101, 0).reject_reason == bx::RejectReason::QuantityZero);
    BX_REQUIRE(book.state_hash() == before);
    require_queue(book, bx::Side::Bid, 100, {{1, 10}, {2, 20}});
    BX_REQUIRE(book.validate());
}

void slot_reuse_and_free_list_integrity() {
    bx::L3Book book(3, 64);
    for (std::uint32_t cycle = 0; cycle < 10'000; ++cycle) {
        const auto base = (cycle % 20) * 3;
        BX_REQUIRE(bx::succeeded(book.add_resting(base, bx::Side::Bid, 99, 1)));
        BX_REQUIRE(bx::succeeded(book.add_resting(base + 1, bx::Side::Ask, 101, 1)));
        BX_REQUIRE(bx::succeeded(book.add_resting(base + 2, bx::Side::Ask, 102, 1)));
        BX_REQUIRE(book.cancel(base + 1) == bx::RejectReason::None);
        BX_REQUIRE(book.cancel(base) == bx::RejectReason::None);
        BX_REQUIRE(book.cancel(base + 2) == bx::RejectReason::None);
        BX_REQUIRE(book.live_orders() == 0);
        BX_REQUIRE(book.validate());
    }
}

void full_capacity_aggressive_orders() {
    {
        bx::L3Book book(1, 8);
        BX_REQUIRE(bx::succeeded(book.add_resting(1, bx::Side::Ask, 100, 10)));
        const auto result = book.submit_limit(2, bx::Side::Bid, 100, 10, bx::TimeInForce::GoodTillCancel);
        BX_REQUIRE(result.accepted());
        BX_REQUIRE(result.traded_quantity == 10);
        BX_REQUIRE(result.rested_quantity == 0);
        BX_REQUIRE(book.live_orders() == 0);
        BX_REQUIRE(book.validate());
    }
    {
        bx::L3Book book(1, 8);
        BX_REQUIRE(bx::succeeded(book.add_resting(1, bx::Side::Ask, 100, 10)));
        const auto result = book.submit_limit(2, bx::Side::Bid, 100, 15, bx::TimeInForce::GoodTillCancel);
        BX_REQUIRE(result.accepted());
        BX_REQUIRE(result.traded_quantity == 10);
        BX_REQUIRE(result.rested_quantity == 5);
        require_queue(book, bx::Side::Bid, 100, {{2, 5}});
        BX_REQUIRE(book.validate());
    }
    {
        bx::L3Book book(1, 8);
        BX_REQUIRE(bx::succeeded(book.add_resting(1, bx::Side::Ask, 101, 10)));
        const auto before = book.state_hash();
        const auto result = book.submit_limit(2, bx::Side::Bid, 100, 5, bx::TimeInForce::GoodTillCancel);
        BX_REQUIRE(!result.accepted());
        BX_REQUIRE(result.reject_reason == bx::RejectReason::CapacityExceeded);
        BX_REQUIRE(book.state_hash() == before);
        BX_REQUIRE(book.validate());
    }
    {
        bx::L3Book book(1, 8);
        BX_REQUIRE(bx::succeeded(book.add_resting(1, bx::Side::Ask, 101, 10)));
        const auto result = book.submit_limit(2, bx::Side::Bid, 100, 5, bx::TimeInForce::ImmediateOrCancel);
        BX_REQUIRE(result.accepted());
        BX_REQUIRE(result.canceled_quantity == 5);
        BX_REQUIRE(book.live_orders() == 1);
        BX_REQUIRE(book.validate());
    }
}

void incoming_validation_is_atomic() {
    bx::L3Book book(4, 8);
    BX_REQUIRE(bx::succeeded(book.add_resting(1, bx::Side::Ask, 100, 10)));
    const auto before = book.state_hash();
    BX_REQUIRE(book.submit_limit(2, bx::Side::Bid, 100, 0).reject_reason == bx::RejectReason::QuantityZero);
    BX_REQUIRE(book.submit_limit(8, bx::Side::Bid, 100, 1).reject_reason == bx::RejectReason::OrderIdOutOfRange);
    BX_REQUIRE(book.submit_limit(1, bx::Side::Bid, 100, 1).reject_reason == bx::RejectReason::DuplicateOrderId);
    const auto invalid_time_in_force = static_cast<bx::TimeInForce>(0xffU);
    BX_REQUIRE(book.submit_limit(2, bx::Side::Bid, 100, 1, invalid_time_in_force).reject_reason
        == bx::RejectReason::UnsupportedTimeInForce);
    BX_REQUIRE(book.state_hash() == before);
    BX_REQUIRE(book.validate());
}

void market_ioc_and_fok() {
    bx::L3Book book(16, 32);
    BX_REQUIRE(bx::succeeded(book.add_resting(1, bx::Side::Ask, 0, 2)));
    BX_REQUIRE(bx::succeeded(book.add_resting(2, bx::Side::Ask, 65'535, 3)));

    const auto invalid = book.submit_market(10, bx::Side::Bid, 1, bx::TimeInForce::GoodTillCancel);
    BX_REQUIRE(invalid.reject_reason == bx::RejectReason::UnsupportedTimeInForce);

    const auto fok_before = book.state_hash();
    const auto fok_reject = book.submit_market(10, bx::Side::Bid, 6, bx::TimeInForce::FillOrKill);
    BX_REQUIRE(fok_reject.reject_reason == bx::RejectReason::InsufficientLiquidity);
    BX_REQUIRE(book.state_hash() == fok_before);

    const auto ioc = book.submit_market(10, bx::Side::Bid, 4, bx::TimeInForce::ImmediateOrCancel);
    BX_REQUIRE(ioc.accepted());
    BX_REQUIRE(ioc.traded_quantity == 4);
    BX_REQUIRE(ioc.notional_ticks == 2ULL * 0 + 2ULL * 65'535);
    require_queue(book, bx::Side::Ask, 65'535, {{2, 1}});
    BX_REQUIRE(book.validate());
}

void maximum_quantity_and_notional_are_safe() {
    bx::L3Book book(4, 8);
    constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
    BX_REQUIRE(bx::succeeded(book.add_resting(1, bx::Side::Ask, 65'535, maximum)));
    const auto result = book.submit_market(2, bx::Side::Bid, maximum, bx::TimeInForce::FillOrKill);
    BX_REQUIRE(result.accepted());
    BX_REQUIRE(result.traded_quantity == maximum);
    BX_REQUIRE(result.notional_ticks == static_cast<std::uint64_t>(65'535) * maximum);
    BX_REQUIRE(book.live_orders() == 0);
    BX_REQUIRE(book.validate());
}

void sparse_1000_level_sweep() {
    bx::L3Book book(1'100, 2'000);
    for (std::uint32_t index = 0; index < 1'000; ++index) {
        const auto price = static_cast<std::uint16_t>(30'000 + index * 31);
        BX_REQUIRE(bx::succeeded(book.add_resting(index + 1, bx::Side::Ask, price, 1)));
    }
    std::vector<bx::Fill> fills;
    const auto result = book.submit_limit(
        1'500,
        bx::Side::Bid,
        65'535,
        1'000,
        bx::TimeInForce::FillOrKill,
        [&](const bx::Fill fill) { fills.push_back(fill); });
    BX_REQUIRE(result.accepted());
    BX_REQUIRE(result.fills == 1'000);
    BX_REQUIRE(result.traded_quantity == 1'000);
    BX_REQUIRE(fills.size() == 1'000);
    for (std::size_t index = 0; index < fills.size(); ++index) {
        BX_REQUIRE(fills[index].maker_order_id == index + 1);
        BX_REQUIRE(fills[index].price == static_cast<std::uint16_t>(30'000 + index * 31));
        BX_REQUIRE(fills[index].quantity == 1);
    }
    BX_REQUIRE(book.live_orders() == 0);
    BX_REQUIRE(book.validate());
}

} // namespace

int main() {
    bxt::Runner runner;
    runner.run("compact L3 layout and empty state", compact_layout_and_empty_state);
    runner.run(
        "invalid slot capacity rejected before allocation",
        invalid_capacity_is_rejected_before_allocation);
    runner.run("zero-capacity and empty-book TIF behavior", zero_capacity_and_empty_book_time_in_force);
    runner.run("resting add validation and crossed-book prevention", add_validation_and_cross_prevention);
    runner.run("boundary prices and best-price updates", boundary_prices_and_best_updates);
    runner.run("strict FIFO and maker-price execution", strict_fifo_and_maker_price);
    runner.run("throwing fill callback preserves invariants", throwing_fill_callback_preserves_book_invariants);
    runner.run("price priority and multi-level matching", price_priority_and_multi_level_matching);
    runner.run("limit-price boundary and FOK preview", limit_price_stops_matching_and_fok_preview);
    runner.run("sell-side matching symmetry", sell_side_symmetry);
    runner.run("market sell-side symmetry", market_sell_side_symmetry);
    runner.run("GTC passive and match-then-rest behavior", gtc_rests_and_crosses_then_rests);
    runner.run("IOC cancellation semantics", ioc_semantics);
    runner.run("FOK atomicity", fok_is_atomic);
    runner.run("post-only atomicity", post_only_is_atomic);
    runner.run("cancel head, middle, tail, and only order", cancel_head_middle_tail_and_only_order);
    runner.run("quantity reduction preserves queue priority", reduce_preserves_priority);
    runner.run("replace uses a new ID and loses priority", replace_gets_new_id_and_new_priority);
    runner.run("replacement crosses then rests remainder", replacement_can_cross_then_rest_remainder);
    runner.run("invalid replacement leaves original untouched", invalid_replace_is_atomic);
    runner.run("slot reuse and free-list integrity", slot_reuse_and_free_list_integrity);
    runner.run("full-capacity aggressive GTC and IOC", full_capacity_aggressive_orders);
    runner.run("incoming-order rejection is atomic", incoming_validation_is_atomic);
    runner.run("market IOC and FOK", market_ioc_and_fok);
    runner.run("maximum quantity and notional safety", maximum_quantity_and_notional_are_safe);
    runner.run("64-bit aggregate level quantity", aggregate_quantity_uses_64_bits);
    runner.run("level iteration and direct order lookup", level_iteration_and_order_lookup);
    runner.run("sparse 1000-level matching sweep", sparse_1000_level_sweep);
    return runner.finish();
}

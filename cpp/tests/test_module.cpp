// The engine reached through `import` instead of `#include`.
//
// Built only when -DBX_MODULE=ON. What this is really testing is the module
// interface itself: whether every name a caller needs is exported, and whether
// those names arrive with enough of their definitions to instantiate templates
// and evaluate constant expressions across the module boundary. A missing
// export or an incomplete type shows up here as a compile error, which is the
// failure mode worth catching -- the behaviour underneath is already covered by
// the three header-based suites.
//
// Deliberately absent: any `#include "bitmap_exchange.hpp"`. If the header
// leaked in, this file would pass without proving anything about the module.

#include "test_support.hpp"

#include <cstdint>
#include <vector>

import bitmap_exchange;

namespace bx = bitmap_exchange;

namespace {

// Constant expressions have to survive the boundary, not just function calls.
static_assert(bx::kPriceCount == (1ULL << 16));
static_assert(bx::side_index(bx::Side::Ask) == 1U);
static_assert(bx::opposite(bx::Side::Bid) == bx::Side::Ask);
static_assert(bx::succeeded(bx::RejectReason::None));

void the_module_exports_a_working_l3_book() {
    bx::L3Book book(64, 256);

    BX_REQUIRE(book.add_resting(1, bx::Side::Bid, 100, 5) == bx::RejectReason::None);
    BX_REQUIRE(book.add_resting(2, bx::Side::Bid, 100, 7) == bx::RejectReason::None);
    BX_REQUIRE(book.best_bid() == 100);
    BX_REQUIRE(book.level_quantity(bx::Side::Bid, 100) == 12U);

    // A fill handler exercises the exported `Fill` as a template argument, which
    // is the case that fails when a type is exported without its definition.
    std::vector<bx::Fill> fills;
    const auto result = book.submit_limit(
        3, bx::Side::Ask, 100, 9, bx::TimeInForce::ImmediateOrCancel,
        [&fills](const bx::Fill& fill) { fills.push_back(fill); });

    BX_REQUIRE(result.accepted());
    BX_REQUIRE(result.traded_quantity == 9U);
    BX_REQUIRE(fills.size() == 2U);
    BX_REQUIRE(fills[0].maker_order_id == 1U); // Price-time priority held.
    BX_REQUIRE(fills[1].maker_order_id == 2U);
    BX_REQUIRE(book.level_quantity(bx::Side::Bid, 100) == 3U);
}

void the_module_exports_a_working_l2_book() {
    bx::L2Book book;

    book.set_level(bx::Side::Bid, 100, 5);
    book.set_level(bx::Side::Bid, 90, 3);
    book.set_level(bx::Side::Ask, 110, 4);

    BX_REQUIRE(book.best_bid() == 100);
    BX_REQUIRE(book.best_ask() == 110);
    BX_REQUIRE(book.validate());

    book.set_level(bx::Side::Bid, 100, 0);
    BX_REQUIRE(book.best_bid() == 90);
    BX_REQUIRE(book.validate());
}

void the_module_exports_the_bitmap() {
    bx::HierarchicalBitmap bitmap;

    BX_REQUIRE(bitmap.first() == bx::kNoAsk);
    bitmap.set(42);
    bitmap.set(9000);
    BX_REQUIRE(bitmap.first() == 42);
    BX_REQUIRE(bitmap.last() == 9000);
    bitmap.reset(42);
    BX_REQUIRE(bitmap.first() == 9000);
}

} // namespace

int main() {
    bitmap_exchange::test::Runner runner;
    runner.run("module exports a working L3 book", the_module_exports_a_working_l3_book);
    runner.run("module exports a working L2 book", the_module_exports_a_working_l2_book);
    runner.run("module exports the hierarchical bitmap", the_module_exports_the_bitmap);
    return runner.finish();
}

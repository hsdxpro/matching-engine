#include "bitmap_exchange.hpp"
#include "reference_model.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace bx = bitmap_exchange;
namespace bxt = bitmap_exchange::test;

namespace {

[[nodiscard]] std::uint32_t random_quantity(const std::uint64_t random) noexcept {
    switch ((random >> 48) & 31U) {
        case 0:
            return 0;
        case 1:
            return std::numeric_limits<std::uint32_t>::max();
        default:
            return static_cast<std::uint32_t>(1 + ((random >> 24) % 10'000));
    }
}

[[nodiscard]] bx::TimeInForce random_tif(const std::uint64_t random) noexcept {
    switch ((random >> 56) & 3U) {
        case 0:
            return bx::TimeInForce::GoodTillCancel;
        case 1:
            return bx::TimeInForce::ImmediateOrCancel;
        case 2:
            return bx::TimeInForce::FillOrKill;
        default:
            return bx::TimeInForce::PostOnly;
    }
}

void require_equivalent(
    const bx::L3Book& fast,
    const bxt::ReferenceL3& reference,
    const std::size_t operation,
    const bool deep_check) {
    BX_REQUIRE_MSG(fast.best_bid() == reference.best_bid(), std::to_string(operation));
    BX_REQUIRE_MSG(fast.best_ask() == reference.best_ask(), std::to_string(operation));
    BX_REQUIRE_MSG(fast.live_orders() == reference.live_orders(), std::to_string(operation));
    if (deep_check) {
        BX_REQUIRE_MSG(fast.validate(), std::to_string(operation));
        BX_REQUIRE_MSG(fast.state_hash() == reference.state_hash(), std::to_string(operation));
        for (const auto side : {bx::Side::Bid, bx::Side::Ask}) {
            std::vector<std::pair<std::uint16_t, std::uint64_t>> fast_levels;
            fast.for_each_level(side, bx::kPriceCount, [&](const auto price, const auto quantity) {
                fast_levels.emplace_back(price, quantity);
            });
            for (const auto& [price, quantity] : fast_levels) {
                BX_REQUIRE_MSG(reference.level_quantity(side, price) == quantity, std::to_string(operation));
                std::vector<bx::OrderView> fast_queue;
                fast.for_each_order_at_level(side, price, [&](const bx::OrderView order) {
                    fast_queue.push_back(order);
                });
                const auto reference_queue = reference.orders_at_level(side, price);
                BX_REQUIRE_MSG(fast_queue.size() == reference_queue.size(), std::to_string(operation));
                for (std::size_t index = 0; index < fast_queue.size(); ++index) {
                    BX_REQUIRE_MSG(fast_queue[index].id == reference_queue[index].id, std::to_string(operation));
                    BX_REQUIRE_MSG(fast_queue[index].side == reference_queue[index].side, std::to_string(operation));
                    BX_REQUIRE_MSG(fast_queue[index].price == reference_queue[index].price, std::to_string(operation));
                    BX_REQUIRE_MSG(fast_queue[index].quantity == reference_queue[index].quantity, std::to_string(operation));
                }
            }
        }
    }
}

void randomized_model_for_seed(
    const std::uint64_t seed,
    const std::size_t capacity,
    const std::size_t order_id_capacity,
    const std::size_t operation_count) {
    bx::L3Book fast(capacity, order_id_capacity);
    bxt::ReferenceL3 reference(capacity, order_id_capacity);
    bxt::Rng rng(seed);

    for (std::size_t operation = 0; operation < operation_count; ++operation) {
        const auto random = rng.next();
        const auto action = static_cast<std::uint32_t>(random % 100);
        const auto id_range = order_id_capacity + 8;
        const auto id = static_cast<std::uint32_t>((random >> 8) % id_range);
        const auto side = ((random >> 20) & 1U) == 0 ? bx::Side::Bid : bx::Side::Ask;
        const auto price = static_cast<std::uint16_t>(random >> 24);
        const auto quantity = random_quantity(random);

        if (action < 22) {
            const auto left = fast.add_resting(id, side, price, quantity);
            const auto right = reference.add_resting(id, side, price, quantity);
            BX_REQUIRE_MSG(left == right, std::to_string(operation));
        } else if (action < 37) {
            const auto left = fast.cancel(id);
            const auto right = reference.cancel(id);
            BX_REQUIRE_MSG(left == right, std::to_string(operation));
        } else if (action < 52) {
            auto new_quantity = quantity;
            if (const auto current = reference.order(id); current.has_value()) {
                switch ((random >> 52) & 3U) {
                    case 0:
                        new_quantity = 0;
                        break;
                    case 1:
                        new_quantity = current->quantity;
                        break;
                    case 2:
                        new_quantity = current->quantity == 1 ? 1 : current->quantity / 2;
                        break;
                    default:
                        new_quantity = current->quantity == std::numeric_limits<std::uint32_t>::max()
                            ? current->quantity
                            : current->quantity + 1;
                        break;
                }
            }
            const auto left = fast.reduce_quantity(id, new_quantity);
            const auto right = reference.reduce_quantity(id, new_quantity);
            BX_REQUIRE_MSG(left == right, std::to_string(operation));
        } else if (action < 67) {
            const auto replacement_id = static_cast<std::uint32_t>((random >> 40) % id_range);
            std::vector<bx::Fill> left_fills;
            std::vector<bx::Fill> right_fills;
            const auto left = fast.replace_order(
                id,
                replacement_id,
                price,
                quantity,
                [&](const bx::Fill fill) { left_fills.push_back(fill); });
            const auto right = reference.replace_order(
                id,
                replacement_id,
                price,
                quantity,
                [&](const bx::Fill fill) { right_fills.push_back(fill); });
            BX_REQUIRE_MSG(bxt::same_result(left, right), std::to_string(operation));
            BX_REQUIRE_MSG(left_fills == right_fills, std::to_string(operation));
        } else {
            std::vector<bx::Fill> left_fills;
            std::vector<bx::Fill> right_fills;
            const auto time_in_force = random_tif(random);
            const auto left = fast.submit_limit(
                id,
                side,
                price,
                quantity,
                time_in_force,
                [&](const bx::Fill fill) { left_fills.push_back(fill); });
            const auto right = reference.submit_limit(
                id,
                side,
                price,
                quantity,
                time_in_force,
                [&](const bx::Fill fill) { right_fills.push_back(fill); });
            BX_REQUIRE_MSG(bxt::same_result(left, right), std::to_string(operation));
            BX_REQUIRE_MSG(left_fills == right_fills, std::to_string(operation));
        }

        require_equivalent(fast, reference, operation, (operation & 511U) == 511U);
    }
    require_equivalent(fast, reference, operation_count, true);
}

void randomized_differential_large_capacity() {
    constexpr std::array<std::uint64_t, 4> seeds{
        0x0ff1'ce42'aa51'9911ULL,
        0x6f02'a911'34c8'd77bULL,
        0x94d0'49bb'1331'11ebULL,
        0xc001'd00d'5eed'1234ULL,
    };
    for (const auto seed : seeds) {
        randomized_model_for_seed(seed, 4'096, 8'192, 250'000);
    }
}

void randomized_differential_capacity_pressure() {
    constexpr std::array<std::pair<std::size_t, std::size_t>, 5> configurations{
        {{0, 8}, {1, 8}, {2, 16}, {7, 32}, {31, 128}},
    };
    std::uint64_t seed = 0x5a17'cafe'1234'9001ULL;
    for (const auto& [capacity, id_capacity] : configurations) {
        randomized_model_for_seed(seed, capacity, id_capacity, 120'000);
        seed = bx::mix64(seed);
    }
}

void exhaustive_fifo_quantities_and_tif() {
    for (std::uint32_t first = 1; first <= 3; ++first) {
        for (std::uint32_t second = 1; second <= 3; ++second) {
            for (std::uint32_t third = 1; third <= 3; ++third) {
                const auto total = first + second + third;
                for (std::uint32_t taker_quantity = 1; taker_quantity <= total + 2; ++taker_quantity) {
                    for (const auto time_in_force : {
                             bx::TimeInForce::GoodTillCancel,
                             bx::TimeInForce::ImmediateOrCancel,
                             bx::TimeInForce::FillOrKill,
                             bx::TimeInForce::PostOnly}) {
                        bx::L3Book fast(8, 16);
                        bxt::ReferenceL3 reference(8, 16);
                        for (const auto& [id, quantity] : std::array<std::pair<std::uint32_t, std::uint32_t>, 3>{
                                 {{1, first}, {2, second}, {3, third}}}) {
                            BX_REQUIRE(fast.add_resting(id, bx::Side::Ask, 100, quantity)
                                == reference.add_resting(id, bx::Side::Ask, 100, quantity));
                        }

                        std::vector<bx::Fill> left_fills;
                        std::vector<bx::Fill> right_fills;
                        const auto left = fast.submit_limit(
                            10,
                            bx::Side::Bid,
                            100,
                            taker_quantity,
                            time_in_force,
                            [&](const bx::Fill fill) { left_fills.push_back(fill); });
                        const auto right = reference.submit_limit(
                            10,
                            bx::Side::Bid,
                            100,
                            taker_quantity,
                            time_in_force,
                            [&](const bx::Fill fill) { right_fills.push_back(fill); });
                        BX_REQUIRE(bxt::same_result(left, right));
                        BX_REQUIRE(left_fills == right_fills);
                        BX_REQUIRE(fast.validate());
                        BX_REQUIRE(fast.state_hash() == reference.state_hash());
                    }
                }
            }
        }
    }
}

struct ReplaySummary final {
    std::uint64_t command_hash{};
    std::uint64_t state_hash{};
    std::uint32_t live_orders{};
};

[[nodiscard]] ReplaySummary deterministic_replay(const std::uint64_t seed) {
    bx::L3Book book(256, 1'024);
    bxt::Rng rng(seed);
    std::uint64_t command_hash = 0;
    for (std::size_t operation = 0; operation < 100'000; ++operation) {
        const auto random = rng.next();
        const auto id = static_cast<std::uint32_t>((random >> 8) % 1'024);
        const auto side = ((random >> 20) & 1U) == 0 ? bx::Side::Bid : bx::Side::Ask;
        const auto price = static_cast<std::uint16_t>(random >> 24);
        const auto quantity = static_cast<std::uint32_t>(1 + ((random >> 40) % 100));
        const auto action = static_cast<std::uint32_t>(random % 4);
        std::uint64_t result_hash = 0;
        switch (action) {
            case 0:
                result_hash = static_cast<std::uint64_t>(book.add_resting(id, side, price, quantity));
                break;
            case 1:
                result_hash = static_cast<std::uint64_t>(book.cancel(id));
                break;
            case 2:
                result_hash = static_cast<std::uint64_t>(book.reduce_quantity(id, quantity));
                break;
            default: {
                const auto result = book.submit_limit(
                    id,
                    side,
                    price,
                    quantity,
                    bx::TimeInForce::ImmediateOrCancel);
                result_hash = result.report_hash
                            ^ (static_cast<std::uint64_t>(result.reject_reason) << 56)
                            ^ result.traded_quantity
                            ^ result.canceled_quantity;
                break;
            }
        }
        command_hash = bx::mix64(command_hash ^ random ^ result_hash ^ operation);
    }
    BX_REQUIRE(book.validate());
    return ReplaySummary{
        .command_hash = command_hash,
        .state_hash = book.state_hash(),
        .live_orders = book.live_orders(),
    };
}

void deterministic_replay_is_reproducible() {
    const auto first = deterministic_replay(0x7af3'0021'9124'beefULL);
    const auto second = deterministic_replay(0x7af3'0021'9124'beefULL);
    BX_REQUIRE(first.command_hash == second.command_hash);
    BX_REQUIRE(first.state_hash == second.state_hash);
    BX_REQUIRE(first.live_orders == second.live_orders);
}

} // namespace

int main() {
    bxt::Runner runner;
    runner.run("randomized differential model: large capacity", randomized_differential_large_capacity);
    runner.run("randomized differential model: capacity pressure", randomized_differential_capacity_pressure);
    runner.run("exhaustive FIFO quantities across all TIF modes", exhaustive_fifo_quantities_and_tif);
    runner.run("deterministic replay reproducibility", deterministic_replay_is_reproducible);
    return runner.finish();
}

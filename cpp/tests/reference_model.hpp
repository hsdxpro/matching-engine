#pragma once

#include "bitmap_exchange.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bitmap_exchange::test {

class ReferenceL2 final {
public:
    void set_level(const Side side, const std::uint16_t price, const std::uint32_t quantity) {
        auto& levels = sides_[side_index(side)];
        if (quantity == 0) {
            levels.erase(price);
        } else {
            levels[price] = quantity;
        }
    }

    [[nodiscard]] std::uint32_t quantity(const Side side, const std::uint16_t price) const noexcept {
        const auto& levels = sides_[side_index(side)];
        const auto found = levels.find(price);
        return found == levels.end() ? 0U : found->second;
    }

    [[nodiscard]] std::uint64_t total_quantity(const Side side) const noexcept {
        std::uint64_t total = 0;
        for (const auto& [price, quantity] : sides_[side_index(side)]) {
            (void)price;
            total += quantity;
        }
        return total;
    }

    [[nodiscard]] std::int32_t best_bid() const noexcept {
        return sides_[0].empty() ? kNoBid : sides_[0].rbegin()->first;
    }

    [[nodiscard]] std::int32_t best_ask() const noexcept {
        return sides_[1].empty() ? kNoAsk : sides_[1].begin()->first;
    }

    [[nodiscard]] std::vector<std::pair<std::uint16_t, std::uint32_t>> top(
        const Side side,
        const std::size_t limit) const {
        std::vector<std::pair<std::uint16_t, std::uint32_t>> result;
        result.reserve(std::min(limit, sides_[side_index(side)].size()));
        const auto& levels = sides_[side_index(side)];
        if (side == Side::Bid) {
            for (auto iterator = levels.rbegin(); iterator != levels.rend() && result.size() < limit; ++iterator) {
                result.push_back(*iterator);
            }
        } else {
            for (auto iterator = levels.begin(); iterator != levels.end() && result.size() < limit; ++iterator) {
                result.push_back(*iterator);
            }
        }
        return result;
    }

    [[nodiscard]] SweepResult sweep(const Side side, const std::uint64_t requested_quantity) const noexcept {
        SweepResult result{.requested_quantity = requested_quantity};
        // `consume` runs as the loop condition, so without this guard a
        // zero-quantity sweep would still count the first level as visited,
        // while the engine reports zero levels visited.
        if (requested_quantity == 0) {
            return result;
        }
        auto remaining = requested_quantity;
        auto consume = [&](const auto& level) {
            const auto fill = std::min(remaining, static_cast<std::uint64_t>(level.second));
            remaining -= fill;
            result.filled_quantity += fill;
            result.notional_ticks += fill * level.first;
            ++result.levels_visited;
            return remaining != 0;
        };

        const auto& levels = sides_[side_index(side)];
        if (side == Side::Bid) {
            for (auto iterator = levels.rbegin(); iterator != levels.rend() && consume(*iterator); ++iterator) {}
        } else {
            for (auto iterator = levels.begin(); iterator != levels.end() && consume(*iterator); ++iterator) {}
        }
        return result;
    }

    [[nodiscard]] std::uint64_t state_hash() const noexcept {
        std::uint64_t hash = mix64(static_cast<std::uint64_t>(best_bid() + 1))
                           ^ mix64(static_cast<std::uint64_t>(best_ask() + 1));
        for (std::size_t side = 0; side < 2; ++side) {
            for (const auto& [price, quantity] : sides_[side]) {
                hash = mix64(hash ^ (static_cast<std::uint64_t>(side) << 63)
                                  ^ (static_cast<std::uint64_t>(price) << 32)
                                  ^ quantity);
            }
        }
        return hash;
    }

private:
    std::array<std::map<std::uint16_t, std::uint32_t>, 2> sides_;
};

class ReferenceL3 final {
public:
    explicit ReferenceL3(const std::size_t max_orders, const std::size_t max_order_ids)
        : max_orders_(max_orders), max_order_ids_(max_order_ids) {}

    [[nodiscard]] std::uint32_t live_orders() const noexcept {
        return static_cast<std::uint32_t>(locations_.size());
    }

    [[nodiscard]] bool contains(const std::uint32_t id) const noexcept {
        return locations_.contains(id);
    }

    [[nodiscard]] std::optional<OrderView> order(const std::uint32_t id) const noexcept {
        const auto found = locations_.find(id);
        if (found == locations_.end()) {
            return std::nullopt;
        }
        return OrderView{
            .id = id,
            .side = found->second.side,
            .price = found->second.price,
            .quantity = found->second.iterator->quantity,
        };
    }

    [[nodiscard]] std::int32_t best_bid() const noexcept {
        return levels_[0].empty() ? kNoBid : levels_[0].rbegin()->first;
    }

    [[nodiscard]] std::int32_t best_ask() const noexcept {
        return levels_[1].empty() ? kNoAsk : levels_[1].begin()->first;
    }

    [[nodiscard]] bool would_cross(const Side side, const std::uint16_t price) const noexcept {
        return side == Side::Bid
            ? best_ask() <= static_cast<std::int32_t>(price)
            : best_bid() >= static_cast<std::int32_t>(price);
    }

    [[nodiscard]] std::uint64_t level_quantity(const Side side, const std::uint16_t price) const noexcept {
        const auto& side_levels = levels_[side_index(side)];
        const auto found = side_levels.find(price);
        if (found == side_levels.end()) {
            return 0;
        }
        std::uint64_t total = 0;
        for (const auto& order_value : found->second) {
            total += order_value.quantity;
        }
        return total;
    }

    [[nodiscard]] RejectReason add_resting(
        const std::uint32_t id,
        const Side side,
        const std::uint16_t price,
        const std::uint32_t quantity) {
        if (const auto validation = validate_new_order(id, quantity); validation != RejectReason::None) {
            return validation;
        }
        if (would_cross(side, price)) {
            return RejectReason::WouldCross;
        }
        if (locations_.size() >= max_orders_) {
            return RejectReason::CapacityExceeded;
        }
        append(id, side, price, quantity);
        return RejectReason::None;
    }

    [[nodiscard]] RejectReason cancel(const std::uint32_t id) {
        const auto found = locations_.find(id);
        if (id >= max_order_ids_) {
            return RejectReason::OrderIdOutOfRange;
        }
        if (found == locations_.end()) {
            return RejectReason::UnknownOrderId;
        }
        erase(found);
        return RejectReason::None;
    }

    [[nodiscard]] RejectReason reduce_quantity(const std::uint32_t id, const std::uint32_t new_quantity) {
        if (id >= max_order_ids_) {
            return RejectReason::OrderIdOutOfRange;
        }
        const auto found = locations_.find(id);
        if (found == locations_.end()) {
            return RejectReason::UnknownOrderId;
        }
        auto& quantity = found->second.iterator->quantity;
        if (new_quantity > quantity) {
            return RejectReason::QuantityIncreaseNotAllowed;
        }
        if (new_quantity == 0) {
            erase(found);
            return RejectReason::None;
        }
        quantity = new_quantity;
        return RejectReason::None;
    }

    template <class FillHandler>
    [[nodiscard]] SubmitResult replace_order(
        const std::uint32_t existing_id,
        const std::uint32_t replacement_id,
        const std::uint16_t new_price,
        const std::uint32_t new_quantity,
        FillHandler&& on_fill) {
        if (existing_id >= max_order_ids_) {
            return rejected(RejectReason::OrderIdOutOfRange);
        }
        const auto found = locations_.find(existing_id);
        if (found == locations_.end()) {
            return rejected(RejectReason::UnknownOrderId);
        }
        if (replacement_id == existing_id) {
            return rejected(RejectReason::ReplacementIdMustDiffer);
        }
        if (const auto validation = validate_new_order(replacement_id, new_quantity);
            validation != RejectReason::None) {
            return rejected(validation);
        }
        const auto side = found->second.side;
        erase(found);
        return submit_limit_impl(
            replacement_id,
            side,
            new_price,
            new_quantity,
            TimeInForce::GoodTillCancel,
            std::forward<FillHandler>(on_fill),
            true);
    }

    [[nodiscard]] SubmitResult replace_order(
        const std::uint32_t existing_id,
        const std::uint32_t replacement_id,
        const std::uint16_t new_price,
        const std::uint32_t new_quantity) {
        return replace_order(existing_id, replacement_id, new_price, new_quantity, [](const Fill&) {});
    }

    template <class FillHandler>
    [[nodiscard]] SubmitResult submit_limit(
        const std::uint32_t id,
        const Side side,
        const std::uint16_t limit_price,
        const std::uint32_t quantity,
        const TimeInForce time_in_force,
        FillHandler&& on_fill) {
        return submit_limit_impl(
            id,
            side,
            limit_price,
            quantity,
            time_in_force,
            std::forward<FillHandler>(on_fill),
            false);
    }

    [[nodiscard]] SubmitResult submit_limit(
        const std::uint32_t id,
        const Side side,
        const std::uint16_t limit_price,
        const std::uint32_t quantity,
        const TimeInForce time_in_force = TimeInForce::GoodTillCancel) {
        return submit_limit(id, side, limit_price, quantity, time_in_force, [](const Fill&) {});
    }

    template <class FillHandler>
    [[nodiscard]] SubmitResult submit_market(
        const std::uint32_t id,
        const Side side,
        const std::uint32_t quantity,
        const TimeInForce time_in_force,
        FillHandler&& on_fill) {
        if (time_in_force != TimeInForce::ImmediateOrCancel
            && time_in_force != TimeInForce::FillOrKill) {
            return rejected(RejectReason::UnsupportedTimeInForce);
        }
        const auto limit = side == Side::Bid ? std::uint16_t{65'535} : std::uint16_t{0};
        return submit_limit(id, side, limit, quantity, time_in_force, std::forward<FillHandler>(on_fill));
    }

    [[nodiscard]] SubmitResult submit_market(
        const std::uint32_t id,
        const Side side,
        const std::uint32_t quantity,
        const TimeInForce time_in_force = TimeInForce::ImmediateOrCancel) {
        return submit_market(id, side, quantity, time_in_force, [](const Fill&) {});
    }

    [[nodiscard]] std::vector<OrderView> orders_at_level(const Side side, const std::uint16_t price) const {
        std::vector<OrderView> result;
        const auto found = levels_[side_index(side)].find(price);
        if (found == levels_[side_index(side)].end()) {
            return result;
        }
        result.reserve(found->second.size());
        for (const auto& order_value : found->second) {
            result.push_back(OrderView{
                .id = order_value.id,
                .side = side,
                .price = price,
                .quantity = order_value.quantity,
            });
        }
        return result;
    }

    [[nodiscard]] std::uint64_t state_hash() const noexcept {
        std::uint64_t hash = mix64(locations_.size() + 7);
        for (std::size_t side = 0; side < 2; ++side) {
            for (const auto& [price, queue] : levels_[side]) {
                std::uint64_t rank = 1;
                for (const auto& order_value : queue) {
                    hash = mix64(hash
                        ^ (static_cast<std::uint64_t>(side) << 63)
                        ^ (static_cast<std::uint64_t>(price) << 40)
                        ^ (static_cast<std::uint64_t>(order_value.id) << 8)
                        ^ order_value.quantity
                        ^ rank++);
                }
            }
        }
        return hash;
    }

private:
    struct RefOrder final {
        std::uint32_t id{};
        std::uint32_t quantity{};
    };
    using Queue = std::list<RefOrder>;
    using LevelMap = std::map<std::uint16_t, Queue>;

    struct Location final {
        Side side{};
        std::uint16_t price{};
        Queue::iterator iterator{};
    };

    struct LiquidityPreview final {
        std::uint32_t executable_quantity{};
        bool releases_slot{};
    };

    [[nodiscard]] static SubmitResult rejected(const RejectReason reason) noexcept {
        return SubmitResult{.reject_reason = reason};
    }

    [[nodiscard]] RejectReason validate_new_order(
        const std::uint32_t id,
        const std::uint32_t quantity) const noexcept {
        if (quantity == 0) {
            return RejectReason::QuantityZero;
        }
        if (id >= max_order_ids_) {
            return RejectReason::OrderIdOutOfRange;
        }
        if (locations_.contains(id)) {
            return RejectReason::DuplicateOrderId;
        }
        return RejectReason::None;
    }

    [[nodiscard]] static bool crosses(
        const Side taker_side,
        const std::uint16_t limit_price,
        const std::int32_t maker_price) noexcept {
        return taker_side == Side::Bid
            ? maker_price <= static_cast<std::int32_t>(limit_price)
            : maker_price >= static_cast<std::int32_t>(limit_price);
    }

    [[nodiscard]] LiquidityPreview preview_liquidity(
        const Side taker_side,
        const std::uint16_t limit_price,
        const std::uint32_t requested_quantity) const noexcept {
        LiquidityPreview preview{};
        auto remaining = requested_quantity;
        const auto& maker_levels = levels_[side_index(opposite(taker_side))];

        auto consume_level = [&](const auto& level) {
            for (const auto& maker : level.second) {
                if (remaining == 0) {
                    break;
                }
                const auto fill = std::min(remaining, maker.quantity);
                remaining -= fill;
                preview.executable_quantity += fill;
                preview.releases_slot = preview.releases_slot || fill == maker.quantity;
            }
            return remaining != 0;
        };

        if (taker_side == Side::Bid) {
            for (auto iterator = maker_levels.begin();
                 iterator != maker_levels.end()
                 && iterator->first <= limit_price
                 && consume_level(*iterator);
                 ++iterator) {}
        } else {
            for (auto iterator = maker_levels.rbegin();
                 iterator != maker_levels.rend()
                 && iterator->first >= limit_price
                 && consume_level(*iterator);
                 ++iterator) {}
        }
        return preview;
    }

    template <class FillHandler>
    [[nodiscard]] SubmitResult submit_limit_impl(
        const std::uint32_t id,
        const Side side,
        const std::uint16_t limit_price,
        const std::uint32_t quantity,
        const TimeInForce time_in_force,
        FillHandler&& on_fill,
        const bool validation_already_performed) {
        if (!valid_time_in_force(time_in_force)) {
            return rejected(RejectReason::UnsupportedTimeInForce);
        }
        if (!validation_already_performed) {
            if (const auto validation = validate_new_order(id, quantity); validation != RejectReason::None) {
                return rejected(validation);
            }
        }

        if (time_in_force == TimeInForce::PostOnly) {
            if (would_cross(side, limit_price)) {
                return rejected(RejectReason::WouldCross);
            }
            if (locations_.size() >= max_orders_) {
                return rejected(RejectReason::CapacityExceeded);
            }
            append(id, side, limit_price, quantity);
            return SubmitResult{.rested_quantity = quantity};
        }

        const auto preview = (time_in_force == TimeInForce::FillOrKill
                              || (time_in_force == TimeInForce::GoodTillCancel
                                  && locations_.size() >= max_orders_))
            ? preview_liquidity(side, limit_price, quantity)
            : LiquidityPreview{};

        if (time_in_force == TimeInForce::FillOrKill && preview.executable_quantity < quantity) {
            return rejected(RejectReason::InsufficientLiquidity);
        }
        if (time_in_force == TimeInForce::GoodTillCancel && locations_.size() >= max_orders_) {
            const auto remainder = quantity - preview.executable_quantity;
            if (remainder != 0 && !preview.releases_slot) {
                return rejected(RejectReason::CapacityExceeded);
            }
        }

        SubmitResult result{};
        auto remaining = quantity;
        const auto maker_side = opposite(side);

        while (remaining != 0) {
            const auto maker_price_value = side == Side::Bid ? best_ask() : best_bid();
            if (maker_price_value < 0
                || maker_price_value >= static_cast<std::int32_t>(kPriceCount)
                || !crosses(side, limit_price, maker_price_value)) {
                break;
            }

            const auto maker_price = static_cast<std::uint16_t>(maker_price_value);
            auto& queue = levels_[side_index(maker_side)].at(maker_price);
            auto maker = queue.begin();
            const auto fill_quantity = std::min(remaining, maker->quantity);
            const Fill fill{
                .maker_order_id = maker->id,
                .maker_side = maker_side,
                .price = maker_price,
                .quantity = fill_quantity,
            };

            remaining -= fill_quantity;
            ++result.fills;
            result.traded_quantity += fill_quantity;
            result.notional_ticks += static_cast<std::uint64_t>(maker_price) * fill_quantity;
            result.report_hash = mix64(result.report_hash
                ^ static_cast<std::uint64_t>(maker->id)
                ^ (static_cast<std::uint64_t>(maker_price) << 32)
                ^ (static_cast<std::uint64_t>(fill_quantity) << 1)
                ^ static_cast<std::uint64_t>(side_index(maker_side)));
            on_fill(fill);

            if (fill_quantity == maker->quantity) {
                locations_.erase(maker->id);
                queue.erase(maker);
                if (queue.empty()) {
                    levels_[side_index(maker_side)].erase(maker_price);
                }
            } else {
                maker->quantity -= fill_quantity;
            }
        }

        if (remaining != 0) {
            if (time_in_force == TimeInForce::GoodTillCancel) {
                append(id, side, limit_price, remaining);
                result.rested_quantity = remaining;
            } else {
                result.canceled_quantity = remaining;
            }
        }
        return result;
    }

    void append(
        const std::uint32_t id,
        const Side side,
        const std::uint16_t price,
        const std::uint32_t quantity) {
        auto& queue = levels_[side_index(side)][price];
        queue.push_back(RefOrder{.id = id, .quantity = quantity});
        locations_.emplace(id, Location{.side = side, .price = price, .iterator = std::prev(queue.end())});
    }

    void erase(const std::unordered_map<std::uint32_t, Location>::iterator found) {
        auto level = levels_[side_index(found->second.side)].find(found->second.price);
        level->second.erase(found->second.iterator);
        if (level->second.empty()) {
            levels_[side_index(found->second.side)].erase(level);
        }
        locations_.erase(found);
    }

    std::size_t max_orders_{};
    std::size_t max_order_ids_{};
    std::array<LevelMap, 2> levels_;
    std::unordered_map<std::uint32_t, Location> locations_;
};

[[nodiscard]] inline bool same_result(const SubmitResult& left, const SubmitResult& right) noexcept {
    return left.reject_reason == right.reject_reason
        && left.fills == right.fills
        && left.rested_quantity == right.rested_quantity
        && left.canceled_quantity == right.canceled_quantity
        && left.traded_quantity == right.traded_quantity
        && left.notional_ticks == right.notional_ticks
        && left.report_hash == right.report_hash;
}

} // namespace bitmap_exchange::test

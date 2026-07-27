#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace bitmap_exchange {

inline constexpr std::size_t kPriceCount = 1ULL << 16;
inline constexpr std::uint32_t kInvalidIndex = std::numeric_limits<std::uint32_t>::max();
inline constexpr std::int32_t kNoBid = -1;
inline constexpr std::int32_t kNoAsk = static_cast<std::int32_t>(kPriceCount);

// A market order is a limit order at the most aggressive representable tick.
// These are the ends of the ladder, not sentinels: a market buy crosses every
// ask, a market sell crosses every bid.
inline constexpr std::uint16_t kMostAggressiveBidTick = std::numeric_limits<std::uint16_t>::max();
inline constexpr std::uint16_t kMostAggressiveAskTick = 0;

enum class Side : std::uint8_t { Bid = 0, Ask = 1 };
enum class TimeInForce : std::uint8_t { GoodTillCancel, ImmediateOrCancel, FillOrKill, PostOnly };

enum class RejectReason : std::uint8_t {
    None,
    QuantityZero,
    OrderIdOutOfRange,
    DuplicateOrderId,
    UnknownOrderId,
    CapacityExceeded,
    WouldCross,
    InsufficientLiquidity,
    QuantityIncreaseNotAllowed,
    ReplacementIdMustDiffer,
    UnsupportedTimeInForce,
};

[[nodiscard]] constexpr std::size_t side_index(const Side side) noexcept {
    // `std::to_underlying` says this in one word, but it is C++23, which MSVC
    // offers only as a preview mode. This is the C++20 spelling of the same
    // cast, and C++20 is the newest standard all three compilers implement
    // properly.
    return static_cast<std::underlying_type_t<Side>>(side);
}

[[nodiscard]] constexpr Side opposite(const Side side) noexcept {
    return side == Side::Bid ? Side::Ask : Side::Bid;
}

[[nodiscard]] constexpr bool succeeded(const RejectReason reason) noexcept {
    return reason == RejectReason::None;
}

[[nodiscard]] constexpr bool valid_time_in_force(const TimeInForce time_in_force) noexcept {
    switch (time_in_force) {
        case TimeInForce::GoodTillCancel:
        case TimeInForce::ImmediateOrCancel:
        case TimeInForce::FillOrKill:
        case TimeInForce::PostOnly:
            return true;
    }
    return false;
}

[[nodiscard]] inline std::uint64_t mix64(std::uint64_t value) noexcept {
    value ^= value >> 30;
    value *= 0xbf58'476d'1ce4'e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d0'49bb'1331'11ebULL;
    return value ^ (value >> 31);
}

class HierarchicalBitmap final {
public:
    static constexpr std::size_t kLeafWordCount = kPriceCount / 64;
    static constexpr std::size_t kSummaryWordCount = kLeafWordCount / 64;

    void clear() noexcept {
        leaf_.fill(0);
        summary_.fill(0);
        root_ = 0;
    }

    [[nodiscard]] bool empty() const noexcept { return root_ == 0; }

    [[nodiscard]] bool test(const std::uint16_t price) const noexcept {
        const auto value = static_cast<std::size_t>(price);
        return ((leaf_[value >> 6] >> (value & 63)) & 1ULL) != 0;
    }

    void set(const std::uint16_t price) noexcept {
        const auto value = static_cast<std::size_t>(price);
        const auto leaf_index = value >> 6;
        const auto leaf_bit = 1ULL << (value & 63);
        const auto old_leaf = leaf_[leaf_index];
        if ((old_leaf & leaf_bit) != 0) {
            return;
        }

        leaf_[leaf_index] = old_leaf | leaf_bit;
        if (old_leaf != 0) {
            return;
        }

        const auto summary_index = leaf_index >> 6;
        const auto summary_bit = 1ULL << (leaf_index & 63);
        const auto old_summary = summary_[summary_index];
        summary_[summary_index] = old_summary | summary_bit;
        if (old_summary == 0) {
            root_ |= 1ULL << summary_index;
        }
    }

    void reset(const std::uint16_t price) noexcept {
        const auto value = static_cast<std::size_t>(price);
        const auto leaf_index = value >> 6;
        const auto leaf_bit = 1ULL << (value & 63);
        const auto old_leaf = leaf_[leaf_index];
        if ((old_leaf & leaf_bit) == 0) {
            return;
        }

        const auto next_leaf = old_leaf & ~leaf_bit;
        leaf_[leaf_index] = next_leaf;
        if (next_leaf != 0) {
            return;
        }

        const auto summary_index = leaf_index >> 6;
        const auto summary_bit = 1ULL << (leaf_index & 63);
        const auto next_summary = summary_[summary_index] & ~summary_bit;
        summary_[summary_index] = next_summary;
        if (next_summary == 0) {
            root_ &= ~(1ULL << summary_index);
        }
    }

    [[nodiscard]] std::int32_t first() const noexcept {
        if (root_ == 0) {
            return kNoAsk;
        }
        const auto summary_index = least_bit(root_);
        const auto leaf_offset = least_bit(summary_[summary_index]);
        const auto leaf_index = (summary_index << 6) + leaf_offset;
        return static_cast<std::int32_t>((leaf_index << 6) + least_bit(leaf_[leaf_index]));
    }

    [[nodiscard]] std::int32_t last() const noexcept {
        if (root_ == 0) {
            return kNoBid;
        }
        const auto summary_index = greatest_bit(root_);
        const auto leaf_offset = greatest_bit(summary_[summary_index]);
        const auto leaf_index = (summary_index << 6) + leaf_offset;
        return static_cast<std::int32_t>((leaf_index << 6) + greatest_bit(leaf_[leaf_index]));
    }

    [[nodiscard]] std::int32_t next(const std::uint16_t price) const noexcept {
        if (price == std::numeric_limits<std::uint16_t>::max()) {
            return kNoAsk;
        }

        const auto value = static_cast<std::size_t>(price) + 1;
        auto leaf_index = value >> 6;
        const auto local = leaf_[leaf_index] & (~0ULL << (value & 63));
        if (local != 0) {
            return static_cast<std::int32_t>((leaf_index << 6) + least_bit(local));
        }

        auto summary_index = leaf_index >> 6;
        const auto summary_start = (leaf_index & 63) + 1;
        auto leaves = summary_start < 64 ? (summary_[summary_index] & (~0ULL << summary_start)) : 0;
        if (leaves == 0) {
            const auto root_start = summary_index + 1;
            const auto summaries = root_start < 64 ? (root_ & (~0ULL << root_start)) : 0;
            if (summaries == 0) {
                return kNoAsk;
            }
            summary_index = least_bit(summaries);
            leaves = summary_[summary_index];
        }

        leaf_index = (summary_index << 6) + least_bit(leaves);
        return static_cast<std::int32_t>((leaf_index << 6) + least_bit(leaf_[leaf_index]));
    }

    [[nodiscard]] std::int32_t previous(const std::uint16_t price) const noexcept {
        if (price == 0) {
            return kNoBid;
        }

        const auto value = static_cast<std::size_t>(price) - 1;
        auto leaf_index = value >> 6;
        const auto local_bit = value & 63;
        const auto local_mask = local_bit == 63 ? ~0ULL : ((1ULL << (local_bit + 1)) - 1);
        const auto local = leaf_[leaf_index] & local_mask;
        if (local != 0) {
            return static_cast<std::int32_t>((leaf_index << 6) + greatest_bit(local));
        }

        auto summary_index = leaf_index >> 6;
        const auto summary_bit = leaf_index & 63;
        auto leaves = summary_bit == 0 ? 0 : (summary_[summary_index] & ((1ULL << summary_bit) - 1));
        if (leaves == 0) {
            const auto summaries = summary_index == 0 ? 0 : (root_ & ((1ULL << summary_index) - 1));
            if (summaries == 0) {
                return kNoBid;
            }
            summary_index = greatest_bit(summaries);
            leaves = summary_[summary_index];
        }

        leaf_index = (summary_index << 6) + greatest_bit(leaves);
        return static_cast<std::int32_t>((leaf_index << 6) + greatest_bit(leaf_[leaf_index]));
    }

    [[nodiscard]] bool validate() const noexcept {
        std::uint64_t expected_root = 0;
        for (std::size_t summary_index = 0; summary_index < kSummaryWordCount; ++summary_index) {
            std::uint64_t expected_summary = 0;
            for (std::size_t offset = 0; offset < 64; ++offset) {
                const auto leaf_index = (summary_index << 6) + offset;
                if (leaf_[leaf_index] != 0) {
                    expected_summary |= 1ULL << offset;
                }
            }
            if (summary_[summary_index] != expected_summary) {
                return false;
            }
            if (expected_summary != 0) {
                expected_root |= 1ULL << summary_index;
            }
        }
        return root_ == expected_root;
    }

private:
    [[nodiscard]] static constexpr std::size_t least_bit(const std::uint64_t value) noexcept {
        return static_cast<std::size_t>(std::countr_zero(value));
    }

    [[nodiscard]] static constexpr std::size_t greatest_bit(const std::uint64_t value) noexcept {
        return static_cast<std::size_t>(63 - std::countl_zero(value));
    }

    std::array<std::uint64_t, kLeafWordCount> leaf_{};
    std::array<std::uint64_t, kSummaryWordCount> summary_{};
    std::uint64_t root_{};
};

struct SweepResult final {
    std::uint64_t requested_quantity{};
    std::uint64_t filled_quantity{};
    std::uint64_t notional_ticks{};
    std::uint32_t levels_visited{};

    [[nodiscard]] double average_price() const noexcept {
        return filled_quantity == 0
            ? 0.0
            : static_cast<double>(notional_ticks) / static_cast<double>(filled_quantity);
    }
};

class L2Book final {
public:
    L2Book() { clear(); }

    void clear() noexcept {
        for (auto& side : sides_) {
            side.quantity.fill(0);
            side.active.clear();
            side.total_quantity = 0;
        }
        sides_[side_index(Side::Bid)].best = kNoBid;
        sides_[side_index(Side::Ask)].best = kNoAsk;
    }

    void set_level(const Side side, const std::uint16_t price, const std::uint32_t quantity) noexcept {
        auto& state = sides_[side_index(side)];
        const auto price_index = static_cast<std::size_t>(price);
        const auto old_quantity = state.quantity[price_index];
        if (old_quantity == quantity) {
            return;
        }

        state.quantity[price_index] = quantity;
        state.total_quantity = state.total_quantity - old_quantity + quantity;

        if (old_quantity == 0 && quantity != 0) {
            state.active.set(price);
            if (side == Side::Bid) {
                state.best = std::max(state.best, static_cast<std::int32_t>(price));
            } else {
                state.best = std::min(state.best, static_cast<std::int32_t>(price));
            }
        } else if (old_quantity != 0 && quantity == 0) {
            state.active.reset(price);
            if (state.best == static_cast<std::int32_t>(price)) {
                state.best = side == Side::Bid ? state.active.last() : state.active.first();
            }
        }
    }

    [[nodiscard]] std::uint32_t quantity(const Side side, const std::uint16_t price) const noexcept {
        return sides_[side_index(side)].quantity[price];
    }

    [[nodiscard]] std::uint64_t total_quantity(const Side side) const noexcept {
        return sides_[side_index(side)].total_quantity;
    }

    [[nodiscard]] std::int32_t best_bid() const noexcept { return sides_[0].best; }
    [[nodiscard]] std::int32_t best_ask() const noexcept { return sides_[1].best; }

    template <class Visitor>
    std::size_t for_each_top(const Side side, const std::size_t limit, Visitor&& visitor) const
        noexcept(noexcept(visitor(std::uint16_t{}, std::uint32_t{}))) {
        const auto& state = sides_[side_index(side)];
        auto price = state.best;
        std::size_t visited = 0;
        while (visited < limit && is_valid_price(price)) {
            visitor(static_cast<std::uint16_t>(price), state.quantity[static_cast<std::size_t>(price)]);
            ++visited;
            price = side == Side::Bid
                ? state.active.previous(static_cast<std::uint16_t>(price))
                : state.active.next(static_cast<std::uint16_t>(price));
        }
        return visited;
    }

    [[nodiscard]] SweepResult sweep(const Side side, const std::uint64_t target_quantity) const noexcept {
        SweepResult result{.requested_quantity = target_quantity};
        const auto& state = sides_[side_index(side)];
        auto remaining = target_quantity;
        auto price = state.best;

        while (remaining != 0 && is_valid_price(price)) {
            const auto available = static_cast<std::uint64_t>(state.quantity[static_cast<std::size_t>(price)]);
            const auto fill = std::min(remaining, available);
            remaining -= fill;
            result.filled_quantity += fill;
            result.notional_ticks += fill * static_cast<std::uint64_t>(price);
            ++result.levels_visited;
            price = side == Side::Bid
                ? state.active.previous(static_cast<std::uint16_t>(price))
                : state.active.next(static_cast<std::uint16_t>(price));
        }
        return result;
    }

    [[nodiscard]] std::uint64_t top_checksum(const Side side, const std::size_t limit) const noexcept {
        std::uint64_t hash = 0;
        std::uint64_t rank = 1;
        for_each_top(side, limit, [&](const std::uint16_t price, const std::uint32_t quantity) {
            hash = mix64(hash ^ (static_cast<std::uint64_t>(price) << 32) ^ quantity ^ rank++);
        });
        return hash;
    }

    [[nodiscard]] std::uint64_t state_hash() const noexcept {
        std::uint64_t hash = mix64(static_cast<std::uint64_t>(best_bid() + 1))
                           ^ mix64(static_cast<std::uint64_t>(best_ask() + 1));
        for (std::size_t side = 0; side < 2; ++side) {
            for (std::size_t price = 0; price < kPriceCount; ++price) {
                const auto level_quantity = sides_[side].quantity[price];
                if (level_quantity != 0) {
                    hash = mix64(hash ^ (static_cast<std::uint64_t>(side) << 63)
                                      ^ (static_cast<std::uint64_t>(price) << 32)
                                      ^ level_quantity);
                }
            }
        }
        return hash;
    }

    [[nodiscard]] bool validate() const noexcept {
        for (std::size_t side = 0; side < 2; ++side) {
            if (!sides_[side].active.validate()) {
                return false;
            }
            std::uint64_t total = 0;
            for (std::size_t price = 0; price < kPriceCount; ++price) {
                const auto level_quantity = sides_[side].quantity[price];
                total += level_quantity;
                if (sides_[side].active.test(static_cast<std::uint16_t>(price)) != (level_quantity != 0)) {
                    return false;
                }
            }
            if (total != sides_[side].total_quantity) {
                return false;
            }
        }
        return best_bid() == sides_[0].active.last() && best_ask() == sides_[1].active.first();
    }

private:
    [[nodiscard]] static constexpr bool is_valid_price(const std::int32_t price) noexcept {
        return price >= 0 && price < static_cast<std::int32_t>(kPriceCount);
    }

    struct alignas(64) SideState final {
        std::array<std::uint32_t, kPriceCount> quantity{};
        HierarchicalBitmap active{};
        std::int32_t best{};
        std::uint64_t total_quantity{};
    };

    std::array<SideState, 2> sides_{};
};

struct OrderSlot final {
    std::uint32_t next{kInvalidIndex};
    std::uint32_t previous{kInvalidIndex};
    std::uint32_t id{};
    std::uint32_t quantity{};
    std::uint32_t free_next{kInvalidIndex};
    std::uint32_t price_side{};

    [[nodiscard]] Side side() const noexcept { return static_cast<Side>(price_side >> 16); }
    [[nodiscard]] std::uint16_t price() const noexcept {
        return static_cast<std::uint16_t>(price_side & 0xffffU);
    }
};
static_assert(sizeof(OrderSlot) == 24);
static_assert(std::is_trivially_copyable_v<OrderSlot>);

struct PriceLevel final {
    std::uint32_t head{kInvalidIndex};
    std::uint32_t tail{kInvalidIndex};
    std::uint64_t total_quantity{};
};
static_assert(sizeof(PriceLevel) == 16);
static_assert(std::is_trivially_copyable_v<PriceLevel>);

struct OrderView final {
    std::uint32_t id{};
    Side side{};
    std::uint16_t price{};
    std::uint32_t quantity{};
};

struct Fill final {
    std::uint32_t maker_order_id{};
    Side maker_side{};
    std::uint16_t price{};
    std::uint32_t quantity{};

    friend constexpr bool operator==(const Fill&, const Fill&) noexcept = default;
};

struct SubmitResult final {
    RejectReason reject_reason{RejectReason::None};
    std::uint32_t fills{};
    std::uint32_t rested_quantity{};
    std::uint32_t canceled_quantity{};
    std::uint64_t traded_quantity{};
    std::uint64_t notional_ticks{};
    std::uint64_t report_hash{};

    [[nodiscard]] bool accepted() const noexcept { return reject_reason == RejectReason::None; }
    [[nodiscard]] double average_price() const noexcept {
        return traded_quantity == 0
            ? 0.0
            : static_cast<double>(notional_ticks) / static_cast<double>(traded_quantity);
    }
};

class L3Book final {
public:
    explicit L3Book(const std::size_t max_orders, const std::size_t max_order_ids)
        : levels_{std::vector<PriceLevel>(kPriceCount), std::vector<PriceLevel>(kPriceCount)},
          slots_(checked_capacity(max_orders)),
          id_to_slot_(max_order_ids, kInvalidIndex) {}

    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }
    [[nodiscard]] std::size_t order_id_capacity() const noexcept { return id_to_slot_.size(); }
    [[nodiscard]] std::uint32_t live_orders() const noexcept { return live_orders_; }
    [[nodiscard]] std::int32_t best_bid() const noexcept { return best_[0]; }
    [[nodiscard]] std::int32_t best_ask() const noexcept { return best_[1]; }

    [[nodiscard]] bool contains(const std::uint32_t id) const noexcept {
        return id < id_to_slot_.size() && id_to_slot_[id] != kInvalidIndex;
    }

    [[nodiscard]] std::optional<OrderView> order(const std::uint32_t id) const noexcept {
        if (!contains(id)) {
            return std::nullopt;
        }
        const auto& slot = slots_[id_to_slot_[id]];
        return OrderView{.id = slot.id, .side = slot.side(), .price = slot.price(), .quantity = slot.quantity};
    }

    [[nodiscard]] std::uint64_t level_quantity(const Side side, const std::uint16_t price) const noexcept {
        return levels_[side_index(side)][price].total_quantity;
    }

    [[nodiscard]] bool would_cross(const Side side, const std::uint16_t price) const noexcept {
        return side == Side::Bid
            ? best_ask() <= static_cast<std::int32_t>(price)
            : best_bid() >= static_cast<std::int32_t>(price);
    }

    [[nodiscard]] RejectReason add_resting(
        const std::uint32_t id,
        const Side side,
        const std::uint16_t price,
        const std::uint32_t quantity) noexcept {
        if (const auto validation = validate_new_order(id, quantity); validation != RejectReason::None) {
            return validation;
        }
        if (would_cross(side, price)) {
            return RejectReason::WouldCross;
        }
        const auto slot_index = allocate_slot();
        if (slot_index == kInvalidIndex) {
            return RejectReason::CapacityExceeded;
        }
        append(slot_index, id, side, price, quantity);
        return RejectReason::None;
    }

    [[nodiscard]] RejectReason cancel(const std::uint32_t id) noexcept {
        const auto lookup = lookup_order(id);
        if (lookup.reason != RejectReason::None) {
            return lookup.reason;
        }
        unlink(lookup.slot_index);
        return RejectReason::None;
    }

    [[nodiscard]] RejectReason reduce_quantity(
        const std::uint32_t id,
        const std::uint32_t new_quantity) noexcept {
        const auto lookup = lookup_order(id);
        if (lookup.reason != RejectReason::None) {
            return lookup.reason;
        }
        auto& slot = slots_[lookup.slot_index];
        if (new_quantity > slot.quantity) {
            return RejectReason::QuantityIncreaseNotAllowed;
        }
        if (new_quantity == 0) {
            unlink(lookup.slot_index);
            return RejectReason::None;
        }
        if (new_quantity == slot.quantity) {
            return RejectReason::None;
        }

        auto& level = levels_[side_index(slot.side())][slot.price()];
        level.total_quantity -= static_cast<std::uint64_t>(slot.quantity - new_quantity);
        slot.quantity = new_quantity;
        return RejectReason::None;
    }

    template <class FillHandler>
    [[nodiscard]] SubmitResult replace_order(
        const std::uint32_t existing_id,
        const std::uint32_t replacement_id,
        const std::uint16_t new_price,
        const std::uint32_t new_quantity,
        FillHandler&& on_fill) noexcept(noexcept(on_fill(Fill{}))) {
        const auto existing = lookup_order(existing_id);
        if (existing.reason != RejectReason::None) {
            return rejected(existing.reason);
        }
        if (replacement_id == existing_id) {
            return rejected(RejectReason::ReplacementIdMustDiffer);
        }
        if (const auto validation = validate_new_order(replacement_id, new_quantity);
            validation != RejectReason::None) {
            return rejected(validation);
        }

        const auto side = slots_[existing.slot_index].side();
        unlink(existing.slot_index);
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
        const std::uint32_t new_quantity) noexcept {
        return replace_order(existing_id, replacement_id, new_price, new_quantity, [](const Fill&) noexcept {});
    }

    template <class FillHandler>
    [[nodiscard]] SubmitResult submit_limit(
        const std::uint32_t id,
        const Side side,
        const std::uint16_t limit_price,
        const std::uint32_t quantity,
        const TimeInForce time_in_force,
        FillHandler&& on_fill) noexcept(noexcept(on_fill(Fill{}))) {
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
        const TimeInForce time_in_force = TimeInForce::GoodTillCancel) noexcept {
        return submit_limit(id, side, limit_price, quantity, time_in_force, [](const Fill&) noexcept {});
    }

    template <class FillHandler>
    [[nodiscard]] SubmitResult submit_market(
        const std::uint32_t id,
        const Side side,
        const std::uint32_t quantity,
        const TimeInForce time_in_force,
        FillHandler&& on_fill) noexcept(noexcept(on_fill(Fill{}))) {
        if (time_in_force != TimeInForce::ImmediateOrCancel
            && time_in_force != TimeInForce::FillOrKill) {
            return rejected(RejectReason::UnsupportedTimeInForce);
        }
        const auto market_limit = side == Side::Bid
            ? kMostAggressiveBidTick
            : kMostAggressiveAskTick;
        return submit_limit(
            id,
            side,
            market_limit,
            quantity,
            time_in_force,
            std::forward<FillHandler>(on_fill));
    }

    [[nodiscard]] SubmitResult submit_market(
        const std::uint32_t id,
        const Side side,
        const std::uint32_t quantity,
        const TimeInForce time_in_force = TimeInForce::ImmediateOrCancel) noexcept {
        return submit_market(id, side, quantity, time_in_force, [](const Fill&) noexcept {});
    }

    template <class Visitor>
    std::size_t for_each_level(const Side side, const std::size_t limit, Visitor&& visitor) const
        noexcept(noexcept(visitor(std::uint16_t{}, std::uint64_t{}))) {
        const auto side_value = side_index(side);
        auto price = best_[side_value];
        std::size_t visited = 0;
        while (visited < limit && is_valid_price(price)) {
            visitor(
                static_cast<std::uint16_t>(price),
                levels_[side_value][static_cast<std::size_t>(price)].total_quantity);
            ++visited;
            price = side == Side::Bid
                ? active_[side_value].previous(static_cast<std::uint16_t>(price))
                : active_[side_value].next(static_cast<std::uint16_t>(price));
        }
        return visited;
    }

    template <class Visitor>
    std::size_t for_each_order_at_level(
        const Side side,
        const std::uint16_t price,
        Visitor&& visitor) const noexcept(noexcept(visitor(OrderView{}))) {
        auto slot_index = levels_[side_index(side)][price].head;
        std::size_t visited = 0;
        while (slot_index != kInvalidIndex) {
            const auto& slot = slots_[slot_index];
            visitor(OrderView{.id = slot.id, .side = side, .price = price, .quantity = slot.quantity});
            ++visited;
            slot_index = slot.next;
        }
        return visited;
    }

    [[nodiscard]] std::uint64_t state_hash() const noexcept {
        std::uint64_t hash = mix64(live_orders_ + 7);
        for (std::size_t side = 0; side < 2; ++side) {
            for (std::size_t price = 0; price < kPriceCount; ++price) {
                auto slot_index = levels_[side][price].head;
                std::uint64_t rank = 1;
                while (slot_index != kInvalidIndex) {
                    const auto& slot = slots_[slot_index];
                    hash = mix64(hash
                        ^ (static_cast<std::uint64_t>(side) << 63)
                        ^ (static_cast<std::uint64_t>(price) << 40)
                        ^ (static_cast<std::uint64_t>(slot.id) << 8)
                        ^ slot.quantity
                        ^ rank++);
                    slot_index = slot.next;
                }
            }
        }
        return hash;
    }

    [[nodiscard]] bool validate() const {
        if (!active_[0].validate() || !active_[1].validate()) {
            return false;
        }
        if (live_orders_ > slots_.size() || next_unused_ > slots_.size()) {
            return false;
        }

        std::vector<std::uint8_t> slot_state(next_unused_, 0);
        std::uint64_t counted_orders = 0;
        for (std::size_t side = 0; side < 2; ++side) {
            for (std::size_t price = 0; price < kPriceCount; ++price) {
                const auto& level = levels_[side][price];
                if ((level.head == kInvalidIndex) != (level.tail == kInvalidIndex)) {
                    return false;
                }

                auto current = level.head;
                auto previous = kInvalidIndex;
                std::uint64_t quantity = 0;
                while (current != kInvalidIndex) {
                    if (current >= next_unused_ || slot_state[current] != 0) {
                        return false;
                    }
                    slot_state[current] = 1;
                    const auto& slot = slots_[current];
                    if (side_index(slot.side()) != side
                        || slot.price() != price
                        || slot.previous != previous
                        || slot.quantity == 0
                        || slot.id >= id_to_slot_.size()
                        || id_to_slot_[slot.id] != current) {
                        return false;
                    }
                    quantity += slot.quantity;
                    ++counted_orders;
                    previous = current;
                    current = slot.next;
                }

                if (previous != level.tail || quantity != level.total_quantity) {
                    return false;
                }
                if (level.head != kInvalidIndex) {
                    if (slots_[level.head].previous != kInvalidIndex || slots_[level.tail].next != kInvalidIndex) {
                        return false;
                    }
                }
                if (active_[side].test(static_cast<std::uint16_t>(price)) != (level.head != kInvalidIndex)) {
                    return false;
                }
            }
        }

        std::uint64_t free_slots = 0;
        auto free_index = free_head_;
        while (free_index != kInvalidIndex) {
            if (free_index >= next_unused_ || slot_state[free_index] != 0) {
                return false;
            }
            slot_state[free_index] = 2;
            ++free_slots;
            free_index = slots_[free_index].free_next;
        }
        for (const auto state : slot_state) {
            if (state == 0) {
                return false;
            }
        }

        if (counted_orders != live_orders_ || counted_orders + free_slots != next_unused_) {
            return false;
        }
        for (std::size_t id = 0; id < id_to_slot_.size(); ++id) {
            const auto slot_index = id_to_slot_[id];
            if (slot_index != kInvalidIndex
                && (slot_index >= next_unused_ || slot_state[slot_index] != 1 || slots_[slot_index].id != id)) {
                return false;
            }
        }
        if (best_bid() != active_[0].last() || best_ask() != active_[1].first()) {
            return false;
        }
        return best_bid() == kNoBid || best_ask() == kNoAsk || best_bid() < best_ask();
    }

private:
    [[nodiscard]] static std::size_t checked_capacity(const std::size_t max_orders) {
        if (max_orders >= static_cast<std::size_t>(kInvalidIndex)) {
            throw std::invalid_argument("max_orders exceeds 32-bit slot index space");
        }
        return max_orders;
    }

    struct LookupResult final {
        RejectReason reason{RejectReason::None};
        std::uint32_t slot_index{kInvalidIndex};
    };

    struct LiquidityPreview final {
        std::uint32_t executable_quantity{};
        bool releases_slot{};
    };

    [[nodiscard]] static constexpr bool is_valid_price(const std::int32_t price) noexcept {
        return price >= 0 && price < static_cast<std::int32_t>(kPriceCount);
    }

    [[nodiscard]] static SubmitResult rejected(const RejectReason reason) noexcept {
        return SubmitResult{.reject_reason = reason};
    }

    [[nodiscard]] RejectReason validate_new_order(
        const std::uint32_t id,
        const std::uint32_t quantity) const noexcept {
        if (quantity == 0) {
            return RejectReason::QuantityZero;
        }
        if (id >= id_to_slot_.size()) {
            return RejectReason::OrderIdOutOfRange;
        }
        if (id_to_slot_[id] != kInvalidIndex) {
            return RejectReason::DuplicateOrderId;
        }
        return RejectReason::None;
    }

    [[nodiscard]] LookupResult lookup_order(const std::uint32_t id) const noexcept {
        if (id >= id_to_slot_.size()) {
            return LookupResult{.reason = RejectReason::OrderIdOutOfRange};
        }
        const auto slot_index = id_to_slot_[id];
        if (slot_index == kInvalidIndex) {
            return LookupResult{.reason = RejectReason::UnknownOrderId};
        }
        return LookupResult{.slot_index = slot_index};
    }

    [[nodiscard]] bool slot_available() const noexcept {
        return free_head_ != kInvalidIndex || next_unused_ < slots_.size();
    }

    [[nodiscard]] LiquidityPreview preview_liquidity(
        const Side taker_side,
        const std::uint16_t limit_price,
        const std::uint32_t requested_quantity) const noexcept {
        LiquidityPreview preview{};
        auto remaining = requested_quantity;
        const auto maker_side = opposite(taker_side);
        const auto maker_side_index = side_index(maker_side);
        auto price = taker_side == Side::Bid ? best_ask() : best_bid();

        while (remaining != 0 && is_valid_price(price) && crosses(taker_side, limit_price, price)) {
            auto slot_index = levels_[maker_side_index][static_cast<std::size_t>(price)].head;
            while (remaining != 0 && slot_index != kInvalidIndex) {
                const auto& maker = slots_[slot_index];
                const auto fill = std::min(remaining, maker.quantity);
                remaining -= fill;
                preview.executable_quantity += fill;
                preview.releases_slot = preview.releases_slot || fill == maker.quantity;
                slot_index = maker.next;
            }
            price = taker_side == Side::Bid
                ? active_[maker_side_index].next(static_cast<std::uint16_t>(price))
                : active_[maker_side_index].previous(static_cast<std::uint16_t>(price));
        }
        return preview;
    }

    [[nodiscard]] static bool crosses(
        const Side taker_side,
        const std::uint16_t limit_price,
        const std::int32_t maker_price) noexcept {
        return taker_side == Side::Bid
            ? maker_price <= static_cast<std::int32_t>(limit_price)
            : maker_price >= static_cast<std::int32_t>(limit_price);
    }

    template <class FillHandler>
    [[nodiscard]] SubmitResult submit_limit_impl(
        const std::uint32_t id,
        const Side side,
        const std::uint16_t limit_price,
        const std::uint32_t quantity,
        const TimeInForce time_in_force,
        FillHandler&& on_fill,
        const bool validation_already_performed) noexcept(noexcept(on_fill(Fill{}))) {
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
            const auto slot_index = allocate_slot();
            if (slot_index == kInvalidIndex) {
                return rejected(RejectReason::CapacityExceeded);
            }
            append(slot_index, id, side, limit_price, quantity);
            return SubmitResult{.rested_quantity = quantity};
        }

        const auto preview = (time_in_force == TimeInForce::FillOrKill
                              || (time_in_force == TimeInForce::GoodTillCancel && !slot_available()))
            ? preview_liquidity(side, limit_price, quantity)
            : LiquidityPreview{};

        if (time_in_force == TimeInForce::FillOrKill && preview.executable_quantity < quantity) {
            return rejected(RejectReason::InsufficientLiquidity);
        }
        if (time_in_force == TimeInForce::GoodTillCancel && !slot_available()) {
            const auto remainder = quantity - preview.executable_quantity;
            if (remainder != 0 && !preview.releases_slot) {
                return rejected(RejectReason::CapacityExceeded);
            }
        }

        SubmitResult result{};
        auto remaining = quantity;
        const auto maker_side = opposite(side);
        const auto maker_side_index = side_index(maker_side);

        while (remaining != 0) {
            const auto maker_price_value = side == Side::Bid ? best_ask() : best_bid();
            if (!is_valid_price(maker_price_value) || !crosses(side, limit_price, maker_price_value)) {
                break;
            }

            const auto maker_price = static_cast<std::uint16_t>(maker_price_value);
            const auto maker_slot_index = levels_[maker_side_index][maker_price].head;
            const auto maker = slots_[maker_slot_index];
            const auto fill_quantity = std::min(remaining, maker.quantity);
            const Fill fill{
                .maker_order_id = maker.id,
                .maker_side = maker_side,
                .price = maker_price,
                .quantity = fill_quantity,
            };

            remaining -= fill_quantity;
            ++result.fills;
            result.traded_quantity += fill_quantity;
            result.notional_ticks += static_cast<std::uint64_t>(maker_price) * fill_quantity;
            result.report_hash = mix64(result.report_hash
                ^ static_cast<std::uint64_t>(maker.id)
                ^ (static_cast<std::uint64_t>(maker_price) << 32)
                ^ (static_cast<std::uint64_t>(fill_quantity) << 1)
                ^ static_cast<std::uint64_t>(maker_side_index));
            if (fill_quantity == maker.quantity) {
                unlink(maker_slot_index);
            } else {
                slots_[maker_slot_index].quantity -= fill_quantity;
                levels_[maker_side_index][maker_price].total_quantity -= fill_quantity;
            }

            // Publish only after the fill has been committed, so an exception from a
            // user callback cannot leave the order book structurally inconsistent.
            on_fill(fill);
        }

        if (remaining != 0) {
            if (time_in_force == TimeInForce::GoodTillCancel) {
                const auto slot_index = allocate_slot();
                if (slot_index == kInvalidIndex) [[unlikely]] {
                    // A successful preflight guarantees a slot. Treat a violation as
                    // an internal engine fault, never as a normal post-fill rejection.
                    std::terminate();
                }
                append(slot_index, id, side, limit_price, remaining);
                result.rested_quantity = remaining;
            } else {
                result.canceled_quantity = remaining;
            }
        }
        return result;
    }

    [[nodiscard]] std::uint32_t allocate_slot() noexcept {
        if (free_head_ != kInvalidIndex) {
            const auto slot_index = free_head_;
            free_head_ = slots_[slot_index].free_next;
            return slot_index;
        }
        if (next_unused_ >= slots_.size()) {
            return kInvalidIndex;
        }
        return next_unused_++;
    }

    void release_slot(const std::uint32_t slot_index) noexcept {
        slots_[slot_index].free_next = free_head_;
        free_head_ = slot_index;
    }

    void append(
        const std::uint32_t slot_index,
        const std::uint32_t id,
        const Side side,
        const std::uint16_t price,
        const std::uint32_t quantity) noexcept {
        const auto side_value = side_index(side);
        auto& level = levels_[side_value][price];
        const bool was_empty = level.head == kInvalidIndex;

        slots_[slot_index] = OrderSlot{
            .next = kInvalidIndex,
            .previous = level.tail,
            .id = id,
            .quantity = quantity,
            .free_next = kInvalidIndex,
            .price_side = (static_cast<std::uint32_t>(side_value) << 16) | price,
        };

        if (level.tail == kInvalidIndex) {
            level.head = slot_index;
        } else {
            slots_[level.tail].next = slot_index;
        }
        level.tail = slot_index;
        level.total_quantity += quantity;
        id_to_slot_[id] = slot_index;
        ++live_orders_;

        if (was_empty) {
            active_[side_value].set(price);
            if (side == Side::Bid) {
                best_[side_value] = std::max(best_[side_value], static_cast<std::int32_t>(price));
            } else {
                best_[side_value] = std::min(best_[side_value], static_cast<std::int32_t>(price));
            }
        }
    }

    void unlink(const std::uint32_t slot_index) noexcept {
        const auto order = slots_[slot_index];
        const auto side_value = side_index(order.side());
        const auto price = order.price();
        auto& level = levels_[side_value][price];

        if (order.previous == kInvalidIndex) {
            level.head = order.next;
        } else {
            slots_[order.previous].next = order.next;
        }
        if (order.next == kInvalidIndex) {
            level.tail = order.previous;
        } else {
            slots_[order.next].previous = order.previous;
        }

        level.total_quantity -= order.quantity;
        id_to_slot_[order.id] = kInvalidIndex;
        --live_orders_;

        if (level.head == kInvalidIndex) {
            active_[side_value].reset(price);
            if (best_[side_value] == static_cast<std::int32_t>(price)) {
                best_[side_value] = order.side() == Side::Bid
                    ? active_[side_value].last()
                    : active_[side_value].first();
            }
        }
        release_slot(slot_index);
    }

    std::array<std::vector<PriceLevel>, 2> levels_;
    std::vector<OrderSlot> slots_;
    std::vector<std::uint32_t> id_to_slot_;
    std::array<HierarchicalBitmap, 2> active_{};
    std::array<std::int32_t, 2> best_{kNoBid, kNoAsk};
    std::uint32_t next_unused_{};
    std::uint32_t free_head_{kInvalidIndex};
    std::uint32_t live_orders_{};
};

} // namespace bitmap_exchange

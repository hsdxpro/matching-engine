#include "bitmap_exchange.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace bx = bitmap_exchange;

namespace {

class Rng final {
public:
    explicit Rng(std::uint64_t seed) : state_(seed) {}
    [[nodiscard]] std::uint64_t next() noexcept {
        auto x = state_;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        state_ = x;
        return x * 2'685'821'657'736'338'717ULL;
    }
private:
    std::uint64_t state_;
};

template <class T>
inline void do_not_optimize(const T& value) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "g"(value) : "memory");
#else
    (void)value;
#endif
}

struct SampleStats final {
    double minimum{};
    double p50{};
    double p95{};
    double p99{};
    double p999{};
    double maximum{};
    double mean{};
};

[[nodiscard]] double percentile(const std::vector<double>& sorted, const double q) {
    const auto position = q * static_cast<double>(sorted.size() - 1);
    const auto lower = static_cast<std::size_t>(position);
    const auto upper = std::min(lower + 1, sorted.size() - 1);
    const auto fraction = position - static_cast<double>(lower);
    return sorted[lower] + (sorted[upper] - sorted[lower]) * fraction;
}

[[nodiscard]] SampleStats summarize(std::vector<double> samples) {
    std::sort(samples.begin(), samples.end());
    return SampleStats{
        .minimum = samples.front(),
        .p50 = percentile(samples, 0.50),
        .p95 = percentile(samples, 0.95),
        .p99 = percentile(samples, 0.99),
        .p999 = percentile(samples, 0.999),
        .maximum = samples.back(),
        .mean = std::accumulate(samples.begin(), samples.end(), 0.0) / static_cast<double>(samples.size()),
    };
}

struct BenchResult final {
    std::string scenario;
    std::string unit;
    SampleStats stats;
    double work_per_operation{1.0};
    std::size_t samples{};
};

[[nodiscard]] int pin_first_allowed_cpu() noexcept {
#if defined(__linux__)
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) {
        return -1;
    }
    for (std::size_t cpu = 0; cpu < static_cast<std::size_t>(CPU_SETSIZE); ++cpu) {
        if (CPU_ISSET(cpu, &allowed)) {
            cpu_set_t selected;
            CPU_ZERO(&selected);
            CPU_SET(cpu, &selected);
            return pthread_setaffinity_np(pthread_self(), sizeof(selected), &selected) == 0 ? static_cast<int>(cpu) : -1;
        }
    }
#endif
    return -1;
}

[[nodiscard]] std::string cpu_model() {
#if defined(__linux__)
    std::ifstream input("/proc/cpuinfo");
    std::string line;
    while (std::getline(input, line)) {
        if (line.starts_with("model name")) {
            const auto colon = line.find(':');
            if (colon != std::string::npos) {
                return line.substr(colon + 2);
            }
        }
    }
#endif
    return "unknown";
}

[[nodiscard]] std::vector<std::uint16_t> sparse_prices(
    const std::uint16_t low,
    const std::uint16_t high,
    const std::size_t count) {
    std::vector<std::uint16_t> prices;
    prices.reserve(count);
    const auto span = static_cast<std::uint32_t>(high) - low;
    for (std::size_t i = 0; i < count; ++i) {
        prices.push_back(static_cast<std::uint16_t>(
            static_cast<std::uint32_t>(low)
            + static_cast<std::uint32_t>((static_cast<std::uint64_t>(i) * span) / count)
            + (i % 3)));
    }
    std::sort(prices.begin(), prices.end());
    prices.erase(std::unique(prices.begin(), prices.end()), prices.end());
    return prices;
}

struct L2Update final {
    bx::Side side{};
    std::uint16_t price{};
    std::uint32_t quantity{};
};

[[nodiscard]] std::vector<L2Update> make_l2_updates(const std::size_t count) {
    const auto bids = sparse_prices(512, 32'000, 2'048);
    const auto asks = sparse_prices(33'536, 65'000, 2'048);
    std::array<std::vector<std::uint32_t>, 2> current{
        std::vector<std::uint32_t>(bids.size()),
        std::vector<std::uint32_t>(asks.size()),
    };
    std::vector<L2Update> updates;
    updates.reserve(count);
    Rng rng(0x1974'5acf'992e'731dULL);

    for (std::size_t i = 0; i < count; ++i) {
        const auto random = rng.next();
        const auto side = (random & 1) == 0 ? bx::Side::Bid : bx::Side::Ask;
        const auto s = bx::side_index(side);
        const auto& prices = side == bx::Side::Bid ? bids : asks;
        const auto index = static_cast<std::size_t>((random >> 8) % prices.size());
        const auto action = static_cast<std::uint32_t>((random >> 24) % 100);
        auto quantity = 1U + static_cast<std::uint32_t>((random >> 32) % 100'000);
        if (current[s][index] != 0 && action < 18) {
            quantity = 0;
        }
        current[s][index] = quantity;
        updates.push_back(L2Update{side, prices[index], quantity});
    }
    return updates;
}

template <class BatchFunction>
[[nodiscard]] SampleStats measure_batches(
    const std::size_t operations,
    const std::size_t batch_size,
    const int runs,
    BatchFunction&& function,
    std::uint64_t& sink) {
    std::vector<double> samples;
    samples.reserve((operations / batch_size + 1) * static_cast<std::size_t>(runs));

    for (int run = 0; run < runs; ++run) {
        function.reset(run);
        for (std::size_t begin = 0; begin < operations; begin += batch_size) {
            const auto end = std::min(begin + batch_size, operations);
            const auto start = std::chrono::steady_clock::now();
            const auto local = function(begin, end);
            const auto stop = std::chrono::steady_clock::now();
            do_not_optimize(local);
            sink = bx::mix64(sink ^ local ^ static_cast<std::uint64_t>(run + 1));
            const auto elapsed_ns = std::chrono::duration<double, std::nano>(stop - start).count();
            samples.push_back(elapsed_ns / static_cast<double>(end - begin));
        }
        sink = bx::mix64(sink ^ function.finish());
    }
    return summarize(std::move(samples));
}

class L2UpdateBench final {
public:
    explicit L2UpdateBench(const std::vector<L2Update>& updates) : updates_(updates) {}
    void reset(int) { book_.clear(); }
    [[nodiscard]] std::uint64_t operator()(const std::size_t begin, const std::size_t end) {
        std::uint64_t hash = 0;
        for (std::size_t i = begin; i < end; ++i) {
            const auto& update = updates_[i];
            book_.set_level(update.side, update.price, update.quantity);
            hash += static_cast<std::uint64_t>(book_.best_bid() + 1);
            hash ^= static_cast<std::uint64_t>(book_.best_ask() + 1);
        }
        return hash;
    }
    [[nodiscard]] std::uint64_t finish() const { return book_.state_hash(); }
private:
    const std::vector<L2Update>& updates_;
    bx::L2Book book_;
};

// Fills in place rather than returning by value: bx::L2Book embeds both
// 65,536-entry ladders, so returning one would put ~528 KB on the stack.
void fill_sparse_l2(bx::L2Book& book) {
    book.clear();
    const auto bids = sparse_prices(512, 32'000, 1'000);
    const auto asks = sparse_prices(33'536, 65'000, 1'000);
    for (std::size_t i = 0; i < bids.size(); ++i) {
        book.set_level(bx::Side::Bid, bids[i], 100U + static_cast<std::uint32_t>(i % 100));
        book.set_level(bx::Side::Ask, asks[i], 100U + static_cast<std::uint32_t>((i * 7) % 100));
    }
}

class L2TopBench final {
public:
    L2TopBench(const std::size_t levels, const bool sweep) : levels_(levels), sweep_(sweep) {}
    void reset(int) { fill_sparse_l2(book_); }
    [[nodiscard]] std::uint64_t operator()(const std::size_t begin, const std::size_t end) const {
        std::uint64_t hash = 0;
        for (std::size_t i = begin; i < end; ++i) {
            const auto side = (i & 1) == 0 ? bx::Side::Bid : bx::Side::Ask;
            if (sweep_) {
                const auto result = book_.sweep(side, book_.total_quantity(side));
                hash = bx::mix64(hash ^ result.notional_ticks ^ result.filled_quantity ^ result.levels_visited);
            } else {
                hash = bx::mix64(hash ^ book_.top_checksum(side, levels_));
            }
        }
        return hash;
    }
    [[nodiscard]] std::uint64_t finish() const { return book_.state_hash(); }
private:
    std::size_t levels_{};
    bool sweep_{};
    bx::L2Book book_;
};

struct RestingOrder final {
    std::uint32_t id{};
    bx::Side side{};
    std::uint16_t price{};
    std::uint32_t quantity{};
};

[[nodiscard]] std::vector<RestingOrder> make_resting_orders(const std::size_t count) {
    const auto bids = sparse_prices(8'192, 32'000, 2'048);
    const auto asks = sparse_prices(33'536, 57'344, 2'048);
    std::vector<RestingOrder> orders;
    orders.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto side = (i & 1) == 0 ? bx::Side::Bid : bx::Side::Ask;
        const auto& prices = side == bx::Side::Bid ? bids : asks;
        orders.push_back(RestingOrder{
            static_cast<std::uint32_t>(i + 1),
            side,
            prices[(i * 2'654'435'761ULL) % prices.size()],
            1U + static_cast<std::uint32_t>(i % 1'000),
        });
    }
    return orders;
}

class L3AddBench final {
public:
    explicit L3AddBench(const std::vector<RestingOrder>& orders)
        : orders_(orders), book_(orders.size() + 1, orders.size() + 2) {}
    void reset(int) { book_ = bx::L3Book(orders_.size() + 1, orders_.size() + 2); }
    [[nodiscard]] std::uint64_t operator()(const std::size_t begin, const std::size_t end) {
        std::uint64_t hash = 0;
        for (std::size_t i = begin; i < end; ++i) {
            const auto& order = orders_[i];
            hash += static_cast<std::uint64_t>(book_.add_resting(order.id, order.side, order.price, order.quantity));
            hash ^= static_cast<std::uint64_t>(book_.best_bid() + 1);
            hash += static_cast<std::uint64_t>(book_.best_ask() + 1);
        }
        return hash;
    }
    [[nodiscard]] std::uint64_t finish() const { return book_.state_hash(); }
private:
    const std::vector<RestingOrder>& orders_;
    bx::L3Book book_;
};

class L3ExistingBench final {
public:
    enum class Operation { Reduce, Replace, Cancel };

    L3ExistingBench(const std::vector<RestingOrder>& orders, const Operation operation)
        : orders_(orders), operation_(operation), ids_(orders.size()),
          book_(orders.size() + 1, orders.size() * 2 + 8) {
        std::iota(ids_.begin(), ids_.end(), 0U);
        std::sort(ids_.begin(), ids_.end(), [](const auto a, const auto b) {
            return (static_cast<std::uint64_t>(a) * 2'654'435'761ULL & 0xffff'ffffULL)
                 < (static_cast<std::uint64_t>(b) * 2'654'435'761ULL & 0xffff'ffffULL);
        });
    }

    void reset(int) {
        book_ = bx::L3Book(orders_.size() + 1, orders_.size() * 2 + 8);
        for (const auto& order : orders_) {
            (void)book_.add_resting(order.id, order.side, order.price, order.quantity);
        }
    }

    [[nodiscard]] std::uint64_t operator()(const std::size_t begin, const std::size_t end) {
        std::uint64_t hash = 0;
        for (std::size_t i = begin; i < end; ++i) {
            const auto index = ids_[i];
            const auto& order = orders_[index];
            std::uint64_t result_code{};
            switch (operation_) {
                case Operation::Reduce:
                    result_code = static_cast<std::uint64_t>(book_.reduce_quantity(
                        order.id, std::max(1U, order.quantity / 2)));
                    break;
                case Operation::Replace: {
                    const auto replacement_id = static_cast<std::uint32_t>(orders_.size()) + order.id;
                    const auto result = book_.replace_order(
                        order.id,
                        replacement_id,
                        static_cast<std::uint16_t>(order.price ^ 31U),
                        order.quantity + 1);
                    result_code = static_cast<std::uint64_t>(result.reject_reason) ^ result.report_hash;
                    break;
                }
                case Operation::Cancel:
                    result_code = static_cast<std::uint64_t>(book_.cancel(order.id));
                    break;
            }
            hash = bx::mix64(hash ^ result_code ^ book_.live_orders());
        }
        return hash;
    }

    [[nodiscard]] std::uint64_t finish() const { return book_.state_hash(); }

private:
    const std::vector<RestingOrder>& orders_;
    Operation operation_{};
    std::vector<std::uint32_t> ids_;
    bx::L3Book book_;
};

class MatchOneBench final {
public:
    explicit MatchOneBench(const std::size_t operations)
        : operations_(operations), book_(operations + 1, operations * 2 + 8) {}
    void reset(int) {
        book_ = bx::L3Book(operations_ + 1, operations_ * 2 + 8);
        for (std::size_t i = 0; i < operations_; ++i) {
            (void)book_.add_resting(static_cast<std::uint32_t>(i + 1), bx::Side::Ask, 40'000, 1);
        }
    }
    [[nodiscard]] std::uint64_t operator()(const std::size_t begin, const std::size_t end) {
        std::uint64_t hash = 0;
        for (std::size_t i = begin; i < end; ++i) {
            const auto result = book_.submit_limit(
                static_cast<std::uint32_t>(operations_ + i + 1),
                bx::Side::Bid,
                40'000,
                1,
                bx::TimeInForce::ImmediateOrCancel);
            hash = bx::mix64(hash ^ result.report_hash ^ result.traded_quantity);
        }
        return hash;
    }
    [[nodiscard]] std::uint64_t finish() const { return book_.state_hash(); }
private:
    std::size_t operations_{};
    bx::L3Book book_;
};

class MatchManyBench final {
public:
    MatchManyBench(const std::size_t operations, const std::uint32_t makers_per_taker)
        : operations_(operations), makers_per_taker_(makers_per_taker),
          maker_count_(operations * makers_per_taker),
          book_(maker_count_ + 1, maker_count_ + operations + 8) {}
    void reset(int) {
        book_ = bx::L3Book(maker_count_ + 1, maker_count_ + operations_ + 8);
        for (std::size_t i = 0; i < maker_count_; ++i) {
            (void)book_.add_resting(static_cast<std::uint32_t>(i + 1), bx::Side::Ask, 40'000, 1);
        }
    }
    [[nodiscard]] std::uint64_t operator()(const std::size_t begin, const std::size_t end) {
        std::uint64_t hash = 0;
        for (std::size_t i = begin; i < end; ++i) {
            const auto result = book_.submit_limit(
                static_cast<std::uint32_t>(maker_count_ + i + 1),
                bx::Side::Bid,
                40'000,
                makers_per_taker_,
                bx::TimeInForce::ImmediateOrCancel);
            hash = bx::mix64(hash ^ result.report_hash ^ result.fills);
        }
        return hash;
    }
    [[nodiscard]] std::uint64_t finish() const { return book_.state_hash(); }
private:
    std::size_t operations_{};
    std::uint32_t makers_per_taker_{};
    std::size_t maker_count_{};
    bx::L3Book book_;
};

[[nodiscard]] SampleStats bench_sparse_1000_sweep(const int runs, std::uint64_t& sink) {
    std::vector<double> samples;
    const auto prices = sparse_prices(33'536, 65'000, 1'000);
    const auto sample_count = 32 * static_cast<std::size_t>(runs);
    samples.reserve(sample_count);

    for (std::size_t sample = 0; sample < sample_count; ++sample) {
        bx::L3Book book(1'001, 2'100);
        for (std::size_t i = 0; i < prices.size(); ++i) {
            (void)book.add_resting(static_cast<std::uint32_t>(i + 1), bx::Side::Ask, prices[i], 1);
        }
        const auto start = std::chrono::steady_clock::now();
        const auto result = book.submit_limit(
            2'000, bx::Side::Bid, 65'535, 1'000, bx::TimeInForce::FillOrKill);
        const auto stop = std::chrono::steady_clock::now();
        do_not_optimize(result.report_hash);
        sink = bx::mix64(sink ^ result.report_hash ^ book.state_hash());
        samples.push_back(std::chrono::duration<double, std::nano>(stop - start).count());
    }
    return summarize(std::move(samples));
}

enum class CommandType : std::uint8_t { Add, Cancel, Reduce, Replace, Match };
struct Command final {
    CommandType type{};
    std::uint32_t id{};
    std::uint32_t replacement_id{};
    bx::Side side{};
    std::uint16_t price{};
    std::uint32_t quantity{};
};

[[nodiscard]] std::vector<Command> make_mixed_commands(const std::size_t count) {
    bx::L3Book model(count + 4'096, count * 2 + 8'192);
    std::vector<Command> commands;
    commands.reserve(count);
    std::vector<std::uint32_t> created;
    created.reserve(count);
    std::vector<bx::Side> side_by_id(count * 2 + 8'192, bx::Side::Bid);
    Rng rng(0x3db1'0914'a2fc'8557ULL);
    std::uint32_t next_id = 1;

    const auto choose_live = [&]() -> std::optional<std::uint32_t> {
        for (int attempt = 0; attempt < 16 && !created.empty(); ++attempt) {
            const auto id = created[rng.next() % created.size()];
            if (model.contains(id)) {
                return id;
            }
        }
        return std::nullopt;
    };

    while (commands.size() < count) {
        const auto random = rng.next();
        auto action = static_cast<std::uint32_t>(random % 100);
        if (created.empty()) {
            action = 0;
        }

        if (action < 50) {
            const auto side = ((random >> 8) & 1) == 0 ? bx::Side::Bid : bx::Side::Ask;
            const auto price = static_cast<std::uint16_t>(side == bx::Side::Bid
                ? 32'000 - static_cast<std::uint16_t>((random >> 16) & 2'047)
                : 33'536 + static_cast<std::uint16_t>((random >> 16) & 2'047));
            const auto quantity = 1U + static_cast<std::uint32_t>((random >> 32) % 100);
            const auto id = next_id++;
            if (bx::succeeded(model.add_resting(id, side, price, quantity))) {
                commands.push_back(Command{CommandType::Add, id, 0, side, price, quantity});
                created.push_back(id);
                side_by_id[id] = side;
            }
        } else if (action < 70) {
            if (const auto id = choose_live()) {
                (void)model.cancel(*id);
                commands.push_back(Command{CommandType::Cancel, *id, 0, side_by_id[*id], 0, 0});
            }
        } else if (action < 80) {
            if (const auto id = choose_live()) {
                const auto current = model.order(*id);
                const auto quantity = current.has_value() ? std::max(1U, current->quantity / 2) : 1U;
                (void)model.reduce_quantity(*id, quantity);
                commands.push_back(Command{CommandType::Reduce, *id, 0, side_by_id[*id], 0, quantity});
            }
        } else if (action < 90) {
            if (const auto id = choose_live()) {
                const auto side = side_by_id[*id];
                const auto price = static_cast<std::uint16_t>(side == bx::Side::Bid
                    ? 32'000 - static_cast<std::uint16_t>((random >> 20) & 2'047)
                    : 33'536 + static_cast<std::uint16_t>((random >> 20) & 2'047));
                const auto quantity = 1U + static_cast<std::uint32_t>((random >> 40) % 200);
                const auto replacement_id = next_id++;
                const auto result = model.replace_order(*id, replacement_id, price, quantity);
                if (result.accepted()) {
                    commands.push_back(Command{
                        CommandType::Replace, *id, replacement_id, side, price, quantity});
                    created.push_back(replacement_id);
                    side_by_id[replacement_id] = side;
                }
            }
        } else {
            const auto side = ((random >> 9) & 1) == 0 ? bx::Side::Bid : bx::Side::Ask;
            const auto price = side == bx::Side::Bid ? std::uint16_t{65'535} : std::uint16_t{0};
            const auto quantity = 1U + static_cast<std::uint32_t>((random >> 32) % 150);
            const auto id = next_id++;
            (void)model.submit_limit(
                id, side, price, quantity, bx::TimeInForce::ImmediateOrCancel);
            commands.push_back(Command{CommandType::Match, id, 0, side, price, quantity});
        }
    }
    return commands;
}

class MixedBench final {
public:
    explicit MixedBench(const std::vector<Command>& commands)
        : commands_(commands), book_(commands.size() + 4'096, commands.size() * 2 + 8'192) {}
    void reset(int) {
        book_ = bx::L3Book(commands_.size() + 4'096, commands_.size() * 2 + 8'192);
    }
    [[nodiscard]] std::uint64_t operator()(const std::size_t begin, const std::size_t end) {
        std::uint64_t hash = 0;
        for (std::size_t i = begin; i < end; ++i) {
            const auto& command = commands_[i];
            switch (command.type) {
                case CommandType::Add:
                    hash ^= static_cast<std::uint64_t>(
                        book_.add_resting(command.id, command.side, command.price, command.quantity));
                    break;
                case CommandType::Cancel:
                    hash ^= static_cast<std::uint64_t>(book_.cancel(command.id));
                    break;
                case CommandType::Reduce:
                    hash ^= static_cast<std::uint64_t>(book_.reduce_quantity(command.id, command.quantity));
                    break;
                case CommandType::Replace:
                    hash ^= book_.replace_order(
                        command.id, command.replacement_id, command.price, command.quantity).report_hash;
                    break;
                case CommandType::Match: {
                    const auto result = book_.submit_limit(
                        command.id,
                        command.side,
                        command.price,
                        command.quantity,
                        bx::TimeInForce::ImmediateOrCancel);
                    hash ^= result.report_hash ^ result.traded_quantity;
                    break;
                }
            }
            hash = bx::mix64(hash ^ book_.live_orders());
        }
        return hash;
    }
    [[nodiscard]] std::uint64_t finish() const { return book_.state_hash(); }
private:
    const std::vector<Command>& commands_;
    bx::L3Book book_;
};

void print_header(const int cpu, const int runs) {
    std::cout << "\nBitmap Exchange Hot-Path Benchmark (C++23)\n";
    std::cout << "CPU: " << cpu_model() << " | pinned core: " << cpu << " | runs: " << runs << '\n';
    std::cout << "Layout: 65,536 ticks | 3-tier occupancy bitmap | OrderSlot=" << sizeof(bx::OrderSlot)
              << " B | PriceLevel=" << sizeof(bx::PriceLevel) << " B\n";
    std::cout << "Numbers are batch-normalized throughput-equivalent service times, not end-to-end latency percentiles.\n\n";
}

void print_section(const std::string_view title, const std::vector<BenchResult>& results) {
    std::cout << title << '\n';
    std::cout << std::string(118, '-') << '\n';
    std::cout << std::left << std::setw(36) << "Scenario"
              << std::right << std::setw(11) << "p50 ns"
              << std::setw(11) << "p95 ns"
              << std::setw(11) << "p99 ns"
              << std::setw(11) << "p99.9"
              << std::setw(12) << "Mops/s"
              << std::setw(13) << "ns/item"
              << std::setw(13) << "samples" << '\n';
    std::cout << std::string(118, '-') << '\n';
    for (const auto& result : results) {
        std::cout << std::left << std::setw(36) << result.scenario
                  << std::right << std::fixed << std::setprecision(2)
                  << std::setw(11) << result.stats.p50
                  << std::setw(11) << result.stats.p95
                  << std::setw(11) << result.stats.p99
                  << std::setw(11) << result.stats.p999
                  << std::setw(12) << (1'000.0 / result.stats.p50)
                  << std::setw(13) << (result.stats.p50 / result.work_per_operation)
                  << std::setw(13) << result.samples << '\n';
    }
    std::cout << '\n';
}

} // namespace

int main(int argc, char** argv) {
    const bool quick = argc > 1 && std::string_view(argv[1]) == "--quick";
    const int runs = quick ? 1 : 3;
    const std::size_t l2_updates_count = quick ? 300'000 : 2'000'000;
    const std::size_t l3_count = quick ? 80'000 : 300'000;
    const int cpu = pin_first_allowed_cpu();
    std::uint64_t sink = 0;

    print_header(cpu, runs);

    const auto l2_updates = make_l2_updates(l2_updates_count);
    // Each L2 fixture embeds a bx::L2Book (~528 KB of inline ladder), so these
    // are heap-allocated; four of them as stack locals overflows the 1 MB
    // default thread stack on Windows.
    const auto l2_update_bench = std::make_unique<L2UpdateBench>(l2_updates);
    auto l2_update_stats = measure_batches(l2_updates_count, 4'096, runs, *l2_update_bench, sink);

    const auto top10 = std::make_unique<L2TopBench>(10, false);
    const auto top10_ops = quick ? 20'000U : 150'000U;
    auto top10_stats = measure_batches(top10_ops, 128, runs, *top10, sink);

    const auto top1000 = std::make_unique<L2TopBench>(1'000, false);
    const auto top1000_ops = quick ? 1'000U : 12'000U;
    auto top1000_stats = measure_batches(top1000_ops, 8, runs, *top1000, sink);

    const auto vwap1000 = std::make_unique<L2TopBench>(1'000, true);
    auto vwap1000_stats = measure_batches(top1000_ops, 8, runs, *vwap1000, sink);

    print_section("L2 bitmap ladder — sparse occupancy", {
        {"set level + cached BBO", "update", l2_update_stats, 1.0,
            (l2_updates_count + 4'095) / 4'096 * static_cast<std::size_t>(runs)},
        {"top 10 sparse levels", "query", top10_stats, 10.0,
            (top10_ops + 127) / 128 * static_cast<std::size_t>(runs)},
        {"top 1,000 sparse levels", "query", top1000_stats, 1'000.0,
            (top1000_ops + 7) / 8 * static_cast<std::size_t>(runs)},
        {"VWAP across 1,000 sparse levels", "query", vwap1000_stats, 1'000.0,
            (top1000_ops + 7) / 8 * static_cast<std::size_t>(runs)},
    });

    const auto orders = make_resting_orders(l3_count);
    L3AddBench add_bench(orders);
    const auto add_stats = measure_batches(l3_count, 2'048, runs, add_bench, sink);

    L3ExistingBench reduce_bench(orders, L3ExistingBench::Operation::Reduce);
    const auto reduce_stats = measure_batches(l3_count, 2'048, runs, reduce_bench, sink);

    L3ExistingBench replace_bench(orders, L3ExistingBench::Operation::Replace);
    const auto replace_stats = measure_batches(l3_count, 2'048, runs, replace_bench, sink);

    L3ExistingBench cancel_bench(orders, L3ExistingBench::Operation::Cancel);
    const auto cancel_stats = measure_batches(l3_count, 2'048, runs, cancel_bench, sink);

    const auto match_one_count = quick ? 50'000U : 250'000U;
    MatchOneBench match_one(match_one_count);
    const auto match_one_stats = measure_batches(match_one_count, 1'024, runs, match_one, sink);

    const auto match64_count = quick ? 1'024U : 6'144U;
    MatchManyBench match64(match64_count, 64);
    const auto match64_stats = measure_batches(match64_count, 32, runs, match64, sink);

    const auto sparse1000_stats = bench_sparse_1000_sweep(runs, sink);

    const auto mixed_count = quick ? 80'000U : 300'000U;
    const auto commands = make_mixed_commands(mixed_count);
    MixedBench mixed(commands);
    const auto mixed_stats = measure_batches(mixed_count, 2'048, runs, mixed, sink);

    print_section("L3 FIFO order book + matching engine", {
        {"resting add + cached BBO", "order", add_stats, 1.0,
            (l3_count + 2'047) / 2'048 * static_cast<std::size_t>(runs)},
        {"same-price quantity reduction", "reduce", reduce_stats, 1.0,
            (l3_count + 2'047) / 2'048 * static_cast<std::size_t>(runs)},
        {"replace (loses FIFO priority)", "replace", replace_stats, 1.0,
            (l3_count + 2'047) / 2'048 * static_cast<std::size_t>(runs)},
        {"direct-ID random cancel", "cancel", cancel_stats, 1.0,
            (l3_count + 2'047) / 2'048 * static_cast<std::size_t>(runs)},
        {"match 1 FIFO maker", "taker", match_one_stats, 1.0,
            (match_one_count + 1'023) / 1'024 * static_cast<std::size_t>(runs)},
        {"match 64 FIFO makers", "taker", match64_stats, 64.0,
            (match64_count + 31) / 32 * static_cast<std::size_t>(runs)},
        {"sweep 1,000 sparse price levels", "taker", sparse1000_stats, 1'000.0,
            32U * static_cast<std::size_t>(runs)},
        {"mixed add/cancel/reduce/replace/match", "message", mixed_stats, 1.0,
            (mixed_count + 2'047) / 2'048 * static_cast<std::size_t>(runs)},
    });

    std::cout << "correctness gate: run ctest --test-dir build --output-on-failure\n";
    std::cout << "anti-optimization sink: 0x" << std::hex << sink << std::dec << '\n';
    return 0;
}

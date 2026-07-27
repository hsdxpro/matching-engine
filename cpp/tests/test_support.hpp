#pragma once

#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bitmap_exchange::test {

class Failure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

inline void require(
    const bool condition,
    const std::string_view expression,
    const std::string_view file,
    const int line,
    const std::string_view detail = {}) {
    if (condition) {
        return;
    }
    std::ostringstream message;
    message << file << ':' << line << ": requirement failed: " << expression;
    if (!detail.empty()) {
        message << " (" << detail << ')';
    }
    throw Failure(message.str());
}

class Runner final {
public:
    template <class Test>
    void run(const std::string_view name, Test&& test) {
        try {
            std::invoke(std::forward<Test>(test));
            ++passed_;
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& error) {
            ++failed_;
            std::cerr << "[FAIL] " << name << "\n       " << error.what() << '\n';
        } catch (...) {
            ++failed_;
            std::cerr << "[FAIL] " << name << "\n       unknown exception\n";
        }
    }

    [[nodiscard]] int finish() const {
        std::cout << "\n" << passed_ << " passed, " << failed_ << " failed\n";
        return failed_ == 0 ? 0 : 1;
    }

private:
    std::uint32_t passed_{};
    std::uint32_t failed_{};
};

class Rng final {
public:
    explicit Rng(const std::uint64_t seed) noexcept : state_(seed) {}

    [[nodiscard]] std::uint64_t next() noexcept {
        state_ += 0x9e37'79b9'7f4a'7c15ULL;
        auto value = state_;
        value = (value ^ (value >> 30)) * 0xbf58'476d'1ce4'e5b9ULL;
        value = (value ^ (value >> 27)) * 0x94d0'49bb'1331'11ebULL;
        return value ^ (value >> 31);
    }

private:
    std::uint64_t state_;
};

} // namespace bitmap_exchange::test

#define BX_REQUIRE(expression) \
    ::bitmap_exchange::test::require(static_cast<bool>(expression), #expression, __FILE__, __LINE__)

#define BX_REQUIRE_MSG(expression, detail) \
    ::bitmap_exchange::test::require(static_cast<bool>(expression), #expression, __FILE__, __LINE__, detail)

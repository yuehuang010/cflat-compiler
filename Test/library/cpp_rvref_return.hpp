#pragma once

#include <optional>
#include <utility>

namespace cppi_rvref {
    inline int constructors = 0;
    inline int copies = 0;
    inline int moves = 0;
    inline int destructors = 0;

    inline void reset_counts() noexcept
    { constructors = copies = moves = destructors = 0; }

    struct Counted
    {
        int n;
        explicit Counted(int value) noexcept : n(value) { ++constructors; }
        Counted(const Counted& other) noexcept : n(other.n) { ++copies; }
        Counted(Counted&& other) noexcept : n(other.n) { other.n = -1; ++moves; }
        ~Counted() noexcept { ++destructors; }
        int value() const noexcept { return n; }
    };

    struct Owner
    {
        Counted item;
        explicit Owner(int value) noexcept : item(value) {}
        Counted&& value() && noexcept { return std::move(item); }
        int peek() const noexcept { return item.value(); }
    };

    inline std::optional<Counted> make_optional(int value)
    { return std::optional<Counted>(std::in_place, value); }
    inline const Counted&& copy_result(const Counted& value) noexcept
    { return std::move(value); }

    inline int by_value(Counted value) noexcept { return value.value(); }
    inline int by_const_ref(const Counted& value) noexcept { return value.value(); }
    inline int by_rvalue_ref(Counted&& value) noexcept { return value.value() + 100; }
}

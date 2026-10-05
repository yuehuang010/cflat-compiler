#pragma once
#include <utility>
namespace cflat_t14 {
template<class = void>
struct GenericFunctor {
    template<class T, class U>
    auto operator()(T&& left, U&& right) const
        -> decltype(std::forward<T>(left) * 10 + std::forward<U>(right))
    { return std::forward<T>(left) * 10 + std::forward<U>(right); }
};
}

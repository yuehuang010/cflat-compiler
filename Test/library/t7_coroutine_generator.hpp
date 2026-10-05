#pragma once
#include <coroutine>

namespace t7coro {
struct generator {
    struct promise_type;
    using handle_type = std::coroutine_handle<promise_type>;

    struct promise_type {
        int value = 0;
        generator get_return_object() { return generator(handle_type::from_promise(*this)); }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        std::suspend_always yield_value(int next) noexcept { value = next; return {}; }
        void return_void() noexcept {}
        void unhandled_exception() { throw; }
    };

    explicit generator(handle_type h) : coro(h) {}
    generator(generator&& other) noexcept : coro(other.coro) { other.coro = nullptr; }
    ~generator() { if (coro) coro.destroy(); }
    bool next() { if (!coro || coro.done()) return false; coro.resume(); return !coro.done(); }
    int value() const { return coro.promise().value; }

private:
    handle_type coro;
};

inline generator generate_sum(int n) {
    int sum = 0;
    for (int i = 1; i <= n; ++i) {
        sum += i;
        co_yield sum;
    }
}

inline int gen_sum(int n) {
    auto g = generate_sum(n);
    int sum = 0;
    while (g.next()) sum = g.value();
    return sum;
}
}

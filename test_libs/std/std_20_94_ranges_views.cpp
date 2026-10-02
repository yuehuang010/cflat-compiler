// C++20 equivalent of std_20_94_ranges_views.cb (compile-time parity baseline)
#include <functional>
#include <ranges>
#include <string>

static bool is_even(int x) { return x % 2 == 0; }
static int triple(int x) { return x * 3; }

int main()
{
    int failures = 0;
    std::function<bool(int)> even_fn(is_even);
    std::function<int(int)> triple_fn(triple);
    auto view = std::views::iota(0, 8)
        | std::views::filter(even_fn)
        | std::views::transform(triple_fn)
        | std::views::take(2);
    int sum = 0;
    for (int x : view) sum += x;
    if (sum != 6) failures |= 1;
    return failures;
}

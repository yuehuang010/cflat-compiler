#include <string>
#include <cstdio>
#include <coroutine>
int main()
{
    auto noop = std::noop_coroutine();
    bool ok = !noop.done() && noop.address() != nullptr;
    noop.resume();
    if (!(ok && !noop.done())) { std::puts("FAIL noop coroutine"); return 1; }
    std::puts("PASS std_full_20_909_noop_coroutine"); return 0;
}

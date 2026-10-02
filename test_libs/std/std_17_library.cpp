// C++20 equivalent of std_17_library.cb (compile-time parity baseline)
#include <string>
#include <algorithm>
#include <functional>
#include <map>
#include <mutex>
#include <numeric>
#include <random>
#include <shared_mutex>
#include <set>
#include <cstdio>
#include <cstdlib>

int twice(int x) { return x * 2; }
int main() {
    int failures = 0;
    std::map<int, int> a, b; a.try_emplace(1, 10); a.insert_or_assign(2, 20);
    auto node = a.extract(1); b.insert(std::move(node)); b.merge(a);
    auto tup = std::make_tuple(3, 4); int applied = std::apply([](int x, int y) { return x + y; }, tup);
    int invoked = std::invoke(twice, 6);
    int clamped = std::clamp(17, 1, 9); int gcdv = std::gcd(42, 30), lcmv = std::lcm(6, 8);
    int values[] = {1, 2, 3, 4}; int red = std::reduce(values, values + 4);
    int tr = std::transform_reduce(values, values + 4, 0, std::plus<>(), [](int x) { return x * 2; });
    int inc[4], exc[4]; std::inclusive_scan(values, values + 4, inc); std::exclusive_scan(values, values + 4, exc, 0);
    int sz = std::size(values), empty = std::empty(values), first = *std::data(values);
    std::mutex m1, m2; int lock_state = 0; { std::scoped_lock lock(m1, m2); lock_state = 1; }
    std::shared_mutex sm; { std::shared_lock lock(sm); lock_state = 2; }
    int parsed = 1234; const int& cref = std::as_const(parsed);
    std::mt19937 rng(123); std::vector<int> sample; std::sample(values, values + 4, std::back_inserter(sample), 2, rng);
#define CHECK(name, expr) do { if (!(expr)) { std::printf("FAIL %s\n", name); failures |= 1; } } while (0)
    CHECK("node_api", b.size() == 2 && b.at(1) == 10 && b.at(2) == 20);
    CHECK("apply_invoke", applied == 7 && invoked == 12); CHECK("clamp_gcd_lcm", clamped == 9 && gcdv == 6 && lcmv == 24);
    CHECK("numeric_algorithms", red == 10 && tr == 20 && inc[3] == 10 && exc[3] == 6);
    CHECK("free_accessors", sz == 4 && !empty && first == 1); CHECK("locks", lock_state == 2);
   
    CHECK("as_const", &cref == &parsed); CHECK("sample", sample.size() == 2);
    if (!failures) std::printf("PASS std_17_library\n");
    return failures;
}

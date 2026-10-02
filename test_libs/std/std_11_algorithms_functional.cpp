// C++20 equivalent of std_11_algorithms_functional.cb (compile-time parity baseline)
#include <algorithm>
#include <functional>
#include <numeric>
#include <string>
#include <vector>
#include <cstdio>

int twice(int x) { return x * 2; }
bool is_three(int x) { return x == 3; }
bool over_two(int x) { return x > 2; }
bool is_even(int x) { return x % 2 == 0; }
bool is_positive(int x) { return x > 0; }
bool is_four(int x) { return x == 4; }
int plus_three(int x) { return x + 3; }
int plus_one(int x) { return x + 1; }
int main() {
    int failures = 0;
    std::vector<int> v{4, 1, 3, 2}; std::sort(v.begin(), v.end());
    auto it = std::find_if(v.begin(), v.end(), std::function<bool(int)>(is_three)); int found = it == v.end() ? -1 : *it;
    int exact = *std::find(v.begin(), v.end(), 2);
    int cnt = (int)std::count_if(v.begin(), v.end(), std::function<bool(int)>(over_two));
    std::vector<int> out; std::copy_if(v.begin(), v.end(), std::back_inserter(out), std::function<bool(int)>(is_even));
    std::transform(v.begin(), v.end(), v.begin(), std::function<int(int)>(plus_one));
    if (found != 3 || exact != 2 || cnt != 2 || out.size() != 2 || out[0] != 2 || out[1] != 4 || v[0] != 2 || !std::is_sorted(v.begin(), v.end()) || std::accumulate(v.begin(), v.end(), 0) != 14 || std::inner_product(v.begin(), v.end(), v.begin(), 0) != 54) { std::printf("FAIL algorithms\n"); failures |= 1; }
    std::vector<int> seq{4, 1, 3, 2, 3}; std::stable_sort(seq.begin(), seq.end());
    int lower = *std::lower_bound(seq.begin(), seq.end(), 3);
    auto mm = std::minmax_element(seq.begin(), seq.end()); int minv = *mm.first; int maxv = *mm.second;
    std::reverse(seq.begin(), seq.end()); std::rotate(seq.begin(), seq.begin() + 1, seq.end());
    auto unique_end = std::unique(seq.begin(), seq.end());
    std::vector<int> numbers{1, 2, 3}; std::iota(numbers.begin(), numbers.end(), 1);
    if (!std::all_of(numbers.begin(), numbers.end(), std::function<bool(int)>(is_positive)) || !std::any_of(numbers.begin(), numbers.end(), std::function<bool(int)>(is_three)) || !std::none_of(numbers.begin(), numbers.end(), std::function<bool(int)>(is_four)) || lower != 3 || minv != 1 || maxv != 4 || unique_end == seq.begin() || numbers[2] != 3) { std::printf("FAIL algorithm predicates/bounds\n"); failures |= 4; }
    std::function<int(int)> f = twice;
    int referenced = 6; auto wrapped = std::ref(referenced);
    if (f(5) != 10 || std::less<int>{}(1, 2) != true || std::greater<int>{}(2, 1) != true || wrapped.get() != 6) { std::printf("FAIL functional\n"); failures |= 2; }
    if (!failures) std::printf("PASS std_11_algorithms_functional\n");
    return failures;
}

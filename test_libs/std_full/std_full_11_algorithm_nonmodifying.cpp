// C++20 equivalent of std_full_11_algorithm_nonmodifying.cb
#include <string>
#include <algorithm>
#include <functional>
#include <vector>
#include <cstdio>

static bool is_even(int x) { return x % 2 == 0; }
static bool is_three(int x) { return x == 3; }
static int sum_acc = 0;
static void add_to_sum(int x) { sum_acc += x; }

int main() {
    int failures = 0;
    std::vector<int> v{1, 2, 3, 2, 3, 3, 4};
    std::function<bool(int)> even = is_even;
    std::function<bool(int)> three = is_three;
    std::function<void(int)> adder = add_to_sum;
    std::for_each(v.begin(), v.end(), adder);
    std::for_each_n(v.begin(), 3, adder);
    if (sum_acc != 18 + 6) { std::printf("FAIL for_each\n"); failures |= 1; }
    auto fin = std::find_if_not(v.begin(), v.end(), even);
    auto fi3 = std::find_if(v.begin(), v.end(), three);
    auto ff = std::find(v.begin(), v.end(), 9);
    if (std::count(v.begin(), v.end(), 3) != 3 || std::count_if(v.begin(), v.end(), even) != 3 || *fin != 1 || std::distance(v.begin(), fi3) != 2 || ff != v.end()) { std::printf("FAIL count_find\n"); failures |= 2; }
    std::vector<int> hay{1, 2, 3, 1, 2, 3};
    std::vector<int> needle{2, 3};
    std::vector<int> any_of_set{9, 3};
    auto fe = std::find_end(hay.begin(), hay.end(), needle.begin(), needle.end());
    auto ffo = std::find_first_of(hay.begin(), hay.end(), any_of_set.begin(), any_of_set.end());
    auto aj = std::adjacent_find(v.begin(), v.end(), std::equal_to<int>());
    std::vector<int> nodup{1, 2, 3};
    auto aj2 = std::adjacent_find(nodup.begin(), nodup.end());
    if (std::distance(hay.begin(), fe) != 4 || std::distance(hay.begin(), ffo) != 2 || std::distance(v.begin(), aj) != 4 || aj2 != nodup.end()) { std::printf("FAIL find_end_first_of_adjacent\n"); failures |= 4; }
    auto se = std::search(hay.begin(), hay.end(), needle.begin(), needle.end());
    auto sn = std::search_n(v.begin(), v.end(), 2, 3);
    auto sn0 = std::search_n(v.begin(), v.end(), 4, 3);
    if (std::distance(hay.begin(), se) != 1 || std::distance(v.begin(), sn) != 4 || sn0 != v.end()) { std::printf("FAIL search\n"); failures |= 8; }
    std::vector<int> a{1, 2, 3}, b{1, 2, 4}, c{1, 2};
    bool lex = std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end());
    bool lex2 = std::lexicographical_compare(b.begin(), b.end(), a.begin(), a.end());
    bool lex3 = std::lexicographical_compare(c.begin(), c.end(), a.begin(), a.end());
    auto cmp = std::lexicographical_compare_three_way(a.begin(), a.end(), b.begin(), b.end());
    auto cmp_eq = std::lexicographical_compare_three_way(a.begin(), a.end(), a.begin(), a.end());
    if (!lex || lex2 || !lex3 || !(cmp < 0) || !(cmp_eq == 0)) { std::printf("FAIL lexicographical\n"); failures |= 16; }
    std::vector<int> p1{1, 2, 3, 4}, p2{4, 3, 2, 1}, p3{1, 2, 3, 5};
    bool perm = std::is_permutation(p1.begin(), p1.end(), p2.begin(), p2.end());
    bool notperm = std::is_permutation(p1.begin(), p1.end(), p3.begin(), p3.end());
    std::vector<int> np{1, 2, 3};
    bool n1 = std::next_permutation(np.begin(), np.end());
    bool n1ok = np[0] == 1 && np[1] == 3 && np[2] == 2;
    bool n2 = std::prev_permutation(np.begin(), np.end());
    bool n2ok = np[0] == 1 && np[1] == 2 && np[2] == 3;
    std::vector<int> last{3, 2, 1};
    bool wrapped = std::next_permutation(last.begin(), last.end());
    if (!perm || notperm || !n1 || !n1ok || !n2 || !n2ok || wrapped || last[0] != 1 || last[2] != 3) { std::printf("FAIL permutations\n"); failures |= 32; }
    if (!std::equal(a.begin(), a.end(), a.begin()) || std::equal(a.begin(), a.end(), b.begin()) || std::all_of(a.begin(), a.end(), three) || !std::any_of(a.begin(), a.end(), three) || std::none_of(a.begin(), a.end(), even)) { std::printf("FAIL equal_all_none\n"); failures |= 64; }
    auto mm = std::mismatch(a.begin(), a.end(), b.begin());
    if (std::distance(a.begin(), mm.first) != 2 || *mm.second != 4) { std::printf("FAIL mismatch\n"); failures |= 128; }
    if (failures == 0) std::printf("PASS std_full_11_algorithm_nonmodifying\n");
    return failures;
}

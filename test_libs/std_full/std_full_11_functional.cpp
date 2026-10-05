// C++20 equivalent of std_full_11_functional.cb (compile-time parity baseline)
#include <string>
#include <functional>
#include <algorithm>
#include <iterator>
#include <cstdio>

int add3(int a, int b, int c) { return a * 100 + b * 10 + c; }
int twice(int x) { return x * 2; }
int thrice(int x) { return x * 3; }

int main() {
    int failures = 0;
    if (std::plus<int>()(3, 4) != 7 || std::minus<int>()(9, 4) != 5 || std::multiplies<int>()(3, 5) != 15 || std::divides<int>()(20, 4) != 5 || std::modulus<int>()(17, 5) != 2 || std::negate<int>()(6) != -6)
    { printf("FAIL arithmetic objects\n"); failures |= 1; }
    if (!std::equal_to<int>()(2, 2) || !std::not_equal_to<int>()(2, 3) || !std::greater<int>()(3, 2) || !std::less<int>()(2, 3) || !std::greater_equal<int>()(3, 3) || !std::less_equal<int>()(3, 3))
    { printf("FAIL comparison objects\n"); failures |= 2; }
    if (!std::logical_and<bool>()(true, true) || std::logical_or<bool>()(false, false) || !std::logical_not<bool>()(false)
        || std::bit_and<int>()(12, 10) != 8 || std::bit_or<int>()(12, 10) != 14 || std::bit_xor<int>()(12, 10) != 6 || std::bit_not<int>()(0) != -1)
    { printf("FAIL logical bit objects\n"); failures |= 4; }
    if (std::identity()(5) != 5 || std::identity()(2.5) != 2.5) { printf("FAIL identity\n"); failures |= 8; }
    int val = 10;
    auto r = std::ref(val); auto cr = std::cref(val);
    r.get() = 11;
    if (val != 11 || cr.get() != 11) { printf("FAIL reference_wrapper\n"); failures |= 16; }
    std::function<int(int, int, int)> f3 = add3;
    auto bf = std::bind_front(f3, 4, 5);
    auto nf = std::not_fn(std::less<int>());
    if (bf(6) != 456 || !nf(3, 2) || nf(2, 3)) { printf("FAIL bind_front not_fn\n"); failures |= 32; }
    std::function<int(int)> f1 = twice, f2 = thrice, none;
    if (!f1 || none || f1(4) != 8) { printf("FAIL function bool\n"); failures |= 64; }
    std::swap(f1, f2);
    if (f1(4) != 12 || f2(4) != 8) { printf("FAIL function swap\n"); failures |= 128; }
    std::string hay = "the quick brown fox";
    std::string needle = "brown";
    auto bm = std::search(hay.begin(), hay.end(), std::boyer_moore_searcher<std::string::iterator>(needle.begin(), needle.end()));
    auto bmh = std::search(hay.begin(), hay.end(), std::boyer_moore_horspool_searcher<std::string::iterator>(needle.begin(), needle.end()));
    auto def = std::search(hay.begin(), hay.end(), std::default_searcher<std::string::iterator>(needle.begin(), needle.end()));
    std::string miss = "zzz";
    auto none_found = std::search(hay.begin(), hay.end(), std::default_searcher<std::string::iterator>(miss.begin(), miss.end()));
    if (std::distance(hay.begin(), bm) != 10 || std::distance(hay.begin(), bmh) != 10 || std::distance(hay.begin(), def) != 10 || none_found != hay.end()) { printf("FAIL searchers\n"); failures |= 256; }
    std::hash<int> hi; std::hash<std::string> hs;
    if (hi(5) != hi(5) || hs(std::string("k")) != hs(std::string("k"))) { printf("FAIL hash\n"); failures |= 512; }
    if (failures == 0) printf("PASS std_full_11_functional\n");
    return failures;
}

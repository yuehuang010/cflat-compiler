// C++20 equivalent of std_full_11_algorithm_modifying.cb
#include <string>
#include <algorithm>
#include <functional>
#include <iterator>
#include <random>
#include <vector>
#include <cstdio>

static bool is_even(int x) { return x % 2 == 0; }
static bool is_two(int x) { return x == 2; }
static int counter = 0;
static int next_val() { counter += 10; return counter; }
static int doubled(int x) { return x * 2; }

int main() {
    int failures = 0;
    std::vector<int> src{1, 2, 3, 4, 5};
    std::vector<int> dst(5, 0);
    std::copy(src.begin(), src.end(), dst.begin());
    std::vector<int> dst2(3, 0);
    std::copy_n(src.begin(), 3, dst2.begin());
    std::vector<int> dst3(7, 0);
    std::copy_backward(src.begin(), src.end(), dst3.end());
    std::vector<int> dst4;
    std::copy_if(src.begin(), src.end(), std::back_inserter(dst4), std::function<bool(int)>(is_even));
    if (dst[4] != 5 || dst2[2] != 3 || dst3[2] != 1 || dst3[6] != 5 || dst3[0] != 0 || dst4.size() != 2 || dst4[1] != 4) { std::printf("FAIL copy_family\n"); failures |= 1; }
    std::vector<std::string> ms{"a", "b", "c"};
    std::vector<std::string> md(3);
    std::move(ms.begin(), ms.end(), md.begin());
    std::vector<int> mb{1, 2, 3};
    std::vector<int> mbd(3, 0);
    std::move_backward(mb.begin(), mb.end(), mbd.end());
    if (md[0] != "a" || md[2] != "c" || mbd[0] != 1 || mbd[2] != 3) { std::printf("FAIL move_family\n"); failures |= 2; }
    std::vector<int> f(5, 1);
    std::fill(f.begin(), f.begin() + 2, 7);
    std::fill_n(f.begin() + 3, 2, 9);
    std::vector<int> g(4, 0);
    std::generate(g.begin(), g.end(), std::function<int()>(next_val));
    std::generate_n(f.begin(), 1, std::function<int()>(next_val));
    if (f[0] != 50 || f[1] != 7 || f[2] != 1 || f[3] != 9 || f[4] != 9 || g[0] != 10 || g[3] != 40) { std::printf("FAIL fill_generate\n"); failures |= 4; }
    std::vector<int> r{1, 2, 3, 2, 4};
    std::replace(r.begin(), r.end(), 4, 40);
    std::replace_if(r.begin(), r.end(), std::function<bool(int)>(is_two), 20);
    std::vector<int> rc(5, 0);
    std::replace_copy(r.begin(), r.end(), rc.begin(), 1, 100);
    if (r[1] != 20 || r[3] != 20 || r[4] != 40 || rc[0] != 100 || rc[2] != 3) { std::printf("FAIL replace\n"); failures |= 8; }
    std::vector<int> rm{1, 2, 3, 4, 5, 6};
    auto rm_end = std::remove_if(rm.begin(), rm.end(), std::function<bool(int)>(is_even));
    rm.erase(rm_end, rm.end());
    std::vector<int> rv{1, 2, 1, 3, 1};
    rv.erase(std::remove(rv.begin(), rv.end(), 1), rv.end());
    std::vector<int> un{1, 1, 2, 2, 2, 3, 1};
    un.erase(std::unique(un.begin(), un.end()), un.end());
    if (rm.size() != 3 || rm[0] != 1 || rm[2] != 5 || rv.size() != 2 || rv[1] != 3 || un.size() != 4 || un[3] != 1) { std::printf("FAIL remove_unique\n"); failures |= 16; }
    std::vector<int> sa{1, 2, 3}, sb{7, 8, 9};
    std::swap_ranges(sa.begin(), sa.end(), sb.begin());
    std::iter_swap(sa.begin(), sa.begin() + 2);
    std::vector<int> tr(3, 0);
    std::transform(sb.begin(), sb.end(), tr.begin(), std::function<int(int)>(doubled));
    std::vector<int> tr2(3, 0);
    std::transform(sb.begin(), sb.end(), sb.begin(), tr2.begin(), std::plus<int>());
    if (sa[0] != 9 || sa[2] != 7 || sb[0] != 1 || sb[2] != 3 || tr[1] != 4 || tr2[2] != 6) { std::printf("FAIL swap_transform\n"); failures |= 32; }
    std::vector<int> sh{1, 2, 3, 4, 5};
    auto sl = std::shift_left(sh.begin(), sh.end(), 2);
    bool sl_ok = sh[0] == 3 && sh[1] == 4 && sh[2] == 5 && std::distance(sh.begin(), sl) == 3;
    std::vector<int> sr{1, 2, 3, 4, 5};
    auto srr = std::shift_right(sr.begin(), sr.end(), 2);
    bool sr_ok = sr[2] == 1 && sr[3] == 2 && sr[4] == 3 && std::distance(sr.begin(), srr) == 2;
    if (!sl_ok || !sr_ok) { std::printf("FAIL shift\n"); failures |= 64; }
    std::vector<int> rev{1, 2, 3, 4};
    std::reverse(rev.begin(), rev.end());
    std::vector<int> rot{1, 2, 3, 4, 5};
    std::rotate(rot.begin(), rot.begin() + 2, rot.end());
    std::vector<int> revc(4, 0);
    std::reverse_copy(rev.begin(), rev.end(), revc.begin());
    if (rev[0] != 4 || rev[3] != 1 || rot[0] != 3 || rot[4] != 2 || revc[0] != 1) { std::printf("FAIL reverse_rotate\n"); failures |= 128; }
    std::vector<int> sh2{1, 2, 3, 4, 5, 6, 7, 8};
    std::mt19937 gen(42);
    std::shuffle(sh2.begin(), sh2.end(), gen);
    std::vector<int> orig{1, 2, 3, 4, 5, 6, 7, 8};
    if (!std::is_permutation(sh2.begin(), sh2.end(), orig.begin(), orig.end()) || sh2.size() != 8) { std::printf("FAIL shuffle\n"); failures |= 256; }
    if (failures == 0) std::printf("PASS std_full_11_algorithm_modifying\n");
    return failures;
}

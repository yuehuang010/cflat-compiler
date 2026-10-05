// C++20 equivalent of std_full_11_algorithm_order.cb
#include <string>
#include <algorithm>
#include <functional>
#include <iterator>
#include <vector>
#include <cstdio>

static bool is_even(int x) { return x % 2 == 0; }

int main() {
    int failures = 0;
    std::function<bool(int)> even = is_even;
    std::vector<int> p{1, 2, 3, 4, 5, 6};
    auto pp = std::partition(p.begin(), p.end(), even);
    bool part_ok = std::distance(p.begin(), pp) == 3 && std::is_partitioned(p.begin(), p.end(), even);
    for (int i = 0; i < 3; i++) part_ok = part_ok && p[i] % 2 == 0;
    std::vector<int> sp{1, 2, 3, 4, 5, 6};
    auto sps = std::stable_partition(sp.begin(), sp.end(), even);
    part_ok = part_ok && sp[0] == 2 && sp[1] == 4 && sp[2] == 6 && sp[3] == 1 && sp[5] == 5 && std::distance(sp.begin(), sps) == 3;
    auto pt = std::partition_point(sp.begin(), sp.end(), even);
    part_ok = part_ok && std::distance(sp.begin(), pt) == 3;
    std::vector<int> evens(3, 0), odds(3, 0);
    std::partition_copy(sp.begin(), sp.end(), evens.begin(), odds.begin(), even);
    part_ok = part_ok && evens[2] == 6 && odds[0] == 1;
    if (!part_ok) { std::printf("FAIL partition\n"); failures |= 1; }
    std::vector<int> ps{5, 3, 8, 1, 9, 2};
    std::partial_sort(ps.begin(), ps.begin() + 3, ps.end());
    std::vector<int> pc(2, 0);
    std::vector<int> pcsrc{5, 3, 8, 1};
    std::partial_sort_copy(pcsrc.begin(), pcsrc.end(), pc.begin(), pc.end());
    std::vector<int> ne{5, 3, 8, 1, 9, 2};
    std::nth_element(ne.begin(), ne.begin() + 2, ne.end());
    if (ps[0] != 1 || ps[1] != 2 || ps[2] != 3 || pc[0] != 1 || pc[1] != 3 || ne[2] != 3) { std::printf("FAIL partial_sort_nth\n"); failures |= 2; }
    std::vector<int> us{1, 2, 4, 3, 5};
    auto sorted_until = std::is_sorted_until(us.begin(), us.end());
    std::vector<int> sd{3, 1, 2};
    std::sort(sd.begin(), sd.end(), std::greater<int>());
    if (std::distance(us.begin(), sorted_until) != 3 || std::is_sorted(us.begin(), us.end()) || sd[0] != 3 || sd[2] != 1 || !std::is_sorted(sd.begin(), sd.end(), std::greater<int>())) { std::printf("FAIL is_sorted\n"); failures |= 4; }
    std::vector<int> bs{1, 2, 2, 2, 3, 5};
    auto lb = std::lower_bound(bs.begin(), bs.end(), 2);
    auto ub = std::upper_bound(bs.begin(), bs.end(), 2);
    auto er = std::equal_range(bs.begin(), bs.end(), 2);
    if (std::distance(bs.begin(), lb) != 1 || std::distance(bs.begin(), ub) != 4 || er.first != lb || er.second != ub || !std::binary_search(bs.begin(), bs.end(), 5) || std::binary_search(bs.begin(), bs.end(), 4)) { std::printf("FAIL binary_search_family\n"); failures |= 8; }
    std::vector<int> s1{1, 2, 3, 5}, s2{2, 3, 4};
    std::vector<int> u, i, d, sdif;
    std::set_union(s1.begin(), s1.end(), s2.begin(), s2.end(), std::back_inserter(u));
    std::set_intersection(s1.begin(), s1.end(), s2.begin(), s2.end(), std::back_inserter(i));
    std::set_difference(s1.begin(), s1.end(), s2.begin(), s2.end(), std::back_inserter(d));
    std::set_symmetric_difference(s1.begin(), s1.end(), s2.begin(), s2.end(), std::back_inserter(sdif));
    std::vector<int> sub{2, 3};
    if (u.size() != 5 || i.size() != 2 || i[0] != 2 || d.size() != 2 || d[1] != 5 || sdif.size() != 3 || !std::includes(s1.begin(), s1.end(), sub.begin(), sub.end()) || std::includes(s2.begin(), s2.end(), s1.begin(), s1.end())) { std::printf("FAIL set_ops\n"); failures |= 16; }
    std::vector<int> m1{1, 4, 7}, m2{2, 3, 9};
    std::vector<int> mo(6, 0);
    std::merge(m1.begin(), m1.end(), m2.begin(), m2.end(), mo.begin());
    std::vector<int> im{1, 4, 7, 2, 3, 9};
    std::inplace_merge(im.begin(), im.begin() + 3, im.end());
    if (mo[0] != 1 || mo[2] != 3 || mo[5] != 9 || !std::is_sorted(mo.begin(), mo.end()) || !std::is_sorted(im.begin(), im.end()) || im[3] != 4) { std::printf("FAIL merge\n"); failures |= 32; }
    std::vector<int> h{3, 1, 4, 1, 5, 9, 2, 6};
    std::make_heap(h.begin(), h.end());
    bool heap_ok = std::is_heap(h.begin(), h.end()) && h[0] == 9;
    h.push_back(10);
    std::push_heap(h.begin(), h.end());
    heap_ok = heap_ok && h[0] == 10;
    std::pop_heap(h.begin(), h.end());
    heap_ok = heap_ok && h.back() == 10;
    h.pop_back();
    std::sort_heap(h.begin(), h.end());
    heap_ok = heap_ok && std::is_sorted(h.begin(), h.end()) && h.size() == 8 && h[7] == 9;
    if (!heap_ok) { std::printf("FAIL heap\n"); failures |= 64; }
    auto mx = std::max({3, 9, 4});
    auto mn = std::min({3, 9, 4});
    auto mm = std::minmax({3, 9, 4});
    std::vector<int> ex{3, 1, 4, 1, 5};
    auto me = std::min_element(ex.begin(), ex.end());
    auto xe = std::max_element(ex.begin(), ex.end());
    auto mme = std::minmax_element(ex.begin(), ex.end());
    if (mx != 9 || mn != 3 || mm.first != 3 || mm.second != 9 || std::max(2, 7) != 7 || std::min(2, 7) != 2 || *me != 1 || *xe != 5 || *mme.first != 1 || *mme.second != 5 || std::clamp(15, 0, 10) != 10 || std::clamp(-3, 0, 10) != 0) { std::printf("FAIL min_max\n"); failures |= 128; }
    std::vector<int> st{4, 2, 4, 1};
    std::stable_sort(st.begin(), st.end());
    if (st[0] != 1 || st[3] != 4) { std::printf("FAIL stable_sort\n"); failures |= 256; }
    if (failures == 0) std::printf("PASS std_full_11_algorithm_order\n");
    return failures;
}

// C++20 equivalent of std_full_11_iterator.cb
#include <string>
#include <algorithm>
#include <deque>
#include <iterator>
#include <list>
#include <set>
#include <sstream>
#include <type_traits>
#include <vector>
#include <cstdio>

int main() {
    int failures = 0;
    std::vector<int> v{10, 20, 30, 40, 50};
    auto it = v.begin();
    std::advance(it, 2);
    long dist = std::distance(v.begin(), v.end());
    auto nx = std::next(v.begin());
    auto nx3 = std::next(v.begin(), 3);
    auto pv = std::prev(v.end());
    auto pv2 = std::prev(v.end(), 2);
    if (*it != 30 || dist != 5 || *nx != 20 || *nx3 != 40 || *pv != 50 || *pv2 != 40) { std::printf("FAIL advance_distance_next_prev\n"); failures |= 1; }
    auto mutate_it = v.begin(); mutate_it++; *mutate_it = 21;
    if (v[1] != 21) { std::printf("FAIL vector_iterator_local_deref_assign\n"); failures |= 128; }
    std::list<int> l{1, 2, 3, 4};
    auto li = l.begin();
    std::advance(li, 3);
    long ld = std::distance(l.begin(), l.end());
    std::advance(li, -2);
    if (*li != 2 || ld != 4) { std::printf("FAIL bidirectional_advance\n"); failures |= 2; }
    std::deque<int> dq;
    std::front_insert_iterator<std::deque<int>> fi = std::front_inserter(dq);
    *fi = 1; *fi = 2;
    std::vector<int> ins{1, 4};
    std::copy(l.begin(), l.end(), std::inserter(ins, ins.begin() + 1));
    std::vector<int> bk;
    std::copy(l.begin(), l.end(), std::back_inserter(bk));
    std::vector<int> direct_back;
    auto back_it = std::back_inserter(direct_back); *back_it = 8; *back_it = 9;
    std::set<int> sset;
    std::copy(l.begin(), l.end(), std::inserter(sset, sset.end()));
    if (dq.size() != 2 || dq[0] != 2 || dq[1] != 1 || ins.size() != 6 || ins[1] != 1 || ins[3] != 3 || ins[5] != 4 || bk.size() != 4 || sset.size() != 4 || direct_back.size() != 2 || direct_back[0] != 8 || direct_back[1] != 9) { std::printf("FAIL inserters\n"); failures |= 4; }
    auto rv = std::make_reverse_iterator(v.end());
    auto rv_end = std::make_reverse_iterator(v.begin());
    auto base_it = rv.base();
    int rsum = 0; int rn = 0;
    for (auto r = rv; r != rv_end; ++r) { rsum = rsum * 10 + (*r / 10); rn++; }
    if (*rv != 50 || base_it != v.end() || rsum != 54321 || rn != 5 || *(rv + 1) != 40 || rv[2] != 30) { std::printf("FAIL reverse_iterator\n"); failures |= 8; }
    std::vector<std::string> src{"alpha", "beta"};
    std::vector<std::string> dst;
    std::copy(std::make_move_iterator(src.begin()), std::make_move_iterator(src.end()), std::back_inserter(dst));
    if (dst.size() != 2 || dst[0] != "alpha" || dst[1] != "beta") { std::printf("FAIL move_iterator\n"); failures |= 16; }
    std::istringstream in("3 5 7 9");
    std::istream_iterator<int> in_it(in);
    std::istream_iterator<int> in_end;
    std::vector<int> parsed(in_it, in_end);
    std::ostringstream out;
    std::ostream_iterator<int> out_it(out, ",");
    std::copy(parsed.begin(), parsed.end(), out_it);
    std::ostringstream direct_out;
    std::ostream_iterator<int> direct_out_it(direct_out, ",");
    *direct_out_it = 1; *direct_out_it = 2;
    std::istringstream advance_in("6 7 8");
    std::istream_iterator<int> advance_it(advance_in);
    advance_it++;
    if (parsed.size() != 4 || parsed[3] != 9 || out.str() != "3,5,7,9," || direct_out.str() != "1,2," || *advance_it != 7) { std::printf("FAIL stream_iterators\n"); failures |= 32; }
    int arr[4] = {1, 2, 3, 4};
    if (std::distance(std::begin(arr), std::end(arr)) != 4 || std::size(arr) != 4 || *std::rbegin(arr) != 4 || std::empty(v) || *std::data(v) != 10) { std::printf("FAIL data_empty_size\n"); failures |= 64; }
    if (failures == 0) std::printf("PASS std_full_11_iterator\n");
    return failures;
}

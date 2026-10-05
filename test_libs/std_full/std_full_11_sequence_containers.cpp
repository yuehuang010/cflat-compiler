// C++20 equivalent of std_full_11_sequence_containers.cb
#include <string>
#include <array>
#include <deque>
#include <forward_list>
#include <list>
#include <tuple>
#include <vector>
#include <cstdio>

static bool is_odd(int x) { return x % 2 != 0; }

int main() {
    int failures = 0;
    // vector ctor forms
    std::vector<int> v0;
    std::vector<int> v1(3);
    std::vector<int> v2(3, 7);
    std::vector<int> v3{1, 2, 3, 4};
    std::vector<int> v4(v3.begin() + 1, v3.end());
    std::vector<int> v5(v3);
    std::vector<int> move_source{11, 12};
    std::vector<int> move_constructed(std::move(move_source));
    std::allocator<int> alloc;
    std::vector<int> alloc_constructed(2, 6, alloc);
    if (move_constructed.size() != 2 || move_constructed[1] != 12 || alloc_constructed.size() != 2 || alloc_constructed[0] != 6) failures |= 256;
    if (!v0.empty() || v1.size() != 3 || v1[0] != 0 || v2.size() != 3 || v2[2] != 7 || v4.size() != 3 || v4[0] != 2 || v5 != v3) { std::printf("FAIL vector_ctors\n"); failures |= 1; }
    // access
    if (v3.at(2) != 3 || v3.front() != 1 || v3.back() != 4 || *v3.data() != 1 || v3.data()[3] != 4) { std::printf("FAIL vector_access\n"); failures |= 2; }
    // reverse iterators
    if (*v3.rbegin() != 4 || *(v3.rend() - 1) != 1 || *v3.crbegin() != 4) { std::printf("FAIL vector_reverse_iter\n"); failures |= 4; }
    // capacity
    std::vector<int> cv;
    cv.reserve(16);
    bool cap_ok = cv.capacity() >= 16 && cv.size() == 0;
    cv.push_back(1); cv.push_back(2);
    cv.shrink_to_fit();
    cap_ok = cap_ok && cv.capacity() >= cv.size() && cv.size() == 2 && cv.max_size() > 1000;
    cv.resize(5, 9);
    cap_ok = cap_ok && cv.size() == 5 && cv[4] == 9 && cv[1] == 2;
    cv.resize(1);
    cap_ok = cap_ok && cv.size() == 1;
    cv.clear();
    cap_ok = cap_ok && cv.empty();
    if (!cap_ok) { std::printf("FAIL vector_capacity\n"); failures |= 8; }
    // insert / emplace / erase forms
    std::vector<int> iv{1, 5};
    iv.insert(iv.begin() + 1, 3);
    iv.insert(iv.begin() + 2, 2, 4);
    std::vector<int> extra{8, 9};
    iv.insert(iv.end(), extra.begin(), extra.end());
    iv.emplace(iv.begin(), 0);
    iv.emplace_back(10);
    iv.pop_back();
    // iv = 0 1 3 4 4 5 8 9
    bool ins_ok = iv.size() == 8 && iv[0] == 0 && iv[2] == 3 && iv[3] == 4 && iv[4] == 4 && iv[7] == 9;
    iv.erase(iv.begin());
    iv.erase(iv.begin() + 2, iv.begin() + 4);
    // iv = 1 3 5 8 9
    ins_ok = ins_ok && iv.size() == 5 && iv[2] == 5 && iv[4] == 9;
    if (!ins_ok) { std::printf("FAIL vector_insert_erase\n"); failures |= 16; }
    // assign, swap, comparisons
    std::vector<int> a1;
    a1.assign(3, 6);
    std::vector<int> a2;
    a2.assign(v3.begin(), v3.begin() + 2);
    std::vector<int> a3{1, 2, 3};
    a3.swap(a2);
    std::vector<int> lo{1, 2}, hi{1, 3};
    if (a1.size() != 3 || a1[1] != 6 || a2.size() != 3 || a3.size() != 2 || a3[1] != 2 || !(lo < hi) || lo == hi || lo != std::vector<int>{1, 2} || !(hi >= lo)) { std::printf("FAIL vector_assign_swap_compare\n"); failures |= 32; }
    // deque
    std::deque<int> d{2, 3};
    d.push_front(1); d.push_back(4); d.emplace_front(0); d.emplace_back(5);
    bool d_ok = d.size() == 6 && d[0] == 0 && d.at(5) == 5 && d.front() == 0 && d.back() == 5;
    d.pop_front(); d.pop_back();
    d_ok = d_ok && d.size() == 4 && d.front() == 1 && d.back() == 4;
    d_ok = d_ok && d[2] == 3;
    std::deque<int> d2(3, 8);
    d.swap(d2);
    d_ok = d_ok && d.size() == 3 && d2.size() == 4 && d[0] == 8;
    if (!d_ok) { std::printf("FAIL deque\n"); failures |= 64; }
    // list
    std::list<int> l{1, 2, 3, 4, 5};
    l.remove_if(is_odd);
    bool l_ok = l.size() == 2 && l.front() == 2 && l.back() == 4;
    std::list<int> l2{3, 5};
    l.merge(l2);
    // l = 2 3 4 5 (merge assumes sorted)
    l_ok = l_ok && l.size() == 4 && l2.empty() && l.front() == 2 && l.back() == 5;
    std::list<int> l3{10, 11};
    auto pos = l.begin(); ++pos;
    l.splice(pos, l3);
    // l = 2 10 11 3 4 5
    l_ok = l_ok && l.size() == 6 && l3.empty() && *(++l.begin()) == 10;
    l.remove(10); l.reverse(); l.sort(); l.unique();
    l_ok = l_ok && l.size() == 5 && l.front() == 2 && l.back() == 11;
    if (!l_ok) { std::printf("FAIL list\n"); failures |= 128; }
    // forward_list
    std::forward_list<int> fl{1, 4};
    auto fpos = fl.begin();
    fl.insert_after(fpos, 2);
    fl.emplace_after(fl.begin(), 9);
    // 1 9 2 4
    std::forward_list<int> fl2{7, 8};
    fl.splice_after(fl.before_begin(), fl2);
    // 7 8 1 9 2 4
    bool fl_ok = fl.front() == 7 && fl2.empty();
    fl.remove(9);
    fl.erase_after(fl.begin());
    // 7 1 2 4
    int sum = 0; int cnt = 0;
    for (auto it = fl.begin(); it != fl.end(); ++it) { sum += *it; cnt++; }
    fl_ok = fl_ok && sum == 14 && cnt == 4;
    fl.reverse();
    fl_ok = fl_ok && fl.front() == 4;
    if (!fl_ok) { std::printf("FAIL forward_list\n"); failures |= 256; }
    // array
    std::array<int, 4> ar{1, 2, 3, 4};
    std::array<int, 4> br{};
    br.fill(5);
    bool ar_ok = ar.size() == 4 && ar.front() == 1 && ar.back() == 4 && ar.at(2) == 3 && ar.data()[1] == 2 && std::get<2>(ar) == 3 && br[3] == 5 && ar < br && !ar.empty();
    ar.swap(br);
    ar_ok = ar_ok && ar[0] == 5 && br[0] == 1 && std::tuple_size<std::array<int, 4>>::value == 4 && *ar.rbegin() == 5;
    if (!ar_ok) { std::printf("FAIL array\n"); failures |= 512; }
    if (failures == 0) std::printf("PASS std_full_11_sequence_containers\n");
    return failures;
}

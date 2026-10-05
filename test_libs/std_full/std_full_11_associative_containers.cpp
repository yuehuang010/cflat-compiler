// C++20 equivalent of std_full_11_associative_containers.cb
#include <string>
#include <functional>
#include <map>
#include <set>
#include <utility>
#include <cstdio>

int main() {
    int failures = 0;
    // map ctor forms, lookup
    std::map<int, int> m{{1, 10}, {2, 20}, {3, 30}};
    std::map<int, int> m2(m);
    std::map<int, int> m3(m.begin(), m.end());
    if (m.size() != 3 || m2 != m || m3.size() != 3 || m.at(2) != 20 || m[3] != 30 || m.count(4) != 0 || m.find(2)->second != 20 || m.find(9) != m.end()) { std::printf("FAIL map_basic\n"); failures |= 1; }
    auto ins = m.insert({4, 40});
    auto dup = m.insert({4, 41});
    m.emplace(5, 50);
    m.try_emplace(5, 99);
    m.insert_or_assign(6, 60);
    m.insert_or_assign(6, 61);
    if (!ins.second || dup.second || m[4] != 40 || m[5] != 50 || m[6] != 61 || m.size() != 6) { std::printf("FAIL map_insert\n"); failures |= 2; }
    // bounds
    auto lb = m.lower_bound(3);
    auto ub = m.upper_bound(3);
    auto er = m.equal_range(3);
    if (lb->first != 3 || ub->first != 4 || er.first != lb || er.second != ub || m.lower_bound(7) != m.end()) { std::printf("FAIL map_bounds\n"); failures |= 4; }
    // hint insert, erase forms
    auto hint = m.insert(m.end(), {7, 70});
    bool er_ok = hint->first == 7 && m.size() == 7;
    size_t n = m.erase(7);
    er_ok = er_ok && n == 1 && m.erase(100) == 0;
    m.erase(m.find(6));
    m.erase(m.lower_bound(4), m.end());
    er_ok = er_ok && m.size() == 3 && m.rbegin()->first == 3 && m.begin()->first == 1;
    if (!er_ok) { std::printf("FAIL map_erase\n"); failures |= 8; }
    // comparisons, key_comp / value_comp
    std::map<int, int> lo{{1, 1}}, hi{{1, 2}};
    if (!(lo < hi) || lo == hi) { std::printf("FAIL map_compare\n"); failures |= 16; }
    // custom comparator
    std::map<int, int, std::greater<int>> gm{{1, 1}, {2, 2}, {3, 3}};
    if (gm.begin()->first != 3 || gm.rbegin()->first != 1) { std::printf("FAIL map_greater\n"); failures |= 32; }
    // multimap
    std::multimap<int, int> mm{{1, 4}, {1, 5}, {2, 6}};
    mm.emplace(1, 7);
    auto mr = mm.equal_range(1);
    int msum = 0;
    for (auto it = mr.first; it != mr.second; ++it) msum += it->second;
    if (mm.count(1) != 3 || msum != 16 || mm.size() != 4 || mm.erase(1) != 3 || mm.size() != 1 || mm.find(2)->second != 6) { std::printf("FAIL multimap\n"); failures |= 64; }
    // set
    std::set<int> s{5, 1, 3};
    auto sins = s.insert(3);
    s.insert({7, 9});
    std::set<int> s2(s.begin(), s.end());
    if (sins.second || s.size() != 5 || *s.begin() != 1 || *s.rbegin() != 9 || s2 != s || s.count(3) != 1 || *s.lower_bound(4) != 5 || *s.upper_bound(5) != 7 || s.find(2) != s.end()) { std::printf("FAIL set\n"); failures |= 128; }
    s.erase(3);
    s.erase(s.begin());
    auto sh = s.insert(s.end(), 11);
    if (s.size() != 4 || *sh != 11) { std::printf("FAIL set_erase_hint\n"); failures |= 256; }
    // multiset
    std::multiset<int> ms{2, 2, 3, 2};
    auto msr = ms.equal_range(2);
    int dist = 0;
    for (auto it = msr.first; it != msr.second; ++it) dist++;
    ms.erase(ms.find(2));
    if (dist != 3 || ms.count(2) != 2 || ms.size() != 3 || ms.erase(2) != 2 || ms.size() != 1) { std::printf("FAIL multiset\n"); failures |= 512; }
    // node handles
    std::map<int, int> nm{{1, 10}, {2, 20}, {3, 30}};
    auto nh = nm.extract(2);
    bool nh_ok = !nh.empty() && nh.key() == 2 && nh.mapped() == 20 && nm.size() == 2;
    nh.key() = 22;
    nm.insert(std::move(nh));
    nh_ok = nh_ok && nm.size() == 3 && nm.count(22) == 1 && nm.count(2) == 0 && nm[22] == 20;
    std::map<int, int> src{{50, 5}, {1, 99}};
    nm.merge(src);
    nh_ok = nh_ok && nm.count(50) == 1 && src.size() == 1 && src.count(1) == 1;
    std::multimap<int, int> nmm{{1, 1}, {1, 2}};
    auto nh2 = nmm.extract(nmm.begin());
    nh_ok = nh_ok && nh2.key() == 1 && nmm.size() == 1;
    std::set<int> ns{1, 2, 3};
    auto nh3 = ns.extract(2);
    nh_ok = nh_ok && nh3.value() == 2 && ns.size() == 2 && !ns.contains(2);
    if (!nh_ok) { std::printf("FAIL node_handles\n"); failures |= 1024; }
    if (failures == 0) std::printf("PASS std_full_11_associative_containers\n");
    return failures;
}

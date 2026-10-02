// C++20 equivalent of std_11_containers.cb (compile-time parity baseline)
#include <array>
#include <deque>
#include <forward_list>
#include <functional>
#include <list>
#include <map>
#include <queue>
#include <set>
#include <stack>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cstdio>

int main() {
    int failures = 0;
    std::array<int, 2> a{{4, 5}};
    std::vector<int> v{1, 3}; v.reserve(8); v.emplace_back(5); v.insert(v.begin() + 1, 2); v.erase(v.begin() + 2);
    if (a[1] != 5 || v.size() != 3 || v[0] != 1 || v[1] != 2 || v[2] != 5 || *v.begin() != 1) { std::printf("FAIL array/vector/iteration\n"); failures |= 1; }
    std::deque<int> d{2, 4}; d.push_front(1); std::list<int> l{7, 8}; l.emplace_back(9); std::forward_list<int> fl{3, 4};
    std::map<int, int> m{{1, 10}}; m.emplace(2, 20); std::multimap<int, int> mm{{1, 4}, {1, 5}};
    std::set<int> s{3, 5}; std::multiset<int> ms{2, 2}; std::unordered_map<int, int> um{{7, 70}}; std::unordered_set<int> us{9};
    std::stack<int> st; st.push(11); std::queue<int> q; q.push(12); std::priority_queue<int> pq; pq.push(13);
    if (d.front() != 1 || l.back() != 9 || fl.front() != 3 || m[2] != 20 || mm.count(1) != 2 || s.count(5) != 1 || ms.count(2) != 2 || um[7] != 70 || us.count(9) != 1 || st.top() != 11 || q.front() != 12 || pq.top() != 13 || std::hash<int>{}(42) != std::hash<int>{}(42) || std::hash<int>{}(42) == std::hash<int>{}(43)) { std::printf("FAIL containers\n"); failures |= 2; }
    if (!failures) std::printf("PASS std_11_containers\n");
    return failures;
}

// C++20 equivalent of std_full_11_container_adaptors.cb
#include <string>
#include <deque>
#include <functional>
#include <list>
#include <queue>
#include <stack>
#include <vector>
#include <cstdio>

int main() {
    int failures = 0;
    std::stack<int> st;
    st.push(1); st.push(2); st.emplace(3);
    int top3 = st.top();
    st.pop();
    if (top3 != 3 || st.top() != 2 || st.size() != 2 || st.empty()) { std::printf("FAIL stack\n"); failures |= 1; }
    std::stack<int, std::vector<int>> sv;
    sv.push(5); sv.push(6);
    std::stack<int, std::vector<int>> sv2;
    sv2.push(9);
    sv.swap(sv2);
    if (sv.size() != 1 || sv.top() != 9 || sv2.size() != 2 || sv2.top() != 6 || sv == sv2) { std::printf("FAIL stack_vector_swap\n"); failures |= 2; }
    std::queue<int> q;
    q.push(1); q.push(2); q.emplace(3);
    bool q_ok = q.front() == 1 && q.back() == 3 && q.size() == 3;
    q.pop();
    q_ok = q_ok && q.front() == 2 && q.size() == 2;
    std::queue<int, std::list<int>> ql;
    ql.push(7); ql.push(8);
    std::queue<int, std::list<int>> ql2;
    ql2.push(1);
    ql.swap(ql2);
    q_ok = q_ok && ql.size() == 1 && ql.front() == 1 && ql2.back() == 8;
    if (!q_ok) { std::printf("FAIL queue\n"); failures |= 4; }
    std::priority_queue<int> pq;
    pq.push(3); pq.push(9); pq.push(5); pq.emplace(7);
    bool pq_ok = pq.top() == 9 && pq.size() == 4;
    pq.pop();
    pq_ok = pq_ok && pq.top() == 7;
    if (!pq_ok) { std::printf("FAIL priority_queue\n"); failures |= 8; }
    std::priority_queue<int, std::vector<int>, std::greater<int>> mpq;
    mpq.push(3); mpq.push(1); mpq.push(2);
    int first = mpq.top(); mpq.pop();
    int second = mpq.top();
    std::priority_queue<int, std::vector<int>, std::greater<int>> mpq2;
    mpq2.push(100);
    mpq.swap(mpq2);
    if (first != 1 || second != 2 || mpq.top() != 100 || mpq.size() != 1 || mpq2.size() != 2 || mpq2.top() != 2) { std::printf("FAIL priority_queue_greater\n"); failures |= 16; }
    std::vector<int> init{4, 8, 6};
    std::priority_queue<int> from_vec(std::less<int>(), init);
    std::deque<int> dq{1, 2, 3};
    std::stack<int> from_deque(dq);
    std::queue<int> from_q_copy(std::deque<int>{5, 6});
    if (from_vec.top() != 8 || from_vec.size() != 3 || from_deque.top() != 3 || from_deque.size() != 3 || from_q_copy.front() != 5 || from_q_copy.back() != 6) { std::printf("FAIL adaptor_ctor_from_container\n"); failures |= 32; }
    if (failures == 0) std::printf("PASS std_full_11_container_adaptors\n");
    return failures;
}

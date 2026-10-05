// C++20 equivalent of std_full_11_929_smart_ptr_member_templates.cb (compile-time parity baseline)
#include <string>
#include <memory>
#include <map>
#include <cstdio>

int main() {
    int failures = 0;
    std::shared_ptr<int> a = std::make_shared<int>(1), b = std::make_shared<int>(2);
    if (a.owner_before(b) == b.owner_before(a)) { printf("FAIL owner_before shared\n"); failures |= 1; }
    std::weak_ptr<int> w = a;
    if (w.owner_before(b) == b.owner_before(w)) { printf("FAIL owner_before weak\n"); failures |= 2; }
    std::map<std::weak_ptr<int>, int, std::owner_less<std::weak_ptr<int>>> om;
    om[a] = 10; om[b] = 20;
    if (om.size() != 2 || om[a] != 10) { printf("FAIL map index from shared_ptr\n"); failures |= 4; }
    if (failures == 0) printf("PASS std_full_11_929_smart_ptr_member_templates\n");
    return failures;
}

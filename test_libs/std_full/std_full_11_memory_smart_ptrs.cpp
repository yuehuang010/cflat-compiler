// C++20 equivalent of std_full_11_memory_smart_ptrs.cb (compile-time parity baseline)
#include <string>
#include <utility>
#include <memory>
#include <map>
#include <set>
#include <stdexcept>
#include <functional>
#include <cstdio>

int main() {
    int failures = 0;
    std::unique_ptr<int> u1 = std::make_unique<int>(1);
    std::unique_ptr<int> u2 = std::make_unique<int>(2);
    std::unique_ptr<int> un;
    int* raw1 = u1.get();
    u1.swap(u2);
    if (*u1 != 2 || *u2 != 1 || u2.get() != raw1) { printf("FAIL unique swap\n"); failures |= 1; }
    std::swap(u1, u2);
    if (*u1 != 1 || u1 == nullptr || un != nullptr || !(un == nullptr) || !un.get() == false || !(static_cast<bool>(u1)) || static_cast<bool>(un)) { printf("FAIL unique nullptr compare\n"); failures |= 2; }
    int* released = u1.release();
    bool rel_ok = u1 == nullptr && *released == 1;
    delete released;
    u1.reset(new int(5));
    int five = *u1;
    u1.reset();
    if (!rel_ok || five != 5 || u1 != nullptr) { printf("FAIL unique release reset\n"); failures |= 4; }
    std::unique_ptr<int> ua(new int(3)), ub(new int(4));
    bool order = (ua < ub) == (ua.get() < ub.get()) && (ua != ub) && !(ua == ub);
    if (!order) { printf("FAIL unique ordering\n"); failures |= 8; }
    std::shared_ptr<std::pair<int, int>> owner = std::make_shared<std::pair<int, int>>(7, 8);
    std::shared_ptr<int> alias(owner, &owner->second);
    if (*alias != 8 || owner.use_count() != 2 || alias.use_count() != 2 || alias.get() != &owner->second) { printf("FAIL aliasing ctor\n"); failures |= 16; }
    owner.reset();
    if (alias.use_count() != 1 || *alias != 8) { printf("FAIL aliasing keeps owner\n"); failures |= 32; }
    std::shared_ptr<int> si = std::make_shared<int>(42);
    std::shared_ptr<void> sv = std::static_pointer_cast<void>(si);
    std::shared_ptr<int> back = std::static_pointer_cast<int>(sv);
    std::shared_ptr<long long> rc = std::reinterpret_pointer_cast<long long>(si);
    if (*back != 42 || back.get() != si.get() || reinterpret_cast<void*>(rc.get()) != si.get() || si.use_count() != 4)
    { printf("FAIL pointer casts\n"); failures |= 64; }
    std::shared_ptr<int> k1 = std::make_shared<int>(1), k2 = std::make_shared<int>(2);
    std::map<std::weak_ptr<int>, int, std::owner_less<std::weak_ptr<int>>> om;
    om[k1] = 10; om[k2] = 20; om[k1] += 1;
    std::owner_less<std::shared_ptr<int>> ol;
    std::set<std::shared_ptr<int>, std::owner_less<std::shared_ptr<int>>> os;
    os.insert(k1); os.insert(k2); os.insert(k1);
    if (om.size() != 2 || om[k1] != 11 || om[k2] != 20 || os.size() != 2 || ol(k1, k2) == ol(k2, k1)) { printf("FAIL owner_less\n"); failures |= 512; }
    std::hash<std::unique_ptr<int>> hu; std::hash<std::shared_ptr<int>> hsh;
    std::unique_ptr<int> hp = std::make_unique<int>(9);
    if (hu(hp) != hu(hp) || hu(hp) != std::hash<int*>()(hp.get()) || hsh(k1) != hsh(k1) || hsh(k1) != std::hash<int*>()(k1.get())) { printf("FAIL hash smart\n"); failures |= 1024; }
    if (failures == 0) printf("PASS std_full_11_memory_smart_ptrs\n");
    return failures;
}

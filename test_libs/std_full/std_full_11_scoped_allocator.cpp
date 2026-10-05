// C++20 equivalent of std_full_11_scoped_allocator.cb (compile-time parity baseline)
#include <string>
#include <scoped_allocator>
#include <vector>
#include <memory>
#include <cstdio>

using Alloc = std::scoped_allocator_adaptor<std::allocator<std::string>>;

int main() {
    int failures = 0;
    Alloc a;
    std::vector<std::string, Alloc> v(a);
    v.push_back("alpha"); v.emplace_back("beta"); v.push_back(std::string("gamma"));
    if (v.size() != 3 || v[0] != "alpha" || v[1] != "beta" || v[2] != "gamma") { printf("FAIL vector push\n"); failures |= 1; }
    Alloc other;
    if (other != a || !(other == a)) { printf("FAIL adaptor compare\n"); failures |= 2; }
    std::allocator<std::string>& outer = a.outer_allocator();
    Alloc& inner = a.inner_allocator();
    std::string* raw = outer.allocate(2);
    outer.deallocate(raw, 2);
    if (inner != a) { printf("FAIL inner allocator\n"); failures |= 4; }
    std::string* p = a.allocate(2);
    a.construct(p, "xyz");
    a.construct(p + 1, 3, 'q');
    bool cons_ok = p[0] == "xyz" && p[1] == "qqq";
    a.destroy(p);
    a.destroy(p + 1);
    a.deallocate(p, 2);
    if (!cons_ok || a.max_size() < 1000) { printf("FAIL adaptor allocate construct\n"); failures |= 8; }
    std::vector<std::string, Alloc> copy(v);
    std::vector<std::string, Alloc> moved(std::move(copy));
    if (moved.size() != 3 || moved[2] != "gamma") { printf("FAIL copy move\n"); failures |= 16; }
    Alloc rebound_src;
    std::scoped_allocator_adaptor<std::allocator<int>> ia(rebound_src);
    int* ip = ia.allocate(3);
    ia.construct(ip, 5);
    bool ir = ip[0] == 5;
    ia.deallocate(ip, 3);
    if (!ir) { printf("FAIL rebind conversion\n"); failures |= 32; }
    if (failures == 0) printf("PASS std_full_11_scoped_allocator\n");
    return failures;
}

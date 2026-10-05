// C++20 equivalent of std_full_17_memory_resource.cb
#include <string>
#include <map>
#include <memory_resource>
#include <string>
#include <vector>
#include <cstdio>

int main() {
    int failures = 0;
    std::pmr::monotonic_buffer_resource mono;
    std::pmr::vector<int> pv(&mono);
    for (int i = 0; i < 100; i++) pv.push_back(i);
    if (pv.size() != 100 || pv[99] != 99 || pv.get_allocator().resource() != &mono) { std::printf("FAIL pmr_vector_monotonic\n"); failures |= 1; }
    char backing[1024];
    std::pmr::monotonic_buffer_resource fixed(backing, sizeof(backing), std::pmr::null_memory_resource());
    std::pmr::vector<int> fv(&fixed);
    fv.push_back(1); fv.push_back(2);
    if (fv.size() != 2 || fv[1] != 2) { std::printf("FAIL pmr_vector_fixed_buffer\n"); failures |= 2; }
    std::pmr::unsynchronized_pool_resource upool;
    std::pmr::synchronized_pool_resource spool;
    void* p1 = upool.allocate(24, 8);
    void* p2 = spool.allocate(64, 16);
    bool pool_ok = p1 != nullptr && p2 != nullptr && p1 != p2;
    upool.deallocate(p1, 24, 8);
    spool.deallocate(p2, 64, 16);
    pool_ok = pool_ok && upool.is_equal(upool) && !upool.is_equal(spool);
    if (!pool_ok) { std::printf("FAIL pool_resources\n"); failures |= 4; }
    std::pmr::memory_resource* nd = std::pmr::new_delete_resource();
    std::pmr::memory_resource* nm = std::pmr::null_memory_resource();
    void* q = nd->allocate(32, 8);
    bool res_ok = q != nullptr && nd->is_equal(*std::pmr::new_delete_resource()) && !nd->is_equal(*nm);
    nd->deallocate(q, 32, 8);
    if (!res_ok) { std::printf("FAIL new_delete_null_resources\n"); failures |= 8; }
    std::pmr::polymorphic_allocator<int> pa(&mono);
    int* arr = pa.allocate(4);
    arr[0] = 5; arr[3] = 8;
    bool pa_ok = arr[0] == 5 && arr[3] == 8 && pa.resource() == &mono;
    pa.deallocate(arr, 4);
    int* obj = pa.new_object<int>(42);
    pa_ok = pa_ok && *obj == 42;
    pa.delete_object(obj);
    int* raw = pa.allocate_object<int>(3);
    raw[2] = 7;
    pa_ok = pa_ok && raw[2] == 7;
    pa.deallocate_object(raw, 3);
    if (!pa_ok) { std::printf("FAIL polymorphic_allocator\n"); failures |= 16; }
    std::pmr::string ps("hello pmr", &mono);
    std::pmr::map<int, int> pm(&mono);
    pm[1] = 10; pm[2] = 20;
    if (ps.size() != 9 || ps != "hello pmr" || pm.size() != 2 || pm[2] != 20) { std::printf("FAIL pmr_string_map\n"); failures |= 32; }
    std::pmr::memory_resource* before = std::pmr::get_default_resource();
    std::pmr::memory_resource* old = std::pmr::set_default_resource(&mono);
    bool def_ok = old == before && std::pmr::get_default_resource() == &mono;
    std::pmr::vector<int> dv;
    dv.push_back(3);
    def_ok = def_ok && dv.get_allocator().resource() == &mono;
    std::pmr::set_default_resource(old);
    def_ok = def_ok && std::pmr::get_default_resource() == before;
    if (!def_ok) { std::printf("FAIL default_resource\n"); failures |= 64; }
    if (failures == 0) std::printf("PASS std_full_17_memory_resource\n");
    return failures;
}

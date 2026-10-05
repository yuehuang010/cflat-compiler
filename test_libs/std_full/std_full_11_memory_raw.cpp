// C++20 equivalent of std_full_11_memory_raw.cb (compile-time parity baseline)
#include <string>
#include <memory>
#include <vector>
#include <type_traits>
#include <cstddef>
#include <cstdint>
#include <cstdio>

int main() {
    int failures = 0;
    std::allocator<int> alloc;
    int* mem = alloc.allocate(8);
    for (int i = 0; i < 8; i++) mem[i] = i * i;
    bool alloc_ok = mem[3] == 9 && mem[7] == 49;
    alloc.deallocate(mem, 8);
    using traits = std::allocator_traits<std::allocator<int>>;
    int* tmem = traits::allocate(alloc, 4);
    traits::construct(alloc, tmem, 77);
    traits::construct(alloc, tmem + 1, 78);
    bool traits_ok = tmem[0] == 77 && tmem[1] == 78 && traits::max_size(alloc) > 1000;
    traits::destroy(alloc, tmem);
    traits::deallocate(alloc, tmem, 4);
    if (!alloc_ok || !traits_ok) { printf("FAIL allocator\n"); failures |= 1; }
    std::allocator<std::string> sa;
    std::string* raw = sa.allocate(8);
    std::string src[3] = {"a", "bb", "ccc"};
    std::string* end = std::uninitialized_copy(src, src + 3, raw);
    std::uninitialized_fill_n(raw + 3, 2, std::string("fill"));
    if (end != raw + 3 || raw[0] != "a" || raw[2] != "ccc" || raw[3] != "fill" || raw[4] != "fill") { printf("FAIL uninitialized copy fill\n"); failures |= 2; }
    std::destroy(raw, raw + 5);
    std::uninitialized_default_construct(raw, raw + 2);
    std::uninitialized_value_construct(raw + 2, raw + 4);
    bool empties = raw[0].empty() && raw[1].empty() && raw[2].empty() && raw[3].empty();
    std::destroy_n(raw, 4);
    std::uninitialized_move(src, src + 3, raw);
    bool moved = raw[0] == "a" && raw[1] == "bb" && raw[2] == "ccc";
    std::destroy(raw, raw + 3);
    sa.deallocate(raw, 8);
    int* ints = std::allocator<int>().allocate(5);
    std::uninitialized_fill(ints, ints + 5, 7);
    std::uninitialized_value_construct(ints + 2, ints + 4);
    bool vals = ints[4] == 7 && ints[0] == 7 && ints[2] == 0 && ints[3] == 0;
    std::allocator<int>().deallocate(ints, 5);
    if (!empties || !moved || !vals) { printf("FAIL default value move construct\n"); failures |= 4; }
    std::string* slot = sa.allocate(1);
    std::string* made = std::construct_at(slot, 3, 'z');
    bool ca_ok = made == slot && *slot == "zzz";
    std::destroy_at(slot);
    sa.deallocate(slot, 1);
    int* islot = std::allocator<int>().allocate(1);
    std::construct_at(islot, 41);
    bool ci_ok = *islot == 41;
    std::destroy_at(islot);
    std::allocator<int>().deallocate(islot, 1);
    if (!ca_ok || !ci_ok) { printf("FAIL construct_at\n"); failures |= 8; }
    int target = 5;
    int* via_traits = std::pointer_traits<int*>::pointer_to(target);
    std::unique_ptr<int> up = std::make_unique<int>(6);
    if (std::addressof(target) != &target || via_traits != &target || std::to_address(&target) != &target || std::to_address(up.get()) != up.get())
    { printf("FAIL pointer utilities\n"); failures |= 16; }
    alignas(64) char buffer[256];
    void* p = buffer + 1;
    std::size_t space = 255;
    void* aligned = std::align(16, 32, p, space);
    bool al_ok = aligned != nullptr && (reinterpret_cast<std::uintptr_t>(aligned) % 16) == 0 && aligned == p && space <= 255 && space >= 32;
    std::size_t tiny = 8;
    void* q = buffer;
    void* none = std::align(16, 64, q, tiny);
    int* ap = std::assume_aligned<16>(reinterpret_cast<int*>(aligned));
    if (!al_ok || none != nullptr || ap != reinterpret_cast<int*>(aligned)) { printf("FAIL align assume_aligned\n"); failures |= 32; }
    if (!std::uses_allocator_v<std::vector<int>, std::allocator<int>> || std::uses_allocator_v<int, std::allocator<int>> || !std::uses_allocator<std::string, std::allocator<char>>::value)
    { printf("FAIL uses_allocator\n"); failures |= 64; }
    std::string made_str = std::make_obj_using_allocator<std::string>(std::allocator<char>(), "abc");
    std::vector<int> made_vec = std::make_obj_using_allocator<std::vector<int>>(std::allocator<int>(), 3, 9);
    if (made_str != "abc" || made_vec.size() != 3 || made_vec[2] != 9) { printf("FAIL make_obj_using_allocator\n"); failures |= 128; }
    if (failures == 0) printf("PASS std_full_11_memory_raw\n");
    return failures;
}

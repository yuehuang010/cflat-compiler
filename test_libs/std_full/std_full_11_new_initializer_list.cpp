// C++20 equivalent of std_full_11_new_initializer_list.cb (compile-time parity baseline)
#include <string>
#include <new>
#include <initializer_list>
#include <algorithm>
#include <cstdint>
#include <cstdio>

int handler_calls = 0;
void my_handler() { handler_calls++; }

int main() {
    int failures = 0;
    std::new_handler old = std::set_new_handler(my_handler);
    std::new_handler cur = std::get_new_handler();
    cur();
    std::set_new_handler(old);
    std::new_handler restored = std::get_new_handler();
    if (handler_calls != 1 || restored != old || restored != nullptr) { printf("FAIL new_handler\n"); failures |= 1; }
    int value = 77;
    int* laundered = std::launder(&value);
    if (laundered != &value || *laundered != 77) { printf("FAIL launder\n"); failures |= 2; }
    if (std::hardware_destructive_interference_size < 32 || std::hardware_constructive_interference_size < 16) { printf("FAIL interference size\n"); failures |= 4; }
    std::bad_alloc ba; std::bad_array_new_length bl;
    if (ba.what() == nullptr || bl.what() == nullptr) { printf("FAIL bad_alloc what\n"); failures |= 8; }
    std::initializer_list<int> il = {3, 9, 4};
    int sum = 0;
    for (const int* it = il.begin(); it != il.end(); ++it) sum += *it;
    const int* first = std::begin(il);
    if (il.size() != 3 || sum != 16 || *first != 3 || std::end(il) - std::begin(il) != 3 || std::max(il) != 9 || std::min(il) != 3)
    { printf("FAIL initializer_list\n"); failures |= 16; }
    if (failures == 0) printf("PASS std_full_11_new_initializer_list\n");
    return failures;
}

// C++20 equivalent of std_full_11_typeinfo_typeindex.cb (compile-time parity baseline)
#include <string>
#include <any>
#include <functional>
#include <map>
#include <typeindex>
#include <typeinfo>
#include <cstdio>

int main() {
    int failures = 0;
    std::bad_cast bc; std::bad_typeid bt;
    if (bc.what() == nullptr || bt.what() == nullptr) { printf("FAIL bad_cast what\n"); failures |= 32; }
    std::bad_any_cast bac;
    if (bac.what() == nullptr) { printf("FAIL bad_any_cast\n"); failures |= 64; }
    if (failures == 0) printf("PASS std_full_11_typeinfo_typeindex\n");
    return failures;
}

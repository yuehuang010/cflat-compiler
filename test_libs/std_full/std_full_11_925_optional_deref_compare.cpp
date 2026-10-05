// C++20 equivalent of std_full_11_925_optional_deref_compare.cb (compile-time parity baseline)
#include <string>
#include <optional>
#include <cstdio>

int main() {
    int failures = 0;
    std::optional<std::string> o = std::string("abc");
    std::string lit = "abc";
    if (!(*o == "abc") || *o != "abc") { printf("FAIL deref == literal\n"); failures |= 1; }
    if (!(*o == lit)) { printf("FAIL deref == string\n"); failures |= 2; }
    if (failures == 0) printf("PASS std_full_11_925_optional_deref_compare\n");
    return failures;
}

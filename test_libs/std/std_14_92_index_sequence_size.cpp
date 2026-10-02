// C++20 equivalent of std_14_92_index_sequence_size.cb (compile-time parity baseline)
#include <string>
#include <utility>
#include <cstdio>
int main() {
    int failures = 0;
    auto values = std::make_index_sequence<3>{};
    if (values.size() != 3) { std::printf("FAIL index_sequence_size\n"); failures |= 1; }
    if (!failures) std::printf("PASS std_14_92_index_sequence_size\n");
    return failures;
}

// C++20 equivalent of std_full_11_916_insert_iterator_local.cb
#include <string>
#include <iterator>
#include <vector>
#include <cstdio>

int main() {
    int failures = 0;
    std::vector<int> ins{1, 4};
    auto ii = std::inserter(ins, ins.begin() + 1);
    *ii = 2; *ii = 3;
    if (ins.size() != 4 || ins[0] != 1 || ins[1] != 2 || ins[2] != 3 || ins[3] != 4) { std::printf("FAIL insert_iterator_local\n"); failures |= 1; }
    if (failures == 0) std::printf("PASS std_full_11_916_insert_iterator_local\n");
    return failures;
}

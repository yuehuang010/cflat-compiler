// C++20 equivalent of std_full_11_921_member_typedef.cb
#include <string>
#include <iterator>
#include <type_traits>
#include <vector>
#include <cstdio>

int main() {
    int failures = 0;
    std::vector<int>::value_type x = 5;
    if (x != 5) { std::printf("FAIL vector_value_type\n"); failures |= 1; }
    bool tr = std::is_same<std::vector<int>::value_type, int>::value;
    if (!tr) { std::printf("FAIL is_same_value_type\n"); failures |= 2; }
    bool it = std::is_same<std::iterator_traits<std::vector<int>::iterator>::iterator_category, std::random_access_iterator_tag>::value;
    if (!it) { std::printf("FAIL iterator_traits_category\n"); failures |= 4; }
    if (failures == 0) std::printf("PASS std_full_11_921_member_typedef\n");
    return failures;
}

// C++20 equivalent of std_20_93_erase_if.cb (compile-time parity baseline)
#include <algorithm>
#include <functional>
#include <set>
#include <string>
#include <unordered_set>

int main()
{
    int failures = 0;
    std::set<int> ordered{4, 8};
    std::unordered_set<int> hashed{5, 9};
    int ordered_erased = std::erase_if(ordered, [](int item) { return item == 4; });
    int hashed_erased = std::erase_if(hashed, [](int item) { return item == 5; });
    if (ordered_erased != 1 || hashed_erased != 1) failures |= 1;
    return failures;
}

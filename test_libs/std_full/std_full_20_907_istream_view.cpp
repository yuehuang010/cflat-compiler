#include <string>
#include <cstdio>
#include <ranges>
#include <sstream>
int main()
{
    std::istringstream input("4 5 6");
    int sum = 0; for (int x : std::ranges::istream_view<int>(input)) sum += x;
    if (sum != 15) { std::puts("FAIL istream view"); return 1; }
    std::puts("PASS std_full_20_907_istream_view"); return 0;
}

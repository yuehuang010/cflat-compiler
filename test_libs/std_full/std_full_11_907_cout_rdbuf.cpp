#include <string>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <cstdio>
int main()
{
    std::cout.flush();
    std::ostringstream captured;
    auto* old = std::cout.rdbuf(captured.rdbuf());
    std::cout << "x";
    std::cout.rdbuf(old);
    std::ostringstream err;
    auto* old_err = std::cerr.rdbuf(err.rdbuf());
    std::cerr << "y";
    std::cerr.rdbuf(old_err);
    if (captured.str() != "x" || err.str() != "y") { std::puts("FAIL cout capture"); return 1; }
    std::puts("PASS std_full_11_907_cout_rdbuf"); return 0;
}

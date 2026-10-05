#include <string>
#include <cmath>
#include <cfenv>
#include <cstdio>
int main()
{
    int failures = 0;
    std::feclearexcept(FE_ALL_EXCEPT);
    bool clear = std::fetestexcept(FE_ALL_EXCEPT) == 0;
    std::feraiseexcept(FE_DIVBYZERO);
    bool raised = std::fetestexcept(FE_DIVBYZERO) != 0 && std::fetestexcept(FE_INVALID) == 0;
    std::feclearexcept(FE_ALL_EXCEPT);
    if (!clear || !raised || std::fetestexcept(FE_DIVBYZERO) != 0) { printf("FAIL exception flags\n"); failures |= 256; }
    if (failures == 0) printf("PASS std_full_11_942_feraiseexcept_flags\n");
    return failures;
}

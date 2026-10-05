#include <cstdlib>
#include <cstdio>
int main()
{
    int failures = 0;
    char* e = nullptr;
    long a = std::strtol("12x", &e, 10);
    if (a != 12 || *e != 'x') { printf("FAIL strtol(&e)\n"); failures |= 1; }
    if (failures == 0) printf("PASS std_full_11_901_address_of_pointer_arg\n");
    return failures;
}

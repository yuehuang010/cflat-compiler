// C++20 equivalent of std_full_11_912_errno_assert_macros.cb (compile-time parity baseline)
#include <string>
#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <cstdio>

int main() {
    int failures = 0;
    int x = 1;
    assert(x == 1);
    errno = 0;
    long big = std::strtol("99999999999999999999999", nullptr, 10);
    int err = errno;
    errno = 0;
    if (err != ERANGE || errno != 0 || big == 0) { printf("FAIL errno ERANGE\n"); failures |= 1; }
    errno = EINVAL;
    if (errno != EINVAL) { printf("FAIL errno assign\n"); failures |= 2; }
    errno = 0;
    if (failures == 0) printf("PASS std_full_11_912_errno_assert_macros\n");
    return failures;
}

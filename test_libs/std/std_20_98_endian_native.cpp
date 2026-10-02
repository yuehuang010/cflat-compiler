// C++20 equivalent of std_20_98_endian_native.cb (compile-time parity baseline)
#include <bit>
#include <string>

int main()
{
    int failures = 0;
    if (std::endian::native != std::endian::little) failures |= 1;
    return failures;
}

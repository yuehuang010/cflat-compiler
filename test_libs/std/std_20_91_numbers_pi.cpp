// C++20 equivalent of std_20_91_numbers_pi.cb (compile-time parity baseline)
#include <numbers>
#include <string>

int main()
{
    int failures = 0;
    double pi = std::numbers::pi;
    if (!(pi > 3.14159 && pi < 3.14160)) failures |= 1;
    return failures;
}

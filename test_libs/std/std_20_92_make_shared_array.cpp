// C++20 equivalent of std_20_92_make_shared_array.cb (compile-time parity baseline)
#include <memory>
#include <string>

int main()
{
    int failures = 0;
    auto values = std::make_shared<int[]>(3);
    values[1] = 42;
    if (values[1] != 42) failures |= 1;
    return failures;
}

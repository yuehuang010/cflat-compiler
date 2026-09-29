// C++20 equivalent of fmt_03_memory_buffer.cb (compile-time parity baseline)
#include <cstdio>
#include <fmt/format.h>

int main()
{
    int failures = 0;
    fmt::memory_buffer buf;
    fmt::format_to(fmt::appender(buf), fmt::runtime("{}"), 42);
    int size = (int)buf.size();
    if (size != 2) { std::printf("FAIL size: got %d want 2\n", size); failures |= 1; }
    if (failures == 0) std::printf("PASS fmt_03_memory_buffer\n");
    return failures;
}

// C++20 equivalent of std_20_96_barrier.cb (compile-time parity baseline)
#include <barrier>
#include <string>

int main()
{
    int failures = 0;
    std::barrier sync{1};
    sync.arrive_and_wait();
    return failures;
}

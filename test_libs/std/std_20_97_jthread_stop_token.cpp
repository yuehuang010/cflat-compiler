// C++20 equivalent of std_20_97_jthread_stop_token.cb (compile-time parity baseline)
#include <string>
#include <thread>

int main()
{
    int failures = 0;
    std::jthread worker([] {});
    worker.request_stop();
    if (!worker.get_stop_token().stop_requested()) failures |= 1;
    worker.join();
    return failures;
}

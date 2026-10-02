// C++20 equivalent of std_20_concurrency.cb (compile-time parity baseline)
#include <atomic>
#include <latch>
#include <semaphore>
#include <string>
#include <thread>
#include <functional>

static void jthread_work() { }

int main()
{
    int failures = 0;
    std::latch gate{1};
    gate.count_down();
    gate.wait();
    if (!gate.try_wait()) { std::printf("FAIL latch completion\n"); failures |= 1; }
    std::counting_semaphore<1> sem{0};
    sem.release();
    if (!sem.try_acquire()) failures |= 4;
    int raw = 10;
    std::atomic_ref<int> ref{raw};
    ref.fetch_add(5);
    if (raw != 15) failures |= 8;
    std::atomic<int> value{0};
    value.store(1);
    value.wait(0);
    value.notify_one();
    if (value.load() != 1) failures |= 16;
    std::jthread worker{std::function<void()>(jthread_work)};
    worker.request_stop();
    if (!worker.get_stop_token().stop_requested()) failures |= 32;
    worker.join();
    return failures;
}

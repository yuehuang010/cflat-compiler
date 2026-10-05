#include <string>
#include <cstdio>
#include <barrier>
#include <condition_variable>
#include <functional>
#include <thread>
#include <mutex>
#include <semaphore>
#include <stop_token>
int main()
{
    int failures = 0;
    std::binary_semaphore sem(1);
    sem.acquire(); bool first = true;
    bool second = sem.try_acquire();
    sem.release();
    sem.acquire(); bool third = true;
    if (!first || second || !third) failures |= 1;
    sem.release();

    std::barrier<> gate(1);
    gate.arrive_and_wait();

    std::stop_source source;
    std::stop_token token = source.get_token();
    int callbacks = 0;
    std::stop_callback callback(token, [&] { ++callbacks; });
    bool requested = source.request_stop();
    if (!requested || !token.stop_requested() || callbacks != 1) failures |= 4;

    std::stop_source stopped;
    stopped.request_stop();
    std::condition_variable_any cv;
    std::mutex mutex;
    std::unique_lock<std::mutex> lock(mutex);
    bool ready = false;
    bool wait_result = cv.wait(lock, stopped.get_token(), [&] { return ready; });
    if (wait_result || !stopped.stop_requested()) failures |= 8;
    if (failures & 1) std::puts("FAIL binary_semaphore");
    if (failures & 4) std::puts("FAIL stop callback");
    if (failures & 8) std::puts("FAIL stop-aware condition wait");
    if (failures == 0) std::puts("PASS std_full_20_sync_primitives");
    return failures;
}

#include <string>
#include <cstdio>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>

std::once_flag once;
int once_count = 0;
void run_once() { ++once_count; }
void no_work() {}
void call_once_worker() { std::call_once(once, run_once); }
int triple(int x) { return x * 3; }
int return_fifteen() { return 15; }
void notify_ready(std::mutex& m, std::condition_variable& cv, bool& ready)
{
    { std::lock_guard<std::mutex> lock(m); ready = true; }
    cv.notify_all();
}
int main()
{
    int failures = 0;
    std::thread empty;
    bool empty_ok = !empty.joinable();
    std::thread t(no_work);
    std::thread::id id = t.get_id();
    bool thread_ok = t.joinable() && id != std::thread::id{};
    std::thread::hardware_concurrency();
    t.join();
    if (!empty_ok || !thread_ok || t.joinable()) failures |= 1;

    std::mutex lock_a, lock_b;
    std::lock(lock_a, lock_b);
    lock_a.unlock(); lock_b.unlock();
    std::mutex lock_c, lock_d;
    { std::scoped_lock both(lock_c, lock_d); }
    std::recursive_mutex recursive;
    recursive.lock(); recursive.lock(); recursive.unlock(); recursive.unlock();
    std::recursive_timed_mutex recursive_timed;
    bool recursive_timed_ok = recursive_timed.try_lock_for(std::chrono::milliseconds(20));
    if (recursive_timed_ok) recursive_timed.unlock();
    std::this_thread::yield(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
    bool thread_api_ok = std::this_thread::get_id() != std::thread::id{};
    std::timed_mutex timed;
    bool timed_ok = timed.try_lock_for(std::chrono::milliseconds(20));
    if (timed_ok) timed.unlock();
    if (!timed_ok || !recursive_timed_ok || !thread_api_ok) failures |= 2;

    std::thread o1(call_once_worker), o2(call_once_worker);
    o1.join(); o2.join();
    if (once_count != 1) failures |= 4;

    std::mutex m;
    std::condition_variable cv;
    bool ready = false;
    std::thread notifier(notify_ready, std::ref(m), std::ref(cv), std::ref(ready));
    std::unique_lock<std::mutex> lk(m);
    bool notified = cv.wait_for(lk, std::chrono::seconds(5), [&] { return ready; });
    lk.unlock(); notifier.join();
    if (!notified || !ready) failures |= 8;

    std::packaged_task<int(int)> task(triple);
    auto result = task.get_future();
    task(7);
    std::promise<int> promise;
    std::shared_future<int> shared = promise.get_future().share();
    promise.set_value(9);
    auto async_result = std::async(std::launch::async, return_fifteen);
    if (result.get() != 21 || shared.get() != 9 || async_result.get() != 15) failures |= 16;

    std::atomic<int> value{4};
    int expected = 4;
    bool exchanged = value.compare_exchange_strong(expected, 8);
    int old = value.exchange(10);
    int ptr_values[3] = {3, 6, 9};
    std::atomic<int*> ptr{&ptr_values[0]};
    int* prior = ptr.fetch_add(1);
    int* after_fetch = ptr.load();
    int* next = ++ptr;
    std::atomic<float> f{1.5f};
    float oldf = f.fetch_add(0.5f);
    std::atomic_flag flag = ATOMIC_FLAG_INIT;
    flag.clear();
    bool first = flag.test_and_set();
    bool observed = flag.test();
    bool second = flag.test_and_set();
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (!exchanged || expected != 4 || old != 8 || value.load() != 10 || prior != &ptr_values[0] || *prior != 3 || after_fetch != &ptr_values[1] || *after_fetch != 6 || next != &ptr_values[2] || *next != 9 || oldf != 1.5f || f.load() != 2.0f || first || !observed || !second) failures |= 32;
    if (failures & 1) std::puts("FAIL thread state");
    if (failures & 2) std::puts("FAIL timed mutex");
    if (failures & 4) std::puts("FAIL call_once");
    if (failures & 8) std::puts("FAIL condition_variable wait_for");
    if (failures & 16) std::puts("FAIL futures");
    if (failures & 32) std::puts("FAIL atomic operations");
    if (failures == 0) std::puts("PASS std_full_11_concurrency_full");
    return failures;
}

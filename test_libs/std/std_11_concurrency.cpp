// C++20 equivalent of std_11_concurrency.cb (compile-time parity baseline)
#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
#include <cstdio>

int main() {
    int failures = 0;
    std::atomic<int> total{0}; std::thread a([&] { total.fetch_add(2); }); std::thread b([&] { total.fetch_add(3); }); a.join(); b.join();
    std::mutex m; int guarded = 0; { std::lock_guard<std::mutex> lock(m); guarded = 7; } { std::unique_lock<std::mutex> lock(m); guarded += 1; }
    std::mutex gate; std::condition_variable cv; bool ready = false;
    std::thread notifier([&] { std::lock_guard<std::mutex> lock(gate); ready = true; cv.notify_one(); });
    { std::unique_lock<std::mutex> lock(gate); while (!ready) cv.wait(lock); }
    notifier.join();
    std::promise<int> p; std::future<int> f = p.get_future(); p.set_value(11);
    int asyncv = std::async(std::launch::async, [] { return 13; }).get();
    if (total.load() != 5 || guarded != 8 || f.get() != 11 || asyncv != 13) { std::printf("FAIL concurrency\n"); failures |= 1; }
    if (!failures) std::printf("PASS std_11_concurrency\n");
    return failures;
}

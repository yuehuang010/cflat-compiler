#include <string>
#include <cstdio>
#include <mutex>
int main()
{
    int failures = 0;
    std::mutex a, b;
    std::unique_lock<std::mutex> deferred(a, std::defer_lock);
    std::unique_lock<std::mutex> attempted(b, std::try_to_lock);
    bool ok = !deferred.owns_lock() && attempted.owns_lock();
    if (!ok) { printf("FAIL defer/try tags\n"); failures |= 1; }
    std::mutex c, d;
    std::unique_lock<std::mutex> left(c, std::defer_lock), right(d, std::defer_lock);
    std::lock(left, right);
    bool lock_ok = left.owns_lock() && right.owns_lock();
    if (!lock_ok) { printf("FAIL lock tags\n"); failures |= 2; }
    left.unlock(); right.unlock();
    deferred.lock();
    bool deferred_ok = deferred.owns_lock();
    if (!deferred_ok) { printf("FAIL deferred lock\n"); failures |= 4; }
    if (failures == 0) std::puts("PASS std_full_11_934_unique_lock_tags");
    return failures;
}

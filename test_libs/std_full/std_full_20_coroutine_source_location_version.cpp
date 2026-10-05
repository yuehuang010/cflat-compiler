#include <string>
#include <cstdio>
#include <coroutine>
#include <source_location>

int main()
{
    int failures = 0;
    std::suspend_always always;
    std::suspend_never never;
    if (always.await_ready() || !never.await_ready()) failures |= 1;
    std::coroutine_handle<> null_handle;
    auto from_null = std::coroutine_handle<>::from_address(nullptr);
    if (null_handle || from_null || from_null.address() != nullptr) failures |= 4;
    std::source_location location{};
    if (location.line() != 0 || location.column() != 0 || location.file_name()[0] != '\0') failures |= 8;
    if (failures & 1) std::puts("FAIL awaiters");
    if (failures & 4) std::puts("FAIL null coroutine handle");
    if (failures & 8) std::puts("FAIL source_location default");
    if (failures == 0) std::puts("PASS std_full_20_coroutine_source_location_version");
    return failures;
}

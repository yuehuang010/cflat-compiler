// C++20 equivalent of std_full_11_913_function_pointer_typedef.cb (compile-time parity baseline)
#include <string>
#include <new>
#include <csignal>
#include <cstdio>

int handler_calls = 0;
void my_handler() { handler_calls++; }
void signal_handler(int s) { handler_calls += s != 0; }

int main() {
    int failures = 0;
    auto signal_prev = std::signal(SIGINT, signal_handler);
    std::signal(SIGINT, signal_prev);
    std::new_handler old = std::set_new_handler(my_handler);
    auto cur = std::get_new_handler();
    cur();
    std::set_new_handler(old);
    if (handler_calls != 1 || std::get_new_handler() != old) { printf("FAIL new_handler typedef\n"); failures |= 1; }
    if (failures == 0) printf("PASS std_full_11_913_function_pointer_typedef\n");
    return failures;
}

// C++20 equivalent of std_full_11_diagnostics_c.cb (compile-time parity baseline)
#include <string>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstdio>

volatile int got_signal = 0;
void on_signal(int s) { got_signal = s; }

int main() {
    int failures = 0;
    if (ERANGE == EDOM || ERANGE == EINVAL || EDOM == EINVAL || ERANGE <= 0) { printf("FAIL errno constants\n"); failures |= 1; }
    void (*prev)(int) = std::signal(SIGINT, on_signal);
    int rc = std::raise(SIGINT);
    if (rc != 0 || got_signal != SIGINT) { printf("FAIL signal raise\n"); failures |= 2; }
    std::signal(SIGINT, on_signal);
    void (*mine)(int) = std::signal(SIGINT, SIG_DFL);
    got_signal = 0; if (mine == nullptr) { printf("FAIL signal restore null\n"); failures |= 4; } else mine(7);
    if (got_signal != 7) { printf("FAIL signal restore\n"); failures |= 4; }
    std::signal(SIGINT, prev);
    got_signal = 0;
    std::signal(SIGTERM, on_signal);
    std::raise(SIGTERM);
    std::signal(SIGTERM, SIG_DFL);
    if (got_signal != SIGTERM) { printf("FAIL sigterm\n"); failures |= 8; }
    if (failures == 0) printf("PASS std_full_11_diagnostics_c\n");
    return failures;
}

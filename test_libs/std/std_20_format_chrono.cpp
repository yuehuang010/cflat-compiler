// C++20 equivalent of std_20_format_chrono.cb (compile-time parity baseline)
#include <chrono>
#include <cstdio>
#include <format>
#include <string>

int main()
{
    int failures = 0;
    if (std::format("{} {:4d} {:.2f} {:>8}", 7, 12, 1.25, "x") != "7   12 1.25        x") failures |= 1;
    using namespace std::chrono;
    year_month_day date{year{2024}, month{2}, day{28}};
    auto next = sys_days{date} + days{2};
    year_month_day after{next};
    weekday wd{sys_days{year{2024}/January/1}};
    hh_mm_ss tod{seconds{7384}};
    if (after.day() != day{1} || after.month() != month{3} || wd.c_encoding() != 1 || tod.hours().count() != 2 || tod.minutes().count() != 3 || tod.seconds().count() != 4) failures |= 2;
    return failures;
}

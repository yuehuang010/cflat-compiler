#include <string>
#include <chrono>
#include <ctime>
#include <format>
#include <cstdio>
#include <cstring>
int main()
{
    int failures = 0;
    using namespace std::chrono;
    milliseconds ms(1550);
    milliseconds nms(-1550);
    if (floor<seconds>(ms).count() != 1 || ceil<seconds>(ms).count() != 2 || round<seconds>(ms).count() != 2 || floor<seconds>(nms).count() != -2 || ceil<seconds>(nms).count() != -1 || round<seconds>(nms).count() != -2 || abs(nms).count() != 1550)
    { printf("FAIL floor/ceil/round/abs\n"); failures |= 1; }
    time_point<system_clock, milliseconds> tp(milliseconds(5500));
    auto ts = time_point_cast<seconds>(tp);
    if (ts.time_since_epoch().count() != 5 || (tp - time_point<system_clock, milliseconds>(milliseconds(500))).count() != 5000)
    { printf("FAIL time_point_cast\n"); failures |= 2; }
    system_clock::time_point epoch = system_clock::from_time_t(0);
    std::time_t back = system_clock::to_time_t(epoch + hours(1));
    std::time_t rt = system_clock::to_time_t(system_clock::from_time_t(86400));
    if (back != 3600 || rt != 86400) { printf("FAIL to_time_t\n"); failures |= 4; }
    file_clock::time_point fnow = file_clock::now();
    file_clock::time_point fnow2 = file_clock::now();
    if (!(fnow2 >= fnow)) { printf("FAIL file_clock\n"); failures |= 8; }
    if (!year(2024).is_leap() || year(2023).is_leap() || !year(2000).is_leap() || year(1900).is_leap()) { printf("FAIL year::is_leap\n"); failures |= 16; }
    month_day_last mdl(month(2));
    year_month_day_last ymdl(year(2024), month_day_last(month(2)));
    year_month_day_last ymdl2(year(2023), month_day_last(month(2)));
    if (!mdl.ok() || unsigned(ymdl.day()) != 29u || unsigned(ymdl2.day()) != 28u) { printf("FAIL month_day_last\n"); failures |= 32; }
    year_month_day ymd(year(2024), month(3), day(15));
    if (!ymd.ok() || int(ymd.year()) != 2024 || unsigned(ymd.month()) != 3u || unsigned(ymd.day()) != 15u) { printf("FAIL year_month_day\n"); failures |= 64; }
    std::string f1 = std::format("{:%Y-%m-%d}", ymd);
    std::string f2 = std::format("{:%H:%M:%S}", seconds(3725));
    if (f1 != "2024-03-15" || f2 != "01:02:05") { printf("FAIL chrono format\n"); failures |= 128; }
    std::time_t t0 = std::time(nullptr);
    std::clock_t c0 = std::clock();
    if (t0 <= 0 || c0 < 0 || std::difftime(t0 + 10, t0) != 10.0) { printf("FAIL time/clock/difftime\n"); failures |= 256; }
    std::time_t fixed = 951782400; // 2000-02-29 00:00:00 UTC
    std::tm* g = std::gmtime(&fixed);
    if (g->tm_year != 100 || g->tm_mon != 1 || g->tm_mday != 29 || g->tm_hour != 0 || g->tm_min != 0 || g->tm_sec != 0 || g->tm_wday != 2 || g->tm_yday != 59)
    { printf("FAIL gmtime\n"); failures |= 512; }
    char buf[32] = {0};
    std::size_t n = std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", g);
    if (n != 19 || std::string(buf) != "2000-02-29 00:00:00") { printf("FAIL strftime\n"); failures |= 1024; }
    std::tm tmv = {};
    tmv.tm_year = 100; tmv.tm_mon = 0; tmv.tm_mday = 1;
    if (std::mktime(&tmv) == (std::time_t)-1) { printf("FAIL mktime\n"); failures |= 2048; }
    if (failures == 0) printf("PASS std_full_11_chrono_ctime\n");
    return failures;
}

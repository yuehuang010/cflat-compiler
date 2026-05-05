// C++ hashset / dictionary throughput comparison.
// Mirrors perf_hashset_dict.cb (CFlat) for direct cross-language comparison.
//
// Uses std::unordered_set<int> and std::unordered_map<int,int>.
// Same three operations (add, contains/get, remove) at the same sizes (N = 10,
// 500, 10000) with the same repeat counts and the same warm-up strategy.
//
// Build (MSVC, from repo root):
//   cmd /c "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 ^
//     && cl /O2 /std:c++17 /EHsc performance\perf_hashset_dict.cpp /Feout\perf_hashset_dict_cpp.exe

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>

using Clock    = std::chrono::steady_clock;
using Micros   = std::chrono::microseconds;

static int64_t now_us() {
    return std::chrono::duration_cast<Micros>(Clock::now().time_since_epoch()).count();
}

// ── hashset<int> ─────────────────────────────────────────────────────────────

static void bench_hashset(int n, int repeats) {
    int64_t total_ops = (int64_t)n * repeats;

    // -- add (pre-warmed: first fill triggers rehash; timed passes use reserve) --
    int64_t add_us;
    {
        std::unordered_set<int> s;
        s.reserve(n * 2);  // match CFlat warm-up: pre-allocate capacity
        for (int i = 0; i < n; i++) s.insert(i);
        s.clear();

        int64_t t0 = now_us();
        for (int r = 0; r < repeats; r++) {
            for (int i = 0; i < n; i++) s.insert(i);
            s.clear();
        }
        add_us = now_us() - t0;
        if (add_us == 0) add_us = 1;
    }

    // -- contains (pre-filled, hit case only) --
    int64_t cnt_us;
    volatile int64_t found = 0;
    {
        std::unordered_set<int> s;
        for (int i = 0; i < n; i++) s.insert(i);

        int64_t t0 = now_us();
        for (int r = 0; r < repeats; r++)
            for (int i = 0; i < n; i++)
                if (s.count(i)) found++;
        cnt_us = now_us() - t0;
        if (cnt_us == 0) cnt_us = 1;
    }

    // -- remove (pause-equivalent: only the erase loop is timed; refill outside) --
    int64_t rem_us = 0;
    {
        std::unordered_set<int> s;
        s.reserve(n * 2);
        for (int i = 0; i < n; i++) s.insert(i);

        for (int r = 0; r < repeats; r++) {
            int64_t t0 = now_us();
            for (int i = 0; i < n; i++) s.erase(i);
            rem_us += now_us() - t0;
            // refill outside timer
            s.clear();
            for (int i = 0; i < n; i++) s.insert(i);
        }
        if (rem_us == 0) rem_us = 1;
    }

    printf("  N=%-6d  add %8lld us (%5lld ns/op)  contains %8lld us (%5lld ns/op)  remove %8lld us (%5lld ns/op)\n",
        n,
        (long long)add_us, (long long)(add_us * 1000 / total_ops),
        (long long)cnt_us, (long long)(cnt_us * 1000 / total_ops),
        (long long)rem_us, (long long)(rem_us * 1000 / total_ops));
}

// ── unordered_map<int,int> ───────────────────────────────────────────────────

static void bench_dict(int n, int repeats) {
    int64_t total_ops = (int64_t)n * repeats;

    // -- add --
    int64_t add_us;
    {
        std::unordered_map<int, int> d;
        d.reserve(n * 2);
        for (int i = 0; i < n; i++) d.emplace(i, i * 2);
        d.clear();

        int64_t t0 = now_us();
        for (int r = 0; r < repeats; r++) {
            for (int i = 0; i < n; i++) d.emplace(i, i * 2);
            d.clear();
        }
        add_us = now_us() - t0;
        if (add_us == 0) add_us = 1;
    }

    // -- get --
    int64_t get_us;
    volatile int64_t sum = 0;
    {
        std::unordered_map<int, int> d;
        for (int i = 0; i < n; i++) d.emplace(i, i * 2);

        int64_t t0 = now_us();
        for (int r = 0; r < repeats; r++)
            for (int i = 0; i < n; i++)
                sum += d.at(i);
        get_us = now_us() - t0;
        if (get_us == 0) get_us = 1;
    }

    // -- remove --
    int64_t rem_us = 0;
    {
        std::unordered_map<int, int> d;
        d.reserve(n * 2);
        for (int i = 0; i < n; i++) d.emplace(i, i * 2);

        for (int r = 0; r < repeats; r++) {
            int64_t t0 = now_us();
            for (int i = 0; i < n; i++) d.erase(i);
            rem_us += now_us() - t0;
            d.clear();
            for (int i = 0; i < n; i++) d.emplace(i, i * 2);
        }
        if (rem_us == 0) rem_us = 1;
    }

    printf("  N=%-6d  add %8lld us (%5lld ns/op)  get %8lld us (%5lld ns/op)  remove %8lld us (%5lld ns/op)\n",
        n,
        (long long)add_us, (long long)(add_us * 1000 / total_ops),
        (long long)get_us, (long long)(get_us * 1000 / total_ops),
        (long long)rem_us, (long long)(rem_us * 1000 / total_ops));
}

// ── main ─────────────────────────────────────────────────────────────────────

int main() {
    printf("\n--- std::unordered_set<int>: add / contains / remove ---\n");
    bench_hashset(10,    500000);
    bench_hashset(500,   5000);
    bench_hashset(10000, 250);

    printf("\n--- std::unordered_map<int,int>: add / get / remove ---\n");
    bench_dict(10,    500000);
    bench_dict(500,   5000);
    bench_dict(10000, 250);

    return 0;
}

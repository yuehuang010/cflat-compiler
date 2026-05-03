// C++ throughput comparison — single-threaded loop vs raw threads + double-buffered stream.
// Mirrors perf_yes_compare.cb (CFlat) for direct cross-language comparison.
//
// Build (MSVC, from repo root — requires the x64 developer environment):
//   cmd /c "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 ^
//     && cl /O2 /std:c++17 /EHsc performance\perf_yes_compare.cpp /Feperformance\perf_yes_compare.exe
//
// Run:
//   performance\perf_yes_compare.exe

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

static const int BENCH_SECS = 5;
static const int BUF_BYTES  = 65536;

// ── Double-buffered text pipe (mirrors stream.cb) ─────────────────────────────
//
// Two fixed-size text buffers (bufA/bufB).  The producer writes into the active
// write buffer.  When it is full the buffer is handed to the consumer via
// pendingFull and the producer blocks until the consumer returns an empty buffer
// via emptyPool.  Blocking only occurs when both buffers are simultaneously full
// (consumer is slow) — the same guarantee as the CFlat stream struct.

struct Stream {
    char  bufA[BUF_BYTES + 1];
    char  bufB[BUF_BYTES + 1];

    char* wBuf;     // producer's active write buffer
    int   wPos;     // bytes filled in wBuf

    std::mutex              mu;
    std::condition_variable cvFull;   // wakes consumer: a full buffer is ready
    std::condition_variable cvEmpty;  // wakes producer: an empty buffer returned

    char* pendingFull;  // filled buffer waiting for the consumer (nullptr = none)
    char* emptyPool;    // empty buffer returned by consumer (nullptr = none)
    bool  eof;

    Stream() : wBuf(bufA), wPos(0), pendingFull(nullptr), emptyPool(bufB), eof(false) {}

    // Flush the current write buffer to the consumer and grab an empty one back.
    void _flush() {
        if (wPos == 0) return;
        wBuf[wPos] = '\0';

        std::unique_lock<std::mutex> lk(mu);

        // Wait for the consumer to drain the previous full buffer.
        cvFull.wait(lk, [&]{ return pendingFull == nullptr; });
        pendingFull = wBuf;
        cvFull.notify_one();

        // Wait for the consumer to return an empty buffer.
        cvEmpty.wait(lk, [&]{ return emptyPool != nullptr; });
        wBuf      = emptyPool;
        emptyPool = nullptr;
        wPos      = 0;
    }

    // Producer: append raw bytes.
    void write(const char* data, int len) {
        while (len > 0) {
            int space = BUF_BYTES - wPos;
            int take  = len < space ? len : space;
            std::memcpy(wBuf + wPos, data, take);
            wPos += take;
            data += take;
            len  -= take;
            if (wPos >= BUF_BYTES) _flush();
        }
    }

    // Producer: flush remaining data then send EOF sentinel.
    void close() {
        _flush();
        std::unique_lock<std::mutex> lk(mu);
        cvFull.wait(lk, [&]{ return pendingFull == nullptr; });
        eof = true;
        cvFull.notify_all();
    }

    // Consumer: block until a full buffer is available; returns its pointer and length.
    // Returns nullptr on EOF.
    char* read_buf(int* out_len) {
        std::unique_lock<std::mutex> lk(mu);
        cvFull.wait(lk, [&]{ return pendingFull != nullptr || eof; });
        if (pendingFull == nullptr) { *out_len = 0; return nullptr; }
        char* buf   = pendingFull;
        *out_len    = static_cast<int>(std::strlen(buf));
        pendingFull = nullptr;
        cvFull.notify_one();  // unblock producer waiting for a free slot
        return buf;
    }

    // Consumer: return a processed buffer to the producer's empty pool.
    void return_buf(char* buf) {
        std::unique_lock<std::mutex> lk(mu);
        emptyPool = buf;
        cvEmpty.notify_one();
    }
};

// ── Variant 2: raw threads + double-buffered stream ───────────────────────────

static Stream g_stream;
static std::atomic<bool> g_stop { false };
static int64_t g_count = 0;

static void producer_thread() {
    static const char line[] = "Y\n";
    while (!g_stop.load(std::memory_order_relaxed))
        g_stream.write(line, 2);
    g_stream.close();
}

static void consumer_thread() {
    int   len;
    int64_t count = 0;
    while (true) {
        char* buf = g_stream.read_buf(&len);
        if (buf == nullptr) break;
        // Count newlines — each "Y\n" is one iteration.
        for (int i = 0; i < len; i++)
            if (buf[i] == '\n') count++;
        g_stream.return_buf(buf);
    }
    g_count = count;
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main() {
    const int     dur_ms = BENCH_SECS * 1000;
    const int64_t dur_s  = BENCH_SECS;

    printf("C++ throughput comparison (each variant: %d seconds)\n\n", BENCH_SECS);

    // Variant 1: single-threaded tight loop (ceiling / reference).
    printf("[1/2] single-threaded loop\n");
    int64_t st_count = 0;
    {
        auto t0 = std::chrono::steady_clock::now();
        while (true) {
            st_count++;
            // Check time every 64 K iterations to amortise clock() overhead.
            if ((st_count & 0xFFFF) == 0) {
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0).count();
                if (ms >= dur_ms) break;
            }
        }
    }
    int64_t st_tps = st_count / dur_s;

    // Variant 2: two raw std::threads + double-buffered stream.
    printf("[2/2] raw threads + double-buffer stream\n");
    {
        std::thread tProd(producer_thread);
        std::thread tCons(consumer_thread);
        std::this_thread::sleep_for(std::chrono::milliseconds(dur_ms));
        g_stop.store(true, std::memory_order_relaxed);
        tProd.join();
        tCons.join();
    }
    int64_t stream_tps = g_count / dur_s;

    printf("\n");
    printf("%-32s %15s\n", "Variant",                         "iter/s");
    printf("%-32s %15s\n", "--------------------------------", "---------------");
    printf("%-32s %15lld\n", "single-threaded loop",           (long long)st_tps);
    printf("%-32s %15lld\n", "raw threads + stream",           (long long)stream_tps);
    printf("\n");
    printf("Compare against perf_yes_compare.cb (CFlat):\n");
    printf("  'single-threaded loop'  <-> 'single-threaded loop'\n");
    printf("  'raw threads + stream'  <-> 'stream + program'\n");

    return 0;
}

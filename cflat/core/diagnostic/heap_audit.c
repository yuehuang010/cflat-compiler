// heap_audit.c - timing-independent double-free / heap-audit detector for cflat.
//
// Opt-in debugging tool. core/diagnostic/heap_audit.cb imports this file (so it is
// compiled by clang-cl and merged into the image by lld-link, like crashdump.c) and
// HeapAudit.enable() installs the two cflat-side hooks (__alloc_audit_hook /
// __free_audit_hook in memory.cb) to point at the two entry points below.
//
// Every allocation that flows through operator new is recorded live (with its size);
// every free that flows through operator delete is checked. A free of a pointer that
// is already recorded freed is a DOUBLE FREE: we print "ptr=0x... size=..." to stderr
// and stop the process loudly and deterministically - at the bad free itself, no
// matter when or on which thread it happens. This catches the double-free/UAF of a
// large allocation (e.g. the ~2MB channel<Task> ring buffer) whose second free hits a
// decommitted page (AV 0xC0000005) far away from the actual bug; size in the report
// discriminates the big ring buffer from small ctx/handle allocations.
//
// Output goes through Win32 WriteFile (GetStdHandle(STD_ERROR_HANDLE)) rather than the
// CRT's stdio: cflat's runtime (cruntime.cb) defines its own strong sprintf/vsprintf,
// so linking any CRT stdio printf-family here would collide at link time. A tiny
// self-contained formatter (mirroring crashdump.c) keeps this object stdio-free.
//
// The audit table never allocates through the cflat allocator (that would recurse
// through operator new/delete); it is a large static, zero-initialized BSS array. The
// table is protected by a CRITICAL_SECTION initialized by a CRT initializer before
// main. ASCII only in all output.

#include <windows.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Output helpers (no CRT stdio; mirror crashdump.c's self-contained formatter).
// ---------------------------------------------------------------------------

static void cflat_ha_append_(char* buf, int cap, int* pos, const char* s)
{
    while (*s && *pos < cap - 1)
        buf[(*pos)++] = *s++;
    buf[*pos] = '\0';
}

static void cflat_ha_append_hex_(char* buf, int cap, int* pos, unsigned long long v, int width)
{
    char tmp[17];
    int n = 0;
    do {
        int d = (int)(v & 0xF);
        tmp[n++] = (char)(d < 10 ? ('0' + d) : ('a' + d - 10));
        v >>= 4;
    } while (v && n < 16);
    while (n < width) tmp[n++] = '0';
    while (n > 0 && *pos < cap - 1)
        buf[(*pos)++] = tmp[--n];
    buf[*pos] = '\0';
}

static void cflat_ha_append_dec_(char* buf, int cap, int* pos, unsigned long long v)
{
    char tmp[24];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + (int)(v % 10));
        v /= 10;
    } while (v && n < 24);
    while (n > 0 && *pos < cap - 1)
        buf[(*pos)++] = tmp[--n];
    buf[*pos] = '\0';
}

static void cflat_ha_write_stderr_(const char* s)
{
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h == INVALID_HANDLE_VALUE || h == NULL)
        return;
    DWORD len = (DWORD)strlen(s);
    DWORD written = 0;
    WriteFile(h, s, len, &written, NULL);
}

// ---------------------------------------------------------------------------
// Audit table: open-addressed pointer -> {state, size}, linear probing.
//
// Entries are never deleted; a freed entry stays in the table marked FREED (keeping
// its size) so a later free of the same pointer is recognized as a double free. An
// allocation that reuses a previously-freed address (legitimate malloc reuse) flips
// the existing entry back to LIVE with the new size - no false positive.
// ---------------------------------------------------------------------------

#define CFLAT_HA_STATE_EMPTY 0
#define CFLAT_HA_STATE_LIVE  1
#define CFLAT_HA_STATE_FREED 2

// 1<<20 slots. Generously sized for a debug tool; ~20MB BSS at 20 bytes/slot rounded.
#define CFLAT_HA_SLOTS (1u << 20)
#define CFLAT_HA_MASK  (CFLAT_HA_SLOTS - 1u)

typedef struct {
    void*              key;    // allocation pointer; NULL means empty slot
    unsigned long long size;   // size passed to operator new
    int                state;  // CFLAT_HA_STATE_*
} cflat_ha_slot;

static cflat_ha_slot  cflat_ha_table[CFLAT_HA_SLOTS];
static CRITICAL_SECTION cflat_ha_lock;
static int            cflat_ha_lock_ready = 0;

// Fibonacci hash of a pointer, folded to the table size.
static unsigned int cflat_ha_hash_(void* p)
{
    unsigned long long x = (unsigned long long)(ULONG_PTR)p;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return (unsigned int)(x & CFLAT_HA_MASK);
}

// Mark an allocation live. Clears any stale FREED record for a reused address.
void cflat_heap_audit_alloc(void* p, unsigned long long size)
{
    if (p == NULL || !cflat_ha_lock_ready) return;
    EnterCriticalSection(&cflat_ha_lock);
    unsigned int idx = cflat_ha_hash_(p);
    for (unsigned int probe = 0; probe < CFLAT_HA_SLOTS; ++probe)
    {
        cflat_ha_slot* slot = &cflat_ha_table[idx];
        if (slot->state == CFLAT_HA_STATE_EMPTY)
        {
            slot->key   = p;
            slot->size  = size;
            slot->state = CFLAT_HA_STATE_LIVE;
            break;
        }
        if (slot->key == p)
        {
            // Re-used (or re-marked) address: flip back to live with the new size.
            slot->size  = size;
            slot->state = CFLAT_HA_STATE_LIVE;
            break;
        }
        idx = (idx + 1u) & CFLAT_HA_MASK;
    }
    // Table full of distinct live addresses: silently stop tracking new ones rather
    // than false-positive. A 1<<20 table is far larger than any realistic live set.
    LeaveCriticalSection(&cflat_ha_lock);
}

// Report a double free and stop the process loudly and unambiguously.
static void cflat_ha_report_double_free_(void* p, unsigned long long size)
{
    char line[160];
    int pos = 0;
    cflat_ha_append_(line, sizeof(line), &pos,
                     "\n*** cflat heap-audit: DOUBLE FREE ptr=0x");
    cflat_ha_append_hex_(line, sizeof(line), &pos, (unsigned long long)(ULONG_PTR)p, 0);
    cflat_ha_append_(line, sizeof(line), &pos, " size=");
    cflat_ha_append_dec_(line, sizeof(line), &pos, size);
    cflat_ha_append_(line, sizeof(line), &pos, " thread=");
    cflat_ha_append_dec_(line, sizeof(line), &pos, (unsigned long long)GetCurrentThreadId());
    cflat_ha_append_(line, sizeof(line), &pos, " ***\n");
    cflat_ha_write_stderr_(line);

    // Stop loudly. Under a debugger, break in so the bad free can be inspected at the
    // exact site. Otherwise terminate with a distinctive exit code (0xDF) so a
    // non-interactive run records that the audit fired (not a stray AV / abort). An
    // unguarded __debugbreak with no debugger raises an unhandled breakpoint that kills
    // the process with exit 3 before TerminateProcess runs, masking the 0xDF code.
    if (IsDebuggerPresent())
        __debugbreak();
    TerminateProcess(GetCurrentProcess(), 0xDF);
    ExitProcess(0xDF);  // belt-and-suspenders if TerminateProcess is intercepted
}

// Check/record a free. Double free -> report + abort. Live -> mark freed (keep size).
// Unknown pointer (allocated before enable(), or evicted) -> ignore.
void cflat_heap_audit_free(void* p)
{
    if (p == NULL || !cflat_ha_lock_ready) return;
    int doubleFree = 0;
    unsigned long long size = 0;
    EnterCriticalSection(&cflat_ha_lock);
    unsigned int idx = cflat_ha_hash_(p);
    for (unsigned int probe = 0; probe < CFLAT_HA_SLOTS; ++probe)
    {
        cflat_ha_slot* slot = &cflat_ha_table[idx];
        if (slot->state == CFLAT_HA_STATE_EMPTY)
            break;  // pointer not tracked - ignore
        if (slot->key == p)
        {
            if (slot->state == CFLAT_HA_STATE_FREED)
            {
                doubleFree = 1;
                size = slot->size;
            }
            else
            {
                slot->state = CFLAT_HA_STATE_FREED;
            }
            break;
        }
        idx = (idx + 1u) & CFLAT_HA_MASK;
    }
    LeaveCriticalSection(&cflat_ha_lock);

    if (doubleFree)
        cflat_ha_report_double_free_(p, size);
}

// ---------------------------------------------------------------------------
// Leak scan. Walk the whole table under the lock and report every slot still in
// the LIVE state - an allocation through operator new with no matching operator
// delete. Prints "ptr=0x... size=..." per leak to stderr and returns the count.
//
// Honest limits (same as the double-free path): only allocations made AFTER
// enable() that flow through operator new/delete are tracked. A still-LIVE entry
// at program exit is a leak; a buffer freed via a raw allocator path that bypasses
// operator delete will look LIVE here (false positive) - so this is a debug oracle,
// not a proof of leak-freedom for arbitrary code.
unsigned long long cflat_heap_audit_report_leaks(void)
{
    if (!cflat_ha_lock_ready) return 0;
    unsigned long long liveCount = 0;
    EnterCriticalSection(&cflat_ha_lock);
    for (unsigned int i = 0; i < CFLAT_HA_SLOTS; ++i)
    {
        cflat_ha_slot* slot = &cflat_ha_table[i];
        if (slot->state != CFLAT_HA_STATE_LIVE)
            continue;
        liveCount++;
        char line[160];
        int pos = 0;
        cflat_ha_append_(line, sizeof(line), &pos,
                         "*** cflat heap-audit: LEAK ptr=0x");
        cflat_ha_append_hex_(line, sizeof(line), &pos,
                             (unsigned long long)(ULONG_PTR)slot->key, 0);
        cflat_ha_append_(line, sizeof(line), &pos, " size=");
        cflat_ha_append_dec_(line, sizeof(line), &pos, slot->size);
        cflat_ha_append_(line, sizeof(line), &pos, " ***\n");
        cflat_ha_write_stderr_(line);
    }
    LeaveCriticalSection(&cflat_ha_lock);
    return liveCount;
}

// ---------------------------------------------------------------------------
// One-time CRITICAL_SECTION init via a CRT initializer (runs before main, single
// threaded). Mirrors crashdump.c's .CRT$XCU registration.
// ---------------------------------------------------------------------------

static int cflat_ha_init_(void)
{
    InitializeCriticalSection(&cflat_ha_lock);
    cflat_ha_lock_ready = 1;
    return 0;
}

#pragma section(".CRT$XCU", read)
__declspec(allocate(".CRT$XCU")) int (*cflat_ha_init_ptr_)(void) = cflat_ha_init_;

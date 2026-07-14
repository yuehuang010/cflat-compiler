// heap_audit.c - leak detector for cflat.
//
// Opt-in debugging tool. core/diagnostic/heap_audit.cb imports this file (so it is
// compiled by the platform C driver and merged into the image by the platform linker -
// clang-cl/lld-link on Windows, host clang/ld64.lld on macOS, host clang/lld on Linux,
// like crashdump.c) and HeapAudit.enable() installs the two cflat-side hooks
// (__alloc_audit_hook / __free_audit_hook in memory.cb) to point at the two entry
// points below.
//
// Every allocation that flows through operator new is recorded live (with its size);
// every free that flows through operator delete flips the matching record to freed.
// At a quiescent point cflat_heap_audit_report_leaks() walks the table and reports every
// allocation still live (a LEAK) to stderr. Reports are advisory and non-fatal: they do
// NOT change the exit code or abort. Double-free detection is intentionally NOT provided
// here - operator new/delete share the process heap with raw malloc/free, so a freed
// address reused by a raw allocator and later operator-deleted is indistinguishable from
// a real double free; --asan is the deterministic oracle for double-free / use-after-free.
//
// Output goes through a raw platform write (Win32 WriteFile to STD_ERROR_HANDLE; POSIX
// write(2) to fd 2) rather than the CRT/libc stdio: cflat's runtime (cruntime.cb) defines
// its own strong sprintf/vsprintf, so linking any stdio printf-family here would collide
// at link time. A tiny self-contained formatter (mirroring crashdump.c) keeps this object
// stdio-free.
//
// The audit table never allocates through the cflat allocator (that would recurse
// through operator new/delete); it is a large static, zero-initialized BSS array. The
// table is protected by a lock (a Win32 CRITICAL_SECTION, or a pthread_mutex_t on POSIX)
// initialized before main (a CRT$XCU initializer on Windows; a
// __attribute__((constructor)) on POSIX). ASCII only in all output.
//
// Symbolization: on Windows, DbgHelp (SymFromAddr / SymGetLineFromAddr64) resolves a
// name and, with a PDB (-g build), a file:line. On POSIX there is no PDB analog, so
// file:line is not available inline; dladdr() resolves the nearest exported symbol name
// plus its containing module and the offsets into each. To get file:line offline, run
// `atos -o <module> -l <module base>` (macOS) or `llvm-symbolizer` against the printed
// module + offset - not shelled out to here, since the macOS path is deliberately
// Xcode-free.

#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>

// Leak-site symbolization (SymFromAddr / SymGetLineFromAddr64) lives in DbgHelp. Pull the
// import lib in from this object so any image that includes heap_audit.c links it - the
// program may self-audit via HeapAudit.enable() without the compiler's --heap-audit flag,
// so we cannot rely on the linker driver adding dbghelp.lib for us.
#pragma comment(lib, "dbghelp.lib")
#else
#include <pthread.h>
#include <unistd.h>
#include <execinfo.h>
#include <dlfcn.h>
#include <stdint.h>
#endif

// ---------------------------------------------------------------------------
// Output helpers (no stdio; mirror crashdump.c's self-contained formatter). Shared
// unchanged by both platforms.
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

// Platform shim: write a NUL-terminated line to stderr.
#ifdef _WIN32
static void cflat_ha_write_stderr_(const char* s)
{
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h == INVALID_HANDLE_VALUE || h == NULL)
        return;
    DWORD len = (DWORD)strlen(s);
    DWORD written = 0;
    WriteFile(h, s, len, &written, NULL);
}
#else
static void cflat_ha_write_stderr_(const char* s)
{
    size_t len = strlen(s);
    size_t off = 0;
    while (off < len)
    {
        ssize_t n = write(2, s + off, len - off);
        if (n <= 0) break;
        off += (size_t)n;
    }
}
#endif

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

// Per-allocation captured stack depth. Enough to skip the hook/operator-new frames
// and still show a few user frames; the leak report symbolizes these (DbgHelp on
// Windows, dladdr on POSIX).
#define CFLAT_HA_FRAMES 8

// 1<<20 slots. Generously sized for a debug tool. ~88 bytes/slot with the captured
// stack -> ~90MB demand-zero BSS; reserved but only paged in for slots actually used.
#define CFLAT_HA_SLOTS (1u << 20)
#define CFLAT_HA_MASK  (CFLAT_HA_SLOTS - 1u)

typedef struct {
    void*              key;    // allocation pointer; NULL means empty slot
    unsigned long long size;   // size passed to operator new
    int                state;  // CFLAT_HA_STATE_*
    unsigned short     frameCount;            // captured stack depth (0 if none)
    void*              frames[CFLAT_HA_FRAMES]; // return addresses at allocation time
} cflat_ha_slot;

static cflat_ha_slot cflat_ha_table[CFLAT_HA_SLOTS];
static int           cflat_ha_lock_ready = 0;

#ifdef _WIN32
static CRITICAL_SECTION cflat_ha_lock;
#else
static pthread_mutex_t cflat_ha_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

// Fibonacci hash of a pointer, folded to the table size.
static unsigned int cflat_ha_hash_(void* p)
{
#ifdef _WIN32
    unsigned long long x = (unsigned long long)(ULONG_PTR)p;
#else
    unsigned long long x = (unsigned long long)(uintptr_t)p;
#endif
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return (unsigned int)(x & CFLAT_HA_MASK);
}

// ---------------------------------------------------------------------------
// Platform primitives. Each shim below has a Win32 body and a POSIX body; the
// shared logic (table, hash, alloc/free/report bodies) calls only these names.
// ---------------------------------------------------------------------------

#ifdef _WIN32
static void cflat_ha_lock_acquire_(void) { EnterCriticalSection(&cflat_ha_lock); }
static void cflat_ha_lock_release_(void) { LeaveCriticalSection(&cflat_ha_lock); }
#else
static void cflat_ha_lock_acquire_(void) { pthread_mutex_lock(&cflat_ha_lock); }
static void cflat_ha_lock_release_(void) { pthread_mutex_unlock(&cflat_ha_lock); }
#endif

// Capture up to maxFrames return addresses, skipping this function's own frame so the
// walk starts at the caller (operator new). Returns the number of frames captured.
// Force-inlined into cflat_heap_audit_alloc on both platforms so it contributes no stack
// frame of its own - without that, "skip 1 frame" would skip this shim's frame instead of
// cflat_heap_audit_alloc's, landing one frame too shallow (on cflat_heap_audit_alloc
// itself rather than operator new).
#ifdef _WIN32
static __forceinline unsigned short cflat_ha_capture_stack_(void** frames, int maxFrames)
{
    return CaptureStackBackTrace(1, (DWORD)maxFrames, frames, NULL);
}
#else
static inline unsigned short cflat_ha_capture_stack_(void** frames, int maxFrames)
    __attribute__((always_inline));
static inline unsigned short cflat_ha_capture_stack_(void** frames, int maxFrames)
{
    void* raw[CFLAT_HA_FRAMES + 1];
    int cap = maxFrames + 1;
    if (cap > CFLAT_HA_FRAMES + 1)
        cap = CFLAT_HA_FRAMES + 1;
    int n = backtrace(raw, cap);
    if (n <= 1)
        return 0;
    int count = n - 1;
    if (count > maxFrames)
        count = maxFrames;
    memcpy(frames, raw + 1, (size_t)count * sizeof(void*));
    return (unsigned short)count;
}
#endif

// Symbolization context threaded through sym-begin/sym-end/print-frame: the process
// HANDLE on Windows (DbgHelp needs it); an unused placeholder on POSIX (dladdr does not).
#ifdef _WIN32
typedef HANDLE cflat_ha_symctx;
#else
typedef int cflat_ha_symctx;
#endif

#ifdef _WIN32
static int cflat_ha_sym_begin_(cflat_ha_symctx* ctx)
{
    *ctx = GetCurrentProcess();
    if (!SymInitialize(*ctx, NULL, TRUE))
        return 0;
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    return 1;
}

static void cflat_ha_sym_end_(cflat_ha_symctx ctx)
{
    SymCleanup(ctx);
}
#else
static int cflat_ha_sym_begin_(cflat_ha_symctx* ctx)
{
    // dladdr needs no init/context; always available.
    *ctx = 0;
    return 1;
}

static void cflat_ha_sym_end_(cflat_ha_symctx ctx)
{
    (void)ctx;
}
#endif

// Symbolize one captured return address and print it as an indented frame under a LEAK
// line. Mirrors crashdump.c's cflat_print_frame_.
#ifdef _WIN32
// Resolves names from the PDB via DbgHelp. Without -g (no line info) it still prints
// the function name and address.
static void cflat_ha_print_frame_(cflat_ha_symctx ctx, int index, void* addr)
{
    HANDLE process = ctx;
    char symbolBuffer[sizeof(SYMBOL_INFO) + 512];
    SYMBOL_INFO* symbol = (SYMBOL_INFO*)symbolBuffer;
    memset(symbolBuffer, 0, sizeof(symbolBuffer));
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 511;

    DWORD64 a = (DWORD64)(ULONG_PTR)addr;
    DWORD64 symDisp = 0;
    const char* name = "<unknown>";
    if (SymFromAddr(process, a, &symDisp, symbol))
        name = symbol->Name;

    char line[1024];
    int pos = 0;
    cflat_ha_append_(line, sizeof(line), &pos, "      #");
    cflat_ha_append_dec_(line, sizeof(line), &pos, (unsigned long long)index);
    cflat_ha_append_(line, sizeof(line), &pos, " 0x");
    cflat_ha_append_hex_(line, sizeof(line), &pos, (unsigned long long)a, 16);
    cflat_ha_append_(line, sizeof(line), &pos, " ");
    cflat_ha_append_(line, sizeof(line), &pos, name);

    IMAGEHLP_LINE64 lineInfo;
    memset(&lineInfo, 0, sizeof(lineInfo));
    lineInfo.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
    DWORD lineDisp = 0;
    if (SymGetLineFromAddr64(process, a, &lineDisp, &lineInfo))
    {
        cflat_ha_append_(line, sizeof(line), &pos, " (");
        cflat_ha_append_(line, sizeof(line), &pos, lineInfo.FileName);
        cflat_ha_append_(line, sizeof(line), &pos, ":");
        cflat_ha_append_dec_(line, sizeof(line), &pos, (unsigned long long)lineInfo.LineNumber);
        cflat_ha_append_(line, sizeof(line), &pos, ")");
    }
    cflat_ha_append_(line, sizeof(line), &pos, "\n");
    cflat_ha_write_stderr_(line);
}
#else
// Resolves the nearest exported symbol via dladdr: name + offset into it, plus the
// containing module + offset into the module (for offline atos/llvm-symbolizer lookup).
// No file:line here - there is no PDB analog; falls back to the raw address if dladdr
// cannot resolve anything (e.g. a stripped or static-only symbol).
static void cflat_ha_print_frame_(cflat_ha_symctx ctx, int index, void* addr)
{
    (void)ctx;
    char line[1024];
    int pos = 0;
    cflat_ha_append_(line, sizeof(line), &pos, "      #");
    cflat_ha_append_dec_(line, sizeof(line), &pos, (unsigned long long)index);
    cflat_ha_append_(line, sizeof(line), &pos, " 0x");
    cflat_ha_append_hex_(line, sizeof(line), &pos, (unsigned long long)(uintptr_t)addr, 16);
    cflat_ha_append_(line, sizeof(line), &pos, " ");

    Dl_info info;
    memset(&info, 0, sizeof(info));
    int have = dladdr(addr, &info);

    if (have && info.dli_sname)
    {
        unsigned long long symOff =
            (unsigned long long)((const char*)addr - (const char*)info.dli_saddr);
        cflat_ha_append_(line, sizeof(line), &pos, info.dli_sname);
        cflat_ha_append_(line, sizeof(line), &pos, "+0x");
        cflat_ha_append_hex_(line, sizeof(line), &pos, symOff, 0);
    }
    else
    {
        cflat_ha_append_(line, sizeof(line), &pos, "<unknown>");
    }

    if (have && info.dli_fbase)
    {
        unsigned long long modOff =
            (unsigned long long)((const char*)addr - (const char*)info.dli_fbase);
        cflat_ha_append_(line, sizeof(line), &pos, " (");
        cflat_ha_append_(line, sizeof(line), &pos, info.dli_fname ? info.dli_fname : "?");
        cflat_ha_append_(line, sizeof(line), &pos, "+0x");
        cflat_ha_append_hex_(line, sizeof(line), &pos, modOff, 0);
        cflat_ha_append_(line, sizeof(line), &pos, ")");
    }
    cflat_ha_append_(line, sizeof(line), &pos, "\n");
    cflat_ha_write_stderr_(line);
}
#endif

// ---------------------------------------------------------------------------
// Shared entry points. Single copy for both platforms - only the primitives above
// are platform-specific.
// ---------------------------------------------------------------------------

// Mark an allocation live. Clears any stale FREED record for a reused address.
void cflat_heap_audit_alloc(void* p, unsigned long long size)
{
    if (p == NULL || !cflat_ha_lock_ready) return;

    // Capture the allocation call stack before taking the lock (stack capture is
    // self-contained and thread-safe). The frames walk out through operator new into
    // user code and let the leak report point at the allocation site. Symbolization
    // (file:line on Windows) needs the program built with -g.
    void* frames[CFLAT_HA_FRAMES];
    unsigned short frameCount = cflat_ha_capture_stack_(frames, CFLAT_HA_FRAMES);

    cflat_ha_lock_acquire_();
    unsigned int idx = cflat_ha_hash_(p);
    for (unsigned int probe = 0; probe < CFLAT_HA_SLOTS; ++probe)
    {
        cflat_ha_slot* slot = &cflat_ha_table[idx];
        if (slot->state == CFLAT_HA_STATE_EMPTY)
        {
            slot->key   = p;
            slot->size  = size;
            slot->state = CFLAT_HA_STATE_LIVE;
            slot->frameCount = frameCount;
            memcpy(slot->frames, frames, frameCount * sizeof(void*));
            break;
        }
        if (slot->key == p)
        {
            // Re-used (or re-marked) address: flip back to live with the new size and the
            // new allocation's stack, so a reported leak shows where it was last allocated.
            slot->size  = size;
            slot->state = CFLAT_HA_STATE_LIVE;
            slot->frameCount = frameCount;
            memcpy(slot->frames, frames, frameCount * sizeof(void*));
            break;
        }
        idx = (idx + 1u) & CFLAT_HA_MASK;
    }
    // Table full of distinct live addresses: silently stop tracking new ones rather
    // than false-positive. A 1<<20 table is far larger than any realistic live set.
    cflat_ha_lock_release_();
}

// Record a free: flip a tracked LIVE allocation to FREED so the leak scan does not report
// it. A FREED slot is left in place (an allocation reusing the address flips it back to
// LIVE in the alloc hook). Unknown pointers (allocated before enable(), or evicted) are
// ignored. No double-free detection - see the file header and use --asan instead.
void cflat_heap_audit_free(void* p)
{
    if (p == NULL || !cflat_ha_lock_ready) return;
    cflat_ha_lock_acquire_();
    unsigned int idx = cflat_ha_hash_(p);
    for (unsigned int probe = 0; probe < CFLAT_HA_SLOTS; ++probe)
    {
        cflat_ha_slot* slot = &cflat_ha_table[idx];
        if (slot->state == CFLAT_HA_STATE_EMPTY)
            break;  // pointer not tracked - ignore
        if (slot->key == p)
        {
            slot->state = CFLAT_HA_STATE_FREED;
            break;
        }
        idx = (idx + 1u) & CFLAT_HA_MASK;
    }
    cflat_ha_lock_release_();
}

// Leak scan. Walk the whole table under the lock and report every slot still in the LIVE
// state - an allocation through operator new with no matching operator delete. Prints
// "ptr=0x... size=..." per leak to stderr and returns the count.
//
// Honest limits: only allocations made AFTER enable() that flow through operator
// new/delete are tracked. A still-LIVE entry at program exit is a leak; a buffer freed
// via a raw allocator path that bypasses operator delete will look LIVE here (false
// positive) - so this is a debug oracle, not a proof of leak-freedom for arbitrary code.
unsigned long long cflat_heap_audit_report_leaks(void)
{
    if (!cflat_ha_lock_ready) return 0;

    // Initialize symbolization once so leak frames can be resolved. If it fails (e.g. no
    // PDB on Windows), addresses still print; we just skip names.
    cflat_ha_symctx symctx;
    int symReady = cflat_ha_sym_begin_(&symctx);

    unsigned long long liveCount = 0;
    cflat_ha_lock_acquire_();
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
#ifdef _WIN32
        cflat_ha_append_hex_(line, sizeof(line), &pos,
                             (unsigned long long)(ULONG_PTR)slot->key, 0);
#else
        cflat_ha_append_hex_(line, sizeof(line), &pos,
                             (unsigned long long)(uintptr_t)slot->key, 0);
#endif
        cflat_ha_append_(line, sizeof(line), &pos, " size=");
        cflat_ha_append_dec_(line, sizeof(line), &pos, slot->size);
        cflat_ha_append_(line, sizeof(line), &pos, " ***\n");
        cflat_ha_write_stderr_(line);

        if (symReady)
        {
            for (int f = 0; f < slot->frameCount; ++f)
                cflat_ha_print_frame_(symctx, f, slot->frames[f]);
        }
    }
    cflat_ha_lock_release_();

    if (symReady)
        cflat_ha_sym_end_(symctx);
    return liveCount;
}

// ---------------------------------------------------------------------------
// One-time lock init before main. Windows: a CRT initializer (.CRT$XCU), mirroring
// crashdump.c's registration. POSIX: a constructor attribute runs the equivalent
// pre-main hook (single threaded at that point on both platforms).
// ---------------------------------------------------------------------------

#ifdef _WIN32
static int cflat_ha_init_(void)
{
    InitializeCriticalSection(&cflat_ha_lock);
    cflat_ha_lock_ready = 1;
    return 0;
}

#pragma section(".CRT$XCU", read)
__declspec(allocate(".CRT$XCU")) int (*cflat_ha_init_ptr_)(void) = cflat_ha_init_;
#else
__attribute__((constructor))
static void cflat_ha_init_(void)
{
    // PTHREAD_MUTEX_INITIALIZER already gives a valid static mutex on POSIX (including
    // Darwin, where a zeroed-but-not-initialized mutex is invalid) - no explicit init call
    // needed, just flip the ready flag.
    cflat_ha_lock_ready = 1;
}
#endif

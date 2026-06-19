// cflat_builtins.c - freestanding replacements for the symbols VCRUNTIME140.dll would
// otherwise supply, linked into every cflat output exe. VCRUNTIME140.dll is the VC++
// redistributable and is NOT part of the base OS, so depending on it means a cflat user
// needs it installed; providing these locally lets the output exe drop that dependency.
//
// Two groups: (1) the mem*/str*/wcs* intrinsics vcruntime exports and the compiler emits
// (struct copies, llvm.memcpy lowering, string ops); (2) the x64 exception-handling
// support the CRT startup and cflat's `program` SEH trampoline reference (see below). The
// set of (1) mirrors VCRUNTIME140.dll's exports exactly (dumpbin /exports).
//
// MUST be compiled with -fno-builtin: otherwise LLVM's loop-idiom recognizer turns the
// byte loops below back into calls to memcpy/memset, which would recurse into these very
// definitions. See internal/plan/remove-vcruntime-dependency.md.

// No #include: this file is compiled at the user's link time, so staying header-free keeps the
// freestanding link from needing any SDK/CRT headers. size_t is the only library type used;
// clang always predefines __SIZE_TYPE__ (correct width per target), so we get it without <stddef.h>.
typedef __SIZE_TYPE__ size_t;

void* memcpy(void* dst, const void* src, size_t n)
{
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    for (size_t i = 0; i < n; ++i)
        d[i] = s[i];
    return dst;
}

void* memset(void* dst, int value, size_t n)
{
    unsigned char* d = (unsigned char*)dst;
    unsigned char b = (unsigned char)value;
    for (size_t i = 0; i < n; ++i)
        d[i] = b;
    return dst;
}

int memcmp(const void* a, const void* b, size_t n)
{
    const unsigned char* p = (const unsigned char*)a;
    const unsigned char* q = (const unsigned char*)b;
    for (size_t i = 0; i < n; ++i)
    {
        if (p[i] != q[i])
            return (int)p[i] - (int)q[i];
    }
    return 0;
}

void* memmove(void* dst, const void* src, size_t n)
{
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    if (d == s || n == 0) return dst;
    if (d < s)
        for (size_t i = 0; i < n; ++i) d[i] = s[i];
    else
        for (size_t i = n; i != 0; --i) d[i - 1] = s[i - 1];   // overlap: copy backward
    return dst;
}

void* memchr(const void* p, int value, size_t n)
{
    const unsigned char* s = (const unsigned char*)p;
    unsigned char b = (unsigned char)value;
    for (size_t i = 0; i < n; ++i)
        if (s[i] == b) return (void*)(s + i);
    return 0;
}

// The remaining string searches mirror vcruntime140's exports. strchr/strrchr match the
// terminating '\0' when searched for, per the C standard.
char* strchr(const char* s, int c)
{
    char ch = (char)c;
    for (;; ++s) { if (*s == ch) return (char*)s; if (*s == 0) return 0; }
}

char* strrchr(const char* s, int c)
{
    char ch = (char)c;
    const char* last = 0;
    for (;; ++s) { if (*s == ch) last = s; if (*s == 0) return (char*)last; }
}

char* strstr(const char* hay, const char* needle)
{
    if (*needle == 0) return (char*)hay;
    for (; *hay; ++hay)
    {
        const char* h = hay; const char* n = needle;
        while (*n && *h == *n) { ++h; ++n; }
        if (*n == 0) return (char*)hay;
    }
    return 0;
}

// Wide variants. wchar_t is 16-bit on Windows; use unsigned short to stay header-free
// (link resolution is by name, so this is ABI-compatible with wchar_t callers).
unsigned short* wcschr(const unsigned short* s, unsigned short c)
{
    for (;; ++s) { if (*s == c) return (unsigned short*)s; if (*s == 0) return 0; }
}

unsigned short* wcsrchr(const unsigned short* s, unsigned short c)
{
    const unsigned short* last = 0;
    for (;; ++s) { if (*s == c) last = s; if (*s == 0) return (unsigned short*)last; }
}

unsigned short* wcsstr(const unsigned short* hay, const unsigned short* needle)
{
    if (*needle == 0) return (unsigned short*)hay;
    for (; *hay; ++hay)
    {
        const unsigned short* h = hay; const unsigned short* n = needle;
        while (*n && *h == *n) { ++h; ++n; }
        if (*n == 0) return (unsigned short*)hay;
    }
    return 0;
}

// ---- Exception handling (x64 language-specific handler) ----
//
// x64 only: x86 uses the stack-based FS:[0] / _except_handler4 model, not this table-based one,
// so the handler below is never invoked on x86 (x86 `program`-construct SEH is gated to the
// stock VC CRT). Guarding it out of x86 also avoids pulling RtlUnwindEx from ntdll there.
#if defined(_M_X64)
//
// Normally provided by VCRUNTIME140.dll. Two consumers reference __C_specific_handler:
//   1. The MSVC CRT startup (mainCRTStartup -> __scrt_common_main_seh) wraps main() in a
//      top-level __try/__except for crash containment.
//   2. cflat's `program` construct emits real SEH in its thread trampoline (catchpad with
//      __C_specific_handler as personality, MainListener.h) to catch a hardware fault in a
//      program's main() and isolate it (exitCode = -1) without killing the process.
//
// (2) requires a WORKING handler, not a decline-stub, so we implement the documented x64
// model here: walk the C scope table the compiler emitted, run filters on the search pass,
// run termination (__finally) handlers on the unwind pass, and RtlUnwindEx to the __except
// target. Written fresh from the public exception model (not copied from LGPL sources) and
// kept header-free: this file is compiled by clang-cl at the USER's link time, so pulling
// <windows.h> would reintroduce the very Windows-SDK dependency we are removing.

typedef unsigned int   u32_;
typedef unsigned long long u64_;

// Only the leading fields are read; the OS passes a pointer to a full record.
typedef struct { u32_ ExceptionCode; u32_ ExceptionFlags; } EXCEPTION_RECORD_;

// One __try region. All addresses are image-relative (RVAs). JumpTarget == 0 marks a
// __finally/cleanup; otherwise it is a __except whose HandlerAddress is the filter (or the
// sentinel 1, meaning "always EXECUTE_HANDLER").
typedef struct { u32_ BeginAddress, EndAddress, HandlerAddress, JumpTarget; } SCOPE_RECORD_;
typedef struct { u32_ Count; SCOPE_RECORD_ ScopeRecord[1]; } SCOPE_TABLE_;

// Exact x64 layout; we index several fields so field order/size must match the OS struct.
typedef struct
{
    u64_  ControlPc;
    u64_  ImageBase;
    void* FunctionEntry;
    u64_  EstablisherFrame;
    u64_  TargetIp;
    void* ContextRecord;
    void* LanguageHandler;
    void* HandlerData;        // -> SCOPE_TABLE_
    void* HistoryTable;
    u32_  ScopeIndex;
    u32_  Fill0;
} DISPATCHER_CONTEXT_;

typedef struct { EXCEPTION_RECORD_* ExceptionRecord; void* ContextRecord; } EXCEPTION_POINTERS_;

// Exception flags / dispositions / filter results.
#define CFLAT_EXC_UNWINDING        0x02
#define CFLAT_EXC_EXIT_UNWIND      0x04
#define CFLAT_EXC_TARGET_UNWIND    0x20
#define CFLAT_DISPOSITION_CONTINUE_EXECUTION 0
#define CFLAT_DISPOSITION_CONTINUE_SEARCH    1
#define CFLAT_FILTER_EXECUTE_HANDLER 1   // also the constant-filter sentinel in HandlerAddress

// RtlUnwindEx lives in ntdll.dll (always present); it transfers control to TargetIp and
// does not return. ntdll.lib is added to the cflat link line.
extern void RtlUnwindEx(void* TargetFrame, void* TargetIp, EXCEPTION_RECORD_* Rec,
                        void* ReturnValue, void* ContextRecord, void* HistoryTable);

typedef int  (*cflat_filter_fn)(EXCEPTION_POINTERS_*, u64_ frame);
typedef void (*cflat_finally_fn)(unsigned char abnormal, u64_ frame);

int __C_specific_handler(EXCEPTION_RECORD_* rec, void* frame, void* ctx, DISPATCHER_CONTEXT_* disp)
{
    SCOPE_TABLE_* table = (SCOPE_TABLE_*)disp->HandlerData;
    u64_ base = disp->ImageBase;
    u32_ pc   = (u32_)(disp->ControlPc - base);   // image-relative
    u64_ est  = (u64_)frame;

    if (rec->ExceptionFlags & (CFLAT_EXC_UNWINDING | CFLAT_EXC_EXIT_UNWIND))
    {
        // Unwind pass: run termination (__finally) handlers whose region covers the fault,
        // except the one we are unwinding into.
        for (u32_ i = disp->ScopeIndex; i < table->Count; ++i)
        {
            SCOPE_RECORD_* s = &table->ScopeRecord[i];
            if (pc < s->BeginAddress || pc >= s->EndAddress) continue;
            if (s->JumpTarget) continue;   // __except, not a __finally
            if ((rec->ExceptionFlags & CFLAT_EXC_TARGET_UNWIND) &&
                disp->TargetIp == base + s->EndAddress)
                continue;
            ((cflat_finally_fn)(base + s->HandlerAddress))(1, est);
        }
        return CFLAT_DISPOSITION_CONTINUE_SEARCH;
    }

    // Search pass: find a __except whose filter accepts, then unwind to its handler.
    for (u32_ i = disp->ScopeIndex; i < table->Count; ++i)
    {
        SCOPE_RECORD_* s = &table->ScopeRecord[i];
        if (pc < s->BeginAddress || pc >= s->EndAddress) continue;
        if (!s->JumpTarget) continue;   // __finally, only runs while unwinding

        int verdict;
        if (s->HandlerAddress == CFLAT_FILTER_EXECUTE_HANDLER)
        {
            verdict = CFLAT_FILTER_EXECUTE_HANDLER;   // constant __except(1)
        }
        else
        {
            EXCEPTION_POINTERS_ ptrs = { rec, ctx };
            verdict = ((cflat_filter_fn)(base + s->HandlerAddress))(&ptrs, est);
        }

        if (verdict < 0) return CFLAT_DISPOSITION_CONTINUE_EXECUTION;
        if (verdict > 0)
        {
            // Transfer to the __except body (sets exitCode = -1 for the program trampoline).
            RtlUnwindEx(frame, (void*)(base + s->JumpTarget), rec,
                        (void*)(u64_)rec->ExceptionCode, disp->ContextRecord, disp->HistoryTable);
            // RtlUnwindEx does not return.
        }
        // verdict == 0: continue searching outer scopes.
    }
    return CFLAT_DISPOSITION_CONTINUE_SEARCH;
}
#endif // _M_X64

// C++ exception state accessors and type-info teardown, referenced by the CRT startup.
// cflat throws no C++ exceptions, so the slots are always empty and the list is empty.
static __declspec(thread) void* cflat_current_exception_ = 0;
static __declspec(thread) void* cflat_current_exception_context_ = 0;

void** __current_exception(void)         { return &cflat_current_exception_; }
void** __current_exception_context(void) { return &cflat_current_exception_context_; }
void   __std_type_info_destroy_list(void* root) { (void)root; }

// ---- CRT header-inline shims ----
//
// A few <time.h> functions are header inlines over the 64-bit-time_t exports: the UCRT DLL
// exports _time64/_difftime64 but not time/difftime. cflat's C-header binding sees the
// prototypes and emits calls to time()/difftime(), which the static CRT (libcmt) used to
// satisfy. Provide the thin forwarders so the freestanding link does not need it. time_t is a
// signed 64-bit integer on this target.
extern long long _time64(long long* t);
extern double    _difftime64(long long endTime, long long startTime);
long long time(long long* t)               { return _time64(t); }
double    difftime(long long e, long long s) { return _difftime64(e, s); }

// ---- Thread-local storage support ----
//
// __declspec(thread) / thread_local data needs the PE TLS directory (_tls_used) and a
// loader-filled slot index (_tls_index); the compiler emits references to both. The CRT
// (msvcrt.lib / libcmt.lib) normally supplies them - we provide them here so the freestanding
// link does not need it. Layout mirrors the standard CRT tlssup.c. x64 only for now (x86 name
// decoration differs - Phase D).

unsigned long _tls_index = 0;

// The per-thread TLS block is everything between _tls_start (.tls) and _tls_end (.tls$ZZZ).
#pragma section(".tls", long, read, write)
#pragma section(".tls$ZZZ", long, read, write)
__declspec(allocate(".tls"))     char _tls_start = 0;
__declspec(allocate(".tls$ZZZ")) char _tls_end   = 0;

// TLS init callbacks: the loader walks the null-terminated array between these markers. We
// register none, so the array is just the two zero boundary entries.
typedef void (__stdcall* cflat_tls_callback)(void*, unsigned long, void*);
#pragma section(".CRT$XLA", long, read)
#pragma section(".CRT$XLZ", long, read)
__declspec(allocate(".CRT$XLA")) cflat_tls_callback cflat_xl_a = 0;
__declspec(allocate(".CRT$XLZ")) cflat_tls_callback cflat_xl_z = 0;

// IMAGE_TLS_DIRECTORY64 - pointer-typed fields so the initializer is a plain address constant
// (8-byte pointers on x64 match the ULONGLONG layout the loader expects).
typedef struct
{
    void* StartAddressOfRawData;
    void* EndAddressOfRawData;
    void* AddressOfIndex;
    void* AddressOfCallBacks;
    unsigned long SizeOfZeroFill;
    unsigned long Characteristics;
} cflat_image_tls_directory64;

// The PE loader reads _tls_used to set up per-thread TLS. Nothing references it by symbol, so
// /include keeps it from being dropped. The object symbol carries the target's leading-underscore
// decoration: _tls_used on x64, __tls_used on x86.
#if defined(_M_IX86)
#pragma comment(linker, "/include:__tls_used")
#else
#pragma comment(linker, "/include:_tls_used")
#endif
const cflat_image_tls_directory64 _tls_used = {
    &_tls_start, &_tls_end, &_tls_index, &cflat_xl_a, 0, 0
};

// ---- CRT startup (own entry point) ----
//
// Normally the exe entry is mainCRTStartup, supplied by the VC startup lib msvcrt.lib
// (a Visual Studio component, not part of the base OS). It initializes the CRT, runs
// static initializers, sets up argv/env, calls main, then exit(). We provide our own so
// the output exe does not need msvcrt.lib - using ONLY Universal CRT entry points
// (api-ms-win-crt-*, OS-resident on Win10+). Linked via /entry:cflat_start with
// /nodefaultlib:msvcrt.lib. See internal/plan/remove-vcruntime-dependency.md Phase A.

typedef void (__cdecl* cflat_pvfv)(void);   // C++ ctor / void initializer thunk
typedef int  (__cdecl* cflat_pifv)(void);   // C initializer (returns an errno-style code)

// Section boundary markers. lld-link concatenates the .CRT$Xip sections alphabetically
// into one block; the CRT-init thunks placed by other translation units (e.g.
// crashdump.c's .CRT$XCU) land between the $A and $Z markers of their group. We must
// define those boundaries since we no longer pull the VC startup object that does.
#pragma section(".CRT$XIA", read)
#pragma section(".CRT$XIZ", read)
#pragma section(".CRT$XCA", read)
#pragma section(".CRT$XCZ", read)
__declspec(allocate(".CRT$XIA")) cflat_pifv cflat_xi_a[1] = { 0 };
__declspec(allocate(".CRT$XIZ")) cflat_pifv cflat_xi_z[1] = { 0 };
__declspec(allocate(".CRT$XCA")) cflat_pvfv cflat_xc_a[1] = { 0 };
__declspec(allocate(".CRT$XCZ")) cflat_pvfv cflat_xc_z[1] = { 0 };

// Universal CRT startup helpers (resolve through ucrt.lib to OS-resident api-ms-win-crt-*).
extern int     __cdecl _initterm_e(cflat_pifv* first, cflat_pifv* last);
extern void    __cdecl _initterm(cflat_pvfv* first, cflat_pvfv* last);
extern int     __cdecl _configure_narrow_argv(int mode);   // 2 == expanded arguments
extern int     __cdecl _initialize_narrow_environment(void);
extern int*    __cdecl __p___argc(void);
extern char*** __cdecl __p___argv(void);
extern void    __cdecl exit(int code);

// cflat's entry. main() may be declared with or without (argc, argv); calling with two
// args is safe under cdecl (the caller cleans the stack and a 0-arg main ignores them).
extern int main(int argc, char** argv);

void cflat_start(void)
{
    // argv/env first so any C initializer that reads them sees populated state.
    _configure_narrow_argv(2);
    _initialize_narrow_environment();
    if (_initterm_e(cflat_xi_a, cflat_xi_z) != 0)   // C initializers; nonzero == failure
        exit(255);
    _initterm(cflat_xc_a, cflat_xc_z);              // C++ ctors -> runs crashdump's .CRT$XCU init
    int    argc = *__p___argc();
    char** argv = *__p___argv();
    exit(main(argc, argv));                         // exit() runs atexit + flushes stdio
}

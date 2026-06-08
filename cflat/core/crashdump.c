// crashdump.c - in-process symbolized crash backtrace for cflat programs.
//
// Linked into a generated executable ONLY when the user builds with -g (debug
// info). With -g the linker produces a PDB next to the exe (see /DEBUG + /PDB in
// LLVMBackend.h::EmitExecutable). At runtime DbgHelp reads that PDB and resolves
// CFlat function names and source line numbers from the CodeView records.
//
// On an unhandled exception (access violation, etc.) the last-chance filter prints
// the exception kind plus a numbered, symbolized backtrace to stderr, then ends the
// process quietly (no Windows Error Reporting popup). Without this handler such a
// crash dies silently with exit code 139 and no diagnostic.
//
// The filter is installed by a CRT dynamic initializer (.CRT$XCU) so it runs before
// main with no changes to the compiler's IR emission. ASCII only in all output.
//
// Output goes through Win32 WriteFile (GetStdHandle(STD_ERROR_HANDLE)) rather than
// the CRT's stdio: cflat's runtime (cruntime.cb) defines its own strong sprintf /
// vsprintf, so linking any CRT stdio printf-family here would collide at link time.
// A tiny self-contained formatter keeps this object free of stdio dependencies.

#include <windows.h>
#include <dbghelp.h>
#include <string.h>

// Append a NUL-terminated string to buf at *pos, never overflowing cap.
static void cflat_append_(char* buf, int cap, int* pos, const char* s)
{
    while (*s && *pos < cap - 1)
        buf[(*pos)++] = *s++;
    buf[*pos] = '\0';
}

// Append an unsigned value in hex, zero-padded to 'width' digits (0 = no padding).
static void cflat_append_hex_(char* buf, int cap, int* pos, unsigned long long v, int width)
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

// Append an unsigned decimal value.
static void cflat_append_dec_(char* buf, int cap, int* pos, unsigned long long v)
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

static void cflat_write_stderr_(const char* s)
{
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h == INVALID_HANDLE_VALUE || h == NULL)
        return;
    DWORD len = (DWORD)strlen(s);
    DWORD written = 0;
    WriteFile(h, s, len, &written, NULL);
}

// Resolve a single instruction address to "name (file:line)" and print one frame.
static void cflat_print_frame_(HANDLE process, int index, DWORD64 addr)
{
    // SYMBOL_INFO is variable-length: reserve room for the trailing name buffer.
    char symbolBuffer[sizeof(SYMBOL_INFO) + 512];
    SYMBOL_INFO* symbol = (SYMBOL_INFO*)symbolBuffer;
    memset(symbolBuffer, 0, sizeof(symbolBuffer));
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 511;

    DWORD64 symDisp = 0;
    const char* name = "<unknown>";
    if (SymFromAddr(process, addr, &symDisp, symbol))
        name = symbol->Name;

    char line[1024];
    int pos = 0;
    cflat_append_(line, sizeof(line), &pos, "  #");
    cflat_append_dec_(line, sizeof(line), &pos, (unsigned long long)index);
    cflat_append_(line, sizeof(line), &pos, " 0x");
    cflat_append_hex_(line, sizeof(line), &pos, (unsigned long long)addr, 16);
    cflat_append_(line, sizeof(line), &pos, " ");
    cflat_append_(line, sizeof(line), &pos, name);

    IMAGEHLP_LINE64 lineInfo;
    memset(&lineInfo, 0, sizeof(lineInfo));
    lineInfo.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
    DWORD lineDisp = 0;
    if (SymGetLineFromAddr64(process, addr, &lineDisp, &lineInfo))
    {
        cflat_append_(line, sizeof(line), &pos, " (");
        cflat_append_(line, sizeof(line), &pos, lineInfo.FileName);
        cflat_append_(line, sizeof(line), &pos, ":");
        cflat_append_dec_(line, sizeof(line), &pos, (unsigned long long)lineInfo.LineNumber);
        cflat_append_(line, sizeof(line), &pos, ")");
    }
    cflat_append_(line, sizeof(line), &pos, "\n");
    cflat_write_stderr_(line);
}

static LONG WINAPI cflat_crash_filter_(EXCEPTION_POINTERS* info)
{
    DWORD code = info->ExceptionRecord->ExceptionCode;

    const char* name = "UNKNOWN_EXCEPTION";
    switch (code)
    {
        case EXCEPTION_ACCESS_VIOLATION:      name = "ACCESS_VIOLATION"; break;
        case EXCEPTION_STACK_OVERFLOW:        name = "STACK_OVERFLOW"; break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    name = "INT_DIVIDE_BY_ZERO"; break;
        case EXCEPTION_INT_OVERFLOW:          name = "INT_OVERFLOW"; break;
        case EXCEPTION_ILLEGAL_INSTRUCTION:   name = "ILLEGAL_INSTRUCTION"; break;
        case EXCEPTION_PRIV_INSTRUCTION:      name = "PRIV_INSTRUCTION"; break;
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:    name = "FLT_DIVIDE_BY_ZERO"; break;
        case EXCEPTION_DATATYPE_MISALIGNMENT: name = "DATATYPE_MISALIGNMENT"; break;
        case EXCEPTION_IN_PAGE_ERROR:         name = "IN_PAGE_ERROR"; break;
        default:                              name = "UNKNOWN_EXCEPTION"; break;
    }

    {
        char hdr[256];
        int pos = 0;
        cflat_append_(hdr, sizeof(hdr), &pos, "\n*** cflat: unhandled exception ");
        cflat_append_(hdr, sizeof(hdr), &pos, name);
        cflat_append_(hdr, sizeof(hdr), &pos, " (0x");
        cflat_append_hex_(hdr, sizeof(hdr), &pos, (unsigned long long)code, 8);
        cflat_append_(hdr, sizeof(hdr), &pos, ") on thread ");
        cflat_append_dec_(hdr, sizeof(hdr), &pos, (unsigned long long)GetCurrentThreadId());
        cflat_append_(hdr, sizeof(hdr), &pos, " ***\n");
        cflat_write_stderr_(hdr);
    }

    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR)
    {
        // ExceptionInformation[0]: 0 = read, 1 = write, 8 = execute (DEP).
        // ExceptionInformation[1]: the faulting virtual address.
        ULONG_PTR kind = info->ExceptionRecord->NumberParameters >= 1
            ? info->ExceptionRecord->ExceptionInformation[0] : 0;
        ULONG_PTR faultAddr = info->ExceptionRecord->NumberParameters >= 2
            ? info->ExceptionRecord->ExceptionInformation[1] : 0;
        const char* kindStr = (kind == 8) ? "execute" : (kind == 1) ? "write" : "read";

        char detail[128];
        int pos = 0;
        cflat_append_(detail, sizeof(detail), &pos, "    ");
        cflat_append_(detail, sizeof(detail), &pos, kindStr);
        cflat_append_(detail, sizeof(detail), &pos, " at address 0x");
        cflat_append_hex_(detail, sizeof(detail), &pos, (unsigned long long)faultAddr, 16);
        cflat_append_(detail, sizeof(detail), &pos, "\n");
        cflat_write_stderr_(detail);
    }

    HANDLE process = GetCurrentProcess();
    HANDLE thread  = GetCurrentThread();

    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    if (!SymInitialize(process, NULL, TRUE))
    {
        cflat_write_stderr_("  (backtrace unavailable: SymInitialize failed)\n");
        return EXCEPTION_EXECUTE_HANDLER;
    }

    CONTEXT* ctx = info->ContextRecord;
    STACKFRAME64 frame;
    memset(&frame, 0, sizeof(frame));
    DWORD machine;

#if defined(_M_X64)
    machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset    = ctx->Rip;
    frame.AddrPC.Mode      = AddrModeFlat;
    frame.AddrFrame.Offset = ctx->Rbp;
    frame.AddrFrame.Mode   = AddrModeFlat;
    frame.AddrStack.Offset = ctx->Rsp;
    frame.AddrStack.Mode   = AddrModeFlat;
#elif defined(_M_IX86)
    machine = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset    = ctx->Eip;
    frame.AddrPC.Mode      = AddrModeFlat;
    frame.AddrFrame.Offset = ctx->Ebp;
    frame.AddrFrame.Mode   = AddrModeFlat;
    frame.AddrStack.Offset = ctx->Esp;
    frame.AddrStack.Mode   = AddrModeFlat;
#else
#error "crashdump.c supports only x64 and x86 targets"
#endif

    cflat_write_stderr_("  backtrace (most recent call first):\n");

    const int maxFrames = 64;
    for (int i = 0; i < maxFrames; ++i)
    {
        if (!StackWalk64(machine, process, thread, &frame, ctx, NULL,
                         SymFunctionTableAccess64, SymGetModuleBase64, NULL))
            break;
        if (frame.AddrPC.Offset == 0)
            break;
        cflat_print_frame_(process, i, frame.AddrPC.Offset);
    }

    SymCleanup(process);
    cflat_write_stderr_("*** end of backtrace ***\n");

    // Terminate the process without a WER popup.
    return EXCEPTION_EXECUTE_HANDLER;
}

// Installed as a CRT initializer; runs once before main on the main thread.
static int cflat_install_crash_handler_(void)
{
    SetUnhandledExceptionFilter(cflat_crash_filter_);
    return 0;
}

// The .CRT$XCU pointer has no external references (the CRT gathers it by section
// name), so lld-link's /OPT:REF would garbage-collect it. It is kept external (not
// static) so EmitExecutable can force-retain it with /INCLUDE: at link time.
#pragma section(".CRT$XCU", read)
__declspec(allocate(".CRT$XCU")) int (*cflat_crash_init_)(void) =
    cflat_install_crash_handler_;

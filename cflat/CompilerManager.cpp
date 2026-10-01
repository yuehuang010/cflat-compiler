// Crash reporting for the compiler process: every fatal path (signal / unhandled SEH exception,
// abort(), CRT assert, LLVM fatal error) funnels into ONE bug-report block on stderr - version,
// host, command line, cause, top CompilerManager::kCrashReportFrames frames - followed by the
// compiler state dump. The first report wins; a follow-on abort() from the same failure only
// re-raises. Setting CFLAT_DEBUG_CRASH=segv|abort|fatal after install triggers a report on
// purpose (see main.cpp) so the output can be checked by hand.

#include "LLVMBackend.h" // CompilerManager::DumpAllState definition
#include "CompilerManager.h"
#include "Version.h"

#include <llvm/Config/llvm-config.h>
#include <llvm/Demangle/Demangle.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/Signals.h>
#include <llvm/TargetParser/Host.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#else
#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/ucontext.h>
#endif
#endif

namespace cflat_crash
{
std::string g_command;
std::string g_host;
std::atomic<bool> g_reported{false};

struct Frame
{
    uintptr_t addr = 0;
    std::string module;   // file name only
    uintptr_t moduleOffset = 0;
    std::string symbol;   // demangled; empty when unknown
    uintptr_t symbolOffset = 0;
    std::string location; // "file:line" when line info is available
};

// First reporter wins; any later fatal path (e.g. the abort() a CRT assert ends in) stays quiet.
bool BeginReport()
{
    return !g_reported.exchange(true);
}

std::string BaseName(const char* path)
{
    if (!path) return {};
    const char* slash = std::strrchr(path, '/');
#if defined(_WIN32)
    const char* bslash = std::strrchr(path, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
    return slash ? slash + 1 : path;
}

// Frames that belong to the crash machinery itself (this file, signal trampolines, abort /
// assert / raise, LLVM's fatal-error entry). Leading ones are dropped so frame #0 is the code
// that failed, not the reporter.
bool IsPlumbing(const std::string& s)
{
    static const char* const kPrefixes[] = {
        "cflat_crash::", "llvm::report_fatal_error", "llvm::sys::RunSignalHandlers",
        "common_assert", "_VCrtDbgReport", "_CrtDbgReport", "__GI_",
    };
    static const char* const kExact[] = {
        "abort", "__abort", "raise", "_wassert", "_assert", "__assert_rtn", "__assert_fail",
        "__pthread_kill", "pthread_kill", "_sigtramp", "__restore_rt", "KiUserExceptionDispatcher",
        "RtlDispatchException", "RtlRaiseException", "UnhandledExceptionFilter",
    };
    for (const char* p : kPrefixes)
        if (s.compare(0, std::strlen(p), p) == 0) return true;
    for (const char* e : kExact)
        if (s == e) return true;
    return false;
}

#if defined(_WIN32)
Frame Symbolize(uintptr_t addr, bool isReturnAddress)
{
    Frame f;
    f.addr = addr;
    HANDLE process = GetCurrentProcess();
    // A return address points past the call; look up the call instruction itself.
    const DWORD64 lookup = isReturnAddress ? addr - 1 : addr;
    const DWORD64 base = SymGetModuleBase64(process, lookup);
    if (base)
    {
        char path[MAX_PATH] = {};
        if (GetModuleFileNameA(reinterpret_cast<HMODULE>(base), path, MAX_PATH))
            f.module = BaseName(path);
        f.moduleOffset = static_cast<uintptr_t>(addr - base);
    }
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buffer);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = MAX_SYM_NAME;
    // Without a PDB dbghelp falls back to the export table and names the nearest exported
    // (clang interpreter) symbol - misleading, so print module+offset only.
    // Checked after SymFromAddr: deferred loading only settles SymType once a lookup ran.
    DWORD64 displacement = 0;
    IMAGEHLP_MODULE64 mi = {};
    mi.SizeOfStruct = sizeof(mi);
    const bool ownModule = base == reinterpret_cast<DWORD64>(GetModuleHandleA(nullptr));
    if (SymFromAddr(process, lookup, &displacement, sym) && SymGetModuleInfo64(process, lookup, &mi) &&
        mi.SymType != SymNone && mi.SymType != SymDeferred && !(ownModule && mi.SymType == SymExport))
    {
        f.symbol = sym->Name;
        f.symbolOffset = static_cast<uintptr_t>(addr - sym->Address);
    }
    IMAGEHLP_LINE64 line = {};
    line.SizeOfStruct = sizeof(line);
    DWORD lineDisplacement = 0;
    if (SymGetLineFromAddr64(process, lookup, &lineDisplacement, &line))
        f.location = BaseName(line.FileName) + ":" + std::to_string(line.LineNumber);
    return f;
}

void InitSymbols()
{
    static bool done = false;
    if (done) return;
    done = true;
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE); // default search path: exe dir -> PDB
}

// Walk from an exception context: frame #0 is the faulting instruction.
std::vector<uintptr_t> WalkContext(CONTEXT ctx, HANDLE thread)
{
    std::vector<uintptr_t> pcs;
    STACKFRAME64 sf = {};
    DWORD machine = 0;
#if defined(_M_X64)
    machine = IMAGE_FILE_MACHINE_AMD64;
    sf.AddrPC.Offset = ctx.Rip;
    sf.AddrStack.Offset = ctx.Rsp;
    sf.AddrFrame.Offset = ctx.Rbp;
#elif defined(_M_ARM64)
    machine = IMAGE_FILE_MACHINE_ARM64;
    sf.AddrPC.Offset = ctx.Pc;
    sf.AddrStack.Offset = ctx.Sp;
    sf.AddrFrame.Offset = ctx.Fp;
#endif
    sf.AddrPC.Mode = sf.AddrStack.Mode = sf.AddrFrame.Mode = AddrModeFlat;
    while (pcs.size() < 64 && StackWalk64(machine, GetCurrentProcess(), thread, &sf, &ctx, nullptr,
                                          SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
    {
        if (sf.AddrPC.Offset == 0) break;
        pcs.push_back(static_cast<uintptr_t>(sf.AddrPC.Offset));
    }
    return pcs;
}

std::vector<uintptr_t> CaptureHere()
{
    void* raw[64];
    const USHORT n = CaptureStackBackTrace(0, 64, raw, nullptr);
    return std::vector<uintptr_t>(reinterpret_cast<uintptr_t*>(raw), reinterpret_cast<uintptr_t*>(raw) + n);
}
#else
Frame Symbolize(uintptr_t addr, bool isReturnAddress)
{
    Frame f;
    f.addr = addr;
    Dl_info info = {};
    if (dladdr(reinterpret_cast<void*>(isReturnAddress ? addr - 1 : addr), &info))
    {
        f.module = BaseName(info.dli_fname);
        f.moduleOffset = addr - reinterpret_cast<uintptr_t>(info.dli_fbase);
        if (info.dli_sname)
        {
            // Qualified name without the parameter list, like dbghelp's names on Windows; the
            // module offset still pins the exact overload.
            llvm::ItaniumPartialDemangler demangler;
            char* name = nullptr;
            if (!demangler.partialDemangle(info.dli_sname) && demangler.isFunction())
                name = demangler.getFunctionName(nullptr, nullptr);
            f.symbol = name ? name : llvm::demangle(info.dli_sname);
            std::free(name);
            f.symbolOffset = addr - reinterpret_cast<uintptr_t>(info.dli_saddr);
        }
    }
    return f;
}

void InitSymbols() {}

std::vector<uintptr_t> CaptureHere()
{
    void* raw[64];
    const int n = backtrace(raw, 64);
    return std::vector<uintptr_t>(reinterpret_cast<uintptr_t*>(raw), reinterpret_cast<uintptr_t*>(raw) + (n > 0 ? n : 0));
}

#if defined(__APPLE__)
// Walk the frame-pointer chain from the interrupted context (Apple ABIs always keep frame
// pointers): frame #0 is the faulting pc, and the walk works from the alternate signal stack,
// where backtrace() stops at the stack-bounds check.
std::vector<uintptr_t> WalkContext(void* uctx)
{
    std::vector<uintptr_t> pcs;
    auto* uc = static_cast<ucontext_t*>(uctx);
#if defined(__arm64__)
    uintptr_t pc = __darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
    uintptr_t fp = __darwin_arm_thread_state64_get_fp(uc->uc_mcontext->__ss);
#else
    uintptr_t pc = uc->uc_mcontext->__ss.__rip;
    uintptr_t fp = uc->uc_mcontext->__ss.__rbp;
#endif
    pcs.push_back(pc);
    // Sanity bounds: frames grow upward, stay aligned, and stay within 64 MB of the first one.
    const uintptr_t lo = fp, hi = fp + (64u << 20);
    while (pcs.size() < 64 && fp && (fp & 0x7) == 0 && fp >= lo && fp < hi)
    {
        const uintptr_t next = reinterpret_cast<uintptr_t*>(fp)[0];
        uintptr_t ret = reinterpret_cast<uintptr_t*>(fp)[1];
#if defined(__arm64__)
        ret &= 0x0000000FFFFFFFFFull; // strip pointer-authentication bits
#endif
        if (!ret) break;
        pcs.push_back(ret);
        if (next <= fp) break;
        fp = next;
    }
    return pcs;
}
#endif
#endif

// Symbolize, drop leading crash-machinery frames, keep the top kCrashReportFrames.
std::vector<Frame> Resolve(const std::vector<uintptr_t>& pcs, bool firstIsFaultPc, size_t& totalOut)
{
    InitSymbols();
    std::vector<Frame> all;
    all.reserve(pcs.size());
    for (size_t i = 0; i < pcs.size(); ++i)
        all.push_back(Symbolize(pcs[i], !(firstIsFaultPc && i == 0)));
    size_t start = 0;
    for (size_t i = 0; i < all.size() && i < 24; ++i)
        if (IsPlumbing(all[i].symbol)) start = i + 1;
    if (start >= all.size()) start = 0;
    totalOut = all.size() - start;
    std::vector<Frame> top(all.begin() + start,
                           all.begin() + std::min(all.size(), start + CompilerManager::kCrashReportFrames));
    return top;
}

void WriteReport(const std::string& cause, const std::vector<Frame>& frames, size_t total)
{
    std::fflush(stdout);
#if defined(NDEBUG)
    const char* config = "Release";
#else
    const char* config = "Debug";
#endif
    const char* rule = "======================================================================\n";
    std::string r = "\n";
    r += rule;
    r += "cflat internal compiler error - please report this as a bug:\n";
    r += "  https://github.com/yuehuang010/cflat-compiler/issues\n";
    r += "Attach this block and, if you can, the source that triggered it.\n";
    r += "----------------------------------------------------------------------\n";
    r += std::format("Version: cflat {} ({}, LLVM {})\n", CFLAT_VERSION_STRING, config, LLVM_VERSION_STRING);
    r += std::format("Host:    {}\n", g_host);
    r += std::format("Command: {}\n", g_command);
    r += std::format("Cause:   {}\n", cause);
    r += std::format("Stack:   top {} of {} frames\n", frames.size(), total);
    for (size_t i = 0; i < frames.size(); ++i)
    {
        const Frame& f = frames[i];
        std::string where = f.module.empty() ? std::string("???")
                                             : std::format("{}+0x{:x}", f.module, f.moduleOffset);
        std::string what = f.symbol.empty() ? std::string()
                                            : std::format("  {} + 0x{:x}", f.symbol, f.symbolOffset);
        std::string loc = f.location.empty() ? std::string() : std::format("  [{}]", f.location);
        r += std::format("  #{:<2} {}{}{}\n", i, where, what, loc);
    }
    r += rule;
    std::fputs(r.c_str(), stderr);
    std::fflush(stderr);

    // A fault inside the dump re-enters the handler (SA_NODEFER) and exits there.
    std::cout << "\n=== compiler state dump ===\n";
    CompilerManager::Instance().DumpAllState();
    std::cout << "===========================\n" << std::flush;
}

void ReportHere(const std::string& cause)
{
    size_t total = 0;
    auto frames = Resolve(CaptureHere(), false, total);
    WriteReport(cause, frames, total);
}

// Fallback if LLVM's own handler ever gets a fault first (ours normally sits on top of it).
void LLVMSignalCallback(void*)
{
    if (BeginReport()) ReportHere("fatal signal (reported through LLVM's handler)");
}

void LLVMFatalHandler(void*, const char* reason, bool)
{
    if (BeginReport()) ReportHere(std::format("LLVM fatal error: {}", reason ? reason : "(no reason)"));
}

#if defined(_WIN32)
const char* ExceptionName(DWORD code)
{
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_BREAKPOINT: return "breakpoint";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case 0xE06D7363: return "unhandled C++ exception";
    default: return "unhandled exception";
    }
}

struct SehJob
{
    EXCEPTION_POINTERS* ep;
    HANDLE thread;
};

// Runs on a helper thread: after a stack overflow the faulting thread has no room for dbghelp.
DWORD WINAPI SehReportThread(void* param)
{
    auto* job = static_cast<SehJob*>(param);
    const EXCEPTION_RECORD* rec = job->ep->ExceptionRecord;
    std::string cause = std::format("exception 0x{:08X} ({})", static_cast<unsigned>(rec->ExceptionCode),
                                    ExceptionName(rec->ExceptionCode));
    if ((rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || rec->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        rec->NumberParameters >= 2)
    {
        const ULONG_PTR op = rec->ExceptionInformation[0];
        cause += std::format(" {} 0x{:x}", op == 0 ? "reading" : op == 8 ? "executing" : "writing",
                             static_cast<uintptr_t>(rec->ExceptionInformation[1]));
    }
    InitSymbols();
    size_t total = 0;
    auto frames = Resolve(WalkContext(*job->ep->ContextRecord, job->thread), true, total);
    WriteReport(cause, frames, total);
    return 0;
}

LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* ep)
{
    if (!BeginReport()) return EXCEPTION_CONTINUE_SEARCH;
    SehJob job = {ep, nullptr};
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &job.thread, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    if (HANDLE worker = CreateThread(nullptr, 0, &SehReportThread, &job, 0, nullptr))
    {
        WaitForSingleObject(worker, INFINITE);
        CloseHandle(worker);
    }
    if (job.thread) CloseHandle(job.thread);
    return EXCEPTION_EXECUTE_HANDLER; // exit code = exception code, no WER dialog
}

int __cdecl AssertHook(int reportType, char* message, int*)
{
    if (reportType == _CRT_ASSERT && BeginReport())
    {
        std::string text = message ? message : "";
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        ReportHere("CRT assertion: " + text);
    }
    return 0; // let the CRT print its own message and abort
}

void OnAbort(int)
{
    if (BeginReport()) ReportHere("abort() called");
    signal(SIGABRT, SIG_DFL);
    raise(SIGABRT);
}
#else
const char* SignalName(int sig)
{
    switch (sig)
    {
    case SIGSEGV: return "SIGSEGV (segmentation fault)";
    case SIGBUS: return "SIGBUS (bus error)";
    case SIGILL: return "SIGILL (illegal instruction)";
    case SIGFPE: return "SIGFPE (arithmetic exception)";
    case SIGABRT: return "SIGABRT (abort)";
    case SIGTRAP: return "SIGTRAP (trap)";
    default: return "fatal signal";
    }
}

void OnSignal(int sig, siginfo_t* info, void* uctx)
{
    if (BeginReport())
    {
        std::string cause = SignalName(sig);
        if ((sig == SIGSEGV || sig == SIGBUS) && info)
            cause += std::format(" at address 0x{:x}", reinterpret_cast<uintptr_t>(info->si_addr));
        size_t total = 0;
#if defined(__APPLE__)
        auto frames = Resolve(WalkContext(uctx), true, total);
#else
        (void)uctx;
        auto frames = Resolve(CaptureHere(), false, total);
#endif
        WriteReport(cause, frames, total);
    }
    // Reporting is not async-signal-safe; do not rely on restoring or re-raising the signal.
    // _exit also terminates immediately if a second fatal signal re-enters this handler.
    _exit(128 + sig);
}
#endif
} // namespace cflat_crash

void CompilerManager::InstallCrashHandlers(int argc, char** argv)
{
    using namespace cflat_crash;
    for (int i = 0; i < argc; ++i)
    {
        if (i) g_command += ' ';
        const std::string arg = argv[i] ? argv[i] : "";
        const bool quote = arg.empty() || arg.find_first_of(" \t\"") != std::string::npos;
        g_command += quote ? "\"" + arg + "\"" : arg;
    }
    g_host = llvm::sys::getProcessTriple();

    // Let LLVM register its handlers FIRST: its registration is once-per-process, so installing
    // ours on top afterwards means a later RemoveFileOnSignal (clang output files) cannot
    // displace them. The callback only reports if a fault somehow reaches LLVM's handler.
    llvm::sys::AddSignalHandler(&LLVMSignalCallback, nullptr);
    llvm::install_fatal_error_handler(&LLVMFatalHandler, nullptr);

#if defined(_WIN32)
    // _CrtSetReportHook2 only intercepts asserts from this module's CRT instance.
    _CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, &AssertHook);
    signal(SIGABRT, &OnAbort);
    // abort() must not also raise a WER fault report after the handler above.
    _set_abort_behavior(0, _CALL_REPORTFAULT);
    // Keep room on the main thread to enter the filter after a stack overflow.
    ULONG guarantee = 64 * 1024;
    SetThreadStackGuarantee(&guarantee);
    SetUnhandledExceptionFilter(&OnUnhandledException);
#else
    // Alternate stack so a stack overflow can still be reported (main thread).
    static char altStack[256 * 1024];
    stack_t ss = {};
    ss.ss_sp = altStack;
    ss.ss_size = sizeof(altStack);
    sigaltstack(&ss, nullptr);
    struct sigaction sa = {};
    sa.sa_sigaction = &OnSignal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP})
        sigaction(sig, &sa, nullptr);
#endif
}

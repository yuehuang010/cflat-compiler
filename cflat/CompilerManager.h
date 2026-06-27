#pragma once

#include <vector>
#include <mutex>
#include <algorithm>
#include <format>
#include <iostream>
#include <csignal>
#if defined(_WIN32)
#include <crtdbg.h>
#endif
#include <llvm/Support/Error.h>

class LLVMBackend;

// DumpAllState is defined after LLVMBackend is fully declared (see bottom of LLVMBackend.h).
// This header only holds the class layout and the hook installer.

class CompilerManager
{
public:
    static CompilerManager& Instance()
    {
        static CompilerManager instance;
        return instance;
    }

    void Register(LLVMBackend* compiler)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        compilers_.push_back(compiler);
    }

    void Unregister(LLVMBackend* compiler)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = std::find(compilers_.begin(), compilers_.end(), compiler);
        if (it != compilers_.end())
            compilers_.erase(it);
    }

    void InstallAssertHook()
    {
#if defined(_WIN32)
        // _CrtSetReportHook2 only intercepts asserts from this module's CRT instance.
        // LLVM is a DLL with its own CRT, so its asserts won't reach this hook.
        // On POSIX there is no CRT assert hook; assert() routes through abort() ->
        // SIGABRT, which the handler below already catches.
        _CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, &CompilerManager::AssertHook);
#endif

        // SIGABRT is process-wide - catches abort() from any module including LLVM DLLs.
        signal(SIGABRT, &CompilerManager::AbortHandler);

        // LLVM fatal errors (llvm_unreachable etc.) that go through LLVM's own handler.
        llvm::install_fatal_error_handler(&CompilerManager::LLVMFatalHandler, nullptr);
    }

    void DumpAllState() const; // defined after LLVMBackend is fully declared

    CompilerManager(const CompilerManager&) = delete;
    CompilerManager& operator=(const CompilerManager&) = delete;

private:
    CompilerManager() = default;

    std::vector<LLVMBackend*> compilers_;
    mutable std::mutex mutex_;

    static void AbortHandler(int)
    {
        std::cout << "\n=== abort() called - compiler state dump ===\n";
        Instance().DumpAllState();
        std::cout << "============================================\n\n";
        // Restore default and re-raise so the process exits with the correct signal.
        signal(SIGABRT, SIG_DFL);
        raise(SIGABRT);
    }

    static void LLVMFatalHandler(void*, const char* reason, bool)
    {
        std::cout << std::format("\n=== LLVM fatal error: {} ===\n", reason);
        Instance().DumpAllState();
        std::cout << "===============================================\n\n";
    }

#if defined(_WIN32)
    static int __cdecl AssertHook(int reportType, char* message, int* returnValue)
    {
        if (reportType == _CRT_ASSERT)
        {
            std::cout << "\n=== LLVM Assert fired - compiler state dump ===\n";
            Instance().DumpAllState();
            std::cout << "===============================================\n\n";
        }
        return 0; // let CRT proceed to abort
    }
#endif
};

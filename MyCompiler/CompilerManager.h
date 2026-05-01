#pragma once

#include <vector>
#include <iostream>
#include <csignal>
#include <crtdbg.h>
#include <llvm/Support/Error.h>

class LLVMBackend;

// DumpAllState is defined after MyCompilerLLVM is fully declared (see bottom of MyCompilerLLVM.h).
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
        compilers_.push_back(compiler);
    }

    void Unregister(LLVMBackend* compiler)
    {
        auto it = std::find(compilers_.begin(), compilers_.end(), compiler);
        if (it != compilers_.end())
            compilers_.erase(it);
    }

    void InstallAssertHook()
    {
        // _CrtSetReportHook2 only intercepts asserts from this module's CRT instance.
        // LLVM is a DLL with its own CRT, so its asserts won't reach this hook.
        _CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, &CompilerManager::AssertHook);

        // SIGABRT is process-wide — catches abort() from any module including LLVM DLLs.
        signal(SIGABRT, &CompilerManager::AbortHandler);

        // LLVM fatal errors (llvm_unreachable etc.) that go through LLVM's own handler.
        llvm::install_fatal_error_handler(&CompilerManager::LLVMFatalHandler, nullptr);
    }

    void DumpAllState() const; // defined after MyCompilerLLVM is fully declared

    CompilerManager(const CompilerManager&) = delete;
    CompilerManager& operator=(const CompilerManager&) = delete;

private:
    CompilerManager() = default;

    std::vector<LLVMBackend*> compilers_;

    static void AbortHandler(int)
    {
        std::cerr << "\n=== abort() called - compiler state dump ===\n";
        Instance().DumpAllState();
        std::cerr << "============================================\n\n";
        // Restore default and re-raise so the process exits with the correct signal.
        signal(SIGABRT, SIG_DFL);
        raise(SIGABRT);
    }

    static void LLVMFatalHandler(void*, const char* reason, bool)
    {
        std::cerr << "\n=== LLVM fatal error: " << reason << " ===\n";
        Instance().DumpAllState();
        std::cerr << "===============================================\n\n";
    }

    static int __cdecl AssertHook(int reportType, char* message, int* returnValue)
    {
        if (reportType == _CRT_ASSERT)
        {
            std::cerr << "\n=== LLVM Assert fired - compiler state dump ===\n";
            Instance().DumpAllState();
            std::cerr << "===============================================\n\n";
        }
        return 0; // let CRT proceed to abort
    }
};

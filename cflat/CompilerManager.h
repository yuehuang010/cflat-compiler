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

    // Installs the crash reporters (CompilerManager.cpp): fatal signals / unhandled SEH
    // exceptions, abort(), CRT asserts and LLVM fatal errors. Each prints ONE bug-report
    // block to stderr - version, host, command line, cause, top kCrashReportFrames frames -
    // then the compiler state dump. argv is copied for the report's command line.
    void InstallCrashHandlers(int argc, char** argv);
    static constexpr int kCrashReportFrames = 10;

    void DumpAllState() const; // defined after LLVMBackend is fully declared

    CompilerManager(const CompilerManager&) = delete;
    CompilerManager& operator=(const CompilerManager&) = delete;

private:
    CompilerManager() = default;

    std::vector<LLVMBackend*> compilers_;
    mutable std::mutex mutex_;
};

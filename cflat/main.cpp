// main.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>
#include <filesystem>
#include <stdlib.h>

#pragma warning(push)
#pragma warning(disable: 4244 4267)
#include <llvm/Support/TimeProfiler.h>
#pragma warning(pop)

#include "LLVMBackend.h"
#include "CompilerManager.h"
#include "ArgParser.h"
#include "Version.h"
#include "LspServer.h"

int main(int argc, char* argv[])
{
    if (argc >= 2 && std::string_view(argv[1]) == "lsp")
        return RunLspServer(argc - 2, argv + 2);

    CompilerManager::Instance().InstallAssertHook();

    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);

    ArgParser args;
    args.addPositional("filename", "Source file to compile");
    args.addOption("output", 'o', "Output native executable path (.exe)");
    args.addOption("out-lli", 'l', "Output LLVM IR file path (.ll)");
    args.addOption("bitcode", 'b', "Output bitcode file path (.bc)");
    args.addFlag("debug-info", 'g', "Emit DWARF debug information");
    args.addOption("import-dir", 'i', "Directory to search for imported modules");
    args.addOption("platform", 'p', "Target platform: win64 (default) or win32", "win64");
    args.addFlag("verbose", 'v', "Print detailed diagnostic messages during compilation");
    args.addFlag("O0", '0', "No optimization (default)");
    args.addFlag("O1", '1', "Optimize for speed (level 1)");
    args.addFlag("O2", '2', "Optimize for speed (level 2)");
    args.addFlag("check", 0, "Check one or more source files for errors without emitting any output (batch)");
    args.addFlag("no-runtime", 0, "Do not auto-import core/runtime.cb");
    args.addFlag("no-opt", 0, "Disable baseline passes (sroa, mem2reg, instcombine, simplifycfg)");
    args.addFlag("nologo", 0, "Hide compiler version and completion messages");
    args.addFlag("ftime-trace", 0, "Write compilation time trace to <input>.time-trace.json");
    args.addMultiOption("c-include", 0, "Header search directory for C library bindings (repeatable)");
    args.addMultiOption("c-lib", 0, "Prebuilt C import library (.lib) to link (repeatable)");
    args.addMultiOption("c-define", 0, "Preprocessor define passed to all clang-cl C compiles/dumps, e.g. NAME or NAME=val (repeatable)");
    args.addOption("vcpkg-exe", 0, "Explicit path to vcpkg.exe (overrides VS-bundled / VCPKG_ROOT / PATH discovery)");
    args.addOption("vcpkg-manifest", 0, "Explicit vcpkg.json path (skips upward walk from the source file)");
    args.addOption("vcpkg-triplet", 0, "vcpkg triplet (default derived from --platform: x64-windows / x86-windows)");

    if (!args.parse(argc, argv))
        return 1;

    if (args.showVersion())
    {
        std::cout << std::format("{}.{}\n", MAJOR_VERSION, MINOR_VERSION);
        return 0;
    }

    auto filename = args.getPositional(0);
    if (!filename)
    {
        std::cerr << "Error: no input file specified.\n\n";
        args.printUsage();
        return 1;
    }

    bool showLogo = !args.hasFlag("nologo");
    if (showLogo)
        std::cout << std::format("CFlat Compiler {}.{}\n", MAJOR_VERSION, MINOR_VERSION);

    // Locate runtime.cb next to this executable.
    char* pgmptr = nullptr;
    _get_pgmptr(&pgmptr);
    std::string runtimeDir = std::filesystem::path(pgmptr ? pgmptr : "").parent_path().string();

    // --check: compile every positional source file in its own fresh backend,
    // emitting no output. One process amortizes the spawn cost across many files
    // (used by test.bat to batch the err_*.cb negative tests). A failing file does
    // not abort the batch; the overall exit code is non-zero if any file failed.
    if (args.hasFlag("check"))
    {
        int failures = 0;
        for (size_t i = 0; i < args.positionalCount(); ++i)
        {
            std::string file = *args.getPositional(i);
            LLVMBackend compiler;
            compiler.SetRuntimeDir(runtimeDir);
            compiler.SetVerbose(args.hasFlag("verbose"));
            compiler.SetSkipRuntimeImport(args.hasFlag("no-runtime"));
            compiler.SetBatchMode(true);
            bool fileOk = false;
            try
            {
                fileOk = compiler.Compile(args, file);
            }
            catch (const CompilerAbortException&) { fileOk = false; }
            catch (const ExpectedErrorReceived&)  { fileOk = false; }
            if (fileOk)
            {
                if (showLogo) std::cout << std::format("PASS: {}\n", file);
            }
            else
            {
                std::cerr << std::format("FAIL: {}\n", file);
                ++failures;
            }
        }
        if (showLogo)
            std::cout << std::format("Checked {} file(s), {} failed.\n", args.positionalCount(), failures);
        return failures == 0 ? 0 : 1;
    }

    bool ftimeTrace = args.hasFlag("ftime-trace");
    if (ftimeTrace)
        llvm::timeTraceProfilerInitialize(500, "cflat");

    LLVMBackend compiler;
    compiler.SetRuntimeDir(runtimeDir);
    compiler.SetVerbose(args.hasFlag("verbose"));
    compiler.SetSkipRuntimeImport(args.hasFlag("no-runtime"));
    bool ok = compiler.Compile(args);

    if (ftimeTrace)
    {
        auto tracePath = std::filesystem::path(*filename).stem().string() + ".time-trace.json";
        if (auto err = llvm::timeTraceProfilerWrite(tracePath, ""))
            llvm::consumeError(std::move(err));
        else if (showLogo)
            std::cout << "Time trace written to " << tracePath << "\n";
        llvm::timeTraceProfilerCleanup();
    }

    if (!ok)
    {
        std::cerr << "Compilation failed.\n";
        return 1;
    }
    if (showLogo)
        std::cout << "Done.\n";
}

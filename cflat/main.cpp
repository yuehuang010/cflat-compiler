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
    args.addFlag("asan", 0, "Instrument with AddressSanitizer and link the asan runtime (pair with -g for source-line reports). Alias: -fsanitize=address");
    args.addFlag("run", 0, "JIT-compile and run the program in-process without writing an exe to disk. Entry must be 'int main()' or 'int main(int argc, char** argv)'; arguments after a bare '--' are passed as argv[1..]. The process exit code is the program's exit code. Read-only: cannot be combined with -o, -l/--out-lli, or -b/--bitcode.");
    args.addOption("import-dir", 'i', "Directory to search for imported modules");
    args.addOption("platform", 'p', "Target platform: win64 (default) or win32", "win64");
    args.addFlag("verbose", 'v', "Print detailed diagnostic messages during compilation");
    args.addFlag("O0", '0', "No optimization (default)");
    args.addFlag("O1", '1', "Optimize for speed (level 1)");
    args.addFlag("O2", '2', "Optimize for speed (level 2)");
    args.addOption("xthread-scan", 0, "Cross-thread sharing scan level 1..3 (default off). Prints [xthread] reports to stdout for non-atomic/unguarded struct fields shared across a thread spawn. 1=borrowed ctx, 2=+ptr handoff, 3=+any struct-ptr call arg");
    args.addFlag("check", 0, "Check one or more source files for errors without emitting any output (batch)");
    args.addFlag("grammar", 0, "Validate the grammar (parse only) of one or more source files; add -v to print the full parse-tree rule stack");
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
    args.addFlag("vcpkg-no-install", 0, "Do not run 'vcpkg install'; error out if a package-vcpkg port is not already installed");
    args.addFlag("init", 0, "Populate %USERPROFILE%\\.cflat\\ cache with linker paths for x64 and x86, then exit");
    args.addFlag("print-supported-cpus", 0, "List target CPUs supported on Windows x86/x64, then exit");
    args.addFlag("print-host-cpu", 0, "Print the LLVM name of the host CPU (what --cpu native resolves to), then exit");
    args.addOption("cpu", 0, "Target CPU for code generation (name from --print-supported-cpus, or 'native'); sets ISA features + tuning", "");
    args.addOption("tune", 0, "Tune scheduling for this CPU without changing the instruction set (name or 'native')", "");
    args.addFlag("no-cache", 0, "Bypass the core bitcode cache and reparse core libraries from source");
    args.addFlag("c-header-cache-deep", 0, "For C headers opted in with the 'cache' import clause, validate every transitively included file (mtime/hash), not just the top header");

    if (!args.parse(argc, argv))
        return 1;

    if (args.showVersion())
    {
        std::cout << CFLAT_VERSION_STRING "\n";
        return 0;
    }

    // Locate runtime.cb next to this executable (needed for lld-link discovery too).
    char* pgmptr = nullptr;
    _get_pgmptr(&pgmptr);
    std::string runtimeDir = std::filesystem::path(pgmptr ? pgmptr : "").parent_path().string();

    if (args.hasFlag("init"))
    {
        bool ok = LLVMBackend::RunInit(runtimeDir, args.hasFlag("verbose"));
        return ok ? 0 : 1;
    }

    if (args.hasFlag("print-supported-cpus"))
        return LLVMBackend::PrintSupportedCpus() ? 0 : 1;

    if (args.hasFlag("print-host-cpu"))
        return LLVMBackend::PrintHostCpu() ? 0 : 1;

    auto filename = args.getPositional(0);
    if (!filename)
    {
        std::cerr << "Error: no input file specified.\n\n";
        args.printUsage();
        return 1;
    }

    bool showLogo = !args.hasFlag("nologo");
    if (showLogo)
        std::cout << "CFlat Compiler " CFLAT_VERSION_STRING "\n";

    // -ftime-trace is a top-level switch: initialize the profiler up front so every
    // code path below (single compile or --check batch) is captured, and write the trace
    // at the matching exit. The TimeTraceScope annotations inside Compile feed it.
    bool ftimeTrace = args.hasFlag("ftime-trace");
    if (ftimeTrace)
        llvm::timeTraceProfilerInitialize(500, "cflat");
    auto writeTimeTrace = [&](const std::string& tracePath)
    {
        if (!ftimeTrace) return;
        if (auto err = llvm::timeTraceProfilerWrite(tracePath, ""))
            llvm::consumeError(std::move(err));
        else if (showLogo)
            std::cout << "Time trace written to " << tracePath << "\n";
        llvm::timeTraceProfilerCleanup();
    };

    // --grammar: parse each positional source file in isolation to validate its syntax,
    // emitting no output and pulling in no imports/core libraries. With -v, the full
    // parse-tree rule stack is printed for each file. A failing file does not abort the
    // batch; the exit code is non-zero if any file failed to parse.
    if (args.hasFlag("grammar"))
    {
        LLVMBackend compiler;
        compiler.SetRuntimeDir(runtimeDir);
        compiler.SetVerbose(args.hasFlag("verbose"));

        int failures = 0;
        for (size_t i = 0; i < args.positionalCount(); ++i)
        {
            if (!compiler.CheckGrammar(*args.getPositional(i)))
                ++failures;
        }
        if (showLogo)
            std::cout << std::format("Checked grammar of {} file(s), {} failed.\n",
                                     args.positionalCount(), failures);
        return failures == 0 ? 0 : 1;
    }

    // --check: compile every positional source file for diagnostics only, emitting no
    // output (used by test.bat to batch the err_*.cb negative tests). A single backend is
    // reused across files - ResetForReanalysis clears per-file state between them while the
    // core-library parse cache persists, so runtime.cb and its transitive imports are
    // parsed once for the whole batch. A failing file does not abort the batch; the overall
    // exit code is non-zero if any file failed.
    if (args.hasFlag("check"))
    {
        LLVMBackend compiler;
        compiler.SetRuntimeDir(runtimeDir);
        compiler.SetVerbose(args.hasFlag("verbose"));
        compiler.SetSkipRuntimeImport(args.hasFlag("no-runtime"));
        compiler.SetBatchMode(true);
        compiler.SetNoCache(args.hasFlag("no-cache"));
        compiler.SetCHeaderCacheDeep(args.hasFlag("c-header-cache-deep"));

        int failures = 0;
        for (size_t i = 0; i < args.positionalCount(); ++i)
        {
            std::string file = *args.getPositional(i);
            if (i > 0)
                compiler.ResetForReanalysis();  // clear per-file state; keep the core parse cache
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

        writeTimeTrace("check.time-trace.json");
        return failures == 0 ? 0 : 1;
    }

    LLVMBackend compiler;
    compiler.SetRuntimeDir(runtimeDir);
    compiler.SetVerbose(args.hasFlag("verbose"));
    compiler.SetSkipRuntimeImport(args.hasFlag("no-runtime"));
    compiler.SetNoCache(args.hasFlag("no-cache"));
    compiler.SetCHeaderCacheDeep(args.hasFlag("c-header-cache-deep"));
    compiler.SetAsan(args.hasFlag("asan"));

    bool runMode = args.hasFlag("run");
    if (runMode && (args.getOption("output") || args.getOption("out-lli") || args.getOption("bitcode")))
    {
        std::cerr << "Error: --run is read-only and writes nothing to disk; it cannot be combined "
                     "with -o, -l/--out-lli, or -b/--bitcode.\n";
        return 1;
    }
    // Program arguments after a bare "--" are only meaningful when JIT-executing with --run;
    // in any other mode they would silently go nowhere, so reject them up front.
    if (!args.passthrough().empty() && !runMode)
    {
        std::cerr << "Error: program arguments after '--' are only valid with --run.\n";
        return 1;
    }
    compiler.SetRunMode(runMode);
    compiler.SetRunArgs(args.passthrough());

    bool ok = compiler.Compile(args);

    writeTimeTrace(std::filesystem::path(*filename).stem().string() + ".time-trace.json");

    if (!ok)
    {
        std::cerr << "Compilation failed.\n";
        return 1;
    }

    // --run: the process exit code is the JIT'd program's exit code. Suppress the trailing
    // "Done." so --run output is exactly what the program itself printed (plus the logo,
    // unless --nologo).
    if (runMode)
        return compiler.GetJitExitCode();

    if (showLogo)
        std::cout << "Done.\n";
}

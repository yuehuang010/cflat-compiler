// main.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>
#include <filesystem>
#include <stdlib.h>
#include <vector>
#include <string>
#include <cstring>
#include <algorithm>
#include <cctype>

#pragma warning(push)
#pragma warning(disable: 4244 4267)
#include <llvm/Support/TimeProfiler.h>
#pragma warning(pop)

#include "LLVMBackend.h"
#include "CompilerManager.h"
#include "ArgParser.h"
#include "SymbolQuery.h"
#include "Version.h"
#include "LspServer.h"
#include "WinmdExtract.h"
#include "WinmdEmit.h"
#include "WinmdSignature.h"


/*
    The local cache root, <exe dir>/.cflat, or "" when the exe directory is unknown.
    GetExeDir() returns "" when the platform lookup fails (null _get_pgmptr, a chroot with no
    /proc), and a bare concatenation would then yield the literal filesystem root "/.cflat" -
    a live create/delete target that passes every guard. Callers must treat "" as "skip".
*/
static std::string LocalCacheDir()
{
    std::string exeDir = GetExeDir();
    if (exeDir.empty()) return {};
    return exeDir + "/.cflat";
}

// Compare two cache roots as normalised paths: on Windows the per-user root uses backslashes
// while the local root is built with '/', so a raw string compare misses the same directory.
static bool SameCacheRoot(const std::string& a, const std::string& b)
{
    if (a.empty() || b.empty()) return false;
    return std::filesystem::path(a).lexically_normal() == std::filesystem::path(b).lexically_normal();
}

static std::optional<IsolatedPolicy> ConfigureIsolatedMode(LLVMBackend& compiler,
                                                           const ArgParser& args)
{
    auto path = args.getOption("isolated");
    if (!path)
    {
        if (args.getOption("isolated-manifest"))
            compiler.ReportIsolatedPolicyError(
                "policy-restricted-language: --isolated-manifest requires --isolated");
        return std::nullopt;
    }
#if defined(__linux__)
    // Isolated enforcement is verified on macOS and Windows hosts; Linux is still pending.
    compiler.ReportIsolatedPolicyError(std::format(
        "policy-output-unsupported: --isolated is not supported on this host platform yet "
        "(verified on macOS and Windows only) (policy '{}')", *path));
    return std::nullopt;
#endif
    if (!compiler.LoadIsolatedPolicy(*path)) return std::nullopt;

    auto reject = [&](const char* category, const std::string& conflict) {
        compiler.ReportIsolatedPolicyError(std::format("{}: {} (policy '{}')", category, conflict, *path));
    };
    if (args.hasFlag("run"))
        reject("policy-restricted-language", "--isolated cannot be combined with --run");
    if (args.getOption("output"))
    {
        bool nativeMacArm64 = false;
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
        auto platform = args.getOption("platform").value_or("macos");
        nativeMacArm64 = platform == "macos" || platform == "macos-arm64";
#endif
        if (!nativeMacArm64)
            reject("policy-output-unsupported", "isolated -o is supported only on native macOS arm64");
    }
    if (args.getOption("isolated-manifest") && args.hasFlag("check")
        && args.positionalCount() > 1)
        reject("policy-restricted-language",
               "--isolated-manifest cannot be combined with multi-file --check");
    if (args.hasFlag("asan"))
        reject("policy-restricted-language", "--isolated cannot be combined with --asan");
    if (args.hasFlag("heap-audit"))
        reject("policy-restricted-language", "--isolated cannot be combined with --heap-audit");

    for (size_t i = 0; i < args.positionalCount(); ++i)
    {
        auto ext = std::filesystem::path(*args.getPositional(i)).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".c")
            reject("policy-restricted-language", "--isolated cannot be combined with a positional .c input");
    }
    if (!args.getMultiOption("c-include").empty() || !args.getMultiOption("c-lib").empty()
        || !args.getMultiOption("c-define").empty() || !args.getMultiOption("framework").empty())
        reject("policy-restricted-language", "--isolated cannot be combined with native interop options");

    return compiler.GetIsolatedPolicy();
}

int main(int argc, char* argv[])
{
    if (argc >= 2 && std::string_view(argv[1]) == "lsp")
        return RunLspServer(argc - 2, argv + 2);

    CompilerManager::Instance().InstallAssertHook();

#if defined(_WIN32)
    // Route CRT assert/error/warning reports to stderr instead of a popup dialog.
    for (int crtMode : {_CRT_ASSERT, _CRT_ERROR, _CRT_WARN})
    {
        _CrtSetReportMode(crtMode, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(crtMode, _CRTDBG_FILE_STDERR);
    }
#endif

    ArgParser args;
    args.addPositional("filename", "Source file to compile");
    args.addOption("output", 'o', "Output native executable path (.exe)");
    args.addFlag("force", 'B', "Rebuild even when the -o output is up to date");
    args.addOption("out-lli", 'l', "Output LLVM IR file path (.ll)");
    args.addOption("out-asm", 0, "Output host-target assembly file path (.s)");
    args.addOption("bitcode", 'b', "Output bitcode file path (.bc)");
    args.addOption("isolated", 0, "Validate against a restricted compiler policy JSON file");
    args.addOption("isolated-manifest", 0, "Write a digest-bound isolated compilation manifest JSON sidecar");
    args.addFlag("debug-info", 'g', "Emit DWARF debug information");
    args.addOption("subsystem", 0, "Windows PE subsystem for -o: console (default) or windows (GUI, no console window)");
    args.addFlag("asan", 0, "Instrument with AddressSanitizer and link the asan runtime (pair with -g for source-line reports). Alias: -fsanitize=address");
    args.addFlag("sanitize-ownership", 0, "Debug-only ownership sanitizer (M1): instrument dereferences of moved-from owning pointer locals and report 'value moved at L:C, dereferenced after move at L:C' at runtime, then abort. Implies -g. Spellings: --sanitize=ownership, -fsanitize=ownership.");
    args.addFlag("heap-audit", 0, "Instrument the program with the HeapAudit leak oracle: auto-import diagnostic/heap_audit.cb, enable it at main entry, and report still-live allocations at every return. Report-only - leaks print to stderr but do not change the exit code or abort. No double-free detection; use --asan for double-free/use-after-free. Requires -o (links a C diagnostic object).");
    args.addFlag("run", 0, "JIT-compile and run the program in-process without writing an exe to disk. Entry must be 'int main()' or 'int main(int argc, char** argv)'; arguments after a bare '--' are passed as argv[1..]. The process exit code is the program's exit code. Read-only: cannot be combined with -o, -l/--out-lli, --out-asm, or -b/--bitcode.");
    args.addMultiOption("import-dir", 'i', "Directory to search for imported modules (repeatable; searched in order, first match wins)");
    // Default target platform = native host OS. --platform overrides it for
    // cross-compilation (e.g. macos Mach-O emission from a Windows/WSL host).
#if defined(_WIN32)
    args.addOption("platform", 'p', "Target platform: win64 (native default), win32, linux, or macos (arm64 Mach-O cross-compile)", "win64");
#elif defined(__APPLE__)
    args.addOption("platform", 'p', "Target platform: macos (native default, arm64 Mach-O), linux, win64, or win32", "macos");
#else
    args.addOption("platform", 'p', "Target platform: linux (native default), win64, win32, or macos (arm64 Mach-O cross-compile)", "linux");
#endif
    args.addFlag("verbose", 'v', "Print detailed diagnostic messages during compilation");
    args.addOption("locale", 0, "Diagnostic locale (default: en-simple; CFLAT_LOCALE is used when omitted; pseudo prints source templates)");
    args.addOption("locale-dir", 0, "Directory containing diagnostic locale JSON files (default: <compiler>/locales)");
    args.addOption("update-locale", 0, "Collect localized templates during compilation and update <locale>.json under --locale-dir");
    args.addFlag("O0", '0', "No optimization (default)");
    args.addFlag("O1", '1', "Optimize for speed (level 1)");
    args.addFlag("O2", '2', "Optimize for speed (level 2)");
    args.addOption("xthread-scan", 0, "Cross-thread sharing scan level 1..3 (default off). Prints [xthread] reports to stdout for non-atomic/unguarded struct fields shared across a thread spawn. 1=borrowed ctx, 2=+ptr handoff, 3=+any struct-ptr call arg");
    args.addFlag("check", 0, "Check one or more source files for errors without emitting any output (batch)");
    args.addFlag("grammar", 0, "Validate the grammar (parse only) of one or more source files; add -v to print the full parse-tree rule stack");
    args.addFlag("no-runtime", 0, "Do not auto-import core/runtime.cb");
    args.addFlag("no-opt", 0, "Disable baseline passes (sroa, mem2reg, instcombine, simplifycfg)");
    args.addFlag("nologo", 0, "Hide progress and summary messages (PASS/Checked/Emitted)");
    args.addFlag("ftime-trace", 0, "Write compilation time trace to <input>.time-trace.json");
    args.addMultiOption("c-include", 0, "Header search directory for C library bindings (repeatable)");
    args.addMultiOption("c-lib", 0, "Prebuilt C import library (.lib) to link (repeatable)");
    args.addMultiOption("framework", 0, "macOS framework to link, e.g. AppKit (repeatable; mirrors `import framework`)");
    args.addMultiOption("c-define", 0, "Preprocessor define passed to all clang-cl C compiles/dumps, e.g. NAME or NAME=val (repeatable)");
    args.addOption("vcpkg-exe", 0, "Explicit path to vcpkg.exe (overrides VS-bundled / VCPKG_ROOT / PATH discovery)");
    args.addOption("vcpkg-manifest", 0, "Explicit vcpkg.json path (skips upward walk from the source file)");
    args.addOption("vcpkg-triplet", 0, "vcpkg triplet (default derived from --platform: x64-windows / x86-windows)");
    args.addFlag("vcpkg-no-install", 0, "Do not run 'vcpkg install'; error out if a package-vcpkg port is not already installed");
    args.addOption("nuget-packages-dir", 0, "Explicit NuGet global packages folder (overrides NUGET_PACKAGES / %USERPROFILE%\\.nuget\\packages discovery)");
    args.addFlag("nuget-no-install", 0, "Do not download NuGet packages; error out if a package-nuget package is not already in the packages folder");
    args.addFlag("init", 0, "Populate %USERPROFILE%\\.cflat\\ cache with linker paths, core bitcode, and the compiler path, then exit");
    args.addFlag("init-local", 0, "Populate <exe dir>/.cflat cache instead of the per-user cache, then exit; later compiles from this exe pick it up automatically");
    args.addFlag("init-clear", 0, "Delete BOTH the per-user (%USERPROFILE%\\.cflat / ~/.cflat) and local (<exe dir>/.cflat) cache directories and exit; re-run --init/--init-local afterward to repopulate");
    args.addFlag("init-clear-local", 0, "Delete only the <exe dir>/.cflat cache directory and exit; leaves the per-user cache alone");
    args.addFlag("print-supported-cpus", 0, "List target CPUs supported on Windows x86/x64, then exit");
    args.addFlag("print-host-cpu", 0, "Print the LLVM name of the host CPU (what --cpu native resolves to), then exit");
    args.addOption("cpu", 0, "Target CPU for code generation (name from --print-supported-cpus, or 'native'); sets ISA features + tuning", "");
    args.addOption("tune", 0, "Tune scheduling for this CPU without changing the instruction set (name or 'native')", "");
    args.addFlag("no-cache", 0, "Bypass the core bitcode cache and reparse core libraries from source");
    args.addFlag("c-header-cache-deep", 0, "For C headers opted in with the 'cache' import clause, validate every transitively included file (mtime/hash), not just the top header");
    args.addMultiOption("symbol", 0, "Look up one or more symbols (IDE-style quick search) and exit. An exact name match prints detailed info (kind, signature, location, members); a miss suggests the closest symbols. Indexes the positional source file if given, otherwise the whole core library");
    args.addMultiOption("symbol-dump", 0, "Dump symbol info for source elements, then exit (repeatable). Selector: line:<n>, line:<a>-<b>, or function:<name>. Requires a positional source file");
    args.addOption("dump-manifest", 0, "Write the merged Win32 manifest XML (exactly what is embedded as the RT_MANIFEST resource) to the given file, or to stdout with '-'. Works with --check");
    args.addOption("dump-winmd", 0, "Read a WinRT metadata file (.winmd) into the projection model and dump it (diagnostic), then exit");
    args.addOption("emit-winmd", 0, "After compiling, write this program's [winrt] interfaces and classes to the given .winmd file");
    args.addFlag("winmd-sig-selftest", 0, "Validate the WinRT parameterized-type signature encoder and PIID derivation against reference IIDs, then exit");
    args.addOption("winmd-instantiate", 0, "Import the given .winmd and instantiate well-known parameterized interfaces (IVector<i32>, IReference<i32>, ...), checking each derived PIID + vtable shape, then exit");

    if (!args.parse(argc, argv))
    {
        if (!args.getError().empty())
            std::cout << args.getError() << "\n";
        return 1;
    }

    // Whole-output dependency manifests are consulted before runtime discovery, parsing, or
    // compiler construction. Read-only and non-executable modes deliberately bypass this path.
    const bool manifestEligible = args.getOption("output").has_value()
        && !args.showVersion()
        && !args.hasFlag("run") && !args.hasFlag("check") && !args.hasFlag("grammar")
        && !args.hasFlag("init") && !args.hasFlag("init-local")
        && !args.hasFlag("init-clear") && !args.hasFlag("init-clear-local")
        && !args.hasFlag("print-supported-cpus") && !args.hasFlag("print-host-cpu")
        && !args.hasFlag("winmd-sig-selftest")
        && !args.getOption("dump-winmd") && !args.getOption("winmd-instantiate")
        && !args.getOption("out-lli") && !args.getOption("out-asm")
        && !args.getOption("bitcode") && args.getMultiOption("symbol").empty()
        && args.getMultiOption("symbol-dump").empty();
    if (manifestEligible && !args.hasFlag("force"))
    {
        std::string effectiveOutput = *args.getOption("output");
#if defined(_WIN32)
        auto platform = args.getOption("platform").value_or("win64");
        if ((platform == "win32" || platform == "win64")
            && !std::filesystem::path(effectiveOutput).has_extension())
            effectiveOutput += ".exe";
#endif
        if (LLVMBackend::IsOutputUpToDate(effectiveOutput, args.normalizedArguments()))
        {
            std::cout << std::format("up to date: {}\n", *args.getOption("output"));
            return 0;
        }
    }

    if (args.showVersion())
    {
        std::cout << CFLAT_VERSION_STRING "\n";
        return 0;
    }

    // Locate runtime.cb next to this executable (needed for lld-link discovery too).
    std::string runtimeDir = GetExeDir();
    std::string diagnosticLocale = args.getOption("locale").value_or("");
    if (diagnosticLocale.empty())
    {
        if (const char* envLocale = std::getenv("CFLAT_LOCALE"); envLocale && *envLocale)
            diagnosticLocale = envLocale;
    }
    if (diagnosticLocale.empty())
        diagnosticLocale = "en-simple";
    std::string diagnosticLocaleDir = args.getOption("locale-dir").value_or(
        (std::filesystem::path(runtimeDir) / "locales").string());
    auto updateLocale = args.getOption("update-locale");

    // --init / --init-local / --init-clear / --init-clear-local are mutually exclusive;
    // refuse rather than guessing which one was meant.
    {
        std::vector<std::string> initFlagsSeen;
        for (const char* f : {"init", "init-local", "init-clear", "init-clear-local"})
            if (args.hasFlag(f))
                initFlagsSeen.push_back(f);
        if (initFlagsSeen.size() > 1)
        {
            std::string joined;
            for (size_t i = 0; i < initFlagsSeen.size(); ++i)
            {
                if (i) joined += ", ";
                joined += "--" + initFlagsSeen[i];
            }
            std::cout << std::format("Error: {} cannot be combined. Pass exactly one of --init, --init-local, --init-clear, --init-clear-local.\n", joined);
            return 1;
        }
    }

    if (args.hasFlag("init-clear-local"))
    {
        std::string localDir = LocalCacheDir();
        if (const char* envDir = std::getenv("CFLAT_CACHE_DIR"); envDir && *envDir)
            std::cout << std::format("Note: CFLAT_CACHE_DIR={} is set; it is not touched by --init-clear-local.\n", envDir);
        if (localDir.empty())
        {
            std::cout << "local cache: (could not determine the local cache path; the executable directory is unknown)\n";
            return 0;
        }
        std::cout << std::format("cache dir: {}\n", localDir);
        bool ok = LLVMBackend::ClearCacheDir(localDir, args.hasFlag("verbose"), "local");
        return ok ? 0 : 1;
    }

    if (args.hasFlag("init-clear"))
    {
        if (const char* envDir = std::getenv("CFLAT_CACHE_DIR"); envDir && *envDir)
            std::cout << std::format("Note: CFLAT_CACHE_DIR={} is set; it is not touched by --init-clear.\n", envDir);

        std::string localDir = LocalCacheDir();
        std::string userDir = LLVMBackend::GetUserCacheDir();
        bool verbose = args.hasFlag("verbose");
        bool ok = true;

        if (localDir.empty())
        {
            std::cout << "local cache: (could not determine the local cache path; the executable directory is unknown)\n";
        }
        else
        {
            std::cout << std::format("cache dir: {}\n", localDir);
            if (!LLVMBackend::ClearCacheDir(localDir, verbose, "local"))
                ok = false;
        }

        if (userDir.empty())
        {
            std::cout << "per-user cache: (could not determine path; HOME/USERPROFILE not set)\n";
        }
        else if (!SameCacheRoot(userDir, localDir))
        {
            std::cout << std::format("cache dir: {}\n", userDir);
            if (!LLVMBackend::ClearCacheDir(userDir, verbose, "per-user"))
                ok = false;
        }

        return ok ? 0 : 1;
    }

    if (args.hasFlag("init-local"))
    {
        std::string localDir = LocalCacheDir();
        if (localDir.empty())
        {
            std::cout << "Error: could not determine the local cache path; the executable directory is unknown.\n";
            return 1;
        }
        if (std::error_code ec = llvm::sys::fs::create_directories(localDir); ec)
        {
            std::cout << std::format("Error: could not create local cache directory {}: {} ({})\n", localDir, ec.message(), ec.value());
            return 1;
        }
        // create_directories succeeds on an existing read-only directory, so probe with a real
        // write; otherwise --init-local exits 0 having written nothing but warnings.
        {
            std::string probePath = (std::filesystem::path(localDir) / ".cflat-write-probe").string();
            std::error_code probeEc;
            {
                llvm::raw_fd_ostream probe(probePath, probeEc, llvm::sys::fs::OF_Text);
                if (!probeEc)
                {
                    probe << "probe\n";
                    probe.close();
                    if (probe.has_error())
                    {
                        probeEc = probe.error();
                        probe.clear_error();
                    }
                }
            }
            if (probeEc)
            {
                std::cout << std::format("Error: local cache directory {} is not writable: {} ({})\n",
                                         localDir, probeEc.message(), probeEc.value());
                return 1;
            }
            std::error_code rmEc;
            std::filesystem::remove(probePath, rmEc);
        }
        LLVMBackend::SetCacheDirOverride(localDir);
        std::cout << std::format("cache dir: {}\n", localDir);

        // --init-local exits before the main -ftime-trace wiring below, so init/write the
        // profiler here too. Captures the CoreCacheJsonBuild/Write scopes in SaveCoreBitcode.
        bool ft = args.hasFlag("ftime-trace");
        if (ft) llvm::timeTraceProfilerInitialize(500, "cflat");
        bool ok = LLVMBackend::RunInit(runtimeDir, args.hasFlag("verbose"));
        if (ft)
        {
            if (auto err = llvm::timeTraceProfilerWrite("init.time-trace.json", ""))
                llvm::consumeError(std::move(err));
            else
                std::cout << "Time trace written to init.time-trace.json\n";
            llvm::timeTraceProfilerCleanup();
        }
        return ok ? 0 : 1;
    }

    if (args.hasFlag("init"))
    {
        std::cout << std::format("cache dir: {}\n", LLVMBackend::GetCflatCacheDir());

        // --init exits before the main -ftime-trace wiring below, so init/write the
        // profiler here too. Captures the CoreCacheJsonBuild/Write scopes in SaveCoreBitcode.
        bool ft = args.hasFlag("ftime-trace");
        if (ft) llvm::timeTraceProfilerInitialize(500, "cflat");
        bool ok = LLVMBackend::RunInit(runtimeDir, args.hasFlag("verbose"));
        if (ft)
        {
            if (auto err = llvm::timeTraceProfilerWrite("init.time-trace.json", ""))
                llvm::consumeError(std::move(err));
            else
                std::cout << "Time trace written to init.time-trace.json\n";
            llvm::timeTraceProfilerCleanup();
        }
        return ok ? 0 : 1;
    }

    if (args.hasFlag("print-supported-cpus"))
        return LLVMBackend::PrintSupportedCpus() ? 0 : 1;

    if (args.hasFlag("print-host-cpu"))
        return LLVMBackend::PrintHostCpu() ? 0 : 1;

    // --winmd-sig-selftest: validate the parameterized-type signature encoder + PIID derivation
    // against published reference IIDs. Self-contained; no input file needed.
    if (args.hasFlag("winmd-sig-selftest"))
    {
        std::string report;
        bool ok = cflat_winmd::WinmdSignatureSelfTest(report);
        std::cout << report;
        return ok ? 0 : 1;
    }

    // --dump-winmd: read a .winmd into the projection model and print it (Phase 0 validation
    // of the WinrtModel + reader). Handled before the input-file check; it is self-contained.
    if (auto winmd = args.getOption("dump-winmd"))
    {
        cflat_winmd::Model model;
        std::string err;
        if (!cflat_winmd::ReadWinmd(*winmd, model, err))
        {
            std::cout << "Error: " << err << "\n";
            return 1;
        }
        std::cout << cflat_winmd::DumpModel(model);
        return 0;
    }

    // --winmd-instantiate: import a .winmd and instantiate well-known parameterized interfaces,
    // checking each derived PIID + vtable shape (M2 acceptance). Self-contained.
    if (auto winmd = args.getOption("winmd-instantiate"))
    {
        LLVMBackend compiler;
        compiler.SetRuntimeDir(runtimeDir);
        compiler.SetVerbose(args.hasFlag("verbose"));
        compiler.SetLocale(diagnosticLocale);
        compiler.SetLocaleDirectory(diagnosticLocaleDir);
        compiler.LoadLocale(args.hasFlag("verbose"));
        std::string report;
        bool ok = compiler.WinmdInstantiateSelfTest(*winmd, report);
        std::cout << report;
        return ok ? 0 : 1;
    }

    // --symbol: IDE-style quick symbol lookup. Handled before the "input file required"
    // check because it falls back to indexing the whole core library when no file is given.
    if (!args.getMultiOption("symbol").empty())
        return RunSymbolQuery(args, runtimeDir, !args.hasFlag("nologo"));
    if (!args.getMultiOption("symbol-dump").empty())
        return RunSymbolDumpQuery(args, runtimeDir, !args.hasFlag("nologo"));

    auto filename = args.getPositional(0);
    if (!filename)
    {
        std::cout << "Error: no input file specified.\n\n";
        args.printUsage();
        return 1;
    }

    // No banner and no "Done." - the version prints only for --version. --nologo still
    // silences the progress/summary lines below (scripts parse those logs).
    bool showProgress = !args.hasFlag("nologo");

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
        else if (showProgress)
            std::cout << std::format("Time trace written to {}\n", tracePath);
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
        compiler.SetLocale(diagnosticLocale);
        compiler.SetLocaleDirectory(diagnosticLocaleDir);
        compiler.LoadLocale(args.hasFlag("verbose"));

        int failures = 0;
        for (size_t i = 0; i < args.positionalCount(); ++i)
        {
            if (!compiler.CheckGrammar(*args.getPositional(i)))
                ++failures;
        }
        if (showProgress)
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
        compiler.SetLocale(diagnosticLocale);
        compiler.SetLocaleDirectory(diagnosticLocaleDir);
        compiler.LoadLocale(args.hasFlag("verbose"));
        compiler.SetLocaleTemplateCollection(updateLocale.has_value());
        compiler.SetSkipRuntimeImport(args.hasFlag("no-runtime"));
        auto isolatedPolicy = ConfigureIsolatedMode(compiler, args);
        compiler.SetBatchMode(true);
        compiler.SetNoCache(args.hasFlag("no-cache") || isolatedPolicy.has_value());
        compiler.SetCHeaderCacheDeep(args.hasFlag("c-header-cache-deep"));

        int failures = 0;
        for (size_t i = 0; i < args.positionalCount(); ++i)
        {
            std::string file = *args.getPositional(i);
            if (i > 0)
            {
                compiler.ResetForReanalysis();  // clear per-file state; keep the core parse cache
                if (isolatedPolicy) compiler.SetIsolatedPolicy(*isolatedPolicy);
            }
            bool fileOk = false;
            try
            {
                // A .winmd is WinRT metadata, not CFlat source: verify it parses into the
                // projection model (parse-only, no registration) instead of compiling it.
                bool isWinmd = file.size() >= 6 &&
                    _stricmp(file.c_str() + file.size() - 6, ".winmd") == 0;
                fileOk = isWinmd ? compiler.CheckWinmd(file) : compiler.Compile(args, file);
            }
            catch (const CompilerAbortException&) { fileOk = false; }
            catch (const ExpectedErrorReceived&)  { fileOk = false; }
            if (fileOk)
            {
                if (showProgress) std::cout << std::format("PASS: {}\n", file);
            }
            else
            {
                std::cout << std::format("FAIL: {}\n", file);
                ++failures;
            }
        }
        if (showProgress)
            std::cout << std::format("Checked {} file(s), {} failed.\n", args.positionalCount(), failures);

        if (updateLocale && !compiler.WriteCollectedLocale(*updateLocale, args.hasFlag("verbose")))
            return 1;

        writeTimeTrace("check.time-trace.json");
        return failures == 0 ? 0 : 1;
    }

    LLVMBackend compiler;
    compiler.SetRuntimeDir(runtimeDir);
    compiler.SetVerbose(args.hasFlag("verbose"));
    compiler.SetLocale(diagnosticLocale);
    compiler.SetLocaleDirectory(diagnosticLocaleDir);
    compiler.LoadLocale(args.hasFlag("verbose"));
    compiler.SetLocaleTemplateCollection(updateLocale.has_value());
    compiler.SetSkipRuntimeImport(args.hasFlag("no-runtime"));
    auto isolatedPolicy = ConfigureIsolatedMode(compiler, args);
    compiler.SetNoCache(args.hasFlag("no-cache") || isolatedPolicy.has_value());
    compiler.SetCHeaderCacheDeep(args.hasFlag("c-header-cache-deep"));
    if (auto sub = args.getOption("subsystem"))
    {
        if (*sub != "console" && *sub != "windows")
        {
            std::cout << "Error: --subsystem must be 'console' or 'windows' (got '" << *sub << "').\n";
            return 1;
        }
        compiler.SetWindowsSubsystem(*sub);
    }
    compiler.SetAsan(args.hasFlag("asan"));
    compiler.SetSanitizeOwnership(args.hasFlag("sanitize-ownership"));

    bool heapAudit = args.hasFlag("heap-audit");
    if (heapAudit && !args.getOption("output"))
    {
        std::cout << "Error: --heap-audit requires -o; it links a C diagnostic object that the "
                     "HeapAudit oracle needs (it cannot run with --run or IR-only output).\n";
        return 1;
    }
    compiler.SetHeapAudit(heapAudit);

    bool runMode = args.hasFlag("run");
    if (runMode && (args.getOption("output") || args.getOption("out-lli")
                    || args.getOption("out-asm") || args.getOption("bitcode")))
    {
        std::cout << "Error: --run is read-only and writes nothing to disk; it cannot be combined "
                     "with -o, -l/--out-lli, --out-asm, or -b/--bitcode.\n";
        return 1;
    }
    // Program arguments after a bare "--" are only meaningful when JIT-executing with --run;
    // in any other mode they would silently go nowhere, so reject them up front.
    if (!args.passthrough().empty() && !runMode)
    {
        std::cout << "Error: program arguments after '--' are only valid with --run.\n";
        return 1;
    }
    compiler.SetRunMode(runMode);
    compiler.SetRunArgs(args.passthrough());

    bool ok = compiler.Compile(args);

    if (updateLocale && !compiler.WriteCollectedLocale(*updateLocale, args.hasFlag("verbose")))
        return 1;

    writeTimeTrace(std::filesystem::path(*filename).stem().string() + ".time-trace.json");

    if (!ok)
    {
        std::cout << "Compilation failed.\n";
        return 1;
    }

    if (manifestEligible)
    {
        std::string effectiveOutput = *args.getOption("output");
#if defined(_WIN32)
        auto platform = args.getOption("platform").value_or("win64");
        if ((platform == "win32" || platform == "win64")
            && !std::filesystem::path(effectiveOutput).has_extension())
            effectiveOutput += ".exe";
#endif
        // The build already succeeded; a manifest that cannot be written only costs the
        // next run its up-to-date short-circuit, so it is reported and not treated as failure.
        if (!LLVMBackend::WriteDependencyManifest(effectiveOutput, args.normalizedArguments(),
                                                  compiler.GetDependencyFiles()))
            std::cout << std::format("note: could not write dependency manifest for '{}'; the next build will not be skipped.\n",
                                     effectiveOutput);
    }

    // --run: the process exit code is the JIT'd program's exit code, and the output is
    // exactly what the program itself printed.
    if (runMode)
        return compiler.GetJitExitCode();

    // --emit-winmd: after a successful compile, write the [winrt] surface to a .winmd. The
    // assembly name is the output file stem (e.g. Zoo.winmd -> "Zoo").
    if (auto winmdOut = args.getOption("emit-winmd"))
    {
        std::string asmName = std::filesystem::path(*winmdOut).stem().string();
        if (!compiler.EmitWinmd(*winmdOut, asmName))
            return 1;
        if (showProgress)
            std::cout << std::format("Emitted {}\n", *winmdOut);
    }

    return 0;
}

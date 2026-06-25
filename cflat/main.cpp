// main.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>
#include <filesystem>
#include <stdlib.h>
#include <algorithm>
#include <vector>
#include <string>
#include <cctype>
#include <cstring>
#include <chrono>
#include <fstream>

#pragma warning(push)
#pragma warning(disable: 4244 4267)
#include <llvm/Support/TimeProfiler.h>
#pragma warning(pop)

#include "LLVMBackend.h"
#include "CompilerManager.h"
#include "ArgParser.h"
#include "Version.h"
#include "LspServer.h"
#include "WinmdExtract.h"
#include "WinmdEmit.h"
#include "WinmdSignature.h"

// ---- --symbol query mode -------------------------------------------------
// A lightweight "IDE quick search" over the symbol index that a real analysis
// pass produces (the same index that drives LSP hover / go-to-definition). For
// each search term an exact (or case-insensitive) name match prints detailed
// symbol info; a miss falls back to substring / edit-distance matching and
// suggests the closest symbols. The point is to give an agent the API-discovery
// surface a human gets from an editor's symbol search.

static std::string ToLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

static const char* SymbolKindName(SymbolKind k)
{
    switch (k)
    {
        case SymbolKind::Function:  return "function";
        case SymbolKind::Struct:    return "struct";
        case SymbolKind::Interface: return "interface";
        case SymbolKind::Namespace: return "namespace";
        case SymbolKind::TypeAlias: return "type alias";
        case SymbolKind::Field:     return "field";
        case SymbolKind::Variable:  return "variable";
    }
    return "symbol";
}

// Case-insensitive Levenshtein edit distance.
static int EditDistance(const std::string& a, const std::string& b)
{
    const size_t n = a.size(), m = b.size();
    std::vector<int> prev(m + 1), cur(m + 1);
    for (size_t j = 0; j <= m; ++j) prev[j] = (int)j;
    for (size_t i = 1; i <= n; ++i)
    {
        cur[0] = (int)i;
        for (size_t j = 1; j <= m; ++j)
        {
            int cost = (std::tolower((unsigned char)a[i - 1]) ==
                        std::tolower((unsigned char)b[j - 1])) ? 0 : 1;
            cur[j] = std::min({ prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost });
        }
        std::swap(prev, cur);
    }
    return prev[m];
}

// Print full detail for an exact symbol hit, including any members - methods and
// fields are registered under "<Type>.<member>", so we scan the index for that prefix.
static void PrintSymbolDetail(const LspSymbolIndex& index, const SymbolDef& def)
{
    std::cout << std::format("{}  ({})\n", def.name, SymbolKindName(def.kind));
    if (!def.signatureMarkdown.empty() && def.signatureMarkdown != def.name)
        std::cout << std::format("  {}\n", def.signatureMarkdown);
    for (const auto& sig : def.overloadSignatures)
        std::cout << std::format("  {}\n", sig);
    if (def.line > 0 && !def.file.empty())
        std::cout << std::format("  defined: {}:{}\n", def.file, def.line);
    if (!def.docComment.empty())
        std::cout << std::format("  doc: {}\n", def.docComment);

    const std::string prefix = def.name + ".";
    std::vector<const SymbolDef*> members;
    for (const auto& [name, m] : index.Symbols())
        if (name.starts_with(prefix) && name.size() > prefix.size())
            members.push_back(&m);
    std::sort(members.begin(), members.end(),
              [](const SymbolDef* a, const SymbolDef* b) { return a->name < b->name; });
    if (!members.empty())
    {
        std::cout << "  members:\n";
        for (const auto* m : members)
        {
            std::string shortName = m->name.substr(prefix.size());
            std::cout << "    " << m->name;
            if (!m->signatureMarkdown.empty() && m->signatureMarkdown != shortName)
                std::cout << "  :  " << m->signatureMarkdown;
            std::cout << "\n";
            for (const auto& sig : m->overloadSignatures)
                std::cout << std::format("    {}  :  {}\n", m->name, sig);
        }
    }
}

// Print "did you mean" suggestions for a term with no exact match. Substring hits
// rank ahead of edit-distance hits; prefix and shorter names rank best within each band.
static void PrintSymbolSuggestions(const LspSymbolIndex& index, const std::string& term)
{
    struct Suggestion { int score; const SymbolDef* def; };
    const std::string lcq = ToLower(term);
    const int threshold = std::max(2, (int)term.size() / 2);

    std::vector<Suggestion> hits;
    for (const auto& [name, def] : index.Symbols())
    {
        const std::string lcs = ToLower(name);
        int score;
        size_t pos = lcs.find(lcq);
        if (pos != std::string::npos)
            score = (int)pos * 2 + (int)(name.size() - term.size());  // substring band: 0..
        else
        {
            int ed = EditDistance(lcq, lcs);
            if (ed > threshold) continue;
            score = 1000 + ed;  // edit-distance band: ranks after every substring hit
        }
        hits.push_back({ score, &def });
    }

    if (hits.empty())
    {
        std::cout << "  (no similar symbols found)\n";
        return;
    }

    std::sort(hits.begin(), hits.end(), [](const Suggestion& a, const Suggestion& b) {
        if (a.score != b.score) return a.score < b.score;
        return a.def->name < b.def->name;
    });

    std::cout << "  did you mean:\n";
    const size_t maxShow = 10;
    for (size_t i = 0; i < hits.size() && i < maxShow; ++i)
    {
        const SymbolDef* d = hits[i].def;
        std::cout << std::format("    {}  ({})", d->name, SymbolKindName(d->kind));
        if (d->line > 0 && !d->file.empty())
            std::cout << std::format("  {}:{}", d->file, d->line);
        std::cout << "\n";
    }
}

static int RunSymbolQuery(ArgParser& args, const std::string& runtimeDir, bool showLogo)
{
    const std::vector<std::string> terms = args.getMultiOption("symbol");
    const std::string importDir = args.getOption("import-dir").value_or("");

    // What to index: an explicit positional source file (true IDE semantics - the
    // index reflects exactly what that file imports), or, when none is given, a
    // synthetic file that imports every core library so the whole standard library
    // is searchable with zero setup.
    std::string sourcePath;
    std::string tempPath;
    if (args.positionalCount() >= 1)
    {
        sourcePath = *args.getPositional(0);
    }
    else
    {
        std::string body;
        std::error_code ec;
        auto coreDir = std::filesystem::path(runtimeDir) / "core";
        if (std::filesystem::is_directory(coreDir, ec))
        {
            std::vector<std::string> names;
            // Recursive: subdirectory libraries (e.g. hpc/vecmath.cb) must be
            // searchable too, or --symbol silently hides whole library families.
            for (const auto& e : std::filesystem::recursive_directory_iterator(coreDir, ec))
            {
                if (e.path().extension() != ".cb" || e.path().filename() == "runtime.cb")
                    continue;
                std::string rel = std::filesystem::relative(e.path(), coreDir, ec).generic_string();
                names.push_back(rel);
            }
            std::sort(names.begin(), names.end());
            for (const auto& n : names)
                body += "import \"" + n + "\";\n";
        }
        body += "extern int main() { return 0; }\n";

        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        tempPath = (std::filesystem::temp_directory_path() /
                    ("cflat_symquery_" + std::to_string(stamp) + ".cb")).string();
        std::ofstream out(tempPath, std::ios::binary);
        out << body;
        out.close();
        sourcePath = tempPath;
    }

    LLVMBackend compiler;
    compiler.SetRuntimeDir(runtimeDir);
    compiler.SetVerbose(args.hasFlag("verbose"));

    LspSymbolIndex index;
    compiler.SetSymbolSink(&index);
    bool ok = compiler.Analyze(sourcePath, importDir, runtimeDir);
    compiler.SetSymbolSink(nullptr);

    if (!tempPath.empty())
    {
        std::error_code ec;
        std::filesystem::remove(tempPath, ec);
    }

    if (index.SymbolCount() == 0)
    {
        std::cout << "Error: no symbols indexed";
        if (!ok) std::cout << std::format(" (analysis of '{}' failed)", sourcePath);
        std::cout << ".\n";
        return 1;
    }
    if (!ok && showLogo)
        std::cout << "(note: analysis reported errors; results may be incomplete)\n";

    bool first = true;
    for (const auto& term : terms)
    {
        if (!first) std::cout << "\n";
        first = false;

        const SymbolDef* exact = index.Lookup(term);
        if (!exact)
        {
            const std::string lcq = ToLower(term);
            for (const auto& [name, def] : index.Symbols())
                if (ToLower(name) == lcq) { exact = &def; break; }
        }

        if (exact)
            PrintSymbolDetail(index, *exact);
        else
        {
            std::cout << std::format("'{}': no exact match.\n", term);
            if (!tempPath.empty())
                std::cout << "  (no source file given, only showing symbols from core libraries; "
                             "pass a .cb that imports your headers to search them)\n";
            PrintSymbolSuggestions(index, term);
        }
    }
    return 0;
}

int main(int argc, char* argv[])
{
    if (argc >= 2 && std::string_view(argv[1]) == "lsp")
        return RunLspServer(argc - 2, argv + 2);

    CompilerManager::Instance().InstallAssertHook();

    for (int crtMode : {_CRT_ASSERT, _CRT_ERROR, _CRT_WARN})
    {
        _CrtSetReportMode(crtMode, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(crtMode, _CRTDBG_FILE_STDERR);
    }

    ArgParser args;
    args.addPositional("filename", "Source file to compile");
    args.addOption("output", 'o', "Output native executable path (.exe)");
    args.addOption("out-lli", 'l', "Output LLVM IR file path (.ll)");
    args.addOption("bitcode", 'b', "Output bitcode file path (.bc)");
    args.addFlag("debug-info", 'g', "Emit DWARF debug information");
    args.addFlag("asan", 0, "Instrument with AddressSanitizer and link the asan runtime (pair with -g for source-line reports). Alias: -fsanitize=address");
    args.addFlag("heap-audit", 0, "Instrument the program with the HeapAudit leak oracle: auto-import diagnostic/heap_audit.cb, enable it at main entry, and report still-live allocations at every return. Report-only - leaks print to stderr but do not change the exit code or abort. No double-free detection; use --asan for double-free/use-after-free. Requires -o (links a C diagnostic object).");
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
    args.addFlag("init", 0, "Populate %USERPROFILE%\\.cflat\\ cache with linker paths, core bitcode, and the compiler path, then exit");
    args.addFlag("print-supported-cpus", 0, "List target CPUs supported on Windows x86/x64, then exit");
    args.addFlag("print-host-cpu", 0, "Print the LLVM name of the host CPU (what --cpu native resolves to), then exit");
    args.addOption("cpu", 0, "Target CPU for code generation (name from --print-supported-cpus, or 'native'); sets ISA features + tuning", "");
    args.addOption("tune", 0, "Tune scheduling for this CPU without changing the instruction set (name or 'native')", "");
    args.addFlag("no-cache", 0, "Bypass the core bitcode cache and reparse core libraries from source");
    args.addFlag("c-header-cache-deep", 0, "For C headers opted in with the 'cache' import clause, validate every transitively included file (mtime/hash), not just the top header");
    args.addMultiOption("symbol", 0, "Look up one or more symbols (IDE-style quick search) and exit. An exact name match prints detailed info (kind, signature, location, members); a miss suggests the closest symbols. Indexes the positional source file if given, otherwise the whole core library");
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

    if (args.showVersion())
    {
        std::cout << CFLAT_VERSION_STRING "\n";
        return 0;
    }

    // Locate runtime.cb next to this executable (needed for lld-link discovery too).
    std::string runtimeDir = GetExeDir();

    if (args.hasFlag("init"))
    {
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
        std::string report;
        bool ok = compiler.WinmdInstantiateSelfTest(*winmd, report);
        std::cout << report;
        return ok ? 0 : 1;
    }

    // --symbol: IDE-style quick symbol lookup. Handled before the "input file required"
    // check because it falls back to indexing the whole core library when no file is given.
    if (!args.getMultiOption("symbol").empty())
        return RunSymbolQuery(args, runtimeDir, !args.hasFlag("nologo"));

    auto filename = args.getPositional(0);
    if (!filename)
    {
        std::cout << "Error: no input file specified.\n\n";
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
                if (showLogo) std::cout << std::format("PASS: {}\n", file);
            }
            else
            {
                std::cout << std::format("FAIL: {}\n", file);
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

    bool heapAudit = args.hasFlag("heap-audit");
    if (heapAudit && !args.getOption("output"))
    {
        std::cout << "Error: --heap-audit requires -o; it links a C diagnostic object that the "
                     "HeapAudit oracle needs (it cannot run with --run or IR-only output).\n";
        return 1;
    }
    compiler.SetHeapAudit(heapAudit);

    bool runMode = args.hasFlag("run");
    if (runMode && (args.getOption("output") || args.getOption("out-lli") || args.getOption("bitcode")))
    {
        std::cout << "Error: --run is read-only and writes nothing to disk; it cannot be combined "
                     "with -o, -l/--out-lli, or -b/--bitcode.\n";
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

    writeTimeTrace(std::filesystem::path(*filename).stem().string() + ".time-trace.json");

    if (!ok)
    {
        std::cout << "Compilation failed.\n";
        return 1;
    }

    // --run: the process exit code is the JIT'd program's exit code. Suppress the trailing
    // "Done." so --run output is exactly what the program itself printed (plus the logo,
    // unless --nologo).
    if (runMode)
        return compiler.GetJitExitCode();

    // --emit-winmd: after a successful compile, write the [winrt] surface to a .winmd. The
    // assembly name is the output file stem (e.g. Zoo.winmd -> "Zoo").
    if (auto winmdOut = args.getOption("emit-winmd"))
    {
        std::string asmName = std::filesystem::path(*winmdOut).stem().string();
        if (!compiler.EmitWinmd(*winmdOut, asmName))
            return 1;
        if (showLogo)
            std::cout << std::format("Emitted {}\n", *winmdOut);
    }

    if (showLogo)
        std::cout << "Done.\n";
}

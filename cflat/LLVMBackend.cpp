#pragma warning(push)
#pragma warning(disable: 4244 4267)
#include <llvm\IR\IRBuilder.h>
#include <llvm\IR\LLVMContext.h>
#include <llvm\IR\Module.h>
#include <llvm\IR\Verifier.h>
#include <llvm\IR\Dominators.h>
#include <llvm\Bitcode\BitcodeWriter.h>
#include <llvm\Bitcode\BitcodeReader.h>
#include <llvm\Linker\Linker.h>
#include <llvm\Passes\PassBuilder.h>
#include <llvm\Transforms\Utils\Mem2Reg.h>
#include <llvm\Transforms\Scalar\SROA.h>
#include <llvm\Transforms\InstCombine\InstCombine.h>
#include <llvm\Transforms\Scalar\SimplifyCFG.h>
#include <llvm\Transforms\Instrumentation\AddressSanitizer.h>
#include <llvm\Support\CommandLine.h>
#include <llvm\Support\TimeProfiler.h>
#include <llvm\Support\JSON.h>
#include <llvm\IR\DiagnosticInfo.h>
#include <llvm\IR\DiagnosticHandler.h>
#pragma warning(pop)
#include <antlr4-runtime.h>

#include "CFlatParser.h"
#include "CFlatLexer.h"
#include "CFlatBaseListener.h"
#include "LLVMBackend.h"
#include "MainListener.h"
#include "GrammarTreeListener.h"
#include <filesystem>
#include <optional>
#include <algorithm>
#include <cctype>
#include <map>

// Win32 unwind registration for the --run JIT (see cflat_jit::SehRegistrationPlugin). Declared
// here, not in LLVMBackend.h, so this stays in a TU that never includes <winnt.h> - otherwise
// the extern "C" decl would clash with the real prototype in TUs that pull in windows.h. x64
// has a single calling convention, so the bare signature links to kernel32's export directly.
extern "C" unsigned char RtlAddFunctionTable(
    void* FunctionTable, unsigned long EntryCount, unsigned long long BaseAddress);

llvm::Error cflat_jit::SehRegistrationPlugin::RegisterUnwindInfo(llvm::jitlink::LinkGraph& G)
{
    auto* pdata = G.findSectionByName(".pdata");
    if (!pdata)
        return llvm::Error::success(); // no unwind entries (e.g. all-leaf code)

    // Recover JITLink's image base from the first readable ADDR32NB relocation: a stored RVA
    // equals (target - imageBase), so imageBase = target - storedRVA.
    std::optional<uint64_t> imageBase;
    for (auto* B : pdata->blocks())
    {
        for (auto& E : B->edges())
        {
            size_t off = E.getOffset();
            if (off + 4 > (size_t)B->getSize()) continue;
            uint32_t storedRVA = 0;
            std::memcpy(&storedRVA, B->getContent().data() + off, 4);
            uint64_t target = E.getTarget().getAddress().getValue() + (uint64_t)E.getAddend();
            imageBase = target - storedRVA;
            break;
        }
        if (imageBase) break;
    }
    if (!imageBase)
        return llvm::Error::success(); // no relocations to anchor the base

    constexpr size_t kRuntimeFunctionSize = 12; // 3 x 4-byte RVA fields
    for (auto* B : pdata->blocks())
    {
        size_t count = (size_t)B->getSize() / kRuntimeFunctionSize;
        if (count == 0) continue;
        void* funcs = B->getAddress().toPtr<void*>();
        if (!RtlAddFunctionTable(funcs, (unsigned long)count, *imageBase))
            return llvm::make_error<llvm::StringError>(
                "RtlAddFunctionTable failed registering JIT'd .pdata unwind info",
                llvm::inconvertibleErrorCode());
    }
    return llvm::Error::success();
}

// Return the dequoted path from an inline `lib "..."` clause on an import declaration,
// or "" if there is none. Used to wire `import package "x.h" lib "y.lib";` into the link.
// Inline `lib "..."` clause(s) on an import line. Returns every named import library in
// source order - one for the `lib "x.lib"` form, several for the `lib { "a", "b" }` brace
// form. Empty when the import carries no lib clause. Each is resolved to a link argument
// by ResolveCLinkLib at the header-bind site.
static std::vector<std::string> DequoteLibClauses(CFlatParser::ImportDeclarationContext* imp)
{
    std::vector<std::string> libs;
    auto* lc = imp->libClause();
    if (!lc) return libs;
    for (auto* lit : lc->StringLiteral())
    {
        std::string raw = lit->getText();
        if (raw.size() >= 2) libs.push_back(raw.substr(1, raw.size() - 2));
    }
    return libs;
}

// Inline `define "..."` clauses on an `import package` line. Each is scoped to
// that header's clang AST dump and appended on top of the process-wide --c-define.
static std::vector<std::string> DequoteDefineClauses(CFlatParser::ImportDeclarationContext* imp)
{
    std::vector<std::string> defines;
    for (auto* dc : imp->defineClause())
    {
        if (!dc->StringLiteral()) continue;
        std::string raw = dc->StringLiteral()->getText();
        if (raw.size() < 2) continue;
        defines.push_back(raw.substr(1, raw.size() - 2));
    }
    return defines;
}

// True when this import line carries an inline `cache` clause, opting the header into the
// persistent C-header disk cache. Applies to the bare-header and `import package` forms.
static bool HasCacheClause(CFlatParser::ImportDeclarationContext* imp)
{
    return imp->cacheClause() != nullptr;
}

// Dequoted filenames from a grouped import `import { "a", "b" };`, in source order.
// Empty when this import is not the grouped form (so callers can fall through to the
// single-StringLiteral handling). Each entry is a plain import: no alias/lib/define/cache.
static std::vector<std::string> DequoteImportGroup(CFlatParser::ImportDeclarationContext* imp)
{
    std::vector<std::string> out;
    auto* grp = imp->importGroup();
    if (!grp) return out;
    for (auto* lit : grp->StringLiteral())
    {
        std::string raw = lit->getText();
        if (raw.size() >= 2) out.push_back(raw.substr(1, raw.size() - 2));
    }
    return out;
}

// True when this import line is `import package-vcpkg "..." from "...";`. Detected by
// the second child token text - mirrors the existing `package` / `program` test.
static bool IsPackageVcpkgImport(CFlatParser::ImportDeclarationContext* imp)
{
    return imp->children.size() >= 2 && imp->children[1]->getText() == "package-vcpkg";
}

// Dequoted port spec from `from "..."` on an import package-vcpkg line (empty if absent).
static std::string DequoteFromClause(CFlatParser::ImportDeclarationContext* imp)
{
    auto* fc = imp->fromClause();
    if (!fc || !fc->StringLiteral()) return "";
    std::string raw = fc->StringLiteral()->getText();
    if (raw.size() < 2) return "";
    return raw.substr(1, raw.size() - 2);
}

static std::vector<std::string> ReadFileToLines(std::ifstream& stream)
{
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(stream, line))
        lines.push_back(line);
    stream.clear();
    stream.seekg(0);
    return lines;
}

void LLVMBackend::ReportParseErrors(const std::vector<ParseDiagnostic>& diagnostics,
                                       const std::vector<std::string>& sourceLines)
{
    for (const auto& d : diagnostics)
    {
        if (diagnosticSink_)
        {
            std::string msg = d.message;
            if (!d.hint.empty())
                msg += "\nhint: " + d.hint;
            diagnosticSink_(d.file, static_cast<size_t>(d.line), static_cast<size_t>(d.col), msg, 1);
            // No throw - parse errors are pre-codegen; caller returns false.
        }
        else
        {
            std::cerr << std::format("{}({},{}): error: {}\n", d.file, d.line, d.col, d.message);

            int lineIdx = d.line - 1;
            if (lineIdx >= 0 && lineIdx < static_cast<int>(sourceLines.size()))
            {
                const std::string& srcLine = sourceLines[lineIdx];
                std::cerr << "    " << srcLine << "\n";
                std::string caret(4 + std::min(d.col, static_cast<int>(srcLine.size())), ' ');
                caret += '^';
                std::cerr << caret << "\n";
            }

            if (!d.hint.empty())
                std::cerr << "hint: " << d.hint << "\n";
        }
    }
}

bool LLVMBackend::CheckGrammar(const std::string& filename)
{
    if (!std::filesystem::exists(filename))
    {
        std::cerr << "Error: input file '" << filename << "' does not exist.\n";
        return false;
    }

    std::ifstream stream;
    stream.open(filename);
    if (!stream)
    {
        std::cerr << "Error: cannot open input file '" << filename << "'.\n";
        return false;
    }
    auto sourceLines = ReadFileToLines(stream);

    antlr4::ANTLRInputStream input(stream);
    CFlatLexer lexer(&input);
    antlr4::CommonTokenStream tokens(&lexer);
    CFlatParser parser(&tokens);

    CFlatErrorListener errorListener(filename, sourceLines);
    lexer.removeErrorListeners();
    lexer.addErrorListener(&errorListener);
    parser.removeErrorListeners();
    parser.addErrorListener(&errorListener);

    tokens.fill();
    auto* compilationUnit = parser.compilationUnit();

    // With -v, print the full parse-tree rule stack so the grammar match can be
    // inspected rule by rule. Printed even when there are syntax errors - the
    // partial tree shows how far the parse got before the error.
    if (verbose)
    {
        std::cout << std::format("[grammar] {}: {} tokens, parse-tree rule stack:\n",
                                 filename, tokens.getTokens().size());
        GrammarTreeListener treeListener(&parser);
        antlr4::tree::ParseTreeWalker().walk(&treeListener, compilationUnit);
    }

    if (errorListener.hasErrors())
    {
        ReportParseErrors(errorListener.getDiagnostics(), sourceLines);
        std::cerr << std::format("GRAMMAR FAIL: {} ({} error(s))\n",
                                 filename, errorListener.getDiagnostics().size());
        return false;
    }

    std::cout << std::format("GRAMMAR OK: {}\n", filename);
    return true;
}

bool LLVMBackend::Compile(const ArgParser& args, const std::string& inputOverride)
{
    // --check: parse and codegen for diagnostics only, emitting no outputs.
    bool checkOnly = args.hasFlag("check");
    auto filename = inputOverride.empty() ? args.getPositional(0).value_or("") : inputOverride;
    sourceFileName = std::filesystem::path(filename).filename().string();
    llvm::TimeTraceScope compileScope("Compilation", sourceFileName);
    auto rootCanonical = std::filesystem::weakly_canonical(filename).string();
    currentSourceFilePath_ = rootCanonical;
    importedFiles.insert(rootCanonical);
    importStack.push_back(rootCanonical);
    struct RootGuard {
        std::vector<std::string>& stack;
        ~RootGuard() { stack.pop_back(); }
    } rootGuard{importStack};
    // In check mode all positionals are independent source files (handled by the
    // batch loop), so no output is emitted and no extra positional is linked.
    auto exePath = checkOnly ? std::optional<std::string>{} : args.getOption("output");
    auto lliPath = checkOnly ? std::optional<std::string>{} : args.getOption("out-lli");
    auto bitcodePath = checkOnly ? std::string{} : args.getOption("bitcode").value_or("");
    bool debugInfo = args.hasFlag("debug-info");

    // Delete any pre-existing outputs up front so a failed compile can never
    // leave a stale binary behind for the user to unknowingly run. Without this,
    // an edit-compile-run loop that hits a compile error silently re-runs the
    // previous build's exe/IR. Use the non-throwing remove (a running/locked exe
    // simply fails to delete, which is fine - that path is unwritable anyway and
    // emission will report it).
    {
        std::error_code rmEc;
        if (exePath)             std::filesystem::remove(*exePath, rmEc);
        if (lliPath)             std::filesystem::remove(*lliPath, rmEc);
        if (!bitcodePath.empty()) std::filesystem::remove(bitcodePath, rmEc);
    }

    importSearchDir = args.getOption("import-dir").value_or("");
    auto platformOption = args.getOption("platform").value_or("win64");
    // Resolve and validate --cpu / --tune once, up front, so an unknown name is a clean
    // error before any clang-cl spawn or codegen, and both the C-interop and native-object
    // paths can use the resolved values verbatim. The CPU table is identical for both
    // triples (shared X86 backend); we validate against the active platform's triple.
    {
        std::string triple = (platformOption == "win32")
            ? "i686-pc-windows-msvc" : "x86_64-pc-windows-msvc";
        if (auto cpuOpt = args.getOption("cpu"))
            if (!ResolveCpuName(*cpuOpt, triple, "--cpu", verbose, targetCpu_))
                return false;
        if (auto tuneOpt = args.getOption("tune"))
            if (!ResolveCpuName(*tuneOpt, triple, "--tune", verbose, tuneCpu_))
                return false;
    }
    // Captured for clang C compiles (imports run during parse, before EmitExecutable).
    cOptLevel_ = args.getOptimizationLevel();
    cDebugInfo_ = debugInfo;
    // Cross-thread sharing scan level (--xthread-scan N, N in 1..3); 0 = silent default.
    // Threads through --check too since that path also calls Compile(args, file).
    xthreadScanLevel_ = args.getXthreadScanLevel();
    // C library bindings: header search dirs, prebuilt import libraries, and defines.
    cIncludeDirs_ = args.getMultiOption("c-include");
    for (const auto& lib : args.getMultiOption("c-lib"))
        cLinkLibs_.push_back(lib);
    cDefines_ = args.getMultiOption("c-define");
    // Vcpkg integration options: forward to the resolver. Each is optional - the
    // resolver auto-discovers vcpkg.exe and the manifest when not overridden.
    if (auto p = args.getOption("vcpkg-exe"))      vcpkg_.SetExeOverride(*p);
    if (auto p = args.getOption("vcpkg-manifest")) vcpkg_.SetManifestOverride(*p);
    if (auto p = args.getOption("vcpkg-triplet"))  vcpkg_.SetTripletOverride(*p);
    vcpkg_.SetNoInstall(args.hasFlag("vcpkg-no-install"));

    if (verbose)
    {
        std::cout << "[verbose] input:        " << filename << "\n";
        std::cout << "[verbose] output exe:   " << (exePath ? *exePath : "(none)") << "\n";
        std::cout << "[verbose] output lli:   " << (lliPath ? *lliPath : "(none)") << "\n";
        std::cout << "[verbose] runtime dir:  " << runtimeDir << "\n";
        std::cout << "[verbose] import dir:   " << (importSearchDir.empty() ? "(none)" : importSearchDir) << "\n";
        std::cout << "[verbose] debug info:   " << (debugInfo ? "yes" : "no") << "\n";
        // Effective codegen CPU: the platform default unless --cpu overrode it. Tune
        // defaults to the target CPU unless --tune set it (mirrors -mtune semantics).
        std::string effectiveCpu = targetCpu_.empty()
            ? (platformOption == "win32" ? "i686" : "x86-64") : targetCpu_;
        std::cout << "[verbose] target cpu:   " << effectiveCpu << "\n";
        std::cout << "[verbose] tune:         "
                  << (tuneCpu_.empty() ? effectiveCpu + " (same as target cpu)" : tuneCpu_) << "\n";
    }

    if (exePath)
    {
        auto exeDir = std::filesystem::path(*exePath).parent_path();
        if (!exeDir.empty() && !std::filesystem::exists(exeDir))
        {
            std::cerr << "Error: output directory '" << exeDir.string() << "' does not exist (-o " << *exePath << ").\n";
            return false;
        }
    }

    if (lliPath)
    {
        auto lliDir = std::filesystem::path(*lliPath).parent_path();
        if (!lliDir.empty() && !std::filesystem::exists(lliDir))
        {
            std::cerr << "Error: output directory '" << lliDir.string() << "' does not exist (--out-lli " << *lliPath << ").\n";
            return false;
        }
    }

    if (!std::filesystem::exists(filename))
    {
        std::cerr << "Error: input file '" << filename << "' does not exist.\n";
        return false;
    }

    // Set the platform constant (__PLATFORM__) based on the target platform.
    // This is a compile-time constant available in all compiled files.
    platformValue = (platformOption == "win32") ? 32 : 64;
    if (verbose) std::cout << "[verbose] __PLATFORM__ = " << platformValue << "\n";

    // Try to load core bitcode cache BEFORE setting up the module, because
    // LoadCoreBitcodeIfFresh replaces context/module/builder with a fresh LLVM
    // context loaded from the bitcode file.  This avoids named-type conflicts with
    // the %string type pre-created by RegisterBuiltinString in Init().
    // --run is read-only (see the run-mode note below): it touches no on-disk caches, so the
    // import/core-bitcode cache is disabled here just like an explicit --no-cache.
    bool bitcodeLoaded = false;
    if (!batchMode_ && !noCache_ && !runMode_ && !runtimeDir.empty() && !skipRuntimeImport)
    {
        std::string bcCacheDir = GetRuntimeBitcodeDir(runtimeDir);
        if (!bcCacheDir.empty())
        {
            llvm::TimeTraceScope bcScope("RuntimeImport", "core.bc");
            bitcodeLoaded = LoadCoreBitcodeIfFresh(bcCacheDir, platformOption);
            if (verbose)
                std::cout << "[verbose] core bitcode cache: " << (bitcodeLoaded ? "hit" : "miss") << "\n";
        }
    }

    // Set the module data layout when not loading from cache.
    // Cache path: the module already has the correct layout from the bitcode.
    // Must precede InitDebugInfo so pointer-width queries during DI construction
    // see the real layout instead of the LLVM default.
    if (!bitcodeLoaded)
    {
        // Win32: 32-bit pointers; Win64: 64-bit pointers. Both little-endian MSVC.
        const char* dl = (platformValue == 32)
            ? "e-m:x-p:32:32-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:32-n8:16:32-S32"
            : "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128";
        module->setDataLayout(llvm::DataLayout(dl));
    }

    if (debugInfo)
    {
        std::filesystem::path filePath = std::filesystem::absolute(filename);
        InitDebugInfo(filePath.filename().string(), filePath.parent_path().string());
    }

    // Pre-populate compile-time macros (constants throughout compilation)
    {
        // __FILE__: source filename (create global string directly without BasicBlock)
        auto* fileGlobalStr = module->getOrInsertGlobal("__FILE__",
            llvm::ArrayType::get(llvm::Type::getInt8Ty(*context), sourceFileName.size() + 1));
        auto* fileConst = llvm::ConstantDataArray::getString(*context, sourceFileName, true);
        if (auto* gv = llvm::dyn_cast<llvm::GlobalVariable>(fileGlobalStr))
        {
            gv->setInitializer(fileConst);
            gv->setConstant(true);
            SetCompileTimeMacro("__FILE__", gv, "string");
        }

        // __PLATFORM__: target platform (64 or 32)
        auto platformConst = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), platformValue);
        SetCompileTimeMacro("__PLATFORM__", platformConst, "int");

        auto win64Const = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), platformValue == 64 ? 1 : 0);
        SetCompileTimeMacro("__WIN64__", win64Const, "int");
        auto win32Const = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), platformValue == 32 ? 1 : 0);
        SetCompileTimeMacro("__WIN32__", win32Const, "int");
        auto windowsConst = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 1);
        SetCompileTimeMacro("__WINDOWS__", windowsConst, "int");
        // Target architecture is always x86/Intel today (win64 -> x86_64, win32 -> i686).
        // Exposed so callers can guard architecture-specific intrinsics like __rdtscp().
        auto x86Const = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 1);
        SetCompileTimeMacro("__X86__", x86Const, "int");

        if (verbose) std::cout << "[verbose] macros: __FILE__ = \"" << sourceFileName << "\", __PLATFORM__ = " << platformValue << "\n";
    }

    if (!bitcodeLoaded && !runtimeDir.empty() && !skipRuntimeImport)
    {
        {
            llvm::TimeTraceScope runtimeScope("RuntimeImport", "runtime.cb");
            auto runtimePath = std::filesystem::path(runtimeDir) / "core" / "runtime.cb";
            if (verbose) std::cout << "[verbose] auto-importing runtime: " << runtimePath.string() << "\n";
            if (std::filesystem::exists(runtimePath))
                CompileImportedFile(runtimePath.string(), "runtime.cb");
            else if (verbose)
                std::cout << "[verbose]   runtime.cb not found, skipping\n";
        }
    }

    {
        if (verbose) std::cout << "[verbose] parsing " << filename << "\n";
        std::ifstream stream;
        stream.open(filename);
        auto sourceLines = ReadFileToLines(stream);

        antlr4::ANTLRInputStream input(stream);
        CFlatLexer lexer(&input);
        antlr4::CommonTokenStream tokens(&lexer);
        CFlatParser parser(&tokens);

        CFlatErrorListener errorListener(filename, sourceLines);
        lexer.removeErrorListeners();
        lexer.addErrorListener(&errorListener);
        parser.removeErrorListeners();
        parser.addErrorListener(&errorListener);

        CFlatParser::CompilationUnitContext* computeUnit;
        {
            llvm::TimeTraceScope parseScope("Parse", sourceFileName);
            tokens.fill();
            computeUnit = parser.compilationUnit();
        }
        if (verbose) std::cout << "[verbose]   parse complete (" << tokens.getTokens().size() << " tokens)\n";

        if (errorListener.hasErrors())
        {
            ReportParseErrors(errorListener.getDiagnostics(), sourceLines);
            return false;
        }

        // Process all top-level imports before scanning the main file so that
        // imported symbols are available to ForwardRefScanner.
        {
            llvm::TimeTraceScope importsScope("ProcessImports", sourceFileName);
            if (auto* tu = computeUnit->translationUnit()) {
                for (auto* decl : tu->externalDeclaration()) {
                    if (auto* imp = decl->importDeclaration()) {
                        // `import { "a", "b" };` - a group of >1 plain imports. Each entry
                        // routes like a plain `import "x";` (no per-entry alias/lib/define). A
                        // trailing `cache` on the group applies to every entry (a no-op for
                        // .cb/.c entries; only the .h header-bind branch consults it).
                        auto groupedImports = DequoteImportGroup(imp);
                        if (groupedImports.size() > 1)
                        {
                            bool cacheGroup = HasCacheClause(imp);
                            for (const auto& gf : groupedImports)
                            {
                                if (verbose) std::cout << "[verbose] import requested (group): " << gf << (cacheGroup ? " (cache)" : "") << "\n";
                                if (!CompileImportedFile(filename, gf, "", "", {}, {}, cacheGroup))
                                    return false;
                            }
                            continue;
                        }
                        // Single filename: from the importGroup (plain/`as`/`cache` form) or,
                        // for the program/package/vcpkg alternatives, the direct StringLiteral.
                        std::string importFilename;
                        if (!groupedImports.empty())
                            importFilename = groupedImports[0];
                        else
                        {
                            std::string raw = imp->StringLiteral()->getText();
                            importFilename = raw.substr(1, raw.size() - 2);
                        }
                        // `import package-vcpkg "header" from "port";` - dispatch to the
                        // vcpkg resolver and skip the regular import dedup/parse machinery.
                        if (IsPackageVcpkgImport(imp))
                        {
                            std::string portSpec = DequoteFromClause(imp);
                            if (verbose) std::cout << "[verbose] vcpkg import: " << importFilename << " from " << portSpec << "\n";
                            if (!CompileVcpkgImport(RootVcpkgImportPath(filename), importFilename, portSpec))
                                return false;
                            continue;
                        }
                        std::string ns = imp->Identifier() ? imp->Identifier()->getText() : "";
                        bool isProgram = imp->children.size() >= 2 && imp->children[1]->getText() == "program";
                        std::string alias = isProgram ? imp->Identifier()->getText() : "";
                        std::string impExt = std::filesystem::path(importFilename).extension().string();
                        std::transform(impExt.begin(), impExt.end(), impExt.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                        bool isCProgram = isProgram && impExt == ".c";
                        std::vector<std::string> explicitLibs = DequoteLibClauses(imp);
                        std::vector<std::string> extraDefines = DequoteDefineClauses(imp);
                        bool cacheHeader = HasCacheClause(imp);
                        if (verbose) std::cout << "[verbose] import requested: " << importFilename << (ns.empty() ? "" : " as " + ns) << (cacheHeader ? " (cache)" : "") << "\n";
                        if (!CompileImportedFile(filename, importFilename, ns, isCProgram ? alias : "", explicitLibs, extraDefines, cacheHeader))
                            return false;

                        if (isProgram)
                        {
                            if (isCProgram)
                            {
                                // CompileCFile already created __imported_main_<alias> and set programTable.
                                if (!programTable[alias].MainFunction)
                                {
                                    LogError(std::format("import program '{}': C file '{}' has no externally-linkable 'main'", alias, importFilename));
                                    return false;
                                }
                            }
                            else
                            {
                                // For 'import program "file.cb" as Name', rename @main to __imported_main_Name
                                auto* mainFn = module->getFunction("main");
                                if (!mainFn)
                                {
                                    LogError(std::format("import program '{}': '{}' has no 'main' function", alias, importFilename));
                                    return false;
                                }
                                mainFn->setName("__imported_main_" + alias);
                                programTable[alias].IsImportedProgram = true;
                                programTable[alias].MainFunction = mainFn;
                            }
                        }
                    }
                }
            }
        }

        // Pre-scan: register all function signatures and struct type shells so
        // that forward references resolve during the main code-gen walk.
        {
            llvm::TimeTraceScope scanScope("ForwardRefScan", sourceFileName);
            if (verbose) std::cout << "[verbose] forward-ref scan (" << sourceFileName << ")\n";
            ForwardRefScanner scanner(this);
            scanner.SetTokens(&tokens);
            // First pass: pre-declare opaque types and constructors for every
            // generic instantiation found anywhere in the file (including inside
            // function bodies), so uses like Box<MyType> b = Box__MyType() resolve.
            if (auto* tu = computeUnit->translationUnit())
                for (auto* decl : tu->externalDeclaration())
                    scanner.ScanGenericTypeUses(decl);
            // Second pass: register non-generic struct shells and function signatures.
            if (auto* tu = computeUnit->translationUnit())
                for (auto* decl : tu->externalDeclaration())
                    scanner.ScanExternalDeclaration(decl);
        }

        // Cross-thread sharing scan (--xthread-scan N): a read-only pre-pass that records
        // the struct types escaping across a thread-spawn boundary, so the codegen walk can
        // report non-atomic, non-guarded field accesses of those types. Silent at level 0.
        ScanCrossThreadEscapes(computeUnit);

        if (verbose) std::cout << "[verbose] code-gen walk (" << sourceFileName << ")\n";
        {
            llvm::TimeTraceScope codegenScope("CodeGeneration", sourceFileName);
            auto myListener = std::make_unique<MainListener>(&parser, this, sourceFileName);
            auto walker = antlr4::tree::ParseTreeWalker();
            walker.walk(myListener.get(), computeUnit);
        }
        stream.close();
        if (verbose) std::cout << "[verbose]   walk complete\n";
    }

    {
        llvm::TimeTraceScope debugScope("FinalizeDebugInfo");
        if (verbose) std::cout << "[verbose] finalizing debug info\n";
        FinalizeDebugInfo();
    }

    // validate that the stack is empty.
    if (stackNamedVariable.size() > 0)
    {
        for (const auto& stack : stackNamedVariable)
        {
            std::cerr << "Warning: scope stack is not empty at end of compilation:\n";
            for (const auto& funcVariable : stack.functionArgument)
            {
                std::cerr << "  Function var: " << funcVariable.first << "\n";
            }
            for (const auto& namedVariable : stack.namedVariable)
            {
                std::cerr << "  namedVar var: " << namedVariable.first << "\n";
            }
        }
    }

    // --tune is tune-only: it must not change the legal instruction set, only the
    // scheduling model. LLVM plumbs that as a per-function "tune-cpu" attribute (the X86
    // subtarget reads it for the sched model while features still come from the target
    // CPU). Stamp it on every defined function, mirroring clang, before any output is
    // emitted so it lands in --out-lli / --bitcode and the linked object alike.
    if (!tuneCpu_.empty())
    {
        for (auto& F : *module)
            if (!F.isDeclaration())
                F.addFnAttr("tune-cpu", tuneCpu_);
    }

    {
        llvm::TimeTraceScope verifyScope("VerifyModule");
        if (verbose) std::cout << "[verbose] verifying module\n";
        if (!VerifyModule())
        {
            std::cerr << "Error: module verification failed.\n";
            return false;
        }
    }

    if (!args.hasFlag("no-opt"))
    {
        llvm::TimeTraceScope baselineScope("BaselinePasses");
        if (verbose) std::cout << "[verbose] running baseline passes (sroa, mem2reg, instcombine, simplifycfg)\n";
        RunBaselinePasses();
    }

    int optLevel = args.getOptimizationLevel();
    // OptimizeModule also hosts the AddressSanitizer pass, which must run even at -O0,
    // so enter it when --asan is set regardless of the optimization level.
    if (optLevel > 0 || asan_)
    {
        llvm::TimeTraceScope optScope("OptimizePasses", std::format("O{}", optLevel));
        if (verbose) std::cout << "[verbose] running optimizations (O" << optLevel
                               << (asan_ ? ", asan" : "") << ")\n";
        OptimizeModule(optLevel);
    }

    // Written after optimization so --out-lli reflects the final IR (vectorized
    // loops, inlining, etc.) that actually lands in the object.
    if (lliPath)
    {
        llvm::TimeTraceScope irScope("WriteIR", *lliPath);
        if (verbose) std::cout << "[verbose] writing IR to " << *lliPath << "\n";
        if (!SaveToFile(*lliPath))
        {
            std::cerr << "Error: failed to save IR to '" << *lliPath << "'.\n";
            return false;
        }
    }

    if (!bitcodePath.empty())
    {
        if (verbose) std::cout << "[verbose] writing bitcode to " << bitcodePath << "\n";
        if (!WriteBitcode(bitcodePath))
        {
            std::cerr << "Error: failed to write bitcode to '" << bitcodePath << "'.\n";
            return false;
        }
    }

    // Dispatch any extra positional .c files to clang and link their objects.
    // Skipped in check mode: extra positionals are independent source files there.
    if (!checkOnly)
    {
        auto isCSource = [](const std::string& p) {
            auto ext = std::filesystem::path(p).extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            return ext == ".c";
        };
        bool anyCSource = false;
        for (size_t i = 1; i < args.positionalCount(); ++i)
            if (isCSource(*args.getPositional(i))) anyCSource = true;

        if (anyCSource && !exePath)
        {
            std::cerr << "Error: C source input requires -o to link the resulting object.\n";
            return false;
        }
        for (size_t i = 1; i < args.positionalCount(); ++i)
        {
            auto cPath = *args.getPositional(i);
            if (!isCSource(cPath)) continue;
            if (!std::filesystem::exists(cPath))
            {
                std::cerr << "Error: C source input '" << cPath << "' does not exist.\n";
                return false;
            }
            if (!CompileCFile(cPath))
                return false;
        }
    }

    // --run: JIT and execute in-process, emitting nothing to disk. Mutually exclusive with
    // -o (checked in main). Consumes the module, so it must come after IR/bitcode writes.
    //
    // NOTE: --run is read-only - it writes nothing to disk. No exe is produced; the disk-writing
    // output flags (-o, -l/--out-lli, -b/--bitcode) are rejected up front in main(); and the
    // on-disk caches (core/import bitcode, C-header import cache) are disabled under run mode
    // (see the gates in Compile() above and in CompileCHeader / CompileVcpkgImport). The
    // IR/bitcode write blocks above are therefore no-ops in run mode (their paths are empty).
    if (runMode_)
    {
        llvm::TimeTraceScope runScope("JitRun", filename);
        if (verbose) std::cout << "[verbose] JIT-executing in-process\n";
        if (!JitRun(jitExitCode_))
            return false;
        return true;
    }

    if (exePath)
    {
        llvm::TimeTraceScope emitScope("EmitExecutable", *exePath);
        if (verbose) std::cout << "[verbose] emitting executable to " << *exePath << "\n";
        if (!EmitExecutable(*exePath, platformOption, debugInfo))
        {
            std::cerr << "Error: failed to emit executable '" << *exePath << "'.\n";
            return false;
        }
    }

    return true;
}

LLVMBackend::CachedParseTree* LLVMBackend::GetOrParseFile(const std::string& canonicalPath, const std::string& displayName, bool isCore)
{
    // Reuse a previously parsed core tree when the file on disk is unchanged.
    if (isCore)
    {
        auto it = parseTreeCache_.find(canonicalPath);
        if (it != parseTreeCache_.end())
        {
            std::error_code tec;
            auto wt = std::filesystem::last_write_time(canonicalPath, tec);
            if (!tec && wt == it->second->writeTime)
            {
                llvm::TimeTraceScope cacheScope("Parse", "cached:" + displayName);
                if (verbose) std::cout << "[verbose]   parse cache hit: " << displayName << "\n";
                return it->second.get();
            }
            parseTreeCache_.erase(it);  // stale - re-parse below
        }
    }

    auto entry = std::make_unique<CachedParseTree>();
    entry->canonicalPath = canonicalPath;

    std::ifstream stream(canonicalPath);
    if (!stream.is_open())
    {
        std::cerr << "Error: failed to open imported file '" << canonicalPath << "'.\n";
        return nullptr;
    }
    auto sourceLines = ReadFileToLines(stream);  // also rewinds the stream to the start

    entry->input  = std::make_unique<antlr4::ANTLRInputStream>(stream);
    entry->lexer  = std::make_unique<CFlatLexer>(entry->input.get());
    entry->tokens = std::make_unique<antlr4::CommonTokenStream>(entry->lexer.get());
    entry->parser = std::make_unique<CFlatParser>(entry->tokens.get());

    CFlatErrorListener errorListener(canonicalPath, sourceLines);
    entry->lexer->removeErrorListeners();
    entry->lexer->addErrorListener(&errorListener);
    entry->parser->removeErrorListeners();
    entry->parser->addErrorListener(&errorListener);

    {
        llvm::TimeTraceScope parseScope("Parse", displayName);
        entry->tokens->fill();
        entry->unit = entry->parser->compilationUnit();
    }
    if (verbose) std::cout << "[verbose]   parse complete (" << entry->tokens->getTokens().size() << " tokens)\n";

    if (errorListener.hasErrors())
    {
        ReportParseErrors(errorListener.getDiagnostics(), sourceLines);
        return nullptr;  // never cache a failed parse
    }

    // errorListener is a stack object about to be destroyed; the parser never parses
    // again, but drop the now-dangling listener pointers so nothing can dereference them.
    entry->lexer->removeErrorListeners();
    entry->parser->removeErrorListeners();

    CachedParseTree* raw = entry.get();
    if (isCore)
    {
        std::error_code tec;
        entry->writeTime = std::filesystem::last_write_time(canonicalPath, tec);
        parseTreeCache_[canonicalPath] = std::move(entry);
    }
    else
    {
        importedParseStates.push_back(std::move(entry));
    }
    return raw;
}

// Detect the Windows SDK "um"/"shared"/"ucrt"/"winrt" include dirs (latest installed version) so a
// bare `import "windows.h"` (and other system headers) resolves without the user passing
// --c-include. Used only as a last-resort resolution fallback in CompileImportedFile; the parse
// itself relies on clang's in-process MSVC toolchain auto-detection. Scanned once and cached.
static const std::vector<std::string>& WindowsSdkIncludeDirs()
{
    static const std::vector<std::string> dirs = [] {
        std::vector<std::string> out;
        const std::string incRoot = "C:\\Program Files (x86)\\Windows Kits\\10\\Include";
        std::error_code ec;
        std::string latest;
        for (auto it = std::filesystem::directory_iterator(incRoot, ec);
             !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
        {
            if (!it->is_directory(ec)) continue;
            auto name = it->path().filename().string();
            if (name > latest && std::filesystem::exists(it->path() / "um", ec))
                latest = name;
        }
        if (!latest.empty())
        {
            std::filesystem::path base = std::filesystem::path(incRoot) / latest;
            for (const char* sub : { "um", "shared", "ucrt", "winrt" })
            {
                auto p = base / sub;
                if (std::filesystem::exists(p, ec)) out.push_back(p.string());
            }
        }
        return out;
    }();
    return dirs;
}

bool LLVMBackend::CompileImportedFile(const std::string& importingFilePath, const std::string& importFilename, const std::string& namespaceName, const std::string& programAlias, const std::vector<std::string>& explicitLibs, const std::vector<std::string>& extraDefines, bool cacheHeader)
{
    auto importingDir = std::filesystem::path(importingFilePath).parent_path();
    auto importPath = (importingDir / importFilename).lexically_normal();

    std::error_code ec;
    auto canonical = std::filesystem::canonical(importPath, ec);
    // For LSP analysis: the "importing" file is a temp file; try the real source directory first.
    if (ec && !sourceFileDir_.empty())
    {
        auto sourcePath = (std::filesystem::path(sourceFileDir_) / importFilename).lexically_normal();
        canonical = std::filesystem::canonical(sourcePath, ec);
    }
    if (ec && !importSearchDir.empty())
    {
        auto searchPath = (std::filesystem::path(importSearchDir) / importFilename).lexically_normal();
        canonical = std::filesystem::canonical(searchPath, ec);
    }
    // C library headers (e.g. "curl/curl.h") live under the --c-include roots.
    for (const auto& inc : cIncludeDirs_)
    {
        if (!ec) break;
        auto incPath = (std::filesystem::path(inc) / importFilename).lexically_normal();
        canonical = std::filesystem::canonical(incPath, ec);
    }
    // Implicit fallback: look in the "core" directory beside the runtime.
    if (ec && !runtimeDir.empty())
    {
        auto corePath = (std::filesystem::path(runtimeDir) / "core" / importFilename).lexically_normal();
        canonical = std::filesystem::canonical(corePath, ec);
    }
    // System headers (e.g. windows.h) live in the Windows SDK include dirs. Fall back to the
    // detected SDK include dirs so `import "windows.h"` resolves with no --c-include flag; the
    // header's own dir then becomes an in-scope root automatically (see ExtractCHeaderClang).
    if (ec)
    {
        for (const auto& inc : WindowsSdkIncludeDirs())
        {
            if (!ec) break;
            auto incPath = (std::filesystem::path(inc) / importFilename).lexically_normal();
            canonical = std::filesystem::canonical(incPath, ec);
        }
    }
    if (ec)
    {
        std::cerr << "Error: imported file not found: " << importFilename
                  << " (searched relative to '" << importingDir.string() << "'"
                  << (sourceFileDir_.empty() ? "" : ", source dir '" + sourceFileDir_ + "'")
                  << (importSearchDir.empty() ? "" : ", import dir '" + importSearchDir + "'")
                  << (cIncludeDirs_.empty() ? "" : ", " + std::to_string(cIncludeDirs_.size()) + " --c-include dir(s)")
                  << (runtimeDir.empty() ? "" : ", runtime core '" + runtimeDir + "/core'")
                  << ").\n";
        return false;
    }
    auto canonicalStr = canonical.string();
    // Check circular import first (file currently being processed).
    auto cycleIt = std::find(importStack.begin(), importStack.end(), canonicalStr);
    if (cycleIt != importStack.end())
    {
        std::string chain;
        for (auto it = cycleIt; it != importStack.end(); ++it)
            chain += std::filesystem::path(*it).filename().string() + " -> ";
        chain += std::filesystem::path(canonicalStr).filename().string();
        LogError(std::format("Circular import detected: {}", chain));
        return false;
    }
    // Check dedup (file already fully processed or eagerly registered).
    if (importedFiles.count(canonicalStr))
    {
        if (verbose) std::cout << "[verbose]   already imported: " << canonicalStr << "\n";
        return true;
    }
    // Cross-path dedup: the same filename may resolve to different canonical paths
    // depending on which search directory finds it first (e.g. runtimeDir/core vs -i dir).
    // Check all alternate search directories so duplicates are caught across paths.
    {
        auto tryAlt = [&](const std::filesystem::path& dir) -> std::string {
            std::error_code altEc;
            auto alt = std::filesystem::canonical((dir / importFilename).lexically_normal(), altEc);
            return altEc ? "" : alt.string();
        };
        std::string altPaths[] = {
            importSearchDir.empty() ? "" : tryAlt(importSearchDir),
            runtimeDir.empty()      ? "" : tryAlt(std::filesystem::path(runtimeDir) / "core"),
        };
        for (auto& alt : altPaths)
        {
            if (!alt.empty() && alt != canonicalStr && importedFiles.count(alt))
            {
                if (verbose) std::cout << "[verbose]   already imported (alt path): " << canonicalStr << "\n";
                importedFiles.insert(canonicalStr);
                return true;
            }
        }
    }
    // Register eagerly so any re-entrant or duplicate import is caught above.
    importedFiles.insert(canonicalStr);
    // Real C source: hand off to clang-cl rather than the CFlat parser. The compiled
    // object is linked by EmitExecutable; the importing .cb supplies the declarations.
    {
        auto ext = std::filesystem::path(canonicalStr).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (ext == ".c")
            return CompileCFile(canonicalStr, programAlias);
        // A C header (real C, not CFlat): extract declarations + enums via clang's
        // AST dump; the prebuilt library is linked via --c-lib in EmitExecutable.
        if (ext == ".h" || ext == ".hpp" || ext == ".hh")
        {
            // Inline `lib "..."` (or `lib { "a", "b" }`) clause: resolve each and add it to
            // the link line, alongside any --c-lib libraries. A library that ships beside the
            // .cb is used by path; a bare system lib name (user32.lib, ...) is passed through
            // for lld-link to find on the Windows SDK lib path cflat discovered for the header.
            for (const auto& explicitLib : explicitLibs)
            {
                if (explicitLib.empty()) continue;
                cLinkLibs_.push_back(ResolveCLinkLib(explicitLib, importingFilePath));
            }
            bool ok = CompileCHeader(canonicalStr, extraDefines, cacheHeader);
            // Translate any queued function-like-macro source into generic templates so
            // they are visible to the importing file's ForwardRefScanner.
            if (ok) ProcessPendingMacroSources();
            return ok;
        }
    }
    importStack.push_back(canonicalStr);
    struct ImportGuard {
        std::vector<std::string>& stack;
        ~ImportGuard() { stack.pop_back(); }
    } importGuard{importStack};

    if (verbose) std::cout << "[verbose] importing: " << canonicalStr << "\n";
    llvm::TimeTraceScope importScope("ImportFile", importFilename);

    // Implicit core-library imports (files under runtimeDir/core) have stable content for
    // the process, so their parse trees are cached and reused across compiles and LSP
    // re-analyses. User imports are parsed fresh and anchored for this compile only.
    bool isCoreImport = false;
    if (!runtimeDir.empty())
    {
        std::error_code cec;
        auto coreDir = std::filesystem::canonical(std::filesystem::path(runtimeDir) / "core", cec);
        if (!cec)
        {
            auto coreStr = coreDir.string();
            isCoreImport = canonicalStr.size() > coreStr.size()
                && canonicalStr.compare(0, coreStr.size(), coreStr) == 0;
        }
    }

    // Get-or-parse the tree (cached for core, freshly parsed otherwise). Generic-template
    // ctx pointers point into this tree; its owner (parseTreeCache_ for core,
    // importedParseStates for user files) keeps it alive long enough to stay valid.
    CachedParseTree* tree = GetOrParseFile(canonicalStr, importFilename, isCoreImport);
    if (!tree)
        return false;

    auto* computeUnit = tree->unit;
    auto* parserPtr   = tree->parser.get();
    auto* tokensPtr   = tree->tokens.get();

    // Recursively process nested imports before scanning this file's declarations
    if (auto* tu = computeUnit->translationUnit())
    {
        for (auto* decl : tu->externalDeclaration())
        {
            if (auto* imp = decl->importDeclaration())
            {
                // Grouped import `import { "a", "b" };` - expand to plain nested imports.
                auto groupedImports = DequoteImportGroup(imp);
                if (groupedImports.size() > 1)
                {
                    for (const auto& gf : groupedImports)
                    {
                        if (verbose) std::cout << "[verbose]   nested import (group): " << gf << "\n";
                        if (!CompileImportedFile(canonicalStr, gf))
                            return false;
                    }
                    continue;
                }
                std::string nested;
                if (!groupedImports.empty())
                    nested = groupedImports[0];
                else
                {
                    std::string raw = imp->StringLiteral()->getText();
                    nested = raw.substr(1, raw.size() - 2);
                }
                if (IsPackageVcpkgImport(imp))
                {
                    std::string portSpec = DequoteFromClause(imp);
                    if (verbose) std::cout << "[verbose]   nested vcpkg import: " << nested << " from " << portSpec << "\n";
                    if (!CompileVcpkgImport(canonicalStr, nested, portSpec))
                        return false;
                    continue;
                }
                std::string nestedNs = imp->Identifier() ? imp->Identifier()->getText() : "";
                std::vector<std::string> nestedLibs = DequoteLibClauses(imp);
                std::vector<std::string> nestedDefines = DequoteDefineClauses(imp);
                if (verbose) std::cout << "[verbose]   nested import: " << nested << (nestedNs.empty() ? "" : " as " + nestedNs) << "\n";
                if (!CompileImportedFile(canonicalStr, nested, nestedNs, "", nestedLibs, nestedDefines))
                    return false;
            }
        }
    }

    // Snapshot tables after nested imports but before this file's scan+codegen.
    // The diff after codegen identifies symbols this file (and only this file) contributed.
    // Track functions by unique mangled name so that a new overload of an existing call-name
    // (e.g. "add") is still detected as new even if a different overload already existed.
    std::unordered_set<std::string> funcUniqBefore, structsBefore, ifacesBefore, nsBefore;
    if (!namespaceName.empty())
    {
        for (auto& [_, syms] : functionTable) for (auto& s : syms) funcUniqBefore.insert(s.UniqueName);
        for (auto& [n, _] : dataStructures) structsBefore.insert(n);
        for (auto& [n, _] : interfaceTable) ifacesBefore.insert(n);
        nsBefore = namespaceTable;
    }

    auto savedSourceFileName = sourceFileName;
    auto savedSourceFilePath = currentSourceFilePath_;
    sourceFileName = std::filesystem::path(canonicalStr).filename().string();
    currentSourceFilePath_ = canonicalStr;

    // Forward-ref scan the imported file
    {
        llvm::TimeTraceScope scanScope("ForwardRefScan", importFilename);
        if (verbose) std::cout << "[verbose]   forward-ref scan: " << importFilename << "\n";
        ForwardRefScanner scanner(this);
        scanner.SetTokens(tokensPtr);
        // Pre-declare opaque types for all generic instantiations found in the file
        // (including inside function/program bodies) before scanning declarations.
        // Without this pass, program definitions that pre-declare run(Name*, list__string)
        // fail because the opaque shell for list__string hasn't been created yet.
        if (auto* tu = computeUnit->translationUnit())
            for (auto* decl : tu->externalDeclaration())
                scanner.ScanGenericTypeUses(decl);
        if (auto* tu = computeUnit->translationUnit())
            for (auto* decl : tu->externalDeclaration())
                scanner.ScanExternalDeclaration(decl);
    }

    // Code-gen walk the imported file
    if (verbose) std::cout << "[verbose]   code-gen walk: " << importFilename << "\n";
    {
        llvm::TimeTraceScope codegenScope("CodeGeneration", importFilename);
        auto myListener = std::make_unique<MainListener>(parserPtr, this, sourceFileName);
        antlr4::tree::ParseTreeWalker().walk(myListener.get(), computeUnit);
    }
    if (verbose) std::cout << "[verbose]   import done: " << importFilename << "\n";

    // Register the file-scoped import alias.  The "$global$:<alias>" sentinel tells
    // ResolveQualifiedName and ParsePostfixExpression to resolve Alias.X -> X only
    // when X was contributed by this file (membership-checked via importAliasMembers).
    if (!namespaceName.empty())
    {
        std::unordered_set<std::string> members;
        for (auto& [callName, syms] : functionTable)
            for (auto& s : syms)
                if (!funcUniqBefore.count(s.UniqueName)) { members.insert(callName); break; }
        for (auto& [n, _] : dataStructures) if (!structsBefore.count(n)) members.insert(n);
        for (auto& [n, _] : interfaceTable) if (!ifacesBefore.count(n)) members.insert(n);
        for (auto& n : namespaceTable)      if (!nsBefore.count(n))      members.insert(n);
        importAliasMembers[namespaceName] = std::move(members);
        RegisterNamespaceAlias(namespaceName, "$global$:" + namespaceName);
    }

    sourceFileName = savedSourceFileName;
    currentSourceFilePath_ = savedSourceFilePath;
    return true;
}

// Drain pendingMacroSources_ (filled by RegisterCFunctionMacros during a C header import)
// and feed each buffer through the lexer/parser/ForwardRefScanner pipeline so the auto
// generic functions land in genericFunctionTemplates before the importing file's own
// ForwardRefScanner runs. Parse state is kept alive in syntheticParseStates_ because
// template ctx pointers outlive this function.
void LLVMBackend::ProcessPendingMacroSources()
{
    if (pendingMacroSources_.empty()) return;
    std::vector<PendingMacroSource> drain;
    drain.swap(pendingMacroSources_);

    for (auto& p : drain)
    {
        SyntheticParseState state;
        state.label  = p.label;
        state.input  = std::make_unique<antlr4::ANTLRInputStream>(p.source);
        state.lexer  = std::make_unique<CFlatLexer>(state.input.get());
        state.tokens = std::make_unique<antlr4::CommonTokenStream>(state.lexer.get());
        state.parser = std::make_unique<CFlatParser>(state.tokens.get());

        // Suppress lexer/parser console spam. Translation errors here indicate a bug in
        // our generator (since the source we synthesized is malformed); user-facing
        // diagnostics for that would be confusing - drop the batch and log under -v.
        state.lexer->removeErrorListeners();
        state.parser->removeErrorListeners();

        try { state.tokens->fill(); } catch (...) {
            if (verbose) std::cout << "[verbose]   lex failed for " << state.label << ", dropping batch\n";
            continue;
        }
        CFlatParser::CompilationUnitContext* cu = nullptr;
        try { cu = state.parser->compilationUnit(); } catch (...) {
            if (verbose) std::cout << "[verbose]   parse failed for " << state.label << ", dropping batch\n";
            continue;
        }
        if (!cu) continue;
        auto* tu = cu->translationUnit();
        if (!tu) { syntheticParseStates_.push_back(std::move(state)); continue; }

        // ForwardRefScanner::ScanFunctionDefinition explicitly skips generic templates
        // (they get registered by MainListener's ParseFunctionDefinition at code-gen time
        // when the importing file is walked). Our synthesized source is never walked by
        // MainListener, so register the templates inline here from the AST.
        size_t registered = 0;
        for (auto* decl : tu->externalDeclaration())
        {
            auto* func = decl->functionDefinition();
            if (!func) continue;
            auto* gtps = func->genericTypeParameters();
            if (!gtps) continue;
            std::string name = ::getFunctionName(func);
            std::vector<std::string> typeParams;
            for (auto* entry : gtps->typeParameterList()->typeParameterEntry())
                typeParams.push_back(entry->getText());
            gts.genericFunctionTemplates[name] = func;
            gts.genericFunctionTypeParams[name] = typeParams;
            ++registered;
        }
        if (verbose)
            std::cout << "[verbose]   registered " << registered << " C function-like macro template(s) from " << state.label << "\n";
        // Keep parse state alive for the rest of the compilation.
        syntheticParseStates_.push_back(std::move(state));
    }
}

void LLVMBackend::RunBaselinePasses()
{
    llvm::PassBuilder PB;
    llvm::LoopAnalysisManager LAM;
    llvm::FunctionAnalysisManager FAM;
    llvm::CGSCCAnalysisManager CGAM;
    llvm::ModuleAnalysisManager MAM;

    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

    llvm::FunctionPassManager FPM;
    FPM.addPass(llvm::SROAPass(llvm::SROAOptions::ModifyCFG));
    FPM.addPass(llvm::PromotePass());
    FPM.addPass(llvm::InstCombinePass());
    FPM.addPass(llvm::SimplifyCFGPass());

    llvm::ModulePassManager MPM;
    MPM.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(FPM)));
    MPM.run(*module, MAM);
}

namespace {

// One captured loop-vectorize analysis remark: LLVM's stable remark name (the
// reliable key) plus its human message (fallback / passthrough).
struct VectorizeRemark { std::string name; std::string msg; };

// Captures loop-vectorize analysis remarks so the `vectorize` keyword's failure
// diagnostics can name a specific reason. The pass/fail decision itself is made by
// scanning the optimized IR (see OptimizeModule); this only enriches the message.
struct VectorizeDiagnosticHandler : public llvm::DiagnosticHandler
{
    std::map<std::string, VectorizeRemark>& remarkByFn;

    explicit VectorizeDiagnosticHandler(std::map<std::string, VectorizeRemark>& remarks)
        : remarkByFn(remarks) {}

    // Enable loop-vectorize remark categories so they reach handleDiagnostics.
    static bool isLoopVectorize(llvm::StringRef passName) { return passName == "loop-vectorize"; }
    bool isAnalysisRemarkEnabled(llvm::StringRef passName) const override { return isLoopVectorize(passName); }
    bool isMissedOptRemarkEnabled(llvm::StringRef passName) const override { return isLoopVectorize(passName); }
    bool isPassedOptRemarkEnabled(llvm::StringRef passName) const override { return isLoopVectorize(passName); }

    bool handleDiagnostics(const llvm::DiagnosticInfo& di) override
    {
        if (const auto* opt = llvm::dyn_cast<llvm::DiagnosticInfoOptimizationBase>(&di))
        {
            // The loop vectorizer's analysis remarks reliably carry a
            // "loop not vectorized: <reason>" message, but the pass name is
            // sometimes empty (vs "loop-vectorize"), so key on the message
            // signature rather than the pass name. Correlated by function; only
            // consulted when the IR scan has already flagged a failed loop there.
            if (di.getKind() == llvm::DK_OptimizationRemarkAnalysis &&
                opt->getMsg().rfind("loop not vectorized:", 0) == 0)
            {
                remarkByFn[opt->getFunction().getName().str()] =
                    { opt->getRemarkName().str(), opt->getMsg() };  // latest for this fn
            }
            return true;  // consumed; do not let the default handler print it
        }
        return false;  // let the default handler deal with anything else
    }
};

// Compose the most precise, well-located reason for a failed `vectorize` loop from
// (a) LLVM's analysis remark - keyed on its stable remark NAME, with the message as
// fallback - and (b) the CFlat-side AST facts gathered at codegen. Updates outLine/
// outCol to point at the specific offending construct (the call, the condition)
// when known, otherwise at the loop itself.
std::string ComposeVectorizeFailure(const LLVMBackend::VectorizeLoopInfo* info,
                                    const VectorizeRemark* remark,
                                    int& outLine, int& outCol)
{
    std::string rn = remark ? remark->name : std::string();
    std::string rm = remark ? remark->msg  : std::string();
    auto nameIs = [&](std::initializer_list<const char*> names) {
        for (auto* n : names) if (rn == n) return true; return false;
    };
    auto msgHas = [&](const char* s) { return !rm.empty() && rm.find(s) != std::string::npos; };

    // 1) Non-inlinable call in the body.
    if (nameIs({ "CantVectorizeLibcall", "CantVectorizeCall" }) || msgHas("call instruction"))
    {
        if (info && info->hasCall)
        {
            outLine = info->callLine; outCol = info->callCol;
            return "the loop body calls '" + info->callName + "', which was not inlined; "
                   "only inlinable or intrinsic calls can be vectorized";
        }
        return "the loop body contains a call that was not inlined; "
               "only inlinable or intrinsic calls can be vectorized";
    }

    // 2) No countable trip count - either LLVM said so, or it is a `while` whose
    //    condition is a sentinel comparison (==/!=) rather than a counted bound.
    if (nameIs({ "CantComputeNumberOfIterations" }) || msgHas("could not determine number of loop iterations")
        || (info && info->isWhile && !info->conditionCounted))
    {
        if (info && info->isWhile && info->condLine)
        {
            outLine = info->condLine; outCol = info->condCol;
            return "the loop has no countable trip count; the condition '" + info->condText +
                   "' is not a counted comparison (use a counted bound like 'i < n')";
        }
        return "the loop has no countable trip count; use a counted 'for' or 'while'";
    }

    // 3) Loop-carried dependence.
    if (nameIs({ "CantReorderMemOps" }) || msgHas("unsafe dependent memory") || msgHas("safe to reorder memory"))
        return "a loop-carried dependence prevents vectorization "
               "(an iteration depends on a value an earlier iteration wrote, e.g. a[k] = a[k-1] + 1)";

    // 4) Cross-iteration value that is not a recognized reduction.
    if (nameIs({ "NonReductionValueUsedOutsideLoop" }) || msgHas("could not be identified as reduction"))
        return "a cross-iteration value here is not a recognized reduction "
               "(only integer reductions vectorize; floating-point reductions are not supported)";

    // 5) Control flow the vectorizer cannot handle.
    if (nameIs({ "LoopContainsSwitch" }) || msgHas("switch statement"))
        return "the loop body contains a switch statement, which cannot be vectorized";
    if (msgHas("control flow is not understood"))
        return "the loop has control flow the vectorizer cannot handle (e.g. a data-dependent break)";

    // 6) Fallbacks. Prefer LLVM's own wording (minus its prefix) when present.
    if (!rm.empty())
    {
        const std::string prefix = "loop not vectorized: ";
        return rm.rfind(prefix, 0) == 0 ? rm.substr(prefix.size()) : rm;
    }
    // No LLVM remark at all (some loop shapes emit only the generic transform
    // warning) - fall back to the best structural guess from the AST facts.
    if (info && info->hasCall)
    {
        outLine = info->callLine; outCol = info->callCol;
        return "the loop body calls '" + info->callName + "', which may not be vectorizable; "
               "only inlinable or intrinsic calls can be vectorized";
    }
    if (info && !info->isWhile)
        return "the loop could not be vectorized "
               "(most often a loop-carried dependence, e.g. a[k] = a[k-1] + ...)";
    return "the loop could not be vectorized "
           "(needs a countable trip count, no carried dependence, and only inlinable calls)";
}

// The loop DID vectorize but LLVM had to guard the vector path with a runtime overlap test
// (vector.memcheck) because it could not prove the pointers disjoint. `vectorize` promises a
// clean unconditional vector loop, so this is a failure. When a span accessor is the cause,
// point at it and prescribe the local-`T[]` fix; otherwise give the general disjointness advice.
std::string ComposeMemcheckFailure(const LLVMBackend::VectorizeLoopInfo* info, int& outLine, int& outCol)
{
    if (info && info->hasSpanAccessor)
    {
        outLine = info->spanLine; outCol = info->spanCol;
        std::string recv = info->spanReceiver.empty() ? std::string("the span") : ("'" + info->spanReceiver + "'");
        std::string data = info->spanReceiver.empty() ? std::string("s") : info->spanReceiver;
        std::string acc = info->spanAccessor == "operator[]" ? std::string("operator[]") : (info->spanAccessor + "()");
        return "a runtime alias check remained because indexing " + recv + " through " + acc +
               " routes through the method's `this`, so the optimizer cannot prove two spans disjoint; "
               "bind a local view once - `T[] v = " + data + ".data();` - and index `v`";
    }
    return "a runtime alias check remained: LLVM could not prove the pointers disjoint; "
           "pass the buffers as `span<T>`/`T[]` (noalias) or otherwise establish disjointness";
}

} // namespace

void LLVMBackend::OptimizeModule(int optimizationLevel)
{
    // AddressSanitizer must instrument even at -O0 (its whole point is to catch bugs in
    // unoptimized debug builds). Run the module-level AddressSanitizerPass in its own pass
    // manager before the optimization pipeline so the __asan_* instrumentation and the
    // asan.module_ctor are in place regardless of the -O level. Gated on asan_ so non-asan
    // builds never touch this and stay byte-for-byte unchanged.
    if (asan_)
    {
        // Force the dynamic shadow. We link the dynamic asan runtime
        // (clang_rt.asan_dynamic), which maps its shadow region at a base chosen
        // at runtime and published in __asan_shadow_memory_dynamic_address. By
        // default the pass bakes in a STATIC Windows shadow offset
        // (kWindowsShadowOffset64 = 0x100000000000); the instrumented shadow load
        // then targets (addr>>3)+0x100000000000, which is not where the dynamic
        // runtime actually placed shadow -> the shadow byte read faults and every
        // check surfaces as a raw access-violation instead of a clean asan report.
        // Forcing the dynamic shadow makes the pass emit a load of
        // __asan_shadow_memory_dynamic_address, matching the linked runtime. (This
        // is what clang-cl does for -fsanitize=address on Windows.)
        {
            auto& regOpts = llvm::cl::getRegisteredOptions();
            auto it = regOpts.find("asan-force-dynamic-shadow");
            if (it != regOpts.end())
            {
                if (auto* flag = static_cast<llvm::cl::opt<bool>*>(it->second))
                    flag->setValue(true);
            }
        }

        auto asanTM = CreateOptTargetMachine();
        if (asanTM)
            module->setDataLayout(asanTM->createDataLayout());

        llvm::PassBuilder PB(asanTM.get());
        llvm::LoopAnalysisManager LAM;
        llvm::FunctionAnalysisManager FAM;
        llvm::CGSCCAnalysisManager CGAM;
        llvm::ModuleAnalysisManager MAM;
        PB.registerModuleAnalyses(MAM);
        PB.registerCGSCCAnalyses(CGAM);
        PB.registerFunctionAnalyses(FAM);
        PB.registerLoopAnalyses(LAM);
        PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

        // Defaults match clang's plain `-fsanitize=address`: stack use-after-scope on,
        // runtime detection of use-after-return, version check left enabled (it pins the
        // instrumentation to the compiler-rt runtime ABI - __asan_version_mismatch_check_v8
        // for this LLVM 18 toolchain, which the linked runtime must export).
        llvm::AddressSanitizerOptions asanOpts;
        asanOpts.UseAfterScope = true;

        // The AddressSanitizer pass only instruments memory accesses in functions that carry
        // the SanitizeAddress attribute - clang stamps it on every function it compiles under
        // -fsanitize=address. cflat does not emit it during codegen, so add it here to every
        // defined function (the module pass still handles globals + the ctor without it). The
        // asan runtime's own helpers are declarations (skipped). Without this, only globals
        // get redzones and no __asan_load*/__asan_store* checks are emitted.
        for (llvm::Function& F : *module)
        {
            if (F.isDeclaration())
                continue;
            F.addFnAttr(llvm::Attribute::SanitizeAddress);
        }

        // Exclude cflat's globals from ASan global instrumentation. On COFF the
        // global-GC path (InstrumentGlobalsCOFF, used by default and required for
        // the dynamic-runtime shadow to work) wraps each instrumented global in a
        // redzone struct and emits its metadata in a COMDAT associative to the
        // instrumented global's symbol. cflat's private internal globals (e.g.
        // __string_empty) don't survive that path - the COFF object writer aborts
        // with "Associative COMDAT symbol '__string_empty.<hash>' does not exist".
        // Marking every global NoAddress makes ASan skip global instrumentation
        // (no redzones, no associative COMDATs) while leaving heap/stack/function
        // instrumentation - what we need for use-after-free - fully intact. We
        // keep UseGlobalGC at its default (true): flipping it to false breaks the
        // Windows dynamic-runtime shadow, turning every instrumented access into a
        // raw shadow-load access-violation instead of a clean asan report. The
        // only cost here is no global-buffer-overflow detection.
        for (llvm::GlobalVariable& G : module->globals())
        {
            llvm::GlobalValue::SanitizerMetadata md =
                G.hasSanitizerMetadata() ? G.getSanitizerMetadata()
                                         : llvm::GlobalValue::SanitizerMetadata();
            md.NoAddress = true;
            G.setSanitizerMetadata(md);
        }

        llvm::ModulePassManager MPM;
        MPM.addPass(llvm::AddressSanitizerPass(asanOpts));
        MPM.run(*module, MAM);
    }

    if (optimizationLevel == 0)
        return;

    // The loop vectorizer only runs at -O2, so `vectorize` is enforced only there.
    // Below -O2 it is a no-op (the metadata is harmless and simply ignored).
    bool enforceVectorize = (optimizationLevel >= 2) && !vectorizeLoops_.empty();
    std::map<std::string, VectorizeRemark> remarkByFn;
    if (enforceVectorize)
    {
        context->setDiagnosticHandler(
            std::make_unique<VectorizeDiagnosticHandler>(remarkByFn),
            /*RespectFilters=*/false);
    }

    // Build the pipeline WITH a TargetMachine so TargetTransformInfo is available;
    // the loop vectorizer needs it to cost vector instructions (without it, it runs
    // target-agnostic and declines to vectorize). Also pin the module's data layout
    // to the TM so vector type sizing is correct.
    auto TM = CreateOptTargetMachine();
    if (TM)
        module->setDataLayout(TM->createDataLayout());

    llvm::PipelineTuningOptions pto;
    pto.LoopVectorization = true;
    pto.SLPVectorization = true;

    llvm::PassBuilder PB(TM.get(), pto);
    llvm::LoopAnalysisManager LAM;
    llvm::FunctionAnalysisManager FAM;
    llvm::CGSCCAnalysisManager CGAM;
    llvm::ModuleAnalysisManager MAM;

    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

    llvm::ModulePassManager MPM;
    if (optimizationLevel == 1)
        MPM = PB.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O1);
    else if (optimizationLevel == 2)
        MPM = PB.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);

    MPM.run(*module, MAM);

    if (enforceVectorize)
    {
        // Restore a plain handler so later diagnostics on this context are not
        // swallowed (the context persists across compiles in batch/LSP).
        context->setDiagnosticHandler(std::make_unique<llvm::DiagnosticHandler>());

        // Decide pass/fail by scanning the optimized IR, the same signal LLVM's
        // own WarnMissedTransformations uses: a loop still carrying our
        // `cflat.vectorize.line` metadata WITHOUT `llvm.loop.isvectorized` was
        // seen by the vectorizer and left scalar -> the contract failed. A loop
        // that vectorized carries `isvectorized`; one that was eliminated carries
        // no metadata at all (vacuously fine). The reason text, when the analysis
        // remark reached our handler, is best-effort.
        auto loopEntry = [](llvm::MDNode* loopID, llvm::StringRef key) -> llvm::MDNode* {
            for (unsigned i = 1; i < loopID->getNumOperands(); ++i)
            {
                auto* entry = llvm::dyn_cast_or_null<llvm::MDNode>(loopID->getOperand(i));
                if (!entry || entry->getNumOperands() == 0) continue;
                auto* s = llvm::dyn_cast<llvm::MDString>(entry->getOperand(0));
                if (s && s->getString() == key) return entry;
            }
            return nullptr;
        };

        for (auto& F : *module)
        {
            if (F.isDeclaration()) continue;
            std::optional<llvm::DominatorTree> domTree;  // built lazily for this function
            for (auto& BB : F)
            {
                auto* term = BB.getTerminator();
                if (!term) continue;
                auto* loopID = term->getMetadata(llvm::LLVMContext::MD_loop);
                if (!loopID) continue;

                auto* lineEntry = loopEntry(loopID, "cflat.vectorize.line");
                if (!lineEntry) continue;                              // not a vectorize loop

                // A loop carrying our hint that vectorized gains `llvm.loop.isvectorized`. That is
                // necessary but not sufficient: if LLVM had to GUARD the vector path with a runtime
                // overlap test (loop versioning), the loop did not vectorize cleanly. The `vectorize`
                // contract is the clean, unconditional vector loop (Detection B) - so a surviving
                // runtime check is a failure even though the loop technically vectorized. Detect the
                // guard by dominance: the vector body (this latch) is reached only when the memcheck
                // passes, so a `vector.memcheck` block dominates it; a clean vector loop has none.
                bool memcheckGuarded = false;
                if (loopEntry(loopID, "llvm.loop.isvectorized"))
                {
                    if (!domTree) domTree.emplace(F);
                    for (auto* d = domTree->getNode(&BB); d; d = d->getIDom())
                        if (d->getBlock()->getName().contains("memcheck")) { memcheckGuarded = true; break; }
                    if (!memcheckGuarded) continue;                    // clean vector loop: contract satisfied
                }

                int line = 0;
                if (lineEntry->getNumOperands() >= 2)
                    if (auto* c = llvm::mdconst::dyn_extract_or_null<llvm::ConstantInt>(lineEntry->getOperand(1)))
                        line = static_cast<int>(c->getSExtValue());

                // Match the loop to the AST facts gathered at codegen (by line) and to LLVM's
                // analysis remark (by function), then compose the most precise, located message.
                const VectorizeLoopInfo* info = nullptr;
                for (const auto& vi : vectorizeLoops_)
                    if (vi.line == line) { info = &vi; break; }

                int outLine = line, outCol = info ? info->col : 0;
                std::string message;
                if (memcheckGuarded)
                {
                    std::string reason = ComposeMemcheckFailure(info, outLine, outCol);
                    message = "vectorize loop did not vectorize cleanly: " + reason;
                }
                else
                {
                    const VectorizeRemark* remark = nullptr;
                    auto r = remarkByFn.find(F.getName().str());
                    if (r != remarkByFn.end()) remark = &r->second;
                    std::string reason = ComposeVectorizeFailure(info, remark, outLine, outCol);
                    message = "vectorize loop could not be vectorized: " + reason;
                }

                currentLine = outLine;
                currentColumn = outCol;
                LogError(message);
                // LogError does not return for a normal compile (it exits / throws).
            }
        }
    }
}

bool LLVMBackend::Analyze(const std::string& filePath,
                              const std::string& importDir,
                              const std::string& runtimeDirPath)
{
    gts.Clear();

    // Diagnostics should name the real document, not the temp copy the LSP hands us.
    // When the caller supplied a display name, use it; otherwise fall back to the
    // analyzed file's own name (e.g. direct CLI --check on a real path).
    sourceFileName = sourceDisplayName_.empty()
        ? std::filesystem::path(filePath).filename().string()
        : sourceDisplayName_;
    auto rootCanonical = std::filesystem::weakly_canonical(filePath).string();
    currentSourceFilePath_ = rootCanonical;
    importedFiles.insert(rootCanonical);
    importStack.push_back(rootCanonical);
    struct RootGuard {
        std::vector<std::string>& stack;
        ~RootGuard() { stack.pop_back(); }
    } rootGuard{importStack};
    importSearchDir = importDir;
    runtimeDir = runtimeDirPath;
    // verbose retains the value set by the LSP via SetVerbose() - left unmodified.
    bool debugInfo = false;

    platformValue = 64;

    // Pre-populate compile-time macros
    {
        auto* fileGlobalStr = module->getOrInsertGlobal("__FILE__",
            llvm::ArrayType::get(llvm::Type::getInt8Ty(*context), sourceFileName.size() + 1));
        auto* fileConst = llvm::ConstantDataArray::getString(*context, sourceFileName, true);
        if (auto* gv = llvm::dyn_cast<llvm::GlobalVariable>(fileGlobalStr))
        {
            gv->setInitializer(fileConst);
            gv->setConstant(true);
            SetCompileTimeMacro("__FILE__", gv, "string");
        }
        auto platformConst = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), platformValue);
        SetCompileTimeMacro("__PLATFORM__", platformConst, "int");
        auto win64Const = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), platformValue == 64 ? 1 : 0);
        SetCompileTimeMacro("__WIN64__", win64Const, "int");
        auto win32Const = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), platformValue == 32 ? 1 : 0);
        SetCompileTimeMacro("__WIN32__", win32Const, "int");
        auto windowsConst = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 1);
        SetCompileTimeMacro("__WINDOWS__", windowsConst, "int");
        // Target architecture is always x86/Intel today (win64 -> x86_64, win32 -> i686).
        // Exposed so callers can guard architecture-specific intrinsics like __rdtscp().
        auto x86Const = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 1);
        SetCompileTimeMacro("__X86__", x86Const, "int");
    }

    if (!runtimeDir.empty())
    if (!skipRuntimeImport)
    {
        auto runtimePath = std::filesystem::path(runtimeDir) / "core" / "runtime.cb";
        if (std::filesystem::exists(runtimePath))
            CompileImportedFile(runtimePath.string(), "runtime.cb");
    }

    if (!std::filesystem::exists(filePath))
    {
        std::cerr << "Error: input file '" << filePath << "' does not exist.\n";
        return false;
    }

    try
    {
        std::ifstream stream;
        stream.open(filePath);
        auto analyzeSourceLines = ReadFileToLines(stream);

        antlr4::ANTLRInputStream input(stream);
        CFlatLexer lexer(&input);
        antlr4::CommonTokenStream tokens(&lexer);
        CFlatParser parser(&tokens);

        // Use the display name (real document) for parse diagnostics, not the temp copy.
        CFlatErrorListener analyzeErrorListener(sourceFileName, analyzeSourceLines);
        lexer.removeErrorListeners();
        lexer.addErrorListener(&analyzeErrorListener);
        parser.removeErrorListeners();
        parser.addErrorListener(&analyzeErrorListener);

        tokens.fill();

        auto computeUnit = parser.compilationUnit();

        if (analyzeErrorListener.hasErrors())
        {
            ReportParseErrors(analyzeErrorListener.getDiagnostics(), analyzeSourceLines);
            return false;
        }

        // Process top-level imports before scanning
        if (auto* tu = computeUnit->translationUnit()) {
            for (auto* decl : tu->externalDeclaration()) {
                if (auto* imp = decl->importDeclaration()) {
                    // Grouped import `import { "a", "b" };` - expand to plain imports.
                    auto groupedImports = DequoteImportGroup(imp);
                    if (groupedImports.size() > 1)
                    {
                        for (const auto& gf : groupedImports)
                            if (!CompileImportedFile(filePath, gf))
                                return false;
                        continue;
                    }
                    std::string importFilename;
                    if (!groupedImports.empty())
                        importFilename = groupedImports[0];
                    else
                    {
                        std::string raw = imp->StringLiteral()->getText();
                        importFilename = raw.substr(1, raw.size() - 2);
                    }
                    if (IsPackageVcpkgImport(imp))
                    {
                        std::string portSpec = DequoteFromClause(imp);
                        if (!CompileVcpkgImport(RootVcpkgImportPath(filePath), importFilename, portSpec))
                            return false;
                        continue;
                    }
                    std::string ns = imp->Identifier() ? imp->Identifier()->getText() : "";
                    bool isProgram = imp->children.size() >= 2 && imp->children[1]->getText() == "program";
                    std::string alias = isProgram && imp->Identifier() ? imp->Identifier()->getText() : "";
                    std::string impExt = std::filesystem::path(importFilename).extension().string();
                    std::transform(impExt.begin(), impExt.end(), impExt.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                    bool isCProgram = isProgram && impExt == ".c";
                    std::vector<std::string> explicitLibs = DequoteLibClauses(imp);
                    std::vector<std::string> extraDefines = DequoteDefineClauses(imp);
                    if (!CompileImportedFile(filePath, importFilename, ns, isCProgram ? alias : "", explicitLibs, extraDefines))
                        return false;

                    // Mirror Compile()'s 'import program "file.cb" as Name' handling:
                    // rename the imported 'main' so the EmitProgramRunWrapper helper
                    // path can find it under programTable[alias].MainFunction.
                    // (.c programs are wired up inside CompileCFile/RegisterCSignatures.)
                    if (isProgram && !isCProgram)
                    {
                        if (auto* mainFn = module->getFunction("main"))
                        {
                            mainFn->setName("__imported_main_" + alias);
                            programTable[alias].IsImportedProgram = true;
                            programTable[alias].MainFunction = mainFn;
                        }
                    }
                }
            }
        }

        // Forward-ref scan
        {
            ForwardRefScanner scanner(this);
            scanner.SetTokens(&tokens);
            if (auto* tu = computeUnit->translationUnit())
                for (auto* decl : tu->externalDeclaration())
                    scanner.ScanGenericTypeUses(decl);
            if (auto* tu = computeUnit->translationUnit())
                for (auto* decl : tu->externalDeclaration())
                    scanner.ScanExternalDeclaration(decl);
        }

        // Code-gen walk
        auto myListener = std::make_unique<MainListener>(&parser, this, sourceFileName);
        auto walker = antlr4::tree::ParseTreeWalker();
        walker.walk(myListener.get(), computeUnit);
        stream.close();
    }
    catch (CompilerAbortException&) { return false; }
    catch (ExpectedErrorReceived&)  { return false; }

    return true;
}

void LLVMBackend::ResetForReanalysis()
{
    // Reset the LLVM context so named struct types from a previous analysis do
    // not survive into the next one.  Without this, two fixtures that define a
    // struct with the same name but different field types (e.g. Point{int,int}
    // then Point{float,float}) would share the stale type object, causing an
    // LLVM assertion when inserting a float constant into an int-typed slot.
    //
    // Destroy module and builder BEFORE the context: both hold internal
    // references to the context, and their destructors must not run against
    // an already-freed context object.
    module.reset();
    builder.reset();
    context = std::make_unique<llvm::LLVMContext>();
    module = std::make_unique<llvm::Module>("cflat", *context);
    builder = std::make_unique<llvm::IRBuilder<>>(*context);

    functionTable.clear();
    dataStructures.clear();
    programTable.clear();
    enumBackingTypes.clear();
    typeAliases.clear();
    interfaceTable.clear();
    interfaceParents.clear();
    globalNamedVariable.clear();
    globalVariableTypes.clear();
    namespaceTable.clear();
    stringPool.clear();
    stackNamedVariable.clear();
    namespaceAliasTable.clear();
    returnBlockTable.clear();
    compileTimeMacros.clear();
    stringLiteralLenByPtr.clear();
    strConcatRegistered = false;
    stringDtorRegistered = false;
    lambdaCounter = 0;
    expectedError.clear();
    expectedErrorScopeDepth = SIZE_MAX;
    currentFunction = nullptr;
    autoVaListAlloca = nullptr;
    returnCapture = std::nullopt;
    autoReturnCapture = std::nullopt;

    // Transient per-call / per-expression result state. These are normally produced and
    // consumed within a single statement, but an aborted compile (e.g. an expect_error that
    // fires mid-assignment, before the bond flag is consumed) can leave them set. Clearing
    // them here prevents stale state from leaking into the next file's analysis - notably a
    // stale lastCallIsBonded would raise a spurious bond error on the next assignment.
    lastCallReturnType = TypeAndValue{};
    lastCallReturnsOwned = false;
    lastOwningResult = false;
    currentFunctionReturnsOwned = false;
    lastCallIsBonded = false;
    lastCallBondedSources.clear();
    lastCallLambdaCaptureNames.clear();
    lastCallRequiredLocks.clear();
    lastCallParameterNames.clear();
    vectorizeLoops_.clear();
    // Cross-thread sharing scan: per-file escaped-type set and report dedupe set.
    // The configured scan LEVEL is intentionally preserved across files in a batch.
    threadSharedTypes_.clear();  // re-derived per top-level file by ScanCrossThreadEscapes
    xthreadReported_.clear();

    // Clear all import state so core files (string, runtime, etc.) are fully re-imported
    // on the next Analyze() call.
    importedFiles.clear();
    importStack.clear();
    importedParseStates.clear();

    // Generic-template state must also be cleared so prior-analysis ANTLR contexts
    // (which point into a discarded parse tree) don't survive into the next run.
    gts.Clear();

    // Re-register the built-in string type that was wiped by the clears above.
    // string.cb references 'string' as a return type before the struct is parsed,
    // so it must exist before any core file is compiled.
    RegisterBuiltinString();
}

// ---- Cross-thread sharing diagnostic (--xthread-scan N) pre-pass ----
//
// Collects the set of struct TYPES whose instances escape to a spawned thread, so the
// two field-access reporting sites in MainListener can flag non-atomic/non-guarded
// shared fields. This is a deliberately lightweight, syntactic pre-pass: it resolves
// a context argument back to a local/parameter declaration to learn its struct type.
namespace {

// Recursively gather every parse-tree node of context type T under `node`.
template <class T>
void CollectContexts(antlr4::tree::ParseTree* node, std::vector<T*>& out)
{
    if (auto* t = dynamic_cast<T*>(node))
        out.push_back(t);
    for (auto* child : node->children)
        CollectContexts<T>(child, out);
}

// Pull the (typeName, isPointer) out of a declarationSpecifiers, skipping the
// soft-keyword/qualifier specifiers that are not the actual type.
void XtDeclTypeInfo(CFlatParser::DeclarationSpecifiersContext* ds, std::string& typeName, bool& isPointer)
{
    typeName.clear();
    isPointer = false;
    if (!ds) return;
    for (auto* d : ds->declarationSpecifier())
    {
        if (auto* ts = d->typeSpecifier())
        {
            std::string t = ts->getText();
            if (t == "move" || t == "bond" || t == "const")
                continue;  // not the type
            if (typeName.empty())
                typeName = t;
        }
        if (d->pointer())
            isPointer = true;
    }
}

// Name bound by a directDeclarator (handles the parenthesized form).
std::string XtDirectDeclName(CFlatParser::DirectDeclaratorContext* dd)
{
    if (!dd) return "";
    if (dd->Identifier()) return dd->Identifier()->getText();
    if (dd->Move()) return "move";
    if (dd->declarator() && dd->declarator()->directDeclarator())
        return XtDirectDeclName(dd->declarator()->directDeclarator());
    return "";
}

// From the raw text of a context argument, recover the referenced identifier and
// whether it was an address-of (&x) and/or a move handoff. Strips leading casts.
void XtParseCtxArg(const std::string& raw, std::string& ident, bool& addressOf, bool& isMove)
{
    ident.clear();
    addressOf = false;
    isMove = false;
    std::string s = raw;
    auto isIdentChar = [](char c) { return std::isalnum((unsigned char)c) != 0 || c == '_'; };

    // Leading `move` keyword (call-site move handoff): ownership transfers, not shared.
    if (s.rfind("move", 0) == 0 && (s.size() == 4 || !isIdentChar(s[4])))
    {
        isMove = true;
        s = s.substr(4);
    }
    // Strip leading cast groups like `(void*)` / `(ComputeCtx*)`.
    while (!s.empty() && s[0] == '(')
    {
        size_t close = s.find(')');
        if (close == std::string::npos) break;
        std::string after = s.substr(close + 1);
        if (after.empty()) break;            // a parenthesized expression, not a cast prefix
        s = after;
    }
    if (!s.empty() && s[0] == '&')
    {
        addressOf = true;
        s = s.substr(1);
    }
    size_t i = 0;
    while (i < s.size() && isIdentChar(s[i]))
        ++i;
    ident = s.substr(0, i);
}

} // namespace

void LLVMBackend::ScanCrossThreadEscapes(CFlatParser::CompilationUnitContext* cu)
{
    if (xthreadScanLevel_ <= 0 || cu == nullptr)
        return;

    const int level = xthreadScanLevel_;

    std::vector<CFlatParser::FunctionDefinitionContext*> functions;
    CollectContexts<CFlatParser::FunctionDefinitionContext>(cu, functions);

    for (auto* fn : functions)
    {
        // Build the local symbol table: parameters + declared locals -> (type, isPointer).
        std::unordered_map<std::string, std::pair<std::string, bool>> locals;

        if (auto* ptl = fn->parameterTypeList())
        {
            if (auto* pl = ptl->parameterList())
            {
                for (auto* pd : pl->parameterDeclaration())
                {
                    std::string type; bool ptr = false;
                    XtDeclTypeInfo(pd->declarationSpecifiers(), type, ptr);
                    if (type.empty() || !pd->declarator())
                        continue;
                    std::string name = XtDirectDeclName(pd->declarator()->directDeclarator());
                    if (!name.empty())
                        locals[name] = { type, ptr };
                }
            }
        }

        std::vector<CFlatParser::DeclarationContext*> decls;
        CollectContexts<CFlatParser::DeclarationContext>(fn, decls);
        for (auto* decl : decls)
        {
            std::string type; bool ptr = false;
            XtDeclTypeInfo(decl->declarationSpecifiers(), type, ptr);
            if (type.empty() || !decl->initDeclaratorList())
                continue;
            for (auto* id : decl->initDeclaratorList()->initDeclarator())
            {
                if (!id->declarator())
                    continue;
                std::string name = XtDirectDeclName(id->declarator()->directDeclarator());
                if (!name.empty())
                    locals[name] = { type, ptr };
            }
        }

        // Resolve one candidate context argument to a struct type and, per level, escape it.
        auto consider = [&](const std::string& argText)
        {
            std::string ident; bool addressOf = false, isMove = false;
            XtParseCtxArg(argText, ident, addressOf, isMove);
            if (ident.empty() || isMove)
                return;                      // unresolved, or ownership handed off (safe)
            auto it = locals.find(ident);
            if (it == locals.end())
                return;
            const std::string& typeName = it->second.first;
            bool isPtr = it->second.second;
            if (dataStructures.find(typeName) == dataStructures.end())
                return;                      // not a known struct type
            if (addressOf && !isPtr)
            {
                // &localStruct -> escape at level >= 1
                if (level >= 1)
                    AddXthreadEscapedType(typeName);
            }
            else if (isPtr)
            {
                // heap struct-pointer handoff -> escape at level >= 2
                if (level >= 2)
                    AddXthreadEscapedType(typeName);
            }
        };

        std::vector<CFlatParser::PostfixExpressionContext*> postfixes;
        CollectContexts<CFlatParser::PostfixExpressionContext>(fn, postfixes);

        for (auto* pe : postfixes)
        {
            const auto& kids = pe->children;
            for (size_t k = 0; k < kids.size(); ++k)
            {
                // Member call `.start(...)` / `->start(...)`: a thread spawn. The
                // context is the 2nd argument (1st is the thread function).
                auto* term = dynamic_cast<antlr4::tree::TerminalNode*>(kids[k]);
                if (!term || term->getText() != "start" || k == 0)
                    continue;
                std::string prev = kids[k - 1]->getText();
                if (prev != "." && prev != "->" && prev != "?.")
                    continue;
                // Find the argument list of this call (first one after the member name).
                CFlatParser::ArgumentExpressionListContext* args = nullptr;
                for (size_t j = k + 1; j < kids.size(); ++j)
                {
                    if (auto* a = dynamic_cast<CFlatParser::ArgumentExpressionListContext*>(kids[j]))
                    {
                        args = a;
                        break;
                    }
                }
                if (!args)
                    continue;
                auto named = args->argumentNamedExpression();
                if (named.size() >= 2 && named[1]->assignmentExpression())
                    consider(named[1]->assignmentExpression()->getText());
            }
        }

        // Level 3: treat ANY struct pointer / address-of-struct passed to ANY call as a
        // potential cross-thread escape (most aggressive, most false positives).
        if (level >= 3)
        {
            std::vector<CFlatParser::ArgumentExpressionListContext*> allArgs;
            CollectContexts<CFlatParser::ArgumentExpressionListContext>(fn, allArgs);
            for (auto* args : allArgs)
                for (auto* nm : args->argumentNamedExpression())
                    if (nm->assignmentExpression())
                        consider(nm->assignmentExpression()->getText());
        }
    }

    if (verbose && !threadSharedTypes_.empty())
    {
        std::cout << "[verbose] xthread escaped types:";
        for (const auto& t : threadSharedTypes_)
            std::cout << " " << t;
        std::cout << "\n";
    }
}

// ---- Linker path cache helpers ----

std::string LLVMBackend::GetCflatCacheDir()
{
    char buf[260] = {};
    size_t len = 0;
    if (getenv_s(&len, buf, sizeof(buf), "USERPROFILE") != 0 || len == 0)
        return {};
    return std::string(buf) + "\\.cflat";
}

LinkerPaths LLVMBackend::DiscoverLinkerPaths(const std::string& arch, const std::string& runtimeDir)
{
    LinkerPaths result;

    // Find lld-link: prefer the copy next to clang-cl (which lives next to cflat.exe).
    {
        std::string clangPath;
        if (!runtimeDir.empty())
        {
            llvm::SmallString<256> c(runtimeDir);
            llvm::sys::path::append(c, "clang-cl.exe");
            if (llvm::sys::fs::exists(c))
                clangPath = c.str().str();
        }
        if (clangPath.empty())
        {
            if (auto p = llvm::sys::findProgramByName("clang-cl"))
                clangPath = *p;
        }
        if (!clangPath.empty())
        {
            llvm::SmallString<256> candidate(llvm::sys::path::parent_path(clangPath));
            llvm::sys::path::append(candidate, "lld-link.exe");
            if (llvm::sys::fs::exists(candidate))
                result.lldLink = candidate.str().str();
        }
        if (result.lldLink.empty())
        {
            if (auto p = llvm::sys::findProgramByName("lld-link"))
                result.lldLink = *p;
        }
    }

    // Find VS install path via vswhere.
    std::string vsPath;
    {
        const char* vswhereFixed = "C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe";
        if (llvm::sys::fs::exists(vswhereFixed))
        {
            llvm::SmallString<256> outFile;
            llvm::sys::path::system_temp_directory(true, outFile);
            int outFD;
            if (!llvm::sys::fs::createTemporaryFile("cflat_vswhere", "txt", outFD, outFile))
            {
                _close(outFD);
                std::string outFileStr = outFile.str().str();
                std::vector<llvm::StringRef> vsArgs = { vswhereFixed, "-latest", "-property", "installationPath" };
                std::optional<llvm::StringRef> vsRedirects[3] = { std::nullopt, llvm::StringRef(outFileStr), std::nullopt };
                llvm::sys::ExecuteAndWait(vswhereFixed, vsArgs, std::nullopt, vsRedirects);
                if (auto buf = llvm::MemoryBuffer::getFile(outFileStr))
                    vsPath = buf.get()->getBuffer().trim().str();
                llvm::sys::fs::remove(outFile);
            }
        }
    }

    // Find the latest MSVC lib directory.
    if (!vsPath.empty())
    {
        std::string msvcRoot = vsPath + "\\VC\\Tools\\MSVC";
        std::string latestVer;
        std::error_code ec;
        for (auto it = llvm::sys::fs::directory_iterator(msvcRoot, ec);
             it != llvm::sys::fs::directory_iterator(); it.increment(ec))
        {
            if (ec) break;
            auto ver = llvm::sys::path::filename(it->path()).str();
            if (ver > latestVer) latestVer = ver;
        }
        if (!latestVer.empty())
            result.msvcLib = msvcRoot + "\\" + latestVer + "\\lib\\" + arch;
    }

    // Find the latest Windows SDK lib directories.
    {
        std::string wkLib = "C:\\Program Files (x86)\\Windows Kits\\10\\Lib";
        std::string latestSDK;
        std::error_code ec;
        for (auto it = llvm::sys::fs::directory_iterator(wkLib, ec);
             it != llvm::sys::fs::directory_iterator(); it.increment(ec))
        {
            if (ec) break;
            auto ver = llvm::sys::path::filename(it->path()).str();
            if (ver > latestSDK) latestSDK = ver;
        }
        if (!latestSDK.empty())
        {
            result.ucrtLib = wkLib + "\\" + latestSDK + "\\ucrt\\" + arch;
            result.umLib   = wkLib + "\\" + latestSDK + "\\um\\"   + arch;
        }
    }

    return result;
}

std::optional<LinkerPaths> LLVMBackend::LoadLinkerPathsFromCache(const std::string& arch)
{
    std::string cacheDir = GetCflatCacheDir();
    if (cacheDir.empty()) return std::nullopt;

    std::string jsonPath = cacheDir + "\\linker_paths_" + arch + ".json";
    auto buf = llvm::MemoryBuffer::getFile(jsonPath);
    if (!buf) return std::nullopt;

    auto parsed = llvm::json::parse(buf.get()->getBuffer());
    if (!parsed) return std::nullopt;

    auto* obj = parsed->getAsObject();
    if (!obj) return std::nullopt;

    LinkerPaths paths;
    if (auto v = obj->getString("lld_link")) paths.lldLink = v->str();
    if (auto v = obj->getString("msvc_lib")) paths.msvcLib = v->str();
    if (auto v = obj->getString("ucrt_lib")) paths.ucrtLib = v->str();
    if (auto v = obj->getString("um_lib"))   paths.umLib   = v->str();

    if (!paths.AllExist()) return std::nullopt;
    return paths;
}

bool LLVMBackend::SaveLinkerPathsToCache(const std::string& arch, const LinkerPaths& paths)
{
    std::string cacheDir = GetCflatCacheDir();
    if (cacheDir.empty()) return false;

    if (std::error_code ec = llvm::sys::fs::create_directories(cacheDir); ec)
        return false;

    llvm::json::Object obj;
    obj["lld_link"] = paths.lldLink;
    obj["msvc_lib"] = paths.msvcLib;
    obj["ucrt_lib"] = paths.ucrtLib;
    obj["um_lib"]   = paths.umLib;

    std::string jsonPath = cacheDir + "\\linker_paths_" + arch + ".json";
    std::error_code fec;
    llvm::raw_fd_ostream out(jsonPath, fec);
    if (fec) return false;

    out << llvm::json::Value(std::move(obj));
    return true;
}

LinkerPaths LLVMBackend::FindLinkerPaths(const std::string& arch, const std::string& runtimeDir)
{
    if (auto cached = LoadLinkerPathsFromCache(arch))
        return *cached;

    LinkerPaths paths = DiscoverLinkerPaths(arch, runtimeDir);
    SaveLinkerPathsToCache(arch, paths);
    return paths;
}

bool LLVMBackend::PrintSupportedCpus()
{
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();

    // CFlat currently targets only Windows on the X86 family (win64 / win32). Both
    // platforms map to LLVM's X86 backend, which exposes a single CPU table regardless
    // of pointer width, so listing it once via the win64 triple covers both.
    const char* triple = "x86_64-pc-windows-msvc";
    std::string err;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, err);
    if (!target)
    {
        std::cerr << "Error: no target for triple '" << triple << "': " << err << "\n";
        return false;
    }

    std::unique_ptr<llvm::MCSubtargetInfo> sti(
        target->createMCSubtargetInfo(triple, "", ""));
    if (!sti)
    {
        std::cerr << "Error: could not create subtarget info for '" << triple << "'.\n";
        return false;
    }

    // getAllProcessorDescriptions() is required to be sorted by Key, so we can print
    // it in order directly. Skip any empty key defensively.
    std::cout << "Supported target CPUs (Windows x86/x64):\n";
    for (const auto& kv : sti->getAllProcessorDescriptions())
    {
        if (kv.Key && *kv.Key)
            std::cout << "  " << kv.Key << "\n";
    }
    return true;
}

bool LLVMBackend::PrintHostCpu()
{
    // getHostCPUName() reads CPUID and maps it to an LLVM CPU name (the same value
    // --cpu native resolves to). No target init is required for this query.
    llvm::StringRef host = llvm::sys::getHostCPUName();
    if (host.empty())
    {
        std::cerr << "Error: could not determine the host CPU.\n";
        return false;
    }
    std::cout << host.str() << "\n";
    return true;
}

bool LLVMBackend::ResolveCpuName(const std::string& requested, const std::string& triple,
                                 const char* label, bool verbose, std::string& resolved)
{
    std::string name = requested;
    if (name == "native")
    {
        name = llvm::sys::getHostCPUName().str();
        if (verbose)
            std::cout << "[verbose] " << label << " native resolved to: " << name << "\n";
    }

    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();

    std::string err;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, err);
    if (!target)
    {
        std::cerr << "Error: no target for triple '" << triple << "': " << err << "\n";
        return false;
    }

    std::unique_ptr<llvm::MCSubtargetInfo> sti(
        target->createMCSubtargetInfo(triple, "", ""));
    if (sti && !sti->isCPUStringValid(name))
    {
        std::cerr << "Error: unknown " << label << " '" << name
                  << "'. Run --print-supported-cpus for the list.\n";
        return false;
    }

    resolved = name;
    return true;
}

bool LLVMBackend::RunInit(const std::string& runtimeDir, bool verbose)
{
    std::string cacheDir = GetCflatCacheDir();
    if (cacheDir.empty())
    {
        std::cerr << "Error: USERPROFILE environment variable is not set.\n";
        return false;
    }

    if (std::error_code ec = llvm::sys::fs::create_directories(cacheDir); ec)
    {
        std::cerr << "Error: could not create " << cacheDir << ": " << ec.message() << "\n";
        return false;
    }
    std::cout << "Cache directory: " << cacheDir << "\n";

    for (const char* arch : {"x64", "x86"})
    {
        std::cout << "Discovering linker paths for " << arch << "...\n";
        LinkerPaths paths = DiscoverLinkerPaths(arch, runtimeDir);

        std::cout << "  lld-link: " << (paths.lldLink.empty() ? "(not found)" : paths.lldLink) << "\n";
        std::cout << "  msvc-lib: " << (paths.msvcLib.empty() ? "(not found)" : paths.msvcLib) << "\n";
        std::cout << "  ucrt-lib: " << (paths.ucrtLib.empty() ? "(not found)" : paths.ucrtLib) << "\n";
        std::cout << "  um-lib:   " << (paths.umLib.empty() ? "(not found)" : paths.umLib) << "\n";

        if (SaveLinkerPathsToCache(arch, paths))
            std::cout << "  Saved linker_paths_" << arch << ".json\n";
        else
            std::cerr << "  Warning: could not write cache file for " << arch << ".\n";
    }

    // Build core bitcode cache for win64 (win32 has pre-existing compilation issues).
    for (const char* platform : {"win64"})
    {
        std::cout << "Building core bitcode cache for " << platform << "...\n";
        LLVMBackend coreCompiler;
        coreCompiler.SetRuntimeDir(runtimeDir);
        coreCompiler.SetVerbose(verbose);
        if (!coreCompiler.CompileCoreOnly(platform))
        {
            std::cerr << "  Warning: core compilation failed for " << platform << ".\n";
            continue;
        }
        std::string bcCacheDir = LLVMBackend::GetRuntimeBitcodeDir(runtimeDir);
        if (bcCacheDir.empty())
        {
            std::cerr << "  Warning: could not determine bitcode cache dir.\n";
            continue;
        }
        if (std::error_code ec = llvm::sys::fs::create_directories(bcCacheDir); ec)
        {
            std::cerr << "  Warning: could not create " << bcCacheDir << ": " << ec.message() << "\n";
            continue;
        }
        if (coreCompiler.SaveCoreBitcode(bcCacheDir, platform))
            std::cout << "  Saved core_" << platform << ".bc + .meta.json\n";
        else
            std::cerr << "  Warning: could not write core bitcode cache for " << platform << ".\n";
    }

    return true;
}

// ---- Core bitcode cache helpers ----

static uint64_t FnvHash64(const void* data, size_t len)
{
    uint64_t h = 14695981039346656037ULL;
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

// FNV-1a hash over all .cb file modification times in runtimeDir/core, sorted by path.
// Returns a 16-hex-char string. Returns "" if the core dir is inaccessible.
static std::string ComputeCoreHash(const std::string& runtimeDir)
{
    auto coreDir = std::filesystem::path(runtimeDir) / "core";
    std::error_code ec;
    std::vector<std::filesystem::path> files;
    for (auto& e : std::filesystem::directory_iterator(coreDir, ec))
        if (e.path().extension() == ".cb") files.push_back(e.path());
    if (ec) return {};
    std::sort(files.begin(), files.end());

    uint64_t h = 14695981039346656037ULL;
    for (auto& f : files)
    {
        auto wt = std::filesystem::last_write_time(f, ec);
        if (ec) continue;
        auto ns = wt.time_since_epoch().count();
        h ^= FnvHash64(&ns, sizeof(ns));
        h *= 1099511628211ULL;
    }
    char buf[17];
    snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

// Returns %USERPROFILE%\.cflat\runtime\<hash>  or "" on failure.
std::string LLVMBackend::GetRuntimeBitcodeDir(const std::string& runtimeDir)
{
    std::string base = GetCflatCacheDir();
    if (base.empty()) return {};
    std::string hash = ComputeCoreHash(runtimeDir);
    if (hash.empty()) return {};
    return base + "\\runtime\\" + hash;
}

// ---- Serialization helpers (file-scope) ----

using TAV  = LLVMBackend::TypeAndValue;
using DTAV = LLVMBackend::DeclTypeAndValue;
using BFI  = LLVMBackend::BitfieldInfo;
using ANN  = LLVMBackend::AnnotationValue;
using FS   = LLVMBackend::FunctionSymbol;
using IM   = LLVMBackend::InterfaceMethod;

static llvm::json::Object SerializeTav(const TAV& t)
{
    llvm::json::Object o;
    o["t"] = t.TypeName;
    if (!t.VariableName.empty())  o["n"]   = t.VariableName;
    if (t.Pointer)                o["p"]   = true;
    if (t.ElemPointer)            o["ep"]  = true;
    if (t.IsInterface)            o["if"]  = true;
    if (t.IsInterfacePointer)     o["ifp"] = true;
    if (t.IsNullable)             o["nl"]  = true;
    if (t.IsMove)                 o["mv"]  = true;
    if (t.IsBond)                 o["bd"]  = true;
    if (t.IsStdcall)              o["sc"]  = true;
    if (t.IsCdecl)                o["cc"]  = true;
    if (t.LockThis)               o["lt"]  = true;
    if (!t.GuardedBy.empty())     o["gb"]  = t.GuardedBy;
    if (t.IsFunctionPointer)
    {
        o["fp"]  = true;
        o["fpr"] = t.FuncPtrReturnTypeName;
        if (t.FuncPtrReturnPointer) o["fprp"] = true;
        llvm::json::Array fps;
        for (auto& p : t.FuncPtrParams)
        {
            llvm::json::Object po;
            po["t"] = p.TypeName;
            if (p.Pointer) po["p"] = true;
            if (p.IsMove)  po["mv"] = true;
            fps.push_back(std::move(po));
        }
        o["fpp"] = std::move(fps);
    }
    if (t.ConstArraySize > 0)
    {
        o["as"] = static_cast<int64_t>(t.ConstArraySize);
        if (!t.ConstInnerDimensions.empty())
        {
            llvm::json::Array dims;
            for (auto d : t.ConstInnerDimensions) dims.push_back(static_cast<int64_t>(d));
            o["aid"] = std::move(dims);
        }
    }
    if (t.IsSimd)
    {
        o["sd"] = true;
        o["sdl"] = static_cast<int64_t>(t.SimdLanes);
    }
    if (t.IsArrayView) o["av"] = true;
    return o;
}

static TAV DeserializeTav(const llvm::json::Object& o)
{
    TAV t;
    if (auto v = o.getString("t"))   t.TypeName = v->str();
    if (auto v = o.getString("n"))   t.VariableName = v->str();
    if (auto v = o.getBoolean("p"))  t.Pointer = *v;
    if (auto v = o.getBoolean("ep")) t.ElemPointer = *v;
    if (auto v = o.getBoolean("if")) t.IsInterface = *v;
    if (auto v = o.getBoolean("ifp"))t.IsInterfacePointer = *v;
    if (auto v = o.getBoolean("nl")) t.IsNullable = *v;
    if (auto v = o.getBoolean("mv")) t.IsMove = *v;
    if (auto v = o.getBoolean("bd")) t.IsBond = *v;
    if (auto v = o.getBoolean("sc")) t.IsStdcall = *v;
    if (auto v = o.getBoolean("cc")) t.IsCdecl = *v;
    if (auto v = o.getBoolean("lt")) t.LockThis = *v;
    if (auto v = o.getString("gb"))  t.GuardedBy = v->str();
    if (auto v = o.getBoolean("fp")) t.IsFunctionPointer = *v;
    if (t.IsFunctionPointer)
    {
        if (auto v = o.getString("fpr"))   t.FuncPtrReturnTypeName = v->str();
        if (auto v = o.getBoolean("fprp")) t.FuncPtrReturnPointer = *v;
        if (auto* fps = o.getArray("fpp"))
            for (auto& elem : *fps)
                if (auto* po = elem.getAsObject())
                {
                    TAV::FuncPtrParam p;
                    if (auto v = po->getString("t"))  p.TypeName = v->str();
                    if (auto v = po->getBoolean("p")) p.Pointer = *v;
                    if (auto v = po->getBoolean("mv"))p.IsMove = *v;
                    t.FuncPtrParams.push_back(std::move(p));
                }
    }
    if (auto v = o.getInteger("as")) t.ConstArraySize = static_cast<uint64_t>(*v);
    if (auto* dims = o.getArray("aid"))
        for (auto& d : *dims)
            if (auto v = d.getAsInteger()) t.ConstInnerDimensions.push_back(static_cast<uint64_t>(*v));
    if (auto v = o.getBoolean("sd")) t.IsSimd = *v;
    if (auto v = o.getInteger("sdl")) t.SimdLanes = static_cast<uint64_t>(*v);
    if (auto v = o.getBoolean("av")) t.IsArrayView = *v;
    return t;
}

static llvm::json::Array SerializeAnnotations(const std::vector<ANN>& anns)
{
    llvm::json::Array arr;
    for (auto& a : anns)
    {
        llvm::json::Object ao;
        ao["n"] = a.Name;
        if (!a.Value.empty()) ao["v"] = a.Value;
        arr.push_back(std::move(ao));
    }
    return arr;
}

static std::vector<ANN> DeserializeAnnotations(const llvm::json::Array* arr)
{
    std::vector<ANN> out;
    if (!arr) return out;
    for (auto& elem : *arr)
        if (auto* ao = elem.getAsObject())
        {
            ANN a;
            if (auto v = ao->getString("n")) a.Name = v->str();
            if (auto v = ao->getString("v")) a.Value = v->str();
            out.push_back(std::move(a));
        }
    return out;
}

static llvm::json::Object SerializeDtav(const DTAV& d)
{
    auto o = SerializeTav(d);
    if (d.external)    o["ext"] = true;
    if (d.threadLocal) o["tl"]  = true;
    if (d.IsBitfield)
    {
        o["bf"]   = true;
        o["bfw"]  = static_cast<int64_t>(d.BitWidth);
        o["bfo"]  = static_cast<int64_t>(d.BitOffset);
        o["bfsi"] = static_cast<int64_t>(d.StorageFieldIndex);
        if (d.IsBitfieldStorage) o["bfs"] = true;
    }
    if (d.UserAlignValue > 0) o["ua"] = static_cast<int64_t>(d.UserAlignValue);
    if (!d.Annotations.empty()) o["ann"] = SerializeAnnotations(d.Annotations);
    return o;
}

static DTAV DeserializeDtav(const llvm::json::Object& o)
{
    DTAV d;
    static_cast<TAV&>(d) = DeserializeTav(o);
    if (auto v = o.getBoolean("ext")) d.external = *v;
    if (auto v = o.getBoolean("tl"))  d.threadLocal = *v;
    if (auto v = o.getBoolean("bf"))
    {
        d.IsBitfield = *v;
        if (auto w = o.getInteger("bfw"))  d.BitWidth = static_cast<unsigned>(*w);
        if (auto w = o.getInteger("bfo"))  d.BitOffset = static_cast<unsigned>(*w);
        if (auto w = o.getInteger("bfsi")) d.StorageFieldIndex = static_cast<unsigned>(*w);
        if (auto w = o.getBoolean("bfs"))  d.IsBitfieldStorage = *w;
    }
    if (auto v = o.getInteger("ua")) d.UserAlignValue = static_cast<uint64_t>(*v);
    d.Annotations = DeserializeAnnotations(o.getArray("ann"));
    return d;
}

static llvm::json::Object SerializeBfi(const BFI& b)
{
    llvm::json::Object o;
    o["n"]  = b.Name;
    o["t"]  = b.TypeName;
    o["si"] = static_cast<int64_t>(b.StorageFieldIndex);
    o["bo"] = static_cast<int64_t>(b.BitOffset);
    o["bw"] = static_cast<int64_t>(b.BitWidth);
    if (b.IsUnsigned) o["u"] = true;
    if (!b.Annotations.empty()) o["ann"] = SerializeAnnotations(b.Annotations);
    return o;
}

static BFI DeserializeBfi(const llvm::json::Object& o)
{
    BFI b;
    if (auto v = o.getString("n"))   b.Name = v->str();
    if (auto v = o.getString("t"))   b.TypeName = v->str();
    if (auto v = o.getInteger("si")) b.StorageFieldIndex = static_cast<unsigned>(*v);
    if (auto v = o.getInteger("bo")) b.BitOffset = static_cast<unsigned>(*v);
    if (auto v = o.getInteger("bw")) b.BitWidth = static_cast<unsigned>(*v);
    if (auto v = o.getBoolean("u"))  b.IsUnsigned = *v;
    b.Annotations = DeserializeAnnotations(o.getArray("ann"));
    return b;
}

static llvm::json::Object SerializeFuncSym(const std::string& key, const FS& s)
{
    llvm::json::Object o;
    o["k"] = key;
    o["u"] = s.UniqueName;
    o["r"] = SerializeTav(s.ReturnType);
    llvm::json::Array ps;
    for (auto& p : s.Parameters) ps.push_back(SerializeTav(p));
    o["ps"] = std::move(ps);
    if (s.Variadic)     o["va"] = true;
    if (s.ReturnsOwned) o["ro"] = true;
    if (s.IsMethod)     o["m"]  = true;
    if (!s.RequiredLocks.empty())
    {
        llvm::json::Array rl;
        for (auto& l : s.RequiredLocks) rl.push_back(l);
        o["rl"] = std::move(rl);
    }
    return o;
}

static llvm::json::Object SerializeIfaceMethod(const IM& m)
{
    llvm::json::Object o;
    o["n"] = m.Name;
    o["r"] = SerializeTav(m.ReturnType);
    llvm::json::Array ps;
    for (auto& p : m.Parameters) ps.push_back(SerializeTav(p));
    o["ps"] = std::move(ps);
    return o;
}

static IM DeserializeIfaceMethod(const llvm::json::Object& o)
{
    IM m;
    if (auto v = o.getString("n")) m.Name = v->str();
    if (auto* r = o.getObject("r")) m.ReturnType = DeserializeTav(*r);
    if (auto* ps = o.getArray("ps"))
        for (auto& elem : *ps)
            if (auto* po = elem.getAsObject()) m.Parameters.push_back(DeserializeTav(*po));
    return m;
}

static llvm::json::Object SerializeConstraints(
    const std::unordered_map<std::string, std::vector<std::string>>& c)
{
    llvm::json::Object o;
    for (auto& [tp, ifaces] : c)
    {
        llvm::json::Array arr;
        for (auto& i : ifaces) arr.push_back(i);
        o[tp] = std::move(arr);
    }
    return o;
}

static std::unordered_map<std::string, std::vector<std::string>>
DeserializeConstraints(const llvm::json::Object* o)
{
    std::unordered_map<std::string, std::vector<std::string>> out;
    if (!o) return out;
    for (auto& kv : *o)
    {
        std::vector<std::string> ifaces;
        if (auto* arr = kv.second.getAsArray())
            for (auto& elem : *arr)
                if (auto v = elem.getAsString()) ifaces.push_back(v->str());
        out[kv.first.str()] = std::move(ifaces);
    }
    return out;
}

bool LLVMBackend::CompileCoreOnly(const std::string& platform)
{
    platformValue = (platform == "win32") ? 32 : 64;
    const char* dl = (platformValue == 32)
        ? "e-m:x-p:32:32-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:32-n8:16:32-S32"
        : "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128";

    // Reinitialize the LLVM module with the correct platform data layout BEFORE
    // RegisterBuiltinString creates pointer types, so %string fields have the right size.
    // The Init()-created module used the LLVM default layout which differs on Win32.
    {
        builder.reset();
        module.reset();
        functionTable.clear();
        dataStructures.clear();
        context = std::make_unique<llvm::LLVMContext>();
        module  = std::make_unique<llvm::Module>("cflat", *context);
        builder = std::make_unique<llvm::IRBuilder<>>(*context);
    }
    module->setDataLayout(llvm::DataLayout(dl));
    module->setTargetTriple((platformValue == 32) ? "i686-pc-windows-msvc" : "x86_64-pc-windows-msvc");
    RegisterBuiltinString();

    auto platformConst = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), platformValue);
    SetCompileTimeMacro("__PLATFORM__", platformConst, "int");
    auto win64Const = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), platformValue == 64 ? 1 : 0);
    SetCompileTimeMacro("__WIN64__", win64Const, "int");
    auto win32Const = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), platformValue == 32 ? 1 : 0);
    SetCompileTimeMacro("__WIN32__", win32Const, "int");
    auto windowsConst = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 1);
    SetCompileTimeMacro("__WINDOWS__", windowsConst, "int");
    // Target architecture is always x86/Intel today (win64 -> x86_64, win32 -> i686).
    // Exposed so callers can guard architecture-specific intrinsics like __rdtscp().
    auto x86Const = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 1);
    SetCompileTimeMacro("__X86__", x86Const, "int");

    if (runtimeDir.empty()) return false;
    auto runtimePath = std::filesystem::path(runtimeDir) / "core" / "runtime.cb";
    if (!std::filesystem::exists(runtimePath))
    {
        std::cerr << "Error: runtime.cb not found in " << runtimeDir << "/core\n";
        return false;
    }
    if (!CompileImportedFile(runtimePath.string(), "runtime.cb"))
        return false;
    // Force lazy-registered functions into the bitcode so they are available
    // when the cache is loaded without re-running EnsureStr*/EnsureString*.
    EnsureStrConcatRegistered();
    EnsureStringDtorRegistered();
    return true;
}

bool LLVMBackend::SaveCoreBitcode(const std::string& cacheDir, const std::string& platform) const
{
    std::string prefix = cacheDir + "\\core_" + platform;

    // Write LLVM bitcode.
    {
        std::error_code ec;
        llvm::raw_fd_ostream bcOut(prefix + ".bc", ec);
        if (ec) return false;
        llvm::WriteBitcodeToFile(*module, bcOut);
    }

    // Build metadata JSON.
    llvm::json::Object root;
    root["version"]   = 1;
    root["platform"]  = platform;
    root["core_hash"] = ComputeCoreHash(runtimeDir);

    // importedFiles - store paths relative to runtimeDir so the cache is
    // portable between Debug and Release builds that share core/*.cb files.
    {
        llvm::json::Array arr;
        std::string rdPrefix = runtimeDir;
        // Normalize to lowercase backslash for consistent prefix matching.
        std::replace(rdPrefix.begin(), rdPrefix.end(), '/', '\\');
        for (auto& f : importedFiles)
        {
            std::string norm = f;
            std::replace(norm.begin(), norm.end(), '/', '\\');
            if (norm.size() > rdPrefix.size() &&
                _strnicmp(norm.c_str(), rdPrefix.c_str(), rdPrefix.size()) == 0 &&
                (norm[rdPrefix.size()] == '\\' || norm[rdPrefix.size()] == '/'))
            {
                // Store relative path (skip separator after prefix).
                arr.push_back(norm.substr(rdPrefix.size() + 1));
            }
            else
            {
                arr.push_back(f);
            }
        }
        root["imported_files"] = std::move(arr);
    }

    // namespaceTable
    {
        llvm::json::Array arr;
        for (auto& ns : namespaceTable) arr.push_back(ns);
        root["namespaces"] = std::move(arr);
    }

    // typeAliases
    {
        llvm::json::Object obj;
        for (auto& [k, v] : typeAliases) obj[k] = v;
        root["type_aliases"] = std::move(obj);
    }

    // enumBackingTypes
    {
        llvm::json::Object obj;
        for (auto& [k, v] : enumBackingTypes) obj[k] = v;
        root["enum_backing_types"] = std::move(obj);
    }

    // functionTable
    {
        llvm::json::Array arr;
        for (auto& [key, overloads] : functionTable)
            for (auto& sym : overloads)
                arr.push_back(SerializeFuncSym(key, sym));
        root["functions"] = std::move(arr);
    }

    // dataStructures
    {
        llvm::json::Array arr;
        for (auto& [name, sd] : dataStructures)
        {
            llvm::json::Object so;
            so["name"]      = name;
            so["llvm_type"] = sd.StructType ? std::string(sd.StructType->getName()) : "";
            so["is_union"]  = sd.IsUnion;
            if (sd.UserRequestedAlignment > 0)
                so["user_align"] = static_cast<int64_t>(sd.UserRequestedAlignment);
            if (sd.Destructor)
                so["destructor"] = std::string(sd.Destructor->getName());
            if (sd.typeDescriptor)
                so["type_desc"] = std::string(sd.typeDescriptor->getName());

            llvm::json::Array fields;
            for (auto& f : sd.StructFields) fields.push_back(SerializeDtav(f));
            so["fields"] = std::move(fields);

            llvm::json::Array ifaces;
            for (auto& i : sd.Interfaces) ifaces.push_back(i);
            so["interfaces"] = std::move(ifaces);

            llvm::json::Object vtabs;
            for (auto& [iname, gv] : sd.VTables)
                vtabs[iname] = std::string(gv->getName());
            so["vtables"] = std::move(vtabs);

            llvm::json::Array bfs;
            for (auto& b : sd.Bitfields) bfs.push_back(SerializeBfi(b));
            so["bitfields"] = std::move(bfs);

            arr.push_back(std::move(so));
        }
        root["structs"] = std::move(arr);
    }

    // interfaceTable + interfaceParents
    {
        llvm::json::Array arr;
        for (auto& [name, methods] : interfaceTable)
        {
            llvm::json::Object io;
            io["name"] = name;
            llvm::json::Array parents;
            if (auto it = interfaceParents.find(name); it != interfaceParents.end())
                for (auto& p : it->second) parents.push_back(p);
            io["parents"] = std::move(parents);
            llvm::json::Array ms;
            for (auto& m : methods) ms.push_back(SerializeIfaceMethod(m));
            io["methods"] = std::move(ms);
            arr.push_back(std::move(io));
        }
        root["interfaces"] = std::move(arr);
    }

    // globalNamedVariable + globalVariableTypes
    {
        llvm::json::Array arr;
        for (auto& [name, gv] : globalNamedVariable)
        {
            auto tit = globalVariableTypes.find(name);
            if (tit == globalVariableTypes.end()) continue;
            llvm::json::Object go;
            go["name"]      = name;
            go["llvm_name"] = std::string(gv->getName());
            go["type"]      = SerializeTav(tit->second);
            arr.push_back(std::move(go));
        }
        root["globals"] = std::move(arr);
    }

    // Generic struct templates: source text + type params + constraints
    {
        llvm::json::Array arr;
        for (auto& [name, ctx] : gts.genericStructTemplates)
        {
            auto* start = ctx->getStart();
            auto* stop  = ctx->getStop();
            antlr4::misc::Interval iv(start->getStartIndex(), stop->getStopIndex());
            std::string src = start->getInputStream()->getText(iv);
            llvm::json::Object to;
            to["name"]   = name;
            to["source"] = src;
            llvm::json::Array tps;
            if (auto it = gts.genericStructTypeParams.find(name); it != gts.genericStructTypeParams.end())
                for (auto& tp : it->second) tps.push_back(tp);
            to["type_params"] = std::move(tps);
            if (auto pit = gts.genericStructPackIndex.find(name); pit != gts.genericStructPackIndex.end())
                to["pack_index"] = static_cast<int64_t>(pit->second);
            if (auto cit = gts.genericStructConstraints.find(name); cit != gts.genericStructConstraints.end())
                to["constraints"] = SerializeConstraints(cit->second);
            arr.push_back(std::move(to));
        }
        root["generic_structs"] = std::move(arr);
    }

    // Generic class templates
    {
        llvm::json::Array arr;
        for (auto& [name, ctx] : gts.genericClassTemplates)
        {
            auto* start = ctx->getStart();
            auto* stop  = ctx->getStop();
            antlr4::misc::Interval iv(start->getStartIndex(), stop->getStopIndex());
            std::string src = start->getInputStream()->getText(iv);
            llvm::json::Object to;
            to["name"]   = name;
            to["source"] = src;
            llvm::json::Array tps;
            if (auto it = gts.genericStructTypeParams.find(name); it != gts.genericStructTypeParams.end())
                for (auto& tp : it->second) tps.push_back(tp);
            to["type_params"] = std::move(tps);
            if (auto pit = gts.genericClassPackIndex.find(name); pit != gts.genericClassPackIndex.end())
                to["pack_index"] = static_cast<int64_t>(pit->second);
            if (auto cit = gts.genericClassConstraints.find(name); cit != gts.genericClassConstraints.end())
                to["constraints"] = SerializeConstraints(cit->second);
            arr.push_back(std::move(to));
        }
        root["generic_classes"] = std::move(arr);
    }

    // Generic interface templates
    {
        llvm::json::Array arr;
        for (auto& [name, ctx] : gts.genericInterfaceTemplates)
        {
            auto* start = ctx->getStart();
            auto* stop  = ctx->getStop();
            antlr4::misc::Interval iv(start->getStartIndex(), stop->getStopIndex());
            std::string src = start->getInputStream()->getText(iv);
            llvm::json::Object to;
            to["name"]   = name;
            to["source"] = src;
            llvm::json::Array tps;
            if (auto it = gts.genericInterfaceTypeParams.find(name); it != gts.genericInterfaceTypeParams.end())
                for (auto& tp : it->second) tps.push_back(tp);
            to["type_params"] = std::move(tps);
            if (auto pit = gts.genericInterfacePackIndex.find(name); pit != gts.genericInterfacePackIndex.end())
                to["pack_index"] = static_cast<int64_t>(pit->second);
            arr.push_back(std::move(to));
        }
        root["generic_interfaces"] = std::move(arr);
    }

    // Generic function templates
    {
        llvm::json::Array arr;
        for (auto& [name, ctx] : gts.genericFunctionTemplates)
        {
            auto* start = ctx->getStart();
            auto* stop  = ctx->getStop();
            antlr4::misc::Interval iv(start->getStartIndex(), stop->getStopIndex());
            std::string src = start->getInputStream()->getText(iv);
            llvm::json::Object to;
            to["name"]   = name;
            to["source"] = src;
            llvm::json::Array tps;
            if (auto it = gts.genericFunctionTypeParams.find(name); it != gts.genericFunctionTypeParams.end())
                for (auto& tp : it->second) tps.push_back(tp);
            to["type_params"] = std::move(tps);
            if (auto pit = gts.genericFunctionPackIndex.find(name); pit != gts.genericFunctionPackIndex.end())
                to["pack_index"] = static_cast<int64_t>(pit->second);
            if (auto cit = gts.genericFunctionConstraints.find(name); cit != gts.genericFunctionConstraints.end())
                to["constraints"] = SerializeConstraints(cit->second);
            arr.push_back(std::move(to));
        }
        root["generic_functions"] = std::move(arr);
    }

    // instantiatedGenerics / instantiatedInterfaces / instantiatedGenericFunctions
    {
        llvm::json::Array arr;
        for (auto& s : gts.instantiatedGenerics) arr.push_back(s);
        root["instantiated_generics"] = std::move(arr);
    }
    {
        llvm::json::Array arr;
        for (auto& s : gts.instantiatedInterfaces) arr.push_back(s);
        root["instantiated_interfaces"] = std::move(arr);
    }
    {
        llvm::json::Array arr;
        for (auto& s : gts.instantiatedGenericFunctions) arr.push_back(s);
        root["instantiated_generic_functions"] = std::move(arr);
    }

    // Write JSON.
    {
        std::error_code ec;
        llvm::raw_fd_ostream jsonOut(prefix + ".meta.json", ec);
        if (ec) return false;
        jsonOut << llvm::json::Value(std::move(root));
    }
    return true;
}

bool LLVMBackend::LoadCoreBitcodeIfFresh(const std::string& cacheDir, const std::string& platform)
{
    std::string prefix   = cacheDir + "\\core_" + platform;
    std::string bcPath   = prefix + ".bc";
    std::string jsonPath = prefix + ".meta.json";

    if (!std::filesystem::exists(bcPath) || !std::filesystem::exists(jsonPath))
        return false;

    // Load and validate JSON metadata.
    auto jsonBuf = llvm::MemoryBuffer::getFile(jsonPath);
    if (!jsonBuf) return false;
    auto parsed = llvm::json::parse((*jsonBuf)->getBuffer());
    if (!parsed) return false;
    auto* root = parsed->getAsObject();
    if (!root) return false;

    auto ver      = root->getInteger("version");
    auto storedPl = root->getString("platform");
    auto storedH  = root->getString("core_hash");
    if (!ver || *ver != 1) return false;
    if (!storedPl || storedPl->str() != platform) return false;
    if (!storedH || storedH->str() != ComputeCoreHash(runtimeDir)) return false;

    // Load bitcode into a FRESH LLVMContext so named types (e.g. %string) from
    // RegisterBuiltinString don't conflict with the bitcode's versions.  After
    // loading we replace context/module/builder with the fresh ones.
    auto freshCtx = std::make_unique<llvm::LLVMContext>();
    auto bcBuf = llvm::MemoryBuffer::getFile(bcPath);
    if (!bcBuf) return false;
    auto parsedMod = llvm::parseBitcodeFile((*bcBuf)->getMemBufferRef(), *freshCtx);
    if (!parsedMod)
    {
        llvm::consumeError(parsedMod.takeError());
        return false;
    }

    // Replace the current LLVM state.  Destroy builder/module before context.
    builder.reset();
    module.reset();
    context = std::move(freshCtx);
    module  = std::move(*parsedMod);
    builder = std::make_unique<llvm::IRBuilder<>>(*context);

    // Clear tables populated by Init()/RegisterBuiltinString() whose LLVM pointers
    // now dangle (they pointed into the old module/context we just destroyed).
    functionTable.clear();
    dataStructures.clear();
    interfaceTable.clear();
    interfaceParents.clear();
    globalNamedVariable.clear();
    globalVariableTypes.clear();
    namespaceTable.clear();
    typeAliases.clear();
    enumBackingTypes.clear();
    // strConcatRegistered / stringDtorRegistered: will be set below after deserialization
    // verifies the functions are present in the bitcode.

    // Restore symbol tables from JSON.

    // importedFiles - paths may be relative to runtimeDir (new format) or
    // absolute (old format). Resolve relative paths against current runtimeDir
    // so the dedup check works even when Debug and Release builds share a cache.
    if (auto* arr = root->getArray("imported_files"))
        for (auto& elem : *arr)
            if (auto v = elem.getAsString())
            {
                std::filesystem::path p(v->str());
                if (p.is_relative())
                    importedFiles.insert(std::filesystem::weakly_canonical(
                        std::filesystem::path(runtimeDir) / p).string());
                else
                    importedFiles.insert(v->str());
            }

    // namespaceTable
    if (auto* arr = root->getArray("namespaces"))
        for (auto& elem : *arr)
            if (auto v = elem.getAsString()) namespaceTable.insert(v->str());

    // typeAliases
    if (auto* obj = root->getObject("type_aliases"))
        for (auto& kv : *obj)
            if (auto v = kv.second.getAsString()) typeAliases[kv.first.str()] = v->str();

    // enumBackingTypes
    if (auto* obj = root->getObject("enum_backing_types"))
        for (auto& kv : *obj)
            if (auto v = kv.second.getAsString()) enumBackingTypes[kv.first.str()] = v->str();

    // functionTable
    if (auto* arr = root->getArray("functions"))
        for (auto& elem : *arr)
        {
            auto* fo = elem.getAsObject();
            if (!fo) continue;
            auto key  = fo->getString("k");
            auto uniq = fo->getString("u");
            if (!key || !uniq) continue;
            FS sym;
            sym.UniqueName = uniq->str();
            sym.Function   = module->getFunction(sym.UniqueName);
            if (!sym.Function) continue;
            if (auto* r = fo->getObject("r")) sym.ReturnType = DeserializeTav(*r);
            if (auto* ps = fo->getArray("ps"))
                for (auto& pe : *ps)
                    if (auto* po = pe.getAsObject()) sym.Parameters.push_back(DeserializeTav(*po));
            if (auto v = fo->getBoolean("va")) sym.Variadic = *v;
            if (auto v = fo->getBoolean("ro")) sym.ReturnsOwned = *v;
            if (auto v = fo->getBoolean("m"))  sym.IsMethod = *v;
            if (auto* rl = fo->getArray("rl"))
                for (auto& le : *rl)
                    if (auto v = le.getAsString()) sym.RequiredLocks.push_back(v->str());
            functionTable[key->str()].push_back(std::move(sym));
        }

    // dataStructures
    if (auto* arr = root->getArray("structs"))
        for (auto& elem : *arr)
        {
            auto* so = elem.getAsObject();
            if (!so) continue;
            auto name = so->getString("name");
            if (!name) continue;
            std::string sname = name->str();

            LLVMBackend::StructData sd;
            if (auto v = so->getString("llvm_type"))
                sd.StructType = llvm::StructType::getTypeByName(*context, v->str());
            if (!sd.StructType) continue;
            if (auto v = so->getBoolean("is_union"))    sd.IsUnion = *v;
            if (auto v = so->getInteger("user_align"))  sd.UserRequestedAlignment = static_cast<uint64_t>(*v);
            if (auto v = so->getString("destructor"))   sd.Destructor = module->getFunction(v->str());
            if (auto v = so->getString("type_desc"))    sd.typeDescriptor = module->getNamedGlobal(v->str());

            if (auto* fields = so->getArray("fields"))
                for (auto& fe : *fields)
                    if (auto* fo = fe.getAsObject()) sd.StructFields.push_back(DeserializeDtav(*fo));

            if (auto* ifaces = so->getArray("interfaces"))
                for (auto& ie : *ifaces)
                    if (auto v = ie.getAsString()) sd.Interfaces.push_back(v->str());

            if (auto* vtabs = so->getObject("vtables"))
                for (auto& kv : *vtabs)
                    if (auto v = kv.second.getAsString())
                        if (auto* gv = module->getNamedGlobal(v->str()))
                            sd.VTables[kv.first.str()] = gv;

            if (auto* bfs = so->getArray("bitfields"))
                for (auto& be : *bfs)
                    if (auto* bo = be.getAsObject()) sd.Bitfields.push_back(DeserializeBfi(*bo));

            dataStructures[sname] = std::move(sd);
        }

    // interfaceTable + interfaceParents
    if (auto* arr = root->getArray("interfaces"))
        for (auto& elem : *arr)
        {
            auto* io = elem.getAsObject();
            if (!io) continue;
            auto name = io->getString("name");
            if (!name) continue;
            std::string iname = name->str();
            if (auto* parents = io->getArray("parents"))
            {
                std::vector<std::string> plist;
                for (auto& pe : *parents)
                    if (auto v = pe.getAsString()) plist.push_back(v->str());
                interfaceParents[iname] = std::move(plist);
            }
            std::vector<IM> methods;
            if (auto* ms = io->getArray("methods"))
                for (auto& me : *ms)
                    if (auto* mo = me.getAsObject()) methods.push_back(DeserializeIfaceMethod(*mo));
            interfaceTable[iname] = std::move(methods);
        }

    // globalNamedVariable + globalVariableTypes
    if (auto* arr = root->getArray("globals"))
        for (auto& elem : *arr)
        {
            auto* go = elem.getAsObject();
            if (!go) continue;
            auto gname = go->getString("name");
            auto llvmn = go->getString("llvm_name");
            if (!gname || !llvmn) continue;
            auto* gv = module->getNamedGlobal(llvmn->str());
            if (!gv) continue;
            globalNamedVariable[gname->str()] = gv;
            if (auto* to = go->getObject("type"))
                globalVariableTypes[gname->str()] = DeserializeTav(*to);
        }

    // Load generic templates: re-parse source text via SyntheticParseState.
    auto loadGenericTemplates = [&](const char* key,
        auto& templateMap, auto& typeParamsMap, auto& constraintsMap, auto& packIndexMap,
        auto extractCtx)
    {
        auto* arr = root->getArray(key);
        if (!arr) return;
        for (auto& elem : *arr)
        {
            auto* to = elem.getAsObject();
            if (!to) continue;
            auto tname   = to->getString("name");
            auto tsource = to->getString("source");
            if (!tname || !tsource) continue;

            std::vector<std::string> tps;
            if (auto* tpsArr = to->getArray("type_params"))
                for (auto& tp : *tpsArr)
                    if (auto v = tp.getAsString()) tps.push_back(v->str());

            SyntheticParseState state;
            state.label  = "cached:" + tname->str();
            state.input  = std::make_unique<antlr4::ANTLRInputStream>(tsource->str());
            state.lexer  = std::make_unique<CFlatLexer>(state.input.get());
            state.tokens = std::make_unique<antlr4::CommonTokenStream>(state.lexer.get());
            state.parser = std::make_unique<CFlatParser>(state.tokens.get());
            state.parser->removeErrorListeners();
            state.tokens->fill();
            auto* cu = state.parser->compilationUnit();

            auto* ctx = extractCtx(cu);
            if (ctx)
            {
                templateMap[tname->str()] = ctx;
                typeParamsMap[tname->str()] = std::move(tps);
                if (auto* cobj = to->getObject("constraints"))
                    constraintsMap[tname->str()] = DeserializeConstraints(cobj);
                if (auto v = to->getInteger("pack_index"))
                    packIndexMap[tname->str()] = static_cast<size_t>(*v);
            }
            syntheticParseStates_.push_back(std::move(state));
        }
    };

    loadGenericTemplates("generic_structs",
        gts.genericStructTemplates, gts.genericStructTypeParams,
        gts.genericStructConstraints, gts.genericStructPackIndex,
        [](CFlatParser::CompilationUnitContext* cu) -> CFlatParser::StructDefinitionContext* {
            if (!cu || !cu->translationUnit()) return nullptr;
            for (auto* decl : cu->translationUnit()->externalDeclaration())
                if (auto* sd = decl->structDefinition())
                    if (sd->genericTypeParameters()) return sd;
            return nullptr;
        });

    loadGenericTemplates("generic_classes",
        gts.genericClassTemplates, gts.genericStructTypeParams,
        gts.genericClassConstraints, gts.genericClassPackIndex,
        [](CFlatParser::CompilationUnitContext* cu) -> CFlatParser::ClassDefinitionContext* {
            if (!cu || !cu->translationUnit()) return nullptr;
            for (auto* decl : cu->translationUnit()->externalDeclaration())
                if (auto* cd = decl->classDefinition())
                    if (cd->genericTypeParameters()) return cd;
            return nullptr;
        });

    // Interfaces have no constraints map; use a local dummy to satisfy the lambda signature.
    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::string>>> ifaceConstraintsDummy;
    loadGenericTemplates("generic_interfaces",
        gts.genericInterfaceTemplates, gts.genericInterfaceTypeParams,
        ifaceConstraintsDummy, gts.genericInterfacePackIndex,
        [](CFlatParser::CompilationUnitContext* cu) -> CFlatParser::InterfaceDefinitionContext* {
            if (!cu || !cu->translationUnit()) return nullptr;
            for (auto* decl : cu->translationUnit()->externalDeclaration())
                if (auto* id = decl->interfaceDefinition())
                    if (id->genericIdentifier() && id->genericIdentifier()->genericTypeParameters())
                        return id;
            return nullptr;
        });

    loadGenericTemplates("generic_functions",
        gts.genericFunctionTemplates, gts.genericFunctionTypeParams,
        gts.genericFunctionConstraints, gts.genericFunctionPackIndex,
        [](CFlatParser::CompilationUnitContext* cu) -> CFlatParser::FunctionDefinitionContext* {
            if (!cu || !cu->translationUnit()) return nullptr;
            for (auto* decl : cu->translationUnit()->externalDeclaration())
                if (auto* fd = decl->functionDefinition())
                    if (fd->genericTypeParameters()) return fd;
            return nullptr;
        });

    // Restore pre-instantiated generic sets.
    if (auto* arr = root->getArray("instantiated_generics"))
        for (auto& elem : *arr)
            if (auto v = elem.getAsString()) gts.instantiatedGenerics.insert(v->str());
    if (auto* arr = root->getArray("instantiated_interfaces"))
        for (auto& elem : *arr)
            if (auto v = elem.getAsString()) gts.instantiatedInterfaces.insert(v->str());
    if (auto* arr = root->getArray("instantiated_generic_functions"))
        for (auto& elem : *arr)
            if (auto v = elem.getAsString()) gts.instantiatedGenericFunctions.insert(v->str());

    // Mark lazy-registered functions as done if they were deserialized from the cache.
    // This prevents EnsureStrConcatRegistered / EnsureStringDtorRegistered from creating
    // duplicate LLVM functions that conflict with the ones already in the loaded bitcode.
    if (!functionTable["__strconcat"].empty()) strConcatRegistered = true;
    if (dataStructures.count("string") && dataStructures["string"].Destructor) stringDtorRegistered = true;

    return true;
}

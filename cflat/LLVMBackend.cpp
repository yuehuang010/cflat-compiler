#pragma warning(push)
#pragma warning(disable: 4244 4267)
#include <llvm/IR/IRBuilder.h>
#include <llvm/TargetParser/AArch64TargetParser.h>  // AArch64::CpuAliases + StrTab (--print-supported-cpus)
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/Dominators.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Transforms/Utils/Mem2Reg.h>
#include <llvm/Transforms/Scalar/SROA.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Transforms/IPO/GlobalDCE.h>
#include <llvm/Transforms/Instrumentation/AddressSanitizer.h>
#include <llvm/Object/COFF.h>
#include <llvm/Object/Binary.h>
#include <llvm/Object/Archive.h>
#include <llvm/Object/COFFImportFile.h>
#include <llvm/Object/MachO.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/TimeProfiler.h>
#include <llvm/Support/SHA256.h>
#include <llvm/Support/JSON.h>
#include <llvm/IR/DiagnosticInfo.h>
#include <llvm/IR/DiagnosticHandler.h>
#include <llvm/IR/Intrinsics.h>
#pragma warning(pop)
#include <antlr4-runtime.h>

#include "platform/GeneratedParser.h"
#include "LLVMBackend.h"
#include "MainListener.h"
#include "GrammarTreeListener.h"
#include "Version.h"
#include <filesystem>
#include <optional>
#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <fstream>
#include <nlohmann/json.hpp>

#if defined(__APPLE__)
// Step 3 (macOS self-contained link): harvest libSystem's exported symbols from
// the live dyld shared cache to synthesize a linker stub, so -o needs no SDK.
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <dlfcn.h>
#include <cstring>
#include <sys/sysctl.h>
#endif

// Win32 unwind registration for the --run JIT (see cflat_jit::SehRegistrationPlugin). Declared
// here, not in LLVMBackend.h, so this stays in a TU that never includes <winnt.h> - otherwise
// the extern "C" decl would clash with the real prototype in TUs that pull in windows.h. x64
// has a single calling convention, so the bare signature links to kernel32's export directly.
#if defined(_WIN32)
extern "C" unsigned char RtlAddFunctionTable(
    void* FunctionTable, unsigned long EntryCount, unsigned long long BaseAddress);
#endif

llvm::Error cflat_jit::SehRegistrationPlugin::RegisterUnwindInfo(llvm::jitlink::LinkGraph& G)
{
    char nosehBuf[16]; size_t nosehLen = 0;
    if (getenv_s(&nosehLen, nosehBuf, sizeof(nosehBuf), "CFLAT_JIT_NOSEH") == 0 && nosehLen > 0)
        return llvm::Error::success(); // DIAG: skip all unwind-table registration

    auto* pdata = G.findSectionByName(".pdata");
    if (!pdata)
        return llvm::Error::success(); // no unwind entries (e.g. all-leaf code)

    // The .pdata RUNTIME_FUNCTION RVAs were baked relative to __ImageBase by the COFF lowering
    // pass, and FixImageBase pinned __ImageBase to the lowest block. Use that same base so the
    // OS resolves base + RVA back to the real JIT'd addresses. (We must use the lowest block
    // rather than reverse-engineering a relocation: lowering rewrites the .pdata edge addends to
    // be base-relative, so the old "imageBase = target - storedRVA" heuristic would yield 0.)
    llvm::jitlink::Block* lowest = LowestBlock(G);
    if (!lowest)
        return llvm::Error::success(); // empty graph
    uint64_t imageBase = lowest->getAddress().getValue();

    // RUNTIME_FUNCTION: { BeginAddress, EndAddress, UnwindInfoAddress } - three 4-byte RVAs.
    struct RuntimeFunction { uint32_t begin, end, unwind; };
    constexpr size_t kRuntimeFunctionSize = sizeof(RuntimeFunction);
    char diagBuf[16]; size_t diagEnvLen = 0;
    const bool diag = (getenv_s(&diagEnvLen, diagBuf, sizeof(diagBuf), "CFLAT_JIT_DIAG") == 0 && diagEnvLen > 0);

    for (auto* B : pdata->blocks())
    {
        size_t count = (size_t)B->getSize() / kRuntimeFunctionSize;
        if (count == 0) continue;

        // Windows requires each dynamic function table to be sorted by BeginAddress
        // (RtlLookupFunctionEntry binary-searches it). LLVM emits .pdata in function order, but
        // JITLink may lay the functions out in a different order, leaving the post-fixup RVAs
        // unsorted - which makes worker-thread SEH dispatch silently miss and crash (notably
        // under the Release LLVM layout). Sort the table in working memory before registering;
        // the sorted bytes are what finalization copies to the executable address we register.
        auto content = B->getMutableContent(G);
        auto* rf = reinterpret_cast<RuntimeFunction*>(content.data());
        bool wasSorted = std::is_sorted(rf, rf + count,
            [](const RuntimeFunction& a, const RuntimeFunction& b) { return a.begin < b.begin; });
        if (!wasSorted)
            std::sort(rf, rf + count,
                [](const RuntimeFunction& a, const RuntimeFunction& b) { return a.begin < b.begin; });
        if (diag)
            fprintf(stderr, "[jitdiag] .pdata block @%p count=%zu base=%llx sorted=%d\n",
                    B->getAddress().toPtr<void*>(), count, (unsigned long long)imageBase, (int)wasSorted);

        void* funcs = B->getAddress().toPtr<void*>();
#if defined(_WIN32)
        if (!RtlAddFunctionTable(funcs, (unsigned long)count, imageBase))
            return llvm::make_error<llvm::StringError>(
                "RtlAddFunctionTable failed registering JIT'd .pdata unwind info",
                llvm::inconvertibleErrorCode());
#else
        (void)funcs; // SEH .pdata registration is Windows-only.
#endif
    }
    return llvm::Error::success();
}

// Returns the lowercased extension of a path (e.g. ".CB" -> ".cb").
static std::string LowerExtension(const std::filesystem::path& p)
{
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return ext;
}

namespace {

std::filesystem::path DependencyManifestPath(const std::filesystem::path& output)
{
    std::error_code ec;
    if (std::filesystem::is_directory(output, ec))
        return output / "cflat-dep.json";
    auto manifest = output;
    manifest.replace_extension(".cflat-dep.json");
    return manifest;
}

std::filesystem::path ExistingAbsolutePath(const std::filesystem::path& path,
                                            std::error_code& ec)
{
    auto result = std::filesystem::weakly_canonical(path, ec);
    if (ec)
        result = std::filesystem::absolute(path, ec);
    return result;
}

bool ReadFileStamp(const std::filesystem::path& path, int64_t& mtime, int64_t& size)
{
    std::error_code ec;
    auto mt = std::filesystem::last_write_time(path, ec);
    if (ec) return false;
    auto bytes = std::filesystem::file_size(path, ec);
    if (ec) return false;
    mtime = static_cast<int64_t>(mt.time_since_epoch().count());
    size = static_cast<int64_t>(bytes);
    return true;
}

} // namespace

void LLVMBackend::RecordDependency(const std::string& path)
{
    if (path.empty()) return;

    auto cached = dependencyPathMemo_.find(path);
    if (cached != dependencyPathMemo_.end())
    {
        if (cached->second.empty()) return;
        if (dependencyFileSet_.insert(cached->second).second)
            dependencyFiles_.push_back(cached->second);
        return;
    }

    std::error_code ec;
    auto absolute = ExistingAbsolutePath(path, ec);
    if (ec || !std::filesystem::is_regular_file(absolute, ec) || ec)
    {
        dependencyPathMemo_.emplace(path, "");
        return;
    }
    auto text = absolute.string();
    dependencyPathMemo_.emplace(path, text);
    if (dependencyFileSet_.insert(text).second)
        dependencyFiles_.push_back(std::move(text));
}

bool LLVMBackend::IsOutputUpToDate(const std::string& outputPath,
                                   const std::vector<std::string>& normalizedArgs)
{
    std::filesystem::path output(outputPath);
    std::filesystem::path manifestPath = DependencyManifestPath(output);
    std::error_code ec;
    if (!std::filesystem::exists(output, ec) || ec
        || !std::filesystem::exists(manifestPath, ec) || ec)
        return false;

    nlohmann::json manifest;
    try
    {
        std::ifstream input(manifestPath, std::ios::binary);
        if (!input) return false;
        input >> manifest;
        if (manifest.value("format", "") != "cflat-dep"
            || manifest.value("format_version", 0) != 1
            || !manifest.contains("compiler") || !manifest.contains("output")
            || !manifest.contains("args") || !manifest.contains("inputs"))
            return false;
        if (manifest.at("args").get<std::vector<std::string>>() != normalizedArgs)
            return false;

        std::error_code pathEc;
        auto compilerPath = ExistingAbsolutePath(PlatformExePath(), pathEc);
        if (pathEc || manifest.at("compiler").at("path").get<std::string>()
                != compilerPath.string())
            return false;
        int64_t compilerMtime = 0, compilerSize = 0;
        if (!ReadFileStamp(compilerPath, compilerMtime, compilerSize)
            || manifest.at("compiler").at("mtime").get<int64_t>() != compilerMtime
            || manifest.at("compiler").at("size").get<int64_t>() != compilerSize)
            return false;

        int64_t outputMtime = 0, outputSize = 0;
        if (!ReadFileStamp(output, outputMtime, outputSize)
            || outputMtime < manifest.at("output").at("mtime").get<int64_t>())
            return false;

        for (const auto& entry : manifest.at("inputs"))
        {
            auto path = entry.at("path").get<std::string>();
            int64_t mtime = 0, size = 0;
            if (!ReadFileStamp(path, mtime, size)
                || mtime != entry.at("mtime").get<int64_t>()
                || size != entry.at("size").get<int64_t>())
                return false;
        }
    }
    catch (...) { return false; }
    return true;
}

bool LLVMBackend::WriteDependencyManifest(const std::string& outputPath,
                                          const std::vector<std::string>& normalizedArgs,
                                          const std::vector<std::string>& dependencyFiles)
{
    std::error_code ec;
    auto output = ExistingAbsolutePath(outputPath, ec);
    if (ec) return false;
    auto compiler = ExistingAbsolutePath(PlatformExePath(), ec);
    if (ec) return false;
    int64_t outputMtime = 0, outputSize = 0;
    int64_t compilerMtime = 0, compilerSize = 0;
    if (!ReadFileStamp(output, outputMtime, outputSize)
        || !ReadFileStamp(compiler, compilerMtime, compilerSize))
        return false;

    nlohmann::json manifest;
    manifest["format"] = "cflat-dep";
    manifest["format_version"] = 1;
    manifest["compiler"] = { {"path", compiler.string()},
                              {"mtime", compilerMtime}, {"size", compilerSize} };
    manifest["output"] = { {"path", output.string()}, {"mtime", outputMtime} };
    manifest["args"] = normalizedArgs;
    manifest["inputs"] = nlohmann::json::array();
    for (const auto& dependency : dependencyFiles)
    {
        auto path = ExistingAbsolutePath(dependency, ec);
        if (ec) return false;
        int64_t mtime = 0, size = 0;
        if (!ReadFileStamp(path, mtime, size)) return false;
        manifest["inputs"].push_back({ {"path", path.string()},
                                       {"mtime", mtime}, {"size", size} });
    }

    auto manifestPath = DependencyManifestPath(outputPath);
    std::ofstream file(manifestPath, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file << manifest.dump() << '\n';
    return static_cast<bool>(file);
}

static bool IsPathUnderDirectory(const std::filesystem::path& path,
                                 const std::filesystem::path& directory)
{
    std::error_code ec;
    auto pathText = std::filesystem::weakly_canonical(path, ec).generic_string();
    if (ec) return false;
    auto directoryText = std::filesystem::weakly_canonical(directory, ec).generic_string();
    if (ec) return false;
    if (directoryText.empty() || pathText.size() <= directoryText.size()) return false;
    return pathText.compare(0, directoryText.size(), directoryText) == 0
        && pathText[directoryText.size()] == '/';
}

// LLVM data layout string for the target platform (32 = win32, else win64). Both little-endian MSVC.
static const char* PlatformDataLayout(int platformValue)
{
    return (platformValue == 32)
        ? "e-m:x-p:32:32-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:32-n8:16:32-S32"
        : "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128";
}

// Inline `lib "..."` clause(s) on an import line. Returns every named import library in
// source order - one for the `lib "x.lib"` form, several for the `lib { "a", "b" }` brace
// form. Empty when the import carries no lib clause. Each is resolved to a link argument
// by ResolveCLinkLib at the header-bind site.
static std::vector<antlr4::tree::TerminalNode*> ImportClauseStringLiterals(
    CFlatParser::ImportDeclarationContext* imp, const char* word)
{
    std::vector<antlr4::tree::TerminalNode*> literals;
    ForEachImportClause(imp, [&](antlr4::ParserRuleContext* clause) {
        if (!ImportClauseHasWord(clause, word)) return;
        if (auto* c = dynamic_cast<CFlatParser::LibClauseContext*>(clause))
            for (auto* lit : c->StringLiteral()) literals.push_back(lit);
        else if (auto* c = dynamic_cast<CFlatParser::FrameworkClauseContext*>(clause))
            for (auto* lit : c->StringLiteral()) literals.push_back(lit);
        else if (auto* c = dynamic_cast<CFlatParser::DefineClauseContext*>(clause))
        {
            if (c->StringLiteral()) literals.push_back(c->StringLiteral());
        }
        else if (auto* c = dynamic_cast<CFlatParser::FromClauseContext*>(clause))
        {
            if (c->StringLiteral()) literals.push_back(c->StringLiteral());
        }
        else if (auto* c = dynamic_cast<CFlatParser::PriClauseContext*>(clause))
        {
            if (c->StringLiteral()) literals.push_back(c->StringLiteral());
        }
    });
    return literals;
}

static std::vector<std::string> DequoteLibClauses(CFlatParser::ImportDeclarationContext* imp)
{
    std::vector<std::string> libs;
    for (auto* lit : ImportClauseStringLiterals(imp, "lib"))
        libs.push_back(DequoteStringLiteral(lit->getText()));
    return libs;
}

// Inline `framework "..."` clause(s) on a header/package import (S3). Returns every
// named macOS framework in source order (one for `framework "X"`, several for the brace
// form). Empty when absent. Each routes through AddFrameworkImport (-> -framework at link).
static std::vector<std::string> DequoteFrameworkClauses(CFlatParser::ImportDeclarationContext* imp)
{
    std::vector<std::string> fws;
    for (auto* lit : ImportClauseStringLiterals(imp, "framework"))
        fws.push_back(DequoteStringLiteral(lit->getText()));
    return fws;
}

// Inline `define "..."` clauses on an `import package` line. Each is scoped to
// that header's clang AST dump and appended on top of the process-wide --c-define.
static std::vector<std::string> DequoteDefineClauses(CFlatParser::ImportDeclarationContext* imp)
{
    std::vector<std::string> defines;
    for (auto* lit : ImportClauseStringLiterals(imp, "define"))
    {
        std::string raw = lit->getText();
        if (raw.size() < 2) continue;
        defines.push_back(DequoteStringLiteral(raw));
    }
    return defines;
}

// True when this import line carries an inline `cache` clause, opting the header into the
// persistent C-header disk cache. Applies to the bare-header and `import package` forms.
static bool HasCacheClause(CFlatParser::ImportDeclarationContext* imp)
{
    bool found = false;
    ForEachImportClause(imp, [&](antlr4::ParserRuleContext* clause) {
        if (ImportClauseHasWord(clause, "cache")) found = true;
    });
    return found;
}

// Dequoted filenames from a grouped import `import { "a", "b" };`, in source order.
// Empty when this import is not the grouped form (so callers can fall through to the
// single-StringLiteral handling). Each entry is a plain import: no alias/lib/define/cache.
static std::vector<std::string> DequoteImportGroup(CFlatParser::ImportDeclarationContext* imp)
{
    std::vector<std::string> out;
    auto* grp = imp->importGroup();
    if (grp != nullptr)
    {
        for (auto* lit : grp->StringLiteral())
            out.push_back(DequoteStringLiteral(lit->getText()));
        return out;
    }
    // Once `framework` is a soft identifier, its standalone form is parsed by the
    // direct-string alternative rather than the old literal-specific alternative.
    if (imp->children.size() >= 2 && imp->children[1]->getText() == "framework")
        if (auto* lit = imp->StringLiteral())
            out.push_back(DequoteStringLiteral(lit->getText()));
    return out;
}

// True when this import line is `import package-vcpkg "..." from "...";`. Detected by
// the second child token text - mirrors the existing `package` / `program` test.
static bool IsPackageVcpkgImport(CFlatParser::ImportDeclarationContext* imp)
{
    return imp->children.size() >= 2 && imp->children[1]->getText() == "package-vcpkg";
}

// True when this import line is `import package-nuget "..." from "...";`. Mirrors
// IsPackageVcpkgImport - detected by the second child token text.
static bool IsPackageNugetImport(CFlatParser::ImportDeclarationContext* imp)
{
    return imp->children.size() >= 2 && imp->children[1]->getText() == "package-nuget";
}

// True when this import line is `import framework "X";` / `import framework { ... };`.
// Detected by the second child token text - mirrors the package/program tests.
static bool IsFrameworkImport(CFlatParser::ImportDeclarationContext* imp)
{
    return imp->children.size() >= 2 && imp->children[1]->getText() == "framework";
}

static bool IsIsolatedNativeImport(CFlatParser::ImportDeclarationContext* imp)
{
    return IsPackageVcpkgImport(imp) || IsPackageNugetImport(imp) || IsFrameworkImport(imp)
        || !DequoteLibClauses(imp).empty() || !DequoteFrameworkClauses(imp).empty()
        || !DequoteDefineClauses(imp).empty();
}

static bool IsAllowedIsolatedIntrinsic(llvm::StringRef name)
{
    static constexpr std::array<llvm::StringRef, 28> prefixes = {
        "llvm.memcpy.", "llvm.memmove.", "llvm.memset.", "llvm.lifetime.", "llvm.dbg.",
        "llvm.experimental.noalias.", "llvm.sadd.with.overflow.", "llvm.uadd.with.overflow.",
        "llvm.ssub.with.overflow.", "llvm.usub.with.overflow.", "llvm.smul.with.overflow.",
        "llvm.umul.with.overflow.", "llvm.fabs.", "llvm.sqrt.", "llvm.pow.", "llvm.sin.",
        "llvm.cos.", "llvm.tan.", "llvm.exp.", "llvm.exp2.", "llvm.log.", "llvm.log2.",
        "llvm.log10.", "llvm.round.", "llvm.floor.", "llvm.ceil.", "llvm.trunc.", "llvm.minnum."};
    for (auto prefix : prefixes)
        if (name.starts_with(prefix)) return true;
    static constexpr std::array<llvm::StringRef, 23> exactOrFamilies = {
        "llvm.assume", "llvm.expect", "llvm.trap", "llvm.eh.typeid.for", "llvm.stacksave",
        "llvm.stackrestore", "llvm.readcyclecounter", "llvm.va_start", "llvm.va_end", "llvm.fma.",
        "llvm.copysign.", "llvm.maxnum.", "llvm.ctlz.", "llvm.ctpop.", "llvm.cttz.", "llvm.fshl.",
        "llvm.masked.", "llvm.prefetch.", "llvm.vector.reduce.", "llvm.invariant.start.",
        "llvm.invariant.end.", "llvm.ptrmask.", "llvm.is.constant."};
    for (auto prefix : exactOrFamilies)
        if (name == prefix || name.starts_with(prefix)) return true;
    return false;
}

// Dequoted port spec from `from "..."` on an import package-vcpkg line (empty if absent).
static std::string DequoteFromClause(CFlatParser::ImportDeclarationContext* imp)
{
    auto literals = ImportClauseStringLiterals(imp, "from");
    return literals.empty() ? "" : DequoteStringLiteral(literals.front()->getText());
}

// Dequoted .pri filename from `pri "..."` on an import package-nuget line (empty if
// absent). The named .pri is deployed next to the exe as <exe>.pri.
static std::string DequotePriClause(CFlatParser::ImportDeclarationContext* imp)
{
    auto literals = ImportClauseStringLiterals(imp, "pri");
    return literals.empty() ? "" : DequoteStringLiteral(literals.front()->getText());
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

bool LLVMBackend::LoadIsolatedPolicy(const std::string& path)
{
    IsolatedPolicy policy;
    std::string error;
    if (!ParseIsolatedPolicyFile(path, policy, error))
    {
        sourceFileName = path;
        // Parser errors that already carry a category keep it; others are policy-invalid.
        if (error.rfind("policy-", 0) == 0)
            LogError(std::format("{} (policy '{}')", error, path));
        else
            LogError(std::format("policy-invalid: {} in policy '{}'", error, path));
        return false;
    }
    isolatedPolicy_ = std::move(policy);
    sourceFileName = path;
    return true;
}

void LLVMBackend::SetIsolatedPolicy(const IsolatedPolicy& policy)
{
    isolatedPolicy_ = policy;
}

std::optional<IsolatedPolicy> LLVMBackend::GetIsolatedPolicy() const
{
    return isolatedPolicy_;
}

bool LLVMBackend::IsIsolated() const
{
    return isolatedPolicy_.has_value();
}

bool LLVMBackend::CurrentSourceIsCoreLibrary() const
{
    // The core-template ORIGIN marker must not depend on which file the caller happened to make
    // the root: an LSP re-analysis of `core/unique.cb` itself analyzes it as user code, which
    // would otherwise erase `unique` from the core-template set for every later analysis.
    if (currentSourceIsCore_) return true;
    return !runtimeDir.empty()
        && IsPathUnderDirectory(currentSourceFilePath_, std::filesystem::path(runtimeDir) / "core");
}

bool LLVMBackend::IsIsolatedCoreSource() const
{
    return isolatedPolicy_.has_value() && !currentSourceIsCore_ && !runtimeDir.empty()
        && IsPathUnderDirectory(currentSourceFilePath_, std::filesystem::path(runtimeDir) / "core");
}

void LLVMBackend::ReportIsolatedPolicyError(const std::string& message) const
{
    LogError(message);
}

void LLVMBackend::ValidateIsolatedExtern(CFlatParser::DeclarationContext* ctx)
{
    if (!isolatedPolicy_ || currentSourceIsCore_ || IsIsolatedCoreSource() || ctx == nullptr) return;
    currentLine = ctx->getStart()->getLine();
    currentColumn = ctx->getStart()->getCharPositionInLine();
    LogError(std::format("policy-restricted-language: user extern declarations are not allowed "
                         "in isolated mode (policy '{}')", isolatedPolicy_->path));
}

void LLVMBackend::ValidateIsolatedParseTree(antlr4::tree::ParseTree* tree)
{
    if (!isolatedPolicy_ || currentSourceIsCore_ || IsIsolatedCoreSource() || tree == nullptr) return;
    if (auto* program = dynamic_cast<CFlatParser::ProgramDefinitionContext*>(tree))
    {
        ValidateIsolatedProgram(program);
        return;
    }
    if (auto* declaration = dynamic_cast<CFlatParser::DeclarationContext*>(tree))
    {
        auto* specs = declaration->declarationSpecifiers();
        if (specs)
            for (auto* spec : specs->declarationSpecifier())
                if (auto* storage = spec->storageClassSpecifier(); storage && storage->Extern() != nullptr)
                {
                    ValidateIsolatedExtern(declaration);
                    break;
                }
    }
    for (auto* child : tree->children)
        ValidateIsolatedParseTree(child);
}

bool LLVMBackend::InstrumentIsolatedResources()
{
    if (!isolatedPolicy_) return true;

    auto& ctx = *context;
    auto* i1 = llvm::Type::getInt1Ty(ctx);
    auto* i8 = llvm::Type::getInt8Ty(ctx);
    auto* i32 = llvm::Type::getInt32Ty(ctx);
    auto* i64 = llvm::Type::getInt64Ty(ctx);
    auto* ptr = llvm::PointerType::get(ctx, 0);
    auto* voidTy = llvm::Type::getVoidTy(ctx);
    auto makeFunction = [&](llvm::StringRef name, llvm::FunctionType* type) {
        return llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage, name, module.get());
    };
    auto getExternal = [&](llvm::StringRef name) -> llvm::Function* {
        auto* function = module->getFunction(name);
        return function && function->isDeclaration() ? function : nullptr;
    };

    llvm::Function* realMalloc = getExternal("malloc");
    llvm::Function* realCalloc = getExternal("calloc");
    llvm::Function* realRealloc = getExternal("realloc");
    llvm::Function* realFree = getExternal("free");
    llvm::Function* realPosixMemalign = getExternal("posix_memalign");
    llvm::Function* heapMalloc = nullptr;
    llvm::Function* heapFree = nullptr;

    auto makeFailure = [&](llvm::StringRef name, llvm::StringRef message) {
        auto* function = makeFunction(name, llvm::FunctionType::get(voidTy, {}, false));
        auto* block = llvm::BasicBlock::Create(ctx, "entry", function);
        llvm::IRBuilder<> b(block);
        auto* messageType = llvm::ArrayType::get(i8, message.size() + 1);
        auto* messageValue = llvm::ConstantDataArray::getString(ctx, message, true);
        auto* messageGlobal = new llvm::GlobalVariable(
            *module, messageType, true, llvm::GlobalValue::PrivateLinkage, messageValue,
            name + ".message");
        messageGlobal->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
        // Stdio is optional: a denied policy uses abort without importing write.
        if (isolatedPolicy_->allow[static_cast<size_t>(IsolatedCapability::Stdio)])
        {
            auto* writeType = targetWindows_
                ? llvm::FunctionType::get(i32, {i32, ptr, i32}, false)
                : llvm::FunctionType::get(i64, {i32, ptr, i64}, false);
            auto writeName = targetWindows_ ? "_write" : "write";
            auto* write = module->getFunction(writeName);
            if (!write)
                write = llvm::Function::Create(writeType, llvm::GlobalValue::ExternalLinkage,
                                               writeName, module.get());
            auto* messagePtr = b.CreateInBoundsGEP(messageType, messageGlobal,
                                                   {b.getInt64(0), b.getInt64(0)});
            llvm::Value* messageLength = targetWindows_
                ? static_cast<llvm::Value*>(b.getInt32(static_cast<uint32_t>(message.size())))
                : static_cast<llvm::Value*>(b.getInt64(message.size()));
            b.CreateCall(writeType, write, {b.getInt32(2), messagePtr, messageLength});
        }
        auto* abortType = llvm::FunctionType::get(voidTy, {}, false);
        auto* abort = module->getFunction("abort");
        if (!abort)
            abort = llvm::Function::Create(abortType, llvm::GlobalValue::ExternalLinkage,
                                           "abort", module.get());
        b.CreateCall(abortType, abort, {});
        b.CreateUnreachable();
        return function;
    };

    auto makeReserve = [&](llvm::StringRef name, llvm::GlobalVariable* counter,
                           uint64_t limit, llvm::Function* failure) {
        auto* function = makeFunction(name, llvm::FunctionType::get(i1, {i64}, false));
        auto* entry = llvm::BasicBlock::Create(ctx, "entry", function);
        auto* retry = llvm::BasicBlock::Create(ctx, "retry", function);
        auto* cas = llvm::BasicBlock::Create(ctx, "cas", function);
        auto* success = llvm::BasicBlock::Create(ctx, "success", function);
        auto* failed = llvm::BasicBlock::Create(ctx, "failed", function);
        llvm::IRBuilder<> b(entry);
        b.CreateBr(retry);
        b.SetInsertPoint(retry);
        auto* current = b.CreateLoad(i64, counter, "current");
        current->setAtomic(llvm::AtomicOrdering::Monotonic);
        auto* cap = b.getInt64(limit);
        auto* remaining = b.CreateSub(cap, current, "remaining");
        auto* tooHigh = b.CreateICmpUGT(current, cap);
        auto* tooMuch = b.CreateICmpUGT(function->getArg(0), remaining);
        b.CreateCondBr(b.CreateOr(tooHigh, tooMuch), failed, cas);
        b.SetInsertPoint(cas);
        auto* desired = b.CreateAdd(current, function->getArg(0), "desired");
        auto* exchange = b.CreateAtomicCmpXchg(counter, current, desired, llvm::MaybeAlign(),
                                               llvm::AtomicOrdering::AcquireRelease,
                                               llvm::AtomicOrdering::Monotonic);
        auto* exchanged = b.CreateExtractValue(exchange, 1, "reserved");
        b.CreateCondBr(exchanged, success, retry);
        b.SetInsertPoint(success);
        b.CreateRet(b.getTrue());
        b.SetInsertPoint(failed);
        b.CreateCall(failure, {});
        b.CreateUnreachable();
        return function;
    };

    auto makeRelease = [&](llvm::StringRef name, llvm::GlobalVariable* counter) {
        auto* function = makeFunction(name, llvm::FunctionType::get(voidTy, {i64}, false));
        auto* entry = llvm::BasicBlock::Create(ctx, "entry", function);
        llvm::IRBuilder<> b(entry);
        b.CreateAtomicRMW(llvm::AtomicRMWInst::Sub, counter, function->getArg(0),
                          llvm::MaybeAlign(), llvm::AtomicOrdering::Monotonic);
        b.CreateRetVoid();
        return function;
    };
    auto makeSubtractRelease = [&](llvm::StringRef name, llvm::GlobalVariable* counter) {
        auto* function = makeFunction(name, llvm::FunctionType::get(voidTy, {i64}, false));
        auto* entry = llvm::BasicBlock::Create(ctx, "entry", function);
        llvm::IRBuilder<> b(entry);
        b.CreateAtomicRMW(llvm::AtomicRMWInst::Sub, counter, function->getArg(0),
                          llvm::MaybeAlign(), llvm::AtomicOrdering::Monotonic);
        b.CreateRetVoid();
        return function;
    };

    llvm::StructType* headerType = nullptr;
    llvm::GlobalVariable* heapCounter = nullptr;
    llvm::Function* heapReserve = nullptr;
    llvm::Function* heapRelease = nullptr;
    if (isolatedPolicy_->heapBytes)
    {
        headerType = llvm::StructType::create(ctx, {i64, ptr}, "__cflat_isolated_heap_header");
        heapCounter = new llvm::GlobalVariable(
            *module, i64, false, llvm::GlobalValue::InternalLinkage,
            llvm::ConstantInt::get(i64, 0), "__cflat_isolated_heap_live");
        heapCounter->setAlignment(llvm::Align(8));
        auto* heapFailure = makeFailure("__cflat_isolated_heap_fail",
                                         "isolated runtime error: restricted heap cap exceeded\n");
        heapReserve = makeReserve("__cflat_isolated_heap_reserve", heapCounter,
                                  *isolatedPolicy_->heapBytes, heapFailure);
        heapRelease = makeRelease("__cflat_isolated_heap_release", heapCounter);
    }

    if (heapCounter && realFree && !realMalloc)
        LogError(std::format("policy-module-denied: isolated heap accounting requires malloc "
                             "when free is present in policy '{}'", isolatedPolicy_->path));

    std::vector<std::pair<llvm::Function*, llvm::Function*>> replacements;
    std::vector<std::pair<llvm::CallBase*, llvm::Function*>> realCalls;
    auto headerFor = [&](llvm::IRBuilder<>& b, llvm::Value* user) {
        auto* bytes = b.CreateConstInBoundsGEP1_64(i8, user, -16);
        return b.CreateBitCast(bytes, cflat_llvm::PointerTo(headerType));
    };
    auto headerSize = [&](llvm::IRBuilder<>& b, llvm::Value* header) {
        return b.CreateStructGEP(headerType, header, 0);
    };
    auto headerRaw = [&](llvm::IRBuilder<>& b, llvm::Value* header) {
        return b.CreateStructGEP(headerType, header, 1);
    };
    auto addUserPointer = [&](llvm::IRBuilder<>& b, llvm::Value* raw) {
        return b.CreateConstInBoundsGEP1_64(i8, raw, 16);
    };
    const uint64_t maxPayload = std::numeric_limits<uint64_t>::max() - 16;

    if (heapCounter && realMalloc && realMalloc->getFunctionType()->getNumParams() == 1)
    {
        heapMalloc = makeFunction("__cflat_isolated_malloc", realMalloc->getFunctionType());
        auto* entry = llvm::BasicBlock::Create(ctx, "entry", heapMalloc);
        auto* fail = llvm::BasicBlock::Create(ctx, "overflow", heapMalloc);
        auto* rawCall = llvm::BasicBlock::Create(ctx, "allocate", heapMalloc);
        auto* null = llvm::BasicBlock::Create(ctx, "null", heapMalloc);
        llvm::IRBuilder<> b(entry);
        auto* size = heapMalloc->getArg(0);
        b.CreateCondBr(b.CreateICmpUGT(size, b.getInt64(maxPayload)), fail, rawCall);
        b.SetInsertPoint(fail);
        auto* failFn = module->getFunction("__cflat_isolated_heap_fail");
        b.CreateCall(failFn, {});
        b.CreateUnreachable();
        b.SetInsertPoint(rawCall);
        b.CreateCall(heapReserve, {size});
        auto* total = b.CreateAdd(size, b.getInt64(16));
        auto* rawCallInst = b.CreateCall(realMalloc->getFunctionType(), realMalloc, {total});
        realCalls.emplace_back(rawCallInst, realMalloc);
        auto* raw = rawCallInst;
        b.CreateCondBr(b.CreateICmpEQ(raw, llvm::ConstantPointerNull::get(ptr)), null,
                       llvm::BasicBlock::Create(ctx, "store", heapMalloc));
        auto* store = cflat_llvm::GetTerminatorOrNull(b.GetInsertBlock())
                          ->getSuccessor(1);
        b.SetInsertPoint(null);
        b.CreateCall(heapRelease, {size});
        b.CreateRet(llvm::ConstantPointerNull::get(ptr));
        b.SetInsertPoint(store);
        auto* header = b.CreateBitCast(raw, cflat_llvm::PointerTo(headerType));
        b.CreateStore(size, headerSize(b, header));
        b.CreateStore(raw, headerRaw(b, header));
        b.CreateRet(addUserPointer(b, raw));
        replacements.emplace_back(realMalloc, heapMalloc);
    }

    if (heapCounter && heapMalloc && realFree && realFree->getFunctionType()->getNumParams() == 1)
    {
        heapFree = makeFunction("__cflat_isolated_free", realFree->getFunctionType());
        auto* entry = llvm::BasicBlock::Create(ctx, "entry", heapFree);
        auto* done = llvm::BasicBlock::Create(ctx, "done", heapFree);
        llvm::IRBuilder<> b(entry);
        auto* user = heapFree->getArg(0);
        b.CreateCondBr(b.CreateICmpEQ(user, llvm::ConstantPointerNull::get(ptr)), done,
                       llvm::BasicBlock::Create(ctx, "release", heapFree));
        auto* release = cflat_llvm::GetTerminatorOrNull(b.GetInsertBlock())
                            ->getSuccessor(1);
        b.SetInsertPoint(release);
        auto* header = headerFor(b, user);
        auto* size = b.CreateLoad(i64, headerSize(b, header));
        auto* raw = b.CreateLoad(ptr, headerRaw(b, header));
        auto* freeCall = b.CreateCall(realFree->getFunctionType(), realFree, {raw});
        realCalls.emplace_back(freeCall, realFree);
        b.CreateCall(heapRelease, {size});
        b.CreateBr(done);
        b.SetInsertPoint(done);
        b.CreateRetVoid();
        replacements.emplace_back(realFree, heapFree);
    }

    if (heapCounter && realCalloc && heapMalloc
        && realCalloc->getFunctionType()->getNumParams() == 2)
    {
        auto* wrapper = makeFunction("__cflat_isolated_calloc", realCalloc->getFunctionType());
        auto* entry = llvm::BasicBlock::Create(ctx, "entry", wrapper);
        auto* overflow = llvm::BasicBlock::Create(ctx, "overflow", wrapper);
        auto* call = llvm::BasicBlock::Create(ctx, "call", wrapper);
        auto* null = llvm::BasicBlock::Create(ctx, "null", wrapper);
        llvm::IRBuilder<> b(entry);
        auto* product = b.CreateIntrinsic(llvm::Intrinsic::umul_with_overflow,
                                          {i64}, {wrapper->getArg(0), wrapper->getArg(1)});
        auto* productValue = b.CreateExtractValue(product, 0);
        auto* productOverflow = b.CreateExtractValue(product, 1);
        b.CreateCondBr(b.CreateOr(productOverflow,
                                  b.CreateICmpUGT(productValue, b.getInt64(maxPayload))),
                       overflow, call);
        b.SetInsertPoint(overflow);
        b.CreateCall(module->getFunction("__cflat_isolated_heap_fail"), {});
        b.CreateUnreachable();
        b.SetInsertPoint(call);
        b.CreateCall(heapReserve, {productValue});
        auto* total = b.CreateAdd(productValue, b.getInt64(16));
        auto* rawCallInst = b.CreateCall(realCalloc->getFunctionType(), realCalloc,
                                         {b.getInt64(1), total});
        realCalls.emplace_back(rawCallInst, realCalloc);
        auto* raw = rawCallInst;
        b.CreateCondBr(b.CreateICmpEQ(raw, llvm::ConstantPointerNull::get(ptr)), null,
                       llvm::BasicBlock::Create(ctx, "store", wrapper));
        auto* store = cflat_llvm::GetTerminatorOrNull(b.GetInsertBlock())
                          ->getSuccessor(1);
        b.SetInsertPoint(null);
        b.CreateCall(heapRelease, {productValue});
        b.CreateRet(llvm::ConstantPointerNull::get(ptr));
        b.SetInsertPoint(store);
        auto* header = b.CreateBitCast(raw, cflat_llvm::PointerTo(headerType));
        b.CreateStore(productValue, headerSize(b, header));
        b.CreateStore(raw, headerRaw(b, header));
        b.CreateRet(addUserPointer(b, raw));
        replacements.emplace_back(realCalloc, wrapper);
    }

    if (heapCounter && realRealloc && heapMalloc && heapFree
        && realRealloc->getFunctionType()->getNumParams() == 2)
    {
        auto* wrapper = makeFunction("__cflat_isolated_realloc", realRealloc->getFunctionType());
        auto* entry = llvm::BasicBlock::Create(ctx, "entry", wrapper);
        auto* nullInput = llvm::BasicBlock::Create(ctx, "null_input", wrapper);
        auto* nonNull = llvm::BasicBlock::Create(ctx, "non_null", wrapper);
        auto* zero = llvm::BasicBlock::Create(ctx, "zero", wrapper);
        auto* resize = llvm::BasicBlock::Create(ctx, "resize", wrapper);
        auto* grow = llvm::BasicBlock::Create(ctx, "grow", wrapper);
        auto* shrink = llvm::BasicBlock::Create(ctx, "shrink", wrapper);
        auto* call = llvm::BasicBlock::Create(ctx, "call", wrapper);
        auto* failed = llvm::BasicBlock::Create(ctx, "failed", wrapper);
        auto* store = llvm::BasicBlock::Create(ctx, "store", wrapper);
        llvm::IRBuilder<> b(entry);
        auto* oldPtr = wrapper->getArg(0);
        auto* newSize = wrapper->getArg(1);
        b.CreateCondBr(b.CreateICmpEQ(oldPtr, llvm::ConstantPointerNull::get(ptr)), nullInput, nonNull);
        b.SetInsertPoint(nullInput);
        b.CreateRet(b.CreateCall(heapMalloc->getFunctionType(), heapMalloc, {newSize}));
        b.SetInsertPoint(nonNull);
        auto* oldHeader = headerFor(b, oldPtr);
        auto* oldSize = b.CreateLoad(i64, headerSize(b, oldHeader));
        auto* raw = b.CreateLoad(ptr, headerRaw(b, oldHeader));
        b.CreateCondBr(b.CreateICmpEQ(newSize, b.getInt64(0)), zero, resize);
        b.SetInsertPoint(zero);
        auto* freeCall = b.CreateCall(realFree->getFunctionType(), realFree, {raw});
        realCalls.emplace_back(freeCall, realFree);
        b.CreateCall(heapRelease, {oldSize});
        b.CreateRet(llvm::ConstantPointerNull::get(ptr));
        b.SetInsertPoint(resize);
        b.CreateCondBr(b.CreateICmpUGT(newSize, oldSize), grow, shrink);
        b.SetInsertPoint(grow);
        b.CreateCall(heapReserve, {b.CreateSub(newSize, oldSize)});
        b.CreateBr(call);
        b.SetInsertPoint(shrink);
        b.CreateBr(call);
        b.SetInsertPoint(call);
        b.CreateCondBr(b.CreateICmpUGT(newSize, oldSize),
                       llvm::BasicBlock::Create(ctx, "call_grow", wrapper),
                       llvm::BasicBlock::Create(ctx, "call_shrink", wrapper));
        auto* callGrow = cflat_llvm::GetTerminatorOrNull(b.GetInsertBlock())
                             ->getSuccessor(0);
        auto* callShrink = cflat_llvm::GetTerminatorOrNull(b.GetInsertBlock())
                               ->getSuccessor(1);
        b.SetInsertPoint(callGrow);
        auto* growTotal = b.CreateAdd(newSize, b.getInt64(16));
        auto* grownCall = b.CreateCall(realRealloc->getFunctionType(), realRealloc, {raw, growTotal});
        realCalls.emplace_back(grownCall, realRealloc);
        auto* grownRaw = grownCall;
        b.CreateCondBr(b.CreateICmpEQ(grownRaw, llvm::ConstantPointerNull::get(ptr)), failed, store);
        b.SetInsertPoint(callShrink);
        auto* shrinkTotal = b.CreateAdd(newSize, b.getInt64(16));
        auto* shrunkCall = b.CreateCall(realRealloc->getFunctionType(), realRealloc, {raw, shrinkTotal});
        realCalls.emplace_back(shrunkCall, realRealloc);
        auto* shrunkRaw = shrunkCall;
        b.CreateCondBr(b.CreateICmpEQ(shrunkRaw, llvm::ConstantPointerNull::get(ptr)), failed, store);
        b.SetInsertPoint(failed);
        b.CreateCall(heapRelease, {b.CreateSelect(b.CreateICmpUGT(newSize, oldSize),
                                                   b.CreateSub(newSize, oldSize),
                                                   b.getInt64(0))});
        b.CreateRet(llvm::ConstantPointerNull::get(ptr));
        b.SetInsertPoint(store);
        auto* resizedRaw = b.CreatePHI(ptr, 2, "resized_raw");
        resizedRaw->addIncoming(grownRaw, callGrow);
        resizedRaw->addIncoming(shrunkRaw, callShrink);
        auto* resizedHeader = b.CreateBitCast(resizedRaw, cflat_llvm::PointerTo(headerType));
        b.CreateStore(newSize, headerSize(b, resizedHeader));
        b.CreateStore(resizedRaw, headerRaw(b, resizedHeader));
        b.CreateCondBr(b.CreateICmpUGT(oldSize, newSize),
                       llvm::BasicBlock::Create(ctx, "release_shrink", wrapper),
                       llvm::BasicBlock::Create(ctx, "return", wrapper));
        auto* releaseShrink = cflat_llvm::GetTerminatorOrNull(b.GetInsertBlock())
                                  ->getSuccessor(0);
        auto* returnBlock = cflat_llvm::GetTerminatorOrNull(b.GetInsertBlock())
                                ->getSuccessor(1);
        b.SetInsertPoint(releaseShrink);
        b.CreateCall(heapRelease, {b.CreateSub(oldSize, newSize)});
        b.CreateBr(returnBlock);
        b.SetInsertPoint(returnBlock);
        b.CreateRet(addUserPointer(b, resizedRaw));
        replacements.emplace_back(realRealloc, wrapper);
    }

    if (heapCounter && heapMalloc && realPosixMemalign
        && realPosixMemalign->getFunctionType()->getNumParams() == 3)
    {
        auto* wrapper = makeFunction("__cflat_isolated_posix_memalign",
                                     realPosixMemalign->getFunctionType());
        auto* entry = llvm::BasicBlock::Create(ctx, "entry", wrapper);
        auto* invalid = llvm::BasicBlock::Create(ctx, "invalid", wrapper);
        auto* allocate = llvm::BasicBlock::Create(ctx, "allocate", wrapper);
        auto* null = llvm::BasicBlock::Create(ctx, "null", wrapper);
        auto* store = llvm::BasicBlock::Create(ctx, "store", wrapper);
        llvm::IRBuilder<> b(entry);
        auto* alignment = wrapper->getArg(1);
        auto* size = wrapper->getArg(2);
        auto* badAlign = b.CreateOr(
            b.CreateICmpULT(alignment, b.getInt64(8)),
            b.CreateICmpNE(b.CreateAnd(alignment, b.CreateSub(alignment, b.getInt64(1))),
                           b.getInt64(0)));
        b.CreateCondBr(badAlign, invalid, allocate);
        b.SetInsertPoint(invalid);
        b.CreateRet(b.getInt32(22));
        b.SetInsertPoint(allocate);
        auto* overflow = llvm::BasicBlock::Create(ctx, "overflow", wrapper);
        auto* sizeAlignment = b.CreateIntrinsic(llvm::Intrinsic::uadd_with_overflow,
                                                {i64}, {size, alignment});
        auto* sizeAlignmentValue = b.CreateExtractValue(sizeAlignment, 0);
        auto* sizeAlignmentOverflow = b.CreateExtractValue(sizeAlignment, 1);
        auto* totalWithHeader = b.CreateIntrinsic(llvm::Intrinsic::uadd_with_overflow,
                                                  {i64}, {sizeAlignmentValue, b.getInt64(15)});
        auto* total = b.CreateExtractValue(totalWithHeader, 0);
        auto* totalOverflow = b.CreateExtractValue(totalWithHeader, 1);
        b.CreateCondBr(b.CreateOr(sizeAlignmentOverflow, totalOverflow), overflow, null);
        b.SetInsertPoint(overflow);
        b.CreateCall(module->getFunction("__cflat_isolated_heap_fail"), {});
        b.CreateUnreachable();
        b.SetInsertPoint(null);
        b.CreateCall(heapReserve, {total});
        auto* rawCallInst = b.CreateCall(realMalloc->getFunctionType(), realMalloc, {total});
        realCalls.emplace_back(rawCallInst, realMalloc);
        auto* raw = rawCallInst;
        b.CreateCondBr(b.CreateICmpEQ(raw, llvm::ConstantPointerNull::get(ptr)),
                       llvm::BasicBlock::Create(ctx, "alloc_fail", wrapper), store);
        auto* allocFail = cflat_llvm::GetTerminatorOrNull(b.GetInsertBlock())
                              ->getSuccessor(0);
        b.SetInsertPoint(allocFail);
        b.CreateCall(heapRelease, {total});
        b.CreateRet(b.getInt32(12));
        b.SetInsertPoint(store);
        auto* rawInt = b.CreatePtrToInt(raw, i64);
        auto* alignedInt = b.CreateAnd(b.CreateAdd(b.CreateAdd(rawInt, b.getInt64(16)),
                                                    b.CreateSub(alignment, b.getInt64(1))),
                                       b.CreateNot(b.CreateSub(alignment, b.getInt64(1))));
        auto* user = b.CreateIntToPtr(alignedInt, ptr);
        auto* header = b.CreateBitCast(b.CreateConstInBoundsGEP1_64(i8, user, -16),
                                       cflat_llvm::PointerTo(headerType));
        b.CreateStore(total, headerSize(b, header));
        b.CreateStore(raw, headerRaw(b, header));
        b.CreateStore(user, wrapper->getArg(0));
        b.CreateRet(b.getInt32(0));
        replacements.emplace_back(realPosixMemalign, wrapper);
    }

    for (auto& [original, replacement] : replacements)
        original->replaceAllUsesWith(replacement);

    if (isolatedPolicy_->maxThreads && *isolatedPolicy_->maxThreads > 1
        && !isolatedPolicy_->IsDenied(IsolatedCapability::Threads))
    {
        std::vector<std::pair<llvm::Function*, llvm::Function*>> threadReplacements;
        auto* available = new llvm::GlobalVariable(
            *module, i64, false, llvm::GlobalValue::InternalLinkage,
            llvm::ConstantInt::get(i64, 0),
            "__cflat_isolated_thread_permits");
        available->setAlignment(llvm::Align(8));
        auto* failure = makeFailure("__cflat_isolated_thread_fail",
                                    "isolated runtime error: max_threads exceeded\n");
        auto* acquire = makeReserve("__cflat_isolated_thread_acquire", available,
                                    *isolatedPolicy_->maxThreads - 1, failure);
        auto* release = makeSubtractRelease("__cflat_isolated_thread_release", available);
        auto* packetType = llvm::StructType::create(ctx, {ptr, ptr},
                                                    "__cflat_isolated_thread_start");
        auto* packetPtr = cflat_llvm::PointerTo(packetType);
        llvm::Function* threadAlloc = heapMalloc ? heapMalloc : realMalloc;
        llvm::Function* threadFree = heapFree ? heapFree : realFree;
        auto* realCreate = getExternal("pthread_create");
        if (realCreate && realCreate->getFunctionType()->getNumParams() == 4)
        {
            auto* trampoline = makeFunction(
                "__cflat_isolated_thread_trampoline",
                llvm::FunctionType::get(ptr, {ptr}, false));
            auto* trampolineEntry = llvm::BasicBlock::Create(ctx, "entry", trampoline);
            llvm::IRBuilder<> tb(trampolineEntry);
            auto* packet = tb.CreateBitCast(trampoline->getArg(0), packetPtr);
            auto* start = tb.CreateLoad(ptr, tb.CreateStructGEP(packetType, packet, 0));
            auto* arg = tb.CreateLoad(ptr, tb.CreateStructGEP(packetType, packet, 1));
            auto* threadStartType = llvm::FunctionType::get(ptr, {ptr}, false);
            auto* threadResult = tb.CreateCall(threadStartType, start, {arg});
            tb.CreateCall(release, {tb.getInt64(1)});
            if (threadFree)
                tb.CreateCall(threadFree->getFunctionType(), threadFree, {trampoline->getArg(0)});
            tb.CreateRet(threadResult);

            auto* create = makeFunction("__cflat_isolated_pthread_create",
                                        realCreate->getFunctionType());
            auto* entry = llvm::BasicBlock::Create(ctx, "entry", create);
            auto* failed = llvm::BasicBlock::Create(ctx, "failed", create);
            auto* noPacket = llvm::BasicBlock::Create(ctx, "no_packet", create);
            llvm::IRBuilder<> b(entry);
            b.CreateCall(acquire, {b.getInt64(1)});
            llvm::Value* packetValue = llvm::ConstantPointerNull::get(ptr);
            if (threadAlloc)
                packetValue = b.CreateCall(threadAlloc->getFunctionType(), threadAlloc,
                                           {b.getInt64(16)});
            b.CreateCondBr(b.CreateICmpEQ(packetValue, llvm::ConstantPointerNull::get(ptr)),
                           noPacket, llvm::BasicBlock::Create(ctx, "start", create));
            auto* startBlock = cflat_llvm::GetTerminatorOrNull(b.GetInsertBlock())
                                   ->getSuccessor(1);
            b.SetInsertPoint(noPacket);
            b.CreateCall(release, {b.getInt64(1)});
            b.CreateRet(b.getInt32(12));
            b.SetInsertPoint(startBlock);
            auto* packetPtrValue = b.CreateBitCast(packetValue, packetPtr);
            b.CreateStore(create->getArg(2), b.CreateStructGEP(packetType, packetPtrValue, 0));
            b.CreateStore(create->getArg(3), b.CreateStructGEP(packetType, packetPtrValue, 1));
            std::vector<llvm::Value*> args = {
                create->getArg(0), create->getArg(1), trampoline, packetValue};
            auto* createResult = b.CreateCall(realCreate->getFunctionType(), realCreate, args);
            realCalls.emplace_back(createResult, realCreate);
            b.CreateCondBr(b.CreateICmpNE(createResult, b.getInt32(0)), failed,
                           llvm::BasicBlock::Create(ctx, "success", create));
            auto* success = cflat_llvm::GetTerminatorOrNull(b.GetInsertBlock())
                                ->getSuccessor(1);
            b.SetInsertPoint(failed);
            if (threadFree)
                b.CreateCall(threadFree->getFunctionType(), threadFree, {packetValue});
            b.CreateCall(release, {b.getInt64(1)});
            b.CreateRet(createResult);
            b.SetInsertPoint(success);
            b.CreateRet(createResult);
            threadReplacements.emplace_back(realCreate, create);
        }
        auto* realCreateThread = getExternal("CreateThread");
        if (realCreateThread && realCreateThread->getFunctionType()->getNumParams() == 6)
        {
            llvm::CallingConv::ID startCallingConv = realCreateThread->getCallingConv();
            for (auto* user : realCreateThread->users())
            {
                auto* call = llvm::dyn_cast<llvm::CallBase>(user);
                if (!call || call->arg_size() <= 2)
                    continue;
                auto* start = call->getArgOperand(2)->stripPointerCasts();
                if (auto* function = llvm::dyn_cast<llvm::Function>(start))
                {
                    startCallingConv = function->getCallingConv();
                    break;
                }
            }
            auto* trampoline = makeFunction(
                "__cflat_isolated_win_thread_trampoline",
                llvm::FunctionType::get(i32, {ptr}, false));
            trampoline->setCallingConv(startCallingConv);
            auto* trampolineEntry = llvm::BasicBlock::Create(ctx, "entry", trampoline);
            llvm::IRBuilder<> tb(trampolineEntry);
            auto* packet = tb.CreateBitCast(trampoline->getArg(0), packetPtr);
            auto* start = tb.CreateLoad(ptr, tb.CreateStructGEP(packetType, packet, 0));
            auto* arg = tb.CreateLoad(ptr, tb.CreateStructGEP(packetType, packet, 1));
            auto* threadStartType = llvm::FunctionType::get(i32, {ptr}, false);
            auto* threadResult = tb.CreateCall(threadStartType, start, {arg});
            threadResult->setCallingConv(startCallingConv);
            tb.CreateCall(release, {tb.getInt64(1)});
            if (threadFree)
                tb.CreateCall(threadFree->getFunctionType(), threadFree, {trampoline->getArg(0)});
            tb.CreateRet(threadResult);

            auto* create = makeFunction("__cflat_isolated_CreateThread",
                                        realCreateThread->getFunctionType());
            create->setCallingConv(realCreateThread->getCallingConv());
            auto* entry = llvm::BasicBlock::Create(ctx, "entry", create);
            auto* failed = llvm::BasicBlock::Create(ctx, "failed", create);
            auto* noPacket = llvm::BasicBlock::Create(ctx, "no_packet", create);
            llvm::IRBuilder<> b(entry);
            b.CreateCall(acquire, {b.getInt64(1)});
            llvm::Value* packetValue = llvm::ConstantPointerNull::get(ptr);
            if (threadAlloc)
                packetValue = b.CreateCall(threadAlloc->getFunctionType(), threadAlloc,
                                           {b.getInt64(16)});
            b.CreateCondBr(b.CreateICmpEQ(packetValue, llvm::ConstantPointerNull::get(ptr)),
                           noPacket, llvm::BasicBlock::Create(ctx, "start", create));
            auto* startBlock = cflat_llvm::GetTerminatorOrNull(b.GetInsertBlock())
                                   ->getSuccessor(1);
            b.SetInsertPoint(noPacket);
            b.CreateCall(release, {b.getInt64(1)});
            b.CreateRet(llvm::ConstantPointerNull::get(ptr));
            b.SetInsertPoint(startBlock);
            auto* packetPtrValue = b.CreateBitCast(packetValue, packetPtr);
            b.CreateStore(create->getArg(2), b.CreateStructGEP(packetType, packetPtrValue, 0));
            b.CreateStore(create->getArg(3), b.CreateStructGEP(packetType, packetPtrValue, 1));
            std::vector<llvm::Value*> args = {
                create->getArg(0), create->getArg(1), trampoline, packetValue,
                create->getArg(4), create->getArg(5)};
            auto* createResult = b.CreateCall(realCreateThread->getFunctionType(),
                                              realCreateThread, args);
            createResult->setCallingConv(realCreateThread->getCallingConv());
            realCalls.emplace_back(createResult, realCreateThread);
            b.CreateCondBr(b.CreateICmpEQ(createResult, llvm::ConstantPointerNull::get(ptr)),
                           failed, llvm::BasicBlock::Create(ctx, "success", create));
            auto* success = cflat_llvm::GetTerminatorOrNull(b.GetInsertBlock())
                                ->getSuccessor(1);
            b.SetInsertPoint(failed);
            if (threadFree)
                b.CreateCall(threadFree->getFunctionType(), threadFree, {packetValue});
            b.CreateCall(release, {b.getInt64(1)});
            b.CreateRet(llvm::ConstantPointerNull::get(ptr));
            b.SetInsertPoint(success);
            b.CreateRet(createResult);
            threadReplacements.emplace_back(realCreateThread, create);
        }
        for (auto& [original, replacement] : threadReplacements)
            original->replaceAllUsesWith(replacement);
    }
    for (auto& [call, original] : realCalls)
        call->setCalledFunction(original->getFunctionType(), original);
    return true;
}

bool LLVMBackend::AuditIsolatedModule()
{
    if (!isolatedPolicy_) return true;
    MaterializeCoreIfLazy();

    auto auditCtorArray = [&](llvm::StringRef name) {
        auto* array = module->getGlobalVariable(name);
        if (!array) return;
        auto* initializer = array->hasInitializer()
            ? llvm::dyn_cast<llvm::ConstantArray>(array->getInitializer()) : nullptr;
        if (!initializer)
        {
            if (array->hasInitializer()
                && llvm::isa<llvm::ConstantAggregateZero>(array->getInitializer()))
                return;
            LogError(std::format("policy-module-denied: '{}' has no valid constructor array in policy '{}'",
                                 name.str(), isolatedPolicy_->path));
            return;
        }
        for (auto* element : initializer->operand_values())
        {
            auto* entry = llvm::dyn_cast<llvm::ConstantStruct>(element);
            auto* functionValue = entry && entry->getNumOperands() > 1
                ? entry->getAggregateElement(1) : nullptr;
            auto* function = functionValue
                ? llvm::dyn_cast<llvm::Function>(functionValue->stripPointerCasts()) : nullptr;
            if (!function || function->isDeclaration())
                LogError(std::format("policy-module-denied: external or invalid function in '{}' in policy '{}'",
                                     name.str(), isolatedPolicy_->path));
        }
    };
    auditCtorArray("llvm.global_ctors");
    auditCtorArray("llvm.global_dtors");

    if (!module->getModuleInlineAsm().empty())
        LogError(std::format("policy-module-denied: module-level inline assembly is not allowed "
                             "in policy '{}'", isolatedPolicy_->path));

    for (auto& alias : module->aliases())
    {
        auto* target = alias.getAliasee()->stripPointerCasts();
        auto* global = llvm::dyn_cast<llvm::GlobalValue>(target);
        if (!global || global->isDeclaration() || global->getParent() != module.get())
            LogError(std::format("policy-module-denied: alias '{}' targets an external or unknown "
                                 "symbol in policy '{}'", alias.getName().str(), isolatedPolicy_->path));
    }

    auto auditUsedList = [&](llvm::StringRef name) {
        auto* used = module->getGlobalVariable(name);
        if (!used || !used->hasInitializer()) return;
        auto* entries = llvm::dyn_cast<llvm::ConstantArray>(used->getInitializer());
        if (!entries)
        {
            if (llvm::isa<llvm::ConstantAggregateZero>(used->getInitializer())) return;
            LogError(std::format("policy-module-denied: '{}' has an invalid initializer in policy '{}'",
                                 name.str(), isolatedPolicy_->path));
            return;
        }
        for (auto* entry : entries->operand_values())
        {
            auto* global = llvm::dyn_cast<llvm::GlobalValue>(entry->stripPointerCasts());
            if (!global || global->isDeclaration())
                LogError(std::format("policy-module-denied: '{}' retains an external or unknown "
                                     "symbol in policy '{}'", name.str(), isolatedPolicy_->path));
        }
    };
    auditUsedList("llvm.used");
    auditUsedList("llvm.compiler.used");

    for (auto& function : module->functions())
    {
        if (function.isDeclaration())
        {
            auto name = function.getName();
            if (name.starts_with("llvm."))
            {
                if (!IsAllowedIsolatedIntrinsic(name))
                    LogError(std::format("policy-module-denied: unknown LLVM intrinsic '{}' in policy '{}'",
                                         name.str(), isolatedPolicy_->path));
            }
            else
            {
                if (IsNeverAllowedIsolatedSymbol(name))
                    LogError(std::format("policy-restricted-language: dynamic loading symbol '{}' is not allowed "
                                         "in isolated mode (policy '{}')", name.str(), isolatedPolicy_->path));
                auto capability = ClassifyIsolatedSymbol(function.getName().str());
                if (!capability)
                    LogError(std::format("policy-module-denied: unknown external symbol '{}' (policy '{}')",
                                         name.str(), isolatedPolicy_->path));
                if (capability && isolatedPolicy_->IsDenied(*capability))
                    LogError(std::format("policy-module-denied: external symbol '{}' requires capability "
                                         "'{}', denied by policy '{}'",
                                         function.getName().str(), IsolatedCapabilityName(*capability),
                                         isolatedPolicy_->path));
            }
        }
    }
    for (auto& global : module->globals())
    {
        if (!global.isDeclaration()) continue;
        auto name = global.getName();
        if (IsNeverAllowedIsolatedSymbol(name))
            LogError(std::format("policy-restricted-language: dynamic loading symbol '{}' is not allowed "
                                 "in isolated mode (policy '{}')", name.str(), isolatedPolicy_->path));
        auto capability = ClassifyIsolatedSymbol(name.str());
        if (!capability)
            LogError(std::format("policy-module-denied: unknown external symbol '{}' (policy '{}')",
                                 name.str(), isolatedPolicy_->path));
        if (capability && isolatedPolicy_->IsDenied(*capability))
        {
            LogError(std::format("policy-module-denied: external symbol '{}' requires capability "
                                 "'{}', denied by policy '{}'",
                                 global.getName().str(), IsolatedCapabilityName(*capability),
                                 isolatedPolicy_->path));
        }
    }

    // A runtime export may be the direct callee of a call, but its address must not
    // enter data or an indirect-call path. Defined CFlat functions remain closed-world targets.
    for (auto& function : module->functions())
    {
        if (!function.isDeclaration() || function.getName().starts_with("llvm.")) continue;
        std::unordered_set<const llvm::User*> visited;
        auto auditAddressUse = [&](auto&& self, llvm::User* user) -> void {
            if (!visited.insert(user).second) return;
            if (auto* call = llvm::dyn_cast<llvm::CallBase>(user))
            {
                if (call->getCalledOperand()->stripPointerCasts() == &function)
                {
                    for (auto& arg : call->args())
                        if (arg->stripPointerCasts() == &function)
                            LogError(std::format("policy-module-denied: address of external function '{}' "
                                                 "escapes through a call argument (policy '{}')",
                                                 function.getName().str(), isolatedPolicy_->path));
                    return;
                }
                LogError(std::format("policy-module-denied: address of external function '{}' escapes "
                                     "through an indirect call or argument (policy '{}')",
                                     function.getName().str(), isolatedPolicy_->path));
            }
            if (auto* expression = llvm::dyn_cast<llvm::ConstantExpr>(user))
            {
                if (!expression->isCast())
                    LogError(std::format("policy-module-denied: address of external function '{}' escapes "
                                         "through a non-cast constant (policy '{}')",
                                         function.getName().str(), isolatedPolicy_->path));
                for (auto* next : user->users()) self(self, next);
                return;
            }
            auto* valueName = user->getValueName();
            LogError(std::format("policy-module-denied: address of external function '{}' escapes "
                                 "through '{}' (policy '{}')",
                                 function.getName().str(), valueName ? valueName->getKey().str() : "<unnamed>",
                                 isolatedPolicy_->path));
        };
        for (auto* user : function.users()) auditAddressUse(auditAddressUse, user);
    }

    for (auto& function : module->functions())
        for (auto& block : function)
            for (auto& instruction : block)
            {
                bool synthesizedWrapper = function.getName().starts_with("__cflat_isolated_")
                    || function.getName().starts_with("___")
                    || isolatedCoreFunctions_.count(function.getName().str()) != 0;
                if (llvm::isa<llvm::IntToPtrInst>(&instruction) && !synthesizedWrapper)
                    LogError(std::format("policy-module-denied: inttoptr outside isolated wrapper '{}' "
                                         "in policy '{}'", function.getName().str(), isolatedPolicy_->path));
                if (auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction))
                {
                    auto* called = call->getCalledOperand()->stripPointerCasts();
                    if (llvm::isa<llvm::InlineAsm>(called))
                        LogError(std::format("policy-module-denied: inline assembly call in function '{}' "
                                             "in policy '{}'", function.getName().str(), isolatedPolicy_->path));
                }
            }

    return true;
}

bool LLVMBackend::AuditIsolatedExecutable(const std::string& exePath)
{
    if (!isolatedPolicy_) return true;
#if !defined(__APPLE__) || !defined(__aarch64__)
    (void)exePath;
    LogError(std::format("policy-output-unsupported: isolated executable audit is only available "
                         "for native macOS arm64 (policy '{}')", isolatedPolicy_->path));
    return false;
#else
    auto bufferOrError = llvm::MemoryBuffer::getFile(exePath);
    if (!bufferOrError)
        LogError(std::format("policy-module-denied: cannot reopen isolated executable '{}' in policy '{}'",
                             exePath, isolatedPolicy_->path));
    auto buffer = std::move(*bufferOrError);
    auto binaryOrError = llvm::object::createBinary(buffer->getMemBufferRef());
    if (!binaryOrError)
        LogError(std::format("policy-module-denied: isolated executable '{}' is not a valid Mach-O image "
                             "in policy '{}'", exePath, isolatedPolicy_->path));
    auto* object = llvm::dyn_cast<llvm::object::MachOObjectFile>(binaryOrError->get());
    if (!object || !object->is64Bit() || object->getArch() != llvm::Triple::aarch64)
        LogError(std::format("policy-module-denied: isolated executable '{}' is not native macOS arm64 "
                             "in policy '{}'", exePath, isolatedPolicy_->path));

    static constexpr auto baseCommand = [](uint32_t command) {
        return command & ~static_cast<uint32_t>(LC_REQ_DYLD);
    };
    static const std::unordered_set<uint32_t> allowedCommands = {
        baseCommand(LC_SEGMENT_64), baseCommand(LC_DYLD_INFO_ONLY), baseCommand(LC_SYMTAB),
        baseCommand(LC_DYSYMTAB), baseCommand(LC_LOAD_DYLINKER), baseCommand(LC_UUID),
        baseCommand(LC_BUILD_VERSION), baseCommand(LC_MAIN), baseCommand(LC_LOAD_DYLIB),
        baseCommand(LC_FUNCTION_STARTS), baseCommand(LC_DATA_IN_CODE), baseCommand(LC_CODE_SIGNATURE)};
    for (const auto& load : object->load_commands())
    {
        uint32_t command = baseCommand(load.C.cmd);
        if (!allowedCommands.count(command))
            LogError(std::format("policy-module-denied: Mach-O load command {} is not allowed in '{}' "
                                 "(policy '{}')", command, exePath, isolatedPolicy_->path));
        if (command == LC_LOAD_DYLIB)
        {
            auto dylib = object->getDylibIDLoadCommand(load);
            auto pathOffset = dylib.dylib.name;
            if (pathOffset >= load.C.cmdsize)
                LogError(std::format("policy-module-denied: malformed Mach-O dylib command in '{}' "
                                     "(policy '{}')", exePath, isolatedPolicy_->path));
            llvm::StringRef path(load.Ptr + pathOffset, load.C.cmdsize - pathOffset);
            path = path.split('\0').first;
            if (path != "/usr/lib/libSystem.B.dylib")
                LogError(std::format("policy-module-denied: Mach-O imports dylib '{}' in '{}' "
                                     "(policy '{}')", path.str(), exePath, isolatedPolicy_->path));
        }
        if (command == LC_LOAD_DYLINKER)
        {
            auto linker = object->getDylinkerCommand(load);
            auto pathOffset = linker.name;
            llvm::StringRef path(load.Ptr + pathOffset, load.C.cmdsize - pathOffset);
            path = path.split('\0').first;
            if (path != "/usr/lib/dyld")
                LogError(std::format("policy-module-denied: Mach-O uses dyld '{}' in '{}' "
                                     "(policy '{}')", path.str(), exePath, isolatedPolicy_->path));
        }
        if (command == LC_SEGMENT_64)
        {
            auto segment = object->getSegment64LoadCommand(load);
            constexpr uint32_t writable = 0x2;
            constexpr uint32_t executable = 0x4;
            if (((segment.initprot | segment.maxprot) & writable)
                && ((segment.initprot | segment.maxprot) & executable))
                LogError(std::format("policy-module-denied: Mach-O segment '{}' is writable and executable "
                                     "in '{}' (policy '{}')", segment.segname, exePath,
                                     isolatedPolicy_->path));
        }
    }

    static const std::unordered_set<std::string> linkerSupport = {
        "dyld_stub_binder", "_tlv_bootstrap"};
    for (auto symbol : object->symbols())
    {
        auto flagsOrError = symbol.getFlags();
        if (!flagsOrError)
            LogError(std::format("policy-module-denied: cannot inspect Mach-O symbol in '{}' "
                                 "(policy '{}')", exePath, isolatedPolicy_->path));
        if (!(*flagsOrError & llvm::object::SymbolRef::SF_Undefined)) continue;
        auto nameOrError = symbol.getName();
        if (!nameOrError)
            LogError(std::format("policy-module-denied: cannot name Mach-O import in '{}' "
                                 "(policy '{}')", exePath, isolatedPolicy_->path));
        std::string name = nameOrError->str();
        if (!name.empty() && name.front() == '_') name.erase(name.begin());
        if (linkerSupport.count(name)) continue;
        auto capability = ClassifyIsolatedSymbol(name);
        if (!capability)
            LogError(std::format("policy-module-denied: Mach-O imports unknown symbol '{}' in '{}' "
                                 "(policy '{}')", name, exePath, isolatedPolicy_->path));
        if (isolatedPolicy_->IsDenied(*capability))
            LogError(std::format("policy-module-denied: Mach-O import '{}' requires capability '{}' "
                                 "denied by policy '{}'", name, IsolatedCapabilityName(*capability),
                                 isolatedPolicy_->path));
    }
    return true;
#endif
}

bool LLVMBackend::WriteIsolatedManifest(const std::string& manifestPath, const std::string& kind,
                                        const std::optional<std::string>& outputPath)
{
    if (!isolatedPolicy_) return true;
    nlohmann::json manifest = nlohmann::json::object();
    manifest["format"] = "cflat-isolated-manifest";
    manifest["format_version"] = 1;
    manifest["compiler_build_id"] = CFLAT_VERSION_STRING;
    manifest["llvm_major"] = LLVM_VERSION_MAJOR;
    std::string targetTriple = module->getTargetTriple().str();
    if (targetTriple.empty())
        targetTriple = targetMacOS_ ? "arm64-apple-macosx11.0.0"
            : targetWindows_ ? (platformValue == 32 ? "i686-pc-windows-msvc"
                                                   : "x86_64-pc-windows-msvc")
                             : llvm::sys::getProcessTriple();
    manifest["target_triple"] = targetTriple;
    manifest["data_layout"] = module->getDataLayoutStr();
    manifest["policy_version"] = 1;
    manifest["policy_sha256"] = isolatedPolicy_->digestHex;

    nlohmann::json output = nlohmann::json::object();
    output["kind"] = kind;
    if (outputPath)
    {
        std::ifstream input(*outputPath, std::ios::binary);
        if (!input)
            LogError(std::format("isolated-manifest-error: cannot read output '{}' for manifest '{}'",
                                 *outputPath, manifestPath));
        std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        llvm::SHA256 sha;
        sha.update(llvm::StringRef(bytes.data(), bytes.size()));
        static constexpr char hex[] = "0123456789abcdef";
        std::string digest;
        for (uint8_t byte : sha.final())
        {
            digest.push_back(hex[byte >> 4]);
            digest.push_back(hex[byte & 0x0f]);
        }
        output["sha256"] = digest;
    }
    manifest["output"] = std::move(output);

    nlohmann::json resolved = nlohmann::json::object();
    for (size_t i = 1; i < IsolatedPolicyCapabilityCount; ++i)
    {
        auto capability = static_cast<IsolatedCapability>(i);
        resolved[IsolatedCapabilityName(capability)] = isolatedPolicy_->allow[i] ? "allow" : "deny";
    }
    manifest["resolved_capabilities"] = std::move(resolved);

    std::set<std::string> required;
    std::map<std::string, IsolatedCapability> exports;
    auto recordExport = [&](llvm::StringRef symbol) {
        auto capability = ClassifyIsolatedSymbol(symbol.str());
        if (!capability) return;
        required.insert(IsolatedCapabilityName(*capability));
        exports.emplace(symbol.str(), *capability);
    };
    for (auto& function : module->functions())
        if (function.isDeclaration() && !function.getName().starts_with("llvm."))
            recordExport(function.getName());
    for (auto& global : module->globals())
        if (global.isDeclaration()) recordExport(global.getName());
    manifest["required_capabilities"] = nlohmann::json::array();
    for (const auto& name : required) manifest["required_capabilities"].push_back(name);
    manifest["runtime_exports"] = nlohmann::json::array();
    for (const auto& [symbol, capability] : exports)
        manifest["runtime_exports"].push_back({{"symbol", symbol},
                                                 {"capability", IsolatedCapabilityName(capability)}});

    nlohmann::json limits = nlohmann::json::object();
    nlohmann::json enforcement = nlohmann::json::object();
    if (isolatedPolicy_->heapBytes)
    {
        limits["heap_bytes"] = *isolatedPolicy_->heapBytes;
        enforcement["heap_bytes"] = "compiler-instrumented";
    }
    if (isolatedPolicy_->maxThreads)
    {
        limits["max_threads"] = *isolatedPolicy_->maxThreads;
        enforcement["max_threads"] = "compiler-instrumented";
    }
    if (!limits.empty()) manifest["limits"] = std::move(limits);
    if (!enforcement.empty()) manifest["enforcement"] = std::move(enforcement);

    std::ofstream outputFile(manifestPath, std::ios::binary | std::ios::trunc);
    if (!outputFile)
        LogError(std::format("isolated-manifest-error: cannot write manifest '{}'", manifestPath));
    outputFile << manifest.dump() << '\n';
    if (!outputFile)
        LogError(std::format("isolated-manifest-error: failed writing manifest '{}'", manifestPath));
    return true;
}

void LLVMBackend::ValidateIsolatedProgram(antlr4::ParserRuleContext* ctx)
{
    if (!isolatedPolicy_ || currentSourceIsCore_ || IsIsolatedCoreSource() || ctx == nullptr) return;
    currentLine = ctx->getStart()->getLine();
    currentColumn = ctx->getStart()->getCharPositionInLine();
    if (isolatedPolicy_->IsDenied(IsolatedCapability::Process))
        LogError(std::format("policy-capability-denied: program requires capability 'process', "
                             "denied by policy '{}'", isolatedPolicy_->path));
    LogError(std::format("policy-restricted-language: 'program' is not available in restricted-v1 "
                         "(policy '{}')", isolatedPolicy_->path));
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
                msg += "\n" + LocalizeMessage("hint: {}", {d.hint});
            diagnosticSink_(d.file, static_cast<size_t>(d.line), static_cast<size_t>(d.col), msg, 1);
            // No throw - parse errors are pre-codegen; caller returns false.
        }
        else
        {
            std::cout << std::format("{}({},{}): error: {}\n", d.file, d.line, d.col, d.message);

            int lineIdx = d.line - 1;
            if (lineIdx >= 0 && lineIdx < static_cast<int>(sourceLines.size()))
            {
                const std::string& srcLine = sourceLines[lineIdx];
                std::cout << std::format("    {}\n", srcLine);
                std::string caret(4 + std::min(d.col, static_cast<int>(srcLine.size())), ' ');
                caret += '^';
                std::cout << caret << "\n";
            }

            if (!d.hint.empty())
                std::cout << LocalizeMessage("hint: {}", {d.hint}) << "\n";
        }
    }
}

bool LLVMBackend::CheckGrammar(const std::string& filename)
{
    if (!std::filesystem::exists(filename))
    {
        std::cout << std::format("Error: input file '{}' does not exist.\n", filename);
        return false;
    }

    std::ifstream stream;
    stream.open(filename);
    if (!stream)
    {
        std::cout << std::format("Error: cannot open input file '{}'.\n", filename);
        return false;
    }
    auto sourceLines = ReadFileToLines(stream);

    antlr4::ANTLRInputStream input(stream);
    CFlatLexer lexer(&input);
    antlr4::CommonTokenStream tokens(&lexer);
    CFlatParser parser(&tokens);

    auto localizeMessage = MakeDiagnosticLocalizer();
    CFlatErrorListener errorListener(filename, sourceLines, localizeMessage);
    lexer.removeErrorListeners();
    lexer.addErrorListener(&errorListener);
    parser.removeErrorListeners();
    parser.addErrorListener(&errorListener);
    parser.setErrorHandler(std::make_shared<CFlatErrorStrategy>(localizeMessage));

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
        std::cout << std::format("GRAMMAR FAIL: {} ({} error(s))\n",
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
    dependencyFiles_.clear();
    dependencyPathMemo_.clear();
    dependencyFileSet_.clear();
    if (inputOverride.empty())
    {
        for (size_t i = 0; i < args.positionalCount(); ++i)
            RecordDependency(*args.getPositional(i));
    }
    else
        RecordDependency(filename);
    sourceFileName = std::filesystem::path(filename).filename().string();
    llvm::TimeTraceScope compileScope("Compilation", sourceFileName);
    auto rootCanonical = std::filesystem::weakly_canonical(filename).string();
    currentSourceFilePath_ = rootCanonical;
    analyzedRootPath_ = rootCanonical;
    currentSourceIsCore_ = false;
    // See rootCoreDir_: a core library source compiled directly imports its siblings from ITS
    // checkout, which is not runtimeDir/core.
    rootCoreDir_ = RootFileNameIsCore(sourceFileName)
        ? std::filesystem::path(rootCanonical).parent_path().string() : std::string();
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
    auto asmPath = checkOnly ? std::optional<std::string>{} : args.getOption("out-asm");
    auto bitcodePath = checkOnly ? std::string{} : args.getOption("bitcode").value_or("");
    // --sanitize=ownership implies -g so the emitted move/use locations are meaningful.
    bool debugInfo = args.hasFlag("debug-info") || sanitizeOwnership_;

    importSearchDirs = args.getMultiOption("import-dir");
    // Default target platform = native host OS. Overridable with --platform for
    // cross-compilation (e.g. macos Mach-O emission from a Windows/WSL host).
    auto platformOption = args.getOption("platform").value_or(DefaultPlatform());

    // Windows images must be named *.exe to be runnable, so `-o foo` would produce a
    // file the shell refuses to launch. Supply the extension when none was given.
    if (exePath && (platformOption == "win32" || platformOption == "win64")
        && !std::filesystem::path(*exePath).has_extension())
        *exePath += ".exe";

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
        if (asmPath)             std::filesystem::remove(*asmPath, rmEc);
        if (!bitcodePath.empty()) std::filesystem::remove(bitcodePath, rmEc);
        if (auto manifestPath = args.getOption("isolated-manifest"))
            std::filesystem::remove(*manifestPath, rmEc);
    }

    // Resolve and validate --cpu / --tune once, up front, so an unknown name is a clean
    // error before any clang-cl spawn or codegen, and both the C-interop and native-object
    // paths can use the resolved values verbatim. Validated against the active platform's
    // triple - the same one --print-supported-cpus lists, so the two never disagree.
    {
        std::string triple = PlatformTargetTriple(platformOption);
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
    if (!args.getError().empty())
    {
        LogError(args.getError());
        return false;
    }
    // C library bindings: header search dirs, prebuilt import libraries, and defines.
    cIncludeDirs_ = args.getMultiOption("c-include");
    cLinkLibs_    = args.getMultiOption("c-lib");
    cDefines_     = args.getMultiOption("c-define");
    // Global compile-time defines (-D). Fixed for the whole compilation: .cb sources can
    // neither add nor mutate them, which keeps module/bitcode processing cacheable.
    if (!SetUserDefines(args.getMultiOption("define")))
        return false;
    // Positional `.c` inputs are dispatched to clang only after the module-end analyses have
    // run, so note their presence now for RunNullIfaceGlobalCheck's interop gate.
    positionalCSource_ = false;
    for (size_t i = 1; i < args.positionalCount(); ++i)
        if (LowerExtension(*args.getPositional(i)) == ".c") positionalCSource_ = true;
    // macOS frameworks seeded from --framework (mirrors --c-lib). `import framework`
    // lines append more during ProcessImports; AddFrameworkImport dedups.
    for (const auto& fw : args.getMultiOption("framework"))
        AddFrameworkImport(fw);
    // Vcpkg integration options: forward to the resolver. Each is optional - the
    // resolver auto-discovers vcpkg.exe and the manifest when not overridden.
    if (auto p = args.getOption("vcpkg-exe"))      vcpkg_.SetExeOverride(*p);
    if (auto p = args.getOption("vcpkg-manifest")) vcpkg_.SetManifestOverride(*p);
    if (auto p = args.getOption("vcpkg-triplet"))  vcpkg_.SetTripletOverride(*p);
    vcpkg_.SetNoInstall(args.hasFlag("vcpkg-no-install"));
    // Nuget integration options: mirror the vcpkg wiring. Both optional - the resolver
    // defaults to the NuGet global packages folder when no override is given.
    if (auto p = args.getOption("nuget-packages-dir")) nuget_.SetPackagesDirOverride(*p);
    nuget_.SetNoInstall(args.hasFlag("nuget-no-install"));

    if (verbose)
    {
        std::cout << std::format("[verbose] input:        {}\n", filename);
        std::cout << std::format("[verbose] output exe:   {}\n", exePath ? *exePath : "(none)");
        std::cout << std::format("[verbose] output lli:   {}\n", lliPath ? *lliPath : "(none)");
        std::cout << std::format("[verbose] output asm:   {}\n", asmPath ? *asmPath : "(none)");
        std::cout << std::format("[verbose] runtime dir:  {}\n", runtimeDir);
        {
            std::string importDirsJoined;
            for (const auto& d : importSearchDirs)
                importDirsJoined += (importDirsJoined.empty() ? "" : ", ") + d;
            std::cout << std::format("[verbose] import dir:   {}\n", importDirsJoined.empty() ? "(none)" : importDirsJoined);
        }
        std::cout << std::format("[verbose] debug info:   {}\n", debugInfo ? "yes" : "no");
        // Effective codegen CPU: the platform default unless --cpu overrode it. Tune
        // defaults to the target CPU unless --tune set it (mirrors -mtune semantics).
        std::string effectiveCpu = targetCpu_.empty()
            ? DefaultCpuForPlatform(platformOption) : targetCpu_;
        std::cout << std::format("[verbose] target cpu:   {}\n", effectiveCpu);
        std::cout << std::format("[verbose] tune:         {}\n", tuneCpu_.empty() ? effectiveCpu + " (same as target cpu)" : tuneCpu_);
    }

    auto checkOutputDir = [](const std::optional<std::string>& path, const char* flag) -> bool {
        if (!path) return true;
        auto dir = std::filesystem::path(*path).parent_path();
        if (!dir.empty() && !std::filesystem::exists(dir))
        {
            // Create the LEAF output directory whose parent already exists (`-o out/x.exe` in a
            // fresh clone); a deeper missing path is still a typo and still reports.
            std::error_code ec;
            auto parent = dir.parent_path();
            if ((parent.empty() || std::filesystem::exists(parent))
                && std::filesystem::create_directory(dir, ec) && !ec)
                return true;
            std::cout << std::format("Error: output directory '{}' does not exist ({} {}).\n", dir.string(), flag, *path);
            return false;
        }
        return true;
    };
    if (!checkOutputDir(exePath, "-o") || !checkOutputDir(lliPath, "--out-lli")
        || !checkOutputDir(asmPath, "--out-asm")
        || !checkOutputDir(args.getOption("isolated-manifest"), "--isolated-manifest"))
        return false;

    if (!std::filesystem::exists(filename))
    {
        std::cout << std::format("Error: input file '{}' does not exist.\n", filename);
        return false;
    }

    // Set the platform constant (__PLATFORM__) based on the target platform.
    // This is a compile-time constant available in all compiled files.
    platformValue = (platformOption == "win32") ? 32 : 64;
    targetWindows_ = (platformOption == "win32" || platformOption == "win64");
    // macos / macos-arm64: Darwin Mach-O on Apple arm64. POSIX (not Windows),
    // 64-bit, arm64 architecture. The only non-x86 target, so it drives the
    // triple, struct ABI, and va_list lowering off targetArm64_.
    targetMacOS_ = (platformOption == "macos" || platformOption == "macos-arm64");
    targetArm64_ = targetMacOS_;
    // `long` follows the target's C ABI: 32-bit on Windows (LLP64) / win32, 64-bit on LP64.
    SetTargetLongWidth(targetWindows_, platformValue);
    if (verbose) std::cout << std::format("[verbose] __PLATFORM__ = {}, __WINDOWS__ = {}, __MACOS__ = {}, arm64 = {}\n",
                                          platformValue, targetWindows_ ? 1 : 0,
                                          targetMacOS_ ? 1 : 0, targetArm64_ ? 1 : 0);

    // Try to load core bitcode cache BEFORE setting up the module, because
    // LoadCoreBitcodeIfFresh replaces context/module/builder with a fresh LLVM
    // context loaded from the bitcode file.  This avoids named-type conflicts with
    // the %string type pre-created by RegisterBuiltinString in Init().
    // --run is read-only (see the run-mode note below): it touches no on-disk caches, so the
    // import/core-bitcode cache is disabled here just like an explicit --no-cache.
    // Batch (--check) loads the cache too: ResetForReanalysis leaves each file in the same
    // state a fresh backend is in at this point, and the load replaces context/module wholesale.
    cachedFunctionNames_.reset();
    bool bitcodeLoaded = false;
    // Isolated mode reparses core sources so import gating sees source import statements;
    // warm-path capability gating is deferred to a later stage.
    if (!noCache_ && !runMode_ && !runtimeDir.empty() && !skipRuntimeImport
        && !RootFileNameIsCore(std::filesystem::path(filename).filename().string()))
    {
        std::string bcCacheDir = GetRuntimeBitcodeDir(runtimeDir);
        if (verbose)
            std::cout << std::format("[verbose] cache dir: {} (rule: {})\n",
                                     GetCflatCacheDir(), GetCflatCacheDirRule());
        if (!bcCacheDir.empty())
        {
            llvm::TimeTraceScope bcScope("RuntimeImport", "core.bc");
            bitcodeLoaded = LoadCoreBitcodeIfFresh(bcCacheDir, platformOption);
            if (bitcodeLoaded)
            {
                cachedFunctionNames_.emplace();
                for (const auto& function : module->functions())
                    cachedFunctionNames_->insert(function.getName().str());
            }
            if (verbose)
                std::cout << std::format("[verbose] core bitcode cache: {}\n", bitcodeLoaded ? "hit" : "miss");
            if (bitcodeLoaded)
            {
                RecordDependency((std::filesystem::path(bcCacheDir)
                                  / ("core_" + platformOption + ".bc")).string());
                for (const auto& imported : importedFiles)
                    RecordDependency(imported);
            }
            // Output compilation may fold core constructors while walking user code. Keep that
            // eager frontend behavior; --check and LSP Analyze retain the lazy, body-free path.
            if (bitcodeLoaded && !checkOnly)
                MaterializeCoreIfLazy();
        }
    }

    // Set the module data layout when not loading from cache.
    // Cache path: the module already has the correct layout from the bitcode.
    // Must precede InitDebugInfo so pointer-width queries during DI construction
    // see the real layout instead of the LLVM default.
    if (!bitcodeLoaded)
    {
        // arm64-apple-macosx layout for the Darwin cross-target; x86 layout otherwise.
        // Set here (not just at emit time) so IR generation, optimization, and codegen
        // all use one consistent layout.
        const char* dl = targetMacOS_
            ? "e-m:o-i64:64-i128:128-n32:64-S128"
            : PlatformDataLayout(platformValue);
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

        SetPlatformMacros();

        if (verbose) std::cout << std::format("[verbose] macros: __FILE__ = \"{}\", __PLATFORM__ = {}\n", sourceFileName, platformValue);
    }

    if (!bitcodeLoaded && !runtimeDir.empty() && !skipRuntimeImport)
    {
        {
            llvm::TimeTraceScope runtimeScope("RuntimeImport", "runtime.cb");
            auto runtimePath = std::filesystem::path(runtimeDir) / "core" / "runtime.cb";
            if (verbose) std::cout << std::format("[verbose] auto-importing runtime: {}\n", runtimePath.string());
            if (std::filesystem::exists(runtimePath))
                CompileImportedFile(runtimePath.string(), "runtime.cb");
            else if (verbose)
                std::cout << "[verbose]   runtime.cb not found, skipping\n";
        }
    }

    // --heap-audit: pull in the leak oracle so its hooks and C backing object
    // are linked, then InjectHeapAuditIntoMain() wires it into main below. Done here (not via
    // a source import) so any program is auditable without editing it. This is a normal
    // on-demand import - parsed even when the core bitcode cache above was hot.
    if (heapAudit_ && !runtimeDir.empty())
    {
        auto auditPath = std::filesystem::path(runtimeDir) / "core" / "diagnostic" / "heap_audit.cb";
        if (verbose) std::cout << std::format("[verbose] --heap-audit: importing {}\n", auditPath.string());
        if (std::filesystem::exists(auditPath))
            CompileImportedFile(auditPath.string(), "heap_audit.cb");
        else
            LogError("--heap-audit: diagnostic/heap_audit.cb not found next to the compiler; "
                     "cannot instrument.");
    }

    // --sanitize=ownership: pull in the trap shim (defines __cflat_own_trap) so the compiler-
    // emitted deref guards have a symbol to call. Flag-gated import - normal builds pay nothing.
    if (sanitizeOwnership_ && !runtimeDir.empty())
    {
        auto ownPath = std::filesystem::path(runtimeDir) / "core" / "diagnostic" / "own_sanitize.cb";
        if (verbose) std::cout << std::format("[verbose] --sanitize=ownership: importing {}\n", ownPath.string());
        if (std::filesystem::exists(ownPath))
            CompileImportedFile(ownPath.string(), "own_sanitize.cb");
        else
            LogError("--sanitize=ownership: diagnostic/own_sanitize.cb not found next to the compiler; "
                     "cannot instrument.");
    }

    {
        if (verbose) std::cout << std::format("[verbose] parsing {}\n", filename);
        std::ifstream stream;
        stream.open(filename);
        auto sourceLines = ReadFileToLines(stream);

        antlr4::ANTLRInputStream input(stream);
        CFlatLexer lexer(&input);
        antlr4::CommonTokenStream tokens(&lexer);
        CFlatParser parser(&tokens);

        auto localizeMessage = MakeDiagnosticLocalizer();
        CFlatErrorListener errorListener(filename, sourceLines, localizeMessage);
        lexer.removeErrorListeners();
        lexer.addErrorListener(&errorListener);
        parser.removeErrorListeners();
        parser.addErrorListener(&errorListener);
        parser.setErrorHandler(std::make_shared<CFlatErrorStrategy>(localizeMessage));

        CFlatParser::CompilationUnitContext* computeUnit;
        {
            llvm::TimeTraceScope parseScope("Parse", sourceFileName);
            tokens.fill();
            computeUnit = parser.compilationUnit();
        }
        if (verbose) std::cout << std::format("[verbose]   parse complete ({} tokens)\n", tokens.getTokens().size());

        if (errorListener.hasErrors())
        {
            struct ParserExpectation
            {
                std::string message;
                int firstLine = 0;
                int firstCol = 0;
                int lastLine = 0;
                int lastCol = 0;
                bool hasRange = false;
            };
            std::vector<ParserExpectation> parserExpectedErrors;
            auto isCompleteParserExpectation = [](CFlatParser::ExpectErrorDeclarationContext* expectDecl) {
                if (expectDecl == nullptr) return false;
                auto* literal = expectDecl->StringLiteral();
                auto* literalToken = literal ? literal->getSymbol() : nullptr;
                auto* stop = expectDecl->getStop();
                if (literalToken == nullptr || literalToken->getTokenIndex() == INVALID_INDEX
                    || stop == nullptr || stop->getType() == cflat::kTokenEOF)
                    return false;
                return stop->getText() == ";" || stop->getText() == "}";
            };
            std::function<void(antlr4::tree::ParseTree*)> collectParserExpectations =
                [&](antlr4::tree::ParseTree* node) {
                    auto* expectDecl = dynamic_cast<CFlatParser::ExpectErrorDeclarationContext*>(node);
                    // Recovery can leave a missing literal or closing brace in the tree. Skip
                    // incomplete expectations and let the parse diagnostics stand.
                    if (isCompleteParserExpectation(expectDecl))
                    {
                        ParserExpectation expectation;
                        expectation.message = DequoteStringLiteral(
                            expectDecl->StringLiteral()->getText());
                        const auto& declarations = expectDecl->externalDeclaration();
                        if (!declarations.empty())
                        {
                            auto* first = declarations.front()->getStart();
                            auto* last = expectDecl->getStop();
                            if (first != nullptr && last != nullptr)
                            {
                                expectation.firstLine = first->getLine();
                                expectation.firstCol = first->getCharPositionInLine();
                                expectation.lastLine = last->getLine();
                                expectation.lastCol = last->getCharPositionInLine();
                                expectation.hasRange = true;
                            }
                        }
                        parserExpectedErrors.push_back(std::move(expectation));
                    }
                    if (node == nullptr) return;
                    for (auto* child : node->children)
                        collectParserExpectations(child);
                };
            if (auto* tu = computeUnit->translationUnit())
                collectParserExpectations(tu);
            // Expectations match the English source message (like the listener stage) so they
            // survive --locale; each diagnostic may be claimed by exactly one expectation.
            auto diagnosticMatchText = [&](size_t diagnosticIndex) -> const std::string& {
                const auto& diagnostic = errorListener.getDiagnostics()[diagnosticIndex];
                return diagnostic.sourceMessage.empty() ? diagnostic.message : diagnostic.sourceMessage;
            };
            auto diagnosticIsInExpectationRange = [&](size_t diagnosticIndex,
                                                       const ParserExpectation& expectation) {
                if (!expectation.hasRange) return true;
                const auto& diagnostic = errorListener.getDiagnostics()[diagnosticIndex];
                if (diagnostic.line < expectation.firstLine || diagnostic.line > expectation.lastLine)
                    return false;
                if (diagnostic.line == expectation.firstLine
                    && diagnostic.col < expectation.firstCol)
                    return false;
                if (diagnostic.line == expectation.lastLine
                    && diagnostic.col > expectation.lastCol)
                    return false;
                return true;
            };
            std::vector<int> diagnosticMatches(errorListener.getDiagnostics().size(), -1);
            auto matchExpectation = [&](auto&& self, size_t expectedIndex,
                                         std::vector<bool>& visited) -> bool
            {
                for (size_t diagnosticIndex = 0;
                     diagnosticIndex < errorListener.getDiagnostics().size(); ++diagnosticIndex)
                {
                    if (visited[diagnosticIndex]) continue;
                    const auto& expectation = parserExpectedErrors[expectedIndex];
                    if (!diagnosticIsInExpectationRange(diagnosticIndex, expectation))
                        continue;
                    if (diagnosticMatchText(diagnosticIndex).find(expectation.message)
                        == std::string::npos)
                        continue;
                    visited[diagnosticIndex] = true;
                    if (diagnosticMatches[diagnosticIndex] == -1 ||
                        self(self, diagnosticMatches[diagnosticIndex], visited))
                    {
                        diagnosticMatches[diagnosticIndex] = static_cast<int>(expectedIndex);
                        return true;
                    }
                }
                return false;
            };
            size_t matchedExpectations = 0;
            for (size_t expectedIndex = 0; expectedIndex < parserExpectedErrors.size(); ++expectedIndex)
            {
                std::vector<bool> visited(errorListener.getDiagnostics().size(), false);
                if (matchExpectation(matchExpectation, expectedIndex, visited))
                    ++matchedExpectations;
            }
            bool parserExpectationsMatch =
                !parserExpectedErrors.empty() &&
                matchedExpectations == parserExpectedErrors.size() &&
                matchedExpectations == errorListener.getDiagnostics().size();
            if (parserExpectationsMatch)
            {
                for (size_t i = 0; i < parserExpectedErrors.size(); ++i)
                    std::cout << "PASS: expected error received\n";
                return true;
            }
            ReportParseErrors(errorListener.getDiagnostics(), sourceLines);
            return false;
        }

        // Arm a file-scope bare-semicolon `expect_error("...");` BEFORE imports run, so a
        // diagnostic raised during ProcessImports (e.g. the orphan-header error) is matched
        // rather than aborting the compile. Only the bare form (no inner declarations) is
        // handled here; the scoped-block form is processed by the listener walk as before.
        // First writer wins - a file declares at most one such expectation.
        if (auto* tu = computeUnit->translationUnit())
        {
            for (auto* decl : tu->externalDeclaration())
            {
                auto* ee = decl->expectErrorDeclaration();
                auto* literal = ee ? ee->StringLiteral() : nullptr;
                auto* literalToken = literal ? literal->getSymbol() : nullptr;
                if (ee && ee->externalDeclaration().empty() && literalToken != nullptr
                    && literalToken->getTokenIndex() != INVALID_INDEX)
                {
                    std::string raw = literal->getText();
                    fileScopeExpectedError_ = DequoteStringLiteral(raw);
                    expectedError = fileScopeExpectedError_;
                    expectedErrorScopeDepth = SIZE_MAX;
                    break;
                }
            }
        }

        // ProcessImports / ForwardRefScan / the code-gen walk can all raise the armed
        // file-scope expectation. ExpectedErrorReceived is thrown only on a match (PASS already
        // printed by LogError), so catching it here and returning success is correct.
        try
        {

        ValidateIsolatedParseTree(computeUnit);

        // Process all top-level imports before scanning the main file so that
        // imported symbols are available to ForwardRefScanner.
        {
            llvm::TimeTraceScope importsScope("ProcessImports", sourceFileName);
            if (auto* tu = computeUnit->translationUnit()) {
                for (auto* decl : tu->externalDeclaration()) {
                    if (auto* imp = decl->importDeclaration()) {
                        // Point diagnostics at this import statement. A not-found error is raised
                        // before any real file is opened (there is no readable file behind it, e.g.
                        // a winmd), so without this it would report (0,0) instead of the import line.
                        SetSourceLocation(imp->getStart()->getLine(), imp->getStart()->getCharPositionInLine());
                        if (auto error = ValidateImportLeadingMarker(imp); !error.empty()) {
                            LogError(error);
                            return false;
                        }
                        if (auto error = ValidateImportSoftKeywords(imp); !error.empty()) {
                            LogError(error);
                            return false;
                        }
                        if (isolatedPolicy_ && IsIsolatedNativeImport(imp))
                        {
                            LogError(std::format("policy-restricted-language: native interop imports "
                                                 "are not allowed in isolated mode (policy '{}')",
                                                 isolatedPolicy_->path));
                            return false;
                        }
                        // `import { "a", "b" };` - a group of >1 plain imports. Each entry
                        // routes like a plain `import "x";` (no per-entry alias/lib/define). A
                        // trailing `cache` on the group applies to every entry (a no-op for
                        // .cb/.c entries; only the .h header-bind branch consults it).
                        auto groupedImports = DequoteImportGroup(imp);
                        // `import framework "X";` / `import framework { ... };` - record each
                        // framework for the Mach-O link. Dispatched before the importGroup
                        // routing since it reuses importGroup for its name list.
                        if (IsFrameworkImport(imp))
                        {
                            for (const auto& fw : groupedImports)
                                if (!AddFrameworkImport(fw)) return false;
                            continue;
                        }
                        // A `framework "X"` clause on a header/package/group import (S3): link the
                        // framework in addition to binding the header. Standalone form handled above.
                        for (const auto& fw : DequoteFrameworkClauses(imp))
                            if (!AddFrameworkImport(fw)) return false;
                        // `import package-nuget importGroup from "id[/version]";` - dispatch on the
                        // keyword BEFORE any importGroup-based routing (package-nuget now also
                        // carries an importGroup, but a multi-entry nuget group is ONE package TU,
                        // not several plain imports). Skips the regular import dedup/parse machinery.
                        if (IsPackageNugetImport(imp))
                        {
                            std::string packageSpec = DequoteFromClause(imp);
                            if (verbose) std::cout << std::format("[verbose] nuget import (group of {}) from {}\n", groupedImports.size(), packageSpec);
                            if (!CompileNugetImport(groupedImports, packageSpec, DequoteDefineClauses(imp), DequotePriClause(imp)))
                                return false;
                            continue;
                        }
                        if (groupedImports.size() > 1)
                        {
                            if (verbose) std::cout << std::format("[verbose] import requested (group of {})\n", groupedImports.size());
                            if (!CompileImportGroup(filename, groupedImports, DequoteLibClauses(imp),
                                                    DequoteDefineClauses(imp), HasCacheClause(imp)))
                                return false;
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
                            importFilename = DequoteStringLiteral(raw);
                        }
                        // `import package-vcpkg "header" from "port";` - dispatch to the
                        // vcpkg resolver and skip the regular import dedup/parse machinery.
                        if (IsPackageVcpkgImport(imp))
                        {
                            std::string portSpec = DequoteFromClause(imp);
                            if (verbose) std::cout << std::format("[verbose] vcpkg import: {} from {}\n", importFilename, portSpec);
                            if (!CompileVcpkgImport(RootVcpkgImportPath(filename), importFilename, portSpec, DequoteDefineClauses(imp)))
                                return false;
                            continue;
                        }
                        std::string ns = ImportNamespace(imp);
                        bool isProgram = imp->children.size() >= 2 && imp->children[1]->getText() == "program";
                        std::string alias = isProgram ? ImportProgramAlias(imp) : "";
                        std::string impExt = LowerExtension(importFilename);
                        bool isCProgram = isProgram && impExt == ".c";
                        std::vector<std::string> explicitLibs = DequoteLibClauses(imp);
                        std::vector<std::string> extraDefines = DequoteDefineClauses(imp);
                        bool cacheHeader = HasCacheClause(imp);
                        if (verbose) std::cout << std::format("[verbose] import requested: {}{}{}\n", importFilename, ns.empty() ? "" : " as " + ns, cacheHeader ? " (cache)" : "");
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
            if (verbose) std::cout << std::format("[verbose] forward-ref scan ({})\n", sourceFileName);
            ForwardRefScanner scanner(this);
            scanner.SetTokens(&tokens);
            scanner.ValidateIsolatedIntegralPointerCasts(computeUnit);
            // First pass: pre-declare opaque types and constructors for every
            // generic instantiation found anywhere in the file (including inside
            // function bodies), so uses like Box<MyType> b = Box$MyType() resolve.
            // Generic interface templates first: a use like Container<int> must not be
            // pre-declared as an opaque struct shell (it is a fat-pointer interface).
            if (auto* tu = computeUnit->translationUnit())
            {
                // Pure-rename `using` aliases first: MangleTypeArg folds them, and both passes
                // must see the same set no matter where the `using` sits in the file.
                scanner.PreRegisterRenameAliases(tu);
                scanner.SeedConstIntGlobals(tu);
                scanner.ScanGenericInterfaceTemplateNames(tu);
                for (auto* decl : tu->externalDeclaration())
                    scanner.ScanGenericTypeUses(decl);
                // Second pass: register non-generic struct shells and function signatures.
                for (auto* decl : tu->externalDeclaration())
                    scanner.ScanExternalDeclaration(decl);
                scanner.PrepareWinrtClasses(tu);
            }
        }

        // Cross-thread sharing scan (--xthread-scan N): a read-only pre-pass that records
        // the struct types escaping across a thread-spawn boundary, so the codegen walk can
        // report non-atomic, non-guarded field accesses of those types. Silent at level 0.
        ScanCrossThreadEscapes(computeUnit);

        if (verbose) std::cout << std::format("[verbose] code-gen walk ({})\n", sourceFileName);
        {
            llvm::TimeTraceScope codegenScope("CodeGeneration", sourceFileName);
            auto myListener = std::make_unique<MainListener>(&parser, this, sourceFileName);
            auto walker = antlr4::tree::ParseTreeWalker();
            walker.walk(myListener.get(), computeUnit);
            {
                NoCurrentFunctionScope noCurrent(this);
                myListener->ResolvePendingGlobalDefaultConstructions();
            }
            // Now that every implementor is registered, emit the interface rebox if-chains.
            // Resolve-created calls can register deferred interface work, so resolve first.
            EmitDeferredInterfaceReboxBodies();
            // Now that every type and generic monomorphization is registered, fill in the
            // bodies of deferred delete-site destructor wrappers (recursive containers whose
            // element type was incomplete when the container dtor was emitted).
            EmitDeferredFullDestructorBodies();
        }
        stream.close();
        if (verbose) std::cout << "[verbose]   walk complete\n";

        // Move-dataflow: solve the MaybeMoved fixpoint over the fully-emitted module and report
        // any loop-carried / cross-block use-after-move the inline checker missed (LogError).
        // Must run INSIDE this try and BEFORE the did-not-occur check so a matching error is
        // caught as ExpectedErrorReceived (file-scope expect_error stays armed to here).
        RunMoveDataflow();

        // Fire deferred compile_error() diagnostics for any poisoned function that is actually
        // called (e.g. copy() on a list of unique elements). Inside the try + before the
        // did-not-occur check so a file-scope expect_error catches it.
        CheckPoisonedFunctionCalls();

        // Every callee body is complete now, so a call whose callee was still a declaration
        // during the walk can finally be answered (record-then-resolve; see tempUniqueFieldArgs_).
        // The walk is over, so `currentFunction` is only the last function the file happened to
        // define - and FunctionBodyIsComplete refuses it, which would exempt exactly that callee.
        {
            NoCurrentFunctionScope noCurrent(this);
            ResolveOwningLocalBorrowingHelperArgs();
            ResolveTempUniqueFieldArgEscapes();

            // The RETURN half of the same deferral: the escape SITE, not the callee question.
            // After the store half, matching the eager order within a statement.
            ResolveDeferredTempUniqueFieldEscapes();
        }

        // Every generic instantiation has been drained by now, so interfaceTable is COMPLETE:
        // resolve the materialisation records captured during the walk. Inside the try so a
        // negative test can expect_error it.
        ResolveMaterializedInterfaceUses();

        }
        catch (const ExpectedErrorReceived&)
        {
            // A file-scope expectation matched somewhere in imports / scan / walk. The PASS
            // line is already printed; report success so --check counts the file as passing.
            return true;
        }

        // The armed file-scope expectation never fired (had it fired, the catch above would
        // have returned). That is the did-not-occur failure for this negative test.
        if (!fileScopeExpectedError_.empty())
        {
            std::cout << std::format("FAIL: expected error '{}' did not occur\n", fileScopeExpectedError_);
            FailCompilation("expected error did not occur");
        }
    }

    {
        llvm::TimeTraceScope debugScope("FinalizeDebugInfo");
        if (verbose) std::cout << "[verbose] finalizing debug info\n";
        FinalizeDebugInfo();
    }

    // --heap-audit: now that all of main's returns are emitted, instrument it with the
    // enable()/reportLeaks() oracle calls. Must run before VerifyModule.
    if (heapAudit_)
        InjectHeapAuditIntoMain();

    // validate that the stack is empty.
    if (stackNamedVariable.size() > 0)
    {
        for (const auto& stack : stackNamedVariable)
        {
            std::cout << "Warning: scope stack is not empty at end of compilation:\n";
            for (const auto& funcVariable : stack.functionArgument)
            {
                std::cout << std::format("  Function var: {}\n", funcVariable.first);
            }
            for (const auto& namedVariable : stack.namedVariable)
            {
                std::cout << std::format("  namedVar var: {}\n", namedVariable.first);
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
            std::cout << "Error: module verification failed.\n";
            return false;
        }
    }

    // Isolated-policy checks audit the optimized module, so retain DCE for that consumer.
    if (!checkOnly || isolatedPolicy_)
    {
        MaterializeCoreIfLazy();
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
            if (verbose) std::cout << std::format("[verbose] running optimizations (O{}{})\n", optLevel, asan_ ? ", asan" : "");
            OptimizeModule(optLevel);
        }
        else if (!args.hasFlag("no-opt"))
        {
            // -O0 (no asan): OptimizeModule was skipped, so nothing pruned the unreachable
            // internal functions the whole core library contributes. Run GlobalDCE alone so
            // codegen does not instruction-select dead core. Before the --out-lli write below
            // so the dumped IR still matches what lands in the object.
            llvm::TimeTraceScope dceScope("GlobalDCE", "O0");
            if (verbose) std::cout << "[verbose] running -O0 global dead-code elimination\n";
            RunGlobalDCE();
        }
    }

    if (isolatedPolicy_ && IsIsolatedCoreSource())
        for (auto& function : module->functions())
            if (!function.isDeclaration()) isolatedCoreFunctions_.insert(function.getName().str());

    if (isolatedPolicy_ && !InstrumentIsolatedResources())
        return false;

    if (isolatedPolicy_)
        MaterializeCoreIfLazy();
    if (isolatedPolicy_ && !VerifyModule())
    {
        std::cout << "Error: module verification failed after isolated resource instrumentation.\n";
        return false;
    }

    if (!AuditIsolatedModule())
        return false;

    // Standalone --out-lli (no -o): dump the post-optimization IR here, since EmitExecutable
    // does not run. For an -o build the dump is deferred into EmitExecutable (after the target
    // triple/data layout are finalized, right before codegen) so the .ll matches the object.
    if (lliPath && !exePath)
    {
        llvm::TimeTraceScope irScope("WriteIR", *lliPath);
        if (verbose) std::cout << std::format("[verbose] writing IR to {}\n", *lliPath);
        if (!SaveToFile(*lliPath))
        {
            std::cout << std::format("Error: failed to save IR to '{}'.\n", *lliPath);
            return false;
        }
    }

    // The module is already optimized at this point; do not ask PrintModuleView to optimize it again.
    // PrintModuleView clones the module and creates its own target machine for assembly emission.
    if (asmPath)
    {
        llvm::TimeTraceScope asmScope("WriteAssembly", *asmPath);
        if (verbose) std::cout << std::format("[verbose] writing asm to {}\n", *asmPath);
        std::string assembly;
        std::ofstream output(*asmPath, std::ios::binary | std::ios::trunc);
        if (!PrintModuleView(assembly, "asm", 0, "", true)
            || !output
            || !(output.write(assembly.data(), static_cast<std::streamsize>(assembly.size()))))
        {
            std::cout << std::format("Error: failed to save asm to '{}'.\n", *asmPath);
            return false;
        }
    }

    if (!bitcodePath.empty())
    {
        if (verbose) std::cout << std::format("[verbose] writing bitcode to {}\n", bitcodePath);
        if (!WriteBitcode(bitcodePath))
        {
            std::cout << std::format("Error: failed to write bitcode to '{}'.\n", bitcodePath);
            return false;
        }
    }

    // Dispatch any extra positional .c files to clang and link their objects.
    // Skipped in check mode: extra positionals are independent source files there.
    if (!checkOnly)
    {
        auto isCSource = [](const std::string& p) {
            return LowerExtension(p) == ".c";
        };
        bool anyCSource = false;
        for (size_t i = 1; i < args.positionalCount(); ++i)
            if (isCSource(*args.getPositional(i))) anyCSource = true;

        if (anyCSource && !exePath)
        {
            std::cout << "Error: C source input requires -o to link the resulting object.\n";
            return false;
        }
        for (size_t i = 1; i < args.positionalCount(); ++i)
        {
            auto cPath = *args.getPositional(i);
            if (!isCSource(cPath)) continue;
            if (!std::filesystem::exists(cPath))
            {
                std::cout << std::format("Error: C source input '{}' does not exist.\n", cPath);
                return false;
            }
            if (!CompileCFile(cPath))
                return false;
        }
    }

    // --run can load objects compiled from imported C source, but it deliberately does not
    // support prebuilt C libraries. Check after imports so inline `lib` clauses and package
    // imports are covered as well as direct --c-lib arguments.
    if (runMode_ && !cLinkLibs_.empty())
    {
        LogError("--run does not support prebuilt C libraries (from --c-lib, package imports, or "
                 "an inline lib clause). Remove the library binding or compile with -o instead.");
        return false;
    }

    for (const auto& lib : cLinkLibs_)
        RecordDependency(lib);

    // --run: JIT and execute in-process, emitting nothing to disk. Mutually exclusive with
    // -o (checked in main). Consumes the module, so it must come after IR/bitcode writes.
    //
    // NOTE: --run is read-only - it writes nothing to disk. No exe is produced; the disk-writing
    // output flags (-o, -l/--out-lli, --out-asm, -b/--bitcode) are rejected up front in main(); and the
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
        std::string emitPath = *exePath;
        bool isolatedOutput = isolatedPolicy_.has_value();
        if (isolatedOutput)
        {
            std::filesystem::path candidate = std::filesystem::path(*exePath);
            candidate += ".isolated.tmp";
            for (unsigned suffix = 1; std::filesystem::exists(candidate); ++suffix)
                candidate = std::filesystem::path(*exePath)
                    .concat(std::format(".isolated.tmp.{}", suffix));
            emitPath = candidate.string();
        }
        struct IsolatedTempCleanup
        {
            std::string path;
            bool active = true;
            ~IsolatedTempCleanup()
            {
                if (active && !path.empty()) std::filesystem::remove(path);
            }
        } tempCleanup{isolatedOutput ? emitPath : std::string(), isolatedOutput};
        if (verbose) std::cout << std::format("[verbose] emitting executable to {}\n", emitPath);
        if (!EmitExecutable(emitPath, platformOption, debugInfo, lliPath))
        {
            std::cout << std::format("Error: failed to emit executable '{}'.\n", *exePath);
            return false;
        }
        if (isolatedOutput)
        {
            if (!AuditIsolatedExecutable(emitPath))
                return false;
            std::error_code renameError;
            std::filesystem::rename(emitPath, *exePath, renameError);
            if (renameError)
            {
                LogError(std::format("policy-module-denied: cannot publish isolated executable '{}' "
                                     "after audit: {}", *exePath, renameError.message()));
                return false;
            }
            tempCleanup.active = false;
        }
    }

    // --dump-manifest: emit the merged XML document that would be embedded as the RT_MANIFEST
    // resource. Independent of the target OS, and runs under --check, so a bad identity is
    // inspectable without a Windows launch.
    if (auto dumpPath = args.getOption("dump-manifest"))
    {
        if (manifestFragments_.empty())
            std::cout << "No manifest declaration in this program.\n";
        else if (auto merged = MergeManifestFragments())
        {
            std::string document =
                "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n" + *merged + "\n";
            if (dumpPath->empty() || *dumpPath == "-")
                std::cout << document;
            else
            {
                std::ofstream out(*dumpPath, std::ios::binary | std::ios::trunc);
                out.write(document.data(), static_cast<std::streamsize>(document.size()));
                if (!out)
                {
                    LogError(std::format("could not write manifest dump '{}'", *dumpPath));
                    return false;
                }
            }
        }
    }

    if (isolatedPolicy_)
        if (auto manifestPath = args.getOption("isolated-manifest"))
        {
            std::string kind = "check";
            std::optional<std::string> outputPath;
            if (!bitcodePath.empty())
            {
                kind = "bitcode";
                outputPath = bitcodePath;
            }
            else if (lliPath)
            {
                kind = "llvm-ir";
                outputPath = *lliPath;
            }
            else if (exePath)
            {
                kind = "executable";
                outputPath = *exePath;
            }
            if (!WriteIsolatedManifest(*manifestPath, kind, outputPath))
                return false;
        }

    return true;
}

LLVMBackend::CachedParseTree* LLVMBackend::GetOrParseFile(const std::string& canonicalPath, const std::string& displayName)
{
    // Reuse an imported tree when the file on disk is unchanged.
    auto it = parseTreeCache_.find(canonicalPath);
    if (it != parseTreeCache_.end())
    {
        std::error_code tec;
        std::error_code sec;
        auto wt = std::filesystem::last_write_time(canonicalPath, tec);
        auto fileSize = std::filesystem::file_size(canonicalPath, sec);
        if (!tec && !sec && wt == it->second->writeTime && fileSize == it->second->fileSize)
        {
            llvm::TimeTraceScope cacheScope("Parse", "cached:" + displayName);
            if (verbose) std::cout << std::format("[verbose]   parse cache hit: {}\n", displayName);
            return it->second.get();
        }
        parseTreeCache_.erase(it);  // stale or unstatable - re-parse below
    }

    auto entry = std::make_unique<CachedParseTree>();
    entry->canonicalPath = canonicalPath;

    std::ifstream stream(canonicalPath);
    if (!stream.is_open())
    {
        std::cout << std::format("Error: failed to open imported file '{}'.\n", canonicalPath);
        return nullptr;
    }
    auto sourceLines = ReadFileToLines(stream);  // also rewinds the stream to the start

    entry->input  = std::make_unique<antlr4::ANTLRInputStream>(stream);
    entry->lexer  = std::make_unique<CFlatLexer>(entry->input.get());
    entry->tokens = std::make_unique<antlr4::CommonTokenStream>(entry->lexer.get());
    entry->parser = std::make_unique<CFlatParser>(entry->tokens.get());

    auto localizeMessage = MakeDiagnosticLocalizer();
    CFlatErrorListener errorListener(canonicalPath, sourceLines, localizeMessage);
    entry->lexer->removeErrorListeners();
    entry->lexer->addErrorListener(&errorListener);
    entry->parser->removeErrorListeners();
    entry->parser->addErrorListener(&errorListener);
    entry->parser->setErrorHandler(std::make_shared<CFlatErrorStrategy>(localizeMessage));

    {
        llvm::TimeTraceScope parseScope("Parse", displayName);
        entry->tokens->fill();
        entry->unit = entry->parser->compilationUnit();
    }
    if (verbose) std::cout << std::format("[verbose]   parse complete ({} tokens)\n", entry->tokens->getTokens().size());

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
    std::error_code tec;
    std::error_code sec;
    entry->writeTime = std::filesystem::last_write_time(canonicalPath, tec);
    entry->fileSize = std::filesystem::file_size(canonicalPath, sec);
    parseTreeCache_[canonicalPath] = std::move(entry);
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

// Standard POSIX system include roots, so a bare `import "math.h";` resolves to the libc
// header on Linux/macOS the way `import "windows.h";` resolves against the SDK on Windows.
// The resolved header's own directory then becomes an in-scope -I root for clang extraction.
// Only the dirs that exist on this host are returned. Scanned once and cached.
static const std::vector<std::string>& PosixSystemIncludeDirs()
{
    static const std::vector<std::string> dirs = [] {
        std::vector<std::string> out;
        std::error_code ec;
#if defined(__APPLE__)
        // macOS has no top-level /usr/include; C system headers live in the active
        // SDK. Prefer $SDKROOT, else `xcrun --show-sdk-path`, then <sdk>/usr/include.
        std::string sdk;
        if (const char* env = std::getenv("SDKROOT")) sdk = env;
        if (sdk.empty()) sdk = CaptureToolLine("xcrun --show-sdk-path 2>/dev/null");
        if (!sdk.empty())
        {
            std::string inc = sdk + "/usr/include";
            if (std::filesystem::exists(inc, ec)) out.push_back(inc);
        }
#endif
        for (const char* cand : { "/usr/include", "/usr/local/include",
                                  "/usr/include/x86_64-linux-gnu" })
        {
            if (std::filesystem::exists(cand, ec)) out.push_back(cand);
        }
        return out;
    }();
    return dirs;
}

// %SystemRoot%\System32\WinMetadata - the system WinRT metadata store - so a bare
// `import "Windows.Foundation.winmd";` resolves without spelling out the full path. Empty if
// SystemRoot is unset. Scanned once and cached.
static const std::string& WinMetadataDir()
{
    static const std::string dir = [] {
        char buf[260] = {};
        size_t len = 0;
        if (getenv_s(&len, buf, sizeof(buf), "SystemRoot") != 0 || len == 0)
            return std::string();
        return std::string(buf) + "\\System32\\WinMetadata";
    }();
    return dir;
}

// Resolve an import filename against the search order described in the header. Logs the
// not-found error (unless quiet) and returns false on failure.
bool LLVMBackend::ResolveImportPath(const std::string& importingFilePath, const std::string& importFilename,
                                    std::string& outCanonical, bool quiet)
{
    auto importingDir = std::filesystem::path(importingFilePath).parent_path();

    // Canonical path of the file doing the importing, used to reject self-resolution below.
    // Empty (checks disabled) when it does not exist on disk, e.g. an LSP temp buffer.
    std::error_code selfEc;
    auto importingCanonical =
        std::filesystem::canonical(std::filesystem::path(importingFilePath).lexically_normal(), selfEc);
    if (selfEc) importingCanonical.clear();

    // Resolve a candidate filename against the full import search order. Returns the canonical
    // path on success, or an empty string if it was not found in any search location.
    auto resolve = [&](const std::string& filename) -> std::string {
        std::error_code ec;
        // A file cannot import itself. The importing file's own directory is probed first, so
        // without this a same-basename import (core/ui_native/cocoa.cb importing "cocoa.cb")
        // would self-resolve instead of reaching the intended file on a later search dir.
        // Remember the self-hit: if nothing else resolves, return it so a genuine self-import
        // still reports as a circular import rather than a bare "not found".
        std::string selfHit;
        auto rejectSelf = [&](const std::filesystem::path& candidate) {
            if (ec || importingCanonical.empty() || candidate != importingCanonical) return false;
            selfHit = candidate.string();
            ec = std::make_error_code(std::errc::no_such_file_or_directory);
            return true;
        };

        auto canonical = std::filesystem::canonical((importingDir / filename).lexically_normal(), ec);
        rejectSelf(canonical);

        // Try an additional base directory (no-op if already resolved or dir is empty).
        auto tryDir = [&](const std::filesystem::path& dir) {
            if (!ec || dir.empty()) return;
            auto candidate = std::filesystem::canonical((dir / filename).lexically_normal(), ec);
            if (!ec && !rejectSelf(candidate)) canonical = candidate;
        };

        // For LSP analysis: the "importing" file is a temp file; try the real source directory first.
        tryDir(sourceFileDir_);
        for (const auto& d : importSearchDirs) tryDir(d);
        // C library headers (e.g. "curl/curl.h") live under the --c-include roots.
        for (const auto& inc : cIncludeDirs_) tryDir(inc);
        // Implicit fallback: look in the "core" directory beside the runtime.
        if (!runtimeDir.empty()) tryDir(std::filesystem::path(runtimeDir) / "core");
        // System headers (e.g. windows.h) live in the Windows SDK include dirs. Fall back to the
        // detected SDK include dirs so `import "windows.h"` resolves with no --c-include flag; the
        // header's own dir then becomes an in-scope root automatically (see ExtractCHeaderClang).
        for (const auto& inc : WindowsSdkIncludeDirs()) tryDir(inc);
        // POSIX system headers (e.g. math.h) live under /usr/include et al. Fall back to them
        // so `import "math.h";` resolves with no --c-include flag on Linux/macOS.
        for (const auto& inc : PosixSystemIncludeDirs()) tryDir(inc);
        // System WinRT metadata: let `import "Windows.Foundation.winmd";` resolve by bare name.
        tryDir(WinMetadataDir());
#if defined(__APPLE__)
        // macOS framework headers use a non-flat layout: `<Foo/Bar.h>` lives at
        // <sdk>/System/Library/Frameworks/Foo.framework/Headers/Bar.h. Map that shape so
        // `import package "CoreGraphics/CoreGraphics.h"` resolves to a real path for clang.
        if (ec && targetMacOS_)
        {
            auto slash = filename.find('/');
            const std::string& sdk = MacSdkPathCached();
            if (slash != std::string::npos && !sdk.empty())
            {
                std::filesystem::path fh = std::filesystem::path(sdk) / "System/Library/Frameworks"
                    / (filename.substr(0, slash) + ".framework") / "Headers" / filename.substr(slash + 1);
                // Keep the framework `Headers` symlink path (do NOT canonicalize the
                // Versions/ symlink): clang spells sibling framework headers via `Headers/`,
                // and the extractor's in-scope filter matches that spelling, not Versions/A.
                std::error_code fex;
                if (std::filesystem::exists(fh, fex))
                {
                    canonical = fh.lexically_normal();
                    ec.clear();
                }
            }
        }
#endif
        // Nothing else matched: hand back the self-hit so the caller's cycle check reports it.
        if (ec && !selfHit.empty()) return selfHit;
        return ec ? std::string() : canonical.string();
    };

    if (auto resolved = resolve(importFilename); !resolved.empty())
    {
        outCanonical = resolved;
        return true;
    }

    if (!quiet)
    {
        // If the bare filename (with any directory prefix stripped) would resolve, suggest it.
        // Catches the common `import "core/list.cb"` mistake where `list.cb` is the valid path.
        std::string suggestion;
        auto bare = std::filesystem::path(importFilename).filename().string();
        if (bare != importFilename)
        {
            // Never suggest a name that only resolves to the importing file itself.
            auto bareHit = resolve(bare);
            if (!bareHit.empty() && std::filesystem::path(bareHit) != importingCanonical)
                suggestion = " Did you mean \"" + bare + "\"?";
        }

        std::string importDirsNote;
        for (const auto& d : importSearchDirs)
            importDirsNote += ", import dir '" + d + "'";

        LogError(std::format(
            "imported file not found: {} (searched relative to '{}'{}{}{}{}, Windows SDK, "
            "WinMetadata).{}",
            importFilename,
            importingDir.string(),
            sourceFileDir_.empty() ? "" : ", source dir '" + sourceFileDir_ + "'",
            importDirsNote,
            cIncludeDirs_.empty() ? "" : ", " + std::to_string(cIncludeDirs_.size()) + " --c-include dir(s)",
            runtimeDir.empty() ? "" : ", runtime core '" + runtimeDir + "/core'",
            suggestion));
    }
    return false;
}

// Bind a grouped import `import { "a", "b" } lib {...} define ...;`. Non-header entries route
// individually (each like a plain `import "x";`) in listed order; every header entry of the
// group is bound as ONE translation unit, in listed order, so an earlier header satisfies a
// later one's prerequisites (the explicit replacement for the old windows.h prepend special
// case). Group-level `lib`/`define`/`cache` clauses apply to the whole header group.
bool LLVMBackend::CompileImportGroup(const std::string& importingFilePath,
                                     const std::vector<std::string>& entries,
                                     const std::vector<std::string>& groupLibs,
                                     const std::vector<std::string>& groupDefines,
                                     bool cacheGroup)
{
    std::vector<std::string> headerCanonicals;
    bool anyNewHeader = false;
    for (const auto& entry : entries)
    {
        std::string ext = LowerExtension(entry);
        const bool isHeader = (ext == ".h" || ext == ".hpp" || ext == ".hh");
        // Same objc runtime auto-link as the single-import path in CompileImportedFile.
        if (isHeader && targetMacOS_ && entry.starts_with("objc/"))
            cLinkObjC_ = true;
        if (!isHeader)
        {
            // .cb / .c entries are independent - route each like a plain `import "x";`.
            if (!CompileImportedFile(importingFilePath, entry))
                return false;
            continue;
        }
        // Header entry: resolve, then defer to the single grouped extraction below. Dedup applies
        // to REGISTRATION, never to the TU inclusion context: every listed header must still be
        // #included (in listed order) so an earlier entry satisfies a later one's prerequisites.
        // An already-imported header keeps its place in the include list - its decls are skipped
        // at registration time (first-writer-wins in RegisterC*). Dropping it here would strip the
        // group's prerequisite context and re-orphan the remaining entries.
        std::string canonicalStr;
        if (!ResolveImportPath(importingFilePath, entry, canonicalStr))
            return false;
        RecordDependency(canonicalStr);
        headerCanonicals.push_back(canonicalStr);
        if (importedFiles.count(canonicalStr))
        {
            if (verbose) std::cout << std::format("[verbose]   group header already imported - kept for include context, registration skipped: {}\n", canonicalStr);
            continue;
        }
        importedFiles.insert(canonicalStr);
        anyNewHeader = true;
    }

    if (headerCanonicals.empty())
        return true;   // group held only .cb/.c entries (already routed)
    if (!anyNewHeader)
    {
        // Every header in the group was already imported elsewhere (a repeated group, or one
        // following standalone imports of all its members) - fully redundant, nothing to register.
        if (verbose) std::cout << "[verbose]   group fully redundant - all headers already imported\n";
        return true;
    }

    // Group-level `lib` clauses link against the whole group (e.g. user32.lib + gdi32.lib).
    for (const auto& lib : groupLibs)
        if (!lib.empty())
            cLinkLibs_.push_back(ResolveCLinkLib(lib, importingFilePath));

    bool ok = CompileCHeaderGroup(headerCanonicals, groupDefines, cacheGroup);
    if (ok) ProcessPendingMacroSources();
    return ok;
}

bool LLVMBackend::CompileImportedFile(const std::string& importingFilePath, const std::string& importFilename, const std::string& namespaceName, const std::string& programAlias, const std::vector<std::string>& explicitLibs, const std::vector<std::string>& extraDefines, bool cacheHeader)
{
    if (isolatedPolicy_)
    {
        auto ext = LowerExtension(importFilename);
        if (ext == ".c" || ext == ".h" || ext == ".hpp" || ext == ".hh" || ext == ".winmd")
        {
            LogError(std::format("policy-restricted-language: native interop import '{}' is not "
                                 "allowed in isolated mode (policy '{}')",
                                 importFilename, isolatedPolicy_->path));
            return false;
        }
    }
    // WinRT metadata (.winmd) is a Windows-only feature. Reject it up front when not
    // targeting Windows - otherwise path resolution fails with a generic "file not
    // found" that unhelpfully lists the (absent) Windows SDK / WinMetadata dirs.
    if (!targetWindows_ && LowerExtension(importFilename) == ".winmd")
    {
        LogErrorMessage("import '{}': WinRT metadata (.winmd) is only supported when targeting Windows. "
                        "WinMD/WinRT is a Windows-only feature; guard the import with "
                        "'if const (__WINDOWS__) {{ import \"...\"; }}'.",
                        { importFilename });
        return false;
    }
    std::string canonicalStr;
    if (!ResolveImportPath(importingFilePath, importFilename, canonicalStr))
        return false;
    RecordDependency(canonicalStr);
    if (isolatedPolicy_ && !runtimeDir.empty())
    {
        std::error_code coreEc;
        auto coreDirectory = std::filesystem::canonical(
            std::filesystem::path(runtimeDir) / "core", coreEc);
        if (!coreEc)
        {
            auto capability = ClassifyIsolatedCoreModule(canonicalStr, coreDirectory);
            if (capability && isolatedPolicy_->IsDenied(*capability))
            {
                LogError(std::format("policy-capability-denied: import '{}' requires capability "
                                     "'{}', denied by policy '{}'",
                                     importFilename, IsolatedCapabilityName(*capability),
                                     isolatedPolicy_->path));
                return false;
            }
            if (capability && *capability == IsolatedCapability::Threads
                && isolatedPolicy_->maxThreads && *isolatedPolicy_->maxThreads == 1)
            {
                LogError(std::format("resource-limit: import '{}' contains a thread construct, "
                                     "but policy 'max_threads' is 1 (policy '{}')",
                                     importFilename, isolatedPolicy_->path));
                return false;
            }
        }
    }
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
        if (verbose) std::cout << std::format("[verbose]   already imported: {}\n", canonicalStr);
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
        std::vector<std::string> altPaths;
        for (const auto& d : importSearchDirs)
            altPaths.push_back(tryAlt(d));
        if (!runtimeDir.empty())
            altPaths.push_back(tryAlt(std::filesystem::path(runtimeDir) / "core"));
        for (auto& alt : altPaths)
        {
            if (!alt.empty() && alt != canonicalStr && importedFiles.count(alt))
            {
                if (verbose) std::cout << std::format("[verbose]   already imported (alt path): {}\n", canonicalStr);
                importedFiles.insert(canonicalStr);
                return true;
            }
        }
    }
    // Register eagerly so any re-entrant or duplicate import is caught above.
    importedFiles.insert(canonicalStr);
    // HeapAudit's C backing object needs a linked diagnostic library and cannot be loaded by
    // the --run JIT. Check only after import dedup so repeated imports do not repeat this work.
    if (runMode_ && !runtimeDir.empty())
    {
        if (heapAuditCanonicalPath_.empty())
        {
            std::error_code auditEc;
            auto auditPath = std::filesystem::canonical(
                std::filesystem::path(runtimeDir) / "core" / "diagnostic" / "heap_audit.cb", auditEc);
            if (!auditEc) heapAuditCanonicalPath_ = auditPath.string();
        }
        if (!heapAuditCanonicalPath_.empty() && canonicalStr == heapAuditCanonicalPath_)
            LogError("HeapAudit requires a linked C diagnostic object and cannot be used with --run.");
    }
    // Real C source: hand off to clang-cl rather than the CFlat parser. The compiled
    // object is linked by EmitExecutable; the importing .cb supplies the declarations.
    {
        auto ext = LowerExtension(canonicalStr);
        if (ext == ".c")
            return CompileCFile(canonicalStr, programAlias);
        // WinRT metadata: read the .winmd and register its interfaces/structs/enums as CFlat
        // types (consume side). Not parsed by the CFlat parser; no object is linked. An inline
        // `lib { "RuntimeObject.lib", "ole32.lib" }` clause names the import libs the projected
        // WinRT APIs need at link time - WinRT has no header to hang them on, so the .winmd
        // import is their home. Resolve each onto the link line, same as the .h branch below.
        if (ext == ".winmd")
        {
            // `as Alias` is meaningless here and used to be dropped in silence: a .winmd carries
            // many WinRT namespaces, so there is no single one to rename, and its types are already
            // registered under their full WinRT name. Point at the mechanism that does work.
            if (!namespaceName.empty())
            {
                LogError("import \"" + importFilename + "\" as " + namespaceName + ": a .winmd has no "
                    "single namespace to alias (its types are registered under their full WinRT "
                    "name). Use a type alias instead, e.g. `using IFoo = Windows.Foo.IFoo;`");
                return false;
            }
            for (const auto& explicitLib : explicitLibs)
            {
                if (explicitLib.empty()) continue;
                cLinkLibs_.push_back(ResolveCLinkLib(explicitLib, importingFilePath));
            }
            return CompileWinmdFile(canonicalStr);
        }
        // A C header (real C, not CFlat): extract declarations + enums via clang's
        // AST dump; the prebuilt library is linked via --c-lib in EmitExecutable.
        if (ext == ".h" || ext == ".hpp" || ext == ".hh")
        {
            // The objc runtime lives in libobjc, carried as a tbd by both the harvested stub
            // root and the real SDK.
            if (targetMacOS_ && importFilename.starts_with("objc/"))
                cLinkObjC_ = true;
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

    if (verbose) std::cout << std::format("[verbose] importing: {}\n", canonicalStr);
    llvm::TimeTraceScope importScope("ImportFile", importFilename);

    // Keep the source-origin classification for code-generation behavior.
    bool isCoreImport = false;
    if (!runtimeDir.empty())
    {
        std::error_code cec;
        auto coreDir = std::filesystem::canonical(std::filesystem::path(runtimeDir) / "core", cec);
        if (!cec)
            isCoreImport = IsPathUnderDirectory(canonicalStr, coreDir);
    }
    // Same library, different checkout: an analyzed core root imports its siblings from ITS
    // directory, not from runtimeDir/core, and they are core all the same.
    if (!isCoreImport && !rootCoreDir_.empty())
        isCoreImport = IsPathUnderDirectory(canonicalStr, rootCoreDir_);

    // All imported trees are mtime-and-size validated and retained for the process lifetime.
    // Generic-template ctx pointers therefore remain valid across re-analysis.
    CachedParseTree* tree = GetOrParseFile(canonicalStr, importFilename);
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
                SetSourceLocation(imp->getStart()->getLine(), imp->getStart()->getCharPositionInLine());
                if (auto error = ValidateImportLeadingMarker(imp); !error.empty()) {
                    LogError(error);
                    return false;
                }
                if (auto error = ValidateImportSoftKeywords(imp); !error.empty()) {
                    LogError(error);
                    return false;
                }
                if (isolatedPolicy_ && IsIsolatedNativeImport(imp))
                {
                    LogError(std::format("policy-restricted-language: native interop imports are not "
                                         "allowed in isolated mode (policy '{}')", isolatedPolicy_->path));
                    return false;
                }
                // Grouped import `import { "a", "b" };` - one TU for header entries, individual
                // routing for .cb/.c entries (see CompileImportGroup).
                auto groupedImports = DequoteImportGroup(imp);
                // `import framework` dispatch first - it reuses importGroup for its names.
                if (IsFrameworkImport(imp))
                {
                    for (const auto& fw : groupedImports)
                        if (!AddFrameworkImport(fw)) return false;
                    continue;
                }
                // A `framework "X"` clause on a header/package/group import (S3): link the
                // framework in addition to binding the header. Standalone form handled above.
                for (const auto& fw : DequoteFrameworkClauses(imp))
                    if (!AddFrameworkImport(fw)) return false;
                // package-nuget dispatch first (it now carries an importGroup too); a multi-entry
                // nuget group is one package TU, not several plain imports.
                if (IsPackageNugetImport(imp))
                {
                    std::string packageSpec = DequoteFromClause(imp);
                    if (verbose) std::cout << std::format("[verbose]   nested nuget import (group of {}) from {}\n", groupedImports.size(), packageSpec);
                    if (!CompileNugetImport(groupedImports, packageSpec, DequoteDefineClauses(imp), DequotePriClause(imp)))
                        return false;
                    continue;
                }
                if (groupedImports.size() > 1)
                {
                    if (verbose) std::cout << std::format("[verbose]   nested import (group of {})\n", groupedImports.size());
                    if (!CompileImportGroup(canonicalStr, groupedImports, DequoteLibClauses(imp),
                                            DequoteDefineClauses(imp), HasCacheClause(imp)))
                        return false;
                    continue;
                }
                std::string nested;
                if (!groupedImports.empty())
                    nested = groupedImports[0];
                else
                {
                    std::string raw = imp->StringLiteral()->getText();
                    nested = DequoteStringLiteral(raw);
                }
                if (IsPackageVcpkgImport(imp))
                {
                    std::string portSpec = DequoteFromClause(imp);
                    if (verbose) std::cout << std::format("[verbose]   nested vcpkg import: {} from {}\n", nested, portSpec);
                    if (!CompileVcpkgImport(canonicalStr, nested, portSpec, DequoteDefineClauses(imp)))
                        return false;
                    continue;
                }
                std::string nestedNs = ImportNamespace(imp);
                std::vector<std::string> nestedLibs = DequoteLibClauses(imp);
                std::vector<std::string> nestedDefines = DequoteDefineClauses(imp);
                if (verbose) std::cout << std::format("[verbose]   nested import: {}{}\n", nested, nestedNs.empty() ? "" : " as " + nestedNs);
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
    auto savedSourceIsCore = currentSourceIsCore_;
    sourceFileName = std::filesystem::path(canonicalStr).filename().string();
    currentSourceFilePath_ = canonicalStr;
    currentSourceIsCore_ = isCoreImport;
    ValidateIsolatedParseTree(computeUnit);
    // Files that follow this import are not scanned yet, so no interface-implementor claim made
    // while codegen'ing it can be proven complete (see InterfaceImplementorSetIsUncertain).
    ImportedFileCompileScope importDepthScope(this);

    // Forward-ref scan the imported file
    {
        llvm::TimeTraceScope scanScope("ForwardRefScan", importFilename);
        if (verbose) std::cout << std::format("[verbose]   forward-ref scan: {}\n", importFilename);
        ForwardRefScanner scanner(this);
        scanner.SetTokens(tokensPtr);
        scanner.ValidateIsolatedIntegralPointerCasts(computeUnit);
        // Pre-declare opaque types for all generic instantiations found in the file
        // (including inside function/program bodies) before scanning declarations.
        // Without this pass, program definitions that pre-declare run(Name*, list$string)
        // fail because the opaque shell for list$string hasn't been created yet.
        // Generic interface templates first (see the main-file scan for why).
        if (auto* tu = computeUnit->translationUnit())
        {
            // Pure-rename `using` aliases first (see the main-file scan for why).
            scanner.PreRegisterRenameAliases(tu);
            scanner.SeedConstIntGlobals(tu);
            scanner.ScanGenericInterfaceTemplateNames(tu);
            for (auto* decl : tu->externalDeclaration())
                scanner.ScanGenericTypeUses(decl);
            for (auto* decl : tu->externalDeclaration())
                scanner.ScanExternalDeclaration(decl);
            scanner.PrepareWinrtClasses(tu);
        }
    }

    // Code-gen walk the imported file
    if (verbose) std::cout << std::format("[verbose]   code-gen walk: {}\n", importFilename);
    {
        llvm::TimeTraceScope codegenScope("CodeGeneration", importFilename);
        auto myListener = std::make_unique<MainListener>(parserPtr, this, sourceFileName);
        antlr4::tree::ParseTreeWalker().walk(myListener.get(), computeUnit);
        {
            NoCurrentFunctionScope noCurrent(this);
            myListener->ResolvePendingGlobalDefaultConstructions();
        }
    }
    if (isCoreImport)
        for (auto& function : module->functions())
            if (!function.isDeclaration()) isolatedCoreFunctions_.insert(function.getName().str());
    if (verbose) std::cout << std::format("[verbose]   import done: {}\n", importFilename);

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
    currentSourceIsCore_ = savedSourceIsCore;
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
        auto localizeMessage = MakeDiagnosticLocalizer();
        state.parser->setErrorHandler(std::make_shared<CFlatErrorStrategy>(localizeMessage));

        try { state.tokens->fill(); } catch (...) {
            if (verbose) std::cout << std::format("[verbose]   lex failed for {}, dropping batch\n", state.label);
            continue;
        }
        CFlatParser::CompilationUnitContext* cu = nullptr;
        try { cu = state.parser->compilationUnit(); } catch (...) {
            if (verbose) std::cout << std::format("[verbose]   parse failed for {}, dropping batch\n", state.label);
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
            std::vector<std::string> valueParams;
            for (auto* entry : gtps->typeParameterList()->typeParameterEntry())
            {
                if (IsValueParameterDeclaration(entry))
                {
                    // A non-integral value parameter is rejected by the listener and dropped there;
                    // drop it here too or these vectors disagree with the listener's.
                    if (ValueParameterTypeSpelling(entry).empty()) continue;
                    typeParams.push_back(entry->valueParameterDeclaration()->Identifier()->getText());
                    valueParams.push_back(ValueParameterTypeSpelling(entry));
                }
                else
                {
                    typeParams.push_back(entry->getText());
                    valueParams.push_back("");
                }
            }
            gts.genericFunctionTemplates[name] = func;
            gts.genericFunctionTypeParams[name] = typeParams;
            gts.genericFunctionValueParams[name] = valueParams;
            ++registered;
        }
        if (verbose)
            std::cout << std::format("[verbose]   registered {} C function-like macro template(s) from {}\n", registered, state.label);
        // Keep parse state alive for the rest of the compilation.
        syntheticParseStates_.push_back(std::move(state));
    }
}

// Build fresh analysis managers, wire the standard proxies, and run the given
// module pass manager. Shared by RunBaselinePasses and RunGlobalDCE.
void LLVMBackend::RunModulePasses(llvm::ModulePassManager& MPM)
{
    llvm::PassBuilder PB;
    llvm::LoopAnalysisManager LAM;
    llvm::FunctionAnalysisManager FAM;
    llvm::CGSCCAnalysisManager CGAM;
    llvm::ModuleAnalysisManager MAM;

    // Same stdio-safe TLI as OptimizeModule: the -O0 baseline pipeline also runs
    // instcombine, whose fortify-folding would otherwise rewrite our __vsnprintf_chk /
    // __vfprintf_chk libc calls back into cflat's own vsnprintf/vfprintf and recurse
    // forever on ELF. Register before registerFunctionAnalyses so it wins.
    llvm::TargetLibraryInfoImpl TLII = MakeStdioSafeTLII(
        llvm::Triple(module->getTargetTriple().str()));
    FAM.registerPass([&] { return llvm::TargetLibraryAnalysis(TLII); });

    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

    MPM.run(*module, MAM);
}

void LLVMBackend::RunBaselinePasses()
{
    MaterializeCoreIfLazy();
    llvm::FunctionPassManager FPM;
    FPM.addPass(llvm::SROAPass(llvm::SROAOptions::ModifyCFG));
    FPM.addPass(llvm::PromotePass());
    FPM.addPass(llvm::InstCombinePass());
    FPM.addPass(llvm::SimplifyCFGPass());

    llvm::ModulePassManager MPM;
    MPM.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(FPM)));
    RunModulePasses(MPM);
}

// At -O0 no module-level reachability pass runs, so every internal-linkage function
// (CreateFunctionDefinition internalizes all non-extern, non-main functions) survives
// into codegen - including the whole core library the program never calls. GlobalDCE
// deletes those unreachable internals; main and the extern/auto-extern surface stay
// external, so they remain roots and the linker entry + C-interop ABI are preserved.
// O1+ already get this via buildPerModuleDefaultPipeline, so this is the -O0-only path.
void LLVMBackend::RunGlobalDCE()
{
    MaterializeCoreIfLazy();
    llvm::ModulePassManager MPM;
    MPM.addPass(llvm::GlobalDCEPass());
    RunModulePasses(MPM);
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

        // Init() never sets a module triple (only the data layout); on macOS the
        // real Mach-O triple is set later in EmitExecutableMachO, after this pass
        // runs. An empty triple defaults to ELF, so the pass stamps a COMDAT on
        // asan.module_ctor - Mach-O has no COMDATs and object emission aborts.
        // Give it the same versioned triple EmitExecutableMachO uses so it takes
        // the Mach-O branch instead. Windows is untouched (relies on the empty
        // triple / ELF-shaped defaults it already worked with).
        if (targetMacOS_)
        {
            module->setTargetTriple(llvm::Triple("arm64-apple-macosx11.0.0"));

            // Apple's asan runtime self-verifies interception at startup by
            // dlsym-ing "puts" and checking it resolves into the asan dylib.
            // cflat reimplements the C stdio family (cruntime.cb defines its own
            // 'puts'), so that symbol legitimately resolves to cflat's code, not
            // asan's interposed libc puts - a false positive that aborts every
            // asan binary before main runs ("Interceptors are not working").
            // Malloc/free/operator-new/delete interception is unaffected; only
            // this one self-check is a false alarm. Bake in a default
            // ASAN_OPTIONS to skip it via the documented __asan_default_options
            // override hook (a strong definition here wins over the runtime's
            // own weak default).
            llvm::IRBuilder<> optsBuilder(*context);
            llvm::Constant* optsStr = optsBuilder.CreateGlobalString(
                "verify_interceptors=0", "asan_default_options_str", 0, module.get());
            llvm::FunctionType* optsFnTy =
                llvm::FunctionType::get(llvm::PointerType::get(*context, 0), false);
            llvm::Function* optsFn = llvm::Function::Create(
                optsFnTy, llvm::GlobalValue::ExternalLinkage, "__asan_default_options", module.get());
            llvm::BasicBlock* optsBB = llvm::BasicBlock::Create(*context, "entry", optsFn);
            optsBuilder.SetInsertPoint(optsBB);
            optsBuilder.CreateRet(optsStr);
        }

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

        // Apple's clang-21 asan runtime only exports the Apple-specific
        // ___asan_version_mismatch_check_apple_clang_2100, not LLVM 18's
        // ___asan_version_mismatch_check_v8; skip the check on macOS to avoid an
        // undefined-symbol link error (the rest of the v8 ABI IS exported).
        if (targetMacOS_)
            asanOpts.InsertVersionCheck = false;

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

    // cflat reimplements the C stdio family (printf/vsnprintf/... in cruntime.cb), so
    // LLVM must NOT treat their libc names as available. Otherwise instcombine's
    // fortify-folding rewrites our __vsnprintf_chk / __vfprintf_chk libc calls back
    // into plain vsnprintf / vfprintf, which on ELF bind to cflat's OWN wrappers and
    // recurse forever. Marking the colliding names unavailable keeps the chk calls
    // intact so they reach libc. Register this TLI before registerFunctionAnalyses so
    // it wins over the default (registerPass is a no-op once an analysis exists).
    llvm::TargetLibraryInfoImpl TLII = MakeStdioSafeTLII(
        llvm::Triple(module->getTargetTriple().str()));
    FAM.registerPass([&] { return llvm::TargetLibraryAnalysis(TLII); });

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
                auto* term = cflat_llvm::GetTerminatorOrNull(&BB);
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

// True when the root file being compiled/analyzed shares its name with a core library
// file, whose definitions the core cache already holds. Matching by BASENAME mirrors the
// cross-path import dedup: a repo-tree core/*.cb and its deployed runtimeDir/core copy
// are treated as the same file there, so the cache guard must agree.
bool LLVMBackend::RootFileNameIsCore(const std::string& fileName) const
{
    if (runtimeDir.empty() || fileName.empty()) return false;
    if (coreFileNames_.empty())
    {
        std::error_code ec;
        std::filesystem::recursive_directory_iterator it(
            std::filesystem::path(runtimeDir) / "core", ec), end;
        for (; !ec && it != end; it.increment(ec))
        {
            std::error_code fec;
            if (it->is_regular_file(fec) && !fec)
                coreFileNames_.insert(it->path().filename().string());
        }
    }
    return coreFileNames_.count(fileName) != 0;
}

bool LLVMBackend::Analyze(const std::string& filePath,
                              const std::vector<std::string>& importDirs,
                              const std::string& runtimeDirPath)
{
    gts.Clear();

    // Diagnostics should name the real document, not the temp copy the LSP hands us.
    // When the caller supplied a display name, use it; otherwise fall back to the
    // analyzed file's own name (e.g. direct CLI --check on a real path).
    sourceFileName = sourceDisplayName_.empty()
        ? std::filesystem::path(filePath).filename().string()
        : sourceDisplayName_;
    llvm::TimeTraceScope analyzeScope("Analyze", sourceFileName);
    auto rootCanonical = std::filesystem::weakly_canonical(filePath).string();
    currentSourceFilePath_ = rootCanonical;
    analyzedRootPath_ = rootCanonical;
    currentSourceIsCore_ = false;   // the analyzed root file is treated as user code
    importedFiles.insert(rootCanonical);
    importStack.push_back(rootCanonical);
    struct RootGuard {
        std::vector<std::string>& stack;
        ~RootGuard() { stack.pop_back(); }
    } rootGuard{importStack};
    importSearchDirs = importDirs;
    runtimeDir = runtimeDirPath;
    // verbose retains the value set by the LSP via SetVerbose() - left unmodified.
    bool debugInfo = analyzeDebugInfo_;

    platformValue = 64;
#if defined(_WIN32)
    const char* platformOption = "win64";
    targetWindows_ = true;
    targetMacOS_ = false;
    targetArm64_ = false;
#elif defined(__APPLE__)
    const char* platformOption = "macos";
    targetWindows_ = false;
    targetMacOS_ = true;
    targetArm64_ = true;
#else
    const char* platformOption = "linux";
    targetWindows_ = false;
    targetMacOS_ = false;
    targetArm64_ = false;
#endif
    SetTargetLongWidth(targetWindows_, platformValue);

    // A core file analyzed as the ROOT document (LSP open / --check on core/*.cb) must
    // not load the cache: the cache already contains this file's own definitions, and
    // re-walking the root over them reports spurious redeclarations.
    bool rootIsCoreFile = RootFileNameIsCore(sourceFileName);
    // The LSP analyzes a TEMP copy, so the root's real directory is sourceFileDir_ - the temp
    // dir is never the library checkout and would make rootCoreDir_ useless.
    rootCoreDir_.clear();
    if (rootIsCoreFile)
    {
        std::filesystem::path rootDir = sourceFileDir_.empty()
            ? std::filesystem::path(rootCanonical).parent_path()
            : std::filesystem::path(sourceFileDir_);
        rootCoreDir_ = std::filesystem::weakly_canonical(rootDir).string();
    }

    bool bitcodeLoaded = false;
    if (!noCache_ && !runtimeDir.empty() && !skipRuntimeImport && !rootIsCoreFile)
    {
        std::string bcCacheDir = GetRuntimeBitcodeDir(runtimeDir);
        if (!bcCacheDir.empty())
            bitcodeLoaded = LoadCoreBitcodeIfFresh(bcCacheDir, platformOption);
    }

    if (!bitcodeLoaded)
    {
        const char* dl = targetMacOS_
            ? "e-m:o-i64:64-i128:128-n32:64-S128"
            : PlatformDataLayout(platformValue);
        module->setDataLayout(llvm::DataLayout(dl));
    }

    if (debugInfo)
    {
        std::filesystem::path filePathAbsolute = std::filesystem::absolute(filePath);
        InitDebugInfo(filePathAbsolute.filename().string(), filePathAbsolute.parent_path().string());
    }

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
        SetPlatformMacros();
    }

    if (bitcodeLoaded && symbolSink_ != nullptr)
        symbolSink_->MergeFrom(coreSymbolIndex_);

    if (!bitcodeLoaded && !runtimeDir.empty())
    if (!skipRuntimeImport)
    {
        auto runtimePath = std::filesystem::path(runtimeDir) / "core" / "runtime.cb";
        if (std::filesystem::exists(runtimePath))
            CompileImportedFile(runtimePath.string(), "runtime.cb");
    }

    if (!std::filesystem::exists(filePath))
    {
        std::cout << std::format("Error: input file '{}' does not exist.\n", filePath);
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
        auto localizeMessage = MakeDiagnosticLocalizer();
        CFlatErrorListener analyzeErrorListener(sourceFileName, analyzeSourceLines,
                                                localizeMessage);
        lexer.removeErrorListeners();
        lexer.addErrorListener(&analyzeErrorListener);
        parser.removeErrorListeners();
        parser.addErrorListener(&analyzeErrorListener);
        parser.setErrorHandler(std::make_shared<CFlatErrorStrategy>(localizeMessage));

        CFlatParser::CompilationUnitContext* computeUnit;
        {
            llvm::TimeTraceScope parseScope("Parse", sourceFileName);
            {
                llvm::TimeTraceScope lexScope("Lex", sourceFileName);
                tokens.fill();
            }
            computeUnit = parser.compilationUnit();
        }

        if (analyzeErrorListener.hasErrors())
        {
            ReportParseErrors(analyzeErrorListener.getDiagnostics(), analyzeSourceLines);
            return false;
        }

        // Process top-level imports before scanning
        if (auto* tu = computeUnit->translationUnit()) {
            llvm::TimeTraceScope importsScope("ProcessImports", sourceFileName);
            for (auto* decl : tu->externalDeclaration()) {
                if (auto* imp = decl->importDeclaration()) {
                    // Point diagnostics at this import statement (mirrors Compile's ProcessImports):
                    // a not-found error would otherwise report a stale location from a prior import.
                    SetSourceLocation(imp->getStart()->getLine(), imp->getStart()->getCharPositionInLine());
                    if (auto error = ValidateImportLeadingMarker(imp); !error.empty()) {
                        LogError(error);
                        return false;
                    }
                    if (auto error = ValidateImportSoftKeywords(imp); !error.empty()) {
                        LogError(error);
                        return false;
                    }
                    // Grouped import `import { "a", "b" };` - one TU for header entries (see
                    // CompileImportGroup); .cb/.c entries route individually.
                    auto groupedImports = DequoteImportGroup(imp);
                    // `import framework` dispatch first - reuses importGroup for its names.
                    // In LSP analyze mode AddFrameworkImport records silently (never errors).
                    if (IsFrameworkImport(imp))
                    {
                        for (const auto& fw : groupedImports)
                            if (!AddFrameworkImport(fw)) return false;
                        continue;
                    }
                    // A `framework "X"` clause on a header/package/group import (S3): link the
                    // framework in addition to binding the header. Standalone form handled above.
                    for (const auto& fw : DequoteFrameworkClauses(imp))
                        if (!AddFrameworkImport(fw)) return false;
                    // package-nuget dispatch first (it now carries an importGroup too); a
                    // multi-entry nuget group is one package TU, not several plain imports.
                    if (IsPackageNugetImport(imp))
                    {
                        std::string packageSpec = DequoteFromClause(imp);
                        if (!CompileNugetImport(groupedImports, packageSpec, DequoteDefineClauses(imp), DequotePriClause(imp)))
                            return false;
                        continue;
                    }
                    if (groupedImports.size() > 1)
                    {
                        if (!CompileImportGroup(filePath, groupedImports, DequoteLibClauses(imp),
                                                DequoteDefineClauses(imp), HasCacheClause(imp)))
                            return false;
                        continue;
                    }
                    std::string importFilename;
                    if (!groupedImports.empty())
                        importFilename = groupedImports[0];
                    else
                    {
                        std::string raw = imp->StringLiteral()->getText();
                        importFilename = DequoteStringLiteral(raw);
                    }
                    if (IsPackageVcpkgImport(imp))
                    {
                        std::string portSpec = DequoteFromClause(imp);
                        if (!CompileVcpkgImport(RootVcpkgImportPath(filePath), importFilename, portSpec, DequoteDefineClauses(imp)))
                            return false;
                        continue;
                    }
                    std::string ns = ImportNamespace(imp);
                    bool isProgram = imp->children.size() >= 2 && imp->children[1]->getText() == "program";
                    std::string alias = isProgram ? ImportProgramAlias(imp) : "";
                    std::string impExt = LowerExtension(importFilename);
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
            llvm::TimeTraceScope scanScope("ForwardRefScan", sourceFileName);
            ForwardRefScanner scanner(this);
            scanner.SetTokens(&tokens);
            scanner.ValidateIsolatedIntegralPointerCasts(computeUnit);
            // Generic interface templates first: a use like Container<int> must not be
            // pre-declared as an opaque struct shell (it is a fat-pointer interface).
            if (auto* tu = computeUnit->translationUnit())
            {
                // Pure-rename `using` aliases first: MangleTypeArg folds them, and both passes
                // must see the same set no matter where the `using` sits in the file.
                scanner.PreRegisterRenameAliases(tu);
                scanner.SeedConstIntGlobals(tu);
                scanner.ScanGenericInterfaceTemplateNames(tu);
                for (auto* decl : tu->externalDeclaration())
                    scanner.ScanGenericTypeUses(decl);
                for (auto* decl : tu->externalDeclaration())
                    scanner.ScanExternalDeclaration(decl);
                scanner.PrepareWinrtClasses(tu);
            }
        }

        // Code-gen walk
        {
            llvm::TimeTraceScope codegenScope("CodeGeneration", sourceFileName);
            auto myListener = std::make_unique<MainListener>(&parser, this, sourceFileName);
            auto walker = antlr4::tree::ParseTreeWalker();
            walker.walk(myListener.get(), computeUnit);
            {
                NoCurrentFunctionScope noCurrent(this);
                myListener->ResolvePendingGlobalDefaultConstructions();
            }
            // Now that every implementor is registered, emit the interface rebox if-chains.
            // Resolve-created calls can register deferred interface work, so resolve first.
            EmitDeferredInterfaceReboxBodies();
            // Now that every type and generic monomorphization is registered, fill in the
            // bodies of deferred delete-site destructor wrappers (recursive containers whose
            // element type was incomplete when the container dtor was emitted).
            EmitDeferredFullDestructorBodies();
        }
        ResolveMaterializedInterfaceUses();
        stream.close();
    }
    catch (CompilerAbortException&) { return false; }
    catch (ExpectedErrorReceived&)  { return false; }

    if (debugInfo)
        FinalizeDebugInfo();

    return true;
}

bool LLVMBackend::LastOptimizedViewWasIncremental() const
{
    return optimizedViewWasIncremental_;
}

void LLVMBackend::ResetForReanalysis()
{
    analyzeDebugInfo_ = false;
    isolatedPolicy_.reset();
    // Core hashes are per-analysis so LSP notices edits; batch mode keeps one process-wide hash.
    if (!batchMode_) coreHashCache_.clear();
    dependencyPathMemo_.clear();
    dependencyFileSet_.clear();
    manifestFragments_.clear();
    compileTimeStringConstants_.clear();
    // Reset the LLVM context so named struct types from a previous analysis do
    // not survive into the next one.  Without this, two fixtures that define a
    // struct with the same name but different field types (e.g. Point{int,int}
    // then Point{float,float}) would share the stale type object, causing an
    // LLVM assertion when inserting a float constant into an int-typed slot.
    //
    // Destroy module and builder BEFORE the context: both hold internal
    // references to the context, and their destructors must not run against
    // an already-freed context object.
    // Drop any move-dataflow events keyed by the old module's Functions before the module
    // (and those Functions) are destroyed - stale keys must never survive the reset.
    moveEventLog_.clear();
    // Origin slots hold llvm::Value* into the module being discarded - drop them too.
    ownOriginSlots_.clear();
    if (optimizedViewCache_.module && optimizedViewCache_.preOptMetadataRecorded)
    {
        llvm::TimeTraceScope snapshotScope("ViewSnapshotSerialize");
        std::string bitcode;
        llvm::raw_string_ostream bitcodeOut(bitcode);
        llvm::WriteBitcodeToFile(*optimizedViewCache_.module, bitcodeOut);
        bitcodeOut.flush();

        IncrementalViewSnapshot snapshot;
        snapshot.optLevel = optimizedViewCache_.optLevel;
        snapshot.rootFile = sourceDisplayName_.empty() ? analyzedRootPath_ : sourceDisplayName_;
        snapshot.bitcode = std::move(bitcode);
        snapshot.funcHashes = std::move(optimizedViewCache_.funcHashes);
        snapshot.globalHashes = std::move(optimizedViewCache_.globalHashes);
        snapshot.callees = std::move(optimizedViewCache_.callees);
        snapshot.addressTaken = std::move(optimizedViewCache_.addressTaken);
        snapshot.rootPathAliases = std::move(optimizedViewCache_.rootPathAliases);
        snapshot.frameRemarks = std::move(optimizedViewCache_.frameRemarks);
        snapshot.remarks = std::move(optimizedViewCache_.remarks);
        snapshot.remarksTruncated = optimizedViewCache_.remarksTruncated;
        incrementalViewSnapshot_ = std::move(snapshot);
    }
    optimizedViewCache_ = OptimizedViewCache{};
    optimizedViewWasIncremental_ = false;
    // Same: both borrow-provenance ledgers are keyed by Functions/CallInsts of the old module.
    uniqueFieldBorrowReturns_.clear();
    uniqueFieldBorrowResults_.clear();
    globalAssignBorrowOrigin_.clear();
    rawArrayResults_.clear();
    mayReachReturnInProgress_.clear();
    joinAddressInProgress_.clear();
    pendingReturnDangleChecks_.clear();
    pendingNullIfaceDispatch_.clear();
    pendingFunctionDeclarations_.clear();
    flushingPendingDeclarations_ = false;
    pendingNullIfaceGlobal_.clear();
    DropModuleEscapeMemo();
    cachedFunctionNames_.reset();

    module.reset();
    coreBitcodeBuffer_.reset();
    builder.reset();
    context = std::make_unique<llvm::LLVMContext>();
    module = std::make_unique<llvm::Module>("cflat", *context);
    builder = std::make_unique<llvm::IRBuilder<>>(*context);

    // Debug-info state is bound to the (now-discarded) module: the DIBuilder, its DIFile/
    // DICompileUnit nodes, and the DIType/DIFile caches all point into the old module's
    // metadata. Clearing them keeps the "nothing module-bound survives the reset" invariant
    // intact for both plain and debug-info analyses
    // (and would prevent the same dangling-pointer crash class if a reanalysis ever ran -g).
    diBuilder.reset();
    diFile = nullptr;
    compileUnit = nullptr;
    currentSubprogram = nullptr;
    diTypeCache.clear();
    diFileCache_.clear();
    pendingGlobalDI_.clear();

    // Array-view noalias metadata is module-bound: aliasDomain_ / aliasScopes_ hold llvm::MDNode*
    // and viewScopeByOrigin_ indexes into that vector, all created against the now-discarded module.
    // They are normally rebuilt per function in createFunctionBlock, but until the next function is
    // emitted they would be dangling pointers into the freed module - a violation of the
    // "nothing module-bound survives the reset" invariant (same crash class as the stale
    // fullDestructorCache_ Function* below). Drop them here so nothing module-bound persists.
    aliasDomain_ = nullptr;
    aliasScopes_.clear();
    viewScopeByOrigin_.clear();
    // returnedStructDtorSkipAlloca points at a returned local's alloca in the discarded module; it
    // is normally saved/restored around the return-path destructor walk, but a compile aborted by an
    // exception between set and restore would leave a dangling alloca pointer that the next file's
    // EmitDestructorsForScope compares against (and could wrongly skip a destructor).
    returnedStructDtorSkipAlloca = nullptr;

    functionTable.clear();
    annotationRegistry.clear();
    synthesizedReflectFunctions.clear();
    pendingCInteropAliases_.clear();
    cAbiFunctionThunkCache_.clear();
    dataStructures.clear();
    // RegisterEncodedClosureType memoizes on this map but writes the encoded closure's backing
    // entries into dataStructures/functionTable, both just cleared. A survivor makes the next
    // file's registration early-return, leaving `Lambda<int(int)>` resolvable as an encoded name
    // with no type behind it - it then reports as `unknown type 'fatfn$...'`.
    encodedClosureTypes_.clear();
    // Body origins describe definitions in the module just discarded; a survivor would make the
    // next analysis of the same file report its own definition as a redefinition of itself.
    functionBodyOrigin_.clear();
    isolatedCoreFunctions_.clear();
    // Memoized full-destructor wrappers are synthesized llvm::Function* objects that live
    // in `module`, so they MUST be discarded with it - a stale entry would hand the next
    // file's analysis a Function* into the freed module/context, which crashes the moment a
    // dtor call is emitted to it (this was the non-deterministic LSP bulk-sweep "Internal
    // compiler error during analysis"). fullDestructorInProgress_ is the recursion guard;
    // an analysis aborted mid-recursion could leave a name in it and make GetOrCreateFullDestructor
    // wrongly short-circuit on the next file.
    fullDestructorCache_.clear();
    fullDestructorInProgress_.clear();
    // Synthesized memberwise copy() functions are also module-bound Function* objects (same
    // stale-pointer class as fullDestructorCache_). Beyond the dangling pointer, a surviving cache
    // entry makes GetOrCreateMemberwiseCopy return early WITHOUT re-registering the synth in the
    // just-cleared functionTable, so a later `x.copy()` (e.g. list<OwningStruct>.copy() calling the
    // element's copy()) fails to resolve on the second file of an LSP bulk sweep. Discard with the module.
    memberwiseCopyCache_.clear();
    // Deferred delete-site dtor wrappers are also module-bound Function* objects (same
    // stale-pointer crash class as fullDestructorCache_), so discard them with the module.
    deferredFullDtor_.clear();
    deferredFullDtorOrder_.clear();
    // Same for the deferred interface rebox thunks: Function* into the discarded module.
    deferredIfaceRebox_.clear();
    deferredIfaceReboxIndex_.clear();
    deferredIfaceReboxDrained_ = false;
    // [winrt] classes and projected delegates cache module-bound objects: WinrtClassInfo holds
    // the static vtable GlobalVariable* (and its StructType*), and the delegate maps hold the
    // COM object StructType* + vtable GlobalVariable*. If these survive the reset, a file that
    // forward-references a [winrt] class (e.g. `new Counter` in a method emitted before the
    // Counter class's EmitWinrtRuntime runs) reads the prior analysis's dangling vtable global
    // and crashes bitcasting a freed Value - the non-deterministic LSP bulk-sweep segfault.
    winrtClasses.clear();
    winrtDelegateObjTy_.clear();
    winrtDelegateVtbl_.clear();
    winrtSlotHResultType_.clear();
    // Consume-side (imported .winmd) bookkeeping is re-derived on the next analysis's imports;
    // clear it here so it does not accumulate stale entries across files (importedFiles below).
    winrtEnumUnderlying_.clear();
    winrtValueStructs_.clear();
    winrtGenericTemplates_.clear();
    winrtInstanceIid_.clear();
    winrtThinInterfaces_.clear();
    winrtConsumedModel_ = cflat_winmd::Model{};
    winrtConsumedLspFile_.clear();
    programTable.clear();
    enumBackingTypes.clear();
    typeAliases.clear();
    aliasScopeStack_.clear();
    aggregateAliasScopes_.clear();
    manglingAliases_.clear();
    genericBaseAliases_.clear();
    functionTypeAliases.clear();
    interfaceTable.clear();
    interfaceFields.clear();
    interfaceParents.clear();
    interfaceDefSites.clear();
    rejectedInterfaceDefSites_.clear();
    rejectedInterfaceDefMessages_.clear();
    coreInterfaceDefs_.clear();
    scannedInterfaceImpls.clear();
    uncertainInterfaceImpls.clear();
    ifConstGuardedImpls_.clear();
    analyzedRootPath_.clear();
    importCompileDepth_ = 0;   // a throw out of an import walk can leave the RAII depth stranded
    typeAnnotations_.clear();
    globalNamedVariable.clear();
    globalVariableTypes.clear();
    globalDeclSite.clear();
    namespaceTable.clear();
    stringPool.clear();
    stackNamedVariable.clear();
    nextDeclSequence = 1;
    namespaceAliasTable.clear();
    returnBlockTable.clear();
    compileTimeMacros.clear();
    constGlobalInts_.clear();
    stringLiteralLenByPtr.clear();
    strConcatRegistered = false;
    stringDtorRegistered = false;
    pendingOwnedClosureTemps.clear();
    lambdaCounter = 0;
    expectedError.clear();
    expectedErrorScopeDepth = SIZE_MAX;
    fileScopeExpectedError_.clear();
    currentFunction = nullptr;
    autoVaListAlloca = nullptr;
    ClearStaticLocalRequest();
    autoReturnCapture = std::nullopt;

    // Transient per-call / per-expression result state. These are normally produced and
    // consumed within a single statement, but an aborted compile (e.g. an expect_error that
    // fires mid-assignment, before the bond flag is consumed) can leave them set. Clearing
    // them here prevents stale state from leaking into the next file's analysis - notably a
    // stale lastCallIsBonded would raise a spurious bond error on the next assignment.
    // A LogError thrown from inside a namespace body unwinds past the NamespaceScope restores,
    // so the namespace can still be set here. It is NOT cosmetic: it steers generic-template key
    // resolution, and a stale value misbinds templates in the NEXT file of a batched --check.
    currentNamespace_.clear();
    lastCallReturnType = TypeAndValue{};
    lastCallReturnsOwned = false;
    lastOwningResult = false;
    lastMovedFromContainerSlot = false;
    lastAllocAlignment = 0;
    currentFunctionReturnsOwned = false;
    // Siblings of currentFunctionReturnsOwned, set together in createFunctionBlock; an aborted
    // compile mid-function would leave them describing the prior file's last function.
    currentFunctionReturnIsArrayView = false;
    currentFunctionReturnTypeName.clear();
    currentFunctionReturnTV = TypeAndValue{};
    currentFunctionAbiRecipe = AbiRecipe{};
    pendingOwnedStringTemps.clear();
    pendingOwnedStructTemps.clear();
    pendingOwnedPtrTemps.clear();
    ownedReturnTemps_.clear();
    ownedReturnReleaseTemps_.clear();
    ownedNewTemps_.clear();
    nullConditionalTempResults_.clear();
    valueElementTypeNames_.clear();
    fatInterfaceValueTypeNames_.clear();
    interfaceBoxRecords_.clear();
    nullCoalesceJoins_.clear();
    joinArmOccurrences_.clear();
    codeValues_.clear();
    dataValues_.clear();
    codeValueDataCasts_.clear();
    currentCastOccurrence_ = 0;
    nextCastOccurrence_ = 1;
    owningTempUniqueFields_.clear();
    launderedTempUniqueFields_.clear();
    pendingLaunderTempUniqueFields_.clear();
    dataValueCodeCasts_.clear();
    movedOutPtrValues_.clear();
    movedBorrowedPtrValues_.clear();
    movedBorrowedThroughFieldValues_.clear();
    nonOwningStructJoins_.clear();
    uniqueFieldReadValues_.clear();
    uniqueFieldReadJoins_.clear();
    aliasValues_.clear();
    tempFieldValues_.clear();
    bondedValues_.clear();
    // Keyed by llvm::Function*, which a rebuilt module invalidates.
    DropModuleEscapeMemo();
    poisonedFunctions.clear();
    firstCallLocation_.clear();
    // Holds llvm::Function* into a module a rebuild invalidates.
    tempUniqueFieldArgs_.clear();
    coreUniqueGetterSource_.clear();
    deferredTempUniqueFieldEscapes_.clear();
    lastAllocAlignment = 0;
    pendingInitAllocAlign = 0;
    lastCallReturnsAllocAlign = 0;
    lastCallIsBonded = false;
    // Left non-zero by an aborted compile mid-ternary, this would wrongly suppress the deref
    // guard for the next file's straight-line code.
    suppressExplicitNullDerefGuard_ = 0;
    // Same hazard for the cross-block half: a stale event log from an aborted compile would
    // misattribute the next file's deref diagnostic.
    nullEventLog_.clear();
    // Same hazard as nullEventLog_: a pending check from an aborted compile keys off a slot
    // in the module being discarded and must not survive into the next file's analysis.
    pendingReturnDangleChecks_.clear();
    // Same hazard: a pending dispatch record keys off a slot and an anchor instruction in the
    // module being discarded, so it must not survive into the next file's analysis.
    pendingNullIfaceDispatch_.clear();
    // Same hazard for the global-receiver ledger, which normally survives to module end.
    pendingNullIfaceGlobal_.clear();
    provenNoReturn_.clear();
    // Sibling of lastCallIsBonded: a per-call bond flag that, if left set by an aborted compile,
    // would mark the next file's first bonded value as by-address.
    lastCallBondByAddress = false;
    lastCallBondedSources.clear();
    lastCallLambdaCaptureNames.clear();
    lastLambdaReferenceCaptureNames.clear();
    lastCallRequiredLocks.clear();
    lastCallParameterNames.clear();
    pendingGlobalGuardedBy.clear();
    vectorizeLoops_.clear();
    // Cross-thread sharing scan: per-file escaped-type set and report dedupe set.
    // The configured scan LEVEL is intentionally preserved across files in a batch.
    threadSharedTypes_.clear();  // re-derived per top-level file by ScanCrossThreadEscapes
    xthreadReported_.clear();

    // Clear per-analysis import state. Imported parse trees deliberately survive this reset;
    // their path, mtime, and size validation keeps them safe and fresh.
    importedFiles.clear();
    importStack.clear();
    importAliasMembers.clear();
    syntheticParseStates_.clear();
    pendingMacroSources_.clear();
    pipeStreamCounter = 0;
    heapAuditCanonicalPath_.clear();

    // Generic-template state must also be cleared so prior-analysis ANTLR contexts
    // (which point into a discarded parse tree) don't survive into the next run.
    gts.Clear();

    // Re-register the built-in string type that was wiped by the clears above.
    // string.cb references 'string' as a return type before the struct is parsed,
    // so it must exist before any core file is compiled.
    RegisterBuiltinString();
    // Same for the closure fat type (Option A): the dataStructures entry was wiped, and
    // its lazily-registered dtor/copy live in the now-discarded module, so re-register the
    // type and arm the lazy lifetime registration to re-run for this file.
    RegisterBuiltinClosure();
    closureLifetimeRegistered = false;
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

std::string LLVMBackend::GetUserCacheDir()
{
#if defined(_WIN32)
    char buf[260] = {};
    size_t len = 0;
    if (getenv_s(&len, buf, sizeof(buf), "USERPROFILE") != 0 || len == 0)
        return {};
    return std::string(buf) + "\\.cflat";
#else
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::string(home) + "/.cflat";
    return {};
#endif
}

// Process-lifetime storage for SetCacheDirOverride(); a function-local static so it survives
// independent of GetCflatCacheDir()'s own memo.
static std::string& CacheDirOverrideStorage()
{
    static std::string override_;
    return override_;
}

void LLVMBackend::SetCacheDirOverride(std::string dir)
{
    CacheDirOverrideStorage() = std::move(dir);
}

// Which resolution rule GetCflatCacheDir() matched, for the -v report. Filled in alongside
// the memo below; empty until the first GetCflatCacheDir() call.
static std::string& CacheDirRuleStorage()
{
    static std::string rule;
    return rule;
}

std::string LLVMBackend::GetCflatCacheDirRule()
{
    return CacheDirRuleStorage();
}

/*
    A local <exeDir>/.cflat only counts when it actually looks populated: an empty or partial
    dir (interrupted or permission-failed --init-local) must not hijack a good per-user cache -
    that would silently cold-start the core bitcode and, on macOS, drop the libSystem stub so
    self-contained linking falls back to the Xcode SDK.
*/
static bool LocalCacheLooksPopulated(const std::filesystem::path& localCache)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(localCache / "compiler_path.txt", ec) || ec)
        return false;
    ec.clear();
    if (!std::filesystem::is_directory(localCache / "runtime", ec) || ec)
        return false;
    return true;
}

/*
    Resolution order (see internal/plan/init-local-cache.md):
      1. Process override - set by --init-local for its own run.
      2. CFLAT_CACHE_DIR env var, if set and non-empty.
      3. <exeDir>/.cflat, if it exists as a directory AND looks populated - makes --init-local
         sticky: once created, every later compile from that exe picks it up with no flag.
      4. The per-user cache (today's behaviour when no local cache exists).
    Resolved once per process into a function-local static. SetCacheDirOverride() must be
    called before the first call to this function in the process.
*/
std::string LLVMBackend::GetCflatCacheDir()
{
    static const std::string memo = []() -> std::string
    {
        if (const std::string& override_ = CacheDirOverrideStorage(); !override_.empty())
        {
            CacheDirRuleStorage() = "override";
            return override_;
        }

        if (const char* envDir = std::getenv("CFLAT_CACHE_DIR"); envDir && *envDir)
        {
            CacheDirRuleStorage() = "CFLAT_CACHE_DIR";
            return std::string(envDir);
        }

        std::string exeDir = std::filesystem::path(PlatformExePath()).parent_path().string();
        if (!exeDir.empty())
        {
            std::filesystem::path localCache = std::filesystem::path(exeDir) / ".cflat";
            std::error_code ec;
            if (std::filesystem::is_directory(localCache, ec) && !ec && LocalCacheLooksPopulated(localCache))
            {
                CacheDirRuleStorage() = "local";
                return localCache.string();
            }
        }

        CacheDirRuleStorage() = "per-user";
        return GetUserCacheDir();
    }();
    return memo;
}

/*
    Deletes the compiler cache tree rooted at `root`, e.g. via --init-clear / --init-clear-local.
    Everything cached lives under the one root (core bitcode, compiler_path.txt,
    linker_paths_*.json, the synthesized import libs, the cheaders/ C-header cache, and the
    macOS macsdk stubs), so a single recursive delete covers it. The caller supplies `root`
    directly - this function never resolves it itself, so --init-clear (both roots) and
    --init-clear-local (local root only) each name their exact target unambiguously. `root` is
    not canonicalised. The name/parent checks below cannot fail for a root produced by
    GetUserCacheDir() or <exeDir>/.cflat as they stand today; they exist to catch a future
    change to either.
*/
bool LLVMBackend::ClearCacheDir(const std::string& root, bool verbose, const std::string& label)
{
    if (root.empty())
    {
        std::cout << std::format("{} cache: (could not determine path)\n", label);
        return true;
    }

    std::filesystem::path rootPath(root);
    if (verbose)
        std::cout << std::format("[verbose] resolved {} cache directory: {}\n", label, rootPath.string());

    // Defence-in-depth only (see the comment above): a caller-supplied root should always end
    // in ".cflat", so neither of these can trip as the code stands today.
    if (rootPath.filename() != ".cflat")
    {
        std::cout << std::format("Error: refusing to delete '{}': the path does not end in '.cflat'.\n", rootPath.string());
        return false;
    }
    if (!rootPath.has_parent_path() || rootPath.parent_path() == rootPath || rootPath == rootPath.root_path())
    {
        std::cout << std::format("Error: refusing to delete '{}': it has no parent directory or is a filesystem root.\n", rootPath.string());
        return false;
    }

    // symlink_status (not status) so a symlinked root is seen as a link, never followed.
    // Check not_found before ec: a missing path reports ENOENT through ec on some libraries.
    std::error_code ec;
    std::filesystem::file_status st = std::filesystem::symlink_status(rootPath, ec);
    if (st.type() == std::filesystem::file_type::not_found)
    {
        std::cout << std::format("{} cache: {}\n", label, rootPath.string());
        std::cout << std::format("  Not present (no {} cache at {}).\n", label, rootPath.string());
        return true;
    }
    if (ec)
    {
        std::cout << std::format("Error: could not inspect {}: {}\n", rootPath.string(), ec.message());
        return false;
    }

    // A symlinked cache root points somewhere we were never asked to touch, so remove
    // only the link itself and leave the target tree intact.
    if (std::filesystem::is_symlink(st))
    {
        // remove() returns false with ec CLEARED when the path is already gone (a race
        // against symlink_status above); only a set ec is a real failure.
        if (!std::filesystem::remove(rootPath, ec) && ec)
        {
            std::cout << std::format("Error: could not remove symlink {}: {}\n", rootPath.string(), ec.message());
            return false;
        }
        std::cout << std::format("{} cache: {}\n", label, rootPath.string());
        std::cout << "  Removed the symlink only; its target tree was left untouched.\n";
        return true;
    }

    if (!std::filesystem::is_directory(st))
    {
        std::cout << std::format("Error: refusing to delete '{}': it is not a directory.\n", rootPath.string());
        return false;
    }

    /*
        remove_all does not report a count, so walk the tree first and tally what will go.
        Classify with symlink_status, never status: file_size/is_directory on a directory_entry
        follow symlinks, which would bill a link's target bytes to a file remove_all leaves
        on disk. skip_permission_denied is deliberately NOT used - it swallows an unreadable
        subtree without setting ec, so the count would silently undercount and we would have
        no offending path to name if the delete then fails.
    */
    uintmax_t fileCount = 0, dirCount = 1, linkCount = 0, byteCount = 0;   // dirCount counts root
    std::string blockedPath;
    bool walkIncomplete = false;
    std::filesystem::recursive_directory_iterator it(rootPath, ec), end;
    if (ec)
    {
        blockedPath = rootPath.string();
        walkIncomplete = true;
    }
    while (!walkIncomplete && it != end)
    {
        // An entry we cannot stat is still deleted by remove_all, just not classifiable here.
        std::error_code statEc;
        std::filesystem::file_status entrySt = std::filesystem::symlink_status(it->path(), statEc);
        if (!statEc)
        {
            if (std::filesystem::is_symlink(entrySt))
                ++linkCount;
            else if (std::filesystem::is_directory(entrySt))
                ++dirCount;
            else
            {
                ++fileCount;
                if (uintmax_t sz = std::filesystem::file_size(it->path(), statEc); !statEc)
                    byteCount += sz;
            }
        }

        // Capture the entry before incrementing: a failure here is the descent into it.
        std::filesystem::path current = it->path();
        it.increment(ec);
        if (ec)
        {
            blockedPath = current.string();
            walkIncomplete = true;
        }
    }
    if (walkIncomplete && verbose)
        std::cout << std::format("[verbose] could not fully walk {} (blocked at {}); counts are a lower bound.\n",
                                 rootPath.string(), blockedPath);

    std::error_code rmEc;
    std::filesystem::remove_all(rootPath, rmEc);
    if (rmEc)
    {
        // remove_all deletes what it can before failing, so the cache is likely half gone.
        std::cout << std::format("Error: could not fully remove {}: {}\n", rootPath.string(), rmEc.message());
        if (!blockedPath.empty())
            std::cout << std::format("  Blocked at: {} (check its permissions).\n", blockedPath);
        else
            std::cout << "  The blocking path was not reported by the filesystem; check permissions under the cache directory.\n";
        std::cout << "  The cache is now PARTIALLY deleted.\n";
        return false;
    }

    std::cout << std::format("{} cache: {}\n", label, rootPath.string());
    std::cout << std::format("  Deleted {} file(s) and {} symlink(s) in {} directory(ies), {} bytes total{}.\n",
                             fileCount, linkCount, dirCount, byteCount, walkIncomplete ? " (at least)" : "");
    return true;
}

// Shared by WriteCompilerPathToCache() and RunInit()'s per-user best-effort write below.
static bool WriteCompilerPathTo(const std::string& dir)
{
    if (dir.empty()) return false;
    if (std::error_code ec = llvm::sys::fs::create_directories(dir); ec)
        return false;

    std::string exePath = PlatformExePath();
    if (exePath.empty())
        return false;

    std::string outPath = (std::filesystem::path(dir) / "compiler_path.txt").string();
    std::error_code ec;
    llvm::raw_fd_ostream os(outPath, ec, llvm::sys::fs::OF_Text);
    if (ec) return false;
    os << exePath << "\n";
    return true;
}

bool LLVMBackend::WriteCompilerPathToCache()
{
    return WriteCompilerPathTo(GetCflatCacheDir());
}

#if defined(_WIN32)
// Win32 registry read. Declared bare (no windows.h) to keep this TU free of <winnt.h>, matching
// the RtlAddFunctionTable pattern above. Resolves against advapi32.lib (inherited default lib).
extern "C" long __stdcall RegGetValueA(
    void* hkey, const char* lpSubKey, const char* lpValue, unsigned long dwFlags,
    unsigned long* pdwType, void* pvData, unsigned long* pcbData);

// Read HKLM\SOFTWARE\Microsoft\Windows Kits\Installed Roots\KitsRoot10 - the canonical Windows SDK
// install root, correct even for non-default install locations. The SDK installer writes it under
// the 32-bit registry view, so try WOW6432 first then the native 64-bit view. Returns "" if absent.
static std::string ReadKitsRoot10FromRegistry()
{
    void* const hklm = reinterpret_cast<void*>(static_cast<intptr_t>(static_cast<int32_t>(0x80000002)));
    const char* subKey = "SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots";
    const unsigned long views[2] = { 0x00020000UL /*RRF_SUBKEY_WOW6432KEY*/, 0x00010000UL /*RRF_SUBKEY_WOW6464KEY*/ };
    for (unsigned long view : views)
    {
        char buf[1024];
        unsigned long cb = sizeof(buf);
        unsigned long flags = 0x00000002UL /*RRF_RT_REG_SZ*/ | view;
        // RegGetValueA NUL-terminates REG_SZ output, so buf is a valid C string on success.
        if (RegGetValueA(hklm, subKey, "KitsRoot10", flags, nullptr, buf, &cb) == 0 && cb > 1)
            return std::string(buf);
    }
    return "";
}

extern "C" long __stdcall RegOpenKeyExA(
    void* hKey, const char* lpSubKey, unsigned long ulOptions, unsigned long samDesired, void** phkResult);
extern "C" long __stdcall RegEnumValueA(
    void* hKey, unsigned long dwIndex, char* lpValueName, unsigned long* lpcchValueName,
    unsigned long* lpReserved, unsigned long* lpType, unsigned char* lpData, unsigned long* lpcbData);
extern "C" long __stdcall RegCloseKey(void* hKey);

// Fallback VS install-path lookup via the legacy HKLM\SOFTWARE\Microsoft\VisualStudio\SxS\VS7 key
// (32-bit view), mapping version ("17.0", "18.0", ...) to install dir. vswhere is preferred and
// authoritative; this only covers a missing/silent vswhere. Returns the greatest version's path.
static std::string ReadVsInstallPathFromRegistry()
{
    void* const hklm = reinterpret_cast<void*>(static_cast<intptr_t>(static_cast<int32_t>(0x80000002)));
    const unsigned long keyReadWow32 = 0x20019UL /*KEY_READ*/ | 0x0200UL /*KEY_WOW64_32KEY*/;
    void* hKey = nullptr;
    if (RegOpenKeyExA(hklm, "SOFTWARE\\Microsoft\\VisualStudio\\SxS\\VS7", 0, keyReadWow32, &hKey) != 0)
        return "";

    std::string bestPath;
    int bestMajor = -1;
    for (unsigned long i = 0; ; ++i)
    {
        char name[64];  unsigned long nameLen = sizeof(name);
        char data[1024]; unsigned long dataLen = sizeof(data);
        unsigned long type = 0;
        if (RegEnumValueA(hKey, i, name, &nameLen, nullptr, &type,
                          reinterpret_cast<unsigned char*>(data), &dataLen) != 0)
            break; // ERROR_NO_MORE_ITEMS or error
        if (type != 1 /*REG_SZ*/) continue;
        int major = 0;
        for (const char* p = name; *p >= '0' && *p <= '9'; ++p) major = major * 10 + (*p - '0');
        // REG_SZ data may not be NUL-terminated; trim any trailing NULs reported in dataLen.
        size_t len = dataLen;
        while (len > 0 && data[len - 1] == '\0') --len;
        if (major > bestMajor) { bestMajor = major; bestPath.assign(data, len); }
    }
    RegCloseKey(hKey);
    return bestPath;
}
#endif // _WIN32

LinkerPaths LLVMBackend::DiscoverLinkerPaths(const std::string& arch, const std::string& runtimeDir, bool verbose)
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

    // Fallback when vswhere is absent or returned nothing: the legacy SxS\VS7 registry key.
    bool vsFromRegistry = false;
#if defined(_WIN32)
    if (vsPath.empty())
    {
        vsPath = ReadVsInstallPathFromRegistry();
        vsFromRegistry = !vsPath.empty();
    }
#endif
    if (verbose)
    {
        if (vsPath.empty())
            std::cout << "[verbose] linker paths: Visual Studio install not found (vswhere + SxS\\VS7 registry)\n";
        else
            std::cout << std::format("[verbose] linker paths: Visual Studio via {} -> {}\n",
                                     vsFromRegistry ? "SxS\\VS7 registry" : "vswhere", vsPath);
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

    // Find the latest Windows SDK lib directories. Prefer the registry-reported install root
    // (handles non-default install locations), falling back to the default install path.
    {
        std::string wkLib;
        bool sdkFromRegistry = false;
#if defined(_WIN32)
        std::string kitsRoot = ReadKitsRoot10FromRegistry();
#else
        std::string kitsRoot;
#endif
        if (!kitsRoot.empty())
        {
            if (kitsRoot.back() != '\\' && kitsRoot.back() != '/') kitsRoot += '\\';
            std::string candidate = kitsRoot + "Lib";
            if (llvm::sys::fs::exists(candidate)) { wkLib = candidate; sdkFromRegistry = true; }
        }
        if (wkLib.empty())
            wkLib = "C:\\Program Files (x86)\\Windows Kits\\10\\Lib";
        if (verbose)
            std::cout << std::format("[verbose] linker paths: Windows SDK lib root via {} -> {}\n",
                                     sdkFromRegistry ? "KitsRoot10 registry" : "default path", wkLib);
        // Pick the latest version folder that actually carries ucrt\<arch>. The Lib root also
        // holds non-version siblings (e.g. 'wdf') and partial installs may lack a given arch;
        // requiring the ucrt subdir keeps a bogus non-existent path out of the result.
        std::string latestSDK;
        std::error_code ec;
        for (auto it = llvm::sys::fs::directory_iterator(wkLib, ec);
             it != llvm::sys::fs::directory_iterator(); it.increment(ec))
        {
            if (ec) break;
            auto ver = llvm::sys::path::filename(it->path()).str();
            if (ver <= latestSDK) continue;
            if (!llvm::sys::fs::exists(wkLib + "\\" + ver + "\\ucrt\\" + arch)) continue;
            latestSDK = ver;
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

    std::string jsonPath = cacheDir + "/linker_paths_" + arch + ".json";
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

    std::string jsonPath = cacheDir + "/linker_paths_" + arch + ".json";
    std::error_code fec;
    llvm::raw_fd_ostream out(jsonPath, fec);
    if (fec) return false;

    out << llvm::json::Value(std::move(obj));
    return true;
}

LinkerPaths LLVMBackend::FindLinkerPaths(const std::string& arch, const std::string& runtimeDir, bool verbose)
{
    if (auto cached = LoadLinkerPathsFromCache(arch))
    {
        // A cached empty ucrtLib means an earlier discovery found no Windows SDK (e.g. the
        // first compile ran before the SDK was installed). AllExist() accepts empty paths, so
        // such a stale entry would otherwise stick forever - re-prompting for --init even after
        // the SDK is installed. Only trust an empty ucrtLib when synthetic libs (from --init)
        // exist to cover it; otherwise fall through to re-discover and pick up a newer SDK.
        bool emptyUcrtCovered = !cached->ucrtLib.empty()
            || std::filesystem::exists(std::filesystem::path(GetSyntheticLibDir(arch)) / "ucrt.lib");
        if (emptyUcrtCovered)
        {
            if (verbose)
                std::cout << std::format("[verbose] linker paths: loaded from cache ({}/linker_paths_{}.json)\n",
                                         GetCflatCacheDir(), arch);
            return *cached;
        }
        if (verbose)
            std::cout << "[verbose] linker paths: cached ucrt-lib empty and no synthetic libs; re-discovering\n";
    }

    LinkerPaths paths = DiscoverLinkerPaths(arch, runtimeDir, verbose);
    SaveLinkerPathsToCache(arch, paths);
    return paths;
}

const char* LLVMBackend::DefaultPlatform()
{
#if defined(_WIN32)
    return "win64";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

std::string LLVMBackend::PlatformTargetTriple(const std::string& platform)
{
    if (platform == "macos" || platform == "macos-arm64") return "arm64-apple-macosx";
    if (platform == "win32") return "i686-pc-windows-msvc";
    if (platform == "win64") return "x86_64-pc-windows-msvc";
    if (platform == "linux") return "x86_64-pc-linux-gnu";
    return llvm::sys::getProcessTriple();
}

std::string LLVMBackend::DefaultCpuForPlatform(const std::string& platform)
{
    if (platform == "macos" || platform == "macos-arm64") return "apple-m1";
    if (platform == "win32") return "i686";
    if (platform == "win64") return "x86-64";
    return llvm::sys::getHostCPUName().str();
}

bool LLVMBackend::PrintSupportedCpus(const std::string& platform)
{
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();

    // Each platform's CPU table comes from its own backend - X86 for win32/win64/linux,
    // AArch64 for macos - so the list must follow the active --platform. Listing the
    // wrong backend's table is worse than useless: every name in it is rejected by
    // ResolveCpuName, which validates against this same triple.
    const std::string tripleStr = PlatformTargetTriple(platform);
    const char* triple = tripleStr.c_str();
    std::string err;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(llvm::Triple(triple), err);
    if (!target)
    {
        std::cout << std::format("Error: no target for triple '{}': {}\n", triple, err);
        return false;
    }

    std::unique_ptr<llvm::MCSubtargetInfo> sti(
        target->createMCSubtargetInfo(llvm::Triple(triple), "", ""));
    if (!sti)
    {
        std::cout << std::format("Error: could not create subtarget info for '{}'.\n", triple);
        return false;
    }

    // getAllProcessorDescriptions() carries the processor models, but AArch64 keeps
    // its CPU aliases (apple-m1, cyclone, ...) in a separate table that --cpu accepts
    // all the same - including apple-m1, the macOS default. Merge both and re-sort,
    // so the list is exactly the set ResolveCpuName lets through.
    std::vector<std::string> cpus;
    for (const auto& kv : sti->getAllProcessorDescriptions())
    {
        const char* key = kv.key();
        if (key && *key) cpus.emplace_back(key);
    }
    if (llvm::Triple(tripleStr).isAArch64())
        for (const auto& alias : llvm::AArch64::CpuAliases)
            cpus.emplace_back(llvm::AArch64::StrTab[alias.AltName].str());
    std::sort(cpus.begin(), cpus.end());
    cpus.erase(std::unique(cpus.begin(), cpus.end()), cpus.end());

    std::cout << std::format("Supported target CPUs for --platform {} ({}):\n",
                             platform, tripleStr);
    for (const auto& cpu : cpus)
        std::cout << std::format("  {}\n", cpu);
    return true;
}

bool LLVMBackend::PrintHostCpu()
{
    // getHostCPUName() reads CPUID and maps it to an LLVM CPU name (the same value
    // --cpu native resolves to). No target init is required for this query.
    llvm::StringRef host = llvm::sys::getHostCPUName();
    if (host.empty())
    {
        std::cout << "Error: could not determine the host CPU.\n";
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
            std::cout << std::format("[verbose] {} native resolved to: {}\n", label, name);
    }

    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();

    std::string err;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(llvm::Triple(triple), err);
    if (!target)
    {
        std::cout << std::format("Error: no target for triple '{}': {}\n", triple, err);
        return false;
    }

    std::unique_ptr<llvm::MCSubtargetInfo> sti(
        target->createMCSubtargetInfo(llvm::Triple(triple), "", ""));
    if (sti && !sti->isCPUStringValid(name))
    {
        std::cout << std::format("Error: unknown {} '{}'. Run --print-supported-cpus for the list.\n", label, name);
        return false;
    }

    resolved = name;
    return true;
}

// ---- Import-library synthesis (SDK-free system libs) ----
//
// Read a system DLL's export table and emit a name-only import library via
// `lld-link /lib /def`. Lets cflat output link against kernel32/ws2_32/ntdll/dbghelp/ucrt
// without the Windows SDK's .lib files: the DLLs are OS-resident, so --init can read their
// exports on any Win10+ machine and rebuild the import libs locally. The link line keeps
// the same lib NAMES (ucrt.lib, kernel32.lib, ...), so only the /libpath flips (Phase C).
// See internal/plan/remove-vcruntime-dependency.md Phase B. x64 only for now - x86 stdcall
// exports need @N decoration in the def (Phase D).

// %SystemRoot%\System32 (64-bit system DLLs on a 64-bit OS). Empty if SystemRoot is unset.
static std::string GetSystem32Dir()
{
    char buf[260] = {};
    size_t len = 0;
    if (getenv_s(&len, buf, sizeof(buf), "SystemRoot") != 0 || len == 0)
        return {};
    return std::string(buf) + "\\System32";
}

static bool ReadDllExportNames(const std::string& dllPath, std::vector<std::string>& names)
{
    auto binOrErr = llvm::object::createBinary(dllPath);
    if (!binOrErr) { llvm::consumeError(binOrErr.takeError()); return false; }
    auto owning = std::move(*binOrErr);
    auto* coff = llvm::dyn_cast<llvm::object::COFFObjectFile>(owning.getBinary());
    if (!coff) return false;
    for (const auto& exp : coff->export_directories())
    {
        llvm::StringRef name;
        if (exp.getSymbolName(name)) continue;   // error reading this entry -> skip
        if (name.empty()) continue;              // ordinal-only export -> we import by name
        names.push_back(name.str());
    }
    return !names.empty();
}

// Symbols cflat_builtins.c defines locally (it replaces VCRUNTIME140.dll). ucrtbase.dll and
// ntdll.dll happen to export the same names; the Windows SDK's curated import libs omit them,
// but reading a DLL's full export table does not - so we must skip them here or the link sees
// a duplicate symbol (our definition vs the synthesized import). Mirror cflat_builtins.c.
static const std::set<std::string>& CflatProvidedSymbols()
{
    static const std::set<std::string> s = {
        "memcpy", "memset", "memcmp", "memmove", "memchr",
        "strchr", "strrchr", "strstr", "wcschr", "wcsrchr", "wcsstr",
        "__C_specific_handler", "__current_exception", "__current_exception_context",
        "__std_type_info_destroy_list",
    };
    return s;
}

// Synthesize <outLib> as an import library for <importDllName>, exporting every name found in
// <dllPath> except those cflat_builtins.c provides. Returns false (and fills errMsg) on failure.
static bool SynthesizeImportLib(const std::string& lldLink, const std::string& dllPath,
                                const std::string& importDllName, const std::string& outLib,
                                const std::string& machine, std::string& errMsg)
{
    std::vector<std::string> names;
    if (!ReadDllExportNames(dllPath, names))
    { errMsg = "could not read exports from " + dllPath; return false; }

    const auto& excluded = CflatProvidedSymbols();
    names.erase(std::remove_if(names.begin(), names.end(),
                    [&](const std::string& n) { return excluded.count(n) != 0; }),
                names.end());

    llvm::SmallString<256> defFile;
    if (auto ec = llvm::sys::fs::createTemporaryFile("cflat_imp", "def", defFile))
    { errMsg = "could not create temp def: " + ec.message(); return false; }

    {
        std::error_code ec;
        llvm::raw_fd_ostream os(defFile, ec);
        if (ec) { errMsg = "could not open def: " + ec.message(); return false; }
        os << "LIBRARY " << importDllName << "\nEXPORTS\n";
        // Quote each name so C++-mangled exports (?, @) do not confuse the def parser.
        for (const auto& n : names) os << "    \"" << n << "\"\n";
    }

    std::string defArg  = "/def:" + defFile.str().str();
    std::string outArg  = "/out:" + outLib;
    std::string machArg = "/machine:" + machine;
    std::vector<std::string> argStrs = { lldLink, "/lib", defArg, outArg, machArg };
    std::vector<llvm::StringRef> args(argStrs.begin(), argStrs.end());

    std::string toolErr;
    int rc = llvm::sys::ExecuteAndWait(lldLink, args, std::nullopt, {}, 0, 0, &toolErr);
    llvm::sys::fs::remove(defFile);
    if (rc != 0)
    {
        errMsg = std::format("lld-link /lib failed (exit {}){}{}", rc,
                             toolErr.empty() ? "" : ": ", toolErr);
        return false;
    }
    return true;
}

// x86 import-lib synthesis. Unlike x64, x86 stdcall imports are decorated with the parameter
// byte count (_Foo@N), which a DLL export table does not carry and `lld-link /lib` cannot
// reconstruct (it has no kill-at). So we read the decorated thunk symbols from the SDK's x86
// import lib (the only reliable @N source, available at cflat build/CI time) and re-emit a
// clean import lib via llvm::object::writeImportLibrary, which applies kill-at: the linker
// symbol stays _Foo@N while the DLL-import name becomes the undecorated Foo. The result carries
// only names (ABI facts), no Microsoft code - the same shape MinGW ships.
static bool SynthesizeX86ImportLibFromSdk(const std::string& sdkLibPath,
                                          const std::string& importDllName,
                                          const std::string& outLib, std::string& errMsg)
{
    auto buf = llvm::MemoryBuffer::getFile(sdkLibPath, /*IsText=*/false, /*NullTerm=*/false);
    if (!buf) { errMsg = "cannot read " + sdkLibPath; return false; }
    auto archOr = llvm::object::Archive::create((*buf)->getMemBufferRef());
    if (!archOr) { llvm::consumeError(archOr.takeError()); errMsg = "not an archive: " + sdkLibPath; return false; }

    const auto& excluded = CflatProvidedSymbols();
    std::vector<llvm::object::COFFShortExport> exports;
    llvm::StringSet<> seen;
    for (const auto& sym : (*archOr)->symbols())
    {
        llvm::StringRef s = sym.getName();
        // Keep only the thunk symbols (_Foo / _Foo@N). Skip the import-address symbols and the
        // import-descriptor bookkeeping - writeImportLibrary regenerates those itself.
        if (!s.starts_with("_") || s.starts_with("__imp_")) continue;
        if (s.contains("IMPORT_DESCRIPTOR") || s.contains("NULL_THUNK_DATA")) continue;
        std::string importName = s.substr(1).str();           // strip leading underscore
        if (auto at = importName.find('@'); at != std::string::npos)
            importName.resize(at);                            // kill-at: drop @N for the DLL name
        if (excluded.count(importName)) continue;             // cflat_builtins.c provides these
        if (!seen.insert(s).second) continue;
        llvm::object::COFFShortExport e;
        e.Name = importName;        // undecorated DLL-import name (kill-at)
        e.SymbolName = s.str();     // decorated linker symbol (_Foo@N)
        exports.push_back(std::move(e));
    }
    if (exports.empty()) { errMsg = "no thunk symbols in " + sdkLibPath; return false; }

    if (llvm::Error e = llvm::object::writeImportLibrary(
            importDllName, outLib, exports, llvm::COFF::IMAGE_FILE_MACHINE_I386, /*MinGW=*/false))
    {
        errMsg = llvm::toString(std::move(e));
        return false;
    }
    return true;
}

// %USERPROFILE%\.cflat\lib\<arch> - where synthesized import libs live. "" on failure.
std::string LLVMBackend::GetSyntheticLibDir(const std::string& arch)
{
    std::string base = GetCflatCacheDir();
    if (base.empty()) return {};
    return base + "/lib/" + arch;
}

// Generate kernel32/ws2_32/ntdll/dbghelp/ucrt import libs for <arch> into the synthetic lib
// dir. ucrt.lib is built from ucrtbase.dll (the OS-resident UCRT; the api-ms-win-crt-* apisets
// all forward to it). Best-effort: logs each lib, returns true if all were written.
bool LLVMBackend::SynthesizeSystemImportLibs(const std::string& arch, const std::string& lldLink)
{
    if (arch != "x64")
        return false;   // x86 needs stdcall @N decoration - Phase D.
    if (lldLink.empty())
    {
        std::cout << "  Warning: lld-link not found; cannot synthesize import libs.\n";
        return false;
    }
    std::string sys32 = GetSystem32Dir();
    if (sys32.empty())
    {
        std::cout << "  Warning: SystemRoot not set; cannot synthesize import libs.\n";
        return false;
    }
    std::string libDir = GetSyntheticLibDir(arch);
    if (libDir.empty()) return false;
    if (std::error_code ec = llvm::sys::fs::create_directories(libDir); ec)
    {
        std::cout << std::format("  Warning: could not create {}: {}\n", libDir, ec.message());
        return false;
    }

    // { source DLL, import DLL name, output lib name }. ucrt maps ucrtbase.dll -> ucrt.lib so
    // the existing `ucrt.lib` link reference resolves unchanged.
    struct LibSpec { const char* dll; const char* importName; const char* out; };
    const LibSpec specs[] = {
        { "kernel32.dll", "kernel32.dll", "kernel32.lib" },
        { "ws2_32.dll",   "ws2_32.dll",   "ws2_32.lib"   },
        { "ntdll.dll",    "ntdll.dll",    "ntdll.lib"    },
        { "dbghelp.dll",  "dbghelp.dll",  "dbghelp.lib"  },
        { "advapi32.dll", "advapi32.dll", "advapi32.lib" },
        { "ucrtbase.dll", "ucrtbase.dll", "ucrt.lib"     },
    };

    bool allOk = true;
    for (const auto& s : specs)
    {
        std::string dllPath = sys32 + "\\" + s.dll;
        std::string outLib  = libDir + "\\" + s.out;
        std::string err;
        if (SynthesizeImportLib(lldLink, dllPath, s.importName, outLib, arch, err))
        {
            std::cout << std::format("  Synthesized {}\n", s.out);
        }
        else
        {
            std::cout << std::format("  Warning: could not synthesize {}: {}\n", s.out, err);
            allOk = false;
        }
    }
    return allOk;
}

// x86 system import libs. x86 stdcall needs @N decoration that only the SDK libs carry, so we
// re-emit clean import libs from them (see SynthesizeX86ImportLibFromSdk). ucrt maps the SDK's
// ucrt.lib -> our ucrt.lib but pointed at ucrtbase.dll (OS-resident; the apisets forward there).
bool LLVMBackend::SynthesizeX86SystemImportLibs(const LinkerPaths& paths)
{
    std::string libDir = GetSyntheticLibDir("x86");
    if (libDir.empty()) return false;
    if (std::error_code ec = llvm::sys::fs::create_directories(libDir); ec)
    {
        std::cout << std::format("  Warning: could not create {}: {}\n", libDir, ec.message());
        return false;
    }
    if (paths.umLib.empty() || paths.ucrtLib.empty())
    {
        std::cout << "  Warning: SDK x86 lib dirs not found; cannot synthesize x86 import libs.\n";
        return false;
    }

    // { source SDK lib, import DLL name, output lib }. The um libs are stdcall; ucrt is cdecl.
    struct LibSpec { std::string sdkLib; const char* importName; const char* out; };
    const LibSpec specs[] = {
        { paths.umLib   + "\\kernel32.lib", "kernel32.dll", "kernel32.lib" },
        { paths.umLib   + "\\ws2_32.lib",   "ws2_32.dll",   "ws2_32.lib"   },
        { paths.umLib   + "\\ntdll.lib",    "ntdll.dll",    "ntdll.lib"    },
        { paths.umLib   + "\\dbghelp.lib",  "dbghelp.dll",  "dbghelp.lib"  },
        { paths.umLib   + "\\advapi32.lib", "advapi32.dll", "advapi32.lib" },
        { paths.ucrtLib + "\\ucrt.lib",     "ucrtbase.dll", "ucrt.lib"     },
    };

    bool allOk = true;
    for (const auto& s : specs)
    {
        std::string outLib = libDir + "\\" + s.out;
        std::string err;
        if (SynthesizeX86ImportLibFromSdk(s.sdkLib, s.importName, outLib, err))
            std::cout << std::format("  Synthesized {}\n", s.out);
        else
        {
            std::cout << std::format("  Warning: could not synthesize {}: {}\n", s.out, err);
            allOk = false;
        }
    }
    return allOk;
}

#if defined(__APPLE__)
namespace {
// Mach-O export-trie decoding for the libSystem stub harvest. The trie is a
// prefix tree: each node has an optional terminal payload (a ULEB size) followed
// by edges (substring + child offset). A terminal node's accumulated prefix is
// an exported symbol name.
uint64_t MachoReadUleb(const uint8_t*& p, const uint8_t* end)
{
    uint64_t r = 0; int bit = 0;
    while (p < end) { uint8_t b = *p++; r |= uint64_t(b & 0x7f) << bit; bit += 7; if (!(b & 0x80)) break; }
    return r;
}
void MachoWalkExportTrie(const uint8_t* start, const uint8_t* p, const uint8_t* end,
                         const std::string& prefix, std::set<std::string>& out)
{
    if (p >= end) return;
    uint64_t termSize = MachoReadUleb(p, end);
    const uint8_t* children = p + termSize;
    // Skip $ld$ linker directives (previous-version aliases); keep real symbols.
    if (termSize != 0 && prefix.find("$ld$") == std::string::npos)
        out.insert(prefix);
    if (children >= end) return;
    uint8_t childCount = *children++;
    const uint8_t* s = children;
    for (uint8_t i = 0; i < childCount && s < end; i++)
    {
        std::string edge(reinterpret_cast<const char*>(s));
        s += edge.size() + 1;
        uint64_t off = MachoReadUleb(s, end);
        MachoWalkExportTrie(start, start + off, end, prefix + edge, out);
    }
}
// Walk one loaded image's export trie. For shared-cache dylibs the __LINKEDIT
// segment is shared; the in-memory trie base is linkedit_vmaddr + slide - linkedit_fileoff.
void MachoHarvestImage(const struct mach_header* mhc, intptr_t slide, std::set<std::string>& out)
{
    auto mh = reinterpret_cast<const mach_header_64*>(mhc);
    if (!mh || mh->magic != MH_MAGIC_64) return;
    uint64_t linkVm = 0, linkFoff = 0, expOff = 0, expSize = 0;
    auto cmd = reinterpret_cast<const load_command*>(reinterpret_cast<const uint8_t*>(mh) + sizeof(*mh));
    for (uint32_t i = 0; i < mh->ncmds; i++)
    {
        if (cmd->cmd == LC_SEGMENT_64)
        {
            auto sc = reinterpret_cast<const segment_command_64*>(cmd);
            if (std::strcmp(sc->segname, "__LINKEDIT") == 0) { linkVm = sc->vmaddr; linkFoff = sc->fileoff; }
        }
        else if (cmd->cmd == LC_DYLD_INFO || cmd->cmd == LC_DYLD_INFO_ONLY)
        {
            auto dc = reinterpret_cast<const dyld_info_command*>(cmd);
            expOff = dc->export_off; expSize = dc->export_size;
        }
        else if (cmd->cmd == LC_DYLD_EXPORTS_TRIE)
        {
            auto lc = reinterpret_cast<const linkedit_data_command*>(cmd);
            expOff = lc->dataoff; expSize = lc->datasize;
        }
        cmd = reinterpret_cast<const load_command*>(reinterpret_cast<const uint8_t*>(cmd) + cmd->cmdsize);
    }
    if (expOff == 0 || expSize == 0) return;
    const uint8_t* linkBase = reinterpret_cast<const uint8_t*>(linkVm + slide - linkFoff);
    const uint8_t* trie = linkBase + expOff;
    MachoWalkExportTrie(trie, trie, trie + expSize, "", out);
}
} // namespace

std::string LLVMBackend::MacStubSyslibroot()
{
    std::string cacheDir = GetCflatCacheDir();
    if (cacheDir.empty()) return {};
    std::string root = cacheDir + "/macsdk";
    if (llvm::sys::fs::exists(root + "/usr/lib/libSystem.tbd")) return root;
    return {};
}

std::string LLVMBackend::MacHostOsProductVersion()
{
    char buf[64] = {};
    size_t size = sizeof(buf);
    if (sysctlbyname("kern.osproductversion", buf, &size, nullptr, 0) != 0) return {};
    return std::string(buf);
}

std::string LLVMBackend::MacStubSdkVersion()
{
    std::string root = MacStubSyslibroot();
    if (root.empty()) return {};
    std::ifstream in(root + "/SDKVersion");
    if (!in) return {};
    std::string line;
    std::getline(in, line);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    return line;
}

bool LLVMBackend::HarvestMacSystemStub(const std::string& cacheDir, bool verbose)
{
    // libSystem's reexport closure lives under /usr/lib/system/ (plus libSystem
    // itself). Its own trie is nearly empty, so we flatten the children's exports.
    std::set<std::string> symbols;
    int images = 0;
    for (uint32_t i = 0; i < _dyld_image_count(); i++)
    {
        const char* name = _dyld_get_image_name(i);
        if (!name) continue;
        if (!std::strstr(name, "/usr/lib/system/lib") &&
            !std::strstr(name, "/usr/lib/libSystem.B.dylib"))
            continue;
        images++;
        MachoHarvestImage(_dyld_get_image_header(i), _dyld_get_image_vmaddr_slide(i), symbols);
    }
    if (symbols.empty())
    {
        std::cout << "  Warning: harvested no libSystem symbols; stub not written.\n";
        return false;
    }

    std::string libDir = cacheDir + "/macsdk/usr/lib";
    if (std::error_code ec = llvm::sys::fs::create_directories(libDir); ec)
    {
        std::cout << std::format("  Warning: could not create {}: {}\n", libDir, ec.message());
        return false;
    }
    std::string tbdPath = libDir + "/libSystem.tbd";
    std::error_code ec;
    llvm::raw_fd_ostream os(tbdPath, ec, llvm::sys::fs::OF_Text);
    if (ec)
    {
        std::cout << std::format("  Warning: could not write {}: {}\n", tbdPath, ec.message());
        return false;
    }
    // Flattened tbd v4: every reexported symbol listed directly under libSystem's
    // install-name. At runtime dyld's reexport chain still resolves each one.
    os << "--- !tapi-tbd\n";
    os << "tbd-version:     4\n";
    os << "targets:         [ arm64-macos ]\n";
    os << "install-name:    '/usr/lib/libSystem.B.dylib'\n";
    os << "current-version: 1\n";
    os << "compatibility-version: 1\n";
    os << "exports:\n";
    os << "  - targets:         [ arm64-macos ]\n";
    os << "    symbols:         [ ";
    bool first = true;
    for (const auto& s : symbols)
    {
        if (!first) os << ", ";
        first = false;
        os << "'" << s << "'";
    }
    os << " ]\n...\n";
    os.close();
    if (verbose)
        std::cout << std::format("  scanned {} images from the dyld shared cache\n", images);
    std::cout << std::format("  Saved macsdk/usr/lib/libSystem.tbd ({} symbols)\n", symbols.size());

    // Provenance: symbols were harvested from the LIVE dyld shared cache of the
    // running OS, so its product version IS the SDK version this stub represents.
    // Non-fatal - an older stub without this file just falls back at link time.
    std::string ver = TwoComponentVersion(MacHostOsProductVersion());
    if (!ver.empty())
    {
        std::string verPath = cacheDir + "/macsdk/SDKVersion";
        std::error_code vec;
        llvm::raw_fd_ostream vos(verPath, vec, llvm::sys::fs::OF_Text);
        if (!vec)
        {
            vos << ver << "\n";
            vos.close();
        }
        else
        {
            std::cout << std::format("  Warning: could not write {}: {}\n", verPath, vec.message());
        }
    }
    else
    {
        std::cout << "  Warning: could not determine host OS version; SDKVersion not written.\n";
    }
    return true;
}

bool LLVMBackend::HarvestMacImageStub(const std::string& cacheDir, const std::string& dlopenPath,
                                      const std::string& relTbdPath, bool verbose)
{
    // dlopen loads the image straight from the dyld shared cache (no on-disk file
    // required). RTLD_NOLOAD first in case it is already mapped into this process.
    void* h = dlopen(dlopenPath.c_str(), RTLD_LAZY | RTLD_NOLOAD);
    if (!h) h = dlopen(dlopenPath.c_str(), RTLD_LAZY);
    if (!h)
    {
        std::cout << std::format("  Warning: could not dlopen {}; stub not written.\n", dlopenPath);
        return false;
    }
    // Locate the loaded image by its install-name and harvest its export trie.
    std::set<std::string> symbols;
    std::string installName;
    for (uint32_t i = 0; i < _dyld_image_count(); i++)
    {
        const char* name = _dyld_get_image_name(i);
        if (!name || std::strcmp(name, dlopenPath.c_str()) != 0) continue;
        installName = name;
        MachoHarvestImage(_dyld_get_image_header(i), _dyld_get_image_vmaddr_slide(i), symbols);
        break;
    }
    if (installName.empty()) installName = dlopenPath;
    if (symbols.empty())
    {
        std::cout << std::format("  Warning: harvested no symbols from {}; stub not written.\n", dlopenPath);
        return false;
    }

    std::string tbdPath = cacheDir + "/macsdk/" + relTbdPath;
    std::string tbdDir = std::filesystem::path(tbdPath).parent_path().string();
    if (std::error_code ec = llvm::sys::fs::create_directories(tbdDir); ec)
    {
        std::cout << std::format("  Warning: could not create {}: {}\n", tbdDir, ec.message());
        return false;
    }
    std::error_code ec;
    llvm::raw_fd_ostream os(tbdPath, ec, llvm::sys::fs::OF_Text);
    if (ec)
    {
        std::cout << std::format("  Warning: could not write {}: {}\n", tbdPath, ec.message());
        return false;
    }
    // tbd v4 stub carrying the image's real install-name; whatever the trie yielded
    // (including weak-def and objc class symbols) is listed under exports.
    os << "--- !tapi-tbd\n";
    os << "tbd-version:     4\n";
    os << "targets:         [ arm64-macos ]\n";
    os << std::format("install-name:    '{}'\n", installName);
    os << "current-version: 1\n";
    os << "compatibility-version: 1\n";
    os << "exports:\n";
    os << "  - targets:         [ arm64-macos ]\n";
    os << "    symbols:         [ ";
    bool first = true;
    for (const auto& s : symbols)
    {
        if (!first) os << ", ";
        first = false;
        os << "'" << s << "'";
    }
    os << " ]\n...\n";
    os.close();
    if (verbose)
        std::cout << std::format("  harvested {}\n", installName);
    std::cout << std::format("  Saved macsdk/{} ({} symbols)\n", relTbdPath, symbols.size());
    return true;
}
#endif

/*
    Should RunInit re-seed the per-user ~/.cflat/compiler_path.txt when this run writes its
    cache somewhere else (--init-local, CFLAT_CACHE_DIR)? Yes when the record is missing,
    unreadable, empty, or names a compiler that no longer exists on disk - a worktree that has
    since been removed leaves the VS Code extension pointed at nothing, and because a local
    cache makes plain --init resolve locally too, no invocation could otherwise repair it.
    No when it names a file that is still there: that is someone's live registration, and
    silently repointing it at a worktree/portable build is the collision --init-local exists
    to avoid (test.sh/test.bat run --init-local on every suite run).
*/
static bool PerUserCompilerRecordIsStale(const std::filesystem::path& userRecord)
{
    std::error_code ec;
    if (!std::filesystem::exists(userRecord, ec) || ec)
        return true;

    std::ifstream in(userRecord);
    if (!in)
        return true;
    std::string recorded;
    std::getline(in, recorded);
    while (!recorded.empty() && (recorded.back() == '\r' || recorded.back() == '\n' ||
                                 recorded.back() == ' '  || recorded.back() == '\t'))
        recorded.pop_back();
    size_t firstNonWs = recorded.find_first_not_of(" \t");
    recorded = (firstNonWs == std::string::npos) ? std::string() : recorded.substr(firstNonWs);
    if (recorded.empty())
        return true;

    ec.clear();
    bool present = std::filesystem::exists(recorded, ec) && !ec;
    return !present;
}

bool LLVMBackend::RunInit(const std::string& runtimeDir, bool verbose)
{
    std::string cacheDir = GetCflatCacheDir();
    if (cacheDir.empty())
    {
        std::cout << "Error: could not determine cache directory (HOME/USERPROFILE not set).\n";
        return false;
    }

    if (std::error_code ec = llvm::sys::fs::create_directories(cacheDir); ec)
    {
        std::cout << std::format("Error: could not create {}: {}\n", cacheDir, ec.message());
        return false;
    }
    std::cout << std::format("Cache directory: {}\n", cacheDir);

    // Record this exe's path so the VS Code extension can auto-detect the compiler.
    if (WriteCompilerPathToCache())
        std::cout << "  Saved compiler_path.txt\n";
    else
        std::cout << "  Warning: could not write compiler_path.txt\n";

    // Best-effort seed of the per-user compiler_path.txt (the only file the VS Code extension
    // reads) when this run writes elsewhere; see PerUserCompilerRecordIsStale above.
    if (std::string userCacheDir = GetUserCacheDir(); !userCacheDir.empty() && userCacheDir != cacheDir)
    {
        std::filesystem::path userRecord = std::filesystem::path(userCacheDir) / "compiler_path.txt";
        if (PerUserCompilerRecordIsStale(userRecord) && WriteCompilerPathTo(userCacheDir))
            std::cout << std::format("  Also saved compiler_path.txt to {} (per-user cache) for the VS Code extension\n", userCacheDir);
    }

#if defined(__APPLE__)
    // macOS: synthesize the SDK-free libSystem link stub. The Windows linker-path /
    // import-lib steps do not apply, but we still build the core bitcode cache below
    // for the "macos" platform so native Darwin compiles get the same warm-start.
    std::cout << "Harvesting libSystem link stub from the dyld shared cache...\n";
    HarvestMacSystemStub(cacheDir, verbose);

    // Framework/dylib stubs so `import framework` links with no Xcode/CLT. Each is
    // dlopen'd from the shared cache and its export trie flattened into a tbd v4.
    std::cout << "Harvesting macOS framework link stubs from the dyld shared cache...\n";
    struct MacFwSpec { const char* dlopenPath; const char* relTbd; };
    const MacFwSpec macFwSpecs[] = {
        { "/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit",
          "System/Library/Frameworks/AppKit.framework/AppKit.tbd" },
        { "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation",
          "System/Library/Frameworks/Foundation.framework/Foundation.tbd" },
        { "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation",
          "System/Library/Frameworks/CoreFoundation.framework/CoreFoundation.tbd" },
        { "/usr/lib/libobjc.A.dylib", "usr/lib/libobjc.tbd" },
    };
    for (const auto& fs : macFwSpecs)
        HarvestMacImageStub(cacheDir, fs.dlopenPath, fs.relTbd, verbose);

    // The core bitcode cache is the one hard requirement of --init: without it the warm-cache
    // test pass runs cold and silently stops testing the serializer round-trip.
    bool anyCoreBitcodeSaved = false;
    for (const char* platform : {"macos"})
    {
        std::cout << std::format("Building core bitcode cache for {}...\n", platform);
        LLVMBackend coreCompiler;
        coreCompiler.SetRuntimeDir(runtimeDir);
        coreCompiler.SetVerbose(verbose);
        if (!coreCompiler.CompileCoreOnly(platform))
        {
            std::cout << std::format("  Warning: core compilation failed for {}.\n", platform);
            continue;
        }
        std::string bcCacheDir = coreCompiler.GetRuntimeBitcodeDir(runtimeDir);
        if (bcCacheDir.empty())
        {
            std::cout << "  Warning: could not determine bitcode cache dir.\n";
            continue;
        }
        if (std::error_code ec = llvm::sys::fs::create_directories(bcCacheDir); ec)
        {
            std::cout << std::format("  Warning: could not create {}: {}\n", bcCacheDir, ec.message());
            continue;
        }
        if (coreCompiler.SaveCoreBitcode(bcCacheDir, platform))
        {
            std::cout << std::format("  Saved core_{}.bc + .meta.json\n", platform);
            anyCoreBitcodeSaved = true;
        }
        else
            std::cout << std::format("  Warning: could not write core bitcode cache for {}.\n", platform);
    }
    if (!anyCoreBitcodeSaved)
    {
        std::cout << "Error: no core bitcode cache could be written; the cache is incomplete.\n";
        return false;
    }
    return true;
#else

    for (const char* arch : {"x64", "x86"})
    {
        std::cout << std::format("Discovering linker paths for {}...\n", arch);
        LinkerPaths paths = DiscoverLinkerPaths(arch, runtimeDir, verbose);

        std::cout << std::format("  lld-link: {}\n", paths.lldLink.empty() ? "(not found)" : paths.lldLink);
        std::cout << std::format("  msvc-lib: {}\n", paths.msvcLib.empty() ? "(not found)" : paths.msvcLib);
        std::cout << std::format("  ucrt-lib: {}\n", paths.ucrtLib.empty() ? "(not found)" : paths.ucrtLib);
        std::cout << std::format("  um-lib:   {}\n", paths.umLib.empty() ? "(not found)" : paths.umLib);

        if (SaveLinkerPathsToCache(arch, paths))
            std::cout << std::format("  Saved linker_paths_{}.json\n", arch);
        else
            std::cout << std::format("  Warning: could not write cache file for {}.\n", arch);

        // Synthesize SDK-free system import libs. x64 reads the OS DLLs directly; x86 re-emits
        // from the SDK libs (stdcall @N is not in the DLL export table - see the helpers).
        if (std::string(arch) == "x64")
        {
            std::cout << std::format("Synthesizing system import libs for {}...\n", arch);
            SynthesizeSystemImportLibs(arch, paths.lldLink);
        }
        else if (std::string(arch) == "x86")
        {
            std::cout << std::format("Synthesizing system import libs for {}...\n", arch);
            SynthesizeX86SystemImportLibs(paths);
        }
    }

    // Build core bitcode cache for win64 (win32 has pre-existing compilation issues). This is
    // the one hard requirement of --init: without it the warm-cache test pass runs cold.
    bool anyCoreBitcodeSaved = false;
    for (const char* platform : {"win64"})
    {
        std::cout << std::format("Building core bitcode cache for {}...\n", platform);
        LLVMBackend coreCompiler;
        coreCompiler.SetRuntimeDir(runtimeDir);
        coreCompiler.SetVerbose(verbose);
        if (!coreCompiler.CompileCoreOnly(platform))
        {
            std::cout << std::format("  Warning: core compilation failed for {}.\n", platform);
            continue;
        }
        std::string bcCacheDir = coreCompiler.GetRuntimeBitcodeDir(runtimeDir);
        if (bcCacheDir.empty())
        {
            std::cout << "  Warning: could not determine bitcode cache dir.\n";
            continue;
        }
        if (std::error_code ec = llvm::sys::fs::create_directories(bcCacheDir); ec)
        {
            std::cout << std::format("  Warning: could not create {}: {}\n", bcCacheDir, ec.message());
            continue;
        }
        if (coreCompiler.SaveCoreBitcode(bcCacheDir, platform))
        {
            std::cout << std::format("  Saved core_{}.bc + .meta.json\n", platform);
            anyCoreBitcodeSaved = true;
        }
        else
            std::cout << std::format("  Warning: could not write core bitcode cache for {}.\n", platform);
    }

    if (!anyCoreBitcodeSaved)
    {
        std::cout << "Error: no core bitcode cache could be written; the cache is incomplete.\n";
        return false;
    }
    return true;
#endif
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
std::string LLVMBackend::ComputeCoreHash(const std::string& runtimeDir) const
{
    auto cached = coreHashCache_.find(runtimeDir);
    if (cached != coreHashCache_.end()) return cached->second.hash;

    CoreHashCacheEntry entry;
    std::error_code ec;
    entry.canonicalRuntimeDir = std::filesystem::weakly_canonical(runtimeDir, ec);
    if (ec) entry.canonicalRuntimeDir = std::filesystem::path(runtimeDir);

    auto coreDir = entry.canonicalRuntimeDir / "core";
    std::vector<std::filesystem::path> files;
    for (auto& e : std::filesystem::directory_iterator(coreDir, ec))
        if (e.path().extension() == ".cb") files.push_back(e.path());
    if (!ec)
    {
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
        entry.hash = buf;
    }
    coreHashCache_.emplace(runtimeDir, std::move(entry));
    return coreHashCache_.find(runtimeDir)->second.hash;
}

// Returns <cache>/runtime/<hash>  or "" on failure. Built with std::filesystem
// so the separator is correct on every platform (backslash on Windows, slash otherwise).
std::string LLVMBackend::GetRuntimeBitcodeDir(const std::string& runtimeDir) const
{
    std::string base = GetCflatCacheDir();
    if (base.empty()) return {};
    std::string hash = ComputeCoreHash(runtimeDir);
    if (hash.empty()) return {};
    return (std::filesystem::path(base) / "runtime" / hash).string();
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
    auto s = LLVMBackend::SerializedTav::From(t);
    llvm::json::Object o;
    o["t"] = s.TypeName;
    if (!s.VariableName.empty())  o["n"]   = s.VariableName;
    if (s.Pointer)                o["p"]   = true;
    if (s.ElemPointer)            o["ep"]  = true;
    if (s.PointerDepth)           o["pd"]  = static_cast<int64_t>(s.PointerDepth);
    if (s.IsInterface)            o["if"]  = true;
    if (s.IsInterfacePointer)     o["ifp"] = true;
    if (s.IsNullable)             o["nl"]  = true;
    if (s.IsMove)                 o["mv"]  = true;
    if (s.IsAdopt)                o["ad"]  = true;
    if (s.IsAlias)                o["al"]  = true;
    if (s.IsOwningSink)           o["osk"] = true;
    if (s.IsConsumeInferredSink)  o["cis"] = true;
    if (s.IsBorrowOfAliasElement) o["bae"] = true;
    if (s.IsBond)                 o["bd"]  = true;
    if (s.IsUnique)               o["uq"]  = true;
    if (s.CallConv != LLVMBackend::CallingConv::Default) o["cc"] = static_cast<int64_t>(s.CallConv);
    if (s.LockThis)               o["lt"]  = true;
    if (s.LockThisMode != LockMode::Exclusive) o["ltm"] = static_cast<int64_t>(s.LockThisMode);
    if (!s.GuardedBy.empty())     o["gb"]  = s.GuardedBy;
    if (s.IsFunctionPointer)
    {
        o["fp"]  = true;
        o["fpr"] = s.FuncPtrReturnTypeName;
        if (s.FuncPtrReturnPointer) o["fprp"] = true;
        if (s.FuncPtrReturnOwned) o["fpro"] = true;
        if (s.FuncPtrReturnAlias) o["fpra"] = true;
        if (s.FuncPtrReturnPointerDepth > 1)
            o["fprd"] = static_cast<int64_t>(s.FuncPtrReturnPointerDepth);
        if (!s.FuncPtrReturnResolvedKey.empty()) o["fprk"] = s.FuncPtrReturnResolvedKey;
        llvm::json::Array fps;
        for (const auto& p : s.FuncPtrParams)
        {
            llvm::json::Object po;
            po["t"] = p.TypeName;
            if (p.Pointer) po["p"] = true;
            if (p.AllocAlignValue > 0) po["aav"] = static_cast<int64_t>(p.AllocAlignValue);
            if (p.IsMove)  po["mv"] = true;
            if (p.IsOwningSink) po["osk"] = true;
            if (p.IsConsumeInferredSink) po["cis"] = true;
            if (p.PointerDepth > 1) po["pd"] = static_cast<int64_t>(p.PointerDepth);
            if (!p.ResolvedTypeKey.empty()) po["rk"] = p.ResolvedTypeKey;
            fps.push_back(std::move(po));
        }
        o["fpp"] = std::move(fps);
    }
    if (s.ConstArraySize > 0)
    {
        o["as"] = static_cast<int64_t>(s.ConstArraySize);
        if (!s.ConstInnerDimensions.empty())
        {
            llvm::json::Array dims;
            for (auto d : s.ConstInnerDimensions) dims.push_back(static_cast<int64_t>(d));
            o["aid"] = std::move(dims);
        }
    }
    if (s.IsSimd)
    {
        o["sd"] = true;
        o["sdl"] = static_cast<int64_t>(s.SimdLanes);
    }
    if (s.IsArrayView) o["av"] = true;
    if (s.AllocAlignValue > 0) o["aa"] = static_cast<int64_t>(s.AllocAlignValue);
    return o;
}

static TAV DeserializeTav(const llvm::json::Object& o)
{
    LLVMBackend::SerializedTav s;
    if (auto v = o.getString("t"))   s.TypeName = v->str();
    if (auto v = o.getString("n"))   s.VariableName = v->str();
    if (auto v = o.getBoolean("p"))  s.Pointer = *v;
    if (auto v = o.getBoolean("ep")) s.ElemPointer = *v;
    if (auto v = o.getInteger("pd")) s.PointerDepth = static_cast<int>(*v);
    if (auto v = o.getBoolean("if")) s.IsInterface = *v;
    if (auto v = o.getBoolean("ifp"))s.IsInterfacePointer = *v;
    if (auto v = o.getBoolean("nl")) s.IsNullable = *v;
    if (auto v = o.getBoolean("mv")) s.IsMove = *v;
    if (auto v = o.getBoolean("ad")) s.IsAdopt = *v;
    if (auto v = o.getBoolean("al")) s.IsAlias = *v;
    if (auto v = o.getBoolean("osk")) s.IsOwningSink = *v;
    if (auto v = o.getBoolean("cis")) s.IsConsumeInferredSink = *v;
    if (auto v = o.getBoolean("bae")) s.IsBorrowOfAliasElement = *v;
    if (auto v = o.getBoolean("bd")) s.IsBond = *v;
    if (auto v = o.getBoolean("uq")) s.IsUnique = *v;
    if (auto v = o.getInteger("cc")) s.CallConv = static_cast<LLVMBackend::CallingConv>(*v);
    if (auto v = o.getBoolean("lt")) s.LockThis = *v;
    if (auto v = o.getInteger("ltm")) s.LockThisMode = static_cast<LockMode>(*v);
    if (auto v = o.getString("gb"))  s.GuardedBy = v->str();
    if (auto v = o.getBoolean("fp")) s.IsFunctionPointer = *v;
    if (s.IsFunctionPointer)
    {
        if (auto v = o.getString("fpr"))   s.FuncPtrReturnTypeName = v->str();
        if (auto v = o.getBoolean("fprp")) s.FuncPtrReturnPointer = *v;
        if (auto v = o.getBoolean("fpro")) s.FuncPtrReturnOwned = *v;
        if (auto v = o.getBoolean("fpra")) s.FuncPtrReturnAlias = *v;
        if (s.FuncPtrReturnPointer) s.FuncPtrReturnPointerDepth = 1;
        if (auto v = o.getInteger("fprd")) s.FuncPtrReturnPointerDepth = static_cast<int>(*v);
        if (auto v = o.getString("fprk")) s.FuncPtrReturnResolvedKey = v->str();
        if (auto* fps = o.getArray("fpp"))
            for (auto& elem : *fps)
                if (auto* po = elem.getAsObject())
                {
                    LLVMBackend::SerializedTav::FuncPtrParam p;
                    if (auto v = po->getString("t"))  p.TypeName = v->str();
                    if (auto v = po->getBoolean("p")) p.Pointer = *v;
                    if (auto v = po->getInteger("aav")) p.AllocAlignValue = static_cast<uint64_t>(*v);
                    if (auto v = po->getBoolean("mv"))p.IsMove = *v;
                    if (auto v = po->getBoolean("osk")) p.IsOwningSink = *v;
                    if (auto v = po->getBoolean("cis")) p.IsConsumeInferredSink = *v;
                    if (p.Pointer) p.PointerDepth = 1;
                    if (auto v = po->getInteger("pd")) p.PointerDepth = static_cast<int>(*v);
                    if (auto v = po->getString("rk")) p.ResolvedTypeKey = v->str();
                    s.FuncPtrParams.push_back(std::move(p));
                }
    }
    if (auto v = o.getInteger("as")) s.ConstArraySize = static_cast<uint64_t>(*v);
    if (auto* dims = o.getArray("aid"))
        for (auto& d : *dims)
            if (auto v = d.getAsInteger()) s.ConstInnerDimensions.push_back(static_cast<uint64_t>(*v));
    if (auto v = o.getBoolean("sd")) s.IsSimd = *v;
    if (auto v = o.getInteger("sdl")) s.SimdLanes = static_cast<uint64_t>(*v);
    if (auto v = o.getBoolean("av")) s.IsArrayView = *v;
    if (auto v = o.getInteger("aa")) s.AllocAlignValue = static_cast<uint64_t>(*v);
    return s.ToTypeAndValue();
}

static llvm::json::Array SerializeAnnotations(const std::vector<ANN>& anns)
{
    llvm::json::Array arr;
    for (auto& a : anns)
    {
        llvm::json::Object ao;
        ao["n"] = a.Name;
        if (!a.Value.empty()) ao["v"] = a.Value;
        if (a.Values.size() > 1)
        {
            llvm::json::Array vs;
            for (auto& v : a.Values) vs.push_back(v);
            ao["vs"] = std::move(vs);
        }
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
            if (auto* vs = ao->getArray("vs"))
            {
                for (auto& ve : *vs)
                    if (auto v = ve.getAsString()) a.Values.push_back(v->str());
            }
            else if (!a.Value.empty()) a.Values.push_back(a.Value);
            out.push_back(std::move(a));
        }
    return out;
}

static llvm::json::Object SerializeDtav(const DTAV& d)
{
    auto o = SerializeTav(d);
    if (d.external)    o["ext"] = true;
    if (d.threadLocal) o["tl"]  = true;
    if (d.staticStorage) o["sta"] = true;
    if (d.IsBitfield)
    {
        o["bf"]   = true;
        o["bfw"]  = static_cast<int64_t>(d.BitWidth);
        o["bfo"]  = static_cast<int64_t>(d.BitOffset);
        o["bfsi"] = static_cast<int64_t>(d.StorageFieldIndex);
    }
    // Synthetic slots: the storage slot itself has IsBitfield=false, so both flags are
    // written outside the bitfield block or they would not round-trip.
    if (d.IsBitfieldStorage) o["bfs"] = true;
    if (d.IsPadding)         o["pad"] = true;
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
    if (auto v = o.getBoolean("sta")) d.staticStorage = *v;
    if (auto v = o.getBoolean("bf"))
    {
        d.IsBitfield = *v;
        if (auto w = o.getInteger("bfw"))  d.BitWidth = static_cast<unsigned>(*w);
        if (auto w = o.getInteger("bfo"))  d.BitOffset = static_cast<unsigned>(*w);
        if (auto w = o.getInteger("bfsi")) d.StorageFieldIndex = static_cast<unsigned>(*w);
    }
    if (auto v = o.getBoolean("bfs")) d.IsBitfieldStorage = *v;
    if (auto v = o.getBoolean("pad")) d.IsPadding = *v;
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
    if (s.External)     o["ext"] = true;
    if (s.ReturnsOwned) o["ro"] = true;
    if (s.ReturnsAlias) o["ra"] = true;
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
    targetWindows_ = (platform == "win32" || platform == "win64");
    // macos / macos-arm64 mirror the normal-compile setup in Init(): POSIX, arm64.
    targetMacOS_ = (platform == "macos" || platform == "macos-arm64");
    targetArm64_ = targetMacOS_;
    // Core bitcode is per-platform (core_<platform>.bc), so `long` must use this target's width.
    SetTargetLongWidth(targetWindows_, platformValue);

    // Reinitialize the LLVM module with the correct platform data layout BEFORE
    // RegisterBuiltinString creates pointer types, so %string fields have the right size.
    // The Init()-created module used the LLVM default layout which differs on Win32.
    {
        builder.reset();
        module.reset();
        coreBitcodeBuffer_.reset();
        functionTable.clear();
        cAbiFunctionThunkCache_.clear();
        dataStructures.clear();
        DropModuleEscapeMemo();   // every llvm::Function* just died with the module
        context = std::make_unique<llvm::LLVMContext>();
        module  = std::make_unique<llvm::Module>("cflat", *context);
        builder = std::make_unique<llvm::IRBuilder<>>(*context);
    }
    // Match the layout/triple the normal compile path selects for each target.
    const char* dl = targetMacOS_
        ? "e-m:o-i64:64-i128:128-n32:64-S128"
        : PlatformDataLayout(platformValue);
    module->setDataLayout(llvm::DataLayout(dl));
    module->setTargetTriple(llvm::Triple(targetMacOS_ ? "arm64-apple-macosx"
                            : (platformValue == 32) ? "i686-pc-windows-msvc"
                                                    : "x86_64-pc-windows-msvc"));
    RegisterBuiltinString();
    RegisterBuiltinClosure();   // closure fat type as an owning value type (Option A)

    coreSymbolIndex_.Clear();
    {
        // Capture core symbol registrations for LSP replay; guard covers every exit
        // including exceptions escaping CompileImportedFile. NOTE: a non-null symbolSink_
        // also flips C-interop/framework paths into LSP-degraded mode; harmless while
        // core imports are pure .cb - decouple before core imports a .c/header/framework.
        symbolSink_ = &coreSymbolIndex_;
        struct SinkGuard
        {
            LLVMBackend* backend;
            ~SinkGuard() { backend->symbolSink_ = nullptr; }
        } sinkGuard{this};

        SetPlatformMacros();

        if (runtimeDir.empty())
            return false;
        auto runtimePath = std::filesystem::path(runtimeDir) / "core" / "runtime.cb";
        if (!std::filesystem::exists(runtimePath))
        {
            std::cout << std::format("Error: runtime.cb not found in {}/core\n", runtimeDir);
            return false;
        }
        if (!CompileImportedFile(runtimePath.string(), "runtime.cb"))
            return false;
    }
    // Force lazy-registered functions into the bitcode so they are available
    // when the cache is loaded without re-running EnsureStr*/EnsureString*.
    EnsureStrConcatRegistered();
    EnsureStringDtorRegistered();
    EnsureClosureLifetimeRegistered();   // closure dtor + env-cloning copy (Option A)
    // Core bitcode must never cache a declaration-only rebox thunk: cache loading adopts such
    // thunks, but cannot repair invalid IR that was already serialized.
    if (!deferredIfaceRebox_.empty())
        LogError("cannot save core bitcode with deferred interface rebox thunks");
    return true;
}

bool LLVMBackend::SaveCoreBitcode(const std::string& cacheDir, const std::string& platform) const
{
    std::string prefix = (std::filesystem::path(cacheDir) / ("core_" + platform)).string();

    // Write LLVM bitcode.
    {
        llvm::TimeTraceScope bcScope("CoreCacheBitcodeWrite", prefix);
        std::error_code ec;
        llvm::raw_fd_ostream bcOut(prefix + ".bc", ec);
        if (ec) return false;
        llvm::WriteBitcodeToFile(*module, bcOut);
    }

    // Build metadata JSON. Scope the serialization walk separately from the file
    // write below so -ftime-trace mirrors the read-side split (deserialize vs parse).
    llvm::json::Object root;
    llvm::json::Object symbolsRoot;
    {
        llvm::TimeTraceScope buildScope("CoreCacheJsonBuild", prefix);
    // Each version added tables an older cache lacks, so an older cache must be rejected rather
    // than silently reused (v6 includes the LSP core-variable replay state; v8 is the non-pointer
    // `alias T` parameter reference ABI - such a parameter is now passed as a pointer to the
    // caller's object, so cached v7 core bitcode has the wrong calling convention for it).
    root["version"]   = 10;
    root["platform"]  = platform;
    root["core_hash"] = ComputeCoreHash(runtimeDir);

    // importedFiles - store paths relative to runtimeDir so the cache is
    // portable between Debug and Release builds that share core/*.cb files.
    // std::filesystem::relative handles the platform separator/case rules; the
    // load side rejoins the stored relative path against the current runtimeDir.
    {
        llvm::json::Array arr;
        std::filesystem::path rdPath(runtimeDir);
        std::error_code ec;
        std::filesystem::path rdCanon = std::filesystem::weakly_canonical(rdPath, ec);
        if (ec) rdCanon = rdPath;
        for (auto& f : importedFiles)
        {
            std::error_code rec;
            std::filesystem::path fCanon = std::filesystem::weakly_canonical(std::filesystem::path(f), rec);
            if (rec) fCanon = std::filesystem::path(f);
            std::filesystem::path rel = std::filesystem::relative(fCanon, rdCanon, rec);
            // Store relative only when f is genuinely under runtimeDir (no leading ..).
            if (!rec && !rel.empty() && rel.native().rfind(std::filesystem::path("..").native(), 0) != 0)
                arr.push_back(rel.string());
            else
                arr.push_back(f);
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

    // manglingAliases_ - pure-rename `using` aliases MangleTypeArg folds. A warm cache never
    // re-scans the core .cb files, so without this a core alias (os.posix.cb `using win_int = i32`)
    // would mangle transparently cold and opaquely warm.
    {
        llvm::json::Object obj;
        for (auto& [k, v] : manglingAliases_) obj[k] = v;
        root["mangling_aliases"] = std::move(obj);
    }

    // enumBackingTypes
    {
        llvm::json::Object obj;
        for (auto& [k, v] : enumBackingTypes) obj[k] = v;
        root["enum_backing_types"] = std::move(obj);
    }

    // annotationRegistry - annotation name -> declared field names. An empty array is
    // meaningful (a no-arg annotation), so every entry must be preserved.
    {
        llvm::json::Object obj;
        for (auto& [name, fields] : annotationRegistry)
        {
            llvm::json::Array arr;
            for (auto& f : fields) arr.push_back(f);
            obj[name] = std::move(arr);
        }
        root["annotation_decls"] = std::move(obj);
    }

    // typeAnnotations_ - type/interface name -> annotations attached to it.
    {
        llvm::json::Object obj;
        for (auto& [name, anns] : typeAnnotations_) obj[name] = SerializeAnnotations(anns);
        root["type_annotations"] = std::move(obj);
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

            if (!sd.StaticInterfaces.empty())
            {
                llvm::json::Array sifaces;
                for (auto& i : sd.StaticInterfaces) sifaces.push_back(i);
                so["static_interfaces"] = std::move(sifaces);
            }

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
            if (auto it = interfaceDefSites.find(name); it != interfaceDefSites.end())
                io["defsite"] = it->second;
            io["defsite_is_core"] = coreInterfaceDefs_.count(name) != 0;
            llvm::json::Array parents;
            if (auto it = interfaceParents.find(name); it != interfaceParents.end())
                for (auto& p : it->second) parents.push_back(p);
            io["parents"] = std::move(parents);
            llvm::json::Array ms;
            for (auto& m : methods) ms.push_back(SerializeIfaceMethod(m));
            io["methods"] = std::move(ms);
            llvm::json::Array fs;
            if (auto it = interfaceFields.find(name); it != interfaceFields.end())
                for (auto& f : it->second) fs.push_back(SerializeTav(f));
            io["fields"] = std::move(fs);
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

    // LSP symbol definitions are emitted by the core listener walk. Keep them in a sidecar so
    // CLI cache loads do not parse data that only LSP and symbol-query consumers need.
    // File paths are stored relative to runtimeDir (like imported_files above) so the cache stays
    // portable between Debug/Release builds sharing one cache directory.
    auto relToRuntime = [&](const std::string& file) -> std::string
    {
        std::error_code rec;
        std::filesystem::path rdCanon = std::filesystem::weakly_canonical(
            std::filesystem::path(runtimeDir), rec);
        if (rec) rdCanon = std::filesystem::path(runtimeDir);
        std::filesystem::path fCanon = std::filesystem::weakly_canonical(
            std::filesystem::path(file), rec);
        if (rec) fCanon = std::filesystem::path(file);
        std::filesystem::path rel = std::filesystem::relative(fCanon, rdCanon, rec);
        if (!rec && !rel.empty()
            && rel.native().rfind(std::filesystem::path("..").native(), 0) != 0)
            return rel.string();
        return file;
    };
    {
        llvm::json::Array arr;
        for (const auto& [name, def] : coreSymbolIndex_.Symbols())
        {
            llvm::json::Object so;
            so["name"] = def.name;
            so["display_name"] = def.displayName;
            so["kind"] = static_cast<int64_t>(def.kind);
            so["file"] = relToRuntime(def.file);
            so["line"] = static_cast<int64_t>(def.line);
            so["column"] = static_cast<int64_t>(def.column);
            so["signature"] = def.signatureMarkdown;
            so["doc"] = def.docComment;
            llvm::json::Array members;
            for (const auto& member : def.memberNames) members.push_back(member);
            so["members"] = std::move(members);
            llvm::json::Array overloads;
            for (const auto& overload : def.overloadSignatures) overloads.push_back(overload);
            so["overloads"] = std::move(overloads);
            arr.push_back(std::move(so));
        }
        symbolsRoot["core_symbols"] = std::move(arr);
    }

    {
        llvm::json::Array arr;
        for (const auto& [name, info] : coreSymbolIndex_.Variables())
        {
            llvm::json::Object vo;
            vo["name"] = name;
            vo["type"] = info.typeName;
            vo["display_type"] = info.displayTypeName;
            vo["file"] = relToRuntime(info.file);
            vo["line"] = static_cast<int64_t>(info.line);
            vo["column"] = static_cast<int64_t>(info.column);
            arr.push_back(std::move(vo));
        }
        symbolsRoot["core_variables"] = std::move(arr);
    }

    // manifest fragments and their [JsonText] leaf records. These are compile-time state from
    // core declarations; dropping them on a warm cache would silently remove the manifest.
    {
        llvm::json::Array arr;
        for (const auto& fragment : manifestFragments_)
        {
            llvm::json::Object fo;
            fo["source_file"] = fragment.SourceFile;
            fo["line"] = static_cast<int64_t>(fragment.Line);
            fo["xml"] = fragment.Xml;
            llvm::json::Array leaves;
            for (const auto& leaf : fragment.Leaves)
            {
                llvm::json::Object lo;
                lo["namespace"] = leaf.Namespace;
                lo["local_name"] = leaf.LocalName;
                lo["text"] = leaf.Text;
                lo["source_file"] = leaf.SourceFile;
                lo["line"] = static_cast<int64_t>(leaf.Line);
                leaves.push_back(std::move(lo));
            }
            fo["leaves"] = std::move(leaves);
            arr.push_back(std::move(fo));
        }
        root["manifest_fragments"] = std::move(arr);
    }

    // Serialize one family of generic templates into root[jsonKey].
    // All template context types expose getStart()/getStop() via ParserRuleContext, so the
    // templates map is accepted as `const auto&` and the type is deduced per call site.
    using TypeParamsMap  = std::unordered_map<std::string, std::vector<std::string>>;
    using PackIndexMap   = std::unordered_map<std::string, size_t>;
    using ConstraintsMap = std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::string>>>;
    using ValueParamsMap = std::unordered_map<std::string, std::vector<std::string>>;

    // "decl_ns" is load-bearing for the --init round-trip: TemplateNamespaceScope reads the
    // declaring namespace when instantiating a cached template, and it cannot be re-derived from
    // the key. Every core template is at global scope today, so dropping it happens to be
    // harmless right now - which is exactly how it would go unnoticed if a core library ever
    // declared a generic inside a namespace.
    auto serializeGenericTemplates = [&](
        const auto&           templatesMap,
        const TypeParamsMap&  typeParams,
        const ValueParamsMap& valueParams,
        const PackIndexMap&   packIndex,
        const ConstraintsMap* constraints,
        llvm::StringRef       jsonKey)
    {
        llvm::json::Array arr;
        for (auto& [name, ctx] : templatesMap)
        {
            auto* start = ctx->getStart();
            auto* stop  = ctx->getStop();
            antlr4::misc::Interval iv(start->getStartIndex(), stop->getStopIndex());
            std::string src = start->getInputStream()->getText(iv);
            llvm::json::Object to;
            to["name"]   = name;
            to["source"] = src;
            llvm::json::Array tps;
            if (auto it = typeParams.find(name); it != typeParams.end())
                for (auto& tp : it->second) tps.push_back(tp);
            to["type_params"] = std::move(tps);
            llvm::json::Array vps;
            if (auto it = valueParams.find(name); it != valueParams.end())
                for (auto& vp : it->second) vps.push_back(vp);
            to["value_param_types"] = std::move(vps);
            if (auto pit = packIndex.find(name); pit != packIndex.end())
                to["pack_index"] = static_cast<int64_t>(pit->second);
            if (constraints)
                if (auto cit = constraints->find(name); cit != constraints->end())
                    to["constraints"] = SerializeConstraints(cit->second);
            if (auto nsIt = gts.genericTemplateNamespace.find(name);
                nsIt != gts.genericTemplateNamespace.end() && !nsIt->second.empty())
                to["decl_ns"] = nsIt->second;
            // "core": the template was DECLARED in a core library file. Load-bearing for the
            // warm cache - the core sources are never re-read, so the origin marker that
            // IsBorrowingContainerElementSink reads has to survive the round-trip.
            if (gts.coreGenericTemplates.count(name) != 0)
                to["core"] = true;
            arr.push_back(std::move(to));
        }
        root[jsonKey] = std::move(arr);
    };

    // Generic struct templates: source text + type params + constraints
    serializeGenericTemplates(gts.genericStructTemplates,
                              gts.genericStructTypeParams,
                              gts.genericStructValueParams,
                              gts.genericStructPackIndex,
                              &gts.genericStructConstraints,
                              "generic_structs");

    // Generic class templates (share genericStructTypeParams - no separate class type-param map)
    serializeGenericTemplates(gts.genericClassTemplates,
                              gts.genericStructTypeParams,
                              gts.genericStructValueParams,
                              gts.genericClassPackIndex,
                              &gts.genericClassConstraints,
                              "generic_classes");

    // Generic interface templates (no constraints)
    serializeGenericTemplates(gts.genericInterfaceTemplates,
                              gts.genericInterfaceTypeParams,
                              gts.genericInterfaceValueParams,
                              gts.genericInterfacePackIndex,
                              nullptr,
                              "generic_interfaces");

    // Generic function templates
    serializeGenericTemplates(gts.genericFunctionTemplates,
                              gts.genericFunctionTypeParams,
                              gts.genericFunctionValueParams,
                              gts.genericFunctionPackIndex,
                              &gts.genericFunctionConstraints,
                              "generic_functions");

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
    } // end CoreCacheJsonBuild scope

    // Write JSON.
    {
        llvm::TimeTraceScope jsonScope("CoreCacheJsonWrite", prefix);
        std::error_code ec;
        llvm::raw_fd_ostream jsonOut(prefix + ".meta.json", ec);
        if (ec) return false;
        jsonOut << llvm::json::Value(std::move(root));

        llvm::raw_fd_ostream symbolsOut(prefix + ".symbols.json", ec);
        if (ec) return false;
        symbolsOut << llvm::json::Value(std::move(symbolsRoot));
    }
    return true;
}

bool LLVMBackend::LoadCoreBitcodeIfFresh(const std::string& cacheDir, const std::string& platform)
{
    std::string prefix   = (std::filesystem::path(cacheDir) / ("core_" + platform)).string();
    std::string bcPath   = prefix + ".bc";
    std::string jsonPath = prefix + ".meta.json";
    std::string symbolsPath = prefix + ".symbols.json";

    if (!std::filesystem::exists(bcPath) || !std::filesystem::exists(jsonPath))
        return false;

    std::string coreHash = ComputeCoreHash(runtimeDir);
    if (coreHash.empty()) return false;

    auto loadJson = [&](const std::string& path,
                        std::unordered_map<std::string, CoreJsonCacheEntry>& cache,
                        const char* traceName) -> llvm::json::Value*
    {
        std::error_code ec;
        auto writeTime = std::filesystem::last_write_time(path, ec);
        if (ec) return nullptr;
        auto fileSize = std::filesystem::file_size(path, ec);
        if (ec) return nullptr;

        auto cached = cache.find(path);
        if (cached != cache.end()
            && cached->second.coreHash == coreHash
            && cached->second.writeTime == writeTime
            && cached->second.fileSize == fileSize
            && cached->second.buffer && cached->second.value)
            return &*cached->second.value;

        auto jsonBuf = llvm::MemoryBuffer::getFile(path);
        if (!jsonBuf) return nullptr;
        auto parsed = [&] {
            llvm::TimeTraceScope jsonScope(traceName, path);
            return llvm::json::parse((*jsonBuf)->getBuffer());
        }();
        if (!parsed) return nullptr;

        CoreJsonCacheEntry entry;
        entry.coreHash = coreHash;
        entry.writeTime = writeTime;
        entry.fileSize = fileSize;
        entry.buffer = std::move(*jsonBuf);
        entry.value = std::move(*parsed);
        auto inserted = cache.insert_or_assign(path, std::move(entry));
        return &*inserted.first->second.value;
    };

    // Load and validate JSON metadata. Scope the read+parse separately from the
    // bitcode parse and the deserialization walk so -ftime-trace shows each slice.
    auto* parsed = loadJson(jsonPath, coreMetaJsonCache_, "CoreCacheJsonParse");
    if (!parsed) return false;
    auto* root = parsed->getAsObject();
    if (!root) return false;

    auto ver      = root->getInteger("version");
    auto storedPl = root->getString("platform");
    auto storedH  = root->getString("core_hash");
    if (!ver || *ver != 10) return false;  // v9 used the pre-dollar type and function mangling
    if (!storedPl || storedPl->str() != platform) return false;
    if (!storedH || storedH->str() != coreHash) return false;
    if (!std::filesystem::exists(symbolsPath)) return false;

    // Read the LSP symbol sidecar here, BEFORE the context swap below, and only when a consumer
    // will use it. Every failure exit in this function must precede that swap: returning false
    // afterwards reports a cache miss on an already-deserialized backend, and the caller then
    // re-imports core on top of it. The parse itself is ~68% of the metadata, hence the gate.
    llvm::json::Value* symbolsJson = nullptr;
    if (symbolSink_ != nullptr)
    {
        symbolsJson = loadJson(symbolsPath, coreSymbolsJsonCache_, "CoreCacheSymbolsParse");
        if (!symbolsJson || !symbolsJson->getAsObject()) return false;
    }

    // Load bitcode into a FRESH LLVMContext so named types (e.g. %string) from
    // RegisterBuiltinString don't conflict with the bitcode's versions.  After
    // loading we replace context/module/builder with the fresh ones.
    auto freshCtx = std::make_unique<llvm::LLVMContext>();
    auto bcBuf = llvm::MemoryBuffer::getFile(bcPath);
    if (!bcBuf) return false;
    auto parsedMod = [&] {
        llvm::TimeTraceScope bcScope("CoreCacheBitcodeParse", bcPath);
        return llvm::getLazyBitcodeModule((*bcBuf)->getMemBufferRef(), *freshCtx);
    }();
    if (!parsedMod)
    {
        llvm::consumeError(parsedMod.takeError());
        return false;
    }

    // Replace the current LLVM state.  Destroy builder/module before context.
    builder.reset();
    module.reset();
    coreBitcodeBuffer_.reset();
    context = std::move(freshCtx);
    module  = std::move(*parsedMod);
    coreBitcodeBuffer_ = std::move(*bcBuf);
    builder = std::make_unique<llvm::IRBuilder<>>(*context);

    // Clear tables populated by Init()/RegisterBuiltinString() whose LLVM pointers
    // now dangle (they pointed into the old module/context we just destroyed).
    functionTable.clear();
    cAbiFunctionThunkCache_.clear();
    dataStructures.clear();
    DropModuleEscapeMemo();   // every llvm::Function* just died with the old module
    interfaceTable.clear();
    interfaceFields.clear();
    interfaceParents.clear();
    interfaceDefSites.clear();
    rejectedInterfaceDefSites_.clear();
    rejectedInterfaceDefMessages_.clear();
    coreInterfaceDefs_.clear();
    scannedInterfaceImpls.clear();
    uncertainInterfaceImpls.clear();
    ifConstGuardedImpls_.clear();
    importCompileDepth_ = 0;   // a throw out of an import walk can leave the RAII depth stranded
    // Every Function* here died with the old module. Re-adopt any bodyless rebox thunk baked
    // into the cached bitcode so finalization still fills it in rather than tripping the verifier.
    deferredIfaceRebox_.clear();
    deferredIfaceReboxIndex_.clear();
    deferredIfaceReboxDrained_ = false;
    AdoptInterfaceReboxThunksFromModule();
    typeAnnotations_.clear();
    annotationRegistry.clear();
    globalNamedVariable.clear();
    globalVariableTypes.clear();
    globalDeclSite.clear();
    coreSymbolIndex_.Clear();
    manifestFragments_.clear();
    namespaceTable.clear();
    typeAliases.clear();
    manglingAliases_.clear();
    enumBackingTypes.clear();
    // strConcatRegistered / stringDtorRegistered: will be set below after deserialization
    // verifies the functions are present in the bitcode.

    // Restore symbol tables from JSON. Scope covers the whole deserialization walk
    // (lives until the function returns) so -ftime-trace separates it from the parses.
    llvm::TimeTraceScope desScope("CoreCacheDeserialize", jsonPath);

    // importedFiles - paths may be relative to runtimeDir (new format) or
    // absolute (old format). Resolve relative paths against current runtimeDir
    // so the dedup check works even when Debug and Release builds share a cache.
    { llvm::TimeTraceScope s("CoreDes:Metadata", jsonPath);
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

    // manglingAliases_ - pure-rename aliases the mangler folds (see the write side).
    if (auto* obj = root->getObject("mangling_aliases"))
        for (auto& kv : *obj)
            if (auto v = kv.second.getAsString()) manglingAliases_[kv.first.str()] = v->str();

    // enumBackingTypes
    if (auto* obj = root->getObject("enum_backing_types"))
        for (auto& kv : *obj)
            if (auto v = kv.second.getAsString()) enumBackingTypes[kv.first.str()] = v->str();

    // annotationRegistry - annotation name -> declared field names.
    if (auto* obj = root->getObject("annotation_decls"))
        for (auto& kv : *obj)
            if (auto* arr = kv.second.getAsArray())
            {
                std::vector<std::string> fields;
                for (auto& f : *arr)
                    if (auto v = f.getAsString()) fields.push_back(v->str());
                annotationRegistry[kv.first.str()] = std::move(fields);
            }

    // typeAnnotations_ - type/interface name -> annotations attached to it.
    if (auto* obj = root->getObject("type_annotations"))
        for (auto& kv : *obj)
            if (auto* arr = kv.second.getAsArray())
                typeAnnotations_[kv.first.str()] = DeserializeAnnotations(arr);

    } // end CoreDes:Metadata

    // functionTable
    { llvm::TimeTraceScope s("CoreDes:Functions", jsonPath);
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
            sym.SourceName = key->str();
            sym.Function   = module->getFunction(sym.UniqueName);
            if (!sym.Function) continue;
            sym.External = sym.Function->hasExternalLinkage();
            if (auto* r = fo->getObject("r")) sym.ReturnType = DeserializeTav(*r);
            if (auto* ps = fo->getArray("ps"))
                for (auto& pe : *ps)
                    if (auto* po = pe.getAsObject()) sym.Parameters.push_back(DeserializeTav(*po));
            if (auto v = fo->getBoolean("va")) sym.Variadic = *v;
            if (auto v = fo->getBoolean("ext")) sym.External = *v;
            if (auto v = fo->getBoolean("ro")) sym.ReturnsOwned = *v;
            if (auto v = fo->getBoolean("ra")) sym.ReturnsAlias = *v;
            if (auto v = fo->getBoolean("m"))  sym.IsMethod = *v;
            if (auto* rl = fo->getArray("rl"))
                for (auto& le : *rl)
                    if (auto v = le.getAsString()) sym.RequiredLocks.push_back(v->str());
            functionTable[key->str()].push_back(std::move(sym));
        }

    } // end CoreDes:Functions

    // dataStructures
    { llvm::TimeTraceScope s("CoreDes:Structs", jsonPath);
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

            if (auto* sifaces = so->getArray("static_interfaces"))
                for (auto& ie : *sifaces)
                    if (auto v = ie.getAsString()) sd.StaticInterfaces.push_back(v->str());

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

    } // end CoreDes:Structs

    // AbiRecipe contains LLVM type pointers and is recomputed after struct metadata is restored.
    for (auto& entry : functionTable)
        for (auto& sym : entry.second)
            if (sym.External)
            {
                auto recipe = ComputeAbiRecipe(sym.ReturnType, sym.Parameters);
                if (recipe.hasLowering) sym.Recipe = std::move(recipe);
            }

    // interfaceTable + interfaceParents
    { llvm::TimeTraceScope s("CoreDes:Interfaces", jsonPath);
    if (auto* arr = root->getArray("interfaces"))
        for (auto& elem : *arr)
        {
            auto* io = elem.getAsObject();
            if (!io) continue;
            auto name = io->getString("name");
            if (!name) continue;
            std::string iname = name->str();
            if (auto site = io->getString("defsite"))
                interfaceDefSites[iname] = site->str();
            if (auto isCore = io->getBoolean("defsite_is_core"); isCore && *isCore)
                coreInterfaceDefs_.insert(iname);
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

            // Interface FIELDS (byte-offset vtable slots). Parents' fields were already
            // flattened into the list at definition time, so restore it verbatim.
            std::vector<TAV> fields;
            if (auto* fs = io->getArray("fields"))
                for (auto& fe : *fs)
                    if (auto* fo = fe.getAsObject()) fields.push_back(DeserializeTav(*fo));
            interfaceFields[iname] = std::move(fields);
        }

    } // end CoreDes:Interfaces

    // globalNamedVariable + globalVariableTypes
    { llvm::TimeTraceScope s("CoreDes:Globals", jsonPath);
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

    } // end CoreDes:Globals

    // Restore symbol-sink entries only for LSP and symbol-query consumers. SetSymbolSink is
    // installed before Analyze() on both paths, so CLI cache loads skip this sidecar entirely.
    if (symbolsJson)
    {
        auto* symbolsRoot = symbolsJson->getAsObject();

        std::unordered_map<std::string, std::string> rejoinRuntimeMemo;
        auto rejoinRuntime = [&](llvm::StringRef file) -> std::string
        {
            std::string raw = file.str();
            if (auto cached = rejoinRuntimeMemo.find(raw); cached != rejoinRuntimeMemo.end())
                return cached->second;

            std::filesystem::path p(raw);
            std::string result = raw;
            if (p.is_relative())
                result = std::filesystem::weakly_canonical(
                    std::filesystem::path(runtimeDir) / p).string();
            return rejoinRuntimeMemo.emplace(std::move(raw), std::move(result)).first->second;
        };
        { llvm::TimeTraceScope s("CoreDes:Symbols", symbolsPath);
        if (auto* arr = symbolsRoot->getArray("core_symbols"))
            for (auto& elem : *arr)
            {
                auto* so = elem.getAsObject();
                if (!so) continue;
                auto name = so->getString("name");
                auto kind = so->getInteger("kind");
                auto file = so->getString("file");
                auto line = so->getInteger("line");
                auto column = so->getInteger("column");
                auto signature = so->getString("signature");
                if (!name || !kind || !file || !line || !column || !signature
                    || *kind < 0 || *kind > static_cast<int64_t>(SymbolKind::Variable))
                    continue;
                SymbolDef def;
                def.name = name->str();
                if (auto displayName = so->getString("display_name"))
                    def.displayName = displayName->str();
                def.kind = static_cast<SymbolKind>(*kind);
                def.file = rejoinRuntime(*file);
                def.line = static_cast<int>(*line);
                def.column = static_cast<int>(*column);
                def.signatureMarkdown = signature->str();
                if (auto doc = so->getString("doc")) def.docComment = doc->str();
                if (auto* members = so->getArray("members"))
                    for (auto& member : *members)
                        if (auto value = member.getAsString()) def.memberNames.push_back(value->str());
                if (auto* overloads = so->getArray("overloads"))
                    for (auto& overload : *overloads)
                        if (auto value = overload.getAsString()) def.overloadSignatures.push_back(value->str());
                coreSymbolIndex_.RegisterDefinition(def);
            }
        } // end CoreDes:Symbols

        { llvm::TimeTraceScope s("CoreDes:Variables", symbolsPath);
        if (auto* arr = symbolsRoot->getArray("core_variables"))
            for (auto& elem : *arr)
            {
                auto* vo = elem.getAsObject();
                if (!vo) continue;
                auto name = vo->getString("name");
                auto type = vo->getString("type");
                auto displayType = vo->getString("display_type");
                auto file = vo->getString("file");
                auto line = vo->getInteger("line");
                auto column = vo->getInteger("column");
                if (!name || !type || !file || !line || !column) continue;
                coreSymbolIndex_.RegisterVariable(name->str(), type->str(), rejoinRuntime(*file),
                                                   static_cast<int>(*line), static_cast<int>(*column),
                                                   displayType ? displayType->str() : std::string{});
            }
        } // end CoreDes:Variables
    }

    // Manifest fragments and their [JsonText] leaf records.
    { llvm::TimeTraceScope s("CoreDes:Manifest", jsonPath);
    if (auto* arr = root->getArray("manifest_fragments"))
        for (auto& elem : *arr)
        {
            auto* fo = elem.getAsObject();
            if (!fo) continue;
            auto source = fo->getString("source_file");
            auto xml = fo->getString("xml");
            if (!source || !xml) continue;
            ManifestFragment fragment;
            fragment.SourceFile = source->str();
            if (auto line = fo->getInteger("line")) fragment.Line = static_cast<size_t>(*line);
            fragment.Xml = xml->str();
            if (auto* leaves = fo->getArray("leaves"))
                for (auto& elem : *leaves)
                {
                    auto* lo = elem.getAsObject();
                    if (!lo) continue;
                    auto ns = lo->getString("namespace");
                    auto local = lo->getString("local_name");
                    auto text = lo->getString("text");
                    auto leafSource = lo->getString("source_file");
                    if (!ns || !local || !text || !leafSource) continue;
                    ManifestFragment::Leaf leaf;
                    leaf.Namespace = ns->str();
                    leaf.LocalName = local->str();
                    leaf.Text = text->str();
                    leaf.SourceFile = leafSource->str();
                    if (auto line = lo->getInteger("line")) leaf.Line = static_cast<size_t>(*line);
                    fragment.Leaves.push_back(std::move(leaf));
                }
            manifestFragments_.push_back(std::move(fragment));
        }
    } // end CoreDes:Manifest

    // Register generic templates lazily: store each template's source text plus a null context
    // placeholder. The ANTLR parse is deferred to MaterializeGeneric* on first instantiation,
    // so a compile pays the parse cost only for the generics it actually uses.
    { llvm::TimeTraceScope s("CoreDes:GenericRegister", jsonPath);
    auto registerLazyTemplates = [&](const char* key,
        auto& templateMap, auto& typeParamsMap, auto& valueParamsMap,
        auto& constraintsMap, auto& packIndexMap)
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
            std::string name = tname->str();

            std::vector<std::string> tps;
            if (auto* tpsArr = to->getArray("type_params"))
                for (auto& tp : *tpsArr)
                    if (auto v = tp.getAsString()) tps.push_back(v->str());

            templateMap[name] = nullptr;            // parsed on first use (MaterializeGeneric*)
            typeParamsMap[name] = std::move(tps);
            std::vector<std::string> vps;
            if (auto* vpsArr = to->getArray("value_param_types"))
                for (auto& vp : *vpsArr)
                    if (auto v = vp.getAsString()) vps.push_back(v->str());
            valueParamsMap[name] = std::move(vps);
            if (auto* cobj = to->getObject("constraints"))
                constraintsMap[name] = DeserializeConstraints(cobj);
            if (auto v = to->getInteger("pack_index"))
                packIndexMap[name] = static_cast<size_t>(*v);
            if (auto dns = to->getString("decl_ns"))
                gts.genericTemplateNamespace[name] = dns->str();
            if (auto isCore = to->getBoolean("core"); isCore && *isCore)
                gts.coreGenericTemplates.insert(name);
            gts.lazyTemplateSource[name] = tsource->str();
        }
    };

    registerLazyTemplates("generic_structs",
        gts.genericStructTemplates, gts.genericStructTypeParams,
        gts.genericStructValueParams,
        gts.genericStructConstraints, gts.genericStructPackIndex);

    registerLazyTemplates("generic_classes",
        gts.genericClassTemplates, gts.genericStructTypeParams,
        gts.genericStructValueParams,
        gts.genericClassConstraints, gts.genericClassPackIndex);

    // Interfaces have no constraints map; use a local dummy to satisfy the lambda signature.
    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::string>>> ifaceConstraintsDummy;
    registerLazyTemplates("generic_interfaces",
        gts.genericInterfaceTemplates, gts.genericInterfaceTypeParams,
        gts.genericInterfaceValueParams,
        ifaceConstraintsDummy, gts.genericInterfacePackIndex);

    registerLazyTemplates("generic_functions",
        gts.genericFunctionTemplates, gts.genericFunctionTypeParams,
        gts.genericFunctionValueParams,
        gts.genericFunctionConstraints, gts.genericFunctionPackIndex);

    } // end CoreDes:GenericRegister

    // Restore pre-instantiated generic sets.
    { llvm::TimeTraceScope s("CoreDes:Instantiated", jsonPath);
    if (auto* arr = root->getArray("instantiated_generics"))
        for (auto& elem : *arr)
            if (auto v = elem.getAsString()) gts.instantiatedGenerics.insert(v->str());
    if (auto* arr = root->getArray("instantiated_interfaces"))
        for (auto& elem : *arr)
            if (auto v = elem.getAsString()) gts.instantiatedInterfaces.insert(v->str());
    if (auto* arr = root->getArray("instantiated_generic_functions"))
        for (auto& elem : *arr)
            if (auto v = elem.getAsString()) gts.instantiatedGenericFunctions.insert(v->str());
    } // end CoreDes:Instantiated

    // Mark lazy-registered functions as done if they were deserialized from the cache.
    // This prevents EnsureStrConcatRegistered / EnsureStringDtorRegistered from creating
    // duplicate LLVM functions that conflict with the ones already in the loaded bitcode.
    if (!functionTable["__strconcat"].empty()) strConcatRegistered = true;
    if (dataStructures.count("string") && dataStructures["string"].Destructor) stringDtorRegistered = true;

    return true;
}

void LLVMBackend::MaterializeCoreIfLazy()
{
    if (!module || !coreBitcodeBuffer_ || module->isMaterialized()) return;
    if (auto error = module->materializeAll())
        LogError("failed to materialize cached core bitcode: " + llvm::toString(std::move(error)));
    coreBitcodeBuffer_.reset();
}

// Lazy materializers: parse a cached generic template's source on first instantiation.
// The extractor pulls the relevant definition context out of the freshly parsed unit;
// it matches the eager extractors that registerLazyTemplates replaced.

CFlatParser::StructDefinitionContext* LLVMBackend::MaterializeGenericStruct(const std::string& name)
{
    return MaterializeGenericTemplate(gts.genericStructTemplates, name,
        [](CFlatParser::CompilationUnitContext* cu) -> CFlatParser::StructDefinitionContext* {
            if (!cu || !cu->translationUnit()) return nullptr;
            for (auto* decl : cu->translationUnit()->externalDeclaration())
                if (auto* sd = decl->structDefinition())
                    if (sd->genericTypeParameters()) return sd;
            return nullptr;
        });
}

CFlatParser::ClassDefinitionContext* LLVMBackend::MaterializeGenericClass(const std::string& name)
{
    return MaterializeGenericTemplate(gts.genericClassTemplates, name,
        [](CFlatParser::CompilationUnitContext* cu) -> CFlatParser::ClassDefinitionContext* {
            if (!cu || !cu->translationUnit()) return nullptr;
            for (auto* decl : cu->translationUnit()->externalDeclaration())
                if (auto* cd = decl->classDefinition())
                    if (cd->genericTypeParameters()) return cd;
            return nullptr;
        });
}

CFlatParser::InterfaceDefinitionContext* LLVMBackend::MaterializeGenericInterface(const std::string& name)
{
    return MaterializeGenericTemplate(gts.genericInterfaceTemplates, name,
        [](CFlatParser::CompilationUnitContext* cu) -> CFlatParser::InterfaceDefinitionContext* {
            if (!cu || !cu->translationUnit()) return nullptr;
            for (auto* decl : cu->translationUnit()->externalDeclaration())
                if (auto* id = decl->interfaceDefinition())
                    if (id->genericIdentifier() && id->genericIdentifier()->genericTypeParameters())
                        return id;
            return nullptr;
        });
}

CFlatParser::FunctionDefinitionContext* LLVMBackend::MaterializeGenericFunction(const std::string& name)
{
    return MaterializeGenericTemplate(gts.genericFunctionTemplates, name,
        [](CFlatParser::CompilationUnitContext* cu) -> CFlatParser::FunctionDefinitionContext* {
            if (!cu || !cu->translationUnit()) return nullptr;
            for (auto* decl : cu->translationUnit()->externalDeclaration())
                if (auto* fd = decl->functionDefinition())
                    if (fd->genericTypeParameters()) return fd;
            return nullptr;
        });
}

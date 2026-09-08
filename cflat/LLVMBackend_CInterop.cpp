#pragma warning(push)
#pragma warning(disable: 4244 4267)
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/Dominators.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Demangle/Demangle.h>
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
#include <llvm/ADT/StringSet.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/TimeProfiler.h>
#include <llvm/Support/JSON.h>
#include <llvm/IR/DiagnosticInfo.h>
#include <llvm/IR/DiagnosticHandler.h>
#pragma warning(pop)
#include <antlr4-runtime.h>

#include "platform/GeneratedParser.h"
#include "LLVMBackend.h"
#include "MainListener.h"
#include "GrammarTreeListener.h"
#include <filesystem>
#include <optional>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>

#if defined(__APPLE__)
// Step 3 (macOS self-contained link): harvest libSystem's exported symbols from
// the live dyld shared cache to synthesize a linker stub, so -o needs no SDK.
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <dlfcn.h>
#include <cstring>
#include <sys/sysctl.h>
#endif

// ---- Definitions moved out of LLVMBackend.h (CInterop) ----

// Whitespace-insensitive key for a C++ type spelling: Clang and the C type mapper disagree only
// about spaces inside a template argument list.
// A libc++ / SDK C++ header is included by NAME (`#include <vector>`), so its own internal
// includes resolve through the driver's C++ search path; anything else by absolute path.
static bool IsSystemCxxHeaderPath(const std::string& path)
{
        std::string p = path;
        std::replace(p.begin(), p.end(), '\\', '/');
        if (p.find("/c++/v1/") != std::string::npos) return true;   // libc++
        // MSVC STL: <VS>/VC/Tools/MSVC/<version>/include/<header>.
        if (p.find("/VC/Tools/MSVC/") != std::string::npos) return true;
        // libstdc++: /usr/include/c++/14/, /usr/include/<triple>/c++/14/. The component after
        // "/c++/" is the release, so require it to start with a digit - that is what separates a
        // standard-library header from a user directory that happens to be named "c++".
        for (size_t pos = 0; (pos = p.find("/c++/", pos)) != std::string::npos; pos += 5)
        {
            const size_t comp = pos + 5;
            if (comp < p.size() && std::isdigit((unsigned char)p[comp])) return true;
        }
        return false;
}

static std::string CxxDefaultWrapperName(const std::string& linkageName, size_t omittedArity);
static bool HasNonConstDefaultSuffix(const std::vector<cflat_cinterop::RawDefaultArg>& defaults,
                                     size_t first);

static bool MergeCxxBitcode(const std::string& first, const std::string& second,
                            std::string& merged)
{
        llvm::LLVMContext context;
        auto left = llvm::parseBitcodeFile(llvm::MemoryBufferRef(first, "cflat-cxx-left"), context);
        auto right = llvm::parseBitcodeFile(llvm::MemoryBufferRef(second, "cflat-cxx-right"), context);
        if (!left || !right) return false;
        llvm::Linker linker(*left.get());
        if (linker.linkInModule(std::move(*right))) return false;
        llvm::raw_string_ostream stream(merged);
        llvm::WriteBitcodeToFile(*left.get(), stream);
        stream.flush();
        return true;
}

/*
 * Namespace names a C++ header spells out, collected by a token scan. Over-approximates on purpose
 * (a name inside a comment or a string counts): the set only ever admits a type REQUEST attempt, and
 * a namespace that contains nothing but class templates registers no decl the walk could seed it
 * from. A namespace opened by a macro (libc++'s _LIBCPP_BEGIN_NAMESPACE_STD) is not found here -
 * "std" is seeded from the system-header spelling instead.
 */
static void CollectHeaderNamespaceNames(const std::string& path,
                                        std::unordered_set<std::string>& out)
{
        std::ifstream in(path, std::ios::binary);
        if (!in) return;
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto identChar = [](char c) { return std::isalnum((unsigned char)c) != 0 || c == '_'; };
        static const std::string kw = "namespace";
        for (size_t pos = 0; (pos = text.find(kw, pos)) != std::string::npos; pos += kw.size())
        {
            if (pos > 0 && identChar(text[pos - 1])) continue;
            size_t i = pos + kw.size();
            if (i < text.size() && identChar(text[i])) continue;
            while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
            const size_t start = i;
            while (i < text.size() && identChar(text[i])) ++i;
            if (i > start) out.insert(text.substr(start, i - start));
        }
}

std::string LLVMBackend::SqueezeCxxSpelling(const std::string& spelling)
{
        std::string out;
        out.reserve(spelling.size());
        for (char c : spelling) if (c != ' ' && c != '\t') out += c;
        return out;
    }

void LLVMBackend::RejectThrowingCxxFunction(const FunctionSymbol& symbol, const std::string& displayName) const
{
        if (!symbol.IsCxx || symbol.IsNoexcept || !cppStrictNoexcept_) return;
        LogError(std::format("call to '{}' may throw - C++ exceptions are not supported yet; "
                             "declare it noexcept or wrap it in an extern \"C\" noexcept function",
                             displayName));
}

/*
 * By-value gate for a C++ declaration. A TRIVIALLY COPYABLE record crosses the boundary as raw
 * bytes under clang's own arrangement, so it is allowed. A NONTRIVIAL one (user copy/move
 * constructor, user destructor, virtuals, a nontrivial member) needs construction and destruction
 * at the call site - that is M4 - so it is refused here, at registration, and the LSP sees the
 * same answer as codegen.
 */
bool LLVMBackend::RejectCxxRecordByValue(const CSigEntry& sig)
{
        auto refuse = [&](const TypeAndValue& tv, bool isReturn) {
            // A `T&` return or parameter is `alias T`: it crosses by address, never by value.
            if (tv.IsAlias) return false;
            if (!IsByValueStructTV(tv)) return false;
            if (cxxTriviallyCopyableRecords_.count(tv.TypeName) != 0) return false;
            if (cxxRecords_.count(tv.TypeName) == 0) return false;  // not a C++ record: C rules apply
            if (isReturn && cxxNontrivialRecords_.count(tv.TypeName) != 0
                && HasBindableCxxDestructor(tv.TypeName))
                return false;
            // A NONTRIVIAL but non-polymorphic, base-less class does cross by value: clang's
            // arrangement makes it Indirect-without-byval, the CALLER owns the temp, and the
            // call site copy- or move-CONSTRUCTS it and destroys it after the call (M4b).
            // The special members that crossing needs must be BINDABLE though: an implicit or
            // inline-only copy constructor / destructor has no symbol to call, so such a record
            // is still refused here rather than at a call site that could not fix it.
            if (cxxNontrivialRecords_.count(tv.TypeName) != 0
                && HasBindableCxxDestructor(tv.TypeName)
                && (isReturn || FindCxxCopyCtor(tv.TypeName) != nullptr
                    || FindCxxMoveCtor(tv.TypeName) != nullptr))
                return false;
            // Refuse THIS signature only, with the reason replayed if CFlat ever calls it. A
            // library header carries many such helpers the program never names.
            std::string reason = std::format(
                "C++ function '{}' takes or returns nontrivial type '{}' by value; a record "
                "passed by value must be trivially copyable (no user copy/move constructor, "
                "destructor, or virtuals) - pass it by pointer or reference instead",
                sig.name, tv.TypeName);
            if (verbose) std::cout << "[verbose]   refused: " << reason << "\n";
            cxxBindingRefusals_.emplace(sig.name, std::move(reason));
            return true;
        };
        if (refuse(sig.ret, true)) return true;
        for (const TypeAndValue& p : sig.params)
            if (refuse(p, false)) return true;
        return false;
    }

void LLVMBackend::CheckPoisonedFunctionCalls()
{
        for (const auto& [name, msg] : poisonedFunctions)
        {
            llvm::Function* f = module->getFunction(name);
            if (f == nullptr) continue;
            // A poisoned function is one this compile emitted, so every call to it is
            // materialized; users() would assert on a lazily loaded core module (--check).
            for (auto* u : f->materialized_users())
            {
                if (llvm::isa<llvm::CallBase>(u))
                {
                    // Point the diagnostic at the call site rather than wherever the walk ended.
                    auto locIt = firstCallLocation_.find(name);
                    if (locIt != firstCallLocation_.end())
                        SetSourceLocation(locIt->second.first, locIt->second.second);
                    LogRawError(msg);
                    break;
                }
            }
        }
    }

bool LLVMBackend::VerifyModule()
{
        std::string errors;
        llvm::raw_string_ostream errorStream(errors);
        if (cachedFunctionNames_)
        {
            // Cached module-level artifacts are not checked here; cold compiles still use verifyModule.
            for (auto& function : module->functions())
            {
                if (function.isDeclaration()
                    || cachedFunctionNames_->contains(function.getName().str()))
                    continue;
                if (llvm::verifyFunction(function, &errorStream))
                {
                    std::cout << std::format("Module verification failed:\n{}\n", errorStream.str());
                    return false;
                }
            }
            return true;
        }
        if (llvm::verifyModule(*module, &errorStream))
        {
            std::cout << std::format("Module verification failed:\n{}\n", errorStream.str());
            return false;
        }
        return true;
    }

llvm::TargetLibraryInfoImpl LLVMBackend::MakeStdioSafeTLII(const llvm::Triple& triple) const
{
        llvm::TargetLibraryInfoImpl tlii{ triple };
        /*
         * core/cruntime.cb DEFINES these with cflat semantics, not the C library's: every write
         * goes through __stdout_write, which honors a `program`'s stdout hook and keeps VT escape
         * sequences intact. Once printf carries C's exact `int printf(const char*, ...)` signature,
         * SimplifyLibCalls recognizes it and rewrites printf("l1\n") into puts("l1") - two hook
         * chunks instead of one, and a no-newline literal into fwrite(), which bypasses the hook
         * entirely. Tell the optimizer these names are not the C library functions.
         */
        for (llvm::LibFunc fn : { llvm::LibFunc_printf, llvm::LibFunc_vprintf, llvm::LibFunc_puts,
                                  llvm::LibFunc_putchar, llvm::LibFunc_fputs, llvm::LibFunc_fputc,
                                  llvm::LibFunc_fprintf, llvm::LibFunc_sprintf,
                                  llvm::LibFunc_snprintf, llvm::LibFunc_vsprintf })
            tlii.setUnavailable(fn);
        if (!targetWindows_)
        {
            tlii.setUnavailable(llvm::LibFunc_vsnprintf);
            tlii.setUnavailable(llvm::LibFunc_vfprintf);
            tlii.setUnavailable(llvm::LibFunc_vsscanf);
            tlii.setUnavailable(llvm::LibFunc_vfscanf);
        }
        return tlii;
    }

bool LLVMBackend::SaveToFile(const std::string& filename)
{
        MaterializeCoreIfLazy();
        std::error_code errorCode;
        llvm::raw_fd_ostream outLL(filename, errorCode);
        if (errorCode)
        {
            std::cout << std::format("Error: could not write IR to '{}': {}\n", filename, errorCode.message());
            return false;
        }
        module->print(outLL, nullptr);
        return true;
    }

bool LLVMBackend::WriteBitcode(const std::string& filename)
{
        MaterializeCoreIfLazy();
        std::error_code errorCode;
        llvm::raw_fd_ostream outBC(filename, errorCode, llvm::sys::fs::OF_None);
        if (errorCode)
        {
            std::cout << std::format("Error: could not write bitcode to '{}': {}\n", filename, errorCode.message());
            return false;
        }
        llvm::WriteBitcodeToFile(*module, outBC);
        return true;
    }

std::string LLVMBackend::FindClangCl() const
{
        if (!runtimeDir.empty())
        {
            llvm::SmallString<256> candidate(runtimeDir);
            llvm::sys::path::append(candidate, "clang-cl.exe");
            if (llvm::sys::fs::exists(candidate))
                return candidate.str().str();
        }
        if (auto p = llvm::sys::findProgramByName("clang-cl"))
            return *p;
        return "";
    }

#if defined(__APPLE__)
std::string LLVMBackend::FindBundledLd64Lld() const
{
        if (!runtimeDir.empty())
        {
            llvm::SmallString<256> cand(runtimeDir);
            llvm::sys::path::append(cand, "ld64.lld");
            if (llvm::sys::fs::exists(cand)) return cand.str().str();
        }
        if (auto p = llvm::sys::findProgramByName("ld64.lld")) return *p;
        return "";
    }
#endif

std::string LLVMBackend::FindCDriver() const
{
        for (const char* cand : { "clang", "clang-18", "cc", "gcc" })
            if (auto p = llvm::sys::findProgramByName(cand)) return *p;
        return "";
}

std::string LLVMBackend::FindCxxDriver() const
{
        for (const char* cand : { "clang++", "clang++-23", "c++", "g++" })
            if (auto p = llvm::sys::findProgramByName(cand)) return *p;
        return "";
}

bool LLVMBackend::CompileCFileElf(const std::string& cSourcePath, const std::string& programAlias,
                                  bool cxxMode)
{
        const std::string cc = cxxMode ? FindCxxDriver() : FindCDriver();
        if (cc.empty())
        {
            LogErrorMessage("no {} compiler driver found - cannot compile native source '{}'.",
                            { cxxMode ? "C++" : "C", cSourcePath });
            return false;
        }

        llvm::SmallString<256> objFile;
        if (auto ec = llvm::sys::fs::createTemporaryFile("cflat_c", "o", objFile))
        {
            LogRawError(std::format("could not create temp object for C source '{}': {}", cSourcePath, ec.message()));
            return false;
        }
        std::string objPath = objFile.str().str();

        // -fPIC so the object links into the position-independent image the ELF path emits.
        std::vector<std::string> argStrs = { cc, "-c", "-fPIC", cSourcePath, "-o", objPath };
        // Match the deployment target EmitExecutableMachO links against. Without it the host
        // clang stamps its own (newer) minos and ld64 warns on every C-interop link.
        if (targetMacOS_)
        {
            argStrs.push_back("-target");
            argStrs.push_back("arm64-apple-macosx11.0.0");
            if (cxxMode)
            {
                argStrs.push_back("-stdlib=libc++");
                argStrs.push_back("-std=" + cppStandard_);
                std::string sdk = MacSdkPathCached();
                if (!sdk.empty()) { argStrs.push_back("-isysroot"); argStrs.push_back(sdk); }
            }
        }
        else if (cxxMode)
            argStrs.push_back("-std=" + cppStandard_);
        if (cOptLevel_ >= 2)      argStrs.push_back("-O2");
        else if (cOptLevel_ == 1) argStrs.push_back("-O1");
        if (cDebugInfo_)          argStrs.push_back("-g");
        if (!targetCpu_.empty()) argStrs.push_back("-march=" + targetCpu_);
        if (!tuneCpu_.empty())   argStrs.push_back("-mtune=" + tuneCpu_);
        for (const auto& def : cDefines_) argStrs.push_back("-D" + def);
        if (!programAlias.empty())
            argStrs.push_back("-Dmain=__imported_main_" + programAlias);

        std::vector<llvm::StringRef> args;
        for (auto& s : argStrs) args.push_back(s);

        if (verbose)
        {
            std::cout << std::format("[verbose] compiling {} source: {} -> {}\n",
                                     cxxMode ? "C++" : "C", cSourcePath, objPath);
            std::cout << "[verbose]   " << cc;
            for (size_t i = 1; i < argStrs.size(); ++i) std::cout << " " << argStrs[i];
            std::cout << "\n";
        }

        std::string compileErr;
        int rc;
        {
            llvm::TimeTraceScope spawnScope("ClangCompileC", cSourcePath);
            rc = llvm::sys::ExecuteAndWait(cc, args, std::nullopt, {}, 0, 0, &compileErr);
        }
        if (rc != 0)
        {
            llvm::sys::fs::remove(objPath);
            LogRawError(std::format("C compiler failed to compile C source '{}' (exit {}){}{}",
                cSourcePath, rc, compileErr.empty() ? "" : ": ", compileErr));
            return false;
        }

        cObjectFiles_.push_back(objPath);
        return true;
    }

bool LLVMBackend::CompileCFile(const std::string& cSourcePath, const std::string& programAlias,
                               bool cxxMode)
{
        RecordDependency(cSourcePath);
        if (cxxMode) cppInteropUsed_ = true;
        // Auto-discover C function signatures so the importing .cb needs no hand-written extern declarations.
        // When programAlias is set, registers C `main` as `__imported_main_<Alias>` in programTable.
        ExtractCSignatures(cSourcePath, programAlias, cxxMode);

        if (symbolSink_ != nullptr)
            return true;

        // Non-Windows targets compile to an ELF object with a GCC-style driver and link
        // via EmitExecutableElf; clang-cl + MSVC flags only apply to the COFF path.
        if (!targetWindows_)
            return CompileCFileElf(cSourcePath, programAlias, cxxMode);

        const std::string clangPath = FindClangCl();
        if (clangPath.empty())
        {
            LogErrorMessage("clang-cl.exe not found - cannot compile C source '{}'.", { cSourcePath });
            return false;
        }

        // Temp object next to the system temp dir; removed after linking.
        llvm::SmallString<256> objFile;
        if (auto ec = llvm::sys::fs::createTemporaryFile("cflat_c", "obj", objFile))
        {
            LogRawError(std::format("could not create temp object for C source '{}': {}", cSourcePath, ec.message()));
            return false;
        }
        std::string objPath = objFile.str().str();

        const std::string target = (platformValue == 32)
            ? "--target=i686-pc-windows-msvc"
            : "--target=x86_64-pc-windows-msvc";
        const std::string foArg = "/Fo" + objPath;

        // /MD (dynamic UCRT) so this object's CRT /defaultlib directives are the dynamic set,
        // not clang-cl's /MT default (libcmt) which the freestanding link cannot satisfy. Covers
        // both user .c interop and the imported diagnostic/heap_audit.c.
        std::vector<std::string> argStrs = { clangPath, "/c", "/MD", "/nologo", target, cSourcePath, foArg };
        if (cxxMode) argStrs.push_back("/std:c++20");
        // cflat's own bundled runtime .c files (e.g. diagnostic/heap_audit.c) are compiled
        // freestanding like crashdump.c/cflat_builtins.c: /GS- so they emit no __security_check_
        // cookie reference (that symbol lives in msvcrt.lib, which the freestanding link drops).
        // User .c interop keeps default /GS - its hardening is the user's call.
        if (!runtimeDir.empty())
        {
            std::error_code pec;
            auto canonSrc  = std::filesystem::weakly_canonical(cSourcePath, pec);
            auto canonCore = std::filesystem::weakly_canonical(std::filesystem::path(runtimeDir) / "core", pec);
            if (!pec)
            {
                std::string s = canonSrc.string(), c = canonCore.string();
                if (s.size() >= c.size() && _strnicmp(s.c_str(), c.c_str(), c.size()) == 0)
                    argStrs.push_back("/GS-");
            }
        }
        if (cOptLevel_ >= 2)      argStrs.push_back("/O2");
        else if (cOptLevel_ == 1) argStrs.push_back("/O1");
        if (cDebugInfo_)          argStrs.push_back("/Z7"); // CodeView in the obj -> PDB via /DEBUG
        if (!targetCpu_.empty()) argStrs.push_back("/clang:-march=" + targetCpu_);
        if (!tuneCpu_.empty())   argStrs.push_back("/clang:-mtune=" + tuneCpu_);
        for (const auto& def : cDefines_) argStrs.push_back("/D" + def);
        if (!programAlias.empty())
            argStrs.push_back("/Dmain=__imported_main_" + programAlias);

        std::vector<llvm::StringRef> args;
        for (auto& s : argStrs) args.push_back(s);

        if (verbose)
        {
            std::cout << std::format("[verbose] compiling C source: {} -> {}\n", cSourcePath, objPath);
            std::cout << "[verbose]   clang-cl";
            for (size_t i = 1; i < argStrs.size(); ++i) std::cout << " " << argStrs[i];
            std::cout << "\n";
        }

        std::string clangCompileErr;
        int rc;
        {
            llvm::TimeTraceScope spawnScope("ClangCompileC", cSourcePath);
            rc = llvm::sys::ExecuteAndWait(clangPath, args, std::nullopt, {}, 0, 0, &clangCompileErr);
        }
        if (rc != 0)
        {
            llvm::sys::fs::remove(objPath);
            LogRawError(std::format("clang-cl failed to compile C source '{}' (exit {}){}{}",
                cSourcePath, rc, clangCompileErr.empty() ? "" : ": ", clangCompileErr));
            return false;
        }

        cObjectFiles_.push_back(objPath);
        return true;
    }

bool LLVMBackend::CompileCrashHandlerObject(const std::string& arch)
{
        if (runtimeDir.empty())
        {
            LogErrorMessage("cannot locate crash handler source: runtime directory is unset.");
            return false;
        }

        llvm::SmallString<256> srcPath(runtimeDir);
        llvm::sys::path::append(srcPath, "core", "diagnostic", "crashdump.c");
        if (!llvm::sys::fs::exists(srcPath))
        {
            LogErrorMessage("crash handler source not found: '{}'.", { srcPath.str().str() });
            return false;
        }

        const std::string clangPath = FindClangCl();
        if (clangPath.empty())
        {
            LogErrorMessage("clang-cl.exe not found - cannot compile crash handler.");
            return false;
        }

        llvm::SmallString<256> objFile;
        if (auto ec = llvm::sys::fs::createTemporaryFile("cflat_crashdump", "obj", objFile))
        {
            LogRawError(std::format("could not create temp object for crash handler: {}", ec.message()));
            return false;
        }
        std::string objPath = objFile.str().str();

        const std::string target = (arch == "x86")
            ? "--target=i686-pc-windows-msvc"
            : "--target=x86_64-pc-windows-msvc";
        const std::string foArg = "/Fo" + objPath;

        // /Z7 puts CodeView in the object so the handler's own frames are symbolizable too.
        // /MD selects the dynamic-CRT /defaultlib directives (suppressed at link time) instead
        // of clang-cl's /MT default (libcmt), which the freestanding link cannot satisfy. /GS-
        // so the buffers here emit no __security_check_cookie reference: that symbol comes from
        // msvcrt.lib, which the freestanding (non-asan) link drops. See Phase A.
        std::vector<std::string> argStrs = {
            clangPath, "/c", "/Z7", "/MD", "/GS-", "/nologo", target, srcPath.str().str(), foArg
        };

        std::vector<llvm::StringRef> args;
        for (auto& s : argStrs) args.push_back(s);

        if (verbose)
        {
            std::cout << std::format("[verbose] compiling crash handler: {} -> {}\n", srcPath.str().str(), objPath);
            std::cout << "[verbose]   clang-cl";
            for (size_t i = 1; i < argStrs.size(); ++i) std::cout << " " << argStrs[i];
            std::cout << "\n";
        }

        std::string clangCompileErr;
        int rc = llvm::sys::ExecuteAndWait(clangPath, args, std::nullopt, {}, 0, 0, &clangCompileErr);
        if (rc != 0)
        {
            llvm::sys::fs::remove(objPath);
            LogRawError(std::format("clang-cl failed to compile crash handler (exit {}){}{}",
                rc, clangCompileErr.empty() ? "" : ": ", clangCompileErr));
            return false;
        }

        cObjectFiles_.push_back(objPath);
        return true;
    }

bool LLVMBackend::CompileBuiltinsObject(const std::string& arch)
{
        if (runtimeDir.empty())
        {
            LogErrorMessage("cannot locate cflat_builtins.c: runtime directory is unset.");
            return false;
        }

        llvm::SmallString<256> srcPath(runtimeDir);
        llvm::sys::path::append(srcPath, "core", "cflat_builtins.c");
        if (!llvm::sys::fs::exists(srcPath))
        {
            LogErrorMessage("builtins source not found: '{}'.", { srcPath.str().str() });
            return false;
        }

        const std::string clangPath = FindClangCl();
        if (clangPath.empty())
        {
            LogErrorMessage("clang-cl.exe not found - cannot compile cflat_builtins.c.");
            return false;
        }

        llvm::SmallString<256> objFile;
        if (auto ec = llvm::sys::fs::createTemporaryFile("cflat_builtins", "obj", objFile))
        {
            LogRawError(std::format("could not create temp object for builtins: {}", ec.message()));
            return false;
        }
        std::string objPath = objFile.str().str();

        const std::string target = (arch == "x86")
            ? "--target=i686-pc-windows-msvc"
            : "--target=x86_64-pc-windows-msvc";
        const std::string foArg = "/Fo" + objPath;

        // /MD so the object's CRT /defaultlib directives are the dynamic set (msvcrt/vcruntime/
        // oldnames - all suppressed at link time) rather than clang-cl's /MT default (libcmt),
        // which the freestanding link cannot satisfy. /GS- so cflat_start and friends emit no
        // __security_check_cookie reference; that symbol lives in msvcrt.lib, which this object's
        // whole point is to let us drop.
        std::vector<std::string> argStrs = {
            clangPath, "/c", "/O2", "/MD", "/GS-", "/nologo", "/clang:-fno-builtin",
            target, srcPath.str().str(), foArg
        };

        std::vector<llvm::StringRef> args;
        for (auto& s : argStrs) args.push_back(s);

        if (verbose)
        {
            std::cout << std::format("[verbose] compiling builtins: {} -> {}\n", srcPath.str().str(), objPath);
            std::cout << "[verbose]   clang-cl";
            for (size_t i = 1; i < argStrs.size(); ++i) std::cout << " " << argStrs[i];
            std::cout << "\n";
        }

        std::string clangCompileErr;
        int rc = llvm::sys::ExecuteAndWait(clangPath, args, std::nullopt, {}, 0, 0, &clangCompileErr);
        if (rc != 0)
        {
            llvm::sys::fs::remove(objPath);
            LogRawError(std::format("clang-cl failed to compile cflat_builtins.c (exit {}){}{}",
                rc, clangCompileErr.empty() ? "" : ": ", clangCompileErr));
            return false;
        }

        cObjectFiles_.push_back(objPath);
        return true;
    }

bool LLVMBackend::VcRuntimeInstalled()
{
        char buf[260] = {};
        size_t len = 0;
        if (getenv_s(&len, buf, sizeof(buf), "SystemRoot") != 0 || len == 0)
            return true;
        std::filesystem::path root(buf);
        for (const char* sub : { "System32", "SysWOW64" })
            if (std::filesystem::exists(root / sub / "vcruntime140.dll"))
                return true;
        return false;
    }

bool LLVMBackend::MapCTypeToTypeAndValue(std::string ctype, TypeAndValue& out)
{
        std::unordered_set<std::string> visited;
        return MapCTypeToTypeAndValueImpl(std::move(ctype), out, visited);
}

void LLVMBackend::SetCInteropTargetFacts(const cflat_cinterop::ExtractResult& raw)
{
        SetCInteropTargetFacts(raw.longDoubleWidth, raw.longDoubleIsIEEEDouble,
                               raw.targetTriple);
}

void LLVMBackend::SetCInteropTargetFacts(uint64_t longDoubleWidth, bool longDoubleIsIEEEDouble,
                                         const std::string& targetTriple)
{
        cInteropLongDoubleWidth_ = longDoubleWidth;
        cInteropLongDoubleIsIEEEDouble_ = longDoubleIsIEEEDouble;
        cInteropTargetTriple_ = targetTriple.empty() ? CInteropTargetTriple() : targetTriple;
}

bool LLVMBackend::IsCInteropLongDoubleSupported() const
{
        return cInteropLongDoubleWidth_ == 64 && cInteropLongDoubleIsIEEEDouble_;
}

std::string LLVMBackend::CInteropLongDoubleRefusal() const
{
        return std::format("'long double' is {} bits on {}; CFlat has no matching type",
                           cInteropLongDoubleWidth_,
                           cInteropTargetTriple_.empty() ? CInteropTargetTriple()
                                                         : cInteropTargetTriple_);
}

bool LLVMBackend::IsLongDoubleSpelling(const std::string& spelling)
{
        return spelling.find("long double") != std::string::npos;
}

bool LLVMBackend::ParseCFunctionPointerSpelling(const std::string& s, TypeAndValue& out,
                                       std::unordered_set<std::string>& visited)
{
        // Locate "(*)" possibly with whitespace around the star.
        size_t markerPos = std::string::npos;
        for (size_t i = 0; i + 2 < s.size(); ++i)
        {
            if (s[i] != '(') continue;
            size_t j = i + 1;
            while (j < s.size() && std::isspace((unsigned char)s[j])) ++j;
            if (j >= s.size() || s[j] != '*') continue;
            ++j;
            while (j < s.size() && std::isspace((unsigned char)s[j])) ++j;
            for (;;)
            {
                bool removed = false;
                for (const char* qualifier : { "const", "volatile", "restrict", "__restrict",
                                               "__restrict__", "_Nonnull", "_Nullable",
                                               "_Null_unspecified" })
                {
                    size_t len = std::strlen(qualifier);
                    if (s.compare(j, len, qualifier) == 0)
                    {
                        j += len;
                        while (j < s.size() && std::isspace((unsigned char)s[j])) ++j;
                        removed = true;
                        break;
                    }
                }
                if (!removed) break;
            }
            if (j < s.size() && s[j] == ')') { markerPos = i; break; }
        }
        if (markerPos == std::string::npos) return false;

        std::string retSpelling = s.substr(0, markerPos);
        while (!retSpelling.empty() && std::isspace((unsigned char)retSpelling.back()))
            retSpelling.pop_back();

        // After "(*)" the next non-space char must be '('.
        size_t after = s.find(')', markerPos) + 1;
        while (after < s.size() && std::isspace((unsigned char)s[after])) ++after;
        if (after >= s.size() || s[after] != '(') return false;
        size_t argOpen = after;
        size_t argClose = std::string::npos;
        int depth = 0;
        for (size_t i = argOpen; i < s.size(); ++i)
        {
            if (s[i] == '(') ++depth;
            else if (s[i] == ')') { --depth; if (depth == 0) { argClose = i; break; } }
        }
        if (argClose == std::string::npos) return false;

        std::string argList = s.substr(argOpen + 1, argClose - argOpen - 1);

        // Resolve the return type via the same recursive resolver.
        TypeAndValue retTV;
        if (!MapCTypeToTypeAndValueImpl(retSpelling, retTV, visited)) return false;
        // Function pointers returning function pointers are not supported here.
        if (retTV.IsFunctionPointer) return false;

        out = TypeAndValue();
        out.IsFunctionPointer = true;
        out.TypeName = "__c_fn_ptr";       // thin: a C function pointer is the thin `function<T>`
        out.FuncPtrReturnTypeName = retTV.TypeName;
        out.FuncPtrReturnPointer = retTV.Pointer;

        // Split argList on top-level commas. Bail on nested fn-ptr arg or variadic.
        if (argList.find("...") != std::string::npos) return false;

        auto trim = [](std::string v) {
            size_t a = 0; while (a < v.size() && std::isspace((unsigned char)v[a])) ++a;
            size_t b = v.size(); while (b > a && std::isspace((unsigned char)v[b-1])) --b;
            return v.substr(a, b - a);
        };

        // Empty arg list or "void" -> zero params.
        std::string normArgs = trim(argList);
        if (normArgs.empty() || normArgs == "void")
            return true;

        std::vector<std::string> parts;
        {
            int d = 0;
            std::string cur;
            for (char c : argList)
            {
                if (c == '(') { ++d; cur += c; }
                else if (c == ')') { --d; cur += c; }
                else if (c == ',' && d == 0) { parts.push_back(cur); cur.clear(); }
                else cur += c;
            }
            if (!cur.empty()) parts.push_back(cur);
        }
        for (auto& p : parts)
        {
            TypeAndValue ptv;
            if (!MapCTypeToTypeAndValueImpl(trim(p), ptv, visited)) return false;
            if (ptv.IsFunctionPointer) return false; // nested fn-ptr arg not supported
            TypeAndValue::FuncPtrParam fp;
            fp.TypeName = ptv.TypeName;
            fp.Pointer = ptv.Pointer;
            fp.PointerDepth = ptv.ValuePointerDepth();
            fp.IsRvalueRef = ptv.IsRvalueRef;
            out.FuncPtrParams.push_back(fp);
        }
        return true;
    }

std::string LLVMBackend::StripFixedArrayDims(const std::string& ctype, std::vector<uint64_t>& dims)
{
        std::string elem;
        size_t i = 0;
        while (i < ctype.size())
        {
            if (ctype[i] == '[')
            {
                size_t close = ctype.find(']', i);
                if (close == std::string::npos) { elem += ctype.substr(i); break; }
                std::string inner = ctype.substr(i + 1, close - i - 1);
                size_t a = inner.find_first_not_of(" \t");
                size_t b = inner.find_last_not_of(" \t");
                std::string num = (a == std::string::npos) ? std::string{} : inner.substr(a, b - a + 1);
                bool allDigits = !num.empty() &&
                    std::all_of(num.begin(), num.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
                if (allDigits)
                {
                    dims.push_back(std::strtoull(num.c_str(), nullptr, 10));
                    i = close + 1;
                    continue;
                }
                elem += ctype.substr(i, close - i + 1);  // keep non-numeric extent verbatim
                i = close + 1;
                continue;
            }
            elem += ctype[i++];
        }
        while (!elem.empty() && (elem.back() == ' ' || elem.back() == '\t')) elem.pop_back();
        return elem;
    }

/*
 * Erase every occurrence of a declarator keyword, matched as a WHOLE TOKEN. A plain find/erase
 * loop also eats the keyword out of an identifier - "constant" becomes "ant", "classBase"
 * becomes "Base" - which loses the type's identity and silently decays the parameter to void*.
 * A word that does not start/end with an identifier character (e.g. "&") has no boundary to
 * check on that side, so it is erased wherever it appears.
 */
static void EraseDeclaratorToken(std::string& s, const char* word)
{
        const size_t n = std::strlen(word);
        if (n == 0) return;
        auto ident = [](char c) { return std::isalnum((unsigned char)c) != 0 || c == '_'; };
        const bool headIdent = ident(word[0]);
        const bool tailIdent = ident(word[n - 1]);
        for (size_t pos = 0; (pos = s.find(word, pos)) != std::string::npos; )
        {
            const bool okBefore = !headIdent || pos == 0 || !ident(s[pos - 1]);
            const bool okAfter  = !tailIdent || pos + n >= s.size() || !ident(s[pos + n]);
            if (okBefore && okAfter) s.erase(pos, n);
            else                     pos += n;
        }
    }

/*
 * The CFlat dotted name of the class a C++ pointer spelling points at, or "" when the spelling is
 * not a pointer to a class. Clang spells a C++ class type as a bare qualified name
 * ("const cpppoly::Right *"), with no "struct"/"union" keyword, so AggregatePointeeTag - which
 * keys on that keyword - never recognizes one.
 */
std::string LLVMBackend::CxxRecordPointeeTag(const std::string& spelling, int& outPtr)
{
        outPtr = 0;
        std::string s = spelling;
        if (s.find('(') != std::string::npos || s.find('[') != std::string::npos)
            return std::string();   // function pointer / array: not a plain record pointer
        for (const char* w : { "const", "volatile", "restrict", "__restrict", "__restrict__",
                               "struct", "class", "union", "&" })
            EraseDeclaratorToken(s, w);
        outPtr = (int)std::count(s.begin(), s.end(), '*');
        if (outPtr == 0) return std::string();
        s.erase(std::remove(s.begin(), s.end(), '*'), s.end());
        size_t a = s.find_first_not_of(" \t");
        size_t b = s.find_last_not_of(" \t");
        if (a == std::string::npos) return std::string();
        s = s.substr(a, b - a + 1);
        if (s.find(' ') != std::string::npos) return std::string();   // a multi-word builtin type
        // "ns::Class" is the same type CFlat registered as "ns.Class".
        for (size_t pos; (pos = s.find("::")) != std::string::npos; ) s.replace(pos, 2, ".");
        return s;
    }

std::string LLVMBackend::AggregatePointeeTag(const std::string& spelling, int& outPtr)
{
        outPtr = 0;
        std::string s = spelling;
        for (const char* w : { "const", "volatile", "restrict", "__restrict", "__restrict__",
                               "_Nonnull", "_Nullable", "_Null_unspecified" })
            EraseDeclaratorToken(s, w);
        outPtr = (int)std::count(s.begin(), s.end(), '*');
        if (outPtr == 0) return std::string();
        s.erase(std::remove(s.begin(), s.end(), '*'), s.end());
        size_t a = s.find_first_not_of(" \t");
        size_t b = s.find_last_not_of(" \t");
        if (a == std::string::npos) return std::string();
        s = s.substr(a, b - a + 1);
        if (s.rfind("struct ", 0) == 0) return s.substr(7);
        if (s.rfind("union ", 0) == 0)  return s.substr(6);
        return std::string();
    }

/*
 * A requested foreign C++ type (M5b), looked up on the INTACT spelling. The keys are Clang's
 * canonical spelling squeezed but otherwise verbatim, so an inner pointer or an inner "const" is
 * part of the key: only the OUTERMOST declarator may be peeled here. Counting and deleting every
 * '*' in the whole spelling first - as the general C mapper does - turns
 * "vector<char *, allocator<char *>>" into "vector<char, allocator<char>>", which matches nothing
 * and silently decays the parameter to void**.
 * Returns true when the spelling names a registered foreign type; `mapped` then says whether the
 * pointer depth is representable.
 */
bool LLVMBackend::TryMapCxxForeignSpelling(const std::string& ctype, TypeAndValue& out,
                                           bool& mapped) const
{
        mapped = false;
        if (cxxForeignTypeSpellings_.empty()) return false;
        if (ctype.find('(') != std::string::npos) return false;   // function pointer / function type
        std::string s = ctype;
        int ptr = 0;
        // A fixed array decays to a pointer to its element, exactly as the general mapper does.
        if (auto br = s.find('['); br != std::string::npos) { s.erase(br); ++ptr; }

        auto identChar = [](char c) { return std::isalnum((unsigned char)c) != 0 || c == '_'; };
        static const char* const cvWords[] = { "const", "volatile", "restrict", "__restrict",
                                               "__restrict__", "_Nonnull", "_Nullable",
                                               "_Null_unspecified" };
        // Peel the outer declarator right to left: '*', '&'/'&&' and trailing cv words only.
        for (bool peeled = true; peeled; )
        {
            peeled = false;
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
            if (s.empty()) return false;
            if (s.back() == '*') { s.pop_back(); ++ptr; peeled = true; continue; }
            if (s.back() == '&')
            {
                s.pop_back();
                if (!s.empty() && s.back() == '&') s.pop_back();
                ++ptr; peeled = true; continue;
            }
            for (const char* w : cvWords)
            {
                const size_t n = std::strlen(w);
                if (s.size() < n || s.compare(s.size() - n, n, w) != 0) continue;
                if (s.size() > n && identChar(s[s.size() - n - 1])) continue;
                s.erase(s.size() - n);
                peeled = true;
                break;
            }
        }
        // Leading whitespace, cv and elaborated-type keywords, outer level only.
        for (bool peeled = true; peeled; )
        {
            peeled = false;
            size_t a = s.find_first_not_of(" \t");
            if (a == std::string::npos) return false;
            if (a > 0) { s.erase(0, a); peeled = true; }
            for (const char* w : { "const ", "volatile ", "class ", "struct ", "union ", "enum " })
                if (s.rfind(w, 0) == 0) { s.erase(0, std::strlen(w)); peeled = true; break; }
        }

        auto it = cxxForeignTypeSpellings_.find(SqueezeCxxSpelling(s));
        if (it == cxxForeignTypeSpellings_.end()) return false;
        out = TypeAndValue{};
        out.TypeName = it->second;
        out.IsRvalueRef = ptr > 0 && ctype.find("&&") != std::string::npos;
        if (ptr > 0) out.Pointer = true;
        if (ptr > 1) out.ElemPointer = true;
        mapped = ptr <= 2;
        return true;
    }

bool LLVMBackend::MapCTypeToTypeAndValueImpl(std::string ctype, TypeAndValue& out,
                                    std::unordered_set<std::string>& visited)
{
        if (ctype.find("::*") != std::string::npos)
            return false;
        // Detect function-pointer spelling before the '*'-strip path mangles it. The declarator
        // may carry cv/nullability words between '*' and ')', so do not rely on the literal
        // "(*)" substring as a precondition for the parser.
        if (ParseCFunctionPointerSpelling(ctype, out, visited))
            return true;

        // Before any '*' or qualifier is removed: a requested C++ specialization is keyed on
        // Clang's canonical spelling, inner pointers and inner const included.
        {
            bool mapped = false;
            if (TryMapCxxForeignSpelling(ctype, out, mapped)) return mapped;
        }

        // Arrays decay to a pointer; drop the '[...]' and bump the pointer level.
        int ptr = 0;
        if (auto br = ctype.find('['); br != std::string::npos)
        {
            ptr++;
            ctype = ctype.substr(0, br);
        }
        // A '*' inside a template argument list (`ImVector<T *>`) belongs to the argument, not
        // to this declarator: only the '*'s after the closing '>' make the type a pointer. A
        // by-value spelling keeps going: a published specialization resolves below, an
        // unpublished one fails there and the caller falls back to an opaque blob.
        if (ctype.find('<') != std::string::npos && ctype.rfind('>') != std::string::npos)
        {
            const size_t tplEnd = ctype.rfind('>');
            const int outer = (int)std::count(ctype.begin() + tplEnd, ctype.end(), '*');
            ptr += outer;
            ctype.erase(std::remove(ctype.begin() + tplEnd, ctype.end(), '*'), ctype.end());
        }
        else
        {
            ptr += (int)std::count(ctype.begin(), ctype.end(), '*');
            ctype.erase(std::remove(ctype.begin(), ctype.end(), '*'), ctype.end());
        }

        // Strip cv / nullability qualifiers - they do not affect the ABI here.
        auto stripWord = [&](const char* w) { EraseDeclaratorToken(ctype, w); };
        for (const char* q : { "const", "volatile", "restrict", "__restrict", "__restrict__",
                               "_Nonnull", "_Nullable", "_Null_unspecified" })
            stripWord(q);

        // C++ references use the same machine representation as a pointer to the referred
        // object at a call boundary. Keep the referred nominal type so overload matching remains
        // useful, and normalize Clang's class/namespace spelling to CFlat's dotted names.
        size_t refPos = ctype.find("&&");
        if (refPos != std::string::npos)
        {
            ctype.erase(refPos, 2);
            ++ptr;
            out.IsRvalueRef = true;
        }
        else if ((refPos = ctype.find('&')) != std::string::npos)
        {
            ctype.erase(refPos, 1);
            ++ptr;
        }

        // Collapse runs of whitespace and trim - so "unsigned   long  long" normalizes.
        std::string base;
        bool prevSpace = true; // leading -> skip
        for (char c : ctype)
        {
            bool isSpace = (c == ' ' || c == '\t' || c == '\n' || c == '\r');
            if (isSpace) { if (!prevSpace) base += ' '; prevSpace = true; }
            else { base += c; prevSpace = false; }
        }
        while (!base.empty() && base.back() == ' ') base.pop_back();
        // The foreign-type lookup ran at entry, on the intact spelling (TryMapCxxForeignSpelling).
        // Repeating it here would search for a spelling whose inner '*' and cv words are gone.
        bool enumTag = false;
        bool structTag = false;
        bool unionTag = false;
        for (const char* tag : { "class ", "struct ", "union ", "enum " })
            if (base.rfind(tag, 0) == 0)
            {
                enumTag = std::strcmp(tag, "enum ") == 0;
                structTag = std::strcmp(tag, "struct ") == 0;
                unionTag = std::strcmp(tag, "union ") == 0;
                base.erase(0, std::strlen(tag));
                break;
            }
        for (size_t pos; (pos = base.find("::")) != std::string::npos; ) base.replace(pos, 2, ".");

        // 3+ levels of indirection collapse to void** - CFlat TypeAndValue has at most two pointer
        // levels. Pointers are the same ABI size on x64/x86, so calls still link correctly.
        if (ptr > 2)
        {
            out.TypeName = "void";
            out.Pointer = true;
            out.ElemPointer = true;
            return true;
        }

        // enum decays to int. struct/union by-value: look up in dataStructures for ABI lowering.
        // struct/union pointers become opaque void* (only a pointer-sized slot is needed).
        std::string mapped;
        if (enumTag)
        {
            if (auto it = enumBackingTypes.find(base); it != enumBackingTypes.end())
            {
                out.TypeName = base;
                out.EnumBacking = it->second;
                out.Pointer = ptr >= 1;
                out.ElemPointer = ptr == 2;
                return true;
            }
            mapped = "int";
        }
        else if (ptr == 0 && (structTag || unionTag))
        {
            std::string tag = base;
            // Trim any trailing whitespace (shouldn't happen post-normalize but be defensive).
            while (!tag.empty() && tag.back() == ' ') tag.pop_back();
            if (tag.empty() || dataStructures.find(tag) == dataStructures.end())
                return false;
            out.TypeName = tag;
            out.Pointer = false;
            out.ElemPointer = false;
            return true;
        }
        else
        {
            static const std::unordered_map<std::string, std::string> scalarMap = {
                { "void", "void" }, { "_Bool", "bool" }, { "bool", "bool" },
                { "char", "char" }, { "signed char", "i8" }, { "unsigned char", "u8" },
                { "short", "short" }, { "short int", "short" }, { "signed short", "short" },
                { "unsigned short", "u16" }, { "unsigned short int", "u16" },
                { "int", "int" }, { "signed", "int" }, { "signed int", "int" },
                { "unsigned", "u32" }, { "unsigned int", "u32" },
                { "long long", "i64" }, { "long long int", "i64" }, { "signed long long", "i64" },
                { "unsigned long long", "u64" }, { "unsigned long long int", "u64" },
                { "__int128", "i128" }, { "unsigned __int128", "u128" },
                { "float", "float" }, { "double", "double" },
                { "char16_t", "u16" }, { "char32_t", "u32" },
            };
            // C `long` is the one scalar whose width is target-dependent: Windows is LLP64
            // (32-bit long), Linux/macOS are LP64 (64-bit long). `size_t` desugars to it.
            static const std::unordered_set<std::string> cLongSigned = {
                "long", "long int", "signed long", "signed long int" };
            static const std::unordered_set<std::string> cLongUnsigned = {
                "unsigned long", "unsigned long int" };

            auto it = scalarMap.find(base);
            if (it != scalarMap.end())
                mapped = it->second;
            else if (base == "std.nullptr_t" || base == "std.__1.nullptr_t" || base == "nullptr_t")
            {
                mapped = "void";
                if (ptr == 0) ptr = 1;
            }
            else if (cLongSigned.count(base) > 0)
                mapped = targetWindows_ ? "i32" : "i64";
            else if (cLongUnsigned.count(base) > 0)
                mapped = targetWindows_ ? "u32" : "u64";
            else if (base == "wchar_t")
                mapped = targetWindows_ ? "u16" : "i32";
            else if (base == "long double")
            {
                if (IsCInteropLongDoubleSupported()) mapped = "double";
                else return false;
            }
            else if (ptr > 0)
                mapped = "void"; // unknown pointee (struct*, function ptr, ...) -> opaque ptr
            else if (dataStructures.find(base) != dataStructures.end())
            {
                // Bare typedef-style spelling resolved to a registered C struct (e.g. clang
                // emitted "Point" for a `typedef struct Point Point;` without the tag prefix).
                out.TypeName = base;
                out.Pointer = false;
                out.ElemPointer = false;
                return true;
            }
            else
            {
                // Chase a user typedef from AdoptRawTypedefs; append pointer stars so HANDLE (ptr=0)->void* works.
                // visited-set prevents pathological self-referential typedefs from looping.
                auto td = cTypedefMap_.find(base);
                if (td != cTypedefMap_.end() && visited.insert(base).second)
                {
                    std::string substituted = td->second;
                    if (ptr > 0) substituted += std::string(ptr, '*');
                    return MapCTypeToTypeAndValueImpl(std::move(substituted), out, visited);
                }
                return false;    // struct/union by value or unknown scalar
            }
        }

        out.TypeName = mapped;
        out.Pointer = ptr >= 1;
        out.ElemPointer = ptr == 2;
        return true;
    }

bool LLVMBackend::HashFileFnv1a(const std::string& path, uint64_t& outHash)
{
        auto bufOrErr = llvm::MemoryBuffer::getFile(path);
        if (!bufOrErr) return false;
        uint64_t h = 1469598103934665603ULL; // FNV offset basis
        for (unsigned char c : (*bufOrErr)->getBuffer())
        {
            h ^= c;
            h *= 1099511628211ULL; // FNV prime
        }
        outHash = h;
        return true;
    }

bool LLVMBackend::HashFileContents(const std::string& path, uint64_t& outHash) const
{
        return HashFileFnv1a(path, outHash);
    }

void LLVMBackend::RegisterCSignatures(const std::vector<CSigEntry>& sigs, const std::string& fileForLsp,
                             const std::string& programAlias)
{
        std::unordered_map<std::string, std::string> stdFunctionClasses;
        for (const CSigEntry& e : sigs)
        {
            if (!e.bindRefusal.empty())
            {
                cxxBindingRefusals_[e.name] = e.bindRefusal;
                continue;
            }
            std::string regName  = e.name;
            if (e.isCxx)
            {
                const size_t op = regName.rfind(".operator");
                if (op != std::string::npos) regName.erase(0, op + 1);
            }
            /*
             * M6 - a C++ pointer parameter to a KNOWN class keeps its pointee type instead of
             * decaying to void* (the string mapper's default for a record pointer). Typing it is
             * what lets a derived-class pointer bind here and be shifted to the base subobject at
             * the call; a void* parameter would swallow the argument and pass the wrong address.
             * Records are always registered before signatures, so the type is available by now.
             * The C path is untouched.
             */
            std::vector<TypeAndValue> retypedParams;
            CSigEntry retypedEntry;
            const CSigEntry* sigp = &e;
            if (e.isCxx && e.paramSpellings.size() == e.params.size())
            {
                retypedParams = e.params;
                bool changed = false;
                for (size_t i = 0; i < retypedParams.size(); ++i)
                {
                    TypeAndValue& p = retypedParams[i];
                    const std::string& ps = e.paramSpellings[i];
                    std::string retText;
                    std::string paramsText;
                    if (cflat_cinterop::SplitStdFunctionSpelling(ps, retText, paramsText))
                    {
                        std::string className;
                        auto cached = stdFunctionClasses.find(ps);
                        const bool wasCached = cached != stdFunctionClasses.end();
                        if (wasCached)
                            className = cached->second;
                        else
                        {
                            TypeAndValue ret;
                            if (MapCTypeToTypeAndValue(retText, ret))
                            {
                                TypeAndValue closure;
                                closure.IsFunctionPointer = true;
                                closure.TypeName = "__c_fn_ptr";
                                closure.FuncPtrReturnTypeName = ret.TypeName;
                                closure.FuncPtrReturnPointer = ret.Pointer;
                                closure.FuncPtrReturnPointerDepth = ret.ValuePointerDepth();
                                size_t start = 0;
                                bool ok = true;
                                int angleDepth = 0;
                                int parenDepth = 0;
                                for (size_t pos = 0; pos <= paramsText.size(); ++pos)
                                {
                                    const bool atEnd = pos == paramsText.size();
                                    const char c = atEnd ? ',' : paramsText[pos];
                                    if (!atEnd && c == '<') ++angleDepth;
                                    else if (!atEnd && c == '>') --angleDepth;
                                    else if (!atEnd && c == '(') ++parenDepth;
                                    else if (!atEnd && c == ')') --parenDepth;
                                    if ((c != ',' || angleDepth != 0 || parenDepth != 0) && !atEnd)
                                        continue;
                                    std::string one = paramsText.substr(start, pos - start);
                                    while (!one.empty() && std::isspace((unsigned char)one.front()))
                                        one.erase(one.begin());
                                    while (!one.empty() && std::isspace((unsigned char)one.back()))
                                        one.pop_back();
                                    if (!one.empty())
                                    {
                                        TypeAndValue pv;
                                        if (!MapCTypeToTypeAndValue(one, pv)) { ok = false; break; }
                                        TypeAndValue::FuncPtrParam fp;
                                        fp.TypeName = pv.TypeName;
                                        fp.Pointer = pv.Pointer;
                                        fp.PointerDepth = pv.ValuePointerDepth();
                                        closure.FuncPtrParams.push_back(std::move(fp));
                                    }
                                    start = pos + 1;
                                }
                                if (ok)
                                {
                                    std::vector<std::pair<std::string, int>> parts;
                                    for (const auto& fp : closure.FuncPtrParams)
                                        parts.push_back({ fp.TypeName, fp.PointerDepth });
                                    const std::string encoded = MangleClosureType(*this, true,
                                        closure.FuncPtrReturnTypeName,
                                        closure.FuncPtrReturnPointerDepth, parts);
                                    RegisterEncodedClosureType(encoded, closure);
                                    className = MangleGenericInstance(*this,
                                        "std.function", { encoded });
                                    std::string requestError;
                                    bool requested = RequestCxxType("std.function", { encoded }, className,
                                                              requestError);
                                    if (!requested)
                                        LogError(requestError.empty()
                                            ? "cannot request the C++ std.function specialization"
                                            : requestError);
                                    stdFunctionClasses.emplace(ps, className);
                                }
                            }
                            if (!className.empty())
                            {
                                p.TypeName = className;
                                p.Pointer = false;
                                p.IsAlias = true;
                                changed = true;
                            }
                        }
                        if (!className.empty() && wasCached)
                        {
                            p.TypeName = className;
                            p.Pointer = false;
                            p.IsAlias = true;
                            changed = true;
                        }
                    }
                    if (p.IsFunctionPointer || !p.Pointer || p.TypeName != "void") continue;
                    int ptrLevels = 0;
                    std::string tag = CxxRecordPointeeTag(e.paramSpellings[i], ptrLevels);
                    if (tag.empty() || ptrLevels != 1) continue;
                    if (dataStructures.find(tag) == dataStructures.end()) continue;
                    if (cxxRecords_.count(tag) == 0) continue;
                    p.TypeName = tag;
                    changed = true;
                }
                if (changed)
                {
                    retypedEntry = e;
                    retypedEntry.params = std::move(retypedParams);
                    sigp = &retypedEntry;
                }
            }
            const CSigEntry& sig = *sigp;
            if (e.isCxx)
            {
                // Prototype boundary: the C++ path carries primitives and bare pointers only.
                // A record by value needs the aggregate ABI arrangement, which is not part of
                // this prototype - refuse at registration so the LSP sees the same answer.
                if (RejectCxxRecordByValue(e)) continue;
                NoteCxxForeignNamespace(regName);
                for (size_t pos = 0; (pos = regName.find('.', pos)) != std::string::npos; ++pos)
                    RegisterNamespace(regName.substr(0, pos));
            }
            bool        isProgMain = (!programAlias.empty() && e.name == "main");
            // The imported program's object was compiled with -Dmain=__imported_main_<alias>,
            // so the declaration must link under regName - never under the source spelling.
            std::string linkageName = isProgMain ? std::string() : e.linkageName;
            if (isProgMain)
                regName = "__imported_main_" + programAlias;

            // external=true: unmangled name + C-compatible types; cdecl on the call.
            // Declaring .c/header published for the call (RAII: LogError throws).
            {
                CInteropDeclarationScope declaringFile(*this, e.file.empty() ? fileForLsp : e.file);
                // Hand clang's arrangement to the declaration; C leaves it null and keeps the
                // existing size heuristic. Cleared by CreateFunctionDeclaration on entry.
                CxxAbiPlanScope abiPlan(*this, (e.isCxx && e.abi.valid) ? &e.abi : nullptr,
                                        /*sink*/ nullptr);
                CreateFunctionDeclaration(regName, sig.ret, sig.params, /*external=*/true, e.variadic,
                                          /*returnsOwned=*/false, /*isMethod=*/false,
                                          CallingConv::Cdecl, linkageName, e.isCxx, e.isNoexcept);
            }
            if (auto fit = functionTable.find(regName); fit != functionTable.end())
            {
                bool assignedDefaults = false;
                for (FunctionSymbol& sym : fit->second)
                    if (sym.External)
                    {
                        sym.IsCInteropDeclaration = true;
                        if (sym.UniqueName == linkageName)
                        {
                            sym.DefaultArguments = e.defaultArgs;
                            assignedDefaults = true;
                        }
                    }
                if (!assignedDefaults && fit->second.size() == 1)
                    fit->second.front().DefaultArguments = e.defaultArgs;
            }

            // A free C++ function whose omitted suffix contains a non-constant default gets an
            // exact-arity overload backed by a C++ forwarding body. Member defaults stay on the
            // refusal path because a wrapper cannot safely reproduce private member access.
            if (e.isCxx && e.defaultArgs.size() == e.params.size())
            {
                const auto canWrap = [&](const TypeAndValue& t) {
                    return t.Pointer || !dataStructures.count(t.TypeName);
                };
                if (canWrap(e.ret))
                    for (size_t n = 0; n < e.params.size(); ++n)
                    {
                        if (n < e.defaultArgs.size() && e.defaultArgs[n].kind == "unsupported")
                            continue;
                        if (!HasNonConstDefaultSuffix(e.defaultArgs, n)) continue;
                        bool supported = true;
                        for (size_t i = 0; i < n; ++i)
                            if (!canWrap(e.params[i])) { supported = false; break; }
                        if (!supported) continue;
                        std::vector<TypeAndValue> prefix(e.params.begin(), e.params.begin() + n);
                        const std::string wrapper = CxxDefaultWrapperName(e.linkageName, n);
                        CInteropDeclarationScope declaringFile(*this,
                            e.file.empty() ? fileForLsp : e.file);
                        CreateFunctionDeclaration(regName, e.ret, prefix, /*external=*/true,
                            /*varargs=*/false, /*returnsOwned=*/false, /*isMethod=*/false,
                            CallingConv::Cdecl, wrapper, /*isCxx=*/true, e.isNoexcept);
                        if (auto wit = functionTable.find(regName); wit != functionTable.end())
                            for (FunctionSymbol& sym : wit->second)
                                if (sym.External && sym.UniqueName == wrapper)
                                {
                                    sym.IsCInteropDeclaration = true;
                                    sym.DefaultArguments.clear();
                                }
                    }
            }

            if (isProgMain)
            {
                programTable[programAlias].MainFunction     = module->getFunction(regName);
                programTable[programAlias].IsImportedProgram = true;
                continue; // not a user-facing symbol; skip the LSP sink registration below
            }

            if (auto* s = GetSymbolSink())
            {
                s->RemoveFunctionAliases(e.name);
                std::string sig = SpellType(*this, e.ret) + " " + e.name + "(";
                bool first = true;
                for (const auto& p : e.params)
                {
                    if (!first) sig += ", ";
                    first = false;
                    sig += SpellType(*this, p);
                    if (!p.VariableName.empty()) sig += " " + p.VariableName;
                }
                sig += ")";
                // Prefer the declaration's own header (presumed loc) so go-to-definition
                // lands on the real prototype, not the umbrella header that was imported.
                const std::string& declFile = e.file.empty() ? fileForLsp : e.file;
                s->Register(SymbolKind::Function, e.name, declFile, e.line, e.col < 0 ? 0 : e.col, sig);
            }
        }

        // A previous import may have supplied an alias before this declaration arrived.
        RegisterCMacroAliases({}, {}, fileForLsp);

        if (verbose)
            std::cout << std::format("[verbose]   registered {} C function(s) from {}\n", sigs.size(), fileForLsp);
}

std::string LLVMBackend::GetCxxBindingRefusal(const std::string& name) const
{
        auto it = cxxBindingRefusals_.find(name);
        if (it != cxxBindingRefusals_.end()) return it->second;
        for (const auto& [key, refusal] : cxxBindingRefusals_)
            if (key.ends_with("." + name)) return refusal;
        return {};
}

/*
 * The triple every C/C++ extraction parse uses. It is derived from the target this compile has
 * already CHOSEN, which is also the triple the companion module must agree with - the main
 * llvm::Module carries no triple until native emission, far after the companion is linked.
 */
std::string LLVMBackend::CInteropTargetTriple() const
{
        if (targetWindows_)
            return platformValue == 32 ? "i686-pc-windows-msvc" : "x86_64-pc-windows-msvc";
        // Matches the codegen triple EmitExecutableMachO sets, so __APPLE__ and the Apple
        // system-header search are active during extraction.
        if (targetMacOS_) return "arm64-apple-macosx11.0.0";
        return platformValue == 32 ? "i686-pc-linux-gnu" : "x86_64-pc-linux-gnu";
    }

std::vector<std::string> LLVMBackend::BuildClangDriverArgs(const std::string& headerDir,
                                               const std::vector<std::string>& extraDefines,
                                               bool errorRecovery, bool asCxx) const
{
        std::vector<std::string> args;
        // Parse the C against the target OS so predefined macros (_WIN32 vs __linux__)
        // and the system header search behave like the real compile would. On non-Windows
        // the MSVC triple would pull in MSVC predefines and break libc header resolution.
        args.push_back("--target=" + CInteropTargetTriple());
        if (targetMacOS_ && !targetWindows_)
        {
            // Headers need a REAL SDK (the harvested ~/.cflat/macsdk carries link stubs only),
            // so isysroot points at $SDKROOT/xcrun.
            std::string sdk;
#if defined(__APPLE__)
            sdk = MacSdkPathCached();
#else
            if (const char* env = std::getenv("SDKROOT")) if (env[0]) sdk = env;
#endif
            if (!sdk.empty())
            {
                args.push_back("-isysroot");
                args.push_back(sdk);
            }
            else if (symbolSink_ == nullptr)
            {
                // Do not fall through to a Linux triple - Apple headers would misparse.
                // In LSP/analyze mode (symbolSink_ set) degrade silently like other binds.
                LogErrorMessage("C header import targeting macOS requires an SDK: set $SDKROOT or install "
                         "Xcode / Command Line Tools so 'xcrun --show-sdk-path' resolves");
            }
        }
#ifdef CFLAT_CLANG_RESOURCE_DIR
        // Compiler-provided headers (stdarg.h, stddef.h, immintrin.h, ...). Without this the
        // in-process frontend looks for them next to the cflat executable and finds nothing, which
        // breaks any header that includes one - every libc++ header does, through <wchar.h>.
        {
            static const std::string resourceDir = [] {
                std::error_code ec;
                std::string d = CFLAT_CLANG_RESOURCE_DIR;
                return std::filesystem::exists(d + "/include", ec) ? d : std::string();
            }();
            if (!resourceDir.empty())
            {
                args.push_back("-resource-dir");
                args.push_back(resourceDir);
            }
        }
#endif
        args.push_back("-fsyntax-only");
        args.push_back("-x");
        // C++ is used only by the focused uuid-harvest pass, so the SDK's MIDL_INTERFACE form
        // (which carries the __declspec(uuid) the C form omits) is parsed. The normal bind is C.
        args.push_back(asCxx ? "c++" : "c");
        if (errorRecovery)
        {
            args.push_back("-ferror-limit=0");
            args.push_back("-Wno-everything");
        }
        if (!headerDir.empty()) args.push_back("-I" + headerDir);
        for (const auto& inc : cIncludeDirs_) args.push_back("-I" + inc);
        for (const auto& def : cDefines_)      args.push_back("-D" + def);
        for (const auto& def : extraDefines)   args.push_back("-D" + def);
        return args;
    }

static bool ParseCxxArrayParameter(const std::string& spelling, std::string& element,
                                   uint64_t& extent);

bool LLVMBackend::MapRawSig(const cflat_cinterop::RawSig& r, CSigEntry& e)
{
        e = CSigEntry();
        e.paramSpellings = r.paramTypes;
        e.retSpelling = r.retType;
        e.defaultArgs = r.defaultArgs;
        e.name     = r.name;
        e.linkageName = r.linkageName;
        e.isCxx = r.isCxx;
        e.abi   = r.abi;
        e.isNoexcept = r.isNoexcept;
        e.bindRefusal = r.bindRefusal;
        e.variadic = r.variadic;
        e.file     = r.file;
        e.line     = r.line ? r.line : 1;
        e.col      = r.col < 0 ? 0 : r.col;
        if (!MapCTypeToTypeAndValue(r.retType, e.ret))
        {
            if (r.isCxx)
                e.bindRefusal = IsLongDoubleSpelling(r.retType) && !IsCInteropLongDoubleSupported()
                    ? std::format("'{}' was not bound: {}", r.name, CInteropLongDoubleRefusal())
                    : std::format("'{}' was not bound: return type '{}' is unsupported",
                                  r.name, r.retType);
            if (verbose) std::cout << std::format("[verbose]   skipping '{}': unsupported return type '{}'\n", r.name, r.retType);
            return r.isCxx;
        }
        if (r.isCxx && r.retType.find('&') != std::string::npos
            && r.retType.find("&&") == std::string::npos
            && !e.ret.IsFunctionPointer)
        {
            e.ret.Pointer = false;
            e.ret.ElemPointer = false;
            e.ret.IsAlias = true;
        }
        for (size_t i = 0; i < r.paramTypes.size(); ++i)
        {
            if (r.isCxx && r.paramTypes[i].find("::*") != std::string::npos)
            {
                const std::string pname = i < r.paramNames.size() && !r.paramNames[i].empty()
                    ? r.paramNames[i] : std::format("p{}", i);
                e.bindRefusal = std::format(
                    "'{}' was not bound: parameter '{}' is a pointer to member; its ABI representation and invocation are not supported (an ordinary pointer is not equivalent)",
                    r.name, pname);
                return true;
            }
            TypeAndValue ptv;
            if (!MapCTypeToTypeAndValue(r.paramTypes[i], ptv))
            {
                std::string arrayElement;
                uint64_t arrayExtent = 0;
                if (r.isCxx && ParseCxxArrayParameter(r.paramTypes[i], arrayElement, arrayExtent)
                    && MapCTypeToTypeAndValue(arrayElement, ptv))
                {
                    ptv.Pointer = true;
                    ptv.ElemPointer = false;
                    ptv.PointerDepth = 1;
                    ptv.ConstArraySize = arrayExtent;
                }
                else if (r.isCxx && r.paramTypes[i].find("function<") != std::string::npos)
                {
                    ptv.TypeName = "void";
                    ptv.Pointer = true;
                }
                else
                {
                const std::string pname = i < r.paramNames.size() && !r.paramNames[i].empty()
                    ? r.paramNames[i] : std::format("p{}", i);
                if (r.isCxx)
                    e.bindRefusal = IsLongDoubleSpelling(r.paramTypes[i])
                            && !IsCInteropLongDoubleSupported()
                        ? std::format("'{}' was not bound: parameter '{}': {}",
                                      r.name, pname, CInteropLongDoubleRefusal())
                        : std::format(
                            "'{}' was not bound: parameter '{}' has unsupported C++ type '{}'",
                            r.name, pname, r.paramTypes[i]);
                if (verbose) std::cout << std::format("[verbose]   skipping '{}': unsupported parameter type '{}'\n", r.name, r.paramTypes[i]);
                return r.isCxx;
                }
            }
            if (i < r.paramNames.size()) ptv.VariableName = r.paramNames[i];
            e.params.push_back(std::move(ptv));
        }
        return true;
    }

static std::string AutoCxxForeignIdentity(const std::string& spelling)
{
        // Keep multi-word primitive template arguments aligned with the CFlat spellings emitted
        // by CxxSpellingForCflatType (for example vector<unsigned char> -> vector$u8).
        std::string normalized = spelling;
        for (const auto& [from, to] : std::array<std::pair<std::string_view, std::string_view>, 6>{
                 std::pair{ "signed char", "i8" },
                 std::pair{ "unsigned char", "u8" },
                 std::pair{ "unsigned short", "u16" },
                 std::pair{ "unsigned int", "u32" },
                 std::pair{ "unsigned long long", "u64" },
                 std::pair{ "long long", "i64" } })
        {
            for (size_t pos = 0; (pos = normalized.find(from, pos)) != std::string::npos; )
            {
                const bool leftOk = pos == 0
                    || (!std::isalnum((unsigned char)normalized[pos - 1])
                        && normalized[pos - 1] != '_');
                const size_t end = pos + from.size();
                const bool rightOk = end == normalized.size()
                    || (!std::isalnum((unsigned char)normalized[end]) && normalized[end] != '_');
                if (leftOk && rightOk)
                {
                    normalized.replace(pos, from.size(), to);
                    pos += to.size();
                }
                else
                    pos = end;
            }
        }
        std::string out;
        for (size_t i = 0; i < normalized.size(); ++i)
        {
            if (normalized[i] == ':' && i + 1 < normalized.size() && normalized[i + 1] == ':')
            { out += '.'; ++i; continue; }
            if (normalized[i] == '<' || normalized[i] == ',') { out += '$'; continue; }
            if (normalized[i] == '>') continue;
            if (std::isspace((unsigned char)normalized[i])) continue;
            if (normalized[i] == '*') { out += "ptr"; continue; }
            if (normalized[i] == '&') { out += "ref"; continue; }
            if (std::isalnum((unsigned char)normalized[i]) || normalized[i] == '_'
                || normalized[i] == '.' || normalized[i] == '$')
                out += normalized[i];
            else
                out += '_';
        }
        return out;
}

static std::string CxxMemberValueSpelling(const std::string& spelling)
{
        std::string out = spelling;
        while (!out.empty() && std::isspace((unsigned char)out.back())) out.pop_back();
        for (;;)
        {
            bool changed = false;
            while (!out.empty() && (out.back() == '*' || out.back() == '&'))
            {
                out.pop_back();
                while (!out.empty() && std::isspace((unsigned char)out.back())) out.pop_back();
                changed = true;
            }
            for (const char* q : { "const ", "volatile ", "class ", "struct ", "union " })
                if (out.starts_with(q))
                {
                    out.erase(0, std::strlen(q));
                    changed = true;
                    break;
                }
            if (!changed) break;
        }
        return out;
}

static std::string CxxDefaultWrapperName(const std::string& linkageName, size_t omittedArity)
{
        std::string out = "__cflat_dflt_";
        for (char c : linkageName)
            out += std::isalnum((unsigned char)c) || c == '_' ? c : '_';
        out += "_" + std::to_string(omittedArity);
        return out;
}

static bool HasNonConstDefaultSuffix(const std::vector<cflat_cinterop::RawDefaultArg>& defaults,
                                     size_t first)
{
        if (first >= defaults.size()) return false;
        for (size_t i = first; i < defaults.size(); ++i)
        {
            if (defaults[i].kind.empty()) return false;
            if (defaults[i].kind == "unsupported") continue;
            if (defaults[i].kind == "nonconst") return true;
        }
        return false;
}

static std::string CxxNameFromCflat(const std::string& name)
{
        std::string out = name;
        for (size_t pos = 0; (pos = out.find('.', pos)) != std::string::npos; pos += 2)
            out.replace(pos, 1, "::");
        return "::" + out;
}

static std::string BuildCxxDefaultWrappers(const std::vector<cflat_cinterop::RawSig>& sigs)
{
        std::string source;
        bool emitted = false;
        for (const auto& sig : sigs)
        {
            if (!sig.isCxx || sig.variadic || sig.linkageName.empty()
                || sig.defaultArgs.size() != sig.paramTypes.size())
                continue;
            const std::string target = CxxNameFromCflat(
                sig.qualifiedName.empty() ? sig.name : sig.qualifiedName);
            for (size_t n = 0; n < sig.paramTypes.size(); ++n)
            {
                if (!HasNonConstDefaultSuffix(sig.defaultArgs, n)) continue;
                if (!emitted)
                {
                    source += "template <class T> struct __cflat_pid { typedef T type; };\n";
                    emitted = true;
                }
                const std::string base = CxxDefaultWrapperName(sig.linkageName, n);
                const std::string cppName = base + "_cpp";
                std::string params;
                std::string args;
                for (size_t i = 0; i < n; ++i)
                {
                    if (!params.empty()) { params += ", "; args += ", "; }
                    const std::string& type = sig.paramTypes[i];
                    params += "typename __cflat_pid<" + type + ">::type a" + std::to_string(i);
                    args += type.ends_with("&&")
                        ? "static_cast<" + type + ">(a" + std::to_string(i) + ")"
                        : "a" + std::to_string(i);
                }
                const std::string suffix = sig.isNoexcept ? " noexcept" : "";
                source += "extern \"C++\" { static " + sig.retType + " " + cppName + "("
                    + params + ")" + suffix + " { ";
                if (sig.retType == "void") source += target + "(" + args + ");";
                else source += "return " + target + "(" + args + ");";
                source += " } }\n";
                source += "extern \"C\" __attribute__((weak)) " + sig.retType + " " + base + "("
                    + params + ")" + suffix + " { ";
                if (sig.retType == "void") source += cppName + "(" + args + ");";
                else source += "return " + cppName + "(" + args + ");";
                source += " }\n";
            }
        }
        return source;
}

/*
 * Whether one spelling out of a C++ signature is worth a type request, and under which identity.
 * Shared by the requesting path and by the batch collector so a batch asks for exactly the
 * spellings the single-request path would have asked for.
 * Returns: 0 skip, 1 plain qualified name, 2 specialization, 3 specialization with no identity.
 */
int LLVMBackend::ClassifyCxxSignatureSpelling(const std::string& spelling,
                                              const std::unordered_set<std::string>* localEnums,
                                              std::string& identity, std::string& named)
{
        if (spelling.find('(') != std::string::npos) return 0;
        named = spelling;
        while (!named.empty() && std::isspace((unsigned char)named.back())) named.pop_back();
        while (!named.empty() && (named.back() == '&' || named.back() == '*'))
        {
            named.pop_back();
            while (!named.empty() && std::isspace((unsigned char)named.back())) named.pop_back();
        }
        while (named.starts_with("const ")) named.erase(0, 6);
        while (named.starts_with("volatile ")) named.erase(0, 9);
        if (named.find("::") == std::string::npos) return 0;
        if (named.find('<') == std::string::npos)
        {
            // Only a plain qualified class name can be a typedef of a specialization worth a
            // request (std::string_view). An enum, a member-pointer fragment, and a record or
            // enum this header already registered are never one - each request is two Clang
            // parses of the whole TU, so the filter pays for itself immediately.
            if (named.starts_with("enum ")) return 0;
            for (const char* kw : { "struct ", "class ", "union " })
                if (named.starts_with(kw)) named.erase(0, std::strlen(kw));
            if (named.find(' ') != std::string::npos) return 0;
            TypeAndValue mapped;
            bool mappedForeign = false;
            if (TryMapCxxForeignSpelling(named, mapped, mappedForeign) && mappedForeign) return 0;
            identity = AutoCxxForeignIdentity(named);
            if (identity.empty()) return 0;
            if (IsDataStructure(identity) || !ResolveEnumTypeName(identity).empty()
                || (localEnums != nullptr && localEnums->count(identity) != 0))
                return 0;
            return 1;
        }
        const size_t open = named.find('<');
        int depth = 0;
        size_t close = std::string::npos;
        for (size_t i = open; i < named.size(); ++i)
        {
            if (named[i] == '<') ++depth;
            else if (named[i] == '>' && --depth == 0) { close = i; break; }
        }
        if (close != std::string::npos) named.erase(close + 1);
        identity = AutoCxxForeignIdentity(named);
        return identity.empty() ? 3 : 2;
}

bool LLVMBackend::RequestCxxSignatureTypes(const cflat_cinterop::RawSig& sig,
                                           const std::unordered_set<std::string>* localEnums)
{
        if (!sig.isCxx) return true;
        bool ok = true;
        auto request = [&](const std::string& spelling) {
            std::string identity, named;
            const int kind = ClassifyCxxSignatureSpelling(spelling, localEnums, identity, named);
            if (kind == 0) return;
            if (kind == 3) { ok = false; return; }
            std::string error;
            if (!RequestCxxForeignType(identity, named, error)
                && (kind == 2 || error.find("does not name a C++ class type") == std::string::npos))
                ok = false;
        };
        request(sig.retType);
        for (const std::string& spelling : sig.paramTypes) request(spelling);
        return ok;
}

// Cache-hit replay: the cold extraction requested the class-template specializations each C++
// signature names; a cached entry stores only the spellings, so request them again here.
void LLVMBackend::RequestCxxSignatureTypes(const std::vector<CSigEntry>& sigs)
{
        for (const CSigEntry& e : sigs)
        {
            if (!e.isCxx) continue;
            cflat_cinterop::RawSig raw;
            raw.isCxx = true;
            raw.retType = e.retSpelling;
            raw.paramTypes = e.paramSpellings;
            RequestCxxSignatureTypes(raw);
        }
}

// The same spellings, collected instead of requested, so one import's requests share a batch.
void LLVMBackend::CollectCxxSignatureRequestItems(const cflat_cinterop::RawSig& sig,
                                                  const std::unordered_set<std::string>* localEnums,
                                                  std::vector<CxxRequestItem>& out)
{
        if (!sig.isCxx) return;
        std::vector<std::string> spellings = sig.paramTypes;
        spellings.push_back(sig.retType);
        for (const std::string& spelling : spellings)
        {
            std::string identity, named;
            const int kind = ClassifyCxxSignatureSpelling(spelling, localEnums, identity, named);
            if (kind != 1 && kind != 2) continue;
            CxxRequestItem item;
            item.cflatName = identity;
            item.cxxSpelling = named;
            out.push_back(std::move(item));
        }
}

void LLVMBackend::CollectCxxSignatureRequestItems(const std::vector<CSigEntry>& sigs,
                                                  std::vector<CxxRequestItem>& out)
{
        for (const CSigEntry& e : sigs)
        {
            if (!e.isCxx) continue;
            cflat_cinterop::RawSig raw;
            raw.isCxx = true;
            raw.retType = e.retSpelling;
            raw.paramTypes = e.paramSpellings;
            CollectCxxSignatureRequestItems(raw, nullptr, out);
        }
}

static bool ParseCxxArrayParameter(const std::string& spelling, std::string& element,
                                   uint64_t& extent)
{
        const size_t lb = spelling.rfind('[');
        const size_t rb = spelling.find(']', lb == std::string::npos ? 0 : lb);
        if (lb == std::string::npos || rb == std::string::npos || rb <= lb + 1) return false;
        std::string digits = spelling.substr(lb + 1, rb - lb - 1);
        extent = 0;
        for (char c : digits)
        {
            if (c < '0' || c > '9') return false;
            extent = extent * 10 + (uint64_t)(c - '0');
        }
        if (extent == 0) return false;
        element = spelling.substr(0, lb);
        while (!element.empty() && std::isspace((unsigned char)element.back())) element.pop_back();
        if (!element.empty() && element.back() == ')') element.pop_back();
        const size_t open = element.rfind('(');
        if (open != std::string::npos) element.erase(open);
        element.erase(std::remove(element.begin(), element.end(), '&'), element.end());
        element.erase(std::remove(element.begin(), element.end(), '*'), element.end());
        while (!element.empty() && std::isspace((unsigned char)element.back())) element.pop_back();
        return !element.empty();
}

std::string LLVMBackend::FunctionPointerAbiKey(const TypeAndValue& ret,
                                               const std::vector<TypeAndValue>& params)
{
        auto component = [](const TypeAndValue& tv) {
            return tv.TypeName + ":" + std::to_string(tv.Pointer) + ":"
                 + std::to_string(tv.ValuePointerDepth())
                 + ":" + std::to_string(tv.IsRvalueRef);
        };
        std::string key = component(ret) + "(";
        for (const auto& p : params) key += component(p) + ";";
        return key + ")";
    }

void LLVMBackend::RegisterCxxFunctionPointerAbis(
    const std::vector<cflat_cinterop::RawFunctionPointerAbi>& plans)
{
        for (const auto& raw : plans)
        {
            CxxFunctionPointerAbiPlan plan;
            if (!MapCTypeToTypeAndValue(raw.retType, plan.ret)) continue;
            for (const auto& spelling : raw.paramTypes)
            {
                TypeAndValue p;
                if (!MapCTypeToTypeAndValue(spelling, p)) { plan.params.clear(); break; }
                // C++ callback references map to CFlat pointers because both are pointer ABI slots.
                plan.params.push_back(std::move(p));
            }
            if (plan.params.size() != raw.paramTypes.size()) continue;
            if (!BuildAbiRecipeFromClangPlan(raw.signature, raw.abi, plan.ret, plan.params,
                                             plan.recipe))
                continue;
            cxxFunctionPointerAbiPlans_.emplace(
                FunctionPointerAbiKey(plan.ret, plan.params), std::move(plan));
        }
    }

bool LLVMBackend::MapRawGlobal(const cflat_cinterop::RawGlobalVar& r, CGlobalEntry& e)
{
        if (r.ctype.find('[') != std::string::npos)
        {
            if (verbose) std::cout << std::format("[verbose]   skipping global '{}': array type '{}' is not bindable\n", r.name, r.ctype);
            return false;
        }
        e = CGlobalEntry();
        e.name = r.name;
        e.line = r.line ? r.line : 1;
        e.col  = r.col < 0 ? 0 : r.col;
        if (!MapCTypeToTypeAndValue(r.ctype, e.type))
        {
            if (verbose) std::cout << std::format("[verbose]   skipping global '{}': unsupported type '{}'\n", r.name, r.ctype);
            return false;
        }
        e.type.VariableName = r.name;
        return true;
    }

bool LLVMBackend::ClassifyRawMacro(const cflat_cinterop::RawMacro& r, CMacroEntry& e)
{
        using K = cflat_cinterop::RawMacro;
        if (r.kind == K::Skip && r.aliasTarget.empty()) return false;
        e = CMacroEntry();
        e.name = r.name; e.file = r.file; e.line = r.line ? r.line : 1; e.col = 0;
        if (r.kind == K::Skip)
        {
            // Did not fold, but the body is one identifier: carry the spelling for the alias pass.
            e.aliasTarget = r.aliasTarget;
            return true;
        }
        if (r.kind == K::String) { e.isString = true; e.stringValue = r.stringValue; return true; }
        if (r.kind == K::Float)  { e.isFloat = true;  e.floatValue  = r.floatValue;  return true; }

        e.value = r.intValue;
        if (!r.naturalType.empty())
        {
            TypeAndValue tv;
            if (MapCTypeToTypeAndValue(r.naturalType, tv))
            {
                if (tv.IsFunctionPointer) { e.isFuncPtr = true; e.funcPtrTV = std::move(tv); }
                else if (tv.Pointer && !tv.ElemPointer && tv.TypeName == "void") e.isPointer = true;
                else if (!tv.Pointer && BitfieldStorageBits(tv.TypeName) != 0 && tv.TypeName != "bool")
                    e.intTypeName = tv.TypeName;   // known plain integer scalar
            }
        }
        return true;
    }

void LLVMBackend::AdoptRawTypedefs(const cflat_cinterop::ExtractResult& raw)
{
        for (const auto& t : raw.typedefs)
            if (!t.name.empty() && !t.underlying.empty() && t.underlying != t.name)
            {
                cTypedefMap_.emplace(t.name, t.underlying);
                if (!t.qualifiedName.empty() && t.qualifiedName != t.name)
                    cTypedefMap_.emplace(t.qualifiedName, t.underlying);
            }
    }

void LLVMBackend::CollectRecordTypedefAliases(const cflat_cinterop::ExtractResult& raw,
                                     std::vector<std::pair<std::string, std::string>>& out)
{
        static const std::unordered_set<std::string> qualifiers = {
            "const", "volatile", "restrict", "__restrict", "__restrict__",
            "_Nonnull", "_Nullable", "_Null_unspecified", "struct", "union" };
        for (const auto& t : raw.typedefs)
        {
            if (t.name.empty() || t.underlying.empty() || t.name == t.underlying) continue;
            if (t.underlying.find('(') != std::string::npos) continue;   // function pointer typedef
            if (t.underlying.find('[') != std::string::npos) continue;   // array typedef

            // Split off the pointer depth, then tokenize what remains: the tag is the one word
            // left after dropping cv/nullability qualifiers and the struct/union keyword.
            std::string spelling = t.underlying;
            int ptr = (int)std::count(spelling.begin(), spelling.end(), '*');
            if (ptr > 2) continue;   // TypeAndValue carries at most two pointer levels
            std::replace(spelling.begin(), spelling.end(), '*', ' ');

            std::string tag;
            bool ambiguous = false;
            std::istringstream words(spelling);
            for (std::string w; words >> w; )
            {
                if (qualifiers.count(w)) continue;
                if (!tag.empty()) { ambiguous = true; break; }   // compound spelling we do not model
                tag = w;
            }
            if (ambiguous || tag.empty() || tag == t.name) continue;
            if (dataStructures.find(tag) == dataStructures.end()) continue;  // not a registered record
            out.emplace_back(t.name, tag + std::string(ptr, '*'));
        }
    }

void LLVMBackend::CollectTypeAliases(const cflat_cinterop::ExtractResult& raw,
                                     std::vector<CTypeAliasEntry>& out)
{
        for (const auto& t : raw.typedefs)
        {
            if (t.name.empty() || t.underlying.empty()) continue;
            if (t.name == t.underlying && !t.isAnonymousRecord) continue;
            CTypeAliasEntry entry;
            entry.name = t.name;
            entry.qualifiedName = t.qualifiedName;
            entry.target = t.underlying;
            entry.cxxSpecialization = t.cxxSpecialization;
            entry.isCxxAliasTemplate = t.isCxxAliasTemplate;
            entry.cxxAliasPattern = t.cxxAliasPattern;
            entry.cxxAliasParams = t.cxxAliasParams;
            entry.file = t.file;
            entry.line = t.line;
            entry.col = t.col;
            entry.isAnonymousRecord = t.isAnonymousRecord;
            out.push_back(std::move(entry));
        }
    }

void LLVMBackend::RegisterTypeAliasSymbol(const std::string& alias, const std::string& target,
                                           const std::string& file, int line, int col,
                                           bool isAnonymousRecord)
{
        auto* s = GetSymbolSink();
        if (s == nullptr || alias.empty()) return;

        std::string targetName = target;
        while (!targetName.empty() && targetName.back() == '*') targetName.pop_back();
        size_t first = targetName.find_first_not_of(" \t");
        if (first != std::string::npos) targetName.erase(0, first);
        if (targetName.starts_with("struct ")) targetName.erase(0, 7);
        else if (targetName.starts_with("union ")) targetName.erase(0, 6);
        else if (targetName.starts_with("enum ")) targetName.erase(0, 5);
        size_t last = targetName.find_last_not_of(" \t");
        if (last == std::string::npos) targetName.clear();
        else targetName.resize(last + 1);
        if (targetName == alias)
        {
            // Clang canonicalizes `typedef struct { ... } Alias` to the typedef name. There is
            // no record tag to register, so surface the typedef itself as the struct symbol.
            if (isAnonymousRecord && dataStructures.find(alias) == dataStructures.end())
                s->Register(SymbolKind::Struct, alias, file, line, col, "struct " + alias);
            return;
        }

        // RegisterRecordAliases already surfaced record typedefs with the CFlat-facing spelling
        // (tagRVPT*, not "struct tagRVPT *"); first-writer-wins keeps that entry and its members.
        if (const SymbolDef* prev = s->Lookup(alias))
            if (prev->kind == SymbolKind::Struct
                || (prev->kind == SymbolKind::TypeAlias && !prev->file.empty()))
                return;

        std::string targetFile = file;
        int targetLine = line;
        int targetCol = col;
        if (const SymbolDef* td = s->Lookup(targetName))
        {
            targetFile = td->file;
            targetLine = td->line;
            targetCol = td->column;
        }
        s->Register(SymbolKind::TypeAlias, alias, targetFile, targetLine, targetCol,
                    "typedef " + target + " " + alias);
    }

void LLVMBackend::RegisterTypeAliasSymbols(const std::vector<CTypeAliasEntry>& aliases)
{
        auto cxxIdentity = [&](const CTypeAliasEntry& a, std::string& baseOut,
                               std::vector<std::string>& argsOut, std::string& out) -> bool {
            const std::string& s = a.cxxSpecialization;
            const size_t open = s.find('<');
            if (open == std::string::npos || s.empty() || s.back() != '>') return false;
            std::string base = s.substr(0, open);
            while (!base.empty() && std::isspace((unsigned char)base.back())) base.pop_back();
            for (size_t p = 0; (p = base.find("::", p)) != std::string::npos; p += 1)
                base.replace(p, 2, ".");
            std::vector<std::string> args;
            int depth = 0;
            size_t start = open + 1;
            for (size_t p = start; p <= s.size(); ++p)
            {
                const bool end = p == s.size() - 1;
                if (!end && s[p] == '<') ++depth;
                else if (!end && s[p] == '>') --depth;
                if ((end || (s[p] == ',' && depth == 0)) && p >= start)
                {
                    std::string arg = s.substr(start, p - start);
                    while (!arg.empty() && std::isspace((unsigned char)arg.front())) arg.erase(arg.begin());
                    while (!arg.empty() && std::isspace((unsigned char)arg.back())) arg.pop_back();
                    if (arg.rfind("class ", 0) == 0) arg.erase(0, 6);
                    if (arg == "int") args.push_back("int");
                    else if (!arg.empty() && std::all_of(arg.begin(), arg.end(),
                                                           [](char c) { return std::isdigit((unsigned char)c); }))
                        args.push_back(arg);
                    else if (arg == "unsigned int") args.push_back("u32");
                    else if (arg == "long long") args.push_back("i64");
                    else if (arg == "unsigned long long") args.push_back("u64");
                    else
                    {
                        auto it = cxxForeignTypeSpellings_.find(SqueezeCxxSpelling(arg));
                        if (it == cxxForeignTypeSpellings_.end()) return false;
                        args.push_back(it->second);
                    }
                    start = p + 1;
                }
            }
            if (base == "std.vector" && args.size() > 1
                && args[1].starts_with("std.")) args.resize(1);
            if (args.empty()) return false;
            baseOut = base;
            argsOut = args;
            out = MangleGenericInstance(*this, base, args);
            return !out.empty();
        };
        for (const auto& a : aliases)
        {
            // An alias of a specialization whose arguments have no CFlat spelling (defaulted or
            // library-internal) is requested lazily by its C++ spelling, under the alias's name.
            if (!a.cxxSpecialization.empty() && !a.qualifiedName.empty() && !a.isCxxAliasTemplate)
            {
                std::string base, identity;
                std::vector<std::string> args;
                if (!cxxIdentity(a, base, args, identity))
                {
                    cxxLazyAliasSpecializations_.emplace(a.qualifiedName, a.cxxSpecialization);
                    if (a.name != a.qualifiedName)
                        cxxLazyAliasSpecializations_.emplace(a.name, a.cxxSpecialization);
                    RegisterTypeAliasSymbol(a.qualifiedName, a.cxxSpecialization, a.file, a.line, a.col);
                    continue;
                }
            }
            if (!a.target.empty() && a.target != a.name)
            {
                cTypedefMap_.emplace(a.name, a.target);
                if (!a.qualifiedName.empty() && a.qualifiedName != a.name)
                {
                    std::string qualifiedTarget = a.target;
                    TypeAndValue mappedTarget;
                    if (MapCTypeToTypeAndValue(a.target, mappedTarget))
                    {
                        qualifiedTarget = mappedTarget.TypeName;
                        if (mappedTarget.Pointer) qualifiedTarget += "*";
                        if (mappedTarget.ElemPointer) qualifiedTarget += "*";
                    }
                    cTypedefMap_.emplace(a.qualifiedName, qualifiedTarget);
                    RegisterTypeAlias(a.qualifiedName, qualifiedTarget);
                }
            }
            if (a.isCxxAliasTemplate)
            {
                const size_t open = a.cxxAliasPattern.find('<');
                if (open == std::string::npos || a.cxxAliasPattern.back() != '>') continue;
                std::string targetBase = a.cxxAliasPattern.substr(0, open);
                while (!targetBase.empty() && std::isspace((unsigned char)targetBase.back()))
                    targetBase.pop_back();
                for (size_t p = 0; (p = targetBase.find("::", p)) != std::string::npos; p += 1)
                    targetBase.replace(p, 2, ".");
                if (targetBase.empty()) continue;
                if (!a.qualifiedName.empty())
                    RegisterGenericBaseAlias(a.qualifiedName, targetBase);
                if (a.name != a.qualifiedName)
                    RegisterGenericBaseAlias(a.name, targetBase);
                RegisterTypeAliasSymbol(a.qualifiedName.empty() ? a.name : a.qualifiedName,
                                        a.cxxAliasPattern, a.file, a.line, a.col);
                continue;
            }
            if (!a.cxxSpecialization.empty() && !a.qualifiedName.empty())
            {
                std::string error;
                std::string identity;
                std::string base;
                std::vector<std::string> args;
                if (!cxxIdentity(a, base, args, identity))
                {
                    if (!error.empty()) LogError(error);
                    continue;
                }
                if (!RequestCxxType(base, args, identity, error))
                {
                    if (!error.empty()) LogError(error);
                    continue;
                }
                const std::string target = ResolveTypeAlias(identity);
                RegisterTypeAlias(a.qualifiedName, target);
                if (a.name != a.qualifiedName)
                    RegisterTypeAlias(a.name, target);
                RegisterTypeAliasSymbol(a.qualifiedName, target, a.file, a.line, a.col);
                if (a.name != a.qualifiedName)
                    RegisterTypeAliasSymbol(a.name, target, a.file, a.line, a.col);
            }
            else
                RegisterTypeAliasSymbol(a.name, a.target, a.file, a.line, a.col, a.isAnonymousRecord);
        }
    }

void LLVMBackend::RegisterRecordAliases(const std::vector<std::pair<std::string, std::string>>& aliases)
{
        for (const auto& [alias, target] : aliases)
        {
            if (dataStructures.find(alias) != dataStructures.end()) continue;  // real type wins
            if (ResolveTypeAlias(alias) != alias) continue;        // first-writer-wins
            RegisterTypeAlias(alias, target);

            // Surface the typedef name itself as a navigable LSP symbol. Type resolution already
            // follows the alias, but the symbol index only knew the underlying tag, so --symbol /
            // hover / go-to-def on the alias name (e.g. ID3DBlob -> ID3D10Blob) found nothing.
            // Inherit the target struct's location (registered just before us by RegisterCRecords)
            // so go-to-def jumps to the aliased definition.
            if (auto* s = GetSymbolSink())
            {
                std::string file;
                int line = 0, col = 0;
                // A handle alias carries pointer stars (CGColorSpaceRef -> CGColorSpace*); the
                // symbol index is keyed on the bare tag, so peel them before looking it up.
                std::string targetTag = target;
                while (!targetTag.empty() && targetTag.back() == '*') targetTag.pop_back();
                if (const SymbolDef* td = s->Lookup(targetTag))
                {
                    file = td->file;
                    line = td->line;
                    col = td->column;
                }
                s->Register(SymbolKind::TypeAlias, alias, file, line, col,
                            "typedef " + target + " " + alias);
            }
        }
    }

void LLVMBackend::PruneRecordsToNeededClosure(cflat_cinterop::ExtractResult& raw)
{
        std::vector<cflat_cinterop::RawRecord>& records = raw.records;

        // Last definition wins on a duplicate tag (forward decls are not definitions, so this is
        // rare); the index just needs to resolve a referenced tag to some record we can keep.
        std::unordered_map<std::string, size_t> byName;
        for (size_t i = 0; i < records.size(); ++i)
            if (!records[i].name.empty()) byName[records[i].name] = i;

        // Extract the by-value dependency tag from a type spelling (field, param, return, or
        // global var). Pointer types are pointer-sized regardless of pointee registration, so skip them.
        auto byValueDep = [](const std::string& ctype) -> std::string {
            if (ctype.find('*') != std::string::npos || ctype.find('&') != std::string::npos)
                return {};   // pointer/reference: no sizing dependency
            std::string s = ctype;
            if (auto br = s.find('['); br != std::string::npos) s = s.substr(0, br);  // drop array suffix
            auto trim = [](std::string& x) {
                size_t a = x.find_first_not_of(" \t");
                size_t b = x.find_last_not_of(" \t");
                x = (a == std::string::npos) ? std::string{} : x.substr(a, b - a + 1);
            };
            trim(s);
            // Strip leading qualifiers / tag keywords to reach the bare tag name.
            for (;;)
            {
                if (s.rfind("const ", 0) == 0)    { s.erase(0, 6); trim(s); continue; }
                if (s.rfind("volatile ", 0) == 0) { s.erase(0, 9); trim(s); continue; }
                if (s.rfind("struct ", 0) == 0)   { s.erase(0, 7); trim(s); continue; }
                if (s.rfind("union ", 0) == 0)    { s.erase(0, 6); trim(s); continue; }
                if (s.rfind("class ", 0) == 0)    { s.erase(0, 6); trim(s); continue; }
                if (s.rfind("enum ", 0) == 0)     return {};   // enum is scalar (int-sized)
                break;
            }
            for (size_t pos; (pos = s.find("::")) != std::string::npos;) s.replace(pos, 2, ".");
            return s;
        };

        std::vector<bool> needed(records.size(), false);
        std::vector<size_t> work;
        for (size_t i = 0; i < records.size(); ++i)
            if (records[i].inScope) { needed[i] = true; work.push_back(i); }

        auto seed = [&](const std::string& ctype) {
            std::string dep = byValueDep(ctype);
            if (dep.empty()) return;
            auto it = byName.find(dep);
            if (it == byName.end() || needed[it->second]) return;
            needed[it->second] = true;
            work.push_back(it->second);
        };
        for (const auto& sig : raw.sigs)
        {
            seed(sig.retType);
            for (const auto& pt : sig.paramTypes) seed(pt);
        }
        for (const auto& g : raw.globals) seed(g.ctype);

        while (!work.empty())
        {
            size_t i = work.back(); work.pop_back();
            for (const auto& f : records[i].fields)
            {
                std::string dep = byValueDep(f.ctype);
                if (dep.empty()) continue;
                auto it = byName.find(dep);
                if (it == byName.end() || needed[it->second]) continue;
                needed[it->second] = true;
                work.push_back(it->second);
            }
        }

        std::vector<cflat_cinterop::RawRecord> kept;
        kept.reserve(records.size());
        for (size_t i = 0; i < records.size(); ++i)
            if (needed[i]) kept.push_back(std::move(records[i]));
        records.swap(kept);
    }

void LLVMBackend::MapRawRecords(const cflat_cinterop::ExtractResult& raw, std::vector<CRecordEntry>& out)
{
        for (const auto& r : raw.records)
        {
            CRecordEntry rec;
            rec.name = r.name; rec.isUnion = r.isUnion;
            rec.isCxx = r.isCxx; rec.isPacked = r.isPacked;
            rec.isTriviallyCopyable = r.isTriviallyCopyable;
            rec.sizeBytes = r.sizeBytes; rec.alignBytes = r.alignBytes;
            rec.isTrivial = r.isTrivial;
            rec.line = r.line ? r.line : 1; rec.col = r.col < 0 ? 0 : r.col;
            rec.uuid = r.uuid;
            rec.isPolymorphic = r.isPolymorphic; rec.hasBases = r.hasBases;
            rec.hasVirtualBases = r.hasVirtualBases; rec.isAbstract = r.isAbstract;
            rec.bases = r.bases; rec.layoutRefusal = r.layoutRefusal;
            rec.canonicalCtype = r.canonicalCtype;
            rec.hasTrivialDefaultCtor = r.hasTrivialDefaultCtor;
            rec.hasTrivialCopyCtor = r.hasTrivialCopyCtor;
            rec.hasTrivialDtor = r.hasTrivialDtor;
            rec.hasDeletedDefaultCtor = r.hasDeletedDefaultCtor;
            rec.hasDeletedCopyCtor = r.hasDeletedCopyCtor;
            rec.hasDefaultCtor = r.hasDefaultCtor; rec.hasCopyCtor = r.hasCopyCtor;
            rec.isAggregate = r.isAggregate;
            rec.members = r.members;
            rec.staticVars = r.staticVars;
            for (const auto& f : r.fields)
            {
                CRecordFieldEntry fe;
                fe.name = f.name; fe.ctype = f.ctype;
                fe.access = f.access;
                fe.isBitfield = f.isBitfield; fe.bitWidth = f.bitWidth;
                fe.offsetBytes = f.offsetBytes;
                fe.sizeBytes = f.sizeBytes; fe.alignBytes = f.alignBytes;
                fe.bitOffset = f.bitOffset;
                rec.fields.push_back(std::move(fe));
            }
            out.push_back(std::move(rec));
        }
    }

bool LLVMBackend::HasComRecord(const std::vector<cflat_cinterop::RawRecord>& records)
{
        for (const auto& r : records)
            for (const auto& f : r.fields)
                if (f.name == "lpVtbl") return true;
        return false;
    }

void LLVMBackend::HarvestComUuids(const std::vector<std::string>& headerPaths, const std::string& primaryDir,
                         const std::vector<std::string>& extraDefines,
                         std::vector<cflat_cinterop::RawRecord>& records)
{
        std::string source;
        for (const auto& h : headerPaths)
        {
            std::string fwd = h;
            std::replace(fwd.begin(), fwd.end(), '\\', '/');
            source += "#include \"" + fwd + "\"\n";
        }

        cflat_cinterop::ExtractRequest req;
        req.mainFileName      = "cflat_uuid_stub.cpp";
        req.source            = source;
        req.args              = BuildClangDriverArgs(primaryDir, extraDefines, /*errorRecovery*/ true, /*asCxx*/ true);
        req.uuidHarvestCxx    = true;
        req.skipFunctionBodies = true;

        cflat_cinterop::ExtractResult uuidRaw;
        std::string err;
        if (!cflat_cinterop::ExtractCInterop(req, uuidRaw, err))
        {
            if (verbose) std::cout << std::format("[verbose]   COM uuid harvest failed: {}\n", err);
            return;
        }

        std::unordered_map<std::string, std::string> byName;
        for (const auto& r : uuidRaw.records)
            if (!r.name.empty() && !r.uuid.empty()) byName.emplace(r.name, r.uuid);
        size_t stamped = 0;
        for (auto& r : records)
        {
            if (!r.uuid.empty()) continue;
            if (auto it = byName.find(r.name); it != byName.end()) { r.uuid = it->second; ++stamped; }
        }
        if (verbose)
            std::cout << std::format("[verbose]   COM uuid harvest: {} interface IID(s) captured, {} stamped\n",
                byName.size(), stamped);
    }

bool LLVMBackend::ExtractCHeaderClang(const std::vector<std::string>& headerPaths,
                             std::vector<CSigEntry>& outSigs, std::vector<CEnumEntry>& outEnums,
                             std::vector<CRecordEntry>& outRecords,
                             std::vector<CMacroEntry>& outMacros,
                             std::vector<CFunctionMacroEntry>& outFuncMacros,
                             std::vector<CGlobalEntry>& outGlobals,
                             std::vector<std::pair<std::string, std::string>>& outAliases,
                             std::vector<CTypeAliasEntry>& outTypeAliases,
                             const std::vector<std::string>& extraDefines,
                             std::vector<std::string>* outIncludes,
                             bool* outPrereqFailure,
                             std::string* outPrereqMsg,
                             bool cxxMode,
                             std::string* outCxxBitcode,
                             std::vector<cflat_cinterop::RawFunctionPointerAbi>* outFunctionPointerAbis,
                             uint64_t* outLongDoubleWidth,
                             bool* outLongDoubleIsIEEEDouble,
                             std::string* outTargetTriple,
                             std::vector<cflat_cinterop::RawFunctionTemplate>* outFunctionTemplates)
{
        if (headerPaths.empty()) return false;

        // The first header's directory anchors the primary -I; also labels TimeTrace scopes
        // and attributes registered decls for LSP go-to-def.
        const std::string& headerPath = headerPaths.front();
        const std::string primaryDir = std::filesystem::path(headerPath).parent_path().string();
        /*
         * A system C++ header (libc++) is a TEMPLATE CATALOG, not a bound surface: walking it would
         * register thousands of implementation details and emit a whole standard library's inline
         * bodies. Nothing is bound at import time; the concrete types the program names are
         * requested one at a time (RequestCxxForeignType), which re-parses this same header set.
         */
        if (cxxMode && IsSystemCxxHeaderPath(headerPath))
        {
            cppInteropUsed_ = true;
            return true;
        }

        std::string source;
        for (const auto& h : headerPaths)
        {
            std::string fwd = h;
            std::replace(fwd.begin(), fwd.end(), '\\', '/');
            source += "#include \"" + fwd + "\"\n";
        }

        cflat_cinterop::ExtractRequest req;
        if (cxxMode) cppInteropUsed_ = true;
        req.mainFileName   = cxxMode ? "cflat_hdr_stub.cpp" : "cflat_hdr_stub.c";
        req.source         = source;
        req.args           = BuildClangDriverArgs(primaryDir, extraDefines, /*errorRecovery*/ true, cxxMode);
        req.cxxMode        = cxxMode;
        req.wantMacros     = true;
        req.requireInScope = true;
        /*
         * M5 - a C++ header bind needs the inline BODIES: they are the only definition of an
         * inline function, an all-inline method or the key function of a vtable, and Clang emits
         * them into the companion module the backend links in. A C header still binds
         * declarations only (faster, and it avoids parsing system-header inline bodies).
         * LSP analysis never runs CodeGen: the symbol index wants declarations, not IR.
         */
        req.emitDefinitions    = cxxMode && symbolSink_ == nullptr;
        req.assumeInlineDefinitions = cxxMode && !req.emitDefinitions;
        req.skipFunctionBodies = !req.emitDefinitions;
        req.wantIncludes   = (outIncludes != nullptr);

        // Expand um/<->shared/ siblings: the Windows SDK splits its surface across both and
        // code from MSDN fails without the sibling (ERROR_SUCCESS, MAX_PATH, etc.).
        auto addScopeDir = [&](const std::string& d) {
            for (const auto& e : req.inScopeDirs) if (e == d) return;
            req.inScopeDirs.push_back(d);
        };
        for (const auto& h : headerPaths)
        {
            std::filesystem::path hdrDirPath = std::filesystem::path(h).parent_path();
            addScopeDir(hdrDirPath.string());
            // macOS framework: sibling headers are spelled `.../X.framework/Headers/...` but the
            // umbrella's real_path is `.../X.framework/Versions/A/Headers/...`. Scope the whole
            // X.framework bundle so both spellings pass the in-scope filter.
            {
                auto pos = h.find(".framework/");
                if (pos != std::string::npos)
                    addScopeDir(h.substr(0, pos + std::string(".framework").size()));
            }
            std::string dirLeaf = hdrDirPath.filename().string();
            std::transform(dirLeaf.begin(), dirLeaf.end(), dirLeaf.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            if (dirLeaf == "um")
                addScopeDir((hdrDirPath.parent_path() / "shared").string());
            else if (dirLeaf == "shared")
                addScopeDir((hdrDirPath.parent_path() / "um").string());
        }
        for (const auto& inc : cIncludeDirs_) addScopeDir(inc);

        if (verbose)
        {
            std::cout << std::format("[verbose] extracting C header{}:", headerPaths.size() > 1 ? " group" : "");
            for (const auto& h : headerPaths) std::cout << " " << h;
            std::cout << " (clang C++ API)\n";
        }

        cflat_cinterop::ExtractResult raw;
        std::string err;
        if (!cflat_cinterop::ExtractCInterop(req, raw, err))
        {
            if (verbose) std::cout << std::format("[verbose]   C header extraction failed: {}\n", err);
            return false;
        }
        SetCInteropTargetFacts(raw);
        if (outLongDoubleWidth) *outLongDoubleWidth = raw.longDoubleWidth;
        if (outLongDoubleIsIEEEDouble)
            *outLongDoubleIsIEEEDouble = raw.longDoubleIsIEEEDouble;
        if (outTargetTriple) *outTargetTriple = raw.targetTriple;
        if (outFunctionTemplates) *outFunctionTemplates = raw.functionTemplates;

        // A definitions-enabled pass gives us the defaults without entering the extractor's
        // declaration-only path. Re-run only when wrappers are needed, adding their bodies to the
        // same synthetic TU so the returned companion contains both the header and forwarding
        // definitions.
        if (cxxMode && req.emitDefinitions)
        {
            const std::string wrappers = BuildCxxDefaultWrappers(raw.sigs);
            if (!wrappers.empty())
            {
                cflat_cinterop::ExtractRequest wrappedReq = req;
                const std::string scratchDir = std::filesystem::absolute("scratch").string();
                wrappedReq.mainFileName = (std::filesystem::path(scratchDir)
                    / "cflat_default_wrappers.cpp").string();
                wrappedReq.inScopeDirs.clear();
                wrappedReq.inScopeDirs.push_back(scratchDir);
                wrappedReq.source = source + wrappers;
                wrappedReq.wantMacros = false;
                wrappedReq.wantIncludes = false;
                cflat_cinterop::ExtractResult wrapped;
                std::string wrappedError;
                if (!cflat_cinterop::ExtractCInterop(wrappedReq, wrapped, wrappedError))
                {
                    if (verbose)
                        std::cout << std::format("[verbose]   default wrapper extraction failed: {}\n",
                                                 wrappedError);
                    return false;
                }
                for (const std::string& dropped : wrapped.droppedCxxDefaultWrappers)
                {
                    bool matched = false;
                    for (auto& sig : raw.sigs)
                    {
                        for (size_t n = 0; n < sig.defaultArgs.size(); ++n)
                        {
                            if (CxxDefaultWrapperName(sig.linkageName, n) != dropped) continue;
                            sig.defaultArgs[n].kind = "unsupported";
                            sig.defaultArgs[n].value.clear();
                            matched = true;
                            if (verbose)
                                std::cout << std::format(
                                    "[verbose]   dropped C++ default wrapper for '{}' ({})\n",
                                    sig.qualifiedName.empty() ? sig.name : sig.qualifiedName, dropped);
                        }
                    }
                    if (!matched && verbose)
                        std::cout << std::format("[verbose]   dropped C++ default wrapper '{}'\n", dropped);
                }
                std::string merged;
                if (raw.bitcode.empty())
                    raw.bitcode = std::move(wrapped.bitcode);
                else if (wrapped.bitcode.empty())
                    merged.clear();
                else if (!MergeCxxBitcode(raw.bitcode, wrapped.bitcode, merged))
                {
                    if (verbose)
                        std::cout << "[verbose]   could not merge default wrapper companion\n";
                    return false;
                }
                if (!merged.empty()) raw.bitcode = std::move(merged);
            }
        }

        // clang drops dependent decls on prereq errors, so registering the remnants would expose
        // a partial/wrong-sized API. Refuse and let the caller suggest a grouped import.
        if (raw.prereqErrors > 0)
        {
            if (outPrereqFailure) *outPrereqFailure = true;
            if (outPrereqMsg) *outPrereqMsg = raw.firstPrereqError;
            if (verbose)
                std::cout << std::format("[verbose]   header is not self-contained: {}\n", raw.firstPrereqError);
            return false;
        }

        if (outIncludes) *outIncludes = std::move(raw.includedFiles);
        if (outFunctionPointerAbis) *outFunctionPointerAbis = raw.functionPointerAbis;

        // Enum-backed widths are needed while mapping signatures and record fields, both of
        // which are processed before the enum constants are published below.
        for (const auto& re : raw.enums)
        {
            if (re.enumType.empty() || re.underlyingType.empty()) continue;
            TypeAndValue backing;
            if (MapCTypeToTypeAndValue(re.underlyingType, backing))
                RegisterEnumBackingType(re.enumType, backing.TypeName);
        }

        // Companion module: adopt it for this compile and hand it back so the header cache can
        // replay the identical bitcode on a warm run.
        if (!raw.bitcode.empty())
        {
            if (verbose)
                std::cout << std::format("[verbose]   C++ companion module: {} definition(s), {} bytes\n",
                                         raw.emittedDefinitions, raw.bitcode.size());
            AdoptCxxCompanionBitcode(raw.bitcode);
            if (outCxxBitcode != nullptr) *outCxxBitcode = std::move(raw.bitcode);
        }

        // Header-COM interfaces (a record carrying an `lpVtbl` field) hide their IID in the C++
        // MIDL_INTERFACE form the C parse above cannot see. When any are present, harvest the GUIDs
        // with a focused C++ parse and stamp them onto the matching C records so iidof() resolves.
        if (HasComRecord(raw.records))
            HarvestComUuids(headerPaths, primaryDir, extraDefines, raw.records);

        {
            llvm::TimeTraceScope adoptScope("AdoptTypedefs", headerPath);
            AdoptRawTypedefs(raw);
        }

        // Records first so struct-by-value param/return types resolve; pruned to the by-value
        // closure so dependency structs (e.g. POINT for MSG) are included but unrelated ones aren't.
        {
            llvm::TimeTraceScope recordScope("RegisterCRecords", headerPath);
            PruneRecordsToNeededClosure(raw);
            MapRawRecords(raw, outRecords);
            RegisterCRecords(outRecords, headerPath);
            // Surface `typedef struct Tag {...} Name;` as Name -> Tag aliases now that the tags
            // are registered, so the user can write MSG/RECT/WNDCLASSEXA, not tagMSG/...
            CollectRecordTypedefAliases(raw, outAliases);
            RegisterRecordAliases(outAliases);
            CollectTypeAliases(raw, outTypeAliases);
            if (cxxMode) RegisterTypeAliasSymbols(outTypeAliases);
            RegisterCxxFunctionPointerAbis(raw.functionPointerAbis);
        }

        {
            llvm::TimeTraceScope sigScope("MapSignatures", headerPath);
            // Enums register after signatures; name them so a signature never requests one.
            std::unordered_set<std::string> localEnums;
            for (const auto& re : raw.enums)
                if (!re.enumType.empty()) localEnums.insert(AutoCxxForeignIdentity(re.enumType));
            if (cxxMode && activeCxxRequestGroup_ != nullptr)
            {
                // Every specialization this header's signatures name, instantiated by ONE pair of
                // Clang frontends instead of two per spelling. The loop below then replays each
                // request off the cache the batch filled.
                PublishCxxGroupNames(activeCxxRequestGroup_->primary, outRecords);
                std::vector<CxxRequestItem> batch;
                for (const auto& rs : raw.sigs)
                {
                    if (rs.name.starts_with("__cflat_dflt_")) continue;
                    CollectCxxSignatureRequestItems(rs, &localEnums, batch);
                }
                PrewarmCxxRequestBatch(std::move(batch));
            }
            for (const auto& rs : raw.sigs)
            {
                if (cxxMode && rs.name.starts_with("__cflat_dflt_")) continue;
                cflat_cinterop::RawSig mappedRaw = rs;
                const bool requested = RequestCxxSignatureTypes(rs, &localEnums);
                if (requested && rs.isCxx && rs.abi.valid)
                    mappedRaw.bindRefusal.clear();
                CSigEntry e;
                if (MapRawSig(mappedRaw, e)) outSigs.push_back(std::move(e));
            }
        }

        {
            llvm::TimeTraceScope enumScope("MapEnums", headerPath);
        for (const auto& re : raw.enums)
        {
                CEnumEntry e;
                e.name = re.name; e.enumType = re.enumType; e.underlyingType = re.underlyingType;
                e.value = re.value;
                e.line = re.line ? re.line : 1; e.col = re.col < 0 ? 0 : re.col;
                outEnums.push_back(std::move(e));
            }
        }

        {
            llvm::TimeTraceScope macroScope("MapMacros", headerPath);
            for (const auto& rm : raw.macros)
            {
                CMacroEntry e;
                if (ClassifyRawMacro(rm, e)) outMacros.push_back(std::move(e));
            }
            for (const auto& rf : raw.funcMacros)
            {
                CFunctionMacroEntry e;
                e.name = rf.name; e.params = rf.params; e.body = rf.body;
                e.file = rf.file; e.line = rf.line ? rf.line : 1; e.col = rf.col < 0 ? 0 : rf.col;
                outFuncMacros.push_back(std::move(e));
            }
        }

        {
            llvm::TimeTraceScope globalScope("MapGlobals", headerPath);
            for (const auto& rg : raw.globals)
            {
                CGlobalEntry e;
                if (MapRawGlobal(rg, e)) outGlobals.push_back(std::move(e));
            }
        }

        if (verbose)
            std::cout << std::format("[verbose]   header bind: {} sig(s), {} enum(s), {} record(s), {} macro(s), {} func-macro(s), {} global(s)\n",
                outSigs.size(), outEnums.size(), outRecords.size(), outMacros.size(), outFuncMacros.size(), outGlobals.size());
        return true;
    }

/*
 * M5b - concrete C++ TYPE REQUESTS (class template specializations, and classes named through a
 * typedef such as `std::string`).
 *
 * Route: one synthetic "explicit instantiation" translation unit per requested type, made of the
 * import group's headers plus `template class std::vector<int>;` and a marker typedef. That needs
 * no long-lived Sema, keeps Clang confined to CClangExtract.cpp, and reuses the whole one-shot
 * extraction path (RawRecord / RawCxxMember / RawAbi, CodeGen into a companion module).
 *
 * The plan's design (internal/plan/cpp-direct-abi-interop.md M5) is a LIVE Sema session per import
 * group that instantiates through Sema::RequireCompleteType; that remains the eventual home. What
 * this route does NOT cover: function templates deduced at a call site (there is no type to name),
 * and a specialization requested from LSP analysis, which never runs CodeGen and therefore only
 * binds what an inline definition can supply.
 *
 * Two stages are needed because libc++ marks nearly every member
 * `__attribute__((exclude_from_explicit_instantiation))`: the explicit instantiation alone
 * instantiates the CLASS but no member BODY, and by the time our consumer runs Sema is past
 * end-of-TU, so nothing can be instantiated on demand. Stage 1 extracts the member list and their
 * canonical signatures; stage 2 re-parses the same TU with one ODR-USE per member appended (a
 * pointer-to-member cast for a method, a placement-new wrapper for a constructor, a
 * pseudo-destructor call for the destructor), which is what makes Sema instantiate the bodies that
 * CodeGen then emits into the companion module.
 */

bool LLVMBackend::IsCxxForeignTypeRegistered(const std::string& cflatName) const
{
        return cxxCflatToCxxSpelling_.find(cflatName) != cxxCflatToCxxSpelling_.end();
}

// CFlat type argument -> C++ spelling. Primitives by width, a previously requested foreign type by
// its own spelling, one trailing pointer level as a pointer. Anything else (a CFlat struct, a CFlat
// generic instantiation) has no C++ identity and is refused by the caller.
bool LLVMBackend::CxxSpellingForCflatType(const std::string& cflatType, std::string& out) const
{
        std::string base = cflatType;
        int ptr = 0;
        while (!base.empty() && base.back() == '*') { base.pop_back(); ++ptr; }
        if (!base.empty() && (std::isdigit((unsigned char)base.front())
                              || (base.front() == '-' && base.size() > 1)))
        {
            bool integer = true;
            for (size_t i = base.front() == '-' ? 1 : 0; i < base.size(); ++i)
                if (!std::isdigit((unsigned char)base[i])) { integer = false; break; }
            if (integer) { out = base; for (int i = 0; i < ptr; ++i) out += " *"; return true; }
        }
        if (ptr == 0)
        {
            if (auto dot = base.rfind('.'); dot != std::string::npos)
            {
                int64_t value = 0;
                if (TryGetEnumMemberInt(base.substr(0, dot), base.substr(dot + 1), value))
                {
                    out = std::to_string(value);
                    return true;
                }
            }
        }
        static const std::unordered_map<std::string, std::string> prims = {
            { "bool", "bool" }, { "char", "char" },
            { "i8", "signed char" }, { "u8", "unsigned char" },
            { "short", "short" }, { "i16", "short" }, { "u16", "unsigned short" },
            { "int", "int" }, { "i32", "int" }, { "uint", "unsigned int" }, { "u32", "unsigned int" },
            { "long", "long long" }, { "i64", "long long" },
            { "ulong", "unsigned long long" }, { "u64", "unsigned long long" },
            { "float", "float" }, { "double", "double" }, { "void", "void" },
        };
        auto enumKey = ResolveEnumTypeName(base);
        if (!enumKey.empty()) base = GetEnumBackingType(enumKey);
        auto closureIt = encodedClosureTypes_.find(base);
        if (closureIt != encodedClosureTypes_.end())
        {
            const auto& sig = closureIt->second;
            auto component = [&](const std::string& name, bool pointer, int depth,
                                 std::string& spelling) {
                if (!CxxSpellingForCflatType(name, spelling)) return false;
                int levels = pointer ? std::max(depth, 1) : 0;
                for (int i = 0; i < levels; ++i) spelling += " *";
                return true;
            };
            std::string ret;
            if (!component(sig.FuncPtrReturnTypeName, sig.FuncPtrReturnPointer,
                           sig.FuncPtrReturnPointerDepth, ret)) return false;
            out = ret + " (";
            for (size_t i = 0; i < sig.FuncPtrParams.size(); ++i)
            {
                if (i != 0) out += ", ";
                std::string param;
                const auto& p = sig.FuncPtrParams[i];
                if (!component(p.TypeName, p.Pointer, p.PointerDepth, param)) return false;
                out += param;
            }
            out += ")";
        }
        else if (auto it = prims.find(base); it != prims.end()) out = it->second;
        else if (auto fit = cxxCflatToCxxSpelling_.find(base); fit != cxxCflatToCxxSpelling_.end())
            out = fit->second;
        else if (auto dot = base.find('.'); dot != std::string::npos
                 && cxxForeignNamespaces_.count(base.substr(0, dot)) != 0
                 && dataStructures.count(base) == 0)
        {
            // A dotted name under an imported C++ namespace that no request has registered yet:
            // spell it as clang would read it and let the request's TU resolve it. This is how a
            // NAMESPACE ALIAS (`simdjson::ondemand` for `simdjson::arm64::ondemand`) works as a
            // template argument; the canonical result aliases onto the registration.
            out = base;
            for (size_t pos = 0; (pos = out.find('.', pos)) != std::string::npos; pos += 2)
                out.replace(pos, 1, "::");
        }
        else return false;
        for (int i = 0; i < ptr; ++i) out += " *";
        return true;
    }

/*
 * The stub's include prologue plus one marker typedef per requested spelling, shared by both
 * stages. The prologue includes ONE import group's headers, never every C++ header imported so
 * far: an import that comes earlier in the file must not change the translation unit a later
 * import's request is instantiated in.
 */
std::string LLVMBackend::BuildCxxRequestPrologue(const CxxRequestGroup& group,
                                                 const std::vector<CxxRequestItem>& items,
                                                 bool instantiateAll) const
{
        std::string src = "#include <new>\n";
        for (const auto& h : group.headers)
        {
            std::string fwd = h;
            std::replace(fwd.begin(), fwd.end(), '\\', '/');
            if (IsSystemCxxHeaderPath(fwd))
                src += "#include <" + std::filesystem::path(fwd).filename().string() + ">\n";
            else
                src += "#include \"" + fwd + "\"\n";
        }
        for (size_t i = 0; i < items.size(); ++i)
        {
            src += "typedef " + items[i].cxxSpelling + " __cflat_req_" + std::to_string(i) + ";\n";
            if ((instantiateAll || items[i].explicitInstantiation)
                && items[i].cxxSpelling.find('<') != std::string::npos)
                src += "template class " + items[i].cxxSpelling + ";\n";
        }
        return src;
    }

/*
 * One ODR-USE per exported member of the stage-1 record, so Sema instantiates the bodies.
 * Each use is its own top-level declaration: a member whose signature cannot be re-spelled (a
 * ref-qualified overload, a default argument that does not survive) errors on its own line, is
 * error-recovered, and leaves every other member emitted.
 */
std::string LLVMBackend::BuildCxxRequestOdrUses(const cflat_cinterop::RawRecord& rec,
                                               const std::string& marker,
                                               const std::string& tagPrefix) const
{
        using Member = cflat_cinterop::RawCxxMember;
        std::string src;
        if (!rec.hasTrivialDtor)
            src = "static void __cflat_use_dtor" + tagPrefix + "(" + marker + "* p) { p->~" + marker + "(); }\n";
        unsigned n = 0;
        for (const auto& m : rec.members)
        {
            if (m.access != cflat_cinterop::AccessPublic || m.isDeleted || m.variadic) continue;
            if (m.isImplicit && m.kind != Member::Constructor) continue;
            if (m.isDefaulted && m.kind != Member::Constructor) continue;
            if (m.kind == Member::Destructor) continue;   // covered above
            const std::string tag = tagPrefix + std::to_string(n++);
            if (m.kind == Member::Constructor)
            {
                // A constructor has no address; construct into raw storage instead. The wrapper's
                // own parameters carry the canonical spellings, so a reference parameter stays a
                // reference and an rvalue reference is re-cast on the way in.
                std::string params, args;
                for (size_t p = 1; p < m.paramTypes.size(); ++p)
                {
                    if (p > 1) { params += ", "; args += ", "; }
                    params += m.paramTypes[p] + " a" + std::to_string(p);
                    args += m.paramTypes[p].size() > 2
                            && m.paramTypes[p].compare(m.paramTypes[p].size() - 2, 2, "&&") == 0
                        ? "static_cast<" + m.paramTypes[p] + ">(a" + std::to_string(p) + ")"
                        : "a" + std::to_string(p);
                }
                src += std::string(m.isCopyCtor || m.isMoveCtor
                                       ? "__attribute__((used, noinline)) " : "")
                     + "static void __cflat_use_ctor" + tag + "(void* m"
                     + (params.empty() ? "" : ", " + params) + ") { (void)::new (m) " + marker + "("
                     + args + "); }\n";
                continue;
            }
            std::string params;
            for (size_t p = (m.kind == Member::StaticMethod ? 0u : 1u); p < m.paramTypes.size(); ++p)
            {
                if (!params.empty()) params += ", ";
                params += m.paramTypes[p];
            }
            const std::string ptrTo = m.kind == Member::StaticMethod
                ? "(*)" : "(" + marker + "::*)";
            src += "static auto __cflat_use" + tag + " = static_cast<" + m.retType + " " + ptrTo
                 + "(" + params + ")" + (m.isConst ? " const" : "") + ">(&" + marker + "::"
                 + m.name + ");\n";
        }
        bool iteratorLike = false;
        for (const auto& m : rec.members)
            iteratorLike = iteratorLike || m.name == "operator++" || m.name == "operator*"
                                        || m.name == "operator->";
        if (iteratorLike)
        {
            // Iterator equality and ordering are commonly non-member function templates found by
            // ADL. The expressions instantiate those overloads so the ordinary free-function
            // extractor can publish them alongside the requested class.
            src += "static void __cflat_use_adl_ops" + tagPrefix + "(" + marker + "* a, "
                 + marker + "* b) { ";
            src += "(void)(*a == *b); (void)(*a != *b); (void)(*a < *b); ";
            src += "}\n";
        }
        // The SFINAE probes themselves live in the TU preamble (one copy per translation unit);
        // only the per-spelling entry points are emitted here.
        src += "static void __cflat_use_member_templates" + tagPrefix + "(" + marker + "* p) { "
               "__cflat_use_value_members_impl(p, 0); }\n";
        src += "static void __cflat_use_inherited_members" + tagPrefix + "(" + marker + "* p) { "
               "__cflat_use_optional_members_impl(p, 0); }\n";
        return src;
}

/*
 * Probes shared by every spelling in one request translation unit: optional-like member templates
 * reached through dependent SFINAE, so a requested class without value_type or has_value still
 * gets its emitted definitions and its cache entry.
 */
static std::string CxxRequestOdrUsePreamble()
{
        std::string src;
        src += "template <typename T> static auto __cflat_use_value_members_impl(T* p, int) -> "
               "decltype((void)p->value_or(typename T::value_type{}), "
               "(void)p->emplace(typename T::value_type{}), void()) { ";
        src += "(void)p->value_or(typename T::value_type{}); "
               "(void)p->emplace(typename T::value_type{}); }\n";
        src += "template <typename T> static void __cflat_use_value_members_impl(T*, long) {}\n";
        src += "template <typename T> static auto __cflat_use_optional_members_impl(T* p, int) -> "
               "decltype((void)p->has_value(), (void)p->reset(), (void)p->value(), void()) { ";
        src += "(void)p->has_value(); p->reset(); (void)p->value(); }\n";
        src += "template <typename T> static void __cflat_use_optional_members_impl(T*, long) {}\n";
        return src;
}

static std::string BuildStdFunctionCtorUse(const std::string& cxxSpelling,
                                           const std::string& marker)
{
        std::string ret;
        std::string params;
        if (!cflat_cinterop::SplitStdFunctionSpelling(cxxSpelling, ret, params)) return {};
        std::string helper = "__cflat_std_function_ctor_";
        for (char c : cxxSpelling)
            helper += std::isalnum((unsigned char)c) ? c : '_';
        return "extern \"C\" void " + helper + "(void* m, " + ret
             + " (*a1)(" + params + ")) { (void)::new (m) " + marker + "(a1); }\n";
}

bool LLVMBackend::RunCxxTypeRequests(const CxxRequestGroup& group,
                                     const std::vector<CxxRequestItem>& items,
                                     const std::string& extraSource, bool emitDefinitions,
                                     cflat_cinterop::ExtractResult& raw, std::string& error)
{
        cflat_cinterop::ExtractRequest req;
        req.mainFileName = "cflat_cpp_request.cpp";
        req.cxxMode = true;
        req.source = BuildCxxRequestPrologue(group, items, /*instantiateAll*/ !emitDefinitions)
                   + extraSource;
        req.emitDefinitions = emitDefinitions;
        req.assumeInlineDefinitions = !emitDefinitions;
        req.skipFunctionBodies = false;
        req.requireInScope = false;
        for (const CxxRequestItem& item : items)
            req.cxxTypeRequests.push_back({ item.cxxSpelling, item.cflatName });
        std::string primaryDir;
        for (const auto& h : group.headers)
            if (!IsSystemCxxHeaderPath(h))
            { primaryDir = std::filesystem::path(h).parent_path().string(); break; }
        req.args = BuildClangDriverArgs(primaryDir, group.defines, /*errorRecovery*/ true,
                                        /*asCxx*/ true);
        return cflat_cinterop::ExtractCInterop(req, raw, error);
    }

/*
 * Identity of a C++ type request: the OWNING import group's headers and defines (never every C++
 * header imported so far), the C++ include dirs, the instantiation spelling, the emit mode (an LSP
 * bind carries no bodies and an empty companion module, which a compile must never reuse), and the
 * compiler build stamp. Shares the C header signature cache, so it shares its row budget and its
 * LRU/root pinning.
 */
std::string LLVMBackend::CxxTypeRequestCacheKey(const CxxRequestGroup& group,
                                                const std::string& cxxSpelling) const
{
        std::string key = "|RQ" + cxxSpelling;
        for (const auto& h : group.headers)     key += "|H" + h;
        for (const auto& inc : cIncludeDirs_)   key += "|I" + inc;
        for (const auto& def : cDefines_)       key += "|D" + def;
        for (const auto& def : group.defines)   key += "|d" + def;
        key += symbolSink_ == nullptr ? "|EDEF" : "|EDECL";
        key += "|M13F";
        key += "|C" + CompilerBuildStamp();
        return key;
    }

std::string LLVMBackend::CxxTypeRequestCacheKey(const CxxRequestGroup& group,
                                                const CxxRequestItem& item) const
{
        return CxxTypeRequestCacheKey(group, item.cxxSpelling)
             + (item.needDefinitions ? "|FULL" : "|LAYOUT")
             + (item.explicitInstantiation ? "|INST" : "|NOINST");
    }

// Newest mtime over the group's headers; false when any of them cannot be stat'ed.
bool LLVMBackend::CxxGroupHeaderStamp(const CxxRequestGroup& group,
                                      std::filesystem::file_time_type& newest) const
{
        newest = std::filesystem::file_time_type{};
        for (const auto& h : group.headers)
        {
            std::error_code ec;
            auto mt = std::filesystem::last_write_time(h, ec);
            if (ec) return false;
            if (mt > newest) newest = mt;
        }
        return true;
    }

uint64_t LLVMBackend::CxxGroupHeaderHash(const CxxRequestGroup& group) const
{
        uint64_t combined = 14695981039346656037ULL;
        for (const auto& h : group.headers)
        {
            uint64_t one = 0;
            HashFileContents(h, one);
            combined ^= one; combined *= 1099511628211ULL;
        }
        return combined;
    }

/*
 * One `import cpp` statement, identified by its ordered headers plus its defines - the same tuple
 * the header disk cache hashes. A second import of the same headers with the same defines is the
 * same group, so a repeated import does not split the request cache.
 */
size_t LLVMBackend::FindOrAddCxxImportGroup(const std::vector<std::string>& headers,
                                            const std::vector<std::string>& defines)
{
        for (size_t i = 0; i < cxxImportGroups_.size(); ++i)
            if (cxxImportGroups_[i].headers == headers && cxxImportGroups_[i].defines == defines)
                return i;
        CxxImportGroup group;
        group.headers = headers;
        group.defines = defines;
        cxxImportGroups_.push_back(std::move(group));
        return cxxImportGroups_.size() - 1;
    }

/*
 * The primary group's headers first, then the headers of the groups that own the requested
 * spelling's template arguments, appended in a canonical (sorted) order so the prologue and the
 * cache key do not depend on the order the file imported them in.
 */
LLVMBackend::CxxRequestGroup LLVMBackend::MakeCxxRequestGroup(size_t primary,
                                                              const std::vector<size_t>& deps) const
{
        CxxRequestGroup out;
        if (primary >= cxxImportGroups_.size()) return out;
        out.primary = primary;
        out.headers = cxxImportGroups_[primary].headers;
        out.defines = cxxImportGroups_[primary].defines;
        std::vector<size_t> sorted;
        for (size_t d : deps)
            if (d != primary && d < cxxImportGroups_.size()
                && std::find(sorted.begin(), sorted.end(), d) == sorted.end())
                sorted.push_back(d);
        std::sort(sorted.begin(), sorted.end(), [&](size_t a, size_t b) {
            const auto& ha = cxxImportGroups_[a].headers;
            const auto& hb = cxxImportGroups_[b].headers;
            if (ha.empty() || hb.empty()) return hb.empty() && !ha.empty();
            return ha.front() < hb.front();
        });
        for (size_t d : sorted)
        {
            for (const auto& h : cxxImportGroups_[d].headers)
                if (std::find(out.headers.begin(), out.headers.end(), h) == out.headers.end())
                    out.headers.push_back(h);
            for (const auto& def : cxxImportGroups_[d].defines)
                if (std::find(out.defines.begin(), out.defines.end(), def) == out.defines.end())
                    out.defines.push_back(def);
        }
        out.label = out.headers.empty()
            ? std::string("<no header>")
            : std::filesystem::path(out.headers.front()).filename().string();
        return out;
    }

// Qualified C++ names an import group brought in, so a later CFlat spelling resolves without a probe.
void LLVMBackend::PublishCxxGroupNames(size_t group, const std::vector<CRecordEntry>& records)
{
        if (group >= cxxImportGroups_.size()) return;
        CxxImportGroup& g = cxxImportGroups_[group];
        for (const CRecordEntry& record : records)
        {
            if (record.name.empty()) continue;
            std::string dotted = record.name;
            const size_t mangled = dotted.find('$');
            if (mangled != std::string::npos) dotted.erase(mangled);
            std::string cxxName = dotted;
            size_t pos = 0;
            while ((pos = cxxName.find('.', pos)) != std::string::npos)
            { cxxName.replace(pos, 1, "::"); pos += 2; }
            g.publishedNames.insert(cxxName);
            cxxTypeOwnerGroup_.emplace(record.name, group);
    }
}

void LLVMBackend::RegisterCxxFunctionTemplates(
        const std::vector<cflat_cinterop::RawFunctionTemplate>& templates, size_t group,
        const std::string& fileForLsp)
{
        for (const auto& t : templates)
        {
            if (t.name.empty() || t.cxxSpelling.empty()) continue;
            auto& entries = cxxFunctionTemplates_[t.name];
            const bool duplicate = std::any_of(entries.begin(), entries.end(), [&](const auto& old) {
                return old.kind == t.kind && old.cxxSpelling == t.cxxSpelling
                    && old.minArity == t.minArity && old.maxArity == t.maxArity
                    && old.typeParameterCount == t.typeParameterCount;
            });
            if (duplicate) continue;
            entries.push_back(t);
            if (group < cxxImportGroups_.size())
                cxxFunctionTemplateOwnerGroup_.emplace(t.name, group);
            if (!t.owner.empty())
            {
                std::string ownerCxx = "::" + t.owner;
                for (size_t pos = 0; (pos = ownerCxx.find('.', pos)) != std::string::npos; )
                {
                    ownerCxx.replace(pos, 1, "::");
                    pos += 2;
                }
                cxxCflatToCxxSpelling_.emplace(t.owner, ownerCxx);
                cxxForeignTypeSpellings_.emplace(SqueezeCxxSpelling(ownerCxx), t.owner);
            }
            NoteCxxForeignNamespace(t.name);
            if (auto* sink = GetSymbolSink())
            {
                const std::string& declFile = t.file.empty() ? fileForLsp : t.file;
                sink->Register(SymbolKind::Function, t.name, declFile,
                               t.line, t.col < 0 ? 0 : t.col,
                               "template <...> " + t.name + "(...)");
            }
        }
}

bool LLVMBackend::HasCxxFunctionTemplate(const std::string& qualifiedName) const
{
        auto it = cxxFunctionTemplates_.find(qualifiedName);
        if (it != cxxFunctionTemplates_.end() && !it->second.empty()) return true;
        const size_t dot = qualifiedName.rfind('.');
        return dot != std::string::npos
            && !ResolveCxxFunctionTemplateName(qualifiedName.substr(0, dot),
                                               qualifiedName.substr(dot + 1)).empty();
}

std::string LLVMBackend::ResolveCxxFunctionTemplateName(const std::string& owner,
                                                        const std::string& memberName) const
{
        const std::string exact = owner + "." + memberName;
        auto exactIt = cxxFunctionTemplates_.find(exact);
        if (exactIt != cxxFunctionTemplates_.end() && !exactIt->second.empty()) return exact;

        std::string ownerSpelling;
        if (!CxxSpellingForCflatType(owner, ownerSpelling))
            if (auto lazy = cxxLazyAliasSpecializations_.find(owner);
                lazy != cxxLazyAliasSpecializations_.end())
                ownerSpelling = lazy->second;
        if (ownerSpelling.empty()) return {};
        const size_t templateStart = ownerSpelling.find('<');
        if (templateStart != std::string::npos) ownerSpelling.resize(templateStart);
        while (ownerSpelling.starts_with("::")) ownerSpelling.erase(0, 2);
        for (size_t pos = 0; (pos = ownerSpelling.find("::", pos)) != std::string::npos; )
        {
            ownerSpelling.replace(pos, 2, ".");
            ++pos;
        }
        const std::string resolved = ownerSpelling + "." + memberName;
        auto resolvedIt = cxxFunctionTemplates_.find(resolved);
        return resolvedIt != cxxFunctionTemplates_.end() && !resolvedIt->second.empty()
            ? resolved : std::string{};
}

bool LLVMBackend::HasCxxFunctionTemplateMember(const std::string& owner,
                                               const std::string& memberName) const
{
        return !ResolveCxxFunctionTemplateName(owner, memberName).empty();
}

bool LLVMBackend::RequestCxxFunctionTemplate(const std::string& functionName,
                                             const std::string& ownerType,
                                             const std::vector<std::string>& explicitArgs,
                                             const std::vector<NamedVariable>& arguments,
                                             std::string& error)
{
        error.clear();
        const std::string lookupName = ownerType.empty()
            ? functionName : ownerType + "." + functionName;
        std::string templateName = lookupName;
        if (!ownerType.empty())
            if (std::string resolved = ResolveCxxFunctionTemplateName(ownerType, functionName);
                !resolved.empty())
                templateName = std::move(resolved);
        auto templatesIt = cxxFunctionTemplates_.find(templateName);
        if (templatesIt == cxxFunctionTemplates_.end()) return false;

        auto cflatTypeOf = [&](const NamedVariable& arg) {
            std::string type = arg.TypeAndValue.TypeName;
            bool pointer = arg.TypeAndValue.Pointer;
            llvm::Type* valueType = arg.Primary != nullptr ? arg.Primary->getType() : arg.BaseType;
            if (type.empty())
            {
                if (auto* constant = llvm::dyn_cast_or_null<llvm::Constant>(arg.Primary);
                    constant != nullptr && IsStringLiteralConstant(constant))
                {
                    type = "char";
                    pointer = true;
                }
                else if (valueType != nullptr && valueType->isFloatTy()) type = "float";
                else if (valueType != nullptr && valueType->isDoubleTy()) type = "double";
                else if (valueType != nullptr && valueType->isIntegerTy())
                {
                    const unsigned bits = valueType->getIntegerBitWidth();
                    type = llvm::dyn_cast_or_null<llvm::ConstantInt>(arg.Primary) != nullptr
                        ? (bits > 32 ? "i64" : "int")
                        : (bits == 1 ? "bool" : bits <= 8 ? "i8" : bits <= 16 ? "short"
                           : bits <= 32 ? "int" : "i64");
                }
                else if (auto* st = llvm::dyn_cast_or_null<llvm::StructType>(valueType))
                    type = st->getName().str();
                if (type.empty()) type = arg.InferSourceTypeName;
            }
            if (pointer)
            {
                type += "*";
                if (arg.TypeAndValue.ElemPointer) type += "*";
            }
            return type;
        };
        auto displayTypeOf = [&](const NamedVariable& arg) {
            std::string type = cflatTypeOf(arg);
            return type.empty() ? std::string("<unknown>") : type;
        };
        std::string argumentDisplay;
        for (size_t i = 0; i < arguments.size(); ++i)
        {
            if (i != 0) argumentDisplay += ", ";
            argumentDisplay += displayTypeOf(arguments[i]);
        }
        auto noMatch = [&](const std::string& extra = std::string()) {
            error = std::format("no instantiation of C++ function template '{}' accepts these "
                                "argument types ({})", lookupName, argumentDisplay);
            if (!extra.empty()) error += " (clang: " + extra + ")";
            return false;
        };

        const cflat_cinterop::RawFunctionTemplate* selected = nullptr;
        for (const auto& candidate : templatesIt->second)
        {
            const bool instance = candidate.kind == cflat_cinterop::RawFunctionTemplate::InstanceMember;
            const bool kindMatches = ownerType.empty()
                ? candidate.kind == cflat_cinterop::RawFunctionTemplate::Free
                    || candidate.kind == cflat_cinterop::RawFunctionTemplate::StaticMember
                : candidate.kind == cflat_cinterop::RawFunctionTemplate::InstanceMember
                    || candidate.kind == cflat_cinterop::RawFunctionTemplate::StaticMember;
            if (!kindMatches || explicitArgs.size() > candidate.typeParameterCount) continue;
            if (instance && arguments.empty()) continue;
            const unsigned arity = (unsigned)arguments.size() - (instance ? 1u : 0u);
            if (arity < candidate.minArity || arity > candidate.maxArity) continue;
            selected = &candidate;
            break;
        }
        if (selected == nullptr) return noMatch();

        std::string ownerSpelling;
        if (selected->kind != cflat_cinterop::RawFunctionTemplate::Free)
        {
            if (!CxxSpellingForCflatType(ownerType, ownerSpelling)
                && !CxxSpellingForCflatType(selected->owner, ownerSpelling))
                return noMatch("the C++ receiver type is not registered");
        }
        std::vector<std::string> parameterSpellings;
        std::vector<std::string> callArguments;
        if (selected->kind == cflat_cinterop::RawFunctionTemplate::InstanceMember)
        {
            const NamedVariable& receiver = arguments.front();
            std::string receiverSpelling = ownerSpelling;
            if (receiver.TypeAndValue.Pointer)
            {
                receiverSpelling = (selected->isConst ? "const " : "") + receiverSpelling + " *";
                callArguments.push_back("p0->" + selected->memberName);
            }
            else
            {
                receiverSpelling = (selected->isConst ? "const " : "") + receiverSpelling + " &";
                callArguments.push_back("p0." + selected->memberName);
            }
            parameterSpellings.push_back(std::move(receiverSpelling));
        }
        for (size_t i = selected->kind == cflat_cinterop::RawFunctionTemplate::InstanceMember ? 1u : 0u;
             i < arguments.size(); ++i)
        {
            const NamedVariable& arg = arguments[i];
            std::string cflatType = cflatTypeOf(arg);
            if (cflatType.empty()) return noMatch("an argument type cannot be spelled in C++");
            std::string spelling;
            const bool stringLiteral = [&] {
                auto* constant = llvm::dyn_cast_or_null<llvm::Constant>(arg.Primary);
                return constant != nullptr && IsStringLiteralConstant(constant);
            }();
            if ((arg.TypeAndValue.TypeName == "char" && arg.TypeAndValue.Pointer && arg.IsRvalue)
                || stringLiteral)
                spelling = "const char *";
            else if (!CxxSpellingForCflatType(cflatType, spelling))
                return noMatch("an argument type cannot be spelled in C++");
            if (!arg.TypeAndValue.Pointer && dataStructures.count(arg.TypeAndValue.TypeName) != 0
                && arg.Storage != nullptr && !arg.IsRvalue)
                spelling += " &";
            parameterSpellings.push_back(std::move(spelling));
            callArguments.push_back("p" + std::to_string(i));
        }

        std::vector<std::string> cxxExplicitArgs;
        for (const std::string& typeArg : explicitArgs)
        {
            std::string spelling;
            if (!CxxSpellingForCflatType(typeArg, spelling))
                return noMatch("an explicit type argument cannot be spelled in C++");
            cxxExplicitArgs.push_back(std::move(spelling));
        }

        std::string explicitSuffix;
        if (!cxxExplicitArgs.empty())
        {
            explicitSuffix = "<";
            for (size_t i = 0; i < cxxExplicitArgs.size(); ++i)
            {
                if (i != 0) explicitSuffix += ", ";
                explicitSuffix += cxxExplicitArgs[i];
            }
            explicitSuffix += ">";
        }
        const bool instance = selected->kind == cflat_cinterop::RawFunctionTemplate::InstanceMember;
        std::string targetCall;
        if (instance)
            targetCall = callArguments.front() + explicitSuffix + "(";
        else
        {
            const std::string staticTarget = selected->kind
                == cflat_cinterop::RawFunctionTemplate::StaticMember
                ? ownerSpelling + "::" + selected->memberName : selected->cxxSpelling;
            targetCall = staticTarget + explicitSuffix + "(";
        }
        for (size_t i = instance ? 1u : 0u; i < callArguments.size(); ++i)
        {
            if (i != (instance ? 1u : 0u)) targetCall += ", ";
            targetCall += callArguments[i];
        }
        targetCall += ")";

        uint64_t hash = 14695981039346656037ULL;
        auto hashText = [&](const std::string& text) {
            for (unsigned char c : text) { hash ^= c; hash *= 1099511628211ULL; }
        };
        hashText(lookupName);
        hashText(std::to_string(selected->kind));
        for (const auto& p : parameterSpellings) hashText(p);
        for (const auto& a : cxxExplicitArgs) hashText(a);
        const std::string wrapperName = std::format("__cflat_tpl_{:016x}", hash);
        std::string wrapperSource = "extern \"C\" auto " + wrapperName + "(";
        for (size_t i = 0; i < parameterSpellings.size(); ++i)
        {
            if (i != 0) wrapperSource += ", ";
            wrapperSource += parameterSpellings[i] + " p" + std::to_string(i);
        }
        wrapperSource += ")";
        if (selected->isNoexcept) wrapperSource += " noexcept";
        wrapperSource += " { return " + targetCall + "; }\n";

        auto groupIt = cxxFunctionTemplateOwnerGroup_.find(selected->name);
        if (groupIt == cxxFunctionTemplateOwnerGroup_.end())
            return noMatch("the template's import group is unavailable");
        CxxRequestGroup group = MakeCxxRequestGroup(groupIt->second, {});
        if (group.headers.empty()) return noMatch("the template's import group is unavailable");
        CxxRequestGroupScope groupScope(*this, &group);

        const std::string requestKey = CxxTypeRequestCacheKey(group, wrapperSource) + "|TPL|FULL";
        const bool emitDefinitions = symbolSink_ == nullptr;
        std::vector<CSigEntry> requestSigs;
        std::string requestBitcode;
        bool cached = false;
        std::filesystem::file_time_type headerMtime{};
        const bool haveMtime = CxxGroupHeaderStamp(group, headerMtime);
        if (haveMtime)
        {
            std::lock_guard<std::mutex> lock(cFileSigCacheMutex_);
            auto it = cFileSigCache_.find(requestKey);
            if (it != cFileSigCache_.end()
                && (it->second.mtime == headerMtime || it->second.hash == CxxGroupHeaderHash(group)))
            {
                it->second.mtime = headerMtime;
                TouchCFileSigEntry(requestKey, it->second);
                requestSigs = it->second.sigs;
                requestBitcode = it->second.cxxBitcode;
                SetCInteropTargetFacts(it->second.longDoubleWidth,
                                       it->second.longDoubleIsIEEEDouble, it->second.targetTriple);
                cached = true;
            }
        }
        if (!cached)
        {
            auto runRequest = [&](bool emit, cflat_cinterop::ExtractResult& out,
                                  std::string& runError) {
                cflat_cinterop::ExtractRequest req;
                req.mainFileName = (std::filesystem::absolute("scratch")
                                    / (wrapperName + ".cpp")).string();
                req.source = BuildCxxRequestPrologue(group, {}, false) + wrapperSource;
                req.cxxMode = true;
                req.emitDefinitions = emit;
                req.assumeInlineDefinitions = !emit;
                req.skipFunctionBodies = false;
                req.requireInScope = false;
                req.cxxFunctionWrapperNames = { wrapperName };
                std::string primaryDir;
                for (const auto& h : group.headers)
                    if (!IsSystemCxxHeaderPath(h))
                    { primaryDir = std::filesystem::path(h).parent_path().string(); break; }
                req.args = BuildClangDriverArgs(primaryDir, group.defines, true, true);
                return cflat_cinterop::ExtractCInterop(req, out, runError);
            };
            cflat_cinterop::ExtractResult probe;
            std::string probeError;
            if (!runRequest(false, probe, probeError))
                return noMatch(probeError);
            auto findWrapper = [&](const cflat_cinterop::ExtractResult& raw)
                -> const cflat_cinterop::RawSig* {
                for (const auto& sig : raw.sigs)
                    if (sig.name == wrapperName || sig.linkageName == wrapperName) return &sig;
                return nullptr;
            };
            const auto* probeSig = findWrapper(probe);
            if (probeSig == nullptr || !probeSig->abi.valid || !probeSig->bindRefusal.empty()
                || !probe.firstError.empty())
                return noMatch(probe.firstError.empty() ? probeError : probe.firstError);
            if (!RequestCxxSignatureTypes(*probeSig))
                return noMatch("the deduced return type is not supported by C++ interop");

            cflat_cinterop::ExtractResult raw = std::move(probe);
            if (emitDefinitions)
            {
                cflat_cinterop::ExtractResult emitted;
                std::string emittedError;
                if (!runRequest(true, emitted, emittedError))
                    return noMatch(emittedError);
                const auto* emittedSig = findWrapper(emitted);
                if (emittedSig == nullptr || emitted.bitcode.empty())
                    return noMatch(emitted.firstError.empty() ? emittedError : emitted.firstError);
                raw = std::move(emitted);
            }
            const auto* finalSig = findWrapper(raw);
            if (finalSig == nullptr || !finalSig->abi.valid)
                return noMatch(raw.firstError);
            CSigEntry mapped;
            if (!MapRawSig(*finalSig, mapped) || !mapped.bindRefusal.empty())
                return noMatch(raw.firstError.empty() ? mapped.bindRefusal : raw.firstError);
            requestSigs.push_back(std::move(mapped));
            requestBitcode = raw.bitcode;
            SetCInteropTargetFacts(raw);
            if (haveMtime)
            {
                CFileSigCacheEntry entry;
                entry.mtime = headerMtime;
                entry.hash = CxxGroupHeaderHash(group);
                entry.longDoubleWidth = raw.longDoubleWidth;
                entry.longDoubleIsIEEEDouble = raw.longDoubleIsIEEEDouble;
                entry.targetTriple = raw.targetTriple;
                entry.sigs = requestSigs;
                entry.cxxBitcode = requestBitcode;
                std::lock_guard<std::mutex> lock(cFileSigCacheMutex_);
                InsertCFileSigEntry(requestKey, std::move(entry), verbose);
            }
        }
        if (requestSigs.empty()) return noMatch("the generated wrapper was not extracted");
        if (!emitDefinitions) requestBitcode.clear();
        if (!requestSigs.front().isCxx) return noMatch("the generated wrapper was not extracted");
        RequestCxxSignatureTypes(requestSigs);
        if (emitDefinitions) AdoptCxxCompanionBitcode(requestBitcode);

        const bool instanceWrapper = selected->kind == cflat_cinterop::RawFunctionTemplate::InstanceMember;
        const std::string registeredName = instanceWrapper ? functionName : lookupName;
        CSigEntry bound = requestSigs.front();
        bound.name = registeredName;
        RegisterCSignatures({ bound }, selected->file.empty() ? group.headers.front() : selected->file);
        bool registered = false;
        if (auto it = functionTable.find(registeredName); it != functionTable.end())
            for (const auto& symbol : it->second)
                if (symbol.External && symbol.UniqueName == wrapperName)
                { registered = true; break; }
        if (!registered) return noMatch("the generated wrapper could not be registered");
        if (auto* sink = GetSymbolSink())
            sink->Register(SymbolKind::Function, selected->name,
                           selected->file.empty() ? group.headers.front() : selected->file,
                           selected->line, selected->col < 0 ? 0 : selected->col,
                           "template <...> " + selected->name + "(...)");
        return true;
}

/*
 * Import groups that could own a C++ base name, best first: the group that already answered for it,
 * then groups that published the name, then a system group whose header IS the name
 * (`std::vector` from `import cpp "vector"`), then groups that seeded the leading namespace, then
 * every other C++ group. Only the first candidate that actually declares the name is used.
 */
std::vector<size_t> LLVMBackend::CandidateCxxGroupsFor(const std::string& cxxBase) const
{
        if (auto known = cxxTemplateOwnerGroup_.find(cxxBase); known != cxxTemplateOwnerGroup_.end())
            return { known->second };
        const size_t lastSep = cxxBase.rfind("::");
        const std::string leaf = lastSep == std::string::npos ? cxxBase : cxxBase.substr(lastSep + 2);
        const size_t firstSep = cxxBase.find("::");
        const std::string lead = firstSep == std::string::npos ? cxxBase : cxxBase.substr(0, firstSep);
        std::vector<size_t> published, byName, byNamespace, rest;
        for (size_t i = 0; i < cxxImportGroups_.size(); ++i)
        {
            const CxxImportGroup& g = cxxImportGroups_[i];
            if (g.publishedNames.count(cxxBase) != 0) { published.push_back(i); continue; }
            bool headerIsName = false;
            for (const auto& h : g.headers)
                headerIsName = headerIsName
                    || std::filesystem::path(h).filename().string() == leaf
                    || std::filesystem::path(h).stem().string() == leaf;
            if (headerIsName) { byName.push_back(i); continue; }
            if (g.namespaces.count(lead) != 0) { byNamespace.push_back(i); continue; }
            rest.push_back(i);
        }
        std::vector<size_t> order = std::move(published);
        order.insert(order.end(), byName.begin(), byName.end());
        order.insert(order.end(), byNamespace.begin(), byNamespace.end());
        order.insert(order.end(), rest.begin(), rest.end());
        return order;
    }

bool LLVMBackend::RequestCxxForeignType(const std::string& cflatName, const std::string& cxxSpelling,
                                        std::string& error, bool needDefinitions,
                                        bool explicitInstantiation, bool tentative)
{
        // One attempt per CFlat identity per analysis; the outcome (including the diagnostic text)
        // is replayed so a second use site reports the same reason without re-parsing libc++.
        if (auto it = cxxForeignRequests_.find(cflatName); it != cxxForeignRequests_.end()
            && (!needDefinitions || cxxForeignDefinitions_.count(cflatName) != 0
                || !it->second.empty()))
        {
            error = it->second;
            return error.empty();
        }
        auto fail = [&](std::string why) {
            error = std::move(why);
            if (!tentative) cxxForeignRequests_[cflatName] = error;
            return false;
        };
        // A request compiles against ONE import group. Callers inside a group's extraction run
        // under that group's scope; a CFlat-spelled type resolves its group first.
        if (activeCxxRequestGroup_ == nullptr || activeCxxRequestGroup_->headers.empty())
            return fail(std::format("C++ type '{}' needs a C++ header in scope - "
                                    "import one with 'import cpp \"<header>\";'", cxxSpelling));
        const CxxRequestGroup& group = *activeCxxRequestGroup_;

        /*
         * A request parses the whole C++ TU TWICE (stage 1 without CodeGen, stage 2 with), which for
         * a libc++ specialization is the most expensive thing this compiler does. The result is a
         * pure function of the import group's headers, its defines, the instantiation spelling and
         * the emit mode, so it is cached process-wide next to the header bindings: a second file in
         * the same invocation, and every LSP reanalysis of the same file, replay it instead.
         * Cleared by nothing that ResetForReanalysis touches - the cache is static and validated by
         * header mtime/content hash, like the header cache.
         */
        CxxRequestItem item;
        item.cflatName = cflatName;
        item.cxxSpelling = cxxSpelling;
        item.needDefinitions = needDefinitions;
        item.explicitInstantiation = explicitInstantiation;
        const std::string requestKey = CxxTypeRequestCacheKey(group, item);
        std::filesystem::file_time_type headerMtime{};
        const bool haveMtime = CxxGroupHeaderStamp(group, headerMtime);

        std::vector<CRecordEntry> records;
        std::vector<CSigEntry> requestSigs;
        std::string requestBitcode;
        bool cached = false;
        if (haveMtime)
        {
            std::lock_guard<std::mutex> lock(cFileSigCacheMutex_);
            auto it = cFileSigCache_.find(requestKey);
            if (it != cFileSigCache_.end()
                && (it->second.mtime == headerMtime || it->second.hash == CxxGroupHeaderHash(group)))
            {
                it->second.mtime = headerMtime;
                TouchCFileSigEntry(requestKey, it->second);
                records = it->second.records;
                requestSigs = it->second.sigs;
                requestBitcode = it->second.cxxBitcode;
                SetCInteropTargetFacts(it->second.longDoubleWidth,
                                       it->second.longDoubleIsIEEEDouble,
                                       it->second.targetTriple);
                cached = true;
            }
        }
        if (cached && verbose)
            std::cout << std::format("[verbose] C++ type request cache hit for {}\n", cxxSpelling);

        // Counted per spelling in the trace, cache-hit replays included, so request counts stay
        // comparable across runs; a replay is marked so the two are told apart.
        llvm::TimeTraceScope scope("CxxTypeRequest",
                                   cached ? cxxSpelling + " (cache)" : cxxSpelling);

        if (!cached)
        {
            const std::vector<CxxRequestItem> single{ item };
            // Stage 1: the member list and their canonical signatures. No CodeGen.
            cflat_cinterop::ExtractResult probe;
            {
                llvm::TimeTraceScope stage1("CxxRequestStage1", cxxSpelling);
                if (!RunCxxTypeRequests(group, single, /*extraSource*/ {},
                                        /*emitDefinitions*/ false, probe, error))
                    return fail(std::format("C++ type '{}' could not be parsed: {}",
                                            cxxSpelling, error));
            }
            if (probe.records.empty())
                return fail(std::format("'{}' does not name a C++ class type in the imported headers",
                                        cxxSpelling));
            const auto findRequestedRecord = [&](const std::vector<cflat_cinterop::RawRecord>& list)
                -> const cflat_cinterop::RawRecord& {
                for (const auto& r : list)
                    if (r.name == cflatName) return r;
                return list.front();
            };
            const auto& probeTarget = findRequestedRecord(probe.records);

            cflat_cinterop::ExtractResult raw;
            if (needDefinitions)
            {
            /*
             * Stage 2: same TU, plus an ODR-USE per member, with CodeGen. Stage 1 alone reports
             * member TEMPLATES only as uninstantiated patterns (no linkage name), so the member
             * surface it sees is SMALLER - `std::string("literal")` would be missing. LSP analysis
             * therefore runs this stage too and only throws the module away, so the editor's
             * diagnostics match the compiler's.
             */
            {
                llvm::TimeTraceScope stage2("CxxRequestStage2", cxxSpelling);
                std::string err2;
                cflat_cinterop::ExtractResult emitted;
                const bool emittedOk = RunCxxTypeRequests(group, single,
                                      CxxRequestOdrUsePreamble()
                                      + BuildCxxRequestOdrUses(probeTarget, "__cflat_req_0", "")
                                      + BuildStdFunctionCtorUse(cxxSpelling, "__cflat_req_0"),
                                      /*emitDefinitions*/ true, emitted, err2);
                if (emittedOk && !emitted.records.empty())
                    raw = std::move(emitted);
            }
            }
            else
                raw = std::move(probe);
            if (!raw.records.empty())
            {
                const auto& rawTarget = findRequestedRecord(raw.records);
                const std::string canonical = rawTarget.canonicalCtype;
                auto known = cxxForeignTypeSpellings_.find(SqueezeCxxSpelling(canonical));
                const std::string mappedName = known == cxxForeignTypeSpellings_.end()
                    ? cflatName : known->second;
                if (!canonical.empty())
                    cxxForeignTypeSpellings_[SqueezeCxxSpelling(canonical)] = mappedName;
                cxxForeignTypeSpellings_[SqueezeCxxSpelling(cxxSpelling)] = mappedName;
                cxxCflatToCxxSpelling_[cflatName] = cxxSpelling;
            }
            SetCInteropTargetFacts(raw);
            MapRawRecords(raw, records);
            for (const auto& rawSig : raw.sigs)
            {
                CSigEntry sig;
                if (MapRawSig(rawSig, sig)) requestSigs.push_back(std::move(sig));
            }
            requestBitcode = raw.bitcode;
            if (haveMtime && !records.empty())
            {
                CFileSigCacheEntry entry;
                entry.mtime = headerMtime;
                entry.hash  = CxxGroupHeaderHash(group);
                entry.longDoubleWidth = raw.longDoubleWidth;
                entry.longDoubleIsIEEEDouble = raw.longDoubleIsIEEEDouble;
                entry.targetTriple = raw.targetTriple;
                entry.sigs = requestSigs;
                entry.records = records;
                entry.cxxBitcode = requestBitcode;
                std::lock_guard<std::mutex> lock(cFileSigCacheMutex_);
                InsertCFileSigEntry(requestKey, std::move(entry), verbose);
            }
        }
        if (records.empty())
            return fail(std::format("'{}' does not name a C++ class type in the imported headers",
                                    cxxSpelling));

        const auto& registeredTarget = [&]() -> const CRecordEntry& {
            for (const auto& r : records)
                if (r.name == cflatName) return r;
            return records.front();
        }();
        const std::string canonical = registeredTarget.canonicalCtype;
        if (!canonical.empty())
        {
            // Same canonical specialization under a second CFlat spelling (a typedef, or the same
            // arguments spelled twice): alias onto the ONE registration instead of a second struct.
            auto known = cxxForeignTypeSpellings_.find(SqueezeCxxSpelling(canonical));
            if (known != cxxForeignTypeSpellings_.end() && known->second != cflatName)
            {
                RegisterTypeAlias(cflatName, known->second);
                cxxCflatToCxxSpelling_[cflatName] = cxxSpelling;
                cxxForeignRequests_[cflatName] = "";
                if (group.primary != static_cast<size_t>(-1))
                    cxxTypeOwnerGroup_[cflatName] = group.primary;
                if (needDefinitions) cxxForeignDefinitions_.insert(cflatName);
                return true;
            }

            cxxForeignTypeSpellings_[SqueezeCxxSpelling(canonical)] = cflatName;
        }
        // A class member may name a second class template specialization by value (for example a
        // vector iterator). Register the outer spelling first so self-parameters and assignment
        // members do not recursively request the same specialization.
        cxxCflatToCxxSpelling_[cflatName] = cxxSpelling;
        cxxForeignRequests_[cflatName] = "";
        if (group.primary != static_cast<size_t>(-1))
            cxxTypeOwnerGroup_[cflatName] = group.primary;
        if (needDefinitions) cxxForeignDefinitions_.insert(cflatName);
        RequestCxxMemberTypes(records);
        if (!requestBitcode.empty() && symbolSink_ == nullptr) AdoptCxxCompanionBitcode(requestBitcode);
        // Registered BEFORE the records so a member signature naming the type itself
        // (`operator=(const vector<int>&)`, `push_back` on a nested element) maps to the CFlat name.
        RegisterCRecords(records, group.headers.front());
        RegisterCSignatures(requestSigs, group.headers.front());
        if (dataStructures.find(cflatName) == dataStructures.end())
        {
            cxxCflatToCxxSpelling_.erase(cflatName);
            return fail(std::format("C++ type '{}' could not be laid out for cflat", cxxSpelling));
        }
        return true;
    }

/*
 * Item 2 - one stage-1 and one stage-2 translation unit for every spelling ONE import statement
 * asks for, instead of two Clang frontends per spelling. Stage 1 runs to a fixpoint so a nested
 * type it discovers (a vector's iterator) joins the same stage 2 instead of starting its own
 * request. The result is split back into exactly the per-spelling cache entries the single-request
 * path writes, so every caller of RequestCxxForeignType, and both cache-hit replays, are
 * unchanged - a spelling missing from the batch simply runs the single path.
 */
void LLVMBackend::PrewarmCxxRequestBatch(std::vector<CxxRequestItem> items)
{
        if (activeCxxRequestGroup_ == nullptr || activeCxxRequestGroup_->headers.empty()) return;
        const CxxRequestGroup& group = *activeCxxRequestGroup_;
        std::filesystem::file_time_type headerMtime{};
        if (!CxxGroupHeaderStamp(group, headerMtime)) return;
        const uint64_t headerHash = CxxGroupHeaderHash(group);

        auto alreadyKnown = [&](const CxxRequestItem& item) {
            if (cxxForeignRequests_.count(item.cflatName) != 0) return true;
            std::lock_guard<std::mutex> lock(cFileSigCacheMutex_);
            auto it = cFileSigCache_.find(CxxTypeRequestCacheKey(group, item));
            return it != cFileSigCache_.end()
                && (it->second.mtime == headerMtime || it->second.hash == headerHash);
        };
        std::vector<CxxRequestItem> pending;
        std::unordered_set<std::string> seen;
        for (CxxRequestItem& item : items)
        {
            if (!seen.insert(item.cflatName).second) continue;
            if (alreadyKnown(item)) continue;
            pending.push_back(std::move(item));
        }
        if (pending.size() < 2) return;   // the single path is already one stage-1 plus one stage-2

        llvm::TimeTraceScope batchScope("CxxRequestBatch", [&] {
            return group.label + " x " + std::to_string(pending.size());
        });

        /*
         * Stage 1 to a fixpoint: each round adds the nested spellings the previous round's member
         * lists named (an iterator class, a pair<const K, V>) so they are instantiated in the same
         * TU as the type that exposes them.
         */
        cflat_cinterop::ExtractResult probe;
        for (int round = 0; round < 3; ++round)
        {
            cflat_cinterop::ExtractResult rounded;
            std::string error;
            {
                llvm::TimeTraceScope stage1("CxxRequestStage1", group.label);
                if (!RunCxxTypeRequests(group, pending, /*extraSource*/ {},
                                        /*emitDefinitions*/ false, rounded, error))
                    return;   // fall back to the single-request path, one spelling at a time
            }
            if (rounded.records.empty()) return;
            probe = std::move(rounded);
            std::vector<CRecordEntry> mapped;
            MapRawRecords(probe, mapped);
            std::vector<CxxRequestItem> nested;
            CollectCxxMemberRequestItems(mapped, nested);
            bool grew = false;
            for (CxxRequestItem& item : nested)
            {
                if (!seen.insert(item.cflatName).second) continue;
                if (alreadyKnown(item)) continue;
                pending.push_back(std::move(item));
                grew = true;
            }
            if (!grew) break;
        }

        // Stage 2: one CodeGen frontend with the ODR-uses of every spelling in the batch.
        cflat_cinterop::ExtractResult emitted;
        bool haveEmitted = false;
        std::vector<size_t> fullItems;
        for (size_t i = 0; i < pending.size(); ++i)
            if (pending[i].needDefinitions) fullItems.push_back(i);
        if (!fullItems.empty())
        {
            std::string extra = CxxRequestOdrUsePreamble();
            for (size_t i : fullItems)
            {
                const std::string marker = "__cflat_req_" + std::to_string(i);
                const cflat_cinterop::RawRecord* target = nullptr;
                for (const auto& r : probe.records)
                    if (r.name == pending[i].cflatName) { target = &r; break; }
                if (target == nullptr) continue;
                extra += BuildCxxRequestOdrUses(*target, marker, "b" + std::to_string(i) + "_");
                extra += BuildStdFunctionCtorUse(pending[i].cxxSpelling, marker);
            }
            llvm::TimeTraceScope stage2("CxxRequestStage2", group.label);
            std::string error;
            haveEmitted = RunCxxTypeRequests(group, pending, extra, /*emitDefinitions*/ true,
                                             emitted, error)
                       && !emitted.records.empty();
        }

        /*
         * Split the batch back into per-spelling entries. The extractor emits a request's base
         * records immediately before the request's own record, in request order, so the record list
         * partitions exactly the way the single-request TU would have produced it. A free operator
         * goes to every spelling it names, and to all of them when it names none.
         */
        /*
         * A spelling has to be mapped to its CFlat identity BEFORE the batch's records and
         * signatures are mapped: the single-request path publishes the canonical spelling of the
         * type it just parsed and then maps that TU's signatures, so a free operator's parameter
         * resolves to the CFlat type rather than to an opaque one. A batch maps several TUs' worth
         * of signatures at once, so it publishes all of them first.
         */
        auto publishSpellings = [&](const cflat_cinterop::ExtractResult& raw) {
            for (const CxxRequestItem& item : pending)
            {
                const cflat_cinterop::RawRecord* rec = nullptr;
                for (const auto& r : raw.records)
                    if (r.name == item.cflatName) { rec = &r; break; }
                if (rec == nullptr) continue;
                const std::string canonical = rec->canonicalCtype;
                auto known = cxxForeignTypeSpellings_.find(SqueezeCxxSpelling(canonical));
                const std::string mappedName = known == cxxForeignTypeSpellings_.end()
                    ? item.cflatName : known->second;
                if (!canonical.empty())
                    cxxForeignTypeSpellings_[SqueezeCxxSpelling(canonical)] = mappedName;
                cxxForeignTypeSpellings_[SqueezeCxxSpelling(item.cxxSpelling)] = mappedName;
                cxxCflatToCxxSpelling_[item.cflatName] = item.cxxSpelling;
            }
        };
        auto storeFrom = [&](const cflat_cinterop::ExtractResult& raw, bool withBitcode) {
            // The published spellings exist only while this batch is mapped: registration itself
            // still runs through the normal request path, which decides what is already mapped.
            const auto savedSpellings = cxxForeignTypeSpellings_;
            const auto savedCflatToCxx = cxxCflatToCxxSpelling_;
            publishSpellings(raw);
            std::vector<CRecordEntry> mapped;
            MapRawRecords(raw, mapped);
            std::vector<CSigEntry> sigs;
            for (const auto& rawSig : raw.sigs)
            {
                CSigEntry sig;
                if (MapRawSig(rawSig, sig)) sigs.push_back(std::move(sig));
            }
            cxxForeignTypeSpellings_ = savedSpellings;
            cxxCflatToCxxSpelling_ = savedCflatToCxx;
            size_t cursor = 0;
            for (size_t i = 0; i < pending.size(); ++i)
            {
                size_t stop = 0;
                bool haveTarget = false;
                for (size_t r = cursor; r < mapped.size(); ++r)
                    if (mapped[r].name == pending[i].cflatName)
                    { stop = r + 1; haveTarget = true; break; }
                if (!haveTarget) continue;   // this spelling produced no record; single path retries
                std::vector<CRecordEntry> slice(mapped.begin() + cursor, mapped.begin() + stop);
                cursor = stop;
                if (pending[i].needDefinitions != withBitcode) continue;
                std::string canonical;
                for (const auto& r : slice)
                    if (r.name == pending[i].cflatName) canonical = SqueezeCxxSpelling(r.canonicalCtype);
                std::vector<CSigEntry> mine;
                const std::string spellingKey = SqueezeCxxSpelling(pending[i].cxxSpelling);
                for (const CSigEntry& sig : sigs)
                {
                    std::vector<std::string> spellings = sig.paramSpellings;
                    spellings.push_back(sig.retSpelling);
                    bool namesMine = false, namesAny = false;
                    for (const auto& spelling : spellings)
                    {
                        const std::string squeezed = SqueezeCxxSpelling(spelling);
                        namesMine = namesMine
                            || (!canonical.empty() && squeezed.find(canonical) != std::string::npos)
                            || squeezed.find(spellingKey) != std::string::npos;
                        for (const CxxRequestItem& other : pending)
                            namesAny = namesAny
                                || squeezed.find(SqueezeCxxSpelling(other.cxxSpelling))
                                   != std::string::npos;
                    }
                    // A free operator goes to every spelling it names; one that names none of them
                    // (a plain helper the TU happened to instantiate) goes to all of them.
                    if (namesMine || !namesAny) mine.push_back(sig);
                }
                if (verbose)
                    std::cout << std::format("[verbose] C++ request batch entry for {} "
                                             "({} record(s), {} signature(s))\n",
                                             pending[i].cxxSpelling, slice.size(), mine.size());
                CFileSigCacheEntry entry;
                entry.mtime = headerMtime;
                entry.hash = headerHash;
                entry.longDoubleWidth = raw.longDoubleWidth;
                entry.longDoubleIsIEEEDouble = raw.longDoubleIsIEEEDouble;
                entry.targetTriple = raw.targetTriple;
                entry.sigs = std::move(mine);
                entry.records = std::move(slice);
                if (withBitcode) entry.cxxBitcode = raw.bitcode;
                std::lock_guard<std::mutex> lock(cFileSigCacheMutex_);
                InsertCFileSigEntry(CxxTypeRequestCacheKey(group, pending[i]), std::move(entry),
                                    verbose);
            }
        };
        storeFrom(probe, /*withBitcode*/ false);
        if (haveEmitted) storeFrom(emitted, /*withBitcode*/ true);
    }

/*
 * The nested specializations a record's members expose (a container's iterator, a smart pointer's
 * pointee). Collected separately from the requesting so one import's batch can instantiate them in
 * the same translation unit as the class that names them.
 */
void LLVMBackend::CollectCxxMemberRequestItems(const std::vector<CRecordEntry>& records,
                                               std::vector<CxxRequestItem>& out)
{
        std::unordered_set<std::string> seen;
        for (const CRecordEntry& record : records)
        {
            // Registration keeps the non-const twin of a const method (CFlat drops const), so the
            // const overload's iterator type (`const_iterator`) would be requested and never bound.
            std::unordered_set<std::string> nonConstNames;
            for (const auto& member : record.members)
                if (!member.isConst) nonConstNames.insert(member.name);
            for (const auto& member : record.members)
            {
                const bool exposesPointee = member.name == "operator->"
                                         || member.name == "operator*";
                const bool exposesIterator = member.name == "begin"
                                          || member.name == "end";
                if (!exposesPointee && !exposesIterator) continue;
                if (member.isConst && nonConstNames.count(member.name) != 0) continue;
                std::vector<std::string> spellings = member.paramTypes;
                spellings.push_back(member.retType);
                for (const std::string& spelling : spellings)
                {
                    std::string named = CxxMemberValueSpelling(spelling);
                    if (named.find('<') == std::string::npos
                        || named.find('(') != std::string::npos)
                        continue;
                    if (!seen.insert(SqueezeCxxSpelling(named)).second) continue;
                    TypeAndValue mapped;
                    bool mappedForeign = false;
                    if (TryMapCxxForeignSpelling(named, mapped, mappedForeign) && mappedForeign) continue;
                    const std::string identity = AutoCxxForeignIdentity(named);
                    if (identity.empty()) continue;
                    CxxRequestItem item;
                    item.cflatName = identity;
                    item.cxxSpelling = named;
                    item.needDefinitions = exposesIterator;
                    item.explicitInstantiation = false;
                    out.push_back(std::move(item));
                }
            }
        }
}

void LLVMBackend::RequestCxxMemberTypes(const std::vector<CRecordEntry>& records)
{
        std::vector<CxxRequestItem> items;
        CollectCxxMemberRequestItems(records, items);
        for (const CxxRequestItem& item : items)
        {
            std::string error;
            RequestCxxForeignType(item.cflatName, item.cxxSpelling, error, item.needDefinitions,
                                  item.explicitInstantiation);
        }
}

/*
 * Entry point for both ParseDeclarationSpecifiers copies: build the C++ spelling from the CFlat
 * name (`std.vector` + { int } -> `std::vector<int>`) and request it. `typeArgs` empty means a
 * plain dotted name (`std.string`), which may still be a typedef for a specialization.
 */
bool LLVMBackend::TryRequestCxxType(const std::string& baseName,
                                    const std::vector<std::string>& typeArgs,
                                    const std::string& cflatName, std::string& error)
{
        error.clear();
        if (!HasCxxImportGroup()) return false;
        if (baseName.find('.') == std::string::npos) return false;
        if (IsCxxForeignTypeRegistered(cflatName)) return true;
        if (typeArgs.empty())
            if (auto lazy = cxxLazyAliasSpecializations_.find(baseName);
                lazy != cxxLazyAliasSpecializations_.end())
            {
                const std::string& spelling = lazy->second;
                if (activeCxxRequestGroup_ != nullptr)
                    return RequestCxxForeignType(baseName, spelling, error);
                std::string cxxBase = spelling.substr(0, spelling.find('<'));
                while (!cxxBase.empty() && std::isspace((unsigned char)cxxBase.back())) cxxBase.pop_back();
                return RequestCxxTypeInOwningGroup(cxxBase, baseName, spelling, {}, error);
            }
        // A CFlat generic or generic interface owns its own instantiation path and its own
        // mangling; the two identities never mix (`list<std.vector<int>>` is a CFlat instantiation
        // whose element happens to be foreign).
        const StructData existingForeignShell = GetDataStructure(cflatName);
        const bool isOpaqueForeignShell = cflatName.find('$') != std::string::npos
            && existingForeignShell.StructType != nullptr
            && existingForeignShell.StructType->isOpaque();
        if (!isOpaqueForeignShell
            && (AnyGenericTypeTemplateNamed(baseName) || IsGenericInterfaceTemplateName(baseName)))
            return false;
        if (IsKnownTypeName(typeArgs.empty() ? baseName : cflatName))
        {
            if (!isOpaqueForeignShell)
                return false;
        }
        /*
         * An unknown dotted name is only a CANDIDATE foreign type when its leading segment came
         * from a C++ import. Without this, every forward reference to a CFlat type declared later
         * in the file (the ForwardRefScanner has not seen it yet) paid two clang parses of the
         * standard library, and a header that happens to declare the same qualified name would
         * capture the identity the CFlat definition is about to claim.
         */
        {
            const std::string lead = baseName.substr(0, baseName.find('.'));
            if (cxxForeignNamespaces_.count(lead) == 0 && !IsCxxForeignTypeRegistered(lead)
                && cxxCflatToCxxSpelling_.count(lead) == 0)
                return false;
        }
        return RequestCxxType(baseName, typeArgs, cflatName, error);
    }

bool LLVMBackend::RequestCxxType(const std::string& baseName, const std::vector<std::string>& typeArgs,
                                 const std::string& cflatName, std::string& error)
{
        if (IsCxxForeignTypeRegistered(cflatName)) return true;
        std::string spelling = baseName;
        size_t pos = 0;
        while ((pos = spelling.find('.', pos)) != std::string::npos)
        { spelling.replace(pos, 1, "::"); pos += 2; }
        if (!typeArgs.empty())
        {
            spelling += "<";
            for (size_t i = 0; i < typeArgs.size(); ++i)
            {
                if (i) spelling += ", ";
                std::string arg;
                if (!CxxSpellingForCflatType(typeArgs[i], arg))
                {
                    error = std::format("'{}' has no C++ spelling, so it cannot be a template "
                                        "argument of the C++ template '{}' - use a primitive, a "
                                        "pointer, or another C++ type", typeArgs[i], baseName);
                    return false;
                }
                spelling += arg;
            }
            // `vector<vector<int>>` needs no space in C++11 and later, but keep the closing pair
            // readable for the diagnostic.
            spelling += ">";
        }
        // Inside an import group's extraction (a signature, a member, a nested type) the group is
        // already fixed and the request must not escape it.
        if (activeCxxRequestGroup_ != nullptr) return RequestCxxForeignType(cflatName, spelling, error);

        // A type spelled by CFlat code: find the import that owns the template, and bring in the
        // groups that own its template arguments so `std.vector<std.string>` still has <string>.
        std::string cxxBase = baseName;
        size_t bpos = 0;
        while ((bpos = cxxBase.find('.', bpos)) != std::string::npos)
        { cxxBase.replace(bpos, 1, "::"); bpos += 2; }
        std::vector<size_t> deps;
        for (const std::string& arg : typeArgs)
        {
            std::string elem = arg;
            while (!elem.empty() && (elem.back() == '*' || elem.back() == ' ')) elem.pop_back();
            if (auto owner = cxxTypeOwnerGroup_.find(elem); owner != cxxTypeOwnerGroup_.end())
                deps.push_back(owner->second);
        }
        return RequestCxxTypeInOwningGroup(cxxBase, cflatName, spelling, deps, error);
    }

/*
 * Group resolution for a type CFlat code spells. Candidates come from what each import published,
 * from a system header that IS the name, and from the namespaces each import seeded; the first
 * candidate whose headers actually declare the spelling wins and is remembered for every later use
 * of the same template. Nothing is memoized as a failure until the last candidate has been tried,
 * so a wrong first guess does not poison the name.
 */
bool LLVMBackend::RequestCxxTypeInOwningGroup(const std::string& cxxBase,
                                              const std::string& cflatName,
                                              const std::string& spelling,
                                              const std::vector<size_t>& deps, std::string& error)
{
        const std::vector<size_t> order = CandidateCxxGroupsFor(cxxBase);
        if (order.empty())
            return false;
        std::vector<std::string> tried;
        for (size_t k = 0; k < order.size(); ++k)
        {
            const bool last = k + 1 == order.size();
            CxxRequestGroup group = MakeCxxRequestGroup(order[k], deps);
            if (group.headers.empty()) continue;
            llvm::TimeTraceScope groupScope("CxxRequestGroup", spelling + " -> " + group.label);
            CxxRequestGroupScope guard(*this, &group);
            std::string localError;
            if (RequestCxxForeignType(cflatName, spelling, localError, /*needDefinitions*/ true,
                                      /*explicitInstantiation*/ true, /*tentative*/ !last))
            {
                cxxTemplateOwnerGroup_[cxxBase] = order[k];
                cxxTypeOwnerGroup_[cflatName] = order[k];
                return true;
            }
            tried.push_back(group.label);
            if (last && order.size() == 1)
            {
                error = localError;
                return false;
            }
        }
        llvm::TimeTraceScope groupScope("CxxRequestGroup", spelling + " -> unresolved");
        std::string names;
        for (size_t i = 0; i < tried.size(); ++i) names += (i ? ", '" : "'") + tried[i] + "'";
        error = std::format("no imported C++ header declares '{}' - tried {}", cxxBase, names);
        cxxForeignRequests_[cflatName] = error;
        return false;
    }

bool LLVMBackend::ExtractCFileClang(const std::string& cSourcePath,
                           std::vector<CSigEntry>& outSigs, std::vector<CRecordEntry>& outRecords,
                           std::vector<CGlobalEntry>& outGlobals, bool cxxMode,
                           uint64_t* outLongDoubleWidth,
                           bool* outLongDoubleIsIEEEDouble,
                           std::string* outTargetTriple)
{
        llvm::TimeTraceScope extractScope("CFileExtract", cSourcePath);

        cflat_cinterop::ExtractRequest req;
        req.realPath        = cSourcePath;     // parsed from disk
        req.args            = BuildClangDriverArgs(/*headerDir*/ "", /*extraDefines*/ {}, /*errorRecovery*/ true, cxxMode);
        req.cxxMode         = cxxMode;
        req.wantMacros      = false;
        req.requireInScope  = true;
        req.inScopeDirs.push_back(std::filesystem::path(cSourcePath).parent_path().string());
        req.definitionsOnly = true;

        if (verbose)
            std::cout << std::format("[verbose] extracting {} signatures: {} (clang C++ API)\n",
                                     cxxMode ? "C++" : "C", cSourcePath);

        cflat_cinterop::ExtractResult raw;
        std::string err;
        if (!cflat_cinterop::ExtractCInterop(req, raw, err))
        {
            if (verbose) std::cout << std::format("[verbose]   C extraction failed: {}\n", err);
            return false;
        }
        SetCInteropTargetFacts(raw);
        if (outLongDoubleWidth) *outLongDoubleWidth = raw.longDoubleWidth;
        if (outLongDoubleIsIEEEDouble)
            *outLongDoubleIsIEEEDouble = raw.longDoubleIsIEEEDouble;
        if (outTargetTriple) *outTargetTriple = raw.targetTriple;

        {
            llvm::TimeTraceScope adoptScope("AdoptTypedefs", cSourcePath);
            AdoptRawTypedefs(raw);
        }
        {
            llvm::TimeTraceScope recordScope("RegisterCRecords", cSourcePath);
            // No prune here: the .c path has no scope filter, so every top-level record is wanted.
            // Pruning would drop the synthesized nested records (inScope=false) a pointer field names.
            MapRawRecords(raw, outRecords);
            RegisterCRecords(outRecords, cSourcePath);
        }
        {
            llvm::TimeTraceScope sigScope("MapSignatures", cSourcePath);
            for (const auto& rs : raw.sigs)
            {
                CSigEntry e;
                if (MapRawSig(rs, e)) outSigs.push_back(std::move(e));
            }
        }
        {
            llvm::TimeTraceScope globalScope("MapGlobals", cSourcePath);
            for (const auto& rg : raw.globals)
            {
                CGlobalEntry e;
                if (MapRawGlobal(rg, e)) outGlobals.push_back(std::move(e));
            }
        }
        return true;
    }

bool LLVMBackend::ExtractCSignatures(const std::string& cSourcePath, const std::string& programAlias, bool cxxMode)
{
        // Canonical path: stable cache key + the real .c for LSP go-to-definition.
        llvm::SmallString<256> realPath;
        std::string fileForLsp = cSourcePath;
        if (!llvm::sys::fs::real_path(cSourcePath, realPath))
            fileForLsp = realPath.str().str();

        // Defines can gate which functions a .c defines, so fold them into the cache key
        // (the file path alone is the LSP identity; the key is path + defines).
        std::string cacheKey = fileForLsp + (cxxMode ? "|CXX" : "|C");
        for (const auto& def : cDefines_) cacheKey += "|D" + def;

        // Hash the file at most once per call, and only when actually needed.
        uint64_t currentHash = 0;
        bool haveHash = false;
        auto hashNow = [&]() -> uint64_t
        {
            if (!haveHash) { HashFileContents(fileForLsp, currentHash); haveHash = true; }
            return currentHash;
        };

        std::error_code mtEc;
        auto currentMtime = std::filesystem::last_write_time(fileForLsp, mtEc);

        // --- Cache lookup under lock. Copy the signatures out, then register after the
        //     lock is released, so the global cache never serializes per-backend work. ---
        std::vector<CSigEntry> hitSigs;
        std::vector<CRecordEntry> hitRecords;
        std::vector<CGlobalEntry> hitGlobals;
        bool hit = false;
        {
            std::lock_guard<std::mutex> lock(cFileSigCacheMutex_);
            auto cacheIt = cFileSigCache_.find(cacheKey);
            if (!mtEc && cacheIt != cFileSigCache_.end())
            {
                CFileSigCacheEntry& entry = cacheIt->second;
                if (entry.mtime == currentMtime)
                {
                    if (verbose) std::cout << std::format("[verbose] C signatures cache hit (mtime) for {}\n", fileForLsp);
                    SetCInteropTargetFacts(entry.longDoubleWidth, entry.longDoubleIsIEEEDouble,
                                           entry.targetTriple);
                    TouchCFileSigEntry(cacheKey, entry);
                    hitSigs = entry.sigs;
                    hitRecords = entry.records;
                    hitGlobals = entry.globals;
                    hit = true;
                }
                // Timestamp moved but content may be identical - only now pay for a hash.
                else if (hashNow() == entry.hash)
                {
                    if (verbose) std::cout << std::format("[verbose] C signatures cache hit (hash) for {}\n", fileForLsp);
                    SetCInteropTargetFacts(entry.longDoubleWidth, entry.longDoubleIsIEEEDouble,
                                           entry.targetTriple);
                    entry.mtime = currentMtime; // refresh so the next check short-circuits on mtime
                    TouchCFileSigEntry(cacheKey, entry);
                    hitSigs = entry.sigs;
                    hitRecords = entry.records;
                    hitGlobals = entry.globals;
                    hit = true;
                }
            }
        }
        if (hit)
        {
            // Records must be registered before sigs so signatures referencing struct-by-
            // value resolve to the same dataStructures entries on cache hits.
            RegisterCRecords(hitRecords, fileForLsp);
            RegisterCSignatures(hitSigs, fileForLsp, programAlias);
            RegisterCGlobals(hitGlobals, fileForLsp);
            return true;
        }

        // Cache miss - parse outside the lock; concurrent misses redo work harmlessly.
        // Extraction uses the clang C++ API in-process (no clang-cl needed), so LSP works too.
        std::vector<CSigEntry> sigs;
        std::vector<CRecordEntry> records;
        std::vector<CGlobalEntry> globals;
        uint64_t longDoubleWidth = 0;
        bool longDoubleIsIEEEDouble = false;
        std::string targetTriple;
        if (!ExtractCFileClang(cSourcePath, sigs, records, globals, cxxMode,
                               &longDoubleWidth, &longDoubleIsIEEEDouble, &targetTriple))
            return false;

        if (!mtEc)
        {
            CFileSigCacheEntry entry;
            entry.mtime = currentMtime;
            entry.hash  = hashNow();
            entry.longDoubleWidth = longDoubleWidth;
            entry.longDoubleIsIEEEDouble = longDoubleIsIEEEDouble;
            entry.targetTriple = targetTriple;
            entry.sigs  = sigs;
            entry.records = records;
            entry.globals = globals;
            std::lock_guard<std::mutex> lock(cFileSigCacheMutex_);
            InsertCFileSigEntry(cacheKey, std::move(entry), verbose);
        }

        // Records were already registered inside ExtractCFileClang (so it could map
        // struct-by-value parameter types); do not re-register here.
        RegisterCSignatures(sigs, fileForLsp, programAlias);
        RegisterCGlobals(globals, fileForLsp);
        return true;
    }

std::string LLVMBackend::ConstIntValueSuffix(const std::string& typeName, long long value)
{
        bool isUnsigned = !typeName.empty() && typeName[0] == 'u';  // u8/u16/u32/u64
        if (isUnsigned)
        {
            unsigned bits = BitfieldStorageBits(typeName);
            uint64_t mask = (bits == 0 || bits >= 64) ? ~0ull : ((1ull << bits) - 1);
            return std::format(" = 0x{:x}", (uint64_t)value & mask);
        }
        return std::format(" = {}", value);
    }

void LLVMBackend::RegisterCEnums(const std::vector<CEnumEntry>& enums, const std::string& fileForLsp)
{
        for (const CEnumEntry& e : enums)
        {
            if (e.name.empty()) continue;
            if (!e.enumType.empty() && !e.underlyingType.empty())
            {
                TypeAndValue backing;
                if (MapCTypeToTypeAndValue(e.underlyingType, backing))
                    RegisterEnumBackingType(e.enumType, backing.TypeName);
            }
            // First writer wins: a hand-written declaration or an earlier header takes
            // precedence over a duplicate constant name.
            if (globalNamedVariable.count(e.name)) continue;
            // A qualified C++ enumerator ("ns.Cls.Kind.One") needs every dotted prefix registered
            // as a namespace before the name resolves at a use site.
            for (size_t pos = 0; (pos = e.name.find('.', pos)) != std::string::npos; ++pos)
                RegisterNamespace(e.name.substr(0, pos));

            bool wide = (e.value < INT32_MIN || e.value > INT32_MAX);
            TypeAndValue tv;
            tv.TypeName     = e.enumType.empty() ? (wide ? "i64" : "int") : e.enumType;
            tv.EnumBacking  = e.enumType.empty() ? std::string{} : GetEnumBackingType(e.enumType);
            tv.VariableName = e.name;
            tv.Pointer      = false;
            llvm::Constant* c = nullptr;
            if (!e.enumType.empty())
            {
                if (auto* et = llvm::dyn_cast_or_null<llvm::IntegerType>(GetType(tv)))
                    // Unsigned backings hold values past the signed range (u8 enumerator 200):
                    // truncate to the backing width instead of asserting on the sign.
                    c = llvm::ConstantInt::get(et, llvm::APInt(et->getBitWidth(), (uint64_t)e.value,
                                                              e.value < 0, /*implicitTrunc*/ true));
            }
            if (c == nullptr)
                c = wide ? static_cast<llvm::Constant*>(builder->getInt64((uint64_t)e.value))
                         : static_cast<llvm::Constant*>(builder->getInt32((uint32_t)(int32_t)e.value));
            CreateGlobalVariable(tv, c);

            if (auto* s = GetSymbolSink())
                s->Register(SymbolKind::Variable, e.name, fileForLsp, e.line, e.col < 0 ? 0 : e.col,
                            tv.TypeName + " " + e.name + ConstIntValueSuffix(tv.TypeName, e.value));
        }
        if (verbose)
            std::cout << std::format("[verbose]   registered {} C enum constant(s) from {}\n", enums.size(), fileForLsp);
    }

void LLVMBackend::RegisterCGlobals(const std::vector<CGlobalEntry>& globals, const std::string& fileForLsp)
{
        for (const CGlobalEntry& e : globals)
        {
            if (e.name.empty()) continue;
            if (globalNamedVariable.count(e.name)) continue;  // first writer wins

            TypeAndValue tv = e.type;
            tv.VariableName = e.name;
            CreateGlobalVariable(tv, /*initValue*/ nullptr, /*threadLocal*/ false,
                                 /*userAlign*/ 0, /*externalDecl*/ true);

            if (auto* s = GetSymbolSink())
                s->Register(SymbolKind::Variable, e.name, fileForLsp, e.line, e.col < 0 ? 0 : e.col,
                            tv.TypeName + (tv.Pointer ? "*" : "") + " " + e.name);
        }
        RegisterCMacroAliases({}, {}, fileForLsp);
        if (verbose)
            std::cout << std::format("[verbose]   registered {} C global(s) from {}\n", globals.size(), fileForLsp);
    }

/*
 * Add unnamed [N x u8] filler fields so the LLVM struct reproduces clang's field offsets. Only
 * INTERIOR gaps are handled here; trailing padding for an over-aligned record is added by
 * CreateStructType from the record's alignment. A padding field carries an empty VariableName,
 * which every consumer (LSP registration, member lookup) already treats as a synthetic slot.
 */
void LLVMBackend::InsertCxxLayoutPadding(const CRecordEntry& r, std::vector<DeclTypeAndValue>& fields)
{
        if (fields.size() != r.fields.size()) return;   // shape changed; the verifier reports it
        const llvm::DataLayout& dl = module->getDataLayout();
        std::vector<DeclTypeAndValue> out;
        out.reserve(fields.size());
        uint64_t at = 0;
        for (size_t i = 0; i < fields.size(); ++i)
        {
            llvm::Type* ft = GetType(fields[i]);
            if (ft == nullptr || !ft->isSized()) return;
            uint64_t want = r.fields[i].offsetBytes;
            uint64_t natural = r.isPacked
                ? at
                : llvm::alignTo(at, dl.getABITypeAlign(ft).value());
            if (want < natural) return;                 // cannot be reached by padding; verifier reports
            if (want > natural)
            {
                DeclTypeAndValue pad;
                pad.TypeName = "u8";
                pad.ConstArraySize = want - natural;
                out.push_back(pad);
                at = want;
            }
            else
                at = natural;
            at += (uint64_t)dl.getTypeAllocSize(ft);
            out.push_back(fields[i]);
        }
        fields.swap(out);
    }

std::string LLVMBackend::VerifyCxxRecordLayout(const CRecordEntry& r)
{
        std::string mismatch;
        auto it = dataStructures.find(r.name);
        if (it == dataStructures.end() || it->second.StructType == nullptr) return mismatch;
        llvm::StructType* st = it->second.StructType;
        if (st->isOpaque() || !st->isSized()) return mismatch;
        if (r.sizeBytes == 0) return mismatch;   // clang reported no layout (opaque forward declaration)

        const llvm::DataLayout& dl = module->getDataLayout();
        uint64_t size = (uint64_t)dl.getTypeAllocSize(st);
        uint64_t align = std::max<uint64_t>(dl.getABITypeAlign(st).value(),
                                            it->second.UserRequestedAlignment);
        auto refuse = [&](const std::string& detail) {
            mismatch = "layout is not representable: " + detail;
            return mismatch;
        };
        if (size != r.sizeBytes)
            return refuse(std::format("cflat lays it out as {} bytes, clang as {}", size, r.sizeBytes));
        if (align != r.alignBytes)
            return refuse(std::format("cflat aligns it to {}, clang to {}", align, r.alignBytes));
        if (r.isUnion) return mismatch;  // a union has every member at offset 0 on both sides

        const llvm::StructLayout* sl = dl.getStructLayout(st);
        const auto& decl = it->second.StructFields;
        size_t elem = 0;
        for (const auto& cf : r.fields)
        {
            if (cf.isBitfield)
            {
                auto bit = std::find_if(it->second.Bitfields.begin(), it->second.Bitfields.end(),
                                        [&](const BitfieldInfo& b) { return b.Name == cf.name; });
                if (bit == it->second.Bitfields.end()) return refuse(
                    std::format("bitfield '{}' was not recorded in cflat", cf.name));
                if (bit->StorageFieldIndex >= st->getNumElements()) return refuse(
                    std::format("bitfield '{}' has an invalid storage slot", cf.name));
                // Compare absolute BIT positions: clang's byte offset of a bitfield is its bit
                // offset / 8, which lands inside the storage unit for any bit past the first byte.
                const uint64_t bitAt = sl->getElementOffset(bit->StorageFieldIndex) * 8 + bit->BitOffset;
                if (bitAt != cf.bitOffset)
                    return refuse(std::format("bitfield '{}' sits at bit {} in cflat and bit {} in clang",
                                              cf.name, bitAt, cf.bitOffset));
                elem = std::max<size_t>(elem, bit->StorageFieldIndex + 1);
                continue;
            }
            // Skip the synthetic padding slots InsertCxxLayoutPadding added.
            while (elem < decl.size() && decl[elem].VariableName.empty()) ++elem;
            if (elem >= decl.size() || elem >= st->getNumElements()) return mismatch;
            uint64_t off = sl->getElementOffset((unsigned)elem);
            if (off != cf.offsetBytes)
                return refuse(std::format("field '{}' sits at byte {} in cflat and byte {} in clang",
                                          cf.name, off, cf.offsetBytes));
            ++elem;
        }
        return mismatch;
    }

// An opaque, correctly sized and aligned stand-in for a C++ field whose type cflat cannot map:
// an array of the widest unsigned integer that matches the field's alignment. Alignment above 8
// has no such integer, so the caller keeps its refusal path for that case.
bool LLVMBackend::MakeOpaqueFieldBlob(const CRecordFieldEntry& f, DeclTypeAndValue& out) const
{
        if (f.sizeBytes == 0 || f.alignBytes == 0 || f.alignBytes > 8) return false;
        uint64_t unit = f.alignBytes;
        while (unit > 1 && f.sizeBytes % unit != 0) unit /= 2;
        out = DeclTypeAndValue{};
        out.TypeName = unit == 8 ? "u64" : unit == 4 ? "u32" : unit == 2 ? "u16" : "u8";
        out.ConstArraySize = f.sizeBytes / unit;
        out.VariableName = f.name;
        return true;
}

void LLVMBackend::RegisterCRecords(std::vector<CRecordEntry>& records, const std::string& fileForLsp)
{
        if (records.empty()) return;

        // Pass 1: opaque shells. CFlat-defined types win; anonymous records are skipped
        // (clang inlines their fields at the JSON layer).
        std::vector<CRecordEntry*> ours;
        ours.reserve(records.size());
        for (auto& r : records)
        {
            if (r.name.empty()) continue;
            // The namespace a C++ import declared a class in, so a dotted name under it may be
            // requested as a specialization later (see cxxForeignNamespaces_).
            if (r.isCxx)
            {
                NoteCxxForeignNamespace(r.name);
                for (size_t pos = 0; (pos = r.name.find('.', pos)) != std::string::npos; ++pos)
                    RegisterNamespace(r.name.substr(0, pos));
            }
            auto existing = dataStructures.find(r.name);
            if (existing != dataStructures.end())
            {
                // The forward pass may create an opaque shell for a requested C++ template
                // specialization. Keep it in this pass so the extracted layout and members fill it.
                if (!r.isCxx || existing->second.StructType == nullptr
                    || !existing->second.StructType->isOpaque())
                    continue;
                ours.push_back(&r);
                continue;
            }
            // Create the opaque shell so later fields/records in the same batch can refer to it.
            CreateStructType(r.name, /*typeAndValues*/{});
            ours.push_back(&r);
        }

        // Pass 2: bodies. On unmappable fields leave the opaque shell in place so a later
        // reference surfaces a clear error rather than crashing on a partial struct.
        // Member registration is DEFERRED to pass 3: a member's ABI recipe needs the layout of
        // every class it takes or returns by value, and that class may be laid out later in this
        // same batch (`padded_string::operator padded_string_view()` precedes the view's body).
        std::vector<std::pair<CRecordEntry*, bool>> deferredMembers;   // (record, laid out)
        for (CRecordEntry* rp : ours)
        {
            CRecordEntry& r = *rp;
            // M6 - a class whose layout cflat could NOT flatten (virtual inheritance, or a
            // bitfield / anonymous member inside a hierarchy) keeps the opaque shell from pass 1:
            // a pointer to it stays a legal handle, while every by-value or member use is refused
            // at the use site by RejectUnsupportedCxxLayout. A polymorphic class WITHOUT those
            // problems is laid out for real below - the vptr slot and each base subobject are
            // already in r.fields at clang's own offsets.
            if (r.isCxx && !r.layoutRefusal.empty())
            {
                cxxRecords_.insert(r.name);
                deferredMembers.emplace_back(&r, false);
                if (auto* s = GetSymbolSink())
                    s->Register(SymbolKind::Struct, r.name, fileForLsp, r.line,
                                r.col < 0 ? 0 : r.col, "class " + r.name);
                continue;
            }
            std::vector<DeclTypeAndValue> fields;
            fields.reserve(r.fields.size());
            bool ok = true;
            std::string badFieldName;
            std::string badFieldType;
            for (const auto& f : r.fields)
            {
                if (r.isCxx && f.ctype.find("::*") != std::string::npos)
                {
                    r.layoutRefusal = std::format(
                        "field '{}' of '{}' is a pointer to member; member-pointer fields are not supported (an ordinary pointer is not equivalent)",
                        f.name, r.name);
                    badFieldName = f.name;
                    badFieldType = f.ctype;
                    ok = false;
                    break;
                }
                TypeAndValue tv;
                // Strip fixed-array dims before mapping: the shared mapper decays `[N]` to a
                // pointer (right for params, wrong for fields), so peel them here first.
                std::vector<uint64_t> arrDims;
                std::string elemSpelling = StripFixedArrayDims(f.ctype, arrDims);
                if (!MapCTypeToTypeAndValue(elemSpelling, tv))
                {
                    // A C++ field whose TYPE has no CFlat mapping yet (a class-template
                    // specialization such as `ImVector<T>`) still has a size and alignment
                    // from Clang: embed it as an opaque blob so the record and its other
                    // fields stay usable. Typed access to the blob is the request layer's
                    // follow-up (internal/issue/cppinterop/template-typed-field-drops-record.md).
                    DeclTypeAndValue blob;
                    if (r.isCxx && !f.isBitfield && arrDims.empty() && MakeOpaqueFieldBlob(f, blob))
                    {
                        if (verbose) std::cout << std::format("[verbose]   C++ struct '{}': field '{}' of type '{}' embedded as {} opaque bytes\n",
                            r.name, f.name, f.ctype, f.sizeBytes);
                        fields.push_back(std::move(blob));
                        continue;
                    }
                    badFieldName = f.name;
                    badFieldType = f.ctype;
                    if (verbose) std::cout << std::format("[verbose]   skipping C {} '{}': unsupported field '{}' of type '{}'\n",
                        r.isUnion ? "union" : "struct", r.name, f.name, f.ctype);
                    ok = false;
                    break;
                }
                if (!arrDims.empty())
                {
                    tv.ConstArraySize = arrDims[0];
                    tv.ConstInnerDimensions.assign(arrDims.begin() + 1, arrDims.end());
                }
                // A C fn-ptr field maps to a THIN function<T> ("__c_fn_ptr") - a bare,
                // pointer-sized C function pointer, same size as the void* it replaces, so the
                // struct layout is unchanged. Keeping the real signature makes MIDL COM vtable
                // slots (e.g. ID3D12DeviceVtbl) callable as `obj->lpVtbl->Method(obj, ...)`
                // through the existing thin-call path, instead of an opaque void* the user
                // must reinterpret by hand.

                // A pointer field to a KNOWN aggregate keeps its pointee type instead of decaying
                // to opaque void* (the shared mapper's default for struct pointers). This is what
                // makes a COM object's `lpVtbl` typed as `<Interface>Vtbl*` so the member-access
                // chain resolves; unknown/opaque pointees still fall back to void*.
                if (!tv.IsFunctionPointer && tv.Pointer && tv.TypeName == "void")
                {
                    int ptrLevels = 0;
                    std::string tag = AggregatePointeeTag(elemSpelling, ptrLevels);
                    if (!tag.empty() && ptrLevels <= 2 && dataStructures.find(tag) != dataStructures.end())
                        tv.TypeName = tag;   // keep Pointer / ElemPointer as the mapper set them
                }
                DeclTypeAndValue d;
                static_cast<TypeAndValue&>(d) = tv;
                d.VariableName = f.name;
                if (f.isBitfield)
                {
                    d.IsBitfield = true;
                    d.BitWidth = f.bitWidth;
                }
                fields.push_back(std::move(d));
            }
            if (fields.empty() && r.isCxx && r.fields.empty())
            {
                cxxRecords_.insert(r.name);
                deferredMembers.emplace_back(&r, false);
                continue;
            }
            if (!ok || fields.empty())
            {
                if (r.isCxx && r.layoutRefusal.empty())
                    r.layoutRefusal = IsLongDoubleSpelling(badFieldType)
                        && !IsCInteropLongDoubleSupported()
                    ? std::format("field '{}' of '{}': {}", badFieldName, r.name,
                                  CInteropLongDoubleRefusal())
                    : std::format(
                        "field '{}' of '{}' has unsupported C++ type '{}'",
                        badFieldName, r.name, badFieldType);
                if (r.isCxx && !r.layoutRefusal.empty())
                {
                    cxxRecords_.insert(r.name);
                    deferredMembers.emplace_back(&r, false);
                }
                continue;
            }
            // An opaque-shell by-value aggregate field has no size; CreateStructType would
            // assert "Cannot getTypeInfo() on unsized". Abandon and leave the shell.
            for (size_t fi = 0; fi < fields.size(); ++fi)
            {
                auto& d = fields[fi];
                if (d.Pointer) continue;            // pointers are always sized
                auto* ft = GetType(d);
                if (ft && !ft->isSized())
                {
                    // Same blob fallback: the field's record was itself refused or dropped,
                    // but Clang still told us how big the field is.
                    DeclTypeAndValue blob;
                    if (r.isCxx && fi < r.fields.size() && !r.fields[fi].isBitfield
                        && MakeOpaqueFieldBlob(r.fields[fi], blob))
                    {
                        if (verbose) std::cout << std::format("[verbose]   C++ struct '{}': field '{}' of unsized type '{}' embedded as {} opaque bytes\n",
                            r.name, d.VariableName, d.TypeName, r.fields[fi].sizeBytes);
                        d = std::move(blob);
                        continue;
                    }
                    if (verbose) std::cout << std::format("[verbose]   skipping C {} '{}': field '{}' has incomplete (unsized) type '{}'\n",
                        r.isUnion ? "union" : "struct", r.name, d.VariableName, d.TypeName);
                    ok = false;
                    break;
                }
            }
            if (!ok) continue;
            // Bitfield packing uses the same MSVC LSB-first layout as native CFlat bitfields;
            // the packing pass produces synthetic slots and CreateStructType stores BitfieldInfo.
            std::vector<BitfieldInfo> packedBitfields;
            bool anyBitfields = false;
            for (const auto& tv : fields) { if (tv.IsBitfield) { anyBitfields = true; break; } }
            // Save semantic fields before PackBitfields replaces them with __bfN slots;
            // used only for LSP symbol registration below.
            std::vector<DeclTypeAndValue> prePackFields;
            if (anyBitfields)
            {
                prePackFields = fields;
                fields = PackBitfields(fields, packedBitfields);
            }
            // A C++ record's layout is clang's, not CFlat's: insert explicit padding wherever
            // clang put a field further along than CFlat's natural packing would (over-aligned
            // members, empty-member slots), so field offsets agree before the type is built.
            if (r.isCxx && !r.isUnion && !anyBitfields)
                InsertCxxLayoutPadding(r, fields);
            if (r.isUnion)
                CreateUnionType(r.name, fields, r.isCxx ? r.alignBytes : 0);
            else
                CreateStructType(r.name, fields, r.isCxx ? r.alignBytes : 0,
                    anyBitfields ? &packedBitfields : nullptr, r.isCxx && r.isPacked);
            if (r.isCxx)
            {
                cxxRecords_.insert(r.name);
                const bool stdValueRecord = r.name.starts_with("std.pair$")
                                          || r.name.starts_with("std.optional$");
                if (r.isTriviallyCopyable || (stdValueRecord && r.hasTrivialDtor))
                    cxxTriviallyCopyableRecords_.insert(r.name);
                // A class that is NOT trivially copyable owns its lifetime: every construction,
                // copy, move and destruction of it must route through the C++ special members
                // (M4b). A POLYMORPHIC class always lands here - its constructor is what writes
                // the vptr, so a CFlat bitwise store could never produce a valid object.
                else if (!r.isUnion)
                    cxxNontrivialRecords_.insert(r.name);
                // A layout cflat could not reproduce is a per-record refusal, not an import
                // failure: pointers to the record stay usable, every by-value or field use is
                // rejected at its own site with this reason (RejectUnsupportedCxxLayout).
                if (std::string mismatch = VerifyCxxRecordLayout(r); !mismatch.empty())
                {
                    r.layoutRefusal = std::move(mismatch);
                    if (verbose) std::cout << std::format("[verbose]   C++ struct '{}': {}\n", r.name, r.layoutRefusal);
                }
                deferredMembers.emplace_back(&r, true);
            }
            if (auto* s = GetSymbolSink())
            {
                s->Register(SymbolKind::Struct, r.name, fileForLsp, r.line, r.col < 0 ? 0 : r.col,
                            (r.isUnion ? "union " : "struct ") + r.name);
                // For bitfield records use prePackFields (semantic names before packing).
                const auto& symFields = anyBitfields ? prePackFields : fields;
                for (const auto& f : symFields)
                {
                    if (f.VariableName.empty()) continue;  // skip unnamed padding markers
                    std::string annSig;
                    for (uint64_t d : f.ConstInnerDimensions)
                        annSig += "[" + std::to_string(d) + "] ";
                    if (f.ConstArraySize > 0)
                        annSig = "[" + std::to_string(f.ConstArraySize) + "] " + annSig;
                    std::string typeSig = SpellType(*this, f);
                    std::string fieldSig = annSig + typeSig + " " + f.VariableName;
                    if (f.IsBitfield && f.BitWidth > 0)
                        fieldSig += ":" + std::to_string(f.BitWidth);
                    s->Register(SymbolKind::Field, r.name + "." + f.VariableName,
                                fileForLsp, r.line, 0, fieldSig);
                }
            }
        }

        // Pass 3: members, now that every body in the batch exists.
        for (const auto& [rp, laidOut] : deferredMembers)
        {
            const CRecordEntry& r = *rp;
            RegisterCxxClassMembers(r, fileForLsp);
            if (!laidOut) continue;
            RegisterCxxInheritedMembers(r);
            // Hook the C++ complete-object destructor into the SAME destructor slot CFlat uses for
            // its own owning struct locals, so every existing scope-exit, early-return, break and
            // continue cleanup path destroys it exactly once.
            if (cxxNontrivialRecords_.count(r.name) != 0)
                GetOrCreateCxxClassDestructor(r.name);
        }

        // Register each header-COM interface's IID as its "uuid" type annotation (over ALL records,
        // not just freshly-created `ours`, so a cache hit re-annotates already-registered types).
        // EmitIidGlobalFor then resolves iidof(<HeaderComType>) through the existing uuid path.
        for (const auto& r : records)
        {
            if (r.uuid.empty()) continue;
            std::vector<AnnotationValue> anns;
            if (auto it = typeAnnotations_.find(r.name); it != typeAnnotations_.end()) anns = it->second;
            bool had = false;
            for (auto& a : anns) if (a.Name == "uuid") { a.Value = r.uuid; had = true; }
            if (!had) anns.push_back(AnnotationValue{ "uuid", r.uuid });
            SetTypeAnnotations(r.name, std::move(anns));
        }

        RegisterCMacroAliases({}, {}, fileForLsp);

        if (verbose)
            std::cout << std::format("[verbose]   registered {} C record(s) from {}\n", ours.size(), fileForLsp);
    }

bool LLVMBackend::RejectUnsupportedCxxLayout(const std::string& typeName)
{
        const CxxClassInfo* info = GetCxxClassInfo(typeName);
        if (info == nullptr || info->layoutRefusal.empty()) return false;
        LogError(std::format("C++ class '{}' {}", typeName, info->layoutRefusal));
        return true;
    }

bool LLVMBackend::RejectAbstractCxxClass(const std::string& typeName, const char* what)
{
        const CxxClassInfo* info = GetCxxClassInfo(typeName);
        if (info == nullptr || !info->isAbstract) return false;
        LogError(std::format(
            "cannot {} C++ class '{}': it is abstract (it has an unoverridden pure virtual "
            "member), so no complete object of it can exist - use a pointer to a derived class",
            what, typeName));
        return true;
    }

bool LLVMBackend::FindCxxBaseOffset(const std::string& derived, const std::string& base,
                                    uint64_t& offsetOut, bool& foundButInaccessible) const
{
        foundButInaccessible = false;
        if (derived == base) { offsetOut = 0; return true; }
        const CxxClassInfo* info = GetCxxClassInfo(derived);
        if (info == nullptr) return false;
        for (const auto& b : info->bases)
        {
            uint64_t inner = 0;
            bool innerInaccessible = false;
            const bool hit = b.name == base
                || FindCxxBaseOffset(b.name, base, inner, innerInaccessible);
            if (!hit) { foundButInaccessible = foundButInaccessible || innerInaccessible; continue; }
            if (b.access != cflat_cinterop::AccessPublic || innerInaccessible)
            {
                foundButInaccessible = true;
                continue;
            }
            offsetOut = b.offsetBytes + (b.name == base ? 0 : inner);
            return true;
        }
        return false;
    }

llvm::Value* LLVMBackend::EmitCxxBaseAdjust(llvm::Value* ptr, uint64_t offsetBytes)
{
        if (ptr == nullptr || offsetBytes == 0) return ptr;
        auto* i8 = builder->getInt8Ty();
        llvm::Value* shifted = builder->CreateGEP(i8, ptr, builder->getInt64(offsetBytes),
                                                 "cxx.base");
        // The C++ derived-to-base conversion is null-preserving; clang emits the same select.
        llvm::Value* isNull = builder->CreateICmpEQ(
            ptr, llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptr->getType())),
            "cxx.base.isnull");
        return builder->CreateSelect(isNull, ptr, shifted, "cxx.base.adj");
    }

llvm::Value* LLVMBackend::AdjustCxxPointerForStore(const TypeAndValue& dest,
                                                   const TypeAndValue& src, llvm::Value* value,
                                                   const std::string& destDesc)
{
        if (value == nullptr || !value->getType()->isPointerTy()) return value;
        if (!dest.Pointer || !src.Pointer || dest.TypeName == src.TypeName) return value;
        if (!IsCxxRecord(dest.TypeName) || !IsCxxRecord(src.TypeName)) return value;
        uint64_t off = 0;
        bool inaccessible = false;
        if (FindCxxBaseOffset(src.TypeName, dest.TypeName, off, inaccessible))
            return EmitCxxBaseAdjust(value, off);
        if (inaccessible)
            LogError(std::format(
                "cannot convert '{}*' to '{}*' for {}: '{}' is not a PUBLIC base of '{}', and a "
                "conversion to a private or protected base is not allowed",
                src.TypeName, dest.TypeName, destDesc, dest.TypeName, src.TypeName));
        return value;
    }

llvm::Value* LLVMBackend::EmitCxxVirtualCallee(const FunctionSymbol& candidate,
                                               llvm::Value* thisPtr)
{
        if (thisPtr == nullptr || candidate.Function == nullptr) return nullptr;
        auto it = cxxVirtualSlotByLinkage_.find(candidate.UniqueName);
        if (it == cxxVirtualSlotByLinkage_.end()) return nullptr;
        auto* ptrTy = cflat_llvm::PointerTo(builder->getInt8Ty());
        llvm::Value* vptr = builder->CreateLoad(ptrTy, thisPtr, "vtable");
        llvm::Value* slot = builder->CreateGEP(ptrTy, vptr, builder->getInt64(it->second),
                                               "vfn.slot");
        return builder->CreateLoad(ptrTy, slot, "vfn");
    }

bool LLVMBackend::EmitCxxVirtualDelete(const std::string& typeName, llvm::Value* ptr)
{
        const CxxClassInfo* info = GetCxxClassInfo(typeName);
        if (info == nullptr || info->dtorDeletingVtableIndex < 0 || ptr == nullptr) return false;
        AbiRecipe recipe;
        llvm::Function* proto = GetOrCreateCxxStructor(typeName, info->destructor, recipe);
        if (proto == nullptr) return false;
        auto* ptrTy = cflat_llvm::PointerTo(builder->getInt8Ty());
        llvm::Value* vptr = builder->CreateLoad(ptrTy, ptr, "vtable");
        llvm::Value* slot = builder->CreateGEP(ptrTy, vptr,
                                               builder->getInt64(info->dtorDeletingVtableIndex),
                                               "vdel.slot");
        llvm::Value* fn = builder->CreateLoad(ptrTy, slot, "vdel");
        // The deleting destructor has the same signature as the complete-object one; it destroys
        // the object AND releases its storage through the C++ deallocator, so no separate
        // destructor call and no operator delete may follow.
        builder->CreateCall(proto->getFunctionType(), fn, { ptr });
        return true;
    }

bool LLVMBackend::RejectInaccessibleCxxMember(const std::string& typeName,
                                              const std::string& memberName)
{
        const CxxClassInfo* info = GetCxxClassInfo(typeName);
        if (info == nullptr) return false;
        // A member of a class whose LAYOUT cflat could not reproduce reports the class, not the
        // member: nothing about such a type is bindable, so naming the reason is the useful answer.
        if (!info->layoutRefusal.empty()
            && (info->fieldAccess.count(memberName) != 0 || info->memberAccess.count(memberName) != 0))
            return RejectUnsupportedCxxLayout(typeName);
        if (auto f = info->fieldAccess.find(memberName); f != info->fieldAccess.end())
        {
            if (f->second == cflat_cinterop::AccessPublic) return false;
            LogError(std::format("field '{}' of C++ class '{}' is {}",
                                 memberName, typeName,
                                 f->second == cflat_cinterop::AccessPrivate ? "private" : "protected"));
            return true;
        }
        if (auto m = info->refusedMembers.find(memberName); m != info->refusedMembers.end())
        {
            const std::string refusal = m->second;
            if (TryBindRefusedCxxMember(typeName, memberName)) return false;
            /*
             * A refusal only speaks when NO overload of that name was bound. A libc++ class
             * commonly has one bindable overload and one cflat cannot express (`append(const
             * char*)` next to `append(initializer_list<char>)`); reporting the refused one would
             * make the callable overload unreachable.
             */
            const CxxClassInfo* currentInfo = GetCxxClassInfo(typeName);
            if (currentInfo != nullptr
                && std::find(currentInfo->instanceMethodNames.begin(),
                             currentInfo->instanceMethodNames.end(), memberName)
                       != currentInfo->instanceMethodNames.end())
                return false;
            if (functionTable.count(typeName + "." + memberName) != 0) return false;   // static
            LogError(std::format("member '{}' of C++ class '{}' {}", memberName, typeName, refusal));
            return true;
        }
        return false;
    }

/*
 * Publish one imported C++ class's callable surface.
 *
 * Instance methods land in functionTable under their BARE name with 'this' as the first
 * parameter, which is exactly the shape CFlat's own struct methods have - so `obj.method(a)` and
 * `ptr.method(a)` dispatch through the existing member-call path with no new lowering. Static
 * methods and out-of-line static data members are published under the dotted type name
 * ("ns.Class.member"), the same spelling the free-function path already uses for namespaces.
 * Constructors and the destructor are NOT user-callable names; they go into CxxClassInfo, which
 * lifetime codegen reads.
 *
 * Nothing is refused silently: a member CFlat cannot bind (virtual, deleted, inaccessible, only
 * inline-defined in the header) is recorded in refusedMembers so the use site can say why.
 */
/*
 * M5b - a C++ REFERENCE at a bound boundary is CFlat's `alias T`: the same machine representation
 * (a pointer) and the same rules (an lvalue passes its address, an rvalue materializes a temporary).
 * That makes `v.push_back(3)`, `s.append(other)` and `v[0] = 5` work without a spelling for
 * references in CFlat. Returns the referred-to spelling, or the input unchanged.
 */
enum class CxxReferenceKind { None, Lvalue, Rvalue, RefToPointer };

static std::string CxxSpellingWithoutRef(const std::string& spelling,
                                         CxxReferenceKind* kind = nullptr)
{
        std::string s = spelling;
        while (!s.empty() && s.back() == ' ') s.pop_back();
        int refs = 0;
        while (!s.empty() && s.back() == '&') { s.pop_back(); ++refs; }
        if (kind != nullptr) *kind = CxxReferenceKind::None;
        if (refs == 0) return spelling;
        while (!s.empty() && s.back() == ' ') s.pop_back();
        if (kind != nullptr)
            *kind = refs > 1 ? CxxReferenceKind::Rvalue
                             : (!s.empty() && s.back() == '*'
                                ? CxxReferenceKind::RefToPointer : CxxReferenceKind::Lvalue);
        return s;
    }

void LLVMBackend::RegisterCxxClassMembers(const CRecordEntry& r, const std::string& fileForLsp,
                                          const std::string& memberFilter)
{
        if (!r.isCxx) return;
        cxxRecordEntries_[r.name] = r;
        // A class whose layout was REFUSED still needs its CxxClassInfo: that is where the refusal
        // text and the field list the diagnostic reads from live.
        // Empty C++ classes still have constructors/destructors and are valid foreign types.
        // Keep the initial header walk bounded; requested specializations carry a '$' in their
        // CFlat identity and are retained even when Clang reports no callable members.
        if (r.members.empty() && r.staticVars.empty() && r.layoutRefusal.empty()
            && r.name.find('$') == std::string::npos && !IsCxxForeignTypeRegistered(r.name)) return;
        // A class imported from a C++ HEADER group also gets a C++ spelling, so it can be a
        // template argument (`cppt.Box<cppi.Tracked>`). A request already recorded its own
        // spelling, which is the one the request TU was built with - never overwrite it.
        if (!r.canonicalCtype.empty() && cxxCflatToCxxSpelling_.count(r.name) == 0)
        {
            std::string spelling = r.canonicalCtype;
            for (const char* tag : { "class ", "struct ", "union ", "enum " })
                if (spelling.rfind(tag, 0) == 0) { spelling.erase(0, strlen(tag)); break; }
            if (!spelling.empty()) cxxCflatToCxxSpelling_[r.name] = spelling;
            // Reverse direction: clang spells this class canonically in every member signature it
            // appears in (`push_back(const cppi::Tracked&)` inside a requested specialization), so
            // the C type mapper needs the spelling -> CFlat identity entry as well. Both the
            // tagged and untagged forms, since a signature may carry either.
            if (cxxForeignTypeSpellings_.count(SqueezeCxxSpelling(r.canonicalCtype)) == 0)
                cxxForeignTypeSpellings_[SqueezeCxxSpelling(r.canonicalCtype)] = r.name;
            if (!spelling.empty() && cxxForeignTypeSpellings_.count(SqueezeCxxSpelling(spelling)) == 0)
                cxxForeignTypeSpellings_[SqueezeCxxSpelling(spelling)] = r.name;
        }

        CxxClassInfo info;
        if (!memberFilter.empty())
        {
            auto existing = cxxClasses_.find(r.name);
            if (existing == cxxClasses_.end()) return;
            info = existing->second;
        }
        else
        {
            info.isPolymorphic         = r.isPolymorphic;
            info.hasBases              = r.hasBases;
            info.hasVirtualBases       = r.hasVirtualBases;
            info.isAbstract            = r.isAbstract;
            info.layoutRefusal         = r.layoutRefusal;
            for (const auto& b : r.bases)
            {
                CxxClassInfo::BaseRef br;
                br.name = b.name; br.offsetBytes = b.offsetBytes; br.access = b.access;
                info.bases.push_back(std::move(br));
            }
            info.hasTrivialDefaultCtor = r.hasTrivialDefaultCtor;
            info.hasTrivialCopyCtor    = r.hasTrivialCopyCtor;
            info.hasTrivialDtor        = r.hasTrivialDtor;
            info.hasDeletedDefaultCtor = r.hasDeletedDefaultCtor;
            info.hasDeletedCopyCtor    = r.hasDeletedCopyCtor;
            info.hasDefaultCtor        = r.hasDefaultCtor;
            info.hasCopyCtor           = r.hasCopyCtor;
            info.isAggregate           = r.isAggregate;
            for (const auto& f : r.fields)
                if (!f.name.empty()) info.fieldAccess[f.name] = f.access;
        }

        // A member's declared type, mapped through the shared C spelling mapper. A pointer to a
        // KNOWN aggregate keeps its pointee (the mapper decays struct pointers to void*), which is
        // what makes a method returning `Other*` chain into `Other`'s own members.
        auto mapType = [&](const std::string& spelling, TypeAndValue& tv) -> bool {
            std::vector<uint64_t> dims;
            std::string elem = StripFixedArrayDims(spelling, dims);
            if (!MapCTypeToTypeAndValue(elem, tv)) return false;
            if (!tv.IsFunctionPointer && tv.Pointer && tv.TypeName == "void")
            {
                int ptrLevels = 0;
                std::string tag = AggregatePointeeTag(elem, ptrLevels);
                if (!tag.empty() && ptrLevels <= 2 && dataStructures.find(tag) != dataStructures.end())
                    tv.TypeName = tag;
            }
            if (!dims.empty())
            {
                tv.ConstArraySize = dims[0];
                tv.ConstInnerDimensions.assign(dims.begin() + 1, dims.end());
            }
            return true;
        };

        using Member = cflat_cinterop::RawCxxMember;

        // const/non-const overload pair: CFlat drops const, so both spell the same CFlat
        // signature. RULING: an lvalue prefers the non-const overload, and the const one is bound
        // only when it is the sole candidate. Decide that here, once, by signature key.
        // The key is the signature CFlat SEES: const is dropped, and `const T&` / `T&` / `T&&`
        // all lvalue references collapse onto `alias T`; T&& keeps a distinct rvalue-alias key.
        /*
         * A conversion function is published under the CFlat spelling of its target, which is the
         * name the explicit-cast path looks up ("operator int", "operator double"). A target the
         * type map cannot express keeps the C++ source name: CFlat has no way to spell that cast,
         * so no use site can reach the entry - it exists only as a recorded refusal.
         */
        auto memberRegName = [&](const Member& m) -> std::string {
            if (!m.isConversion) return m.name;
            TypeAndValue convRet;
            if (!mapType(m.retType, convRet)) return m.name;
            std::string spelling = SpellType(*this, convRet);
            return spelling.empty() ? m.name : "operator " + spelling;
        };
        auto cflatSigKey = [&](const Member& m) {
            std::string key = memberRegName(m);
            for (size_t p = 1; p < m.paramTypes.size(); ++p)
            {
                CxxReferenceKind refKind = CxxReferenceKind::None;
                std::string t = CxxSpellingWithoutRef(m.paramTypes[p], &refKind);
                if (t.rfind("const ", 0) == 0) t = t.substr(6);
                if (refKind == CxxReferenceKind::Rvalue) t += "|rvalue";
                else if (refKind == CxxReferenceKind::RefToPointer) t += "|refptr";
                key += "|" + t;
            }
            return key;
        };
        std::map<std::string, size_t> instanceBySig;
        for (size_t i = 0; i < r.members.size(); ++i)
        {
            const Member& m = r.members[i];
            // Copy and move assignment are captured by FLAG, not by CFlat signature, and they
            // collapse onto the same key (`const T&` and `T&&` both drop to `T`) - deduping them
            // here would delete the move leg of every class that has one.
            if (m.kind != Member::Instance || m.isCopyAssign || m.isMoveAssign) continue;
            const std::string key = cflatSigKey(m);
            auto it = instanceBySig.find(key);
            if (it == instanceBySig.end()) { instanceBySig[key] = i; continue; }
            const Member& kept = r.members[it->second];
            // Prefer a non-const method over its const twin, and an lvalue-reference
            // parameterization over the rvalue-reference one (a const T& overload accepts both an
            // lvalue and a materialized temporary; a T&& overload accepts only the temporary).
            bool keptRvalue = false, mineRvalue = false;
            for (size_t p = 1; p < kept.paramTypes.size(); ++p)
                if (kept.paramTypes[p].size() > 1
                    && kept.paramTypes[p].compare(kept.paramTypes[p].size() - 2, 2, "&&") == 0)
                    keptRvalue = true;
            for (size_t p = 1; p < m.paramTypes.size(); ++p)
                if (m.paramTypes[p].size() > 1
                    && m.paramTypes[p].compare(m.paramTypes[p].size() - 2, 2, "&&") == 0)
                    mineRvalue = true;
            if (kept.isConst && !m.isConst) it->second = i;
            else if (keptRvalue && !mineRvalue) it->second = i;
        }

        for (size_t i = 0; i < r.members.size(); ++i)
        {
            const Member& m = r.members[i];
            const bool isStructor = m.kind == Member::Constructor || m.kind == Member::Destructor;

            // Settled BEFORE the refusal checks so a private, deleted or otherwise unbindable
            // conversion is recorded under the same key the cast site will ask for.
            const std::string cflatName = memberRegName(m);
            if (!memberFilter.empty() && cflatName != memberFilter) continue;

            if (m.kind == Member::Instance || m.kind == Member::StaticMethod)
            {
                auto ma = info.memberAccess.find(cflatName);
                if (ma == info.memberAccess.end()) info.memberAccess[cflatName] = m.access;
                else if (m.access < ma->second)    ma->second = m.access;
            }

            auto refuse = [&](const std::string& why) {
                if (verbose)
                    std::cout << std::format("[verbose]   C++ member {}.{} not bound: {}\n",
                                             r.name, m.name, why);
                if (!isStructor && info.refusedMembers.count(cflatName) == 0)
                    info.refusedMembers[cflatName] = why;
            };
            if (m.access == cflat_cinterop::AccessPrivate)    { refuse("is private");   continue; }
            if (m.access == cflat_cinterop::AccessProtected)  { refuse("is protected"); continue; }
            if (m.isDeleted)              { refuse("is deleted");                        continue; }
            if (!m.bindRefusal.empty())   { refuse(m.bindRefusal);                       continue; }
            if (!r.layoutRefusal.empty() && m.kind != Member::StaticMethod)
            {
                refuse("belongs to a class whose layout cflat cannot reproduce");
                continue;
            }
            if (m.variadic)               { refuse("is variadic");                       continue; }
            // A POINTER TO MEMBER has an ABI representation of its own (Itanium: a two-word
            // {ptr, adj} pair for a member function, a byte offset for a data member) and its own
            // invocation sequence; an ordinary function pointer is not a substitute. Name it
            // explicitly rather than letting the type mapper report "unsupported type".
            {
                bool memberPtr = m.retType.find("::*") != std::string::npos;
                for (const auto& pt : m.paramTypes)
                    if (pt.find("::*") != std::string::npos) memberPtr = true;
                if (memberPtr)
                {
                    refuse("uses a pointer to member, whose ABI representation and invocation "
                           "are not implemented (an ordinary function pointer is not equivalent)");
                    continue;
                }
            }
            // A VIRTUAL member is dispatched through the vtable. An all-inline hierarchy is
            // supported now (M7): clang emits the bodies AND the vtable into the companion module.
            // This only fires when no definition exists anywhere - no symbol in the bound library
            // and no body clang could emit - in which case the vtable slot would be empty.
            if (m.isVirtual && m.needsLocalDefinition)
                                          { refuse("is virtual and has no definition cflat can reach: neither an out-of-line symbol in the bound library nor a body clang could emit, so its vtable slot is empty"); continue; }
            if (m.isVirtual && m.covariantReturnNeedsAdjust)
                                          { refuse("has a covariant return type whose base conversion is not at offset zero, which needs a return-adjusting thunk cflat cannot synthesize (not supported yet)"); continue; }
            if (m.isVirtual && m.vtableIndex < 0)
                                          { refuse("is virtual but cflat could not determine its vtable slot"); continue; }
            // Inline bodies ARE emitted (M7). What is left here is a member with no body at all:
            // an implicit or defaulted special member Sema declined to define, or a template
            // member awaiting instantiation support.
            if (m.needsLocalDefinition)   { refuse("has no definition cflat can reach: clang emitted no body for it (an implicit, defaulted or template member)"); continue; }
            if (!m.abi.valid)             { refuse("has a calling convention cflat cannot reproduce"); continue; }
            if (m.linkageName.empty())    { refuse("has no external linkage");            continue; }
            if (m.kind == Member::Instance && !m.isCopyAssign && !m.isMoveAssign)
            {
                auto it = instanceBySig.find(cflatSigKey(m));
                if (it != instanceBySig.end() && it->second != i) continue;   // const / ref twin
            }

            TypeAndValue ret;
            if (!mapType(m.retType, ret))
            {
                refuse(IsLongDoubleSpelling(m.retType) && !IsCInteropLongDoubleSupported()
                    ? CInteropLongDoubleRefusal()
                    : std::format("returns unsupported type '{}'", m.retType));
                continue;
            }
            CxxReferenceKind returnRefKind = CxxReferenceKind::None;
            CxxSpellingWithoutRef(m.retType, &returnRefKind);
            if (returnRefKind == CxxReferenceKind::Lvalue && ret.Pointer
                && ret.TypeName == "void")
            {
                // An unknown T& is represented as void* by the scalar mapper. Do not turn it
                // into an alias void result: that would declare a different LLVM return type
                // from clang and report a misleading ABI mismatch.
                refuse(std::format("returns a reference to unsupported type '{}'", m.retType));
                continue;
            }
            /*
             * A C++ LVALUE REFERENCE return (`int &`, `std::string &` - what `operator[]` and
             * `back()` hand out) is a borrowed lvalue, which is exactly CFlat's `alias T`: the
             * callee returns a pointer (the Itanium representation, so the ABI is unchanged) and
             * the call site turns it back into storage, so `int x = v[0];` reads and `v[0] = 5;`
             * stores. Without this the result would be a bare `int *` and every use would need a
             * manual dereference. Structors and assignment operators keep their own shapes.
             */
            // A structor's parameters and an assignment operator's result keep the pointer shape
            // the construct-into-slot and assignment paths already expect.
            const bool aliasRefs = !isStructor && !m.isCopyAssign && !m.isMoveAssign;
            auto asAliasIfRef = [&](const std::string& spelling, TypeAndValue& tv) {
                CxxReferenceKind refKind = CxxReferenceKind::None;
                const std::string bare = CxxSpellingWithoutRef(spelling, &refKind);
                if (refKind == CxxReferenceKind::None || refKind == CxxReferenceKind::Rvalue) return;
                if (!tv.Pointer || tv.IsFunctionPointer || tv.IsArrayView) return;
                if (tv.ElemPointer)
                {
                    if (bare.empty() || bare.back() != '*'
                        || std::count(bare.begin(), bare.end(), '*') != 1)
                        return;
                    tv.ElemPointer = false;
                    tv.IsCxxRefToPointer = true;
                }
                else
                    tv.Pointer = false;
                tv.IsAlias = true;
            };
            if (aliasRefs) asAliasIfRef(m.retType, ret);
            std::vector<TypeAndValue> params;
            bool paramsOk = true;
            for (size_t p = 0; p < m.paramTypes.size(); ++p)
            {
                TypeAndValue tv;
                if (p == 0 && m.kind != Member::StaticMethod)
                {
                    // 'this' is spelled as a pointer to the record itself, never as the decayed
                    // void* the string mapper produces for a struct pointer - the member-call path
                    // matches the receiver against Parameters[0].TypeName.
                    tv = TypeAndValue{};
                    tv.TypeName = r.name;
                    tv.Pointer = true;
                }
                else if (!mapType(m.paramTypes[p], tv)
                         || (!tv.Pointer && !tv.IsFunctionPointer && tv.TypeName == "void"))
                {
                    // A by-value spelling the mapper cannot express arrives as bare "void";
                    // an LLVM function type with a void parameter asserts (garbage in Release).
                    refuse(IsLongDoubleSpelling(m.paramTypes[p])
                            && !IsCInteropLongDoubleSupported()
                        ? CInteropLongDoubleRefusal()
                        : std::format("takes unsupported type '{}'", m.paramTypes[p]));
                    paramsOk = false;
                    break;
                }
                else if (aliasRefs) asAliasIfRef(m.paramTypes[p], tv);
                // libc++ keeps vector<T*>::push_back's reference in the dependent
                // value_type spelling on some Clang versions, which canonicalizes without
                // exposing '&'. Its ABI is still the T*& slot shape.
                if (aliasRefs && !tv.IsCxxRefToPointer && tv.Pointer && tv.ElemPointer
                    && r.name.starts_with("std.vector$") && m.name == "push_back")
                {
                    tv.ElemPointer = false;
                    tv.IsCxxRefToPointer = true;
                    tv.IsAlias = true;
                }
                tv.VariableName = p < m.paramNames.size() && !m.paramNames[p].empty()
                    ? m.paramNames[p] : std::format("p{}", p);
                // The mapped LLVM type must be a legal function argument (never void, a bare
                // function type, or a non-first-class type): LLVM asserts, Release miscompiles.
                if (llvm::Type* lt = GetType(tv); lt == nullptr || !llvm::FunctionType::isValidArgumentType(lt))
                {
                    refuse(std::format("takes unsupported type '{}'", m.paramTypes[p]));
                    paramsOk = false;
                    break;
                }
                params.push_back(std::move(tv));
            }
            if (!paramsOk) continue;

            if (isStructor || m.isCopyAssign || m.isMoveAssign)
            {
                CxxClassInfo::Structor st;
                st.linkageName = m.linkageName;
                st.params = params;
                // On Itanium/Darwin a structor hands 'this' back and an assignment operator
                // returns 'T&'. Declare the callee with the result clang actually emits: the
                // exported retType is "void" for a structor, which would mis-type the callee.
                if (isStructor && m.returnsThis)
                {
                    st.ret = TypeAndValue{};
                    st.ret.TypeName = r.name;
                    st.ret.Pointer = true;
                }
                else
                {
                    st.ret = ret;
                }
                st.isDefaultCtor = m.isDefaultCtor;
                st.isCopyCtor = m.isCopyCtor;
                st.isMoveCtor = m.isMoveCtor;
                st.isDeleted = m.isDeleted;
                st.needsLocalDefinition = m.needsLocalDefinition;
                st.isNoexcept = m.isNoexcept;
                st.access = m.access;
                st.abi = m.abi;
                st.defaultArgs = m.defaultArgs;
                if (m.kind == Member::Constructor) info.constructors.push_back(std::move(st));
                else if (m.kind == Member::Destructor)
                {
                    info.destructor = std::move(st); info.hasDtor = true;
                    if (m.isVirtual)
                    {
                        info.dtorVtableIndex = m.vtableIndex;
                        info.dtorDeletingVtableIndex = m.vtableIndexDeleting;
                    }
                }
                else if (m.isCopyAssign) { info.copyAssign = std::move(st); info.hasCopyAssign = true; }
                else { info.moveAssign = std::move(st); info.hasMoveAssign = true; }
                continue;
            }

            const std::string regName = m.kind == Member::StaticMethod
                ? r.name + "." + cflatName
                : cflatName;
            if (m.kind == Member::StaticMethod)
            {
                NoteCxxForeignNamespace(regName);
                for (size_t pos = 0; (pos = regName.find('.', pos)) != std::string::npos; ++pos)
                    RegisterNamespace(regName.substr(0, pos));
            }

            std::string abiMismatch;
            {
                CInteropDeclarationScope declaringFile(*this, m.file.empty() ? fileForLsp : m.file);
                CxxAbiPlanScope abiPlan(*this, &m.abi, &abiMismatch);
                CreateFunctionDeclaration(regName, ret, params, /*external=*/true, /*varargs=*/false,
                                          /*returnsOwned=*/false,
                                          /*isMethod=*/m.kind == Member::Instance,
                                          CallingConv::Cdecl, m.linkageName, /*isCxx=*/true,
                                          m.isNoexcept);
            }
            // A member whose lowering clang disagrees with is refused HERE, at registration: the
            // use site reports why, and importing the class stays clean.
            if (!abiMismatch.empty()) { refuse(abiMismatch); continue; }
            if (auto fit = functionTable.find(regName); fit != functionTable.end())
                for (FunctionSymbol& sym : fit->second)
                    if (sym.External && sym.UniqueName == m.linkageName)
                    {
                        sym.IsCInteropDeclaration = true;
                        sym.DefaultArguments = m.defaultArgs;
                    }
            if (m.kind == Member::Instance)
            {
                info.instanceMethodNames.push_back(cflatName);
                // A virtual member is called through the receiver's vptr at this slot; the
                // declaration above exists only to carry clang's arrangement and to keep the
                // callee's LLVM type available.
                if (m.isVirtual) cxxVirtualSlotByLinkage_[m.linkageName] = m.vtableIndex;
            }

            if (auto* s = GetSymbolSink())
            {
                std::string sig = SpellType(*this, ret) + " " + r.name + "." + cflatName + "(";
                bool first = true;
                for (size_t p = (m.kind == Member::Instance ? 1u : 0u); p < params.size(); ++p)
                {
                    if (!first) sig += ", ";
                    first = false;
                    sig += SpellType(*this, params[p]);
                }
                sig += ")";
                s->Register(SymbolKind::Function, r.name + "." + cflatName,
                            m.file.empty() ? fileForLsp : m.file, m.line, m.col < 0 ? 0 : m.col, sig);
            }
        }

        if (memberFilter.empty()) for (const auto& sv : r.staticVars)
        {
            if (sv.access != cflat_cinterop::AccessPublic) continue;
            TypeAndValue tv;
            if (!mapType(sv.ctype, tv)) continue;
            const std::string regName = r.name + "." + sv.name;
            if (globalNamedVariable.count(regName)) continue;
            for (size_t pos = 0; (pos = regName.find('.', pos)) != std::string::npos; ++pos)
                RegisterNamespace(regName.substr(0, pos));
            tv.VariableName = regName;
            llvm::Constant* init = nullptr;
            if (sv.isCompileTimeConstant)
            {
                llvm::Type* valueType = GetType(tv);
                if (!valueType->isIntegerTy()) continue;
                init = llvm::ConstantInt::get(valueType, (uint64_t)sv.constantValue, true);
            }
            auto* staticGlobal = CreateGlobalVariable(tv, init, /*threadLocal*/ false, /*userAlign*/ 0,
                                 /*externalDecl*/ !sv.isCompileTimeConstant,
                                 /*srcIsUnsigned*/ false, sv.linkageName);
            if (sv.isCompileTimeConstant)
            {
                SetConstGlobalInt(regName, sv.constantValue);
                SetConstGlobalInt(staticGlobal->getName().str(), sv.constantValue);
            }
            if (auto* s = GetSymbolSink())
                s->Register(SymbolKind::Variable, regName,
                            sv.file.empty() ? fileForLsp : sv.file, sv.line, sv.col < 0 ? 0 : sv.col,
                            SpellType(*this, tv) + " " + regName
                                + (sv.isCompileTimeConstant
                                    ? ConstIntValueSuffix(tv.TypeName, sv.constantValue) : ""));
        }

        cxxClasses_[r.name] = std::move(info);
    }

bool LLVMBackend::TryBindRefusedCxxMember(const std::string& typeName,
                                          const std::string& memberName)
{
        auto infoIt = cxxClasses_.find(typeName);
        auto recordIt = cxxRecordEntries_.find(typeName);
        if (infoIt == cxxClasses_.end() || recordIt == cxxRecordEntries_.end()) return false;
        auto refusalIt = infoIt->second.refusedMembers.find(memberName);
        if (refusalIt == infoIt->second.refusedMembers.end()) return false;

        const std::string& refusal = refusalIt->second;
        const bool unsupportedReturn = refusal.starts_with("returns unsupported type '");
        const bool unsupportedParameter = refusal.starts_with("takes unsupported type '");
        const bool incompleteReturn = refusal.starts_with("returns '")
            && refusal.find("' by value, whose definition this translation unit does not have")
                   != std::string::npos;
        const bool incompleteParameter = refusal.starts_with("takes '")
            && refusal.find("' by value, whose definition this translation unit does not have")
                   != std::string::npos;
        if (!unsupportedReturn && !unsupportedParameter
            && !incompleteReturn && !incompleteParameter)
            return false;

        std::map<std::string, std::string> requests;
        std::set<std::string> successfulSpellings;
        for (const auto& member : recordIt->second.members)
        {
            if (member.name != memberName
                || (member.kind != cflat_cinterop::RawCxxMember::Instance
                    && member.kind != cflat_cinterop::RawCxxMember::StaticMethod))
                continue;
            auto addSpelling = [&](const std::string& raw) {
                const std::string spelling = CxxMemberValueSpelling(raw);
                if (spelling.find("::") == std::string::npos) return;
                TypeAndValue mapped;
                bool mappedForeign = false;
                const bool mapFound = TryMapCxxForeignSpelling(spelling, mapped, mappedForeign);
                if (mapFound && mappedForeign)
                {
                    successfulSpellings.insert(spelling);
                    return;
                }
                const std::string identity = AutoCxxForeignIdentity(spelling);
                if (!identity.empty()) requests.emplace(identity, spelling);
            };
            if (unsupportedReturn || incompleteReturn) addSpelling(member.retType);
            if (unsupportedParameter || incompleteParameter)
                for (const auto& spelling : member.paramTypes) addSpelling(spelling);
        }
        if (requests.empty() && successfulSpellings.empty()) return false;

        auto ownerIt = cxxTypeOwnerGroup_.find(typeName);
        if (ownerIt == cxxTypeOwnerGroup_.end()) return false;
        CxxRequestGroup group = MakeCxxRequestGroup(ownerIt->second, {});
        if (group.headers.empty()) return false;
        CxxRequestGroupScope groupScope(*this, &group);
        for (const auto& [identity, spelling] : requests)
        {
            std::string error;
            if (RequestCxxForeignType(identity, spelling, error, /*needDefinitions*/ true,
                                      /*explicitInstantiation*/ true))
                successfulSpellings.insert(spelling);
        }
        if (successfulSpellings.empty()) return false;

        const std::string fileForLsp = recordIt->second.members.empty()
            ? std::string() : recordIt->second.members.front().file;
        const CxxClassInfo previousInfo = infoIt->second;
        const CRecordEntry previousRecord = recordIt->second;
        CRecordEntry reboundRecord = recordIt->second;
        if (incompleteReturn || incompleteParameter)
        {
            std::string ownerSpelling;
            if (auto spelling = cxxCflatToCxxSpelling_.find(typeName);
                spelling != cxxCflatToCxxSpelling_.end())
                ownerSpelling = spelling->second;
            if (ownerSpelling.empty())
            {
                ownerSpelling = typeName;
                const size_t templateSep = ownerSpelling.find('$');
                if (templateSep != std::string::npos) ownerSpelling.erase(templateSep);
                for (size_t pos = 0; (pos = ownerSpelling.find('.', pos)) != std::string::npos; )
                { ownerSpelling.replace(pos, 1, "::"); pos += 2; }
            }
            std::vector<CxxRequestItem> refreshItems;
            for (const auto& [identity, spelling] : requests)
                refreshItems.push_back({ identity, spelling, true, true });
            refreshItems.push_back({ typeName, ownerSpelling, true, true });
            cflat_cinterop::ExtractResult refreshed;
            std::string refreshError;
            if (RunCxxTypeRequests(group, refreshItems, {}, /*emitDefinitions*/ true,
                                    refreshed, refreshError))
            {
                std::vector<CRecordEntry> mapped;
                MapRawRecords(refreshed, mapped);
                for (const CRecordEntry& candidate : mapped)
                    if (candidate.name == typeName)
                    { reboundRecord = candidate; break; }
            }
        }
        for (auto& member : reboundRecord.members)
        {
            if (member.name != memberName
                || (member.kind != cflat_cinterop::RawCxxMember::Instance
                    && member.kind != cflat_cinterop::RawCxxMember::StaticMethod))
                continue;
            auto wasRequested = [&](const std::string& raw) {
                const std::string spelling = CxxMemberValueSpelling(raw);
                return successfulSpellings.count(spelling) != 0;
            };
            if (((unsupportedReturn || incompleteReturn) && wasRequested(member.retType))
                || ((unsupportedParameter || incompleteParameter)
                    && std::any_of(member.paramTypes.begin(),
                                   member.paramTypes.end(), wasRequested)))
                member.bindRefusal.clear();
        }
        RegisterCxxClassMembers(reboundRecord, fileForLsp, memberName);

        auto updated = cxxClasses_.find(typeName);
        if (updated == cxxClasses_.end()) return false;
        const bool instanceBound = std::find(updated->second.instanceMethodNames.begin(),
                                             updated->second.instanceMethodNames.end(),
                                             memberName)
                                != updated->second.instanceMethodNames.end();
        bool staticBound = false;
        if (!instanceBound)
        {
            auto functions = functionTable.find(typeName + "." + memberName);
            if (functions != functionTable.end())
                for (const auto& function : functions->second)
                    if (function.IsCxx && function.External) { staticBound = true; break; }
        }
        if (!instanceBound && !staticBound)
        {
            cxxClasses_[typeName] = previousInfo;
            cxxRecordEntries_[typeName] = previousRecord;
            return false;
        }

        updated->second.refusedMembers.erase(memberName);
        if (instanceBound
            && std::find(updated->second.instanceMethodNames.begin(),
                         updated->second.instanceMethodNames.end(), memberName)
                   == updated->second.instanceMethodNames.end())
            updated->second.instanceMethodNames.push_back(memberName);
        return true;
    }

/*
 * M6 - make every method a class INHERITS callable on the derived type.
 *
 * A CFlat instance method is resolved by matching the receiver against Parameters[0].TypeName, so
 * a base's method registered with `this` typed as the BASE is invisible on a derived receiver.
 * Rather than teach the member-call path to walk a base graph, each public base's methods are
 * cloned into the derived class's overload set with `this` retyped - same llvm::Function, same
 * clang arrangement, same vtable slot - and the base subobject offset is recorded in
 * cxxThisAdjust_, which the call path adds to `this` right before the call. A non-primary base
 * therefore gets the correct adjusted `this` (Clang's own `getelementptr i8, ptr %obj, N`), and a
 * method the derived class OVERRIDES is left alone: its own registration already won.
 *
 * Records arrive base-before-derived (clang visits them in source order), so a base's own
 * inherited clones are already present when its derived class is processed - which is what makes
 * this work transitively without a second pass.
 */
void LLVMBackend::RegisterCxxInheritedMembers(const CRecordEntry& r)
{
        if (!r.isCxx || !r.layoutRefusal.empty() || r.bases.empty()) return;
        auto self = cxxClasses_.find(r.name);
        if (self == cxxClasses_.end()) return;

        // Signature key of one overload, ignoring `this` - two members with the same key are the
        // same method, so the most derived declaration wins.
        auto sigKey = [](const std::string& name, const std::vector<TypeAndValue>& params) {
            std::string k = name;
            for (size_t i = 1; i < params.size(); ++i)
                k += "|" + params[i].TypeName + (params[i].Pointer ? "*" : "");
            return k;
        };

        std::set<std::string> present;
        for (const std::string& mn : self->second.instanceMethodNames)
        {
            auto fit = functionTable.find(mn);
            if (fit == functionTable.end()) continue;
            for (const FunctionSymbol& sym : fit->second)
                if (sym.IsMethod && !sym.Parameters.empty() && sym.Parameters[0].TypeName == r.name)
                    present.insert(sigKey(mn, sym.Parameters));
        }

        for (const auto& b : r.bases)
        {
            if (b.access != cflat_cinterop::AccessPublic) continue;
            auto bit = cxxClasses_.find(b.name);
            if (bit == cxxClasses_.end()) continue;
            for (const std::string& mn : bit->second.instanceMethodNames)
            {
                auto fit = functionTable.find(mn);
                if (fit == functionTable.end()) continue;
                // Snapshot: the loop below appends to this same overload vector.
                std::vector<FunctionSymbol> fromBase;
                for (const FunctionSymbol& sym : fit->second)
                {
                    if (sym.IsMethod && sym.IsCxx && !sym.Parameters.empty()
                        && sym.Parameters[0].TypeName == b.name)
                        fromBase.push_back(sym);
                }
                for (FunctionSymbol sym : fromBase)
                {
                    const std::string key = sigKey(mn, sym.Parameters);
                    if (!present.insert(key).second) continue;
                    const uint64_t inherited = [&] {
                        auto a = cxxThisAdjust_.find(CxxThisAdjustKey(b.name, sym.UniqueName));
                        return a == cxxThisAdjust_.end() ? 0ull : a->second;
                    }();
                    sym.Parameters[0].TypeName = r.name;
                    const uint64_t adjust = b.offsetBytes + inherited;
                    if (adjust != 0)
                        cxxThisAdjust_[CxxThisAdjustKey(r.name, sym.UniqueName)] = adjust;
                    functionTable[mn].push_back(std::move(sym));
                    self->second.instanceMethodNames.push_back(mn);
                    auto ma = self->second.memberAccess.find(mn);
                    if (ma == self->second.memberAccess.end())
                        self->second.memberAccess[mn] = cflat_cinterop::AccessPublic;
                }
            }
        }
    }

void LLVMBackend::RegisterCMacros(const std::vector<CMacroEntry>& macros)
{
        size_t registered = 0;
        for (const CMacroEntry& m : macros)
        {
            if (m.name.empty()) continue;
            if (!m.aliasTarget.empty()) continue;   // no folded value; RegisterCMacroAliases binds it
            if (globalNamedVariable.count(m.name)) continue;

            TypeAndValue tv;
            tv.VariableName = m.name;
            llvm::Constant* c = nullptr;
            std::string valSuffix;
            if (m.isString)
            {
                // Intern the string literal and register a char* global (char* matches const char* ABI).
                tv.TypeName = "char";
                tv.Pointer  = true;
                llvm::Value* strGv = CreateGlobalString(".cmacro." + m.name, m.stringValue);
                c = llvm::cast<llvm::Constant>(strGv);
                valSuffix = std::format(" = \"{}\"", m.stringValue);
            }
            else if (m.isFloat)
            {
                // Float/double macro (e.g. M_PI). Always registered as `double` - CFlat narrows at
                // use site; double matches C's default FP promotion in variadic/unprototyped calls.
                tv.TypeName = "double";
                tv.Pointer  = false;
                c = llvm::ConstantFP::get(builder->getDoubleTy(), m.floatValue);
                valSuffix = std::format(" = {}", m.floatValue);
            }
            else if (m.isFuncPtr)
            {
                // A C function-pointer macro (e.g. ((int(*)(int,int))0)) is the THIN
                // function<R(P...)>: a bare C function pointer frozen at link time. The
                // constant is just the bit pattern reinterpreted as the thin signature.
                // Force the thin marker: funcPtrTV can arrive with an empty TypeName (e.g.
                // from a cached extraction), which would make GetType pick the fat closure
                // type {ptr,ptr} for the global while the initializer below is a thin ptr -
                // a definition the verifier rejects. A C macro fn-ptr is always thin.
                tv = m.funcPtrTV;
                tv.TypeName = "__c_fn_ptr";
                tv.VariableName = m.name;
                c = llvm::ConstantExpr::getIntToPtr(
                    builder->getInt64((uint64_t)m.value), BuildThinFnPtrType(tv));
            }
            else if (m.isPointer)
            {
                // Sentinel pointer: reinterpret the bit pattern as a void* so comparisons
                // against HANDLE-returning APIs work without an explicit cast.
                tv.TypeName = "void";
                tv.Pointer  = true;
                llvm::Type* i8Ptr = llvm::PointerType::get(*context, 0);
                llvm::Constant* bits = builder->getInt64((uint64_t)m.value);
                c = llvm::ConstantExpr::getIntToPtr(bits, i8Ptr);
                valSuffix = std::format(" = 0x{:x}", (uint64_t)m.value);
            }
            else if (!m.intTypeName.empty())
            {
                // Register with the macro's natural C type so call sites match without casts;
                // build at the type's width so truncation keeps the bit pattern exact.
                tv.TypeName = m.intTypeName;
                tv.Pointer  = false;
                unsigned bits = BitfieldStorageBits(m.intTypeName);
                c = cflat_llvm::GetIntTruncated(llvm::IntegerType::get(*context, bits),
                                                      (uint64_t)m.value);
                valSuffix = ConstIntValueSuffix(tv.TypeName, m.value);
            }
            else
            {
                // Natural type unknown: width-guess from the folded value.
                bool wide = (m.value < INT32_MIN || m.value > INT32_MAX);
                tv.TypeName = wide ? "i64" : "int";
                tv.Pointer  = false;
                c = wide
                    ? static_cast<llvm::Constant*>(builder->getInt64((uint64_t)m.value))
                    : static_cast<llvm::Constant*>(builder->getInt32((uint32_t)(int32_t)m.value));
                valSuffix = ConstIntValueSuffix(tv.TypeName, m.value);
            }
            CreateGlobalVariable(tv, c);
            ++registered;

            if (auto* s = GetSymbolSink())
                s->Register(SymbolKind::Variable, m.name, m.file, m.line, m.col < 0 ? 0 : m.col,
                            tv.TypeName + (tv.Pointer ? "* " : " ") + m.name + valSuffix);
        }
        if (verbose && !macros.empty())
            std::cout << std::format("[verbose]   registered {} C macro constant(s) (of {} object-like candidates)\n",
                registered, macros.size());
    }

// Bind object-like macros whose body is a single identifier (`#define A B`). Runs after every
// C entity of the import is registered, so B is resolvable whatever kind of thing it names.
void LLVMBackend::RegisterCMacroAliases(const std::vector<CMacroEntry>& macros,
                                        const std::vector<CFunctionMacroEntry>& funcMacros,
                                        const std::string& fileForLsp)
{
        std::vector<CMacroEntry> candidates = pendingCInteropAliases_;
        candidates.insert(candidates.end(), macros.begin(), macros.end());

        std::unordered_map<std::string, std::string> aliasMap;
        for (const CMacroEntry& m : candidates)
            if (!m.name.empty() && !m.aliasTarget.empty()) aliasMap.emplace(m.name, m.aliasTarget);
        if (aliasMap.empty())
        {
            pendingCInteropAliases_.clear();
            return;
        }

        std::unordered_map<std::string, const CFunctionMacroEntry*> funcMacroByName;
        for (const CFunctionMacroEntry& f : funcMacros) funcMacroByName.emplace(f.name, &f);

        // An alias may name another alias (`#define A B` / `#define B fn`). Walk the chain to
        // the first name that resolves to something real; the hop cap breaks a self-reference.
        auto isCFunctionTarget = [](const std::vector<FunctionSymbol>& syms) {
            for (const FunctionSymbol& sym : syms)
                if (sym.IsCInteropDeclaration || sym.IsCInteropAlias)
                    return true;
            return false;
        };

        auto resolvable = [&](const std::string& n) {
            auto fit = functionTable.find(n);
            return (fit != functionTable.end() && isCFunctionTarget(fit->second))
                || globalNamedVariable.count(n) != 0
                || funcMacroByName.count(n) != 0 || dataStructures.count(n) != 0
                || ResolveTypeAlias(n) != n;
        };

        size_t bound = 0;
        std::vector<CFunctionMacroEntry> macroTemplateAliases;
        std::vector<CMacroEntry> unresolved;
        for (const CMacroEntry& m : candidates)
        {
            if (m.name.empty() || m.aliasTarget.empty()) continue;
            // A real declaration of the same name always wins over the alias spelling.
            if (functionTable.count(m.name) || globalNamedVariable.count(m.name)) continue;

            std::string target = m.aliasTarget;
            for (int hop = 0; hop < 16 && !resolvable(target); ++hop)
            {
                auto next = aliasMap.find(target);
                if (next == aliasMap.end() || next->second == target) break;
                target = next->second;
            }
            if (target == m.name) continue;   // self-reference

            if (auto fn = functionTable.find(target);
                fn != functionTable.end() && isCFunctionTarget(fn->second))
            {
                // Reuse the target's llvm::Function and its whole signature: the alias is a
                // second lookup name for one linkage symbol, never a new external symbol.
                for (const FunctionSymbol& sym : fn->second)
                {
                    if (!sym.IsCInteropDeclaration && !sym.IsCInteropAlias) continue;
                    FunctionSymbol aliasSym = sym;
                    aliasSym.IsCInteropAlias = true;
                    functionTable[m.name].push_back(std::move(aliasSym));
                }
                if (auto* sink = GetSymbolSink())
                    sink->Register(SymbolKind::Function, m.name, m.file, m.line, m.col < 0 ? 0 : m.col,
                                   "#define " + m.name + " " + target);
                ++bound;
            }
            else if (auto gv = globalNamedVariable.find(target); gv != globalNamedVariable.end())
            {
                globalNamedVariable[m.name] = gv->second;
                auto gt = globalVariableTypes.find(target);
                if (gt != globalVariableTypes.end())
                {
                    TypeAndValue tv = gt->second;
                    tv.VariableName = m.name;
                    globalVariableTypes[m.name] = tv;
                }
                if (auto* sink = GetSymbolSink())
                    sink->Register(SymbolKind::Variable, m.name, m.file, m.line, m.col < 0 ? 0 : m.col,
                                   "#define " + m.name + " " + target);
                ++bound;
            }
            else if (auto fm = funcMacroByName.find(target); fm != funcMacroByName.end())
            {
                // A function-like macro is a generated CFlat template, so the alias is a second
                // template with the same parameters and body.
                CFunctionMacroEntry copy = *fm->second;
                copy.name = m.name;
                copy.file = m.file; copy.line = m.line; copy.col = m.col < 0 ? 0 : m.col;
                macroTemplateAliases.push_back(std::move(copy));
                ++bound;
            }
            else if (dataStructures.count(target) || ResolveTypeAlias(target) != target)
            {
                if (ResolveTypeAlias(m.name) == m.name && !dataStructures.count(m.name))
                {
                    RegisterTypeAlias(m.name, target);
                    RegisterTypeAliasSymbol(m.name, target, m.file, m.line, m.col < 0 ? 0 : m.col);
                    ++bound;
                }
            }
            else
            {
                // Keep unresolved aliases for a later import. An unknown identifier still stays
                // unimported, silently: a header is full of names CFlat has no business binding.
                unresolved.push_back(m);
            }
        }

        pendingCInteropAliases_.swap(unresolved);

        if (!macroTemplateAliases.empty())
            RegisterCFunctionMacros(macroTemplateAliases, fileForLsp + "@aliases");

        if (verbose && bound)
            std::cout << std::format("[verbose]   bound {} alias macro(s) (of {} candidate(s)) from {}\n",
                bound, aliasMap.size(), fileForLsp);
    }

bool LLVMBackend::TranslateMacroBody(const CFunctionMacroEntry& m, std::string& out) const
{
        std::unordered_set<std::string> paramSet(m.params.begin(), m.params.end());
        out.clear();
        out.reserve(m.body.size());
        const std::string& s = m.body;
        size_t i = 0;
        bool hasContent = false;

        // 'c' = argument list of a validated call (',' allowed);
        // 'g' = grouping paren (',' would be the comma operator, rejected).
        std::vector<char> parenCtx;
        bool pendingCallParen = false;

        while (i < s.size())
        {
            char c = s[i];
            if (std::isspace((unsigned char)c)) { out += c; ++i; continue; }

            // Block comment: /* ... */ allowed (and stripped); line comments rejected.
            if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') return false;
            if (c == '/' && i + 1 < s.size() && s[i + 1] == '*')
            {
                i += 2;
                while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/')) ++i;
                if (i + 1 < s.size()) i += 2;
                continue;
            }

            // Identifier: a parameter, a call into a known function, or a bare reference to
            // a known global / enum constant. Anything else (unknown name) drops the macro.
            if (std::isalpha((unsigned char)c) || c == '_')
            {
                size_t start = i;
                while (i < s.size() && (std::isalnum((unsigned char)s[i]) || s[i] == '_')) ++i;
                std::string ident = s.substr(start, i - start);
                if (paramSet.count(ident)) { out += ident; hasContent = true; continue; }

                // Peek past whitespace: an identifier followed by '(' is a call.
                size_t j = i;
                while (j < s.size() && std::isspace((unsigned char)s[j])) ++j;
                bool isCall = (j < s.size() && s[j] == '(');
                if (isCall)
                {
                    if (!functionTable.count(ident)) return false;   // unknown callee: drop
                    pendingCallParen = true;
                }
                else if (!globalNamedVariable.count(ident))
                {
                    return false;   // unknown bare identifier (not a constant): drop
                }
                out += ident;
                hasContent = true;
                continue;
            }

            // Integer suffixes (u/U/l/L) are stripped; float suffixes kept; hex floats dropped.
            if (std::isdigit((unsigned char)c) ||
                (c == '.' && i + 1 < s.size() && std::isdigit((unsigned char)s[i + 1])))
            {
                size_t start = i;
                bool isHex = (c == '0' && i + 1 < s.size() && (s[i + 1] == 'x' || s[i + 1] == 'X'));
                if (isHex)
                {
                    i += 2;
                    while (i < s.size() && std::isxdigit((unsigned char)s[i])) ++i;
                    if (i < s.size() && (s[i] == '.' || s[i] == 'p' || s[i] == 'P'))
                        return false;   // hex float: drop
                    out += s.substr(start, i - start);
                    while (i < s.size() && (s[i] == 'u' || s[i] == 'U' || s[i] == 'l' || s[i] == 'L')) ++i;
                    hasContent = true;
                    continue;
                }

                bool isFloat = false;
                while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
                if (i < s.size() && s[i] == '.')
                {
                    isFloat = true;
                    ++i;
                    while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
                }
                if (i < s.size() && (s[i] == 'e' || s[i] == 'E'))
                {
                    size_t save = i;
                    ++i;
                    if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
                    if (i < s.size() && std::isdigit((unsigned char)s[i]))
                    {
                        isFloat = true;
                        while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
                    }
                    else { i = save; }   // a stray 'e' that is not an exponent
                }
                out += s.substr(start, i - start);
                if (isFloat)
                    while (i < s.size() && (s[i] == 'f' || s[i] == 'F' || s[i] == 'l' || s[i] == 'L'))
                        { out += s[i]; ++i; }
                else
                    while (i < s.size() && (s[i] == 'u' || s[i] == 'U' || s[i] == 'l' || s[i] == 'L')) ++i;
                hasContent = true;
                continue;
            }

            // Char literal: 'x' / '\n' etc. - cflat accepts the same C escape forms. The
            // closing quote must be found (respecting backslash escapes) or the macro drops.
            if (c == '\'')
            {
                size_t start = i;
                ++i;
                while (i < s.size() && s[i] != '\'')
                {
                    if (s[i] == '\\' && i + 1 < s.size()) i += 2;
                    else ++i;
                }
                if (i >= s.size()) return false;   // unterminated
                ++i;                                // closing quote
                out += s.substr(start, i - start);
                hasContent = true;
                continue;
            }

            // String literals and disallowed punctuation.
            if (c == '"')                                        return false;
            if (c == '#' || c == '[' || c == ']' || c == '.' || c == ';') return false;
            if (c == '-' && i + 1 < s.size() && s[i + 1] == '>') return false; // ->

            // Parens: track call vs grouping context so ',' is only allowed in a call's
            // argument list.
            if (c == '(')
            {
                parenCtx.push_back(pendingCallParen ? 'c' : 'g');
                pendingCallParen = false;
                out += c; ++i; hasContent = true; continue;
            }
            if (c == ')')
            {
                if (!parenCtx.empty()) parenCtx.pop_back();
                out += c; ++i; hasContent = true; continue;
            }
            if (c == ',')
            {
                if (parenCtx.empty() || parenCtx.back() != 'c') return false; // comma operator: drop
                out += c; ++i; hasContent = true; continue;
            }

            // Two-char operators kept intact.
            if (i + 1 < s.size())
            {
                std::string two = s.substr(i, 2);
                static const std::unordered_set<std::string> twoChar = {
                    "<=", ">=", "==", "!=", "&&", "||", "<<", ">>"
                };
                if (twoChar.count(two)) { out += two; i += 2; hasContent = true; continue; }
            }

            // Single-character operators.
            static const std::string allowedSingle = "+-*/%&|^~!<>?:";
            if (allowedSingle.find(c) != std::string::npos)
            {
                out += c;
                ++i;
                hasContent = true;
                continue;
            }

            return false; // anything else: reject
        }

        if (!hasContent) return false;
        while (!out.empty() && std::isspace((unsigned char)out.back())) out.pop_back();
        if (out.empty()) return false;

        // Strip parens around bare identifiers: '(n)' is a C-style cast in CFlat grammar,
        // but never strip call-argument parens (preceded by identifier char).
        size_t pos = 0;
        while ((pos = out.find('(', pos)) != std::string::npos)
        {
            size_t prevPos = pos;
            while (prevPos > 0 && std::isspace((unsigned char)out[prevPos - 1])) --prevPos;
            if (prevPos > 0)
            {
                char prev = out[prevPos - 1];
                if (std::isalnum((unsigned char)prev) || prev == '_') { ++pos; continue; }
            }
            size_t inner = pos + 1;
            while (inner < out.size() && std::isspace((unsigned char)out[inner])) ++inner;
            if (inner >= out.size() ||
                !(std::isalpha((unsigned char)out[inner]) || out[inner] == '_'))
            {
                ++pos; continue;
            }
            size_t identEnd = inner;
            while (identEnd < out.size() &&
                   (std::isalnum((unsigned char)out[identEnd]) || out[identEnd] == '_'))
                ++identEnd;
            size_t after = identEnd;
            while (after < out.size() && std::isspace((unsigned char)out[after])) ++after;
            if (after >= out.size() || out[after] != ')') { ++pos; continue; }
            out.replace(pos, after + 1 - pos, out.substr(inner, identEnd - inner));
            // Stay at pos: the rewrite may expose another stripping opportunity.
        }
        return true;
    }

void LLVMBackend::RegisterCFunctionMacros(const std::vector<CFunctionMacroEntry>& funcMacros,
                                 const std::string& fileForLsp,
                                 std::vector<CFunctionMacroEntry>* retryMacros)
{
        if (funcMacros.empty()) return;

        std::string generated;
        generated.reserve(funcMacros.size() * 60);
        size_t accepted = 0, rejected = 0, skipped = 0;

        for (const auto& m : funcMacros)
        {
            if (functionTable.count(m.name) ||
                globalNamedVariable.count(m.name))
            {
                ++skipped;
                if (verbose) std::cout << std::format("[verbose]   skip macro {}: name already defined\n", m.name);
                continue;
            }

            std::string translatedBody;
            if (!TranslateMacroBody(m, translatedBody))
            {
                ++rejected;
                if (retryMacros != nullptr)
                    retryMacros->push_back(m);
                if (verbose) std::cout << std::format("[verbose]   reject macro {}: body uses unsupported tokens\n", m.name);
                continue;
            }

            generated += "auto " + m.name + "<";
            for (size_t i = 0; i < m.params.size(); ++i)
            {
                if (i) generated += ", ";
                generated += "T" + std::to_string(i);
            }
            generated += ">(";
            for (size_t i = 0; i < m.params.size(); ++i)
            {
                if (i) generated += ", ";
                generated += "T" + std::to_string(i) + " " + m.params[i];
            }
            generated += ") { return (" + translatedBody + "); }\n";
            ++accepted;
        }

        if (verbose)
            std::cout << std::format("[verbose]   function-like C macros: {} translated, {} rejected, {} skipped (already defined) from {}\n",
                accepted, rejected, skipped, fileForLsp);

        if (!generated.empty())
            pendingMacroSources_.push_back({ fileForLsp + "@cmacros", std::move(generated) });
    }

void LLVMBackend::ReportOrphanHeader(const std::vector<std::string>& headerPaths, const std::string& clangErr)
{
        std::string name = std::filesystem::path(headerPaths.front()).filename().string();
        std::string detail = clangErr.empty() ? "a required type is undefined" : clangErr;
        if (headerPaths.size() > 1)
        {
            std::string grp;
            for (size_t i = 0; i < headerPaths.size(); ++i)
                grp += (i ? ", \"" : "\"") + std::filesystem::path(headerPaths[i]).filename().string() + "\"";
            LogRawError(std::format(
                "C header '{}' did not compile in this group ({}). Reorder the group so the "
                "prerequisite header comes first, or add the missing one: import {{ {} }};",
                name, detail, grp));
            return;
        }
        LogRawError(std::format(
            "C header '{}' does not compile on its own ({}). It likely needs a prerequisite "
            "header included first. Import them together as one group so they share a single "
            "translation unit, e.g. import {{ \"prerequisite.h\", \"{}\" }};",
            name, detail, name));
    }

bool LLVMBackend::CompileCHeader(const std::string& headerPath, const std::vector<std::string>& extraDefines,
                        bool diskCache, bool cppMode)
{
        return CompileCHeaderGroup(std::vector<std::string>{ headerPath }, extraDefines, diskCache, cppMode);
    }

bool LLVMBackend::CompileCHeaderGroup(const std::vector<std::string>& headerPaths,
                             const std::vector<std::string>& extraDefines,
                             bool diskCache, bool cppMode)
{
        if (headerPaths.empty()) return true;

        std::vector<std::string> realPaths;
        realPaths.reserve(headerPaths.size());
        for (const auto& h : headerPaths)
        {
            llvm::SmallString<256> rp;
            realPaths.push_back(!llvm::sys::fs::real_path(h, rp) ? rp.str().str() : h);
            RecordDependency(realPaths.back());
        }
        const std::string& fileForLsp = realPaths.front();

        // The C++ link decision must not depend on whether the parse ran: a warm in-memory or
        // disk cache hit skips ExtractCHeaderClang entirely, and the flag would stay false.
        if (cppMode) cppInteropUsed_ = true;

        /*
         * Same reason the flag above is set here: a concrete C++ type request must see this group's
         * headers and defines even when no parse ran for it in this analysis. The group is THIS
         * import statement only - every request this import triggers compiles against it, and no
         * other import can change that translation unit or its cache identity.
         */
        size_t cxxGroupIndex = static_cast<size_t>(-1);
        CxxRequestGroup cxxGroup;
        if (cppMode)
        {
            cxxGroupIndex = FindOrAddCxxImportGroup(realPaths, extraDefines);
            // A standard-library header is a template catalog the walk deliberately skips, so no
            // record of it is ever registered - seed its namespace here or `std.vector<int>` could
            // never be requested at all.
            for (const auto& h : realPaths)
            {
                if (IsSystemCxxHeaderPath(h))
                {
                    cxxForeignNamespaces_.insert("std");
                    cxxImportGroups_[cxxGroupIndex].namespaces.insert("std");
                }
                else
                {
                    CollectHeaderNamespaceNames(h, cxxForeignNamespaces_);
                    CollectHeaderNamespaceNames(h, cxxImportGroups_[cxxGroupIndex].namespaces);
                }
            }
            cxxGroup = MakeCxxRequestGroup(cxxGroupIndex, {});
        }
        CxxRequestGroupScope cxxGroupGuard(*this, cppMode ? &cxxGroup : nullptr);

        // Best-effort alias retry: a macro that still cannot be resolved is dropped, exactly as
        // the first registration pass drops it. A real header carries many such macros.
        bool aliasRetryFailed = false;
        auto macroUsesObjectAlias = [](const auto& macro, const auto& aliases) {
            std::unordered_set<std::string> params(macro.params.begin(), macro.params.end());
            std::unordered_set<std::string> aliasNames;
            for (const auto& alias : aliases)
                if (!alias.name.empty() && !alias.aliasTarget.empty()) aliasNames.insert(alias.name);
            size_t i = 0;
            while (i < macro.body.size())
            {
                if (!std::isalpha((unsigned char)macro.body[i]) && macro.body[i] != '_')
                {
                    ++i;
                    continue;
                }
                size_t start = i++;
                while (i < macro.body.size()
                       && (std::isalnum((unsigned char)macro.body[i]) || macro.body[i] == '_')) ++i;
                std::string ident = macro.body.substr(start, i - start);
                if (!params.count(ident) && aliasNames.count(ident)) return true;
            }
            return false;
        };
        auto rewriteObjectAliases = [&](CFunctionMacroEntry macro, const auto& aliases) {
            std::unordered_map<std::string, std::string> aliasMap;
            for (const auto& alias : aliases)
                if (!alias.name.empty() && !alias.aliasTarget.empty())
                    aliasMap.emplace(alias.name, alias.aliasTarget);
            std::unordered_set<std::string> params(macro.params.begin(), macro.params.end());
            auto resolve = [&](const std::string& name) {
                std::string current = name;
                std::unordered_set<std::string> seen;
                for (int depth = 0; depth < 16; ++depth)
                {
                    auto it = aliasMap.find(current);
                    if (it == aliasMap.end()) return current;
                    if (!seen.insert(current).second) { aliasRetryFailed = true; return current; }
                    current = it->second;
                }
                aliasRetryFailed = true;
                return current;
            };

            std::string rewritten;
            rewritten.reserve(macro.body.size());
            size_t i = 0;
            while (i < macro.body.size())
            {
                if (!std::isalpha((unsigned char)macro.body[i]) && macro.body[i] != '_')
                {
                    rewritten += macro.body[i++];
                    continue;
                }
                size_t start = i++;
                while (i < macro.body.size()
                       && (std::isalnum((unsigned char)macro.body[i]) || macro.body[i] == '_')) ++i;
                std::string ident = macro.body.substr(start, i - start);
                if (!params.count(ident) && aliasMap.count(ident)) rewritten += resolve(ident);
                else rewritten += ident;
            }
            macro.body = std::move(rewritten);
            return macro;
        };
        auto expandFunctionMacroAliases = [&](CFunctionMacroEntry macro,
                                               const auto& aliases, const auto& functionMacros) {
            std::unordered_map<std::string, std::string> aliasMap;
            for (const auto& alias : aliases)
                if (!alias.name.empty() && !alias.aliasTarget.empty())
                    aliasMap.emplace(alias.name, alias.aliasTarget);
            std::unordered_map<std::string, const CFunctionMacroEntry*> functionMacroMap;
            for (const auto& functionMacro : functionMacros)
                functionMacroMap.emplace(functionMacro.name, &functionMacro);
            std::unordered_set<std::string> params(macro.params.begin(), macro.params.end());

            auto resolve = [&](const std::string& name) {
                std::string current = name;
                std::unordered_set<std::string> seen;
                for (int depth = 0; depth < 16; ++depth)
                {
                    auto it = aliasMap.find(current);
                    if (it == aliasMap.end()) return current;
                    if (!seen.insert(current).second) { aliasRetryFailed = true; return current; }
                    current = it->second;
                }
                aliasRetryFailed = true;
                return current;
            };
            auto substitute = [](const CFunctionMacroEntry& functionMacro,
                                 const std::vector<std::string>& arguments) {
                std::unordered_map<std::string, std::string> replacements;
                for (size_t i = 0; i < functionMacro.params.size(); ++i)
                    replacements.emplace(functionMacro.params[i], arguments[i]);
                std::string result;
                size_t i = 0;
                while (i < functionMacro.body.size())
                {
                    if (!std::isalpha((unsigned char)functionMacro.body[i])
                        && functionMacro.body[i] != '_')
                    {
                        result += functionMacro.body[i++];
                        continue;
                    }
                    size_t start = i++;
                    while (i < functionMacro.body.size()
                           && (std::isalnum((unsigned char)functionMacro.body[i])
                               || functionMacro.body[i] == '_')) ++i;
                    std::string ident = functionMacro.body.substr(start, i - start);
                    auto replacement = replacements.find(ident);
                    result += replacement == replacements.end() ? ident : replacement->second;
                }
                return result;
            };

            for (int pass = 0; pass < 16; ++pass)
            {
                bool expanded = false;
                size_t i = 0;
                while (i < macro.body.size())
                {
                    if (!std::isalpha((unsigned char)macro.body[i]) && macro.body[i] != '_')
                    {
                        ++i;
                        continue;
                    }
                    size_t start = i++;
                    while (i < macro.body.size()
                           && (std::isalnum((unsigned char)macro.body[i]) || macro.body[i] == '_')) ++i;
                    std::string ident = macro.body.substr(start, i - start);
                    if (params.count(ident)) continue;
                    size_t open = i;
                    while (open < macro.body.size() && std::isspace((unsigned char)macro.body[open])) ++open;
                    auto functionMacro = functionMacroMap.find(resolve(ident));
                    if (aliasRetryFailed) return macro;
                    if (open >= macro.body.size() || macro.body[open] != '(' || functionMacro == functionMacroMap.end())
                        continue;

                    size_t close = open + 1;
                    int depth = 1;
                    size_t argumentStart = close;
                    std::vector<std::string> arguments;
                    while (close < macro.body.size() && depth > 0)
                    {
                        if (macro.body[close] == '(') ++depth;
                        else if (macro.body[close] == ')') --depth;
                        if (depth == 1 && macro.body[close] == ',')
                        {
                            arguments.push_back(macro.body.substr(argumentStart, close - argumentStart));
                            argumentStart = close + 1;
                        }
                        ++close;
                    }
                    if (depth != 0) { aliasRetryFailed = true; return macro; }
                    if (close - 1 > argumentStart || !functionMacro->second->params.empty())
                        arguments.push_back(macro.body.substr(argumentStart, close - 1 - argumentStart));
                    if (arguments.size() != functionMacro->second->params.size())
                        { aliasRetryFailed = true; return macro; }

                    std::string replacement = substitute(*functionMacro->second, arguments);
                    macro.body.replace(start, close - start, replacement);
                    expanded = true;
                    break;
                }
                if (!expanded) return macro;
            }
            aliasRetryFailed = true;
            return macro;
        };

        // Fold headers, include-dirs, and defines: same header under different roots/defines or
        // standalone vs. grouped must not share a stale cache entry.
        std::string cacheKey;
        for (const auto& rp : realPaths)       cacheKey += "|H" + rp;
        for (const auto& inc : cIncludeDirs_)  cacheKey += "|I" + inc;
        for (const auto& def : cDefines_)      cacheKey += "|D" + def;
        for (const auto& def : extraDefines)   cacheKey += "|d" + def;
        // C and C++ mode bind the same header differently (qualified names, linkage names),
        // so they must never share a cache entry.
        if (cppMode) cacheKey += "|CXX";
        /*
         * LSP analysis binds a C++ group with assumeInlineDefinitions instead of emitDefinitions:
         * every inline member gets a linkage name and a callable surface, but no body is emitted
         * and the companion bitcode stays EMPTY. Sharing one key with a real compile would let the
         * compile adopt that empty companion and bind bodies nothing ever emits - a link-time
         * "undefined symbol" with no diagnostic. The mode is part of the identity of the result.
         */
        const bool cxxDefinitionsEmitted = cppMode && symbolSink_ == nullptr;
        if (cppMode) cacheKey += cxxDefinitionsEmitted ? "|EDEF" : "|EDECL";
        // The cached bindings were produced by THIS compiler's C type mapper, and an upgrade
        // can change it (e.g. the LP64 `long` width). Without the compiler's identity in the
        // key, a stale entry silently outlives the code that wrote it.
        cacheKey += "|C" + CompilerBuildStamp();

        uint64_t currentHash = 0;
        bool haveHash = false;
        auto hashNow = [&]() -> uint64_t {
            if (!haveHash)
            {
                currentHash = 14695981039346656037ULL;
                for (const auto& rp : realPaths)
                {
                    uint64_t h = 0;
                    HashFileContents(rp, h);
                    currentHash ^= h; currentHash *= 1099511628211ULL;
                }
                haveHash = true;
            }
            return currentHash;
        };

        std::error_code mtEc;
        std::filesystem::file_time_type currentMtime{};
        for (const auto& rp : realPaths)
        {
            std::error_code ec;
            auto mt = std::filesystem::last_write_time(rp, ec);
            if (ec) { mtEc = ec; break; }
            if (mt > currentMtime) currentMtime = mt;
        }

        std::vector<CSigEntry> hitSigs;
        std::vector<CEnumEntry> hitEnums;
        std::vector<CRecordEntry> hitRecords;
        std::vector<CMacroEntry> hitMacros;
        std::vector<CFunctionMacroEntry> hitFuncMacros;
        std::vector<CGlobalEntry> hitGlobals;
        std::vector<std::pair<std::string, std::string>> hitAliases;
        std::vector<CTypeAliasEntry> hitTypeAliases;
        std::vector<cflat_cinterop::RawFunctionTemplate> hitFunctionTemplates;
        std::vector<cflat_cinterop::RawFunctionPointerAbi> hitFunctionPointerAbis;
        std::string hitCxxBitcode;
        bool hit = false;
        {
            std::lock_guard<std::mutex> lock(cFileSigCacheMutex_);
            auto cacheIt = cFileSigCache_.find(cacheKey);
            if (!mtEc && cacheIt != cFileSigCache_.end())
            {
                CFileSigCacheEntry& entry = cacheIt->second;
                if (entry.mtime == currentMtime)
                {
                    if (verbose) std::cout << std::format("[verbose] C header cache hit (mtime) for {}\n", fileForLsp);
                    SetCInteropTargetFacts(entry.longDoubleWidth, entry.longDoubleIsIEEEDouble,
                                           entry.targetTriple);
                    TouchCFileSigEntry(cacheKey, entry);
                    hitSigs = entry.sigs; hitEnums = entry.enums; hitRecords = entry.records;
                    hitMacros = entry.macros; hitFuncMacros = entry.funcMacros; hitGlobals = entry.globals;
                    hitAliases = entry.recordAliases; hit = true;
                    hitTypeAliases = entry.typeAliases;
                    hitFunctionTemplates = entry.functionTemplates;
                    hitFunctionPointerAbis = entry.functionPointerAbis;
                    hitCxxBitcode = entry.cxxBitcode;
                }
                else if (hashNow() == entry.hash)
                {
                    if (verbose) std::cout << std::format("[verbose] C header cache hit (hash) for {}\n", fileForLsp);
                    SetCInteropTargetFacts(entry.longDoubleWidth, entry.longDoubleIsIEEEDouble,
                                           entry.targetTriple);
                    entry.mtime = currentMtime;
                    TouchCFileSigEntry(cacheKey, entry);
                    hitSigs = entry.sigs; hitEnums = entry.enums; hitRecords = entry.records;
                    hitMacros = entry.macros; hitFuncMacros = entry.funcMacros; hitGlobals = entry.globals;
                    hitAliases = entry.recordAliases; hit = true;
                    hitTypeAliases = entry.typeAliases;
                    hitFunctionTemplates = entry.functionTemplates;
                    hitFunctionPointerAbis = entry.functionPointerAbis;
                    hitCxxBitcode = entry.cxxBitcode;
                }
            }
        }
        if (hit)
        {
            // The C++ definitions this header needed were emitted on the cold run; relink the very
            // same bitcode instead of running CodeGen again.
            AdoptCxxCompanionBitcode(hitCxxBitcode);
            RegisterCEnums(hitEnums, fileForLsp);
            // Records before sigs so struct-by-value signatures resolve to the same types.
            RegisterCRecords(hitRecords, fileForLsp);
            RegisterRecordAliases(hitAliases);
            RegisterTypeAliasSymbols(hitTypeAliases);
            if (cppMode)
                RegisterCxxFunctionTemplates(hitFunctionTemplates, cxxGroupIndex, fileForLsp);
            RegisterCxxFunctionPointerAbis(hitFunctionPointerAbis);
            if (cppMode && activeCxxRequestGroup_ != nullptr)
            {
                // Cache-hit replay: the group still owns these names, and the requests the cold
                // path made are replayed through the same batch.
                PublishCxxGroupNames(activeCxxRequestGroup_->primary, hitRecords);
                std::vector<CxxRequestItem> batch;
                CollectCxxSignatureRequestItems(hitSigs, batch);
                PrewarmCxxRequestBatch(std::move(batch));
            }
            RequestCxxSignatureTypes(hitSigs);
            RegisterCSignatures(hitSigs, fileForLsp);
            RegisterCEnums(hitEnums, fileForLsp);
            RegisterCMacros(hitMacros);
            std::vector<CFunctionMacroEntry> retryMacros;
            RegisterCFunctionMacros(hitFuncMacros, fileForLsp, &retryMacros);
            RegisterCGlobals(hitGlobals, fileForLsp);
            RegisterCMacroAliases(hitMacros, hitFuncMacros, fileForLsp);
            std::vector<CFunctionMacroEntry> aliasRetries;
            for (const auto& m : retryMacros)
                if (macroUsesObjectAlias(m, hitMacros))
                {
                    aliasRetryFailed = false;
                    CFunctionMacroEntry rewritten = rewriteObjectAliases(m, hitMacros);
                    if (!aliasRetryFailed)
                        rewritten = expandFunctionMacroAliases(rewritten, hitMacros, hitFuncMacros);
                    std::string translated;
                    if (!aliasRetryFailed && TranslateMacroBody(rewritten, translated))
                        aliasRetries.push_back(std::move(rewritten));
                }
            RegisterCFunctionMacros(aliasRetries, fileForLsp + "@alias-retry");
            return true;
        }

        // Persistent disk cache (opt-in via `cache` import clause). On hit, preloads the
        // in-memory cache and registers decls, skipping the clang header parse entirely.
        std::filesystem::path cHeaderCacheDir = GetCHeaderCacheDir();
        uint64_t diskKey = 0;
        if (diskCache && !mtEc && !cHeaderCacheDir.empty())
        {
            diskKey = CHeaderDiskCacheKey(realPaths, cIncludeDirs_, cDefines_, extraDefines, cppMode,
                                          cxxDefinitionsEmitted);
            CFileSigCacheEntry diskEntry;
            bool diskHit;
            {
                llvm::TimeTraceScope loadScope("CHeaderJsonLoad", fileForLsp);
                diskHit = TryLoadCHeaderDiskCache(cHeaderCacheDir, diskKey, currentMtime, hashNow(), diskEntry);
            }
            if (diskHit)
            {
                if (verbose) std::cout << std::format("[verbose] C header disk cache hit for {}\n", fileForLsp);
                {
                    std::lock_guard<std::mutex> lock(cFileSigCacheMutex_);
                    InsertCFileSigEntry(cacheKey, CFileSigCacheEntry(diskEntry), verbose);
                }
                llvm::TimeTraceScope registerScope("CHeaderRegister", fileForLsp);
                AdoptCxxCompanionBitcode(diskEntry.cxxBitcode);
                SetCInteropTargetFacts(diskEntry.longDoubleWidth,
                                       diskEntry.longDoubleIsIEEEDouble,
                                       diskEntry.targetTriple);
                RegisterCEnums(diskEntry.enums, fileForLsp);
                RegisterCRecords(diskEntry.records, fileForLsp);
                RegisterRecordAliases(diskEntry.recordAliases);
                RegisterTypeAliasSymbols(diskEntry.typeAliases);
                if (cppMode)
                    RegisterCxxFunctionTemplates(diskEntry.functionTemplates, cxxGroupIndex, fileForLsp);
                RegisterCxxFunctionPointerAbis(diskEntry.functionPointerAbis);
                if (cppMode && activeCxxRequestGroup_ != nullptr)
                {
                    PublishCxxGroupNames(activeCxxRequestGroup_->primary, diskEntry.records);
                    std::vector<CxxRequestItem> batch;
                    CollectCxxSignatureRequestItems(diskEntry.sigs, batch);
                    PrewarmCxxRequestBatch(std::move(batch));
                }
                RequestCxxSignatureTypes(diskEntry.sigs);
                RegisterCSignatures(diskEntry.sigs, fileForLsp);
                RegisterCEnums(diskEntry.enums, fileForLsp);
                RegisterCMacros(diskEntry.macros);
                std::vector<CFunctionMacroEntry> retryMacros;
                RegisterCFunctionMacros(diskEntry.funcMacros, fileForLsp, &retryMacros);
                RegisterCGlobals(diskEntry.globals, fileForLsp);
                RegisterCMacroAliases(diskEntry.macros, diskEntry.funcMacros, fileForLsp);
                std::vector<CFunctionMacroEntry> aliasRetries;
                for (const auto& m : retryMacros)
                    if (macroUsesObjectAlias(m, diskEntry.macros))
                    {
                        aliasRetryFailed = false;
                        CFunctionMacroEntry rewritten = rewriteObjectAliases(m, diskEntry.macros);
                        if (!aliasRetryFailed)
                            rewritten = expandFunctionMacroAliases(rewritten, diskEntry.macros, diskEntry.funcMacros);
                        std::string translated;
                        if (!aliasRetryFailed && TranslateMacroBody(rewritten, translated))
                            aliasRetries.push_back(std::move(rewritten));
                    }
                RegisterCFunctionMacros(aliasRetries, fileForLsp + "@alias-retry");
                return true;
            }
        }

        std::vector<CSigEntry> sigs;
        std::vector<CEnumEntry> enums;
        std::vector<CRecordEntry> records;
        std::vector<CMacroEntry> macros;
        std::vector<CFunctionMacroEntry> funcMacros;
        std::vector<CGlobalEntry> globals;
        std::vector<std::pair<std::string, std::string>> aliases;
        std::vector<CTypeAliasEntry> typeAliases;
        std::vector<cflat_cinterop::RawFunctionTemplate> functionTemplates;
        std::vector<cflat_cinterop::RawFunctionPointerAbi> functionPointerAbis;
        uint64_t longDoubleWidth = 0;
        bool longDoubleIsIEEEDouble = false;
        std::string targetTriple;
        // Deep mode: collect the transitive include set so the disk entry can validate it.
        bool wantDeps = diskCache && cHeaderCacheDeep_ && !cHeaderCacheDir.empty();
        std::vector<std::string> includes;
        std::string cxxBitcode;   // M5 companion module for this group, empty for C imports
        {
            // All C entities are extracted in one full parse (plus a cheap preprocess-only prepass
            // for macro names). Uses clang C++ API, not clang-cl or libclang.
            llvm::TimeTraceScope extractScope("CHeaderExtract", fileForLsp);
            bool prereqFailure = false;
            std::string prereqMsg;
            if (!ExtractCHeaderClang(realPaths, sigs, enums, records, macros, funcMacros, globals,
                                     aliases, typeAliases, extraDefines, wantDeps ? &includes : nullptr,
                                     &prereqFailure, &prereqMsg, cppMode, &cxxBitcode,
                                     &functionPointerAbis, &longDoubleWidth,
                                     &longDoubleIsIEEEDouble, &targetTriple, &functionTemplates))
            {
                if (prereqFailure)
                    ReportOrphanHeader(headerPaths, prereqMsg);
                return false;
            }
        }

        if (!mtEc)
        {
            CFileSigCacheEntry entry;
            entry.mtime = currentMtime;
            entry.hash  = hashNow();
            entry.longDoubleWidth = longDoubleWidth;
            entry.longDoubleIsIEEEDouble = longDoubleIsIEEEDouble;
            entry.targetTriple = targetTriple;
            entry.sigs  = sigs;
            entry.functionTemplates = functionTemplates;
            entry.enums = enums;
            entry.records = records;
            entry.macros = macros;
            entry.funcMacros = funcMacros;
            entry.globals = globals;
            entry.recordAliases = aliases;
            entry.typeAliases = typeAliases;
            entry.functionPointerAbis = functionPointerAbis;
            entry.cxxBitcode = cxxBitcode;
            // Keep only real on-disk paths in the transitive dependency list (deep mode).
            // Non-existent deps would otherwise poison every later cache validation.
            if (wantDeps)
            {
                std::unordered_set<std::string> seen;
                for (const auto& inc : includes)
                {
                    RecordDependency(inc);
                    std::error_code dec;
                    auto dm = std::filesystem::last_write_time(inc, dec);
                    if (dec) continue;
                    if (!seen.insert(inc).second) continue;
                    CHeaderDep dep;
                    dep.path  = inc;
                    dep.mtime = (int64_t)dm.time_since_epoch().count();
                    HashFileFnv1a(inc, dep.hash);
                    entry.deps.push_back(std::move(dep));
                }
            }
            // --run is read-only: never persist header cache to disk even with 'cache' clause.
            // The in-memory entry still serves this compile.
            // Nor does LSP analysis write: its entry carries a bound surface with no bodies
            // (see the cache-key comment above), which is not a result a compile may reuse.
            // A huge companion blob is not worth persisting; the whole entry is skipped rather
            // than written without it, since an entry with a bound surface and no bodies is the
            // link-time-undefined trap the mode key above exists to prevent.
            const bool blobTooBigForDisk = entry.cxxBitcode.size() > kMaxDiskCachedCxxBitcode;
            if (blobTooBigForDisk && verbose)
                std::cout << std::format("[verbose] C++ companion module for {} is {} bytes - "
                                         "kept in memory, not written to the header disk cache\n",
                                         fileForLsp, entry.cxxBitcode.size());
            if (diskCache && !runMode_ && symbolSink_ == nullptr && !blobTooBigForDisk
                && !cHeaderCacheDir.empty())
                WriteCHeaderDiskCache(cHeaderCacheDir, diskKey, currentMtime, hashNow(), entry);
            std::lock_guard<std::mutex> lock(cFileSigCacheMutex_);
            InsertCFileSigEntry(cacheKey, std::move(entry), verbose);
        }

        // Records were already registered inside ExtractCHeaderClang.
        {
            llvm::TimeTraceScope registerScope("CHeaderRegister", fileForLsp);
            if (cppMode)
                RegisterCxxFunctionTemplates(functionTemplates, cxxGroupIndex, fileForLsp);
            RegisterCSignatures(sigs, fileForLsp);
            if (!cppMode) RegisterTypeAliasSymbols(typeAliases);
            RegisterCEnums(enums, fileForLsp);
            RegisterCMacros(macros);
            std::vector<CFunctionMacroEntry> retryMacros;
            RegisterCFunctionMacros(funcMacros, fileForLsp, &retryMacros);
            RegisterCGlobals(globals, fileForLsp);
            RegisterCMacroAliases(macros, funcMacros, fileForLsp);
            std::vector<CFunctionMacroEntry> aliasRetries;
            for (const auto& m : retryMacros)
                if (macroUsesObjectAlias(m, macros))
                {
                    aliasRetryFailed = false;
                    CFunctionMacroEntry rewritten = rewriteObjectAliases(m, macros);
                    if (!aliasRetryFailed)
                        rewritten = expandFunctionMacroAliases(rewritten, macros, funcMacros);
                    std::string translated;
                    if (!aliasRetryFailed && TranslateMacroBody(rewritten, translated))
                        aliasRetries.push_back(std::move(rewritten));
                }
            RegisterCFunctionMacros(aliasRetries, fileForLsp + "@alias-retry");
        }
        return true;
    }

/*
 * ============================================================================================
 * M4b - foreign NONTRIVIAL C++ class lifetime
 *
 * Everything below serves ONE invariant: a nontrivial imported C++ object is created, copied,
 * moved and destroyed by its own C++ special members, on storage CFlat allocated, and never by
 * a CFlat bitwise store. The declaration site allocates the slot and calls a constructor INTO
 * it (see MainListener::TryDeclareForeignCxxLocal); the destructor is registered as the class's
 * CFlat destructor so the existing scope-exit machinery is the only cleanup path.
 * ============================================================================================
 */

llvm::Function* LLVMBackend::GetOrCreateCxxStructor(const std::string& typeName,
                                                    const CxxClassInfo::Structor& st,
                                                    AbiRecipe& recipeOut)
{
        if (st.linkageName.empty() || st.params.empty()) return nullptr;
        if (!BuildAbiRecipeFromClangPlan(typeName, st.abi, st.ret, st.params, recipeOut))
            return nullptr;
        llvm::FunctionType* fnTy = BuildExternFunctionType(st.ret, st.params, /*varargs*/ false,
                                                           recipeOut);
        if (fnTy == nullptr) return nullptr;
        if (llvm::Function* existing = module->getFunction(st.linkageName))
        {
            if (existing->getFunctionType() != fnTy)
            {
                // Every user-facing name is demangled; the Itanium symbol is not a spelling
                // the user wrote. llvm::demangle returns the input unchanged if it is not one.
                LogError(std::format(
                    "internal: C++ special member '{}' of '{}' was already declared with a "
                    "different signature", llvm::demangle(st.linkageName), typeName));
                return nullptr;
            }
            return existing;
        }
        auto* fn = llvm::Function::Create(fnTy, llvm::GlobalValue::ExternalLinkage,
                                          st.linkageName, module.get());
        ApplyAbiAttributes(fn, recipeOut);
        return fn;
    }

bool LLVMBackend::CxxConstantDefaultsFrom(const CxxClassInfo::Structor& st, size_t first)
{
        if (st.defaultArgs.size() != st.params.size()) return false;
        for (size_t i = first; i < st.defaultArgs.size(); ++i)
        {
            const std::string& k = st.defaultArgs[i].kind;
            if (k != "int" && k != "bool" && k != "enum" && k != "float" && k != "double"
                && k != "nullptr")
                return false;
        }
        return first < st.params.size();
}

llvm::Value* LLVMBackend::MaterializeCxxDefaultArgument(const cflat_cinterop::RawDefaultArg& def,
                                                        const TypeAndValue& param)
{
        llvm::Type* ty = GetType(param);
        if (ty == nullptr) return nullptr;
        if (def.kind == "nullptr")
            return ty->isPointerTy() ? llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ty))
                                     : nullptr;
        if ((def.kind == "int" || def.kind == "bool" || def.kind == "enum") && ty->isIntegerTy())
            return llvm::ConstantInt::get(*context, llvm::APInt(ty->getIntegerBitWidth(), def.value, 10));
        if ((def.kind == "float" || def.kind == "double") && ty->isFloatingPointTy())
            return llvm::ConstantFP::get(ty, std::stod(def.value));
        return nullptr;
}

bool LLVMBackend::EmitCxxStructorCall(const std::string& typeName,
                                      const CxxClassInfo::Structor& st,
                                      llvm::Value* slot,
                                      const std::vector<llvm::Value*>& extraArgs)
{
        if (slot == nullptr) return false;
        AbiRecipe recipe;
        llvm::Function* fn = GetOrCreateCxxStructor(typeName, st, recipe);
        if (fn == nullptr) return false;
        std::vector<llvm::Value*> args;
        args.reserve(extraArgs.size() + 1);
        args.push_back(slot);                       // 'this'
        for (size_t i = 0; i < extraArgs.size(); ++i)
        {
            llvm::Value* a = extraArgs[i];
            const TypeAndValue* param = i + 1 < st.params.size() ? &st.params[i + 1] : nullptr;
            if (param != nullptr && param->Pointer && a != nullptr
                && !a->getType()->isPointerTy() && a->getType()->isStructTy())
            {
                auto* temp = AllocaAtEntry(a->getType(), nullptr, "ctor.refarg");
                builder->CreateStore(a, temp);
                a = temp;
            }
            args.push_back(a);
        }
        // Omitted trailing parameters take their constant C++ defaults.
        for (size_t i = args.size(); i < st.params.size() && i < st.defaultArgs.size(); ++i)
        {
            llvm::Value* v = MaterializeCxxDefaultArgument(st.defaultArgs[i], st.params[i]);
            if (v == nullptr) break;
            args.push_back(v);
        }
        if (args.size() != st.params.size())
        {
            LogError(std::format("C++ special member of '{}' expects {} argument(s), got {}",
                                 typeName, (uint64_t)st.params.size() - 1,
                                 (uint64_t)args.size() - 1));
            return false;
        }
        FunctionSymbol sym;
        sym.UniqueName = st.linkageName;
        sym.SourceName = st.linkageName;
        sym.Function = fn;
        sym.ReturnType = st.ret;
        sym.Parameters = st.params;
        sym.External = true;
        sym.IsCxx = true;
        sym.IsNoexcept = st.isNoexcept;
        sym.Recipe = recipe;
        if (recipe.hasLowering)
            EmitAbiLoweredCall(sym, args);
        else
            CreateFunctionCall(fn, args);
        return true;
    }

llvm::Function* LLVMBackend::GetOrCreateCxxClassDestructor(const std::string& typeName)
{
        auto dsIt = dataStructures.find(typeName);
        if (dsIt == dataStructures.end()) return nullptr;
        if (dsIt->second.Destructor != nullptr) return dsIt->second.Destructor;
        const CxxClassInfo* info = GetCxxClassInfo(typeName);
        if (info == nullptr || !info->hasDtor) return nullptr;
        AbiRecipe recipe;
        llvm::Function* fn = GetOrCreateCxxStructor(typeName, info->destructor, recipe);
        if (fn == nullptr) return nullptr;
        // The complete-object destructor takes exactly 'this' and (on Itanium) returns it. That
        // is call-compatible with the shape EmitFullDestructorOverStorage emits, so the C++
        // symbol IS the class's CFlat destructor - no wrapper, no second cleanup mechanism.
        if (fn->arg_size() != 1) return nullptr;
        RegisterDestructor(typeName, fn);
        return fn;
    }

const LLVMBackend::CxxClassInfo::Structor* LLVMBackend::FindCxxDefaultCtor(const std::string& typeName) const
{
        const CxxClassInfo* info = GetCxxClassInfo(typeName);
        if (info == nullptr) return nullptr;
        for (const auto& c : info->constructors)
            if (c.isDefaultCtor && (c.params.size() == 1 || CxxConstantDefaultsFrom(c, 1)))
                return &c;
        return nullptr;
    }

const LLVMBackend::CxxClassInfo::Structor* LLVMBackend::FindCxxCopyCtor(const std::string& typeName) const
{
        const CxxClassInfo* info = GetCxxClassInfo(typeName);
        if (info == nullptr) return nullptr;
        for (const auto& c : info->constructors)
            if (c.isCopyCtor) return &c;
        return nullptr;
    }

const LLVMBackend::CxxClassInfo::Structor* LLVMBackend::FindCxxMoveCtor(const std::string& typeName) const
{
        const CxxClassInfo* info = GetCxxClassInfo(typeName);
        if (info == nullptr) return nullptr;
        for (const auto& c : info->constructors)
            if (c.isMoveCtor) return &c;
        return nullptr;
    }

/*
 * Constructor overload selection for `T(args)`. Arity first, then a per-argument compatibility
 * test that is deliberately narrow: a scalar matches a scalar of the same CFlat family, a
 * pointer matches a pointer, and a T-shaped argument matches the copy/move leg. Anything
 * outside that is refused rather than silently coerced - a wrong overload on a nontrivial type
 * corrupts an object, and clang's own ranking is not available at this point.
 */
const LLVMBackend::CxxClassInfo::Structor* LLVMBackend::SelectCxxConstructor(
        const std::string& typeName, const std::vector<TypeAndValue>& argTypes,
        std::string& why) const
{
        const CxxClassInfo* info = GetCxxClassInfo(typeName);
        if (info == nullptr) { why = "has no imported constructors"; return nullptr; }
        auto scalarFamily = [](const TypeAndValue& tv) -> int {
            if (tv.Pointer) return 3;
            if (tv.TypeName == "float" || tv.TypeName == "double") return 2;
            if (tv.TypeName == "bool") return 1;
            return 1;   // every remaining primitive is integral for selection purposes
        };
        auto compatible = [&](const TypeAndValue& want, const TypeAndValue& got) {
            if (want.TypeName == got.TypeName && want.Pointer == got.Pointer) return true;
            if (want.Pointer != got.Pointer) return false;
            if (want.Pointer) return false;   // unrelated pointee types never convert here
            if (dataStructures.count(want.TypeName) != 0 || dataStructures.count(got.TypeName) != 0)
                return false;                 // record types must match exactly
            return scalarFamily(want) == scalarFamily(got);
        };

        const CxxClassInfo::Structor* found = nullptr;
        size_t candidates = 0;
        auto sameBoundaryType = [](const TypeAndValue& a, const TypeAndValue& b) {
            return a.TypeName == b.TypeName && a.Pointer == b.Pointer
                && a.ElemPointer == b.ElemPointer && a.PointerDepth == b.PointerDepth
                && a.IsFunctionPointer == b.IsFunctionPointer && a.IsAlias == b.IsAlias
                && a.IsMove == b.IsMove && a.IsRvalueRef == b.IsRvalueRef;
        };
        size_t foundOmitted = 0;
        size_t foundExact = 0;
        for (const auto& c : info->constructors)
        {
            // Fewer arguments than parameters is fine when every omitted one has a constant
            // default (`parser(size_t max_capacity = DEFAULT_MAX_CAPACITY)` called as `parser()`).
            if (c.params.size() < argTypes.size() + 1) continue;
            const size_t omitted = c.params.size() - argTypes.size() - 1;
            if (omitted != 0 && !CxxConstantDefaultsFrom(c, argTypes.size() + 1)) continue;
            ++candidates;
            bool ok = true;
            size_t exact = 0;
            for (size_t i = 0; i < argTypes.size(); ++i)
            {
                const auto& want = c.params[i + 1];
                const auto& got = argTypes[i];
                if (want.TypeName == got.TypeName && want.Pointer == got.Pointer) ++exact;
                if (got.TypeName == "__closure_fat_ptr" && want.IsFunctionPointer
                    && want.IsThinFnPtr())
                {
                    why = "a capturing closure cannot be passed to C++; pass a plain function";
                    return nullptr;
                }
                if (c.isMoveCtor && !got.IsMove) { ok = false; break; }
                if (c.isCopyCtor && got.IsMove) { ok = false; break; }
                bool sameType = want.TypeName == got.TypeName;
                if (!sameType && IsStdFunctionSpecialization(typeName)
                    && IsStdFunctionSpecialization(got.TypeName))
                    sameType = true;
                bool copyRef = (c.isCopyCtor || c.isMoveCtor) && sameType;
                if (!copyRef && !compatible(want, got)) { ok = false; break; }
            }
            if (!ok) continue;
            if (verbose)
            {
                std::string shape;
                for (size_t i = 1; i < c.params.size(); ++i)
                    shape += (i > 1 ? ", " : "") + c.params[i].TypeName + (c.params[i].Pointer ? "*" : "");
                std::string got;
                for (const auto& a : argTypes) got += (got.empty() ? "" : ", ") + a.TypeName + (a.Pointer ? "*" : "");
                std::cout << std::format("[verbose]   ctor candidate {}({}) for ({}): exact={} omitted={}\n",
                                         typeName, shape, got, exact, omitted);
            }
            if (found != nullptr)
            {
                // An exact-arity overload beats one that fills defaults in, like C++ does, and
                // more exactly-typed parameters beat same-family conversions (`format_int(42)`
                // picks the int constructor over unsigned and long long).
                if (omitted > foundOmitted) continue;
                if (omitted < foundOmitted) { found = &c; foundOmitted = omitted; foundExact = exact; continue; }
                if (exact < foundExact) continue;
                if (exact > foundExact) { found = &c; foundExact = exact; continue; }
                bool sameShape = c.params.size() == found->params.size();
                for (size_t i = 0; sameShape && i < c.params.size(); ++i)
                    sameShape = sameBoundaryType(c.params[i], found->params[i]);
                if (sameShape) continue;
                why = "matches more than one constructor overload";
                return nullptr;
            }
            found = &c;
            foundOmitted = omitted;
            foundExact = exact;
        }
        if (found != nullptr) return found;
        if (candidates == 0)
        {
            why = std::format("has no constructor taking {} argument(s)", (uint64_t)argTypes.size());
            return nullptr;
        }
        // Name the argument types: with several same-arity overloads, "no match" alone does not say
        // which argument is the problem.
        std::string args;
        for (const auto& a : argTypes)
        {
            if (!args.empty()) args += ", ";
            args += "'" + SpellType(*this, a) + "'";
        }
        why = std::format("has no constructor whose parameter types match these arguments ({})", args);
        return nullptr;
    }

bool LLVMBackend::EmitCxxCopyOrMoveConstruct(const std::string& typeName, llvm::Value* dest,
                                             llvm::Value* src, bool useMove, const char* context)
{
        if (dest == nullptr || src == nullptr) return false;
        // C++ falls back from move to COPY construction when the class declares no move
        // constructor. It never falls back the other way: silently moving where a copy was
        // written would leave the source consumed behind the user's back.
        const CxxClassInfo::Structor* ctor = useMove ? FindCxxMoveCtor(typeName) : nullptr;
        if (ctor == nullptr) ctor = FindCxxCopyCtor(typeName);
        if (ctor == nullptr)
        {
            const CxxClassInfo* info = GetCxxClassInfo(typeName);
            const bool deleted = info != nullptr && info->hasDeletedCopyCtor;
            LogError(std::format(
                "cannot {} C++ class '{}' {}: its {} constructor is {} - "
                "pass or hold it by pointer instead",
                useMove ? "move" : "copy", typeName, context,
                useMove ? "move or copy" : "copy",
                deleted ? "deleted" : "not accessible from the imported header"));
            return false;
        }
        return EmitCxxStructorCall(typeName, *ctor, dest, { src });
    }

bool LLVMBackend::CxxObjectSizeAndAlign(const std::string& typeName, uint64_t& size, uint64_t& align)
{
        TypeAndValue tv{ .TypeName = typeName };
        llvm::Type* t = GetType(tv);
        if (t == nullptr || !t->isSized()) return false;
        align = GetEffectiveAlignmentForType(typeName, t);
        size = GetEffectiveAllocSize(t, align);
        return true;
    }

llvm::Function* LLVMBackend::GetCxxOperatorNew(bool overAligned)
{
        const char* name = overAligned ? "_ZnwmSt11align_val_t" : "_Znwm";
        if (llvm::Function* existing = module->getFunction(name)) return existing;
        auto* i64 = builder->getInt64Ty();
        auto* ptr = cflat_llvm::PointerTo(builder->getInt8Ty());
        std::vector<llvm::Type*> params{ i64 };
        if (overAligned) params.push_back(i64);   // std::align_val_t is a size_t-sized enum
        auto* fnTy = llvm::FunctionType::get(ptr, params, false);
        return llvm::Function::Create(fnTy, llvm::GlobalValue::ExternalLinkage, name, module.get());
    }

llvm::Function* LLVMBackend::GetCxxOperatorDelete(bool overAligned)
{
        const char* name = overAligned ? "_ZdlPvmSt11align_val_t" : "_ZdlPvm";
        if (llvm::Function* existing = module->getFunction(name)) return existing;
        auto* i64 = builder->getInt64Ty();
        auto* ptr = cflat_llvm::PointerTo(builder->getInt8Ty());
        std::vector<llvm::Type*> params{ ptr, i64 };
        if (overAligned) params.push_back(i64);
        auto* fnTy = llvm::FunctionType::get(builder->getVoidTy(), params, false);
        return llvm::Function::Create(fnTy, llvm::GlobalValue::ExternalLinkage, name, module.get());
    }

llvm::Value* LLVMBackend::EmitCxxHeapAllocate(const std::string& typeName)
{
        uint64_t size = 0, align = 0;
        if (!CxxObjectSizeAndAlign(typeName, size, align)) return nullptr;
        const bool overAligned = align > kDefaultNewAlign;
        llvm::Function* fn = GetCxxOperatorNew(overAligned);
        std::vector<llvm::Value*> args{ builder->getInt64(size) };
        if (overAligned) args.push_back(builder->getInt64(align));
        return builder->CreateCall(fn->getFunctionType(), fn, args, "cxx.new");
    }

void LLVMBackend::EmitCxxHeapFree(const std::string& typeName, llvm::Value* ptr)
{
        uint64_t size = 0, align = 0;
        if (ptr == nullptr || !CxxObjectSizeAndAlign(typeName, size, align)) return;
        const bool overAligned = align > kDefaultNewAlign;
        llvm::Function* fn = GetCxxOperatorDelete(overAligned);
        auto* voidPtr = builder->CreateBitCast(ptr, cflat_llvm::PointerTo(builder->getInt8Ty()));
        std::vector<llvm::Value*> args{ voidPtr, builder->getInt64(size) };
        if (overAligned) args.push_back(builder->getInt64(align));
        builder->CreateCall(fn->getFunctionType(), fn, args);
    }

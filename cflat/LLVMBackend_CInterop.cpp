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

void LLVMBackend::CheckPoisonedFunctionCalls()
{
        for (const auto& [name, msg] : poisonedFunctions)
        {
            llvm::Function* f = module->getFunction(name);
            if (f == nullptr) continue;
            for (auto* u : f->users())
            {
                if (llvm::isa<llvm::CallBase>(u))
                {
                    // Point the diagnostic at the call site rather than wherever the walk ended.
                    auto locIt = firstCallLocation_.find(name);
                    if (locIt != firstCallLocation_.end())
                        SetSourceLocation(locIt->second.first, locIt->second.second);
                    LogError(msg);
                    break;
                }
            }
        }
    }

bool LLVMBackend::VerifyModule()
{
        std::string errors;
        llvm::raw_string_ostream errorStream(errors);
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

std::string LLVMBackend::FindCDriver() const
{
        for (const char* cand : { "clang", "clang-18", "cc", "gcc" })
            if (auto p = llvm::sys::findProgramByName(cand)) return *p;
        return "";
    }

bool LLVMBackend::CompileCFileElf(const std::string& cSourcePath, const std::string& programAlias)
{
        const std::string cc = FindCDriver();
        if (cc.empty())
        {
            LogError(std::format("no C compiler driver (clang/cc/gcc) found - cannot compile C source '{}'.", cSourcePath));
            return false;
        }

        llvm::SmallString<256> objFile;
        if (auto ec = llvm::sys::fs::createTemporaryFile("cflat_c", "o", objFile))
        {
            LogError(std::format("could not create temp object for C source '{}': {}", cSourcePath, ec.message()));
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
        }
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
            std::cout << std::format("[verbose] compiling C source: {} -> {}\n", cSourcePath, objPath);
            std::cout << "[verbose]   cc";
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
            LogError(std::format("C compiler failed to compile C source '{}' (exit {}){}{}",
                cSourcePath, rc, compileErr.empty() ? "" : ": ", compileErr));
            return false;
        }

        cObjectFiles_.push_back(objPath);
        return true;
    }

bool LLVMBackend::CompileCFile(const std::string& cSourcePath, const std::string& programAlias)
{
        // Auto-discover C function signatures so the importing .cb needs no hand-written extern declarations.
        // When programAlias is set, registers C `main` as `__imported_main_<Alias>` in programTable.
        ExtractCSignatures(cSourcePath, programAlias);

        if (symbolSink_ != nullptr)
            return true;

        // Non-Windows targets compile to an ELF object with a GCC-style driver and link
        // via EmitExecutableElf; clang-cl + MSVC flags only apply to the COFF path.
        if (!targetWindows_)
            return CompileCFileElf(cSourcePath, programAlias);

        const std::string clangPath = FindClangCl();
        if (clangPath.empty())
        {
            LogError(std::format("clang-cl.exe not found - cannot compile C source '{}'.", cSourcePath));
            return false;
        }

        // Temp object next to the system temp dir; removed after linking.
        llvm::SmallString<256> objFile;
        if (auto ec = llvm::sys::fs::createTemporaryFile("cflat_c", "obj", objFile))
        {
            LogError(std::format("could not create temp object for C source '{}': {}", cSourcePath, ec.message()));
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
            LogError(std::format("clang-cl failed to compile C source '{}' (exit {}){}{}",
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
            LogError("cannot locate crash handler source: runtime directory is unset.");
            return false;
        }

        llvm::SmallString<256> srcPath(runtimeDir);
        llvm::sys::path::append(srcPath, "core", "diagnostic", "crashdump.c");
        if (!llvm::sys::fs::exists(srcPath))
        {
            LogError(std::format("crash handler source not found: '{}'.", srcPath.str().str()));
            return false;
        }

        const std::string clangPath = FindClangCl();
        if (clangPath.empty())
        {
            LogError("clang-cl.exe not found - cannot compile crash handler.");
            return false;
        }

        llvm::SmallString<256> objFile;
        if (auto ec = llvm::sys::fs::createTemporaryFile("cflat_crashdump", "obj", objFile))
        {
            LogError(std::format("could not create temp object for crash handler: {}", ec.message()));
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
            LogError(std::format("clang-cl failed to compile crash handler (exit {}){}{}",
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
            LogError("cannot locate cflat_builtins.c: runtime directory is unset.");
            return false;
        }

        llvm::SmallString<256> srcPath(runtimeDir);
        llvm::sys::path::append(srcPath, "core", "cflat_builtins.c");
        if (!llvm::sys::fs::exists(srcPath))
        {
            LogError(std::format("builtins source not found: '{}'.", srcPath.str().str()));
            return false;
        }

        const std::string clangPath = FindClangCl();
        if (clangPath.empty())
        {
            LogError("clang-cl.exe not found - cannot compile cflat_builtins.c.");
            return false;
        }

        llvm::SmallString<256> objFile;
        if (auto ec = llvm::sys::fs::createTemporaryFile("cflat_builtins", "obj", objFile))
        {
            LogError(std::format("could not create temp object for builtins: {}", ec.message()));
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
            LogError(std::format("clang-cl failed to compile cflat_builtins.c (exit {}){}{}",
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

std::string LLVMBackend::AggregatePointeeTag(const std::string& spelling, int& outPtr)
{
        outPtr = 0;
        std::string s = spelling;
        for (const char* w : { "const", "volatile", "restrict", "__restrict", "__restrict__",
                               "_Nonnull", "_Nullable", "_Null_unspecified" })
            for (size_t pos; (pos = s.find(w)) != std::string::npos; ) s.erase(pos, std::strlen(w));
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

bool LLVMBackend::MapCTypeToTypeAndValueImpl(std::string ctype, TypeAndValue& out,
                                    std::unordered_set<std::string>& visited)
{
        // Detect function-pointer spelling before the '*'-strip path mangles it.
        // clang spells these as "R (*)(args)"; the "(*)" token disambiguates from a declaration.
        if (ctype.find("(*)") != std::string::npos)
        {
            if (ParseCFunctionPointerSpelling(ctype, out, visited))
                return true;
            // Fall through only if the parse failed - lets unknown shapes hit the normal
            // "return false" path below instead of being silently accepted.
            return false;
        }

        // Arrays decay to a pointer; drop the '[...]' and bump the pointer level.
        int ptr = 0;
        if (auto br = ctype.find('['); br != std::string::npos)
        {
            ptr++;
            ctype = ctype.substr(0, br);
        }
        ptr += (int)std::count(ctype.begin(), ctype.end(), '*');
        ctype.erase(std::remove(ctype.begin(), ctype.end(), '*'), ctype.end());

        // Strip cv / nullability qualifiers - they do not affect the ABI here.
        auto stripWord = [&](const char* w)
        {
            std::string word = w;
            for (size_t pos; (pos = ctype.find(word)) != std::string::npos; )
                ctype.erase(pos, word.size());
        };
        for (const char* q : { "const", "volatile", "restrict", "__restrict", "__restrict__",
                               "_Nonnull", "_Nullable", "_Null_unspecified" })
            stripWord(q);

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
        if (base.rfind("enum ", 0) == 0)
        {
            mapped = "int";
        }
        else if (ptr == 0 && (base.rfind("struct ", 0) == 0 || base.rfind("union ", 0) == 0))
        {
            std::string tag = (base.rfind("struct ", 0) == 0)
                ? base.substr(7)
                : base.substr(6);
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
                { "float", "float" }, { "double", "double" }, { "long double", "double" },
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
            else if (cLongSigned.count(base) > 0)
                mapped = targetWindows_ ? "i32" : "i64";
            else if (cLongUnsigned.count(base) > 0)
                mapped = targetWindows_ ? "u32" : "u64";
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
        for (const CSigEntry& e : sigs)
        {
            std::string regName  = e.name;
            bool        isProgMain = (!programAlias.empty() && e.name == "main");
            if (isProgMain)
                regName = "__imported_main_" + programAlias;

            // external=true: unmangled name + C-compatible types; cdecl on the call.
            CreateFunctionDeclaration(regName, e.ret, e.params, /*external=*/true, e.variadic,
                                      /*returnsOwned=*/false, /*isMethod=*/false,
                                      CallingConv::Cdecl);

            if (isProgMain)
            {
                programTable[programAlias].MainFunction     = module->getFunction(regName);
                programTable[programAlias].IsImportedProgram = true;
                continue; // not a user-facing symbol; skip the LSP sink registration below
            }

            if (auto* s = GetSymbolSink())
            {
                std::string sig = e.ret.TypeName + (e.ret.Pointer ? "*" : "") + " " + e.name + "(";
                bool first = true;
                for (const auto& p : e.params)
                {
                    if (!first) sig += ", ";
                    first = false;
                    sig += p.TypeName;
                    if (p.Pointer) sig += "*";
                    if (!p.VariableName.empty()) sig += " " + p.VariableName;
                }
                sig += ")";
                // Prefer the declaration's own header (presumed loc) so go-to-definition
                // lands on the real prototype, not the umbrella header that was imported.
                const std::string& declFile = e.file.empty() ? fileForLsp : e.file;
                s->Register(SymbolKind::Function, e.name, declFile, e.line, e.col < 0 ? 0 : e.col, sig);
            }
        }

        if (verbose)
            std::cout << std::format("[verbose]   registered {} C function(s) from {}\n", sigs.size(), fileForLsp);
    }

std::vector<std::string> LLVMBackend::BuildClangDriverArgs(const std::string& headerDir,
                                               const std::vector<std::string>& extraDefines,
                                               bool errorRecovery, bool asCxx) const
{
        std::vector<std::string> args;
        // Parse the C against the target OS so predefined macros (_WIN32 vs __linux__)
        // and the system header search behave like the real compile would. On non-Windows
        // the MSVC triple would pull in MSVC predefines and break libc header resolution.
        if (targetWindows_)
            args.push_back(platformValue == 32 ? "--target=i686-pc-windows-msvc"
                                               : "--target=x86_64-pc-windows-msvc");
        else if (targetMacOS_)
        {
            // Match the codegen triple from EmitExecutableMachO so __APPLE__ and the Apple
            // system-header search are active. Headers need a REAL SDK (the harvested
            // ~/.cflat/macsdk carries link stubs only), so isysroot points at $SDKROOT/xcrun.
            args.push_back("--target=arm64-apple-macosx11.0.0");
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
                LogError("C header import targeting macOS requires an SDK: set $SDKROOT or install "
                         "Xcode / Command Line Tools so 'xcrun --show-sdk-path' resolves");
            }
        }
        else
            args.push_back(platformValue == 32 ? "--target=i686-pc-linux-gnu"
                                               : "--target=x86_64-pc-linux-gnu");
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

bool LLVMBackend::MapRawSig(const cflat_cinterop::RawSig& r, CSigEntry& e)
{
        e = CSigEntry();
        e.name     = r.name;
        e.variadic = r.variadic;
        e.file     = r.file;
        e.line     = r.line ? r.line : 1;
        e.col      = r.col < 0 ? 0 : r.col;
        if (!MapCTypeToTypeAndValue(r.retType, e.ret))
        {
            if (verbose) std::cout << std::format("[verbose]   skipping '{}': unsupported return type '{}'\n", r.name, r.retType);
            return false;
        }
        for (size_t i = 0; i < r.paramTypes.size(); ++i)
        {
            TypeAndValue ptv;
            if (!MapCTypeToTypeAndValue(r.paramTypes[i], ptv))
            {
                if (verbose) std::cout << std::format("[verbose]   skipping '{}': unsupported parameter type '{}'\n", r.name, r.paramTypes[i]);
                return false;
            }
            if (i < r.paramNames.size()) ptv.VariableName = r.paramNames[i];
            e.params.push_back(std::move(ptv));
        }
        return true;
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
        if (r.kind == K::Skip) return false;
        e = CMacroEntry();
        e.name = r.name; e.file = r.file; e.line = r.line ? r.line : 1; e.col = 0;
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
                cTypedefMap_.emplace(t.name, t.underlying);
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

void LLVMBackend::RegisterRecordAliases(const std::vector<std::pair<std::string, std::string>>& aliases)
{
        for (const auto& [alias, target] : aliases)
        {
            if (dataStructures.find(alias) != dataStructures.end()) continue;  // real type wins
            if (typeAliases.find(alias) != typeAliases.end()) continue;        // first-writer-wins
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
            if (ctype.find('*') != std::string::npos) return {};   // pointer: no sizing dependency
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
                if (s.rfind("enum ", 0) == 0)     return {};   // enum is scalar (int-sized)
                break;
            }
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
            rec.line = r.line ? r.line : 1; rec.col = r.col < 0 ? 0 : r.col;
            rec.uuid = r.uuid;
            for (const auto& f : r.fields)
            {
                CRecordFieldEntry fe;
                fe.name = f.name; fe.ctype = f.ctype;
                fe.isBitfield = f.isBitfield; fe.bitWidth = f.bitWidth;
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
                             const std::vector<std::string>& extraDefines,
                             std::vector<std::string>* outIncludes,
                             bool* outPrereqFailure,
                             std::string* outPrereqMsg)
{
        if (headerPaths.empty()) return false;

        // The first header's directory anchors the primary -I; also labels TimeTrace scopes
        // and attributes registered decls for LSP go-to-def.
        const std::string& headerPath = headerPaths.front();
        const std::string primaryDir = std::filesystem::path(headerPath).parent_path().string();

        std::string source;
        for (const auto& h : headerPaths)
        {
            std::string fwd = h;
            std::replace(fwd.begin(), fwd.end(), '\\', '/');
            source += "#include \"" + fwd + "\"\n";
        }

        cflat_cinterop::ExtractRequest req;
        req.mainFileName   = "cflat_hdr_stub.c";
        req.source         = source;
        req.args           = BuildClangDriverArgs(primaryDir, extraDefines, /*errorRecovery*/ true);
        req.wantMacros     = true;
        req.requireInScope = true;
        req.skipFunctionBodies = true;   // headers: declarations only - skip inline bodies
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
        }

        {
            llvm::TimeTraceScope sigScope("MapSignatures", headerPath);
            for (const auto& rs : raw.sigs)
            {
                CSigEntry e;
                if (MapRawSig(rs, e)) outSigs.push_back(std::move(e));
            }
        }

        {
            llvm::TimeTraceScope enumScope("MapEnums", headerPath);
            for (const auto& re : raw.enums)
            {
                CEnumEntry e;
                e.name = re.name; e.value = re.value;
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

bool LLVMBackend::ExtractCFileClang(const std::string& cSourcePath,
                           std::vector<CSigEntry>& outSigs, std::vector<CRecordEntry>& outRecords,
                           std::vector<CGlobalEntry>& outGlobals)
{
        llvm::TimeTraceScope extractScope("CFileExtract", cSourcePath);

        cflat_cinterop::ExtractRequest req;
        req.realPath        = cSourcePath;     // parsed from disk
        req.args            = BuildClangDriverArgs(/*headerDir*/ "", /*extraDefines*/ {}, /*errorRecovery*/ true);
        req.wantMacros      = false;
        req.requireInScope  = false;
        req.definitionsOnly = true;

        if (verbose)
            std::cout << std::format("[verbose] extracting C signatures: {} (clang C++ API)\n", cSourcePath);

        cflat_cinterop::ExtractResult raw;
        std::string err;
        if (!cflat_cinterop::ExtractCInterop(req, raw, err))
        {
            if (verbose) std::cout << std::format("[verbose]   C extraction failed: {}\n", err);
            return false;
        }

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

bool LLVMBackend::ExtractCSignatures(const std::string& cSourcePath, const std::string& programAlias)
{
        // Canonical path: stable cache key + the real .c for LSP go-to-definition.
        llvm::SmallString<256> realPath;
        std::string fileForLsp = cSourcePath;
        if (!llvm::sys::fs::real_path(cSourcePath, realPath))
            fileForLsp = realPath.str().str();

        // Defines can gate which functions a .c defines, so fold them into the cache key
        // (the file path alone is the LSP identity; the key is path + defines).
        std::string cacheKey = fileForLsp;
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
                    hitSigs = entry.sigs;
                    hitRecords = entry.records;
                    hitGlobals = entry.globals;
                    hit = true;
                }
                // Timestamp moved but content may be identical - only now pay for a hash.
                else if (hashNow() == entry.hash)
                {
                    if (verbose) std::cout << std::format("[verbose] C signatures cache hit (hash) for {}\n", fileForLsp);
                    entry.mtime = currentMtime; // refresh so the next check short-circuits on mtime
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
        if (!ExtractCFileClang(cSourcePath, sigs, records, globals))
            return false;

        if (!mtEc)
        {
            CFileSigCacheEntry entry;
            entry.mtime = currentMtime;
            entry.hash  = hashNow();
            entry.sigs  = sigs;
            entry.records = records;
            entry.globals = globals;
            std::lock_guard<std::mutex> lock(cFileSigCacheMutex_);
            cFileSigCache_[cacheKey] = std::move(entry);
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
            // First writer wins: a hand-written declaration or an earlier header takes
            // precedence over a duplicate constant name.
            if (globalNamedVariable.count(e.name)) continue;

            bool wide = (e.value < INT32_MIN || e.value > INT32_MAX);
            TypeAndValue tv;
            tv.TypeName     = wide ? "i64" : "int";
            tv.VariableName = e.name;
            tv.Pointer      = false;
            llvm::Constant* c = wide
                ? static_cast<llvm::Constant*>(builder->getInt64((uint64_t)e.value))
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
        if (verbose)
            std::cout << std::format("[verbose]   registered {} C global(s) from {}\n", globals.size(), fileForLsp);
    }

void LLVMBackend::RegisterCRecords(const std::vector<CRecordEntry>& records, const std::string& fileForLsp)
{
        if (records.empty()) return;

        // Pass 1: opaque shells. CFlat-defined types win; anonymous records are skipped
        // (clang inlines their fields at the JSON layer).
        std::vector<const CRecordEntry*> ours;
        ours.reserve(records.size());
        for (const auto& r : records)
        {
            if (r.name.empty()) continue;
            if (dataStructures.find(r.name) != dataStructures.end()) continue;
            // Create the opaque shell so later fields/records in the same batch can refer to it.
            CreateStructType(r.name, /*typeAndValues*/{});
            ours.push_back(&r);
        }

        // Pass 2: bodies. On unmappable fields leave the opaque shell in place so a later
        // reference surfaces a clear error rather than crashing on a partial struct.
        for (const CRecordEntry* rp : ours)
        {
            const CRecordEntry& r = *rp;
            std::vector<DeclTypeAndValue> fields;
            fields.reserve(r.fields.size());
            bool ok = true;
            for (const auto& f : r.fields)
            {
                TypeAndValue tv;
                // Strip fixed-array dims before mapping: the shared mapper decays `[N]` to a
                // pointer (right for params, wrong for fields), so peel them here first.
                std::vector<uint64_t> arrDims;
                std::string elemSpelling = StripFixedArrayDims(f.ctype, arrDims);
                if (!MapCTypeToTypeAndValue(elemSpelling, tv))
                {
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
            if (!ok || fields.empty())
            {
                continue;
            }
            // An opaque-shell by-value aggregate field has no size; CreateStructType would
            // assert "Cannot getTypeInfo() on unsized". Abandon and leave the shell.
            for (const auto& d : fields)
            {
                if (d.Pointer) continue;            // pointers are always sized
                auto* ft = GetType(d);
                if (ft && !ft->isSized())
                {
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
            if (r.isUnion)
                CreateUnionType(r.name, fields);
            else
                CreateStructType(r.name, fields, 0,
                    anyBitfields ? &packedBitfields : nullptr);
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
                    std::string typeSig = f.TypeName;
                    if (f.Pointer) typeSig += "*";
                    if (f.ElemPointer) typeSig += "*";
                    std::string fieldSig = annSig + typeSig + " " + f.VariableName;
                    if (f.IsBitfield && f.BitWidth > 0)
                        fieldSig += ":" + std::to_string(f.BitWidth);
                    s->Register(SymbolKind::Field, r.name + "." + f.VariableName,
                                fileForLsp, r.line, 0, fieldSig);
                }
            }
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

        if (verbose)
            std::cout << std::format("[verbose]   registered {} C record(s) from {}\n", ours.size(), fileForLsp);
    }

void LLVMBackend::RegisterCMacros(const std::vector<CMacroEntry>& macros)
{
        size_t registered = 0;
        for (const CMacroEntry& m : macros)
        {
            if (m.name.empty()) continue;
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
                c = llvm::ConstantInt::get(llvm::IntegerType::get(*context, bits),
                                           (uint64_t)m.value, /*isSigned*/ true);
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
            if (pos > 0)
            {
                char prev = out[pos - 1];
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
                                 const std::string& fileForLsp)
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
            LogError(std::format(
                "C header '{}' did not compile in this group ({}). Reorder the group so the "
                "prerequisite header comes first, or add the missing one: import {{ {} }};",
                name, detail, grp));
            return;
        }
        LogError(std::format(
            "C header '{}' does not compile on its own ({}). It likely needs a prerequisite "
            "header included first. Import them together as one group so they share a single "
            "translation unit, e.g. import {{ \"prerequisite.h\", \"{}\" }};",
            name, detail, name));
    }

bool LLVMBackend::CompileCHeader(const std::string& headerPath, const std::vector<std::string>& extraDefines,
                        bool diskCache)
{
        return CompileCHeaderGroup(std::vector<std::string>{ headerPath }, extraDefines, diskCache);
    }

bool LLVMBackend::CompileCHeaderGroup(const std::vector<std::string>& headerPaths,
                             const std::vector<std::string>& extraDefines,
                             bool diskCache)
{
        if (headerPaths.empty()) return true;

        std::vector<std::string> realPaths;
        realPaths.reserve(headerPaths.size());
        for (const auto& h : headerPaths)
        {
            llvm::SmallString<256> rp;
            realPaths.push_back(!llvm::sys::fs::real_path(h, rp) ? rp.str().str() : h);
        }
        const std::string& fileForLsp = realPaths.front();

        // Fold headers, include-dirs, and defines: same header under different roots/defines or
        // standalone vs. grouped must not share a stale cache entry.
        std::string cacheKey;
        for (const auto& rp : realPaths)       cacheKey += "|H" + rp;
        for (const auto& inc : cIncludeDirs_)  cacheKey += "|I" + inc;
        for (const auto& def : cDefines_)      cacheKey += "|D" + def;
        for (const auto& def : extraDefines)   cacheKey += "|d" + def;
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
                    hitSigs = entry.sigs; hitEnums = entry.enums; hitRecords = entry.records;
                    hitMacros = entry.macros; hitFuncMacros = entry.funcMacros; hitGlobals = entry.globals;
                    hitAliases = entry.recordAliases; hit = true;
                }
                else if (hashNow() == entry.hash)
                {
                    if (verbose) std::cout << std::format("[verbose] C header cache hit (hash) for {}\n", fileForLsp);
                    entry.mtime = currentMtime;
                    hitSigs = entry.sigs; hitEnums = entry.enums; hitRecords = entry.records;
                    hitMacros = entry.macros; hitFuncMacros = entry.funcMacros; hitGlobals = entry.globals;
                    hitAliases = entry.recordAliases; hit = true;
                }
            }
        }
        if (hit)
        {
            // Records before sigs so struct-by-value signatures resolve to the same types.
            RegisterCRecords(hitRecords, fileForLsp);
            RegisterRecordAliases(hitAliases);
            RegisterCSignatures(hitSigs, fileForLsp);
            RegisterCEnums(hitEnums, fileForLsp);
            RegisterCMacros(hitMacros);
            RegisterCFunctionMacros(hitFuncMacros, fileForLsp);
            RegisterCGlobals(hitGlobals, fileForLsp);
            return true;
        }

        // Persistent disk cache (opt-in via `cache` import clause). On hit, preloads the
        // in-memory cache and registers decls, skipping the clang header parse entirely.
        std::filesystem::path cHeaderCacheDir = GetCHeaderCacheDir();
        uint64_t diskKey = 0;
        if (diskCache && !mtEc && !cHeaderCacheDir.empty())
        {
            diskKey = CHeaderDiskCacheKey(realPaths, cIncludeDirs_, cDefines_, extraDefines);
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
                    cFileSigCache_[cacheKey] = diskEntry;
                }
                llvm::TimeTraceScope registerScope("CHeaderRegister", fileForLsp);
                RegisterCRecords(diskEntry.records, fileForLsp);
                RegisterRecordAliases(diskEntry.recordAliases);
                RegisterCSignatures(diskEntry.sigs, fileForLsp);
                RegisterCEnums(diskEntry.enums, fileForLsp);
                RegisterCMacros(diskEntry.macros);
                RegisterCFunctionMacros(diskEntry.funcMacros, fileForLsp);
                RegisterCGlobals(diskEntry.globals, fileForLsp);
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
        // Deep mode: collect the transitive include set so the disk entry can validate it.
        bool wantDeps = diskCache && cHeaderCacheDeep_ && !cHeaderCacheDir.empty();
        std::vector<std::string> includes;
        {
            // All C entities are extracted in one full parse (plus a cheap preprocess-only prepass
            // for macro names). Uses clang C++ API, not clang-cl or libclang.
            llvm::TimeTraceScope extractScope("CHeaderExtract", fileForLsp);
            bool prereqFailure = false;
            std::string prereqMsg;
            if (!ExtractCHeaderClang(realPaths, sigs, enums, records, macros, funcMacros, globals,
                                     aliases, extraDefines, wantDeps ? &includes : nullptr,
                                     &prereqFailure, &prereqMsg))
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
            entry.sigs  = sigs;
            entry.enums = enums;
            entry.records = records;
            entry.macros = macros;
            entry.funcMacros = funcMacros;
            entry.globals = globals;
            entry.recordAliases = aliases;
            // Keep only real on-disk paths in the transitive dependency list (deep mode).
            // Non-existent deps would otherwise poison every later cache validation.
            if (wantDeps)
            {
                std::unordered_set<std::string> seen;
                for (const auto& inc : includes)
                {
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
            if (diskCache && !runMode_ && !cHeaderCacheDir.empty())
                WriteCHeaderDiskCache(cHeaderCacheDir, diskKey, currentMtime, hashNow(), entry);
            std::lock_guard<std::mutex> lock(cFileSigCacheMutex_);
            cFileSigCache_[cacheKey] = std::move(entry);
        }

        // Records were already registered inside ExtractCHeaderClang.
        {
            llvm::TimeTraceScope registerScope("CHeaderRegister", fileForLsp);
            RegisterCSignatures(sigs, fileForLsp);
            RegisterCEnums(enums, fileForLsp);
            RegisterCMacros(macros);
            RegisterCFunctionMacros(funcMacros, fileForLsp);
            RegisterCGlobals(globals, fileForLsp);
        }
        return true;
    }


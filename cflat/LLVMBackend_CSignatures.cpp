#pragma warning(push)
#pragma warning(disable: 4244 4267)
#include <bit>
#include <cctype>
#include <thread>
#include <fstream>
#include <iterator>
#include <llvm/Support/xxhash.h>
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
#include <llvm/Transforms/Utils/Cloning.h>
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
#include <llvm/Support/Process.h>
#include <llvm/Support/JSON.h>
#include <llvm/IR/DiagnosticInfo.h>
#include <llvm/IR/DiagnosticHandler.h>
#pragma warning(pop)
#include <antlr4-runtime.h>

#include "platform/GeneratedParser.h"
#include "LLVMBackend.h"
#include "CxxSpelling.h"
#include "CppStructName.h"
#include "CxxIncrementalGroup.h"
#include "MainListener.h"
#include "GrammarTreeListener.h"
#include "TypeMangling.h"
#include <filesystem>
#include <optional>
#include <algorithm>
#include <cctype>
#include <atomic>
#include <mutex>
#include <chrono>
#include <fstream>
#include <map>
#include <unordered_map>
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

#include "LLVMBackend_CInteropShared.h"

void LLVMBackend::RegisterCSignatures(const std::vector<CSigEntry>& sigs, const std::string& fileForLsp,
                             const std::string& programAlias)
{
        std::vector<CSigEntry> rebound;
        const std::vector<CSigEntry>* signatures = &sigs;
        if (std::any_of(sigs.begin(), sigs.end(), [](const CSigEntry& e) {
                return e.needsCxxRebind;
            }))
        {
            rebound = sigs;
            RebindCxxCachedSignatures(rebound);
            signatures = &rebound;
        }
        std::unordered_map<std::string, std::string> stdFunctionClasses;
        for (const CSigEntry& e : *signatures)
        {
            if (!e.isCxx && e.isInline && !e.name.empty() && !e.file.empty()
                && !e.inlineHeaders.empty())
            {
                auto found = cInlineBodyRequests_.find(e.name);
                if (found == cInlineBodyRequests_.end() || found->second.headers.empty())
                    cInlineBodyRequests_[e.name] = CInlineBodyRequest{
                        std::filesystem::path(e.file).lexically_normal().string(), e.name,
                        e.inlineDefines, e.inlineHeaders, e.inlineHashFiles, e.isStaticInline};
            }
            // CFlat [cpp] method helpers have C linkage but need their extracted free-function ABI.
            if (e.isCxx || e.name.starts_with("__cflat_ovr_")
                || e.linkageName.starts_with("__cflat_ovr_")
                || e.name.starts_with("__cflat_mth_")
                || e.linkageName.starts_with("__cflat_mth_"))
            {
                auto& rawEntries = cxxFunctionSignatures_[e.name];
                const bool duplicate = std::any_of(rawEntries.begin(), rawEntries.end(), [&](const auto& old) {
                    return old.linkageName == e.linkageName && old.paramSpellings == e.paramSpellings
                        && old.retSpelling == e.retSpelling;
                });
                if (!duplicate) rawEntries.push_back(e);
                if (e.isCxx && !e.linkageName.empty())
                {
                    auto [spellings, inserted] = cxxFunctionParameterSpellingsByLinkage_.try_emplace(
                        e.linkageName, e.paramSpellings);
                    if (!inserted && spellings->second != e.paramSpellings)
                        spellings->second.clear();
                }
                if (activeCxxRequestGroup_ != nullptr)
                    cxxFunctionOwnerGroup_.emplace(e.name, activeCxxRequestGroup_->primary);
            }
            if (e.isCxx && !e.variadic && !e.linkageName.empty()
                && e.defaultArgs.size() == e.paramSpellings.size())
                for (size_t n = 0; n < e.defaultArgs.size(); ++n)
                    if (e.defaultArgs[n].kind != "unsupported"
                        && HasNonConstDefaultSuffix(e.defaultArgs, n))
                        RememberCxxDefaultWrapper(e.name, e.linkageName, n, false);
            if (!e.bindRefusal.empty())
            {
                cxxBindingRefusals_[e.name] = e.bindRefusal;
                if (e.isCxx) SeedCxxNamespacesOfDottedName(e.name);
                continue;
            }
            // Deferred binding: declare this signature once its C++ types exist, not before.
            if (e.isCxx && !CxxSignatureTypesRegistered(e))
            {
                // The namespace exists whether or not the signature binds now: without this a
                // namespace of only-deferred declarations reports "Undefined variable <ns>".
                SeedCxxNamespacesOfDottedName(e.name);
                continue;
            }
            std::string regName  = e.name;
            if (e.isCxx)
            {
                const size_t op = regName.rfind(".operator");
                if (op != std::string::npos) regName.erase(0, op + 1);
            }
            // Member operators are registered by RegisterCxxClassMembers. A non-member C++
            // operator is registered under the bare spelling above for compatibility, and also
            // gets a private qualified alias so ADL can select only its namespace's free overloads.
            const bool isCxxFreeOperator = e.isCxx
                && (e.name.starts_with("operator") || e.name.rfind(".operator") != std::string::npos);
            const std::string freeOperatorLookupName = isCxxFreeOperator
                ? (e.name.find('.') == std::string::npos ? "__cxx_free." + e.name : e.name)
                : std::string();
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
            bool changed = false;
            TypeAndValue retypedRet = e.ret;
            // A std::function RETURN crosses by value as its specialization, the same type the
            // parameter retyping below produces - MapRawSig left a placeholder for it.
            if (e.isCxx && retypedRet.Pointer && retypedRet.TypeName == "void"
                && !retypedRet.IsFunctionPointer
                && IsTopLevelStdFunctionSpelling(e.retSpelling))
            {
                auto cached = stdFunctionClasses.find(e.retSpelling);
                std::string className;
                if (cached != stdFunctionClasses.end()) className = cached->second;
                else
                {
                    std::string requestError;
                    className = StdFunctionSpecializationForSpelling(e.retSpelling, &requestError);
                    if (!requestError.empty()) LogError(requestError);
                    if (!className.empty()) stdFunctionClasses.emplace(e.retSpelling, className);
                }
                if (!className.empty())
                {
                    retypedRet.TypeName = className;
                    retypedRet.Pointer = false;
                    // An lvalue-reference return is a borrowed lvalue (`alias T`), as elsewhere.
                    retypedRet.IsAlias = e.retSpelling.find('&') != std::string::npos
                        && e.retSpelling.find("&&") == std::string::npos;
                    changed = true;
                }
                else
                {
                    // The placeholder must never reach a declaration: without the specialization
                    // the return has no CFlat type, so keep the refusal MapRawSig would have made.
                    cxxBindingRefusals_[e.name] = std::format(
                        "'{}' was not bound: return type '{}' is unsupported", e.name, e.retSpelling);
                    SeedCxxNamespacesOfDottedName(e.name);
                    continue;
                }
            }
            if (e.isCxx && e.paramSpellings.size() == e.params.size())
            {
                retypedParams = e.params;
                for (size_t i = 0; i < retypedParams.size(); ++i)
                {
                    TypeAndValue& p = retypedParams[i];
                    const std::string& ps = e.paramSpellings[i];
                    {
                        auto cached = stdFunctionClasses.find(ps);
                        std::string className;
                        if (cached != stdFunctionClasses.end()) className = cached->second;
                        else
                        {
                            std::string requestError;
                            // Top-level only: `std::vector<std::function<int(int)>>` is a vector,
                            // and retyping it as the inner std.function passed the wrong object.
                            className = StdFunctionSpecializationForSpelling(ps, &requestError);
                            if (!requestError.empty()) LogError(requestError);
                            if (!className.empty()) stdFunctionClasses.emplace(ps, className);
                        }
                        if (!className.empty())
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
            }
            if (changed)
            {
                retypedEntry = e;
                if (!retypedParams.empty()) retypedEntry.params = std::move(retypedParams);
                retypedEntry.ret = retypedRet;
                sigp = &retypedEntry;
            }
            const CSigEntry& sig = *sigp;
            /*
             * Every C++ type request hands back its group's whole free-function surface, so one
             * declaration arrives here once per request. One this loop already bound, still in
             * the table with this exact (retyped) signature, has nothing left to register.
             */
            std::string boundKey;
            if (e.isCxx && !e.linkageName.empty() && programAlias.empty())
            {
                boundKey = e.name + '\x1f' + e.linkageName;
                if (cxxBoundSignatureKeys_.count(boundKey) != 0
                    && IsCxxDeclarationRegistered(regName, sig))
                    continue;
            }
            if (e.isCxx)
            {
                // Prototype boundary: the C++ path carries primitives and bare pointers only.
                // A record by value needs the aggregate ABI arrangement, which is not part of
                // this prototype - refuse at registration so the LSP sees the same answer.
                // A refused declaration still proves its namespace exists - seed before the drop.
                if (RejectCxxRecordByValue(e)) { SeedCxxNamespacesOfDottedName(e.name); continue; }
                auto needsProjectedByValueRecord = [&](const TypeAndValue& type) {
                    return !type.TypeName.empty() && !type.Pointer && !type.IsAlias
                        && !type.IsInterface && !type.IsFunctionPointer
                        && cxxRecords_.count(type.TypeName) != 0
                        && pendingCxxRecordProjections_.count(type.TypeName) != 0;
                };
                bool deferForRecordLayout = needsProjectedByValueRecord(sig.ret);
                for (const TypeAndValue& param : sig.params)
                    deferForRecordLayout = deferForRecordLayout
                        || needsProjectedByValueRecord(param);
                const bool generatedWrapper = e.linkageName.starts_with("__cflat_");
                if (generatedWrapper)
                {
                    auto projectByValueRecord = [&](const TypeAndValue& type) {
                        if (needsProjectedByValueRecord(type))
                            EnsureCxxRecordProjected(type.TypeName, false);
                    };
                    projectByValueRecord(sig.ret);
                    for (const TypeAndValue& param : sig.params) projectByValueRecord(param);
                }
                if (deferForRecordLayout && !generatedWrapper)
                {
                    // Keep the raw overload in cxxFunctionSignatures_ for LSP and overload
                    // resolution. Its ABI needs an aggregate layout, so bind it when first used.
                    SeedCxxNamespacesOfDottedName(e.name);
                    continue;
                }
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
            std::string abiMismatch;
            {
                CInteropDeclarationScope declaringFile(*this, e.file.empty() ? fileForLsp : e.file);
                // Hand clang's arrangement to the declaration; C leaves it null and keeps the
                // existing size heuristic. Cleared by CreateFunctionDeclaration on entry.
                CxxAbiPlanScope abiPlan(*this, (e.isCxx && e.abi.valid) ? &e.abi : nullptr,
                                        e.isCxx ? &abiMismatch : nullptr);
                CreateFunctionDeclaration(regName, sig.ret, sig.params, /*external=*/true, e.variadic,
                                          /*returnsOwned=*/false, /*isMethod=*/false,
                                          CallingConv::Cdecl, linkageName, e.isCxx, e.isNoexcept,
                                          cflat_cinterop::CxxRefQualifierNone, e.isDeleted);
                if (e.isTemplateSpecialization)
                    if (auto found = functionTable.find(regName); found != functionTable.end())
                        for (auto it = found->second.rbegin(); it != found->second.rend(); ++it)
                            if (it->External && it->UniqueName == linkageName)
                            {
                                it->IsCxxTemplateSpecialization = true;
                                break;
                            }
            }
            if (!abiMismatch.empty())
            {
                // The sink holds a subjectless fragment (the member path names the member around
                // it), so name the function here or the replayed refusal has no subject.
                cxxBindingRefusals_[e.name] =
                    std::format("C++ function '{}' {}", e.name, abiMismatch);
                if (verbose)
                    std::cout << std::format("[verbose]   C++ function {} not bound: {}\n",
                                             e.name, abiMismatch);
                continue;
            }
            if (auto fit = functionTable.find(regName); fit != functionTable.end())
            {
                bool assignedDefaults = false;
                for (FunctionSymbol& sym : fit->second)
                    if (sym.External)
                    {
                        sym.IsCInteropDeclaration = true;
                        if (sym.UniqueName == linkageName) sym.CxxCtorPick = e.ctorPick;
                        if (sym.UniqueName == linkageName)
                        {
                            sym.DefaultArguments = e.defaultArgs;
                            assignedDefaults = true;
                        }
                    }
                if (!assignedDefaults && fit->second.size() == 1)
                    fit->second.front().DefaultArguments = e.defaultArgs;
            }

            // The Windows SDK's friendly Interlocked names are object-like aliases of these
            // clang builtins, but the defining intrin.h macros can sit outside the imported
            // C++ header's macro scope. Preserve the SDK call spelling when the declaration binds.
            if (e.isCxx && e.name.starts_with("_Interlocked"))
            {
                const std::string aliasName = e.name.substr(1);
                auto target = functionTable.find(regName);
                if (target != functionTable.end())
                {
                    auto& aliases = functionTable[aliasName];
                    for (const FunctionSymbol& sym : target->second)
                    {
                        if (!sym.IsCInteropDeclaration || sym.UniqueName != linkageName) continue;
                        const bool already = std::any_of(aliases.begin(), aliases.end(),
                            [&](const FunctionSymbol& old) {
                                return old.UniqueName == sym.UniqueName
                                    && old.Parameters.size() == sym.Parameters.size();
                            });
                        if (already) continue;
                        FunctionSymbol alias = sym;
                        alias.IsCInteropAlias = true;
                        aliases.push_back(std::move(alias));
                    }
                }
            }

            if (!freeOperatorLookupName.empty())
            {
                NoteCxxForeignNamespace(freeOperatorLookupName);
                for (size_t pos = 0; (pos = freeOperatorLookupName.find('.', pos)) != std::string::npos; ++pos)
                    RegisterNamespace(freeOperatorLookupName.substr(0, pos));
                auto source = functionTable.find(regName);
                if (source != functionTable.end())
                {
                    auto& aliases = functionTable[freeOperatorLookupName];
                    for (const FunctionSymbol& sym : source->second)
                    {
                        if (!sym.IsCInteropDeclaration || sym.UniqueName != linkageName) continue;
                        bool already = std::any_of(aliases.begin(), aliases.end(), [&](const auto& old) {
                            return old.UniqueName == sym.UniqueName;
                        });
                        if (already) continue;
                        FunctionSymbol alias = sym;
                        alias.SourceName = e.name;
                        alias.IsCInteropAlias = true;
                        aliases.push_back(std::move(alias));
                    }
                }
            }

            // A free C++ function whose omitted suffix contains a non-constant default gets an
            // exact-arity overload backed by a C++ forwarding body. Member defaults stay on the
            // refusal path because a wrapper cannot safely reproduce private member access.
            if (e.isCxx && e.defaultArgs.size() == e.params.size())
            {
                // With clang's own arrangement in hand a record is fine on either side: the
                // wrapper has the SAME return type and a PREFIX of the parameter types, and every
                // ABI cflat targets classifies each argument independently, so the plan's slots
                // truncate exactly. Without a plan, keep the old scalar/pointer-only rule.
                const bool havePlan = e.abi.valid && e.abi.params.size() == e.params.size();
                const auto canWrap = [&](const TypeAndValue& t) {
                    return havePlan || t.Pointer || !dataStructures.count(t.TypeName);
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
                        cflat_cinterop::RawAbi wrapperPlan;
                        if (havePlan)
                        {
                            wrapperPlan = e.abi;
                            wrapperPlan.params.resize(n);
                            // The cross-check text describes the FULL arity; the truncated plan
                            // has no matching text, so the slot-by-slot build stands on its own.
                            wrapperPlan.fnTypeText.clear();
                        }
                        std::string wrapperMismatch;
                        CInteropDeclarationScope declaringFile(*this,
                            e.file.empty() ? fileForLsp : e.file);
                        {
                            CxxAbiPlanScope wrapperAbi(*this, havePlan ? &wrapperPlan : nullptr,
                                                       &wrapperMismatch);
                            CreateFunctionDeclaration(regName, e.ret, prefix, /*external=*/true,
                                /*varargs=*/false, /*returnsOwned=*/false, /*isMethod=*/false,
                                CallingConv::Cdecl, wrapper, /*isCxx=*/true, e.isNoexcept);
                        }
                        if (!wrapperMismatch.empty())
                        {
                            if (verbose)
                                std::cout << std::format(
                                    "[verbose]   C++ default wrapper {} not bound: {}\n",
                                    wrapper, wrapperMismatch);
                            continue;
                        }
                        if (auto wit = functionTable.find(regName); wit != functionTable.end())
                            for (FunctionSymbol& sym : wit->second)
                                if (sym.External && sym.UniqueName == wrapper)
                                {
                                    sym.IsCInteropDeclaration = true;
                                    sym.DefaultArguments.clear();
                                }
                    }
            }

            if (!boundKey.empty()) cxxBoundSignatureKeys_.insert(std::move(boundKey));
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
        if (targetMacOS_) return ("arm64-apple-macosx" + cflat::platform::MacDeploymentVersion());
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
            const std::string& sdk = MacSdkPathCached();
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
        // Header extraction must see the same language level the companion module and the
        // request TUs are compiled with (C++20 requires-clauses in libtorch).
        if (asCxx) args.push_back("-std=" + cppStandard_);
        // The companion module must agree with the /MD objects CompileCFile builds from imported
        // .cpp files: clang-cl's /MT default embeds a RuntimeLibrary FAILIFMISMATCH against them.
        if (asCxx && targetWindows_) args.push_back("-fms-runtime-lib=dll");
        if (errorRecovery)
        {
            args.push_back("-ferror-limit=0");
            args.push_back("-Wno-everything");
            // Typo correction would silently resolve an unknown request spelling to a similar
            // real class (std::make_shared -> std::make_signed) and bind that instead.
            args.push_back("-fno-spell-checking");
        }
        if (!headerDir.empty()) args.push_back("-I" + headerDir);
        for (const auto& inc : cIncludeDirs_) args.push_back("-I" + inc);
        for (const auto& def : cDefines_)      args.push_back("-D" + def);
        for (const auto& def : extraDefines)   args.push_back("-D" + def);
        return args;
    }





// `T *const &` - const at the POINTER level under one lvalue reference. A distinct overload
// from `T *&`, and the only one of the pair a pointer rvalue can bind.

/*
 * A C++ REFERENCE data member (`int& r;`, `const int& r;`, `int&& r;`, `int*& p;`) keeps the
 * pointer-sized slot the layout needs, and is marked as an alias of the referent: the same
 * Pointer + IsAlias shape a `T&` parameter has. The field access then reads through the slot.
 */
void LLVMBackend::MarkCxxReferenceField(const std::string& ctype, TypeAndValue& tv, bool isCxx)
{
        std::string s = ctype;
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
        if (isCxx && CxxSpellingHasVolatileTarget(s)) tv.IsCxxPointeeVolatile = true;
        const size_t arrayRefParen = s.find("(&)");
        const size_t arrayRefOpen = s.find("(&)[");
        const size_t arrayRefAt = arrayRefOpen != std::string::npos
            ? arrayRefOpen : arrayRefParen;
        if (isCxx && arrayRefAt != std::string::npos
            && s.substr(0, arrayRefAt).find("const") != std::string::npos)
            tv.SetPointeeConstBit(true);
        if (tv.IsArrayView)
        {
            return;
        }
        if (arrayRefAt != std::string::npos) return;
        if (s.empty() || s.back() != '&' || !tv.Pointer || tv.IsFunctionPointer || tv.IsArrayView)
            return;
        const bool rvalue = s.size() >= 2 && s[s.size() - 2] == '&';
        // The referent's OUTER depth, from the mapper (it skips template arguments): a class
        // referent such as `Box<int *>` is depth 0 and `C *` is 1; the slot's own ElemPointer is not it.
        const std::string referent = s.substr(0, s.size() - (rvalue ? 2 : 1));
        TypeAndValue referentTv;
        const bool mapped = MapCTypeToTypeAndValue(referent, referentTv, isCxx)
            && !referentTv.IsFunctionPointer;
        tv.PointerDepth = mapped ? referentTv.ValuePointerDepth() : 0;
        tv.IsCxxRefToPointer = tv.PointerDepth > 0;
        tv.IsAlias = true;
        tv.MarkCxxRef();
        tv.SetCxxConstRef(rvalue ? CxxRvalueReferenceIsConst(s)
            : CxxParamIsConstLvalueReference(s)
            || (tv.IsCxxRefToPointer && CxxParamIsConstRefToPointer(s)));
}


/*
 * True when a C++ pointer spelling has a const INNERMOST pointee: `const T *`, `T const *&`,
 * `const T *const &`, `const T *const *`. Template arguments and function-pointer spellings are
 * not looked into; `T *const &` (const pointer slot only) is false.
 */





bool LLVMBackend::MapRawSig(const cflat_cinterop::RawSig& r, CSigEntry& e)
{
        e = CSigEntry();
        e.paramSpellings = r.paramTypes;
        e.paramTemporarySpellings = r.paramTemporaryTypes;
        e.resultBorrowsBraceList = r.resultBorrowsBraceList;
        e.braceListElementSpellings = r.braceListElementTypes;
        e.calleeIdentity = r.calleeIdentity;
        e.ctorPick = r.ctorPick;
        e.paramBindKinds = r.paramBindKinds;
        e.conditionalArgumentKinds = r.conditionalArgumentKinds;
        e.closureKeptParams = r.closureKeptParams;
        e.selectedCalleeType = r.selectedCalleeType;
        e.selectedCalleeTemplateArgs = r.selectedCalleeTemplateArgs;
        e.selectedCalleeDefaults = r.selectedCalleeDefaults;
        e.retSpelling = r.retType;
        e.defaultArgs = r.defaultArgs;
        e.name     = r.name;
        e.linkageName = r.linkageName;
        e.isCxx = r.isCxx;
        e.isInline = r.isInline;
        e.isStaticInline = r.isStaticInline;
        e.isDeleted = r.isDeleted;
        e.isTemplateSpecialization = r.isTemplateSpecialization;
        e.abi   = r.abi;
        e.isNoexcept = r.isNoexcept;
        e.paramNames = r.paramNames;
        e.bindRefusal = r.bindRefusal;
        e.sourceBindRefusal = r.bindRefusal;
        e.variadic = r.variadic;
        e.file     = r.file;
        e.line     = r.line ? r.line : 1;
        e.col      = r.col < 0 ? 0 : r.col;
        // A returned `T (&)[N]` (a forwarded literal) is the array's address: a `T *` value.
        std::vector<uint64_t> retArrayDims;
        const std::string retMapSpelling = r.isCxx && (r.retType.find("(&)[") != std::string::npos
                                                       || r.retType.find("(&&)[") != std::string::npos)
            ? StripFixedArrayDims(r.retType, retArrayDims) : r.retType;
        bool retMapped = MapCTypeToTypeAndValue(retMapSpelling, e.ret, r.isCxx);
        // A std::function return is a placeholder here for the same reason a std::function
        // PARAMETER is below: RegisterCSignatures retypes it once the specialization exists.
        if (!retMapped && r.isCxx && IsTopLevelStdFunctionSpelling(r.retType))
        {
            e.ret = TypeAndValue{};
            e.ret.TypeName = "void";
            e.ret.Pointer = true;
            retMapped = true;
        }
        if (!retMapped)
        {
            if (r.isCxx)
                e.bindRefusal = IsLongDoubleSpelling(r.retType) && !IsCInteropLongDoubleSupported()
                    ? std::format("'{}' was not bound: {}", r.name, CInteropLongDoubleRefusal())
                    : std::format("'{}' was not bound: return type '{}' is unsupported",
                                  r.name, r.retType);
            if (verbose) std::cout << std::format("[verbose]   skipping '{}': unsupported return type '{}'\n", r.name, r.retType);
            return r.isCxx;
        }
        if (!r.isCxx && e.ret.Pointer && !e.ret.IsFunctionPointer
            && e.ret.TypeName == "void" && e.ret.ValuePointerDepth() == 1)
        {
            std::unordered_set<std::string> seenTypedefs;
            std::function<std::string(const std::string&)> cRecordPointee =
                [&](const std::string& spelling) -> std::string {
                int ptrLevels = 0;
                const std::string tag = AggregatePointeeTag(spelling, ptrLevels);
                if (ptrLevels == 1)
                    if (auto record = dataStructures.find(tag); record != dataStructures.end()
                        && record->second.StructType != nullptr && !record->second.StructType->isOpaque())
                        return tag;

                std::string base = spelling;
                for (const char* word : { "const", "volatile", "restrict", "__restrict",
                                           "__restrict__", "_Nonnull", "_Nullable",
                                           "_Null_unspecified" })
                    EraseDeclaratorToken(base, word);
                base.erase(std::remove(base.begin(), base.end(), '*'), base.end());
                size_t first = base.find_first_not_of(" \t");
                size_t last = base.find_last_not_of(" \t");
                if (first == std::string::npos) return {};
                base = base.substr(first, last - first + 1);
                auto alias = cTypedefMap_.find(base);
                if (alias == cTypedefMap_.end() || !seenTypedefs.insert(base).second) return {};
                return cRecordPointee(alias->second + std::string(ptrLevels, '*'));
            };
            const std::string tag = cRecordPointee(r.retType);
            if (!tag.empty()) e.ret.TypeName = tag;
        }
        if (r.isCxx && e.ret.IsFunctionPointer && e.ret.Pointer
            && (e.ret.IsAlias || e.ret.IsRvalueRef))
        {
            e.bindRefusal = std::format(
                "'{}' was not bound: return type '{}' is an unsupported reference to a function pointer",
                r.name, r.retType);
            return true;
        }
        const bool cxxLvalueReferenceReturn = r.retType.ends_with('&')
            && !r.retType.ends_with("&&");
        const bool cxxScalarRvalueReferenceReturn = r.retType.ends_with("&&")
            && e.ret.Pointer && !e.ret.IsFunctionPointer
            && !IsCxxRecord(e.ret.TypeName)
            && !IsCxxLazyAliasSpecialization(e.ret.TypeName);
        if (r.isCxx && (cxxLvalueReferenceReturn || cxxScalarRvalueReferenceReturn)
            && !e.ret.IsFunctionPointer)
        {
            // A T*& is one CFlat pointer plus the ABI reference slot. T**& already arrives at
            // its CFlat depth with the flag set by the mapper; do not peel either level there.
            if (!e.ret.IsCxxRefToPointer)
            {
                if (e.ret.ElemPointer)
                {
                    e.ret.ElemPointer = false;
                    e.ret.IsCxxRefToPointer = true;
                }
                else
                    e.ret.Pointer = false;
            }
            e.ret.IsAlias = true;
            e.ret.MarkCxxRef();
            if (CxxParamIsConstLvalueReference(r.retType)
                || (cxxScalarRvalueReferenceReturn
                    && CxxRvalueReferenceIsConst(r.retType))
                || (e.ret.IsCxxRefToPointer && CxxParamIsConstRefToPointer(r.retType)))
                e.ret.SetCxxConstRef(true);
        }
        // A class `const T&&` result keeps its pointer shape; the flag tells a move site to copy.
        if (r.isCxx && e.ret.Pointer && e.ret.IsRvalueRef && !e.ret.IsFunctionPointer
            && IsCxxRecord(e.ret.TypeName) && CxxRvalueReferenceIsConst(r.retType))
            e.ret.SetCxxConstRef(true);
        if (r.isCxx && !e.ret.Pointer && !e.ret.IsAlias && !e.ret.IsFunctionPointer
            && IsCxxRecord(e.ret.TypeName) && CxxSpellingHasTopLevelConst(r.retType))
            e.ret.IsCxxConstValueReturn = true;
        if (r.isCxx && e.ret.Pointer && !e.ret.IsFunctionPointer)
            e.ret.ConstMask |= CxxSpellingPointeeConstMask(r.retType, e.ret.ValuePointerDepth());
        // A C result keeps its spelled pointee levels like a C parameter (`const char *` -> 1).
        if (!r.isCxx && e.ret.Pointer && !e.ret.IsFunctionPointer && !e.ret.IsArrayView)
            if (const uint8_t mask = CSpellingPointeeConstMask(r.retType, e.ret.ValuePointerDepth());
                mask != 0 && (mask & (mask + 1u)) == 0)
                e.ret.ConstMask = mask;
        if (r.isCxx && CxxSpellingHasVolatileTarget(r.retType))
            e.ret.IsCxxPointeeVolatile = true;
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
            if (!MapCTypeToTypeAndValue(r.paramTypes[i], ptv, r.isCxx))
            {
                std::string arrayElement;
                uint64_t arrayExtent = 0;
                if (r.isCxx && ParseCxxArrayParameter(r.paramTypes[i], arrayElement, arrayExtent)
                    && MapCTypeToTypeAndValue(arrayElement, ptv, r.isCxx))
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
            // C++ lvalue references are aliases at the CFlat surface. Keep the pointer ABI in
            // the declaration machinery, but mark the parameter so call lowering can distinguish
            // `const T&` from a raw `T*` and materialize a temporary for an implicit conversion.
            std::string arrayElement;
            uint64_t arrayExtent = 0;
            const bool cxxArrayReference = r.isCxx
                && ParseCxxArrayParameter(r.paramTypes[i], arrayElement, arrayExtent);
            if (cxxArrayReference)
            {
                ptv.IsAlias = false;
                ptv.IsCxxRefToPointer = false;
            }
            if (r.isCxx && !cxxArrayReference
                && r.paramTypes[i].find('&') != std::string::npos
                && r.paramTypes[i].find("&&") == std::string::npos
                && !ptv.IsFunctionPointer && ptv.Pointer)
            {
                ptv.IsAlias = true;
                // A T**& already arrives flagged and at its CFlat depth (the mapper could not
                // store the reference as a third level); only a T*& still has one to peel.
                if (ptv.ElemPointer && !ptv.IsCxxRefToPointer)
                {
                    ptv.ElemPointer = false;
                    ptv.IsCxxRefToPointer = true;
                }
                ptv.MarkCxxRef();
                ptv.SetCxxConstRef(CxxParamIsConstLvalueReference(r.paramTypes[i])
                    || (ptv.IsCxxRefToPointer
                        && CxxParamIsConstRefToPointer(r.paramTypes[i])));
            }
            // `const T&&`: its referent is const in ConstMask, as the other C++ reference mappers record.
            else if (r.isCxx && !cxxArrayReference && ptv.IsRvalueRef && ptv.Pointer
                && !ptv.IsFunctionPointer && !ptv.IsCxxRefToPointer
                && CxxRvalueReferenceIsConst(r.paramTypes[i]))
            {
                ptv.MarkCxxRef();
                ptv.SetCxxConstRef(true);
            }
            /*
             * A C parameter keeps the levels it spells (`const int **` is const at the int only);
             * one the level parser cannot read but whose pointee is const is read-only at every
             * level. A const C object may be passed to it but never to a mutable pointer parameter
             * (T183). A C++ parameter's levels come from its C++ spelling.
             */
            if (ptv.Pointer && !ptv.IsFunctionPointer)
            {
                if (r.isCxx)
                    ptv.ConstMask |= CxxSpellingPointeeConstMask(r.paramTypes[i], ptv.ValuePointerDepth());
                else
                {
                    if (!ptv.IsArrayView)
                        ptv.ConstMask = CSpellingPointeeConstMask(r.paramTypes[i], ptv.ValuePointerDepth());
                    if (CxxSpellingHasConstPointee(r.paramTypes[i]))
                    {
                        if (ptv.ConstMask == 0)
                            ptv.ConstMask = static_cast<uint8_t>((1u << ptv.ValuePointerDepth()) - 1u);
                        ptv.SetPointeeConstBit(true);
                    }
                }
            }
            if (r.isCxx && CxxSpellingHasVolatileTarget(r.paramTypes[i]))
                ptv.IsCxxPointeeVolatile = true;
            if (i < r.paramNames.size()) ptv.VariableName = r.paramNames[i];
            e.params.push_back(std::move(ptv));
        }
        return true;
    }

// Cache entries contain mapped CFlat names from their storing analysis; remap each C++ signature
// from its preserved spelling after this backend has registered the requested and nested types.
void LLVMBackend::RebindCxxCachedSignatures(std::vector<CSigEntry>& sigs)
{
        llvm::TimeTraceScope scope("RebindCxxCachedSignatures", std::to_string(sigs.size()));
        for (CSigEntry& cached : sigs)
        {
            if (!cached.isCxx
                || (cached.paramSpellings.empty() && cached.retSpelling.empty()))
            {
                cached.needsCxxRebind = false;
                continue;
            }
            cflat_cinterop::RawSig raw;
            raw.name = cached.name;
            raw.linkageName = cached.linkageName;
            raw.retType = cached.retSpelling;
            raw.paramTypes = cached.paramSpellings;
            raw.paramTemporaryTypes = cached.paramTemporarySpellings;
            raw.resultBorrowsBraceList = cached.resultBorrowsBraceList;
            raw.braceListElementTypes = cached.braceListElementSpellings;
            raw.calleeIdentity = cached.calleeIdentity;
            raw.ctorPick = cached.ctorPick;
            raw.paramBindKinds = cached.paramBindKinds;
            raw.conditionalArgumentKinds = cached.conditionalArgumentKinds;
            raw.closureKeptParams = cached.closureKeptParams;
            raw.selectedCalleeType = cached.selectedCalleeType;
            raw.selectedCalleeTemplateArgs = cached.selectedCalleeTemplateArgs;
            raw.selectedCalleeDefaults = cached.selectedCalleeDefaults;
            raw.paramNames = cached.paramNames;
            raw.defaultArgs = cached.defaultArgs;
            raw.variadic = cached.variadic;
            raw.isCxx = true;
            raw.isDeleted = cached.isDeleted;
            raw.isTemplateSpecialization = cached.isTemplateSpecialization;
            raw.isNoexcept = cached.isNoexcept;
            raw.bindRefusal = cached.sourceBindRefusal;
            raw.abi = cached.abi;
            raw.file = cached.file;
            raw.line = cached.line;
            raw.col = cached.col;
            CSigEntry remapped;
            if (MapRawSig(raw, remapped)) cached = std::move(remapped);
        }
}



// Encode every decorated-name byte so wrapper names can be reversed without collisions.





/*
 * Whether the shortened call a default-argument wrapper makes (the first `kept` parameters of
 * one overload) is ambiguous in C++: another overload of the same name takes the same leading
 * parameter types and defaults all the rest, so both are exact matches. Such a wrapper never
 * compiles; it is dropped up front, as the request-TU path drops a wrapper whose body failed.
 */
template <class Overload, class SameShape>
static bool CxxDefaultWrapperCallAmbiguous(const Overload& self,
                                           const std::vector<const Overload*>& overloads,
                                           size_t kept, SameShape sameShape)
{
        for (const Overload* other : overloads)
        {
            // A redeclaration harvested twice is the same function, not a rival overload.
            if (other == &self || other->linkageName == self.linkageName
                || !sameShape(*other) || other->paramTypes.size() < kept
                || other->defaultArgs.size() != other->paramTypes.size())
                continue;
            bool viable = true;
            for (size_t i = 0; i < kept && viable; ++i)
                viable = other->paramTypes[i] == self.paramTypes[i];
            // An "unsupported" default still exists in C++, so it still makes the call viable.
            for (size_t i = kept; i < other->paramTypes.size() && viable; ++i)
                viable = !other->defaultArgs[i].kind.empty();
            if (viable) return true;
        }
        return false;
}

static std::string BuildCxxDefaultWrappers(
    const std::vector<cflat_cinterop::RawSig>& sigs,
    const std::vector<cflat_cinterop::RawRecord>& records,
    std::vector<std::string>* ambiguousWrappers = nullptr,
    const std::string& requestedWrapper = {})
{
        std::string source;
        bool emitted = false;
        // One definition per wrapper name: a signature harvested twice must not define it twice.
        std::unordered_set<std::string> emittedWrappers;
        auto appendWrapper = [&](const std::string& base,
                                 const std::string& retType,
                                 const std::vector<std::string>& paramTypes,
                                 size_t keptCount,
                                 const std::string& target,
                                 bool instance,
                                 const std::string& memberName,
                                 bool isConst,
                                 bool isNoexcept,
                                 int refQualifier) {
            if (!emittedWrappers.insert(base).second) return;
            if (!emitted)
            {
                source += kCxxWrapperPidDecl;
                emitted = true;
            }
            const std::string cppName = base + "_cpp";
            std::string params;
            std::string args;
            if (instance)
            {
                const std::string receiver = (isConst ? "const " : "") + target + " *";
                params = "typename __cflat_pid<" + receiver + ">::type a0";
            }
            const size_t firstUser = instance ? 1u : 0u;
            for (size_t i = firstUser; i < keptCount; ++i)
            {
                if (!params.empty()) params += ", ";
                if (!args.empty()) args += ", ";
                const std::string& type = paramTypes[i];
                params += "typename __cflat_pid<" + type + ">::type a" + std::to_string(i);
                // A by-value parameter is the wrapper's own copy: move it on, so a move-only
                // type (std::unique_ptr) forwards as C++'s own shortened call would.
                args += type.ends_with("&&")
                    ? "static_cast<" + type + ">(a" + std::to_string(i) + ")"
                    : !type.ends_with("&")
                        ? "static_cast<typename __cflat_pid<" + type + ">::type&&>(a"
                            + std::to_string(i) + ")"
                        : "a" + std::to_string(i);
            }
            const std::string receiver = instance
                ? (refQualifier == cflat_cinterop::CxxRefQualifierRValue
                    ? std::string("static_cast<") + (isConst ? "const " : "")
                        + target + "&&>(*a0)"
                    : "(*a0)")
                : std::string{};
            const std::string call = instance
                ? receiver + "." + memberName + "(" + args + ")"
                : target + "(" + args + ")";
            const std::string weakArgs = instance
                ? (std::string("a0") + (args.empty() ? std::string{} : ", " + args))
                : args;
            const std::string suffix = isNoexcept ? " noexcept" : "";
            source += "extern \"C++\" { static " + retType + " " + cppName + "("
                + params + ")" + suffix + " { ";
            if (retType == "void") source += call + ";";
            else source += "return " + call + ";";
            source += " } }\n";
            source += "extern \"C\" __attribute__((weak)) " + retType + " " + base + "("
                + params + ")" + suffix + " { ";
            if (retType == "void") source += cppName + "(" + weakArgs + ");";
            else source += "return " + cppName + "(" + weakArgs + ");";
            source += " }\n";
        };
        std::unordered_map<std::string, std::vector<const cflat_cinterop::RawSig*>> freeOverloads;
        for (const auto& sig : sigs)
            if (sig.isCxx && !sig.variadic)
                freeOverloads[sig.qualifiedName.empty() ? sig.name : sig.qualifiedName]
                    .push_back(&sig);
        for (const auto& sig : sigs)
        {
            if (!sig.isCxx || sig.variadic || sig.linkageName.empty()
                || sig.defaultArgs.size() != sig.paramTypes.size())
                continue;
            const std::string target = CxxNameFromCflat(
                sig.qualifiedName.empty() ? sig.name : sig.qualifiedName);
            const auto& overloads =
                freeOverloads[sig.qualifiedName.empty() ? sig.name : sig.qualifiedName];
            for (size_t n = 0; n < sig.paramTypes.size(); ++n)
            {
                if (!HasNonConstDefaultSuffix(sig.defaultArgs, n)) continue;
                const std::string base = CxxDefaultWrapperName(sig.linkageName, n);
                if (!requestedWrapper.empty() && base != requestedWrapper) continue;
                if (CxxDefaultWrapperCallAmbiguous(sig, overloads, n,
                                                   [](const cflat_cinterop::RawSig&) { return true; }))
                {
                    if (ambiguousWrappers != nullptr) ambiguousWrappers->push_back(base);
                    continue;
                }
                appendWrapper(base, sig.retType, sig.paramTypes, n, target,
                              /*instance*/ false, {}, false, sig.isNoexcept,
                              cflat_cinterop::CxxRefQualifierNone);
            }
        }
        for (const auto& record : records)
        {
            const std::string owner = CxxRecordOwnerSpelling(record);
            if (owner == "::" || record.name.empty()) continue;
            std::unordered_map<std::string, std::vector<const cflat_cinterop::RawCxxMember*>>
                memberOverloads;
            for (const auto& member : record.members)
                if (member.kind == cflat_cinterop::RawCxxMember::Instance
                    || member.kind == cflat_cinterop::RawCxxMember::StaticMethod)
                    memberOverloads[member.name].push_back(&member);
            for (const auto& member : record.members)
            {
                if ((member.kind != cflat_cinterop::RawCxxMember::Instance
                        && member.kind != cflat_cinterop::RawCxxMember::StaticMethod)
                    || member.access != cflat_cinterop::AccessPublic
                    || member.linkageName.empty()
                    || member.defaultArgs.size() != member.paramTypes.size())
                    continue;
                const bool instance = member.kind == cflat_cinterop::RawCxxMember::Instance;
                const std::string target = instance ? owner : owner + "::" + member.name;
                for (size_t n = 0; n < member.paramTypes.size(); ++n)
                {
                    if (member.defaultArgs[n].kind == "unsupported"
                        || !HasNonConstDefaultSuffix(member.defaultArgs, n))
                        continue;
                    if (!requestedWrapper.empty()
                        && CxxDefaultWrapperName(member.linkageName, n) != requestedWrapper) continue;
                    // The receiver is part of the call: a const / ref-qualifier split decides it.
                    if (CxxDefaultWrapperCallAmbiguous(member, memberOverloads[member.name], n,
                            [&](const cflat_cinterop::RawCxxMember& other) {
                                return other.kind == member.kind
                                    && other.isConst == member.isConst
                                    && other.refQualifier == member.refQualifier;
                            }))
                    {
                        if (ambiguousWrappers != nullptr)
                            ambiguousWrappers->push_back(
                                CxxDefaultWrapperName(member.linkageName, n));
                        continue;
                    }
                    appendWrapper(CxxDefaultWrapperName(member.linkageName, n), member.retType,
                                  member.paramTypes, n, target, instance, member.name,
                                  member.isConst, member.isNoexcept, member.refQualifier);
                }
            }
        }
        return source;
}

void LLVMBackend::RememberCxxDefaultWrapper(const std::string& owner,
                                            const std::string& linkage, size_t arity,
                                            bool member)
{
        const std::string name = CxxDefaultWrapperName(linkage, arity);
        if (cxxDefaultWrapperRequests_.count(name)) return;
        CxxDefaultWrapperRequest request;
        request.owner = owner;
        request.linkage = linkage;
        request.arity = arity;
        request.member = member;
        if (activeCxxRequestGroup_ != nullptr)
            request.group = *activeCxxRequestGroup_;
        else
        {
            size_t primary = static_cast<size_t>(-1);
            if (member)
                GetCxxTypeOwnerGroup(owner, primary);
            else if (auto found = cxxFunctionOwnerGroup_.find(owner);
                     found != cxxFunctionOwnerGroup_.end())
                primary = found->second;
            if (primary < cxxImportGroups_.size())
                request.group = MakeCxxRequestGroup(primary, {});
        }
        cxxDefaultWrapperRequests_.emplace(name, std::move(request));
}

bool LLVMBackend::EnsureCxxDefaultWrapper(const std::string& name)
{
        auto found = cxxDefaultWrapperRequests_.find(name);
        if (found == cxxDefaultWrapperRequests_.end()) return true;
        if (found->second.result != 0) return found->second.result > 0;
        const CxxDefaultWrapperRequest request = found->second;
        std::vector<cflat_cinterop::RawSig> sigs;
        std::vector<cflat_cinterop::RawRecord> records;
        if (request.member)
        {
            if (auto record = cxxRecordEntries_.find(request.owner);
                record != cxxRecordEntries_.end())
                records.push_back(RawRecordFromCxxCache(record->second));
        }
        else if (auto signatures = cxxFunctionSignatures_.find(request.owner);
                 signatures != cxxFunctionSignatures_.end())
            for (const CSigEntry& cached : signatures->second)
            {
                cflat_cinterop::RawSig sig;
                sig.name = sig.qualifiedName = cached.name;
                sig.linkageName = cached.linkageName;
                sig.isCxx = cached.isCxx;
                sig.variadic = cached.variadic;
                sig.isNoexcept = cached.isNoexcept;
                sig.retType = cached.retSpelling;
                sig.paramTypes = cached.paramSpellings;
                sig.defaultArgs = cached.defaultArgs;
                sigs.push_back(std::move(sig));
            }
        std::vector<std::string> ambiguous;
        const std::string source = BuildCxxDefaultWrappers(sigs, records, &ambiguous, name);
        CSigEntry ignored;
        std::string error;
        CxxExtractionStageTimer timer(verbose, "default-wrapper demand parse/cache");
        const bool canRequest = !source.empty() && ambiguous.empty()
            && request.group.primary < cxxImportGroups_.size();
        // Class defaults built in place by an initializer: the wrapper body is never needed.
        if (canRequest && !request.forceWrapper && TryCxxDirectDefaultInit(name, request, sigs, records))
        {
            auto& direct = cxxDefaultWrapperRequests_.at(name);
            direct.result = 1;
            direct.direct = true;
            return true;
        }
        const bool ok = canRequest
            && RequestGeneratedCxxWrapper(request.group, source, name, "DEFAULT", ignored, error);
        auto& result = cxxDefaultWrapperRequests_.at(name);
        result.result = ok ? 1 : -1;
        if (!ok)
        {
            const std::string cause = canRequest ? CxxFirstDiagnosticLine(lastCxxWrapperCause_)
                                                 : std::string();
            result.ambiguous = !ambiguous.empty();
            result.error = result.ambiguous ? std::string() : !cause.empty() ? cause : error;
        }
        return ok;
}

void LLVMBackend::PrepareCxxDefaultCandidates(std::vector<FunctionSymbol>& candidates,
                                              size_t arity)
{
        // Requests can register signature types, so callers pass a stable candidate snapshot.
        for (auto& candidate : candidates)
        {
            if (!candidate.IsCxx || candidate.Variadic) continue;
            const bool wrapper = candidate.UniqueName.starts_with("__cflat_dflt_");
            if (wrapper ? candidate.Parameters.size() != arity
                        : candidate.Parameters.size() <= arity)
                continue;
            const std::string name = wrapper ? candidate.UniqueName
                : CxxDefaultWrapperName(candidate.UniqueName, arity);
            // Lowering failure does not remove the C++ declaration from overload ranking.
            EnsureCxxDefaultWrapper(name);
        }
}

// One initializer per C++ declaration: `void <name>(int param, void* slot)`.
std::string LLVMBackend::CxxDefaultInitHelperName(const std::string& linkageName)
{
        std::string out = CxxDefaultWrapperName(linkageName, 0);
        out.replace(0, std::strlen("__cflat_dflt_"), "__cflat_dfi_");
        out.erase(out.rfind('_'));
        return out;
}

const LLVMBackend::FunctionSymbol* LLVMBackend::FindFunctionSymbolByUniqueName(
    const std::string& uniqueName) const
{
        for (const auto& entry : functionTable)
            for (const FunctionSymbol& symbol : entry.second)
                if (symbol.UniqueName == uniqueName) return &symbol;
        return nullptr;
}

/*
 * Whether a call through default wrapper `wrapper` (its first `arity` parameters) can call
 * `original` directly: the kept parameters agree, every omitted default is a constant CFlat can
 * spell or a by-value C++ class default with captured text passed in memory, and one is a class.
 */
bool LLVMBackend::CxxDirectDefaultEligible(const FunctionSymbol& wrapper, const FunctionSymbol& original,
                                           size_t arity)
{
        if (!original.IsCxx || original.Variadic || !original.Recipe.hasLowering
            || wrapper.Parameters.size() != arity || original.Parameters.size() <= arity
            || original.DefaultArguments.size() != original.Parameters.size()
            || original.Recipe.paramSlots.size() < original.Parameters.size())
            return false;
        for (size_t i = 0; i < arity; ++i)
        {
            const TypeAndValue& a = wrapper.Parameters[i];
            const TypeAndValue& b = original.Parameters[i];
            if (ResolveTypeAlias(a.TypeName) != ResolveTypeAlias(b.TypeName) || a.Pointer != b.Pointer
                || a.PointerDepth != b.PointerDepth || a.IsAlias != b.IsAlias || a.IsRvalueRef != b.IsRvalueRef)
                return false;
        }
        bool classDefault = false;
        for (size_t i = arity; i < original.Parameters.size(); ++i)
        {
            const TypeAndValue& param = original.Parameters[i];
            const cflat_cinterop::RawDefaultArg& def = original.DefaultArguments[i];
            if (param.IsAlias || param.IsRvalueRef || param.IsCxxRef()) return false;
            if (def.kind != "nonconst")
            {
                if (def.kind.empty() || def.kind == "unsupported"
                    || MaterializeCxxDefaultArgument(def, param) == nullptr)
                    return false;
                continue;
            }
            if (param.Pointer || param.PointerDepth != 0 || !IsForeignNontrivialCxxClass(param.TypeName)
                || original.Recipe.paramSlots[i].kind != AbiSlot::ByVal
                || original.Recipe.paramSlots[i].structTy == nullptr
                || def.value.find('\x1f') == std::string::npos)
                return false;
            classDefault = true;
        }
        return classDefault;
}

/*
 * Compiles `original`'s class-default initializer: each default's text is re-read in its own
 * namespace (a member's inside a class derived from the owner, so the owner's static members
 * and nested types resolve) and copy-initializes the caller's parameter slot. The prvalue of
 * the inner function is the slot object itself (guaranteed elision), as C++'s own call does.
 */
bool LLVMBackend::TryCxxDirectDefaultInit(const std::string& wrapperName,
                                          const CxxDefaultWrapperRequest& request,
                                          const std::vector<cflat_cinterop::RawSig>& sigs,
                                          const std::vector<cflat_cinterop::RawRecord>& records)
{
        const FunctionSymbol* wrapper = FindFunctionSymbolByUniqueName(wrapperName);
        const FunctionSymbol* original = FindFunctionSymbolByUniqueName(request.linkage);
        if (wrapper == nullptr || original == nullptr
            || !CxxDirectDefaultEligible(*wrapper, *original, request.arity))
            return false;
        const std::vector<std::string>* spellings = nullptr;
        std::string owner;
        if (!request.member)
        {
            for (const auto& sig : sigs)
                if (sig.linkageName == request.linkage) spellings = &sig.paramTypes;
        }
        else if (!records.empty())
        {
            const auto& record = records.front();
            owner = CxxRecordOwnerSpelling(record);
            for (const auto& member : record.members)
                if (member.linkageName == request.linkage) spellings = &member.paramTypes;
        }
        if (spellings == nullptr) return false;
        return EnsureCxxDefaultInitHelper(request.linkage, original->DefaultArguments, *spellings,
                                          request.member, owner, request.group);
}

bool LLVMBackend::EnsureCxxDefaultInitHelper(const std::string& linkage,
                                             const std::vector<cflat_cinterop::RawDefaultArg>& defaults,
                                             const std::vector<std::string>& spellings, bool member,
                                             const std::string& owner, const CxxRequestGroup& group)
{
        if (auto known = cxxDefaultInitHelpers_.find(linkage); known != cxxDefaultInitHelpers_.end())
            return known->second > 0;
        cxxDefaultInitHelpers_[linkage] = -1;
        if (spellings.size() != defaults.size() || (member && owner.size() <= 2)
            || group.primary >= cxxImportGroups_.size())
            return false;
        const std::string helper = CxxDefaultInitHelperName(linkage);
        std::string source = "#include <new>\n";
        std::string cases;
        for (size_t i = 0; i < defaults.size(); ++i)
        {
            const cflat_cinterop::RawDefaultArg& def = defaults[i];
            const size_t split = def.value.find('\x1f');
            if (def.kind != "nonconst" || split == std::string::npos) continue;
            const std::string scope = def.value.substr(0, split);
            const std::string text = def.value.substr(split + 1);
            const std::string stem = helper + "_" + std::to_string(i);
            source += "using " + stem + "_t = " + spellings[i] + ";\n";
            if (!scope.empty()) source += "namespace " + scope + " { ";
            std::string make;
            if (member)
            {
                // Only reserved names: a plain member name would hide the owner's (R1: `make()`).
                source += "struct " + stem + "_s : " + owner + " { static ::" + stem + "_t " + stem
                    + "_make() { return " + text + "; } };";
                make = stem + "_s::" + stem + "_make()";
            }
            else
            {
                source += "static ::" + stem + "_t " + stem + "_make() { return " + text + "; }";
                make = stem + "_make()";
            }
            if (!scope.empty()) source += " }";
            source += "\n";
            cases += "case " + std::to_string(i) + ": ::new (slot) ::" + stem + "_t("
                + (scope.empty() ? "::" : "::" + scope + "::") + make + "); break; ";
        }
        if (cases.empty()) return false;
        source += "extern \"C\" __attribute__((weak)) void " + helper + "(int param, void* slot) { switch (param) { "
            + cases + "default: break; } }\n";
        CSigEntry ignored;
        std::string error;
        CxxRequestGroupScope groupScope(*this, &group);
        if (!RequestGeneratedCxxWrapper(group, source, helper, "DEFAULT", ignored, error))
            return false;
        cxxDefaultInitHelpers_[linkage] = 1;
        return true;
}

/*
 * Whether a constructor call omitting `st`'s parameters from `first` on builds its class
 * defaults in place (EmitCxxStructorCall) instead of through clang's constructor wrapper.
 */
bool LLVMBackend::CxxCtorDefaultsDirect(const std::string& typeName, const CxxClassInfo::Structor& st,
                                        size_t first)
{
        if (st.linkageName.empty() || st.isDeleted || st.defaultArgs.size() != st.params.size()
            || first == 0 || first >= st.params.size())
            return false;
        bool classDefault = false;
        for (size_t i = first; i < st.params.size(); ++i)
        {
            const TypeAndValue& param = st.params[i];
            const cflat_cinterop::RawDefaultArg& def = st.defaultArgs[i];
            if (param.IsAlias || param.IsRvalueRef || param.IsCxxRef()) return false;
            if (def.kind != "nonconst")
            {
                if (def.kind.empty() || def.kind == "unsupported"
                    || MaterializeCxxDefaultArgument(def, param) == nullptr)
                    return false;
                continue;
            }
            if (param.Pointer || param.PointerDepth != 0 || !IsForeignNontrivialCxxClass(param.TypeName)
                || def.value.find('\x1f') == std::string::npos)
                return false;
            classDefault = true;
        }
        if (!classDefault) return false;
        // Built in memory the callee is handed: a class passed in registers (MS x64) keeps clang's wrapper.
        AbiRecipe recipe;
        if (!BuildAbiRecipeFromClangPlan(typeName, st.abi, st.ret, st.params, recipe)
            || recipe.paramSlots.size() < st.params.size())
            return false;
        for (size_t i = first; i < st.params.size(); ++i)
            if (st.defaultArgs[i].kind == "nonconst"
                && (recipe.paramSlots[i].kind != AbiSlot::ByVal || recipe.paramSlots[i].structTy == nullptr))
                return false;
        if (auto known = cxxDefaultInitHelpers_.find(st.linkageName); known != cxxDefaultInitHelpers_.end())
            return known->second > 0;
        auto record = cxxRecordEntries_.find(typeName);
        size_t primary = static_cast<size_t>(-1);
        if (record == cxxRecordEntries_.end() || !GetCxxTypeOwnerGroup(typeName, primary)
            || primary >= cxxImportGroups_.size())
            return false;
        const std::string owner = CxxRecordOwnerSpelling(record->second);
        for (const auto& member : record->second.members)
            if (member.kind == cflat_cinterop::RawCxxMember::Constructor
                && member.linkageName == st.linkageName)
            {
                const CxxRequestGroup group = MakeCxxRequestGroup(primary, {});
                return EnsureCxxDefaultInitHelper(st.linkageName, st.defaultArgs, member.paramTypes,
                                                  /*member*/ true, owner, group);
            }
        return false;
}

void LLVMBackend::SwitchCxxDefaultWrapperToDirect(FunctionSymbol& candidate,
                                                  std::vector<NamedVariable>& matched)
{
        auto request = cxxDefaultWrapperRequests_.find(candidate.UniqueName);
        if (request == cxxDefaultWrapperRequests_.end() || !request->second.direct) return;
        const FunctionSymbol* found = FindFunctionSymbolByUniqueName(request->second.linkage);
        if (found == nullptr || matched.size() != request->second.arity
            || !CxxDirectDefaultEligible(candidate, *found, request->second.arity))
        {
            // Not this call's shape after all: the forwarding wrapper's body is requested now.
            request->second.direct = false;
            request->second.forceWrapper = true;
            request->second.result = 0;
            EnsureCxxDefaultWrapper(candidate.UniqueName);
            return;
        }
        FunctionSymbol original = *found;
        for (size_t i = matched.size(); i < original.Parameters.size(); ++i)
        {
            const TypeAndValue& param = original.Parameters[i];
            const cflat_cinterop::RawDefaultArg& def = original.DefaultArguments[i];
            NamedVariable value;
            value.TypeAndValue = param;
            value.IsRvalue = true;
            if (def.kind == "nonconst")
            {
                const AbiSlot& slot = original.Recipe.paramSlots[i];
                value.Storage = AllocaAtEntry(slot.structTy, nullptr, "cxx.default.arg", slot.align);
                value.BaseType = slot.structTy;
                value.Primary = llvm::PoisonValue::get(slot.structTy);
                value.IsCxxPrvalueTemp = true;
                value.CxxDefaultInitIndex = static_cast<int>(i);
            }
            else
            {
                value.BaseType = GetType(param);
                value.Primary = MaterializeCxxDefaultArgument(def, param);
            }
            matched.push_back(std::move(value));
        }
        candidate = std::move(original);
}

void LLVMBackend::RejectFailedCxxDefaultCandidate(const FunctionSymbol& candidate, size_t arity)
{
        if (!candidate.IsCxx || candidate.Variadic) return;
        const bool wrapper = candidate.UniqueName.starts_with("__cflat_dflt_");
        if (!wrapper && candidate.Parameters.size() <= arity) return;
        const std::string name = wrapper ? candidate.UniqueName
            : CxxDefaultWrapperName(candidate.UniqueName, arity);
        auto request = cxxDefaultWrapperRequests_.find(name);
        if (request == cxxDefaultWrapperRequests_.end() || request->second.result >= 0) return;
        // CFlat's own finding, not clang text: no "clang: " prefix.
        if (request->second.ambiguous)
        {
            LogErrorMessage("ambiguous call to '{}': without its default arguments the call matches more "
                            "than one C++ overload. Pass the defaulted arguments to pick one.",
                            { request->second.owner });
            return;
        }
        if (!request->second.error.empty())
            LogError("clang: " + request->second.error);
        LogError("cannot lower C++ default arguments for '" + request->second.owner + "'");
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
        if (IsTopLevelStdFunctionSpelling(spelling) || CxxSpellingHasTopLevelParen(spelling))
            return 0;
        named = spelling;
        while (!named.empty() && std::isspace((unsigned char)named.back())) named.pop_back();
        while (!named.empty() && (named.back() == '&' || named.back() == '*'))
        {
            named.pop_back();
            while (!named.empty() && std::isspace((unsigned char)named.back())) named.pop_back();
        }
        while (named.starts_with("const ")) named.erase(0, 6);
        while (named.starts_with("volatile ")) named.erase(0, 9);
        const std::string foreignLookupSpelling = named;
        if (CxxSpellingIsBareNullptrT(named)) return 0;   // a builtin: the mapper answers it
        named = CanonicalizeCxxUniquePtrSpelling(named);
        if (named.find("::") == std::string::npos)
        {
            const size_t globalTemplateOpen = named.find('<');
            if (globalTemplateOpen == std::string::npos
                || !HasCxxClassTemplate(named.substr(0, globalTemplateOpen)))
                return 0;
            identity = CanonicalCxxForeignIdentity(named);
            return identity.empty() ? 3 : 2;
        }
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
            if (TryMapCxxForeignSpelling(foreignLookupSpelling, mapped, mappedForeign)
                && mappedForeign) return 0;
            identity = CanonicalCxxForeignIdentity(named);
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
        TypeAndValue mapped;
        bool mappedForeign = false;
        if (TryMapCxxForeignSpelling(foreignLookupSpelling, mapped, mappedForeign)
            && mappedForeign)
        {
            identity = mapped.TypeName;
            return 2;
        }
        identity = CanonicalCxxForeignIdentity(named);
        return identity.empty() ? 3 : 2;
}

bool LLVMBackend::RequestCxxSignatureTypes(const cflat_cinterop::RawSig& sig,
                                           const std::unordered_set<std::string>* localEnums)
{
        if (!sig.isCxx) return true;
        bool ok = true;
        auto request = [&](const std::string& spelling) {
            if (spelling.find("__cflat_user::") != std::string::npos
                && GeneratedCxxPrefixForSpelling(spelling).empty())
                return;
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
        std::unordered_set<std::string> spellings;
        std::vector<std::string> orderedSpellings;
        for (const CSigEntry& e : sigs)
        {
            if (!e.isCxx) continue;
            auto addSpelling = [&](const std::string& spelling) {
                if (!spelling.empty() && spellings.insert(spelling).second)
                    orderedSpellings.push_back(spelling);
            };
            addSpelling(e.retSpelling);
            for (const std::string& spelling : e.paramSpellings) addSpelling(spelling);
        }
        for (const std::string& spelling : orderedSpellings)
        {
            cflat_cinterop::RawSig raw;
            raw.isCxx = true;
            raw.retType = spelling;
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
            if (!MapCTypeToTypeAndValue(raw.retType, plan.ret, true)) continue;
            for (const auto& spelling : raw.paramTypes)
            {
                TypeAndValue p;
                if (!MapCTypeToTypeAndValue(spelling, p, true)) { plan.params.clear(); break; }
                // C++ callback references map to CFlat pointers because both are pointer ABI slots.
                plan.params.push_back(std::move(p));
            }
            if (plan.params.size() != raw.paramTypes.size()) continue;
            // A callback plan only needs the by-value ABI of these records (destructor, copy/move),
            // the same slice the by-value signature gate projects; the full ctor set stays lazy.
            if (!plan.ret.Pointer && IsCxxRecord(plan.ret.TypeName))
                EnsureCxxRecordProjected(plan.ret.TypeName, false, true);
            for (const auto& param : plan.params)
                if (!param.Pointer && IsCxxRecord(param.TypeName))
                    EnsureCxxRecordProjected(param.TypeName, false, true);
            // A callback ABI plan describes a function POINTER type, not a declaration a CFlat
            // call site can name, so there is nothing to attach a refusal to. Route the refusal
            // into a local sink: the unsupported plan stays out of the registry and the import
            // survives instead of failing the whole header.
            std::string abiMismatch;
            CxxAbiPlanScope abiPlan(*this, &raw.abi, &abiMismatch);
            if (!BuildAbiRecipeFromClangPlan(raw.signature, raw.abi, plan.ret, plan.params,
                                             plan.recipe))
            {
                if (verbose && !abiMismatch.empty())
                    std::cout << std::format("[verbose]   C++ callback {} not bound: {}\n",
                                             raw.signature, abiMismatch);
                // Record the refusal so a call site that lowers a function pointer of this shape
                // reports it instead of falling back to the heuristic ABI silently.
                cxxFunctionPointerAbiRefusals_.emplace(
                    FunctionPointerAbiKey(plan.ret, plan.params),
                    abiMismatch.empty() ? raw.signature : abiMismatch);
                continue;
            }
            cxxFunctionPointerAbiPlans_.emplace(
                FunctionPointerAbiKey(plan.ret, plan.params), std::move(plan));
        }
    }

bool LLVMBackend::MapRawGlobal(const cflat_cinterop::RawGlobalVar& r, CGlobalEntry& e,
                               bool cxxBoundary)
{
        e = CGlobalEntry();
        e.name = cxxBoundary && !r.qualifiedName.empty() ? r.qualifiedName : r.name;
        e.qualifiedName = r.qualifiedName;
        e.linkageName = r.linkageName;
        e.isConst = r.isConst;
        e.isCompileTimeConstant = r.isCompileTimeConstant;
        e.constantValue = r.constantValue;
        e.isFloatConstant = r.isFloatConstant;
        e.floatValue = r.floatValue;
        e.isCxxConstexpr = r.isCxxConstexpr;
        e.file = r.file;
        e.isInternalLinkage = r.isInternalLinkage;
        e.constInitHash = r.constInitHash;
        e.isDllImport = r.isDllImport;
        e.fromCxxHeader = cxxBoundary;
        e.line = r.line ? r.line : 1;
        e.col  = r.col < 0 ? 0 : r.col;

        std::vector<uint64_t> arrayDims;
        const bool isArray = r.ctype.find('[') != std::string::npos;
        const std::string mappedType = isArray ? StripFixedArrayDims(r.ctype, arrayDims) : r.ctype;
        if (isArray && (mappedType.find('[') != std::string::npos || arrayDims.empty()
                        || std::any_of(arrayDims.begin(), arrayDims.end(),
                                       [](uint64_t dim) { return dim == 0; })))
        {
            if (r.ctype.find("[]") != std::string::npos)
                e.bindRefusal = std::format(
                    "array type '{}' is incomplete and global '{}' cannot be bound",
                    r.ctype, e.name);
            else
                e.bindRefusal = std::format(
                    "array type '{}' has unsupported variable size and global '{}' cannot be bound",
                    r.ctype, e.name);
            return true;
        }
        if (!MapCTypeToTypeAndValue(mappedType, e.type, cxxBoundary))
        {
            if (isArray)
            {
                e.bindRefusal = std::format(
                    "array type '{}' has an unsupported element type and global '{}' cannot be bound",
                    r.ctype, e.name);
                return true;
            }
            if (verbose) std::cout << std::format("[verbose]   skipping global '{}': unsupported type '{}'\n", r.name, r.ctype);
            return false;
        }
        if (isArray)
        {
            e.type.ConstArraySize = arrayDims.front();
            if (arrayDims.size() > 1)
                e.type.ConstInnerDimensions.assign(arrayDims.begin() + 1, arrayDims.end());
        }
        // A C global carries the ConstMask of its native spelling (an array's mask is its element's);
        // only a downward-closed mask is a CFlat spelling (R11), so `int* const` keeps none.
        if (!cxxBoundary && !e.type.IsFunctionPointer && !e.type.IsArrayView)
        {
            const int depth = e.type.ValuePointerDepth();
            const uint8_t mask = static_cast<uint8_t>(CSpellingPointeeConstMask(mappedType, depth)
                | (r.isConst ? (1u << depth) : 0u));
            if (mask != 0 && (mask & (mask + 1u)) == 0) e.type.ConstMask = mask;
        }
        // A C const array, or a C `const int *` global (the pointer is writable, its pointee is
        // read-only), is read-only C storage to the boundary checks.
        if (isArray)
        {
            if (r.isConst) e.type.SetPointeeConstBit(true);
        }
        else if (!cxxBoundary && e.type.Pointer && !e.type.IsFunctionPointer
                 && CxxSpellingHasConstPointee(r.ctype))
        {
            e.type.SetPointeeConstBit(true);
        }
        // A C++ `const int *` global keeps its spelled pointee const, so a ctor wrapper spells the
        // argument as clang sees it (`const int *&` binds only a const pointee).
        if (cxxBoundary && e.type.Pointer && !e.type.IsFunctionPointer)
            e.type.ConstMask |= CxxSpellingPointeeConstMask(r.ctype, e.type.ValuePointerDepth());
        // The global's own const is its object level in ConstMask (C and C++, `T *const` too): the
        // one source the boundary checks read. A header global's levels are exact, never filled.
        if (r.isConst && !e.type.IsArrayView)
            e.type.ConstMask |= static_cast<uint8_t>(1u << (isArray ? 0 : e.type.ValuePointerDepth()));
        e.type.VariableName = r.name;
        if (auto it = cxxBindingRefusals_.find(e.name);
            it != cxxBindingRefusals_.end() && it->second.starts_with("global array '"))
            cxxBindingRefusals_.erase(it);
        return true;
    }

bool LLVMBackend::ClassifyRawMacro(const cflat_cinterop::RawMacro& r, CMacroEntry& e,
                                   bool cxxBoundary)
{
        using K = cflat_cinterop::RawMacro;
        if (r.kind == K::Skip && r.aliasTarget.empty()) return false;
        e = CMacroEntry();
        e.name = r.name; e.file = r.file; e.line = r.line ? r.line : 1; e.col = 0;
        e.isIntegerLiteralZero = r.isIntegerLiteralZero;
        e.isIntegerLiteralToken = r.isIntegerLiteralToken;
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
            if (MapCTypeToTypeAndValue(r.naturalType, tv, cxxBoundary))
            {
                if (tv.IsFunctionPointer) { e.isFuncPtr = true; e.funcPtrTV = std::move(tv); }
                else if (tv.Pointer && !tv.ElemPointer && tv.TypeName == "void") e.isPointer = true;
                // C++ spells a handle macro's type as the record pointer itself ((HKEY)0x80000002 is
                // "HKEY__ *"); the C import sees the same constant as void*, so bind it that way.
                else if (cxxBoundary && tv.Pointer && !tv.ElemPointer && BitfieldStorageBits(tv.TypeName) == 0
                         && tv.TypeName != "float" && tv.TypeName != "double")
                    e.isPointer = true;
                // A scalar cast pointer ((int*)0) is a typed pointer in C++, not its integer
                // value: keep the pointee so the global is int*, as clang types it (T296).
                // A const pointee is not carried (the global would bind mutable int*): stays as master.
                else if (cxxBoundary && tv.Pointer && !tv.ElemPointer && !r.pointeeConst)
                {
                    e.isPointer = true;
                    e.pointeeTypeName = tv.TypeName;
                }
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
            entry.cxxAliasTargetBase = t.cxxAliasTargetBase;
            entry.cxxAliasArgs = t.cxxAliasArgs;
            entry.cxxAliasParamDefaults = t.cxxAliasParamDefaults;
            entry.cxxAliasParams = t.cxxAliasParams;
            entry.cxxAliasParamPackFlags = t.cxxAliasParamPackFlags;
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

void LLVMBackend::RegisterTypeAliasSymbols(const std::vector<CTypeAliasEntry>& aliases,
                                           bool cxxBoundary)
{
        auto cxxIdentity = [&](const CTypeAliasEntry& a, std::string& baseOut,
                               std::vector<std::string>& argsOut, std::string& out) -> bool {
            const std::string s = CanonicalizeCxxUniquePtrSpelling(a.cxxSpecialization);
            const size_t open = s.find('<');
            if (open == std::string::npos || s.empty() || s.back() != '>') return false;
            std::string base = s.substr(0, open);
            while (!base.empty() && std::isspace((unsigned char)base.back())) base.pop_back();
            for (size_t p = 0; (p = base.find("::", p)) != std::string::npos; p += 1)
                base.replace(p, 2, ".");
            std::vector<std::string> args;
            bool hasNonTypeArgument = false;
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
                    {
                        args.push_back(arg);
                        hasNonTypeArgument = true;
                    }
                    else if (const char* integer = CxxCompactIntegerSpellingToCflat(SqueezeCxxSpelling(arg)))
                        args.push_back(integer);
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
            return !hasNonTypeArgument && !out.empty();
        };
        for (const auto& a : aliases)
        {
            // A C++ alias of a specialization is a DECLARATION, not a reason to instantiate the
            // class while importing an umbrella header: an umbrella like torch/torch.h names
            // hundreds of them and each instantiation is a pair of full re-parses. Record the
            // spelling and request it on first use of the CFlat name instead.
            if (!a.cxxSpecialization.empty() && !a.qualifiedName.empty() && !a.isCxxAliasTemplate)
            {
                cxxLazyAliasSpecializations_.emplace(a.qualifiedName, a.cxxSpecialization);
                if (a.name != a.qualifiedName)
                    cxxLazyAliasSpecializations_.emplace(a.name, a.cxxSpecialization);
                if (activeCxxRequestGroup_ != nullptr)
                {
                    cxxTypeOwnerGroup_.emplace(a.qualifiedName, activeCxxRequestGroup_->primary);
                    if (a.name != a.qualifiedName)
                        cxxTypeOwnerGroup_.emplace(a.name, activeCxxRequestGroup_->primary);
                }
                RegisterTypeAliasSymbol(a.qualifiedName, a.cxxSpecialization, a.file, a.line, a.col);
                if (a.name != a.qualifiedName)
                    RegisterTypeAliasSymbol(a.name, a.cxxSpecialization, a.file, a.line, a.col);
                continue;
            }
            // Namespaced/class-nested anonymous typedef: clang spells it by the qualified typedef
            // name, so bind that name and request the record by it on first use.
            if (a.isAnonymousRecord && !a.qualifiedName.empty() && a.qualifiedName != a.name)
            {
                RegisterTypeAliasSymbol(a.qualifiedName, a.qualifiedName, a.file, a.line, a.col, true);
                continue;
            }
            if (!a.target.empty() && a.target != a.name)
            {
                cTypedefMap_.emplace(a.name, a.target);
                if (!a.qualifiedName.empty() && a.qualifiedName != a.name)
                {
                    std::string qualifiedTarget = a.target;
                    TypeAndValue mappedTarget;
                    const bool mapped = MapCTypeToTypeAndValue(a.target, mappedTarget, cxxBoundary);
                    if (mapped)
                    {
                        if (cxxBoundary && !mappedTarget.Pointer && !mappedTarget.ElemPointer
                            && !mappedTarget.IsFunctionPointer
                            && IsCxxRecord(mappedTarget.TypeName)
                            && CxxSpellingHasTopLevelConst(a.target))
                            cxxTopLevelConstTypeAliases_.insert(a.qualifiedName);
                        qualifiedTarget = mappedTarget.TypeName;
                        if (mappedTarget.Pointer) qualifiedTarget += "*";
                        if (mappedTarget.ElemPointer) qualifiedTarget += "*";
                    }
                    cTypedefMap_.emplace(a.qualifiedName, qualifiedTarget);
                    // A function-pointer typedef carries its signature: bind it like `using F = function<...>`.
                    if (mapped && mappedTarget.IsFunctionPointer)
                        RegisterFunctionTypeAlias(a.qualifiedName, mappedTarget);
                    else
                        // `using Name = const char*` keeps its pointee const (a `(const char)*`).
                        RegisterTypeAlias(a.qualifiedName, qualifiedTarget,
                            mapped && cxxBoundary && mappedTarget.Pointer && !mappedTarget.ElemPointer
                                && !mappedTarget.IsFunctionPointer && CxxSpellingHasConstPointee(a.target) ? 1 : 0);
                    if (mapped && mappedTarget.IsCxxNullptrT) cxxNullptrTAliases_.insert(a.qualifiedName);
                }
                // A global C++ typedef of a non-record type binds by its plain name, as clang sees
                // it; mapped on first lookup, since one import names thousands (windows.h).
                else if (cxxBoundary && a.cxxSpecialization.empty() && !a.isCxxAliasTemplate
                         && a.target.find('<') == std::string::npos
                         && !a.target.starts_with("struct ") && !a.target.starts_with("union ")
                         && !a.target.starts_with("class ") && !a.target.starts_with("enum "))
                    cxxLazyGlobalTypedefs_.emplace(a.name, a.target);
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
                // The structured pattern carries the QUALIFIED target base, so prefer it: the
                // printed pattern names a target in the alias's own namespace with no qualifier.
                if (!a.cxxAliasTargetBase.empty()) targetBase = a.cxxAliasTargetBase;
                if (!a.qualifiedName.empty())
                    RegisterGenericBaseAlias(a.qualifiedName, targetBase);
                if (a.name != a.qualifiedName)
                    RegisterGenericBaseAlias(a.name, targetBase);
                if (!a.cxxAliasTargetBase.empty())
                {
                    CxxAliasPattern pattern{ a.cxxAliasTargetBase, a.cxxAliasParams,
                                             a.cxxAliasParamPackFlags, a.cxxAliasParamDefaults,
                                             a.cxxAliasArgs,
                                             a.qualifiedName.empty() ? a.name : a.qualifiedName };
                    if (!a.qualifiedName.empty()) RegisterCxxAliasPattern(a.qualifiedName, pattern);
                    if (a.name != a.qualifiedName) RegisterCxxAliasPattern(a.name, pattern);
                }
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
            {
                // A plain C function-pointer typedef (`typedef R (*F)(...)`) names a function type,
                // bound like `using F = function<...>` so `F f = ...` resolves by name. Eager, C only.
                TypeAndValue fnPtrTarget;
                if (!cxxBoundary && a.target.find("(*)") != std::string::npos
                    && !NativeTypeShadowsFunctionAlias(a.name)
                    && MapCTypeToTypeAndValue(a.target, fnPtrTarget, false)
                    && fnPtrTarget.IsFunctionPointer)
                    RegisterFunctionTypeAlias(a.name, fnPtrTarget);
                RegisterTypeAliasSymbol(a.name, a.target, a.file, a.line, a.col, a.isAnonymousRecord);
            }
        }
    }

bool LLVMBackend::MaterializeCxxGlobalTypedef(const std::string& name)
{
        auto it = cxxLazyGlobalTypedefs_.find(name);
        if (it == cxxLazyGlobalTypedefs_.end()) return false;
        const std::string target = it->second;
        cxxLazyGlobalTypedefs_.erase(it);   // before mapping: the mapper may look the name up again
        // A CFlat type of the same name, or an alias bound since the import, wins.
        if (dataStructures.count(name) != 0 || HasInterface(name) || typeAliases.count(name) != 0
            || functionTypeAliases.count(name) != 0)
            return false;
        TypeAndValue mapped;
        if (!MapCTypeToTypeAndValue(target, mapped, /*cxxBoundary*/ true) || mapped.TypeName.empty())
            return false;
        // Root keys, never the alias frame of the body that happened to look the name up first.
        if (mapped.IsFunctionPointer)
        {
            functionTypeAliases[name] = mapped;
            return true;
        }
        std::string spelled = mapped.TypeName;
        if (mapped.Pointer) spelled += "*";
        if (mapped.ElemPointer) spelled += "*";
        if (spelled == name) return false;
        typeAliases[name] = spelled;
        if (mapped.Pointer && !mapped.ElemPointer && CxxSpellingHasConstPointee(target))
            typeAliasConstMasks[name] = 1;
        if (mapped.IsCxxNullptrT) cxxNullptrTAliases_.insert(name);
        return true;
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
        std::function<std::string(const std::string&, std::unordered_set<std::string>&)> resultRecordTag =
            [&](const std::string& spelling, std::unordered_set<std::string>& seen) -> std::string {
            int pointerDepth = 0;
            const std::string tag = AggregatePointeeTag(spelling, pointerDepth);
            if (pointerDepth != 1) return {};
            if (!tag.empty()) return tag;
            std::string base = spelling;
            for (const char* word : { "const", "volatile", "restrict", "__restrict",
                                       "__restrict__", "_Nonnull", "_Nullable",
                                       "_Null_unspecified" })
                EraseDeclaratorToken(base, word);
            base.erase(std::remove(base.begin(), base.end(), '*'), base.end());
            size_t first = base.find_first_not_of(" \t");
            size_t last = base.find_last_not_of(" \t");
            if (first == std::string::npos) return {};
            base = base.substr(first, last - first + 1);
            if (!seen.insert(base).second) return {};
            auto alias = std::find_if(raw.typedefs.begin(), raw.typedefs.end(), [&](const auto& td) {
                return td.name == base;
            });
            return alias == raw.typedefs.end() ? std::string()
                : resultRecordTag(alias->underlying + "*", seen);
        };
        for (const auto& sig : raw.sigs)
        {
            seed(sig.retType);
            int resultPointerDepth = 0;
            const std::string resultTag = AggregatePointeeTag(sig.retType, resultPointerDepth);
            std::unordered_set<std::string> seenTypedefs;
            // C results only: C++ record pointers bind through their own retyping path.
            const std::string pointeeTag = resultPointerDepth == 1 && !sig.isCxx
                ? (resultTag.empty() ? resultRecordTag(sig.retType, seenTypedefs) : resultTag)
                : std::string();
            if (!pointeeTag.empty())
            {
                auto it = byName.find(pointeeTag);
                if (it != byName.end() && !needed[it->second])
                {
                    needed[it->second] = true;
                    work.push_back(it->second);
                }
            }
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
            rec.specialMembersPending = r.specialMembersPending;
            rec.isTriviallyCopyable = r.isTriviallyCopyable;
            rec.isTriviallyRelocatable = r.isTriviallyRelocatable;
            rec.scopeNames = r.scopeNames;
            rec.sizeBytes = r.sizeBytes; rec.alignBytes = r.alignBytes;
            rec.isTrivial = r.isTrivial;
            rec.line = r.line ? r.line : 1; rec.col = r.col < 0 ? 0 : r.col;
            rec.uuid = r.uuid;
            rec.qualifiedName = r.qualifiedName;
            rec.file = r.file;
            rec.inScope = r.inScope;
            rec.isPolymorphic = r.isPolymorphic; rec.hasBases = r.hasBases;
            rec.vtableDeleteVerdict = r.vtableDeleteVerdict;
            rec.hasVirtualBases = r.hasVirtualBases; rec.isAbstract = r.isAbstract;
            rec.isFinal = r.isFinal;
            rec.hasFriendOperators = r.hasFriendOperators;
            rec.friendNames = r.friendNames;
            rec.bases = r.bases; rec.layoutRefusal = r.layoutRefusal;
            rec.virtualBases = r.virtualBases;
            rec.canonicalCtype = r.canonicalCtype;
            rec.blobIndirections = r.blobIndirections;
            rec.stdOwnedType = r.stdOwnedType;
            rec.hasTrivialDefaultCtor = r.hasTrivialDefaultCtor;
            rec.hasTrivialCopyCtor = r.hasTrivialCopyCtor;
            rec.hasTrivialCopyAssign = r.hasTrivialCopyAssign;
            rec.hasTrivialMoveAssign = r.hasTrivialMoveAssign;
            rec.hasTrivialDtor = r.hasTrivialDtor;
            rec.paramDestroyedInCallee = r.paramDestroyedInCallee;
            rec.hasDeletedDefaultCtor = r.hasDeletedDefaultCtor;
            rec.hasDeletedCopyCtor = r.hasDeletedCopyCtor;
            rec.hasDefaultCtor = r.hasDefaultCtor; rec.hasCopyCtor = r.hasCopyCtor;
            rec.hasCtorTemplate = r.hasCtorTemplate;
            rec.hasAssignTemplate = r.hasAssignTemplate;
            rec.moveAssignBindsConstRef = r.moveAssignBindsConstRef;
            rec.moveAssignConstSourceBindsConstRef = r.moveAssignConstSourceBindsConstRef;
            rec.isAggregate = r.isAggregate;
            rec.members = r.members;
            rec.ctorTemplates = r.ctorTemplates;
            rec.staticVars = r.staticVars;
            for (const auto& f : r.fields)
            {
                CRecordFieldEntry fe;
                fe.name = f.name; fe.ctype = f.ctype;
                fe.access = f.access;
                fe.isBitfield = f.isBitfield; fe.bitWidth = f.bitWidth;
                fe.isPromoted = f.isPromoted;
                fe.isZeroSize = f.isZeroSize;
                fe.isConst = f.isConst;
                fe.isMutable = f.isMutable;
                fe.inherited = f.inherited;
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
        req.verbose            = verbose;
        if (activeCxxRequestGroup_ != nullptr)
            AttachCxxHeaderParseGuard(req, *activeCxxRequestGroup_);

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


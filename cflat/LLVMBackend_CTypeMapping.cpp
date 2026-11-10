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

bool LLVMBackend::IsCxxFixedArrayArgument(const NamedVariable& arg)
{
        const bool arrayBaseType = llvm::isa_and_nonnull<llvm::ArrayType>(arg.BaseType);
        return (arg.TypeAndValue.ConstArraySize != 0
                || !arg.TypeAndValue.ConstInnerDimensions.empty()
                || arrayBaseType)
            && !arg.TypeAndValue.IsArrayView
            && (arrayBaseType || arg.Primary == nullptr || arg.Primary->getType()->isPointerTy());
}

LLVMBackend::TypeAndValue LLVMBackend::CxxFixedArrayDecayType(const NamedVariable& arg)
{
        LLVMBackend::TypeAndValue type = arg.TypeAndValue;
        if (!IsCxxFixedArrayArgument(arg)) return type;
        type.PointerDepth = type.PointerDepth > 0 ? type.PointerDepth + 1
            : (type.Pointer ? 2 : 1);
        type.ElemPointer = type.ElemPointer || type.Pointer;
        type.Pointer = true;
        type.ConstArraySize = 0;
        type.ConstInnerDimensions.clear();
        type.IsArrayView = false;
        return type;
}


// ---- Definitions moved out of LLVMBackend.h (CInterop) ----

// Whitespace-insensitive key for a C++ type spelling: Clang and the C type mapper disagree only
// about spaces inside a template argument list.
// A libc++ / SDK C++ header is included by NAME (`#include <vector>`), so its own internal
// includes resolve through the driver's C++ search path; anything else by absolute path.



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
// Canonical spelling minus a leading record tag, squeezed - the comparison key that decides
// whether two spellings name the same specialization.
std::string LLVMBackend::StripCxxRecordTag(std::string spelling)
{
        for (const char* tag : { "class ", "struct ", "union ", "enum " })
            if (spelling.rfind(tag, 0) == 0) { spelling.erase(0, std::strlen(tag)); break; }
        return SqueezeCxxSpelling(spelling);
}



std::string LLVMBackend::SqueezeCxxSpelling(const std::string& spelling)
{
        if (spelling.find_first_of(" \t") == std::string::npos) return spelling;
        std::string out;
        out.reserve(spelling.size());
        for (char c : spelling) if (c != ' ' && c != '\t') out += c;
        return out;
}

std::string LLVMBackend::CanonicalizeCxxUniquePtrSpelling(const std::string& spelling) const
{
        if (!targetWindows_) return spelling;
        const std::string squeezed = SqueezeCxxSpelling(spelling);
        constexpr std::string_view prefix = "std::unique_ptr<";
        if (!squeezed.starts_with(prefix) || squeezed.back() != '>') return spelling;

        const size_t open = prefix.size() - 1;
        int depth = 0;
        size_t comma = std::string::npos;
        for (size_t i = open + 1; i + 1 < squeezed.size(); ++i)
        {
            if (squeezed[i] == '<') ++depth;
            else if (squeezed[i] == '>') --depth;
            else if (squeezed[i] == ',' && depth == 0)
            {
                if (comma != std::string::npos) return spelling;
                comma = i;
            }
        }
        if (comma == std::string::npos) return spelling;
        const std::string pointee = squeezed.substr(open + 1, comma - open - 1);
        const std::string deleter = squeezed.substr(comma + 1, squeezed.size() - comma - 2);
        const std::string expected = "std::default_delete<" + pointee + ">";
        if (pointee.empty() || deleter != expected) return spelling;
        return "std::unique_ptr<" + squeezed.substr(open + 1, comma - open - 1) + ">";
}

/*
 * Structural test for the standard conversion C++ spells `iterator -> const_iterator`: the SAME
 * class template, whose template arguments agree except that the parameter added `const` to a
 * POINTER argument. `__wrap_iter<int *>` -> `__wrap_iter<const int *>` is the libc++ spelling, but
 * nothing here is keyed on that name - `reverse_iterator` and user templates follow the same rule.
 * Only ADDING const converts; removing it, or a different pointee, is not a conversion.
 * SPELLING ONLY. The argument is passed by reinterpreting its storage as the parameter type, so
 * every caller must also require HasIdenticalCxxRecordLayout - a partial specialization keyed on
 * constness (`template <class P> struct S<const P*>`) can give the two a different layout.
 */
bool LLVMBackend::IsCxxConstAddedPointerSpecialization(const std::string& argSpelling,
                                                        const std::string& paramSpelling) const
{
        // Splits `Name<a, b>` into the squeezed template name and its depth-1 arguments.
        auto split = [](const std::string& spelling, std::string& name,
                        std::vector<std::string>& args) {
            const size_t open = spelling.find('<');
            if (open == std::string::npos || spelling.back() != '>') return false;
            name = SqueezeCxxSpelling(spelling.substr(0, open));
            int depth = 0;
            std::string current;
            for (size_t i = open; i + 1 < spelling.size(); ++i)
            {
                const char c = spelling[i];
                if (c == '<')
                {
                    if (++depth == 1) continue;
                }
                else if (c == '>')
                {
                    --depth;
                }
                else if (c == ',' && depth == 1)
                {
                    args.push_back(current);
                    current.clear();
                    continue;
                }
                current += c;
            }
            args.push_back(current);
            return !name.empty() && depth == 1;
        };

        std::string argName;
        std::string paramName;
        std::vector<std::string> argArgs;
        std::vector<std::string> paramArgs;
        if (!split(argSpelling, argName, argArgs) || !split(paramSpelling, paramName, paramArgs))
            return false;
        if (argName != paramName || argArgs.size() != paramArgs.size()) return false;

        bool anyConstAdded = false;
        for (size_t i = 0; i < argArgs.size(); ++i)
        {
            const std::string from = SqueezeCxxSpelling(argArgs[i]);
            const std::string to = SqueezeCxxSpelling(paramArgs[i]);
            if (from == to) continue;
            // Only a pointer argument may gain const, in either written order.
            if (from.empty() || from.back() != '*') return false;
            if (to == "const" + from) { anyConstAdded = true; continue; }
            if (to == from.substr(0, from.size() - 1) + "const*") { anyConstAdded = true; continue; }
            return false;
        }
        return anyConstAdded;
}

/*
 * Two registered records whose LLVM bodies agree field for field. The const-added conversion
 * lowers as a bitwise reinterpretation of the argument object, which is only sound at identical
 * layout, so refuse whenever either side is unknown or still opaque - a refusal is the behaviour
 * before the conversion existed, while a wrong accept is a silent miscompile.
 */
bool LLVMBackend::HasIdenticalCxxRecordLayout(const std::string& argTypeName,
                                              const std::string& paramTypeName) const
{
        auto argIt = dataStructures.find(argTypeName);
        auto paramIt = dataStructures.find(paramTypeName);
        if (argIt == dataStructures.end() || paramIt == dataStructures.end()) return false;
        llvm::StructType* argType = argIt->second.StructType;
        llvm::StructType* paramType = paramIt->second.StructType;
        if (argType == nullptr || paramType == nullptr) return false;
        if (argType == paramType) return true;
        if (argType->isOpaque() || paramType->isOpaque()) return false;
        return argType->isLayoutIdentical(paramType);
}

void LLVMBackend::RejectThrowingCxxFunction(const FunctionSymbol& symbol, const std::string& displayName) const
{
        if (!symbol.IsCxx || symbol.IsNoexcept || !cppStrictNoexcept_) return;
        LogError(std::format("call to '{}' may throw - C++ exceptions are not supported yet; "
                             "declare it noexcept or wrap it in an extern \"C\" noexcept function",
                             displayName));
}

llvm::Function* LLVMBackend::GetTargetEhPersonality()
{
        auto* i32Ty = llvm::Type::getInt32Ty(*context);
        if (targetWindows_)
        {
            if (currentFunction != nullptr && cppInteropUsed_ && FrameOwesUnwindCleanup())
            {
                llvm::Function* cxx = module->getFunction("__CxxFrameHandler3");
                if (cxx == nullptr)
                {
                    cxx = llvm::cast<llvm::Function>(module->getOrInsertFunction(
                        "__CxxFrameHandler3",
                        llvm::FunctionType::get(i32Ty, /*isVarArg=*/true)).getCallee());
                    cxx->setDLLStorageClass(llvm::GlobalValue::DLLImportStorageClass);
                }
                return cxx;
            }
            // Win32 has no personality: LLVM's x86 backend drops handler bodies under
            // _except_handler3 (see the `program` trampoline).
            if (platformValue != 64) return nullptr;
            llvm::Function* csh = module->getFunction("__C_specific_handler");
            if (csh == nullptr)
            {
                csh = llvm::cast<llvm::Function>(module->getOrInsertFunction(
                    "__C_specific_handler", llvm::FunctionType::get(i32Ty, /*isVarArg=*/true)).getCallee());
                csh->setDLLStorageClass(llvm::GlobalValue::DLLImportStorageClass);
            }
            return csh;
        }
        return llvm::cast<llvm::Function>(module->getOrInsertFunction(
            "__gxx_personality_v0", llvm::FunctionType::get(i32Ty, /*isVarArg=*/true)).getCallee());
}

llvm::Function* LLVMBackend::EnsureCxxProgramEhGuard(const std::string& programName)
{
        constexpr const char* guardName = "__cflat_program_eh_guard";
        if (symbolSink_ != nullptr) return nullptr;
        if (cxxProgramEhGuardAttempted_) return module->getFunction(guardName);
        cxxProgramEhGuardAttempted_ = true;

        cflat_cinterop::ExtractRequest req;
        req.mainFileName = "cflat_program_eh_guard.cpp";
        req.source = R"cpp(
extern "C" int __cflat_program_eh_guard(int (*fn)(void*), void* ctx, int* rc) noexcept
{
    try { *rc = fn(ctx); return 0; } catch (...) { return 1; }
}
)cpp";
        req.args = BuildClangDriverArgs("", {}, /*errorRecovery*/ false, /*asCxx*/ true);
        req.cxxMode = true;
        req.emitDefinitions = true;
        req.verbose = verbose;

        cflat_cinterop::ExtractResult raw;
        std::string error;
        if (!cflat_cinterop::ExtractCInterop(req, raw, error) || raw.bitcode.empty())
        {
            std::string detail = raw.firstError.empty() ? error : raw.firstError;
            const size_t newline = detail.find('\n');
            if (newline != std::string::npos) detail.resize(newline);
            if (detail.empty()) detail = "no companion bitcode was emitted";
            LogError(std::format(
                "program '{}': could not compile the C++ exception guard - {}",
                programName, detail));
            return nullptr;
        }

        AdoptCxxCompanionBitcode(raw.bitcode);
        auto* ptrType = llvm::PointerType::get(*context, 0);
        auto* guardType = llvm::FunctionType::get(
            llvm::Type::getInt32Ty(*context), {ptrType, ptrType, ptrType}, false);
        return llvm::cast<llvm::Function>(
            module->getOrInsertFunction(guardName, guardType).getCallee());
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
        auto rawDestructorAbsent = [&](const CRecordEntry& record) -> std::optional<bool> {
            using Member = cflat_cinterop::RawCxxMember;
            for (const Member& member : record.members)
            {
                if (member.kind != cflat_cinterop::RawCxxMember::Destructor) continue;
                // These are the projection gates before type mapping. A rejected raw member is
                // certainly absent from CxxClassInfo; a candidate that reaches mapping is still
                // checked by the projecting path because mapping can reject more shapes.
                if (member.access != cflat_cinterop::AccessPublic || member.isDeleted
                    || !member.bindRefusal.empty() || !record.layoutRefusal.empty()
                    || member.variadic || member.requiresConstructorWrapper
                    || member.needsLocalDefinition || !member.abi.valid
                    || member.linkageName.empty()) continue;
                // The raw member flags do not encode every later projection gate (for example,
                // signature mapping and ABI-plan validation). Defer positive bindability claims
                // to the existing path; absence or an earlier refusal is decisive here.
                return std::nullopt;
            }
            // No raw destructor candidate can reach the projected special-member set.
            return false;
        };
        auto rawDecision = [&](const TypeAndValue& tv, bool isReturn,
                               std::optional<bool>& decision) -> bool {
            if (tv.IsAlias || tv.Pointer || tv.IsInterface || tv.IsFunctionPointer)
            {
                decision = false;
                return true;
            }
            if (cxxRecords_.count(tv.TypeName) == 0)
            {
                decision = false;
                return true;
            }
            if (tv.TypeName.empty())
            {
                decision = false;
                return true;
            }
            auto found = cxxRecordEntries_.find(tv.TypeName);
            if (found == cxxRecordEntries_.end()) return false;
            // The destructor facts below are the deferred ones (R1): settle them first.
            if (found->second.specialMembersPending)
            {
                CompleteCxxRecordSpecialMembers(tv.TypeName);
                found = cxxRecordEntries_.find(tv.TypeName);
                if (found == cxxRecordEntries_.end()) return false;
            }
            const CRecordEntry& record = found->second;
            const bool trivial = record.isTriviallyCopyable
                || ((tv.TypeName.starts_with("std.pair$")
                     || tv.TypeName.starts_with("std.optional$")
                     || tv.TypeName.starts_with("std.tuple$")) && record.hasTrivialDtor);
            if (trivial)
            {
                decision = false;
                return true;
            }
            const bool nontrivial = !record.isUnion;
            if (!nontrivial)
            {
                decision = true;
                return true;
            }
            const auto dtor = rawDestructorAbsent(record);
            if (!dtor) return false;
            if (isReturn && record.hasTrivialDtor)
            {
                decision = false;
                return true;
            }
            decision = true;
            return true;
        };
        auto rawReject = [&](bool& determinate, std::string& rejectedType) {
            determinate = true;
            auto reject = [&](const TypeAndValue& tv, bool isReturn) {
                if (tv.IsAlias) return false;
                std::optional<bool> result;
                if (!rawDecision(tv, isReturn, result))
                {
                    determinate = false;
                    return false;
                }
                if (*result)
                {
                    rejectedType = tv.TypeName;
                    return true;
                }
                return false;
            };
            if (reject(sig.ret, true)) return true;
            for (const TypeAndValue& p : sig.params)
                if (reject(p, false)) return true;
            return false;
        };
        auto refusalFor = [&](const std::string& typeName) {
            return std::format(
                "C++ function '{}' takes or returns nontrivial type '{}' by value; a record "
                "passed by value must be trivially copyable (no user copy/move constructor, "
                "destructor, or virtuals) - pass it by pointer or reference instead",
                sig.name, typeName);
        };
        auto projectingReject = [&]() {
        auto hasProjectedCopyMoveCtor = [&](const std::string& typeName) {
            auto info = cxxClasses_.find(typeName);
            return info != cxxClasses_.end()
                && std::any_of(info->second.constructors.begin(), info->second.constructors.end(),
                    [](const CxxClassInfo::Structor& ctor) {
                        return ctor.isCopyCtor || ctor.isMoveCtor;
                    });
        };
        auto refuse = [&](const TypeAndValue& tv, bool isReturn) {
            // A `T&` return or parameter is `alias T`: it crosses by address, never by value.
            if (tv.IsAlias) return false;
            if (cxxRecords_.count(tv.TypeName) != 0)
                EnsureCxxRecordProjected(tv.TypeName, false, true);
            if (!IsByValueStructTV(tv)) return false;
            if (cxxTriviallyCopyableRecords_.count(tv.TypeName) != 0) return false;
            if (cxxRecords_.count(tv.TypeName) == 0) return false;  // not a C++ record: C rules apply
            // A RETURN needs only a destructor the caller can run on its sret temp: a bindable
            // one, or a trivial one that needs no call (the member path binds the same shape).
            if (isReturn && cxxNontrivialRecords_.count(tv.TypeName) != 0
                && (HasBindableCxxDestructor(tv.TypeName) || HasTrivialCxxDtor(tv.TypeName)))
                return false;
            // A NONTRIVIAL but non-polymorphic, base-less class does cross by value: clang's
            // arrangement makes it Indirect-without-byval, the CALLER owns the temp, and the
            // call site copy- or move-CONSTRUCTS it and destroys it after the call (M4b).
            // The special members that crossing needs must be BINDABLE though: an implicit or
            // inline-only copy constructor / destructor has no symbol to call, so such a record
            // is still refused here rather than at a call site that could not fix it.
            if (cxxNontrivialRecords_.count(tv.TypeName) != 0
                && HasBindableCxxDestructor(tv.TypeName)
                && (isReturn || hasProjectedCopyMoveCtor(tv.TypeName)))
                return false;
            // Refuse THIS signature only, with the reason replayed if CFlat ever calls it. A
            // library header carries many such helpers the program never names.
            std::string reason = refusalFor(tv.TypeName);
            if (verbose) std::cout << "[verbose]   refused: " << reason << "\n";
            cxxBindingRefusals_.emplace(sig.name, std::move(reason));
            return true;
        };
        if (refuse(sig.ret, true)) return true;
        for (const TypeAndValue& p : sig.params)
            if (refuse(p, false)) return true;
        return false;
        };
        bool determinate = false;
        std::string rawRejectedType;
        const bool rawResult = rawReject(determinate, rawRejectedType);
        if (determinate)
        {
            if (!rawResult) return false;
            std::string reason = refusalFor(rawRejectedType);
            if (verbose) std::cout << "[verbose]   refused: " << reason << "\n";
            cxxBindingRefusals_.emplace(sig.name, std::move(reason));
            return true;
        }
        return projectingReject();
}

void LLVMBackend::CheckPoisonedFunctionCalls()
{
        // Report poisoned functions reachable from live code first (a call inside an uncalled
        // container method is the consequence), then in first-call order - never hash order:
        // the first error is the one an expect_error sees, and unordered_map iteration differs
        // between libc++ and MSVC STL.
        auto reachable = [](llvm::Function* f) {
            std::vector<llvm::Function*> work{f};
            std::unordered_set<llvm::Function*> seen{f};
            while (!work.empty())
            {
                llvm::Function* g = work.back();
                work.pop_back();
                if (!g->hasLocalLinkage()) return true;
                for (auto* u : g->materialized_users())
                {
                    auto* call = llvm::dyn_cast<llvm::CallBase>(u);
                    // An address use (vtable, function value) is conservatively live.
                    if (call == nullptr || call->getCalledOperand() != g) return true;
                    if (llvm::Function* caller = call->getFunction(); seen.insert(caller).second)
                        work.push_back(caller);
                }
            }
            return false;
        };
        struct Called { const std::string* Name; const std::string* Msg; const FirstCallSite* Site; bool Live; };
        std::vector<Called> called;
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
                    auto locIt = firstCallLocation_.find(name);
                    called.push_back({&name, &msg,
                        locIt != firstCallLocation_.end() ? &locIt->second : nullptr, reachable(f)});
                    break;
                }
            }
        }
        std::sort(called.begin(), called.end(), [](const Called& a, const Called& b) {
            if (a.Live != b.Live) return a.Live;
            if ((a.Site == nullptr) != (b.Site == nullptr)) return b.Site == nullptr;
            if (a.Site != nullptr && a.Site->Order != b.Site->Order) return a.Site->Order < b.Site->Order;
            return *a.Name < *b.Name;
        });
        for (const Called& c : called)
        {
            // Point the diagnostic at the call site rather than wherever the walk ended.
            if (c.Site != nullptr) SetSourceLocation(c.Site->Line, c.Site->Column);
            LogRawError(*c.Msg);
        }
    }

void LLVMBackend::CheckAliasReturnSlotAddressTaken()
{
        // Offenders are reported in source order of their first address use; functionTable is
        // unordered, so iterating it directly made the order host-dependent.
        struct Offender
        {
            std::string Shown;
            std::string Param;
            std::string Mangled;
            const AliasReturnAddressUse* Use = nullptr;
        };
        std::vector<Offender> offenders;
        for (const auto& [name, symbols] : functionTable)
            for (const auto& sym : symbols)
            {
                if (sym.Function == nullptr || sym.IsCxx || sym.External) continue;
                const std::string param = AliasReturnSlotParamName(sym.ReturnType, sym.Parameters);
                if (param.empty()) continue;
                for (auto* u : sym.Function->materialized_users())
                {
                auto* call = llvm::dyn_cast<llvm::CallBase>(u);
                // A direct call from a C-ABI reverse thunk is the function value escaping, not a
                // source call: the thunk still cannot provide the caller-owned slot.
                if (call != nullptr && call->getCalledOperand() == sym.Function
                    && !call->getFunction()->getName().starts_with("__cflat_reverse_abi_"))
                    continue;
                auto useIt = firstAliasReturnAddressUse_.find(sym.Function->getName().str());
                offenders.push_back({ sym.SourceName.empty() ? name : sym.SourceName, param,
                    sym.Function->getName().str(), useIt != firstAliasReturnAddressUse_.end() ? &useIt->second : nullptr });
                break;
                }
            }
        std::sort(offenders.begin(), offenders.end(), [](const Offender& a, const Offender& b)
        {
            if ((a.Use != nullptr) != (b.Use != nullptr)) return a.Use != nullptr;
            if (a.Use != nullptr)
            {
                if (a.Use->File != b.Use->File) return a.Use->File < b.Use->File;
                if (a.Use->Line != b.Use->Line) return a.Use->Line < b.Use->Line;
                if (a.Use->Column != b.Use->Column) return a.Use->Column < b.Use->Column;
            }
            // Overloads share a source name and use site; the mangled name keeps the order total.
            if (a.Shown != b.Shown) return a.Shown < b.Shown;
            return a.Mangled < b.Mangled;
        });
        for (const Offender& o : offenders)
        {
            if (o.Use != nullptr)
            {
                sourceFileName = o.Use->File;
                SetSourceLocation(o.Use->Line, o.Use->Column);
            }
            if (o.Use != nullptr && !o.Use->InterfaceName.empty())
                LogError(std::format(
                    "cannot convert to interface '{}': method '{}' has an 'alias' result that can "
                    "be its by-value parameter '{}', which an interface call cannot pass through "
                    "the required caller-owned slot", o.Use->InterfaceName, o.Shown, o.Param));
            LogError(std::format(
                "cannot use '{}' as a function value: its 'alias' result can be its by-value "
                "parameter '{}', which a function value cannot pass through the required "
                "caller-owned slot. Call '{}' directly, or declare the parameter 'move' so the "
                "result is owned.", o.Shown, o.Param, o.Shown));
        }
    }

bool LLVMBackend::VerifyModule()
{
        std::string errors;
        llvm::raw_string_ostream errorStream(errors);
        if (cachedFunctionNames_ || !companionFunctionNames_.empty())
        {
            // Cached artifacts and clang-generated companion bodies are not checked here.
            for (auto& function : module->functions())
            {
                if (function.isDeclaration()
                    || (cachedFunctionNames_
                        && cachedFunctionNames_->contains(function.getName().str()))
                    || companionFunctionNames_.contains(function.getName().str()))
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

std::string LLVMBackend::FindDsymutil() const
{
        std::vector<std::filesystem::path> installs;
        for (const char* homeVar : { "HOME", "USERPROFILE" })
        {
            const char* home = std::getenv(homeVar);
            if (!home || !*home) continue;
            std::filesystem::path deps = std::filesystem::path(home) / ".cflat-compiler-deps";
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator(deps, ec))
            {
                if (!entry.is_directory(ec)) continue;
                const std::string name = entry.path().filename().string();
                if (name.rfind("llvm-", 0) == 0) installs.push_back(entry.path());
            }
            if (!installs.empty()) break;
        }
        // Prefer the release install when both release and assertion-enabled trees exist.
        std::sort(installs.begin(), installs.end(), [](const auto& a, const auto& b) {
            const bool aAssert = a.filename().string().find("-assert") != std::string::npos;
            const bool bAssert = b.filename().string().find("-assert") != std::string::npos;
            if (aAssert != bAssert) return !aAssert;
            return a.string() > b.string();
        });
        for (const auto& install : installs)
        {
            for (const char* name : { "dsymutil", "llvm-dsymutil" })
            {
                const auto candidate = install / "bin" / name;
                if (llvm::sys::fs::exists(candidate.string())) return candidate.string();
            }
        }
        if (auto p = llvm::sys::findProgramByName("dsymutil")) return *p;
        if (auto p = llvm::sys::findProgramByName("llvm-dsymutil")) return *p;
        return "";
    }

bool LLVMBackend::EmitMacDebugInfo(const std::string& exePath) const
{
        const std::string dsymutil = FindDsymutil();
        if (dsymutil.empty())
        {
            if (verbose)
                std::cout << "[verbose] dsymutil not found; keeping debug object files\n";
            return false;
        }

        const std::string dsymPath = exePath + ".dSYM";
        std::vector<std::string> argStrs = { dsymutil, exePath, "-o", dsymPath };
        std::vector<llvm::StringRef> args;
        for (auto& arg : argStrs) args.push_back(arg);
        std::string dsymErr;
        int rc = llvm::sys::ExecuteAndWait(dsymutil, args, std::nullopt, {}, 0, 0, &dsymErr);
        if (rc != 0 || !llvm::sys::fs::exists(dsymPath))
        {
            if (verbose)
                std::cout << std::format("[verbose] dsymutil failed; keeping debug object files{}{}\n",
                    dsymErr.empty() ? "" : ": ", dsymErr);
            return false;
        }
        return true;
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

// Depfile target name passed with -MT, so the prerequisite list starts at a known offset.
static constexpr const char* kCObjectDepTarget = "cflat_obj";
// Bump when the object cache layout or the meaning of its key changes.
static constexpr int64_t kCObjectCacheVersion = 1;

// Prerequisites of a Make-style depfile written with `-MT cflat_obj`: escaped spaces, `$$`
// and backslash-newline continuations are undone; any other backslash is a path separator.
static std::vector<std::string> ParseCObjectDepFile(llvm::StringRef text)
{
        std::vector<std::string> paths;
        const std::string target = std::string(kCObjectDepTarget) + ":";
        if (!text.starts_with(target)) return paths;
        std::string current;
        auto flush = [&]() {
            if (!current.empty()) paths.push_back(std::move(current));
            current.clear();
        };
        for (size_t i = target.size(); i < text.size(); ++i)
        {
            const char c = text[i];
            const char next = i + 1 < text.size() ? text[i + 1] : '\0';
            if (c == '\\' && (next == '\n' || next == '\r')) { flush(); continue; }
            if (c == '\\' && (next == ' ' || next == '#')) { current += next; ++i; continue; }
            if (c == '$' && next == '$') { current += '$'; ++i; continue; }
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { flush(); continue; }
            current += c;
        }
        flush();
        return paths;
}

uint64_t LLVMBackend::CObjectCommandKey(const std::vector<std::string>& argStrs, const std::string& cSourcePath)
{
        // The source is keyed by absolute path: the same relative spelling from another
        // working directory names a different file.
        std::error_code ec;
        const std::string absoluteSource = std::filesystem::absolute(cSourcePath, ec).string();
        uint64_t h = 14695981039346656037ULL;
        auto fold = [&h](const std::string& s) {
            for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
        };
        fold(std::format("|V{}", kCObjectCacheVersion));
        for (const auto& arg : argStrs)
        {
            fold("|A");
            fold(arg == cSourcePath && !ec ? absoluteSource : arg);
        }
        return h;
}

bool LLVMBackend::TryReuseCObject(uint64_t commandKey, const std::string& objPath)
{
        const std::string cacheDir = GetCHeaderCacheDir();
        if (cacheDir.empty()) return false;
        const std::filesystem::path base =
            std::filesystem::path(cacheDir) / "objects" / std::format("{:016x}", commandKey);
        const std::filesystem::path depsPath = base.string() + ".deps.json";
        std::error_code depsTimeError;
        const auto cacheWriteTime = std::filesystem::last_write_time(depsPath, depsTimeError);
        if (depsTimeError) return false;
        auto stampText = llvm::MemoryBuffer::getFile(depsPath.string());
        if (!stampText) return false;
        llvm::Expected<llvm::json::Value> parsed = llvm::json::parse((*stampText)->getBuffer());
        if (!parsed)
        {
            llvm::consumeError(parsed.takeError());
            return false;
        }
        const llvm::json::Object* root = parsed->getAsObject();
        const llvm::json::Array* stamps = root != nullptr ? root->getArray("deps") : nullptr;
        if (stamps == nullptr || root->getInteger("version") != kCObjectCacheVersion) return false;

        std::vector<CHeaderDep> deps;
        const int64_t cacheWriteTimeCount = (int64_t)cacheWriteTime.time_since_epoch().count();
        for (const llvm::json::Value& value : *stamps)
        {
            const llvm::json::Object* stamp = value.getAsObject();
            if (stamp == nullptr) return false;
            std::optional<llvm::StringRef> path = stamp->getString("f");
            std::optional<int64_t> mtime = stamp->getInteger("mt");
            std::optional<llvm::StringRef> hash = stamp->getString("h");
            CHeaderDep dep;
            if (!path || !mtime || !hash || hash->getAsInteger(16, dep.hash)) return false;
            dep.path = path->str();
            dep.mtime = *mtime;
            dep.hashTime = stamp->getInteger("ht").value_or(cacheWriteTimeCount);
            if (!CHeaderDepFresh(dep, cacheWriteTime))
            {
                if (verbose)
                    std::cout << std::format("[verbose] C object cache stale: {} changed\n", dep.path);
                return false;
            }
            deps.push_back(std::move(dep));
        }
        if (llvm::sys::fs::copy_file(base.string() + ".obj", objPath)) return false;
        for (const auto& dep : deps) RecordDependency(dep.path);
        if (verbose)
            std::cout << std::format("[verbose] C object cache hit: {}\n", base.string() + ".obj");
        return true;
}

void LLVMBackend::StoreCObject(uint64_t commandKey, const std::string& objPath, const std::string& depFilePath)
{
        auto depText = llvm::MemoryBuffer::getFile(depFilePath);
        (void)llvm::sys::fs::remove(depFilePath);
        if (!depText) return;
        llvm::json::Array stamps;
        std::unordered_set<std::string> seen;
        for (const std::string& listed : ParseCObjectDepFile((*depText)->getBuffer()))
        {
            std::error_code ec;
            const std::string path = std::filesystem::absolute(listed, ec).string();
            if (ec || !seen.insert(path).second) continue;
            RecordDependency(path);
            auto mtime = std::filesystem::last_write_time(path, ec);
            const int64_t hashTime = (int64_t)std::filesystem::file_time_type::clock::now()
                                         .time_since_epoch().count();
            uint64_t hash = 0;
            if (ec || !HashFileFnv1a(path, hash)) continue;
            stamps.push_back(llvm::json::Object{
                {"f", path},
                {"mt", (int64_t)mtime.time_since_epoch().count()},
                {"h", std::format("{:016x}", hash)},
                {"ht", hashTime}});
        }
        // A depfile without inputs cannot validate a reuse, so it is never stored.
        const std::string cacheDir = GetCHeaderCacheDir();
        if (stamps.empty() || cacheDir.empty() || runMode_) return;

        std::error_code ec;
        const std::filesystem::path dir = std::filesystem::path(cacheDir) / "objects";
        std::filesystem::create_directories(dir, ec);
        if (ec) return;
        const std::string base = (dir / std::format("{:016x}", commandKey)).string();
        // The object lands first and the stamp file last, each by rename: a stamp file on disk
        // always names a complete object.
        const std::string tempSuffix = std::format(".{}.tmp", llvm::sys::Process::getProcessId());
        if (llvm::sys::fs::copy_file(objPath, base + ".obj" + tempSuffix)
            || llvm::sys::fs::rename(base + ".obj" + tempSuffix, base + ".obj"))
        {
            (void)llvm::sys::fs::remove(base + ".obj" + tempSuffix);
            return;
        }
        std::string json;
        llvm::raw_string_ostream os(json);
        os << llvm::json::Value(llvm::json::Object{{"version", kCObjectCacheVersion},
                                                   {"deps", std::move(stamps)}});
        os.flush();
        const std::string depsTemp = base + ".deps.json" + tempSuffix;
        bool written;
        {
            std::ofstream out(depsTemp, std::ios::binary | std::ios::trunc);
            out << json;
            written = static_cast<bool>(out);
        }
        if (!written || llvm::sys::fs::rename(depsTemp, base + ".deps.json"))
            (void)llvm::sys::fs::remove(depsTemp);
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
        std::vector<std::string> argStrs = { cc, "-c", "-fPIC", cSourcePath };
        // Match the deployment target EmitExecutableMachO links against. Without it the host
        // clang stamps its own (newer) minos and ld64 warns on every C-interop link.
        if (targetMacOS_)
        {
            argStrs.push_back("-target");
            argStrs.push_back(("arm64-apple-macosx" + cflat::platform::MacDeploymentVersion()));
            if (cxxMode)
            {
                argStrs.push_back("-stdlib=libc++");
                argStrs.push_back("-std=" + cppStandard_);
                const std::string& sdk = MacSdkPathCached();
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

        const uint64_t commandKey = CObjectCommandKey(argStrs, cSourcePath);
        if (TryReuseCObject(commandKey, objPath))
        {
            cObjectFiles_.push_back(objPath);
            return true;
        }
        if (!CountTuParse(cxxMode ? "C++ source" : "C source", std::format("obj:{:016x}", commandKey),
                          cSourcePath, "object compile"))
        {
            (void)llvm::sys::fs::remove(objPath);
            return false;
        }
        const std::string depPath = objPath + ".d";
        for (const std::string& arg : { std::string("-o"), objPath, std::string("-MD"), std::string("-MF"),
                                        depPath, std::string("-MT"), std::string(kCObjectDepTarget) })
            argStrs.push_back(arg);

        std::vector<llvm::StringRef> args;
        args.reserve(args.size() + argStrs.size());
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
            (void)llvm::sys::fs::remove(objPath);
            (void)llvm::sys::fs::remove(depPath);
            LogRawError(std::format("C compiler failed to compile C source '{}' (exit {}){}{}",
                cSourcePath, rc, compileErr.empty() ? "" : ": ", compileErr));
            return false;
        }

        StoreCObject(commandKey, objPath, depPath);
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
        // /MD (dynamic UCRT) so this object's CRT /defaultlib directives are the dynamic set,
        // not clang-cl's /MT default (libcmt) which the freestanding link cannot satisfy. Covers
        // both user .c interop and the imported diagnostic/heap_audit.c.
        std::vector<std::string> argStrs = { clangPath, "/c", "/MD", "/nologo", target, cSourcePath };
        if (cxxMode)
        {
            argStrs.push_back("/std:" + ClangClCppStandard(cppStandard_));
            argStrs.push_back("/EHsc");
        }
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

        const uint64_t commandKey = CObjectCommandKey(argStrs, cSourcePath);
        if (TryReuseCObject(commandKey, objPath))
        {
            cObjectFiles_.push_back(objPath);
            return true;
        }
        if (!CountTuParse(cxxMode ? "C++ source" : "C source", std::format("obj:{:016x}", commandKey),
                          cSourcePath, "object compile"))
        {
            (void)llvm::sys::fs::remove(objPath);
            return false;
        }
        const std::string depPath = objPath + ".d";
        argStrs.push_back("/Fo" + objPath);
        argStrs.push_back("/clang:-MD");
        argStrs.push_back("/clang:-MF" + depPath);
        argStrs.push_back(std::string("/clang:-MT") + kCObjectDepTarget);

        std::vector<llvm::StringRef> args;
        args.reserve(args.size() + argStrs.size());
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
            (void)llvm::sys::fs::remove(objPath);
            (void)llvm::sys::fs::remove(depPath);
            LogRawError(std::format("clang-cl failed to compile C source '{}' (exit {}){}{}",
                cSourcePath, rc, clangCompileErr.empty() ? "" : ": ", clangCompileErr));
            return false;
        }

        StoreCObject(commandKey, objPath, depPath);
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
        args.reserve(args.size() + argStrs.size());
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
            (void)llvm::sys::fs::remove(objPath);
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
        args.reserve(args.size() + argStrs.size());
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
            (void)llvm::sys::fs::remove(objPath);
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

bool LLVMBackend::MapCTypeToTypeAndValue(std::string ctype, TypeAndValue& out,
                                         bool cxxBoundary)
{
        std::unordered_set<std::string> visited;
        return MapCTypeToTypeAndValueImpl(std::move(ctype), out, visited, cxxBoundary);
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

void LLVMBackend::RegisterCxxUsingDirectives(
        const std::vector<std::pair<std::string, std::string>>& directives)
{
        auto registerNamespace = [this](const std::string& name) {
            if (name.empty()) return;
            size_t start = 0;
            while (true)
            {
                const size_t dot = name.find('.', start);
                if (dot == std::string::npos) break;
                if (dot > 0) RegisterNamespace(name.substr(0, dot));
                start = dot + 1;
            }
            RegisterNamespace(name);

            const size_t firstDot = name.find('.');
            const std::string lead = firstDot == std::string::npos
                ? name : name.substr(0, firstDot);
            cxxForeignNamespaces_.insert(lead);
            if (activeCxxRequestGroup_ != nullptr
                && activeCxxRequestGroup_->primary < cxxImportGroups_.size())
                cxxImportGroups_[activeCxxRequestGroup_->primary].namespaces.insert(lead);
        };

        for (const auto& [from, to] : directives)
        {
            if (from.empty() || to.empty()) continue;
            registerNamespace(from);
            registerNamespace(to);
            auto& nominated = cxxUsingDirectives_[from];
            if (std::find(nominated.begin(), nominated.end(), to) == nominated.end())
                nominated.push_back(to);
        }
}

// A C++ namespace alias is the same relationship CFlat's own `namespace a = b;` records, so it
// registers in the same table and every lookup that hops an alias hops this one too.
void LLVMBackend::RegisterCxxNamespaceAliases(
        const std::vector<std::pair<std::string, std::string>>& aliases)
{
        for (const auto& [alias, target] : aliases)
        {
            if (alias.empty() || target.empty() || alias == target) continue;
            RegisterNamespaceAlias(alias, target);
            // The alias spelling itself is not a declared namespace, so publish its leading
            // segment as a foreign one - that is what makes a name led by it a C++ candidate.
            const size_t firstDot = alias.find('.');
            const std::string lead = firstDot == std::string::npos
                ? alias : alias.substr(0, firstDot);
            cxxForeignNamespaces_.insert(lead);
            if (activeCxxRequestGroup_ != nullptr
                && activeCxxRequestGroup_->primary < cxxImportGroups_.size())
                cxxImportGroups_[activeCxxRequestGroup_->primary].namespaces.insert(lead);
        }
}

bool LLVMBackend::IsCInteropLongDoubleSupported() const
{
        if (cInteropLongDoubleWidth_ != 0)
            return cInteropLongDoubleWidth_ == 64 && cInteropLongDoubleIsIEEEDouble_;

        // Native declarations can appear before the first C/C++ header is imported, so use
        // the configured target for the two known 64-bit IEEE long-double ABIs in that case.
        const std::string triple = cInteropTargetTriple_.empty()
            ? CInteropTargetTriple() : cInteropTargetTriple_;
        const bool windowsX64 = triple.find("x86_64") != std::string::npos
            && triple.find("windows") != std::string::npos;
        const bool macArm64 = (triple.find("arm64") != std::string::npos
                               || triple.find("aarch64") != std::string::npos)
            && triple.find("apple") != std::string::npos;
        return windowsX64 || macArm64;
}

std::string LLVMBackend::CInteropLongDoubleRefusal() const
{
        const std::string triple = cInteropTargetTriple_.empty()
            ? CInteropTargetTriple() : cInteropTargetTriple_;
        uint64_t width = cInteropLongDoubleWidth_;
        if (width == 0)
        {
            if (triple.find("windows") != std::string::npos) width = 64;
            else if (triple.find("x86_64") != std::string::npos) width = 80;
            else if (triple.find("aarch64") != std::string::npos
                     || triple.find("arm64") != std::string::npos)
                width = triple.find("apple") != std::string::npos ? 64 : 128;
        }
        if (width > 0)
            return std::format("'long double' is {} bits on {}; CFlat has no matching type",
                               width, triple);
        return std::format("'long double' has no matching CFlat type on {}", triple);
}

bool LLVMBackend::IsLongDoubleSpelling(const std::string& spelling)
{
        return spelling.find("long double") != std::string::npos;
}


// A C++ field declared const: its object level.

bool LLVMBackend::ParseCFunctionPointerSpelling(const std::string& s, TypeAndValue& out,
                                       std::unordered_set<std::string>& visited,
                                       bool cxxBoundary)
{
        // Locate "(*)" possibly with whitespace around the star.
        size_t markerPos = std::string::npos;
        bool functionPointerLvalueRef = false;
        bool functionPointerRvalueRef = false;
        bool functionPointerConst = false;
        int angleDepth = 0;   // a `(*)` inside template arguments is not this type's declarator
        for (size_t i = 0; i + 2 < s.size(); ++i)
        {
            if (s[i] == '<') ++angleDepth;
            else if (s[i] == '>' && angleDepth > 0) --angleDepth;
            if (s[i] != '(' || angleDepth != 0) continue;
            bool candidateLvalueRef = false;
            bool candidateRvalueRef = false;
            bool candidateConst = false;
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
                        candidateConst = candidateConst || std::strcmp(qualifier, "const") == 0;
                        j += len;
                        while (j < s.size() && std::isspace((unsigned char)s[j])) ++j;
                        removed = true;
                        break;
                    }
                }
                if (!removed) break;
            }
            if (j + 1 < s.size() && s.compare(j, 2, "&&") == 0)
            {
                candidateRvalueRef = true;
                j += 2;
                while (j < s.size() && std::isspace((unsigned char)s[j])) ++j;
            }
            else if (j < s.size() && s[j] == '&')
            {
                candidateLvalueRef = true;
                ++j;
                while (j < s.size() && std::isspace((unsigned char)s[j])) ++j;
            }
            if (j < s.size() && s[j] == ')')
            {
                markerPos = i;
                functionPointerLvalueRef = candidateLvalueRef;
                functionPointerRvalueRef = candidateRvalueRef;
                functionPointerConst = candidateConst;
                break;
            }
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

        // A function pointer returning an array reference puts the array declarator after the
        // function parameter list, so it belongs to the return type as well.
        if (cxxBoundary && retSpelling.find("(&") != std::string::npos)
        {
            std::string suffix = s.substr(argClose + 1);
            size_t next = suffix.find_first_not_of(" \t");
            if (next != std::string::npos && suffix[next] == ')')
            {
                ++next;
                next = suffix.find_first_not_of(" \t", next);
                if (next != std::string::npos && suffix[next] == '[')
                    retSpelling += suffix;
            }
        }

        // Resolve the return type via the same recursive resolver.
        TypeAndValue retTV;
        std::vector<uint64_t> returnArrayDims;
        const bool cxxArrayReferenceReturn = cxxBoundary
            && (retSpelling.find("(&)[") != std::string::npos
                || retSpelling.find("(&&)[") != std::string::npos);
        const size_t returnArrayDimensionCount = cxxArrayReferenceReturn
            ? static_cast<size_t>(std::count(retSpelling.begin(), retSpelling.end(), '[')) : 0;
        const std::string retMapSpelling = cxxArrayReferenceReturn
            ? StripFixedArrayDims(retSpelling, returnArrayDims) : retSpelling;
        if (!MapCTypeToTypeAndValueImpl(retMapSpelling, retTV, visited, cxxBoundary)) return false;
        // Function pointers returning function pointers are not supported here.
        if (retTV.IsFunctionPointer) return false;
        const bool cxxReferenceReturn = cxxBoundary
            && (cxxArrayReferenceReturn || retSpelling.ends_with('&')
                || retSpelling.ends_with("&&"));
        if (cxxReferenceReturn && !cxxArrayReferenceReturn)
            MarkCxxReferenceField(retSpelling, retTV, true);
        if (cxxBoundary && retTV.Pointer)
            retTV.ConstMask |= CxxSpellingPointeeConstMask(retSpelling, retTV.ValuePointerDepth());

        out = TypeAndValue();
        out.IsFunctionPointer = true;
        out.TypeName = "__c_fn_ptr";       // thin: a C function pointer is the thin `function<T>`
        if (functionPointerLvalueRef || functionPointerRvalueRef)
        {
            out.Pointer = true;
            out.IsAlias = functionPointerLvalueRef;
            out.IsRvalueRef = functionPointerRvalueRef;
            out.IsCxxRefToPointer = functionPointerLvalueRef;
            out.MarkCxxRef();
            out.SetCxxConstRef(functionPointerConst);
        }
        out.FuncPtrReturnTypeName = retTV.TypeName;
        out.FuncPtrReturnPointer = retTV.Pointer;
        // Keep the ABI reference slot pointer-shaped at this indirect-call door.
        out.FuncPtrReturnAlias = cxxReferenceReturn ? false : retTV.IsAlias;
        out.FuncPtrReturnPointerDepth = retTV.ValuePointerDepth();
        if (cxxArrayReferenceReturn)
        {
            out.FuncPtrReturnAliasSpelling = retSpelling;
            out.FuncPtrReturnHasStrippedInnerDimensions = returnArrayDimensionCount > 1;
        }
        // ConstMask is part of the C++ callable's return type, including a reference referent.
        if (!cxxBoundary)
        {
            uint32_t mask = CSpellingPointeeConstMask(retSpelling, retTV.ValuePointerDepth());
            out.FuncPtrReturnConstMask = mask != 0 ? mask : retTV.PointeeConstMask();
        }
        else
            out.FuncPtrReturnConstMask = retTV.FunctionReturnConstMask();

        auto trim = [](std::string v) {
            size_t a = 0; while (a < v.size() && std::isspace((unsigned char)v[a])) ++a;
            size_t b = v.size(); while (b > a && std::isspace((unsigned char)v[b-1])) --b;
            return v.substr(a, b - a);
        };

        // A trailing `, ...` is the variadic tail (C requires at least one named parameter before
        // it). Any other ellipsis is refused, as is a nested fn-ptr arg below.
        std::string normArgs = trim(argList);
        const bool variadic = normArgs.ends_with("...");
        if (variadic)
        {
            const size_t comma = normArgs.rfind(',');
            if (comma == std::string::npos || trim(normArgs.substr(comma + 1)) != "...")
                return false;
            argList = normArgs.substr(0, comma);
            normArgs = trim(argList);
            if (normArgs.empty() || normArgs == "void") return false;   // `(void, ...)` is not C
        }
        out.FuncPtrVariadic = variadic;
        out.FuncPtrNoPrototype = !cxxBoundary && normArgs.empty();
        if (argList.find("...") != std::string::npos) return false;

        // Empty arg list or "void" -> zero params.
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
            const std::string parameterSpelling = trim(p);
            if (!MapCTypeToTypeAndValueImpl(parameterSpelling, ptv, visited, cxxBoundary)) return false;
            if (ptv.IsFunctionPointer) return false; // nested fn-ptr arg not supported
            TypeAndValue::FuncPtrParam fp;
            fp.TypeName = ptv.TypeName;
            fp.Pointer = ptv.Pointer;
            fp.PointerDepth = ptv.ValuePointerDepth();
            fp.IsRvalueRef = ptv.IsRvalueRef;
            const bool cxxLvalueReference = cxxBoundary
                && parameterSpelling.ends_with('&') && !parameterSpelling.ends_with("&&");
            const bool cxxPointerReference = cxxBoundary && ptv.Pointer
                && parameterSpelling.find('*') != std::string::npos
                && parameterSpelling.ends_with('&');
            const bool cxxClassLvalueReference = cxxLvalueReference
                && IsCxxRecord(ResolveTypeAlias(ptv.TypeName));
            fp.IsAlias = cxxPointerReference || cxxClassLvalueReference;
            // Leading const qualifies the innermost pointee (`const void *`).
            fp.IsPointeeConst = ptv.Pointer && parameterSpelling.starts_with("const ");
            if (!cxxBoundary)
            {
                uint32_t mask = CSpellingPointeeConstMask(parameterSpelling, ptv.ValuePointerDepth());
                fp.ConstMask = mask != 0 ? mask : ptv.PointeeConstMask();
            }
            else if (fp.Pointer)
            {
                const int valueDepth = fp.PointerDepth - (cxxPointerReference ? 1 : 0);
                fp.ConstMask = CxxSpellingPointeeConstMask(parameterSpelling, valueDepth);
                if (cxxPointerReference && CxxParamIsConstRefToPointer(parameterSpelling))
                    fp.ConstMask |= static_cast<int64_t>(LLVMBackend::TypeAndValue::ConstBitAt(fp.PointerDepth));
            }
            if (cxxBoundary && ((cxxClassLvalueReference && !cxxPointerReference)
                || (fp.IsRvalueRef && !cxxPointerReference)))
                fp.ConstMask = CxxParamIsConstLvalueReference(parameterSpelling)
                    || (fp.IsRvalueRef && CxxRvalueReferenceIsConst(parameterSpelling)) ? 1 : 0;
            out.FuncPtrParams.push_back(fp);
        }
        return true;
    }

std::string LLVMBackend::StripFixedArrayDims(const std::string& ctype, std::vector<uint64_t>& dims)
{
        // A REFERENCE to an array (`const char (&)[5]`, a forward_as_tuple member) is one pointer
        // to the first element, never N stored references.
        for (std::string_view declarator : { "(&)[", "(&&)[" })
            if (size_t at = ctype.find(declarator);
                at != std::string::npos && ctype.find('<') == std::string::npos)
            {
                std::string elem = ctype.substr(0, at);
                while (!elem.empty() && elem.back() == ' ') elem.pop_back();
                return elem + " *";
            }
        std::string elem;
        size_t i = 0;
        int angle = 0;   // a `[N]` inside template arguments belongs to the argument
        while (i < ctype.size())
        {
            if (ctype[i] == '<') ++angle;
            else if (ctype[i] == '>' && angle > 0) --angle;
            if (ctype[i] == '[' && angle == 0)
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
 * The CFlat dotted name of the class a C++ pointer or reference spelling points at, or "" when
 * the spelling is not a pointer/reference to a class. Clang spells a C++ class type as a bare qualified name
 * ("const cpppoly::Right *"), with no "struct"/"union" keyword, so AggregatePointeeTag - which
 * keys on that keyword - never recognizes one.
 */
std::string LLVMBackend::CxxRecordPointeeTag(const std::string& spelling, int& outPtr)
{
        outPtr = 0;
        std::string s = spelling;
        if (s.find('(') != std::string::npos || s.find('[') != std::string::npos)
            return std::string();   // function pointer / array: not a plain record pointer
        const bool isReference = s.find('&') != std::string::npos;
        for (const char* w : { "const", "volatile", "restrict", "__restrict", "__restrict__",
                               "struct", "class", "union", "&" })
            EraseDeclaratorToken(s, w);
        outPtr = (int)std::count(s.begin(), s.end(), '*') + (isReference ? 1 : 0);
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

// The reverse spelling index is rebuilt from cxxRecordEntries_ on the next miss. A re-store of
// a known record under the same canonical spelling leaves the index exact, so it stays clean.
void LLVMBackend::StoreCxxRecordEntry(const std::string& name, const CRecordEntry& record)
{
        cxxFriendNames_.insert(record.friendNames.begin(), record.friendNames.end());
        auto [it, inserted] = cxxRecordEntries_.try_emplace(name, record);
        if (!inserted)
        {
            if (it->second.canonicalCtype == record.canonicalCtype)
            {
                it->second = record;
                return;
            }
            it->second = record;
        }
        cxxRecordSpellingIndexDirty_ = true;
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

// An argument of the C++ null type: a value marked std::nullptr_t (a nullptr_t result, alias or
// `auto v = nullptr` local) or the bare `nullptr` literal - a constant null with no CFlat type and
// no pointer declarator. Pointer arithmetic or `f ? p : nullptr` is never one. A wrapper spells it
// decltype(nullptr), so C++ deduces std::nullptr_t.
bool LLVMBackend::IsCxxNullTypedArgument(const NamedVariable& arg)
{
        if (arg.TypeAndValue.IsCxxNullptrT) return true;
        const TypeAndValue& t = arg.TypeAndValue;
        return arg.LiteralIdentity.empty() && !t.Pointer && t.TypeName.empty()
            && !t.IsFunctionPointer && llvm::isa_and_nonnull<llvm::ConstantPointerNull>(arg.Primary);
}

// std::nullptr_t by value (outer cv ignored): a builtin, never a class worth a type request.

bool LLVMBackend::TryMapCxxForeignSpelling(const std::string& ctype, TypeAndValue& out,
                                           bool& mapped) const
{
        mapped = false;
        // A top-level std::function keeps its own callable mapping; only the '(' of a function
        // pointer / function type declines here, not one nested in a template argument.
        if (IsTopLevelStdFunctionSpelling(ctype) || CxxSpellingHasTopLevelParen(ctype)) return false;
        std::string s = ctype;
        int ptr = 0;
        int stars = 0;
        int refs = 0;
        // A fixed array decays to a pointer to its element, exactly as the general mapper does.
        // Only an outer '[' is a declarator: `default_delete<int[]>` keeps its argument.
        {
            int angle = 0;
            for (size_t i = 0; i < s.size(); ++i)
            {
                if (s[i] == '<') ++angle;
                else if (s[i] == '>' && angle > 0) --angle;
                else if (s[i] == '[' && angle == 0) { s.erase(i); ++ptr; ++stars; break; }
            }
        }

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
            if (s.back() == '*') { s.pop_back(); ++ptr; ++stars; peeled = true; continue; }
            if (s.back() == '&')
            {
                s.pop_back();
                if (!s.empty() && s.back() == '&') s.pop_back();
                ++ptr; ++refs; peeled = true; continue;
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

        const std::string originalSqueezed = SqueezeCxxSpelling(s);
        s = CanonicalizeCxxUniquePtrSpelling(s);
        const std::string squeezed = SqueezeCxxSpelling(s);
        std::string foreignName;
        if (auto it = cxxForeignTypeSpellings_.find(squeezed);
            it != cxxForeignTypeSpellings_.end())
            foreignName = it->second;
        else if (auto it = cxxForeignTypeSpellings_.find(originalSqueezed);
                 it != cxxForeignTypeSpellings_.end())
            foreignName = it->second;
        else
        {
            // Generated-wrapper replay can see the requested record before the reverse spelling
            // map is rebuilt. Index the fallback once instead of scanning every record per lookup.
            if (cxxRecordSpellingIndexDirty_)
            {
                cxxRecordSpellingIndex_.clear();
                for (const auto& [name, record] : cxxRecordEntries_)
                    if (!record.canonicalCtype.empty())
                        cxxRecordSpellingIndex_.try_emplace(
                            StripCxxRecordTag(record.canonicalCtype), name);
                cxxRecordSpellingIndexDirty_ = false;
            }
            if (auto it = cxxRecordSpellingIndex_.find(squeezed);
                it != cxxRecordSpellingIndex_.end())
                foreignName = it->second;
            else if (originalSqueezed != squeezed)
                if (auto it = cxxRecordSpellingIndex_.find(originalSqueezed);
                    it != cxxRecordSpellingIndex_.end())
                    foreignName = it->second;
        }
        if (foreignName.empty()) return false;
        out = TypeAndValue{};
        out.TypeName = foreignName;
        if (auto it = enumBackingTypes.find(foreignName); it != enumBackingTypes.end())
        {
            out.EnumBacking = it->second;
            out.IsScopedEnum = scopedEnumTypes_.count(foreignName) != 0;
        }
        out.IsRvalueRef = ptr > 0 && ctype.find("&&") != std::string::npos;
        if (ptr > 0) out.Pointer = true;
        if (ptr > 1) out.ElemPointer = true;
        mapped = ptr <= 2;
        // A REFERENCE to a T** is three declarator levels but only two CFlat ones: the reference
        // is the ABI's extra pointer, which IsCxxRefToPointer already encodes. Store T** and let
        // the caller keep the flag instead of peeling a level it cannot represent.
        if (!mapped && refs == 1 && stars == 2)
        {
            out.ElemPointer = true;
            out.IsCxxRefToPointer = true;
            mapped = true;
        }
        return true;
    }

bool LLVMBackend::MapCTypeToTypeAndValueImpl(std::string ctype, TypeAndValue& out,
                                    std::unordered_set<std::string>& visited,
                                    bool cxxBoundary)
{
        if (ctype.find("::*") != std::string::npos)
            return false;
        // Detect function-pointer spelling before the '*'-strip path mangles it. The declarator
        // may carry cv/nullability words between '*' and ')', so do not rely on the literal
        // "(*)" substring as a precondition for the parser.
        if (ParseCFunctionPointerSpelling(ctype, out, visited, cxxBoundary))
            return true;

        // Before any '*' or qualifier is removed: a requested C++ specialization is keyed on
        // Clang's canonical spelling, inner pointers and inner const included.
        {
            bool mapped = false;
            if (TryMapCxxForeignSpelling(ctype, out, mapped)) return mapped;
        }

        // Arrays decay to a pointer; drop the '[...]' and bump the pointer level.
        int ptr = 0;
        bool nullptrT = false;   // by-value std::nullptr_t, mapped to void*
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
        const int starLevels = ptr;
        bool isReference = false;
        size_t refPos = ctype.find("&&");
        if (refPos != std::string::npos)
        {
            ctype.erase(refPos, 2);
            ++ptr;
            isReference = true;
            out.IsRvalueRef = true;
        }
        else if ((refPos = ctype.find('&')) != std::string::npos)
        {
            ctype.erase(refPos, 1);
            ++ptr;
            isReference = true;
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

        // A REFERENCE to a T** keeps the pointee: the reference is the ABI's extra level and
        // IsCxxRefToPointer already encodes it, so T** fits the two CFlat levels.
        if (ptr == 3 && isReference && starLevels == 2)
        {
            ptr = 2;
            out.IsCxxRefToPointer = true;
        }
        // 3+ levels of indirection collapse to void** - CFlat TypeAndValue has at most two pointer
        // levels. Pointers are the same ABI size on x64/x86, so calls still link correctly.
        if (ptr > 2)
        {
            out.TypeName = "void";
            out.Pointer = true;
            out.ElemPointer = true;
            return true;
        }

        /*
         * The C++20 comparison categories are ONE signed byte of representation (libc++ holds a
         * `signed char __value_`), and the standard defines their meaning as that value's
         * relation to literal 0. Map them to CFlat's i8: clang returns a one-byte trivially
         * copyable class in exactly the register a `signed char` uses, so the ABI is unchanged,
         * and the relational operators rewritten from `operator<=>` compare the result directly.
         */
        // Anchored to namespace std (plus libc++'s inline `std.__1.`): a user type that merely
        // ends in `weak_ordering` is a different class with a different layout and must not be
        // lowered to one byte.
        if (ptr == 0)
            if (std::string category = StdOrderingCategoryOf(base); !category.empty())
            {
                out.TypeName = "i8";
                // The category stays visible for `a <=> b` results and C++ overload binding.
                out.DiagnosticTypeName = std::move(category);
                return true;
            }

        // enum decays to int. struct/union by-value: look up in dataStructures for ABI lowering.
        // struct/union pointers become opaque void* (only a pointer-sized slot is needed).
        std::string mapped;
        if (enumTag || enumBackingTypes.count(base) != 0)
        {
            if (auto it = enumBackingTypes.find(base); it != enumBackingTypes.end())
            {
                out.TypeName = base;
                out.EnumBacking = it->second;
                out.IsScopedEnum = scopedEnumTypes_.count(base) != 0;
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
            // C-boundary table: differs from CxxCompactIntegerSpellingToCflat because the
            // char*_t words map to plain unsigned widths and `long` is target-dependent below.
            static const std::unordered_map<std::string, std::string> scalarMap = {
                { "void", "void" }, { "_Bool", "bool" }, { "bool", "bool" },
                { "char", "char" }, { "signed char", "i8" }, { "unsigned char", "u8" },
                { "short", "short" }, { "short int", "short" }, { "signed short", "short" },
                { "signed short int", "short" },
                { "unsigned short", "u16" }, { "unsigned short int", "u16" },
                { "int", "int" }, { "signed", "int" }, { "signed int", "int" },
                { "unsigned", "u32" }, { "unsigned int", "u32" },
                { "long long", "i64" }, { "long long int", "i64" }, { "signed long long", "i64" },
                { "signed long long int", "i64" },
                { "unsigned long long", "u64" }, { "unsigned long long int", "u64" },
                { "__int128", "i128" }, { "unsigned __int128", "u128" },
                { "float", "float" }, { "double", "double" },
                { "char8_t", "u8" }, { "char16_t", "u16" }, { "char32_t", "u32" },
            };
            // C `long` is the one scalar whose width is target-dependent: Windows is LLP64
            // (32-bit long), Linux/macOS are LP64 (64-bit long). `size_t` desugars to it.
            static const std::unordered_set<std::string> cLongSigned = {
                "long", "long int", "signed long", "signed long int" };
            static const std::unordered_set<std::string> cLongUnsigned = {
                "unsigned long", "unsigned long int" };

            // Both C and C++ retain native long identity; its width follows the target ABI.
            if (cxxBoundary && base == "char8_t")
                mapped = "c8";
            else if (cxxBoundary && base == "char16_t")
                mapped = "c16";
            else if (cxxBoundary && base == "char32_t")
                mapped = "c32";
            else if (cxxBoundary && base == "wchar_t")
                mapped = "wchar";
            else if (auto it = scalarMap.find(base); it != scalarMap.end())
                mapped = it->second;
            else if (base == "std.nullptr_t" || base == "std.__1.nullptr_t" || base == "nullptr_t")
            {
                mapped = "void";
                if (ptr == 0) { ptr = 1; nullptrT = true; }
            }
            else if (cLongSigned.count(base) > 0)
                mapped = "long";
            else if (cLongUnsigned.count(base) > 0)
                mapped = "ulong";
            else if (base == "wchar_t")
                mapped = targetWindows_ ? "u16" : "i32";
            else if (base == "long double")
            {
                if (IsCInteropLongDoubleSupported()) mapped = "longdouble";
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
                    return MapCTypeToTypeAndValueImpl(std::move(substituted), out, visited,
                                                      cxxBoundary);
                }
                return false;    // struct/union by value or unknown scalar
            }
        }

        out.TypeName = mapped;
        out.Pointer = ptr >= 1;
        out.ElemPointer = ptr == 2;
        out.IsCxxNullptrT = nullptrT;
        return true;
    }

bool LLVMBackend::HashFileFnv1a(const std::string& path, uint64_t& outHash)
{
        llvm::TimeTraceScope hashScope("CHeaderFileHash", path);
        auto bufOrErr = llvm::MemoryBuffer::getFile(path);
        if (!bufOrErr) return false;
        const llvm::StringRef bytes = (*bufOrErr)->getBuffer();
        outHash = Fnv1aBytes(std::string_view(bytes.data(), bytes.size()));
        return true;
    }

// Preserve include order and stamps while sharing buffer hashes with early closure capture.
void LLVMBackend::CollectCHeaderDependencies(const std::vector<std::string>& paths,
                                              const std::unordered_map<std::string, std::pair<uint64_t, int64_t>>& known,
                                              std::vector<CHeaderDep>& deps)
{
        llvm::TimeTraceScope depsScope("CHeaderDependencies");
        PrefetchDependencyDirs(paths);
        // Each distinct include is stamped once, a few at a time; the loop below keeps
        // the include order.
        std::unordered_map<std::string_view, size_t> stampSlot;
        std::vector<std::string_view> stampPaths;
        for (const auto& inc : paths)
            if (stampSlot.emplace(inc, stampPaths.size()).second) stampPaths.push_back(inc);
        std::vector<std::optional<int64_t>> stamps(stampPaths.size());
        RunParallel(stampPaths.size(), stampPaths.size() < 64 ? 1u : 4u,
                    [&](size_t i, unsigned) {
            std::error_code dec;
            const auto dm = std::filesystem::last_write_time(
                std::filesystem::path(stampPaths[i]), dec);
            if (!dec) stamps[i] = (int64_t)dm.time_since_epoch().count();
        });
        std::unordered_set<std::string> seen;
        for (const auto& inc : paths)
        {
            const std::optional<int64_t>& stamp = stamps[stampSlot.at(inc)];
            if (!stamp) continue;
            if (!seen.insert(inc).second) continue;
            CHeaderDep dep;
            dep.path  = inc;
            dep.mtime = *stamp;
            deps.push_back(std::move(dep));
        }
        HashCHeaderDependencies(deps, known);
}

// Hash only dependencies whose bytes were unavailable in clang's buffers or the primary hash.
void LLVMBackend::HashCHeaderDependencies(std::vector<CHeaderDep>& deps,
                                          const std::unordered_map<std::string, std::pair<uint64_t, int64_t>>& known)
{
        llvm::TimeTraceScope hashScope("CHeaderDependencyHashes");
        std::vector<size_t> unread;
        for (size_t i = 0; i < deps.size(); ++i)
        {
            const auto hit = known.find(deps[i].path);
            if (hit != known.end())
            {
                deps[i].hash = hit->second.first;
                deps[i].hashTime = hit->second.second;
            }
            else unread.push_back(i);
        }
        RunParallel(unread.size(), unread.size() < 32 ? 1u : 4u, [&](size_t i, unsigned) {
            deps[unread[i]].hashTime = (int64_t)std::filesystem::file_time_type::clock::now()
                                           .time_since_epoch().count();
            HashFileFnv1a(deps[unread[i]].path, deps[unread[i]].hash);
        });
}

bool LLVMBackend::HashFileContents(const std::string& path, uint64_t& outHash) const
{
        return HashFileFnv1a(path, outHash);
    }

// Parentheses inside a class template argument (for example, std::function<int(int)>)
// are part of that class type, not a function declarator at the C++ boundary.

/*
 * Deferred binding gate. A C++ import declares every free function its umbrella header reaches, so
 * the types those signatures name are no longer instantiated eagerly (each instantiation is a pair
 * of full re-parses). A signature is declared here only when every class it names is already
 * registered; otherwise it stays in cxxFunctionSignatures_ and TryBindCxxFunction requests the
 * missing types the first time CFlat code names the function.
 */
bool LLVMBackend::CxxSignatureTypesRegistered(const CSigEntry& e)
{
        auto known = [&](const std::string& spelling) {
            std::string identity, named;
            const int kind = ClassifyCxxSignatureSpelling(spelling, nullptr, identity, named);
            if (kind != 1 && kind != 2) return true;   // 0 = nothing to request, 3 = unmappable
            return IsCxxForeignTypeRegistered(identity) || IsDataStructure(identity);
        };
        if (!known(e.retSpelling)) return false;
        for (const std::string& spelling : e.paramSpellings)
            if (!known(spelling)) return false;
        return true;
}

// A C interop declaration of `sig`'s linkage symbol is in the function table under `regName`
// with the same parameter and return types.
bool LLVMBackend::IsCxxDeclarationRegistered(const std::string& regName, const CSigEntry& sig) const
{
        auto it = functionTable.find(regName);
        if (it == functionTable.end()) return false;
        auto sameType = [](const TypeAndValue& a, const TypeAndValue& b) {
            return a.TypeName == b.TypeName && a.Pointer == b.Pointer
                && a.PointerDepth == b.PointerDepth && a.IsAlias == b.IsAlias;
        };
        return std::any_of(it->second.begin(), it->second.end(), [&](const FunctionSymbol& sym) {
            if (!sym.IsCInteropDeclaration || sym.UniqueName != sig.linkageName) return false;
            if (!sameType(sym.ReturnType, sig.ret)
                || sym.Parameters.size() != sig.params.size())
                return false;
            for (size_t i = 0; i < sig.params.size(); ++i)
                if (!sameType(sym.Parameters[i], sig.params[i])) return false;
            return true;
        });
}

/*
 * Register every namespace prefix of a dotted C++ declaration name, and note its leading segment
 * as a foreign namespace of the active import group. Used by the sites that DROP a declaration:
 * the namespace is a fact of the header, not a consequence of one declaration binding.
 */
void LLVMBackend::SeedCxxNamespacesOfDottedName(const std::string& dottedName)
{
        const size_t firstDot = dottedName.find('.');
        if (firstDot == std::string::npos || firstDot == 0) return;
        NoteCxxForeignNamespace(dottedName);
        for (size_t pos = firstDot; pos != std::string::npos; pos = dottedName.find('.', pos + 1))
            RegisterNamespace(dottedName.substr(0, pos));
        if (activeCxxRequestGroup_ != nullptr
            && activeCxxRequestGroup_->primary < cxxImportGroups_.size())
            cxxImportGroups_[activeCxxRequestGroup_->primary].namespaces.insert(
                dottedName.substr(0, firstDot));
}

/*
 * A spelling that IS a std::function, not one that merely CONTAINS one: SplitStdFunctionSpelling
 * finds `std::function<` anywhere, so `std::vector<std::function<int(int)>>` and
 * `std::function<int(int)>*` would otherwise be retyped as the specialization itself. cv and
 * reference decoration is stripped; whatever is left must be the specialization and nothing else.
 */
bool LLVMBackend::IsTopLevelStdFunctionSpelling(const std::string& spelling)
{
        std::string bare = spelling;
        auto trim = [](std::string& t) {
            while (!t.empty() && std::isspace((unsigned char)t.back())) t.pop_back();
            while (!t.empty() && std::isspace((unsigned char)t.front())) t.erase(t.begin());
        };
        auto stripSuffix = [&](const char* word) {
            const size_t n = std::strlen(word);
            if (bare.size() <= n || bare.compare(bare.size() - n, n, word) != 0) return false;
            if (!std::isspace((unsigned char)bare[bare.size() - n - 1])) return false;
            bare.erase(bare.size() - n);
            return true;
        };
        for (bool more = true; more; )
        {
            trim(bare);
            more = false;
            while (!bare.empty() && bare.back() == '&') { bare.pop_back(); trim(bare); more = true; }
            if (stripSuffix("const") || stripSuffix("volatile")) more = true;
            if (bare.rfind("const ", 0) == 0)    { bare.erase(0, 6); more = true; }
            if (bare.rfind("volatile ", 0) == 0) { bare.erase(0, 9); more = true; }
        }
        return bare.rfind("std::function<", 0) == 0 && !bare.empty() && bare.back() == '>';
}

bool LLVMBackend::IsStdFunctionTypeName(const std::string& spelling)
{
        std::string cxxSpelling = spelling;
        if (cxxSpelling.rfind("std.function<", 0) == 0)
            cxxSpelling.replace(0, 12, "std::function");
        return IsTopLevelStdFunctionSpelling(cxxSpelling);
}

/*
 * A `std::function<R(Args)>` spelling names the std.function specialization over the equivalent
 * CFlat closure type. Returns that specialization's CFlat name, or "" when the spelling is not a
 * std::function, when its signature types have no CFlat mapping, or when the request failed.
 * A spelling that merely CONTAINS a std::function is not one in any position - member, return or
 * free parameter - so the top-level anchor is unconditional here.
 */
std::string LLVMBackend::StdFunctionSpecializationForSpelling(const std::string& spelling,
                                                              std::string* requestError)
{
        if (requestError != nullptr) requestError->clear();
        if (!IsTopLevelStdFunctionSpelling(spelling)) return {};
        std::string retText;
        std::string paramsText;
        if (!cflat_cinterop::SplitStdFunctionSpelling(spelling, retText, paramsText)) return {};
        TypeAndValue ret;
        if (!MapCTypeToTypeAndValue(retText, ret, /*isCxx*/ true)) return {};
        TypeAndValue closure;
        closure.IsFunctionPointer = true;
        closure.TypeName = "__c_fn_ptr";
        closure.FuncPtrReturnTypeName = ret.TypeName;
        closure.FuncPtrReturnPointer = ret.Pointer;
        closure.FuncPtrReturnPointerDepth = ret.ValuePointerDepth();
        size_t start = 0;
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
            if ((c != ',' || angleDepth != 0 || parenDepth != 0) && !atEnd) continue;
            std::string one = paramsText.substr(start, pos - start);
            while (!one.empty() && std::isspace((unsigned char)one.front())) one.erase(one.begin());
            while (!one.empty() && std::isspace((unsigned char)one.back())) one.pop_back();
            if (!one.empty())
            {
                TypeAndValue pv;
                if (!MapCTypeToTypeAndValue(one, pv, /*isCxx*/ true)) return {};
                TypeAndValue::FuncPtrParam fp;
                fp.TypeName = pv.TypeName;
                fp.Pointer = pv.Pointer;
                fp.PointerDepth = pv.ValuePointerDepth();
                closure.FuncPtrParams.push_back(std::move(fp));
            }
            start = pos + 1;
        }
        std::vector<std::pair<std::string, int>> parts;
        std::vector<bool> partAliases;
        partAliases.reserve(partAliases.size() + closure.FuncPtrParams.size());
        for (const auto& fp : closure.FuncPtrParams)
            partAliases.push_back(fp.IsAlias);
        parts.reserve(parts.size() + closure.FuncPtrParams.size());
        for (const auto& fp : closure.FuncPtrParams)
            parts.push_back({ fp.TypeName, fp.PointerDepth });
        const std::string encoded = MangleClosureType(*this, true, closure.FuncPtrReturnTypeName,
                                                      closure.FuncPtrReturnPointerDepth, parts,
                                                      -1, nullptr, nullptr, &partAliases,
                                                      closure.FuncPtrReturnAlias);
        RegisterEncodedClosureType(encoded, closure);
        std::string functionTypeArg = encoded;
        functionTypeArg.replace(0, 3, "barefn");
        RegisterEncodedClosureType(functionTypeArg, closure);
        std::string className = MangleGenericInstance(
            *this, "std.function", { functionTypeArg });
        // The request is memoized per identity (RequestCxxForeignType), so asking again for an
        // already-registered specialization costs a map lookup - and skipping it would NOT be
        // equivalent: registration alone leaves the specialization without the definitions a use
        // site needs.
        std::string error;
        if (RequestCxxType("std.function", { functionTypeArg }, className, error)) return className;
        if (requestError != nullptr)
            *requestError = error.empty()
                ? "cannot request the C++ std.function specialization" : error;
        return {};
}

const LLVMBackend::TypeAndValue* LLVMBackend::StdFunctionClosureSignature(
    const std::string& name) const
{
        const std::string resolved = ResolveTypeAlias(name);
        if (!resolved.starts_with("std.function$")) return nullptr;
        const TypeAndValue* signature =
            GetEncodedClosureType(MangledGenericArgument(*this, resolved, 0));
        return signature != nullptr && signature->IsFunctionPointer ? signature : nullptr;
}

bool LLVMBackend::ResetMovedStdFunctionSource(const std::string& typeName,
        llvm::Value* sourceStorage, const std::string& sourceName)
{
        if (StdFunctionClosureSignature(typeName) == nullptr || sourceStorage == nullptr)
            return true;
        auto* destructor = GetOrCreateCxxClassDestructor(typeName);
        if (destructor != nullptr)
            builder->CreateCall(destructor->getFunctionType(), destructor, { sourceStorage });
        const auto* defaultCtor = FindCxxDefaultCtor(typeName);
        if (defaultCtor == nullptr)
        {
            LogError(std::format("cannot reset moved C++ std.function '{}' to its empty state: "
                                 "the default constructor is not accessible", DisplayCxxClassName(typeName)));
            return false;
        }
        if (!EmitCxxStructorCall(typeName, *defaultCtor, sourceStorage, {})) return false;
        const std::string name = sourceName.empty() ? FindVariableNameByStorage(sourceStorage) : sourceName;
        if (!name.empty()) MarkVariableExplicitlyMovedNull(name);
        return true;
}

bool LLVMBackend::CxxThinFunctionPointerSpelling(const TypeAndValue& type, std::string& out,
                                                bool closureAbi) const
{
        // A stored function<> argument may carry its signature with IsFunctionPointer unset
        // (see TypeAndValue::FuncPtrReturnTypeName); the thin TypeName still identifies it.
        if (!type.IsThinFnPtr() || (!type.IsFunctionPointer && type.FuncPtrReturnTypeName.empty()))
            return false;
        // Depth 0 means "not recorded" (see FuncPtrReturnPointerDepth): fall back to Pointer.
        auto spell = [&](const std::string& name, bool pointer, int depth, std::string& result) {
            if (name.empty()) return false;
            if (closureAbi && (name == "string" || name == "__cflat_move_string_arg"))
            {
                result = name == "string" ? "__cflat_string_arg" : name;
                if (pointer)
                    for (int i = 0; i < std::max(depth, 1); ++i) result += " *";
                return true;
            }
            std::string cflat = name;
            const int stars = depth > 0 ? depth : (pointer ? 1 : 0);
            for (int i = 0; i < stars; ++i) cflat += "*";
            return CxxSpellingForCflatType(cflat, result);
        };
        std::string ret;
        const std::string retName = type.FuncPtrReturnTypeName.empty()
            ? std::string("void") : type.FuncPtrReturnTypeName;
        if (!spell(retName, type.FuncPtrReturnPointer, type.FuncPtrReturnPointerDepth, ret))
            return false;
        if (type.FuncPtrReturnAlias) ret += " &";
        std::string params;
        for (const auto& param : type.FuncPtrParams)
        {
            std::string one;
            if (!spell(param.IsMove && closureAbi && param.TypeName == "string"
                           ? std::string("__cflat_move_string_arg") : param.TypeName,
                       param.Pointer, param.PointerDepth, one))
                return false;
            // CFlat drops const; a spelled `const T*` parameter keeps it for clang.
            if (param.IsPointeeConst && one.find('(') == std::string::npos
                && !one.starts_with("const "))
                one = "const " + one;
            if (param.IsAlias) one += " &";
            else if (param.IsRvalueRef) one += " &&";
            if (!params.empty()) params += ", ";
            params += one;
        }
        if (type.FuncPtrVariadic)
        {
            if (!params.empty()) params += ", ";
            params += "...";
        }
        out = "__typeof__(" + ret + " (*)(" + params + "))";
        return true;
}

bool LLVMBackend::CxxRecordStoresForwardedClosureReference(const std::string& typeName,
                                                           bool closureProjectionInResult) const
{
        std::unordered_set<std::string> visited;
        auto closurePointerOrReference = [](const std::string& spelling) {
            size_t closure = spelling.find("__cflat_closure<");
            while (closure != std::string::npos)
            {
                size_t depth = 0;
                size_t close = std::string::npos;
                for (size_t i = spelling.find('<', closure); i < spelling.size(); ++i)
                {
                    if (spelling[i] == '<') ++depth;
                    else if (spelling[i] == '>' && --depth == 0)
                    {
                        close = i;
                        break;
                    }
                }
                if (close == std::string::npos) return false;
                size_t next = close + 1;
                while (next < spelling.size() && std::isspace((unsigned char)spelling[next])) ++next;
                if (next < spelling.size() && (spelling[next] == '&' || spelling[next] == '*'))
                    return true;
                closure = spelling.find("__cflat_closure<", close + 1);
            }
            return false;
        };
        /*
         * Visible (flattened) fields: a raw T* is an OWNED copy only inside a standard-library owning
         * template the extractor identified from clang's decl (shared_ptr<T> incl. MSVC's
         * _Ptr_base<T>::_Ptr, unique_ptr<T, default_delete<T>>, vector<T>, optional<T>). A user
         * template's destructor is no ownership evidence. Blob facts were filtered at extraction too,
         * and this std-owner set also covers a blob base or member reached under the owner.
         */
        auto stripTag = [&](std::string spelling) {
            for (const char* tag : { "class ", "struct ", "union " })
                if (spelling.starts_with(tag)) spelling.erase(0, std::strlen(tag));
            return SqueezeCxxSpelling(spelling);
        };
        auto addOwnedArgs = [&](const CRecordEntry& record, std::set<std::string>& owned) {
            if (!record.stdOwnedType.empty()) owned.insert(stripTag(record.stdOwnedType));
        };
        auto ownedPointer = [&](const std::string& pointerSpelling, const std::set<std::string>& owned) {
            const std::string squeezed = SqueezeCxxSpelling(pointerSpelling);
            return !owned.empty() && squeezed.ends_with('*')
                && owned.count(stripTag(squeezed.substr(0, squeezed.size() - 1))) != 0;
        };
        std::function<bool(const std::string&, std::set<std::string>)> contains =
            [&](const std::string& name, std::set<std::string> owned) -> bool {
            auto record = cxxRecordEntries_.find(name);
            if (record == cxxRecordEntries_.end()) return false;
            addOwnedArgs(record->second, owned);
            std::string key = name;
            for (const std::string& arg : owned) key += "|" + arg;
            if (!visited.insert(key).second) return false;
            for (const auto& base : record->second.bases)
                if (contains(base.name, owned)) return true;
            // An opaque blob keeps no fields; clang's layout facts ("R:"/"P:" + immediate pointee)
            // stand in. Only a pointee that IS the closure type counts, never a template argument.
            for (const std::string& held : record->second.blobIndirections)
            {
                const std::string_view full = std::string_view(held).substr(held.size() > 2 ? 2 : held.size());
                std::string_view pointee = full;   // cv-qualified pointee: the closure type underneath
                while (pointee.starts_with("const ") || pointee.starts_with("volatile "))
                    pointee.remove_prefix(pointee.find(' ') + 1);
                if (pointee.starts_with("__cflat_closure<") && pointee.ends_with('>'))
                {
                    size_t depth = 0, close = std::string_view::npos;
                    for (size_t i = pointee.find('<'); i < pointee.size(); ++i)
                        if (pointee[i] == '<') ++depth;
                        else if (pointee[i] == '>' && --depth == 0) { close = i; break; }
                    // A std owner's base can itself be a blob (MSVC std::_Ptr_base<T> under shared_ptr<T>).
                    if (close == pointee.size() - 1
                        && !(held.starts_with("P:") && ownedPointer(std::string(full) + "*", owned)))
                        return true;
                }
            }
            for (const CRecordFieldEntry& field : record->second.fields)
            {
                std::string fieldType = field.ctype;
                if (const size_t array = fieldType.find('['); array != std::string::npos)
                    fieldType.erase(array);
                if (ownedPointer(fieldType, owned)) continue;
                if (closurePointerOrReference(field.ctype))
                    return true;
                const std::string squeezedFieldType = SqueezeCxxSpelling(fieldType);
                if (closureProjectionInResult && fieldType.find('(') == std::string::npos
                    && squeezedFieldType.find("void*") != std::string::npos)
                    return true;
                for (const auto& [nestedName, nested] : cxxRecordEntries_)
                    if (!nested.canonicalCtype.empty()
                        && SqueezeCxxSpelling(nested.canonicalCtype) == squeezedFieldType
                        && contains(nestedName, owned))
                        return true;
            }
            return false;
        };
        return contains(typeName, {});
}

bool LLVMBackend::LowerNonCapturingClosureForCxx(NamedVariable& arg, const TypeAndValue* target)
{
        // A generic instantiation named as a value (`dbl<int>`) resolves through its mangled name to
        // the one function it denotes; an overload set stays unresolved (refused as before).
        if (arg.GenericFunctionValue && arg.Primary == nullptr && arg.Storage == nullptr
            && arg.TypeAndValue.TypeName.empty() && !arg.CallerName.empty())
        {
            auto overloads = functionTable.find(ResolveQualifiedName(arg.CallerName));
            if (overloads == functionTable.end() || overloads->second.size() != 1) return false;
            NamedVariable resolved = arg;
            resolved.Primary = GetFunctionForFuncPtr(arg.CallerName);
            resolved.GenericFunctionValue = false;
            if (resolved.Primary == nullptr || !LowerNonCapturingClosureForCxx(resolved, target))
                return false;
            arg = resolved;
            return true;
        }
        // A bare function NAME (`add3`) is its llvm::Function with no type yet: it decays to a
        // function pointer, as a C++ function name does. An overload set has no single signature.
        if (auto* fn = llvm::dyn_cast_or_null<llvm::Function>(arg.Primary);
            fn != nullptr && !arg.TypeAndValue.IsFunctionPointer && arg.TypeAndValue.TypeName.empty()
            && !arg.CallerName.empty() && arg.Storage == nullptr)
        {
            auto overloads = functionTable.find(ResolveQualifiedName(arg.CallerName));
            if (overloads != functionTable.end() && overloads->second.size() > 1) return false;
            // An `alias` param has no function-pointer spelling (same door as a function<> bind).
            const FunctionSymbol* fnSym = FindSymbolForFunction(fn);
            if (fnSym != nullptr
                && RejectFuncPtrBind(arg.CallerName, *fnSym, target, nullptr)) return false;
            TypeAndValue thin = FuncPtrSigOfBoundFunction(arg.CallerName, fn);
            if (!thin.IsFunctionPointer) return false;
            // std::function's rule, not function<>'s exact one: the call must be valid with
            // implicit conversions. A converting signature binds through a thunk of the target's.
            if (target != nullptr && target->IsFunctionPointer)
            {
                std::string refusal;
                llvm::Function* bridge = StdFunctionConvertingThunk(arg.CallerName, fn,
                    fnSym != nullptr ? FuncPtrSigOfSymbol(*fnSym) : thin, *target, refusal);
                if (!refusal.empty())
                {
                    LogError(refusal);
                    return false;
                }
                if (bridge != nullptr)
                {
                    fn = bridge;
                    thin = *target;
                    thin.TypeName = "__c_fn_ptr";
                }
                else if (fnSym != nullptr
                    && CxxCallableNeedsRawArrayCountAdapter(
                        FuncPtrSigOfSymbol(*fnSym), *target))
                {
                    thin = *target;
                    thin.TypeName = "__c_fn_ptr";
                }
            }
            arg.Primary = MakeThinFnPtrValue(fn, thin, target != nullptr);
            arg.BaseType = arg.Primary->getType();
            arg.TypeAndValue = thin;
            arg.IsRvalue = true;
            return true;
        }
        if (!arg.TypeAndValue.IsFunctionPointer || arg.TypeAndValue.IsThinFnPtr()) return false;
        llvm::Value* fat = arg.Primary;
        if (fat == nullptr && arg.Storage != nullptr) return false;   // a named Lambda<> stays fat
        if (fat == nullptr || fat->getType() != GetClosureFatPtrType()) return false;
        if (!ClosureIsStaticallyNonCapturing(fat)) return false;
        TypeAndValue thin = arg.TypeAndValue;
        thin.TypeName = "__c_fn_ptr";
        UnregisterOwnedClosureTemp(fat);
        arg.Primary = CoerceClosureFatToThin(fat, thin);
        arg.BaseType = arg.Primary->getType();
        arg.TypeAndValue = thin;
        arg.IsRvalue = true;
        return true;
}

llvm::Function* LLVMBackend::StdFunctionConvertingThunk(const std::string& name, llvm::Function* fn,
                                                       const TypeAndValue& fnSig,
                                                       const TypeAndValue& target,
                                                       std::string& refusal)
{
        std::vector<FuncPtrComponent> sa, sb;
        if (!FuncPtrSignatureOf(fnSig, sa) || !FuncPtrSignatureOf(target, sb)) return nullptr;
        const std::string shown = SpellFunctionSymbol(*this, name);
        auto mismatch = [&](const std::string& why) {
            refusal = std::format("cannot bind function '{}' to a C++ callable of type '{}': '{}' is "
                                  "'{}' - {}",
                shown, FuncPtrSpellingOf(target), shown, FuncPtrSpellingOf(fnSig), why);
            return nullptr;
        };
        if (sa.size() != sb.size())
            return mismatch(std::format("it takes {} parameter(s) but the call supplies {}",
                                        sa.size() - 1, sb.size() - 1));
        auto arithmetic = [](const FuncPtrComponent& c) {
            return c.Known && !c.Canon.empty() && (c.Canon[0] == 'i' || c.Canon[0] == 'f'
                                                   || c.Canon == "b");
        };
        bool converts = false;
        for (size_t i = 0; i < sa.size(); ++i)
        {
            // A void target discards whatever the function returns.
            if (i == 0 && sb[0].Known && sb[0].Canon == "v")
            {
                // Only a value with nothing to destroy can be dropped by the thunk.
                const bool voidReturn = sa[0].Known && sa[0].Canon == "v";
                if (!voidReturn && !arithmetic(sa[0]))
                    return mismatch("the discarded return value needs destruction");
                converts = converts || !voidReturn;
                continue;
            }
            if (!ComponentsProvablyDiffer(sa[i], sb[i])) continue;
            if (!arithmetic(sa[i]) || !arithmetic(sb[i]))
                return mismatch(i == 0 ? "the return type does not convert"
                                       : std::format("parameter {} does not convert", i));
            converts = true;
        }
        if (!converts) return nullptr;

        TypeAndValue fnRet, targetRet;
        std::vector<TypeAndValue> fnParams, targetParams;
        UnpackFuncPtrSignature(fnSig, fnRet, fnParams);
        UnpackFuncPtrSignature(target, targetRet, targetParams);
        llvm::FunctionType* fnTy = fn->getFunctionType();
        llvm::FunctionType* thunkTy = GetFunctionType(targetRet, targetParams);
        // Hidden ABI parameters (sret, owned-return out-pointers) have no plain thunk shape.
        if (thunkTy == nullptr || fnTy->getNumParams() != fnParams.size()
            || thunkTy->getNumParams() != fnParams.size())
            return mismatch("its signature differs and has no converting form");
        const std::string thunkName = "__cflat_stdfn_conv." + fn->getName().str() + "."
            + FuncPtrSpellingOf(target);
        if (auto* existing = module->getFunction(thunkName);
            existing != nullptr && existing->getFunctionType() == thunkTy)
            return existing;
        auto* thunk = llvm::Function::Create(thunkTy, llvm::Function::InternalLinkage, thunkName,
                                             *module);
        llvm::IRBuilder<> b(llvm::BasicBlock::Create(*context, "entry", thunk));
        // Set when a floating -> integer adaptation is refused, so the mismatch names the cause.
        bool floatToIntRefused = false;
        auto convert = [&](llvm::Value* v, llvm::Type* to, bool srcUnsigned,
                           bool dstUnsigned) -> llvm::Value* {
            llvm::Type* from = v->getType();
            if (from == to) return v;
            if (from->isIntegerTy() && to->isIntegerTy())
            {
                if (to->getIntegerBitWidth() == 1)
                    return b.CreateICmpNE(v, llvm::ConstantInt::get(from, 0));
                return b.CreateIntCast(v, to, !srcUnsigned && from->getIntegerBitWidth() != 1);
            }
            if (from->isIntegerTy() && to->isFloatingPointTy())
                return srcUnsigned || from->getIntegerBitWidth() == 1 ? b.CreateUIToFP(v, to)
                                                                       : b.CreateSIToFP(v, to);
            if (from->isFloatingPointTy() && to->isIntegerTy())
            {
                if (to->getIntegerBitWidth() == 1)
                    return b.CreateFCmpUNE(v, llvm::ConstantFP::get(from, 0.0));
                // Implicit float -> integer adaptation is refused at binding (ruling 2026-10-13).
                floatToIntRefused = true;
                return nullptr;
            }
            if (from->isFloatingPointTy() && to->isFloatingPointTy()) return b.CreateFPCast(v, to);
            return nullptr;
        };
        std::vector<llvm::Value*> callArgs;
        for (unsigned i = 0; i < fnTy->getNumParams(); ++i)
        {
            llvm::Value* v = convert(thunk->getArg(i), fnTy->getParamType(i),
                                     targetParams[i].IsUnsignedInteger() != -1,
                                     fnParams[i].IsUnsignedInteger() != -1);
            if (v == nullptr)
            {
                thunk->eraseFromParent();
                if (floatToIntRefused)
                    return mismatch(std::format("parameter {} would convert a floating value to an integer; use an explicit cast", i + 1));
                return mismatch(std::format("parameter {} does not convert", i + 1));
            }
            callArgs.push_back(v);
        }
        auto* call = b.CreateCall(fnTy, fn, callArgs);
        call->setCallingConv(fn->getCallingConv());
        call->setAttributes(fn->getAttributes());
        if (thunkTy->getReturnType()->isVoidTy())
            b.CreateRetVoid();
        else
        {
            llvm::Value* r = convert(call, thunkTy->getReturnType(),
                                     fnRet.IsUnsignedInteger() != -1,
                                     targetRet.IsUnsignedInteger() != -1);
            if (r == nullptr)
            {
                thunk->eraseFromParent();
                if (floatToIntRefused)
                    return mismatch("the return would convert a floating value to an integer; use an explicit cast");
                return mismatch("the return type does not convert");
            }
            b.CreateRet(r);
        }
        return thunk;
}

bool LLVMBackend::IsUnwrappedFatClosure(const NamedVariable& arg) const
{
        if (!arg.CxxClosureSignature.empty()) return false;
        const bool fatType = (arg.TypeAndValue.IsFunctionPointer && !arg.TypeAndValue.IsThinFnPtr())
            || (!arg.TypeAndValue.Pointer && arg.TypeAndValue.TypeName == "__closure_fat_ptr");
        const bool fatValue = arg.Primary != nullptr
            && arg.Primary->getType() == GetClosureFatPtrType();
        return fatType || fatValue;
}

const char* LLVMBackend::CxxClosureWrapperPreamble()
{
        // The CFlat closure ABI is env-last with the ownership tag bit cleared: code(args..., env).
        // Copy, move and destroy go through CFlat closure routines (EnsureCxxClosureEnvHelpers).
        // No include guard (an incremental group parses declaration by declaration): an
        // incremental group remembers it as a prefix source, as it does kCxxWrapperPidDecl.
        // A CFlat string argument (`split`) is passed as its two fields, as the natural ABI
        // passes a first-class {ptr, i32}; MS x64 would pass the 16-byte struct by pointer.
        return "#include <type_traits>\n#include <tuple>\n"
               "extern \"C\" void* __cflat_closure_env_copy(void*);\n"
               "extern \"C\" void* __cflat_closure_env_move(void**);\n"
               "extern \"C\" void __cflat_closure_env_destroy(void*);\n"
               "template <class A> struct __cflat_closure_arg { using type = A; static constexpr bool split = false; "
               "template <class U> static A make(U&& value) { return static_cast<A>(static_cast<U&&>(value)); } "
               "template <class U> static std::tuple<A> lower(U&& value) { return std::tuple<A>(make(static_cast<U&&>(value))); } };\n"
               "template <class R> struct __cflat_closure_return { using type = R; "
               "static R convert(R value) { return value; } };\n"
               "template <> struct __cflat_closure_return<void> { using type = void; };\n"
               "template <class P> class __cflat_closure;\n"
               "template <class T> struct __cflat_is_closure_value : std::false_type {};\n"
               "template <class P> struct __cflat_is_closure_value<__cflat_closure<P>> : std::true_type {};\n"
               "template <class R, class... A> class __cflat_closure<R (*)(A...)> {\n"
               "    // Same layout as the CFlat closure pair {code, env}: a CFlat closure variable IS this\n"
               "    // object, so `F &` binds the caller's own slot. Ownership is the env's tag bit.\n"
               "    void* code_; void* env_;\n"
               "public:\n"
               "    explicit __cflat_closure(void* fat) noexcept : code_(static_cast<void**>(fat)[0]),\n"
               "        env_(static_cast<void**>(fat)[1]) {}\n"
               "    __cflat_closure(const __cflat_closure& o) : code_(o.code_),\n"
               "        env_(o.env_ != nullptr ? __cflat_closure_env_copy(o.env_) : nullptr) {}\n"
               "    __cflat_closure(__cflat_closure&& o) : code_(o.code_), env_(o.env_) {\n"
               "        if (env_ != nullptr) env_ = __cflat_closure_env_move(&o.env_); }\n"
               "    __cflat_closure& operator=(const __cflat_closure&) = delete;\n"
               "    void release_to(void* out) noexcept { void** p = static_cast<void**>(out); "
               "p[0] = code_; p[1] = env_; env_ = nullptr; }\n"
               "    ~__cflat_closure() { if (env_ != nullptr) __cflat_closure_env_destroy(env_); }\n"
               "    template <class... U> typename __cflat_closure_return<R>::type operator()(U&&... a) const {\n"
               "        void* env = reinterpret_cast<void*>(\n"
               "            reinterpret_cast<__UINTPTR_TYPE__>(env_) & ~static_cast<__UINTPTR_TYPE__>(1));\n"
               "        if constexpr ((__cflat_closure_arg<A>::split || ... || false)) {\n"
               "            auto lowered = std::tuple_cat(__cflat_closure_arg<A>::lower(static_cast<U&&>(a))..., std::tuple<void*>(env));\n"
               "            return std::apply([this](auto&&... v) -> typename __cflat_closure_return<R>::type {\n"
               "                auto call = reinterpret_cast<R (*)(std::remove_reference_t<decltype(v)>...)>(code_);\n"
               "                if constexpr (std::is_void_v<R>) { call(static_cast<decltype(v)&&>(v)...); }\n"
               "                else return __cflat_closure_return<R>::convert(call(static_cast<decltype(v)&&>(v)...));\n"
               "            }, std::move(lowered));\n"
               "        } else {\n"
               "        auto call = reinterpret_cast<R (*)(typename __cflat_closure_arg<A>::type..., void*)>(code_);\n"
               "        if constexpr (std::is_void_v<R>) { call(__cflat_closure_arg<A>::make(static_cast<U&&>(a))..., env); }\n"
               "        else return __cflat_closure_return<R>::convert(call(__cflat_closure_arg<A>::make(static_cast<U&&>(a))..., env));\n"
               "        }\n"
               "    }\n"
               "};\n";
}

const char* LLVMBackend::CxxClosureStringWrapperPreamble()
{
        return "#include <string>\n#include <cstring>\n"
               "extern \"C\" void* __cflat_closure_string_alloc(unsigned long long);\n"
               "extern \"C\" void __cflat_closure_string_free(void*);\n"
               "struct __cflat_string_arg { const char* _ptr; int _len; };\n"
               "struct __cflat_move_string_arg {};\n"
               "template <> struct __cflat_closure_arg<__cflat_string_arg> { using type = __cflat_string_arg; static constexpr bool split = true; "
               "template <class U> static type make(U&& value) { return { value.data(), static_cast<int>(value.size()) }; } "
               "template <class U> static std::tuple<const char*, int> lower(U&& value) { type s = make(static_cast<U&&>(value)); return { s._ptr, s._len }; } };\n"
               "template <> struct __cflat_closure_arg<__cflat_move_string_arg> { using type = __cflat_string_arg; static constexpr bool split = true; "
               "template <class U> static type make(U&& value) { unsigned long long n = value.size(); "
               "char* p = static_cast<char*>(__cflat_closure_string_alloc(n + 1)); "
               "std::memcpy(p, value.data(), n); p[n] = 0; "
               "return { p, static_cast<int>(n) | static_cast<int>(0x80000000u) }; } "
               "template <class U> static std::tuple<const char*, int> lower(U&& value) { type s = make(static_cast<U&&>(value)); return { s._ptr, s._len }; } };\n"
               "template <> struct __cflat_closure_return<__cflat_string_arg> { using type = std::string; "
               "static type convert(__cflat_string_arg value) { unsigned int n = static_cast<unsigned int>(value._len); "
               "bool owned = (n & 0x80000000u) != 0; n &= 0x7fffffffu; "
               "type result(value._ptr == nullptr ? \"\" : value._ptr, n); "
               "if (owned && value._ptr != nullptr) __cflat_closure_string_free(const_cast<char*>(value._ptr)); "
               "return result; } };\n";
}

std::string LLVMBackend::CxxClosureCallArgument(const NamedVariable& arg, const std::string& param)
{
        if (arg.CxxClosureSignature.empty()) return {};
        const std::string type = "__cflat_closure<" + arg.CxxClosureSignature + ">";
        // A closure variable is passed as its own slot (an lvalue of the caller's frame, as in C++);
        // a temporary is moved into the owning prvalue the wrapper builds.
        if (arg.CxxClosureBorrowed) return "(*static_cast<" + type + " *>(" + param + "))";
        return type + "(" + param + ")";
}

// A wrapper result spelled as a reference or pointer to the generated closure object itself.


std::string LLVMBackend::FindLiveCxxClosureLender(llvm::Value* storage) const
{
        for (auto frame = stackNamedVariable.rbegin(); frame != stackNamedVariable.rend(); ++frame)
        {
            auto find = [&](const auto& variables) -> std::string {
                for (const auto& [name, variable] : variables)
                    for (const auto& lender : variable.CxxClosureLenders)
                        if (lender.first == storage) return lender.second;
                return {};
            };
            if (std::string lender = find(frame->namedVariable); !lender.empty()) return lender;
            if (std::string lender = find(frame->functionArgument); !lender.empty()) return lender;
        }
        return {};
}

bool LLVMBackend::ClosureSlotHasOnlyTrivialCaptures(llvm::Value* slot) const
{
        if (slot == nullptr || !slot->getType()->isPointerTy()) return false;
        auto isClosureEnvNew = [&](const llvm::Function* function) {
            if (auto helpers = functionTable.find("__closure_env_new"); helpers != functionTable.end())
                for (const auto& symbol : helpers->second)
                    if (symbol.Function == function) return true;
            return module->getFunction("__closure_env_new") == function;
        };
        std::unordered_set<llvm::Value*> visitedValues;
        std::function<bool(llvm::Value*)> trivialEnv;
        std::function<bool(llvm::Value*)> trivialFat;
        std::function<bool(llvm::Value*)> trivialSlot;
        auto envFieldOffset = [&](llvm::GetElementPtrInst* gep) -> std::optional<int64_t> {
            llvm::APInt offset(64, 0);
            if (!gep->accumulateConstantOffset(module->getDataLayout(), offset)) return std::nullopt;
            return offset.getSExtValue();
        };
        trivialEnv = [&](llvm::Value* env) {
            if (env == nullptr) return false;
            if (llvm::isa<llvm::ConstantPointerNull>(env)) return true;
            if (auto* function = llvm::dyn_cast<llvm::Function>(env->stripPointerCasts());
                function != nullptr && function->getName().starts_with("__cflat_reverse_abi_"))
                return true;
            if (!visitedValues.insert(env).second) return false;
            if (auto* load = llvm::dyn_cast<llvm::LoadInst>(env))
                return trivialSlot(load->getPointerOperand());
            if (auto* phi = llvm::dyn_cast<llvm::PHINode>(env))
            {
                if (phi->getNumIncomingValues() == 0) return false;
                for (llvm::Value* incoming : phi->incoming_values())
                    if (!trivialEnv(incoming)) return false;
                return true;
            }
            if (auto* select = llvm::dyn_cast<llvm::SelectInst>(env))
                return trivialEnv(select->getTrueValue()) && trivialEnv(select->getFalseValue());
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(env);
                call != nullptr && call->getCalledFunction() != nullptr)
            {
                if (isClosureEnvNew(call->getCalledFunction()) && call->arg_size() == 2)
                    return llvm::isa<llvm::ConstantPointerNull>(call->getArgOperand(1));
                if (IsClosureHelperName(call->getCalledFunction()->getName(), "__cflat_closure_env_copy")
                    && call->arg_size() == 1)
                    return trivialEnv(call->getArgOperand(0));
            }
            if (auto* extract = llvm::dyn_cast<llvm::ExtractValueInst>(env);
                extract != nullptr && extract->getNumIndices() == 1 && extract->getIndices()[0] == 1)
            {
                llvm::Value* aggregate = extract->getAggregateOperand();
                if (auto* call = llvm::dyn_cast<llvm::CallBase>(aggregate);
                    call != nullptr && call->getCalledFunction() != nullptr
                    && IsClosureHelperName(call->getCalledFunction()->getName(), "__closure_fat_ptr.copy")
                    && call->arg_size() == 1)
                    return trivialFat(call->getArgOperand(0));
                return trivialFat(aggregate);
            }
            return false;
        };
        trivialFat = [&](llvm::Value* fat) {
            if (fat == nullptr || !visitedValues.insert(fat).second) return false;
            if (auto* load = llvm::dyn_cast<llvm::LoadInst>(fat))
                return trivialSlot(load->getPointerOperand());
            if (auto* call = llvm::dyn_cast<llvm::CallBase>(fat);
                call != nullptr && call->getCalledFunction() != nullptr
                && IsClosureHelperName(call->getCalledFunction()->getName(), "__closure_fat_ptr.copy")
                && call->arg_size() == 1)
                return trivialFat(call->getArgOperand(0));
            if (auto* phi = llvm::dyn_cast<llvm::PHINode>(fat))
            {
                if (phi->getNumIncomingValues() == 0) return false;
                for (llvm::Value* incoming : phi->incoming_values())
                    if (!trivialFat(incoming)) return false;
                return true;
            }
            if (auto* select = llvm::dyn_cast<llvm::SelectInst>(fat))
                return trivialFat(select->getTrueValue()) && trivialFat(select->getFalseValue());
            while (auto* insert = llvm::dyn_cast<llvm::InsertValueInst>(fat))
            {
                if (insert->getNumIndices() == 1 && insert->getIndices()[0] == 1)
                    return trivialEnv(insert->getInsertedValueOperand());
                fat = insert->getAggregateOperand();
            }
            if (auto* constant = llvm::dyn_cast<llvm::Constant>(fat))
                return constant->isNullValue()
                    || (constant->getAggregateElement(1u) != nullptr
                        && trivialEnv(constant->getAggregateElement(1u)));
            return false;
        };
        // A slot is judged at its root alloca: every write that may reach the closure at byte
        // `off` of `base` must store a trivial fat or env. -1 unsafe, 0 no write, 1 trivial write.
        const llvm::DataLayout& layout = module->getDataLayout();
        const int64_t ptrSize = (int64_t)layout.getPointerSize();
        const int64_t fatSize = 2 * ptrSize;
        std::map<std::pair<llvm::Value*, int64_t>, int> slotMemo;
        std::function<int(llvm::Value*, int64_t, int64_t)> trivialRange;
        std::function<bool(llvm::Value*)> readOnly;
        std::function<bool(llvm::Value*, int64_t)> trivialSlotAt;
        trivialSlot = [&](llvm::Value* storage) { return trivialSlotAt(storage, 0); };
        auto isDestructor = [&](llvm::Function* function) {
            if (IsClosureHelperName(function->getName(), "__closure_fat_ptr.dtor")) return true;
            for (const auto& [typeName, destructor] : fullDestructorCache_)
                if (destructor == function) return true;
            return false;
        };
        auto overlaps = [&](int64_t begin, int64_t size, int64_t off) {
            return size < 0 || (begin < off + fatSize && off < begin + size);
        };
        readOnly = [&](llvm::Value* pointer) {
            for (llvm::User* user : pointer->users())
            {
                if (auto* load = llvm::dyn_cast<llvm::LoadInst>(user);
                    load != nullptr && load->getPointerOperand() == pointer)
                    continue;
                if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(user);
                    gep != nullptr && gep->getPointerOperand() == pointer)
                {
                    if (gep->getMetadata("cflat.cxx.closure.ref") == nullptr && !readOnly(gep))
                        return false;
                    continue;
                }
                if (auto* call = llvm::dyn_cast<llvm::CallBase>(user);
                    call != nullptr && call->getCalledFunction() != nullptr
                    && isDestructor(call->getCalledFunction()))
                    continue;
                return false;
            }
            return true;
        };
        // A closure-reference adapter result (`pass(f)`) is the closure it was lent, or a copy of it:
        // judge the one closure slot the call borrowed.
        auto closureRefAdapterSource = [&](llvm::Value* value) -> llvm::Value* {
            auto* call = llvm::dyn_cast<llvm::CallBase>(value);
            if (call == nullptr || call->getCalledFunction() == nullptr
                || !call->getCalledFunction()->getName().starts_with("__cflat_tpl_closure_ref_"))
                return nullptr;
            llvm::Value* source = nullptr;
            for (llvm::Value* operand : call->args())
                if (auto* reference = llvm::dyn_cast<llvm::GetElementPtrInst>(operand);
                    reference != nullptr && reference->getMetadata("cflat.cxx.closure.ref") != nullptr)
                {
                    if (source != nullptr) return nullptr;
                    source = reference->getPointerOperand();
                }
            return source;
        };
        trivialSlotAt = [&](llvm::Value* storage, int64_t off) {
            if (storage == nullptr || !storage->getType()->isPointerTy()) return false;
            llvm::Value* root = storage;
            for (;;)
            {
                if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(root))
                {
                    const auto offset = envFieldOffset(gep);
                    if (!offset) return false;
                    off += *offset;
                    root = gep->getPointerOperand();
                }
                else if (auto* cast = llvm::dyn_cast<llvm::BitCastInst>(root))
                    root = cast->getOperand(0);
                else if (llvm::Value* lent = closureRefAdapterSource(root))
                    root = lent;
                else
                    break;
            }
            // A loaded pointer (`p->fn`) or a parameter has writers this walk cannot see.
            auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(root);
            if (alloca == nullptr) return false;
            const auto size = alloca->getAllocationSize(layout);
            return trivialRange(alloca, off,
                                size && !size->isScalable() ? (int64_t)size->getFixedValue() : -1) == 1;
        };
        trivialRange = [&](llvm::Value* base, int64_t off, int64_t extent) {
            const auto key = std::make_pair(base, off);
            if (auto it = slotMemo.find(key); it != slotMemo.end()) return it->second;
            slotMemo[key] = 0;
            int result = 0;
            auto merge = [&](int r) { result = r < 0 || result < 0 ? -1 : std::max(result, r); };
            for (llvm::User* user : base->users())
            {
                if (result < 0) break;
                if (auto* load = llvm::dyn_cast<llvm::LoadInst>(user);
                    load != nullptr && load->getPointerOperand() == base)
                    continue;
                if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(user);
                    gep != nullptr && gep->getPointerOperand() == base)
                {
                    if (gep->getMetadata("cflat.cxx.closure.ref") != nullptr || gep->use_empty()) continue;
                    llvm::Type* element = gep->getResultElementType();
                    const int64_t size = element->isSized() && !element->isIntegerTy(8)
                        ? (int64_t)layout.getTypeAllocSize(element).getFixedValue() : -1;
                    const auto offset = envFieldOffset(gep);
                    if (!offset)
                    {
                        // A variable index may alias the closure: it must only read.
                        if (!readOnly(gep)) result = -1;
                        continue;
                    }
                    if (overlaps(*offset, size, off)) merge(trivialRange(gep, off - *offset, size));
                    continue;
                }
                if (auto* store = llvm::dyn_cast<llvm::StoreInst>(user);
                    store != nullptr && store->getPointerOperand() == base
                    && store->getValueOperand() != base)
                {
                    llvm::Value* value = store->getValueOperand();
                    const int64_t size = (int64_t)layout.getTypeStoreSize(value->getType()).getFixedValue();
                    if (!overlaps(0, size, off)) continue;
                    // CFlat stores the code pointer at byte zero, the owning env at +sizeof(void*).
                    if (off == 0 && value->getType() == GetClosureFatPtrType())
                        merge(trivialFat(value) ? 1 : -1);
                    else if (off == -ptrSize && value->getType()->isPointerTy())
                        merge(trivialEnv(value) ? 1 : -1);
                    else if (off == 0 && value->getType()->isPointerTy())
                        continue;
                    else if (off >= 0 && off + fatSize <= size)
                    {
                        // A whole-record store: zero-fill, or a copy of another slot's record.
                        auto* constant = llvm::dyn_cast<llvm::Constant>(value);
                        auto* load = llvm::dyn_cast<llvm::LoadInst>(value);
                        if (constant != nullptr && constant->isNullValue()) merge(1);
                        else if (load != nullptr)
                            merge(trivialSlotAt(load->getPointerOperand(), off) ? 1 : -1);
                        else
                            result = -1;
                    }
                    else
                        result = -1;
                    continue;
                }
                if (auto* call = llvm::dyn_cast<llvm::CallBase>(user);
                    call != nullptr && call->getCalledFunction() != nullptr)
                {
                    llvm::Function* function = call->getCalledFunction();
                    if (isDestructor(function)) continue;
                    if (function->getIntrinsicID() == llvm::Intrinsic::memset && call->getArgOperand(0) == base)
                    {
                        auto* fill = llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(1));
                        auto* length = llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(2));
                        if (fill == nullptr || !fill->isZero()) result = -1;
                        else if (length == nullptr || overlaps(0, (int64_t)length->getZExtValue(), off)) merge(1);
                        continue;
                    }
                    if (function->isDeclaration()) { result = -1; continue; }
                    for (unsigned i = 0; i < call->arg_size() && result >= 0; ++i)
                        if (call->getArgOperand(i) == base)
                        {
                            // Only a callee that provably initializes the slot (a ctor) is trusted.
                            const int callee = i < function->arg_size()
                                ? trivialRange(function->getArg(i), off, extent) : -1;
                            merge(callee == 1 ? 1 : -1);
                        }
                    continue;
                }
                if (auto* cast = llvm::dyn_cast<llvm::BitCastInst>(user);
                    cast != nullptr && cast->getOperand(0) == base)
                {
                    merge(trivialRange(cast, off, extent));
                    continue;
                }
                result = -1;
            }
            slotMemo[key] = result;
            return result;
        };
        return trivialSlot(slot);
}

void LLVMBackend::EnsureCxxClosureEnvHelpers()
{
        auto ensureStringAllocators = [&]() {
            auto* i8PtrTy = cflat_llvm::PointerTo(builder->getInt8Ty());
            if (module->getFunction("__cflat_closure_string_alloc") == nullptr)
            {
                llvm::Function* allocate = GetFunction("operator new");
                if (allocate != nullptr && allocate->arg_size() == 1)
                {
                    auto* ty = llvm::FunctionType::get(i8PtrTy, { builder->getInt64Ty() }, false);
                    auto* wrapper = llvm::Function::Create(
                        ty, llvm::Function::WeakODRLinkage,
                        "__cflat_closure_string_alloc", *module);
                    llvm::IRBuilder<> b(llvm::BasicBlock::Create(*context, "entry", wrapper));
                    llvm::Value* size = wrapper->getArg(0);
                    llvm::Type* parameterType = allocate->getFunctionType()->getParamType(0);
                    if (parameterType != size->getType())
                        size = b.CreateIntCast(size, parameterType, false);
                    b.CreateRet(b.CreateCall(allocate->getFunctionType(), allocate, { size }));
                }
            }
            if (module->getFunction("__cflat_closure_string_free") == nullptr)
            {
                llvm::Function* release = GetFunction("operator delete");
                if (release != nullptr && release->arg_size() == 1)
                {
                    auto* ty = llvm::FunctionType::get(builder->getVoidTy(), { i8PtrTy }, false);
                    auto* wrapper = llvm::Function::Create(
                        ty, llvm::Function::WeakODRLinkage,
                        "__cflat_closure_string_free", *module);
                    llvm::IRBuilder<> b(llvm::BasicBlock::Create(*context, "entry", wrapper));
                    llvm::Value* pointer = wrapper->getArg(0);
                    llvm::Type* parameterType = release->getFunctionType()->getParamType(0);
                    if (parameterType != pointer->getType())
                        pointer = b.CreatePointerCast(pointer, parameterType);
                    b.CreateCall(release->getFunctionType(), release, { pointer });
                    b.CreateRetVoid();
                }
            }
        };
        ensureStringAllocators();
        if (module->getFunction("__cflat_closure_env_copy") != nullptr) return;
        EnsureClosureLifetimeRegistered();
        // The registered routines: a module restored from the core cache may carry them renamed.
        llvm::Function* copyFn = nullptr;
        if (auto copies = functionTable.find("copy"); copies != functionTable.end())
            for (const auto& symbol : copies->second)
                if (symbol.UniqueName == "__closure_fat_ptr.copy") copyFn = symbol.Function;
        auto closureData = dataStructures.find("__closure_fat_ptr");
        llvm::Function* dtorFn = closureData != dataStructures.end()
            ? closureData->second.Destructor : nullptr;
        if (copyFn == nullptr || dtorFn == nullptr) return;
        auto* closureTy = GetClosureFatPtrType();
        auto* i8PtrTy = cflat_llvm::PointerTo(builder->getInt8Ty());
        auto makeFat = [&](llvm::IRBuilder<>& b, llvm::Value* env) {
            llvm::Value* fat = llvm::UndefValue::get(closureTy);
            fat = b.CreateInsertValue(fat, llvm::ConstantPointerNull::get(i8PtrTy), { 0u });
            return b.CreateInsertValue(fat, env, { 1u });
        };
        // weak_odr: one definition survives however many CFlat modules emitted it.
        auto* copyTy = llvm::FunctionType::get(i8PtrTy, { i8PtrTy }, false);
        auto* envCopy = llvm::Function::Create(copyTy, llvm::Function::WeakODRLinkage,
                                               "__cflat_closure_env_copy", *module);
        {
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(*context, "entry", envCopy));
            llvm::Value* copied = b.CreateCall(copyFn->getFunctionType(), copyFn,
                                               { makeFat(b, envCopy->getArg(0)) });
            b.CreateRet(b.CreateExtractValue(copied, { 1u }));
        }
        auto* destroyTy = llvm::FunctionType::get(builder->getVoidTy(), { i8PtrTy }, false);
        auto* envDestroy = llvm::Function::Create(destroyTy, llvm::Function::WeakODRLinkage,
                                                  "__cflat_closure_env_destroy", *module);
        {
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(*context, "entry", envDestroy));
            auto* slot = b.CreateAlloca(closureTy);
            b.CreateStore(makeFat(b, envDestroy->getArg(0)), slot);
            b.CreateCall(dtorFn->getFunctionType(), dtorFn, { slot });
            b.CreateRetVoid();
        }
        // Move out of an owned env (C++ moving the wrapper): GetOrCreateClosureEnvMove.
        llvm::Function* envMoveImpl = GetOrCreateClosureEnvMove();
        if (envMoveImpl == nullptr) return;
        auto* envMove = llvm::Function::Create(envMoveImpl->getFunctionType(),
                                               llvm::Function::WeakODRLinkage,
                                               "__cflat_closure_env_move", *module);
        {
            llvm::IRBuilder<> b(llvm::BasicBlock::Create(*context, "entry", envMove));
            b.CreateRet(b.CreateCall(envMoveImpl->getFunctionType(), envMoveImpl,
                                     { envMove->getArg(0) }));
        }
}

std::string LLVMBackend::CxxClosureArgumentRefusal(NamedVariable& arg, const std::string& target,
                                                   const TypeAndValue* signatureHint,
                                                   bool deferReferenceCaptureCheck)
{
        if (!IsUnwrappedFatClosure(arg)) return {};
        // Explicitly moved closure arguments no longer have storage to recover their binding type from.
        if (!arg.TypeAndValue.IsFunctionPointer && arg.HasOriginalArgumentType
            && arg.OriginalArgumentType.IsFunctionPointer)
        {
            const std::string typeName = arg.TypeAndValue.TypeName;
            arg.TypeAndValue = arg.OriginalArgumentType;
            if (!typeName.empty()) arg.TypeAndValue.TypeName = typeName;
        }
        // A call argument may carry only the closure type name; its variable has the signature.
        if (!arg.TypeAndValue.IsFunctionPointer && arg.Storage != nullptr)
            if (const NamedVariable* variable = FindVariableByStorage(arg.Storage);
                variable != nullptr && variable->TypeAndValue.IsFunctionPointer
                && !variable->TypeAndValue.IsThinFnPtr())
            {
                arg.TypeAndValue = variable->TypeAndValue;
                arg.TypeAndValue.VariableName.clear();
            }
        // An `auto` closure slot keeps no signature: take the target's when every invoker stored
        // into the slot was lowered with exactly that signature (plus the trailing env).
        if (!arg.TypeAndValue.IsFunctionPointer && signatureHint != nullptr
            && signatureHint->IsFunctionPointer
            && llvm::isa_and_nonnull<llvm::AllocaInst>(arg.Storage))
        {
            TypeAndValue hintRet;
            std::vector<TypeAndValue> hintParams;
            UnpackFuncPtrSignature(*signatureHint, hintRet, hintParams);
            llvm::FunctionType* plain = GetFunctionType(hintRet, hintParams);
            llvm::FunctionType* expected = nullptr;
            if (plain != nullptr)
            {
                std::vector<llvm::Type*> withEnv(plain->param_begin(), plain->param_end());
                withEnv.push_back(cflat_llvm::PointerTo(builder->getInt8Ty()));
                expected = llvm::FunctionType::get(plain->getReturnType(), withEnv, false);
            }
            bool matches = expected != nullptr;
            bool sawStore = false;
            for (llvm::User* user : arg.Storage->users())
            {
                auto* store = llvm::dyn_cast<llvm::StoreInst>(user);
                if (store == nullptr || store->getPointerOperand() != arg.Storage) continue;
                llvm::Value* fat = store->getValueOperand();
                llvm::Value* code = nullptr;
                while (auto* insert = llvm::dyn_cast<llvm::InsertValueInst>(fat))
                {
                    if (insert->getNumIndices() == 1 && insert->getIndices()[0] == 0)
                    {
                        code = insert->getInsertedValueOperand();
                        break;
                    }
                    fat = insert->getAggregateOperand();
                }
                if (code == nullptr)
                    if (auto* constant = llvm::dyn_cast<llvm::Constant>(fat))
                        code = constant->getAggregateElement(0u);
                auto* invoker = llvm::dyn_cast_or_null<llvm::Function>(
                    code != nullptr ? code->stripPointerCasts() : nullptr);
                sawStore = true;
                if (invoker == nullptr || invoker->getFunctionType() != expected) matches = false;
            }
            if (matches && sawStore)
            {
                const std::string typeName = arg.TypeAndValue.TypeName;
                arg.TypeAndValue = *signatureHint;
                arg.TypeAndValue.TypeName = typeName;
                arg.TypeAndValue.VariableName.clear();
            }
        }
        // By-reference captures borrow the defining frame; C++ may keep the callable past it.
        if (arg.IsBonded || arg.ContainsBondedClosure || !arg.LambdaReferenceCaptureNames.empty())
        {
            // A move-only capture is refused whether or not the by-reference check is deferred.
            for (const std::string& name : arg.LambdaReferenceCaptureNames)
            {
                const NamedVariable* captured = FindLiveNamedVariable(name);
                if (captured != nullptr && MangledBase(captured->TypeAndValue.TypeName) == "unique")
                    return std::format(
                        "a closure capturing the move-only '{}' cannot be passed to C++ '{}': the "
                        "C++ callable must be copyable, as C++ refuses a move-only lambda there",
                        name, target);
            }
            if (!deferReferenceCaptureCheck)
            {
                std::string names;
                for (const std::string& name : arg.LambdaReferenceCaptureNames)
                    names += (names.empty() ? "" : ", ") + name;
                return std::format(
                    "a closure that captures by reference{} cannot be passed to C++ '{}': the C++ "
                    "callable can outlive the captured local; capture a pointer instead",
                    names.empty() ? std::string() : " ('" + names + "')", target);
            }
        }
        // The generated wrapper adapts CFlat records, aliases and string values at the call edge.
        TypeAndValue thin = arg.TypeAndValue;
        thin.TypeName = "__c_fn_ptr";
        TypeAndValue ret;
        std::vector<TypeAndValue> params;
        std::string signature;
        bool scalarSignature = thin.IsFunctionPointer;
        if (scalarSignature)
        {
            UnpackFuncPtrSignature(thin, ret, params);
            auto supported = [&](const TypeAndValue& t, bool isReturn) {
                if (t.IsAlias || t.IsRvalueRef) return true;
                if (t.Pointer) return true;
                if (isReturn && (t.TypeName.empty() || t.TypeName == "void")) return true;
                return (t.IsPrimitive() && t.TypeName != "void")
                    || t.TypeName == "string" || IsCxxRecord(t.TypeName);
            };
            scalarSignature = supported(ret, true);
            for (const TypeAndValue& p : params) scalarSignature = scalarSignature && supported(p, false);
        }
        if (!thin.IsFunctionPointer)
            return std::format(
                "a closure whose signature is not known at this argument (an 'auto' local, a call "
                "result or a field) cannot be passed to C++ '{}'; bind it to a 'Lambda<...>' local "
                "first", target);
        if (!scalarSignature || !CxxThinFunctionPointerSpelling(thin, signature, true))
            return std::format(
                "a capturing closure cannot be passed to C++ '{}': its parameter or return type "
                "cannot cross the closure wrapper", target);

        llvm::Value* fat = arg.Primary;
        const bool explicitMove = arg.IsExplicitMove || arg.TypeAndValue.IsMove;
        // Any lvalue (a local, a field, an element) still owns its env: C++ only borrows it.
        const bool named = arg.Storage != nullptr && !explicitMove
            && (fat == nullptr || !IsOwnedClosureTemp(fat));
        llvm::Value* slot = nullptr;
        if (named)
        {
            // C++ may copy the borrowed closure: allowed only when no owning capture is copied.
            const bool trivialValueCaptures = [&] {
                if (arg.LambdaCaptureNames.empty()) return false;
                for (const std::string& name : arg.LambdaCaptureNames)
                {
                    const NamedVariable* captured = FindLiveNamedVariable(name);
                    if (captured == nullptr || captured->TypeAndValue.Pointer) return false;
                    const TypeAndValue& type = captured->TypeAndValue;
                    if (type.IsInteger() >= 0 || type.IsFloatingPoint() >= 0) continue;
                    if (IsCxxTriviallyCopyableRecord(type.TypeName)) continue;
                    return false;
                }
                return true;
            }();
            if (!ClosureSlotHasOnlyTrivialCaptures(arg.Storage)
                && !(deferReferenceCaptureCheck
                     && (!arg.LambdaReferenceCaptureNames.empty() || trivialValueCaptures)))
                return std::format(
                    "a closure that may own non-trivial captures cannot be copied into C++ '{}' "
                    "implicitly; pass 'move {}' to hand it over", target,
                    arg.CallerName.empty() ? std::string("<closure>") : arg.CallerName);
            // The variable's own slot is the C++ closure object (same layout): C++ binds `F &` to
            // it as clang binds the caller's lambda. The marked GEP is a reference, not a store.
            auto* reference = llvm::GetElementPtrInst::CreateInBounds(
                builder->getInt8Ty(), arg.Storage, { builder->getInt64(0) }, "cxx_closure_ref");
            builder->Insert(reference);
            reference->setMetadata("cflat.cxx.closure.ref", llvm::MDNode::get(*context, {}));
            slot = reference;
        }
        else
        {
            if (fat == nullptr && arg.Storage != nullptr)
                fat = CreateLoad(GetClosureFatPtrType(), arg.Storage);
            if (fat == nullptr || fat->getType() != GetClosureFatPtrType())
                return std::format("a closure value cannot be passed to C++ '{}' here", target);
            // The C++ wrapper owns the env from here on.
            UnregisterOwnedClosureTemp(fat);
            // `move l`: the source slot is drained so its CFlat dtor sees a null env.
            if (explicitMove && arg.Storage != nullptr && arg.Storage->getType()->isPointerTy())
                builder->CreateStore(llvm::ConstantAggregateZero::get(GetClosureFatPtrType()), arg.Storage);
        }
        if (slot == nullptr)
        {
            llvm::AllocaInst* temp = AllocaAtEntry(GetClosureFatPtrType(), nullptr, "cxx_closure");
            builder->CreateStore(fat, temp);
            slot = temp;
        }
        EnsureCxxClosureEnvHelpers();
        NamedVariable wrapped;
        wrapped.Primary = slot;
        wrapped.BaseType = slot->getType();
        wrapped.TypeAndValue.TypeName = "void";
        wrapped.TypeAndValue.Pointer = true;
        wrapped.OriginalArgumentType = arg.TypeAndValue;
        wrapped.HasOriginalArgumentType = arg.TypeAndValue.IsFunctionPointer;
        wrapped.IsRvalue = !named;   // a closure variable crosses as the lvalue it is
        wrapped.CxxClosureSignature = signature;
        wrapped.CxxClosureBorrowed = named;
        wrapped.CxxClosureDisplay = "Lambda<" + FuncPtrSpellingOf(thin) + ">";
        wrapped.IsBonded = arg.IsBonded;
        wrapped.ContainsBondedClosure = arg.ContainsBondedClosure;
        wrapped.LambdaReferenceCaptureNames = arg.LambdaReferenceCaptureNames;
        arg = std::move(wrapped);
        return {};
}


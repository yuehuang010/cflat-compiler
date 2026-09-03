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
#include <functional>

#if defined(__APPLE__)
// Step 3 (macOS self-contained link): harvest libSystem's exported symbols from
// the live dyld shared cache to synthesize a linker stub, so -o needs no SDK.
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <dlfcn.h>
#include <cstring>
#include <sys/sysctl.h>
#endif

// ---- Definitions moved out of LLVMBackend.h (Lookup) ----

bool LLVMBackend::IsKnownTypeName(const std::string& name) const
{
        static const std::unordered_set<std::string> scalars = {
            "void", "char", "i8", "u8", "short", "i16", "u16", "int", "i32", "u32",
            "long", "ulong", "i64", "u64", "float", "double", "bool", "va_list", "auto" };
        if (scalars.count(name)) return true;
        std::string resolved = ResolveTypeAlias(name);
        return enumBackingTypes.count(resolved) > 0 || resolved != name
            || HasInterface(resolved) || dataStructures.count(resolved) > 0;
    }

/*
 * Decode a raw `simd<T,N>` spelling into its element type and lane count. The declarator path
 * records those two facts directly from the parse tree; a NAME-keyed position (cast target,
 * lambda parameter, tuple element, closure signature component) only ever sees this text, so
 * this is where the special form re-enters the type system. Returns false for any other name.
 */
static bool DecodeSimdSpelling(const std::string& text, std::string& elemOut, uint64_t& lanesOut)
{
    static const std::string kPrefix = "simd<";
    if (!text.starts_with(kPrefix) || text.back() != '>') return false;
    std::string inner = text.substr(kPrefix.size(), text.size() - kPrefix.size() - 1);
    // Split on the LAST top-level comma: the element type can itself carry brackets.
    int depth = 0;
    size_t comma = std::string::npos;
    for (size_t i = 0; i < inner.size(); i++)
    {
        if (inner[i] == '<' || inner[i] == '(') depth++;
        else if (inner[i] == '>' || inner[i] == ')') depth--;
        else if (inner[i] == ',' && depth == 0) comma = i;
    }
    if (comma == std::string::npos) return false;
    std::string elem = inner.substr(0, comma);
    std::string lanes = inner.substr(comma + 1);
    auto trim = [](std::string& s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    };
    trim(elem); trim(lanes);
    if (elem.empty() || lanes.empty()) return false;
    try
    {
        size_t pos = 0;
        lanesOut = std::stoull(lanes, &pos, 0);
        if (pos != lanes.size()) return false;
    }
    catch (...)
    {
        return false;
    }
    if (lanesOut < 2 || lanesOut > 64 || (lanesOut & (lanesOut - 1)) != 0)
        return false;
    elemOut = elem;
    return true;
}

llvm::Type* LLVMBackend::GetType(const LLVMBackend::TypeAndValue& typeAndValue, llvm::Type* autoType, bool allowPointer) const
{
        if (typeAndValue.IsFunctionPointer)
        {
            llvm::Type* fnPtrType = typeAndValue.IsThinFnPtr()
                ? BuildThinFnPtrType(typeAndValue)
                : GetClosureFatPtrType();
            // A 'function<T>[N]' (or 'Lambda<T>[N]') array needs the same outer array wrap
            // every other type gets; the bare scalar type under-sizes the alloca.
            uint64_t fnOuterDim = typeAndValue.ConstArraySize;
            if (fnOuterDim > 0)
            {
                llvm::Type* inner = fnPtrType;
                const auto& fnInnerDims = typeAndValue.ConstInnerDimensions;
                for (int i = (int)fnInnerDims.size() - 1; i >= 0; i--)
                    inner = llvm::ArrayType::get(inner, fnInnerDims[i]);
                return llvm::ArrayType::get(inner, fnOuterDim);
            }
            return fnPtrType;
        }

        llvm::Type* type = nullptr;
        const auto& typeName = typeAndValue.TypeName;

        // Resolve enum type names to their backing type if registered
        std::string resolvedTypeName = typeName;
        if (!resolvedTypeName.empty())
        {
            auto it = enumBackingTypes.find(resolvedTypeName);
            if (it != enumBackingTypes.end())
                resolvedTypeName = it->second;
        }
        // `simd<T,N>` is a builtin special form recognised by the DECLARATOR path, which records
        // it as element type + lane count. Every other position resolves a type by NAME and hands
        // us the raw spelling, which names no registered type - decode it here so a cast target, a
        // lambda parameter and a tuple/signature component lower exactly as a declaration does.
        bool simdFromSpelling = false;
        uint64_t simdSpellingLanes = 0;
        if (std::string simdElem; DecodeSimdSpelling(resolvedTypeName, simdElem, simdSpellingLanes))
        {
            resolvedTypeName = simdElem;
            simdFromSpelling = true;
        }
        // Resolve user-defined type aliases. A pointer alias (using Handle = void*) keeps its
        // trailing stars in the stored string; peel them into a local pointer-depth count and
        // OR it onto the typeAndValue pointer flags below (storage stays string-shaped).
        int aliasPtrDepth = 0;
        std::vector<uint64_t> aliasArrayDims;  // outer dimension first, from an array alias
        if (!resolvedTypeName.empty())
        {
            std::string aliasedName = ResolveTypeAlias(resolvedTypeName);
            if (aliasedName != resolvedTypeName)
            {
                resolvedTypeName = aliasedName;
                // Array brackets are outermost in the stored string ("int*[3]"); peel them first.
                std::vector<uint64_t> rev;
                while (!resolvedTypeName.empty() && resolvedTypeName.back() == ']')
                {
                    size_t open = resolvedTypeName.rfind('[');
                    if (open == std::string::npos) break;
                    std::string num = resolvedTypeName.substr(open + 1, resolvedTypeName.size() - open - 2);
                    if (num.empty() || num.find_first_not_of("0123456789") != std::string::npos) break;
                    rev.push_back(std::strtoull(num.c_str(), nullptr, 10));
                    resolvedTypeName.erase(open);
                }
                for (auto rit = rev.rbegin(); rit != rev.rend(); ++rit) aliasArrayDims.push_back(*rit);
                while (!resolvedTypeName.empty() && resolvedTypeName.back() == '*')
                {
                    resolvedTypeName.pop_back();
                    aliasPtrDepth++;
                }
            }
        }
        // Namespace-relative type reference: a bare "_SystemInfo" inside namespace
        // os.windows resolves to "os.windows._SystemInfo". Only accept a resolution
        // that names a type, so a sibling function never hijacks a type name.
        if (!resolvedTypeName.empty()
            && dataStructures.find(resolvedTypeName) == dataStructures.end()
            && !HasInterface(resolvedTypeName))
        {
            std::string nsResolved = ResolveQualifiedName(resolvedTypeName);
            if (nsResolved != resolvedTypeName
                && (dataStructures.count(nsResolved) || HasInterface(nsResolved)))
                resolvedTypeName = nsResolved;
        }

        // resolvedTypeName is final; hoist the struct/interface map lookups once.
        auto dsIt = dataStructures.find(resolvedTypeName);
        // A generic interface instantiation lowers to a fat pointer even before its interfaceTable
        // entry exists (the forward-ref scan materializes signatures first).
        bool isInterface = HasInterface(resolvedTypeName)
                        || gts.genericInterfaceInstances.count(resolvedTypeName) > 0;
        bool skipPointerWrap = false;

        if (resolvedTypeName == "void") { type = builder->getVoidTy(); }
        else if (resolvedTypeName == "char" || resolvedTypeName == "i8" || resolvedTypeName == "u8") { type = builder->getInt8Ty(); }
        else if (resolvedTypeName == "short" || resolvedTypeName == "i16" || resolvedTypeName == "u16") { type = builder->getInt16Ty(); }
        else if (resolvedTypeName == "int" || resolvedTypeName == "i32" || resolvedTypeName == "u32") { type = builder->getInt32Ty(); }
        else if (resolvedTypeName == "i64" || resolvedTypeName == "u64") { type = builder->getInt64Ty(); }
        // `long`/`ulong` are the target's native C long: i32 on Windows (LLP64), i64 on LP64.
        else if (resolvedTypeName == "long" || resolvedTypeName == "ulong")
            { type = (longBits_ == 32) ? builder->getInt32Ty() : builder->getInt64Ty(); }
        else if (resolvedTypeName == "float") { type = builder->getFloatTy(); }
        else if (resolvedTypeName == "double") { type = builder->getDoubleTy(); }
        else if (resolvedTypeName == "bool") { type = builder->getInt1Ty(); }
        else if (resolvedTypeName == "va_list") { type = llvm::PointerType::getUnqual(*context); }
        else if (resolvedTypeName == "auto" && autoType != nullptr) { type = autoType; }
        else
        {
            // Check if it is an interface type first
            if (isInterface)
            {
                auto* fatTy = GetFatPtrType();
                if (typeAndValue.ElemPointer)
                    type = cflat_llvm::PointerTo(cflat_llvm::PointerTo(fatTy)); // {i8*,i8*}** (T* where T=IFace*)
                // An `IFace[]` array view is a thin pointer to a run of fat structs, so it lowers
                // like `IFace*`; without this it would lower to the bare struct and every GEP fails.
                else if (allowPointer && (typeAndValue.IsInterfacePointer || typeAndValue.IsArrayView))
                    type = cflat_llvm::PointerTo(fatTy);                 // {i8*,i8*}* (T* where T=IFace, or T=IFace*)
                else
                    type = fatTy;                                 // {i8*,i8*}  (bare fat ptr)
                // The arms above already encode every pointer level an interface can have, so the
                // generic wrap below must not run; only the array wrap at the tail still applies.
                skipPointerWrap = true;
            }
            else
            {
                // A THIN encoded closure (list<function<T>> element, Box<function<T>> field) is a
                // bare code pointer with no struct backing - lower it as `function<T>` lowers.
                const TypeAndValue* thinEnc = GetEncodedClosureType(resolvedTypeName);
                if (thinEnc != nullptr && thinEnc->IsThinFnPtr())
                {
                    type = BuildThinFnPtrType(*thinEnc);
                }
                else if (dsIt != dataStructures.end())
                {
                    type = dsIt->second.StructType;
                }
                else
                {
                    std::string genericKey = resolvedTypeName;
                    if (!IsGenericTemplateKey(genericKey))
                        genericKey = ResolveGenericTemplateBase(genericKey);

                    const std::vector<std::string>* typeParams = nullptr;
                    if (IsGenericTemplateKey(genericKey))
                    {
                        auto structParams = gts.genericStructTypeParams.find(genericKey);
                        if (structParams != gts.genericStructTypeParams.end())
                            typeParams = &structParams->second;
                        else
                        {
                            auto interfaceParams = gts.genericInterfaceTypeParams.find(genericKey);
                            if (interfaceParams != gts.genericInterfaceTypeParams.end())
                                typeParams = &interfaceParams->second;
                        }
                    }

                    if (typeParams != nullptr && !typeParams->empty())
                    {
                        std::string parameterNames;
                        std::string exampleArgs;
                        for (size_t i = 0; i < typeParams->size(); i++)
                        {
                            if (i != 0)
                            {
                                parameterNames += ", ";
                                exampleArgs += ", ";
                            }
                            parameterNames += (*typeParams)[i];
                            exampleArgs += "int";
                        }
                        std::string count = std::format("{} type parameter{}", typeParams->size(),
                            typeParams->size() == 1 ? "" : "s");
                        TypeAndValue displayType;
                        displayType.TypeName = resolvedTypeName;
                        const std::string displayName = SpellType(*this, displayType);
                        LogErrorMessage("'{}' is a generic type; type arguments are required "
                            "(expects {}: {}), e.g. {}",
                            { displayName, count, parameterNames,
                              displayName + "<" + exampleArgs + ">" });
                    }
                    else
                    {
                        TypeAndValue displayType;
                        displayType.TypeName = resolvedTypeName;
                        LogErrorMessage("unknown type '{}'", { SpellType(*this, displayType) });
                    }
                    type = builder->getVoidTy();
                }
            }
        }

        // simd<T,N> -> LLVM <N x T> vector primitive. Wrap before pointer wrapping so
        // simd<float,8>* lowers to <8 x float>*. Element must be a numeric scalar.
        if (typeAndValue.IsSimd || simdFromSpelling)
        {
            uint64_t lanes = typeAndValue.IsSimd ? typeAndValue.SimdLanes : simdSpellingLanes;
            if (lanes == 0)
                return type;  // malformed lane count already reported; avoid a 0-width vector
            bool numeric = type->isFloatTy() || type->isDoubleTy()
                || (type->isIntegerTy() && !type->isIntegerTy(1));
            // simd<bool,N> (i1 lanes) is the mask type: the result of a vector comparison and the
            // first argument of simd<T,N>.select. It is a valid simd element even though it is not
            // an arithmetic scalar (you cannot + - * / a mask, but you can store and select with it).
            bool isMask = type->isIntegerTy(1);
            if (!numeric && !isMask)
            {
                LogErrorMessage("simd element type must be a numeric scalar "
                    "({}, {}, {}, {}) or {} for a mask, got '{}'",
                    { "i8..i64", "u8..u64", "float", "double", "bool",
                      SpellType(*this, TypeAndValue{ .TypeName = resolvedTypeName }) });
                return type;
            }
            type = llvm::FixedVectorType::get(type, static_cast<unsigned>(lanes));
        }

        // Apply pointer wrapping to get the element type before array wrapping.
        // This ensures char*[3] -> [3 x ptr] not [3 x i8].
        // aliasPtrDepth folds in a pointer alias's stars (using Handle = void* -> 1) so a call
        // site that passes the un-stripped alias name still lowers correctly.
        bool wantPointer = typeAndValue.Pointer || aliasPtrDepth >= 1;
        bool wantElemPointer = typeAndValue.ElemPointer || aliasPtrDepth >= 2
            || (typeAndValue.Pointer && aliasPtrDepth >= 1);
        if (allowPointer && wantPointer && !skipPointerWrap)
        {
            // Note: LLVM doesn't have void ptr, instead use i8 ptr.
            if (type->isVoidTy())
                type = cflat_llvm::PointerTo(builder->getInt8Ty());
            else
                type = cflat_llvm::PointerTo(type);
            if (wantElemPointer)
                type = cflat_llvm::PointerTo(type);
        }

        // Prefer the explicit ConstArraySize set by the declaration handler; fall back to an
        // array alias's dims (using Vec3 = float[3]) for call sites that pass only the alias name
        // (sizeof(Vec3), a parameter of type Vec3) and never run the per-declarator finalization.
        uint64_t outerDim = typeAndValue.ConstArraySize;
        const std::vector<uint64_t>* innerDims = &typeAndValue.ConstInnerDimensions;
        std::vector<uint64_t> aliasInner;
        if (outerDim == 0 && !aliasArrayDims.empty())
        {
            outerDim = aliasArrayDims[0];
            aliasInner.assign(aliasArrayDims.begin() + 1, aliasArrayDims.end());
            innerDims = &aliasInner;
        }
        if (outerDim > 0)
        {
            // Build from innermost to outermost: T[N1][N2] -> [N1 x [N2 x T]]
            llvm::Type* inner = type;
            for (int i = (int)innerDims->size() - 1; i >= 0; i--)
                inner = llvm::ArrayType::get(inner, (*innerDims)[i]);
            return llvm::ArrayType::get(inner, outerDim);
        }

        return type;
    }

int LLVMBackend::ScoreMoveAgreement(const std::vector<NamedVariable>& arguments, const FunctionSymbol& candidate) const
{
        int moveScore = 0;
        auto pi = candidate.Parameters.begin();
        for (const auto& arg : arguments)
        {
            if (pi == candidate.Parameters.end()) break;
            /*
             * An explicit 'move' at the call site is a direct request for the move
             * overload, and the ONLY thing that selects it for a NAMED lvalue: passing an
             * owning variable plainly (value OR pointer) means "borrow".
             * An RVALUE prefers the move overload the way C++ binds an rvalue to 'T&&'
             * over 'const T&' - the temporary dies right after the call, so a borrow
             * overload would deep-copy for nothing. "Rvalue" = no addressable storage and
             * a CallerName naming nothing in scope (for a call result CallerName holds the
             * CALLEE's name). Restricted to aggregates, where the copy actually costs.
             */
            bool argIsUnbound = arg.Storage == nullptr
                && FindVariableStorage(arg.CallerName).Storage == nullptr;
            bool argIsRValue = argIsUnbound && arg.FieldName.empty()
                && !arg.TypeAndValue.Pointer
                && (arg.IsOwningString || arg.IsOwningStruct
                    || IsDataStructure(arg.TypeAndValue.TypeName));
            bool argOwning = arg.IsExplicitMove || argIsRValue;
            if (pi->IsMove == argOwning)
                moveScore++;
            if (pi->IsFunctionPointer && !arg.CallerName.empty()
                && HasFunctionWithMoveFlags(arg.CallerName, pi->FuncPtrParams))
            {
                moveScore += 2;  // weight funcptr-IsMove match heavily so it can break a tie
            }
            ++pi;
        }
        return moveScore;
    }

int LLVMBackend::FunctionPointerShapeOf(const LLVMBackend::TypeAndValue& tv, const NamedVariable* nv)
{
        if (tv.IsArrayView || tv.ConstArraySize > 0)
            return 2;
        if (nv != nullptr)
        {
            if (nv->BaseType != nullptr && nv->BaseType->isArrayTy())
                return 2;
            if (auto* slot = llvm::dyn_cast_or_null<llvm::AllocaInst>(nv->Storage))
                if (slot->getAllocatedType()->isArrayTy())
                    return 2;
        }
        return tv.Pointer ? 1 : 0;
    }

std::string LLVMBackend::FuncPtrShapeWord(const TypeAndValue& tv, const NamedVariable* nv) const
{
        // Family, when the source says which one; empty means "say 'closure' and no more".
        std::string family;
        if (tv.TypeName == "__c_fn_ptr") family = "function<>";
        else if (tv.TypeName == "__closure_fat_ptr") family = "Lambda<>";
        else if (IsEncodedClosureType(tv.TypeName))
            family = IsThinEncodedClosureType(tv.TypeName) ? "function<>" : "Lambda<>";
        else if (nv != nullptr && nv->Primary != nullptr && llvm::isa<llvm::Function>(nv->Primary))
            family = "function<>";  // a bare named function is a thin code address

        int shape = FunctionPointerShapeOf(tv, nv);
        if (shape == 2)
        {
            // A fixed 'T[N]' and a 'T[]' view share a shape but are not the same spelling; the
            // length is only known from ConstArraySize, which an alloca-derived shape lacks.
            bool isView = tv.IsArrayView && tv.ConstArraySize == 0;
            if (family.empty()) return isView ? "a closure view" : "a closure array";
            std::string dim = isView ? "[]"
                : (tv.ConstArraySize > 0 ? std::format("[{}]", tv.ConstArraySize) : "[N]");
            return std::format("a '{}{}' {}", family, dim, isView ? "view" : "array");
        }
        if (shape == 1)
            return family.empty() ? "a closure pointer" : std::format("a '{}*' pointer", family);
        return family.empty() ? "a closure value" : std::format("a '{}' value", family);
    }

std::string LLVMBackend::ResolveFuncPtrTypeSpelling(const std::string& typeName) const
{
        std::string cur = typeName;
        for (int hop = 0; hop < 8 && !cur.empty(); hop++)
        {
            std::string aliased = ResolveTypeAlias(cur);
            if (aliased != cur) { cur = aliased; continue; }
            if (auto e = enumBackingTypes.find(cur); e != enumBackingTypes.end()) { cur = e->second; continue; }
            break;
        }
        return cur;
    }

std::string LLVMBackend::FuncPtrScalarCanon(const std::string& resolved) const
{
        if (resolved == "void") return "v";
        if (resolved == "bool") return "b";
        LLVMBackend::TypeAndValue probe;
        probe.TypeName = resolved;
        int bits = probe.IsInteger();
        if (bits != -1) return "i" + std::to_string(bits);
        int fbits = probe.IsFloatingPoint();
        if (fbits != -1) return "f" + std::to_string(fbits);
        return "";
    }

std::string LLVMBackend::FuncPtrAbiCanonKey(const std::string& key) const
{
        TypeSpelling spelling;
        if (!DemangleType(*this, key, spelling)) return key;
        std::function<std::string(const TypeSpelling&)> canon =
            [&](const TypeSpelling& value) -> std::string
        {
            if (!value.args.empty())
            {
                std::vector<std::string> args;
                args.reserve(value.args.size());
                for (const auto& arg : value.args)
                    args.push_back(canon(arg));
                return MangleGenericInstance(*this, value.base, args);
            }
            TypeSpelling scalar = value;
            std::string resolved = ResolveFuncPtrTypeSpelling(value.base);
            if (std::string scalarName = FuncPtrScalarCanon(resolved); !scalarName.empty())
                scalar.base = std::move(scalarName);
            return MangleType(*this, scalar);
        };
        return canon(spelling);
}

std::vector<std::string> LLVMBackend::FuncPtrStructCandidates(const std::string& spelling) const
{
        std::vector<std::string> keys;
        if (spelling.empty() || HasInterface(spelling)) return keys;
        for (const auto& entry : dataStructures)
        {
            const std::string& key = entry.first;
            // The spelling matches a key outright, or matches its trailing dotted component.
            bool tail = key.size() > spelling.size()
                && key.compare(key.size() - spelling.size(), spelling.size(), spelling) == 0
                && key[key.size() - spelling.size() - 1] == '.';
            if (key != spelling && !tail) continue;
            if (HasInterface(key)) continue;
            // The SPELLING match is made on the raw key (that is what names the struct); the
            // recorded identity is the ABI-canonical form, so two instantiations that differ
            // only in a scalar spelling or in signedness compare equal.
            keys.push_back(FuncPtrAbiCanonKey(key));
        }
        std::sort(keys.begin(), keys.end());
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
        return keys;
    }

LLVMBackend::FuncPtrComponent LLVMBackend::FuncPtrComponentOf(const std::string& typeName, bool pointer,
        int pointerDepth, const std::string& resolvedKey) const
{
        FuncPtrComponent c;
        c.PointerDepth = pointer ? pointerDepth : 0;
        std::string resolved = ResolveFuncPtrTypeSpelling(typeName);
        if (resolved.empty()) return c;
        // `string` normalizes to char*, a single level - it cannot carry a written '*' count.
        if (resolved == "string") { resolved = "char"; pointer = true; c.PointerDepth = 1; }

        std::string scalar = FuncPtrScalarCanon(resolved);
        if (!pointer)
        {
            if (!scalar.empty()) { c.Known = true; c.Canon = scalar; return c; }
        }
        else
        {
            if (scalar == "v") { c.Known = true; c.VoidPointee = true; c.Canon = "p:v"; return c; }
            if (!scalar.empty()) { c.Known = true; c.Canon = "p:" + scalar; return c; }
        }
        // A spelling that names at least one registered struct carries its whole candidate set.
        // An interface never does (a struct implementing it converts), nor does an unknown name.
        std::vector<std::string> keys = FuncPtrStructCandidates(resolved);
        // A recorded declaring-scope key collapses an ambiguity the compiler ITSELF resolved.
        // Membership-only: a stale or wrong key can never invent a rejection, only narrow.
        // The ABI-canon hop is kept, or Box<int> vs Box<i32> would start false-rejecting.
        if (!resolvedKey.empty() && keys.size() > 1)
        {
            std::string canon = FuncPtrAbiCanonKey(resolvedKey);
            if (std::find(keys.begin(), keys.end(), canon) != keys.end())
                keys = { canon };
        }
        if (!keys.empty())
        {
            c.Known = true;
            c.Canon = pointer ? "p:s" : "s";
            c.StructKeys = std::move(keys);
        }
        return c;
    }

bool LLVMBackend::ComponentsProvablyDiffer(const FuncPtrComponent& a, const FuncPtrComponent& b)
{
        if (!a.Known || !b.Known) return false;
        if (a.VoidPointee || b.VoidPointee)
        {
            bool aPtr = a.Canon.rfind("p:", 0) == 0;
            bool bPtr = b.Canon.rfind("p:", 0) == 0;
            if (aPtr && bPtr) return false;
        }
        if (a.Canon != b.Canon) return true;
        // Same pointee, different number of '*'. Canon collapses every depth onto one "p:" token,
        // so this is the only place `int*` and `int**` are told apart - and an unrecorded depth (0)
        // on either side proves nothing, exactly like an unresolvable Canon.
        if (a.PointerDepth > 0 && b.PointerDepth > 0 && a.PointerDepth != b.PointerDepth)
            return true;
        // Two struct spellings at the same shape. Proof is that NO registered struct could answer
        // to BOTH; an intersection means one type satisfies both spellings, so nothing is proven.
        if (!a.StructKeys.empty() && !b.StructKeys.empty())
        {
            std::vector<std::string> shared;
            std::set_intersection(a.StructKeys.begin(), a.StructKeys.end(),
                b.StructKeys.begin(), b.StructKeys.end(), std::back_inserter(shared));
            return shared.empty();
        }
        return false;
    }

bool LLVMBackend::FuncPtrSignatureOf(const LLVMBackend::TypeAndValue& tv, std::vector<FuncPtrComponent>& out) const
{
        if (tv.FuncPtrReturnTypeName.empty()) return false;
        out.clear();
        out.push_back(FuncPtrComponentOf(tv.FuncPtrReturnTypeName, tv.FuncPtrReturnPointer,
            tv.FuncPtrReturnPointerDepth, tv.FuncPtrReturnResolvedKey));
        for (const auto& p : tv.FuncPtrParams)
            out.push_back(FuncPtrComponentOf(p.TypeName, p.Pointer, p.PointerDepth, p.ResolvedTypeKey));
        return true;
    }

bool LLVMBackend::FuncPtrSignaturesProvablyDiffer(const LLVMBackend::TypeAndValue& a,
        const LLVMBackend::TypeAndValue& b) const
{
        std::vector<FuncPtrComponent> sa, sb;
        if (!FuncPtrSignatureOf(a, sa) || !FuncPtrSignatureOf(b, sb))
            return false;
        if (sa.size() != sb.size())
            return true;
        for (size_t i = 0; i < sa.size(); i++)
            if (ComponentsProvablyDiffer(sa[i], sb[i]))
                return true;
        return false;
    }

LLVMBackend::TypeAndValue LLVMBackend::FuncPtrSigOfSymbol(const FunctionSymbol& sym) const
{
        TypeAndValue sig;
        sig.IsFunctionPointer = true;
        sig.TypeName = "__c_fn_ptr";
        sig.FuncPtrReturnTypeName = sym.ReturnType.TypeName;
        sig.FuncPtrReturnPointer = sym.ReturnType.Pointer;
        sig.FuncPtrReturnOwned = sym.ReturnType.IsMove;
        sig.FuncPtrReturnAlias = sym.ReturnType.IsAlias;
        sig.FuncPtrReturnPointerDepth = sym.ReturnType.ValuePointerDepth();
        for (const auto& p : sym.Parameters)
        {
            TypeAndValue::FuncPtrParam fp;
            fp.TypeName = p.TypeName;
            fp.Pointer = p.Pointer;
            fp.AllocAlignValue = p.AllocAlignValue;
            fp.IsMove = p.IsMove;
            // The function's INFERRED owning sinks are part of what a funcptr bound to it must
            // honour at the indirect call; a declared spelling cannot restate them.
            fp.IsOwningSink = p.IsOwningSink;
            fp.IsConsumeInferredSink = p.IsConsumeInferredSink;
            fp.PointerDepth = p.ValuePointerDepth();
            sig.FuncPtrParams.push_back(fp);
        }
        return sig;
    }

LLVMBackend::TypeAndValue LLVMBackend::FuncPtrSigOfBoundFunction(const std::string& functionName,
        const llvm::Function* fn) const
{
        if (fn == nullptr) return {};
        auto it = functionTable.find(ResolveQualifiedName(functionName));
        if (it == functionTable.end()) return {};
        for (const auto& sym : it->second)
            if (sym.Function == fn) return FuncPtrSigOfSymbol(sym);
        return {};
    }

bool LLVMBackend::NamedFunctionProvablyMismatchesFuncPtr(const std::string& functionName,
        const TypeAndValue& param) const
{
        auto it = functionTable.find(functionName);
        if (it == functionTable.end() || it->second.empty()) return false;
        for (const auto& sym : it->second)
            if (!FuncPtrSignaturesProvablyDiffer(FuncPtrSigOfSymbol(sym), param))
                return false;
        return true;
    }

bool LLVMBackend::NamedFunctionArgMismatches(const NamedVariable& arg, const TypeAndValue& param) const
{
        if (!param.IsFunctionPointer || arg.CallerName.empty()) return false;
        if (!arg.TypeAndValue.FuncPtrReturnTypeName.empty()) return false;
        if (FindVariableStorage(arg.CallerName).Storage != nullptr) return false;
        return NamedFunctionProvablyMismatchesFuncPtr(arg.CallerName, param);
    }

std::string LLVMBackend::FuncPtrSpellingOf(const LLVMBackend::TypeAndValue& tv) const
{
        // One '*' per recorded level, so a depth mismatch is visible as `int**` vs `int*` rather
        // than as two identical-looking spellings.
        TypeAndValue returnType;
        returnType.TypeName = tv.FuncPtrReturnTypeName;
        returnType.Pointer = tv.FuncPtrReturnPointer;
        returnType.PointerDepth = tv.FuncPtrReturnPointerDepth;
        std::string s = SpellType(*this, returnType) + "(";
        for (size_t i = 0; i < tv.FuncPtrParams.size(); i++)
        {
            if (i > 0) s += ", ";
            TypeAndValue parameterType;
            parameterType.TypeName = tv.FuncPtrParams[i].TypeName;
            parameterType.Pointer = tv.FuncPtrParams[i].Pointer;
            parameterType.PointerDepth = tv.FuncPtrParams[i].PointerDepth;
            s += SpellType(*this, parameterType);
        }
        return s + ")";
    }

std::string LLVMBackend::FuncPtrDifferenceOf(const std::vector<FuncPtrComponent>& sa,
        const std::vector<FuncPtrComponent>& sb) const
{
        if (sa.size() != sb.size())
            return std::format("arity differs: one takes {} parameter(s), the other {}",
                sa.size() - 1, sb.size() - 1);
        for (size_t i = 0; i < sa.size(); i++)
            if (ComponentsProvablyDiffer(sa[i], sb[i]))
                return (i == 0) ? "the return type differs" : std::format("parameter {} differs", i);
        return "";
    }

std::string LLVMBackend::DescribeFuncPtrSignatureMismatch(const LLVMBackend::TypeAndValue& arg,
        const LLVMBackend::TypeAndValue& param) const
{
        std::vector<FuncPtrComponent> sa, sb;
        if (!FuncPtrSignatureOf(arg, sa) || !FuncPtrSignatureOf(param, sb))
            return "";
        std::string why = FuncPtrDifferenceOf(sa, sb);
        if (why.empty()) return "";
        return std::format(
            "function-pointer signature mismatch: parameter takes '{}' but the argument is "
            "'{}' - {}", FuncPtrSpellingOf(param), FuncPtrSpellingOf(arg), why);
    }

std::string LLVMBackend::DescribeFuncPtrBindMismatch(const std::string& functionName,
        const LLVMBackend::TypeAndValue& fn, const LLVMBackend::TypeAndValue& dest) const
{
        std::vector<FuncPtrComponent> sa, sb;
        if (!FuncPtrSignatureOf(fn, sa) || !FuncPtrSignatureOf(dest, sb))
            return "";
        std::string why;
        if (sa.size() > sb.size())
            why = std::format("it takes {} parameter(s) but the slot supplies only {}",
                sa.size() - 1, sb.size() - 1);
        for (size_t i = 0; i < sa.size() && why.empty(); i++)
            if (ComponentsProvablyDiffer(sa[i], sb[i]))
                why = (i == 0) ? "the return type differs" : std::format("parameter {} differs", i);
        if (why.empty()) return "";
        return std::format("cannot bind function '{}' to function-pointer type '{}': '{}' is "
            "'{}' - {}. A function pointer is called with no conversion site, so the signature "
            "has to match.",
            SpellFunctionSymbol(*this, functionName), FuncPtrSpellingOf(dest),
            SpellFunctionSymbol(*this, functionName), FuncPtrSpellingOf(fn), why);
    }

bool LLVMBackend::ArgumentIsFunctionPointerish(const NamedVariable& arg) const
{
        return arg.TypeAndValue.IsFunctionPointer
            || !arg.TypeAndValue.FuncPtrReturnTypeName.empty()
            || arg.TypeAndValue.TypeName == "__c_fn_ptr"
            || arg.TypeAndValue.TypeName == "__closure_fat_ptr"
            || IsEncodedClosureType(arg.TypeAndValue.TypeName)
            || (arg.BaseType && arg.BaseType->isStructTy()
                && llvm::isa<llvm::StructType>(arg.BaseType)
                && llvm::cast<llvm::StructType>(arg.BaseType)->getName() == "__closure_fat_ptr")
            || (arg.Primary && llvm::isa<llvm::Function>(arg.Primary));
    }

bool LLVMBackend::ArgumentIsCodeValue(const NamedVariable& arg, size_t occurrence) const
{
        if (FunctionPointerShapeOf(arg.TypeAndValue, &arg) != 0) return false;
        // A join is the one shape carrying no declared facts at all - resolve it through the
        // per-value ledger, keyed by the caller's occurrence (see the header comment).
        return ArgumentIsFunctionPointerish(arg) || JoinCarriesCodeValue(arg.Primary, occurrence);
    }

bool LLVMBackend::ArgumentIsDataValue(const NamedVariable& arg) const
{
        if (!arg.TypeAndValue.Pointer) return false;
        return !ArgumentIsFunctionPointerish(arg);
    }

bool LLVMBackend::ParameterStoresData(const TypeAndValue& param) const
{
        if (param.IsFunctionPointer || IsEncodedClosureType(param.TypeName)) return false;
        return param.Pointer || param.TypeName == "string";
    }

bool LLVMBackend::ParameterAcceptsCodeValue(const TypeAndValue& param) const
{
        return param.IsFunctionPointer
            || param.TypeName == "__closure_fat_ptr"
            || IsEncodedClosureType(param.TypeName);
    }

bool LLVMBackend::CodeValueIntoDataDestination(const NamedVariable& src, const TypeAndValue& dest) const
{
        return ArgumentIsCodeValue(src, src.CastOccurrenceId) && ParameterStoresData(dest);
    }

std::string LLVMBackend::DescribeCodeValueIntoData(const std::string& spelling, const std::string& role,
                                          const std::string& castAdvice,
                                          const std::string& what) const
{
        std::string target = what.empty() ? std::string() : std::format(" {}", what);
        std::string msg = std::format("cannot {} a function-pointer or closure VALUE into{} data "
            "type '{}' - code does not convert to a data pointer", role, target, spelling);
        if (!castAdvice.empty())
            msg += std::format("; write an explicit '({})' cast if the raw code address is what "
                "you want", castAdvice);
        return msg;
    }

std::string LLVMBackend::DescribeCodeValueAsCompoundOperand(const std::string& spelling, const std::string& op,
                                                   bool destIsPointer) const
{
        return std::format("cannot use a function-pointer or closure VALUE as the right operand of "
            "'{}' on data type '{}' - {}", op, spelling,
            destIsPointer ? "a code address is not an offset"
                          : "a code address is not a value of that type");
    }

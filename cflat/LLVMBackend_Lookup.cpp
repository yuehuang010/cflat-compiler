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
            "void", "char", "i8", "u8", "c8", "short", "i16", "u16", "c16", "int", "i32", "u32", "uint", "c32",
            "long", "ulong", "i64", "u64", "longlong", "ulonglong", "i128", "u128", "wchar", "float", "double", "longdouble", "bool", "va_list", "auto" };
        if (scalars.count(name)) return true;
        std::string resolved = ResolveTypeAlias(name);
        return enumBackingTypes.count(resolved) > 0 || resolved != name
            || HasInterface(resolved) || dataStructures.count(resolved) > 0
            || !ResolveEnumTypeName(name).empty();
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

/*
 * A FIXED array indexes by its OWN element, so every door that judges an element must see it as
 * the view it decays to. A fixed array spells the element's star in `Pointer` ('int*[2]') where a
 * view spells the same depth in `ElemPointer`, so the depth is carried over - without that carry
 * every 'T*[N]' reads as a 'T' element. A simd value is not an element source.
 */
bool LLVMBackend::ReshapeFixedArrayAsView(LLVMBackend::TypeAndValue& typeAndValue) const
{
        if (typeAndValue.IsArrayView || typeAndValue.ConstArraySize == 0 || typeAndValue.IsSimd)
            return false;
        typeAndValue.ElemPointer = typeAndValue.ElemPointer || typeAndValue.Pointer;
        typeAndValue.Pointer = false;
        typeAndValue.IsArrayView = true;
        return true;
}

/*
 * A view SOURCE indexes by its ELEMENT, so binding it to a destination whose element differs
 * strides and loads with the wrong shape (silent garbage, or a raw verifier failure when the
 * loaded kind changes). Proves the difference before rejecting: the ELEMENT pointer depth, or -
 * for a view destination only - the element's lowered llvm type. A plain `T*` destination is the
 * one-way `T[] -> T*` decay, where a differing element NAME is the long-standing byte/scalar
 * reinterpret idiom (`u8* p = intView;`) and only a depth change is proven broken.
 */
bool LLVMBackend::ArrayViewElementMismatch(const LLVMBackend::TypeAndValue& dest,
                                           const LLVMBackend::TypeAndValue& rawSrc,
                                           std::string& destElement, std::string& srcElement) const
{
        // A FIXED array binds to a VIEW destination by decaying to its own view, so it indexes by
        // its own element too. Scoped to a view destination: the one-way 'T[] -> T*' decay keeps
        // its own byte/scalar reinterpret rules, and a simd value is not an element source.
        LLVMBackend::TypeAndValue shapedSrc = rawSrc;
        if (dest.IsArrayView) ReshapeFixedArrayAsView(shapedSrc);
        const LLVMBackend::TypeAndValue& src = shapedSrc;
        if (!src.IsArrayView) return false;
        if (!dest.IsArrayView && !dest.Pointer) return false;
        // An interface destination reboxes a SCALAR through its own gate, so it does not index by
        // the view's element. A view destination is not reboxed per element - see below.
        if (dest.IsInterface && !dest.IsArrayView) return false;
        if (dest.IsInterface)
        {
            // No rebox is emitted per view element, so every element must ALREADY carry this
            // interface's fat pointer. Unproven (unnamed) elements are accepted, not rejected.
            if (dest.TypeName.empty() || src.TypeName.empty()) return false;
            if (dest.ElemPointer == src.ElemPointer && dest.TypeName == src.TypeName) return false;
            destElement = dest.TypeName + (dest.ElemPointer ? "*" : "");
            srcElement = src.TypeName + (src.ElemPointer ? "*" : "");
            return true;
        }
        if (dest.IsFunctionPointer || src.IsFunctionPointer) return false;
        // 'void*' is the universal address destination - it never indexes by an element.
        if (dest.TypeName == "void") return false;

        // The DEPTH axis needs no name: an unnamed loaded view value still records its element
        // star, and a pointer element against a value element is proven either way.
        bool depthDiffers = dest.ElemPointer != src.ElemPointer;
        bool nameDiffers = false;
        if (dest.IsArrayView && !depthDiffers
            && !dest.TypeName.empty() && !src.TypeName.empty() && dest.TypeName != src.TypeName)
        {
            // Compare the LOWERED element type, so 'int'/'i32'/'u32', an alias and an enum all
            // read as the same representation. GetType does NOT return null on an unknown name -
            // it reports "unknown type" and hands back void - so gate on the silent lookup first.
            if (IsKnownTypeName(dest.TypeName) && IsKnownTypeName(src.TypeName))
            {
                LLVMBackend::TypeAndValue destBase;
                destBase.TypeName = dest.TypeName;
                LLVMBackend::TypeAndValue srcBase;
                srcBase.TypeName = src.TypeName;
                llvm::Type* destType = GetType(destBase, nullptr, false);
                llvm::Type* srcType = GetType(srcBase, nullptr, false);
                nameDiffers = destType != nullptr && srcType != nullptr && destType != srcType;
            }
        }
        if (!depthDiffers && !nameDiffers) return false;

        // 'T' stands in when a loaded value carries no declared name; the star half is always known.
        destElement = (dest.TypeName.empty() ? std::string("T") : dest.TypeName)
            + (dest.ElemPointer ? "*" : "");
        srcElement = (src.TypeName.empty() ? std::string("T") : src.TypeName)
            + (src.ElemPointer ? "*" : "");
        return true;
    }

// Spellings the grammar reserves as primitive type keywords: no user declaration can carry them.
static bool IsKeywordPrimitiveSpelling(const std::string& name)
{
    static const std::unordered_set<std::string_view> keywords = {
        "void", "char", "short", "int", "long", "float", "double",
        "i8", "i16", "i32", "i64", "i128", "u8", "u16", "u32", "u64", "u128",
    };
    return name.size() <= 6 && keywords.count(std::string_view(name)) != 0;
}

void LLVMBackend::TouchWaitingInstance(const std::string& name) const
{
        if (!gts.drainWaitingInstance || gts.pendingInstantiations.empty() || name.empty())
            return;
        gts.drainWaitingInstance(name);
}

void LLVMBackend::TouchWaitingLayout(llvm::Type* type) const
{
        while (type != nullptr && type->isArrayTy())
            type = llvm::cast<llvm::ArrayType>(type)->getElementType();
        if (auto* st = type != nullptr ? llvm::dyn_cast<llvm::StructType>(type) : nullptr; st != nullptr && st->hasName())
            TouchWaitingInstance(st->getName().str());
}

llvm::Type* LLVMBackend::GetType(const LLVMBackend::TypeAndValue& typeAndValue, llvm::Type* autoType, bool allowPointer) const
{
        if (typeAndValue.IsFunctionPointer)
        {
            llvm::Type* fnPtrType = typeAndValue.IsThinFnPtr()
                ? BuildThinFnPtrType(typeAndValue)
                : GetClosureFatPtrType();
            // A C++ reference to a function-pointer object crosses as the address of that
            // pointer. The ordinary function<> value remains the bare callback pointer.
            if (typeAndValue.Pointer && typeAndValue.IsCxxRefToPointer
                && (typeAndValue.IsAlias || typeAndValue.IsRvalueRef))
                return llvm::PointerType::getUnqual(fnPtrType);
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

        // Resolve enum type names to their backing type if registered. The enum may be reached
        // through an alias (`using D = Dir;`) or spelled unqualified from inside its namespace,
        // so go through the scoped/alias walk rather than a bare exact-key lookup.
        std::string resolvedTypeName = typeName;
        // A keyword primitive spelling (int, double, u8, i64, ...) that no enum, alias, struct
        // or interface registered under that exact name at file scope resolves to itself: skip
        // the scoped alias / enum / interface walks, which allocate candidate lists per call.
        bool plainPrimitive = false;
        if (currentNamespace_.empty() && IsKeywordPrimitiveSpelling(typeName))
        {
            plainPrimitive = enumBackingTypes.find(typeName) == enumBackingTypes.end()
                && typeAliases.find(typeName) == typeAliases.end()
                && dataStructures.find(typeName) == dataStructures.end()
                && interfaceTable.find(typeName) == interfaceTable.end()
                && gts.genericInterfaceInstances.count(typeName) == 0;
            if (plainPrimitive)
                for (const auto& frame : aliasScopeStack_)
                    if (frame.typeAliases.find(typeName) != frame.typeAliases.end())
                    {
                        plainPrimitive = false;
                        break;
                    }
        }
        if (!resolvedTypeName.empty() && !plainPrimitive)
        {
            auto it = enumBackingTypes.find(resolvedTypeName);
            if (it != enumBackingTypes.end())
                resolvedTypeName = it->second;
            else if (std::string enumKey = ResolveEnumTypeName(resolvedTypeName); !enumKey.empty())
                resolvedTypeName = enumBackingTypes.at(enumKey);
        }
        // `simd<T,N>` is a builtin special form recognised by the DECLARATOR path, which records
        // it as element type + lane count. Every other position resolves a type by NAME and hands
        // us the raw spelling, which names no registered type - decode it here so a cast target, a
        // lambda parameter and a tuple/signature component lower exactly as a declaration does.
        bool simdFromSpelling = false;
        uint64_t simdSpellingLanes = 0;
        if (std::string simdElem;
            !plainPrimitive && DecodeSimdSpelling(resolvedTypeName, simdElem, simdSpellingLanes))
        {
            resolvedTypeName = simdElem;
            simdFromSpelling = true;
        }
        // Resolve user-defined type aliases. A pointer alias (using Handle = void*) keeps its
        // trailing stars in the stored string; peel them into a local pointer-depth count and
        // OR it onto the typeAndValue pointer flags below (storage stays string-shaped).
        int aliasPtrDepth = 0;
        std::vector<uint64_t> aliasArrayDims;  // outer dimension first, from an array alias
        if (!resolvedTypeName.empty() && !plainPrimitive)
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
        // -1 = unknown; else HasInterface(resolvedTypeName) for the current spelling.
        int resolvedIsInterface = -1;
        if (!resolvedTypeName.empty() && !plainPrimitive
            && dataStructures.find(resolvedTypeName) == dataStructures.end())
        {
            resolvedIsInterface = HasInterface(resolvedTypeName) ? 1 : 0;
            if (resolvedIsInterface == 0)
            {
                std::string nsResolved = ResolveQualifiedName(resolvedTypeName);
                if (nsResolved != resolvedTypeName
                    && (dataStructures.count(nsResolved) || HasInterface(nsResolved)))
                {
                    resolvedTypeName = nsResolved;
                    resolvedIsInterface = -1;
                }
            }
        }

        // Type lookup needs imported layout, while members are demanded at their use sites.
        if (!registeringCxxProjection_ && !typeAndValue.Pointer && !typeAndValue.ElemPointer
            && aliasPtrDepth == 0
            && dataStructures.find(resolvedTypeName) != dataStructures.end())
            const_cast<LLVMBackend*>(this)->EnsureCxxRecordProjected(resolvedTypeName, false);
        // resolvedTypeName is final; hoist the struct/interface map lookups once.
        auto dsIt = dataStructures.find(resolvedTypeName);
        // A generic interface instantiation lowers to a fat pointer even before its interfaceTable
        // entry exists (the forward-ref scan materializes signatures first).
        bool isInterface = !plainPrimitive
                        && ((resolvedIsInterface >= 0 ? resolvedIsInterface == 1
                                                      : HasInterface(resolvedTypeName))
                            || gts.genericInterfaceInstances.count(resolvedTypeName) > 0);
        bool skipPointerWrap = false;

        if (resolvedTypeName == "void") { type = builder->getVoidTy(); }
        else if (resolvedTypeName == "char" || resolvedTypeName == "i8" || resolvedTypeName == "u8" || resolvedTypeName == "c8") { type = builder->getInt8Ty(); }
        else if (resolvedTypeName == "short" || resolvedTypeName == "i16" || resolvedTypeName == "u16" || resolvedTypeName == "c16") { type = builder->getInt16Ty(); }
        else if (resolvedTypeName == "int" || resolvedTypeName == "i32" || resolvedTypeName == "u32"
            || resolvedTypeName == "uint" || resolvedTypeName == "c32") { type = builder->getInt32Ty(); }
        else if (resolvedTypeName == "i64" || resolvedTypeName == "u64") { type = builder->getInt64Ty(); }
        else if (resolvedTypeName == "i128" || resolvedTypeName == "u128") { type = builder->getInt128Ty(); }
        // `long`/`ulong` are the target's native C long: i32 on Windows (LLP64), i64 on LP64.
        else if (resolvedTypeName == "long" || resolvedTypeName == "ulong")
            { type = (longBits_ == 32) ? builder->getInt32Ty() : builder->getInt64Ty(); }
        else if (resolvedTypeName == "wchar")
            { type = (wcharBits_ == 16) ? builder->getInt16Ty() : builder->getInt32Ty(); }
        else if (resolvedTypeName == "float") { type = builder->getFloatTy(); }
        else if (resolvedTypeName == "double" || resolvedTypeName == "longdouble") { type = builder->getDoubleTy(); }
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
                        TypeAndValue displayType;
                        displayType.TypeName = resolvedTypeName;
                        const std::string displayName = SpellType(*this, displayType);
                        LogGenericTypeArgsRequired(displayName, *typeParams);
                    }
                    else
                    {
                        TypeAndValue displayType;
                        displayType.TypeName = resolvedTypeName;
                        const std::string formed = SpellType(*this, displayType);
                        if (!ReportArrayViewInstantiationFailure(formed))
                            LogErrorMessage("unknown type '{}'", { formed });
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
                    { "i8..i128", "u8..u128", "float", "double", "bool",
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

// A temporary attributed to a live variable (`k.toString()` keeps `k` as its owner name) is
// not consumable: the move transfer would mark that variable moved.
bool LLVMBackend::IsConsumableTemporary(const NamedVariable& arg) const
{
        if (!arg.IsRvalue && !IsOwnedTempValue(arg)) return false;
        return arg.CallerName.empty() || FindVariableStorage(arg.CallerName).Storage == nullptr;
    }

bool LLVMBackend::IsRvalueReferenceArgument(const NamedVariable& arg) const
{
        return arg.IsExplicitMove || IsConsumableTemporary(arg);
}

bool LLVMBackend::IsCopyDeletedCxxLvalue(const NamedVariable& arg) const
{
        const std::string& type = arg.TypeAndValue.TypeName;
        if (type.empty() || arg.TypeAndValue.Pointer || arg.IsExplicitMove
            || IsCxxRvalueReferenceArgument(arg))
            return false;
        if (IsCppStructName(type)) return true;
        const CxxClassInfo* info = GetCxxClassInfo(type);
        return info != nullptr && info->hasDeletedCopyCtor;
}

std::string LLVMBackend::CxxDeletedCopyMessage(const NamedVariable& arg,
                                               const std::string& paramName,
                                               const std::string& functionName,
                                               bool moveRemedy) const
{
        std::string shown = SpellDiagnosticType(*this, arg.TypeAndValue);
        if (arg.TypeAndValue.DiagnosticTypeName.empty() && !arg.CallerName.empty())
            for (auto frame = stackNamedVariable.rbegin(); frame != stackNamedVariable.rend(); ++frame)
                if (auto it = frame->namedVariable.find(arg.CallerName);
                    it != frame->namedVariable.end()
                    && !it->second.TypeAndValue.DiagnosticTypeName.empty())
                {
                    shown = it->second.TypeAndValue.DiagnosticTypeName;
                    break;
                }
        if (moveRemedy)
            return std::format("cannot copy C++ class '{}' into parameter '{}' of '{}': its copy "
                               "constructor is deleted - pass 'move x' or a temporary",
                               shown, paramName, functionName);
        return std::format("cannot copy C++ class '{}' into parameter '{}' of '{}': its copy "
                           "constructor is deleted", shown, paramName, functionName);
}

bool LLVMBackend::FindRefusedCxxCopySink(const std::string& receiverType,
                                         const std::string& memberName,
                                         const std::vector<NamedVariable>& args,
                                         size_t& argIndex, std::string& paramName,
                                         std::string& refusalCause, bool& rvalueSibling,
                                         bool requireRefusal) const
{
        rvalueSibling = false;
        refusalCause.clear();
        auto record = cxxRecordEntries_.find(receiverType);
        if (record == cxxRecordEntries_.end()) return false;
        for (size_t i = 0; i < args.size(); ++i)
        {
            if (!IsCopyDeletedCxxLvalue(args[i])) continue;
            std::string argSpelling;
            if (!CxxSpellingForCflatType(args[i].TypeAndValue.TypeName, argSpelling)) continue;
            const std::string wanted = "const " + argSpelling + " &";
            for (const auto& member : record->second.members)
            {
                if (member.name != memberName
                    || (requireRefusal && !member.bindRefusal.starts_with("cannot be instantiated")
                        && !cxxConstructorBodyRefusals_.contains(member.linkageName))
                    || member.paramTypes.size() != args.size() + 1
                    || member.paramTypes[i + 1] != wanted
                    || !CxxDiagnosticBlamesCopyOf(member.refusalCause, argSpelling))
                    continue;
                argIndex = i;
                paramName = i + 1 < member.paramNames.size() ? member.paramNames[i + 1]
                                                             : std::string();
                if (paramName.empty()) paramName = std::format("parameter {}", i + 1);
                refusalCause = member.refusalCause;
                // A T&& sibling exists; the caller decides whether it is bound.
                rvalueSibling = std::any_of(record->second.members.begin(),
                    record->second.members.end(), [&](const auto& other) {
                        return other.name == memberName
                            && other.paramTypes.size() == args.size() + 1
                            && other.paramTypes[i + 1].ends_with("&&");
                    });
                return true;
            }
        }
        return false;
}

std::string LLVMBackend::CxxFirstDiagnosticLine(const std::string& diagnostics) const
{
        const size_t end = diagnostics.find('\n');
        std::string line = diagnostics.substr(0, end);
        const size_t error = line.find(": error: ");
        if (error == std::string::npos) return line;
        const size_t column = line.rfind(':', error - 1);
        if (column == std::string::npos) return line;
        const size_t row = line.rfind(':', column - 1);
        if (row == std::string::npos) return line;
        const std::string filename = line.substr(0, row);
        if (column == 0 || filename.empty()
            || (filename.front() != '/' && filename.find('\\') == std::string::npos))
            return line;
        const size_t slash = filename.find_last_of("/\\");
        const std::string basename = slash == std::string::npos ? filename : filename.substr(slash + 1);
        const std::string lineNumber = line.substr(row + 1, column - row - 1);
        if (lineNumber.empty() || !std::all_of(lineNumber.begin(), lineNumber.end(),
                                               [](unsigned char c) { return std::isdigit(c); }))
            return line;
        return basename + ":" + lineNumber + line.substr(error);
}

bool LLVMBackend::CxxDiagnosticBlamesCopyOf(const std::string& diagnostics,
                                            const std::string& cxxSpelling) const
{
        if (diagnostics.empty() || cxxSpelling.empty()) return false;
        // A pair<K, V> is copied member by member: clang names K's (or V's) constructor.
        std::vector<std::string> classes{ cxxSpelling };
        if (cxxSpelling.starts_with("std::pair<") && cxxSpelling.ends_with(">"))
        {
            const std::string inner = cxxSpelling.substr(10, cxxSpelling.size() - 11);
            int depth = 0;
            size_t start = 0;
            for (size_t i = 0; i <= inner.size(); ++i)
            {
                if (i < inner.size() && inner[i] == '<') ++depth;
                else if (i < inner.size() && inner[i] == '>') --depth;
                else if (i == inner.size() || (inner[i] == ',' && depth == 0))
                {
                    std::string part = inner.substr(start, i - start);
                    while (!part.empty() && part.front() == ' ') part.erase(0, 1);
                    if (!part.empty()) classes.push_back(part);
                    start = i + 1;
                }
            }
        }
        size_t begin = 0;
        while (begin < diagnostics.size())
        {
            size_t end = diagnostics.find('\n', begin);
            if (end == std::string::npos) end = diagnostics.size();
            const std::string line = diagnostics.substr(begin, end - begin);
            begin = end + 1;
            if (line.find("copy constructor") == std::string::npos
                && line.find("deleted") == std::string::npos)
                continue;
            for (const std::string& cls : classes)
                for (const char* prefix : { "", "const ", "struct ", "class ", "const struct ",
                                            "const class " })
                    if (line.find("'" + std::string(prefix) + cls + "'") != std::string::npos)
                        return true;
            if (cxxSpelling.starts_with("std::pair<") && line.find("std::pair<") != std::string::npos
                && line.find("copy constructor") != std::string::npos)
            {
                auto normalizePair = [](const std::string& value) {
                    std::string normalized;
                    for (size_t i = 0; i < value.size(); ++i)
                    {
                        if (value.compare(i, 6, "const ") == 0) { i += 5; continue; }
                        if (!std::isspace(static_cast<unsigned char>(value[i])))
                            normalized += value[i];
                    }
                    return normalized;
                };
                if (normalizePair(line).find(normalizePair(cxxSpelling)) != std::string::npos)
                    return true;
            }
        }
        return false;
}

// A refused member taking the receiver's element (or map pair) by const reference: its
// refusal is the element's deleted copy, which the call site reports once it sees the argument.
bool LLVMBackend::IsCxxElementCopySinkRefusal(const std::string& receiverType,
                                              const std::string& memberName) const
{
        // Only the template route sees the arguments; without it the refusal must speak now.
        if (!HasCxxFunctionTemplateMember(receiverType, memberName)) return false;
        auto record = cxxRecordEntries_.find(receiverType);
        if (record == cxxRecordEntries_.end()) return false;
        const std::string& owner = record->second.canonicalCtype;
        const size_t open = owner.find('<');
        if (open == std::string::npos || !owner.ends_with(">")) return false;
        const std::string ownerArgs = owner.substr(open + 1, owner.size() - open - 2);
        for (const auto& member : record->second.members)
        {
            if (member.name != memberName
                || !member.bindRefusal.starts_with("cannot be instantiated"))
                continue;
            for (size_t i = 1; i < member.paramTypes.size(); ++i)
            {
                // `const E &` or by-value E, with E the receiver's element (a map's pair).
                std::string referent = member.paramTypes[i];
                if (referent.starts_with("const ") && referent.ends_with(" &"))
                    referent = referent.substr(6, referent.size() - 8);
                else if (referent.ends_with("&") || referent.ends_with("*"))
                    continue;
                std::string element = referent;
                if (element.starts_with("std::pair<") && element.ends_with(">"))
                    element = element.substr(10, element.size() - 11);
                if (element.starts_with("const ")) element = element.substr(6);
                if (element.empty() || !ownerArgs.starts_with(element)
                    || (ownerArgs.size() != element.size() && ownerArgs[element.size()] != ','))
                    continue;
                // Clang must have named E's copy as the failure.
                if (CxxDiagnosticBlamesCopyOf(member.refusalCause, referent)) return true;
            }
        }
        return false;
}

bool LLVMBackend::IsCxxRvalueRefClassResult(const NamedVariable& nv) const
{
        const TypeAndValue& t = nv.TypeAndValue;
        if (!t.IsRvalueRef || !t.Pointer || t.ElemPointer || t.IsCxxRefToPointer) return false;
        if (nv.Storage != nullptr || nv.Primary == nullptr || !nv.Primary->getType()->isPointerTy())
            return false;
        return IsCxxRecord(t.TypeName);
}

bool LLVMBackend::IsCxxMacroGlobalName(const std::string& name)
{
        // Identity, not spelling: a local of the same name shadows the macro and is no macro.
        if (FindVariableStorage(name).Storage != nullptr) return false;
        // An implicit-this field resolves before globals, so it shadows the macro too.
        if (HasMemberVariable(name)) return false;
        auto it = globalNamedVariable.find(name);
        return it != globalNamedVariable.end() && it->second != nullptr
            && cxxMacroGlobalNames_.count(it->second->getName().str()) != 0;
}

bool LLVMBackend::IsCxxRvalueReferenceArgument(const NamedVariable& arg) const
{
        // Rvalue-ness must be PROVEN. Without argument provenance nothing is proven, and an
        // argument that has addressable storage or came back as a reference is an lvalue.
        // An imported integer-zero macro expands to an integer literal prvalue in C++.
        if (IsNullPointerConstantArgument(arg, true)) return true;
        // Every imported macro constant, and a `?:` joining two of them, is a C++ prvalue.
        if (arg.IsCxxMacroGlobal || arg.IsCxxMacroJoin) return true;
        if (arg.IsExplicitMove) return true;
        if (arg.IsRvalue && arg.TypeAndValue.Pointer) return true;
        if (arg.IsRvalue && arg.TypeAndValue.IsRvalueRef) return true;
        if (arg.IsRvalue && IsConsumableTemporary(arg)) return true;
        if (arg.IsRvalue)
        {
            if (arg.Storage != nullptr) return false;
            if (!arg.CallerName.empty() && FindVariableStorage(arg.CallerName).Storage != nullptr)
                return false;
            return true;
        }
        if (IsRvalueReferenceArgument(arg)) return true;
        if (arg.TypeAndValue.IsAlias) return false;   // a reference-returning call result
        llvm::Value* storage = arg.Storage;
        if (storage == nullptr && !arg.CallerName.empty())
            storage = FindVariableStorage(arg.CallerName).Storage;
        if (storage == nullptr) return true;          // nothing addressable behind it
        if (IsImportedEnumeratorStorage(storage)) return true;   // an enumerator is a prvalue
        // Storage alone is not enough: a conversion keeps the SOURCE variable's slot while its
        // value is a temporary. An lvalue's value is read from that very slot.
        if (llvm::isa<llvm::PHINode>(storage)) return false;   // a '?:' join of addresses
        if (arg.Primary == nullptr || arg.Primary == storage) return false;
        auto* load = llvm::dyn_cast<llvm::LoadInst>(arg.Primary);
        return load == nullptr || load->getPointerOperand() != storage;
}

/*
 * POSITIVE proof that a scalar call argument is an lvalue a C++ `T&` may bind: the front end
 * classified it as an lvalue designator (or a call returning `T&`), it is not flagged rvalue, and
 * its value was loaded from its own storage in this very expression. No name lookup.
 */
bool LLVMBackend::IsProvenCxxScalarLvalue(const NamedVariable& arg, bool thinFnPtrSlot) const
{
        if (arg.CxxLvalueKind == 0 || arg.CxxLvalueKind == 3 || arg.IsExplicitMove) return false;
        // An imported integer-zero macro is a literal prvalue, as IsCxxRvalueReferenceArgument says.
        if (IsNullPointerConstantArgument(arg, true)) return false;
        if (arg.IsCxxMacroGlobal || arg.IsCxxMacroJoin) return false;
        if (IsRvalueReferenceArgument(arg)) return false;
        // A function<> value is a function pointer VALUE in C++, and the extractor cannot spell
        // `R (*&)(A)`; a caller passing the slot as `void *` opts in (thinFnPtrSlot).
        if (!thinFnPtrSlot && arg.TypeAndValue.IsThinFnPtr()) return false;
        /*
         * A call is an lvalue only through a `T&` result (its IsRvalue marks the call itself):
         * an alias, or a value loaded from the very pointer the call returned.
         */
        if (arg.CxxLvalueKind == 2)
        {
            if (arg.TypeAndValue.IsAlias) return true;
            auto* load = llvm::dyn_cast_or_null<llvm::LoadInst>(arg.Primary);
            return load != nullptr && llvm::isa<llvm::CallBase>(load->getPointerOperand())
                && (arg.Storage == nullptr || arg.Storage == load->getPointerOperand());
        }
        if (arg.IsRvalue) return false;
        if (arg.TypeAndValue.IsAlias) return true;
        if (arg.Storage == nullptr) return false;
        // An imported enumerator is a prvalue in C++ ([expr.prim.id.unqual]); its global is no object.
        if (IsImportedEnumeratorStorage(arg.Storage)) return false;
        if (llvm::isa<llvm::PHINode>(arg.Storage)) return true;   // a '?:' join of addresses
        auto* load = llvm::dyn_cast_or_null<llvm::LoadInst>(arg.Primary);
        return load != nullptr && load->getPointerOperand() == arg.Storage;
}

bool LLVMBackend::IsCxxReferenceParameter(const FunctionSymbol& candidate, size_t index) const
{
        if (!candidate.IsCxx || candidate.UniqueName.starts_with("__cflat_udc_")) return false;
        const std::string spelling = CxxReferenceParameterSpelling(candidate, index);
        if (!spelling.empty()) return spelling.find('&') != std::string::npos;
        return index < candidate.Parameters.size()
            && (candidate.Parameters[index].IsRvalueRef
                || candidate.Parameters[index].IsCxxRefToPointer);
}

std::string LLVMBackend::CxxReferenceParameterSpelling(const FunctionSymbol& candidate,
                                                       size_t index) const
{
        if (!candidate.IsCxx || candidate.UniqueName.starts_with("__cflat_udc_")) return {};
        // A default-argument wrapper takes a PREFIX of its declaration's parameters: same spelling.
        const std::string declared = candidate.UniqueName.starts_with("__cflat_dflt_")
            ? CxxDeclarationLinkageName(candidate.UniqueName) : candidate.UniqueName;
        for (const std::string* linkage : { &candidate.UniqueName, &declared })
        {
            if (auto rawIt = cxxFunctionSignatures_.find(candidate.SourceName);
                rawIt != cxxFunctionSignatures_.end())
                for (const auto& raw : rawIt->second)
                    if (raw.linkageName == *linkage
                        && index < raw.paramSpellings.size())
                        return raw.paramSpellings[index];
            if (auto memo = cxxMethodLinkageMemo_.find(*linkage); memo != cxxMethodLinkageMemo_.end())
                if (auto record = cxxRecordEntries_.find(memo->second.first);
                    record != cxxRecordEntries_.end() && memo->second.second < record->second.members.size())
                {
                    const auto& method = record->second.members[memo->second.second];
                    if (method.linkageName == *linkage && index < method.paramTypes.size())
                        return method.paramTypes[index];
                }
            for (const auto& [className, record] : cxxRecordEntries_)
                for (size_t mi = 0; mi < record.members.size(); ++mi)
                {
                    const auto& method = record.members[mi];
                    if (method.linkageName == *linkage
                        && index < method.paramTypes.size())
                    {
                        cxxMethodLinkageMemo_[*linkage] = { className, mi };
                        return method.paramTypes[index];
                    }
                }
            if (declared == candidate.UniqueName) break;
        }
        return {};
}

bool LLVMBackend::CxxReferenceArgumentMatches(const TypeAndValue& param,
                                               const NamedVariable& arg) const
{
        if (!param.Pointer && !param.IsAlias) return false;
        if (param.IsCxxRefToPointer)
            return arg.TypeAndValue.Pointer
                && (arg.BaseType == nullptr || arg.BaseType == GetType(param));
        if (param.IsRvalueRef && param.ElemPointer && arg.TypeAndValue.Pointer)
        {
            auto referent = param;
            referent.ElemPointer = false;
            referent.Pointer = true;
            referent.PointerDepth = 1;
            referent.IsAlias = false;
            referent.IsRvalueRef = false;
            return arg.BaseType != nullptr && GetType(referent) == arg.BaseType;
        }
        if (param.IsRvalueRef && !param.ElemPointer && !arg.TypeAndValue.Pointer)
        {
            TypeAndValue referent = param;
            referent.Pointer = referent.IsAlias = referent.IsRvalueRef = false;
            referent.PointerDepth = 0;
            if (referent.IsPrimitive() && arg.TypeAndValue.IsPrimitive()
                && ClassifyScalarArgConversion(arg, referent) == ScalarArgConversion::Widen)
                return true; // a standard conversion materializes a temporary for T&&
        }
        // A C++ signature can expand a typedef while the CFlat argument keeps the alias identity.
        // Compare their canonical foreign spellings so the same specialization matches either way.
        if (!arg.TypeAndValue.Pointer && !arg.TypeAndValue.TypeName.empty()
            && !param.TypeName.empty())
        {
            std::string paramSpelling;
            std::string argSpelling;
            if (CxxSpellingForCflatType(param.TypeName, paramSpelling)
                && CxxSpellingForCflatType(arg.TypeAndValue.TypeName, argSpelling)
                && SqueezeCxxSpelling(paramSpelling) == SqueezeCxxSpelling(argSpelling))
                return true;
        }
        if (arg.Storage == nullptr && arg.BaseType != nullptr
            && !arg.BaseType->isStructTy()
            && !IsForeignCxxClassWithConstructors(param.TypeName)
            && llvm::isa_and_nonnull<llvm::Constant>(arg.Primary))
            return true;
        if (arg.TypeAndValue.Pointer) return false;
        if (!param.TypeName.empty() && param.TypeName == arg.TypeAndValue.TypeName)
            return true;
        TypeAndValue referent = param;
        referent.Pointer = false;
        referent.ElemPointer = false;
        referent.PointerDepth = 0;
        referent.IsAlias = false;
        referent.IsRvalueRef = false;
        referent.IsCxxConstRef = false;
        return arg.BaseType != nullptr && GetType(referent) == arg.BaseType;
}

int LLVMBackend::ScoreMoveAgreement(const std::vector<NamedVariable>& arguments, const FunctionSymbol& candidate) const
{
        int moveScore = 0;
        auto pi = candidate.Parameters.begin();
        for (const auto& arg : arguments)
        {
            if (pi == candidate.Parameters.end()) break;
            // Explicit moves prefer consuming overloads; a temporary does too, but only where
            // the borrow overload would deep-copy an aggregate for nothing.
            bool argIsRValue = IsConsumableTemporary(arg) && !arg.TypeAndValue.Pointer
                && (arg.IsOwningString || arg.IsOwningStruct
                    || IsDataStructure(arg.TypeAndValue.TypeName));
            bool argOwning = arg.IsExplicitMove || argIsRValue;
            if (pi->IsMove == argOwning)
                moveScore++;
            const bool rvalue = IsRvalueReferenceArgument(arg);
            if (pi->IsRvalueRef == rvalue)
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
            // A namespaced or aliased enum only matches through the scoped walk.
            if (std::string k = ResolveEnumTypeName(cur); !k.empty()) { cur = enumBackingTypes.at(k); continue; }
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

// EXACT registry hit only. The name handed in is either the declaring-scope key recorded at
// declaration time or an already-resolved spelling, so no scope walk runs here - walking the
// namespace active at COMPARISON time would answer for the wrong scope.
std::string LLVMBackend::FuncPtrStructKey(const std::string& name) const
{
        if (name.empty()) return "";
        std::string resolved = ResolveFuncPtrTypeSpelling(name);
        if (resolved.empty() || HasInterface(resolved)) return "";
        if (dataStructures.count(resolved) == 0) return "";
        return FuncPtrAbiCanonKey(resolved);
    }

std::string LLVMBackend::FuncPtrComponentDeclaringKey(const std::string& name) const
{
        if (name.empty()) return "";
        std::string key = ResolveTypeArgBaseName(name);
        if (key != name) return key;                            // qualified by the scoped walk
        if (name.find('.') != std::string::npos) return name;    // already qualified
        // The walk did not qualify it. It still names the GLOBAL type when one is registered
        // under exactly that key - global scope is the last rung of the very same walk.
        return dataStructures.count(name) != 0 ? name : "";
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
        // The declaring-scope key recorded where the spelling still had a scope; with none recorded
        // the spelling is tried as an exact key (FuncPtrSigOfSymbol, already fully scoped).
        std::string key = FuncPtrStructKey(resolvedKey);
        if (key.empty()) key = FuncPtrStructKey(resolved);
        if (!key.empty())
        {
            c.Known = true;
            c.Canon = pointer ? "p:s" : "s";
            c.StructKey = std::move(key);
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
        // Two struct components at the same shape. Both sides resolved to exactly one declaring
        // key, so different keys are different types; an unresolved side proves nothing above.
        if (!a.StructKey.empty() && !b.StructKey.empty())
            return a.StructKey != b.StructKey;
        return false;
    }

bool LLVMBackend::FuncPtrSignatureOf(const LLVMBackend::TypeAndValue& tv, std::vector<FuncPtrComponent>& out) const
{
        TypeAndValue signature = tv;
        if (signature.FuncPtrReturnTypeName.empty() && !signature.Pointer)
            if (const auto* encoded = GetEncodedClosureType(signature.TypeName))
                signature = *encoded;
        if (signature.FuncPtrReturnTypeName.empty()) return false;
        out.clear();
        out.push_back(FuncPtrComponentOf(signature.FuncPtrReturnTypeName, signature.FuncPtrReturnPointer,
            signature.FuncPtrReturnPointerDepth, signature.FuncPtrReturnResolvedKey));
        for (const auto& p : signature.FuncPtrParams)
            out.push_back(FuncPtrComponentOf(p.TypeName, p.Pointer, p.PointerDepth, p.ResolvedTypeKey));
        return true;
    }

static bool IsUnknownFatClosureSignature(const LLVMBackend::TypeAndValue& tv)
{
        return !tv.Pointer && tv.FuncPtrReturnTypeName.empty()
            && ((tv.IsFunctionPointer && !tv.IsThinFnPtr())
                || tv.TypeName == "__closure_fat_ptr");
}

bool LLVMBackend::FuncPtrSignaturesProvablyDiffer(const LLVMBackend::TypeAndValue& a,
        const LLVMBackend::TypeAndValue& b) const
{
        TypeAndValue left = a, right = b;
        if (left.FuncPtrReturnTypeName.empty() && !left.Pointer)
            if (const auto* encoded = GetEncodedClosureType(left.TypeName))
                left = *encoded;
        if (right.FuncPtrReturnTypeName.empty() && !right.Pointer)
            if (const auto* encoded = GetEncodedClosureType(right.TypeName))
                right = *encoded;
        std::vector<FuncPtrComponent> sa, sb;
        if (!FuncPtrSignatureOf(left, sa))
            return IsUnknownFatClosureSignature(left)
                && FuncPtrSignatureOf(right, sb)
                && right.IsFunctionPointer && !right.IsThinFnPtr();
        if (!FuncPtrSignatureOf(right, sb))
            return false;
        // C: `int(*)(int, ...)` and `int(*)(int)` are different types, whatever the fixed parts.
        if (left.FuncPtrVariadic != right.FuncPtrVariadic)
            return true;
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
        sig.FuncPtrVariadic = sym.Variadic;
        sig.FuncPtrReturnPointerDepth = sym.ReturnType.ValuePointerDepth();
        // Pointee const is part of the function type; a C++ function's stays unrecorded (phase 5).
        if (!sym.IsCxx)
            sig.FuncPtrReturnConstMask = sym.ReturnType.PointeeConstMask();
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
            fp.IsReturnInferredSink = p.IsReturnInferredSink;
            fp.IsWriteInferredSink = p.IsWriteInferredSink;
            fp.PointerDepth = p.ValuePointerDepth();
            if (!sym.IsCxx) fp.ConstMask = p.PointeeConstMask();
            // A C++ member's mapped pointee const is recorded only as IsCxxPointeeConst.
            fp.IsPointeeConst = p.IsSpelledPointeeConst || (sym.IsCxx && p.IsCxxPointeeConst);
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
        {
            const auto sig = FuncPtrSigOfSymbol(sym);
            if (!FuncPtrSignaturesProvablyDiffer(sig, param)
                || FuncPtrFewerParamsCanAdapt(sig, param))
                return false;
        }
        return true;
    }

const LLVMBackend::FunctionSymbol* LLVMBackend::NamedFunctionOverloadForFuncPtr(
        const std::string& functionName, const TypeAndValue& param) const
{
        auto it = functionTable.find(ResolveQualifiedName(functionName));
        if (it == functionTable.end()) return nullptr;
        for (const auto& sym : it->second)
        {
            const auto sig = FuncPtrSigOfSymbol(sym);
            if (!sym.IsMethod && (!FuncPtrSignaturesProvablyDiffer(sig, param)
                || FuncPtrFewerParamsCanAdapt(sig, param)))
                return &sym;
        }
        return nullptr;
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
        TypeAndValue signature = tv;
        if (signature.FuncPtrReturnTypeName.empty() && !signature.Pointer)
            if (const auto* encoded = GetEncodedClosureType(signature.TypeName))
                signature = *encoded;
        // One '*' per recorded level, so a depth mismatch is visible as `int**` vs `int*` rather
        // than as two identical-looking spellings; recorded pointee const prints whole-word.
        auto component = [&](const std::string& name, bool pointer, int depth, int constMask) {
            TypeAndValue type;
            type.TypeName = name;
            type.Pointer = pointer;
            type.PointerDepth = depth;
            if (pointer && constMask > 0)
            {
                type.PointerDepth = std::max(depth, 1);
                type.ElemPointer = type.PointerDepth >= 2;
                type.ConstMask = static_cast<uint8_t>(constMask);
                return SpellTypeForSymbolDump(*this, type);
            }
            return SpellType(*this, type);
        };
        std::string s = component(signature.FuncPtrReturnTypeName, signature.FuncPtrReturnPointer,
            signature.FuncPtrReturnPointerDepth, signature.FuncPtrReturnConstMask) + "(";
        for (size_t i = 0; i < signature.FuncPtrParams.size(); i++)
        {
            if (i > 0) s += ", ";
            const auto& p = signature.FuncPtrParams[i];
            s += component(p.TypeName, p.Pointer, p.PointerDepth, p.ConstMask);
        }
        if (signature.FuncPtrVariadic) s += signature.FuncPtrParams.empty() ? "..." : ", ...";
        return s + ")";
    }

bool LLVMBackend::FuncPtrPointeeConstDiffers(const TypeAndValue& value, const TypeAndValue& slot,
        std::string& valueSpelled, std::string& slotSpelled) const
{
        TypeAndValue source = value, target = slot;
        if (source.FuncPtrReturnTypeName.empty() && !source.Pointer)
            if (const auto* encoded = GetEncodedClosureType(source.TypeName))
                source = *encoded;
        if (target.FuncPtrReturnTypeName.empty() && !target.Pointer)
            if (const auto* encoded = GetEncodedClosureType(target.TypeName))
                target = *encoded;
        if (source.FuncPtrReturnTypeName.empty() || target.FuncPtrReturnTypeName.empty()
            || !target.IsFunctionPointer)
            return false;
        auto differ = [](int a, int b) { return a >= 0 && b >= 0 && a != b; };
        // A by-value component records 0; a pointer alias component (`CStr`) records its alias's levels.
        bool differs = differ(source.FuncPtrReturnConstMask, target.FuncPtrReturnConstMask);
        // The shared prefix: an arity difference is the bind rule's business.
        for (size_t i = 0; i < source.FuncPtrParams.size() && i < target.FuncPtrParams.size(); i++)
            differs = differs || differ(source.FuncPtrParams[i].ConstMask, target.FuncPtrParams[i].ConstMask);
        if (!differs) return false;
        auto family = [](const TypeAndValue& t) {
            return std::string(t.IsFunctionPointer && !t.IsThinFnPtr() ? "Lambda" : "function");
        };
        valueSpelled = family(source) + "<" + FuncPtrSpellingOf(source) + ">";
        slotSpelled = family(target) + "<" + FuncPtrSpellingOf(target) + ">";
        return true;
    }

LLVMBackend::TypeAndValue LLVMBackend::FuncPtrValueSignature(const NamedVariable& value) const
{
        if (auto* fn = llvm::dyn_cast_or_null<llvm::Function>(value.Primary))
            if (const auto* symbol = FindSymbolForFunction(fn))
                return FuncPtrSigOfSymbol(*symbol);
        return value.TypeAndValue;
    }

std::string LLVMBackend::DescribeFuncPtrConstMismatch(const TypeAndValue& slot, const NamedVariable& value,
        FuncPtrConstRole role) const
{
        std::string from, to;
        if (!FuncPtrPointeeConstDiffers(FuncPtrValueSignature(value), slot, from, to)) return "";
        if (role == FuncPtrConstRole::Return)
            return std::format("cannot return '{}' as '{}': the conversion discards const", from, to);
        return std::format("cannot convert '{}' to '{}': the conversion discards const. "
            "Use an explicit cast if the object is really writable", from, to);
    }

// Variadic-ness is a signature fact: C never converts between `R(*)(P, ...)` and `R(*)(P)`.
static std::string VariadicMismatchReason(bool slotVariadic)
{
        return slotVariadic
            ? "the slot takes a variadic '...' tail and the function does not"
            : "the function takes a variadic '...' tail and the slot does not";
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
        TypeAndValue argument = arg, parameter = param;
        if (argument.FuncPtrReturnTypeName.empty() && !argument.Pointer)
            if (const auto* encoded = GetEncodedClosureType(argument.TypeName))
                argument = *encoded;
        if (parameter.FuncPtrReturnTypeName.empty() && !parameter.Pointer)
            if (const auto* encoded = GetEncodedClosureType(parameter.TypeName))
                parameter = *encoded;
        std::vector<FuncPtrComponent> sa, sb;
        const bool hasArgumentSignature = FuncPtrSignatureOf(argument, sa);
        const bool hasParameterSignature = FuncPtrSignatureOf(parameter, sb);
        if (!hasArgumentSignature || !hasParameterSignature)
        {
            if (IsUnknownFatClosureSignature(argument) && hasParameterSignature
                && parameter.IsFunctionPointer && !parameter.IsThinFnPtr())
                return std::format("function-pointer signature mismatch: parameter takes '{}' but the "
                    "fat closure's signature is unknown", FuncPtrSpellingOf(parameter));
            return "";
        }
        std::string why = argument.FuncPtrVariadic != parameter.FuncPtrVariadic
            ? VariadicMismatchReason(parameter.FuncPtrVariadic)
            : FuncPtrDifferenceOf(sa, sb);
        if (why.empty()) return "";
        return std::format(
            "function-pointer signature mismatch: parameter takes '{}' but the argument is "
            "'{}' - {}", FuncPtrSpellingOf(parameter), FuncPtrSpellingOf(argument), why);
    }

std::string LLVMBackend::DescribeFuncPtrVariadicMismatch(const LLVMBackend::TypeAndValue& slot,
        const LLVMBackend::TypeAndValue& value) const
{
        TypeAndValue slotSignature = slot, valueSignature = value;
        if (slotSignature.FuncPtrReturnTypeName.empty() && !slotSignature.Pointer)
            if (const auto* encoded = GetEncodedClosureType(slotSignature.TypeName))
                slotSignature = *encoded;
        if (valueSignature.FuncPtrReturnTypeName.empty() && !valueSignature.Pointer)
            if (const auto* encoded = GetEncodedClosureType(valueSignature.TypeName))
                valueSignature = *encoded;
        std::vector<FuncPtrComponent> ss, sv;
        if (!FuncPtrSignatureOf(slotSignature, ss) || !FuncPtrSignatureOf(valueSignature, sv))
            return "";
        if (slotSignature.FuncPtrVariadic == valueSignature.FuncPtrVariadic)
            return "";
        return std::format(
            "function-pointer signature mismatch: slot takes '{}' but the value is '{}' - {}",
            FuncPtrSpellingOf(slotSignature), FuncPtrSpellingOf(valueSignature),
            VariadicMismatchReason(slotSignature.FuncPtrVariadic));
    }

// The bind rule itself, shared by named functions and lambda literals. Empty when fn can bind dest.
std::string LLVMBackend::FuncPtrBindReason(const LLVMBackend::TypeAndValue& fn,
        const LLVMBackend::TypeAndValue& dest, bool compareReturnOwnership) const
{
        TypeAndValue function = fn, destination = dest;
        if (function.FuncPtrReturnTypeName.empty() && !function.Pointer)
            if (const auto* encoded = GetEncodedClosureType(function.TypeName))
                function = *encoded;
        if (destination.FuncPtrReturnTypeName.empty() && !destination.Pointer)
            if (const auto* encoded = GetEncodedClosureType(destination.TypeName))
                destination = *encoded;
        std::vector<FuncPtrComponent> sa, sb;
        if (!FuncPtrSignatureOf(function, sa) || !FuncPtrSignatureOf(destination, sb))
            return "";
        std::string why;
        if (function.FuncPtrVariadic != destination.FuncPtrVariadic)
            why = VariadicMismatchReason(destination.FuncPtrVariadic);
        else if (sa.size() > sb.size())
            why = std::format("it takes {} parameter(s) but the slot supplies only {}",
                sa.size() - 1, sb.size() - 1);
        for (size_t i = 0; i < sa.size() && why.empty(); i++)
            if (ComponentsProvablyDiffer(sa[i], sb[i]))
                why = (i == 0) ? "the return type differs" : std::format("parameter {} differs", i);
        if (why.empty() && compareReturnOwnership
            && function.FuncPtrReturnPointer && destination.FuncPtrReturnPointer
            && function.FuncPtrReturnOwned && !destination.FuncPtrReturnOwned)
            why = "it returns an owning 'move' pointer but the slot's return is borrowed (declare the "
                  "slot's return 'move')";
        return why;
    }

// T287: the ownership half of the bind rule, for a function-pointer VALUE. A named function gets
// the same two facts from FuncPtrBindReason; a lambda literal or variable carries them on its type.
std::string LLVMBackend::FuncPtrValueOwnershipReason(const LLVMBackend::TypeAndValue& source,
        const LLVMBackend::TypeAndValue& slot)
{
        TypeAndValue src = source, dst = slot;
        if (src.FuncPtrReturnTypeName.empty() && !src.Pointer)
            if (const auto* encoded = GetEncodedClosureType(src.TypeName))
                src = *encoded;
        if (dst.FuncPtrReturnTypeName.empty() && !dst.Pointer)
            if (const auto* encoded = GetEncodedClosureType(dst.TypeName))
                dst = *encoded;
        if (!dst.IsFunctionPointer) return "";
        if (src.FuncPtrReturnPointer && dst.FuncPtrReturnPointer
            && src.FuncPtrReturnOwned && !dst.FuncPtrReturnOwned)
            return "it returns an owning 'move' pointer but the slot's return is borrowed (declare the "
                   "slot's return 'move')";
        // Different arities are the adaptation rule's business (FuncPtrFewerParamsCanAdapt), not 'move'.
        if (src.FuncPtrParams.size() != dst.FuncPtrParams.size()) return "";
        for (size_t i = 0; i < dst.FuncPtrParams.size(); i++)
            if (!FuncPtrParamMoveAgrees(dst.FuncPtrParams[i], src.FuncPtrParams[i]))
                return std::format("parameter {} differs in 'move' modifier - 'move' is part of the "
                    "function-pointer type", i + 1);
        return "";
    }

bool LLVMBackend::FuncPtrFewerParamsCanAdapt(const TypeAndValue& source,
        const TypeAndValue& slot) const
{
        TypeAndValue sourceSig = source;
        TypeAndValue slotSig = slot;
        if (!sourceSig.IsFunctionPointer && !sourceSig.Pointer)
            if (const auto* enc = GetEncodedClosureType(sourceSig.TypeName))
                sourceSig = *enc;
        if (!slotSig.IsFunctionPointer && !slotSig.Pointer)
            if (const auto* enc = GetEncodedClosureType(slotSig.TypeName))
                slotSig = *enc;
        if (!slotSig.IsFunctionPointer || slotSig.IsThinFnPtr()
            || sourceSig.FuncPtrParams.size() >= slotSig.FuncPtrParams.size()
            || sourceSig.FuncPtrVariadic || slotSig.FuncPtrVariadic
            || !FuncPtrBindReason(sourceSig, slotSig).empty())
            return false;
        for (size_t i = 0; i < sourceSig.FuncPtrParams.size(); ++i)
        {
            const auto& src = sourceSig.FuncPtrParams[i];
            const auto& dst = slotSig.FuncPtrParams[i];
            if ((src.AllocAlignValue != 0 && dst.AllocAlignValue != 0
                    && src.AllocAlignValue != dst.AllocAlignValue)
                || src.IsMove != dst.IsMove)
                return false;
        }
        return true;
    }

std::string LLVMBackend::DescribeFuncPtrBindMismatch(const std::string& functionName,
        const LLVMBackend::TypeAndValue& fn, const LLVMBackend::TypeAndValue& dest,
        bool compareReturnOwnership) const
{
        std::string why = FuncPtrBindReason(fn, dest, compareReturnOwnership);
        if (why.empty()) return "";
        return std::format("cannot bind function '{}' to function-pointer type '{}': '{}' is "
            "'{}' - {}. A function pointer is called with no conversion site, so the signature "
            "has to match.",
            SpellFunctionSymbol(*this, functionName), FuncPtrSpellingOf(dest),
            SpellFunctionSymbol(*this, functionName), FuncPtrSpellingOf(fn), why);
    }

std::string LLVMBackend::DescribeFuncPtrValueMismatch(const LLVMBackend::TypeAndValue& slot,
        const LLVMBackend::TypeAndValue& value) const
{
        TypeAndValue slotSignature = slot, valueSignature = value;
        if (slotSignature.FuncPtrReturnTypeName.empty() && !slotSignature.Pointer)
            if (const auto* encoded = GetEncodedClosureType(slotSignature.TypeName))
                slotSignature = *encoded;
        if (valueSignature.FuncPtrReturnTypeName.empty() && !valueSignature.Pointer)
            if (const auto* encoded = GetEncodedClosureType(valueSignature.TypeName))
                valueSignature = *encoded;
        if (IsUnknownFatClosureSignature(valueSignature)
            && slotSignature.IsFunctionPointer && !slotSignature.IsThinFnPtr()
            && !slotSignature.FuncPtrReturnTypeName.empty())
            return std::format("function-pointer signature mismatch: slot takes '{}' but the fat "
                "closure's signature is unknown", FuncPtrSpellingOf(slotSignature));
        if (std::string why = DescribeFuncPtrVariadicMismatch(slotSignature, valueSignature); !why.empty())
            return why;
        if (!slotSignature.IsFunctionPointer || !valueSignature.IsFunctionPointer)
            return "";
        std::string why = FuncPtrBindReason(valueSignature, slotSignature);
        if (why.empty() && valueSignature.TypeName == "__closure_fat_ptr"
            && valueSignature.FuncPtrParams.size() < slotSignature.FuncPtrParams.size())
            why = std::format("it takes {} parameter(s) but the slot supplies {}; a Lambda<> value may carry captured state, and "
                "a closure's environment is its last argument, so the slot's extra arguments could be read as that "
                "state - wrap it in a lambda literal that takes the slot's parameters",
                valueSignature.FuncPtrParams.size(), slotSignature.FuncPtrParams.size());
        if (why.empty()) return "";
        return std::format("function-pointer signature mismatch: slot takes '{}' but the value is '{}' - {}",
            FuncPtrSpellingOf(slotSignature), FuncPtrSpellingOf(valueSignature), why);
    }

std::string LLVMBackend::DescribeFuncPtrValueMismatch(const LLVMBackend::TypeAndValue& slot,
        const NamedVariable& value) const
{
        TypeAndValue source = value.TypeAndValue;
        if (auto* fn = llvm::dyn_cast_or_null<llvm::Function>(value.Primary))
            if (const auto* symbol = FindSymbolForFunction(fn))
                source = FuncPtrSigOfSymbol(*symbol);
        if (source.FuncPtrReturnTypeName.empty() && !source.Pointer)
            if (const auto* encoded = GetEncodedClosureType(source.TypeName))
                source = *encoded;
        if (source.FuncPtrReturnTypeName.empty())
            return IsUnknownFatClosureSignature(source)
                ? DescribeFuncPtrValueMismatch(slot, source) : "";
        if (FuncPtrFewerParamsCanAdapt(source, slot)
            && ((value.ClosureEnvKnownFree && !value.ClosureEnvProvenanceTainted)
                || value.IsExplicitMove
                || (value.Primary && llvm::isa<llvm::Function>(value.Primary))))
            return "";
        return DescribeFuncPtrValueMismatch(slot, source);
    }

std::string LLVMBackend::DescribeLambdaBindMismatch(const LLVMBackend::TypeAndValue& lambda,
        const LLVMBackend::TypeAndValue& slot) const
{
        std::string why = FuncPtrBindReason(lambda, slot);
        if (why.empty()) return "";
        return std::format("cannot bind this lambda of type '{}' to function-pointer type '{}' - {}. "
            "A lambda's parameters must match the slot it is bound to; it may take fewer parameters "
            "than the slot supplies.", FuncPtrSpellingOf(lambda), FuncPtrSpellingOf(slot), why);
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

void LLVMBackend::LogGenericTypeArgsRequired(const std::string& displayName,
                                             const std::vector<std::string>& parameterNames) const
{
    std::string names;
    std::string exampleArgs;
    for (size_t i = 0; i < parameterNames.size(); ++i)
    {
        if (i != 0)
        {
            names += ", ";
            exampleArgs += ", ";
        }
        names += parameterNames[i];
        exampleArgs += "int";
    }
    const std::string count = std::format("{} type parameter{}", parameterNames.size(),
        parameterNames.size() == 1 ? "" : "s");
    LogErrorMessage("'{}' is a generic type; type arguments are required "
        "(expects {}: {}), e.g. {}",
        { displayName, count, names, displayName + "<" + exampleArgs + ">" });
}

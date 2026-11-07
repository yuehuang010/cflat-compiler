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

llvm::Value* LLVMBackend::MaterializeCxxDefaultArgument(const cflat_cinterop::RawDefaultArg& def,
                                                        const TypeAndValue& param)
{
        llvm::Type* ty = GetType(param);
        if (ty == nullptr) return nullptr;
        if (def.kind == "nullptr")
            return ty->isPointerTy() ? llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ty))
                                     : nullptr;
        if (def.kind == "bool" && ty->isIntegerTy(1))
            return llvm::ConstantInt::get(ty, def.value != "0");
        if ((def.kind == "int" || def.kind == "enum") && ty->isIntegerTy())
            return llvm::ConstantInt::get(*context, llvm::APInt(ty->getIntegerBitWidth(), def.value, 10));
        if ((def.kind == "float" || def.kind == "double") && ty->isFloatingPointTy())
            return llvm::ConstantFP::get(ty, std::stod(def.value));
        return nullptr;
}

void LLVMBackend::RejectLiteralZeroCxxConstructorFunctionSlotArgs(
        const std::string& typeName, const std::vector<NamedVariable>& args,
        const std::vector<llvm::Value*>& values) const
{
        const CxxClassInfo* info = nullptr;
        for (size_t i = 0; i < args.size() && i < values.size(); ++i)
        {
            if (args[i].LiteralIdentity.empty() && !args[i].IsCxxMacroGlobal) continue;
            llvm::Value* source = args[i].Primary != nullptr ? args[i].Primary : values[i];
            if (!IsLiteralZeroIntegerToPointer(source, args[i])) continue;
            // A constructor template may take the zero (clang deduces T = int), so its set stays with clang.
            if (info == nullptr) info = GetCxxClassInfoWithConstructors(typeName);
            if (info == nullptr || info->hasCtorTemplate) return;
            bool hasApplicableCtor = false;
            bool everyCandidateUsesFunctionSlot = true;
            for (const auto& ctor : info->constructors)
            {
                if (ctor.params.empty() || ctor.params.size() < args.size() + 1) continue;
                // Copy / move / deleted / inaccessible ctors never take a literal 0 ([over.best.ics]/4).
                if (ctor.isDeleted || ctor.access != cflat_cinterop::AccessPublic
                    || ctor.isCopyCtor || ctor.isMoveCtor)
                    continue;
                bool defaultsPresent = true;
                for (size_t n = args.size() + 1; n < ctor.params.size(); ++n)
                    if (n >= ctor.defaultArgs.size() || ctor.defaultArgs[n].kind.empty())
                        defaultsPresent = false;
                if (!defaultsPresent) continue;
                hasApplicableCtor = true;
                const auto& param = ctor.params[i + 1];
                if (!param.IsFunctionPointer && !IsStdFunctionTypeName(param.TypeName)
                    && !IsStdFunctionSpecialization(param.TypeName))
                {
                    everyCandidateUsesFunctionSlot = false;
                    break;
                }
            }
            if (hasApplicableCtor && everyCandidateUsesFunctionSlot)
                RejectLiteralZeroFunctionSlot(source, args[i]);
        }
}

bool LLVMBackend::EmitCxxStructorCall(const std::string& typeName,
                                      const CxxClassInfo::Structor& selected,
                                      llvm::Value* slot,
                                      const std::vector<llvm::Value*>& extraArgs,
                                      const std::vector<NamedVariable>* extraArgVars,
                                      bool assignmentOperator,
                                      llvm::Value* assignmentResultLiveFlag)
{
        if (slot == nullptr) return false;
        const CxxClassInfo::Structor st = selected;
        std::vector<NamedVariable> loweredArgVars;
        std::vector<llvm::Value*> loweredExtraArgs;
        const std::vector<NamedVariable>* callArgVars = extraArgVars;
        const std::vector<llvm::Value*>* callArgs = &extraArgs;
        if (extraArgVars != nullptr)
        {
            loweredArgVars = *extraArgVars;
            loweredExtraArgs = extraArgs;
            for (size_t i = 0; i < loweredArgVars.size() && i + 1 < st.params.size(); ++i)
            {
                if (!st.params[i + 1].IsFunctionPointer
                    && !IsStdFunctionTypeName(st.params[i + 1].TypeName)
                    && !IsStdFunctionSpecialization(st.params[i + 1].TypeName))
                    continue;
                if (loweredArgVars[i].Primary != nullptr)
                    RejectLiteralZeroFunctionSlot(loweredArgVars[i].Primary, loweredArgVars[i]);
                if (LowerNonCapturingClosureForCxx(loweredArgVars[i]))
                    loweredExtraArgs[i] = loweredArgVars[i].Primary;
                // A function name passes the overload constructor selection matched, not the one
                // its bare-name lowering picked first.
                NamedVariable& named = loweredArgVars[i];
                if (!named.CallerName.empty() && named.Storage == nullptr
                    && IsNamedFunctionValue(named))
                    if (const FunctionSymbol* sym =
                            NamedFunctionOverloadForFuncPtr(named.CallerName, st.params[i + 1]);
                        sym != nullptr && sym->Function != nullptr)
                    {
                        if (RejectFuncPtrBind(named.CallerName, *sym, &st.params[i + 1], nullptr,
                                              /*aliasFirst*/ false,
                                              /*stopOnAliasResult*/ false))
                            return false;
                        RejectThrowingCxxFunction(*sym, named.CallerName);
                        named.Primary = sym->Function;
                        loweredExtraArgs[i] = sym->Function;
                    }
            }
            callArgVars = &loweredArgVars;
            callArgs = &loweredExtraArgs;
        }
        ValidateCxxDemandAtUseSite(st.linkageName);
        // Constructing the object installs its vtable, whose deleting destructor needs a usable
        // operator delete. Destroying or assigning one never does, so only construction checks.
        if (const std::string error = ValidateCxxVTableDeleteDemand(typeName);
            !error.empty() && IsCxxConstructingStructor(typeName, st))
            LogError(error);
        AbiRecipe recipe;
        llvm::Function* fn = GetOrCreateCxxStructor(typeName, st, recipe);
        if (fn == nullptr) return false;
        const bool byValueAssignmentResult = assignmentOperator
            && recipe.retSlot.kind == AbiSlot::SRetReturn
            && st.params.size() == 2 && !st.isDefaultCtor && !st.isCopyCtor && !st.isMoveCtor
            && !st.ret.Pointer && !st.ret.ElemPointer && !st.ret.IsAlias && !st.ret.IsRvalueRef
            && ResolveTypeAlias(st.ret.TypeName) == ResolveTypeAlias(typeName);
        llvm::Value* assignmentResultTemp = nullptr;
        if (byValueAssignmentResult)
            assignmentResultTemp = AllocaAtEntry(recipe.retSlot.structTy, nullptr,
                "cxx.assign.result", recipe.retSlot.align);
        std::vector<llvm::Value*> args;
        args.reserve(callArgs->size() + 1);
        args.push_back(slot);                       // 'this'
        for (size_t i = 0; i < callArgs->size(); ++i)
        {
            llvm::Value* a = (*callArgs)[i];
            const TypeAndValue* param = i + 1 < st.params.size() ? &st.params[i + 1] : nullptr;
            if (param != nullptr && a != nullptr && a->getType()->isIntegerTy())
            {
                llvm::Type* paramType = GetType(*param);
                if (paramType != nullptr && paramType->isIntegerTy())
                {
                    const bool sourceUnsigned = callArgVars != nullptr
                        && i < callArgVars->size()
                        && (*callArgVars)[i].TypeAndValue.IsUnsignedInteger() != -1;
                    a = CreateCast(a, paramType, !sourceUnsigned, param->IsUnsignedInteger() != -1);
                }
            }
            // A C++ bool by value takes a pointer value (a CFlat function pointer) as its truth,
            // as the call-argument path does; the raw pointer failed module verification.
            if (param != nullptr && a != nullptr && a->getType()->isPointerTy()
                && !param->Pointer && !param->IsAlias && !param->IsRvalueRef
                && param->TypeName == "bool")
                if (llvm::Type* paramType = GetType(*param); paramType != nullptr && paramType->isIntegerTy(1))
                    a = builder->CreateIsNotNull(a, "ptr.to.bool");
            // A function value bound to a `const bool&` binds a temporary holding its truth (clang's
            // boolean conversion); the raw function address would read the code bytes as the bool.
            if (param != nullptr && a != nullptr && a->getType()->isPointerTy()
                && param->Pointer && param->IsAlias && param->CxxConstRef() && !param->IsRvalueRef
                && !param->ElemPointer && param->TypeName == "bool" && callArgVars != nullptr
                && i < callArgVars->size()
                && ((*callArgVars)[i].TypeAndValue.IsFunctionPointer
                    || llvm::isa_and_nonnull<llvm::Function>((*callArgVars)[i].Primary)))
            {
                llvm::AllocaInst* truthSlot = CreateAlloca(llvm::Type::getInt1Ty(*context));
                builder->CreateStore(builder->CreateIsNotNull(a, "ptr.to.bool"), truthSlot);
                a = truthSlot;
            }
            // Pointer -> integer needs an explicit cast everywhere (ruling 2026-09-26). A function
            // or pointer value into a by-value C++ integer parameter is refused; the null stand-in
            // keeps the module verifiable so the refusal is the only diagnostic.
            if (param != nullptr && a != nullptr && a->getType()->isPointerTy()
                && !param->Pointer && !param->IsAlias && !param->IsRvalueRef
                && param->TypeName != "bool")
                if (llvm::Type* paramType = GetType(*param);
                    paramType != nullptr && paramType->isIntegerTy())
                {
                    LogError(std::format("no matching conversion to parameter {} ('{}') of C++ "
                        "constructor '{}': a function or pointer value cannot convert to an integer "
                        "(pointer -> integer needs an explicit cast)",
                        i + 1, param->TypeName, DisplayCxxClassName(typeName)));
                    a = llvm::Constant::getNullValue(paramType);
                }
            if (callArgVars != nullptr && i < callArgVars->size() && param != nullptr)
            {
                const NamedVariable& source = (*callArgVars)[i];
                if (param->IsCxxRefToPointer && !param->CxxConstRef() && !param->IsRvalueRef
                    && source.IsCxxConstPointerSlot())
                    LogError("cannot pass C++ const pointer argument");
                if (param->IsCxxRefToPointer && source.TypeAndValue.Pointer
                    && source.Storage != nullptr && source.BaseType == GetType(*param)
                    && !source.IsRvalue)
                    a = source.Storage;
                if (param->IsFunctionPointer && param->Pointer && !param->ElemPointer
                    && (param->IsAlias || param->IsRvalueRef || param->CxxConstRef()))
                {
                    if (source.TypeAndValue.IsFunctionPointer && source.Storage != nullptr)
                        a = source.Storage;
                    else
                    {
                        llvm::Type* pointerValueType = BuildThinFnPtrType(*param);
                        auto* temp = AllocaAtEntry(pointerValueType, nullptr, "ctor.fnref");
                        builder->CreateStore(a, temp);
                        a = temp;
                    }
                }
                uint64_t offset = 0;
                bool inaccessible = false;
                if (source.TypeAndValue.Pointer && param->Pointer && !source.TypeAndValue.ElemPointer
                    && (IsCxxDerivedToBasePointer(source.TypeAndValue, *param)
                        || IsCxxDerivedToClangBasePointer(source.TypeAndValue, *param)))
                {
                    a = LowerCxxPointerToBase(source.TypeAndValue.TypeName, param->TypeName, false,
                                              a, SpellType(*this, source.TypeAndValue),
                                              SpellType(*this, *param), "the argument");
                }
                else if (source.TypeAndValue.Pointer && param->Pointer
                    && IsCxxDerivedToBasePointer(source.TypeAndValue, *param)
                    && FindCxxBaseOffset(source.TypeAndValue.TypeName, param->TypeName,
                                         offset, inaccessible))
                {
                    a = EmitCxxBaseAdjust(a, offset);
                }
                // A class pointer parameter bound as `void*` still takes the base subobject.
                else if (std::string spelledBase; source.TypeAndValue.Pointer && param->Pointer
                         && param->TypeName == "void")
                {
                    bool bySpelling = false;
                    if (CxxSpelledClassPointerRelation(
                            CxxStructorReferentSpelling(typeName, st.linkageName, i + 1,
                                                        st.params.size()),
                            *param, source.TypeAndValue, spelledBase, bySpelling)
                        == CxxSpelledParamRelation::Base)
                    {
                        TypeAndValue spelledParam{ .TypeName = spelledBase };
                        spelledParam.Pointer = true;
                        a = LowerCxxPointerToBase(
                            source.TypeAndValue.TypeName, spelledBase, bySpelling, a,
                            SpellType(*this, source.TypeAndValue),
                            bySpelling ? spelledBase + "*" : SpellType(*this, spelledParam),
                            "the argument");
                    }
                }
                else if (!source.TypeAndValue.Pointer && param->IsAlias && !param->ElemPointer
                         && IsCxxDerivedToBaseValue(source.TypeAndValue, *param, nullptr))
                {
                    llvm::Value* address = source.Storage;
                    if (address == nullptr && a != nullptr && a->getType()->isStructTy()
                        && a->getType()->isSized())
                    {
                        address = AllocaAtEntry(a->getType(), nullptr, "ctor.refarg", 0);
                        builder->CreateStore(a, address);
                    }
                    if (address != nullptr
                        && LowerCxxReferenceToBase(source.TypeAndValue.TypeName, param->TypeName,
                                                   source, address))
                        a = address;
                }
                // An opaque `void&` whose harvested referent is a base of the argument class
                // binds the base subobject, not the object start.
                else if (!source.TypeAndValue.Pointer && param->Pointer && !param->ElemPointer
                         && (param->IsAlias || param->IsRvalueRef || param->CxxConstRef())
                         && param->TypeName == "void"
                         && IsForeignCxxClassWithConstructors(source.TypeAndValue.TypeName)
                         && (source.Storage != nullptr
                             || (a != nullptr && a->getType()->isStructTy())))
                {
                    const std::string referent = CxxStructorReferentSpelling(
                        typeName, st.linkageName, i + 1, st.params.size());
                    llvm::Value* address = source.Storage;
                    if (address == nullptr)
                    {
                        address = AllocaAtEntry(a->getType(), nullptr, "ctor.refarg", 0);
                        builder->CreateStore(a, address);
                    }
                    a = address;
                    cxxVirtualBaseRefusal_.clear();
                    const CxxBaseRoutes routes = referent.empty() ? CxxBaseRoutes{}
                        : CollectCxxBaseRoutes(source.TypeAndValue.TypeName,
                                               SqueezeCxxSpelling(referent), true);
                    // A virtual or ambiguous referent of a non-complete object: clang converts.
                    if (routes.NeedsClang()
                        && (routes.nonVirtual > 0 || !IsCxxCompleteObjectValue(source)))
                        a = LowerCxxPointerToBase(source.TypeAndValue.TypeName, referent, true,
                                                  address, SpellType(*this, source.TypeAndValue),
                                                  referent, "the reference");
                    else if (!referent.empty()
                        && FindCxxReferentSpellingOffset(source.TypeAndValue.TypeName, referent,
                                                         offset, &source))
                        a = EmitCxxBaseAdjust(address, offset);
                    else if (!cxxVirtualBaseRefusal_.empty())
                        LogError(cxxVirtualBaseRefusal_);
                    cxxVirtualBaseRefusal_.clear();
                }
                // A C++ reference parameter takes the ARGUMENT's address, the same binding the
                // free and member call paths use; a copy loses every write the callee makes.
                else if (!source.TypeAndValue.Pointer && param->Pointer && !param->ElemPointer
                         && (param->IsAlias || param->IsRvalueRef || param->CxxConstRef())
                         && source.Storage != nullptr
                         && (param->TypeName == source.TypeAndValue.TypeName
                             || (param->TypeName == "void"
                                 && IsForeignCxxClassWithConstructors(
                                     source.TypeAndValue.TypeName))))
                {
                    a = source.Storage;
                }
            }
            // `T&&` where T is itself a pointer receives the pointer VALUE through a temporary
            // slot; passing `&x` directly makes the callee read x's bytes as an address.
            if (param != nullptr && param->IsRvalueRef && param->ElemPointer
                && a != nullptr && a->getType()->isPointerTy())
            {
                TypeAndValue referent = *param;
                referent.ElemPointer = false;
                referent.Pointer = true;
                referent.PointerDepth = 1;
                referent.IsAlias = false;
                referent.IsRvalueRef = false;
                if (llvm::Type* refTy = GetType(referent); refTy != nullptr
                    && refTy->isPointerTy())
                {
                    auto* temp = AllocaAtEntry(refTy, nullptr, "ctor.rrefarg");
                    builder->CreateStore(a, temp);
                    a = temp;
                }
            }
            // A reference parameter with no addressable argument left keeps its materialized
            // temporary; passing the raw value would hand the callee an integer as an address.
            if (param != nullptr && param->Pointer && a != nullptr
                && !a->getType()->isPointerTy()
                && (a->getType()->isStructTy()
                    || ((param->IsAlias || param->IsRvalueRef || param->CxxConstRef())
                        && !param->ElemPointer
                        && llvm::FunctionType::isValidArgumentType(a->getType()))))
            {
                // The temporary holds the REFERENT, not the argument: a CFlat integer literal
                // arrives as i8, and a 1-byte buffer read back as `int` is garbage.
                llvm::Type* refTy = a->getType();
                if ((param->IsAlias || param->IsRvalueRef || param->CxxConstRef())
                    && !param->ElemPointer)
                {
                    TypeAndValue referent = *param;
                    referent.Pointer = false;
                    referent.IsAlias = false;
                    referent.IsRvalueRef = false;
                    llvm::Type* rt = GetType(referent);
                    if (rt != nullptr && rt != refTy
                        && (rt->isIntegerTy() || rt->isFloatingPointTy())
                        && (refTy->isIntegerTy() || refTy->isFloatingPointTy()))
                    {
                        a = CreateCast(a, rt, param->IsUnsignedInteger() == -1, referent.IsUnsignedInteger() != -1);
                        refTy = a->getType();
                    }
                }
                auto* temp = AllocaAtEntry(refTy, nullptr, "ctor.refarg");
                builder->CreateStore(a, temp);
                a = temp;
            }
            args.push_back(a);
        }
        // Omitted trailing parameters take their constant C++ defaults; class defaults are built
        // in their slots by the generated initializer, after the explicit arguments.
        const bool directDefaults = args.size() < st.params.size()
            && CxxCtorDefaultsDirect(typeName, st, args.size());
        // Built at their by-value lowering below, so explicit copies run first (as clang does).
        std::vector<llvm::AllocaInst*> directDefaultSlots;
        auto constructDirectDefault = [&](size_t i) {
            llvm::AllocaInst* defaultSlot = directDefaultSlots[i];
            directDefaultSlots[i] = nullptr;
            auto* initTy = llvm::FunctionType::get(builder->getVoidTy(),
                { builder->getInt32Ty(), builder->getPtrTy() }, false);
            CreateCallOrInvoke(initTy, module->getOrInsertFunction(
                    CxxDefaultInitHelperName(st.linkageName), initTy).getCallee(),
                { builder->getInt32(static_cast<int>(i)), defaultSlot }, /*mayUnwind*/ true);
            if (!IsCxxParamDestroyedInCallee(st.params[i].TypeName))
                RegisterOwnedCxxParamTempAt(defaultSlot, st.params[i].TypeName,
                                            pendingOwnedStructTemps.size());
            return defaultSlot;
        };
        for (size_t i = args.size(); i < st.params.size() && i < st.defaultArgs.size(); ++i)
        {
            if (directDefaults && st.defaultArgs[i].kind == "nonconst")
            {
                llvm::Type* classTy = GetType(TypeAndValue{ .TypeName = st.params[i].TypeName });
                if (classTy == nullptr) break;
                auto* defaultSlot = AllocaAtEntry(classTy, nullptr, "cxx.default.arg");
                directDefaultSlots.resize(st.params.size(), nullptr);
                directDefaultSlots[i] = defaultSlot;
                args.push_back(defaultSlot);
                continue;
            }
            llvm::Value* v = MaterializeCxxDefaultArgument(st.defaultArgs[i], st.params[i]);
            if (v == nullptr) break;
            args.push_back(v);
        }
        if (args.size() != st.params.size())
        {
            LogError(std::format("C++ special member of '{}' expects {} argument(s), got {}",
                                 DisplayCxxClassName(typeName),
                                 (uint64_t)st.params.size() - 1,
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
        /*
         * Same rule the ordinary call path applies (M4b): a foreign NONTRIVIAL C++ class crossing
         * by value is arranged Indirect-without-byval, so the caller hands over a pointer to
         * storage it owns and the object must be copy- or move-CONSTRUCTED into it. The generic
         * ByVal lowering would alloca+store the loaded struct instead, which aliases the source's
         * heap buffer: `SGD(lin.parameters(), sopt)` then destroys the returned vector while the
         * optimizer holds the same buffer. Under Itanium the caller destroys the temp after the
         * call, so it joins the end-of-statement owned-temp list.
         */
        std::vector<llvm::Value*> indirectArgAddrs;
        size_t insertedCxxParamTemps = 0;
        if (recipe.hasLowering && callArgVars != nullptr)
            for (size_t i = 1; i < recipe.paramSlots.size() && i < st.params.size(); ++i)
            {
                if (recipe.paramSlots[i].kind != AbiSlot::ByVal) continue;
                const std::string& pn = st.params[i].TypeName;
                if (!IsForeignNontrivialCxxClass(pn)) continue;
                if (i < directDefaultSlots.size() && directDefaultSlots[i] != nullptr)
                {
                    indirectArgAddrs.resize(recipe.paramSlots.size(), nullptr);
                    indirectArgAddrs[i] = constructDirectDefault(i);
                    continue;
                }
                const size_t argIndex = i - 1;
                if (argIndex >= callArgVars->size()
                    || (*callArgVars)[argIndex].Storage == nullptr)
                {
                    LogError(std::format(
                        "cannot pass C++ class '{}' by value to a constructor of '{}': the "
                        "argument must be a variable, a field or another addressable object so "
                        "its copy constructor can run", DisplayCxxClassName(pn),
                        DisplayCxxClassName(typeName)));
                    continue;
                }
                // A prvalue argument is the parameter object itself (C++17 elision), as for calls.
                if (IsOwnedCxxPrvalueArgument((*callArgVars)[argIndex], recipe.paramSlots[i].structTy))
                {
                    if (IsCxxParamDestroyedInCallee(pn))
                        UnregisterOwnedStructTemp((*callArgVars)[argIndex].Storage);
                    indirectArgAddrs.resize(recipe.paramSlots.size(), nullptr);
                    indirectArgAddrs[i] = (*callArgVars)[argIndex].Storage;
                    continue;
                }
                auto* temp = AllocaAtEntry(recipe.paramSlots[i].structTy, nullptr, "cxx.argtemp",
                                           recipe.paramSlots[i].align);
                const bool useMove = (*callArgVars)[argIndex].IsExplicitMove
                    || (*callArgVars)[argIndex].CxxParamLastUse;
                if (!EmitCxxCopyOrMoveConstruct(pn, temp, (*callArgVars)[argIndex].Storage,
                                                useMove,
                                                "into a by-value constructor parameter", {},
                                                CxxConstReceiverKind((*callArgVars)[argIndex]) != 0))
                    continue;
                if (!IsCxxParamDestroyedInCallee(pn))
                {
                    size_t index = pendingOwnedStructTemps.size();
                    if ((*callArgVars)[argIndex].HasOwnedStructTempRange)
                        index = (*callArgVars)[argIndex].OwnedStructTempEnd + insertedCxxParamTemps;
                    RegisterOwnedCxxParamTempAt(temp, pn, index);
                    ++insertedCxxParamTemps;
                }
                if (useMove && (*callArgVars)[argIndex].IsRvalue)
                    DestroyMovedCxxArgSourceTemp((*callArgVars)[argIndex].Storage, temp);
                if ((*callArgVars)[argIndex].CxxParamLastUse
                    && !(*callArgVars)[argIndex].IsElementAccess
                    && (*callArgVars)[argIndex].FieldName.empty())
                {
                    const std::string sourceName = (*callArgVars)[argIndex].CallerName.empty()
                        ? (*callArgVars)[argIndex].TypeAndValue.VariableName
                        : (*callArgVars)[argIndex].CallerName;
                    MarkVariableMoved(sourceName);
                }
                indirectArgAddrs.resize(recipe.paramSlots.size(), nullptr);
                indirectArgAddrs[i] = temp;
            }
        for (size_t i = 0; i < directDefaultSlots.size(); ++i)
            if (directDefaultSlots[i] != nullptr) constructDirectDefault(i);
        // The argument values are loaded: a by-value `move f` nulls its source BEFORE the call, so
        // a write the constructor makes to the source survives (no after-call pass nulls it).
        if (callArgVars != nullptr)
            for (size_t i = 0; i < callArgVars->size() && i + 1 < st.params.size(); ++i)
                NullExplicitFunctionMoveToValue(st.params[i + 1], (*callArgVars)[i]);
        // A constructor or assignment without noexcept can throw into this frame's cleanup.
        const bool mayUnwind = CalleeMayUnwind(sym);
        if (recipe.hasLowering)
            EmitAbiLoweredCall(sym, args, assignmentResultTemp,
                               indirectArgAddrs.empty() ? nullptr : &indirectArgAddrs,
                               nullptr, nullptr, mayUnwind);
        else
            CreateFunctionCall(fn, args, mayUnwind, sym.HasCFlatDefinition());
        if (assignmentResultTemp != nullptr)
        {
            if (assignmentResultLiveFlag != nullptr && currentFunction != nullptr)
                pendingOwnedStructTemps.push_back({ assignmentResultTemp, typeName,
                    &currentFunction->getEntryBlock(), assignmentResultLiveFlag });
            else
                RegisterOwnedStructTemp(assignmentResultTemp, typeName);
            if (assignmentResultLiveFlag != nullptr)
                builder->CreateStore(builder->getInt1(true), assignmentResultLiveFlag);
        }
        if (callArgVars != nullptr && !callArgVars->empty())
        {
            NamedVariable self;
            self.Primary = slot;
            self.Storage = slot;
            self.BaseType = slot->getType();
            if (!st.params.empty()) self.TypeAndValue = st.params.front();
            std::vector<NamedVariable> transferArgs;
            transferArgs.reserve(callArgVars->size() + 1);
            transferArgs.push_back(std::move(self));
            transferArgs.insert(transferArgs.end(), callArgVars->begin(), callArgVars->end());
            ApplyMoveParamTransfer(typeName, st.params, transferArgs,
                                   /*paramsCarryAllocAlign=*/false,
                                   /*calleeIsMethod=*/false,
                                   /*beforeCall=*/false);
        }
        return true;
    }

llvm::Function* LLVMBackend::GetOrCreateCxxClassDestructor(const std::string& typeName)
{
        const std::string stableTypeName = typeName;
        EnsureCxxRecordProjected(stableTypeName, false);
        auto dsIt = dataStructures.find(stableTypeName);
        if (dsIt == dataStructures.end()) return nullptr;
        if (dsIt->second.Destructor != nullptr) return dsIt->second.Destructor;
        const CxxClassInfo* info = GetCxxClassInfo(stableTypeName);
        if (info == nullptr || !info->hasDtor) return nullptr;
        const CxxClassInfo::Structor destructor = info->destructor;
        AbiRecipe recipe;
        llvm::Function* fn = GetOrCreateCxxStructor(stableTypeName, destructor, recipe);
        if (fn == nullptr) return nullptr;
        // The complete-object destructor takes exactly 'this' and (on Itanium) returns it. That
        // is call-compatible with the shape EmitFullDestructorOverStorage emits, so the C++
        // symbol IS the class's CFlat destructor - no wrapper, no second cleanup mechanism.
        if (fn->arg_size() != 1) return nullptr;
        RegisterDestructor(stableTypeName, fn);
        return fn;
    }

const LLVMBackend::CxxClassInfo::Structor* LLVMBackend::FindCxxDefaultCtor(const std::string& typeName) const
{
        const_cast<LLVMBackend*>(this)->EnsureCxxMemberProjected(typeName, "__ctor");
        const CxxClassInfo* info = GetCxxClassInfo(typeName);
        if (info == nullptr) return nullptr;
        std::string ambiguityError;
        if (const_cast<LLVMBackend*>(this)->CxxDefaultCtorAmbiguityError(
                typeName, ambiguityError))
            return nullptr;
        for (const auto& c : info->constructors)
            if (c.access == cflat_cinterop::AccessPublic && c.isDefaultCtor
                && (c.params.size() == 1 || CxxConstantDefaultsFrom(c, 1)))
                return &c;
        return nullptr;
}

bool LLVMBackend::HasNonPublicCxxDefaultCtor(const std::string& typeName) const
{
        auto record = cxxRecordEntries_.find(typeName);
        if (record == cxxRecordEntries_.end())
        {
            const size_t specialization = typeName.find('$');
            if (specialization != std::string::npos)
                record = cxxRecordEntries_.find(typeName.substr(0, specialization));
        }
        if (record == cxxRecordEntries_.end()) return false;
        return std::any_of(record->second.members.begin(), record->second.members.end(),
            [](const auto& member) {
                return member.kind == cflat_cinterop::RawCxxMember::Constructor
                    && member.isDefaultCtor && member.access != cflat_cinterop::AccessPublic;
            });
}

bool LLVMBackend::CxxDefaultCtorAmbiguous(const std::string& typeName) const
{
        const_cast<LLVMBackend*>(this)->EnsureCxxMemberProjected(typeName, "__ctor");
        const CxxClassInfo* info = GetCxxClassInfo(typeName);
        if (info == nullptr) return false;
        size_t callable = 0;
        for (const auto& c : info->constructors)
            if (c.isDefaultCtor && (c.params.size() == 1 || CxxConstantDefaultsFrom(c, 1)))
                ++callable;
        // T88: a private or protected default constructor is still a candidate for clang's
        // overload resolution, so it competes with a public one that takes defaulted arguments.
        if (HasNonPublicCxxDefaultCtor(typeName)) ++callable;
        return callable > 1;
}

bool LLVMBackend::CxxDefaultCtorAmbiguityError(const std::string& typeName,
                                               std::string& error)
{
        error.clear();
        if (!CxxDefaultCtorAmbiguous(typeName)) return false;
        std::string wrapperName;
        std::string clangError;
        RequestCxxVariadicConstructor(typeName, {}, wrapperName, clangError);
        if (clangError.find("ambiguous") == std::string::npos) return false;
        const std::string detail = CxxFirstDiagnosticLine(clangError);
        error = std::format("C++ class '{}' has ambiguous default construction{}",
                            DisplayCxxClassName(typeName),
                            detail.empty() ? "" : std::format(": {}", detail));
        return true;
}

bool LLVMBackend::CxxEmptyBraceUsesListCtor(const std::string& typeName)
{
        std::string ambiguity;
        std::string bindError;
        TryBindCxxImplicitDefaultCtor(typeName, bindError);
        if (FindCxxDefaultCtor(typeName) != nullptr) return false;
        if (CxxDefaultCtorAmbiguityError(typeName, ambiguity)) return false;
        auto record = cxxRecordEntries_.find(typeName);
        if (record == cxxRecordEntries_.end()) return false;
        for (const auto& member : record->second.members)
            if (member.kind == cflat_cinterop::RawCxxMember::Constructor
                && member.paramTypes.size() >= 2
                && CxxBraceContainerKindOf(member.paramTypes[1]) == CxxBraceContainerKind::InitializerList)
                return true;
        return false;
}

/*
 * Default-CONSTRUCT every element of a C++-class array (`T[N] a = default;`, `new T[n]`).
 * The whole-array zeroinitializer these paths used to store skipped the constructor entirely
 * while the scope-exit / delete path already destroyed each element, so a class whose default
 * constructor establishes an invariant was destroyed having never been constructed, and a
 * polymorphic element was left with a null vptr. `count` may be a runtime value.
 * Returns false with `error` set when the class has no default constructor cflat can call.
 */
bool LLVMBackend::EmitCxxArrayDefaultConstruction(const std::string& typeName, llvm::Value* base,
                                                  llvm::Type* elemTy, llvm::Value* count,
                                                  std::string& error)
{
        if (base == nullptr || elemTy == nullptr || count == nullptr) return true;
        std::string bindError;
        TryBindCxxImplicitDefaultCtor(typeName, bindError);
        const CxxClassInfo::Structor* ctor = FindCxxDefaultCtor(typeName);
        if (ctor == nullptr)
        {
            if (CxxDefaultCtorAmbiguityError(typeName, error)) return false;
            const CxxClassInfo* info = GetCxxClassInfo(typeName);
            error = std::format(
                "C++ class '{}' has no default constructor cflat can call{}, so an array of it "
                "cannot be default-initialized - declare the array as '{}*[N]' and allocate each "
                "element with 'new {}(args)'", DisplayCxxClassName(typeName),
                info != nullptr && info->hasDeletedDefaultCtor ? " (it is deleted)" : "",
                DisplayCxxClassName(typeName), DisplayCxxClassName(typeName));
            return false;
        }
        auto callCtor = [&](llvm::Value* elemPtr) {
            EmitCxxStructorCall(typeName, *ctor, elemPtr, {});
        };

        // A constant extent reuses the walk the per-element DESTRUCTOR uses, so the two
        // traversals cannot drift; a runtime `new T[n]` needs its own counted loop. Either way a
        // throwing constructor destroys only the elements before it.
        if (auto* constCount = llvm::dyn_cast<llvm::ConstantInt>(count))
        {
            EmitArrayConstructionWalk(base, elemTy, constCount->getZExtValue(), typeName, callCtor);
            return true;
        }

        auto* i64Ty = builder->getInt64Ty();
        llvm::Value* total = builder->CreateZExtOrTrunc(count, i64Ty, "cxxarr.n");
        auto* fn = builder->GetInsertBlock()->getParent();
        auto* preBB = builder->GetInsertBlock();
        auto* loopBB = llvm::BasicBlock::Create(*context, "cxxarrctor.loop", fn);
        auto* doneBB = llvm::BasicBlock::Create(*context, "cxxarrctor.done", fn);
        builder->CreateCondBr(builder->CreateICmpUGT(total, builder->getInt64(0)), loopBB, doneBB);

        builder->SetInsertPoint(loopBB);
        auto* idx = builder->CreatePHI(i64Ty, 2, "cxxarrctor.i");
        idx->addIncoming(builder->getInt64(0), preBB);
        {
            UnwindPartialScope builtElements(*this);
            NoteUnwindArrayPrefix(base, elemTy, idx, typeName);
            callCtor(builder->CreateInBoundsGEP(elemTy, base, { idx }, "cxxarrelem"));
        }
        auto* next = builder->CreateAdd(idx, builder->getInt64(1), "cxxarrctor.next");
        idx->addIncoming(next, builder->GetInsertBlock());
        builder->CreateCondBr(builder->CreateICmpULT(next, total), loopBB, doneBB);
        builder->SetInsertPoint(doneBB);
        return true;
    }

const LLVMBackend::CxxClassInfo::Structor* LLVMBackend::FindCxxCopyCtor(
        const std::string& typeName, bool constSource, bool allowExplicit) const
{
        const_cast<LLVMBackend*>(this)->EnsureCxxMemberProjected(typeName, "__ctor");
        const CxxClassInfo* info = GetCxxClassInfo(typeName);
        if (info == nullptr) return nullptr;
        const CxxClassInfo::Structor* mutableCopy = nullptr;
        const CxxClassInfo::Structor* constCopy = nullptr;
        for (const auto& c : info->constructors)
            if (c.isCopyCtor)
            {
                if (c.isExplicit && !allowExplicit) continue;
                const bool takesConst = c.params.size() > 1 && c.params[1].CxxConstRef();
                if (takesConst && constCopy == nullptr) constCopy = &c;
                else if (!takesConst && mutableCopy == nullptr) mutableCopy = &c;
            }
        return constSource ? constCopy : mutableCopy != nullptr ? mutableCopy : constCopy;
    }

const LLVMBackend::CxxClassInfo::Structor* LLVMBackend::FindCxxMoveCtor(const std::string& typeName) const
{
        const_cast<LLVMBackend*>(this)->EnsureCxxMemberProjected(typeName, "__ctor");
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
/*
 * The listed pick, with every pick whose body fails to instantiate refused and the selection
 * re-run without it, as a request-time refusal did before bodies moved to demand.
 */
std::string LLVMBackend::CxxCtorNoMatchReason(const std::vector<TypeAndValue>& argTypes) const
{
        // Name the argument types: with several same-arity overloads, "no match" alone does not say
        // which argument is the problem.
        std::string args;
        for (const auto& a : argTypes)
        {
            if (!args.empty()) args += ", ";
            args += "'" + SpellType(*this, a) + "'";
        }
        return std::format("has no constructor whose parameter types match these arguments ({})", args);
}

const LLVMBackend::CxxClassInfo::Structor* LLVMBackend::SelectCxxConstructor(
        const std::string& typeName, const std::vector<TypeAndValue>& argTypes,
        std::string& why, bool allowNumericConversions,
        const std::vector<NamedVariable>* argVars, bool allowExplicit,
        bool allowScalarNarrowing) const
{
        auto* self = const_cast<LLVMBackend*>(this);
        bool checkedDefaultedSinks = false;
        for (int attempt = 0;; ++attempt)
        {
            const CxxClassInfo::Structor* found = SelectCxxConstructorListed(
                typeName, argTypes, why, allowNumericConversions, argVars, allowExplicit,
                allowScalarNarrowing);
            if (found == nullptr && !checkedDefaultedSinks && argVars != nullptr)
            {
                // A const T& constructor needing a non-constant default (MSVC's allocator) is never
                // picked, so its body is never demanded: check it so the copy-sink refusal can name it.
                checkedDefaultedSinks = true;
                bool refused = false;
                if (auto record = cxxRecordEntries_.find(typeName); record != cxxRecordEntries_.end())
                    for (const auto& member : record->second.members)
                    {
                        if (member.kind != cflat_cinterop::RawCxxMember::Constructor
                            || member.name != "__ctor" || member.linkageName.empty()
                            || !member.bindRefusal.empty()
                            || member.paramTypes.size() <= argVars->size() + 1)
                            continue;
                        bool sink = false;
                        for (size_t i = 0; !sink && i < argVars->size(); ++i)
                        {
                            std::string argSpelling;
                            sink = IsCopyDeletedCxxLvalue((*argVars)[i])
                                && CxxSpellingForCflatType((*argVars)[i].TypeAndValue.TypeName, argSpelling)
                                && member.paramTypes[i + 1] == "const " + argSpelling + " &";
                        }
                        if (!sink) continue;
                        const std::string symbol = member.linkageName;
                        if (std::string error = self->CheckCxxDemand(symbol); !error.empty())
                        {
                            self->RefuseCxxDemandMember(symbol, error);
                            refused = true;
                            break;
                        }
                    }
                if (refused) continue;
            }
            if (found == nullptr || found->isDeleted || found->linkageName.empty() || attempt >= 8)
                return found;
            const std::string symbol = found->linkageName;
            const std::string error = self->CheckCxxDemand(symbol);
            if (error.empty()) return found;
            self->RefuseCxxDemandMember(symbol, error);
            return found;
        }
}

bool LLVMBackend::CxxConstructorThunkParameterIsReference(
        const std::string& typeName, const std::vector<NamedVariable>& arguments,
        size_t index, bool rvalueReference, bool& isReference, bool forceClangPick)
{
        isReference = false;
        const CxxClassInfo* classInfo = GetCxxClassInfoWithConstructors(typeName);
        auto record = cxxRecordEntries_.find(typeName);
        if (record == cxxRecordEntries_.end())
        {
            const size_t specialization = typeName.find('$');
            if (specialization != std::string::npos)
                record = cxxRecordEntries_.find(typeName.substr(0, specialization));
        }
        // A listed overload set that cannot pick (a user-defined conversion on another argument)
        // is resolved by clang through the thunk's mirror.
        bool needsClangPick = forceClangPick || (classInfo != nullptr && classInfo->hasCtorTemplate);
        if (record != cxxRecordEntries_.end())
        {
            needsClangPick = needsClangPick || !record->second.ctorTemplates.empty();
            for (const auto& member : record->second.members)
                if (member.kind == cflat_cinterop::RawCxxMember::Constructor
                    && (member.variadic || member.requiresConstructorWrapper))
                    needsClangPick = true;
        }
        if (!needsClangPick) return false;

        // The generated thunk is lazy and cached; its extracted parameter spelling includes clang's pick.
        std::string wrapperName;
        std::string error;
        if (!RequestCxxVariadicConstructor(typeName, arguments, wrapperName, error))
            return false;
        auto signatures = cxxFunctionSignatures_.find(wrapperName);
        if (signatures == cxxFunctionSignatures_.end()) return false;
        for (auto signature = signatures->second.rbegin();
             signature != signatures->second.rend(); ++signature)
        {
            if (index + 1 >= signature->paramSpellings.size()) continue;
            const std::string& parameter = signature->paramSpellings[index + 1];
            isReference = rvalueReference ? parameter.ends_with("&&")
                                          : parameter.find('&') != std::string::npos;
            return true;
        }
        return false;
}

// [over.ics.rank] 3.2.1 integral / floating promotion of a scalar: bool and every integer narrower
// than int, to int; float to double. Other scalar conversions are plain conversions (rank 2).
static bool IsCxxPromotionTo(const std::string& from, const std::string& to)
{
        if (from == "float" && to == "double") return true;
        if (to != "int") return false;
        if (from == "bool") return true;
        LLVMBackend::TypeAndValue probe;
        probe.TypeName = from;
        return probe.IsInteger() > 0 && probe.IsInteger() < 32;
}

const LLVMBackend::CxxClassInfo::Structor* LLVMBackend::SelectCxxConstructorListed(
        const std::string& typeName, const std::vector<TypeAndValue>& argTypes,
        std::string& why, bool allowNumericConversions,
        const std::vector<NamedVariable>* argVars, bool allowExplicit,
        bool allowScalarNarrowing) const
{
        const_cast<LLVMBackend*>(this)->EnsureCxxMemberProjected(typeName, "__ctor");
        const CxxClassInfo* info = GetCxxClassInfo(typeName);
        if (info == nullptr) { why = "has no imported constructors"; return nullptr; }
        if (!info->layoutRefusal.empty()) { why = info->layoutRefusal; return nullptr; }
        auto scalarFamily = [](const TypeAndValue& tv) -> int {
            if (tv.Pointer) return 3;
            if (tv.TypeName == "float" || tv.TypeName == "double"
                || tv.TypeName == "longdouble") return 2;
            if (tv.TypeName == "bool") return 1;
            return 1;   // every remaining primitive is integral for selection purposes
        };
        auto scalarEquivalent = [](const TypeAndValue& a, const TypeAndValue& b) {
            if (a.TypeName == b.TypeName) return true;
            const int aBits = a.IsInteger();
            if (aBits != -1 && aBits == b.IsInteger())
                return a.IsUnsignedInteger() == b.IsUnsignedInteger();
            const int aFloat = a.IsFloatingPoint();
            return aFloat != -1 && aFloat == b.IsFloatingPoint();
        };
        auto argumentIsRvalue = [&](size_t index) {
            // Rvalue-ness must be PROVEN. Without argument provenance nothing is proven, and an
            // argument that has addressable storage or came back as a reference is an lvalue.
            if (argVars == nullptr || index >= argVars->size())
                return false;
            const NamedVariable& arg = (*argVars)[index];
            // The shared C++ predicate proves rvalue-ness the same way for every callee kind.
            return IsCxxRvalueReferenceArgument(arg);
        };
        auto isForeignClass = [&](const std::string& name) {
            return IsCxxRecord(name) || IsCxxLazyAliasSpecialization(name);
        };
        auto sameForeignClass = [&](const std::string& a, const std::string& b) {
            return isForeignClass(a) && isForeignClass(b)
                && ResolveTypeAlias(a) == ResolveTypeAlias(b);
        };
        auto scalarRvalueReference = [&](const TypeAndValue& want,
                                         const TypeAndValue& got, bool rvalue) {
            TypeAndValue referent = want;
            referent.Pointer = false;
            referent.ElemPointer = false;
            return want.Pointer && want.IsRvalueRef && !want.ElemPointer && !got.Pointer
                && rvalue && want.TypeName != "void" && !isForeignClass(want.TypeName)
                && scalarFamily(referent) == scalarFamily(got);
        };
        // The argument behind `got` when the caller has it (virtual-base conversions need it).
        auto compatible = [&](const TypeAndValue& want, const TypeAndValue& got) {
            // Opaque LLVM pointers compare equal regardless of indirection. Apply the same
            // proven depth rule used by call overload scoring before any pointer fast path.
            if (got.PointerDepthRefuses(want)) return false;
            // A function pointer converts to bool, but never to an integer constructor parameter.
            if ((got.IsFunctionPointer || got.TypeName == "__c_fn_ptr"
                    || !got.FuncPtrReturnTypeName.empty())
                && !want.Pointer && want.TypeName != "bool" && want.IsInteger() != -1)
                return false;
            if (got.IsScopedEnum)
                return IsScopedEnumMatch(got, want);
            // C++ function-pointer references use an indirect parameter slot, while a CFlat
            // function name supplies the pointer value itself. Bind the pointee's exact signature.
            if ((want.IsFunctionPointer || want.TypeName == "__c_fn_ptr")
                && (want.IsAlias || want.IsRvalueRef) && !want.ElemPointer && !got.Pointer)
            {
                std::vector<FuncPtrComponent> wantSignature, gotSignature;
                if (FuncPtrSignatureOf(want, wantSignature)
                    && FuncPtrSignatureOf(got, gotSignature))
                    return !FuncPtrSignaturesProvablyDiffer(want, got);
            }
            if (want.TypeName == got.TypeName && want.Pointer == got.Pointer) return true;
            if (want.Pointer && got.Pointer && (IsCxxDerivedToBasePointer(got, want)
                                                || IsCxxDerivedToClangBasePointer(got, want)))
                return true;                 // a public derived pointer converts to Base*
            if (want.Pointer && got.Pointer && want.IsRvalueRef && want.ElemPointer
                && !got.ElemPointer && dataStructures.count(want.TypeName) == 0
                && dataStructures.count(got.TypeName) == 0 && scalarEquivalent(want, got))
                return true;                 // a pointer rvalue-reference binds a pointer value
            if (want.Pointer && got.Pointer == false && want.IsAlias && !want.ElemPointer
                && IsCxxDerivedToBaseValue(got, want, nullptr))
                return true;                 // a public derived value binds to Base&
            // A public derived VALUE slices to a by-value base parameter: viable, then ranked. One
            // public non-virtual path only (a diamond is ambiguous), and a base copy ctor not deleted.
            if (!want.Pointer && !got.Pointer && IsCxxRecord(want.TypeName)
                && IsCxxDerivedToBaseValue(got, want, nullptr))
            {
                const auto routes = CollectCxxBaseRoutes(got.TypeName, want.TypeName, false);
                if (!routes.unknown && !routes.viaVirtual && routes.nonVirtual == 1
                    && routes.offsetPublic)
                    return true;
            }
            if (want.Pointer && !got.Pointer && (want.IsAlias || want.IsRvalueRef)
                && !want.ElemPointer && want.TypeName == "void"
                && IsForeignCxxClassWithConstructors(got.TypeName))
                return true;                 // a late-requested C++ class reference was opaque
            if (want.Pointer && !got.Pointer && (want.IsAlias || want.IsRvalueRef)
                && !want.ElemPointer && isForeignClass(want.TypeName)
                && isForeignClass(got.TypeName))
            {
                if (sameForeignClass(want.TypeName, got.TypeName))
                    return true;             // typedef and canonical identities share a referent
                std::string wantSpelling;
                std::string gotSpelling;
                if (CxxSpellingForCflatType(want.TypeName, wantSpelling)
                    && CxxSpellingForCflatType(got.TypeName, gotSpelling)
                    && SqueezeCxxSpelling(wantSpelling) == SqueezeCxxSpelling(gotSpelling))
                    return true;             // a typedef alias and its canonical class spelling
            }
            if (want.Pointer && !got.Pointer && (want.IsAlias || want.IsRvalueRef)
                && !want.ElemPointer && want.TypeName == got.TypeName
                && isForeignClass(want.TypeName))
                return true;                  // a class reference accepts a value of its referent
            // A reference to a PRIMITIVE is an indirect pointer at this stage; an lvalue of
            // exactly that type binds it (`HasRef(int&)` called with an `int` variable). An
            // rvalue reference is excluded: `optional<int>(in_place_t, int&&)` must not take it.
            if (want.Pointer && !got.Pointer && want.IsAlias && !want.IsRvalueRef
                && !want.ElemPointer && !got.ElemPointer && want.TypeName == got.TypeName
                && !isForeignClass(want.TypeName))
                return true;
            // A scalar C++ reference is represented as an indirect pointer at this stage, but
            // an ordinary pointer parameter is not a numeric conversion target. Without the
            // reference check, an integer index could select std::string(const char*) over size_t.
            if (allowNumericConversions && (want.CxxConstRef() || want.IsRvalueRef)
                && want.Pointer != got.Pointer
                && !want.ElemPointer && !got.ElemPointer
                && ((want.IsFloatingPoint() != -1 || want.IsInteger() != -1)
                    && (got.IsFloatingPoint() != -1 || got.IsInteger() != -1)))
                return true;                  // scalar references use an indirect pointer shape
            // A CFlat function value is a non-null pointer: a bool target (by value, or `const bool&`
            // binding a converted temporary) takes its truth, as clang's boolean conversion does.
            if (want.TypeName == "bool" && !want.ElemPointer
                && (!want.IsAlias || want.CxxConstRef()) && !want.IsRvalueRef
                && (got.IsFunctionPointer || got.TypeName == "__c_fn_ptr") && !got.ElemPointer)
                return true;
            if (want.Pointer != got.Pointer) return false;
            if (want.Pointer)
            {
                // C++ permits an object pointer to convert to void*. The reverse conversion
                // still needs an explicit cast, and function pointers are not object pointers.
                if (want.TypeName == "void" && !want.ElemPointer && !got.IsFunctionPointer
                    && (!got.PointeeConstBit() || want.PointeeConstBit()))
                    return true;
                // A pointer to a PRIMITIVE may be spelled with a CFlat synonym (`long`) while the
                // C++ declaration uses another name for the same width (`long long`, i.e. int64_t
                // in c10::IntArrayRef). Same width and signedness means the same pointer ABI.
                // Record pointers stay exact-only so unrelated classes cannot bind.
                if (want.ElemPointer != got.ElemPointer || want.TypeName == "void"
                    || got.TypeName == "void" || dataStructures.count(want.TypeName) != 0
                    || dataStructures.count(got.TypeName) != 0)
                    return false;
                return scalarEquivalent(want, got);
            }
            if (dataStructures.count(want.TypeName) != 0 || dataStructures.count(got.TypeName) != 0)
                return false;                 // record types must match exactly
            // Ordinary C++ scalar conversions are allowed when selecting a converting
            // constructor. Exact same-family matches still win through the exact counter.
            const bool boolTarget = !want.Pointer && want.TypeName == "bool";
            if (allowNumericConversions
                && ((want.IsFloatingPoint() != -1 && got.IsInteger() != -1)
                || (want.IsInteger() != -1 && got.IsFloatingPoint() != -1)
                || (boolTarget && (got.IsInteger() != -1 || got.IsFloatingPoint() != -1))))
                return true;
            // Scalar table (ruling 2026-10-09): a narrowing or other-enum argument is not a
            // match; clang would otherwise take it silently.
            if (!want.Pointer && !got.Pointer && !want.IsAlias && !got.IsAlias)
            {
                NamedVariable probe;
                probe.TypeAndValue = got;
                const ScalarArgConversion conv = ClassifyScalarArgConversion(probe, want);
                // Copy-initialization is not a call argument: clang takes a narrowing standard
                // conversion there (`E e = int` picks E(signed char)), so the caller may lift it.
                if (conv == ScalarArgConversion::EnumMismatch
                    || (conv == ScalarArgConversion::Narrow && !allowScalarNarrowing))
                    return false;
            }
            return scalarFamily(want) == scalarFamily(got);
        };

        bool rejectedIntegerFunctionPointer = false;
        bool rejectedFunctionPointerToInteger = false;
        size_t rejectedFunctionPointerIntegerParameter = 0;
        std::string rejectedFunctionPointerIntegerType;

        /*
         * An opaque `void&` parameter (its class was not requested when the constructor was
         * harvested) accepts any C++ class value. When the harvested C++ spelling names a
         * DIFFERENT class that is not a base of the argument, the binding is proven wrong:
         * `vector<T>(n, const allocator_type&)` must not take a T.
         */
        auto opaqueReferentMismatch = [&](const CxxClassInfo::Structor& c, size_t paramIndex,
                                          const TypeAndValue& want, const TypeAndValue& got,
                                          bool& sameReferent) {
            sameReferent = false;
            if (want.TypeName != "void" || !want.Pointer || got.Pointer
                || !(want.IsAlias || want.IsRvalueRef))
                return false;
            auto record = cxxRecordEntries_.find(typeName);
            if (record == cxxRecordEntries_.end()) return false;
            std::string referent;
            for (const auto& member : record->second.members)
                if (member.linkageName == c.linkageName && paramIndex < member.paramTypes.size()
                    && member.paramTypes.size() == c.params.size())
                {
                    referent = member.paramTypes[paramIndex];
                    break;
                }
            if (referent.starts_with("const ")) referent.erase(0, 6);
            while (!referent.empty() && (referent.back() == '&' || referent.back() == ' '))
                referent.pop_back();
            std::string argSpelling;
            if (referent.empty() || !CxxSpellingForCflatType(got.TypeName, argSpelling))
                return false;
            auto canonicalForeignIdentity = [&](const std::string& spelling) {
                TypeAndValue mapped;
                bool mappedForeign = false;
                if (!TryMapCxxForeignSpelling(spelling, mapped, mappedForeign)
                    || !mappedForeign)
                    return std::string();
                return ResolveTypeAlias(mapped.TypeName);
            };
            const std::string referentIdentity = canonicalForeignIdentity(referent);
            if (!referentIdentity.empty()
                && referentIdentity == canonicalForeignIdentity(argSpelling))
            {
                sameReferent = true;
                return false;
            }
            std::vector<std::string> pending{ got.TypeName };
            std::set<std::string> seen;
            while (!pending.empty())
            {
                const std::string name = pending.back();
                pending.pop_back();
                if (!seen.insert(name).second) continue;
                std::string spelling;
                if (CxxSpellingForCflatType(name, spelling) && spelling == referent)
                {
                    sameReferent = true;
                    return false;
                }
                if (std::string cppBase; IsCppStructName(name) && GetCppStructBase(name, cppBase))
                    pending.push_back(cppBase);
                const CxxClassInfo* classInfo = GetCxxClassInfo(name);
                if (classInfo == nullptr) continue;
                for (const auto& base : classInfo->bases)
                {
                    if (base.canonicalType == referent)
                    {
                        sameReferent = true;
                        return false;
                    }
                    pending.push_back(base.name);
                }
            }
            return true;
        };

        auto isNullptrArgument = [&](size_t index) {
            if (argVars == nullptr || index >= argVars->size()) return false;
            const NamedVariable& arg = (*argVars)[index];
            if (IsCxxNullTypedArgument(arg)) return true;
            return !arg.TypeAndValue.Pointer && arg.TypeAndValue.TypeName.empty()
                && llvm::isa_and_nonnull<llvm::ConstantPointerNull>(arg.Primary);
        };
        // `std::nullptr_t` maps to `void*`; only the harvested spelling tells the two apart.
        // Kind: 0 not nullptr_t, 1 by value (any top-level cv), 2 binds a prvalue exactly, 3 other ref.
        auto spelledNullptrT = [&](const CxxClassInfo::Structor& c, size_t paramIndex) {
            auto record = cxxRecordEntries_.find(typeName);
            if (record == cxxRecordEntries_.end()) return 0;
            for (const auto& member : record->second.members)
                if (member.linkageName == c.linkageName && paramIndex < member.paramTypes.size()
                    && member.paramTypes.size() == c.params.size())
                {
                    std::string spelled = SqueezeCxxSpelling(member.paramTypes[paramIndex]);
                    const bool rvalueRef = spelled.ends_with("&&");
                    const bool reference = spelled.ends_with('&');
                    while (spelled.ends_with('&')) spelled.pop_back();
                    bool isConst = false, isVolatile = false;
                    for (bool again = true; again;)
                    {
                        again = false;
                        if (spelled.starts_with("const")) { spelled.erase(0, 5); isConst = again = true; }
                        if (spelled.ends_with("const")) { spelled.resize(spelled.size() - 5); isConst = again = true; }
                        if (spelled.starts_with("volatile")) { spelled.erase(0, 8); isVolatile = again = true; }
                        if (spelled.ends_with("volatile")) { spelled.resize(spelled.size() - 8); isVolatile = again = true; }
                    }
                    if (spelled.starts_with("::")) spelled.erase(0, 2);
                    if (spelled != "std::nullptr_t" && spelled != "nullptr_t"
                        && spelled != "decltype(nullptr)")
                        return 0;
                    if (!reference) return 1;
                    // A prvalue binds any rvalue reference, or a const non-volatile lvalue reference.
                    return rvalueRef || (isConst && !isVolatile) ? 2 : 3;
                }
            return 0;
        };

        const CxxClassInfo::Structor* found = nullptr;
        size_t candidates = 0;
        auto sameBoundaryType = [](const TypeAndValue& a, const TypeAndValue& b) {
            return a.TypeName == b.TypeName && a.Pointer == b.Pointer
                && a.ElemPointer == b.ElemPointer && a.PointerDepth == b.PointerDepth
                && a.IsFunctionPointer == b.IsFunctionPointer && a.IsAlias == b.IsAlias
                && a.IsMove == b.IsMove && a.IsRvalueRef == b.IsRvalueRef;
        };
        size_t foundExact = 0;
        size_t foundPromotion = 0;
        size_t foundReferencePreference = 0;
        std::string referenceRejection;
        auto referenceParameterName = [](const TypeAndValue& param, size_t index) {
            return param.VariableName.empty()
                ? std::format("parameter {}", index + 1) : param.VariableName;
        };
        auto referencePreference = [](const TypeAndValue& param, bool rvalue) {
            if (!param.Pointer) return size_t(0);
            if (param.IsRvalueRef) return rvalue ? size_t(2) : size_t(0);
            if (!param.IsAlias) return size_t(0);
            return param.CxxConstRef() ? size_t(1) : (rvalue ? size_t(0) : size_t(2));
        };
        struct Viable
        {
            const CxxClassInfo::Structor* ctor;
            size_t exact;
            size_t promotion;
            size_t referencePreference;
            bool blocked;
            bool numeric;   // blocked by an integer <-> floating argument, not by a default
            std::vector<CxxConversionRank> ranks;
            bool ranked;
            bool deletedSlice = false;   // blocked by a by-value base whose copy constructor is deleted
        };
        std::vector<Viable> viable;
        std::string blockedWhy;
        // A tie is only final once every candidate has been seen: a later exact match wins.
        bool ambiguous = false;
        std::vector<const CxxClassInfo::Structor*> tied;   // the equal-rank candidates, for the note
        for (const auto& c : info->constructors)
        {
            // Fewer arguments than parameters is fine when every omitted one has a constant
            // default (`parser(size_t max_capacity = DEFAULT_MAX_CAPACITY)` called as `parser()`).
            if (c.params.size() < argTypes.size() + 1) continue;
            // C++ offers only NON-explicit constructors as the one implicit user-defined
            // conversion; an explicit one stays reachable through the spelled form `T(x)`.
            if (!allowExplicit && c.isExplicit) continue;
            const size_t omitted = c.params.size() - argTypes.size() - 1;
            // A candidate whose omitted defaults are not constants cannot be CALLED here, but C++
            // still ranks it: it is kept as `blocked` so a better one is refused, not skipped.
            const bool blocked = omitted != 0 && !CxxConstantDefaultsFrom(c, argTypes.size() + 1)
                && !const_cast<LLVMBackend*>(this)->CxxCtorDefaultsDirect(typeName, c, argTypes.size() + 1);
            if (blocked)
            {
                // Viable in C++ only when EVERY omitted parameter has some default at all.
                bool everyDefaulted = c.defaultArgs.size() == c.params.size();
                for (size_t i = argTypes.size() + 1; everyDefaulted && i < c.params.size(); ++i)
                    everyDefaulted = !c.defaultArgs[i].kind.empty();
                if (!everyDefaulted) continue;
            }
            if (!blocked) ++candidates;
            bool softRefused = false;
            bool deletedSlice = false;   // a by-value base slice whose base copy constructor is deleted
            bool ok = true;
            size_t exact = 0;
            size_t promotion = 0;
            size_t currentReferencePreference = 0;
            for (size_t i = 0; i < argTypes.size(); ++i)
            {
                const auto& want = c.params[i + 1];
                TypeAndValue got = argTypes[i];
                if (argVars != nullptr && i < argVars->size()
                    && IsCxxFixedArrayArgument((*argVars)[i]))
                    got = CxxFixedArrayDecayType((*argVars)[i]);
                if (argVars != nullptr && i < argVars->size())
                {
                    const NamedVariable& argument = (*argVars)[i];
                    auto* constant = llvm::dyn_cast_or_null<llvm::Constant>(argument.Primary);
                    if (argument.IsStringLiteral
                        || (constant != nullptr && IsStringLiteralConstant(constant)))
                        got.SetPointeeConstBit(true);
                }
                // A bare `nullptr` has no CFlat type: it binds a by-value pointer or function
                // pointer (a null pointer conversion), or exactly a `std::nullptr_t` by value or
                // by const lvalue reference, and nothing else.
                if (isNullptrArgument(i))
                {
                    const int nullptrKind = spelledNullptrT(c, i + 1);
                    if (nullptrKind == 2) { ++exact; continue; }
                    // A named std::nullptr_t is an lvalue: it binds `nullptr_t&` too.
                    if (nullptrKind == 3 && (*argVars)[i].TypeAndValue.IsCxxNullptrT
                        && (*argVars)[i].Storage != nullptr) { ++exact; continue; }
                    if ((!want.Pointer && !want.IsFunctionPointer) || want.IsAlias
                        || want.IsRvalueRef || want.IsCxxRefToPointer)
                    { ok = false; break; }
                    if (nullptrKind == 1) ++exact;
                    continue;
                }
                // std::nullptr_t is represented by void*, but a pointer object is not a null
                // pointer constant. Preserve an actual nullptr_t variable; reject other pointers.
                if (spelledNullptrT(c, i + 1) != 0
                    && argVars != nullptr && i < argVars->size()
                    && ((*argVars)[i].TypeAndValue.Pointer
                        || (*argVars)[i].TypeAndValue.IsFunctionPointer)
                    && !(*argVars)[i].TypeAndValue.IsCxxNullptrT)
                {
                    ok = false;
                    break;
                }
                if (want.IsFunctionPointer && !got.IsFunctionPointer && got.IsInteger() != -1)
                {
                    const bool nullPointerConstant = argVars != nullptr && i < argVars->size()
                        && IsNullPointerConstantArgument((*argVars)[i], true);
                    if (!nullPointerConstant)
                    {
                        if (!blocked && !softRefused) rejectedIntegerFunctionPointer = true;
                        ok = false;
                        break;
                    }
                }
                const bool rvalue = argumentIsRvalue(i);
                const bool functionPointerValue = got.IsFunctionPointer
                    || got.TypeName == "__c_fn_ptr" || !got.FuncPtrReturnTypeName.empty();
                const bool functionPointerToInteger = functionPointerValue && !want.Pointer
                    && want.TypeName != "bool" && want.IsInteger() != -1;
                if (functionPointerToInteger)
                {
                    rejectedFunctionPointerToInteger = true;
                    if (rejectedFunctionPointerIntegerParameter == 0)
                    {
                        rejectedFunctionPointerIntegerParameter = i + 1;
                        rejectedFunctionPointerIntegerType = want.TypeName;
                    }
                }
                const bool constClassArgument = argVars != nullptr && i < argVars->size()
                    && !got.Pointer && CxxConstReceiverKind((*argVars)[i]) != 0;
                const bool mutableClassReference = want.Pointer
                    && (want.IsAlias || want.IsRvalueRef) && !want.CxxConstRef()
                    && !want.ElemPointer && isForeignClass(want.TypeName);
                if (constClassArgument && mutableClassReference)
                {
                    if (!blocked && !softRefused && !c.needsLocalDefinition
                        && referenceRejection.empty())
                        referenceRejection = "const class object cannot bind a non-const class reference";
                    ok = false;
                    break;
                }
                const bool originalFunctionPointer = argVars != nullptr && i < argVars->size()
                    && (*argVars)[i].HasOriginalArgumentType
                    && (*argVars)[i].OriginalArgumentType.IsFunctionPointer;
                const bool constFunctionPointerLvalue = argVars != nullptr
                    && i < argVars->size() && !rvalue
                    && (got.IsFunctionPointer || originalFunctionPointer)
                    && CxxConstReceiverKind((*argVars)[i]) != 0;
                if (constFunctionPointerLvalue && want.IsFunctionPointer && want.IsAlias
                    && !want.CxxConstRef() && !want.IsRvalueRef)
                {
                    ok = false;
                    break;
                }
                bool sameOpaqueReferent = false;
                const bool opaqueReferentMismatchForArgument = opaqueReferentMismatch(
                    c, i + 1, want, got, sameOpaqueReferent);
                const bool samePointerReferenceReferent
                    = (want.IsCxxRefToPointer || want.IsRvalueRef)
                    && want.TypeName == got.TypeName && want.Pointer && got.Pointer
                    && want.ElemPointer == got.ElemPointer;
                const bool sameReferenceReferent = ((want.TypeName == got.TypeName
                    || sameForeignClass(want.TypeName, got.TypeName))
                    && ((want.ElemPointer && want.Pointer && got.Pointer)
                        || (!want.ElemPointer && !got.Pointer)))
                    || samePointerReferenceReferent
                    || sameOpaqueReferent
                    || scalarRvalueReference(want, got, rvalue);
                if (sameReferenceReferent
                    && want.Pointer && want.IsRvalueRef && !rvalue)
                {
                    if (!blocked && !softRefused && !c.needsLocalDefinition && referenceRejection.empty())
                        referenceRejection = std::format(
                            "constructor '{}' parameter '{}' is an rvalue reference; "
                            "pass 'move <argument>' or a temporary value",
                            typeName, referenceParameterName(want, i));
                    ok = false;
                    break;
                }
                // A const record pointer never binds a mutable pointer parameter (clang rule).
                if (CxxRecordPointeeConstRelation(got, want) == CxxPointeeConstRelation::Drops)
                {
                    if (!blocked && !softRefused && !c.needsLocalDefinition && referenceRejection.empty())
                        referenceRejection = std::format(
                            "constructor '{}' parameter '{}' is a mutable C++ pointer and "
                            "argument {} points to a const C++ object, so the parameter could "
                            "write through read-only storage",
                            typeName, referenceParameterName(want, i), i + 1);
                    ok = false;
                    break;
                }
                if (want.IsCxxRefToPointer && !want.CxxConstRef() && !want.IsRvalueRef
                    && got.Pointer && argVars != nullptr && i < argVars->size()
                    && CxxPointerReferenceNeedsConversion((*argVars)[i], want))
                {
                    if (!blocked && !softRefused && !c.needsLocalDefinition
                        && referenceRejection.empty())
                    {
                        const uint32_t sourceMask = got.PointeeConstMask();
                        const uint32_t targetMask = want.PointeeConstMask();
                        const bool addsConst = sourceMask != targetMask
                            && (sourceMask | targetMask) == targetMask;
                        referenceRejection = addsConst
                            ? "cannot bind a pointer whose pointee const would need to be added"
                            : "cannot bind a pointer conversion to a non-const reference to a pointer";
                    }
                    ok = false;
                    break;
                }
                if (sameReferenceReferent && want.Pointer
                    && (want.IsAlias || want.IsCxxRefToPointer)
                    && !want.IsRvalueRef && !want.CxxConstRef()
                    && rvalue)
                {
                    if (!blocked && !softRefused && !c.needsLocalDefinition && referenceRejection.empty())
                        referenceRejection = std::format(
                            "constructor '{}' parameter '{}' is a non-const lvalue "
                            "reference and cannot bind an rvalue; pass an lvalue",
                            typeName, referenceParameterName(want, i));
                    ok = false;
                    break;
                }
                if (want.IsFunctionPointer && want.IsAlias && !want.CxxConstRef()
                    && argVars != nullptr && i < argVars->size()
                    && (*argVars)[i].Storage == nullptr
                    && !(*argVars)[i].CallerName.empty())
                {
                    if (!blocked && !softRefused && !c.needsLocalDefinition
                        && referenceRejection.empty())
                        referenceRejection = std::format(
                            "constructor '{}' parameter '{}' is a non-const lvalue reference "
                            "and cannot bind an rvalue; pass an lvalue",
                            typeName, referenceParameterName(want, i));
                    ok = false;
                    break;
                }
                // Ruling (MIX18): a converted scalar binds a temporary; by-value and T&& tie (clang: by-value
                // vs T&& is ambiguous), both beat const T&. Standard rank (promotion) is compared first.
                const auto scalarTarget = [&](const TypeAndValue& t) {
                    return !t.ElemPointer && !IsCxxRecord(t.TypeName) && !t.IsScopedEnum
                        && (t.IsInteger() != -1 || t.IsFloatingPoint() != -1);
                };
                const bool convertedScalar = allowNumericConversions && scalarTarget(want)
                    && scalarTarget(got) && !got.Pointer && want.TypeName != got.TypeName
                    && (!want.Pointer || (want.IsAlias && !want.IsCxxRefToPointer)
                        || (want.IsRvalueRef && !want.IsCxxRefToPointer));
                if (convertedScalar && IsCxxPromotionTo(got.TypeName, want.TypeName))
                    ++promotion;
                if (convertedScalar && !want.Pointer)
                    currentReferencePreference += 1;
                else if (convertedScalar && want.IsRvalueRef)
                    currentReferencePreference += 1;
                else if (convertedScalar && want.CxxConstRef())
                    currentReferencePreference += 0;
                // An exact scalar `const T&` ties a by-value `T` (both identity): clang finds no best.
                else if (scalarTarget(want) && scalarTarget(got) && want.Pointer && want.CxxConstRef()
                    && want.TypeName == got.TypeName && !got.Pointer)
                    currentReferencePreference += 0;
                else
                    currentReferencePreference += referencePreference(want, rvalue);
                if (want.TypeName == got.TypeName
                    && (want.Pointer == got.Pointer
                        || (allowNumericConversions && want.Pointer && !got.Pointer)))
                    ++exact;
                const bool lambdaLiteralIsCaptureless = argVars != nullptr
                    && i < argVars->size() && (*argVars)[i].Storage == nullptr
                    && (*argVars)[i].LambdaCaptureNames.empty()
                    && (*argVars)[i].LambdaReferenceCaptureNames.empty();
                if (got.TypeName == "__closure_fat_ptr" && want.IsFunctionPointer
                    && want.IsThinFnPtr() && !lambdaLiteralIsCaptureless)
                {
                    if (blocked || softRefused) { ok = false; break; }
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
                const bool scalarRvalueRef = scalarRvalueReference(want, got, rvalue);
                if (argVars != nullptr && i < argVars->size()
                    && NamedFunctionArgMismatches((*argVars)[i], want))
                { ok = false; break; }
                // A function name arrives typed with its own signature, which the scalar `D&&`
                // rule would not look at: it binds a C++ function pointer only when it matches.
                if (want.IsFunctionPointer && argVars != nullptr && i < argVars->size()
                    && !(*argVars)[i].CallerName.empty() && (*argVars)[i].Storage == nullptr
                    && IsNamedFunctionValue((*argVars)[i])
                    && !(*argVars)[i].TypeAndValue.Pointer
                    && !(*argVars)[i].TypeAndValue.FuncPtrReturnTypeName.empty()
                    && FuncPtrSignaturesProvablyDiffer(want, (*argVars)[i].TypeAndValue))
                { ok = false; break; }
                // A named function (possibly an overload set) binds only through the overload whose
                // signature matches; EmitCxxStructorCall forms the callback from that same overload.
                bool namedFunctionMatches = false;
                if (argVars != nullptr && i < argVars->size()
                    && !(*argVars)[i].CallerName.empty()
                    && (*argVars)[i].Storage == nullptr && want.IsFunctionPointer
                    && IsNamedFunctionValue((*argVars)[i]))
                {
                    if (NamedFunctionOverloadForFuncPtr((*argVars)[i].CallerName, want) == nullptr)
                    { ok = false; break; }
                    namedFunctionMatches = true;
                }
                // A null pointer constant binds a scalar `const T&` / `T&&` parameter (clang materializes
                // the referent). Identity-proven, so the numeric gate below does not apply; `T&` never does.
                const bool nullScalarReference = argVars != nullptr && i < argVars->size()
                    && want.Pointer && (want.IsRvalueRef || want.CxxConstRef())
                    && !want.ElemPointer && !want.IsCxxRefToPointer && want.IsInteger() != -1
                    && !got.Pointer && IsNullPointerConstantArgument((*argVars)[i], true);
                const bool compatibleArg = namedFunctionMatches || nullScalarReference
                    || compatible(want, got);
                // The slice ties like any by-value base, but refuses when C++ picks it.
                if (compatibleArg && !want.Pointer && !got.Pointer && want.TypeName != got.TypeName
                    && IsCxxRecord(want.TypeName) && IsCxxDerivedToBaseValue(got, want, nullptr))
                    if (const CxxClassInfo* baseInfo = GetCxxClassInfo(want.TypeName);
                        baseInfo != nullptr && baseInfo->hasDeletedCopyCtor)
                        deletedSlice = true;
                if (!copyRef && !scalarRvalueRef && !compatibleArg)
                {
                    // An integer <-> floating argument is refused as a CALL here, but C++ still
                    // ranks that overload: keep it as `blocked` so ranking can see it.
                    const bool numericMismatch = !allowNumericConversions
                        && !want.Pointer && !got.Pointer && !want.IsAlias && !want.IsRvalueRef
                        && ((want.IsFloatingPoint() != -1
                                && (got.IsInteger() != -1 || enumBackingTypes.count(got.TypeName) != 0))
                            || (want.IsInteger() != -1 && got.IsFloatingPoint() != -1))
                        && dataStructures.count(want.TypeName) == 0
                        && dataStructures.count(got.TypeName) == 0;
                    if (!numericMismatch) { ok = false; break; }
                    softRefused = true;
                    continue;
                }
                // No implicit integer -> enum conversion; needs the argument to prove it.
                if (argVars != nullptr && i < argVars->size()
                    && CxxEnumParameterRefusesArgument((*argVars)[i], got, want))
                { ok = false; break; }
                if (!copyRef && opaqueReferentMismatchForArgument)
                { ok = false; break; }
                // A class pointer parameter bound as `void*` takes only that class or a derived one.
                if (std::string spelledBase; bool bySpelling = want.Pointer && got.Pointer)
                    if (CxxSpelledClassPointerRelation(
                            CxxStructorReferentSpelling(typeName, c.linkageName, i + 1,
                                                        c.params.size()),
                            want, got, spelledBase, bySpelling)
                        == CxxSpelledParamRelation::Unrelated)
                    { ok = false; break; }
                // A reference to a PRIMITIVE needs an ADDRESSABLE argument: a literal has none,
                // and binding one would hand the callee a pointer into a dead temporary.
                if (want.Pointer && !got.Pointer && (want.IsAlias || want.IsRvalueRef)
                    && !want.ElemPointer && !isForeignClass(want.TypeName)
                    && argVars != nullptr && i < argVars->size()
                    && (*argVars)[i].Storage == nullptr)
                {
                    // A const reference and an rvalue reference both materialize a temporary, and
                    // so does an lvalue whose address the front end did not keep. Only a PROVEN
                    // rvalue is refused by a non-const lvalue reference.
                    if (!want.CxxConstRef() && !want.IsRvalueRef && rvalue)
                    {
                        if (!blocked && !softRefused && !c.needsLocalDefinition && referenceRejection.empty())
                            referenceRejection = std::format(
                                "constructor '{}' parameter '{}' is a non-const lvalue "
                                "reference and cannot bind an rvalue; pass an lvalue",
                                typeName, referenceParameterName(want, i));
                        ok = false;
                        break;
                    }
                }
            }
            if (!ok) continue;
            if (verbose)
            {
                std::string shape;
                for (size_t i = 1; i < c.params.size(); ++i)
                    shape += (i > 1 ? ", " : "") + c.params[i].TypeName + (c.params[i].Pointer ? "*" : "");
                std::string got;
                for (const auto& a : argTypes) got += (got.empty() ? "" : ", ") + a.TypeName + (a.Pointer ? "*" : "");
                std::cout << std::format("[verbose]   ctor candidate {}({}) for ({}): exact={} omitted={} linkage={} blocked={}\n",
                                         typeName, shape, got, exact, omitted, c.linkageName, blocked || softRefused);
            }
            viable.push_back({ &c, exact, promotion, currentReferencePreference,
                               blocked || softRefused || deletedSlice, softRefused, {}, false, deletedSlice });
        }
        // Legacy pick over `pool` (in declaration order). C++ ranks only the PASSED arguments:
        // more exactly-typed parameters beat same-family conversions (`format_int(42)` picks the int
        // constructor over unsigned and long long), then reference binding; filled-in defaults only
        // break what is left, and only where per-argument ranking could not judge the pair.
        auto legacyPick = [&](const std::vector<const Viable*>& pool) {
            found = nullptr; ambiguous = false; tied.clear();
            for (const Viable* v : pool)
            {
                const auto& c = *v->ctor;
                auto take = [&] {
                    found = &c; foundExact = v->exact;
                    foundPromotion = v->promotion;
                    foundReferencePreference = v->referencePreference;
                    ambiguous = false; tied.clear();
                };
                if (found == nullptr) { take(); continue; }
                if (v->exact < foundExact) continue;
                if (v->exact > foundExact) { take(); continue; }
                // A summed promotion count only equals the per-argument [over.ics.rank] verdict for one
                // argument; crossed multi-argument promotions stay with the remaining tie-breaks (ambiguous).
                if (argTypes.size() == 1 && v->promotion < foundPromotion) continue;
                if (argTypes.size() == 1 && v->promotion > foundPromotion) { take(); continue; }
                if (v->referencePreference < foundReferencePreference) continue;
                if (v->referencePreference > foundReferencePreference) { take(); continue; }
                bool sameShape = c.params.size() == found->params.size();
                for (size_t i = 0; sameShape && i < c.params.size(); ++i)
                    sameShape = sameBoundaryType(c.params[i], found->params[i]);
                if (!sameShape)
                {
                    ambiguous = true;
                    if (tied.empty()) tied.push_back(found);
                    tied.push_back(&c);
                }
            }
        };
        {
            // A synthesized `__cflat_*` constructor (an implicit default ctor for a defaulted
            // declaration) stands for a blocked candidate: clang already resolved it.
            if (std::any_of(viable.begin(), viable.end(), [](const Viable& v) {
                    return !v.blocked && v.ctor->linkageName.starts_with("__cflat_"); }))
                std::erase_if(viable, [](const Viable& v) { return v.blocked; });
            std::vector<const Viable*> pool;
            for (const Viable& v : viable)
                if (!v.blocked) pool.push_back(&v);
            /*
             * [over.match.best] per argument, through the same conversion-sequence ranking the
             * free-function and method path uses: a candidate no worse everywhere and better
             * somewhere removes the other, and survivors no argument tells apart are ambiguous
             * (an omitted default is NOT a tie-breaker). Pairs the ranking cannot judge (a
             * non-const reference, an rvalue reference, a scalar-to-class) keep the legacy pick.
             */
            if (viable.size() > 1 && argVars != nullptr && argVars->size() == argTypes.size())
            {
                auto* self = const_cast<LLVMBackend*>(this);
                for (Viable& v : viable)
                {
                    FunctionSymbol sym;
                    sym.IsCxx = true;
                    sym.IsMethod = true;
                    sym.UniqueName = v.ctor->linkageName;
                    sym.Parameters = v.ctor->params;
                    sym.Parameters[0].VariableName = "this";
                    sym.Parameters[0].TypeName = typeName;
                    std::vector<NamedVariable> args(1);
                    args[0].TypeAndValue.TypeName = typeName;
                    args.insert(args.end(), argVars->begin(), argVars->end());
                    std::vector<CxxConversionRank> ranks = self->RankCxxConversionSequences(args, sym);
                    ranks.erase(ranks.begin());   // the implicit object is common to every candidate
                    for (size_t i = 0; i < argTypes.size() && i < ranks.size(); ++i)
                    {
                        if (!isNullptrArgument(i)) continue;
                        // nullptr: exactly a spelled std::nullptr_t (0), else a null pointer
                        // conversion to a by-value pointer or function pointer (conversion, 2).
                        const int kind = spelledNullptrT(*v.ctor, i + 1);
                        const auto& want = v.ctor->params[i + 1];
                        CxxConversionRank fresh;
                        if (kind == 1 || kind == 2) fresh.rank = 0;
                        else if ((want.Pointer || want.IsFunctionPointer) && !want.IsAlias
                                 && !want.IsRvalueRef && !want.IsCxxRefToPointer) fresh.rank = 2;
                        else continue;
                        fresh.cxxViable = true;
                        fresh.from = "nullptr";
                        ranks[i] = fresh;
                    }
                    v.ranks = std::move(ranks);
                    v.ranked = true;
                }
                auto fullyRanked = [](const Viable& v) {
                    return v.ranked && std::none_of(v.ranks.begin(), v.ranks.end(),
                        [](const CxxConversionRank& r) { return r.rank < 0; });
                };
                std::set<const Viable*> dominated;
                for (const Viable& x : viable)
                    for (const Viable& y : viable)
                    {
                        bool crossing = false;
                        if (&x == &y) continue;
                        if (CompareCxxConversionRanks(x.ranks, y.ranks, crossing) == -1)
                            dominated.insert(&y);
                    }
                std::vector<const Viable*> survivors;
                for (const Viable& v : viable)
                    if (dominated.count(&v) == 0) survivors.push_back(&v);
                bool allJudged = !survivors.empty();
                for (const Viable* v : survivors) allJudged = allJudged && fullyRanked(*v);
                // The one C++ would call cannot be called from here: pick nothing, and say why
                // (a caller may still hand the whole call to clang).
                bool refusedByBlocked = false;
                if (allJudged)
                    for (const Viable* v : survivors)
                        if (v->blocked && !refusedByBlocked)
                        {
                            refusedByBlocked = true;
                            std::string shape = DisplayCxxClassName(typeName) + "(";
                            for (size_t i = 1; i < v->ctor->params.size(); ++i)
                            {
                                if (i > 1) shape += ", ";
                                shape += v->ctor->params[i].TypeName;
                                if (v->ctor->params[i].Pointer)
                                    shape += v->ctor->params[i].IsAlias ? "&" : "*";
                            }
                            blockedWhy = "the overload C++ selects, " + shape + "), "
                                + (v->deletedSlice ? "takes a by-value base class whose copy constructor is deleted"
                                   : v->numeric ? "needs an implicit integer <-> floating conversion"
                                                : "has a default argument that is not a constant");
                        }
                if (refusedByBlocked) pool.clear();
                else if (!survivors.empty())
                {
                    pool.clear();
                    for (const Viable* v : survivors)
                        if (!v->blocked) pool.push_back(v);
                }
                // Distinct declarations no argument separates: C++ finds no best function.
                std::vector<const Viable*> distinct;
                for (const Viable* v : pool)
                {
                    bool duplicate = false;
                    for (const Viable* d : distinct)
                    {
                        bool same = d->ctor->params.size() == v->ctor->params.size();
                        for (size_t i = 0; same && i < v->ctor->params.size(); ++i)
                            same = sameBoundaryType(d->ctor->params[i], v->ctor->params[i]);
                        duplicate = duplicate || same;
                    }
                    if (!duplicate) distinct.push_back(v);
                }
                auto referenceMix = [](const CxxClassInfo::Structor& x, const CxxClassInfo::Structor& y) {
                    for (size_t k = 1; k < x.params.size() && k < y.params.size(); ++k)
                        if ((x.params[k].IsAlias || x.params[k].IsRvalueRef)
                            != (y.params[k].IsAlias || y.params[k].IsRvalueRef))
                            return true;
                    return false;
                };
                // [over.ics.rank] breaks a tie by reference binding only between two reference
                // parameters. A by-value parameter against a reference one stays tied only when
                // each such position binds an lvalue of exactly the referent type (clang: `Amb(const
                // V&)` with `Amb(V)` is ambiguous for an lvalue V). A materialized temporary keeps
                // the legacy pick, which takes the by-value parameter (ruling, Overloads ~4528).
                auto exactLvalueMixes = [&](const Viable& dx, const Viable& dy) {
                    const CxxClassInfo::Structor& x = *dx.ctor;
                    const CxxClassInfo::Structor& y = *dy.ctor;
                    for (size_t k = 1; k < x.params.size() && k < y.params.size(); ++k)
                    {
                        const bool xRef = x.params[k].IsAlias || x.params[k].IsRvalueRef;
                        const bool yRef = y.params[k].IsAlias || y.params[k].IsRvalueRef;
                        if (xRef == yRef) continue;
                        const size_t i = k - 1;
                        if (argVars == nullptr || i >= argVars->size()) return false;
                        const NamedVariable& arg = (*argVars)[i];
                        // A class prvalue or xvalue of the parameter's own class ties too (clang: the same
                        // identity bind); a scalar temporary keeps the legacy pick.
                        const bool classRvalue = argumentIsRvalue(i) && IsCxxRecord(arg.TypeAndValue.TypeName);
                        if (arg.TypeAndValue.Pointer || (!classRvalue && (argumentIsRvalue(i) || arg.Storage == nullptr)))
                            return false;
                        // A derived argument ties when both constructors bind the same base (rank 2).
                        const bool sameBase = i < dx.ranks.size() && i < dy.ranks.size()
                            && CxxSameBaseBindRank(dx.ranks[i], dy.ranks[i], x.params[k].TypeName);
                        if (arg.TypeAndValue.TypeName != x.params[k].TypeName && !sameBase) return false;
                    }
                    return true;
                };
                bool indistinguishable = distinct.size() > 1;
                for (size_t i = 0; indistinguishable && i < distinct.size(); ++i)
                {
                    indistinguishable = fullyRanked(*distinct[i]);
                    for (size_t j = i + 1; indistinguishable && j < distinct.size(); ++j)
                    {
                        bool crossing = false;
                        indistinguishable = CompareCxxConversionRanks(
                                distinct[i]->ranks, distinct[j]->ranks, crossing) == 0
                            && (!referenceMix(*distinct[i]->ctor, *distinct[j]->ctor)
                                || exactLvalueMixes(*distinct[i], *distinct[j]));
                    }
                }
                if (indistinguishable)
                {
                    ambiguous = true;
                    for (const Viable* v : distinct) tied.push_back(v->ctor);
                }
            }
            if (!ambiguous) legacyPick(pool);
        }
        if (ambiguous)
        {
            why = "matches more than one constructor overload";
            // Plan converting-constructors.md point 5: name every equal-rank candidate.
            std::string listed;
            for (const auto* candidate : tied)
            {
                std::string shape = DisplayCxxClassName(typeName) + "(";
                for (size_t i = 1; i < candidate->params.size(); ++i)
                {
                    if (i > 1) shape += ", ";
                    shape += candidate->params[i].TypeName;
                    if (candidate->params[i].Pointer)
                        shape += candidate->params[i].IsAlias ? "&" : "*";
                }
                listed += (listed.empty() ? "" : ", ") + shape + ")";
            }
            if (!listed.empty()) why += " (" + listed + ")";
            return nullptr;
        }
        if (found != nullptr) return found;
        if (rejectedIntegerFunctionPointer)
        {
            why = std::format("no overload of '{}' matches the given arguments.",
                              DisplayCxxClassName(typeName));
            return nullptr;
        }
        if (rejectedFunctionPointerToInteger)
        {
            why = std::format("no overload of '{}' matches the given arguments; "
                              "no matching conversion to parameter {} ('{}') of C++ constructor: "
                              "a function or pointer value cannot convert to an integer "
                              "(pointer -> integer needs an explicit cast)",
                              DisplayCxxClassName(typeName),
                              rejectedFunctionPointerIntegerParameter,
                              rejectedFunctionPointerIntegerType);
            return nullptr;
        }
        // A copy-deleted lvalue whose const T& constructor clang refused to instantiate.
        size_t sinkIndex = 0;
        std::string sinkParam, sinkCause;
        bool refusedRvalue = false;
        bool refusedCopySink = argVars != nullptr
            && FindRefusedCxxCopySink(typeName, "__ctor", *argVars, sinkIndex, sinkParam,
                                      sinkCause, refusedRvalue);
        // Constructor records include `this` at index zero. A defaulted allocator can make the
        // raw signature longer than this call, so match its provided prefix and verify the tail.
        if (!refusedCopySink && argVars != nullptr)
            if (auto record = cxxRecordEntries_.find(typeName);
                record != cxxRecordEntries_.end())
                for (const auto& member : record->second.members)
                {
                    if (member.kind != cflat_cinterop::RawCxxMember::Constructor
                        || member.name != "__ctor"
                        || (!member.bindRefusal.starts_with("cannot be instantiated")
                            && !cxxConstructorBodyRefusals_.contains(member.linkageName))
                        || member.paramTypes.size() <= argVars->size() + 1
                        || member.defaultArgs.size() != member.paramTypes.size())
                        continue;
                    bool defaultsAvailable = true;
                    for (size_t i = argVars->size() + 1; i < member.defaultArgs.size(); ++i)
                        defaultsAvailable = defaultsAvailable
                            && !member.defaultArgs[i].kind.empty()
                            && member.defaultArgs[i].kind != "unsupported";
                    if (!defaultsAvailable) continue;
                    for (size_t i = 0; i < argVars->size(); ++i)
                    {
                        const auto& arg = (*argVars)[i];
                        if (!IsCopyDeletedCxxLvalue(arg)) continue;
                        std::string argSpelling;
                        if (!CxxSpellingForCflatType(arg.TypeAndValue.TypeName, argSpelling)
                            || member.paramTypes[i + 1] != "const " + argSpelling + " &"
                            || !CxxDiagnosticBlamesCopyOf(member.refusalCause, argSpelling))
                            continue;
                        sinkIndex = i;
                        sinkParam = i + 1 < member.paramNames.size()
                            ? member.paramNames[i + 1] : std::string();
                        if (sinkParam.empty())
                            sinkParam = std::format("parameter {}", i + 1);
                        sinkCause = member.refusalCause;
                        refusedCopySink = true;
                        break;
                    }
                    if (refusedCopySink) break;
                }
        if (refusedCopySink)
            {
                why = std::format("cannot copy C++ class '{}' into constructor parameter '{}': "
                                  "its copy constructor is deleted",
                                  DisplayCxxClassName((*argVars)[sinkIndex].TypeAndValue.TypeName),
                                  sinkParam);
                const std::string causeLine = CxxFirstDiagnosticLine(sinkCause);
                if (!causeLine.empty()) why += std::format(" (clang: {})", causeLine);
                return nullptr;
            }
        if (candidates == 0)
        {
            why = std::format("has no constructor taking {} argument(s)", (uint64_t)argTypes.size());
            return nullptr;
        }
        if (!referenceRejection.empty())
        {
            why = referenceRejection;
            return nullptr;
        }
        if (!blockedWhy.empty())
        {
            if (info->hasCtorTemplate && argVars != nullptr
                && argVars->size() == argTypes.size())
            {
                std::string wrapperName;
                std::string wrapperError;
                auto* self = const_cast<LLVMBackend*>(this);
                const bool requested = self->RequestCxxVariadicConstructor(
                    typeName, *argVars, wrapperName, wrapperError);
                if (!requested && wrapperError.starts_with("clang: "))
                    why = wrapperError;
                else if (!requested && !self->lastCxxWrapperNoViableAtRequest_)
                {
                    std::string cause = self->CxxFirstDiagnosticLine(
                        self->lastCxxWrapperCause_);
                    if (size_t error = cause.find("error: "); error != std::string::npos)
                        cause.erase(0, error + 7);
                    why = cause.empty() ? blockedWhy : "clang: " + cause;
                }
                else
                    why = blockedWhy;
            }
            else
                why = blockedWhy;
            return nullptr;
        }
        why = CxxCtorNoMatchReason(argTypes);
        return nullptr;
    }

/*
 * The call-argument builder blanks a primitive pointer's TypeName and keeps the declared pointee
 * in InferSourceTypeName. That name is the pointee only for a PROVEN single-level pointer: a
 * recorded depth of 1, or an unrecorded `&k` whose operand slot holds a non-pointer scalar.
 */
static bool IsProvenPrimitiveSinglePointerArg(const LLVMBackend::NamedVariable& arg)
{
        const auto& type = arg.TypeAndValue;
        if (!type.Pointer || !type.TypeName.empty() || arg.InferSourceTypeName == "void"
            || !LLVMBackend::IsPrimitiveTypeName(arg.InferSourceTypeName))
            return false;
        if (type.IsProvenSinglePointerDepth()) return true;
        if (type.PointerDepth != 0 || type.ElemPointer || !type.DepthIsAboutThisValue())
            return false;
        llvm::Type* slot = nullptr;
        if (auto* alloca = llvm::dyn_cast_or_null<llvm::AllocaInst>(arg.Primary))
            slot = alloca->getAllocatedType();
        else if (auto* gep = llvm::dyn_cast_or_null<llvm::GEPOperator>(arg.Primary))
            slot = gep->getResultElementType();
        else if (auto* global = llvm::dyn_cast_or_null<llvm::GlobalVariable>(arg.Primary))
            slot = global->getValueType();
        return slot != nullptr && (slot->isIntegerTy() || slot->isFloatingPointTy());
}

// A C++ null pointer constant (the literal 0, an imported zero macro), which converts to
// std::nullptr_t ([conv.ptr]) when no ctor takes it as an int. `isNullConstant` is
// IsNullPointerConstantArgument(arg, true), the one rule. Returns the argument as a nullptr value.
static bool ZeroLiteralAsNullptrArgument(const LLVMBackend::NamedVariable& arg, bool isNullConstant,
                                         llvm::LLVMContext& context, LLVMBackend::NamedVariable& out)
{
        if (!isNullConstant || arg.TypeAndValue.Pointer) return false;
        out = arg;
        out.Primary = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(context));
        out.BaseType = out.Primary->getType();
        out.Storage = nullptr;
        out.LiteralIdentity.clear();
        out.IsCxxMacroGlobal = false;
        out.IsCxxNullIntegerMacro = false;
        out.TypeAndValue = {};
        out.TypeAndValue.IsCxxNullptrT = true;
        return true;
}

static LLVMBackend::TypeAndValue InferImplicitCxxArgumentType(
        const LLVMBackend::NamedVariable& arg, const LLVMBackend& backend)
{
        if (LLVMBackend::IsCxxFixedArrayArgument(arg))
            return LLVMBackend::CxxFixedArrayDecayType(arg);
        LLVMBackend::TypeAndValue type = arg.TypeAndValue;
        // A nullptr value converts through a ctor taking std::nullptr_t, never as a raw pointer.
        if (LLVMBackend::IsCxxNullTypedArgument(arg))
        {
            type.TypeName = "std.nullptr_t";
            type.Pointer = false;
            type.IsCxxNullptrT = true;
            return type;
        }
        // A pointer join can inherit its destination's class spelling; its arms are the source.
        if (arg.Primary != nullptr && arg.Primary->getType()->isPointerTy()
            && backend.JoinIsCharPointer(arg.Primary))
        {
            type.TypeName = "char";
            type.Pointer = true;
            type.ElemPointer = false;
            type.IsArrayView = false;
            type.IsFunctionPointer = false;
            type.CxxRefLevel = -1;
            type.IsAlias = false;
            type.IsRvalueRef = false;
            type.IsCxxRefToPointer = false;
            return type;
        }
        // A blanked primitive pointer: a C++ ctor needs `int*` back, as the spelled `T(&k)` does.
        if (IsProvenPrimitiveSinglePointerArg(arg))
            type.TypeName = arg.InferSourceTypeName;
        if (type.TypeName.empty() && type.Pointer)
        {
            if (!arg.InferSourceTypeName.empty())
                type.TypeName = arg.InferSourceTypeName;
            return type;
        }
        if (!type.TypeName.empty() || type.Pointer || arg.BaseType == nullptr)
            return type;
        if (!arg.InferSourceTypeName.empty())
        {
            type.TypeName = arg.InferSourceTypeName;
            return type;
        }
        if (arg.BaseType->isFloatingPointTy())
        {
            type.TypeName = arg.BaseType->isFloatTy() ? "float" : "double";
            return type;
        }
        if (arg.BaseType->isIntegerTy())
        {
            if (llvm::isa<llvm::ConstantInt>(arg.Primary))
            {
                // A bool literal is an i1 constant; it is a C++ bool, so B(bool) outranks B(int).
                if (arg.BaseType->getIntegerBitWidth() == 1) type.TypeName = "bool";
                else type.TypeName = arg.BaseType->getIntegerBitWidth() > 32 ? "i64" : "int";
                return type;
            }
            switch (arg.BaseType->getIntegerBitWidth())
            {
                case 1:  type.TypeName = "bool";  break;
                case 8:  type.TypeName = "char";  break;
                case 16: type.TypeName = "short"; break;
                case 32: type.TypeName = "int";   break;
                case 64: type.TypeName = "i64";   break;
                default: break;
            }
            return type;
        }
        if (arg.BaseType->isPointerTy())
            if (auto* constant = llvm::dyn_cast_or_null<llvm::Constant>(arg.Primary);
                constant != nullptr && backend.IsStringLiteralConstant(constant))
            {
                type.TypeName = "char";
                type.Pointer = true;
            }
        // A C++ operator result (Eigen's `a + b`) can arrive with only its record struct type.
        if (auto* record = llvm::dyn_cast<llvm::StructType>(arg.BaseType);
            record != nullptr && record->hasName() && backend.IsCxxRecord(record->getName().str()))
            type.TypeName = record->getName().str();
        return type;
}

std::string LLVMBackend::ExplicitCxxConstructorBlocking(
        const std::string& typeName, const std::vector<TypeAndValue>& argTypes)
{
        TryBindRefusedCxxMember(typeName, "__ctor", argTypes.size());
        std::string why;
        if (SelectCxxConstructor(typeName, argTypes, why, /*allowNumericConversions*/ true,
                                 nullptr, /*allowExplicit*/ false) != nullptr)
            return {};
        why.clear();
        const auto* any = SelectCxxConstructor(typeName, argTypes, why,
                                               /*allowNumericConversions*/ true, nullptr,
                                               /*allowExplicit*/ true);
        if (any == nullptr || !any->isExplicit) return {};
        std::string shape = typeName + "(";
        for (size_t i = 1; i < any->params.size(); ++i)
        {
            if (i > 1) shape += ", ";
            shape += any->params[i].TypeName;
            if (any->params[i].Pointer) shape += any->params[i].IsAlias ? "&" : "*";
        }
        return shape + ")";
}

/*
 * One classification for the ONE implicit user-defined conversion C++ allows at an argument, so
 * the ranking predicate and the failure note can never disagree about why a cell was refused.
 */
LLVMBackend::CxxArgConversion LLVMBackend::ClassifyCxxImplicitArgument(
        const NamedVariable& arg, const TypeAndValue& param, bool cxxByValueParam,
        bool allowTemplateClassConversion, bool allowScalarNarrowing)
{
        // An unnamed `new T[n]` result is a thin view that C++ receives as the T* it decays to,
        // as the spelled `T(new T[n])` does; a named `int[]` binding keeps its CFlat rule.
        if (arg.TypeAndValue.IsArrayView && arg.TypeAndValue.Pointer && arg.Storage == nullptr
            && IsRawArrayResult(arg.Primary))
        {
            NamedVariable decayed = arg;
            decayed.TypeAndValue.IsArrayView = false;
            // A call argument blanks a primitive pointee's TypeName; restore it as C++ spells it.
            if (decayed.TypeAndValue.TypeName.empty())
                decayed.TypeAndValue.TypeName = decayed.InferSourceTypeName;
            return ClassifyCxxImplicitArgument(decayed, param, cxxByValueParam,
                                               allowTemplateClassConversion, allowScalarNarrowing);
        }
        // R5: a raw T* (or nullptr) at a std::unique_ptr<T> parameter - the `unique T*` sugar -
        // adopts through unique_ptr(pointer); a non-const lvalue reference still takes no temp.
        if (!param.IsInterface && IsRawPointerForCxxUniquePtr(param.TypeName, arg))
            return param.IsAlias && !param.IsRvalueRef && !param.CxxConstRef()
                    && !param.ElemPointer && !cxxByValueParam
                ? CxxArgConversion::NonConstLvalueRef : CxxArgConversion::Convertible;
        // A CFlat closure handed to C++ constructs the class from its owning __cflat_closure<>;
        // clang decides at the generated constructor wrapper (std::function's template ctor).
        if (!arg.CxxClosureSignature.empty())
        {
            if (param.TypeName.empty() || param.Pointer || param.IsInterface
                || !IsForeignCxxClassWithConstructors(param.TypeName))
                return CxxArgConversion::NotApplicable;
            return param.IsAlias && !param.IsRvalueRef && !param.CxxConstRef()
                    && !param.ElemPointer && !cxxByValueParam
                ? CxxArgConversion::NonConstLvalueRef : CxxArgConversion::Convertible;
        }
        TypeAndValue argType = InferImplicitCxxArgumentType(arg, *this);
        // A class pointer needs an explicit opt-in; raw void* and resolved char* remain data pointers.
        const bool classPointerArg = argType.Pointer
            && !IsCxxNullTypedArgument(arg)
            && ((argType.TypeName.empty() && arg.InferSourceTypeName != "void"
                 && !IsProvenPrimitiveSinglePointerArg(arg))
                || IsCxxRecord(argType.TypeName)
                || dataStructures.count(argType.TypeName) != 0
                || argType.IsFunctionPointer || argType.IsArrayView);
        if (param.TypeName.empty() || param.IsInterface
            || (classPointerArg && (!arg.AllowCxxClassPointerCtorConversion
                || (param.Pointer && (!param.IsAlias || param.IsCxxRefToPointer))))
            || (param.Pointer && !cxxByValueParam
                && (!param.IsAlias || param.IsCxxRefToPointer)
                && !IsForeignCxxClassWithConstructors(param.TypeName))
            || !IsForeignCxxClassWithConstructors(param.TypeName)
            || argType.TypeName == param.TypeName)
            return CxxArgConversion::NotApplicable;

        argType.IsScopedEnum = argType.IsScopedEnum || IsScopedEnumTypeName(argType.TypeName);
        if (argType.TypeName.empty()) return CxxArgConversion::NotApplicable;
        const size_t sourceTemplate = argType.TypeName.find('$');
        const size_t targetTemplate = param.TypeName.find('$');
        const bool bothTemplateSpecializations = sourceTemplate != std::string::npos
            && targetTemplate != std::string::npos;
        const bool plainClassPair = sourceTemplate == std::string::npos
            && targetTemplate == std::string::npos;
        // Constructor templates are absent from the extracted list, so ask clang to prove this
        // class-to-class user-defined sequence before ranking a C++ value/rvalue parameter.
        // A lone candidate's `const X&` / `X&&` binds the converted temporary too ([dcl.init.ref]).
        if ((allowTemplateClassConversion || param.IsRvalueRef || param.CxxConstRef())
            && IsCxxRecord(argType.TypeName)
            && (cxxByValueParam || param.IsRvalueRef || param.CxxConstRef()))
        {
            const CxxClassInfo* target = GetCxxClassInfoWithConstructors(param.TypeName);
            if (target != nullptr && target->hasCtorTemplate
                && (bothTemplateSpecializations || plainClassPair))
            {
                NamedVariable probe = arg;
                probe.TypeAndValue = argType;
                std::string wrapperName;
                std::string wrapperError;
                if (RequestCxxVariadicConstructor(param.TypeName, { probe }, wrapperName,
                                                  wrapperError, /*copyInit*/ true))
                    return CxxArgConversion::Convertible;
            }
        }
        // A class argument into a class reachable only through constructor templates keeps the
        // C++-side call path it had before those templates were recorded.
        if (IsCxxRecord(argType.TypeName)
            && !IsForeignCxxClassWithConstructors(param.TypeName, /*countCtorTemplates*/ false))
            return CxxArgConversion::NotApplicable;
        std::string argSpelling;
        std::string paramSpelling;
        if (CxxSpellingForCflatType(argType.TypeName, argSpelling)
            && CxxSpellingForCflatType(param.TypeName, paramSpelling)
            && SqueezeCxxSpelling(argSpelling) == SqueezeCxxSpelling(paramSpelling))
            return CxxArgConversion::NotApplicable;
        // A non-const lvalue reference never takes a user-defined conversion - the temporary has
        // no address. A member's reference arrives bare with IsAlias, so key on IsAlias alone.
        const bool nonConstLvalueRef = param.IsAlias && !param.IsRvalueRef
            && !param.CxxConstRef() && !param.ElemPointer && !cxxByValueParam;
        auto answer = [&](CxxArgConversion found) {
            return nonConstLvalueRef ? CxxArgConversion::NonConstLvalueRef : found;
        };
        // An unnamed imported enum is represented as int, so the listed int ctor would
        // otherwise return before clang can rank the enum template's exact match.
        if (!nonConstLvalueRef && CxxUnnamedEnumTemplateArgument(param.TypeName, arg))
            {
                NamedVariable probe = arg;
                probe.TypeAndValue = argType;
                std::string wrapperName;
                std::string wrapperError;
                if (RequestCxxVariadicConstructor(param.TypeName, { probe }, wrapperName,
                                                  wrapperError, /*copyInit*/ true))
                    return answer(CxxArgConversion::Convertible);
            }
        if (IsCxxSharedPtrUpcast(argType, param)) return answer(CxxArgConversion::Convertible);
        // A derived lvalue binds directly to a C++ base reference. Do not turn that standard
        // conversion into a user-defined base copy, even when the base has a copy constructor.
        if (param.IsAlias && !param.ElemPointer && IsCxxDerivedToBaseValue(argType, param, nullptr))
            return CxxArgConversion::NotApplicable;
        // A derived-class value slices to a PUBLIC base parameter through the base's copy ctor.
        if (IsCxxDerivedToBaseValue(argType, param, nullptr)) argType.TypeName = param.TypeName;
        std::string why;
        // A scalar-reference overload set is clang's to rank: the copy-initializing probe
        // answers, never the listed pick.
        if (CxxCtorForwardsScalarReference(param.TypeName, { argType }))
        {
            if (nonConstLvalueRef) return CxxArgConversion::NotApplicable;
            NamedVariable probe = arg;
            probe.TypeAndValue = argType;
            std::string wrapperName;
            std::string wrapperError;
            if (RequestCxxVariadicConstructor(param.TypeName, { probe }, wrapperName,
                                              wrapperError, /*copyInit*/ true))
                return answer(CxxArgConversion::Convertible);
            // Clang's pick has a body that failed to instantiate: the use site refuses it.
            return wrapperError.starts_with("clang: ") ? answer(CxxArgConversion::Convertible)
                                                       : CxxArgConversion::NotApplicable;
        }
        // The argument's provenance lets a moved class source prove it binds `T(U&&)`.
        const std::vector<NamedVariable> argVars = { arg };
        TryBindRefusedCxxMember(param.TypeName, "__ctor", 1);
        const CxxClassInfo::Structor* selectedCtor = SelectCxxConstructor(
            param.TypeName, { argType }, why, /*allowNumericConversions*/ true, &argVars,
            /*allowExplicit*/ false, allowScalarNarrowing);
        if (selectedCtor != nullptr)
        {
            if (classPointerArg && arg.AllowCxxClassPointerCtorConversion)
            {
                const TypeAndValue* ctorArg = selectedCtor->params.size() > 1
                    ? &selectedCtor->params[1] : nullptr;
                const bool pointerByValue = ctorArg != nullptr && ctorArg->Pointer
                    && !ctorArg->ElemPointer && !ctorArg->IsAlias
                    && !ctorArg->IsCxxRefToPointer && !ctorArg->IsRvalueRef;
                const bool constPointerRef = ctorArg != nullptr && ctorArg->Pointer
                    && !ctorArg->ElemPointer && ctorArg->IsAlias
                    && ctorArg->IsCxxRefToPointer && ctorArg->CxxConstRef()
                    && !ctorArg->IsRvalueRef;
                if (!pointerByValue && !constPointerRef)
                    return CxxArgConversion::NotApplicable;
            }
            return answer(CxxArgConversion::Convertible);
        }
        // The literal 0 with no int-taking ctor: a null pointer constant into a nullptr_t ctor.
        // Copy-initialization skips explicit constructors, so this route is tried before the block.
        if (NamedVariable nullArg; ZeroLiteralAsNullptrArgument(arg,
                IsNullPointerConstantArgument(arg, true), *context, nullArg)
            && ClassifyCxxImplicitArgument(nullArg, param, cxxByValueParam,
                                           allowTemplateClassConversion)
                == CxxArgConversion::Convertible)
            return answer(CxxArgConversion::Convertible);
        if (!ExplicitCxxConstructorBlocking(param.TypeName, { argType }).empty())
            return answer(CxxArgConversion::ExplicitCtor);
        // A class source whose only route is its `operator X()` into a `const X&` / `X&&`:
        // clang copy-initializes the temporary (an ambiguous or explicit route refuses there).
        if ((param.IsRvalueRef || param.CxxConstRef()) && !argType.Pointer
            && IsCxxRecord(argType.TypeName) && HasImplicitClassConversionOperator(argType.TypeName))
        {
            NamedVariable probe = arg;
            probe.TypeAndValue = argType;
            std::string wrapperName;
            std::string wrapperError;
            if (RequestCxxVariadicConstructor(param.TypeName, { probe }, wrapperName, wrapperError,
                                              /*copyInit*/ true))
                return CxxArgConversion::Convertible;
        }
        // Past this point only a speculative clang request could answer, and it re-registers the
        // class - never run it for a parameter that could not have taken the conversion anyway.
        if (nonConstLvalueRef) return CxxArgConversion::NotApplicable;
        // Enum -> class goes through a trait-gated SFINAE ctor template the imported inventory
        // may not retain, so clang decides this narrow, lazy case.
        const bool enumArgument = argType.IsScopedEnum || !argType.EnumBacking.empty()
            || enumBackingTypes.count(argType.TypeName) != 0
            || !ResolveEnumTypeName(argType.TypeName).empty()
            || CxxUnnamedEnumExpression(arg.Storage) != nullptr
            || (!argType.TypeName.empty() && arg.BaseType != nullptr
                && arg.BaseType->isIntegerTy()
                && !IsPrimitiveTypeName(argType.TypeName)
                && !IsCxxRecord(argType.TypeName)
                && dataStructures.count(argType.TypeName) == 0);
        const CxxClassInfo* enumTargetInfo = GetCxxClassInfoWithConstructors(param.TypeName);
        if (enumArgument && enumTargetInfo != nullptr && enumTargetInfo->hasCtorTemplate)
        {
            NamedVariable probe = arg;
            probe.TypeAndValue = argType;
            std::string wrapperName;
            std::string wrapperError;
            const bool accepted = RequestCxxVariadicConstructor(param.TypeName, { probe },
                wrapperName, wrapperError, /*copyInit*/ true);
            return accepted ? answer(CxxArgConversion::Convertible)
                            : CxxArgConversion::NotApplicable;
        }
        // A converting constructor clang never lists as a member (std::optional's
        // `template <class U> optional(U&&)`): ask C++ itself whether the conversion compiles.
        // Only for a class with NO listed constructor, or a constructor TEMPLATE fed a non-class
        // argument (a class argument keeps the C++-side call path) - a speculative clang request
        // re-registers the class. The probe copy-initializes: an explicit ctor never converts.
        const auto* info = GetCxxClassInfoWithConstructors(param.TypeName);
        const bool plainClassReferenceCtor
            = (param.IsRvalueRef || param.CxxConstRef())
                && argType.TypeName.find('$') == std::string::npos
                && param.TypeName.find('$') == std::string::npos;
        if (info == nullptr || (!info->constructors.empty()
                                && !(info->hasCtorTemplate
                                     && (!IsCxxRecord(argType.TypeName)
                                         || plainClassReferenceCtor))))
            return CxxArgConversion::NotApplicable;
        NamedVariable probe = arg;
        probe.TypeAndValue = argType;
        std::string wrapperName;
        std::string wrapperError;
        return RequestCxxVariadicConstructor(param.TypeName, { probe }, wrapperName, wrapperError,
                                             /*copyInit*/ true)
            ? CxxArgConversion::Convertible : CxxArgConversion::NotApplicable;
}

bool LLVMBackend::CanImplicitlyConstructCxxClass(const NamedVariable& arg,
                                                  const TypeAndValue& param,
                                                  bool cxxByValueParam,
                                                  bool allowTemplateClassConversion)
{
        return ClassifyCxxImplicitArgument(arg, param, cxxByValueParam,
                                           allowTemplateClassConversion)
            == CxxArgConversion::Convertible;
}

bool LLVMBackend::IsCxxConvertingTemporaryForRvalueRef(const NamedVariable& arg,
                                                       const TypeAndValue& param,
                                                       bool allowTemplateClassConversion)
{
        if (!param.IsRvalueRef || !param.Pointer || param.ElemPointer || param.IsCxxRefToPointer
            || param.TypeName.empty())
            return false;
        const TypeAndValue argType = InferImplicitCxxArgumentType(arg, *this);
        TypeAndValue scalarReferent = param;
        scalarReferent.Pointer = scalarReferent.IsAlias = scalarReferent.IsRvalueRef = false;
        scalarReferent.PointerDepth = 0;
        if (!argType.Pointer && scalarReferent.IsPrimitive() && argType.IsPrimitive()
            && ClassifyScalarArgConversion(arg, scalarReferent) == ScalarArgConversion::Widen
            && IsProvenCxxScalarLvalue(arg))
            return true; // standard conversion creates the rvalue; it does not move the source
        if (!IsCxxRecord(param.TypeName)) return false;
        // A reference-related lvalue (same class or a derived one) binds directly or not at all.
        if (!argType.Pointer && !argType.TypeName.empty()
            && (argType.TypeName == param.TypeName
                || (IsCxxRecord(argType.TypeName) && IsCxxBaseOf(param.TypeName, argType.TypeName))))
            return false;
        // Cheap filter: some listed converting ctor or conversion operator could make the temp.
        const bool viaCtor = CanImplicitlyConstructCxxClass(arg, param, /*cxxByValueParam*/ false,
                                                            allowTemplateClassConversion);
        const bool constClassSource = !argType.Pointer && IsCxxRecord(argType.TypeName)
            && CxxConstReceiverKind(arg) != 0;
        TypeAndValue referent = param;
        referent.Pointer = referent.IsAlias = referent.IsRvalueRef = false;
        referent.PointerDepth = 0;
        if (!viaCtor && !constClassSource && (argType.Pointer || argType.TypeName.empty()
                         || CxxConversionOperatorTo(argType.TypeName, referent, false).empty()))
            return false;
        // Clang decides: `X&& r = source;` on the source's real cv and category (ambiguity between
        // a ctor and an operator, a const source into a `T(S&)` ctor, explicit, constraints).
        std::string parameter;
        std::string targetSpelling;
        const std::string key = CxxRvalueRefBindKey(arg, param, parameter, targetSpelling);
        if (key.empty()) return false;
        if (auto known = cxxRvalueRefBindVerdicts_.find(key); known != cxxRvalueRefBindVerdicts_.end())
            return known->second.first;
        bool verdict = false;
        std::string error;
        auto groupIt = cxxTypeOwnerGroup_.find(param.TypeName);
        if (groupIt != cxxTypeOwnerGroup_.end())
        {
            std::string generatedTypeSource;
            std::set<size_t> dependencySet;
            std::unordered_set<std::string> incompleteTypes;
            GeneratedCxxDefinitionsFor({ param.TypeName, argType.TypeName }, generatedTypeSource,
                                       dependencySet, incompleteTypes);
            if (auto owner = cxxTypeOwnerGroup_.find(argType.TypeName);
                owner != cxxTypeOwnerGroup_.end() && owner->second != groupIt->second)
                dependencySet.insert(owner->second);
            const std::vector<size_t> dependencies(dependencySet.begin(), dependencySet.end());
            CxxRequestGroup group = MakeCxxRequestGroup(groupIt->second, dependencies);
            if (!group.headers.empty())
            {
                const std::string wrapperName = std::format("__cflat_rrbind_{:016x}",
                    HashWrapperKey("rrbind|" + key));
                const std::string wrapperSource = "extern \"C\" __attribute__((weak)) void "
                    + wrapperName + "(" + parameter + " p0) { " + targetSpelling
                    + " && r = p0; (void)r; }\n";
                CxxRequestGroupScope groupScope(*this, &group);
                CSigEntry bound;
                verdict = RequestGeneratedCxxWrapper(group, generatedTypeSource + wrapperSource,
                                                     wrapperName, "RVALUE_REF_BIND", bound, error,
                                                     incompleteTypes.empty());
                if (verbose)
                    std::cout << std::format("[verbose]   rvalue-ref bind probe {} -> {}\n", key,
                                             verdict ? "binds" : "refused");
            }
        }
        cxxRvalueRefBindVerdicts_.emplace(key, std::pair{ verdict, verdict ? std::string()
                                                                  : FirstCxxErrorLine(error) });
        return verdict;
}

// "<source parameter spelling>|<target spelling>", or empty when either has no C++ spelling.
std::string LLVMBackend::CxxRvalueRefBindKey(const NamedVariable& arg, const TypeAndValue& param,
                                             std::string& parameter, std::string& targetSpelling)
{
        const TypeAndValue argType = InferImplicitCxxArgumentType(arg, *this);
        std::string sourceSpelling;
        if (!CxxSpellingForCflatType(argType.TypeName, sourceSpelling)
            || !CxxSpellingForCflatType(param.TypeName, targetSpelling))
            return {};
        if (argType.Pointer)
            parameter = (argType.PointeeConstBit() ? "const " : "") + sourceSpelling
                + (argType.ElemPointer ? " **" : " *");
        else
            parameter = (CxxConstReceiverKind(arg) != 0 ? "const " : "") + sourceSpelling + " &";
        return parameter + "|" + targetSpelling;
}

// Clang's refusal of `T r = source;` on the source's real cv and category (an rvalue source is
// spelled as an xvalue), empty when it compiles. Asked only on a refused path; cached per shape.
std::string LLVMBackend::CxxCopyInitRefusal(const NamedVariable& arg, const std::string& targetType)
{
        TypeAndValue target;
        target.TypeName = targetType;
        std::string parameter;
        std::string targetSpelling;
        std::string key = CxxRvalueRefBindKey(arg, target, parameter, targetSpelling);
        const TypeAndValue argType = InferImplicitCxxArgumentType(arg, *this);
        if (key.empty() || argType.Pointer || !parameter.ends_with(" &")) return {};
        const bool rvalue = IsCxxRvalueReferenceArgument(arg);
        key = (rvalue ? "copyinit|rvalue|" : "copyinit|lvalue|") + key;
        if (auto known = cxxRvalueRefBindVerdicts_.find(key); known != cxxRvalueRefBindVerdicts_.end())
            return known->second.first ? std::string() : known->second.second;
        bool verdict = true;
        std::string error;
        auto groupIt = cxxTypeOwnerGroup_.find(targetType);
        if (groupIt == cxxTypeOwnerGroup_.end()) return {};
        std::string generatedTypeSource;
        std::set<size_t> dependencySet;
        std::unordered_set<std::string> incompleteTypes;
        GeneratedCxxDefinitionsFor({ targetType, argType.TypeName }, generatedTypeSource,
                                   dependencySet, incompleteTypes);
        if (auto owner = cxxTypeOwnerGroup_.find(argType.TypeName);
            owner != cxxTypeOwnerGroup_.end() && owner->second != groupIt->second)
            dependencySet.insert(owner->second);
        const std::vector<size_t> dependencies(dependencySet.begin(), dependencySet.end());
        CxxRequestGroup group = MakeCxxRequestGroup(groupIt->second, dependencies);
        if (group.headers.empty()) return {};
        const std::string wrapperName = std::format("__cflat_copyinit_{:016x}",
            HashWrapperKey(key));
        const std::string initializer = rvalue
            ? "static_cast<" + parameter.substr(0, parameter.size() - 1) + "&&>(p0)" : "p0";
        const std::string wrapperSource = "extern \"C\" __attribute__((weak)) void "
            + wrapperName + "(" + parameter + " p0) { " + targetSpelling + " r = "
            + initializer + "; (void)r; }\n";
        {
            CxxRequestGroupScope groupScope(*this, &group);
            CSigEntry bound;
            verdict = RequestGeneratedCxxWrapper(group, generatedTypeSource + wrapperSource,
                                                 wrapperName, "COPY_INIT", bound, error,
                                                 incompleteTypes.empty());
        }
        if (verbose)
            std::cout << std::format("[verbose]   copy-init probe {} -> {}\n", key,
                                     verdict ? "compiles" : "refused");
        const std::string line = verdict ? std::string() : FirstCxxErrorLine(error);
        cxxRvalueRefBindVerdicts_.emplace(key, std::pair{ verdict, line });
        return line;
}

std::string LLVMBackend::CxxRvalueRefBindRefusal(const NamedVariable& arg, const TypeAndValue& param)
{
        std::string parameter;
        std::string targetSpelling;
        const std::string key = CxxRvalueRefBindKey(arg, param, parameter, targetSpelling);
        auto known = cxxRvalueRefBindVerdicts_.find(key);
        if (key.empty() || known == cxxRvalueRefBindVerdicts_.end() || known->second.first
            || known->second.second.empty())
            return {};
        std::string source = parameter.substr(0, parameter.size() - 2);
        return std::format("no implicit conversion from '{}' binds '{} &&' (clang: {})",
                           parameter.ends_with(" &") ? source : parameter, targetSpelling,
                           known->second.second);
}

std::string LLVMBackend::DescribeCxxImplicitArgumentBlock(const NamedVariable& arg,
                                                          const TypeAndValue& param,
                                                          bool cxxByValueParam)
{
        const CxxArgConversion verdict = ClassifyCxxImplicitArgument(arg, param, cxxByValueParam);
        if (verdict == CxxArgConversion::ExplicitCtor)
        {
            TypeAndValue argType = InferImplicitCxxArgumentType(arg, *this);
            std::string shape = ExplicitCxxConstructorBlocking(param.TypeName, { argType });
            if (shape.empty()) return {};
            // A class template specialization shows its CFlat spelling, not the mangled key.
            const std::string display = DisplayCxxClassName(param.TypeName);
            if (shape.starts_with(param.TypeName + "("))
                shape = display + shape.substr(param.TypeName.size());
            return std::format("C++ constructor '{}' is declared explicit, so it is not offered "
                               "as an implicit conversion here; spell the conversion out as "
                               "'{}(<argument>)'", shape, display);
        }
        if (verdict == CxxArgConversion::NonConstLvalueRef)
            return std::format("a user-defined conversion cannot bind to the non-const reference "
                               "parameter '{}&'; pass a named {} lvalue, or take the parameter by "
                               "value or by const reference", param.TypeName, param.TypeName);
        return {};
}

bool LLVMBackend::IsCxxContiguousViewSource(const NamedVariable& arg,
                                            const TypeAndValue& param,
                                            bool* useConstData,
                                            std::string* deducedElementType) const
{
        if (useConstData != nullptr) *useConstData = false;
        if (deducedElementType != nullptr) deducedElementType->clear();
        if (!param.IsArrayView || param.IsMove || param.IsUnique || param.IsOwningSink
            || arg.TypeAndValue.Pointer || arg.TypeAndValue.IsArrayView
            || arg.TypeAndValue.TypeName.empty() || !IsCxxRecord(arg.TypeAndValue.TypeName))
            return false;

        const CxxClassInfo* info = GetCxxClassInfo(arg.TypeAndValue.TypeName);
        bool hasData = false;
        bool hasConstData = false;
        bool hasMutableData = false;
        bool hasSize = false;
        std::string inferredElementType;
        bool conflictingDeducedElement = false;
        std::string expectedElement;
        const bool deduceElement = param.TypeName.empty();
        if (!deduceElement && !CxxSpellingForCflatType(param.TypeName, expectedElement))
            expectedElement = param.TypeName;
        auto compact = [](std::string text) {
            text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) {
                return std::isspace(c);
            }), text.end());
            if (text.rfind("class", 0) == 0) text.erase(0, 5);
            if (text.rfind("struct", 0) == 0) text.erase(0, 6);
            return text;
        };
        auto inspect = [&](const cflat_cinterop::RawCxxMember& raw) {
            if (raw.access != cflat_cinterop::AccessPublic
                || raw.kind != cflat_cinterop::RawCxxMember::Instance
                || raw.isDeleted || !raw.bindRefusal.empty()
                || raw.paramTypes.size() != 1)
                return;
            if (raw.name == "data")
            {
                std::string returned = compact(raw.retType);
                const size_t stars = std::count(returned.begin(), returned.end(), '*');
                const size_t expectedStars = 1 + (param.ElemPointer ? 1 : 0);
                returned.erase(std::remove(returned.begin(), returned.end(), '*'), returned.end());
                if (returned.rfind("const", 0) == 0) returned.erase(0, 5);
                bool elementMatches = returned == compact(expectedElement);
                std::string candidateElementType;
                if (deduceElement && deducedElementType != nullptr && stars == expectedStars)
                {
                    static const std::unordered_map<std::string, std::string> primitives = {
                        { "bool", "bool" },
                        { "float", "float" }, { "double", "double" },
                        { "longdouble", "longdouble" }, { "wchar_t", "wchar" },
                        { "char8_t", "c8" }, { "char16_t", "c16" }, { "char32_t", "c32" }
                    };
                    if (const char* integer = CxxCompactIntegerSpellingToCflat(returned))
                    {
                        candidateElementType = integer;
                    }
                    else if (auto it = primitives.find(returned); it != primitives.end())
                    {
                        candidateElementType = it->second;
                    }
                    else
                    {
                        for (const auto& [cflatName, cxxSpelling] : cxxCflatToCxxSpelling_)
                            if (compact(cxxSpelling) == returned)
                            {
                                candidateElementType = cflatName;
                                break;
                            }
                    }
                    if (!candidateElementType.empty())
                    {
                        if (inferredElementType.empty()) inferredElementType = candidateElementType;
                        else if (inferredElementType != candidateElementType)
                            conflictingDeducedElement = true;
                        elementMatches = !conflictingDeducedElement
                            && inferredElementType == candidateElementType;
                    }
                    else
                        elementMatches = false;
                }
                if (stars == expectedStars && elementMatches)
                {
                    hasData = true;
                    hasConstData = hasConstData || raw.isConst;
                    hasMutableData = hasMutableData || !raw.isConst;
                }
            }
            else if (raw.name == "size")
            {
                std::string result = compact(raw.retType);
                if (result.rfind("const", 0) == 0) result.erase(0, 5);
                if (CxxCompactIntegerSpellingToCflat(result) != nullptr
                    || result == "size_t" || result == "std::size_t")
                    hasSize = true;
            }
        };
        if (info != nullptr)
        {
            for (const auto& method : info->directMethods) inspect(method.raw);
        }
        std::string sourceSpelling;
        CxxSpellingForCflatType(arg.TypeAndValue.TypeName, sourceSpelling);
        for (const auto& [name, record] : cxxRecordEntries_)
        {
            std::string recordSpelling;
            const bool sameType = name == arg.TypeAndValue.TypeName
                || (!sourceSpelling.empty()
                    && CxxSpellingForCflatType(name, recordSpelling)
                    && SqueezeCxxSpelling(sourceSpelling) == SqueezeCxxSpelling(recordSpelling));
            if (sameType)
                for (const auto& raw : record.members) inspect(raw);
        }
        if (hasData && hasSize && useConstData != nullptr)
            *useConstData = hasConstData && hasMutableData;
        const bool matched = hasData && hasSize && !conflictingDeducedElement;
        if (matched && deducedElementType != nullptr)
            *deducedElementType = inferredElementType;
        if (deducedElementType != nullptr && !matched) deducedElementType->clear();
        return matched;
}

bool LLVMBackend::IsCxxDerivedToBaseValue(const TypeAndValue& from, const TypeAndValue& to,
                                          const NamedVariable* source) const
{
        const bool cxxReference = to.IsAlias && to.Pointer && !to.ElemPointer;
        if (from.Pointer || from.TypeName.empty() || to.TypeName.empty()
            || (to.Pointer && !cxxReference)) return false;
        if (from.TypeName == to.TypeName) return false;
        if (!IsCxxRecord(from.TypeName) || !IsCxxRecord(to.TypeName)) return false;
        uint64_t offset = 0;
        bool inaccessible = false;
        return FindCxxValueBaseOffset(from.TypeName, to.TypeName, offset, inaccessible, source);
}

bool LLVMBackend::IsCxxDerivedToBaseReferenceBinding(const TypeAndValue& from, const TypeAndValue& to,
                                                    const NamedVariable& arg) const
{
        if (to.ElemPointer) return false;
        // An lvalue reference binds exactly as before.
        if (!to.IsRvalueRef) return to.IsAlias && IsCxxDerivedToBaseValue(from, to, nullptr);
        // `Base&&` binds a derived prvalue only when the argument's rvalue-ness is proven; an
        // rvalue reference is never IsAlias (CInterop 4176), so it is checked as an alias here.
        if (!IsCxxRvalueReferenceArgument(arg)) return false;
        TypeAndValue target = to;
        target.IsAlias = true;
        return IsCxxDerivedToBaseValue(from, target, nullptr);
}

bool LLVMBackend::IsCxxSharedPtrUpcast(const TypeAndValue& from,
                                       const TypeAndValue& to) const
{
        if (from.Pointer || from.TypeName.empty() || to.TypeName.empty()) return false;
        // Cheap shape reject first: this runs per overload candidate, and demangling is not free.
        if (MangledBase(from.TypeName) != "std.shared_ptr"
            || MangledBase(to.TypeName) != "std.shared_ptr") return false;
        TypeSpelling fromSpelling;
        TypeSpelling toSpelling;
        if (!DemangleType(*this, from.TypeName, fromSpelling)
            || !DemangleType(*this, to.TypeName, toSpelling)
            || fromSpelling.base != "std.shared_ptr"
            || toSpelling.base != "std.shared_ptr"
            || fromSpelling.args.size() != 1 || toSpelling.args.size() != 1)
            return false;
        TypeAndValue sourceElement{ .TypeName = MangleType(*this, fromSpelling.args[0]) };
        TypeAndValue targetElement{ .TypeName = MangleType(*this, toSpelling.args[0]) };
        return IsCxxRecord(sourceElement.TypeName) && IsCxxRecord(targetElement.TypeName)
            && IsCxxBaseOf(targetElement.TypeName, sourceElement.TypeName);
}

void LLVMBackend::ConstructThroughCxxCtorWrapper(NamedVariable& arg, const NamedVariable& source,
                                                 const std::string& targetType,
                                                 const std::string& wrapperName,
                                                 llvm::Value* intoSlot)
{
        llvm::Type* objectType = GetType(TypeAndValue{ .TypeName = targetType });
        auto* wrapperSlot = intoSlot != nullptr ? intoSlot : CreateAlloca(objectType);
        NamedVariable self;
        self.Primary = wrapperSlot;
        self.BaseType = wrapperSlot->getType();
        self.TypeAndValue.TypeName = targetType;
        self.TypeAndValue.Pointer = true;
        self.IsRvalue = true;
        std::vector<NamedVariable> wrapperArguments = { self, source };
        CreateOverloadedFunctionCall(wrapperName, wrapperArguments);
        if (intoSlot == nullptr && IsForeignNontrivialCxxClass(targetType))
            RegisterOwnedStructTemp(wrapperSlot, targetType);
        NamedVariable built;
        built.Primary = CreateLoad(objectType, wrapperSlot);
        built.Storage = wrapperSlot;
        built.BaseType = objectType;
        built.TypeAndValue.TypeName = targetType;
        built.TypeAndValue.VariableName = arg.TypeAndValue.VariableName;
        built.IsRvalue = intoSlot == nullptr;
        arg = std::move(built);
}

bool LLVMBackend::MaterializeImplicitCxxClassArgument(NamedVariable& arg,
                                                       const TypeAndValue& param,
                                                       llvm::Value* intoSlot,
                                                       bool allowTemplateClassConversion,
                                                       bool allowScalarNarrowing)
{
        // Same decay as ClassifyCxxImplicitArgument: the unnamed `new T[n]` result converts as its T*.
        if (arg.TypeAndValue.IsArrayView && arg.TypeAndValue.Pointer && arg.Storage == nullptr
            && IsRawArrayResult(arg.Primary))
        {
            NamedVariable decayed = arg;
            decayed.TypeAndValue.IsArrayView = false;
            if (decayed.TypeAndValue.TypeName.empty())
                decayed.TypeAndValue.TypeName = decayed.InferSourceTypeName;
            if (!MaterializeImplicitCxxClassArgument(decayed, param, intoSlot,
                                                     allowTemplateClassConversion,
                                                     allowScalarNarrowing))
                return false;
            arg = std::move(decayed);
            return true;
        }
        if (IsRawPointerForCxxUniquePtr(param.TypeName, arg))
        {
            TypeAndValue classType{ .TypeName = param.TypeName };
            llvm::Type* objectType = GetType(classType);
            if (objectType == nullptr || !objectType->isSized()) return false;
            auto* adoptSlot = intoSlot != nullptr ? intoSlot : CreateAlloca(objectType);
            if (!TryAdoptRawPointerIntoCxxUniquePtr(param.TypeName, adoptSlot, arg,
                                                    "a call argument", &param))
                return false;
            // The temporary dies at the end of the full expression, moved-from or not.
            if (intoSlot == nullptr) RegisterOwnedStructTemp(adoptSlot, param.TypeName);
            NamedVariable adopted;
            adopted.Primary = CreateLoad(objectType, adoptSlot);
            adopted.Storage = adoptSlot;
            adopted.BaseType = objectType;
            adopted.TypeAndValue.TypeName = param.TypeName;
            adopted.IsRvalue = intoSlot == nullptr;
            adopted.IsExplicitMove = true;
            arg = std::move(adopted);
            return true;
        }
        // The literal 0 takes an int ctor when one exists, else the nullptr_t one.
        if (NamedVariable nullArg; ZeroLiteralAsNullptrArgument(arg,
                IsNullPointerConstantArgument(arg, true), *context, nullArg))
        {
            std::string intWhy;
            TypeAndValue intType{ .TypeName = "int" };
            const std::vector<NamedVariable> zeroVars = { arg };
            if (SelectCxxConstructor(param.TypeName, { intType }, intWhy,
                                     /*allowNumericConversions*/ true, &zeroVars,
                                     /*allowExplicit*/ false,
                                     /*allowScalarNarrowing*/ true) == nullptr
                && ClassifyCxxImplicitArgument(nullArg, param, /*cxxByValueParam*/ true)
                    == CxxArgConversion::Convertible)
                arg = std::move(nullArg);
        }
        TypeAndValue argType = InferImplicitCxxArgumentType(arg, *this);
        // A wrapped CFlat closure takes the same generated-constructor route as a shared_ptr upcast.
        const bool sharedPtrConversion = IsCxxSharedPtrUpcast(argType, param)
            || !arg.CxxClosureSignature.empty();
        if (sharedPtrConversion)
        {
            TypeAndValue classType{ .TypeName = param.TypeName };
            llvm::Type* objectType = GetType(classType);
            if (objectType == nullptr || !objectType->isSized()) return false;
            NamedVariable source = arg;
            source.TypeAndValue = argType;
            source.TypeAndValue.VariableName.clear();
            std::string wrapperName;
            std::string wrapperError;
            if (!RequestCxxVariadicConstructor(param.TypeName, { source }, wrapperName,
                                               wrapperError))
                return false;
            auto* wrapperSlot = intoSlot != nullptr ? intoSlot : CreateAlloca(objectType);
            NamedVariable self;
            self.Primary = wrapperSlot;
            self.BaseType = wrapperSlot->getType();
            self.TypeAndValue.TypeName = param.TypeName;
            self.TypeAndValue.Pointer = true;
            self.IsRvalue = true;
            std::vector<NamedVariable> wrapperArguments{ self, source };
            ConvertCxxCtorThunkArguments(wrapperName, wrapperArguments);
            CreateOverloadedFunctionCall(wrapperName, wrapperArguments, false,
                                         DisplayCxxClassName(param.TypeName));
            if (intoSlot == nullptr && IsForeignNontrivialCxxClass(param.TypeName))
                RegisterOwnedStructTemp(wrapperSlot, param.TypeName);
            NamedVariable converted;
            converted.Primary = CreateLoad(objectType, wrapperSlot);
            converted.Storage = wrapperSlot;
            converted.BaseType = objectType;
            converted.TypeAndValue.TypeName = param.TypeName;
            converted.TypeAndValue.VariableName = arg.TypeAndValue.VariableName;
            converted.IsRvalue = intoSlot == nullptr;
            arg = std::move(converted);
            return true;
        }
        uint64_t baseOffset = 0;
        bool baseInaccessible = false;
        const bool slicesToBase = IsCxxDerivedToBaseValue(argType, param, &arg)
            && FindCxxValueBaseOffset(argType.TypeName, param.TypeName, baseOffset, baseInaccessible,
                                      &arg)
            && !baseInaccessible;
        cxxVirtualBaseRefusal_.clear();     // unproven: clang's copy wrapper takes the source
        if (slicesToBase) argType.TypeName = param.TypeName;
        std::string why;
        const std::vector<NamedVariable> argVars = { arg };
        TryBindRefusedCxxMember(param.TypeName, "__ctor", 1);
        // A literal 0 is no narrowing at all: it fits the char/short ctor, so the null tie is seen.
        const auto* ctor = SelectCxxConstructor(param.TypeName, { argType }, why,
                                                /*allowNumericConversions*/ true, &argVars,
                                                /*allowExplicit*/ false,
                                                allowScalarNarrowing
                                                    || IsNullPointerConstantArgument(arg, true));
        // A derived PRVALUE sliced to its base binds the base's move constructor (clang: one move).
        if (slicesToBase && ctor != nullptr && arg.IsRvalue && IsCxxRvalueReferenceArgument(arg))
            if (const auto* moveCtor = FindCxxMoveCtor(param.TypeName); moveCtor != nullptr)
                ctor = moveCtor;
        // A zero-literal tie (SCL(char) beside SCL(long)): clang's copy-init wrapper names it.
        const bool zeroTie = ctor == nullptr && IsNullPointerConstantArgument(arg, true)
            && why.starts_with("matches more than one constructor overload");
        // A derived rvalue (prvalue or `move d`) slices through the base's move constructor, as
        // clang binds B&& to the base subobject; a derived lvalue keeps the copy constructor.
        if (slicesToBase && IsCxxRvalueReferenceArgument(arg))
            if (const auto* moveCtor = FindCxxMoveCtor(param.TypeName);
                moveCtor != nullptr && !moveCtor->isDeleted && !moveCtor->linkageName.empty()
                && CheckCxxDemand(moveCtor->linkageName).empty())
                ctor = moveCtor;
        // A converting constructor TEMPLATE can outrank a listed non-identity pick.
        bool nullPointerCandidate = false;
        const bool clangResolves = !slicesToBase
            && CxxConstructorNeedsClangResolution(param.TypeName, ctor, { argType},
                                                   &argVars, /*allowExplicit*/ false,
                                                   &nullPointerCandidate);
        if (const std::string sink = CxxConstructorCopySinkError(ctor, argVars);
            !sink.empty())
            LogError(sink);
        if (ctor != nullptr && ctor->params.size() < 2 && !clangResolves) return false;

        TypeAndValue classType;
        classType.TypeName = param.TypeName;
        llvm::Type* objectType = GetType(classType);
        if (objectType == nullptr || !objectType->isSized()) return false;
        llvm::Value* value = arg.Primary != nullptr ? arg.Primary : LoadArgStorage(arg);
        if (value == nullptr) return false;
        // No listed constructor, or a template that may outrank it: construct through the
        // generated copy-initializing C++ wrapper, which lets clang resolve the conversion.
        std::string wrapperName;
        std::string wrapperError;
        NamedVariable source = arg;
        source.TypeAndValue = argType;
        // A slice keeps the derived source type: the wrapper's `Base&&` then binds the base subobject
        // of the object (LowerAliasByPointerArg), not a cast of the derived value.
        if (slicesToBase) source.TypeAndValue = arg.TypeAndValue;
        source.TypeAndValue.VariableName.clear();
        const auto* info = GetCxxClassInfoWithConstructors(param.TypeName);
        // A non-const `operator T()` beats `T(const S&)` on an lvalue; copy-initialization
        // in the wrapper lets clang make that pick.
        const bool operatorBindsBetter = !argType.Pointer && !slicesToBase
            && CxxConversionOperatorBindsBetter(argType.TypeName, param.TypeName);
        const size_t sourceTemplate = argType.TypeName.find('$');
        const size_t targetTemplate = param.TypeName.find('$');
        const bool bothTemplateSpecializations = sourceTemplate != std::string::npos
            && targetTemplate != std::string::npos;
        const bool plainClassPair = sourceTemplate == std::string::npos
            && targetTemplate == std::string::npos;
        const bool classTemplateConversion
            = (allowTemplateClassConversion || param.IsRvalueRef || param.CxxConstRef())
            && info != nullptr && info->hasCtorTemplate
            && (bothTemplateSpecializations || plainClassPair)
            && IsCxxRecord(argType.TypeName);
        // Only an `operator X()` converts a class source into a reference parameter (above).
        const bool operatorOnly = ctor == nullptr && !slicesToBase && !argType.Pointer
            && (param.IsRvalueRef || param.CxxConstRef()) && IsCxxRecord(argType.TypeName)
            && HasImplicitClassConversionOperator(argType.TypeName);
        const bool unnamedEnumTemplateSelection = CxxUnnamedEnumTemplateArgument(param.TypeName, arg);
        const bool defaultedCtorTie = HasDefaultedCxxConstructorTie(
            param.TypeName, { argType }, /*allowExplicit*/ false);
        // A slice with no listed constructor is the implicit base copy: clang's wrapper builds it.
        const bool sliceByWrapper = slicesToBase && ctor == nullptr;
        const bool wrapped = (sliceByWrapper || info != nullptr)
            && (sliceByWrapper || operatorBindsBetter || classTemplateConversion || operatorOnly || zeroTie
                || defaultedCtorTie
                || unnamedEnumTemplateSelection
                || ((ctor == nullptr || clangResolves)
                    && (info->constructors.empty()
                        || (info->hasCtorTemplate && !IsCxxRecord(argType.TypeName))
                        || CxxCtorForwardsScalarReference(param.TypeName, { argType })
                        || nullPointerCandidate)))
            && RequestCxxVariadicConstructor(param.TypeName, { source }, wrapperName,
                                             wrapperError, /*copyInit*/ true);
        // Clang's pick, or the listed pick called directly, failed to instantiate its body.
        if (!wrapped && wrapperError.starts_with("clang: "))
        {
            LogError(wrapperError);
            return false;
        }
        if (!wrapped)
            if (std::string refusal = CxxConstructorBodyRefusalError(ctor); !refusal.empty())
            {
                LogError(refusal);
                return false;
            }
        if (!wrapped && (nullPointerCandidate || zeroTie) && !wrapperError.empty())
            LogError(wrapperError);
        // A scalar-reference overload set has no listed fallback: clang's refusal stands.
        if (!wrapped && (ctor == nullptr || ctor->params.size() < 2
                         || CxxCtorForwardsScalarReference(param.TypeName, { argType })
                         || nullPointerCandidate))
            return false;
        if (wrapped)
        {
            ConstructThroughCxxCtorWrapper(arg, source, param.TypeName, wrapperName, intoSlot);
            return true;
        }

        llvm::Type* constructorType = GetType(ctor->params[1]);
        if (constructorType == nullptr) return false;
        auto sameScalarCxxType = [&](const std::string& left, const std::string& right) {
            std::string leftSpelling;
            std::string rightSpelling;
            return CxxSpellingForCflatType(left, leftSpelling)
                && CxxSpellingForCflatType(right, rightSpelling)
                && SqueezeCxxSpelling(leftSpelling) == SqueezeCxxSpelling(rightSpelling);
        };
        bool refPointerBound = false;
        if (slicesToBase)
        {
            // The base copy ctor takes a reference: point it at the base subobject of the
            // derived value (stored to a temp when the value is not already addressable).
            if (!constructorType->isPointerTy()) return false;
            llvm::Value* address = arg.Storage;
            if (address == nullptr)
            {
                address = AllocaAtEntry(value->getType(), nullptr, "cxx.slice.arg");
                builder->CreateStore(value, address);
            }
            value = EmitCxxBaseAdjust(address, baseOffset);
        }
        else if (arg.Storage != nullptr && !arg.TypeAndValue.Pointer
                 && ctor->params[1].Pointer && !ctor->params[1].ElemPointer
                 && (ctor->params[1].IsAlias || ctor->params[1].IsRvalueRef)
                 && IsCxxRecord(argType.TypeName)
                 && sameScalarCxxType(ctor->params[1].TypeName, argType.TypeName))
        {
            // A class reference parameter binds the source OBJECT, as `T(u)` does: a bitwise
            // copy would let `T(U&&)` steal from a copy while the source keeps its resources.
            value = arg.Storage;
        }
        else if (auto* load = llvm::dyn_cast<llvm::LoadInst>(value);
                 load != nullptr && !arg.TypeAndValue.Pointer && ctor->params[1].Pointer
                 && !ctor->params[1].ElemPointer && ctor->params[1].IsAlias
                 && constructorType->isPointerTy() && !IsCxxRvalueReferenceArgument(arg)
                 && (load->getType()->isIntegerTy() || load->getType()->isFloatingPointTy())
                 && sameScalarCxxType(ctor->params[1].TypeName, argType.TypeName))
        {
            // A scalar lvalue binds `T&` / `const T&` to its own slot, as `T(x)` does in C++.
            value = load->getPointerOperand();
        }
        else if (arg.TypeAndValue.Pointer && constructorType->isPointerTy()
                 && ctor->params[1].IsCxxRefToPointer && ctor->params[1].IsAlias
                 && !ctor->params[1].IsRvalueRef)
        {
            if (!ctor->params[1].CxxConstRef())
            {
                // A non-const `T*&` binds the argument's own pointer slot (a write must reach
                // it); only an lvalue of exactly that pointer type can bind, as in C++.
                llvm::Value* slotOfArg = arg.Storage;
                if (auto* load = llvm::dyn_cast<llvm::LoadInst>(value);
                    slotOfArg == nullptr && load != nullptr && !arg.IsRvalue
                    && !IsCxxRvalueReferenceArgument(arg))
                    slotOfArg = load->getPointerOperand();
                if (slotOfArg == nullptr || !argType.Pointer || argType.ElemPointer
                    || ctor->params[1].ElemPointer
                    || IsCxxDerivedToBasePointer(argType, ctor->params[1])
                    || IsCxxDerivedToClangBasePointer(argType, ctor->params[1])
                    || !sameScalarCxxType(ctor->params[1].TypeName, argType.TypeName))
                    return false;
                value = slotOfArg;
            }
            else
            {
                // A const reference to a pointer binds the address of a pointer temporary. A
                // derived pointer is converted to the base (null-preserving) BEFORE the store.
                if (!source.TypeAndValue.ElemPointer && ctor->params[1].Pointer
                    && (IsCxxDerivedToBasePointer(source.TypeAndValue, ctor->params[1])
                        || IsCxxDerivedToClangBasePointer(source.TypeAndValue, ctor->params[1])))
                {
                    value = LowerCxxPointerToBase(source.TypeAndValue.TypeName,
                                                  ctor->params[1].TypeName, false, value,
                                                  SpellType(*this, source.TypeAndValue),
                                                  SpellType(*this, ctor->params[1]),
                                                  "the argument");
                    if (value == nullptr) return false;
                }
                auto* temp = AllocaAtEntry(value->getType(), nullptr, "cxx.conv.refptr");
                builder->CreateStore(value, temp);
                value = temp;
            }
            refPointerBound = true;
        }
        else if (value->getType() != constructorType)
        {
            if (constructorType->isPointerTy() && !value->getType()->isPointerTy())
            {
                // LLVM opaque pointers hide the referred-to type of a C++ T&& parameter.
                // Materialize scalar conversions in the constructor's nominal T type, not in
                // the source literal's narrow type, or the C++ constructor reads past the temp.
                if (value->getType()->isIntegerTy())
                {
                    TypeAndValue referred = ctor->params[1];
                    referred.Pointer = false;
                    referred.ElemPointer = false;
                    referred.PointerDepth = 0;
                    referred.IsRvalueRef = false;
                    if (llvm::Type* referredType = GetType(referred);
                        referredType != nullptr && referredType->isIntegerTy())
                    {
                        value = CreateCast(value, referredType,
                            arg.TypeAndValue.IsUnsignedInteger() == -1, referred.IsUnsignedInteger() != -1);
                    }
                }
                auto* temp = AllocaAtEntry(value->getType(), nullptr, "cxx.conv.refarg");
                builder->CreateStore(value, temp);
                value = temp;
            }
            else
            {
                // Implicit converting constructor: a floating argument into an integer
                // parameter is refused (ruling 2026-10-13). Explicit casts stay legal.
                RefuseImplicitFloatToInt(value, constructorType,
                                         ctor->params.size() > 1 && ctor->params[1].IsUnsignedInteger() != -1);
                value = Upconvert(value, constructorType,
                                  arg.TypeAndValue.IsUnsignedInteger() != -1);
                if (value->getType() != constructorType)
                    value = CreateCast(value, constructorType);
            }
        }

        auto* slot = intoSlot != nullptr ? intoSlot : CreateAlloca(objectType);
        const bool derivedPointerCtorArg = !refPointerBound && source.TypeAndValue.Pointer
            && ctor->params.size() > 1 && ctor->params[1].Pointer
            && !source.TypeAndValue.ElemPointer
            && (IsCxxDerivedToBasePointer(source.TypeAndValue, ctor->params[1])
                || IsCxxDerivedToClangBasePointer(source.TypeAndValue, ctor->params[1]));
        const bool byValueCxxParameterCopiesSource = ctor->params.size() > 1
            && !ctor->params[1].IsCxxRef()
            && IsForeignNontrivialCxxClass(ctor->params[1].TypeName)
            && !source.IsRvalue;
        const std::vector<NamedVariable> ctorArgVars{ source };
        if (!EmitCxxStructorCall(param.TypeName, *ctor, slot, { value },
                                 (derivedPointerCtorArg || byValueCxxParameterCopiesSource)
                                     ? &ctorArgVars : nullptr)) return false;
        if (intoSlot == nullptr && IsForeignNontrivialCxxClass(param.TypeName))
            RegisterOwnedStructTemp(slot, param.TypeName);

        NamedVariable converted;
        converted.Primary = CreateLoad(objectType, slot);
        converted.Storage = slot;
        converted.BaseType = objectType;
        converted.TypeAndValue.TypeName = param.TypeName;
        converted.TypeAndValue.VariableName = arg.TypeAndValue.VariableName;
        converted.IsRvalue = intoSlot == nullptr;
        arg = std::move(converted);
        return true;
}

bool LLVMBackend::HasImplicitClassConversionOperator(const std::string& sourceType)
{
        std::set<std::string> visited;
        auto hasOperator = [&](auto&& self, const std::string& type) -> bool {
            if (!visited.insert(type).second) return false;
            EnsureCxxRecordProjected(type, false);
            auto record = cxxRecordEntries_.find(type);
            if (record == cxxRecordEntries_.end()) return false;
            for (const auto& member : record->second.members)
            {
                if (!member.isConversion || member.isExplicit || member.isDeleted) continue;
                TypeAndValue returnType;
                if (MapCTypeToTypeAndValue(member.retType, returnType, /*cxxBoundary*/ true)
                    && !returnType.IsPrimitive() && !returnType.ElemPointer
                    && (!returnType.Pointer || returnType.IsAlias))
                    return true;
            }
            for (const auto& base : record->second.bases)
                if (base.access == cflat_cinterop::AccessPublic && self(self, base.name))
                    return true;
            return false;
        };
        return hasOperator(hasOperator, sourceType);
}

bool LLVMBackend::HasDeletedConvertingCtor(const std::string& targetType)
{
        // Deleted constructors never reach the ranked table, so read the raw record members.
        // Any deleted non-explicit one-argument constructor (template included) can win a pick
        // CFlat's ranking cannot see, so clang decides for every class source of this target.
        EnsureCxxRecordProjected(targetType, false);
        auto record = cxxRecordEntries_.find(targetType);
        if (record == cxxRecordEntries_.end()) return false;
        for (const auto& member : record->second.members)
            if (member.kind == cflat_cinterop::RawCxxMember::Constructor && member.isDeleted
                && !member.isExplicit && !member.isCopyCtor && !member.isMoveCtor
                && member.paramTypes.size() == 2)
                return true;
        for (const auto& tmpl : record->second.ctorTemplates)
            if (tmpl.isDeleted && !tmpl.isExplicit && !tmpl.paramTypes.empty())
                return true;
        return false;
}

bool LLVMBackend::HasCxxImplicitConvertingCtor(const std::string& targetType)
{
        EnsureCxxRecordProjected(targetType, false);
        auto record = cxxRecordEntries_.find(targetType);
        if (record == cxxRecordEntries_.end()) return false;
        for (const auto& ctor : record->second.ctorTemplates)
            if (!ctor.isExplicit && !ctor.isDeleted && !ctor.paramTypes.empty()) return true;
        for (const auto& member : record->second.members)
        {
            // paramTypes[0] is the implicit object; [1] the one argument, the rest defaulted.
            if (member.kind != cflat_cinterop::RawCxxMember::Constructor || member.isExplicit
                || member.isDeleted || member.paramTypes.size() < 2)
                continue;
            bool oneArgument = true;
            for (size_t n = 2; oneArgument && n < member.paramTypes.size(); ++n)
                if ((n >= member.defaultArgs.size() || member.defaultArgs[n].kind.empty())
                    && !member.paramTypes[n].ends_with("..."))
                    oneArgument = false;
            if (oneArgument) return true;
        }
        return false;
}



LLVMBackend::CxxTernaryVerdict LLVMBackend::ClassifyCxxTernary(const TypeAndValue& trueArm,
                                                               CxxArmCategory trueCategory,
                                                               const TypeAndValue& falseArm,
                                                               CxxArmCategory falseCategory)
{
        const bool trueIsPrvalue = trueCategory == CxxArmCategory::Prvalue;
        const bool falseIsPrvalue = falseCategory == CxxArmCategory::Prvalue;
        CxxTernaryVerdict verdict;
        if (trueArm.Pointer || falseArm.Pointer || trueArm.IsArrayView || falseArm.IsArrayView
            || trueArm.IsInterface || falseArm.IsInterface || trueArm.IsUnique || falseArm.IsUnique)
            return verdict;
        const std::string trueName = ResolveTypeAlias(trueArm.TypeName);
        const std::string falseName = ResolveTypeAlias(falseArm.TypeName);
        if (trueName.empty() || falseName.empty() || trueName == falseName
            || !IsCxxRecord(trueName) || !IsCxxRecord(falseName))
            return verdict;
        const bool trueConst = CxxTernaryArmConst(trueArm);
        const bool falseConst = CxxTernaryArmConst(falseArm);
        const std::string key = trueName + (trueConst ? "|const|" : "||")
            + CxxArmCategoryTag(trueCategory) + "|" + falseName + (falseConst ? "|const|" : "||")
            + CxxArmCategoryTag(falseCategory);
        if (auto known = cxxTernaryVerdicts_.find(key); known != cxxTernaryVerdicts_.end())
            return known->second;
        std::string trueSpelling;
        std::string falseSpelling;
        auto trueOwner = cxxTypeOwnerGroup_.find(trueName);
        auto falseOwner = cxxTypeOwnerGroup_.find(falseName);
        if (!CxxSpellingForCflatType(trueName, trueSpelling)
            || !CxxSpellingForCflatType(falseName, falseSpelling)
            || trueOwner == cxxTypeOwnerGroup_.end() || falseOwner == cxxTypeOwnerGroup_.end())
            return verdict;
        CxxRequestGroup group = MakeCxxClassRequestGroup(trueOwner->second, trueName,
                                                         { falseOwner->second });
        if (group.headers.empty()) return verdict;
        const std::string trueCv = (trueConst ? "const " : "") + trueSpelling;
        const std::string falseCv = (falseConst ? "const " : "") + falseSpelling;
        // Declared names are per pair: every probe of one TU shares the declarations' scope.
        const uint64_t pairHash = HashWrapperKey(key);
        const std::string trueCall = std::format("__cflat_ternary_pv_t_{:016x}", pairHash);
        const std::string falseCall = std::format("__cflat_ternary_pv_f_{:016x}", pairHash);
        std::string decls;
        if (trueIsPrvalue) decls += trueCv + " " + trueCall + "(); ";
        if (falseIsPrvalue) decls += falseCv + " " + falseCall + "(); ";
        // An xvalue arm is the caller's object cast to an rvalue reference, never a temporary.
        auto armExpr = [](CxxArmCategory category, const std::string& call,
                          const std::string& cv, const char* object) {
            if (category == CxxArmCategory::Prvalue) return call + "()";
            if (category == CxxArmCategory::Xvalue)
                return "static_cast<" + cv + " &&>(" + object + ")";
            return std::string(object);
        };
        const std::string trueExpr = armExpr(trueCategory, trueCall, trueCv, "*p0");
        const std::string falseExpr = armExpr(falseCategory, falseCall, falseCv, "*p1");
        const std::string conditional = "true ? " + trueExpr + " : " + falseExpr;
        std::string lastError;
        auto probe = [&](const std::string& tag, const std::string& check) {
            const std::string name = std::format("__cflat_ternary_{}_{:016x}", tag,
                HashWrapperKey(key + "|" + tag));
            const std::string source = "#include <type_traits>\n"
                "extern \"C\" __attribute__((weak)) void " + name + "(" + trueCv + " * p0, "
                + falseCv + " * p1) { " + decls + "using R = decltype(" + conditional + "); "
                + check + " }\n";
            CxxRequestGroupScope groupScope(*this, &group);
            CSigEntry ignored;
            std::string error;
            const bool accepted = RequestGeneratedCxxWrapper(group, source, name,
                "TERNARY_PROBE", ignored, error);
            if (!accepted) lastError = error;
            return accepted;
        };
        using Kind = CxxTernaryVerdict::Kind;
        if (!probe("valid", ""))
            verdict.kind = lastError.find("ambiguous") != std::string::npos
                ? Kind::Ambiguous : Kind::Ill;
        else if (probe("lref", "static_assert(std::is_lvalue_reference_v<R>, \"\");"))
            verdict.kind = Kind::LvalueRef;
        else if (probe("xref", "static_assert(std::is_rvalue_reference_v<R>, \"\");"))
            verdict.kind = Kind::XvalueRef;
        else
            verdict.kind = Kind::Prvalue;
        if (verdict.kind == Kind::LvalueRef || verdict.kind == Kind::XvalueRef
            || verdict.kind == Kind::Prvalue)
            verdict.resultIsTrue = probe("true_result", "static_assert(std::is_same_v<"
                "std::remove_cvref_t<R>, " + trueSpelling + ">, \"\");");
        cxxTernaryVerdicts_[key] = verdict;
        return verdict;
}

/*
 * Lowers one arm of a classified `c ? A : B` to the result type. The wrapper evaluates the real
 * conditional with this arm taken and the other arm in the dead branch of a constant condition, so
 * clang emits exactly the binding, conversion or copy. A reference result hands back the referent's
 * address; a prvalue result is constructed into a new temp. An arm already of the result type is
 * left alone unless it must be copied into a prvalue.
 */
bool LLVMBackend::LowerCxxTernaryArm(NamedVariable& arm, const TypeAndValue& trueArm,
                                     CxxArmCategory trueCategory, const TypeAndValue& falseArm,
                                     CxxArmCategory falseCategory, bool armIsTrue,
                                     const CxxTernaryVerdict& verdict, bool discardResult,
                                     llvm::Value* resultSlot, llvm::Value* callSink,
                                     const std::string& callReturnSpelling,
                                     std::vector<llvm::Value*>* callerOwnedProducerSlots,
                                     bool trackConvertedProducer,
                                     llvm::Value* callSinkEnvironment,
                                     bool callSinkTakesReference)
{
        using Kind = CxxTernaryVerdict::Kind;
        if (callSink != nullptr && callReturnSpelling.empty())
            return false;
        const TypeAndValue& armType = armIsTrue ? trueArm : falseArm;
        const TypeAndValue& otherType = armIsTrue ? falseArm : trueArm;
        const CxxArmCategory armCategory = armIsTrue ? trueCategory : falseCategory;
        const CxxArmCategory otherCategory = armIsTrue ? falseCategory : trueCategory;
        const bool armIsPrvalue = armCategory == CxxArmCategory::Prvalue;
        const bool refResult = verdict.kind == Kind::LvalueRef || verdict.kind == Kind::XvalueRef;
        const std::string armName = ResolveTypeAlias(armType.TypeName);
        const std::string otherName = ResolveTypeAlias(otherType.TypeName);
        const std::string resultName = ResolveTypeAlias(
            verdict.resultIsTrue ? trueArm.TypeName : falseArm.TypeName);
        if (resultSlot == nullptr && callSink == nullptr && armName == resultName
            && (refResult || armIsPrvalue))
            return true;
        std::string armSpelling;
        std::string otherSpelling;
        std::string resultSpelling;
        if (!CxxSpellingForCflatType(armName, armSpelling)
            || !CxxSpellingForCflatType(otherName, otherSpelling)
            || !CxxSpellingForCflatType(resultName, resultSpelling))
            return false;
        auto armOwner = cxxTypeOwnerGroup_.find(armName);
        auto otherOwner = cxxTypeOwnerGroup_.find(otherName);
        auto resultOwner = cxxTypeOwnerGroup_.find(resultName);
        if (armOwner == cxxTypeOwnerGroup_.end() || otherOwner == cxxTypeOwnerGroup_.end()
            || resultOwner == cxxTypeOwnerGroup_.end())
            return false;
        const std::string armCv = (CxxTernaryArmConst(armType) ? "const " : "") + armSpelling;
        const std::string otherCv = (CxxTernaryArmConst(otherType) ? "const " : "") + otherSpelling;
        if (resultSlot != nullptr && !refResult && armName != resultName)
        {
            llvm::Value* sourceSlot = arm.Storage;
            NamedVariable converted = arm;
            std::string refusal;
            const size_t armParamTempMark = pendingOwnedStructTemps.size();
            if (ConvertThroughCxxConvertingCtor(converted, resultName, resultSlot, refusal)
                == CxxConvertingCtorResult::Converted)
            {
                if (armCategory == CxxArmCategory::Lvalue && trackConvertedProducer
                    && callerOwnedProducerSlots != nullptr)
                    for (size_t i = armParamTempMark; i < pendingOwnedStructTemps.size(); ++i)
                    {
                        const auto& temp = pendingOwnedStructTemps[i];
                        if (temp.Alloca == nullptr || ResolveTypeAlias(temp.TypeName) != armName)
                            continue;
                        if (std::find(callerOwnedProducerSlots->begin(),
                                callerOwnedProducerSlots->end(), temp.Alloca)
                            == callerOwnedProducerSlots->end())
                            callerOwnedProducerSlots->push_back(temp.Alloca);
                    }
                if (armIsPrvalue && trackConvertedProducer && sourceSlot != nullptr
                    && callerOwnedProducerSlots != nullptr)
                {
                    std::vector<llvm::AllocaInst*> sourceSlots;
                    std::vector<llvm::PHINode*> sourceJoins;
                    auto collectSourceSlots = [&](auto&& self, llvm::Value* value) -> bool {
                        if (auto* slot = llvm::dyn_cast_or_null<llvm::AllocaInst>(value))
                        {
                            if (std::find(sourceSlots.begin(), sourceSlots.end(), slot)
                                == sourceSlots.end())
                                sourceSlots.push_back(slot);
                            return true;
                        }
                        auto* join = llvm::dyn_cast_or_null<llvm::PHINode>(value);
                        if (join == nullptr || !join->getType()->isPointerTy()) return false;
                        if (std::find(sourceJoins.begin(), sourceJoins.end(), join)
                            != sourceJoins.end())
                            return true;
                        sourceJoins.push_back(join);
                        for (llvm::Value* incoming : join->incoming_values())
                            if (!self(self, incoming)) return false;
                        return true;
                    };
                    if (collectSourceSlots(collectSourceSlots, sourceSlot))
                        for (llvm::AllocaInst* slot : sourceSlots)
                            callerOwnedProducerSlots->push_back(slot);
                    else
                        callerOwnedProducerSlots->push_back(sourceSlot);
                }
                arm = std::move(converted);
                arm.TypeAndValue.TypeName = resultName;
                arm.IsRvalue = true;
                if (callSink == nullptr)
                {
                    arm.BaseType = GetType(TypeAndValue{ .TypeName = resultName });
                    return true;
                }

                const std::string sinkName = std::format("__cflat_ternary_sink_{:016x}",
                    HashWrapperKey(resultSpelling + "|direct|" + armCv + "|"
                        + callReturnSpelling + (callSinkEnvironment != nullptr ? "|env" : "")
                        + (callSinkTakesReference ? "|ref" : "")));
                const std::string sinkArgs = callSinkEnvironment != nullptr
                    ? "void * sink, void * call_env, " : "void * sink, ";
                const std::string callback = "reinterpret_cast<" + callReturnSpelling + " (*) ("
                    + resultSpelling + " &&" + (callSinkEnvironment != nullptr ? ", void *" : "")
                    + ")>(sink)(static_cast<" + resultSpelling + " &&>(*self)"
                    + (callSinkEnvironment != nullptr ? ", call_env" : "") + ")";
                const std::string finish = callReturnSpelling == "void"
                    ? callback + "; guard.value = nullptr; return;"
                    : "auto result = " + callback + "; guard.value = nullptr; return result;";
                const std::string source = "#include <memory>\nextern \"C\" __attribute__((weak)) "
                    + callReturnSpelling + " " + sinkName + "(" + sinkArgs + resultSpelling
                    + " * self) { struct Guard { " + resultSpelling
                    + " * value; ~Guard() { if (value != nullptr) std::destroy_at(value); } }; "
                    + "Guard guard{ self }; " + finish + " }\n";
                CxxRequestGroup group = MakeCxxClassRequestGroup(resultOwner->second, resultName);
                if (group.headers.empty()) return false;
                CxxRequestGroupScope groupScope(*this, &group);
                CSigEntry signature;
                std::string error;
                if (!RequestGeneratedCxxWrapper(group, source, sinkName, "TERNARY_SINK",
                                                 signature, error))
                    return false;
                signature.name = sinkName;
                RegisterCSignatures({ signature }, group.headers.front());
                llvm::Function* function = GetFunction(sinkName);
                if (function == nullptr) return false;
                std::vector<llvm::Value*> wrapperArgs{ callSink };
                if (callSinkEnvironment != nullptr)
                    wrapperArgs.push_back(callSinkEnvironment);
                wrapperArgs.push_back(resultSlot);
                arm.Primary = CreateCallOrInvoke(function->getFunctionType(), function,
                    wrapperArgs, true, callReturnSpelling == "void" ? "" : "cxx.ternary.arg.call");
                arm.Storage = resultSlot;
                arm.BaseType = arm.Primary != nullptr ? arm.Primary->getType() : nullptr;
                return arm.Primary != nullptr;
            }
        }
        /*
         * A prvalue arm converted into a prvalue result keeps guaranteed elision: when its producer
         * is one call returning into the arm's slot (sret), that call moves into a thunk and clang
         * calls the thunk inside the conversion, so the produced object is the converted parameter.
         */
        // Every writer of the arm slot: one per path of a nested `?:` arm, each an sret call or an
        // initializer taking the slot once (a constructor expression) with its result unused.
        struct Producer { llvm::CallBase* call; unsigned slotArg; bool sret; };
        std::vector<Producer> producers;
        // A nested arm whose paths built into different slots joins them with a storage PHI.
        // Deeper nesting joins those PHIs again; every leaf slot is collected through them.
        std::vector<llvm::AllocaInst*> producerSlots;
        std::vector<llvm::PHINode*> storageJoins;
        auto collectSlots = [&](auto&& self, llvm::Value* value) -> bool {
            if (auto* slot = llvm::dyn_cast_or_null<llvm::AllocaInst>(value))
            {
                if (std::find(producerSlots.begin(), producerSlots.end(), slot) == producerSlots.end())
                    producerSlots.push_back(slot);
                return true;
            }
            auto* join = llvm::dyn_cast_or_null<llvm::PHINode>(value);
            if (join == nullptr || !join->getType()->isPointerTy()) return false;
            if (std::find(storageJoins.begin(), storageJoins.end(), join) != storageJoins.end())
                return true;
            storageJoins.push_back(join);
            for (llvm::Value* incoming : join->incoming_values())
                if (!self(self, incoming)) return false;
            return true;
        };
        if (!collectSlots(collectSlots, arm.Storage)) producerSlots.clear();
        if (trackConvertedProducer)
            if (auto* direct = llvm::dyn_cast_or_null<llvm::AllocaInst>(arm.Storage);
                direct != nullptr
                && std::find(producerSlots.begin(), producerSlots.end(), direct) == producerSlots.end())
                producerSlots.push_back(direct);
        auto isStorageJoin = [&](llvm::User* user) {
            return std::find(storageJoins.begin(), storageJoins.end(), user) != storageJoins.end();
        };
        if (armIsPrvalue && !refResult)
            for (llvm::AllocaInst* slot : producerSlots)
            {
                bool clean = true;
                for (llvm::User* user : slot->users())
                {
                    if (llvm::isa<llvm::LoadInst>(user) || isStorageJoin(user)) continue;
                    // A nested arm's join zero-fills the shared slot before either path runs.
                    if (auto* store = llvm::dyn_cast<llvm::StoreInst>(user);
                        store != nullptr && store->getPointerOperand() == slot
                        && llvm::isa<llvm::Constant>(store->getValueOperand()))
                        continue;
                    auto* call = llvm::dyn_cast<llvm::CallBase>(user);
                    if (call == nullptr || call->getFunction() != builder->GetInsertBlock()->getParent())
                    {
                        clean = false;
                        break;
                    }
                    if (call->isLifetimeStartOrEnd()) continue;
                    unsigned uses = 0;
                    for (unsigned n = 0; n < call->arg_size(); ++n)
                    {
                        if (call->getArgOperand(n) != slot) continue;
                        const bool sret = call->paramHasAttr(n, llvm::Attribute::StructRet);
                        if (++uses > 1 || (!sret && !call->use_empty())) clean = false;
                        else producers.push_back({ call, n, sret });
                    }
                    if (uses == 0) clean = false;
                }
                if (!clean)
                {
                    producers.clear();
                    break;
                }
            }
        bool needsReturnProbe = false;
        for (const Producer& producer : producers) needsReturnProbe |= !producer.sret;
        // An initializer producer becomes `A f(void *)`; only valid when clang returns A indirectly.
        if (needsReturnProbe)
        {
            const std::string probeName = std::format("__cflat_ternary_retabi_{:016x}",
                HashWrapperKey(armCv + "|ternary-return-abi"));
            const std::string probeSource = "extern \"C\" __attribute__((weak)) " + armCv + " "
                + probeName + "(void * p0) { return reinterpret_cast<" + armCv
                + " (*)(void *)>(p0)(nullptr); }\n";
            CxxRequestGroup probeGroup = MakeCxxClassRequestGroup(armOwner->second, armName);
            CSigEntry probeSignature;
            std::string probeError;
            bool indirect = false;
            if (!probeGroup.headers.empty())
            {
                CxxRequestGroupScope probeScope(*this, &probeGroup);
                indirect = RequestGeneratedCxxWrapper(probeGroup, probeSource, probeName,
                        "TERNARY_RETURN_ABI", probeSignature, probeError)
                    && probeSignature.abi.valid
                    && probeSignature.abi.ret.kind == cflat_cinterop::RawAbiSlot::Indirect
                    && !probeSignature.abi.ret.sretAfterThis;
            }
            if (!indirect) producers.clear();
        }
        // MS sink wrapper (no result slot, callee-destroyed B): re-call the producer in the
        // wrapper so A is elided into B's parameter; a caller-owned A would be moved (extra move).
        const bool callerOwnedProducer = trackConvertedProducer && !producerSlots.empty()
            && (resultSlot != nullptr
                || (callSink != nullptr && !IsCxxParamDestroyedInCallee(resultName)))
            && armIsPrvalue && !refResult && armName != resultName;
        if (callerOwnedProducer && callerOwnedProducerSlots != nullptr)
            for (llvm::AllocaInst* slot : producerSlots)
                callerOwnedProducerSlots->push_back(slot);
        // A discarded result is the whole full-expression in clang: temporaries die in its order.
        const bool thunkArm = !producers.empty() && !callerOwnedProducer;
        const bool discardInClang = thunkArm && discardResult;
        // The never-taken arm is a null dereference: a constant condition never evaluates it.
        // Lvalue arms stay lvalues; xvalue and (modelled) prvalue arms are rvalue-reference casts.
        const std::string taken = thunkArm
            ? "reinterpret_cast<" + armCv + " (*)(void *)>(p0)(p1)"
            : armCategory != CxxArmCategory::Lvalue
            ? "static_cast<" + armCv + " &&>(*p0)" : std::string("*p0");
        const std::string other = otherCategory != CxxArmCategory::Lvalue
            ? "static_cast<" + otherCv + " &&>(*(" + otherCv + " *)nullptr)"
            : "*(" + otherCv + " *)nullptr";
        const std::string conditional = std::string("(") + (armIsTrue ? "true" : "false") + " ? "
            + (armIsTrue ? taken : other) + " : " + (armIsTrue ? other : taken) + ")";
        const std::string wrapperKey = resultSpelling + "|" + armCv + "|" + otherCv + "|"
            + (armIsTrue ? "t" : "f") + CxxArmCategoryTag(armCategory) + "|"
            + CxxArmCategoryTag(otherCategory)
            + (refResult ? "r" : "v") + (thunkArm ? "k" : "") + (discardInClang ? "d" : "")
            + (callSink != nullptr ? "|call|" + callReturnSpelling
                + (callSinkEnvironment != nullptr ? "|env" : "")
                + (callSinkTakesReference ? "|ref" : "")
                + (resultSlot != nullptr ? "|out" : "") : std::string())
            + "|" + conditional;
        const std::string wrapperName = std::format("__cflat_ternary_arm_{:016x}",
            HashWrapperKey(wrapperKey));
        const std::string prologue = "#include <memory>\n#include <new>\n#include <type_traits>\n"
            "extern \"C\" __attribute__((weak)) ";
        const std::string armParams = thunkArm ? std::string("void * p0, void * p1")
                                               : armCv + " * p0";
        std::string source;
        const bool callSinkHasEnvironment = callSink != nullptr && callSinkEnvironment != nullptr;
        const std::string sinkParams = callSinkHasEnvironment
            ? "void * sink, void * call_env, " : "void * sink, ";
        const std::string sinkEnvironmentArg = callSinkHasEnvironment ? ", call_env" : "";
        // A sink taking a reference never destroys B: the wrapper's temporary does, after the call.
        if (callSink != nullptr && resultSlot == nullptr && !callSinkTakesReference
            && IsCxxParamDestroyedInCallee(resultName))
        {
            // MS ABI: callee destroys the by-value parameter; B lives in caller-owned storage.
            // One full-expression keeps the converted-from arm alive through the call.
            const std::string callback = "reinterpret_cast<" + callReturnSpelling + " (*)("
                + resultSpelling + " &&" + (callSinkHasEnvironment ? ", void *" : std::string())
                + ")>(sink)(static_cast<" + resultSpelling + " &&>(*new (storage) "
                + resultSpelling + "(" + conditional + "))" + sinkEnvironmentArg + ")";
            source = prologue + callReturnSpelling + " " + wrapperName + "(" + sinkParams
                + armParams + ") { alignas(" + resultSpelling + ") unsigned char storage[sizeof("
                + resultSpelling + ")]; return " + callback + "; }\n";
        }
        else if (callSink != nullptr && resultSlot != nullptr)
        {
            const std::string callback = "reinterpret_cast<" + callReturnSpelling + " (*) ("
                + resultSpelling + " &&" + (callSinkHasEnvironment ? ", void *" : std::string())
                + ")>(sink)(static_cast<" + resultSpelling + " &&>(*self)"
                + sinkEnvironmentArg + ")";
            const std::string finish = callReturnSpelling == "void"
                ? callback + "; guard.value = nullptr; return;"
                : "auto result = " + callback + "; guard.value = nullptr; return result;";
            source = prologue + callReturnSpelling + " " + wrapperName + "(" + sinkParams
                + resultSpelling + " * self, " + armParams + ") { ::new (static_cast<void *>(self)) "
                + resultSpelling + "(" + conditional + "); struct Guard { " + resultSpelling
                + " * value; ~Guard() { if (value != nullptr) std::destroy_at(value); } }; "
                + "Guard guard{ self }; " + finish + " }\n";
        }
        else if (callSink != nullptr)
        {
            const std::string callback = "reinterpret_cast<" + callReturnSpelling + " (*)("
                + resultSpelling + " &&" + (callSinkHasEnvironment ? ", void *" : std::string())
                + ")>(sink)(" + conditional + sinkEnvironmentArg + ")";
            source = prologue + callReturnSpelling + " " + wrapperName + "(" + sinkParams
                + armParams + ") { return " + callback + "; }\n";
        }
        else if (refResult && resultSlot == nullptr)
            source = prologue + resultSpelling + " * " + wrapperName + "(" + armCv + " * p0) { "
                "static_assert(std::is_reference_v<decltype(" + conditional + ")>, \"\"); "
                "auto &&r = " + conditional + "; return const_cast<" + resultSpelling
                + " *>(&r); }\n";
        else if (discardInClang)
            source = prologue + "void " + wrapperName + "(" + armParams + ") { (void)"
                + conditional + "; }\n";
        else
            source = prologue + "void " + wrapperName + "(" + resultSpelling + " * self, "
                + armParams + ") { ::new (static_cast<void *>(self)) " + resultSpelling + "("
                + conditional + "); }\n";
        CxxRequestGroup group = MakeCxxClassRequestGroup(resultOwner->second, resultName,
            { armOwner->second, otherOwner->second });
        if (group.headers.empty()) return false;
        CxxRequestGroupScope groupScope(*this, &group);
        CSigEntry signature;
        std::string error;
        if (!RequestGeneratedCxxWrapper(group, source, wrapperName, "TERNARY_ARM",
                                        signature, error))
            return false;
        signature.name = wrapperName;
        RegisterCSignatures({ signature }, group.headers.front());
        llvm::Function* function = GetFunction(wrapperName);
        if (function == nullptr) return false;
        llvm::Type* resultObject = GetType(TypeAndValue{ .TypeName = resultName });
        if (thunkArm)
        {
            /*
             * Each producer's other operands (and an indirect callee) are stored into the env at the
             * producer's own position, with a path selector when there are several; the thunk
             * rebuilds the selected call with its sret parameter as the destination.
             */
            const bool selected = producers.size() > 1;
            std::vector<llvm::Type*> fieldTypes;
            if (selected) fieldTypes.push_back(builder->getInt32Ty());
            struct Captured { std::vector<llvm::Value*> values; unsigned first; bool direct; };
            std::vector<Captured> captures;
            for (const Producer& producer : producers)
            {
                Captured captured{ {}, static_cast<unsigned>(fieldTypes.size()),
                                   llvm::isa<llvm::Function>(producer.call->getCalledOperand()) };
                for (unsigned n = 0; n < producer.call->arg_size(); ++n)
                    if (n != producer.slotArg) captured.values.push_back(producer.call->getArgOperand(n));
                if (!captured.direct) captured.values.push_back(producer.call->getCalledOperand());
                for (llvm::Value* value : captured.values) fieldTypes.push_back(value->getType());
                captures.push_back(std::move(captured));
            }
            auto* envType = llvm::StructType::get(*context, fieldTypes);
            llvm::Value* env = llvm::ConstantPointerNull::get(builder->getPtrTy());
            if (!fieldTypes.empty())
            {
                env = AllocaAtEntry(envType, nullptr, "cxx.ternary.env");
                for (size_t p = 0; p < producers.size(); ++p)
                {
                    llvm::IRBuilder<> at(producers[p].call);
                    if (selected)
                        at.CreateStore(at.getInt32(static_cast<uint32_t>(p)),
                                       at.CreateStructGEP(envType, env, 0));
                    for (unsigned n = 0; n < captures[p].values.size(); ++n)
                        at.CreateStore(captures[p].values[n],
                                       at.CreateStructGEP(envType, env, captures[p].first + n));
                }
            }
            auto* thunkType = llvm::FunctionType::get(builder->getVoidTy(),
                { builder->getPtrTy(), builder->getPtrTy() }, false);
            auto* thunk = llvm::Function::Create(thunkType, llvm::GlobalValue::InternalLinkage,
                "__cflat_ternary_thunk", module.get());
            if (llvm::Type* armObject = GetType(TypeAndValue{ .TypeName = armName }))
            {
                thunk->addParamAttr(0, llvm::Attribute::getWithStructRetType(*context, armObject));
                thunk->addParamAttr(0, llvm::Attribute::NoAlias);
            }
            auto* entry = llvm::BasicBlock::Create(*context, "entry", thunk);
            auto* done = llvm::BasicBlock::Create(*context, "done", thunk);
            llvm::IRBuilder<>(done).CreateRetVoid();
            llvm::IRBuilder<> head(entry);
            llvm::SwitchInst* select = nullptr;
            if (selected)
                select = head.CreateSwitch(head.CreateLoad(head.getInt32Ty(),
                    head.CreateStructGEP(envType, thunk->getArg(1), 0)), done,
                    static_cast<unsigned>(producers.size()));
            for (size_t p = 0; p < producers.size(); ++p)
            {
                llvm::CallBase* original = producers[p].call;
                auto* block = selected ? llvm::BasicBlock::Create(*context, "path", thunk) : entry;
                if (selected) select->addCase(head.getInt32(static_cast<uint32_t>(p)), block);
                llvm::IRBuilder<> body(block);
                std::vector<llvm::Value*> loaded;
                for (unsigned n = 0; n < captures[p].values.size(); ++n)
                    loaded.push_back(body.CreateLoad(fieldTypes[captures[p].first + n],
                        body.CreateStructGEP(envType, thunk->getArg(1), captures[p].first + n)));
                std::vector<llvm::Value*> args;
                for (unsigned n = 0, next = 0; n < original->arg_size(); ++n)
                    args.push_back(n == producers[p].slotArg ? thunk->getArg(0) : loaded[next++]);
                llvm::CallInst* rebuilt = body.CreateCall(original->getFunctionType(),
                    captures[p].direct ? original->getCalledOperand() : loaded.back(), args);
                rebuilt->setAttributes(original->getAttributes());
                rebuilt->setCallingConv(original->getCallingConv());
                body.CreateBr(done);
            }
            // The produced object now lives and dies in clang's conversion, not in the arm's slot.
            for (llvm::AllocaInst* slot : producerSlots) ReleaseOwnedStructTempInPlace(slot);
            for (const Producer& producer : producers)
            {
                // An invoke leaves its block by its normal edge; the call now runs in the thunk.
                if (auto* invoke = llvm::dyn_cast<llvm::InvokeInst>(producer.call))
                {
                    invoke->getUnwindDest()->removePredecessor(invoke->getParent());
                    llvm::BranchInst::Create(invoke->getNormalDest(), invoke->getIterator());
                }
                producer.call->eraseFromParent();
            }
            // The wrapper (and the producer it now runs) may throw: unwind through this frame's cleanups.
            if (discardInClang)
            {
                CreateCallOrInvoke(function->getFunctionType(), function, { thunk, env }, true);
                llvm::Value* slot = AllocaAtEntry(resultObject, nullptr, "cxx.ternary.discard");
                arm.Storage = slot;
                arm.Primary = llvm::Constant::getNullValue(resultObject);
            }
            else
            {
                if (callSink != nullptr)
                {
                    std::vector<llvm::Value*> wrapperArgs{ callSink };
                    if (callSinkHasEnvironment) wrapperArgs.push_back(callSinkEnvironment);
                    if (resultSlot != nullptr) wrapperArgs.push_back(resultSlot);
                    wrapperArgs.push_back(thunk);
                    wrapperArgs.push_back(env);
                    arm.Primary = CreateCallOrInvoke(function->getFunctionType(), function,
                        wrapperArgs, true, callReturnSpelling == "void" ? "" : "cxx.ternary.arg.call");
                    arm.Storage = resultSlot;
                    arm.BaseType = arm.Primary != nullptr ? arm.Primary->getType() : nullptr;
                    return arm.Primary != nullptr;
                }
                llvm::Value* slot = resultSlot != nullptr ? resultSlot : CreateAlloca(resultObject);
                CreateCallOrInvoke(function->getFunctionType(), function, { slot, thunk, env }, true);
                if (resultSlot == nullptr && IsForeignNontrivialCxxClass(resultName))
                    RegisterOwnedStructTemp(slot, resultName);
                arm.Storage = slot;
                arm.Primary = CreateLoad(resultObject, slot);
            }
        }
        else
        {
            llvm::Value* storage = arm.Storage;
            if (storage == nullptr)
            {
                llvm::Type* valueType = arm.BaseType != nullptr ? arm.BaseType : GetType(armType);
                if (valueType == nullptr || !valueType->isSized()) return false;
                storage = AllocaAtEntry(valueType, nullptr, "cxx.ternary.arm");
                llvm::Value* value = arm.Primary != nullptr ? arm.Primary : LoadArgStorage(arm);
                if (value == nullptr) return false;
                builder->CreateStore(value, storage);
            }
            if (callSink != nullptr)
            {
                std::vector<llvm::Value*> wrapperArgs{ callSink };
                if (callSinkHasEnvironment) wrapperArgs.push_back(callSinkEnvironment);
                if (resultSlot != nullptr) wrapperArgs.push_back(resultSlot);
                wrapperArgs.push_back(storage);
                arm.Primary = CreateCallOrInvoke(function->getFunctionType(), function,
                    wrapperArgs, true, callReturnSpelling == "void" ? "" : "cxx.ternary.arg.call");
                arm.Storage = resultSlot;
                arm.BaseType = arm.Primary != nullptr ? arm.Primary->getType() : nullptr;
                return arm.Primary != nullptr;
            }
            if (refResult && resultSlot == nullptr)
            {
                llvm::Value* pointer = CreateCallOrInvoke(function->getFunctionType(), function,
                    { storage }, true, "cxx.ternary.ref");
                arm.Storage = pointer;
                arm.Primary = CreateLoad(resultObject, pointer);
            }
            else
            {
                llvm::Value* slot = resultSlot != nullptr ? resultSlot : CreateAlloca(resultObject);
                CreateCallOrInvoke(function->getFunctionType(), function, { slot, storage }, true);
                if (resultSlot == nullptr && IsForeignNontrivialCxxClass(resultName))
                    RegisterOwnedStructTemp(slot, resultName);
                arm.Storage = slot;
                arm.Primary = CreateLoad(resultObject, slot);
        }
        }
        arm.BaseType = resultObject;
        arm.TypeAndValue = TypeAndValue{};
        arm.TypeAndValue.TypeName = resultName;
        arm.IsRvalue = resultSlot != nullptr || !refResult;
        return true;
}

/*
 * Whether a `sourceType` xvalue implicitly converts to `[const] pointeeType *`, decided by clang:
 * a wrapper returning the xvalue from a pointer-returning function is copy-initialization, i.e.
 * std::is_convertible<S&&, P*>. Inherited, templated and derived-pointer conversion operators
 * (plus the trailing standard conversion) are all clang's lookup; any rejection means "no".
 */
bool LLVMBackend::HasImplicitClassPointerConversion(const std::string& sourceType,
                                                    const std::string& pointeeType,
                                                    bool pointeeConst)
{
        const std::string key = sourceType + "|" + pointeeType + (pointeeConst ? "|const" : "|");
        if (auto known = cxxXvaluePointerConversions_.find(key);
            known != cxxXvaluePointerConversions_.end())
            return known->second;
        bool& answer = cxxXvaluePointerConversions_[key];
        answer = false;
        std::string sourceSpelling;
        std::string pointeeSpelling;
        if (!IsCxxRecord(sourceType) || !CxxSpellingForCflatType(sourceType, sourceSpelling)
            || !CxxSpellingForCflatType(pointeeType, pointeeSpelling))
            return false;
        auto groupIt = cxxTypeOwnerGroup_.find(sourceType);
        if (groupIt == cxxTypeOwnerGroup_.end()) return false;
        CxxRequestGroup group = MakeCxxClassRequestGroup(groupIt->second, sourceType);
        if (group.headers.empty()) return false;
        const std::string target = (pointeeConst ? "const " : "") + pointeeSpelling + " *";
        const std::string wrapperName = std::format("__cflat_xvptr_{:016x}",
            HashWrapperKey(key + "|xvalue-pointer-conversion|" + sourceSpelling + "|" + target));
        const std::string wrapperSource = "extern \"C\" __attribute__((weak)) " + target + " "
            + wrapperName + "(" + sourceSpelling + " * p0) { return static_cast<" + sourceSpelling
            + " &&>(*p0); }\n";
        CxxRequestGroupScope groupScope(*this, &group);
        CSigEntry signature;
        std::string error;
        answer = RequestGeneratedCxxWrapper(group, wrapperSource, wrapperName,
                                            "XVALUE_POINTER_CONVERSION", signature, error);
        return answer;
}

llvm::Value* LLVMBackend::ConvertCxxXvalueToPointer(const NamedVariable& source,
                                                     const TypeAndValue& pointerType)
{
        const std::string sourceType = source.TypeAndValue.TypeName;
        if (source.TypeAndValue.Pointer || !IsCxxRecord(sourceType)
            || !pointerType.Pointer || pointerType.TypeName.empty())
            return nullptr;
        std::string sourceSpelling;
        std::string pointeeSpelling;
        if (!CxxSpellingForCflatType(sourceType, sourceSpelling)
            || !CxxSpellingForCflatType(pointerType.TypeName, pointeeSpelling))
            return nullptr;
        const bool pointeeConst = pointerType.PointeeConstBit();
        const std::string key = sourceType + "|" + pointerType.TypeName
            + (pointeeConst ? "|const" : "|");
        auto owner = cxxTypeOwnerGroup_.find(sourceType);
        if (owner == cxxTypeOwnerGroup_.end()) return nullptr;
        CxxRequestGroup group = MakeCxxClassRequestGroup(owner->second, sourceType);
        if (group.headers.empty()) return nullptr;
        const std::string target = (pointeeConst ? "const " : "") + pointeeSpelling + " *";
        const std::string wrapperName = std::format("__cflat_xvptr_{:016x}",
            HashWrapperKey(key + "|xvalue-pointer-conversion|" + sourceSpelling + "|" + target));
        const std::string wrapperSource = "extern \"C\" __attribute__((weak)) " + target + " "
            + wrapperName + "(" + sourceSpelling + " * p0) { return static_cast<" + sourceSpelling
            + " &&>(*p0); }\n";
        CxxRequestGroupScope groupScope(*this, &group);
        CSigEntry signature;
        std::string error;
        if (!RequestGeneratedCxxWrapper(group, wrapperSource, wrapperName,
                                        "XVALUE_POINTER_CONVERSION", signature, error))
            return nullptr;
        signature.name = wrapperName;
        RegisterCSignatures({ signature }, group.headers.front());
        llvm::Function* function = GetFunction(wrapperName);
        if (function == nullptr) return nullptr;
        llvm::Value* storage = source.Storage;
        if (storage == nullptr)
        {
            llvm::Type* valueType = source.BaseType != nullptr
                ? source.BaseType : GetType(source.TypeAndValue);
            if (valueType == nullptr || !valueType->isSized()) return nullptr;
            storage = AllocaAtEntry(valueType, nullptr, "cxx.xvalue.pointer");
            llvm::Value* value = source.Primary != nullptr ? source.Primary : LoadArgStorage(source);
            if (value == nullptr) return nullptr;
            builder->CreateStore(value, storage);
        }
        return builder->CreateCall(function, { storage }, "cxx.xvalue.pointer.result");
}

LLVMBackend::CxxConvertingCtorResult LLVMBackend::ConvertThroughCxxConvertingCtor(
        NamedVariable& source, const std::string& targetType, llvm::Value* intoSlot,
        std::string& refusal, bool directInit, bool allowScalarNarrowing)
{
        refusal.clear();
        if (source.Primary == nullptr && source.Storage != nullptr
            && (source.TypeAndValue.Pointer || source.TypeAndValue.TypeName != targetType))
            source.Primary = LoadArgStorage(source);
        if (source.Primary == nullptr) return CxxConvertingCtorResult::NotApplicable;
        // A literal reaches here with only its value; type it the way a call argument is typed.
        if (source.BaseType == nullptr) source.BaseType = source.Primary->getType();
        // Direct-initialization (`T(x)`, a cast): explicit constructors are candidates and clang
        // ranks them, so the wrapper is spelled as the constructor call (literals as const char*).
        if (directInit)
        {
            if (!IsCxxRecord(targetType))
                return CxxConvertingCtorResult::NotApplicable;
            NamedVariable probe = source;
            // Only a literal is const char*; a mutable char* rvalue keeps its spelling.
            if (!source.IsStringLiteral) probe.IsRvalue = false;
            probe.IsTopLevelConst = CxxConstReceiverKind(source) != 0;
            // Keep array shape here so clang can bind array references before applying decay.
            if (!IsCxxFixedArrayArgument(source))
                probe.TypeAndValue = InferImplicitCxxArgumentType(source, *this);
            probe.TypeAndValue.VariableName.clear();
            std::string wrapperName;
            std::string wrapperError;
            if (!RequestCxxVariadicConstructor(targetType, { probe }, wrapperName, wrapperError,
                                               /*copyInit*/ false))
            {
                refusal = wrapperError;
                return refusal.empty() ? CxxConvertingCtorResult::NotApplicable
                                       : CxxConvertingCtorResult::Refused;
            }
            ConstructThroughCxxCtorWrapper(source, probe, targetType, wrapperName, intoSlot);
            return CxxConvertingCtorResult::Converted;
        }
        // A copy-initialized slot is the by-value parameter shape of the call-argument position.
        // When the source has an implicit conversion operator, let clang resolve the whole
        // copy-initialization sequence before the listed constructor fast path can choose one.
        const TypeAndValue argType = InferImplicitCxxArgumentType(source, *this);
        const std::vector<NamedVariable> scalarSource{ source };
        if (!argType.Pointer && IsCxxRecord(targetType)
            && CxxUnnamedEnumExpression(source.Storage) != nullptr
            && HasHiddenScalarCxxConstructor(targetType, { argType }, &scalarSource))
        {
            NamedVariable probe = source;
            probe.TypeAndValue = argType;
            probe.TypeAndValue.VariableName.clear();
            probe.IsRvalue = true;
            std::string wrapperName;
            std::string wrapperError;
            if (RequestCxxVariadicConstructor(targetType, { probe }, wrapperName, wrapperError,
                                              /*copyInit*/ true))
            {
                ConstructThroughCxxCtorWrapper(source, probe, targetType, wrapperName, intoSlot);
                return CxxConvertingCtorResult::Converted;
            }
            refusal = wrapperError;
            return refusal.empty() ? CxxConvertingCtorResult::NotApplicable
                                   : CxxConvertingCtorResult::Refused;
        }
        if (!argType.Pointer && IsCxxRecord(argType.TypeName) && IsCxxRecord(targetType)
            && ResolveTypeAlias(argType.TypeName) != ResolveTypeAlias(targetType))
        {
            NamedVariable probe = source;
            probe.TypeAndValue = argType;
            probe.TypeAndValue.VariableName.clear();
            std::string why;
            std::vector<NamedVariable> arguments{ probe };
            if (SelectCxxConstructorListed(targetType, { argType }, why,
                    /*allowNumericConversions*/ true, &arguments,
                    /*allowExplicit*/ false, allowScalarNarrowing) == nullptr
                && (why == "const class object cannot bind a non-const class reference"
                    || why.find("non-const lvalue reference") != std::string::npos
                    || (CxxConstReceiverKind(probe) != 0
                        && [&]() {
                            const CxxClassInfo* info = GetCxxClassInfo(targetType);
                            if (info == nullptr) return false;
                            return std::any_of(info->constructors.begin(), info->constructors.end(),
                                [](const CxxClassInfo::Structor& candidate) {
                                    return candidate.params.size() > 1
                                        && candidate.params[1].Pointer
                                        && candidate.params[1].IsAlias
                                        && !candidate.params[1].CxxConstRef();
                                });
                        }())))
            {
                std::string wrapperName;
                std::string wrapperError;
                if (!RequestCxxVariadicConstructor(targetType, { probe }, wrapperName,
                                                   wrapperError, /*copyInit*/ true))
                {
                    refusal = wrapperError;
                    // No wrapper text: relay clang's own copy-initialization refusal (lazy, cached).
                    if (refusal.empty())
                        if (std::string clangText = CxxCopyInitRefusal(probe, targetType);
                            !clangText.empty())
                            refusal = "clang: " + clangText;
                    return refusal.empty() ? CxxConvertingCtorResult::NotApplicable
                                           : CxxConvertingCtorResult::Refused;
                }
            }
        }
        // A deleted converting constructor (`Src&&`, `const Src&&`, a template) may win the pick for
        // this source: clang's copy-initialization decides, not CFlat's ranking.
        if (!source.TypeAndValue.Pointer && IsCxxRecord(argType.TypeName)
            && argType.TypeName != targetType && IsCxxRecord(targetType)
            && HasDeletedConvertingCtor(targetType))
        {
            NamedVariable probe = source;
            probe.TypeAndValue = argType;
            probe.TypeAndValue.VariableName.clear();
            probe.IsRvalue = true;
            std::string wrapperName;
            std::string wrapperError;
            if (RequestCxxVariadicConstructor(targetType, { probe }, wrapperName, wrapperError,
                                              /*copyInit*/ true))
            {
                ConstructThroughCxxCtorWrapper(source, probe, targetType, wrapperName, intoSlot);
                return CxxConvertingCtorResult::Converted;
            }
            refusal = wrapperError;
            return CxxConvertingCtorResult::Refused;
        }
        if (!source.TypeAndValue.Pointer && IsCxxRecord(argType.TypeName)
            && HasImplicitClassConversionOperator(argType.TypeName)
            && argType.TypeName != targetType && IsCxxRecord(targetType))
        {
            NamedVariable probe = source;
            probe.TypeAndValue = argType;
            probe.TypeAndValue.VariableName.clear();
            std::string wrapperName;
            std::string wrapperError;
            if (RequestCxxVariadicConstructor(targetType, { probe }, wrapperName, wrapperError,
                                              /*copyInit*/ true))
            {
                ConstructThroughCxxCtorWrapper(source, probe, targetType, wrapperName, intoSlot);
                return CxxConvertingCtorResult::Converted;
            }
            if (!wrapperError.empty())
            {
                refusal = wrapperError;
                return CxxConvertingCtorResult::Refused;
            }
        }

        // A constructor template may be constrained or explicit(bool). Let clang perform
        // copy-initialization before the extracted constructor table can choose a fast path.
        if (!source.TypeAndValue.Pointer && IsCxxRecord(argType.TypeName)
            && argType.TypeName != targetType && IsCxxRecord(targetType))
        {
            if (const auto* info = GetCxxClassInfo(targetType);
                info != nullptr && info->hasCtorTemplate)
            {
                llvm::Type* objectType = GetType(TypeAndValue{ .TypeName = targetType });
                if (objectType == nullptr || !objectType->isSized())
                    return CxxConvertingCtorResult::NotApplicable;
                NamedVariable probe = source;
                probe.TypeAndValue = argType;
                probe.TypeAndValue.VariableName.clear();
                std::string wrapperName;
                std::string wrapperError;
                if (RequestCxxVariadicConstructor(targetType, { probe }, wrapperName, wrapperError,
                                                  /*copyInit*/ true))
                {
                    ConstructThroughCxxCtorWrapper(source, probe, targetType, wrapperName, intoSlot);
                    return CxxConvertingCtorResult::Converted;
                }
                if (!wrapperError.empty())
                {
                    refusal = wrapperError;
                    return CxxConvertingCtorResult::Refused;
                }
            }
        }

        TypeAndValue param;
        param.TypeName = targetType;
        switch (ClassifyCxxImplicitArgument(source, param, /*cxxByValueParam*/ true,
                                            /*allowTemplateClassConversion*/ false,
                                            allowScalarNarrowing))
        {
        case CxxArgConversion::Convertible:
            return MaterializeImplicitCxxClassArgument(source, param, intoSlot,
                                                       /*allowTemplateClassConversion*/ false,
                                                       allowScalarNarrowing)
                ? CxxConvertingCtorResult::Converted : CxxConvertingCtorResult::NotApplicable;
        case CxxArgConversion::ExplicitCtor:
            refusal = DescribeCxxImplicitArgumentBlock(source, param,
                                                       /*cxxByValueParam*/ true);
            return refusal.empty() ? CxxConvertingCtorResult::NotApplicable
                                   : CxxConvertingCtorResult::Refused;
        case CxxArgConversion::NonConstLvalueRef:
            refusal = DescribeCxxImplicitArgumentBlock(source, param, /*cxxByValueParam*/ true);
            return refusal.empty() ? CxxConvertingCtorResult::NotApplicable
                                   : CxxConvertingCtorResult::Refused;
        default:
            break;
        }
        // Two converting constructors of equal rank (plan point 5): name the ambiguity.
        if (source.TypeAndValue.Pointer || !IsForeignCxxClassWithConstructors(targetType))
            return CxxConvertingCtorResult::NotApplicable;
        if (argType.TypeName.empty() || argType.TypeName == targetType)
            return CxxConvertingCtorResult::NotApplicable;
        NamedVariable rankingArgument = source;
        rankingArgument.TypeAndValue = argType;
        rankingArgument.TypeAndValue.VariableName.clear();
        const std::vector<NamedVariable> rankingArguments{ rankingArgument };
        std::string why;
        TryBindRefusedCxxMember(targetType, "__ctor", 1);
        if (SelectCxxConstructor(targetType, { argType }, why, /*allowNumericConversions*/ true,
                                 &rankingArguments, /*allowExplicit*/ false,
                                 allowScalarNarrowing) == nullptr
            && why.starts_with("matches more than one constructor overload"))
        {
            NamedVariable probe = source;
            probe.TypeAndValue = argType;
            probe.TypeAndValue.VariableName.clear();
            // A tie only narrowing creates (`SCL s = 5;` over SCL(char), SCL(long)) is clang's to
            // name, as the call path does. An equal-rank widening tie keeps CFlat's refusal below.
            bool scalarTableTie = false;
            if (const CxxClassInfo* tieInfo = GetCxxClassInfoWithConstructors(targetType);
                tieInfo != nullptr)
                for (const CxxClassInfo::Structor& candidate : tieInfo->constructors)
                    if (candidate.params.size() == 2 && !candidate.isExplicit && !candidate.isDeleted
                        && ClassifyScalarArgConversion(probe, candidate.params[1])
                            == ScalarArgConversion::Narrow)
                        scalarTableTie = true;
            std::string widenWhy;
            if (scalarTableTie && SelectCxxConstructor(targetType, { argType }, widenWhy,
                    /*allowNumericConversions*/ true, nullptr, /*allowExplicit*/ false,
                    /*allowScalarNarrowing*/ false) == nullptr
                && widenWhy.starts_with("matches more than one constructor overload"))
                scalarTableTie = false;
            const bool defaultedCtorTie = HasDefaultedCxxConstructorTie(
                targetType, { argType }, /*allowExplicit*/ false);
            std::string wrapperName;
            std::string wrapperError;
            if ((scalarTableTie || defaultedCtorTie)
                && RequestCxxVariadicConstructor(targetType, { probe }, wrapperName,
                                                                wrapperError, /*copyInit*/ true))
            {
                ConstructThroughCxxCtorWrapper(source, probe, targetType, wrapperName, intoSlot);
                return CxxConvertingCtorResult::Converted;
            }
            if (defaultedCtorTie)
            {
                refusal = wrapperError;
                return CxxConvertingCtorResult::Refused;
            }
            // Clang confirms this tie but omits candidate signatures from the wrapper diagnostic.
            // Keep its other refusals; the typed constructor table below supplies the candidates.
            if (scalarTableTie && !wrapperError.empty()
                && wrapperError.find("ambiguous") == std::string::npos)
            {
                refusal = wrapperError;
                return CxxConvertingCtorResult::Refused;
            }
            const std::string prefix = "matches more than one constructor overload";
            std::string candidates = why.substr(prefix.size());
            if (candidates.starts_with(" (") && candidates.ends_with(")"))
                candidates = candidates.substr(2, candidates.size() - 3);
            refusal = std::format("the implicit conversion from '{}' is ambiguous: C++ class '{}' "
                                  "has more than one converting constructor of equal rank ({}); "
                                  "spell the conversion out as '{}(<argument>)' with a cast argument",
                                  argType.TypeName, DisplayCxxClassName(targetType),
                                  candidates, DisplayCxxClassName(targetType));
            if (!wrapperError.empty()) refusal += "; " + wrapperError;
            return CxxConvertingCtorResult::Refused;
        }
        return CxxConvertingCtorResult::NotApplicable;
}

bool LLVMBackend::EmitLoweredMemberwiseMove(const std::string& typeName, llvm::Type* valueType,
                                            llvm::Value* dest, llvm::Value* src,
                                            const char* context, uint64_t skipBefore,
                                            uint64_t skipAfter)
{
        UnwindPartialScope movedFields(*this);
        if (auto* arrayType = llvm::dyn_cast<llvm::ArrayType>(valueType))
        {
            const size_t arrayMark = unwindPartial_.size();
            for (uint64_t i = 0; i < arrayType->getNumElements(); ++i)
            {
                unwindPartial_.resize(arrayMark);
                NoteUnwindArrayPrefix(dest, arrayType->getElementType(), builder->getInt64(i),
                                      typeName);
                auto* index = builder->getInt64(i);
                auto* dstElem = builder->CreateInBoundsGEP(arrayType, dest,
                                                           { builder->getInt32(0), index });
                auto* srcElem = builder->CreateInBoundsGEP(arrayType, src,
                                                           { builder->getInt32(0), index });
                if (!EmitLoweredMemberwiseMove(typeName, arrayType->getElementType(), dstElem,
                                               srcElem, context))
                    return false;
                unwindPartial_.resize(arrayMark);
                NoteUnwindArrayPrefix(dest, arrayType->getElementType(), builder->getInt64(i + 1),
                                      typeName);
            }
            movedFields.Release();
            return true;
        }
        const bool generated = generatedCxxRecords_.count(typeName) != 0;
        const bool wholeWalk = skipBefore == 0 && skipAfter == UINT64_MAX;
        if (!generated && IsForeignNontrivialCxxClass(typeName))
            return EmitCxxCopyOrMoveConstruct(typeName, dest, src, true, context);
        if (generated && wholeWalk)
            return EmitCxxCopyOrMoveConstruct(typeName, dest, src, true, context);
        if (valueType->isStructTy() && NeedsCxxLoweringType(typeName))
        {
            const auto data = GetDataStructure(typeName);
            auto* structType = llvm::cast<llvm::StructType>(valueType);
            const auto* layout = module->getDataLayout().getStructLayout(structType);
            for (size_t i = 0; i < data.StructFields.size()
                 && i < structType->getNumElements(); ++i)
            {
                const uint64_t offset = layout->getElementOffset((unsigned)i);
                if (offset < skipBefore || offset >= skipAfter) continue;
                const auto& field = data.StructFields[i];
                auto* fieldType = structType->getElementType((unsigned)i);
                auto* dstField = builder->CreateStructGEP(structType, dest, (unsigned)i);
                auto* srcField = builder->CreateStructGEP(structType, src, (unsigned)i);
                const bool memberwise = !field.Pointer && !field.ElemPointer && !field.IsArrayView;
                if (memberwise)
                {
                    if (!EmitLoweredMemberwiseMove(field.TypeName, fieldType, dstField, srcField,
                                                   context))
                        return false;
                    if (field.ConstArraySize > 0)
                    {
                        auto* arrayType = llvm::dyn_cast<llvm::ArrayType>(fieldType);
                        if (arrayType == nullptr) return false;
                        NoteUnwindArrayPrefix(dstField, arrayType->getElementType(),
                            builder->getInt64(arrayType->getNumElements()), field.TypeName);
                    }
                    else
                        NoteUnwindPartial(UnwindPartialEntry::Kind::Slot, dstField,
                                          field.TypeName);
                    continue;
                }
                auto* value = builder->CreateLoad(fieldType, srcField);
                builder->CreateStore(value, dstField);
                builder->CreateStore(llvm::Constant::getNullValue(fieldType), srcField);
            }
            movedFields.Release();
            return true;
        }
        auto* value = builder->CreateLoad(valueType, src);
        builder->CreateStore(value, dest);
        builder->CreateStore(llvm::Constant::getNullValue(valueType), src);
        movedFields.Release();
        return true;
}

bool LLVMBackend::EmitLoweredMemberwiseCopy(const std::string& typeName, llvm::Type* valueType,
                                            llvm::Value* dest, llvm::Value* src,
                                            const char* context)
{
        if (dest == nullptr || src == nullptr || valueType == nullptr) return false;
        if (IsForeignNontrivialCxxClass(typeName))
            return EmitCxxCopyOrMoveConstruct(typeName, dest, src, false, context);
        if (!IsLoweredCFlatOnlyStruct(typeName)) return false;
        const auto data = GetDataStructure(typeName);
        auto* structType = llvm::dyn_cast<llvm::StructType>(valueType);
        if (structType == nullptr || data.StructType == nullptr) return false;

        auto hasExplicitCopy = [&](const std::string& fieldTypeName) {
            return HasCopyOverloadFor(fieldTypeName)
                && !memberwiseCopyCache_.contains(fieldTypeName);
        };
        auto copyWithUserMethod = [&](const std::string& fieldTypeName, llvm::Type* fieldType,
                                      llvm::Value* dst, llvm::Value* src) {
            NamedVariable source;
            source.Storage = src;
            source.BaseType = fieldType;
            source.TypeAndValue.TypeName = fieldTypeName;
            auto* copied = CreateOverloadedFunctionCall("copy", { source });
            if (copied == nullptr) return false;
            if (IsLoweredCFlatOnlyStruct(fieldTypeName)
                && copied == lastLoweredRetValue_ && lastLoweredRetTemp_ != nullptr)
            {
                auto* resultTemp = lastLoweredRetTemp_;
                if (resultTemp != dst) resultTemp->replaceAllUsesWith(dst);
                if (lastLoweredRetTemp_ == resultTemp) lastLoweredRetTemp_ = dst;
                UnregisterOwnedStructTemp(resultTemp);
            }
            else
                builder->CreateStore(copied, dst);
            return true;
        };

        builder->CreateStore(llvm::Constant::getNullValue(valueType), dest);
        UnwindPartialScope copiedFields(*this);
        for (size_t i = 0; i < data.StructFields.size()
             && i < structType->getNumElements(); ++i)
        {
            const auto& field = data.StructFields[i];
            const unsigned element = CxxFieldElementIndex(data, i);
            if (element == std::numeric_limits<unsigned>::max())
            {
                if (!IsForeignNontrivialCxxClass(field.TypeName)) continue;
                auto* dstField = CreateCxxFieldGEP(data, dest, (unsigned)i);
                auto* srcField = CreateCxxFieldGEP(data, src, (unsigned)i);
                const std::string fieldContext = std::format(
                    "in synthesized copy of struct '{}', field '{}'", typeName,
                    field.VariableName);
                if (!EmitCxxCopyOrMoveConstruct(field.TypeName, dstField, srcField, false,
                                                fieldContext.c_str(), {}, false,
                                                /*directInit*/ true))
                    return false;
                NoteUnwindPartial(UnwindPartialEntry::Kind::Slot, dstField, field.TypeName);
                continue;
            }

            auto* fieldType = structType->getElementType(element);
            auto* dstField = builder->CreateStructGEP(structType, dest, element);
            auto* srcField = builder->CreateStructGEP(structType, src, element);
            if (field.Pointer || field.ElemPointer || field.IsArrayView || field.IsSimd
                || field.IsBitfield || field.IsPadding)
            {
                builder->CreateStore(builder->CreateLoad(fieldType, srcField), dstField);
                continue;
            }
            if (field.ConstArraySize > 0)
            {
                auto* arrayType = llvm::dyn_cast<llvm::ArrayType>(fieldType);
                if (arrayType == nullptr) return false;
                auto* elemType = arrayType->getElementType();
                for (uint64_t n = 0; n < arrayType->getNumElements(); ++n)
                {
                    UnwindPartialScope builtElements(*this);
                    NoteUnwindArrayPrefix(dstField, elemType, builder->getInt64(n),
                                          field.TypeName);
                    llvm::Value* indices[] = { builder->getInt32(0), builder->getInt64(n) };
                    auto* dstElem = builder->CreateInBoundsGEP(arrayType, dstField, indices);
                    auto* srcElem = builder->CreateInBoundsGEP(arrayType, srcField, indices);
                    const std::string fieldContext = std::format(
                        "in synthesized copy of struct '{}', field '{}'", typeName,
                        field.VariableName);
                    if (IsForeignNontrivialCxxClass(field.TypeName))
                    {
                        if (!EmitCxxCopyOrMoveConstruct(field.TypeName, dstElem, srcElem,
                                                         false, fieldContext.c_str(), {}, false,
                                                /*directInit*/ true))
                            return false;
                    }
                    else if (hasExplicitCopy(field.TypeName))
                    {
                        if (!copyWithUserMethod(field.TypeName, elemType, dstElem, srcElem))
                            return false;
                    }
                    else if (IsLoweredCFlatOnlyStruct(field.TypeName))
                    {
                        if (!EmitLoweredMemberwiseCopy(field.TypeName, elemType, dstElem,
                                                       srcElem, fieldContext.c_str()))
                            return false;
                    }
                    else if (HasCopyOverloadFor(field.TypeName)
                             || IsOwningValueType(field.TypeName))
                    {
                        NamedVariable elem;
                        elem.Storage = srcElem;
                        elem.BaseType = elemType;
                        elem.TypeAndValue.TypeName = field.TypeName;
                        auto* copied = CreateOverloadedFunctionCall("copy", { elem });
                        if (copied == nullptr) return false;
                        builder->CreateStore(copied, dstElem);
                    }
                    else
                        builder->CreateStore(builder->CreateLoad(elemType, srcElem), dstElem);
                }
                NoteUnwindArrayPrefix(dstField, elemType,
                                      builder->getInt64(arrayType->getNumElements()),
                                      field.TypeName);
                continue;
            }

            const std::string fieldContext = std::format(
                "in synthesized copy of struct '{}', field '{}'", typeName,
                field.VariableName);
            if (IsForeignNontrivialCxxClass(field.TypeName))
            {
                if (!EmitCxxCopyOrMoveConstruct(field.TypeName, dstField, srcField, false,
                                                 fieldContext.c_str(), {}, false,
                                                /*directInit*/ true))
                    return false;
                NoteUnwindPartial(UnwindPartialEntry::Kind::Slot, dstField, field.TypeName);
            }
            else if (hasExplicitCopy(field.TypeName))
            {
                if (!copyWithUserMethod(field.TypeName, fieldType, dstField, srcField))
                    return false;
                NoteUnwindPartial(UnwindPartialEntry::Kind::Slot, dstField, field.TypeName);
            }
            else if (IsLoweredCFlatOnlyStruct(field.TypeName))
            {
                if (!EmitLoweredMemberwiseCopy(field.TypeName, fieldType, dstField, srcField,
                                               fieldContext.c_str()))
                    return false;
                NoteUnwindPartial(UnwindPartialEntry::Kind::Slot, dstField, field.TypeName);
            }
            else if (HasCopyOverloadFor(field.TypeName) || IsOwningValueType(field.TypeName))
            {
                NamedVariable source;
                source.Storage = srcField;
                source.BaseType = fieldType;
                source.TypeAndValue.TypeName = field.TypeName;
                auto* copied = CreateOverloadedFunctionCall("copy", { source });
                if (copied == nullptr) return false;
                builder->CreateStore(copied, dstField);
                NoteUnwindPartial(UnwindPartialEntry::Kind::Slot, dstField, field.TypeName);
            }
            else
                builder->CreateStore(builder->CreateLoad(fieldType, srcField), dstField);
        }
        copiedFields.Release();
        return true;
}

/*
 * Assignment into a live lowered struct, C++ style: each C++ field runs its own operator=, a
 * nested lowered struct recurses, an owning CFlat value field drops its old value and takes a
 * copy (or the moved value), plain fields copy bitwise. A struct with a user destructor or a
 * `unique` field cannot be assigned field by field: the old value is destroyed and the new one
 * constructed in place, skipped at runtime when the source is the destination itself.
 */
bool LLVMBackend::EmitLoweredMemberwiseAssign(const std::string& typeName, llvm::Type* valueType,
        llvm::Value* dest, llvm::Value* src, bool useMove, const char* context)
{
        if (dest == nullptr || src == nullptr || valueType == nullptr) return false;
        auto cxxAssign = [&](const std::string& cxxType, llvm::Value* dst, llvm::Value* from,
                             const std::string& fieldName) -> bool {
            EnsureCxxMemberProjected(cxxType, "operator=");
            const CxxClassInfo* info = GetCxxClassInfo(cxxType);
            const CxxClassInfo::Structor* op = nullptr;
            if (info != nullptr)
            {
                const auto& overloads = useMove && !info->moveAssignOverloads.empty()
                    ? info->moveAssignOverloads : info->copyAssignOverloads;
                if (!overloads.empty()) op = &overloads.front();
                if (op == nullptr)
                {
                    if (useMove && info->hasMoveAssign) op = &info->moveAssign;
                    else if (info->hasCopyAssign) op = &info->copyAssign;
                }
            }
            if (op != nullptr && op->isDeleted)
                LogError("clang: overload resolution selected deleted operator '='");
            // Clang's implicit assignment is trivial and not deleted (no symbol): a byte copy,
            // as EmitCxxCopyOrMoveConstruct byte-copies a trivial implicit copy constructor.
            if (op == nullptr && info != nullptr
                && (info->hasTrivialCopyAssign || (useMove && info->hasTrivialMoveAssign)))
            {
                auto* recordType = GetType(TypeAndValue{ .TypeName = cxxType });
                if (recordType != nullptr && recordType->isSized())
                {
                    const auto& layout = module->getDataLayout();
                    builder->CreateMemCpy(dst, layout.getABITypeAlign(recordType), from,
                                          layout.getABITypeAlign(recordType),
                                          layout.getTypeAllocSize(recordType));
                    return true;
                }
            }
            if (op == nullptr)
            {
                LogError(std::format("cannot {}-assign struct '{}': field '{}' of C++ class '{}' "
                    "has no usable assignment operator", useMove ? "move" : "copy", typeName,
                    fieldName, DisplayCxxClassName(cxxType)));
                return false;
            }
            return EmitCxxStructorCall(cxxType, *op, dst, { from });
        };
        if (IsForeignNontrivialCxxClass(typeName)) return cxxAssign(typeName, dest, src, "");
        auto* structType = llvm::dyn_cast<llvm::StructType>(valueType);
        auto data = dataStructures.find(typeName);
        if (structType == nullptr || data == dataStructures.end()) return false;
        const auto& sd = data->second;

        bool rebuild = sd.Destructor != nullptr;
        for (const auto& field : sd.StructFields)
            if (field.IsUnique && !field.IsAlias) rebuild = true;
        if (rebuild)
        {
            auto* doRebuild = CreateBasicBlock("lowered.assign.rebuild");
            auto* done = CreateBasicBlock("lowered.assign.done");
            builder->CreateCondBr(builder->CreateICmpEQ(dest, src), done, doRebuild);
            SwitchToBlock(doRebuild);
            if (auto* dtor = GetOrCreateFullDestructor(typeName))
                builder->CreateCall(dtor->getFunctionType(), dtor, { dest });
            const bool built = useMove
                ? EmitLoweredMemberwiseMove(typeName, valueType, dest, src, context)
                : EmitLoweredMemberwiseCopy(typeName, valueType, dest, src, context);
            if (!built) return false;
            if (IsInsertBlockLive()) CreateJump(done);
            SwitchToBlock(done);
            return true;
        }

        // One slot of a field: a scalar field, or one element of a fixed-array field.
        auto assignSlot = [&](const LLVMBackend::TypeAndValue& field, llvm::Type* slotType,
                              llvm::Value* dst, llvm::Value* from) -> bool {
            if (IsForeignNontrivialCxxClass(field.TypeName))
                return cxxAssign(field.TypeName, dst, from, field.VariableName);
            if (IsLoweredCFlatOnlyStruct(field.TypeName))
                return EmitLoweredMemberwiseAssign(field.TypeName, slotType, dst, from, useMove,
                                                   context);
            llvm::Function* dtor = slotType->isStructTy() && IsDataStructure(field.TypeName)
                ? GetOrCreateFullDestructor(field.TypeName) : nullptr;
            if (dtor == nullptr)
            {
                builder->CreateStore(builder->CreateLoad(slotType, from), dst);
                return true;
            }
            llvm::Value* value = nullptr;
            if (useMove)
            {
                value = builder->CreateLoad(slotType, from);
                builder->CreateStore(llvm::Constant::getNullValue(slotType), from);
            }
            else
            {
                NamedVariable source;
                source.Storage = from;
                source.BaseType = slotType;
                source.TypeAndValue.TypeName = field.TypeName;
                value = CreateOverloadedFunctionCall("copy", { source });
                if (value == nullptr) return false;
            }
            builder->CreateCall(dtor->getFunctionType(), dtor, { dst });
            builder->CreateStore(value, dst);
            return true;
        };

        for (size_t i = 0; i < sd.StructFields.size(); ++i)
        {
            const auto& field = sd.StructFields[i];
            const unsigned element = CxxFieldElementIndex(sd, (unsigned)i);
            if (element == std::numeric_limits<unsigned>::max()
                && !IsForeignNontrivialCxxClass(field.TypeName))
                continue;
            if (element != std::numeric_limits<unsigned>::max()
                && element >= structType->getNumElements())
                continue;
            auto* fieldType = element == std::numeric_limits<unsigned>::max()
                ? GetType(field) : structType->getElementType(element);
            auto* dstField = CreateCxxFieldGEP(sd, dest, (unsigned)i);
            auto* srcField = CreateCxxFieldGEP(sd, src, (unsigned)i);
            if (field.Pointer || field.ElemPointer || field.IsArrayView || field.IsSimd
                || field.IsBitfield || field.IsPadding || field.IsAlias)
            {
                builder->CreateStore(builder->CreateLoad(fieldType, srcField), dstField);
                continue;
            }
            if (auto* arrayType = llvm::dyn_cast<llvm::ArrayType>(fieldType);
                arrayType != nullptr && field.ConstArraySize > 0)
            {
                for (uint64_t n = 0; n < arrayType->getNumElements(); ++n)
                {
                    llvm::Value* indices[] = { builder->getInt32(0), builder->getInt64(n) };
                    if (!assignSlot(field, arrayType->getElementType(),
                                    builder->CreateInBoundsGEP(arrayType, dstField, indices),
                                    builder->CreateInBoundsGEP(arrayType, srcField, indices)))
                        return false;
                }
                continue;
            }
            if (!assignSlot(field, fieldType, dstField, srcField)) return false;
        }
        return true;
}

llvm::Function* LLVMBackend::FindCxxWrapperFunction(const std::string& wrapperName)
{
        if (auto symbols = functionTable.find(wrapperName); symbols != functionTable.end())
            for (const FunctionSymbol& symbol : symbols->second)
                if (symbol.External && symbol.UniqueName == wrapperName && symbol.Function != nullptr)
                    return symbol.Function;
        return nullptr;
}

bool LLVMBackend::RequestSameClassCopy(const std::string& typeName, const NamedVariable& source,
                                       bool constSource, SameClassCopyRequest& out,
                                       std::string& error)
{
        error.clear();
        out = {};
        const std::string key = typeName + (constSource ? "|const" : "|lvalue");
        if (auto cached = sameClassCopyRequests_.find(key); cached != sameClassCopyRequests_.end())
        {
            // A memoized wrapper whose module function is gone is requested again.
            if (!cached->second.requested || FindCxxWrapperFunction(cached->second.wrapperName) != nullptr)
            {
                out = cached->second;
                return true;
            }
            sameClassCopyRequests_.erase(cached);
        }
        std::string wrapperName;
        if (!RequestCxxVariadicConstructor(typeName, { source }, wrapperName, error, /*copyInit*/ true))
        {
            if (!error.empty()) return false;
            sameClassCopyRequests_[key] = out;
            return true;
        }
        out.requested = true;
        out.wrapperName = wrapperName;
        if (auto signatures = cxxFunctionSignatures_.find(wrapperName);
            signatures != cxxFunctionSignatures_.end() && !signatures->second.empty())
            out.pick = signatures->second.front().ctorPick;
        sameClassCopyRequests_[key] = out;
        return true;
}

bool LLVMBackend::EmitCxxCopyOrMoveConstruct(const std::string& typeName, llvm::Value* dest,
                                             llvm::Value* src, bool useMove, const char* context,
                                             const std::string& displayTypeName,
                                             bool constSource, bool directInit)
{
        if (dest == nullptr || src == nullptr) return false;
        if (IsLoweredCFlatOnlyStruct(typeName))
        {
            auto* valueType = GetType(TypeAndValue{ .TypeName = typeName });
            return useMove
                ? EmitLoweredMemberwiseMove(typeName, valueType, dest, src, context)
                : EmitLoweredMemberwiseCopy(typeName, valueType, dest, src, context);
        }
        // C++ falls back from move to COPY construction when the class declares no move
        // constructor. It never falls back the other way: silently moving where a copy was
        // written would leave the source consumed behind the user's back.
        NamedVariable sourceObject;
        sourceObject.Storage = src;
        sourceObject.TypeAndValue.TypeName = typeName;
        constSource = constSource || CxxConstReceiverKind(sourceObject) != 0;
        sourceObject.IsTopLevelConst = constSource;
        const CxxClassInfo* implicitInfo = GetCxxClassInfo(typeName);
        if (!useMove && !constSource && implicitInfo != nullptr
            && implicitInfo->hasCtorTemplate && implicitInfo->hasTrivialCopyCtor
            && !implicitInfo->hasDeletedCopyCtor
            && cxxTriviallyCopyableRecords_.count(typeName) != 0)
        {
            SameClassCopyRequest request;
            std::string wrapperError;
            if (!RequestSameClassCopy(typeName, sourceObject, constSource, request, wrapperError))
            {
                LogError(std::format(
                    "cannot copy C++ class '{}' {}: clang could not resolve its same-class "
                    "constructor",
                    displayTypeName.empty() ? DisplayCxxClassName(typeName) : displayTypeName,
                    context));
                return false;
            }
            if (request.requested && request.pick != cflat_cinterop::CtorPick::CopyOrMove)
            {
                llvm::Function* wrapper = FindCxxWrapperFunction(request.wrapperName);
                if (wrapper == nullptr)
                {
                    LogError(std::format(
                        "cannot copy C++ class '{}' {}: clang selected a constructor but its "
                        "generated wrapper could not be called",
                        displayTypeName.empty() ? DisplayCxxClassName(typeName) : displayTypeName,
                        context));
                    return false;
                }
                builder->CreateCall(wrapper->getFunctionType(), wrapper, { dest, src });
                return true;
            }
        }
        const CxxClassInfo::Structor* ctor = useMove ? FindCxxMoveCtor(typeName) : nullptr;
        if (ctor == nullptr) ctor = FindCxxCopyCtor(typeName, constSource, directInit);
        // A class first constructed through a generated wrapper (a CFlat closure into
        // std::function) never ran constructor selection: bind its one-argument ctors now.
        if (ctor == nullptr && TryBindRefusedCxxMember(typeName, "__ctor", 1))
        {
            ctor = useMove ? FindCxxMoveCtor(typeName) : nullptr;
            if (ctor == nullptr) ctor = FindCxxCopyCtor(typeName, constSource, directInit);
        }
        if (ctor == nullptr && useMove) ctor = TryBindCxxGeneratedMoveCtor(typeName);
        if (ctor == nullptr)
        {
            // An implicit trivial copy constructor (clang's verdict) has no symbol: a byte copy.
            if (implicitInfo != nullptr && !implicitInfo->hasDeletedCopyCtor)
            {
                auto* recordType = GetType(TypeAndValue{ .TypeName = typeName });
                if (implicitInfo->hasTrivialCopyCtor && recordType != nullptr
                    && recordType->isSized())
                {
                    const auto& layout = module->getDataLayout();
                    builder->CreateMemCpy(dest, layout.getABITypeAlign(recordType), src,
                                          layout.getABITypeAlign(recordType),
                                          layout.getTypeAllocSize(recordType));
                    return true;
                }
            }
        }
        if (ctor == nullptr)
        {
            const CxxClassInfo* info = GetCxxClassInfo(typeName);
            // A CFlat struct holding a nontrivial C++ field is not a C++ class: say so.
            if (info == nullptr && dataStructures.contains(typeName))
            {
                LogError(std::format(
                    "cannot {} struct '{}' {}: it holds a C++ field that is not trivially "
                    "relocatable, and member-wise {} of such a struct is not supported - "
                    "pass or hold it by pointer instead",
                    useMove ? "move" : "copy",
                    displayTypeName.empty() ? typeName : displayTypeName, context,
                    useMove ? "move" : "copy"));
                return false;
            }
            const bool hasExplicitCopy = !useMove && !directInit && info != nullptr
                && std::any_of(info->constructors.begin(), info->constructors.end(),
                    [](const CxxClassInfo::Structor& candidate) {
                        return candidate.isCopyCtor && candidate.isExplicit;
                    });
            if (hasExplicitCopy)
            {
                std::string wrapperName;
                std::string wrapperError;
                if (!RequestCxxVariadicConstructor(typeName, { sourceObject }, wrapperName,
                                                   wrapperError, /*copyInit*/ true))
                {
                    if (!wrapperError.empty())
                    {
                        LogError(wrapperError);
                        return false;
                    }
                }
                else if (llvm::Function* wrapper = FindCxxWrapperFunction(wrapperName))
                {
                    builder->CreateCall(wrapper->getFunctionType(), wrapper, { dest, src });
                    return true;
                }
            }
            if (constSource && FindCxxCopyCtor(typeName) != nullptr)
            {
                // The listed copy-ctor set omits some clang-valid one-argument constructors.
                // Ask clang lazily when const-source copy selection found no listed candidate.
                NamedVariable constCopySource = sourceObject;
                constCopySource.IsTopLevelConst = true;
                std::string wrapperName;
                std::string wrapperError;
                if (RequestCxxVariadicConstructor(typeName, { constCopySource }, wrapperName,
                                                  wrapperError, /*copyInit*/ true))
                {
                    llvm::Function* wrapper = nullptr;
                    if (auto symbols = functionTable.find(wrapperName);
                        symbols != functionTable.end())
                        for (const FunctionSymbol& symbol : symbols->second)
                            if (symbol.External && symbol.UniqueName == wrapperName
                                && symbol.Function != nullptr)
                            {
                                wrapper = symbol.Function;
                                break;
                            }
                    if (wrapper != nullptr)
                    {
                        builder->CreateCall(wrapper->getFunctionType(), wrapper, { dest, src });
                        return true;
                    }
                    LogError(std::format(
                        "cannot copy C++ class '{}' {}: clang selected a const-source constructor "
                        "but its generated wrapper could not be called",
                        displayTypeName.empty() ? DisplayCxxClassName(typeName) : displayTypeName,
                        context));
                    return false;
                }
                LogError(std::format(
                    "cannot copy C++ class '{}' {} from a const source: no copy constructor "
                    "accepts a const source",
                    displayTypeName.empty() ? DisplayCxxClassName(typeName) : displayTypeName,
                    context));
                return false;
            }
            const bool deleted = info != nullptr && info->hasDeletedCopyCtor;
            LogError(std::format(
                "cannot {} C++ class '{}' {}: its {} constructor is {} - "
                "pass or hold it by pointer instead",
                useMove ? "move" : "copy",
                displayTypeName.empty() ? DisplayCxxClassName(typeName) : displayTypeName, context,
                useMove ? "move or copy" : "copy",
                deleted ? "deleted" : "not accessible from the imported header"));
            return false;
        }
        if (!EmitCxxStructorCall(typeName, *ctor, dest, { src })) return false;
        return !useMove || ResetMovedStdFunctionSource(typeName, src);
    }

bool LLVMBackend::EmitCxxByValueParamConstruct(const std::string& typeName, llvm::Value* dest,
                                                llvm::Value* src, bool useMove,
                                                const char* context, bool constSource)
{
        if (IsForeignNontrivialCxxClass(typeName))
            return EmitCxxCopyOrMoveConstruct(typeName, dest, src, useMove, context, {},
                                              constSource);
        if (!HasForeignNontrivialCxxField(typeName)) return false;
        if (!useMove)
        {
            if (IsLoweredCFlatOnlyStruct(typeName))
            {
                auto* valueType = GetType(TypeAndValue{ .TypeName = typeName });
                if (!IsCopyableType(typeName))
                    return EmitLoweredMemberwiseMove(typeName, valueType, dest, src,
                                                     "into a by-value parameter");
                if (HasCopyOverloadFor(typeName))
                {
                    NamedVariable source;
                    source.Storage = src;
                    source.BaseType = valueType;
                    source.TypeAndValue.TypeName = typeName;
                    auto* copied = CreateOverloadedFunctionCall("copy", { source });
                    if (copied == nullptr) return false;
                    builder->CreateStore(copied, dest);
                    return true;
                }
                return EmitLoweredMemberwiseCopy(typeName, valueType, dest, src, context);
            }
            auto* copyFn = GetOrCreateMemberwiseCopy(typeName);
            if (copyFn == nullptr) return false;
            auto* copied = builder->CreateCall(copyFn,
                { builder->CreateLoad(GetType(TypeAndValue{ .TypeName = typeName }), src) },
                "cxx.holder.copy");
            builder->CreateStore(copied, dest);
            return true;
        }

        if (IsLoweredCFlatOnlyStruct(typeName))
            return EmitLoweredMemberwiseMove(typeName,
                GetType(TypeAndValue{ .TypeName = typeName }), dest, src, context);

        auto* holderTy = GetType(TypeAndValue{ .TypeName = typeName });
        builder->CreateStore(builder->CreateLoad(holderTy, src), dest);
        std::function<bool(const std::string&, llvm::Value*, llvm::Value*)> moveFields;
        moveFields = [&](const std::string& name, llvm::Value* target, llvm::Value* source) {
            auto it = dataStructures.find(name);
            if (it == dataStructures.end() || it->second.StructType == nullptr) return true;
            for (unsigned i = 0; i < it->second.StructFields.size(); ++i)
            {
                const auto& field = it->second.StructFields[i];
                const unsigned element = CxxFieldElementIndex(it->second, i);
                if (field.Pointer || field.ElemPointer || field.IsArrayView || field.IsSimd
                    || field.IsBitfield || field.IsPadding) continue;
                if (field.ConstArraySize > 0)
                {
                    if (element == std::numeric_limits<unsigned>::max()) continue;
                    auto* arrayTy = llvm::dyn_cast<llvm::ArrayType>(
                        it->second.StructType->getElementType(element));
                    if (arrayTy == nullptr) continue;
                    auto* dstArray = CreateCxxFieldGEP(it->second, target, i);
                    auto* srcArray = CreateCxxFieldGEP(it->second, source, i);
                    for (uint64_t n = 0; n < arrayTy->getNumElements(); ++n)
                    {
                        llvm::Value* indices[] = { builder->getInt32(0),
                            builder->getInt32(static_cast<uint32_t>(n)) };
                        auto* dstElement = builder->CreateInBoundsGEP(arrayTy, dstArray, indices);
                        auto* srcElement = builder->CreateInBoundsGEP(arrayTy, srcArray, indices);
                        if (IsForeignNontrivialCxxClass(field.TypeName))
                        {
                            if (!EmitCxxCopyOrMoveConstruct(field.TypeName, dstElement, srcElement,
                                    true, context)) return false;
                        }
                        else if (HasForeignNontrivialCxxField(field.TypeName)
                            && !moveFields(field.TypeName, dstElement, srcElement)) return false;
                    }
                    continue;
                }
                if (IsForeignNontrivialCxxClass(field.TypeName))
                {
                    auto* dstField = CreateCxxFieldGEP(it->second, target, i);
                    auto* srcField = CreateCxxFieldGEP(it->second, source, i);
                    if (!EmitCxxCopyOrMoveConstruct(field.TypeName, dstField, srcField, true,
                            context)) return false;
                }
                else if (HasForeignNontrivialCxxField(field.TypeName)
                    && !moveFields(field.TypeName,
                        CreateCxxFieldGEP(it->second, target, i),
                        CreateCxxFieldGEP(it->second, source, i))) return false;
            }
            return true;
        };
        return moveFields(typeName, dest, src);
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
        // Itanium `_Znwm` / MS `??2@YAPEAX_K@Z`: operator new(size_t[, std::align_val_t]).
        const char* name = targetWindows_
            ? (overAligned ? "??2@YAPEAX_KW4align_val_t@std@@@Z" : "??2@YAPEAX_K@Z")
            : (overAligned ? "_ZnwmSt11align_val_t" : "_Znwm");
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
        // Itanium `_ZdlPvm` / MS `??3@YAXPEAX_K@Z`: operator delete(void*, size_t[, align_val_t]).
        const char* name = targetWindows_
            ? (overAligned ? "??3@YAXPEAX_KW4align_val_t@std@@@Z" : "??3@YAXPEAX_K@Z")
            : (overAligned ? "_ZdlPvmSt11align_val_t" : "_ZdlPvm");
        if (llvm::Function* existing = module->getFunction(name)) return existing;
        auto* i64 = builder->getInt64Ty();
        auto* ptr = cflat_llvm::PointerTo(builder->getInt8Ty());
        std::vector<llvm::Type*> params{ ptr, i64 };
        if (overAligned) params.push_back(i64);
        auto* fnTy = llvm::FunctionType::get(builder->getVoidTy(), params, false);
        return llvm::Function::Create(fnTy, llvm::GlobalValue::ExternalLinkage, name, module.get());
    }

llvm::Function* LLVMBackend::GetCxxOperatorNewArray(bool overAligned)
{
        // Itanium `_Znam` / MS `??_U@YAPEAX_K@Z`: operator new[](size_t[, std::align_val_t]).
        const char* name = targetWindows_
            ? (overAligned ? "??_U@YAPEAX_KW4align_val_t@std@@@Z" : "??_U@YAPEAX_K@Z")
            : (overAligned ? "_ZnamSt11align_val_t" : "_Znam");
        if (llvm::Function* existing = module->getFunction(name)) return existing;
        auto* i64 = builder->getInt64Ty();
        std::vector<llvm::Type*> params{ i64 };
        if (overAligned) params.push_back(i64);
        auto* fnTy = llvm::FunctionType::get(cflat_llvm::PointerTo(builder->getInt8Ty()), params, false);
        return llvm::Function::Create(fnTy, llvm::GlobalValue::ExternalLinkage, name, module.get());
    }

llvm::Function* LLVMBackend::GetCxxOperatorDeleteArray(bool overAligned)
{
        // Itanium `_ZdaPv` / MS `??_V@YAXPEAX@Z`: operator delete[](void*[, std::align_val_t]).
        // The unsized form: a CFlat array block carries no cookie to recover the size from.
        const char* name = targetWindows_
            ? (overAligned ? "??_V@YAXPEAXW4align_val_t@std@@@Z" : "??_V@YAXPEAX@Z")
            : (overAligned ? "_ZdaPvSt11align_val_t" : "_ZdaPv");
        if (llvm::Function* existing = module->getFunction(name)) return existing;
        std::vector<llvm::Type*> params{ cflat_llvm::PointerTo(builder->getInt8Ty()) };
        if (overAligned) params.push_back(builder->getInt64Ty());
        auto* fnTy = llvm::FunctionType::get(builder->getVoidTy(), params, false);
        return llvm::Function::Create(fnTy, llvm::GlobalValue::ExternalLinkage, name, module.get());
    }

const LLVMBackend::FunctionSymbol* LLVMBackend::FindCxxClassAllocFunction(
    const std::string& typeName, const std::string& opName, bool& sized)
{
        /*
         * C++ name lookup for a class-scope allocation function: the class, then its bases, and
         * the FIRST scope declaring the name wins (a base's operator is inherited). Within that
         * scope the usual forms were the only ones exported: operator new(size_t) and operator
         * delete(void*) / (void*, size_t), where a class-scope delete prefers the unsized one.
         */
        sized = false;
        std::vector<std::string> work{ typeName };
        std::set<std::string> seen;
        for (size_t i = 0; i < work.size(); ++i)
        {
            const std::string cls = work[i];
            if (!seen.insert(cls).second) continue;
            EnsureCxxMemberProjected(cls, opName);
            auto it = functionTable.find(cls + "." + opName);
            if (it != functionTable.end() && !it->second.empty())
            {
                const FunctionSymbol* unsized = nullptr;
                const FunctionSymbol* withSize = nullptr;
                for (const FunctionSymbol& sym : it->second)
                {
                    if (!sym.IsCxx || sym.IsMethod || sym.Function == nullptr) continue;
                    if (sym.Parameters.size() == 1 && unsized == nullptr) unsized = &sym;
                    else if (sym.Parameters.size() == 2 && withSize == nullptr) withSize = &sym;
                }
                if (unsized != nullptr) return unsized;
                sized = withSize != nullptr;
                return withSize;
            }
            if (const CxxClassInfo* info = GetCxxClassInfo(cls))
                for (const CxxClassInfo::BaseRef& b : info->bases)
                    if (!b.isVirtual) work.push_back(b.name);
        }
        return nullptr;
    }

bool LLVMBackend::HasCflatClassAllocFunction(const std::string& typeName, const std::string& opName)
{
        // Projected base members share this table entry; only CFlat-declared ones answer here.
        auto it = functionTable.find(typeName + "." + opName);
        if (it == functionTable.end()) return false;
        for (const FunctionSymbol& sym : it->second)
            if (!sym.IsCxx) return true;
        return false;
    }

bool LLVMBackend::CxxClassUsesCxxAllocator(const std::string& typeName)
{
        if (typeName.empty() || GetCxxClassInfo(typeName) == nullptr) return false;
        if (IsForeignNontrivialCxxClass(typeName)
            || IsForeignCxxClassWithUserDeclaredConstructor(typeName)) return true;
        bool sized = false;
        for (const char* op : { "operator new", "operator delete", "operator new[]", "operator delete[]" })
            if (FindCxxClassAllocFunction(typeName, op, sized) != nullptr) return true;
        return false;
    }

llvm::Value* LLVMBackend::CallCxxClassAllocFunction(const FunctionSymbol& sym,
                                                    std::vector<llvm::Value*> args, bool mayUnwind)
{
        llvm::Function* fn = sym.Function;
        llvm::FunctionType* fty = fn->getFunctionType();
        if (fty->getNumParams() != args.size()) return nullptr;
        for (unsigned i = 0; i < args.size(); ++i)
        {
            llvm::Type* want = fty->getParamType(i);
            if (want->isIntegerTy()) args[i] = builder->CreateZExtOrTrunc(args[i], want);
            else if (want->isPointerTy()) args[i] = builder->CreateBitCast(args[i], want);
        }
        return CreateCallOrInvoke(fty, fn, args, mayUnwind && !sym.IsNoexcept,
                                  fty->getReturnType()->isVoidTy() ? "" : "cxx.clsalloc");
    }

llvm::Value* LLVMBackend::EmitCxxHeapAllocate(const std::string& typeName)
{
        uint64_t size = 0, align = 0;
        if (!CxxObjectSizeAndAlign(typeName, size, align)) return nullptr;
        // A CFlat-declared operator new on the class itself hides the base's: CFlat's call.
        if (HasCflatClassAllocFunction(typeName, "operator new"))
        {
            LLVMBackend::NamedVariable szArg;
            szArg.Primary = builder->getInt64(size);
            szArg.BaseType = szArg.Primary->getType();
            return CreateOverloadedFunctionCall(typeName + ".operator new", { szArg });
        }
        // A class-scope operator new (own or inherited) replaces the global one.
        bool sized = false;
        if (const FunctionSymbol* cls = FindCxxClassAllocFunction(typeName, "operator new", sized))
            return CallCxxClassAllocFunction(*cls, { builder->getInt64(size) }, true);
        const bool overAligned = align > kDefaultNewAlign;
        llvm::Function* fn = GetCxxOperatorNew(overAligned);
        std::vector<llvm::Value*> args{ builder->getInt64(size) };
        if (overAligned) args.push_back(builder->getInt64(align));
        // operator new throws std::bad_alloc through this frame.
        return CreateCallOrInvoke(fn->getFunctionType(), fn, args, /*mayUnwind=*/true, "cxx.new");
    }

void LLVMBackend::EmitCxxHeapFree(const std::string& typeName, llvm::Value* ptr)
{
        uint64_t size = 0, align = 0;
        if (ptr == nullptr || !CxxObjectSizeAndAlign(typeName, size, align)) return;
        auto* voidPtr = builder->CreateBitCast(ptr, cflat_llvm::PointerTo(builder->getInt8Ty()));
        // A CFlat-declared operator delete on the class itself hides the base's C++ ones.
        if (HasCflatClassAllocFunction(typeName, "operator delete"))
        {
            EmitUserNewHeapFree(typeName, voidPtr, builder->getInt64(size));
            return;
        }
        bool sized = false;
        if (const FunctionSymbol* cls = FindCxxClassAllocFunction(typeName, "operator delete", sized))
        {
            std::vector<llvm::Value*> args{ voidPtr };
            if (sized) args.push_back(builder->getInt64(size));
            CallCxxClassAllocFunction(*cls, args, false);
            return;
        }
        const bool overAligned = align > kDefaultNewAlign;
        llvm::Function* fn = GetCxxOperatorDelete(overAligned);
        std::vector<llvm::Value*> args{ voidPtr, builder->getInt64(size) };
        if (overAligned) args.push_back(builder->getInt64(align));
        builder->CreateCall(fn->getFunctionType(), fn, args);
    }

llvm::Value* LLVMBackend::EmitCxxHeapAllocateArray(const std::string& typeName, llvm::Value* bytes,
                                                   uint64_t allocAlign)
{
        uint64_t size = 0, align = 0;
        if (bytes == nullptr || !CxxObjectSizeAndAlign(typeName, size, align)) return nullptr;
        bytes = builder->CreateZExtOrTrunc(bytes, builder->getInt64Ty());
        // `new T[n]` looks up operator new[] only: a class-scope operator new never applies.
        bool sized = false;
        if (const FunctionSymbol* cls = FindCxxClassAllocFunction(typeName, "operator new[]", sized))
            return CallCxxClassAllocFunction(*cls, { bytes }, true);
        align = std::max(align, allocAlign);
        const bool overAligned = align > kDefaultNewAlign;
        llvm::Function* fn = GetCxxOperatorNewArray(overAligned);
        std::vector<llvm::Value*> args{ bytes };
        if (overAligned) args.push_back(builder->getInt64(align));
        return CreateCallOrInvoke(fn->getFunctionType(), fn, args, /*mayUnwind=*/true, "cxx.newarr");
    }

bool LLVMBackend::EmitCxxHeapFreeArray(const std::string& typeName, llvm::Value* ptr,
                                       llvm::Value* count, uint64_t allocAlign)
{
        uint64_t size = 0, align = 0;
        if (ptr == nullptr || !CxxObjectSizeAndAlign(typeName, size, align)) return true;
        auto* voidPtr = builder->CreateBitCast(ptr, cflat_llvm::PointerTo(builder->getInt8Ty()));
        bool sized = false;
        if (const FunctionSymbol* cls = FindCxxClassAllocFunction(typeName, "operator delete[]", sized))
        {
            std::vector<llvm::Value*> args{ voidPtr };
            if (sized)
            {
                // No cookie: the size is the element count times the stride, so it must be known.
                if (count == nullptr) return false;
                args.push_back(builder->CreateMul(
                    builder->CreateZExtOrTrunc(count, builder->getInt64Ty()), builder->getInt64(size)));
            }
            CallCxxClassAllocFunction(*cls, args, false);
            return true;
        }
        align = std::max(align, allocAlign);
        const bool overAligned = align > kDefaultNewAlign;
        llvm::Function* fn = GetCxxOperatorDeleteArray(overAligned);
        std::vector<llvm::Value*> args{ voidPtr };
        if (overAligned) args.push_back(builder->getInt64(align));
        builder->CreateCall(fn->getFunctionType(), fn, args);
        return true;
    }

void LLVMBackend::EmitCxxHeapFreeCounted(const std::string& typeName, llvm::Value* ptr,
                                         llvm::Value* count, uint64_t allocAlign)
{
        // A raw array count < 0 (or none) marks a single object; >= 0 a `new T[n]` block.
        if (count == nullptr) { EmitCxxHeapFree(typeName, ptr); return; }
        count = builder->CreateSExtOrTrunc(count, builder->getInt64Ty());
        if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(count))
        {
            if (c->isNegative()) EmitCxxHeapFree(typeName, ptr);
            else EmitCxxHeapFreeArray(typeName, ptr, count, allocAlign);
            return;
        }
        auto* fn = builder->GetInsertBlock()->getParent();
        auto* arrBB = llvm::BasicBlock::Create(*context, "cxxfree.arr", fn);
        auto* oneBB = llvm::BasicBlock::Create(*context, "cxxfree.one", fn);
        auto* doneBB = llvm::BasicBlock::Create(*context, "cxxfree.done", fn);
        builder->CreateCondBr(builder->CreateICmpSGE(count, builder->getInt64(0)), arrBB, oneBB);
        builder->SetInsertPoint(arrBB);
        EmitCxxHeapFreeArray(typeName, ptr, count, allocAlign);
        builder->CreateBr(doneBB);
        builder->SetInsertPoint(oneBB);
        EmitCxxHeapFree(typeName, ptr);
        builder->CreateBr(doneBB);
        builder->SetInsertPoint(doneBB);
    }

// Scalar targets a conversion operator may name, most-preferred first. 'int' precedes 'u32'
// so a signed spelling wins when two spellings share one LLVM type.
const std::vector<std::string>& LLVMBackend::ScalarConversionSpellings()
{
        static const std::vector<std::string> spellings = {
            "bool", "int", "i64", "u32", "u64", "double", "longdouble", "float", "char",
            "short", "long", "ulong", "u8", "i8", "u16" };
        return spellings;
}

/*
 * Conversion-OPERATOR direction of a user-defined conversion: the source is a C++ (or CFlat
 * struct) value carrying `operator D`, and the destination is an ordinary CFlat type. This is
 * the mirror of CanImplicitlyConstructCxxClass, which converts INTO a C++ class through a
 * CONSTRUCTOR; the two are kept apart deliberately.
 *
 * Returns the registration name of the operator to call ("operator bool", or the
 * "<Type>.operator bool" member spelling), and reports through `needsStandardConversion`
 * whether the result still needs ONE standard conversion to reach `dest` (C++ allows exactly
 * one after a user-defined conversion).
 */
std::string LLVMBackend::CxxConversionOperatorTo(const std::string& sourceTypeName,
                                                 const TypeAndValue& dest,
                                                 bool allowExplicit,
                                                 bool* needsStandardConversion,
                                                 bool* ambiguous,
                                                 std::vector<std::string>* ambiguousCandidates,
                                                 const TypeAndValue* receiver) const
{
        if (needsStandardConversion != nullptr) *needsStandardConversion = false;
        if (ambiguous != nullptr) *ambiguous = false;
        if (sourceTypeName.empty() || dest.TypeName.empty() || dest.TypeName == "void") return {};
        if (sourceTypeName == dest.TypeName) return {};
        const bool classReferenceTarget = dest.IsAlias && IsCxxRecord(dest.TypeName);
        if (dest.ElemPointer
            || dest.IsArrayView || dest.IsInterface
            || dest.IsFunctionPointer)
            return {};
        if (!dest.IsPrimitive() && !dest.Pointer && !classReferenceTarget) return {};
        if (IsPrimitiveTypeName(sourceTypeName)) return {};
        // A CFlat-native `operator T` is EXPLICIT-only by ruling (Test/errors/err_operator_
        // conversion.cb pins it), so only an imported C++ record converts implicitly - there
        // `explicit` is spelled in the source and travels on the member.
        if (!allowExplicit && !IsCxxRecord(sourceTypeName)) return {};

        auto* self = const_cast<LLVMBackend*>(this);
        self->EnsureCxxConversionOperatorsProjected(sourceTypeName);
        const CxxClassInfo* info = GetCxxClassInfo(sourceTypeName);
        if (allowExplicit)
            if (auto record = cxxRecordEntries_.find(sourceTypeName);
                record != cxxRecordEntries_.end())
                for (const auto& member : record->second.members)
                    if (member.isDeleted && member.name == "operator " + dest.TypeName)
                    {
                        std::string sourceSpelling;
                        if (!CxxSpellingForCflatType(sourceTypeName, sourceSpelling))
                            sourceSpelling = DisplayCxxClassName(sourceTypeName);
                        LogError("member 'operator " + dest.TypeName + "' of C++ class '"
                                 + DisplayCxxClassName(sourceTypeName) + "' is deleted (clang: "
                                 + "static_cast from '" + sourceSpelling + "' to '"
                                 + dest.TypeName + "' uses deleted function)");
                    }
        // A class-reference target takes the same conversion-name lookup and explicit filter
        // as every other target (usable below), so only real conversion operators bind.
        // A conversion operator is published either under the bare name or, when the bare name
        // is taken by a free conversion, under "<Type>.operator <spelling>".
        auto candidateNames = [&](const std::string& spelling) {
            if (classReferenceTarget)
            {
                // A reference result is projected in pointer shape ("operator T*"), see
                // EnsureCxxConversionOperatorsProjected.
                const std::string cflatSpelling =
                    SpellType(*this, TypeAndValue{ .TypeName = spelling, .Pointer = true });
                return std::vector<std::string>{ "operator " + cflatSpelling,
                                                 sourceTypeName + ".operator " + cflatSpelling };
            }
            TypeAndValue targetType;
            targetType.TypeName = spelling;
            targetType.Pointer = dest.Pointer;
            const std::string cflatSpelling = dest.Pointer
                ? SpellType(*this, targetType)
                : CanonicalPrimitiveTypeName(ResolveTypeAlias(spelling));
            std::vector<std::string> result{ "operator " + cflatSpelling,
                                             sourceTypeName + ".operator " + cflatSpelling };
            if (!dest.Pointer && !classReferenceTarget)
            {
                TypeAndValue refTarget = targetType;
                refTarget.Pointer = true;
                refTarget.IsAlias = true;
                const std::string refSpelling = SpellType(*this, refTarget);
                result.push_back("operator " + refSpelling);
                result.push_back(sourceTypeName + ".operator " + refSpelling);
            }
            if (auto record = cxxRecordEntries_.find(sourceTypeName);
                record != cxxRecordEntries_.end())
                for (const auto& member : record->second.members)
                {
                    if (!member.isConversion) continue;
                    TypeAndValue ret;
                    if (!self->MapCTypeToTypeAndValue(member.retType, ret, true)
                        || CanonicalPrimitiveTypeName(ResolveTypeAlias(ret.TypeName))
                            != CanonicalPrimitiveTypeName(ResolveTypeAlias(spelling))) continue;
                    const bool referenceResult = member.retType.find('&') != std::string::npos;
                    const bool shapeMatches = classReferenceTarget
                        ? ret.Pointer && referenceResult
                        : dest.Pointer ? ret.Pointer && !referenceResult
                        : !ret.Pointer || referenceResult;
                    if (!shapeMatches) continue;
                    result.push_back(member.name);
                    result.push_back(sourceTypeName + "." + member.name);
                    // A const member registers under its const twin name; a const receiver
                    // reaches it only through this name (listed last: non-const wins otherwise).
                    result.push_back(CxxConstTwinName(member.name));
                }
            // An INHERITED pair has no own member here, but its const twin is cloned onto this
            // class under the bare spelling; list it unconditionally (a miss costs one lookup).
            result.push_back(CxxConstTwinName("operator " + cflatSpelling));
            return result;
        };

        // clang ranks the implicit object parameter BEFORE the return conversion. A const
        // receiver binds only const member conversions; a non-const one prefers a non-const
        // member (rank 0) over a const one (rank 1). CFlat-native sources have no receiver cv.
        const bool receiverKnown = receiver != nullptr && IsCxxRecord(sourceTypeName);
        const bool receiverConst = receiverKnown
            && (receiver->IsConstObject() || receiver->CxxConstRef());
        // One candidate's return type; false when its shape cannot reach dest.
        auto fits = [&](const auto& candidate, std::string& returnTypeName) {
                if (candidate.Parameters.size() != 1) return false;
                const auto& p = candidate.Parameters[0];
                // A C++ member conversion carries a 'this' receiver, so a registered C++ record
                // accepts the pointer shape; the call path adjusts the receiver.
                if ((p.Pointer && !IsCxxRecord(sourceTypeName)) || p.ElemPointer
                    || p.TypeName != sourceTypeName)
                    return false;
                const auto& ret = candidate.ReturnType;
                if (classReferenceTarget)
                {
                    // A const referent (`operator const X&`) never binds a mutable X& or X&&.
                    if (ret.CxxConstRef() && !dest.CxxConstRef()) return false;
                    if (ret.TypeName == dest.TypeName && !ret.ElemPointer
                        && ret.IsAlias)
                    {
                        returnTypeName = ret.TypeName;
                        return true;
                    }
                    return false;
                }
                bool referenceResult = ret.IsAlias;
                if (candidate.IsCxx)
                    if (auto signatures = cxxFunctionSignatures_.find(candidate.SourceName);
                        signatures != cxxFunctionSignatures_.end())
                        for (const auto& raw : signatures->second)
                            if (raw.linkageName == candidate.UniqueName)
                                referenceResult = raw.retSpelling.find('&') != std::string::npos;
                const bool scalarReference = !dest.Pointer && ret.Pointer && referenceResult
                    && CanonicalPrimitiveTypeName(ResolveTypeAlias(ret.TypeName))
                        == CanonicalPrimitiveTypeName(ResolveTypeAlias(dest.TypeName));
                if ((!scalarReference && ret.Pointer != dest.Pointer) || ret.ElemPointer
                    || ret.IsArrayView || (!dest.Pointer && !scalarReference && !ret.IsPrimitive()))
                    return false;
                if (dest.Pointer
                    && CanonicalPrimitiveTypeName(ResolveTypeAlias(ret.TypeName))
                        != CanonicalPrimitiveTypeName(ResolveTypeAlias(dest.TypeName))) return false;
                returnTypeName = ret.TypeName;
                return true;
        };
        auto usable = [&](const std::string& opName, std::string& returnTypeName,
                          int* receiverRank = nullptr) {
            // "<Type>.operator <spelling>" keeps its spelling, which may hold dots ("t104.X").
            const size_t qualifier = opName.find(".operator ");
            if (info != nullptr && !allowExplicit
                && info->explicitConversions.count(qualifier == std::string::npos
                        ? opName : opName.substr(qualifier + 1)) != 0)
                return false;
            auto it = functionTable.find(opName);
            if (it == functionTable.end()) return false;
            bool found = false;
            int bestRank = 2;
            for (const auto& candidate : it->second)
            {
                std::string name;
                if (!fits(candidate, name)) continue;
                int rank = 0;
                if (receiverKnown)
                {
                    const bool thisConst = candidate.CxxConst;
                    if (receiverConst && !thisConst) continue;
                    rank = !receiverConst && thisConst ? 1 : 0;
                }
                if (!found || rank < bestRank)
                {
                    found = true;
                    bestRank = rank;
                    returnTypeName = name;
                }
                if (bestRank == 0) break;
            }
            if (found && receiverRank != nullptr) *receiverRank = bestRank;
            return found;
        };

        // Built-in spellings of ONE C++ type (i32 = int, i16 = short, uint = u32) are the same type.
        auto canonical = [this](const std::string& name) {
            return CanonicalPrimitiveTypeName(ResolveTypeAlias(name));
        };
        const std::string resolvedDestType = canonical(dest.TypeName);
        // Exactness is CFlat type identity after alias resolution, not LLVM representation.
        std::string returnTypeName;
        int exactReceiverRank = 0;
        std::string exactName;
        for (const std::string& name : candidateNames(dest.TypeName))
            if (usable(name, returnTypeName, &exactReceiverRank))
            {
                if (classReferenceTarget) return name;
                if (canonical(returnTypeName) == resolvedDestType)
                {
                    exactName = name;
                    break;
                }
            }
        if (!exactName.empty() && (exactReceiverRank == 0 || classReferenceTarget || dest.Pointer))
            return exactName;

        if (classReferenceTarget) return {};
        // Pointer conversions are exact here. Their standard pointer conversions need a richer
        // ranking than the scalar conversion table below, so do not treat them as scalar casts.
        if (dest.Pointer) return {};

        /*
         * No exact operator: ONE standard conversion may sit on top of the user-defined one.
         * Every conversion function that can reach `dest` that way is an equally ranked
         * candidate, so TWO of them is an ambiguity (clang rejects `double d = bt;` when the
         * class declares both `operator int` and `operator bool`) - collect, never take first.
         */
        if (info == nullptr) return exactName;
        struct StandardViable { std::string name; int rank; int receiverRank; };
        std::vector<StandardViable> viable;
        std::set<std::string> tried;
        for (const std::string& spelling : ScalarConversionSpellings())
        {
            if (ResolveTypeAlias(spelling) == ResolveTypeAlias(dest.TypeName)) continue;
            for (const std::string& name : candidateNames(spelling))
            {
                if (!tried.insert(name).second) continue;
                int candidateReceiverRank = 0;
                if (!usable(name, returnTypeName, &candidateReceiverRank)) continue;
                // Explicit conversion functions cannot supply an intermediate scalar for a cast.
                const size_t qualifier = name.find(".operator ");
                if (info->explicitConversions.count(qualifier == std::string::npos
                        ? name : name.substr(qualifier + 1)) != 0)
                    continue;
                int rank = 2;
                const std::string resolvedReturnType = canonical(returnTypeName);
                if (resolvedReturnType == resolvedDestType)
                    rank = 0;
                else if (resolvedDestType == "int"
                         && (resolvedReturnType == "bool" || resolvedReturnType == "char"
                             || resolvedReturnType == "i8" || resolvedReturnType == "u8"
                             || resolvedReturnType == "short" || resolvedReturnType == "i16"
                             || resolvedReturnType == "u16"))
                    rank = 1;
                else if (resolvedReturnType == "float" && resolvedDestType == "double")
                    rank = 1;
                viable.push_back({ name, rank, candidateReceiverRank });
                break;   // the two spellings of ONE operator are the same candidate
            }
        }
        // Receiver binding ranks first (clang): a worse-binding candidate never competes with a
        // better-binding one, so a plain exact match (rank 0 receiver) is the only choice left.
        if (std::any_of(viable.begin(), viable.end(),
                [](const StandardViable& v) { return v.receiverRank == 0; }))
            std::erase_if(viable, [](const StandardViable& v) { return v.receiverRank != 0; });
        else if (!exactName.empty())
            return exactName;   // the exact match is the only candidate that binds
        if (viable.empty()) return exactName;
        const int bestRank = std::min_element(viable.begin(), viable.end(),
            [](const auto& a, const auto& b) { return a.rank < b.rank; })->rank;
        std::erase_if(viable, [bestRank](const StandardViable& v) { return v.rank != bestRank; });
        if (viable.size() > 1)
        {
            if (ambiguous != nullptr) *ambiguous = true;
            if (ambiguousCandidates != nullptr)
                for (const auto& candidate : viable) ambiguousCandidates->push_back(candidate.name);
            return {};
        }
        if (needsStandardConversion != nullptr) *needsStandardConversion = true;
        return viable.front().name;
}

/*
 * Report the ambiguity CxxConversionOperatorTo detected. Split from the lookup because the
 * lookup also runs as an overload-ranking PREDICATE, where LogError (which throws) must not.
 */
void LLVMBackend::ReportAmbiguousCxxConversion(const std::string& sourceTypeName,
                                               const TypeAndValue& dest,
                                               const std::vector<std::string>& candidates) const
{
        TypeAndValue sourceType;
        sourceType.TypeName = sourceTypeName;
        std::string sourceDisplay = SpellType(*this, sourceType);
        if (sourceDisplay.empty()) sourceDisplay = sourceTypeName;
        std::string destDisplay = SpellType(*this, dest);
        if (destDisplay.empty()) destDisplay = dest.TypeName;
        std::string list;
        for (size_t i = 0; i < candidates.size(); ++i)
        {
            if (i > 0) list += " and ";
            list += "'" + candidates[i] + "'";
        }
        LogErrorMessage(
            "conversion from '{}' to '{}' is ambiguous: {} each reach it through one standard"
            " conversion. Cast to the conversion's own type first.",
            { sourceDisplay, destDisplay, list });
}

bool LLVMBackend::ApplyCxxConversionOperator(NamedVariable& nv, const TypeAndValue& dest,
                                             bool allowExplicit)
{
        if (nv.Primary == nullptr && nv.Storage == nullptr) return false;
        const std::string sourceTypeName = nv.TypeAndValue.TypeName;
        if (nv.TypeAndValue.Pointer || nv.TypeAndValue.ElemPointer) return false;
        bool needsStandard = false;
        bool ambiguous = false;
        std::vector<std::string> ambiguousCandidates;
        const std::string opName = CxxConversionOperatorTo(sourceTypeName, dest, allowExplicit,
                                                           &needsStandard, &ambiguous,
                                                           &ambiguousCandidates, &nv.TypeAndValue);
        // A materialization site is the one place the ambiguity can be reported; the ranking
        // predicate shares this lookup and must stay silent (LogError throws).
        if (ambiguous) ReportAmbiguousCxxConversion(sourceTypeName, dest, ambiguousCandidates);
        if (opName.empty()) return false;

        auto argNV = nv;
        argNV.TypeAndValue.VariableName.clear();
        auto savedReturnType = lastCallReturnType;
        bool savedReturnsOwned = lastCallReturnsOwned;
        bool savedOwningResult = lastOwningResult;
        llvm::Value* result = CreateOverloadedFunctionCall(opName, { argNV });
        const TypeAndValue conversionReturnType = lastCallReturnType;
        lastCallReturnType = savedReturnType;
        lastCallReturnsOwned = savedReturnsOwned;
        lastOwningResult = savedOwningResult;
        if (result == nullptr) return false;

        // A scalar `T&` return is an address too (MIX4: `operator const double&()`).
        const bool scalarRefReturn = conversionReturnType.IsCxxRef()
            && !conversionReturnType.IsCxxRefToPointer && !IsCxxRecord(dest.TypeName);
        if (!dest.Pointer && (conversionReturnType.Pointer || scalarRefReturn)
            && conversionReturnType.TypeName == dest.TypeName
            && result->getType()->isPointerTy())
        {
            llvm::Type* referentType = GetType(const_cast<TypeAndValue&>(dest));
            if (referentType == nullptr) return false;
            result = builder->CreateLoad(referentType, result, "cxx.conversion.ref");
        }

        if (dest.IsAlias && IsCxxRecord(dest.TypeName)
            && conversionReturnType.IsAlias
            && result->getType()->isPointerTy())
        {
            nv.Primary = dest.IsRvalueRef ? result : nullptr;
            nv.Storage = dest.IsRvalueRef ? nullptr : result;
            nv.BaseType = GetType(const_cast<TypeAndValue&>(dest));
            nv.TypeAndValue = dest;
            // The referent stays const through lowering: a const result never reaches a mutable dest.
            nv.TypeAndValue.SetCxxConstRef(dest.CxxConstRef() || conversionReturnType.CxxConstRef());
            nv.TypeAndValue.VariableName.clear();
            return true;
        }

        if (needsStandard)
        {
            llvm::Type* destLLVM = GetType(const_cast<TypeAndValue&>(dest));
            if (destLLVM == nullptr) return false;
            // An explicit '(T)conv' reaches here with allowExplicit; only an implicit store is refused.
            if (!allowExplicit) RefuseImplicitFloatToInt(result, destLLVM, dest.IsUnsignedInteger() != -1);
            if (result->getType() != destLLVM)
                result = CreateCast(result, destLLVM, !result->getType()->isIntegerTy(1),
                                    dest.IsUnsignedInteger() != -1);
        }
        nv.Primary = result;
        nv.Storage = nullptr;
        nv.TypeAndValue = dest;
        nv.TypeAndValue.VariableName.clear();
        return true;
}

/*
 * Value-level entry to the conversion-operator path, for sites that hold only the lowered
 * value (CreateAssignment). IMPLICIT conversions only: an `explicit operator T` is reserved
 * for a cast and for a boolean context.
 */
llvm::Value* LLVMBackend::ConvertViaImplicitConversionOperator(llvm::Value* value,
                                                               const TypeAndValue& dest)
{
        auto* st = llvm::dyn_cast_or_null<llvm::StructType>(
            value != nullptr ? value->getType() : nullptr);
        if (st == nullptr || st->isLiteral() || !st->hasName()) return nullptr;
        NamedVariable nv;
        nv.Primary = value;
        nv.BaseType = value->getType();
        nv.TypeAndValue.TypeName = st->getName().str();
        nv.Storage = StorageOfLoadedCxxObject(value);
        if (!ApplyCxxConversionOperator(nv, dest, /*allowExplicit*/ false)) return nullptr;
        return nv.Primary;
}

/*
 * Value-level entry for a site that knows only the lowered destination TYPE (CreateCast, which
 * the declaration initializer `int i = k;` reaches). IMPLICIT conversions only.
 */
llvm::Value* LLVMBackend::ConvertAggregateViaImplicitConversionOperator(llvm::Value* value,
                                                                        llvm::Type* destType)
{
        if (destType == nullptr
            || (!destType->isIntegerTy() && !destType->isFloatingPointTy()
                && !destType->isPointerTy()))
            return nullptr;
        auto* st = llvm::dyn_cast_or_null<llvm::StructType>(
            value != nullptr ? value->getType() : nullptr);
        if (st == nullptr || st->isLiteral() || !st->hasName()) return nullptr;
        if (destType->isPointerTy())
        {
            const std::string sourceType = st->getName().str();
            EnsureCxxConversionOperatorsProjected(sourceType);
            std::set<std::string> visited;
            auto find = [&](auto&& self, const std::string& owner) -> llvm::Value* {
                if (!visited.insert(owner).second) return nullptr;
                if (const auto* info = GetCxxClassInfo(owner))
                    for (const auto& method : info->directMethods)
                        if (method.spellable && method.raw.isConversion && !method.raw.isExplicit
                            && !method.raw.isDeleted && method.raw.bindRefusal.empty()
                            && method.ret.Pointer && !method.ret.ElemPointer
                            && GetType(const_cast<TypeAndValue&>(method.ret)) == destType)
                            if (auto* converted = ConvertViaImplicitConversionOperator(value, method.ret))
                                return converted;
                if (auto record = cxxRecordEntries_.find(owner); record != cxxRecordEntries_.end())
                    for (const auto& base : record->second.bases)
                    {
                        const std::string baseName = ResolveCxxBaseRecordName(base);
                        if (!baseName.empty())
                            if (auto* converted = self(self, baseName)) return converted;
                    }
                return nullptr;
            };
            return find(find, sourceType);
        }
        for (const std::string& spelling : ScalarConversionSpellings())
        {
            TypeAndValue dest;
            dest.TypeName = spelling;
            if (GetType(dest) != destType) continue;
            return ConvertViaImplicitConversionOperator(value, dest);
        }
        return nullptr;
}

/*
 * Ranking helper for the conversion-OPERATOR branch of overload scoring: reports the operator
 * that binds `arg` to `param` and accumulates this candidate's user-defined-conversion cost.
 * Preserve ambiguous sequences as distinct candidates so overload ranking cannot pick one.
 */
std::string LLVMBackend::ScoreCxxConversionOperatorArgument(const NamedVariable& arg,
                                                            const TypeAndValue& param,
                                                            bool preserveAmbiguous,
                                                            bool& needsStandardConversion,
                                                            int& userConversions,
                                                            int& userConversionCost,
                                                            std::string& userConversionNames) const
{
        needsStandardConversion = false;
        bool ambiguous = false;
        std::vector<std::string> ambiguousCandidates;
        const std::string opName = CxxConversionOperatorTo(arg.TypeAndValue.TypeName, param,
                                                           false, &needsStandardConversion,
                                                           &ambiguous, &ambiguousCandidates,
                                                           &arg.TypeAndValue);
        if (ambiguous)
        {
            if (!preserveAmbiguous) return {};
            needsStandardConversion = true;
            ++userConversions;
            ++userConversionCost;
            userConversionNames += "ambiguous{" + param.TypeName + ":";
            for (const auto& candidate : ambiguousCandidates)
                userConversionNames += candidate + ",";
            userConversionNames += "}|";
            return "ambiguous conversion";
        }
        if (opName.empty()) return {};
        ++userConversions;
        if (needsStandardConversion) ++userConversionCost;
        userConversionNames += opName + "|";
        return opName;
}

#include "MainListener.h"
#include <llvm/Support/SaveAndRestore.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IntrinsicInst.h>
#include "MainListener_ExpressionsShared.h"
LLVMBackend::NamedVariable MainListener::ParseNewExpression(CFlatParser::NewExpressionContext* ctx) {
        auto* compiler = Compiler(ctx);
        CallPostfixScope newCallPostfix(this);
        const bool desugarToArray = ctx == arrayNewDesugarCtx;
        arrayNewDesugarCtx = nullptr;  // one-shot
        // The destination of `new T` is the allocated pointer, not its array count or
        // constructor arguments. Those child expressions have their own types and must not
        // inherit an enclosing initializer/return destination.
        DeclExpectedTypeScope newExpectedScope(&declExpectedType, {});
        DiagnoseAssignmentArguments(ctx->argumentExpressionList());
        uint32_t elementConstMask = 0;
        std::string typeName = NewExpressionElementTypeName(ctx, &elementConstMask);
        const bool forceRootType = IsGlobalScopeQualifiedType(ctx->typeSpecifier());
        bool isArray = ctx->assignmentExpression() != nullptr;

        // If type substitution produced a pointer type (e.g. T=Employee*), strip the '*'
        // and treat as pointer-to-element so GetType resolves the base type correctly.
        bool typeIsPtr = false;
        int typeStars = 0;
        while (!typeName.empty() && typeName.back() == '*')
        {
            typeName.pop_back();
            typeIsPtr = true;
            ++typeStars;
        }
        const auto spellNewType = [&]() {
            return typeName + std::string(typeStars, '*');
        };
        if (typeStars + 1 > MaxDeclaratorPointerDepth)
        {
            LogErrorContext(ctx, std::format("'new {}' produces pointer depth {}, exceeding the limit of {}",
                spellNewType(), typeStars + 1, MaxDeclaratorPointerDepth));
            return {};
        }

        // `array<T> a = new T[n];` (and the assignment / return forms): the element count lives in
        // the array, so the value is built as `array<T> tmp; tmp.init(n);` - zero-filled, with
        // per-element construction done by init's own `new T[n]`.
        if (desugarToArray && isArray)
        {
            std::string elemSpelling = JoinConstTypeArgument(spellNewType(),
                ContiguousLowConstLevels(elementConstMask));
            std::string wantName = MangledGenericName("array", { elemSpelling });
            if (wantName != arrayNewDesugarTarget.TypeName)
            {
                LogErrorContext(ctx, compiler->LocalizeMessage(
                    "cannot store 'new {}[n]' into '{}': the element types differ",
                    { elemSpelling, SpellType(*compiler, arrayNewDesugarTarget) }));
                return {};
            }
            if (ctx->alignmentSpecifier() != nullptr)
            {
                LogErrorContext(ctx, compiler->LocalizeMessage(
                    "'alignas' is not supported on a 'new T[n]' stored into 'array<T>': "
                    "the array frees its buffer through the plain deallocator"));
                return {};
            }
            compiler->pendingInitAllocAlign = 0;
            llvm::Value* countVal = ParseAssignmentExpression(ctx->assignmentExpression());
            if (countVal == nullptr) return {};
            llvm::Value* ctorVal = compiler->CreateOverloadedFunctionCall(arrayNewDesugarTarget.TypeName, {});
            if (ctorVal == nullptr) return {};
            auto* arrTy = ctorVal->getType();
            auto* tempAlloca = compiler->AllocaAtEntry(arrTy, nullptr, "newarr");
            compiler->builder->CreateStore(ctorVal, tempAlloca);
            LLVMBackend::NamedVariable selfArg;
            selfArg.TypeAndValue = arrayNewDesugarTarget;
            selfArg.Storage = tempAlloca;
            selfArg.Primary = ctorVal;
            selfArg.BaseType = arrTy;
            LLVMBackend::NamedVariable countArg;
            countArg.Primary = compiler->Upconvert(countVal, compiler->builder->getInt32Ty());
            countArg.BaseType = countArg.Primary->getType();
            countArg.TypeAndValue.TypeName = "int";
            compiler->CreateOverloadedFunctionCall("init", { selfArg, countArg });
            LLVMBackend::NamedVariable result;
            result.TypeAndValue = arrayNewDesugarTarget;
            result.Primary = compiler->builder->CreateLoad(arrTy, tempAlloca, "newarr.val");
            result.BaseType = arrTy;
            compiler->lastOwningResult = true;
            return result;
        }

        LLVMBackend::TypeAndValue typeInfo{ .TypeName = typeName, .Pointer = typeIsPtr };
        const bool nativeCtorSret = compiler->IsNativeValueConstructor(typeName, typeInfo, {});
        llvm::Type* elemType = compiler->GetType(typeInfo);
        auto* braceInitList = ctx->initializerList();
        auto* braceArgs = ctx->argumentExpressionList();
        const bool freshBraceInit = !isArray && !typeIsPtr
            && (braceArgs == nullptr || braceArgs->argumentNamedExpression().empty())
            && CanFreshBraceInitializeAggregate(typeName, braceInitList);

        // Compute allocation size
        if (!elemType || !elemType->isSized())
        {
            // A C++ class left as an opaque shell on purpose (virtual inheritance) names its own
            // refusal, the same one a by-value local gets, instead of a generic size failure.
            if (!compiler->RejectUnsupportedCxxLayout(typeName))
                LogErrorContext(ctx, std::format("'new': cannot compute size of unsized or unresolved type '{}'",
                    SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = typeName })));
            return {};
        }
        const std::string allocOperator = isArray ? "operator new[]" : "operator new";
        const std::string deleteOperator = isArray ? "operator delete[]" : "operator delete";
        auto selectedAllocAccess = [&](const std::string& name) -> std::optional<int> {
            std::vector<std::string> work{ typeName };
            std::set<std::string> seen;
            for (size_t i = 0; i < work.size(); ++i)
            {
                if (!seen.insert(work[i]).second) continue;
                const auto* info = compiler->GetCxxClassInfo(work[i]);
                if (info == nullptr) continue;
                if (auto it = info->memberAccess.find(name); it != info->memberAccess.end())
                    return it->second;
                if (auto it = info->refusedMembers.find(name); it != info->refusedMembers.end())
                {
                    if (it->second == "is private") return cflat_cinterop::AccessPrivate;
                    if (it->second == "is protected") return cflat_cinterop::AccessProtected;
                }
                if (auto it = compiler->functionTable.find(work[i] + "." + name);
                    it != compiler->functionTable.end() && !it->second.empty())
                    return cflat_cinterop::AccessPublic;
                for (const auto& base : info->bases)
                    if (!base.isVirtual) work.push_back(base.name);
            }
            return std::nullopt;
        };
        auto allocAccess = typeIsPtr ? std::optional<int>{} : selectedAllocAccess(allocOperator);
        auto deleteAccess = typeIsPtr ? std::optional<int>{} : selectedAllocAccess(deleteOperator);
        const std::string denied = allocAccess && *allocAccess != cflat_cinterop::AccessPublic
            ? allocOperator
            : deleteAccess && *deleteAccess != cflat_cinterop::AccessPublic
                ? deleteOperator : std::string{};
        if (!denied.empty())
        {
            LogErrorContext(ctx, std::format(
                "cannot 'new' C++ class '{}': its class-level '{}' is not accessible here",
                SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = typeName }), denied));
            return {};
        }
        // `new K{0}` on a C++ class: plain positional brace items take the paren form's ctor
        // resolution (0 is the null pointer constant); other brace forms keep their refusal.
        const auto braceFields = ctx->initializerList() != nullptr
            ? ctx->initializerList()->fieldInit() : std::vector<CFlatParser::FieldInitContext*>{};
        const bool braceCxxArgs = ctx->initializerList() != nullptr && !isArray && !typeIsPtr
            && !braceFields.empty()
            && std::all_of(braceFields.begin(), braceFields.end(),
                           [](CFlatParser::FieldInitContext* field) {
                               return field->Identifier() == nullptr && field->Assign() == nullptr
                                   && field->LeftBrace() == nullptr && field->Colon() == nullptr
                                   && field->assignmentExpression().size() == 1;
                           })
            && (compiler->IsForeignNontrivialCxxClass(typeName)
                || compiler->IsForeignCxxClassWithUserDeclaredConstructor(typeName))
            && !compiler->IsForeignCxxAggregateClass(typeName);
        const bool hasNewCxxArgs = braceCxxArgs || (ctx->argumentExpressionList() != nullptr
            && !ctx->argumentExpressionList()->argumentNamedExpression().empty());
        const std::string resolvedNewCxxType = hasNewCxxArgs
            ? compiler->ResolveTypeAlias(typeName) : std::string{};
        const bool hasForeignCxxRecordArgs = hasNewCxxArgs && !isArray && !typeIsPtr
            && !compiler->CxxRecordCanonicalSpelling(resolvedNewCxxType).empty();
        const bool newCxxArgsAreAggregate = hasForeignCxxRecordArgs
            && compiler->IsForeignCxxAggregateClass(resolvedNewCxxType);
        const std::string& newCxxClassType = hasForeignCxxRecordArgs
            ? resolvedNewCxxType : typeName;
        /*
         * `new T(args)` on a foreign C++ record follows constructor overload resolution. A
         * trivial implicit copy is a memcpy and needs no copy-constructor wrapper.
         */
        if (!isArray && !typeIsPtr
            && (compiler->IsForeignNontrivialCxxClass(typeName)
                || compiler->IsForeignCxxClassWithUserDeclaredConstructor(typeName)
                || hasForeignCxxRecordArgs))
        {
            if (compiler->RejectUnsupportedCxxLayout(newCxxClassType)) return {};
            if (compiler->RejectAbstractCxxClass(newCxxClassType, "allocate")) return {};
            std::set<std::string> visitedDestructorBases;
            auto hasInheritedVirtualDestructor = [&](auto&& self,
                                                      const std::string& current) -> bool {
                if (!visitedDestructorBases.insert(current).second) return false;
                if (compiler->CxxHasVirtualDestructor(current)) return true;
                const auto* info = compiler->GetCxxClassInfo(current);
                if (info == nullptr) return false;
                for (const auto& base : info->bases)
                    if (!base.isVirtual && self(self, base.name)) return true;
                return false;
            };
            if (!compiler->HasTrivialCxxDtor(newCxxClassType)
                && !hasInheritedVirtualDestructor(hasInheritedVirtualDestructor,
                                                   newCxxClassType)
                && compiler->GetOrCreateCxxClassDestructor(newCxxClassType) == nullptr)
            {
                LogErrorContext(ctx, std::format(
                    "cannot allocate C++ class '{}' with 'new': it has no destructor cflat can "
                    "call, so the object could never be deleted",
                    compiler->DisplayCxxClassName(typeName)));
                return {};
            }
            if (ctx->initializerList() != nullptr && !braceCxxArgs)
            {
                LogErrorContext(ctx, std::format(
                    "cannot brace-initialize C++ class '{}'; use 'new {}(args)'",
                    compiler->DisplayCxxClassName(typeName),
                    compiler->DisplayCxxClassName(typeName)));
                return {};
            }
            std::vector<llvm::Value*> ctorArgs;
            std::vector<LLVMBackend::NamedVariable> ctorArgVars;
            std::vector<LLVMBackend::TypeAndValue> ctorArgTypes;
            // The constructor arguments: the paren list, or the positional elements of `new K{...}`.
            std::vector<antlr4::ParserRuleContext*> ctorArgNodes;
            std::vector<CFlatParser::AssignmentExpressionContext*> ctorArgExprs;
            if (braceCxxArgs)
                for (auto* field : braceFields)
                {
                    ctorArgNodes.push_back(field);
                    ctorArgExprs.push_back(field->assignmentExpression(0));
                }
            else if (auto* argList = ctx->argumentExpressionList())
                for (auto* named : argList->argumentNamedExpression())
                {
                    ctorArgNodes.push_back(named);
                    ctorArgExprs.push_back(named->assignmentExpression());
                }
            for (size_t argIndex = 0; argIndex < ctorArgNodes.size(); ++argIndex)
                {
                    auto* named = ctorArgNodes[argIndex];
                    auto* argAssign = ctorArgExprs[argIndex];
                    if (argAssign == nullptr)
                    {
                        LogErrorContext(named, std::format(
                            "a constructor argument for C++ class '{}' must be a plain expression",
                            compiler->DisplayCxxClassName(typeName)));
                        return {};
                    }
                    // A C++ constructor argument is a C++ call argument (T55 usual arithmetic for ?:).
                    LLVMBackend::TypeAndValue argExpected;
                    const bool hasDefaultArg = IsDefaultOnlyExpression(argAssign)
                        || HasDefaultOnlyTernaryArm(argAssign);
                    if (hasDefaultArg)
                        compiler->GetUniqueCxxConstructorValueParameter(typeName, argIndex,
                            ctorArgNodes.size(), argExpected);
                    std::optional<DeclExpectedTypeScope> argExpectedScope;
                    if (!argExpected.TypeName.empty())
                        argExpectedScope.emplace(&declExpectedType, argExpected, true);
                    CxxCallArgumentExpectedTypeScope cxxArgumentExpected(
                        cxxCallArgumentExpectedType_, argExpected);
                    CallArgumentScope callArgumentScope(inCallArgument_, ternaryCallArgumentDepth_);
                    CxxCallArgumentScope cxxNewArgumentScope(inCxxCallArgument_, true);
                    auto nv = [&] {
                        CxxCallArgumentParseStateScope argumentState(compiler);
                        auto parsed = ParseAssignmentExpressionNamed(argAssign);
                        llvm::Value* retTemp = compiler->lastCxxRetTemp_;
                        llvm::Value* retValue = compiler->lastCxxRetValue_;
                        if (retTemp != nullptr
                            && (parsed.Storage == retTemp || parsed.Primary == retValue))
                        {
                            parsed.Storage = retTemp;
                            parsed.IsRvalue = true;
                            parsed.IsCxxPrvalueTemp = !parsed.IsExplicitMove;
                        }
                        return parsed;
                    }();
                    NormalizeCxxRvalueReferenceArgument(argAssign, nv);
                    const bool hadSourceTypeName = !nv.TypeAndValue.TypeName.empty();
                    // Same literal identity the call-argument builder records (integer -> enum refusal).
                    bool suffixedLiteral = false;
                    if (nv.LiteralIdentity.empty())
                        nv.LiteralIdentity = LLVMBackend::LiteralIdentityForOverload(
                            argAssign->getText(), &suffixedLiteral);
                    llvm::Value* value = LoadNamedVariable(nv);
                    ctorArgs.push_back(value);
                    ctorArgTypes.push_back(nv.TypeAndValue);
                    TypeUntypedCtorArg(ctorArgTypes.back(), value,
                                       suffixedLiteral ? nv.LiteralIdentity : std::string(),
                                       &nv.TypeAndValue);
                    // Selection ranks the source type, not a literal's narrowed LLVM constant.
                    nv.Primary = value;
                    nv.BaseType = value ? value->getType() : nullptr;
                    // The forwarding thunk spells the argument's exact type and category: give
                    // a blank primitive its typed twin, and mark an address-less constant rvalue.
                    if (hadSourceTypeName && nv.TypeAndValue.TypeName.empty()
                        && !nv.TypeAndValue.Pointer)
                        nv.TypeAndValue.TypeName = ctorArgTypes.back().TypeName;
                    if (!nv.TypeAndValue.Pointer
                        && (nv.TypeAndValue.IsInteger() != -1
                            || nv.TypeAndValue.IsFloatingPoint() != -1))
                    {
                        llvm::Type* sourceType = compiler->GetType(nv.TypeAndValue);
                        if (sourceType != nullptr
                            && (sourceType->isIntegerTy() || sourceType->isFloatingPointTy()))
                            nv.BaseType = sourceType;
                    }
                    if (llvm::isa_and_nonnull<llvm::Constant>(ctorArgs.back())
                        && nv.Storage == nullptr && !nv.TypeAndValue.IsAlias)
                        nv.IsRvalue = true;
                    nv.OwnedStructTempEnd = compiler->pendingOwnedStructTemps.size();
                    nv.HasOwnedStructTempRange = true;
                    ctorArgVars.push_back(std::move(nv));
                }
            bool trivialCopyNeedsClangResolution = false;
            const auto* newCxxInfo = hasForeignCxxRecordArgs
                ? compiler->GetCxxClassInfo(resolvedNewCxxType) : nullptr;
            const bool trivialCopyableNewCxx = hasForeignCxxRecordArgs
                && !newCxxArgsAreAggregate && newCxxInfo != nullptr
                && newCxxInfo->hasTrivialCopyCtor && !newCxxInfo->hasDeletedCopyCtor
                && !compiler->IsForeignNontrivialCxxClass(resolvedNewCxxType);
            if (trivialCopyableNewCxx)
            {
                const bool hasDeclaredCtor =
                    compiler->IsForeignCxxClassWithUserDeclaredConstructor(resolvedNewCxxType);
                bool copySourceMatches = false;
                bool slicesToBase = false;
                uint64_t baseOffset = 0;
                if (ctorArgVars.size() == 1)
                {
                    const auto& source = ctorArgVars.front();
                    const std::string sourceTypeName = source.TypeAndValue.TypeName.empty()
                        ? std::string{} : compiler->ResolveTypeAlias(source.TypeAndValue.TypeName);
                    copySourceMatches = !source.TypeAndValue.Pointer
                        && sourceTypeName == resolvedNewCxxType;
                    if (!copySourceMatches && !source.TypeAndValue.Pointer
                        && !sourceTypeName.empty())
                    {
                        LLVMBackend::TypeAndValue sourceType = source.TypeAndValue;
                        sourceType.TypeName = sourceTypeName;
                        const LLVMBackend::TypeAndValue targetType{
                            .TypeName = resolvedNewCxxType };
                        bool inaccessible = false;
                        slicesToBase = compiler->IsCxxDerivedToBaseValue(
                                sourceType, targetType, &source)
                            && compiler->FindCxxValueBaseOffset(sourceTypeName,
                                resolvedNewCxxType, baseOffset, inaccessible, &source)
                            && !inaccessible;
                        copySourceMatches = slicesToBase;
                    }
                    // Another C++ class may convert (operator T): clang resolves it.
                    if (!copySourceMatches && !hasDeclaredCtor && !source.TypeAndValue.Pointer
                        && !sourceTypeName.empty()
                        && (!compiler->CxxRecordCanonicalSpelling(sourceTypeName).empty()
                            || !compiler->CxxConversionOperatorTo(sourceTypeName,
                                LLVMBackend::TypeAndValue{ .TypeName = resolvedNewCxxType }, false).empty()))
                        trivialCopyNeedsClangResolution = true;
                }
                // A declared constructor (template, deleted or private copy) takes clang's resolution.
                if (copySourceMatches && !hasDeclaredCtor && ctorArgVars.size() == 1)
                {
                    auto& source = ctorArgVars.front();
                    const std::string sourceTypeName =
                        compiler->ResolveTypeAlias(source.TypeAndValue.TypeName);
                    llvm::Value* sourceStorage = source.Storage;
                    if (sourceStorage == nullptr && source.CxxRefValueType != nullptr
                        && source.Primary != nullptr && source.Primary->getType()->isPointerTy())
                        sourceStorage = source.Primary;
                    if (sourceStorage == nullptr)
                    {
                        llvm::Type* sourceType = compiler->GetType(
                            LLVMBackend::TypeAndValue{ .TypeName = sourceTypeName });
                        llvm::Value* sourceValue = source.Primary != nullptr
                            ? source.Primary : LoadNamedVariable(source);
                        if (sourceType == nullptr || sourceValue == nullptr
                            || sourceValue->getType() != sourceType)
                        {
                            LogErrorContext(ctx, std::format(
                                "cannot copy C++ class '{}' from this value",
                                compiler->DisplayCxxClassName(typeName)));
                            return {};
                        }
                        sourceStorage = compiler->AllocaAtEntry(sourceType, nullptr,
                                                               "new.copy.src");
                        compiler->builder->CreateStore(sourceValue, sourceStorage);
                    }
                    if (slicesToBase)
                        sourceStorage = compiler->EmitCxxBaseAdjust(sourceStorage, baseOffset);

                    newCallPostfix.Flush();
                    llvm::Value* block = compiler->EmitCxxHeapAllocate(resolvedNewCxxType);
                    if (block == nullptr)
                    {
                        LogErrorContext(ctx, std::format(
                            "'new': cannot compute the size of C++ class '{}'",
                            compiler->DisplayCxxClassName(typeName)));
                        return {};
                    }
                    LLVMBackend::UnwindPartialScope newBlockScope(*compiler);
                    compiler->NoteUnwindPartial(LLVMBackend::UnwindPartialEntry::Kind::CxxHeap,
                                                block, resolvedNewCxxType);
                    llvm::Type* copyType = compiler->GetType(
                        LLVMBackend::TypeAndValue{ .TypeName = resolvedNewCxxType });
                    if (copyType == nullptr || !copyType->isSized())
                    {
                        LogErrorContext(ctx, std::format(
                            "cannot copy C++ class '{}' with an unresolved layout",
                            compiler->DisplayCxxClassName(typeName)));
                        return {};
                    }
                    const auto& layout = compiler->module->getDataLayout();
                    compiler->builder->CreateMemCpy(block, layout.getABITypeAlign(copyType),
                        sourceStorage, layout.getABITypeAlign(copyType),
                        layout.getTypeAllocSize(copyType));
                    newBlockScope.Release();

                    LLVMBackend::NamedVariable result;
                    result.TypeAndValue = LLVMBackend::TypeAndValue{
                        .TypeName = typeName, .Pointer = true };
                    result.Primary = block;
                    result.BaseType = block->getType();
                    compiler->lastOwningResult = true;
                    compiler->RegisterOwnedNewTemp(block, typeName, 0);
                    compiler->RegisterValueElementTypeName(block, typeName);
                    return result;
                }
                if (!hasDeclaredCtor && !trivialCopyNeedsClangResolution)
                {
                    LogErrorContext(ctx, std::format(
                        "C++ class '{}' has no constructor matching the provided arguments",
                        compiler->DisplayCxxClassName(typeName)));
                    return {};
                }
            }
            // `new T()` needs the default constructor bound when clang must apply its defaults.
            if (ctorArgTypes.empty())
            {
                std::string defaultCtorError;
                compiler->TryBindCxxImplicitDefaultCtor(typeName, defaultCtorError);
                if (!defaultCtorError.empty()) LogErrorContext(ctx, defaultCtorError);
                if (compiler->FindCxxDefaultCtor(typeName) == nullptr)
                {
                    std::string ambiguityError;
                    if (compiler->CxxDefaultCtorAmbiguityError(typeName, ambiguityError))
                        LogErrorContext(ctx, ambiguityError);
                    LogErrorContext(ctx, std::format("C++ class '{}' has no default constructor cflat can call",
                        compiler->DisplayCxxClassName(typeName)));
                }
            }
            std::string why;
            compiler->TryBindRefusedCxxMember(typeName, "__ctor", ctorArgTypes.size());
            bool scalarReferenceSet = compiler->CxxCtorSetRankedByCflat(
                typeName, ctorArgTypes, &ctorArgVars);
            bool forwardsScalarRef = compiler->CxxCtorForwardsScalarReference(typeName, ctorArgTypes);
            const auto* ctor = compiler->SelectCxxConstructor(typeName, ctorArgTypes, why, scalarReferenceSet,
                                                             &ctorArgVars);
            // A constructor template can outrank the listed pick: clang resolves it then.
            std::string wrapperName;
            std::string wrapperError;
            bool nullPointerCandidate = false;
            bool clangResolves = trivialCopyNeedsClangResolution
                || compiler->CxxConstructorNeedsClangResolution(
                typeName, ctor, ctorArgTypes, &ctorArgVars, /*allowExplicit*/ true,
                &nullPointerCandidate, /*rankScalarReferenceSet*/ scalarReferenceSet);
            if (ctor == nullptr && !clangResolves
                && !why.starts_with("matches more than one constructor overload"))
            {
                bool slotBound = false;
                for (size_t i = 0; i < ctorArgVars.size(); ++i)
                {
                    auto& arg = ctorArgVars[i];
                    BindCxxCtorPointerSlotAddress(ctorArgNodes[i], arg, typeName, i,
                                                  ctorArgVars.size());
                    if (arg.Storage == nullptr || !arg.IsCxxAddressOfPointerValue) continue;
                    ctorArgTypes[i] = arg.TypeAndValue;
                    ctorArgs[i] = LoadNamedVariable(arg);
                    slotBound = true;
                }
                if (slotBound)
                {
                    why.clear();
                    ctor = compiler->SelectCxxConstructor(typeName, ctorArgTypes, why, scalarReferenceSet,
                                                         &ctorArgVars);
                    nullPointerCandidate = false;
                    clangResolves = trivialCopyNeedsClangResolution
                        || compiler->CxxConstructorNeedsClangResolution(
                            typeName, ctor, ctorArgTypes, &ctorArgVars, /*allowExplicit*/ true,
                            &nullPointerCandidate, /*rankScalarReferenceSet*/ scalarReferenceSet);
                }
            }
            const bool clangOnlyConstructorSet =
                (forwardsScalarRef && !scalarReferenceSet)
                || nullPointerCandidate;
            const bool hardReferenceRejection = !clangOnlyConstructorSet
                && (why.starts_with("constructor '") || why.starts_with("no overload of '"));
            if (const std::string sink =
                    compiler->CxxConstructorCopySinkError(ctor, ctorArgVars);
                !sink.empty())
                LogErrorContext(ctx, sink);
            const bool narrowingRefused
                = compiler->CxxCtorSetRequiresNarrowing(typeName, &ctorArgVars);
            const bool floatToIntegerSet = (!clangResolves || narrowingRefused)
                && compiler->CxxCtorSetRequiresFloatToInteger(typeName, ctorArgTypes);
            if (floatToIntegerSet)
            {
                std::string floatNarrowing
                    = compiler->CxxCtorSelectedNarrowingError(typeName, ctorArgVars, true);
                if (floatNarrowing.empty())
                    floatNarrowing = "all viable constructors require a floating-to-integer conversion "
                        "(no implicit narrowing at a call); cast it explicitly";
                LogErrorContext(ctx, std::format("C++ class '{}' {}; {}",
                    compiler->DisplayCxxClassName(typeName),
                    compiler->CxxCtorNoMatchReason(ctorArgTypes), floatNarrowing));
                return {};
            }
            // The scalar table applies whether or not clang resolves the call: a template or
            // clang-selected constructor cannot take a narrowing argument silently (ruling 2026-10-09).
            const bool preserveNumericNarrowing
                = (!clangResolves && floatToIntegerSet) || narrowingRefused;
            // The set keeps a non-narrowing candidate: the one C++ selects must not narrow either.
            if (!preserveNumericNarrowing
                && ((ctor == nullptr && !hardReferenceRejection) || clangResolves))
                if (const std::string narrowing
                        = compiler->CxxCtorSelectedNarrowingError(typeName, ctorArgVars);
                    !narrowing.empty())
                {
                    LogErrorContext(ctx, std::format("C++ class '{}' {}",
                        compiler->DisplayCxxClassName(typeName), narrowing));
                    return {};
                }
            const bool wrapped = !preserveNumericNarrowing
                && ((ctor == nullptr && !hardReferenceRejection)
                    || clangResolves)
                && compiler->RequestCxxVariadicConstructor(typeName, ctorArgVars, wrapperName,
                                                           wrapperError);
            // The copy-sink verdict names the parameter and the remedy: it outranks the body
            // refusal of the constructor clang selected (MSVC's fill constructor).
            if (wrapperError.starts_with("clang: ") && why.starts_with("cannot copy C++ class"))
                wrapperError.clear();
            // Clang refusing its own wrapper (a deleted or ambiguous pick) stands over the listed
            // constructor the CFlat side would otherwise call.
            const bool clangRefusedWrapper = !wrapped && ctor != nullptr && !wrapperError.empty()
                && compiler->lastCxxVariadicCtorClangRefused_;
            if (!wrapped && (wrapperError.starts_with("clang: ") || clangRefusedWrapper))
            {
                // Clang picked a constructor whose body failed to instantiate.
                LogErrorContext(ctx, wrapperError);
                return {};
            }
            // A scalar-reference overload set has no listed fallback: clang's refusal stands.
            if (!wrapped && (ctor == nullptr
                             || clangOnlyConstructorSet))
            {
                if (!wrapperError.empty() && !why.starts_with("constructor '"))
                    why = why.starts_with("matches more than one constructor overload")
                        ? why + "; " + wrapperError : wrapperError;
                // A scalar-reference set whose every constructor narrows has no listed refusal.
                if (why.empty()) why = compiler->CxxCtorNoMatchReason(ctorArgTypes);
                LogErrorContext(ctx, why.starts_with("no overload of '")
                    ? why : std::format("C++ class '{}' {}",
                        compiler->DisplayCxxClassName(typeName), why));
                return {};
            }
            if (!wrapped)
                if (const std::string refusal = compiler->CxxConstructorBodyRefusalError(ctor);
                    !refusal.empty())
                {
                    // The listed pick is called directly and its body failed to instantiate.
                    LogErrorContext(ctx, refusal);
                    return {};
                }
            compiler->RejectLiteralZeroCxxConstructorFunctionSlotArgs(
                typeName, ctorArgVars, ctorArgs);
            newCallPostfix.Flush();
            llvm::Value* block = compiler->EmitCxxHeapAllocate(typeName);
            if (block == nullptr)
            {
                LogErrorContext(ctx, std::format(
                    "'new': cannot compute the size of C++ class '{}'",
                    compiler->DisplayCxxClassName(typeName)));
                return {};
            }
            // A throwing constructor frees the block (operator delete) and never destroys it.
            LLVMBackend::UnwindPartialScope newBlockScope(*compiler);
            compiler->NoteUnwindPartial(LLVMBackend::UnwindPartialEntry::Kind::CxxHeap, block,
                                        typeName);
            if (wrapped)
            {
                LLVMBackend::NamedVariable self;
                self.Primary = block;
                self.BaseType = block->getType();
                self.TypeAndValue.TypeName = typeName;
                self.TypeAndValue.Pointer = true;
                self.IsRvalue = true;
                std::vector<LLVMBackend::NamedVariable> wrapperArguments{ self };
                // A positional argument keeps its source name; the wrapper would read it as named.
                for (size_t i = 0; i < ctorArgVars.size(); ++i)
                {
                    LLVMBackend::NamedVariable argVar = ctorArgVars[i];
                    bool isReference = false;
                    if (ctor != nullptr && i + 1 < ctor->params.size())
                    {
                        const auto& param = ctor->params[i + 1];
                        isReference = param.IsAlias || param.IsRvalueRef
                            || param.CxxRefLevel >= 0;
                    }
                    bool thunkReference = false;
                    if (compiler->CxxConstructorThunkParameterIsReference(
                            typeName, ctorArgVars, i, /*rvalueReference*/ false,
                            thunkReference))
                        isReference = thunkReference;
                    argVar.IsCxxReferenceSink = isReference;
                    argVar.TypeAndValue.VariableName.clear();
                    wrapperArguments.push_back(std::move(argVar));
                }
                compiler->ConvertCxxCtorThunkArguments(wrapperName, wrapperArguments);
                compiler->CreateOverloadedFunctionCall(wrapperName, wrapperArguments, false,
                                                      compiler->DisplayCxxClassName(typeName));
            }
            else
                compiler->EmitCxxStructorCall(typeName, *ctor, block, ctorArgs, &ctorArgVars);
            newBlockScope.Release();
            LLVMBackend::NamedVariable result;
            result.TypeAndValue = LLVMBackend::TypeAndValue{ .TypeName = typeName, .Pointer = true };
            result.TypeAndValue.NewAllocAlignment =
                compiler->GetEffectiveAlignmentForType(typeName, elemType);
            result.Primary = block;
            result.BaseType = block->getType();
            compiler->lastOwningResult = true;
            compiler->RegisterOwnedNewTemp(block, typeName, 0);
            return result;
        }

        // Honor struct-level alignas: pad sizeof so arrays stride correctly.
        uint64_t effAlign = typeIsPtr
            ? 0
            : compiler->GetEffectiveAlignmentForType(typeName, elemType);
        llvm::Value* sizeVal;
        if (effAlign > 1)
        {
            uint64_t paddedSize = compiler->GetEffectiveAllocSize(elemType, effAlign);
            sizeVal = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*compiler->context), paddedSize);
        }
        else
        {
            sizeVal = compiler->GetTypeSizeBytes(elemType);
        }
        llvm::Value* count = nullptr;
        if (isArray)
        {
            count = ParseAssignmentExpression(ctx->assignmentExpression());
            count = compiler->Upconvert(count, compiler->builder->getInt64Ty());
            sizeVal = compiler->builder->CreateMul(sizeVal, count, "arraysz");
        }

        // Per-allocation alignment. This is an allocation property only - it must NOT change the
        // type's sizeof/stride, so the size above stays derived from the type's effAlign. allocAlign
        // drives the aligned operator-new route and the assume below; it is max(site, type).
        // Three sources, in priority order:
        //   1. Explicit clause on the `new`: `alignas(N)` (1-arg = allocation, back-compat) or
        //      `alignas(S, N)` (arg2 = allocation; the slot arg is meaningless for a heap block).
        //   2. Otherwise a BARE `new` INHERITS the target declaration's clause via pendingInitAllocAlign
        //      (inbound channel) - direct decl-init / direct assignment only; indirect contexts leave
        //      it 0 so the receiving decl errors rather than silently under-align.
        uint64_t allocAlign = effAlign;
        if (auto* alignSpec = ctx->alignmentSpecifier())
        {
            uint64_t slotAlign = ParseAlignmentSpecifier(alignSpec);  // arg1 (0 = natural / error)
            uint64_t blockAlign = ParseAllocAlignArg(alignSpec);      // arg2 (0 = absent / error)
            uint64_t siteAlign = (blockAlign != 0) ? blockAlign : slotAlign;
            if (siteAlign > allocAlign) allocAlign = siteAlign;
        }
        else if (compiler->pendingInitAllocAlign > allocAlign)
        {
            allocAlign = compiler->pendingInitAllocAlign;
        }
        // One-shot: the inbound clause is consumed (or is inapplicable) at this `new`.
        compiler->pendingInitAllocAlign = 0;

        // Call operator new: class-specific -> global. When effective alignment
        // exceeds the default-new threshold (16 on x64), route to the 2-arg
        // overload `operator new(size, align)` so the allocator picks aligned
        // memory and the matching delete uses operator delete_aligned.
        bool useAligned = allocAlign > LLVMBackend::kDefaultNewAlign;
        llvm::Value* rawPtr = nullptr;
        std::string opNewName = typeName + ".operator new";
        LLVMBackend::NamedVariable szArg;
        szArg.Primary = sizeVal;
        szArg.BaseType = sizeVal->getType();
        std::vector<LLVMBackend::NamedVariable> newArgs = { szArg };
        if (useAligned)
        {
            LLVMBackend::NamedVariable alignArg;
            alignArg.Primary = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*compiler->context), allocAlign);
            alignArg.BaseType = alignArg.Primary->getType();
            newArgs.push_back(alignArg);
        }
        // A C++ class allocates through C++'s allocation functions, matched on every free site:
        // `new T[n]` looks up operator new[] (class-scope, else global), never operator new.
        // A CFlat-declared operator new on T hides the base's C++ one: the scalar form calls it.
        const bool cxxAllocator = !typeIsPtr && compiler->CxxClassUsesCxxAllocator(typeName)
            && (isArray || !compiler->HasCflatClassAllocFunction(typeName, "operator new"));
        if (cxxAllocator)
        {
            rawPtr = isArray ? compiler->EmitCxxHeapAllocateArray(typeName, sizeVal, allocAlign)
                             : compiler->EmitCxxHeapAllocate(typeName);
            if (rawPtr == nullptr)
            {
                LogErrorContext(ctx, std::format(
                    "'new': cannot compute the size of C++ class '{}'",
                    compiler->DisplayCxxClassName(typeName)));
                return {};
            }
            // CFlat's operator new hands back zeroed storage; keep that for a trivial element.
            compiler->builder->CreateMemSet(rawPtr, compiler->builder->getInt8(0), sizeVal,
                                            llvm::MaybeAlign(1));
        }
        else if (!typeIsPtr && !typeName.empty() && compiler->GetFunction(opNewName))
        {
            rawPtr = compiler->CreateOverloadedFunctionCall(opNewName, newArgs);
        }
        else if (compiler->GetFunction("operator new"))
        {
            rawPtr = compiler->CreateOverloadedFunctionCall("operator new", newArgs);
        }
        else
        {
            LogErrorContext(ctx, "'new' requires 'operator new' to be defined");
            return {};
        }

        llvm::Type* ptrTy = cflat_llvm::PointerTo(elemType);
        llvm::Value* typedPtr = compiler->builder->CreateBitCast(rawPtr, ptrTy, "newptr");

        // Assume-aligned: tell the optimizer the buffer is aligned so LoopVectorize
        // can emit aligned vector moves over it. Sound because operator new(size, align)
        // returns memory aligned to allocAlign. Emit only for the array-view result.
        if (isArray && useAligned)
            compiler->builder->CreateAlignmentAssumption(compiler->module->getDataLayout(), typedPtr, (unsigned)allocAlign);

        // Zero a pointer- or interface-element buffer: no per-element ctor runs for either, so an
        // unset slot must read null, not a stale pointer or a garbage vtable an indirect call uses.
        if (isArray && count && (typeIsPtr || compiler->IsInterfaceType(typeName)))
        {
            llvm::Align ptrAlign = compiler->module->getDataLayout().getABITypeAlign(elemType);
            compiler->builder->CreateMemSet(typedPtr, compiler->builder->getInt8(0), sizeVal, ptrAlign);
        }

        /*
         * For array new of a class type: call the default constructor for each element (like C++).
         * Skip when typeIsPtr - the element is a pointer (e.g. Point*), not a struct; calling
         * the struct ctor would store sizeof(Point)=8 bytes into a sizeof(ptr)=4-byte slot on
         * Win32, corrupting the heap (on Win64 they happen to be equal so the bug is silent).
         * A foreign C++ class has no CFlat constructor FUNCTION, so the second arm never ran for
         * it while `delete` already destroyed every element: its own constructor is the first arm.
         */
        // A throwing element constructor destroys the elements before it, then frees the block.
        LLVMBackend::UnwindPartialScope newArrayScope(*compiler);
        if (isArray && count && cxxAllocator
            && compiler->CxxElementNeedsDefaultConstruction(typeName))
            compiler->NoteUnwindPartial(LLVMBackend::UnwindPartialEntry::Kind::CxxHeapArray,
                                        typedPtr, typeName, allocAlign, count);
        else if (isArray && count && !typeIsPtr
            && (compiler->GetFunction(typeName) != nullptr
                || compiler->CxxElementNeedsDefaultConstruction(typeName)))
            compiler->NoteUnwindPartial(compiler->GetFunction(opNewName)
                    ? LLVMBackend::UnwindPartialEntry::Kind::UserNewHeap
                    : LLVMBackend::UnwindPartialEntry::Kind::CflatHeap,
                typedPtr, typeName, useAligned ? allocAlign : 0, sizeVal);
        if (isArray && count && !typeIsPtr && compiler->GetFunction(typeName) == nullptr
            && compiler->CxxElementNeedsDefaultConstruction(typeName))
        {
            std::string ctorError;
            if (!compiler->EmitCxxArrayDefaultConstruction(typeName, typedPtr, elemType, count,
                                                           ctorError))
                LogErrorContext(ctx, ctorError);
        }
        else if (isArray && count && !typeIsPtr && compiler->GetFunction(typeName))
        {
            auto* i64Ty = compiler->builder->getInt64Ty();
            auto* indexAlloca = compiler->builder->CreateAlloca(i64Ty, nullptr, "init_i");
            compiler->builder->CreateStore(compiler->builder->getInt64(0), indexAlloca);

            auto* condBB = compiler->CreateBasicBlock("new_ctor_cond");
            auto* bodyBB = compiler->CreateBasicBlock("new_ctor_body");
            auto* afterBB = compiler->CreateBasicBlock("new_ctor_after");
            compiler->builder->CreateBr(condBB);

            compiler->builder->SetInsertPoint(condBB);
            auto* idx = compiler->builder->CreateLoad(i64Ty, indexAlloca);
            auto* cmp = compiler->builder->CreateICmpSLT(idx, count);
            compiler->builder->CreateCondBr(cmp, bodyBB, afterBB);

            compiler->builder->SetInsertPoint(bodyBB);
            auto* idx2 = compiler->builder->CreateLoad(i64Ty, indexAlloca);
            auto* elemPtr = compiler->builder->CreateGEP(elemType, typedPtr, idx2, "new_elem");
            {
                LLVMBackend::UnwindPartialScope builtElements(*compiler);
                compiler->NoteUnwindArrayPrefix(typedPtr, elemType, idx2, typeName);
                if (nativeCtorSret)
                {
                    compiler->pendingCxxSretDest_ = elemPtr;
                    compiler->pendingCxxSretTypeName_ = typeName;
                    compiler->pendingCxxSretForFixedArray_ = true;
                }
                llvm::Value* structVal = compiler->CreateOverloadedFunctionCall(
                    typeName, {}, forceRootType);
                const bool builtInPlace = nativeCtorSret
                    && compiler->pendingCxxSretDest_ == nullptr;
                if (nativeCtorSret)
                {
                    compiler->pendingCxxSretDest_ = nullptr;
                    compiler->pendingCxxSretTypeName_.clear();
                    compiler->pendingCxxSretForFixedArray_ = false;
                }
                if (structVal && !builtInPlace)
                    compiler->builder->CreateStore(structVal, elemPtr);
            }
            auto* next = compiler->builder->CreateAdd(idx2, compiler->builder->getInt64(1));
            compiler->builder->CreateStore(next, indexAlloca);
            compiler->builder->CreateBr(condBB);

            compiler->builder->SetInsertPoint(afterBB);
        }
        newArrayScope.Release();

        // For non-array new of a class type: call constructor and store result
        if (!isArray && !typeIsPtr && compiler->GetFunction(typeName) && !freshBraceInit)
        {
            // An unwind out of the arguments or the constructor frees the global-new block.
            LLVMBackend::UnwindPartialScope newBlockScope(*compiler);
            compiler->NoteUnwindPartial(compiler->GetFunction(opNewName)
                    ? LLVMBackend::UnwindPartialEntry::Kind::UserNewHeap
                    : LLVMBackend::UnwindPartialEntry::Kind::CflatHeap,
                typedPtr, typeName, useAligned ? allocAlign : 0, sizeVal);
            std::vector<LLVMBackend::NamedVariable> ctorArgs;
            auto argList = ctx->argumentExpressionList();
            if (argList != nullptr)
            {
                for (auto* namedArg : argList->argumentNamedExpression())
                {
                    // Preserve legacy value loading while retaining metadata for core unique args.
                    size_t savedCastOcc = compiler->BeginCastOccurrence();
                    size_t thisCastOcc = compiler->CurrentCastOccurrence();
                    auto argNV = ParseAssignmentExpressionNamed(namedArg->assignmentExpression());
                    llvm::Value* argVal = argNV.Primary != nullptr
                        ? argNV.Primary : LoadNamedVariable(argNV);
                    compiler->EndCastOccurrence(savedCastOcc);
                    if (!argVal) break;
                    LLVMBackend::NamedVariable argVar;
                    if (compiler->IsCoreUniqueType(argNV.TypeAndValue.TypeName)
                        && !argNV.TypeAndValue.Pointer)
                        argVar = argNV;
                    else
                        argVar.TypeAndValue = argNV.TypeAndValue;
                    argVar.BaseType = argVal->getType();
                    argVar.Primary = argVal;
                    argVar.IsTupleLiteral = argNV.IsTupleLiteral;
                    argVar.TupleLiteralElements = argNV.TupleLiteralElements;
                    argVar.TypeAndValue.VariableName.clear();
                    argVar.CastOccurrenceId = thisCastOcc;
                    argVar.OwnedStructTempEnd = compiler->pendingOwnedStructTemps.size();
                    argVar.HasOwnedStructTempRange = true;
                    ctorArgs.push_back(argVar);
                }
            }
            newCallPostfix.Flush();
            const bool armLoweredSret = (compiler->IsLoweredCFlatOnlyStruct(typeName)
                && compiler->ReturnsViaInPlaceSret(typeName)) || nativeCtorSret;
            if (armLoweredSret)
            {
                compiler->pendingCxxSretDest_ = typedPtr;
                compiler->pendingCxxSretTypeName_ = typeName;
                compiler->pendingCxxSretReturn_ = true;
            }
            llvm::Value* structVal = compiler->CreateOverloadedFunctionCall(
                typeName, ctorArgs, forceRootType);
            const bool builtInPlace = armLoweredSret && compiler->pendingCxxSretDest_ == nullptr;
            if (armLoweredSret)
            {
                compiler->pendingCxxSretDest_ = nullptr;
                compiler->pendingCxxSretTypeName_.clear();
                compiler->pendingCxxSretReturn_ = false;
            }
            newBlockScope.Release();
            // The constructed value is about to land in a HEAP block that outlives the statement.
            // No other escape site sees this store, so the ctor-launder leg is asked here.
            if (structVal)
            {
                LLVMBackend::NamedVariable ctorNV;
                ctorNV.Primary = structVal;
                GuardOwningTempUniqueFieldEscape(
                    ctorNV, std::format("the heap object 'new {}' allocates", spellNewType()), ctx);
            }
            if (structVal && !builtInPlace)
                compiler->builder->CreateStore(structVal, typedPtr);
        }

        // `new T()` of a C++ record with no CFlat constructor function and a NONTRIVIAL default
        // constructor (default member initializers, a non-trivial base): run it, as C++ does.
        // The zeroed block alone would leave the initializers unrun.
        const bool noCxxArgs = ctx->argumentExpressionList() == nullptr
            || ctx->argumentExpressionList()->argumentNamedExpression().empty();
        if (!isArray && !typeIsPtr && noCxxArgs && ctx->initializerList() == nullptr
            && compiler->GetFunction(typeName) == nullptr && compiler->IsCxxRecord(typeName)
            && compiler->CxxElementNeedsDefaultConstruction(typeName))
        {
            std::string bindError;
            compiler->TryBindCxxImplicitDefaultCtor(typeName, bindError);
            if (!bindError.empty()) LogErrorContext(ctx, bindError);
            if (compiler->FindCxxDefaultCtor(typeName) == nullptr)
            {
                std::string ambiguityError;
                if (compiler->CxxDefaultCtorAmbiguityError(typeName, ambiguityError))
                    LogErrorContext(ctx, ambiguityError);
                LogErrorContext(ctx, std::format(
                    "C++ class '{}' has no default constructor cflat can call, so 'new {}()' "
                    "cannot value-initialize it",
                    compiler->DisplayCxxClassName(typeName),
                    compiler->DisplayCxxClassName(typeName)));
                return {};
            }
            // A throwing initializer frees the block; the constructed object is never destroyed.
            LLVMBackend::UnwindPartialScope newBlockScope(*compiler);
            compiler->NoteUnwindPartial(cxxAllocator
                    ? LLVMBackend::UnwindPartialEntry::Kind::CxxHeap
                    : LLVMBackend::UnwindPartialEntry::Kind::CflatHeap,
                typedPtr, typeName, cxxAllocator ? 0 : (useAligned ? allocAlign : 0));
            std::string ctorError;
            if (!compiler->EmitCxxArrayDefaultConstruction(typeName, typedPtr, elemType,
                    llvm::ConstantInt::get(compiler->builder->getInt64Ty(), 1), ctorError))
                LogErrorContext(ctx, ctorError);
            newBlockScope.Release();
        }
        // A trivial C++ record value-initializes to zero; CFlat's operator new is plain malloc.
        else if (!isArray && !typeIsPtr && !cxxAllocator && noCxxArgs && ctx->initializerList() == nullptr
            && compiler->GetFunction(typeName) == nullptr && compiler->IsCxxRecord(typeName))
        {
            const uint64_t recSize = compiler->module->getDataLayout().getTypeAllocSize(elemType);
            compiler->builder->CreateMemSet(typedPtr, compiler->builder->getInt8(0), recSize, llvm::MaybeAlign(1));
        }

        // A non-class `new T(value)` has no constructor function to consume the argument. Keep
        // the C++ value-initialization meaning and use the ordinary assignment store conversion.
        if (!isArray && (typeIsPtr
            || (compiler->GetFunction(typeName) == nullptr && !compiler->IsCxxRecord(typeName))))
        {
            auto* argList = ctx->argumentExpressionList();
            // `new T()` (and bare `new T`) value-initializes: the fresh heap slot reads zero.
            if (argList == nullptr || argList->argumentNamedExpression().empty())
                compiler->builder->CreateStore(llvm::Constant::getNullValue(elemType), typedPtr);
            else
            {
                auto args = argList->argumentNamedExpression();
                if (args.size() != 1)
                {
                    LogErrorContext(ctx, std::format(
                        "'new {}' with a non-class type requires exactly one argument",
                        spellNewType()));
                    return {};
                }
                auto* namedArg = args.front();
                if (namedArg->assignmentExpression() == nullptr || namedArg->Identifier() != nullptr)
                {
                    LogErrorContext(namedArg, std::format(
                        "the initializer for 'new {}' must be a plain expression",
                        spellNewType()));
                    return {};
                }
                auto argNV = ParseAssignmentExpressionNamed(namedArg->assignmentExpression());
                llvm::Value* argVal = argNV.Primary != nullptr
                    ? argNV.Primary : LoadNamedVariable(argNV);
                LLVMBackend::TypeAndValue slotType{ .TypeName = typeName, .Pointer = typeIsPtr };
                slotType.PointerDepth = typeStars;
                slotType.ConstMask = elementConstMask;
                if (argVal != nullptr)
                {
                    compiler->RejectImplicitIntegerPointeePointerConversion(
                        argNV.TypeAndValue, slotType);
                    if (RejectImplicitPrimitiveToPointer(
                            namedArg, slotType, argNV, argVal, "initialize",
                            std::format("the slot of 'new {}'", spellNewType()))
                        || RejectImplicitPointerToNumber(
                            namedArg, slotType, argNV, argVal, "initialize",
                            std::format("the slot of 'new {}'", spellNewType()))
                        || RejectConstDiscardingConversion(namedArg, argNV, argVal, slotType))
                        return {};
                }
                if (argVal != nullptr)
                    compiler->CreateAssignment(
                        argVal, typedPtr, argNV.TypeAndValue.IsUnsignedInteger() != -1, elemType);
            }
        }

        // Apply field initializer: new Type { field=val, ... }
        if (auto* initList = ctx->initializerList())
        {
            // Reachable only when substitution made the element a pointer ('new T{...}' with
            // T=S*): 'typedPtr' then addresses a POINTER slot, not an 'S'.
            if (typeIsPtr)
                // The element type IS 'typeName*' here; allocating one 'typeName' is the remedy.
                LogPointerBraceInitReject(ctx, std::format("element of 'new {}'", spellNewType()),
                    typeName, spellNewType(), true);
            if (freshBraceInit)
            {
                LLVMBackend::UnwindPartialScope newBraceScope(*compiler);
                compiler->NoteUnwindPartial(compiler->GetFunction(opNewName)
                        ? LLVMBackend::UnwindPartialEntry::Kind::UserNewHeap
                        : LLVMBackend::UnwindPartialEntry::Kind::CflatHeap,
                    typedPtr, typeName, useAligned ? allocAlign : 0, sizeVal);
                if (!EmitFreshBraceInitializeAggregate(typedPtr, typeName, initList))
                    EmitFieldInitializer(typedPtr, typeName, initList);
                newBraceScope.Release();
            }
            else if (!cxxAllocator && !isArray)
            {
                // An override that throws frees the block; a constructor that completed also has its
                // object destroyed (the constructor's own block scope ended before this point).
                const bool ctorBuilt = compiler->GetFunction(typeName) != nullptr;
                LLVMBackend::UnwindPartialScope newBraceScope(*compiler);
                compiler->NoteUnwindPartial(compiler->GetFunction(opNewName)
                        ? LLVMBackend::UnwindPartialEntry::Kind::UserNewHeap
                        : LLVMBackend::UnwindPartialEntry::Kind::CflatHeap,
                    typedPtr, typeName, useAligned ? allocAlign : 0, sizeVal);
                if (ctorBuilt)
                    EmitConstructedFieldOverrides(typedPtr, typeName, initList);
                else
                    EmitFieldInitializer(typedPtr, typeName, initList);
                newBraceScope.Release();
            }
            else
                EmitFieldInitializer(typedPtr, typeName, initList);
        }

        // [winrt] object: wire lpVtbl -> static vtable and set refcount = 1 after the ctor ran.
        bool isWinrtNew = !isArray && !typeIsPtr
            && compiler->FindTypeAnnotation(typeName, "winrt") != nullptr;
        if (isWinrtNew)
            compiler->WireWinrtObject(typedPtr, typeName);

        LLVMBackend::NamedVariable result;
        result.TypeAndValue.TypeName = typeName;
        result.TypeAndValue.Pointer = true;
        result.TypeAndValue.NewAllocAlignment = cxxAllocator ? effAlign : allocAlign;
        // `new T[n]` yields a thin array-view: a fresh, whole, distinct allocation - the blessed
        // way to obtain an `int[]`. Decays implicitly to `T*` when stored into a pointer lvalue.
        result.TypeAndValue.IsArrayView = isArray;
        // A substituted POINTER element ('new T[n]' with T=S*) is an 'S*[]' view; without the
        // star the result read as an 'S[]' and bound anything an 'S[]' binds.
        result.TypeAndValue.ElemPointer = typeIsPtr;
        result.TypeAndValue.PointerDepth = isArray ? typeStars : typeStars + 1;
        result.Primary = typedPtr;
        // `new T[n]` / `new T` with T bound to a pointee-const argument (`(const char)*`).
        if (elementConstMask != 0 && typeStars > 0)
        {
            result.TypeAndValue.ConstMask = elementConstMask;
            result.Primary = compiler->MarkCastQualification(typedPtr, result.TypeAndValue,
                SpellTypeForSymbolDump(*compiler, result.TypeAndValue));
        }
        result.BaseType = ptrTy;
        result.RawArrayLength = isArray ? count : nullptr;
        // A free site does not need the alignment VALUE - only which deallocator to use, since
        // __delete_aligned recovers the raw block without it. So the tag is needed only when the
        // element TYPE alone would NOT already route there: an over-aligned type (`struct
        // alignas(64) T`) makes both `new` and `delete` pick the aligned pair off the static
        // type, exactly as in C++, so it escapes into fields/containers/returns untagged. Only a
        // per-site `alignas(N)` on an ordinarily-aligned type is invisible to every free site.
        bool typeAlreadyAligned = effAlign > LLVMBackend::kDefaultNewAlign;
        uint64_t siteExcess = (useAligned && !typeAlreadyAligned) ? allocAlign : 0;
        result.AllocAlignment = siteExcess;
        if (isArray)
            compiler->RegisterRawArrayResult(typedPtr, count, siteExcess);
        // A COM object's lifetime is refcounted via Release, not owning-pointer auto-free, so the
        // caller must NOT auto-delete it at scope exit.
        compiler->lastOwningResult = !isWinrtNew;
        compiler->lastAllocAlignment = siteExcess;
        // Ledger the result BY VALUE so an owning `new` reaching an assignment through a
        // transparent wrapper (a '?:' arm) is still recognized as owning. See ownedNewTemps_.
        if (!isWinrtNew && !isArray)
            compiler->RegisterOwnedNewTemp(typedPtr, typeName, siteExcess);
        // Ledger the element name too (arrays included): a '?:' phi or a direct `new` argument
        // carries no TypeName, and the interface upcast / view gate need one. See valueElementTypeNames_.
        compiler->RegisterValueElementTypeName(typedPtr, typeName);
        return result;
    }

bool MainListener::ElementTypeIsOverAligned(const LLVMBackend::TypeAndValue& typeAndValue) const {
        if (typeAndValue.TypeName.empty() || typeAndValue.ElemPointer)
            return false;
        LLVMBackend::TypeAndValue elem{ .TypeName = typeAndValue.TypeName };
        llvm::Type* elemType = compilerLLVM->GetType(elem);
        if (elemType == nullptr || !elemType->isSized())
            return false;
        return compilerLLVM->GetEffectiveAlignmentForType(elem.TypeName, elemType)
            > LLVMBackend::kDefaultNewAlign;
    }

std::string MainListener::SplitEnclosingStruct(const std::string& funcName, LLVMBackend* compiler) const {
        if (funcName.empty()) return "";
        if (funcName[0] == '~')
        {
            std::string typeName = funcName.substr(1);
            return compiler->IsDataStructure(typeName) ? typeName : std::string{};
        }
        auto dot = funcName.find('.');
        if (dot == std::string::npos) return "";
        std::string typeName = funcName.substr(0, dot);
        return compiler->IsDataStructure(typeName) ? typeName : std::string{};
    }

LLVMBackend::NamedVariable MainListener::ParseDeleteExpression(CFlatParser::DeleteExpressionContext* ctx) {
        auto* compiler = Compiler(ctx);
        DeclExpectedTypeScope deleteExpectedScope(&declExpectedType, {});
        bool isArray      = ctx->LeftBracket() != nullptr;
        bool hasSizeExpr  = ctx->deleteArraySize() != nullptr;
        // delete[_] ptr - free backing buffer only, no destructor calls.
        bool isRawFree    = hasSizeExpr && ctx->deleteArraySize()->expression()->getText() == "_";
        if (hasSizeExpr && ctx->deleteArraySize()->expression()->getText() == "0")
        {
            LogErrorContext(ctx, "'delete[0]' is not allowed - use 'delete[_]' to free a raw buffer without calling destructors");
            return {};
        }

        // Parse the pointer expression and determine the pointed-to struct type name.
        // getPointerElementType() is unavailable with LLVM opaque pointers; use the
        // AST-level type information from the unary expression instead.
        std::string typeName;
        bool elemIsPtr = false;         // true when deleting array of pointers (e.g. list<T*> buffer)
        bool isInterfaceOperand = false; // true when the operand is an interface fat-ptr value
        llvm::Value* ptrVal = nullptr;
        llvm::Value* srcAlloca = nullptr;
        llvm::Type* srcAllocaElemType = nullptr;
        LLVMBackend::NamedVariable operandNamedVar;
        std::string targetName;         // name of the deleted local (empty for field/expr targets)
        uint64_t operandAllocAlign = 0; // per-site over-alignment carried on the operand (`new T[n] alignas(N)`)

        // Peel leading casts so 'delete[_] (T*)p' recovers the underlying owning local and its alloca;
        // without this, a cast target drops storage/ownership info and double-frees at scope exit.
        CFlatParser::UnaryExpressionContext* ue = tryGetUnaryExpression(ctx->expression());
        std::string castTypeName;
        bool castElemPtr = false;
        bool sawCast     = false;
        if (ue == nullptr)
        {
            auto* castCtx = tryGetCastExpression(ctx->expression());
            while (castCtx != nullptr
                   && castCtx->castTypeName() != nullptr
                   && castCtx->castExpression() != nullptr && !CastIsCallSpelling(castCtx))
            {
                if (!sawCast)
                {
                    auto destTV  = ParseCastTypeName(castCtx->castTypeName(), false, castCtx);
                    castTypeName = destTV.TypeName;
                    castElemPtr  = destTV.ElemPointer;
                    sawCast      = true;
                }
                auto* inner = castCtx->castExpression();
                ue = tryGetUnaryExpression(inner);
                if (ue != nullptr) break;
                castCtx = tryGetCastExpression(inner);
            }
        }

        if (ue != nullptr)
        {
            auto namedVar = ParseUnaryExpression(ue);
            operandNamedVar = namedVar;
            // `delete w->get();` - an unbound call result, so nothing classified it as a borrow.
            ApplyCallResultBorrowProvenance(compiler, namedVar);
            typeName  = namedVar.TypeAndValue.TypeName;
            elemIsPtr = namedVar.TypeAndValue.ValuePointerDepth() >= 2;
            // A DECLARED 'alignas(0, N)' clause counts as well as a tracked block alignment: the
            // scope-exit path (EmitOwningPtrCleanup) already frees on the max of the two, so an
            // explicit 'delete' of the same local must reach the same deallocator.
            operandAllocAlign = std::max(namedVar.AllocAlignment,
                                         namedVar.TypeAndValue.AllocAlignValue);
            // An interface value is a {vtable, data} fat struct, not a raw pointer; flag it so
            // the free path below extracts the data pointer and dispatches the dtor virtually.
            isInterfaceOperand = namedVar.TypeAndValue.IsFatInterfaceValue() && !sawCast;

            // Error: 'delete' on a value-type local (a struct VALUE, not a pointer). Value
            // types are auto-destructed at scope exit, so this is always a user error; without
            // this check the operand flows into icmp/call/bitcast instructions that require a
            // pointer and LLVM's module verifier fails with no source location.
            // An `alias` DIRECT call result of blessed unique<X> (list's `alias T get`) is a
            // borrow, not a local; it belongs to the alias-result gate further below.
            bool aliasCoreUniqueResult = (namedVar.TypeAndValue.IsAlias || namedVar.PointsToAliasBorrow)
                && (namedVar.Storage == nullptr || namedVar.CallerName.empty())
                && !namedVar.TypeAndValue.Pointer && compiler->IsCoreUniqueType(typeName);
            if (!namedVar.TypeAndValue.Pointer && !isInterfaceOperand && !aliasCoreUniqueResult)
            {
                if (compiler->IsCoreUniqueType(typeName))
                {
                    bool isField = !namedVar.FieldName.empty()
                        || llvm::isa_and_nonnull<llvm::GetElementPtrInst>(namedVar.Storage);
                    if (isField)
                    {
                        std::string fieldName = namedVar.FieldName.empty()
                            ? namedVar.TypeAndValue.VariableName : namedVar.FieldName;
                        std::string owner = namedVar.OwningStructName.empty()
                            ? SplitEnclosingStruct(compiler->GetCurrentFunctionName(), compiler)
                            : namedVar.OwningStructName;
                        std::string fieldDesc = owner.empty()
                            ? std::format("'{}'", fieldName)
                            : std::format("'{}.{}'", owner, fieldName);
                        LogErrorContext(ctx, std::format(
                            "cannot delete unique field {} - the synthesized destructor already deletes it, "
                            "so this is a double-free. Assign '{} = nullptr;' to release it instead.",
                            fieldDesc, fieldName));
                        return {};
                    }
                    LogErrorContext(ctx, std::format(
                        "cannot delete unique local '{}' - a unique local is freed automatically at "
                        "scope exit (and reassigning it frees the current object first), so an explicit "
                        "delete is unnecessary. Let it go out of scope to release it.",
                        namedVar.CallerName.empty() ? "<expr>" : namedVar.CallerName));
                    return {};
                }
                std::string displayType = SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = typeName });
                LogErrorContext(ctx, std::format(
                    "cannot 'delete' value-type local '{}' of type '{}' - value types are "
                    "destructed automatically at scope exit; 'delete' applies to pointers "
                    "from 'new'",
                    namedVar.CallerName.empty() ? "<expr>" : namedVar.CallerName, displayType));
                return {};
            }

            // Error: deleting an array-view local whose DECLARATION bound it from stack/global
            // fixed-array storage ('int[3] a; int[] v = a;' or the 'auto' spelling deducing the
            // same view), and which was never reassigned since (SetViewOfFixedArrayStorage
            // clears the flag for good on any later '='). A T[] view is a thin T* with no
            // runtime tag distinguishing a heap allocation from decayed fixed-array storage, so
            // without this the operand reaches free() with a non-heap address and aborts with no
            // diagnostic. A view whose origin is a parameter, a field, a call result, a
            // conditional join, or ANY reassigned local (even one only conditionally reassigned,
            // or reassigned back to a fixed array) is never flagged here and stays compiling -
            // rejecting a reassigned local would be a reject on a MAY-alias, not a proof, which
            // is unsound in the reject direction. Matches the RejectRawPointerToArrayView guard's
            // polarity. Scoped to !sawCast: casting a stack-bound view ('delete[_] (int*)v;') is
            // a known, deliberately unproven hole - see the cast-peeling comment above.
            if (!sawCast && namedVar.TypeAndValue.IsArrayView && namedVar.ViewOfFixedArrayStorage)
            {
                std::string name = namedVar.CallerName.empty()
                    ? namedVar.TypeAndValue.VariableName : namedVar.CallerName;
                std::string boundFrom = namedVar.ViewOfFixedArraySourceName.empty()
                    ? std::string("a stack or global fixed array")
                    : std::format("'{}', a stack or global fixed array", namedVar.ViewOfFixedArraySourceName);
                LogErrorContext(ctx, std::format(
                    "cannot delete array-view '{}' - it was bound from {}. That source is fixed "
                    "backing storage, not a heap allocation, so 'delete' would hand free() a "
                    "non-heap address. "
                    "Only a view from 'new {}[n]' "
                    "can be deleted.",
                    name, boundFrom, SpellType(*compiler, namedVar.TypeAndValue)));
                return {};
            }

            // A cast target renames the element type; honor it for destructor selection.
            if (sawCast)
            {
                typeName  = castTypeName;
                elemIsPtr = castElemPtr;
            }

            // Error: deleting the DIRECT result of a call whose callee returns 'alias' (a borrow the
            // owner retains, e.g. list.get's `alias T get`). The owner still holds the slot and
            // frees its resources at its own dtor - deleting it here would double-free. Non-pointer
            // alias results now have Storage because the ABI is a reference; an alias local has a
            // non-empty CallerName and is not this direct-call case.
            // `&aliasResult` drops the alias markers (it is a plain borrowed pointer for stores),
            // so the provenance flag is the form this gate sees for an address-of operand.
            if ((namedVar.TypeAndValue.IsAlias || namedVar.PointsToAliasBorrow)
                && (namedVar.Storage == nullptr || namedVar.CallerName.empty()))
            {
                std::string calleeName = DeleteOperandCalleeName(ue);
                // A borrowing container hands back a borrow even from take()/removeAt(), so
                // steering to them would be circular - point at the real owner instead.
                bool calleeIsOwningAccessor = (calleeName == "take" || calleeName == "removeAt");
                LogErrorContext(ctx, std::format(
                    "cannot delete the alias (borrowed) result of '{}': the owner still holds it, so "
                    "this delete would double-free when the owner frees it. {}",
                    calleeName.empty() ? "<call>" : calleeName,
                    calleeIsOwningAccessor
                        ? "This container only borrows its elements, so it has no owning accessor - "
                          "free through the owner, or use an owning container such as 'list<unique T*>'."
                        : "Use an owning accessor such as take()/removeAt(), or let the owner free it."));
                return {};
            }

            // Error: deleting a pointer whose ownership was boxed into an interface ('IFace x = ptr').
            // The boxing transferred ownership to the interface and nulled the source, so this
            // 'delete ptr' frees nothing; and because interface locals are not auto-destructed, the
            // object leaks unless the interface itself is deleted. Scoped to the interface-box case
            // (MovedIntoInterface) - a plain move into a 'move' param or a view alias is NOT flagged,
            // since deleting the source there is either correct or a harmless no-op.
            if (!sawCast && namedVar.MovedIntoInterface)
            {
                LogErrorContext(ctx, std::format(
                    "cannot delete '{}' - its ownership was boxed into an interface, so this delete "
                    "frees nothing and the object leaks (interface locals are not auto-destructed). "
                    "Delete through the interface instead.",
                    namedVar.CallerName.empty() ? "<expr>" : namedVar.CallerName));
                return {};
            }

            // Error: deleting an interface box whose object a DIFFERENT owner already frees - see
            // BorrowedInterfaceBox. Proven at the binding site; never inferred at this one.
            if (isInterfaceOperand && namedVar.BorrowedInterfaceBox
                && InterfaceBoxBorrowStillKeepsOwner(namedVar)
                && namedVar.Storage != nullptr && llvm::isa<llvm::AllocaInst>(namedVar.Storage))
            {
                std::string name = namedVar.CallerName.empty()
                    ? namedVar.TypeAndValue.VariableName : namedVar.CallerName;
                if (name.empty()) name = "<expr>";
                std::string owner = namedVar.BorrowedInterfaceBoxSource.empty()
                    ? std::string("the binding it was boxed from")
                    : namedVar.BorrowedInterfaceBoxSource;
                LogErrorContext(ctx, std::format(
                    "cannot delete interface '{}' - it boxes an object that {} already frees, so this "
                    "is a double-free. Remove this delete and let {} release it; box a 'new' object "
                    "into '{}' instead if this frame should own it.",
                    name, owner, owner, name));
                return {};
            }

            // Error: deleting a local bound to a container-owned element (list<unique X*>.get).
            // The container owns the pointee and its destructor frees it, so this delete is a
            // guaranteed double-free. Binding the borrow to a named local defeated the direct-call
            // check; this tag (set at decl-init) catches the laundered form.
            if (compiler->OwnedElementBorrowReaches(namedVar)
                && namedVar.Storage != nullptr
                && llvm::isa<llvm::AllocaInst>(namedVar.Storage))
            {
                LogErrorContext(ctx, compiler->OwnedElementDeleteMessage(namedVar));
                return {};
            }

            // Error: deleting a borrowed (non-move) parameter directly. If the caller owns the
            // pointer, it will be freed again when the caller's scope exits - double-free.
            // Restrict to direct alloca storage (simple variable reference, not field access)
            // so 'delete param->field' is still allowed.
            if (!namedVar.CallerName.empty()
                && !namedVar.IsOwning
                && namedVar.Storage != nullptr
                && llvm::isa<llvm::AllocaInst>(namedVar.Storage)
                && compiler->IsFunctionParameter(namedVar.CallerName)
                && !compiler->BorrowProofRetiredByRebind(namedVar))
            {
                LogErrorContext(ctx, std::format(
                    "cannot delete borrowed parameter '{}' - caller may own this pointer and "
                    "will free it on scope exit. Declare the parameter 'move {}' to take ownership.",
                    namedVar.CallerName, namedVar.CallerName));
                return {};
            }

            // Deleting a call result PROVEN to borrow a live `unique` field. It has no Storage at
            // all, so the own-slot clause below cannot see it; the field is the one owner either way.
            if (!namedVar.IsOwning && namedVar.BorrowedUniqueFieldViaCall
                && !namedVar.BorrowedUniqueField.empty())
            {
                LogErrorContext(ctx, std::format(
                    "cannot delete this call result - it was returned as a borrow of unique field "
                    "'{}', whose synthesized destructor already frees it, so this is a double-free. "
                    "Remove this delete and let the field's destructor free it.",
                    namedVar.BorrowedUniqueField));
                return {};
            }

            // Error: deleting a local that aliases a borrowed parameter (e.g. via cast).
            // 'Payload* p = (Payload*)raw; delete p;' is the laundered form of the case above.
            // Exempt field accesses (storage is a GEP, not an alloca): 'delete param->field'
            // is intentionally allowed - the field's lifetime is the caller's contract, just
            // like the simple-variable check above.
            // An address-of proof (`p = &x;`) owns the message for its own borrow: the address-of
            // check below names it, and a borrow recorded from the same store must not pre-empt it.
            if (!namedVar.IsOwning
                && namedVar.IsBorrowed
                && !namedVar.BorrowedOrigin.empty()
                && !namedVar.PointsToBorrowedAddress
                && namedVar.Storage != nullptr
                && llvm::isa<llvm::AllocaInst>(namedVar.Storage)
                && !compiler->BorrowProofRetiredByRebind(namedVar))
            {
                std::string name = namedVar.CallerName.empty() ? namedVar.BorrowedOrigin : namedVar.CallerName;
                // Trap B: the borrow came from a `unique` field, not a parameter. The field's
                // synthesized destructor is the owner, so name it and point at `move` instead.
                if (!namedVar.BorrowedUniqueField.empty() && namedVar.BorrowedUniqueFieldViaCall)
                    LogErrorContext(ctx, std::format(
                        "cannot delete '{}' - it was returned as a borrow of unique field '{}', whose "
                        "synthesized destructor already frees it, so this is a double-free. Remove "
                        "this delete and let the field's destructor free it.",
                        name, namedVar.BorrowedUniqueField));
                else if (!namedVar.BorrowedUniqueField.empty())
                    LogErrorContext(ctx, std::format(
                        "cannot delete '{}' - it aliases unique field '{}', whose synthesized destructor "
                        "already frees it, so this is a double-free. Use 'move {}' to take ownership out "
                        "of the field (which nulls it), or let the field's destructor free it.",
                        name, namedVar.BorrowedUniqueField, namedVar.BorrowedOrigin));
                else
                    LogErrorContext(ctx, std::format(
                        "cannot delete '{}' - it aliases borrowed parameter '{}'. The caller may own "
                        "this pointer and will free it on scope exit. Declare the source parameter "
                        "'move {}' to take ownership.",
                        name, namedVar.BorrowedOrigin, namedVar.BorrowedOrigin));
                return {};
            }

            /*
             * q11: the GLOBAL spelling of the check just above (`g = p; delete g;`). The fact
             * lives in the per-function global ledger rather than on a NamedVariable, because
             * GetGlobalVariableNV builds a fresh NV per lookup and carries no borrow bits.
             * Same-function only by construction.
             */
            if (!namedVar.IsOwning
                && namedVar.Storage != nullptr
                && llvm::isa<llvm::GlobalVariable>(namedVar.Storage))
            {
                auto gb = compiler->globalAssignBorrowOrigin_.find(namedVar.Storage);
                if (gb != compiler->globalAssignBorrowOrigin_.end())
                {
                    std::string name = namedVar.CallerName.empty()
                        ? namedVar.Storage->getName().str() : namedVar.CallerName;
                    LogErrorContext(ctx, std::format(
                        "cannot delete '{}' - it aliases borrowed parameter '{}'. The caller may own "
                        "this pointer and will free it on scope exit. Declare the source parameter "
                        "'move {}' to take ownership.",
                        name, gb->second, gb->second));
                    return {};
                }
            }

            /*
             * Error: deleting a pointer PROVEN to hold an address-of result. The provenance is
             * recorded where the value is PRODUCED (RegisterBorrowedAddressValue) and carried on
             * the binding by the declaration and '=' doors, which retire it on any other RHS - so
             * a rebind to `new` is not poisoned. Three spellings share the one proof: the named
             * local's flag, the direct `delete &x;` whose operand value IS the address-of result,
             * and the global destination's per-function ledger twin.
             */
            bool operandIsBorrowedAddress = false;
            if (namedVar.Storage == nullptr)
                operandIsBorrowedAddress = compiler->IsBorrowedAddressValue(namedVar.Primary);
            else if (llvm::isa<llvm::AllocaInst>(namedVar.Storage))
                operandIsBorrowedAddress = namedVar.PointsToBorrowedAddress;
            else if (llvm::isa<llvm::GlobalVariable>(namedVar.Storage))
                operandIsBorrowedAddress =
                    compiler->globalAssignBorrowedAddress_.count(namedVar.Storage) != 0;
            if (!namedVar.IsOwning && operandIsBorrowedAddress)
            {
                std::string name = namedVar.CallerName.empty()
                    ? namedVar.TypeAndValue.VariableName : namedVar.CallerName;
                if (name.empty()) name = "<expr>";
                LogErrorContext(ctx, std::format(
                    "cannot delete '{}' - the address-of expression it comes from borrows storage "
                    "it does not own (a stack local, global, field or array element), so handing "
                    "that address to free() corrupts the "
                    "heap. Only a pointer from 'new' may be deleted.",
                    name));
                return {};
            }

            // Error: deleting a plain copy of an OWNING local (`T* b = c;`, `alias T* b = c;`). The
            // source still frees the pointee at its own scope exit, so this delete double-frees.
            // Retired by any reassignment: a rebound copy is the sole owner of what it now holds.
            if (compiler->OwningLocalCopyStillAliases(namedVar)
                && namedVar.Storage != nullptr
                && llvm::isa<llvm::AllocaInst>(namedVar.Storage))
            {
                std::string name = namedVar.CallerName.empty()
                    ? namedVar.TypeAndValue.VariableName : namedVar.CallerName;
                if (name.empty()) name = "<expr>";
                LogErrorContext(ctx, std::format(
                    "cannot delete '{}' - it copies '{}', which still owns the object and frees it at "
                    "scope exit, so this is a double-free. Delete '{}' instead, or use 'move {}' to "
                    "take ownership out of it (which nulls it).",
                    name, namedVar.OwningLocalOrigin, namedVar.OwningLocalOrigin,
                    namedVar.OwningLocalOrigin));
                return {};
            }

            // Error: deleting a local bound from a '?:' / '??' JOIN whose every non-null arm proves
            // another owner. The join carries no source binding, so the proof is recorded where the
            // arms are in hand - the declaration, or the '=' that rebound this local - and re-asked
            // here. Refreshed by every later '=', so it never outlives its own store.
            if (!namedVar.IsOwning
                && JoinArmsStillKeepOwner(namedVar)
                && !namedVar.JoinKeepsOwnerSource.empty()
                && namedVar.Storage != nullptr
                && llvm::isa<llvm::AllocaInst>(namedVar.Storage))
            {
                std::string name = namedVar.CallerName.empty()
                    ? namedVar.TypeAndValue.VariableName : namedVar.CallerName;
                if (name.empty()) name = "<expr>";
                LogErrorContext(ctx, std::format(
                    "cannot delete '{}' - every arm of the join it was bound from holds an object {} "
                    "already frees, so this is a double-free. Remove this delete and let {} release "
                    "it, or bind '{}' to an object this frame owns.",
                    name, namedVar.JoinKeepsOwnerSource, namedVar.JoinKeepsOwnerSource, name));
                return {};
            }

            // Error: deleting a `unique` field. The synthesized destructor calls the user dtor
            // FIRST and then deletes the field, so a hand-written `delete _root;` double-frees -
            // exactly what a migration produces when someone adds `unique` and forgets to remove
            // the old destructor. Keyed on IsUnique rather than FieldName: a bare self-field
            // access (`delete _root;` inside the struct's own method - the case that matters, and
            // the one the from-outside rule below cannot see) comes from GetMemberVariable, which
            // purposely leaves OwningStructName/FieldName empty. `unique` now also lands on LOCALS
            // and params; the alloca-storage test inside splits those out to a local-specific
            // message. Checked ahead of the from-outside rule so the precise reason wins for inside
            // and outside deletes alike.
            std::string enclosing = SplitEnclosingStruct(compiler->GetCurrentFunctionName(), compiler);
            bool isRawUnionMember = false;
            if (!namedVar.TypeAndValue.ParentVariableName.empty())
            {
                auto enclosingIt = compiler->dataStructures.find(enclosing);
                if (enclosingIt != compiler->dataStructures.end() && enclosingIt->second.IsUnion)
                {
                    for (const auto& field : enclosingIt->second.StructFields)
                    {
                        if (field.VariableName == namedVar.TypeAndValue.ParentVariableName)
                        {
                            isRawUnionMember = true;
                            break;
                        }
                    }
                }
            }
            // The blessed unique<X> is always a VALUE: a `unique<X>*` names a heap ARRAY of
            // wrappers (a container's element buffer), which its owner does delete by hand.
            if ((namedVar.TypeAndValue.IsUnique
                    || (compiler->IsCoreUniqueType(namedVar.TypeAndValue.TypeName)
                        && !namedVar.TypeAndValue.Pointer))
                && !isRawUnionMember)
            {
                // A `unique` LOCAL (alloca storage, no field name) is freed automatically at scope
                // exit, and reassignment frees the current pointee first. Distinguish it from the
                // field case (whose storage is a GEP off `this`) so the advice fits.
                bool isField = !namedVar.FieldName.empty()
                    || llvm::isa_and_nonnull<llvm::GetElementPtrInst>(namedVar.Storage);
                bool isLocal = !isField && namedVar.Storage != nullptr
                    && llvm::isa<llvm::AllocaInst>(namedVar.Storage)
                    && namedVar.FieldName.empty();
                if (isLocal)
                {
                    std::string nm = namedVar.CallerName.empty()
                        ? namedVar.TypeAndValue.VariableName : namedVar.CallerName;
                    LogErrorContext(ctx, std::format(
                        "cannot delete unique local '{}' - a unique local is freed automatically at "
                        "scope exit (and reassigning it frees the current object first), so an explicit "
                        "delete is unnecessary. Let it go out of scope to release it.",
                        nm));
                    return {};
                }
                std::string fieldName = namedVar.FieldName.empty()
                    ? namedVar.TypeAndValue.VariableName : namedVar.FieldName;
                std::string owner = namedVar.OwningStructName.empty()
                    ? enclosing
                    : namedVar.OwningStructName;
                std::string fieldDesc = owner.empty()
                    ? std::format("'{}'", fieldName)
                    : std::format("'{}.{}'", owner, fieldName);
                LogErrorContext(ctx, std::format(
                    "cannot delete unique field {} - the synthesized destructor already deletes it, so "
                    "this is a double-free. An explicit delete is never needed: assigning '{} = nullptr;' "
                    "frees the current pointee, and '{} = new T();' frees it and re-roots. Remove this "
                    "delete, or drop 'unique' from the field to manage it by hand.",
                    fieldDesc, fieldName, fieldName));
                return {};
            }

            // Error: deleting a struct field from outside the owning struct's own methods.
            // Encapsulation: only the owner is allowed to free its own pointer fields directly.
            // External callers must extract with 'T* p = move obj->field; delete p;' so the
            // ownership transfer is visible at the call site and the owner sees nullptr.
            if (!namedVar.OwningStructName.empty()
                && namedVar.Storage != nullptr
                && !llvm::isa<llvm::AllocaInst>(namedVar.Storage)
                && !isRawUnionMember)
            {
                if (enclosing != namedVar.OwningStructName)
                {
                    LogErrorContext(ctx, std::format(
                        "cannot delete field '{}.{}' from outside '{}'. Extract it first with "
                        "'T* p = move expr; delete p;' so the ownership transfer is explicit.",
                        namedVar.OwningStructName, namedVar.FieldName, namedVar.OwningStructName));
                    return {};
                }
            }

            if (namedVar.Storage)
            {
                ptrVal = compiler->CreateLoad(namedVar.Storage);
                compiler->NoteOwnSlotLeavingLoad(ptrVal);   // `delete p` frees what it read
                if (llvm::isa<llvm::AllocaInst>(namedVar.Storage))
                {
                    // The block is freed here, so a '??' fallback owner bound to this slot must not free it again.
                    compiler->NoteFallbackRootConsumed(namedVar.Storage);
                    srcAlloca = namedVar.Storage;
                    srcAllocaElemType = namedVar.BaseType;
                    targetName = namedVar.CallerName;
                }
            }
            else
            {
                ptrVal = namedVar.Primary;
            }
        }
        else
        {
            ptrVal = ParseExpression(ctx->expression());
        }
        if (!ptrVal) return {};

        // Interface fat-pointer operand: ptrVal is a {vtable, data} struct value. Extract the
        // data pointer, run the concrete destructor via the vtable, and free - a plain bitcast
        // of the fat struct to a raw pointer is invalid. srcAlloca (the local's storage) gets its
        // data field nulled so scope-exit / a second delete cannot double-free.
        if (isInterfaceOperand && !isArray)
        {
            compiler->DeleteInterfaceValue(ptrVal, typeName, srcAlloca);
            return {};
        }

        // Reject destructive delete of a raw 'string' local: string has no runtime ownership flag
        // so slots may hold borrowed text. srcAlloca gate leaves container internals (list/array) unaffected.
        if (typeName == "string" && !isRawFree && !elemIsPtr && srcAlloca != nullptr)
        {
            std::string named = targetName.empty()
                ? std::string("this 'string' buffer")
                : std::format("the 'string' buffer '{}'", targetName);
            std::string freeHint = targetName.empty()
                ? std::string("'delete[_] ptr'")
                : std::format("'delete[_] {}'", targetName);
            LogErrorContext(ctx, std::format(
                "cannot destructively 'delete' {} - a raw 'new string[n]' does not take "
                "ownership of assigned strings (unlike list/dictionary), so its slots may hold "
                "borrowed text (a string literal, a view, or an alias of another buffer) and "
                "running the string destructor on them would free non-owned memory and corrupt "
                "the heap. Use {} to free the buffer without destructing elements, or use "
                "'list<string>', which owns and frees its strings.",
                named, freeHint));
            return {};
        }

        // Error: bare delete[] on a named struct array - the caller must supply the count.
        if (isArray && !hasSizeExpr && compiler->IsDataStructure(typeName) && !elemIsPtr)
        {
            LogErrorContext(ctx, std::format(
                "'delete[]' is not allowed for struct type '{}' - use 'delete[n] ptr' to call destructors or 'delete[_] ptr' to free the raw buffer",
                SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = typeName })));
            return {};
        }

        /*
         * M6 - `delete p` where p's static type is a foreign C++ class with a VIRTUAL destructor.
         * Itanium puts a DELETING destructor (D0) in the vtable next to the complete-object one;
         * it runs the derived destructor chain AND releases the storage through the C++
         * deallocator. So this is the whole operation: no CFlat destructor call, no operator
         * delete. Getting this wrong through a base pointer would run the base destructor only
         * and free with the wrong size.
         */
        // A C++ class block's element count, when this operand carries one: it decides between
        // the single-object and the array deallocation function below.
        const bool cxxAllocator = !elemIsPtr && compiler->CxxClassUsesCxxAllocator(typeName);
        const bool viewOperand = !sawCast && operandNamedVar.TypeAndValue.IsArrayView;
        llvm::Value* cxxArrayCount = nullptr;
        if (cxxAllocator && !isArray)
        {
            cxxArrayCount = compiler->RawArrayCountOf(ptrVal);
            if (cxxArrayCount == nullptr && (operandNamedVar.RawArrayLengthStorage != nullptr
                                             || operandNamedVar.RawArrayLength != nullptr))
                cxxArrayCount = compiler->LoadRawArrayLength(operandNamedVar);
        }
        // A `new T[n]` block is never released through the deleting destructor of element 0.
        auto* constCount = llvm::dyn_cast_or_null<llvm::ConstantInt>(cxxArrayCount);
        const bool knownCxxArray = cxxAllocator
            && (viewOperand || (constCount != nullptr && !constCount->isNegative()));
        if (cxxAllocator && !viewOperand && !isArray && !isRawFree && !elemIsPtr
            && cxxArrayCount != nullptr && constCount == nullptr
            && compiler->CxxHasVirtualDestructor(typeName))
        {
            auto* fn = compiler->builder->GetInsertBlock()->getParent();
            auto* arrayBB = llvm::BasicBlock::Create(*compiler->context, "cxxdelete.runtime.array", fn);
            auto* singleBB = llvm::BasicBlock::Create(*compiler->context, "cxxdelete.runtime.single", fn);
            auto* doneBB = llvm::BasicBlock::Create(*compiler->context, "cxxdelete.runtime.done", fn);
            auto* nonNullBB = llvm::BasicBlock::Create(*compiler->context, "cxxdelete.runtime.nonnull", fn);
            auto* nullPtr = llvm::ConstantPointerNull::get(
                llvm::cast<llvm::PointerType>(ptrVal->getType()));
            auto* count = compiler->builder->CreateSExtOrTrunc(
                cxxArrayCount, compiler->builder->getInt64Ty());
            compiler->builder->CreateCondBr(
                compiler->builder->CreateICmpEQ(ptrVal, nullPtr), doneBB, nonNullBB);
            compiler->builder->SetInsertPoint(nonNullBB);
            compiler->builder->CreateCondBr(
                compiler->builder->CreateICmpSGE(count, compiler->builder->getInt64(0)),
                arrayBB, singleBB);
            compiler->builder->SetInsertPoint(arrayBB);
            compiler->EmitCountedArrayDestruction(ptrVal, typeName, count);
            auto* voidPtr = compiler->builder->CreateBitCast(
                ptrVal, cflat_llvm::PointerTo(compiler->builder->getInt8Ty()));
            compiler->EmitCxxHeapFreeArray(typeName, voidPtr, count, operandAllocAlign);
            compiler->builder->CreateBr(doneBB);
            compiler->builder->SetInsertPoint(singleBB);
            compiler->EmitCxxVirtualDelete(typeName, ptrVal);
            compiler->builder->CreateBr(doneBB);
            compiler->builder->SetInsertPoint(doneBB);
            if (srcAlloca != nullptr && srcAllocaElemType != nullptr)
                if (auto* ptrTy = llvm::dyn_cast<llvm::PointerType>(srcAllocaElemType))
                {
                    compiler->builder->CreateStore(llvm::ConstantPointerNull::get(ptrTy), srcAlloca);
                    compiler->StoreRawArrayLength(operandNamedVar, nullptr);
                }
            return {};
        }
        if (!isArray && !isRawFree && !elemIsPtr && !typeName.empty() && !knownCxxArray
            && compiler->CxxHasVirtualDestructor(typeName))
        {
            auto* nullPtr = llvm::ConstantPointerNull::get(
                llvm::cast<llvm::PointerType>(ptrVal->getType()));
            auto* isNull  = compiler->builder->CreateICmpEQ(ptrVal, nullPtr, "vdel_isnull");
            auto* delBB   = compiler->CreateBasicBlock("vdel");
            auto* contBB  = compiler->CreateBasicBlock("vdel_cont");
            compiler->builder->CreateCondBr(isNull, contBB, delBB);
            compiler->builder->SetInsertPoint(delBB);
            const bool emitted = compiler->EmitCxxVirtualDelete(typeName, ptrVal);
            compiler->builder->CreateBr(contBB);
            compiler->builder->SetInsertPoint(contBB);
            if (emitted)
            {
                // Same source-alloca nulling step 4 does, so scope-exit cleanup cannot free twice.
                if (srcAlloca != nullptr && srcAllocaElemType != nullptr)
                    if (auto* ptrTy = llvm::dyn_cast<llvm::PointerType>(srcAllocaElemType))
                    {
                        compiler->builder->CreateStore(
                            llvm::ConstantPointerNull::get(ptrTy), srcAlloca);
                        compiler->StoreRawArrayLength(operandNamedVar, nullptr);
                    }
                return {};
            }
        }

        // Inside a loop the backedge may still bring a borrow to this delete: re-check it when the
        // loop closes (LLVMBackend::EndDeferredLoop).
        if (!loopBreakMovedStates_.empty() && operandNamedVar.Storage != nullptr
            && llvm::isa<llvm::AllocaInst>(operandNamedVar.Storage))
            compiler->DeferOwnedElementDelete(operandNamedVar, static_cast<int>(ctx->getStart()->getLine()),
                static_cast<int>(ctx->getStart()->getCharPositionInLine()));

        // 1. Call the full destructor (user dtor + member fields) if needed (non-array only).
        // Guard on null: 'delete nullptr' must be a no-op (operator delete below already
        // null-checks). Without this, the destructor would dereference a null pointer - a
        // recursive tree teardown ('delete left; delete right;' with null leaf children)
        // would crash on the first null child.
        if (!isArray && !typeName.empty())
        {
            if (compiler->GetFullDestructorForDelete(typeName) != nullptr)
            {
                auto* nullPtr = llvm::ConstantPointerNull::get(
                    llvm::cast<llvm::PointerType>(ptrVal->getType()));
                auto* isNull  = compiler->builder->CreateICmpEQ(ptrVal, nullPtr, "del_isnull");
                auto* dtorBB  = compiler->CreateBasicBlock("del_dtor");
                auto* contBB  = compiler->CreateBasicBlock("del_dtor_cont");
                compiler->builder->CreateCondBr(isNull, contBB, dtorBB);
                compiler->builder->SetInsertPoint(dtorBB);
                compiler->EmitOwningPtrDestructor(
                    operandNamedVar, ptrVal, typeName, compiler->RawArrayCountOf(ptrVal));
                compiler->builder->CreateBr(contBB);
                compiler->builder->SetInsertPoint(contBB);
            }
        }

        // 1b. For delete[n]: call ~T() on each element using the caller-supplied count.
        // elemIsPtr suppresses destructor calls for pointer-element arrays (e.g. list<T*> buffer).
        llvm::Value* freeBase = ptrVal;
        llvm::Value* sizeExprCount = nullptr;
        if (hasSizeExpr && !isRawFree && compiler->IsDataStructure(typeName) && !elemIsPtr)
        {
            llvm::Function* elemDtor = compiler->GetFullDestructorForDelete(typeName);
            if (elemDtor)
            {
                llvm::Value* arrCount = ParseExpression(ctx->deleteArraySize()->expression());
                sizeExprCount = arrCount;
                compiler->EmitCountedArrayDestruction(ptrVal, typeName, arrCount);
            }
        }
        if (cxxAllocator && hasSizeExpr && !isRawFree && sizeExprCount == nullptr)
            sizeExprCount = ParseExpression(ctx->deleteArraySize()->expression());

        // 2. Convert free base to void*
        auto* voidPtrTy = cflat_llvm::PointerTo(compiler->builder->getInt8Ty());
        llvm::Value* voidPtr = compiler->builder->CreateBitCast(freeBase, voidPtrTy, "freeptr");

        // 3. Call operator delete: class-specific -> global. When the static
        // type carries effective alignment > 16 (the default-new threshold),
        // route to `operator delete_aligned` so it matches the aligned new path.
        std::string opDelName = typeName + ".operator delete";
        LLVMBackend::NamedVariable ptrArg;
        ptrArg.Primary = voidPtr;
        ptrArg.BaseType = voidPtrTy;
        uint64_t deleteEffAlign = 0;
        if (!typeName.empty() && !elemIsPtr)
        {
            LLVMBackend::TypeAndValue tv{ .TypeName = typeName };
            llvm::Type* t = compiler->GetType(tv);
            if (t != nullptr && t->isSized())
                deleteEffAlign = compiler->GetEffectiveAlignmentForType(typeName, t);
        }
        // Per-site over-alignment (`new T[n] alignas(N)`) is not in the static type, so
        // fold in the alignment carried on the operand; either source routes to __delete_aligned.
        if (operandAllocAlign > deleteEffAlign) deleteEffAlign = operandAllocAlign;
        bool useAlignedDelete = deleteEffAlign > LLVMBackend::kDefaultNewAlign;
        // A foreign nontrivial C++ object came from the C++ global allocator, so it must go back
        // to the matching C++ operator delete (sized, and over-aligned where the class is).
        // `delete[..]` and a view free a `new T[n]` block; a bare `delete p` decides by the raw
        // array count it carries (none = one object).
        if (cxxAllocator && (isArray || viewOperand))
        {
            llvm::Value* n = sizeExprCount;
            if (n == nullptr) n = compiler->RawArrayCountOf(ptrVal);
            if (n == nullptr && (operandNamedVar.RawArrayLengthStorage != nullptr
                                 || operandNamedVar.RawArrayLength != nullptr))
                n = compiler->LoadRawArrayLength(operandNamedVar);
            if (!compiler->EmitCxxHeapFreeArray(typeName, voidPtr, n, operandAllocAlign))
                LogErrorContext(ctx, std::format(
                    "cannot free this 'new {}[n]' block: the class declares only a sized "
                    "'operator delete[](void*, size_t)', which needs the element count, and this "
                    "operand carries none - write 'delete[n] ptr' with the count",
                    SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = typeName })));
        }
        else if (cxxAllocator)
        {
            compiler->EmitCxxHeapFreeCounted(typeName, voidPtr, cxxArrayCount, operandAllocAlign);
        }
        else if (!elemIsPtr && !typeName.empty() && compiler->GetFunction(opDelName))
        {
            compiler->CreateOverloadedFunctionCall(opDelName, { ptrArg });
        }
        else if (useAlignedDelete && compiler->GetFunction("__delete_aligned"))
        {
            compiler->CreateOverloadedFunctionCall("__delete_aligned", { ptrArg });
        }
        else if (compiler->GetFunction("operator delete"))
        {
            compiler->CreateOverloadedFunctionCall("operator delete", { ptrArg });
        }
        else
        {
            LogErrorContext(ctx, "'delete' requires 'operator delete' to be defined");
        }

        // 4. Null the source alloca so scope-exit cleanup (IsNewAllocated) doesn't double-free.
        if (srcAlloca && srcAllocaElemType)
        {
            if (auto* ptrTy = llvm::dyn_cast<llvm::PointerType>(srcAllocaElemType))
            {
                compiler->builder->CreateStore(
                    llvm::ConstantPointerNull::get(ptrTy), srcAlloca);
                compiler->StoreRawArrayLength(operandNamedVar, nullptr);
            }
        }
        else if (operandNamedVar.TypeAndValue.IsArrayView
            && llvm::isa_and_nonnull<llvm::GetElementPtrInst>(operandNamedVar.Storage))
        {
            if (auto* ptrTy = llvm::dyn_cast<llvm::PointerType>(operandNamedVar.BaseType))
            {
                compiler->builder->CreateStore(
                    llvm::ConstantPointerNull::get(ptrTy), operandNamedVar.Storage);
                compiler->StoreRawArrayLength(operandNamedVar, nullptr);
            }
        }

        return {};
    }

bool MainListener::IsDirectCallArgument(antlr4::ParserRuleContext* ctx) {
        for (auto* node = ctx->parent; node != nullptr; )
        {
            if (dynamic_cast<CFlatParser::ArgumentNamedExpressionContext*>(node) != nullptr)
                return true;
            auto* rule = dynamic_cast<antlr4::ParserRuleContext*>(node);
            if (rule == nullptr || rule->children.size() != 1) return false;
            node = rule->parent;
        }
        return false;
    }

bool MainListener::IsBorrowedByValueParamBinding(
        LLVMBackend* compiler, const LLVMBackend::NamedVariable& nv) {
        if (compiler == nullptr || nv.Storage == nullptr) return false;
        const std::string& name = nv.TypeAndValue.VariableName;
        if (name.empty() || !compiler->IsFunctionParameter(name)) return false;
        // STORAGE identity, not the name: an inner block may declare a local of the parameter's
        // name, and that local carries none of the parameter's borrow contract.
        auto arg = compiler->GetFunctionArgument(name);
        if (arg.Storage == nullptr || arg.Storage != nv.Storage) return false;
        // A string param the body rebinds owns its entry copy (IsOwningString), as an owning struct does.
        if (nv.TypeAndValue.Pointer || nv.TypeAndValue.IsMove || nv.IsOwningStruct || nv.IsOwningString)
            return false;
        return compiler->IsDataStructure(nv.TypeAndValue.TypeName);
    }

void MainListener::PrepareBorrowedStringParamForDataMut(
        LLVMBackend* compiler, const LLVMBackend::NamedVariable& receiver) {
        if (compiler == nullptr || receiver.TypeAndValue.TypeName != "string"
            || !IsBorrowedByValueParamBinding(compiler, receiver))
            return;
        if (compiler->OwningSinkConsumesConcrete(
                compiler->GetFunctionArgument(receiver.TypeAndValue.VariableName).TypeAndValue))
            return;

        // Clear the entry descriptor before data_mut() can install a private buffer.
        llvm::StoreInst* lastParamStore = nullptr;
        if (compiler->currentFunction == nullptr || receiver.Storage == nullptr)
            return;
        auto* entry = &compiler->currentFunction->getEntryBlock();
        for (auto& instruction : *entry)
            if (auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction))
            {
                llvm::Value* destination = store->getPointerOperand()->stripPointerCasts();
                while (auto* gep = llvm::dyn_cast<llvm::GEPOperator>(destination))
                    destination = gep->getPointerOperand()->stripPointerCasts();
                if (destination == receiver.Storage)
                    lastParamStore = store;
            }
        if (lastParamStore == nullptr)
            return;
        auto* stringTy = llvm::StructType::getTypeByName(*compiler->context, "string");
        if (stringTy == nullptr)
            return;
        auto savedIP = compiler->builder->saveIP();
        if (auto* next = lastParamStore->getNextNode())
            compiler->builder->SetInsertPoint(next);
        else
            compiler->builder->SetInsertPoint(entry);
        auto* value = compiler->builder->CreateLoad(stringTy, receiver.Storage);
        compiler->builder->CreateStore(compiler->ClearStringOwnedBit(value), receiver.Storage);
        compiler->builder->restoreIP(savedIP);
        compiler->MarkVariableOwningString(receiver.TypeAndValue.VariableName);
    }

bool MainListener::IsBorrowedStructParameter(LLVMBackend* compiler, const std::string& name) {
        if (compiler == nullptr || name.empty()) return false;
        if (!compiler->IsFunctionParameter(name)) return false;
        auto nv = compiler->GetScopedLocalOrArgument(name);
        // Resolve the NAME to a binding, then judge the BINDING. A shadowing local answers
        // IsFunctionParameter (the name is a parameter's) while owning its own storage.
        nv.TypeAndValue.VariableName = name;
        return IsBorrowedByValueParamBinding(compiler, nv);
    }

bool MainListener::IsMoveOwningStructParameter(LLVMBackend* compiler, const std::string& name) {
        if (compiler == nullptr || name.empty()) return false;
        if (!compiler->IsFunctionParameter(name)) return false;
        auto nv = compiler->GetScopedLocalOrArgument(name);
        if (nv.TypeAndValue.Pointer || nv.TypeAndValue.ElemPointer || !nv.TypeAndValue.IsMove)
            return false;
        return compiler->IsOwningValueType(nv.TypeAndValue.TypeName);
    }

std::string MainListener::BorrowedOriginRoot(const std::string& origin) {
        auto dot = origin.find('.');
        return dot == std::string::npos ? origin : origin.substr(0, dot);
    }

std::string MainListener::FieldPathRootName(const LLVMBackend::NamedVariable& nv) {
        return nv.FieldPathRoot.empty()
            ? BorrowedOriginRoot(nv.TypeAndValue.ParentVariableName) : nv.FieldPathRoot;
    }

// Is this field an owning VALUE (a struct with a destructor or an owning field), as opposed to a
// pointer? Only the value family is covered by the q06 move-required/move-allowed ruling.
bool MainListener::FieldIsOwningValue(LLVMBackend* compiler, const LLVMBackend::NamedVariable& nv)
{
    if (nv.TypeAndValue.Pointer || nv.TypeAndValue.ElemPointer) return false;
    if (nv.TypeAndValue.TypeName.empty()) return false;
    return compiler->IsOwningValueType(nv.TypeAndValue.TypeName);
}

// A method's implicit `this` receiver is a borrowed pointer parameter too, but `this.field`
// consume is a RATIFIED accept (issue q06) - the ruling covers written parameters only.
bool MainListener::BorrowOriginIsMethodReceiver(LLVMBackend* compiler, const std::string& origin)
{
    if (origin.empty() || compiler->builder->GetInsertBlock() == nullptr) return false;
    const auto* sym = compiler->FindSymbolForFunction(
        compiler->builder->GetInsertBlock()->getParent());
    return sym != nullptr && sym->IsMethod && !sym->Parameters.empty()
        && sym->Parameters[0].VariableName == origin;
}

bool MainListener::RejectConsumeOfBorrowedByValueParamField(
        LLVMBackend* compiler, const LLVMBackend::NamedVariable& srcNV,
        antlr4::ParserRuleContext* ctx, bool constantUniqueArrayFieldElement) {
        const bool implicitThisFieldMove = srcNV.TypeAndValue.IsUnique
            || compiler->IsCoreUniqueType(srcNV.TypeAndValue.TypeName)
            || (compiler->IsOwningValueType(srcNV.TypeAndValue.TypeName)
                && !compiler->IsCopyableType(srcNV.TypeAndValue.TypeName));
        if (implicitThisFieldMove && srcNV.RootIsBorrowedThisField
            && RejectImplicitThisFieldConsume(compiler, srcNV, false, ctx))
            return true;
        // Only a FIELD PATH can be rooted at a parameter without naming it; a whole-value
        // consume of the parameter itself is the owning-sink inference's job, not this guard.
        if (srcNV.OwningStructName.empty() || srcNV.FieldName.empty()) return false;
        const std::string shownOwner = SpellType(*compiler, LLVMBackend::TypeAndValue{
            .TypeName = srcNV.OwningStructName });
        // The RECORDED answer from the field-access site, not a fresh lookup of the root's name.
        // The borrowed POINTER-parameter root is the third spelling (RULED 2026-08-12: move
        // required, move allowed). Element slots stay exempt (IsBorrowed tags pointer bases) unless the
        // caller adopts a constant-index unique array field element into a local, a field consume.
        bool rootIsBorrowedPtrParam = srcNV.IsBorrowed && !srcNV.BorrowedOrigin.empty()
                                   && (!srcNV.IsElementAccess || constantUniqueArrayFieldElement)
                                   && FieldIsOwningValue(compiler, srcNV)
                                   && !BorrowOriginIsMethodReceiver(compiler, srcNV.BorrowedOrigin);
        if (!srcNV.RootIsBorrowedByValueParam && !srcNV.RootIsAliasBorrowLocal
            && !rootIsBorrowedPtrParam)
            return false;
        if (rootIsBorrowedPtrParam && !srcNV.RootIsBorrowedByValueParam
            && !srcNV.RootIsAliasBorrowLocal)
        {
            const std::string fieldPath = srcNV.FieldPathText.empty()
                ? srcNV.BorrowedOrigin + "." + srcNV.FieldName : srcNV.FieldPathText;
            LogErrorContext(ctx, std::format(
                "cannot consume field '{}.{}' through borrowed parameter '{}' - the store takes the "
                "caller's value and leaves its field null, without the caller ever agreeing to "
                "surrender it. Write 'move {}' to say so explicitly, or declare the parameter "
                "'move {}' to take ownership of the whole struct.",
                shownOwner, srcNV.FieldName, srcNV.BorrowedOrigin,
                fieldPath, srcNV.BorrowedOrigin));
            return true;
        }
        const std::string root = FieldPathRootName(srcNV);
        if (srcNV.RootIsAliasBorrowLocal && !srcNV.RootIsBorrowedByValueParam)
        {
            // An alias local may refer to an owning local in this same frame. Consuming
            // that field still updates the real owner's slot, so it is safe; the reject
            // remains for aliases of globals, parameters, or other outliving storage.
            bool aliasesFrameOwner = false;
            auto rootStorage = compiler->FindVariableStorage(root).Storage;
            if (rootStorage != nullptr && llvm::isa<llvm::AllocaInst>(rootStorage))
                for (const auto& frame : compiler->stackNamedVariable)
                    for (const auto& [name, binding] : frame.namedVariable)
                        if (binding.Storage == rootStorage && !binding.IsAliasBorrow
                            && !binding.IsBorrowedOwningValue)
                            aliasesFrameOwner = true;
            if (aliasesFrameOwner)
                return false;
            LogErrorContext(ctx, std::format(
                "cannot consume field '{}.{}' out of 'alias' borrow '{}' - the store nulls only "
                "this shallow copy, so the real owner's field still frees the pointee (double-free). "
                "Read the field without storing it, or use '.copy()' for an independent owned copy.",
                shownOwner, srcNV.FieldName, root));
            return true;
        }
        // Reached through `T* p = &param`: `p` is a LOCAL, so it must not be called a parameter
        // and 'move p' is not the remedy - the parameter that owns the storage is not nameable here.
        if (srcNV.PointsToBorrowedByValueParam)
        {
            LogErrorContext(ctx, std::format(
                "cannot consume field '{}.{}' through pointer '{}' - it points at a borrowed "
                "parameter, which does not own the caller's struct, so the store cannot transfer "
                "ownership and the caller's struct still frees the pointee (double-free). Declare "
                "that parameter 'move' to take ownership, or read the field without storing it.",
                shownOwner, srcNV.FieldName, root));
            return true;
        }
        // On `w.arr[0]` this names the owning ARRAY field rather than the slot: coarse, but the
        // field, the parameter and the remedy are all correct. IsElementAccess is not set here.
        LogErrorContext(ctx, std::format(
            "cannot consume field '{}.{}' out of borrowed parameter '{}' - the parameter binding "
            "borrows the caller's struct and does not own it, so the caller's struct still frees the "
            "pointee (double-free). Declare the parameter 'move {}' to take ownership of the caller's "
            "struct, or read the field without storing it (a read borrows, a store consumes).",
            shownOwner, srcNV.FieldName, root, root));
        return true;
    }

bool MainListener::RejectJoinedSourceIntoUniqueField(
        LLVMBackend* compiler, const LLVMBackend::NamedVariable& rightNV,
        antlr4::ParserRuleContext* ctx) {
        const auto* join = compiler->FindUniqueFieldReadJoin(rightNV.Primary);
        if (join == nullptr) return false;
        for (const auto& source : join->Sources)
        {
            if (source.Storage == nullptr) continue;
            // A (possibly nested) field of a BORROWED by-value parameter: the caller keeps the pointee.
            // Owner/field come from the leaf GEP; struct-field GEP bases lead back to the parameter.
            auto isStructFieldGep = [](const llvm::Value* value) {
                auto* g = llvm::dyn_cast_or_null<llvm::GetElementPtrInst>(value);
                return g != nullptr && g->getNumIndices() == 2
                    && g->getSourceElementType()->isStructTy()
                    && g->getSourceElementType()->getStructName().size()
                    && llvm::isa<llvm::ConstantInt>(g->getOperand(2));
            };
            auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(source.Storage);
            if (!isStructFieldGep(gep)) continue;
            llvm::Value* root = gep->getPointerOperand();
            while (isStructFieldGep(root))
                root = llvm::cast<llvm::GetElementPtrInst>(root)->getPointerOperand();
            const auto* param = compiler->FindVariableByStorage(root);
            if (param == nullptr || !IsBorrowedByValueParamBinding(compiler, *param)) continue;
            const std::string owner = gep->getSourceElementType()->getStructName().str();
            const auto& fields = compiler->GetDataStructure(owner).StructFields;
            const auto* index = llvm::dyn_cast<llvm::ConstantInt>(gep->getOperand(2));
            if (index == nullptr || index->getZExtValue() >= fields.size()) continue;
            LLVMBackend::NamedVariable srcNV;
            srcNV.OwningStructName = owner;
            srcNV.FieldName = fields[index->getZExtValue()].VariableName;
            srcNV.RootIsBorrowedByValueParam = true;
            srcNV.FieldPathRoot = param->TypeAndValue.VariableName;
            if (RejectConsumeOfBorrowedByValueParamField(compiler, srcNV, ctx))
                return true;
        }
        return false;
    }

bool MainListener::RejectImplicitConsumeOfOutlivingOwner(
        LLVMBackend* compiler, const LLVMBackend::NamedVariable& srcNV,
        bool srcIsMove, antlr4::ParserRuleContext* ctx) {
        if (RejectImplicitThisFieldConsume(compiler, srcNV, srcIsMove, ctx)) return true;
        const std::string message = compiler->ImplicitOutlivingOwnerConsumeError(srcNV, srcIsMove);
        if (message.empty()) return false;
        // The declaration-init caller (EmitImplicitUniqueFieldMove) carries no parse context.
        if (ctx != nullptr) LogErrorContext(ctx, message);
        else                compiler->LogError(message);
        return true;
    }

bool MainListener::RejectImplicitThisFieldConsume(
        LLVMBackend* compiler, const LLVMBackend::NamedVariable& srcNV,
        bool srcIsMove, antlr4::ParserRuleContext* ctx) {
        // Only a proven identical slot (same path, same constant index) is a self-assignment.
        if (const std::string key = ThisFieldSlotKey(srcNV);
            !key.empty() && key == thisFieldAssignDestPath_)
            return false;
        const std::string message = compiler->ImplicitThisFieldConsumeError(srcNV, srcIsMove);
        if (message.empty()) return false;
        if (ctx != nullptr) LogErrorContext(ctx, message);
        else                compiler->LogError(message);
        return true;
    }

bool MainListener::RejectStoreIntoBorrowedField(
        LLVMBackend* compiler, const LLVMBackend::NamedVariable& destNV,
        antlr4::ParserRuleContext* ctx) {
        if (destNV.OwningStructName.empty() || destNV.FieldName.empty()) return false;
        if (!destNV.RootIsBorrowedByValueParam && !destNV.RootIsAliasBorrowLocal) return false;
        const bool ownsValue = compiler->IsOwningValueType(destNV.TypeAndValue.TypeName)
            || IsOwningUniquePointerField(destNV.TypeAndValue)
            || IsOwningUniqueInterfaceField(destNV.TypeAndValue);
        if (!ownsValue) return false;

        const std::string root = FieldPathRootName(destNV);
        if (destNV.RootIsBorrowedByValueParam)
        {
            LogErrorContext(ctx, std::format(
                "cannot assign owning field '{}.{}' of borrowed parameter '{}' - the parameter "
                "binding only borrows, so dropping the old value would free a pointee the caller's "
                "struct still owns. Declare the parameter 'move {}' to take ownership, or assign "
                "through the owner.",
                destNV.OwningStructName, destNV.FieldName, root, root));
        }
        else
        {
            LogErrorContext(ctx, std::format(
                "cannot assign owning field '{}.{}' through 'alias' borrow '{}' - dropping the old "
                "value would free the real owner's pointee through a shallow copy. Rebind the whole "
                "borrow local or assign through the owner.",
                destNV.OwningStructName, destNV.FieldName, root));
        }
        return true;
    }

void MainListener::AdoptWrapperProvenance(LLVMBackend::NamedVariable& dst,
                                          const LLVMBackend::NamedVariable& src) {
        // The name the move-dataflow, the owning-sink inference and the diagnostics key on.
        dst.CallerName      = src.CallerName;
        dst.PointeeMove     = src.PointeeMove;
        dst.MoveSourceStorage = src.MoveSourceStorage;
        dst.MoveSourcePathKey = src.MoveSourcePathKey;
        dst.MoveSourceFieldPath = src.MoveSourceFieldPath;
        dst.IdentifierLine  = src.IdentifierLine;
        dst.IdentifierColumn = src.IdentifierColumn;
        // The field-path provenance the six consume arms, the return arm and the
        // borrowed-by-value-parameter reject guard read.
        dst.OwningStructName = src.OwningStructName;
        dst.FieldName        = src.FieldName;
        dst.FromOwningTempField = src.FromOwningTempField;
        dst.OwningTempParent = src.OwningTempParent;
        dst.MovableTempField = src.MovableTempField;
        dst.MoveTempStructAlloca = src.MoveTempStructAlloca;
        dst.MoveTempStructType = src.MoveTempStructType;
        dst.MoveTempFieldIndex = src.MoveTempFieldIndex;
        dst.MovePathKey      = src.MovePathKey;
        dst.MovePathHasDynamicIndex = src.MovePathHasDynamicIndex;
        dst.FieldPathText    = src.FieldPathText;
        dst.FieldPathRoot    = src.FieldPathRoot;
        dst.FieldPathThroughPointer = src.FieldPathThroughPointer;
        dst.FieldPathHasPointerDereference = src.FieldPathHasPointerDereference;
        dst.FieldPathPointerFrameLocal = src.FieldPathPointerFrameLocal;
        dst.FieldPathFromForwardedArrow = false;
        dst.IsStaticLocal = src.IsStaticLocal;
        dst.RootIsBorrowedByValueParam = src.RootIsBorrowedByValueParam;
        dst.RootIsAliasBorrowLocal = src.RootIsAliasBorrowLocal;
        dst.PointsToBorrowedByValueParam = src.PointsToBorrowedByValueParam;
        dst.IsInterfaceField = src.IsInterfaceField;
        // Element provenance: a view slot is LIVE storage, a container's internal slot is not.
        dst.IsElementAccess = src.IsElementAccess;
        dst.IsViewElement   = src.IsViewElement;
        dst.IsTempSpillStorage = src.IsTempSpillStorage;
        // Borrow origin, so a wrapped source is refused wherever the bare one is.
        dst.IsBorrowed             = src.IsBorrowed;
        dst.BorrowedThroughField   = src.BorrowedThroughField;
        dst.BorrowedOrigin         = src.BorrowedOrigin;
        dst.StackCharBufferBorrow  = src.StackCharBufferBorrow;
        dst.StackCharBufferSource  = src.StackCharBufferSource;
        dst.StackCharBufferScopeDepth = src.StackCharBufferScopeDepth;
        dst.BorrowedUniqueField    = src.BorrowedUniqueField;
        dst.BorrowedUniqueFieldViaCall = src.BorrowedUniqueFieldViaCall;
        dst.IsUniqueFieldAlias     = src.IsUniqueFieldAlias;
        dst.IsBorrowedOwningValue  = src.IsBorrowedOwningValue;
        dst.IsAliasBorrow          = src.IsAliasBorrow;
        dst.BorrowsOwnedString     = src.BorrowsOwnedString;
        dst.IsClosureValueCapture  = src.IsClosureValueCapture;
        dst.IsClosureRefCapture    = src.IsClosureRefCapture;
        dst.IsMoved                = src.IsMoved;
        dst.IsRvalue               = src.IsRvalue;
        dst.IsExplicitMove         = src.IsExplicitMove;
        dst.IsNormalizedCxxXvalue = src.IsNormalizedCxxXvalue;
        dst.IsCxxPrvalueSubobject = src.IsCxxPrvalueSubobject;
        dst.IsCxxPrvaluePointerSubobject = src.IsCxxPrvaluePointerSubobject;
        dst.IsCxxPrvalueConditional = src.IsCxxPrvalueConditional;
        dst.CxxPrvalueOwnerType = src.CxxPrvalueOwnerType;
        // OWNERSHIP state. These travel together with the borrow facts above: a guard that reads
        // one and not the other reports the opposite of the truth - `delete (n)` on a `move`
        // parameter was rejected as "borrowed" when only CallerName came across.
        dst.IsOwning         = src.IsOwning;
        dst.IsNewAllocated   = src.IsNewAllocated;
        dst.IsAdoptable      = src.IsAdoptable;
        dst.IsOwningString   = src.IsOwningString;
        dst.IsOwningStruct   = src.IsOwningStruct;
        dst.IsNullConditionalOwningValue = src.IsNullConditionalOwningValue;
        dst.NullConditionalBorrowStorage = src.NullConditionalBorrowStorage;
        dst.AllocAlignment   = src.AllocAlignment;
        dst.AllocatedByRawNewArray = src.AllocatedByRawNewArray;
        dst.IsCoreUniqueRelease = src.IsCoreUniqueRelease;
        dst.RawArrayLength = src.RawArrayLength;
        dst.RawArrayLengthStorage = src.RawArrayLengthStorage;
        dst.IsStaticLocal    = src.IsStaticLocal;
        dst.MovedIntoInterface = src.MovedIntoInterface;
        dst.ExplicitlyMovedNull = src.ExplicitlyMovedNull;
        dst.ExplicitNullBlock   = src.ExplicitNullBlock;
        dst.ConditionalDropFlag = src.ConditionalDropFlag;
        dst.DeclarationScopeDepth = src.DeclarationScopeDepth;
        dst.DeclarationBlock = src.DeclarationBlock;
        // The element / view / owning-local borrow proofs the delete guards consult.
        dst.BorrowsOwnedElement            = src.BorrowsOwnedElement;
        dst.BorrowedElementExternallyOwned = src.BorrowedElementExternallyOwned;
        dst.OwnedElementContainer          = src.OwnedElementContainer;
        dst.BorrowsOwningLocal             = src.BorrowsOwningLocal;
        dst.OwningLocalOrigin              = src.OwningLocalOrigin;
        dst.OwningLocalStorage             = src.OwningLocalStorage;
        dst.OwningLocalBorrowAfterRebind  = src.OwningLocalBorrowAfterRebind;
        dst.ViewOfFixedArrayStorage        = src.ViewOfFixedArrayStorage;
        dst.ViewOfFixedArraySourceName     = src.ViewOfFixedArraySourceName;
        dst.BorrowedInterfaceBox           = src.BorrowedInterfaceBox;
        dst.BorrowedInterfaceBoxSource     = src.BorrowedInterfaceBoxSource;
        dst.BorrowedInterfaceBoxSlots      = src.BorrowedInterfaceBoxSlots;
        dst.OwnedStringBorrowBlock         = src.OwnedStringBorrowBlock;
        dst.OwnedStringBorrowFunction      = src.OwnedStringBorrowFunction;
        dst.OwnedElementEvents             = src.OwnedElementEvents;
        dst.BondDeclBlock                  = src.BondDeclBlock;
        dst.BondDeclFunction               = src.BondDeclFunction;
        dst.IsBonded                        = src.IsBonded;
        dst.BondByAddress                   = src.BondByAddress;
        dst.BondedSources                   = src.BondedSources;
        dst.ContainsBondedClosure          = src.ContainsBondedClosure;
        dst.LambdaCaptureNames             = src.LambdaCaptureNames;
        dst.LambdaReferenceCaptureNames    = src.LambdaReferenceCaptureNames;
    }

bool MainListener::IsRedundantCastOfSource(const LLVMBackend::TypeAndValue& src,
                                           const LLVMBackend::TypeAndValue& dest) {
        auto* compiler = compilerLLVM;
        if (src.TypeName.empty() || dest.TypeName.empty()) return false;
        // Canonicalized, not merely alias-resolved: a namespace-relative cast spelling ('(NBox)p'
        // inside NBox's own namespace) names the same type its operand already has.
        if (CanonicalWrapperTypeName(compiler, src.TypeName)
            != CanonicalWrapperTypeName(compiler, dest.TypeName))
            return false;
        // Every shape decoration must agree too: `(T*)t`, `(T[])p` and `(IFace)v` all CHANGE the
        // value's shape even when the base name matches, so none of them is a pass-through.
        return src.Pointer == dest.Pointer
            && src.ElemPointer == dest.ElemPointer
            && src.IsArrayView == dest.IsArrayView
            && src.IsInterface == dest.IsInterface
            && src.IsFunctionPointer == dest.IsFunctionPointer;
    }

CFlatParser::TypeCheckExpressionContext* MainListener::SoleTypeCheckExpressionOf(
        CFlatParser::ConditionalExpressionContext* ctx) {
        if (ctx == nullptr || ctx->Question() != nullptr || ctx->QuestionQuestion() != nullptr)
            return nullptr;
        auto* lor = ctx->logicalOrExpression();
        if (lor == nullptr) return nullptr;
        auto las = lor->logicalAndExpression();
        if (las.size() != 1) return nullptr;
        auto ios = las[0]->inclusiveOrExpression();
        if (ios.size() != 1) return nullptr;
        auto eos = ios[0]->exclusiveOrExpression();
        if (eos.size() != 1) return nullptr;
        auto ands = eos[0]->andExpression();
        if (ands.size() != 1) return nullptr;
        auto eqs = ands[0]->equalityExpression();
        if (eqs.size() != 1) return nullptr;
        auto tcs = eqs[0]->typeCheckExpression();
        return tcs.size() == 1 ? tcs[0] : nullptr;
    }

LLVMBackend::NamedVariable MainListener::ParseMoveExpression(CFlatParser::MoveExpressionContext* ctx) {
        auto* compiler = Compiler(ctx);
        const auto operandLayers = DirectOperandPostfixes(ctx->unaryExpression());
        FlexibleArrayKeepScope keep(flexibleArrayKeep_, operandLayers);
        auto argNV = ParseUnaryExpression(ctx->unaryExpression());
        // A copied field is non-owning, not [immovable], and holds no C++ class beyond a trivially
        // copyable record (a C++ move ctor must still run and leave the source moved-from).
        const auto copiesAsPlainValue = [&](const LLVMBackend::NamedVariable& nv) {
            const std::string& tn = nv.TypeAndValue.TypeName;
            return !FieldIsOwningValue(compiler, nv) && !compiler->IsImmovableType(tn)
                && (!compiler->HasCxxClassMember(tn)
                    || (compiler->IsCxxRecord(tn) && compiler->IsCxxTriviallyCopyableRecord(tn)));
        };
        const std::string moveOperandText = ctx->unaryExpression()->getText();
        // A method or a lambda that captures `this` (the lambda is not itself a method) reads the member.
        const bool namesImplicitThisField = IsBareIdentifierText(moveOperandText)
            && compiler->GetScopedLocalOrArgument(moveOperandText).Storage == nullptr
            && compiler->HasMemberVariable(moveOperandText)
            && compiler->GetFunctionArgument("this").GetValue() != nullptr;
        // Implicit-this `data` names the same member as `this->data`: same refusal, decided before the copy path.
        if (namesImplicitThisField && (argNV.TypeAndValue.ConstArraySize > 0
                || argNV.TypeAndValue.IsFlexibleArrayMember || argNV.TypeAndValue.IsZeroLengthArrayMember))
        {
            LogErrorContext(ctx,
                "cannot move an array member; take its address or copy the elements");
            return {};
        }
        // A view moves into a plain local only as a copy; any other destination is a transfer.
        const bool viewCopy = argNV.TypeAndValue.IsArrayViewValue() && MoveIsLocalDeclInitializer(ctx);
        if (namesImplicitThisField && (!argNV.TypeAndValue.Pointer || viewCopy)
            && !argNV.TypeAndValue.ElemPointer && copiesAsPlainValue(argNV))
        {
            argNV.TypeAndValue.IsMove = false;
            argNV.IsExplicitMove = false;
            argNV.IsRvalue = false;
            return argNV;
        }
        // The operand's LAST suffix decides member vs element: `arr[0].data` is a member, `a->data[0]` an element.
        bool operandEndsInMember = false;
        if (!operandLayers.empty() && operandLayers.back()->children.size() >= 3)
        {
            const auto& kids = operandLayers.back()->children;
            const std::string op = kids[kids.size() - 2]->getText();
            operandEndsInMember = op == "." || op == "->" || op == "?.";
        }
        if (operandEndsInMember && !argNV.FieldName.empty()
            && (argNV.TypeAndValue.ConstArraySize > 0
                || argNV.TypeAndValue.IsFlexibleArrayMember))
        {
            LogErrorContext(ctx,
                "cannot move an array member; take its address or copy the elements");
            return {};
        }
        // Into a C++ parameter the move binds `const T&&` / `const T&` (C++ rule) and nulls nothing.
        if (!inCxxCallArgument_)
            RefuseConstObjectWrite(ctx->unaryExpression(), argNV, argNV.Storage, "move");
        auto markMovedPath = [&]() {
            const std::string movePath = LLVMBackend::FieldMoveKey(argNV);
            const std::string fieldRootName = !argNV.CallerName.empty()
                ? argNV.CallerName : !argNV.FieldPathRoot.empty()
                    ? argNV.FieldPathRoot : argNV.TypeAndValue.ParentVariableName;
            if (argNV.TypeAndValue.IsFunctionPointer || argNV.TypeAndValue.TypeName == "__c_fn_ptr"
                || compiler->GetEncodedClosureType(argNV.TypeAndValue.TypeName) != nullptr)
                return;
            if (movePath.empty() || fieldRootName.empty() || argNV.IsBorrowed)
                return;
            // A plain field path needs no mark here: the zero-filled field is destroyed as usual.
            if (movePath.find('[') == std::string::npos)
                return;
            // Array slots need an indexed compile-time path so a sibling element cannot revive
            // or hide the moved element. Keep the existing ownership gate for those paths.
            if (argNV.IsOwning || argNV.TypeAndValue.IsUnique || argNV.IsOwningString
                || compiler->IsCoreUniqueType(argNV.TypeAndValue.TypeName)
                || compiler->IsOwningValueType(argNV.TypeAndValue.TypeName))
                compiler->MarkVariableFieldMoved(fieldRootName, movePath);
        };
        const bool movedPointee = argNV.FieldPathHasPointerDereference
            && argNV.FieldName.empty() && argNV.OwningStructName.empty()
            && !argNV.IsElementAccess;
        argNV.PointeeMove = movedPointee;
        if (argNV.ContainsBondedClosure)
            LogErrorContext(ctx,
                "cannot move a holder containing a bonded closure - the closure would outlive its captured local");
        const auto* lent = argNV.Storage != nullptr
            ? compiler->FindStackVariableByStorage(argNV.Storage) : nullptr;
        const std::string lender = lent != nullptr && !lent->CxxClosureLentTo.empty()
            ? lent->CxxClosureLentTo : compiler->FindLiveCxxClosureLender(argNV.Storage);
        if (!lender.empty())
            LogErrorContext(ctx, std::format(
                "cannot move closure '{}': it was passed by reference to C++ '{}', whose result may keep "
                "that reference, and 'move' leaves the closure null; copy it instead",
                ctx->unaryExpression()->getText(), lender));

        // [immovable]: a value of the type may never be transferred, so `move` of one is refused
        // at the operation. A pointer to it is a movable pointer and passes through unchanged.
        if (!argNV.TypeAndValue.Pointer && compiler->IsImmovableType(argNV.TypeAndValue.TypeName))
        {
            LogErrorContext(ctx, std::format(
                "cannot 'move' a value of type '{}': the type is [immovable], so its value must stay "
                "where it was constructed (take a pointer or alias instead)",
                SpellDiagnosticType(*compiler, argNV.TypeAndValue)));
            return {};
        }

        // Moving a foreign C++ object moves its value, not its ownership. Its real owner still
        // destroys the moved-from object, so alias-owner checks for pointer transfers do not apply.
        if (!argNV.TypeAndValue.Pointer && argNV.Storage != nullptr
            && !argNV.IsTempSpillStorage
            && compiler->IsForeignNontrivialCxxReturnClass(argNV.TypeAndValue.TypeName))
        {
            argNV.TypeAndValue.IsMove = true;
            argNV.IsExplicitMove = true;
            argNV.IsRvalue = true;
            return argNV;
        }

        // A const C++ object becomes `const T&&`; keep its referent, including for trivial records.
        if (!argNV.TypeAndValue.Pointer && argNV.Storage != nullptr
            && !argNV.IsTempSpillStorage
            && compiler->IsCxxRecord(argNV.TypeAndValue.TypeName)
            && !compiler->IsCppStructName(argNV.TypeAndValue.TypeName)
            && compiler->CxxConstReceiverKind(argNV) != 0)
        {
            if (ctx == trivialCxxMoveSite_)
            {
                LLVMBackend::NamedVariable built = argNV;
                std::string refusal;
                const std::string sourceName = !argNV.CallerName.empty() ? argNV.CallerName
                    : ctx->unaryExpression() != nullptr ? ctx->unaryExpression()->getText()
                                                        : std::string();
                if (compiler->MaterializeTrivialCxxExplicitMove(built, sourceName, refusal))
                {
                    built.IsExplicitMove = true;
                    built.IsRvalue = true;
                    return built;
                }
                if (!refusal.empty())
                {
                    LogErrorContext(ctx, refusal);
                    return {};
                }
            }
            argNV.IsExplicitMove = true;
            argNV.IsRvalue = true;
            return argNV;
        }

        // Reject 'move' through a temp spill (`move mk().vals[i]`): the subscript spilled a SHALLOW
        // copy of the extracted array, so nulling a slot there leaves the original temporary still
        // holding the pointer - a leak or a double free. Reads of the spill stay legal.
        if (argNV.IsTempSpillStorage)
        {
            LogErrorContext(ctx,
                "cannot 'move' out of an inline array field of a temporary value - the slot lives in "
                "a shallow copy that dies with the expression, so the original still owns the pointee. "
                "Bind the call result to a local first, then move out of it.");
            return {};
        }

        // Reject 'move h.p' where `h` is a BORROWED by-value struct parameter: the move nulls the
        // callee's copy of the field, but the caller's original still holds the pointer, so both
        // synthesized destructors free it. Declaring the parameter 'move' makes the callee the owner.
        // Uses the answer RECORDED at the field-access site, which is also what makes a NESTED path
        // (`move w.a.b`) reach this leg: ParentVariableName names the intermediate field, not `w`.
        if (!argNV.OwningStructName.empty() && argNV.PointsToBorrowedByValueParam)
        {
            const std::string pointer = FieldPathRootName(argNV);
            const std::string fieldPath = argNV.FieldPathText.empty()
                ? pointer + "." + argNV.FieldName : argNV.FieldPathText;
            LogErrorContext(ctx, std::format(
                "cannot 'move' field '{}' through pointer '{}' - it points at a borrowed "
                "parameter, which does not own the caller's struct, so the caller's struct still "
                "frees the pointee (double-free). Declare that parameter 'move' to take ownership, "
                "or read the field without moving it.",
                fieldPath, pointer));
            return {};
        }
        if (!argNV.OwningStructName.empty() && argNV.RootIsBorrowedByValueParam)
        {
            const std::string parent = FieldPathRootName(argNV);
            LogErrorContext(ctx, std::format(
                "cannot 'move' field '{}.{}' out of borrowed parameter '{}' - the parameter binding "
                "borrows the caller's struct and does not own it, so the caller's struct still frees "
                "the pointee (double-free). Declare the parameter 'move {}' to take ownership of the "
                "caller's struct, or read the field without moving it (a read borrows, a move consumes).",
                argNV.OwningStructName, argNV.FieldName, parent, parent));
            return {};
        }

        /*
         * Same hazard with an `alias`-BORROW LOCAL root (`Box k = w.get(); move k.item;`): the
         * borrow shallow-copies the owner's struct, so nulling this copy's field leaves the real
         * owner's field live and both the receiver and the owner free the pointee. Uses the answer
         * RECORDED at the field-access site, which also carries a NESTED path (`move k.b.item`).
         * A by-reference lambda capture is carved out at the recording (its Storage IS the outer
         * owner's address, so the move really does null the one owner's field), and a borrow that
         * was RE-BOUND is no longer a borrow, so `move k.item` after `k = makeBox(2)` stays legal.
         */
        if (!argNV.OwningStructName.empty() && !argNV.FieldName.empty()
            && argNV.RootIsAliasBorrowLocal)
        {
            const std::string parent = FieldPathRootName(argNV);
            LogErrorContext(ctx, std::format(
                "cannot 'move' field '{}.{}' out of 'alias' borrow '{}' - '{}' shallow-copies storage "
                "it does not own, so the move nulls only this copy and the real owner's field still "
                "frees the pointee (double-free). Read the field without moving it (a read borrows, "
                "a move consumes), or use '.copy()' for an independent owned copy.",
                argNV.OwningStructName, argNV.FieldName, parent, parent));
            return {};
        }

        // `move` of any non-owning value field through a borrow is a copy; it preserves the field
        // and carries no ownership-transfer state into later lowering.
        if ((!argNV.OwningStructName.empty() || argNV.RootIsBorrowedThisField)
            && (argNV.IsBorrowed || argNV.RootIsBorrowedThisField)
            && (!argNV.BorrowedOrigin.empty() || argNV.RootIsBorrowedThisField)
            && !argNV.FieldName.empty()
            && !argNV.IsElementAccess
            && !argNV.TypeAndValue.ElemPointer
            && (!argNV.TypeAndValue.Pointer || viewCopy)
            && copiesAsPlainValue(argNV))
        {
            argNV.TypeAndValue.IsMove = false;
            argNV.IsExplicitMove = false;
            argNV.IsRvalue = false;
            return argNV;
        }

        /*
         * RULED 2026-08-12 (move required, move allowed): `move w.b` through a borrowed POINTER
         * parameter is LEGAL for an owning VALUE field. The move nulls the one real object the
         * caller can see - exactly what the implicit store already did silently, and that spelling
         * is the one now rejected with this as its named remedy. A POINTER field keeps the
         * rejection: handing out the raw pointer is the delete-borrow hazard q10 ruled DEFER, and
         * its own downstream guards would reject the remedy anyway. Non-owning value fields are
         * plain copies and bypass this refusal; by-value and alias-borrow roots above stay rejected.
         */
        if (!argNV.OwningStructName.empty()
            && argNV.IsBorrowed
            && !argNV.BorrowedOrigin.empty()
            && !argNV.IsElementAccess
            && argNV.TypeAndValue.Pointer
            && !BorrowOriginIsMethodReceiver(compiler, argNV.BorrowedOrigin))
        {
            LogErrorContext(ctx, std::format(
                "cannot 'move' field '{}.{}' through borrowed parameter '{}'. Declare the "
                "source parameter 'move {}' to transfer ownership of its fields.",
                argNV.OwningStructName, argNV.FieldName, argNV.BorrowedOrigin, argNV.BorrowedOrigin));
            return {};
        }

        /*
         * Reject 'move' of a local that aliases a `unique` field - Trap B in its original spelling.
         * Moving the alias nulls the LOCAL, never the field, so the field's synthesized destructor
         * still frees the pointee. Forwarding an ordinary borrow as 'move' across an EXPLICIT
         * ownership contract - a `move` parameter of a callee - stays legal (the programmer asserts
         * the borrow is dead, which core/hpc/btree.cb relies on); for a `unique` field that
         * assertion cannot hold, because the field's destructor is synthesized and will run. A
         * plain `T*` destination asserts nothing, so it does not adopt at all (see
         * borrowMoveKeepsBorrow in ParseDeclaration), and a `move` RETURN type is rejected at the
         * return statement rather than forwarded.
         */
        if (!argNV.BorrowedUniqueField.empty())
        {
            std::string name = argNV.CallerName.empty() ? argNV.BorrowedOrigin : argNV.CallerName;
            std::string root = BorrowedOriginRoot(argNV.BorrowedOrigin);
            // Proven through a CALL: the owner is only nameable as "Struct.field", so neither
            // 'move <origin>' spelling below exists at this site.
            if (argNV.BorrowedUniqueFieldViaCall)
            {
                LogErrorContext(ctx, std::format(
                    "cannot 'move' '{}' - it was returned as a borrow of unique field '{}', and moving "
                    "the alias does not null the field, so the field's synthesized destructor still "
                    "frees the pointee. Pass it to a plain (non-'move') parameter, which borrows, or "
                    "pass a value the receiver can own.",
                    name, argNV.BorrowedUniqueField));
                return {};
            }
            // 'move <origin>' is the right remedy only when the struct holding the field is OWNED.
            // If it is a borrowed by-value parameter, that advice is itself a double-free.
            if (IsBorrowedStructParameter(compiler, root))
                LogErrorContext(ctx, std::format(
                    "cannot 'move' '{}' - it aliases unique field '{}', and moving the alias does not "
                    "null the field, so the field's synthesized destructor still frees the pointee. "
                    "'move {}' is not a remedy here either: '{}' is a borrowed parameter and does not "
                    "own the caller's struct. Declare the parameter 'move {}' to take "
                    "ownership of the caller's struct.",
                    name, argNV.BorrowedUniqueField, argNV.BorrowedOrigin, root, root));
            else
                LogErrorContext(ctx, std::format(
                    "cannot 'move' '{}' - it aliases unique field '{}', and moving the alias does not "
                    "null the field, so the field's synthesized destructor still frees the pointee. "
                    "Use 'move {}' to move the field itself (which nulls it), provided the struct "
                    "holding it is owned here.",
                    name, argNV.BorrowedUniqueField, argNV.BorrowedOrigin));
            return {};
        }

        /*
         * 'move' of a POINTER binding that PROVABLY borrows - a plain copy of a live owning local,
         * or a container-owned element - transfers nothing: the real owner still frees the pointee,
         * so whatever adopts it here frees it twice. The raw `delete` guard rejects exactly these
         * two proofs, and rejecting the `move` spelling too is what stops the two from disagreeing.
         * Destination-agnostic on purpose: a plain `T*` destination double-freed just as a `unique`
         * one did. Only these two proofs, re-asked for liveness - an ordinary borrow forwarded as
         * `move` stays legal by the rule stated above, and a rebound copy is the sole owner.
         * An ordinary `IsBorrowed` source is deliberately NOT a third proof here: it is handled by
         * non-adoption at the destination instead, because rejecting it destination-agnostically
         * false-rejects core/hpc/btree.cb's `_rebalanceFrom` and six ratified join legs.
         */
        if (argNV.TypeAndValue.Pointer && argNV.FieldName.empty() && !argNV.IsElementAccess
            && llvm::isa_and_nonnull<llvm::AllocaInst>(argNV.Storage))
        {
            const auto* srcBind = compiler->FindVariableByStorage(argNV.Storage);
            std::string name = argNV.CallerName.empty()
                ? argNV.TypeAndValue.VariableName : argNV.CallerName;
            if (name.empty()) name = "<expr>";
            if (srcBind != nullptr && compiler->OwningLocalCopyStillAliases(*srcBind))
            {
                LogErrorContext(ctx, std::format(
                    "cannot 'move' '{}' - it copies '{}', which still owns the object and frees it at "
                    "scope exit, so this move transfers nothing and the destination double-frees it. "
                    "Use 'move {}' to take ownership out of the owner itself (which nulls it).",
                    name, srcBind->OwningLocalOrigin, srcBind->OwningLocalOrigin));
                return {};
            }
            if (srcBind != nullptr && !srcBind->IsOwning && srcBind->BorrowsOwnedElement)
            {
                std::string owner = srcBind->OwnedElementContainer.empty()
                    ? std::string("its container") : "'" + srcBind->OwnedElementContainer + "'";
                LogErrorContext(ctx, std::format(
                    "cannot 'move' '{}' - it borrows an element {} owns, and moving the borrow does "
                    "not clear the container's slot, so the container's destructor still frees the "
                    "pointee. Move the element slot itself, or remove it from {} first.",
                    name, owner, owner));
                return {};
            }
            // Third proof: a local bound from a '?:' / '??' JOIN whose every non-null arm proves
            // another owner. Same BOTH-ARMS fact (and same retirement) the raw `delete` guard asks,
            // so the two spellings agree; a MIXED join records nothing and stays legal.
            if (srcBind != nullptr && !srcBind->IsOwning
                && JoinArmsStillKeepOwner(*srcBind)
                && !srcBind->JoinKeepsOwnerSource.empty())
            {
                LogErrorContext(ctx, std::format(
                    "cannot 'move' '{}' - every arm of the join it was bound from holds an object {} "
                    "already frees, so this move transfers nothing and the destination double-frees "
                    "it. Bind '{}' to an object this frame owns, or drop the 'move' and let {} "
                    "release it.",
                    name, srcBind->JoinKeepsOwnerSource, name, srcBind->JoinKeepsOwnerSource));
                return {};
            }
        }

        // 'move' of a whole value this function only BORROWS (a plain by-value owning-value
        // parameter - string/owning-struct copyable arg the caller still owns): the buffer belongs
        // to the caller, so transferring it would free the caller's live value (heap-use-after-free).
        // Degrade to a plain read; the enclosing copy machinery deep-copies a copyable owner, exactly
        // as `dest = value` (no `move`) would. A direct call argument keeps the deferred path (its
        // laundering is caught in ApplyMoveParamTransfer); a field/element source is handled above.
        if (argNV.FieldName.empty() && !argNV.IsElementAccess
            && !argNV.CallerName.empty() && !IsDirectCallArgument(ctx)
            && compiler->IsVariableBorrowedOwningValue(argNV.CallerName))
            return argNV;

        // 'move' of a whole borrow zeroes only the borrow's OWN slot, so the receiver destroys
        // storage the real owner still frees. Rejected at the argument so it can name the local.
        if (argNV.FieldName.empty() && !argNV.IsElementAccess
            && BorrowAdoptionIsUnsound(compiler, argNV))
        {
            LogErrorContext(ctx, std::format(
                "cannot 'move' the 'alias' value '{}'; it borrows storage it does not own, so the "
                "move transfers nothing and the receiver would free storage the real owner still "
                "holds. Use '.copy()' for an independent owned copy.",
                argNV.CallerName.empty() ? SpellType(*compiler, argNV.TypeAndValue)
                                          : argNV.CallerName));
            return {};
        }

        argNV.IsExplicitMove = true;
        llvm::Value* ptrVal = LoadNamedVariable(argNV);
        const auto* encodedClosure = compiler->GetEncodedClosureType(argNV.TypeAndValue.TypeName);
        const bool movingFatClosure = !argNV.TypeAndValue.Pointer && argNV.Storage != nullptr
            && ((argNV.TypeAndValue.IsFunctionPointer && !argNV.TypeAndValue.IsThinFnPtr())
                || (encodedClosure != nullptr && !encodedClosure->IsThinFnPtr()));
        if (movingFatClosure && ptrVal != nullptr && ptrVal->getType()->isStructTy()
            && lambdaExpectedType.IsFunctionPointer && !lambdaExpectedType.IsThinFnPtr()
            && compiler->FuncPtrFewerParamsCanAdapt(argNV.TypeAndValue, lambdaExpectedType))
        {
            argNV.Primary = ptrVal;
            argNV.IsExplicitMove = true;
            argNV.IsRvalue = true;
            if (IsDirectCallArgument(ctx)) return argNV;
        }

        // A thin function pointer is a pointer-shaped VALUE, not a registered aggregate. Move it
        // like the pointer owner path: retain its value, null the source slot, and keep null readable.
        const bool movingThinFunction = !argNV.TypeAndValue.Pointer && argNV.Storage != nullptr
            && ((argNV.TypeAndValue.IsFunctionPointer && argNV.TypeAndValue.IsThinFnPtr())
                || (encodedClosure != nullptr && encodedClosure->IsThinFnPtr()));
        if (movingThinFunction)
        {
            if (IsDirectCallArgument(ctx))
            {
                argNV.IsExplicitMove = true;
                return argNV;
            }
            if (ptrVal != nullptr)
                compiler->builder->CreateStore(llvm::Constant::getNullValue(ptrVal->getType()),
                                               argNV.Storage);
            if (!argNV.IsElementAccess && argNV.FieldName.empty()
                && !argNV.CallerName.empty())
                compiler->MarkVariableExplicitlyMovedNull(argNV.CallerName, movedPointee);
            LLVMBackend::NamedVariable result;
            result.Primary = ptrVal;
            result.Storage = nullptr;
            result.MoveSourceStorage = argNV.Storage;
            result.CxxClosureLenders = argNV.CxxClosureLenders;
            if (auto* source = argNV.Storage != nullptr
                    ? compiler->FindStackVariableByStorage(argNV.Storage) : nullptr)
            {
                if (result.CxxClosureLenders.empty())
                    result.CxxClosureLenders = source->CxxClosureLenders;
                source->CxxClosureLenders.clear();
            }
            result.MoveSourceFieldPath = LLVMBackend::FieldMoveKey(argNV);
            result.BaseType = argNV.BaseType;
            result.TypeAndValue = argNV.TypeAndValue;
            result.IsExplicitMove = true;
            result.IsRvalue = true;
            return result;
        }
        const bool deferLoweredBraceMove = ctx == loweredBraceMoveSite_
            && argNV.Storage != nullptr && argNV.Storage != loweredBraceMoveDestination_
            && !argNV.TypeAndValue.Pointer
            && compiler->ResolveTypeAlias(argNV.TypeAndValue.TypeName)
                == compiler->ResolveTypeAlias(loweredBraceMoveTypeName_)
            && compiler->IsLoweredCFlatOnlyStruct(argNV.TypeAndValue.TypeName);

        // move on a named struct value type: capture the value, then zero the source storage
        // to leave it in a "moved-from" (default) state - enables safe delete[n] on the source.
        // Primitive value types (int, etc.) remain a no-op.
        // An interface fat-pointer ELEMENT (`move arr[i]`) takes no deferred transfer path (that one
        // would mark the whole array moved), and an interface is not a dataStructures entry, so the
        // slot below never zeroes. Zero the {i8*,i8*} here or the array teardown double-frees it.
        if (!argNV.TypeAndValue.Pointer && argNV.IsElementAccess && argNV.Storage
            && argNV.TypeAndValue.IsFatInterfaceValue()
            && !compiler->IsCoreUniqueType(argNV.TypeAndValue.TypeName))
        {
            compiler->builder->CreateStore(
                llvm::ConstantAggregateZero::get(compiler->GetFatPtrType()), argNV.Storage);
            // The element read demoted `unique` away; let the decl site re-derive ownership of a
            // dropped local from the DESTINATION type (a `unique IShape` slot must free on drop).
            compiler->lastMovedFromContainerSlot = true;
            LLVMBackend::NamedVariable result;
            result.Primary      = ptrVal;
            result.Storage      = nullptr;
            result.BaseType     = argNV.BaseType;
            result.TypeAndValue = argNV.TypeAndValue;
            result.IsExplicitMove = true;
            return result;
        }

        // move of a named OWNING `unique <interface>` local: an interface fat value is not a
        // dataStructures entry, so the branches below never zero it and the source keeps owning the
        // boxed heap object. Transfer it exactly as a thin `unique R*` move does - capture the value,
        // zero the source slot, and signal lastOwningResult so the init/assign path
        // adopts ownership (freeing any old destination box first). A direct call arg is deferred to
        // ApplyMoveParamTransfer; a non-owning (borrowed) source keeps the borrow-forward behavior.
        // The gate is ownership (IsVariableOwning), not strictly `unique`: a non-unique owning
        // interface local (e.g. from a move-returning call) is equally safe to move - it is not
        // auto-freed at scope exit, so consuming it here leaks nothing.
        if (!argNV.TypeAndValue.Pointer && argNV.Storage
            && argNV.TypeAndValue.IsFatInterfaceValue()
            && !compiler->IsCoreUniqueType(argNV.TypeAndValue.TypeName)
            && !IsDirectCallArgument(ctx)
            && !argNV.CallerName.empty() && compiler->IsVariableOwning(argNV.CallerName))
        {
            compiler->builder->CreateStore(
                llvm::ConstantAggregateZero::get(compiler->GetFatPtrType()), argNV.Storage);
            // Same-block deref guard only (see ExplicitlyMovedNull). The local stays plain-readable
            // as a zeroed fat pointer, exactly as a moved thin `unique R*` stays readable as null.
            compiler->MarkVariableExplicitlyMovedNull(argNV.CallerName, movedPointee);
            compiler->lastOwningResult = true;
            // Same fact by VALUE IDENTITY, as the thin-pointer move path does: a '?:' join must be
            // able to score this arm owning without trusting the sticky flag. The enclosing
            // IsVariableOwning gate keeps a move of a BORROWED interface out of the ledger.
            compiler->RegisterMovedOutPtrValue(ptrVal);
            LLVMBackend::NamedVariable result;
            result.Primary      = ptrVal;
            result.Storage      = nullptr;
            result.BaseType     = argNV.BaseType;
            result.TypeAndValue = argNV.TypeAndValue;
            result.IsExplicitMove = true;
            return result;
        }

        if (!argNV.TypeAndValue.Pointer)
        {
            if (argNV.Storage && compiler->IsDataStructure(argNV.TypeAndValue.TypeName))
            {
                // Directly a call argument: DEFER the zeroing to ApplyMoveParamTransfer, which
                // runs after overload resolution and knows whether the bound parameter is 'move'.
                // Zeroing here would consume the value even for a borrowing param (silent leak)
                // and would strip CallerName, inverting the move/borrow overload tie-breaker.
                // An ELEMENT access (`move a->keys[i]`) is excluded: the deferred path keeps
                // CallerName, and the move-tracking it drives marks the WHOLE field moved even
                // though only one element left. Keep the eager behavior there.
                if (IsDirectCallArgument(ctx) && !argNV.IsElementAccess)
                {
                    if (NamedVarIsString(argNV))
                        argNV.IsOwningString = argNV.IsOwningString
                            || (!argNV.CallerName.empty() && compiler->IsVariableOwningString(argNV.CallerName));
                    argNV.IsExplicitMove = true;
                    return argNV;
                }
                // A trivially copyable C++ class whose std::move(x) pick is not the byte copy:
                // build a temporary through it; the source stays live (consumed), as in C++.
                if (ctx == trivialCxxMoveSite_)
                {
                    LLVMBackend::NamedVariable built = argNV;
                    std::string refusal;
                    const std::string sourceName = !argNV.CallerName.empty() ? argNV.CallerName
                        : ctx->unaryExpression() != nullptr ? ctx->unaryExpression()->getText()
                                                            : std::string();
                    if (compiler->MaterializeTrivialCxxExplicitMove(built, sourceName, refusal))
                    {
                        if (!argNV.IsElementAccess && argNV.FieldName.empty()
                            && !argNV.CallerName.empty())
                            compiler->MarkVariableMoved(argNV.CallerName, movedPointee);
                        built.CallerName.clear();
                        built.IsExplicitMove = true;
                        built.IsRvalue = true;
                        return built;
                    }
                    if (!refusal.empty())
                    {
                        LogErrorContext(ctx, refusal);
                        return {};
                    }
                }

                if (deferLoweredBraceMove)
                {
                    if (compiler->IsCoreUniqueType(argNV.TypeAndValue.TypeName)
                        && !argNV.IsElementAccess && argNV.FieldName.empty())
                        compiler->RecordNullSet(argNV.CallerName, movedPointee);
                    if (!argNV.IsElementAccess && argNV.FieldName.empty()
                        && !argNV.CallerName.empty())
                        compiler->MarkVariableExplicitlyMovedNull(argNV.CallerName, movedPointee);
                    // A moved function value (thin or fat closure) stays readable as null, like unique;
                    // an unguarded call is diagnosed at the call site (IsExplicitlyMovedNullHere).
                    const bool callableValue = argNV.TypeAndValue.IsFunctionPointer
                        || compiler->GetEncodedClosureType(argNV.TypeAndValue.TypeName) != nullptr
                        || argNV.TypeAndValue.TypeName == "__c_fn_ptr";
                    if (!argNV.IsElementAccess && argNV.FieldName.empty()
                        && !argNV.IsBorrowed && !callableValue
                        && !compiler->IsCoreUniqueType(argNV.TypeAndValue.TypeName))
                        compiler->MarkVariableMoved(argNV.CallerName, movedPointee);
                    markMovedPath();
                    if (std::string key = compiler->UniqueFieldNullKey(argNV); !key.empty())
                        compiler->RecordNullSet(key);
                    compiler->lastOwningResult = true;
                    if (!argNV.IsBorrowed)
                        compiler->RegisterMovedOutPtrValue(ptrVal);
                    if (argNV.IsElementAccess)
                        compiler->lastMovedFromContainerSlot = true;

                    LLVMBackend::NamedVariable result = argNV;
                    result.Primary = ptrVal;
                    result.TypeAndValue.IsMove = true;
                    result.IsExplicitMove = true;
                    result.IsRvalue = true;
                    return result;
                }

                llvm::Type* structType = compiler->GetType(argNV.TypeAndValue);
                if (structType)
                    compiler->builder->CreateStore(
                        llvm::ConstantAggregateZero::get(structType), argNV.Storage);
                // Only a WHOLE-variable move nulls the variable. An element or field move
                // (`move n->values[k]`) leaves the base live, so it must not enter the ledger.
                if (compiler->IsCoreUniqueType(argNV.TypeAndValue.TypeName)
                    && !argNV.IsElementAccess && argNV.FieldName.empty())
                    compiler->RecordNullSet(argNV.CallerName, movedPointee);
                if (!argNV.IsElementAccess && argNV.FieldName.empty() && !argNV.CallerName.empty())
                    compiler->MarkVariableExplicitlyMovedNull(argNV.CallerName, movedPointee);
                const bool callableValue = argNV.TypeAndValue.IsFunctionPointer
                    || compiler->GetEncodedClosureType(argNV.TypeAndValue.TypeName) != nullptr
                    || argNV.TypeAndValue.TypeName == "__c_fn_ptr";
                if (!argNV.IsElementAccess && argNV.FieldName.empty() && !argNV.IsBorrowed
                    && !callableValue && !compiler->IsCoreUniqueType(argNV.TypeAndValue.TypeName))
                    compiler->MarkVariableMoved(argNV.CallerName, movedPointee);
                markMovedPath();
                // A field move of a `unique` wrapper nulls the FIELD path, like a moved local.
                if (std::string key = compiler->UniqueFieldNullKey(argNV); !key.empty())
                    compiler->RecordNullSet(key);
                // Signal ownership transfer exactly as the POINTER branch below does. Without it
                // `return move r` is classified as a BORROW and its owned bits are stripped (leak).
                compiler->lastOwningResult = true;
                // Same fact by VALUE IDENTITY, as the pointer/interface branches do: the source is
                // zeroed, so a '?:' join must be able to score this arm owning without the sticky
                // flag. A move of a BORROWED source transfers nothing (the real owner still frees
                // it), so it stays OUT - the join then scores it non-owning and suppresses.
                if (!argNV.IsBorrowed)
                    compiler->RegisterMovedOutPtrValue(ptrVal);
                // A value/string ELEMENT (`_ = move _data[i]`) is detached here with Storage null,
                // so the discard path cannot free it via a Storage-backed temp helper. Flag the
                // slot move so the discard site materializes the value and runs its full teardown.
                if (argNV.IsElementAccess)
                    compiler->lastMovedFromContainerSlot = true;

                LLVMBackend::NamedVariable result;
                result.Primary      = ptrVal;       // original value captured before zeroing
                result.Storage      = nullptr;      // prevent re-load from zeroed storage
                result.MoveSourceStorage = argNV.Storage;
                result.CxxClosureLenders = argNV.CxxClosureLenders;
                if (auto* source = argNV.Storage != nullptr
                        ? compiler->FindStackVariableByStorage(argNV.Storage) : nullptr)
                {
                    if (result.CxxClosureLenders.empty())
                        result.CxxClosureLenders = source->CxxClosureLenders;
                    source->CxxClosureLenders.clear();
                }
                result.MoveSourceFieldPath = LLVMBackend::FieldMoveKey(argNV);
                result.BaseType     = argNV.BaseType;
                result.TypeAndValue = argNV.TypeAndValue;
                result.TypeAndValue.IsMove = true;
                result.StackCharBufferBorrow = argNV.StackCharBufferBorrow;
                result.StackCharBufferSource = argNV.StackCharBufferSource;
                result.StackCharBufferScopeDepth = argNV.StackCharBufferScopeDepth;
                result.IsExplicitMove = true;
                result.IsAliasBorrow = argNV.IsAliasBorrow;
                result.IsViewElement = argNV.IsViewElement;
                result.BorrowsOwnedString = argNV.BorrowsOwnedString;
                result.FromOwningTempField = argNV.FromOwningTempField;
                result.IsBonded = argNV.IsBonded;
                result.BondByAddress = argNV.BondByAddress;
                result.BondedSources = argNV.BondedSources;
                // A moved closure keeps its capture names, so CxxClosureArgumentRefusal still
                // names a move-only or by-reference capture after the move (T213).
                result.LambdaCaptureNames = argNV.LambdaCaptureNames;
                result.LambdaReferenceCaptureNames = argNV.LambdaReferenceCaptureNames;
                // CallerName is deliberately NOT carried: the source is already detached and
                // zeroed, and keeping it re-marks the origin in the move tracker (false positives).
                // Carry the owning flag so 'move s' directly as a 'move string' arg TRANSFERS the buffer;
                // without it, the defensive copy in CreateOverloadedFunctionCall orphans the original.
                if (NamedVarIsString(argNV))
                    result.IsOwningString = argNV.IsOwningString
                        || (!argNV.CallerName.empty() && compiler->IsVariableOwningString(argNV.CallerName));
                return result;
            }
            // A scalar has no ownership state to detach, but an explicit move still carries the
            // C++ value category needed by a foreign rvalue-reference parameter.
            if (IsDirectCallArgument(ctx) && !argNV.IsElementAccess)
                argNV.IsExplicitMove = true;
            // `move ZERO` is a new value, not the macro's literal.
            argNV.IsCxxMacroGlobal = false;
            argNV.IsCxxNullIntegerMacro = false;
            return argNV;
        }

        if (argNV.Storage == nullptr)
        {
            LogErrorContext(ctx, "'move' expression requires an addressable source (field or local).");
            return {};
        }

        // Directly a call argument: DEFER the nulling to ApplyMoveParamTransfer, exactly as the
        // value path above. Returning a DETACHED variable here strips CallerName/IsOwning and
        // inverts the move/borrow tie-breaker, so 'take(move p)' picked the BORROW overload.
        // Element accesses stay eager (the deferred path would mark the whole field moved).
        if (IsDirectCallArgument(ctx) && !argNV.IsElementAccess)
        {
            argNV.IsExplicitMove = true;
            return argNV;
        }

        const bool moveSourceIsWholeBinding = !argNV.IsElementAccess
            && argNV.FieldName.empty() && argNV.OwningStructName.empty()
            && (llvm::isa<llvm::AllocaInst>(argNV.Storage)
                || llvm::isa<llvm::GlobalVariable>(argNV.Storage));
        // A field or element-field owns one scalar pointee, never the base array.
        if (!moveSourceIsWholeBinding)
        {
            argNV.AllocatedByRawNewArray = false;
            argNV.RawArrayLength = nullptr;
        }
        llvm::Value* movedRawArrayLength = moveSourceIsWholeBinding
            ? compiler->LoadRawArrayLength(argNV) : nullptr;
        bool movedRawNewArray = moveSourceIsWholeBinding && argNV.AllocatedByRawNewArray;
        llvm::Value* movedViewOwns = nullptr;
        bool movedViewGuaranteedOwns = false;
        if (moveSourceIsWholeBinding && argNV.TypeAndValue.IsArrayView)
        {
            movedViewGuaranteedOwns = argNV.IsOwning && !argNV.OwnFlagMayBeFalse;
            if (!llvm::isa<llvm::AllocaInst>(argNV.Storage))
            {
                const std::string sourcePathKey = argNV.MoveSourcePathKey.empty()
                    ? compiler->NonLocalElementPathKey(argNV) : argNV.MoveSourcePathKey;
                const bool nonLocalMayBeBorrowed = compiler->NonLocalViewMayBeBorrowed(
                    argNV.Storage, sourcePathKey).value_or(false);
                movedViewGuaranteedOwns = movedViewGuaranteedOwns
                    || !nonLocalMayBeBorrowed;
            }
            if (argNV.IsOwning && argNV.OwnFlagActive && argNV.OwnFlag != nullptr)
                movedViewOwns = compiler->builder->CreateLoad(
                    compiler->builder->getInt1Ty(), argNV.OwnFlag, "moved_view_owns");
            else
                movedViewOwns = compiler->builder->getInt1(movedViewGuaranteedOwns);
        }

        // An explicit move of a borrow created after an owning pointer was rebound can consume
        // that current owner: the borrow was made after the source acquired this same value.
        if (argNV.BorrowsOwningLocal && argNV.OwningLocalBorrowAfterRebind
            && argNV.OwningLocalStorage != nullptr)
        {
            const auto* owner = compiler->FindVariableByStorage(argNV.OwningLocalStorage);
            const std::string ownerName = compiler->FindVariableNameByStorage(
                argNV.OwningLocalStorage);
            if (owner != nullptr && owner->IsOwning && !ownerName.empty())
            {
                if (llvm::isa<llvm::AllocaInst>(owner->Storage)
                    || llvm::isa<llvm::GlobalVariable>(owner->Storage))
                {
                    auto* ownerPtrTy = llvm::dyn_cast<llvm::PointerType>(owner->BaseType);
                    if (ownerPtrTy != nullptr)
                        compiler->builder->CreateStore(
                            llvm::ConstantPointerNull::get(ownerPtrTy), owner->Storage);
                }
                else
                    compiler->SetVariableOwning(ownerName, false);
                compiler->MarkVariableMoved(ownerName);
                compiler->ConsumeOwnedNewTemp(owner->Primary);
            }
        }

        // Null the source (field GEP or local alloca) to transfer ownership.
        if (auto* ptrTy = llvm::dyn_cast<llvm::PointerType>(ptrVal->getType()))
        {
            compiler->builder->CreateStore(llvm::ConstantPointerNull::get(ptrTy), argNV.Storage);
            compiler->NoteOwnSlotLeavingLoad(ptrVal);
        }
        markMovedPath();
        if (moveSourceIsWholeBinding)
            compiler->StoreRawArrayLength(argNV, nullptr);

        // Explicit 'move a' of a whole thin pointer local: nulled but plain-readable (only a later
        // SAME-BLOCK deref errors - see MarkVariableExplicitlyMovedNull). Excludes a field-path move.
        if (argNV.FieldName.empty() && argNV.OwningStructName.empty() && !argNV.IsElementAccess
            && !argNV.CallerName.empty())
            compiler->MarkVariableExplicitlyMovedNull(argNV.CallerName, movedPointee);

        // A `unique` field-path move nulls the FIELD path, not the base (as `t.p = s.p` does).
        if (std::string key = compiler->UniqueFieldNullKey(argNV); !key.empty())
            compiler->RecordNullSet(key);

        // --sanitize=ownership (M1): record the move site for a tracked owning-pointer local.
        // A field-path move (OwningStructName set) is M2 scope, so its GEP is naturally untracked.
        compiler->SetOwnMoveOrigin(argNV.Storage,
            ctx->getStart()->getLine(), ctx->getStart()->getCharPositionInLine());

        // Signal ParseDeclaration to mark the target local as IsOwning. The over-alignment of the
        // moved-from source travels with the value, so the new owner frees it the same way.
        compiler->lastOwningResult = true;
        // Same fact, by VALUE IDENTITY, for consumers that must not trust the sticky flag: the
        // source is nulled, so nothing owns this value until a receiver adopts it (see a '?:' join).
        // A BORROWED source transfers nothing - nulling the callee's copy leaves the real owner
        // sole owner - so it must not enter the ledger, or a join scores it owning and the receiver
        // frees a live pointee. The ledger thus carries provenance: membership means "owns".
        // The other side of that provenance: a borrowed source is ledgered as provably NON-owning,
        // so a receiver that must own the value rejects it even after a '?:' join laundered it.
        bool srcBorrowLive = argNV.IsBorrowed;
        bool srcBorrowThroughField = false;
        if (srcBorrowLive && argNV.FieldName.empty() && !argNV.IsElementAccess
            && llvm::isa_and_nonnull<llvm::AllocaInst>(argNV.Storage))
        {
            const auto* srcBorrowBind = compiler->FindVariableByStorage(argNV.Storage);
            if (srcBorrowBind != nullptr
                && (srcBorrowBind->IsOwning || compiler->BorrowProofRetiredByRebind(*srcBorrowBind)))
                srcBorrowLive = false;
            if (srcBorrowBind != nullptr) srcBorrowThroughField = srcBorrowBind->BorrowedThroughField;
        }
        if (!srcBorrowLive)
        {
            compiler->RegisterMovedOutPtrValue(ptrVal);
            if (llvm::isa_and_nonnull<llvm::AllocaInst>(argNV.Storage))
                compiler->RegisterMovedOutSourceSlot(ptrVal, argNV.Storage);
        }
        else
        {
            compiler->RegisterMovedBorrowedPtrValue(ptrVal,
                argNV.BorrowedOrigin.empty() ? argNV.CallerName : argNV.BorrowedOrigin);
            if (srcBorrowThroughField) compiler->RegisterMovedBorrowedThroughField(ptrVal);
        }
        compiler->lastAllocAlignment = argNV.AllocAlignment;
        // Element-slot source (`move _data[i]`): the pointer read demoted `unique` to a bare borrow,
        // so let the decl site re-key ownership off the DEST type - a bare `T*` element must NOT own
        // (no spurious delete -> double-free), a `unique T*` element must own. A named local / param
        // / field source stays source-keyed (IsElementAccess false leaves lastOwningResult in force).
        if (argNV.IsElementAccess && argNV.FieldName.empty())
            compiler->lastMovedFromContainerSlot = true;

        LLVMBackend::NamedVariable result;
        result.Primary        = ptrVal;
        result.Storage        = nullptr;
        result.MoveSourceStorage = argNV.Storage;
        result.CxxClosureLenders = argNV.CxxClosureLenders;
        if (auto* source = argNV.Storage != nullptr
                ? compiler->FindStackVariableByStorage(argNV.Storage) : nullptr)
        {
            if (result.CxxClosureLenders.empty())
                result.CxxClosureLenders = source->CxxClosureLenders;
            source->CxxClosureLenders.clear();
        }
        result.MoveSourcePathKey = compiler->NonLocalElementPathKey(argNV);
        result.MoveSourceFieldPath = LLVMBackend::FieldMoveKey(argNV);
        result.BaseType       = ptrVal ? ptrVal->getType() : nullptr;
        result.TypeAndValue   = argNV.TypeAndValue;
        result.IsExplicitMove = true;
        result.AllocAlignment = argNV.AllocAlignment;
        // Provenance: the moved value's allocation alignment is DETERMINATE when the source slot's
        // is. A field / element / deref source is governed by its declared slot type; a whole
        // binding by its tracked allocation, its own carried provenance, or a parameter clause.
        // Deref sources are excluded: an `E**` can be filled by `&local` past every store door, so
        // only field / element slots (whose stores RejectFieldAllocAlignMismatch checks) are proof.
        const bool moveSourceIsCheckedSlot = argNV.IsElementAccess
            || !argNV.FieldName.empty() || !argNV.OwningStructName.empty();
        result.AllocAlignKnown = (!moveSourceIsWholeBinding && moveSourceIsCheckedSlot)
            || argNV.AllocAlignKnown
            || argNV.IsNewAllocated
            || argNV.AllocatedByRawNewArray
            || (argNV.TypeAndValue.IsArrayView && argNV.IsOwning)
            || compiler->IsFunctionParameterStorage(argNV.Storage);
        result.AllocatedByRawNewArray = movedRawNewArray;
        result.RawArrayLength = movedRawArrayLength;
        if (moveSourceIsWholeBinding && argNV.TypeAndValue.IsArrayView)
            compiler->RegisterRawArrayResult(ptrVal, movedRawArrayLength,
                argNV.AllocAlignment, movedViewGuaranteedOwns, movedViewOwns, true);
        // A 'move' of a BORROWED pointer parameter transfers nothing - the caller still owns the
        // pointee. Carry the provenance so a store into a `unique` field (a deferred delete, exactly
        // like the already-blocked `delete b`) is rejected, and so a PLAIN `T*` destination declines
        // to adopt, instead of double-freeing.
        result.IsBorrowed     = srcBorrowLive;
        result.IsOwning       = argNV.IsOwning || movedViewGuaranteedOwns;
        result.BorrowedOrigin = srcBorrowLive ? argNV.BorrowedOrigin : std::string();
        return result;
    }

LLVMBackend::NamedVariable MainListener::ParseOperatorStringExpression(CFlatParser::OperatorStringExpressionContext* ctx) {
        auto* compiler = Compiler(ctx);
        DeclExpectedTypeScope operatorStringExpectedScope(&declExpectedType, {});
        DiagnoseAssignmentArguments(ctx->argumentExpressionList());

        // Collect arguments passed to operator string(...)
        std::vector<LLVMBackend::NamedVariable> arguments;
        if (auto* argList = ctx->argumentExpressionList())
        {
            for (auto* argExpr : argList->argumentNamedExpression())
            {
                llvm::Value* argVal = ParseAssignmentExpression(argExpr->assignmentExpression());
                if (!argVal) break;
                LLVMBackend::NamedVariable argVar;
                argVar.Primary = argVal;
                argVar.BaseType = argVal->getType();
                arguments.push_back(argVar);
            }
        }

        // Dispatch to the global "operator string" overload matching the argument types.
        auto result = compiler->CreateOverloadedFunctionCall("operator string", arguments);
        if (!result) { LogErrorContext(ctx, "'operator string' is not defined"); return {}; }

        LLVMBackend::NamedVariable ret;
        ret.Primary = result;
        ret.TypeAndValue.TypeName = "string";
        ret.TypeAndValue.IsInterface = false;
        ret.TypeAndValue.Pointer = false;
        return ret;
    }

LLVMBackend::TypeAndValue MainListener::ParseSimdTypeSpec(CFlatParser::SimdTypeSpecifierContext* sd) {
        LLVMBackend::TypeAndValue tv;
        PrimitiveTypeError elemError;
        std::string elemType = CanonicalTypeSpecifierText(
            sd->typeSpecifier(), sd->multiWordTypeSuffix(), false, &elemError);
        if (HasPrimitiveTypeError(elemError))
            LogErrorContext(sd, LocalizePrimitiveTypeError(Compiler(), elemError));
        auto substIt = activeTypeSubstitutions.find(elemType);
        if (substIt != activeTypeSubstitutions.end())
            elemType = substIt->second;
        uint64_t lanes = 0;
        std::string err;
        if (!TryParseSimdLaneCount(Compiler(), sd->assignmentExpression(), lanes, err))
            LogErrorContext(sd, err);
        tv.TypeName = elemType;
        tv.IsSimd = true;
        tv.SimdLanes = lanes;
        return tv;
    }

LLVMBackend::NamedVariable MainListener::ParseSimdStaticMethod(
        CFlatParser::PostfixExpressionContext* ctx, CFlatParser::SimdTypeSpecifierContext* simdSpec) {
        auto* compiler = Compiler(ctx);
        LLVMBackend::NamedVariable result;

        // Method name is the identifier after the dot; the call args are the one argument list.
        auto identifiers = ctx->memberNameToken();
        auto argLists = ctx->argumentExpressionList();
        if (identifiers.empty())
        {
            // No '.method' at all: this is a TYPE spelled where a value belongs - usually a
            // `simd<T,N>*` / `simd<T,N>[N]*` declaration the parser re-read as a multiplication.
            std::string spelling = simdSpec->getText();
            std::string whole = ctx->getText();
            bool fixedArray = whole.size() > spelling.size() && whole[spelling.size()] == '[';
            LogErrorContext(ctx, SimdPointerTypeMessage(spelling, fixedArray));
            return result;
        }
        if (argLists.empty())
        {
            LogErrorContext(ctx, "simd<T,N> supports the static calls '.load(array, index)' and '.store(vector, array, index)'.");
            return result;
        }
        std::string method = identifiers[0]->getText();

        LLVMBackend::TypeAndValue simdTv = ParseSimdTypeSpec(simdSpec);
        auto* vecTy = llvm::cast<llvm::FixedVectorType>(compiler->GetType(simdTv));
        auto* elemTy = vecTy->getElementType();
        auto* int64Ty = compiler->builder->getInt64Ty();
        llvm::Align al = compiler->module->getDataLayout().getABITypeAlign(elemTy);

        std::vector<LLVMBackend::NamedVariable> argNVs;
        for (auto* na : argLists[0]->argumentNamedExpression())
            argNVs.push_back(ParseAssignmentExpressionNamed(na->assignmentExpression()));
        auto argValue = [&](size_t i) -> llvm::Value* {
            return argNVs[i].Primary ? argNVs[i].Primary : LoadNamedVariable(argNVs[i]);
        };

        if (method == "load")
        {
            if (argNVs.size() != 2)
                LogErrorContext(ctx, "simd<T,N>.load expects (array, index).");
            llvm::Value* basePtr = argValue(0);
            llvm::Value* index   = compiler->CreateCast(argValue(1), int64Ty, true);
            llvm::Value* elemPtr = compiler->CreateGEP(elemTy, basePtr, index);
            result.Primary = compiler->builder->CreateAlignedLoad(vecTy, elemPtr, al, "simdload");
            result.BaseType = vecTy;
            result.TypeAndValue = simdTv;
            result.Storage = nullptr;
        }
        else if (method == "store")
        {
            if (argNVs.size() != 3)
                LogErrorContext(ctx, "simd<T,N>.store expects (vector, array, index).");
            llvm::Value* vecVal = argValue(0);
            if (vecVal->getType() != vecTy)
                LogErrorContext(ctx, "simd<T,N>.store: the value's type does not match the simd<T,N> it is stored through.");
            llvm::Value* basePtr = argValue(1);
            llvm::Value* index   = compiler->CreateCast(argValue(2), int64Ty, true);
            llvm::Value* elemPtr = compiler->CreateGEP(elemTy, basePtr, index);
            compiler->builder->CreateAlignedStore(vecVal, elemPtr, al);
            // store returns nothing
        }
        else if (auto* intrin = LookupSimdMathIntrinsic(method))
        {
            // Elementwise vector math: lowers to a true SIMD instruction (sqrtps, roundps,
            // vfmadd, minps, ...). FP-only; every operand must be the same simd<T,N>.
            if (!elemTy->isFloatingPointTy())
                LogErrorContext(ctx, std::format("simd<T,N>.{} requires a floating-point element type.", method));
            if ((int)argNVs.size() != intrin->Arity)
                LogErrorContext(ctx, std::format("simd<T,N>.{} expects {} vector argument(s).", method, intrin->Arity));
            std::vector<llvm::Value*> ops;
            ops.reserve(ops.size() + static_cast<size_t>(intrin->Arity));
            for (int k = 0; k < intrin->Arity; k++)
                ops.push_back(requireSimdArg(ctx, argValue(k), k, vecTy, method));
            auto* fn = llvm::Intrinsic::getOrInsertDeclaration(compiler->module.get(), intrin->Id, {vecTy});
            result.Primary = compiler->builder->CreateCall(fn, ops, "simdmath");
            result.BaseType = vecTy;
            result.TypeAndValue = simdTv;
            result.Storage = nullptr;
        }
        else if (method == "clamp")
        {
            // clamp(x, lo, hi) = minnum(maxnum(x, lo), hi). Two intrinsics, all-vector.
            if (!elemTy->isFloatingPointTy())
                LogErrorContext(ctx, "simd<T,N>.clamp requires a floating-point element type.");
            if (argNVs.size() != 3)
                LogErrorContext(ctx, "simd<T,N>.clamp expects (value, lo, hi).");
            llvm::Value* x  = requireSimdArg(ctx, argValue(0), 0, vecTy, method);
            llvm::Value* lo = requireSimdArg(ctx, argValue(1), 1, vecTy, method);
            llvm::Value* hi = requireSimdArg(ctx, argValue(2), 2, vecTy, method);
            auto* maxFn = llvm::Intrinsic::getOrInsertDeclaration(compiler->module.get(), llvm::Intrinsic::maxnum, {vecTy});
            auto* minFn = llvm::Intrinsic::getOrInsertDeclaration(compiler->module.get(), llvm::Intrinsic::minnum, {vecTy});
            llvm::Value* lower = compiler->builder->CreateCall(maxFn, {x, lo}, "simdclamplo");
            result.Primary = compiler->builder->CreateCall(minFn, {lower, hi}, "simdclamp");
            result.BaseType = vecTy;
            result.TypeAndValue = simdTv;
            result.Storage = nullptr;
        }
        else if (method == "sign")
        {
            // sign(x) = (x == 0) ? 0 : copysign(1, x). One intrinsic + a vector compare/select.
            // Matches Math.sign for finite values incl. +-0; a NaN lane yields +-1 (copysign of
            // the NaN's sign bit) rather than 0, since copysign is a pure bit op.
            if (!elemTy->isFloatingPointTy())
                LogErrorContext(ctx, "simd<T,N>.sign requires a floating-point element type.");
            if (argNVs.size() != 1)
                LogErrorContext(ctx, "simd<T,N>.sign expects (vector).");
            llvm::Value* x = requireSimdArg(ctx, argValue(0), 0, vecTy, method);
            auto* one  = llvm::ConstantFP::get(vecTy, 1.0);
            auto* zero = llvm::ConstantFP::get(vecTy, 0.0);
            auto* csFn = llvm::Intrinsic::getOrInsertDeclaration(compiler->module.get(), llvm::Intrinsic::copysign, {vecTy});
            llvm::Value* magOne = compiler->builder->CreateCall(csFn, {one, x}, "simdsignmag");
            llvm::Value* isZero = compiler->builder->CreateFCmpOEQ(x, zero, "simdsigniszero");
            result.Primary = compiler->builder->CreateSelect(isZero, zero, magOne, "simdsign");
            result.BaseType = vecTy;
            result.TypeAndValue = simdTv;
            result.Storage = nullptr;
        }
        else if (method == "select")
        {
            // select(mask, a, b): per-lane branchless select. Lowers to a single vblendvps/vpblendvb;
            // mask must be simd<bool,N> from a vector comparison. Works for any element type.
            if (argNVs.size() != 3)
                LogErrorContext(ctx, "simd<T,N>.select expects (mask, a, b).");
            llvm::Value* mask = argValue(0);
            auto* maskTy = llvm::dyn_cast<llvm::FixedVectorType>(mask->getType());
            if (!maskTy || !maskTy->getElementType()->isIntegerTy(1)
                || maskTy->getNumElements() != vecTy->getNumElements())
                LogErrorContext(ctx, "simd<T,N>.select: the mask must be a simd<bool,N> of the same lane count (produce one with a vector comparison, e.g. `a < b`).");
            llvm::Value* a = requireSimdArg(ctx, argValue(1), 1, vecTy, method);
            llvm::Value* b = requireSimdArg(ctx, argValue(2), 2, vecTy, method);
            result.Primary = compiler->builder->CreateSelect(mask, a, b, "simdselect");
            result.BaseType = vecTy;
            result.TypeAndValue = simdTv;
            result.Storage = nullptr;
        }
        else if (method == "reduce_add" || method == "reduce_min" || method == "reduce_max")
        {
            // Horizontal reduction across all N lanes -> scalar T. reduce_add on float is
            // reassociating (matches the vectorize contract's reduction semantics); reduce_min/max
            // follow the minnum/maxnum NaN family used by the existing min/max vector ops. See doc/HPC.md.
            if (elemTy->isIntegerTy(1))
            {
                LogErrorContext(ctx, std::format("simd<T,N>.{} does not support bool lanes; use select/mask ops instead.", method));
                return result;
            }
            if (argNVs.size() != 1)
            {
                LogErrorContext(ctx, std::format("simd<T,N>.{} expects 1 argument.", method));
                return result;
            }
            llvm::Value* v = requireSimdArg(ctx, argValue(0), 0, vecTy, method);
            bool isFloat = elemTy->isFloatingPointTy();
            bool isUnsigned = simdTv.IsUnsignedInteger() != -1;
            llvm::Value* reduced;
            if (method == "reduce_add")
            {
                if (isFloat)
                {
                    auto* fn = llvm::Intrinsic::getOrInsertDeclaration(compiler->module.get(), llvm::Intrinsic::vector_reduce_fadd, {vecTy});
                    auto* start = llvm::ConstantFP::get(elemTy, 0.0);
                    auto* call = compiler->builder->CreateCall(fn, {start, v}, "simdreduceadd");
                    call->setHasAllowReassoc(true);
                    reduced = call;
                }
                else
                {
                    auto* fn = llvm::Intrinsic::getOrInsertDeclaration(compiler->module.get(), llvm::Intrinsic::vector_reduce_add, {vecTy});
                    reduced = compiler->builder->CreateCall(fn, {v}, "simdreduceadd");
                }
            }
            else
            {
                bool isMin = (method == "reduce_min");
                llvm::Intrinsic::ID rid = isFloat
                    ? (isMin ? llvm::Intrinsic::vector_reduce_fmin : llvm::Intrinsic::vector_reduce_fmax)
                    : isUnsigned
                        ? (isMin ? llvm::Intrinsic::vector_reduce_umin : llvm::Intrinsic::vector_reduce_umax)
                        : (isMin ? llvm::Intrinsic::vector_reduce_smin : llvm::Intrinsic::vector_reduce_smax);
                auto* fn = llvm::Intrinsic::getOrInsertDeclaration(compiler->module.get(), rid, {vecTy});
                reduced = compiler->builder->CreateCall(fn, {v}, "simdreduce");
            }
            result.Primary = reduced;
            result.BaseType = elemTy;
            result.TypeAndValue = simdTv;
            // Result is a scalar T, not a vector - drop the simd-ness (mirrors the lane-read path).
            result.TypeAndValue.IsSimd = false;
            result.TypeAndValue.SimdLanes = 0;
            result.Storage = nullptr;
        }
        else if (method == "lanes")
        {
            // Constant iota {0,1,...,N-1} as a simd<T,N>. Needed to build a tail mask
            // (compare lanes() against a splat of the remaining count) - there is no other
            // ergonomic way to construct a lane-index vector today.
            if (argNVs.size() != 0)
            {
                LogErrorContext(ctx, "simd<T,N>.lanes expects no arguments.");
                return result;
            }
            unsigned n = (unsigned)vecTy->getNumElements();
            std::vector<llvm::Constant*> elems;
            elems.reserve(n);
            for (unsigned i = 0; i < n; i++)
                elems.push_back(elemTy->isFloatingPointTy()
                    ? (llvm::Constant*)LLVMBackend::ConvertIntegerConstantToFloatingPoint(
                        llvm::ConstantInt::get(llvm::Type::getInt32Ty(*compiler->context), i),
                        elemTy, true)
                    : (llvm::Constant*)llvm::ConstantInt::get(elemTy, i));
            result.Primary = llvm::ConstantVector::get(elems);
            result.BaseType = vecTy;
            result.TypeAndValue = simdTv;
            result.Storage = nullptr;
        }
        else if (method == "load_masked")
        {
            // Masked load: lanes where mask[i]=0 take the passthru value instead of reading
            // memory (the tail-free replacement for a scalar epilogue loop). passthru is a
            // required argument, not defaulted, to keep dispatch dumb.
            if (argNVs.size() != 4)
            {
                LogErrorContext(ctx, "simd<T,N>.load_masked expects (array, index, mask, passthru).");
                return result;
            }
            llvm::Value* basePtr = argValue(0);
            llvm::Value* index   = compiler->CreateCast(argValue(1), int64Ty, true);
            llvm::Value* mask    = argValue(2);
            auto* maskTy = llvm::dyn_cast<llvm::FixedVectorType>(mask->getType());
            if (!maskTy || !maskTy->getElementType()->isIntegerTy(1)
                || maskTy->getNumElements() != vecTy->getNumElements())
            {
                LogErrorContext(ctx, "simd<T,N>.load_masked: the mask must be a simd<bool,N> of the same lane count (produce one with a vector comparison, e.g. `simd<T,N>.lanes() < remaining`).");
                return result;
            }
            llvm::Value* passthru = requireSimdArg(ctx, argValue(3), 3, vecTy, method);
            llvm::Value* elemPtr = compiler->CreateGEP(elemTy, basePtr, index);
            result.Primary = compiler->builder->CreateCall(
                llvm::Intrinsic::getOrInsertDeclaration(compiler->module.get(),
                    llvm::Intrinsic::masked_load, { vecTy, compiler->builder->getPtrTy() }),
                { elemPtr, mask, passthru }, "simdloadmasked");
            result.BaseType = vecTy;
            result.TypeAndValue = simdTv;
            result.Storage = nullptr;
        }
        else if (method == "store_masked")
        {
            // Masked store: lanes where mask[i]=0 leave the destination memory untouched.
            if (argNVs.size() != 4)
            {
                LogErrorContext(ctx, "simd<T,N>.store_masked expects (vector, array, index, mask).");
                return result;
            }
            llvm::Value* vecVal  = requireSimdArg(ctx, argValue(0), 0, vecTy, method);
            llvm::Value* basePtr = argValue(1);
            llvm::Value* index   = compiler->CreateCast(argValue(2), int64Ty, true);
            llvm::Value* mask    = argValue(3);
            auto* maskTy = llvm::dyn_cast<llvm::FixedVectorType>(mask->getType());
            if (!maskTy || !maskTy->getElementType()->isIntegerTy(1)
                || maskTy->getNumElements() != vecTy->getNumElements())
            {
                LogErrorContext(ctx, "simd<T,N>.store_masked: the mask must be a simd<bool,N> of the same lane count (produce one with a vector comparison, e.g. `simd<T,N>.lanes() < remaining`).");
                return result;
            }
            llvm::Value* elemPtr = compiler->CreateGEP(elemTy, basePtr, index);
            compiler->builder->CreateCall(
                llvm::Intrinsic::getOrInsertDeclaration(compiler->module.get(),
                    llvm::Intrinsic::masked_store, { vecTy, compiler->builder->getPtrTy() }),
                { vecVal, elemPtr, mask });
            // store returns nothing
        }
        else if (method == "load_gather")
        {
            // Unmasked MVP gather: one CreateGEP with a vector index yields <N x ptr>, then
            // llvm.masked.gather with an all-true mask constant. Passthru is poison since every
            // lane is always read (no masked-off lane to preserve).
            if (argNVs.size() != 2)
            {
                LogErrorContext(ctx, "simd<T,N>.load_gather expects (array, index_vector).");
                return result;
            }
            llvm::Value* basePtr = argValue(0);
            llvm::Value* idxVec  = argValue(1);
            auto* idxVecTy = llvm::dyn_cast<llvm::FixedVectorType>(idxVec->getType());
            if (!idxVecTy
                || !(idxVecTy->getElementType()->isIntegerTy(32) || idxVecTy->getElementType()->isIntegerTy(64))
                || idxVecTy->getNumElements() != vecTy->getNumElements())
            {
                LogErrorContext(ctx, "simd<T,N>.load_gather: the index vector must be a simd<int,N> or simd<long,N> of the same lane count.");
                return result;
            }
            llvm::Value* ptrVec = compiler->CreateGEP(elemTy, basePtr, idxVec);
            auto* ptrVecTy = llvm::FixedVectorType::get(compiler->builder->getPtrTy(), vecTy->getNumElements());
            auto* maskTy = llvm::FixedVectorType::get(compiler->builder->getInt1Ty(), vecTy->getNumElements());
            llvm::Value* allTrue = llvm::ConstantInt::get(maskTy, 1);
            llvm::Value* passthru = llvm::PoisonValue::get(vecTy);
            result.Primary = compiler->builder->CreateCall(
                llvm::Intrinsic::getOrInsertDeclaration(compiler->module.get(),
                    llvm::Intrinsic::masked_gather, { vecTy, ptrVecTy }),
                { ptrVec, allTrue, passthru }, "simdgather");
            result.BaseType = vecTy;
            result.TypeAndValue = simdTv;
            result.Storage = nullptr;
        }
        else if (method == "store_scatter")
        {
            // Unmasked MVP scatter. Duplicate indices store in lane order (last lane wins) -
            // LLVM's defined behaviour for llvm.masked.scatter; see doc/HPC.md.
            if (argNVs.size() != 3)
            {
                LogErrorContext(ctx, "simd<T,N>.store_scatter expects (vector, array, index_vector).");
                return result;
            }
            llvm::Value* vecVal  = requireSimdArg(ctx, argValue(0), 0, vecTy, method);
            llvm::Value* basePtr = argValue(1);
            llvm::Value* idxVec  = argValue(2);
            auto* idxVecTy = llvm::dyn_cast<llvm::FixedVectorType>(idxVec->getType());
            if (!idxVecTy
                || !(idxVecTy->getElementType()->isIntegerTy(32) || idxVecTy->getElementType()->isIntegerTy(64))
                || idxVecTy->getNumElements() != vecTy->getNumElements())
            {
                LogErrorContext(ctx, "simd<T,N>.store_scatter: the index vector must be a simd<int,N> or simd<long,N> of the same lane count.");
                return result;
            }
            llvm::Value* ptrVec = compiler->CreateGEP(elemTy, basePtr, idxVec);
            auto* ptrVecTy = llvm::FixedVectorType::get(compiler->builder->getPtrTy(), vecTy->getNumElements());
            auto* maskTy = llvm::FixedVectorType::get(compiler->builder->getInt1Ty(), vecTy->getNumElements());
            llvm::Value* allTrue = llvm::ConstantInt::get(maskTy, 1);
            compiler->builder->CreateCall(
                llvm::Intrinsic::getOrInsertDeclaration(compiler->module.get(),
                    llvm::Intrinsic::masked_scatter, { vecTy, ptrVecTy }),
                { vecVal, ptrVec, allTrue });
            // store returns nothing
        }
        else
        {
            LogErrorContext(ctx, std::format("'{}' is not a static method on simd<T,N>; expected 'load', 'store', 'select', 'lanes', "
                "'reduce_add', 'reduce_min', 'reduce_max', 'load_masked', 'store_masked', 'load_gather', 'store_scatter', "
                "or a vector math op (sqrt, abs, floor, ceil, trunc, round, rint, min, max, clamp, copysign, sign, fma).", method));
        }
        return result;
    }

llvm::Value* MainListener::requireSimdArg(antlr4::ParserRuleContext* ctx, llvm::Value* v, int k,
                                llvm::Type* vecTy, const std::string& method) {
        if (v->getType() != vecTy)
            LogErrorContext(ctx, std::format("simd<T,N>.{}: argument {} must be a simd<T,N> of the same type.", method, k + 1));
        return v;
    }

const MainListener::SimdMathIntrinsic* MainListener::LookupSimdMathIntrinsic(const std::string& name) {
        static const std::unordered_map<std::string, SimdMathIntrinsic> table = {
            {"sqrt",     {llvm::Intrinsic::sqrt,     1}},
            {"abs",      {llvm::Intrinsic::fabs,     1}},
            {"fabs",     {llvm::Intrinsic::fabs,     1}},
            {"floor",    {llvm::Intrinsic::floor,    1}},
            {"ceil",     {llvm::Intrinsic::ceil,     1}},
            {"trunc",    {llvm::Intrinsic::trunc,    1}},
            {"round",    {llvm::Intrinsic::round,    1}},
            {"rint",     {llvm::Intrinsic::rint,     1}},
            {"min",      {llvm::Intrinsic::minnum,   2}},
            {"max",      {llvm::Intrinsic::maxnum,   2}},
            {"copysign", {llvm::Intrinsic::copysign, 2}},
            {"fma",      {llvm::Intrinsic::fma,      3}},
        };
        auto it = table.find(name);
        return it == table.end() ? nullptr : &it->second;
    }

bool MainListener::FindTransparentAnonFieldPath(const std::string& structName,
        const std::string& fieldName, std::vector<unsigned>& path)
{
        auto* compiler = Compiler();
        std::function<bool(const LLVMBackend::StructData&)> find =
            [&](const LLVMBackend::StructData& data) -> bool
        {
            for (unsigned i = 0; i < data.StructFields.size(); ++i)
            {
                const auto& field = data.StructFields[i];
                if (!field.IsAnonymousMember) continue;
                const auto nested = compiler->GetDataStructure(field.TypeName);
                if (nested.StructType == nullptr) continue;
                for (unsigned k = 0; k < nested.StructFields.size(); ++k)
                    if (nested.StructFields[k].VariableName == fieldName)
                    {
                        path.push_back(i);
                        path.push_back(k);
                        return true;
                    }
                path.push_back(i);
                if (find(nested)) return true;
                path.pop_back();
            }
            return false;
        };
        path.clear();
        return find(compiler->GetDataStructure(structName));
}

bool MainListener::ResolveTransparentAnonField(
        antlr4::ParserRuleContext* ctx,
        const LLVMBackend::NamedVariable& structVar,
        const std::string& fieldName,
        LLVMBackend::NamedVariable& out) {
        auto* compiler = Compiler(ctx);
        // Transparent access needs an addressable base to GEP through the anonymous member chain.
        if (!structVar.Storage) return false;
        auto* topStructTy = llvm::dyn_cast_or_null<llvm::StructType>(structVar.BaseType);
        if (!topStructTy) return false;

        // DFS through marked anonymous members for fieldName. `chain` holds field indices root-to-leaf.
        // bitfieldHit captured by value: GetDataStructure returns a copy, so a pointer into `inner` would dangle.
        std::vector<int> chain;
        bool isBitfieldHit = false;
        LLVMBackend::BitfieldInfo bitfieldHit{};
        std::function<bool(const LLVMBackend::StructData&)> dfs =
            [&](const LLVMBackend::StructData& sd) -> bool
        {
            for (int i = 0; i < (int)sd.StructFields.size(); ++i)
            {
                const auto& f = sd.StructFields[i];
                if (!f.IsAnonymousMember) continue;
                const auto& inner = compiler->GetDataStructure(f.TypeName);
                if (!inner.StructType) continue;
                for (int k = 0; k < (int)inner.StructFields.size(); ++k)
                {
                    if (inner.StructFields[k].VariableName == fieldName)
                    {
                        chain.push_back(i);
                        chain.push_back(k);
                        return true;
                    }
                }
                for (const auto& b : inner.Bitfields)
                {
                    if (b.Name == fieldName)
                    {
                        isBitfieldHit = true;
                        bitfieldHit = b;
                        chain.push_back(i);
                        chain.push_back((int)b.StorageFieldIndex);
                        return true;
                    }
                }
                chain.push_back(i);
                if (dfs(inner)) return true;
                chain.pop_back();
            }
            return false;
        };
        if (!dfs(compiler->GetDataStructure(topStructTy)) || chain.empty())
            return false;

        // Walk the chain, advancing the base pointer through each anonymous-member hop. A union
        // member sits at offset 0 (pointer unchanged); a struct member needs a StructGEP by index.
        llvm::Value* ptr = structVar.Storage;
        LLVMBackend::StructData curSd = compiler->GetDataStructure(topStructTy);
        llvm::Type* curType = topStructTy;
        for (size_t k = 0; k + 1 < chain.size(); ++k)
        {
            const auto& anonField = curSd.StructFields[chain[k]];
            if (!curSd.IsUnion)
                ptr = compiler->CreateCxxFieldGEP(curSd, ptr, (unsigned)chain[k]);
            curType = compiler->GetType(anonField);
            curSd = compiler->GetDataStructure(anonField.TypeName);
        }

        // Bitfield leaf: chain.back() is the storage-word slot in the deepest record. GEP to that
        // word (union arms alias at offset 0; struct fields need a StructGEP) and reuse the shared
        // bitfield codegen so the read masks/shifts and the write goes through bitfieldAssign.
        if (isBitfieldHit)
        {
            const auto& storageField = curSd.StructFields[chain.back()];
            auto* storageTy = compiler->GetType(storageField);
            llvm::Value* storagePtr = curSd.IsUnion
                ? ptr
                : compiler->CreateStructGEP(curType, ptr, (unsigned)chain.back());
            out = compiler->EmitBitfieldRead(storagePtr, storageTy, bitfieldHit,
                                             structVar.TypeAndValue.VariableName,
                                             structVar.TypeAndValue.TypeName);
            out.IsBorrowed = structVar.IsBorrowed;
            out.BorrowedOrigin = structVar.BorrowedOrigin;
            return true;
        }

        const auto& leafField = curSd.StructFields[chain.back()];
        auto* leafLLVMType = compiler->GetType(leafField);
        out = {};
        if (curSd.IsUnion)
        {
            // Leaf lives directly in an anonymous union arm: it aliases at offset 0.
            out.Storage = ptr;
            out.UnionFieldType = leafLLVMType;
            if (llvm::isa<llvm::ArrayType>(leafLLVMType))
            {
                out.Primary = nullptr;
                out.BaseType = leafLLVMType;
            }
            else
            {
                out.Primary = compiler->CreateLoad(leafLLVMType, ptr);
                out.BaseType = out.Primary->getType();
            }
        }
        else
        {
            out.Storage = compiler->CreateCxxFieldGEP(curSd, ptr, (unsigned)chain.back());
            if (llvm::isa<llvm::ArrayType>(leafLLVMType))
            {
                out.Primary = nullptr;
                out.BaseType = leafLLVMType;
            }
            else
            {
                out.Primary = compiler->CreateLoad(leafLLVMType, out.Storage);
                out.BaseType = out.Primary->getType();
            }
        }
        out.TypeAndValue = leafField;
        out.TypeAndValue.ParentVariableName = structVar.TypeAndValue.VariableName;
        out.OwningStructName = structVar.TypeAndValue.TypeName;
        out.FieldName = fieldName;
        // Preserve unique-field provenance across a later cast (see the sibling named-field read).
        if (leafField.IsUnique
            && leafField.Pointer)
            out.IsUniqueFieldAlias = true;
        out.IsBorrowed = structVar.IsBorrowed;
        out.BorrowedOrigin = structVar.BorrowedOrigin;
        return true;
    }

void MainListener::ClassifyPostfixCallResult(
        antlr4::ParserRuleContext* ctx,
        LLVMBackend::NamedVariable& result,
        LLVMBackend::NamedVariable& structVar,
        LLVMBackend::NamedVariable& interfaceVar) {
        PrepareAliasCallResult(ctx, result);
        if (result.TypeAndValue.IsInterface)
        {
            interfaceVar = result;
            structVar = {};
        }
        else if (result.BaseType && result.BaseType->isStructTy())
        {
            structVar = result;
            interfaceVar = {};
        }
        else if (!result.TypeAndValue.TypeName.empty() && result.TypeAndValue.Pointer
                 && Compiler(ctx)->GetDataStructure(result.TypeAndValue.TypeName).StructType != nullptr)
        {
            structVar = result;
            interfaceVar = {};
        }
}

void MainListener::CarryCxxOperatorResult(
        antlr4::ParserRuleContext* ctx, llvm::Value* result, bool moreOperands,
        llvm::Value*& accValue, llvm::Value*& accStorage, bool& accIsRvalue,
        llvm::Type*& accRefType, std::string& accSourceTypeName) {
        auto* compiler = Compiler(ctx);
        const auto& returnType = compiler->lastCallReturnType;
        accSourceTypeName = compiler->IsCxxRecord(returnType.TypeName)
            ? returnType.TypeName : std::string();
        accValue = result;
        accStorage = nullptr;
        accIsRvalue = !returnType.IsAlias;
        accRefType = nullptr;
        if (result == nullptr) return;
        if (moreOperands && result->getType()->isPointerTy())
        {
            llvm::Value* storage = compiler->lastCxxRetTemp_ != nullptr
                ? compiler->lastCxxRetTemp_ : result;
            auto* allocation = llvm::dyn_cast<llvm::AllocaInst>(storage);
            auto* storedType = allocation != nullptr
                ? llvm::dyn_cast<llvm::StructType>(allocation->getAllocatedType()) : nullptr;
            if (storedType != nullptr && storedType->hasName()
                && compiler->IsCxxRecord(storedType->getName().str()))
            {
                accStorage = storage;
                accValue = compiler->CreateLoad(storedType, storage);
                accIsRvalue = false;
                return;
            }
        }
        if (moreOperands && result->getType()->isPointerTy()
            && !returnType.TypeName.empty() && compiler->IsCxxRecord(returnType.TypeName)
            && (returnType.IsAlias || compiler->lastCxxRetTemp_ != nullptr))
        {
            auto valueTypeInfo = returnType;
            valueTypeInfo.Pointer = false;
            valueTypeInfo.PointerDepth = 0;
            valueTypeInfo.ElemPointer = false;
            auto* valueType = compiler->GetType(valueTypeInfo);
            if (valueType != nullptr && valueType->isStructTy())
            {
                accStorage = returnType.IsAlias ? result : compiler->lastCxxRetTemp_;
                accValue = compiler->CreateLoad(valueType, accStorage);
                accIsRvalue = false;
                return;
            }
        }
        // A by-value C++ class return lives in its sret temporary; that slot is the operand's
        // storage for a following operator, exactly as the additive path already carried it.
        if (!returnType.IsAlias)
        {
            if (result->getType()->isStructTy() && compiler->lastCxxRetValue_ == result)
                accStorage = compiler->lastCxxRetTemp_;
            return;
        }
        if (!result->getType()->isPointerTy()) return;
        auto* valueType = compiler->GetType(returnType);
        if (valueType == nullptr) return;
        if (!moreOperands)
        {
            if (compiler->IsCxxRecord(returnType.TypeName))
            {
                accStorage = result;
                accIsRvalue = false;
                accRefType = valueType;
                return;
            }
            accRefType = valueType;
            return;
        }
        accValue = compiler->CreateLoad(valueType, result);
        accStorage = result;
        accIsRvalue = false;
}

llvm::Type* MainListener::CxxReferenceResultType(
        antlr4::ParserRuleContext* ctx, const LLVMBackend::NamedVariable& result,
        llvm::Value* value) {
        if (!result.TypeAndValue.IsAlias || result.TypeAndValue.Pointer || value == nullptr
            || !value->getType()->isPointerTy())
            return nullptr;
        return Compiler(ctx)->GetType(result.TypeAndValue);
}

void MainListener::NormalizeCxxReferenceNamed(
        antlr4::ParserRuleContext* ctx, LLVMBackend::NamedVariable& operand) {
        if (operand.CxxRefValueType == nullptr && operand.TypeAndValue.Pointer
            && operand.TypeAndValue.IsRvalueRef && !operand.TypeAndValue.ElemPointer
            && !operand.TypeAndValue.IsFunctionPointer
            && Compiler(ctx)->IsCxxRecord(operand.TypeAndValue.TypeName)
            && operand.Primary != nullptr && operand.Primary->getType()->isPointerTy())
        {
            auto valueTypeInfo = operand.TypeAndValue;
            valueTypeInfo.Pointer = false;
            valueTypeInfo.PointerDepth = 0;
            valueTypeInfo.ElemPointer = false;
            auto* valueType = Compiler(ctx)->GetType(valueTypeInfo);
            if (valueType != nullptr && valueType->isStructTy())
                operand.CxxRefValueType = valueType;
        }
        if (operand.CxxRefValueType == nullptr || operand.Primary == nullptr
            || !operand.Primary->getType()->isPointerTy())
            return;
        operand.Storage = operand.Primary;
        operand.BaseType = operand.CxxRefValueType;
        operand.Primary = Compiler(ctx)->CreateLoad(operand.CxxRefValueType, operand.Storage);
        operand.CxxRefValueType = nullptr;
        // Constness limits moves, but a const T&& result is still an xvalue.
        operand.IsRvalue = operand.TypeAndValue.IsRvalueRef;
        operand.TypeAndValue.Pointer = false;
        operand.TypeAndValue.PointerDepth = 0;
        operand.TypeAndValue.ElemPointer = false;
}

void MainListener::NormalizeCxxReferenceOperand(
        antlr4::ParserRuleContext* ctx, LLVMBackend::TypedValue& operand) {
        if (operand.cxxRefValueType == nullptr && operand.hasSourceType
            && operand.sourceType.Pointer && operand.sourceType.IsRvalueRef
            && !operand.sourceType.ElemPointer && !operand.sourceType.IsFunctionPointer
            && Compiler(ctx)->IsCxxRecord(operand.sourceType.TypeName)
            && operand.value != nullptr && operand.value->getType()->isPointerTy())
        {
            auto valueTypeInfo = operand.sourceType;
            valueTypeInfo.Pointer = false;
            valueTypeInfo.PointerDepth = 0;
            valueTypeInfo.ElemPointer = false;
            auto* valueType = Compiler(ctx)->GetType(valueTypeInfo);
            if (valueType != nullptr && valueType->isStructTy())
                operand.cxxRefValueType = valueType;
        }
        if (operand.cxxRefValueType == nullptr || operand.value == nullptr
            || !operand.value->getType()->isPointerTy())
            return;
        auto* compiler = Compiler(ctx);
        operand.receiverStorage = operand.value;
        operand.value = compiler->CreateLoad(operand.cxxRefValueType, operand.value);
        operand.cxxRefValueType = nullptr;
        operand.isRvalue = operand.hasSourceType && operand.sourceType.IsRvalueRef;
        if (operand.hasSourceType)
        {
            operand.sourceType.Pointer = false;
            operand.sourceType.PointerDepth = 0;
            operand.sourceType.ElemPointer = false;
        }
        operand.elemType = nullptr;
        operand.pointerDepth = 0;
        operand.elemPointer = false;
}

void MainListener::PrepareAliasCallResult(
        antlr4::ParserRuleContext* ctx,
        LLVMBackend::NamedVariable& result, bool markRvalue) {
        // Provenance identifies a value that is itself a field. A method or operator result is
        // derived from its receiver, even when the receiver was an implicit-this field.
        result.RootIsBorrowedThisField = false;
        result.ThisFieldPath.clear();
        result.IsTopLevelConst = result.TypeAndValue.IsCxxConstValueReturn;
        if (markRvalue && result.Primary != nullptr && !result.TypeAndValue.IsAlias)
            result.IsRvalue = true;
        // A nontrivial foreign C++ class returned by value lives in the sret temporary until the
        // end of the full expression. Keep that storage for a chained member call; reloading the
        // backend's placeholder value would copy only its first ABI word.
        auto* compiler = Compiler(ctx);
        if (!result.TypeAndValue.Pointer && !result.TypeAndValue.IsAlias
            && result.TypeAndValue.TypeName.size() > 0
            && compiler->lastCxxRetTemp_ != nullptr
            && compiler->IsForeignNontrivialCxxReturnClass(result.TypeAndValue.TypeName))
        {
            result.Storage = compiler->lastCxxRetTemp_;
            result.BaseType = compiler->GetType(result.TypeAndValue);
            result.Primary = nullptr;
        }
        auto* unaryCtx = dynamic_cast<CFlatParser::UnaryExpressionContext*>(ctx);
        const bool isDereferenceOperatorCall = unaryCtx != nullptr
            && unaryCtx->unaryOperator() != nullptr
            && unaryCtx->unaryOperator()->getText() == "*";
        if (isDereferenceOperatorCall
            && result.TypeAndValue.Pointer && result.TypeAndValue.IsRvalueRef
            && !result.TypeAndValue.IsFunctionPointer
            && compiler->IsCxxRecord(result.TypeAndValue.TypeName)
            && result.Primary != nullptr && result.Primary->getType()->isPointerTy())
        {
            auto valueTypeInfo = result.TypeAndValue;
            valueTypeInfo.Pointer = false;
            valueTypeInfo.PointerDepth = 0;
            valueTypeInfo.ElemPointer = false;
            valueTypeInfo.IsAlias = false;
            auto* valueType = compiler->GetType(valueTypeInfo);
            if (valueType != nullptr && valueType->isStructTy())
            {
                result.TypeAndValue.Pointer = false;
                result.TypeAndValue.PointerDepth = 0;
                result.TypeAndValue.ElemPointer = false;
                result.BaseType = valueType;
                result.Storage = result.Primary;
                result.Primary = nullptr;
                result.IsRvalue = true;
            }
        }
        if (!result.TypeAndValue.IsAlias
            || (result.TypeAndValue.Pointer && !result.TypeAndValue.IsCxxRefToPointer)
            || result.Primary == nullptr)
            return;

        if (!result.Primary->getType()->isPointerTy())
            return;

        auto* valueType = compiler->GetType(result.TypeAndValue);
        if (valueType == nullptr) return;
        result.BaseType = valueType;
        result.Storage = result.Primary;
        result.Primary = nullptr;
        // `const T&`, `const T&&`, `T *const &`: the referent is read-only (ruling 2026-09-27);
        // IsCxxConstReferent derives it from the callee's return ConstMask.
}

void MainListener::NormalizeCxxRvalueReferenceArgument(
        antlr4::ParserRuleContext* ctx, LLVMBackend::NamedVariable& argument) {
        auto& type = argument.TypeAndValue;
        if (!type.IsRvalueRef || type.CxxConstRef() || argument.IsExplicitMove) return;

        // A C++ T&& result carries the referent slot in its pointer-shaped return value. Keep
        // that slot and expose the same storage/value category as `move *p` at argument doors.
        if (argument.Storage == nullptr && argument.Primary != nullptr
            && argument.Primary->getType()->isPointerTy()) {
            argument.Storage = argument.Primary;
            argument.Primary = nullptr;
        }
        if (type.Pointer && !type.IsCxxRefToPointer && Compiler(ctx)->IsCxxRecord(type.TypeName)) {
            type.Pointer = false;
            type.PointerDepth = 0;
            type.ElemPointer = false;
        }
        type.IsAlias = false;
        type.IsRvalueRef = false;
        type.IsCxxRefToPointer = false;
        if (!type.Pointer && Compiler(ctx)->IsCxxRecord(type.TypeName))
            type.IsMove = true;
        argument.IsExplicitMove = true;
        argument.IsNormalizedCxxXvalue = true;
        argument.IsRvalue = true;
        if (argument.Storage != nullptr)
            argument.BaseType = Compiler(ctx)->GetType(type);
}

/*
 * CFlat spells "bind x by reference" as `&x` at a reference parameter. At a C++ constructor whose
 * `T*&` / `T* const&` parameter takes this position, `&p` over a `T*` lvalue therefore binds p's
 * slot: the argument becomes that lvalue (Storage = the computed address), not a `T**` prvalue.
 */
void MainListener::BindCxxCtorPointerSlotAddress(
        antlr4::ParserRuleContext* ctx, LLVMBackend::NamedVariable& argument,
        const std::string& typeName, size_t index, size_t count) {
        auto& type = argument.TypeAndValue;
        if (!argument.IsCxxAddressOfPointerValue || argument.IsCxxConstPointerSlot()
            || !type.IsProvenDoublePointer()
            || argument.Storage != nullptr
            || argument.Primary == nullptr || !argument.Primary->getType()->isPointerTy())
            return;
        // Only a bare `&operand` (single-child chain down to the unary expression).
        antlr4::tree::ParseTree* node = ctx;
        CFlatParser::UnaryExpressionContext* unary = nullptr;
        while (node != nullptr)
        {
            if (auto* u = dynamic_cast<CFlatParser::UnaryExpressionContext*>(node);
                u != nullptr && u->unaryOperator() != nullptr)
            {
                unary = u;
                break;
            }
            if (node->children.size() != 1) return;
            node = node->children[0];
        }
        if (unary == nullptr || unary->unaryOperator()->getText() != "&"
            || unary->castExpression() == nullptr)
            return;
        auto* compiler = Compiler(ctx);
        const auto* info = compiler->GetCxxClassInfoWithConstructors(typeName);
        // A constructor template may take the `T**` as written (U&& = T**); leave the pick to clang.
        if (info == nullptr) return;
        bool wantsSlot = false;
        for (const auto& c : info->constructors)
        {
            if (c.isDeleted || c.params.size() < count + 1 || index + 1 >= c.params.size())
                continue;
            const auto& want = c.params[index + 1];
            if (!want.Pointer || want.IsFunctionPointer || want.IsRvalueRef) continue;
            // A `T**` parameter (or `T**&`) takes the address as written.
            if (want.IsProvenDoublePointer()) return;
            if (want.IsCxxRefToPointer && want.TypeName == type.TypeName) wantsSlot = true;
        }
        if (!wantsSlot) return;
        type.ElemPointer = false;
        type.PointerDepth = 1;
        argument.Storage = argument.Primary;
        argument.Primary = nullptr;
        argument.BaseType = compiler->GetType(type);
        argument.IsRvalue = false;
        const std::string operand = unary->castExpression()->getText();
        if (argument.CallerName.empty() && compiler->FindLiveNamedVariable(operand) != nullptr)
            argument.CallerName = operand;
}

bool MainListener::RefuseConstWrite(antlr4::ParserRuleContext* ctx,
                                    const LLVMBackend::NamedVariable& target,
                                    llvm::Value* destination,
                                    const std::string& nativeOperation,
                                    const std::string& cxxOperation) {
        // Every write operator (=, +=, ++, ...) shares the const-element refusal: the target object
        // itself is const (a pointer element's own level, not its pointee's).
        const bool constElement = target.TypeAndValue.Pointer
            ? target.IsElementAccess && target.TypeAndValue.IsConstObject()
            : target.TypeAndValue.PointeeConstBit();
        // A mutable member is writable.
        const bool cxxChecked = constElement || !target.TypeAndValue.IsCxxMutableField;
        if (cxxChecked && RefuseCxxConstReferentWrite(ctx, destination, cxxOperation, constElement))
            return true;
        if (RefuseConstObjectWrite(ctx, target, destination, nativeOperation)) return true;
        return cxxChecked && RefuseCxxConstReferentWrite(ctx, destination, cxxOperation, constElement, true);
}

bool MainListener::RefuseCxxConstReferentWrite(antlr4::ParserRuleContext* ctx,
                                               llvm::Value* destination,
                                               const std::string& operation,
                                               bool constElement,
                                               bool fallback) {
        std::string target = ctx != nullptr ? ctx->getText() : std::string();
        if (target.ends_with("++") || target.ends_with("--")) target.resize(target.size() - 2);
        auto* compiler = Compiler(ctx);
        if (compiler->IsCxxMutableFieldAccess(destination)) return false;
        if (fallback)
        {
            // A field of the object a native constructor is building is still being set.
            for (llvm::Value* step = destination; auto* gep = llvm::dyn_cast_or_null<llvm::GEPOperator>(step);
                 step = gep->getPointerOperand())
                if (compiler->IsConstructingStorage(gep->getPointerOperand())) return false;
            if (!constElement) return false;
            LogErrorContext(ctx, std::format(
                "cannot {} '{}': it is an element of a C++ const reference, so its storage is "
                "read-only", operation, target));
            return true;
        }
        if (destination != nullptr) {
            auto* constGlobal = llvm::dyn_cast<llvm::GlobalVariable>(
                llvm::getUnderlyingObject(destination));
            auto isCConstObject = [&]() { return IsReachedFromCGlobal(destination); };
            llvm::Value* pointeeBase = destination;
            while (auto* gep = llvm::dyn_cast_or_null<llvm::GetElementPtrInst>(pointeeBase))
                pointeeBase = gep->getPointerOperand();
            const LLVMBackend::TypeAndValue* pointerSlot = nullptr;
            bool cxxPointerField = false;
            if (auto* load = llvm::dyn_cast_or_null<llvm::LoadInst>(pointeeBase))
            {
                pointerSlot = compiler->FindDeclaredFieldTypeAndValueForStorage(
                    load->getPointerOperand());
                if (auto* slotGep = llvm::dyn_cast<llvm::GetElementPtrInst>(
                        load->getPointerOperand()))
                {
                    if (const llvm::MDNode* field = slotGep->getMetadata("cflat.cxx.field"))
                    {
                        if (field->getNumOperands() == 2)
                            if (auto* typeName = llvm::dyn_cast<llvm::MDString>(field->getOperand(0)))
                                cxxPointerField = compiler->IsCxxRecord(typeName->getString().str());
                    }
                    else if (auto* type = llvm::dyn_cast<llvm::StructType>(
                                 slotGep->getSourceElementType());
                             type != nullptr && type->hasName())
                        cxxPointerField = compiler->IsCxxRecord(type->getName().str());
                }
            }
            const bool constPointeeAccess = constElement && cxxPointerField
                && pointerSlot != nullptr && pointerSlot->Pointer
                && pointerSlot->PointeeConstBit()
                && destination == pointeeBase;
            // By value inside a const C++ class object, or a const global of C/C++ (any write).
            const auto cause = compiler->FindConstWriteCause(destination);
            using Kind = LLVMBackend::ConstWriteCause::CauseKind;
            const bool constField = cause.CxxObject && (cause.Kind == Kind::Field || cause.Kind == Kind::Object);
            const bool constGlobalObject = compiler->IsConstHeaderGlobal(constGlobal);
            // Through a pointer to const that a const C++ global decayed into (`(*kRecords).c`).
            const bool constGlobalPointee = cause.Kind == Kind::ThroughPointer
                && GlobalBehindWrite(destination, false) != nullptr;
            // A const field the native check names (#2) keeps that wording.
            if (constPointeeAccess || constField || constGlobalObject || constGlobalPointee
                || (constElement && cause.Kind != Kind::Field && isCConstObject())) {
                const std::string advice = operation == "assign to"
                    ? "Copy it into a local and assign to that instead."
                    : "Copy it into a local and modify that instead.";
                if (constPointeeAccess)
                {
                    LogErrorContext(ctx, std::format(
                        "cannot {} '{}': it is reached through a const C++ pointee, so it is "
                        "read-only. {}", operation, target, advice));
                    return true;
                }
                if (isCConstObject())
                    LogErrorContext(ctx, std::format(
                        "cannot {} '{}': it is a const C object, so its storage is read-only. {}",
                        operation, target, advice));
                else
                    LogErrorContext(ctx, std::format(
                        "cannot {} '{}': it is a const C++ object, so its storage is read-only. {}",
                        operation, target, advice));
                return true;
            }
        }
        if (destination == nullptr) return false;
        if (compiler->IsCxxConstReferent(destination))
        {
            LogErrorContext(ctx, std::format(
                "cannot {} '{}': it is reached through a C++ const reference result or const pointer result, so it is "
                "read-only. Copy it into a local and modify that instead.", operation, target));
            return true;
        }
        // `++` / `--` on a const namespace object; `=` has its own check in the assignment path.
        auto* constGlobal = operation == "assign to" ? nullptr
            : llvm::dyn_cast<llvm::GlobalVariable>(llvm::getUnderlyingObject(destination));
        // ConstMask decides (const C and C++ globals); cGlobalSymbols_ only picks wording.
        const bool constGlobalSymbol = compiler->IsConstHeaderGlobal(constGlobal);
        if (constGlobalSymbol && compiler->cGlobalSymbols_.count(constGlobal->getName().str()) != 0)
            LogErrorContext(ctx, std::format(
                "cannot {} '{}': it is a const C object, so its storage is read-only. "
                "Copy it into a local and modify that instead.", operation, target));
        else if (constGlobalSymbol)
            LogErrorContext(ctx, std::format(
                "cannot {} '{}': it is a const C++ object, so its storage is read-only. "
                "Copy it into a local and modify that instead.", operation, target));
        return constGlobalSymbol;
}

// The global a write target lies in, directly or through a pointer loaded from it (`*gp`, a
// decayed `*arr`, a local whose stored value is such a global). Wording only.
llvm::GlobalVariable* MainListener::GlobalBehindWrite(llvm::Value* destination, bool cGlobalsOnly) {
        if (destination == nullptr) return nullptr;
        auto* compiler = Compiler();
        const auto pick = [&](llvm::GlobalVariable* g) -> llvm::GlobalVariable* {
            if (g == nullptr) return nullptr;
            const std::string name = g->getName().str();
            return (cGlobalsOnly ? compiler->cGlobalSymbols_.count(name) != 0
                                 : compiler->IsConstHeaderGlobal(g)) ? g : nullptr;
        };
        llvm::Value* base = llvm::getUnderlyingObject(destination);
        // A pointer read from the global itself, not from an element or field of it.
        if (auto* load = llvm::dyn_cast<llvm::LoadInst>(base))
            base = load->getPointerOperand();
        if (auto* slot = llvm::dyn_cast<llvm::AllocaInst>(base)) {
            for (llvm::User* user : slot->users()) {
                auto* store = llvm::dyn_cast<llvm::StoreInst>(user);
                if (store == nullptr || store->getPointerOperand() != slot) continue;
                if (auto* g = pick(llvm::dyn_cast<llvm::GlobalVariable>(
                        llvm::getUnderlyingObject(store->getValueOperand()))))
                    return g;
            }
            return nullptr;
        }
        return pick(llvm::dyn_cast<llvm::GlobalVariable>(base));
}

bool MainListener::IsReachedFromCGlobal(llvm::Value* destination) {
        return GlobalBehindWrite(destination, true) != nullptr;
}

bool MainListener::RefuseConstObjectWrite(antlr4::ParserRuleContext* ctx,
                                          const LLVMBackend::NamedVariable& target,
                                          llvm::Value* destination,
                                          const std::string& operation) {
        if (ctx == nullptr || destination == nullptr) return false;
        auto cause = Compiler(ctx)->FindConstWriteCause(destination);
        using Kind = LLVMBackend::ConstWriteCause::CauseKind;
        // A union arm shares the union's address (no field GEP), so the arm comes from the access.
        const bool unionArm = cause.Kind == Kind::None && target.UnionFieldType != nullptr
            && Compiler(ctx)->IsNativeConstObject(target.TypeAndValue)
            && !Compiler(ctx)->IsConstructingStorage(destination);
        if (unionArm) cause.Name = target.TypeAndValue.VariableName;
        if (cause.Kind == Kind::None && !unionArm) return false;
        // `move` of a const C++ class object is std::move: it binds the copy constructor.
        if (operation == "move" && Compiler(ctx)->IsCxxRecord(target.TypeAndValue.TypeName)) return false;
        // A const element reached from a C global keeps the C-object wording of
        // RefuseCxxConstReferentWrite, which reports every such write.
        if (cause.Kind == Kind::ThroughPointer && operation != "move"
            && target.TypeAndValue.PointeeConstBit()
            && (target.IsElementAccess || !target.TypeAndValue.Pointer)
            && IsReachedFromCGlobal(destination))
            return false;
        std::string path = ctx->getText();
        if (path.ends_with("++") || path.ends_with("--")) path.resize(path.size() - 2);
        const std::string verb = operation == "move" ? "move out of" : operation;
        if (cause.Kind == Kind::Field || unionArm)
        {
            LogErrorContext(ctx, std::format(
                "cannot {} '{}': field '{}' is const and can only be set during construction",
                verb, path, cause.Name));
            return true;
        }
        // #9: the pointer is the implicit `(const S)*` this of the explicit const method being
        // compiled - judged by that method's own this parameter, never by a name pattern.
        auto* insertBlock = Compiler(ctx)->builder->GetInsertBlock();
        const auto* currentSym = cause.Kind == Kind::ThroughPointer && insertBlock != nullptr
            ? Compiler(ctx)->GetFunctionSymbol(insertBlock->getParent()) : nullptr;
        if (currentSym != nullptr && currentSym->IsExplicitConstMethod()
            && cause.Name == currentSym->Parameters.front().VariableName)
        {
            LogErrorContext(ctx, std::format("method '{}' is declared const but writes '{}'",
                                             Compiler(ctx)->GetCurrentFunctionName(), path));
            return true;
        }
        if (cause.Kind == Kind::ThroughPointer)
        {
            LogErrorContext(ctx, std::format(
                "cannot {} '{}': it is reached through a pointer to const ('{}')", verb, path, cause.Name));
            return true;
        }
        const auto* root = &cause.Root;
        // The whole object, or a field / element reached from it by value.
        const bool wholeObject = destination == root->Storage
            && (target.BaseType == nullptr || target.BaseType == root->Type);
        if (operation == "move")
            LogErrorContext(ctx, std::format(
                "cannot move out of const '{}': 'move' nulls the source, which is a write. Let the "
                "last use consume it, or drop 'const'", path));
        else if (wholeObject)
            LogErrorContext(ctx, std::format("cannot {} '{}': it is declared const", operation, path));
        else
            LogErrorContext(ctx, std::format("cannot {} '{}': '{}' is const", operation, path, root->Name));
        return true;
}

bool MainListener::RefuseCxxConstReceiverCall(antlr4::ParserRuleContext* ctx,
                                              const std::vector<LLVMBackend::NamedVariable>& arguments,
                                              const std::string& receiverType,
                                              const std::string& member,
                                              const std::string& receiverText) {
        if (arguments.empty() || receiverType.empty()) return false;
        auto* compiler = Compiler(ctx);
        const int constKind = compiler->CxxConstReceiverKind(arguments.front());
        if (constKind == 0 || !compiler->CxxMemberIsOnlyNonConst(
                receiverType, member, arguments.size()))
            return false;
        const std::string object = receiverText.empty()
            ? arguments.front().TypeAndValue.VariableName : receiverText;
        const bool pointerReceiver = arguments.front().TypeAndValue.Pointer
            || arguments.front().TypeAndValue.PointeeConstBit();
        const std::string copyAdvice = pointerReceiver
            ? "Copy the pointee into a local ('auto c = *q;') and call it on that instead."
            : "Copy it into a local and call it on that instead.";
        LogErrorContext(ctx, std::format(
            "cannot call non-const member '{}' on '{}': {}, so it is read-only and only a const "
            "member function may be called on it. {}",
            member, object, constKind == 2 && compiler->IsCConstGlobalObject(arguments.front())
                ? "it is a const C object"
                : constKind == 3 ? "it is reached through a C++ const reference result or const pointer result"
                                 : "it is a const C++ object", copyAdvice));
        return true;
}

std::string MainListener::NextMemberName(CFlatParser::PostfixExpressionContext* ctx,
                                      antlr4::tree::ParseTree* opNode) {
        const auto& children = ctx->children;
        for (size_t i = 0; i + 1 < children.size(); ++i)
        {
            if (children[i] != opNode) continue;
            auto* next = children[i + 1];
            auto* nextRule = dynamic_cast<antlr4::RuleContext*>(next);
            if (nextRule != nullptr
                && nextRule->getRuleIndex() == CFlatParser::RuleMemberNameToken)
                return next->getText();
            if (next->getTreeType() != antlr4::tree::ParseTreeType::TERMINAL) return "";
            auto* term = dynamic_cast<antlr4::tree::TerminalNode*>(next);
            auto tok = term->getSymbol()->getType();
            if (tok == CFlatParser::Identifier || tok == CFlatParser::Move
                || tok == CFlatParser::String)
                return term->getText();
            return "";
        }
        return "";
    }

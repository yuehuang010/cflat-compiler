#include "MainListener.h"
#include <llvm/Support/SaveAndRestore.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IntrinsicInst.h>
#include "MainListener_ExpressionsShared.h"

std::string MainListener::UniqueHiddenGlobalName(LLVMBackend* compiler, const std::string& kind)
{
        const std::string prefix = std::string(1, char(1)) + "cflat.array_view." + kind + ".";
        for (uint64_t index = 0;; ++index)
        {
            std::string candidate = prefix + std::to_string(index);
            if (compiler->module->getNamedValue(candidate) == nullptr)
                return candidate;
        }
}

std::string MainListener::ParseTupleTypeSpecifier(
    CFlatParser::TupleTypeSpecifierContext* tupleSpec) {
        auto* compiler = Compiler(tupleSpec);
        auto elementContext = TupleTypeElementContext(tupleSpec);
        std::vector<std::string> typeArgs;
        if (tupleSpec->tupleTypePackEntry() != nullptr)
        {
            auto* packEntry = tupleSpec->tupleTypePackEntry();
            PrimitiveTypeError packError;
            std::string packName = CanonicalTypeSpecifierText(
                packEntry->typeSpecifier(), packEntry->multiWordTypeSuffix(), false, &packError);
            if (HasPrimitiveTypeError(packError))
                LogErrorContext(packEntry, LocalizePrimitiveTypeError(compiler, packError));
            auto packIt = activePackSubstitutions.find(packName);
            if (packIt != activePackSubstitutions.end()) typeArgs = packIt->second;
            else typeArgs.push_back(compiler->ResolveTypeArgBaseName(packName));
        }
        else
        {
            for (auto* entry : tupleSpec->tupleTypeEntry())
            {
                PrimitiveTypeError argError;
                std::string argName = ResolveTupleTypeElementArgName(
                    elementContext, entry, &argError);
                if (HasPrimitiveTypeError(argError))
                    LogErrorContext(entry, LocalizePrimitiveTypeError(compiler, argError));
                auto* typeSpec = entry->typeSpecifier();
                if (typeSpec != nullptr && typeSpec->genericIdentifier() != nullptr
                    && typeSpec->genericIdentifier()->genericTypeParameters() == nullptr
                    && !MentionsSignatureTypeParam(argName) && !compiler->IsKnownTypeName(argName)
                    && !compiler->IsTypeArgTypeKey(argName))
                    LogErrorContext(entry, compiler->LocalizeMessage(
                        "unknown type '{}'", { argName }));
                typeArgs.push_back(std::move(argName));
            }
        }
        std::string tupleName = MangledGenericName("tuple", typeArgs);
        tupleTypeArgs[tupleName] = typeArgs;
        EnsureTupleInstantiated(tupleName);
        return tupleName;
    }

TupleTypeElementResolveContext MainListener::TupleTypeElementContext(
    antlr4::ParserRuleContext* site)
{
        auto* compiler = Compiler(site);
        const std::string namespaceName = compiler->GetCurrentNamespace();
        std::unordered_set<std::string> localTypes;
        for (antlr4::tree::ParseTree* parent = site; parent != nullptr; parent = parent->parent)
            if (auto* function = dynamic_cast<CFlatParser::FunctionDefinitionContext*>(parent))
            {
                localTypes = FunctionLocalEnumNames(function);
                break;
            }
        return TupleTypeElementResolveContext{
            .Compiler = compiler,
            .Substitutions = &activeTypeSubstitutions,
            .Namespace = namespaceName,
            .LocalTypes = std::move(localTypes),
            .ResolveType = [this, namespaceName](auto* typeSpec, auto* suffix) {
                return ResolveTupleTypeElementSpecifier(
                    typeSpec, suffix, &activeTypeSubstitutions, namespaceName);
            },
            .ReportError = [this](antlr4::ParserRuleContext* errorSite, std::string message) {
                LogErrorContext(errorSite, std::move(message));
            },
            .ResolveUniqueType = [this](const std::string& pointee, bool isPointer,
                                        antlr4::ParserRuleContext* useSite) {
                auto* compiler = Compiler(useSite);
                std::string cxxError;
                const std::string cxxUnique = isPointer
                    ? compiler->CxxUniquePtrForUniqueKeyword(pointee, cxxError) : std::string();
                if (!cxxError.empty()) LogErrorContext(useSite, std::move(cxxError));
                if (!cxxUnique.empty()) return cxxUnique;
                const std::string uniqueType = MangledGenericName("unique", { pointee }, useSite);
                QueueGenericInstantiation("unique", { pointee }, uniqueType, useSite);
                return uniqueType;
            }
        };
}

std::string MainListener::ResolveTupleTypeElementSpecifier(
    CFlatParser::TypeSpecifierContext* typeSpec,
    CFlatParser::MultiWordTypeSuffixContext* suffix,
    const std::unordered_map<std::string, std::string>* substitutions,
    const std::string& namespaceName)
{
        if (typeSpec == nullptr) return {};
        auto* compiler = Compiler(typeSpec);
        LLVMBackend::NamespaceScope scope(compiler, namespaceName);
        if (typeSpec->tupleTypeSpecifier() != nullptr)
            return ParseTupleTypeSpecifier(typeSpec->tupleTypeSpecifier());
        if (typeSpec->functionPointerSpecifier() != nullptr)
            return EncodeClosureCodegen(typeSpec->functionPointerSpecifier());
        std::string genericBase;
        if (auto* generic = GenericSpecOf(typeSpec, genericBase))
        {
            std::vector<std::string> args;
            for (auto* entry : generic->typeParameterList()->typeParameterEntry())
                args.push_back(ResolveTypeArgEntry(entry));
            ResolveEnumTypeArguments(args);
            const std::string spelledBase = genericBase;
            const std::string enclosingScope = !structScopeStack.empty() ? structScopeStack.back()
                : EnclosingAggregateScopeFromContext(compiler, generic);
            genericBase = ResolveGenericTemplateSpellingAtScope(
                compiler, spelledBase, args, enclosingScope, generic, spelledBase, true);
            compiler->CompleteCxxTemplateTypeArgs(genericBase, args);
            std::string resolved = MangledGenericName(genericBase, args);
            if (!compiler->AnyGenericTypeTemplateNamed(genericBase))
            {
                std::string cxxError;
                if (!compiler->TryRequestCxxType(genericBase, args, resolved, cxxError)
                    && !cxxError.empty())
                    LogCxxErrorContext(typeSpec, cxxError);
                resolved = compiler->ResolveTypeAlias(resolved);
            }
            if (!compiler->AnyGenericTypeTemplateNamed(genericBase)
                || !QueueAndProcessLocalEnumInstantiation(
                    generic, genericBase, args, resolved, typeSpec))
                QueueGenericInstantiation(genericBase, args, resolved, typeSpec);
            return resolved;
        }
        if (typeSpec->typeName() != nullptr)
        {
            ConstTypeSyntax shape;
            if (ResolveConstTypeSyntax(compiler, typeSpec->typeName(), shape, substitutions,
                    [this](auto* entry) { return ResolveTypeArgEntry(entry); },
                    [this](auto* member) { return ResolveMemberTypeSpecifier(member); },
                    [this](auto* tuple) { return ParseTupleTypeSpecifier(tuple); },
                    &MainListener::ReportCxxTypeUseError, this))
                return compiler->ResolveTypeAlias(shape.TypeName)
                    + std::string(shape.PointerDepth, '*');
        }
        return compiler->ResolveTypeAlias(ParseTypeSpecifierName(typeSpec, suffix));
    }

std::string MainListener::ParseTypeSpecifierName(
    CFlatParser::TypeSpecifierContext* ctx,
    CFlatParser::MultiWordTypeSuffixContext* suffix) {
        if (auto* tupleSpec = ctx->tupleTypeSpecifier())
            return ParseTupleTypeSpecifier(tupleSpec);
        std::string base;
        if (auto* genParams = GenericSpecOf(ctx, base))
        {
            // Generic type: Box<int> -> Box$int
            // Also apply type substitutions to arguments (e.g. Box<T> with T=int -> Box$int)
            std::vector<std::string> args;
            // ResolveTypeArgEntry applies active substitutions AND recursively
            // resolves/queues nested generics (e.g. list<int> inside list<list<int>>).
            for (auto* entry : genParams->typeParameterList()->typeParameterEntry())
                args.push_back(ResolveTypeArgEntry(entry));
            ResolveEnumTypeArguments(args);
            const std::string spelledBase = base;
            const std::string enclosingScope = !structScopeStack.empty() ? structScopeStack.back()
                : EnclosingAggregateScopeFromContext(Compiler(ctx), genParams);
            base = ResolveGenericTemplateSpellingAtScope(
                Compiler(ctx), spelledBase, args, enclosingScope, genParams, spelledBase, true);
            Compiler(ctx)->CompleteCxxTemplateTypeArgs(base, args);
            std::string mangledName = MangledGenericName(base, args);
            if (Compiler(ctx)->AnyGenericTypeTemplateNamed(base))
                QueueAndProcessLocalEnumInstantiation(genParams, base, args, mangledName, ctx);
            return mangledName;
        }
        if (auto* memberType = ctx->memberTypeSpecifier())
            return ResolveMemberTypeSpecifier(memberType);
        PrimitiveTypeError canonicalError;
        std::string name = CanonicalTypeSpecifierText(ctx, suffix, false, &canonicalError);
        if (HasPrimitiveTypeError(canonicalError))
            LogErrorContext(ctx, LocalizePrimitiveTypeError(Compiler(), canonicalError));
        // Apply active type substitutions (for generic templates)
        auto it = activeTypeSubstitutions.find(name);
        // A substituted name is already resolved in the caller's scope - resolve it from the root
        // so the template's declaring namespace cannot rebind it (layer 3).
        bool wasSubstituted = it != activeTypeSubstitutions.end();
        if (it != activeTypeSubstitutions.end()) name = it->second;
        StripOwnershipQualifiers(name);  // resolve to the underlying LLVM type; storage markers only
        std::string defaultedAliasType;
        std::string defaultedAliasError;
        if (Compiler(ctx)->TryResolveDefaultedCxxGenericBaseAlias(
                name, defaultedAliasType, defaultedAliasError))
            return defaultedAliasType;
        if (!defaultedAliasError.empty()) LogCxxErrorContext(ctx, defaultedAliasError);
        std::string resolvedName = Compiler(ctx)->ResolveQualifiedName(name, wasSubstituted);
        if (!wasSubstituted && IsUnparameterizedNamedTypeSpecifier(ctx))
        {
            const auto aggregateScopes = EnclosingAggregateScopesForType(ctx);
            const std::string contextual = ResolveContextualTypeName(Compiler(ctx),
                ctx->getText(), resolvedName, ctx, aggregateScopes,
                NamespaceNameFromContext(Compiler(ctx), ctx));
            if (!contextual.empty()) resolvedName = contextual;
        }
        return resolvedName;
    }

int MainListener::SubstitutedConstDepth(CFlatParser::TypeSpecifierContext* ctx,
    CFlatParser::MultiWordTypeSuffixContext* suffix) {
        if (ctx == nullptr || activeTypeSubstitutions.empty() || ctx->tupleTypeSpecifier() != nullptr
            || ctx->memberTypeSpecifier() != nullptr)
            return 0;
        std::string base;
        if (GenericSpecOf(ctx, base) != nullptr) return 0;
        std::string name = CanonicalTypeSpecifierText(ctx, suffix, false, nullptr);
        return activeTypeSubstitutions.ConstDepth(name);
    }

std::string MainListener::ResolveInitializerArgType(
        antlr4::ParserRuleContext* ctx,
        const std::string& functionName,
        int effectiveParamIdx,
        const std::string& namedParam) {
        auto* compiler = Compiler(ctx);
        auto it = compiler->functionTable.find(functionName);
        if (it == compiler->functionTable.end())
        {
            LogErrorContext(ctx, std::format("field initializer: unknown function '{}'",
                SpellFunctionSymbol(*compiler, functionName)));
            return "";
        }

        std::string resolved;
        bool ambiguous = false;
        for (const auto& sym : it->second)
        {
            const LLVMBackend::TypeAndValue* param = nullptr;
            if (!namedParam.empty())
            {
                for (const auto& p : sym.Parameters)
                {
                    if (p.VariableName == namedParam) { param = &p; break; }
                }
            }
            else if (effectiveParamIdx >= 0 && effectiveParamIdx < (int)sym.Parameters.size())
            {
                param = &sym.Parameters[effectiveParamIdx];
            }

            if (param && !param->TypeName.empty() && compiler->IsDataStructure(param->TypeName))
            {
                if (resolved.empty())
                    resolved = param->TypeName;
                else if (resolved != param->TypeName)
                    ambiguous = true;
            }
        }

        if (ambiguous)
        {
            LogErrorContext(ctx, namedParam.empty()
                ? std::format("field initializer: ambiguous struct type at argument position {}", effectiveParamIdx)
                : std::format("field initializer: ambiguous struct type for parameter '{}'", namedParam));
            return "";
        }
        if (resolved.empty())
        {
            LogErrorContext(ctx, namedParam.empty()
                ? std::format("field initializer: cannot infer struct type for argument position {}", effectiveParamIdx)
                : std::format("field initializer: parameter '{}' is not a struct type", namedParam));
            return "";
        }
        return resolved;
    }

llvm::Value* MainListener::CoerceInitValueToInterface(
        LLVMBackend::NamedVariable& rightNV,
        llvm::Value* val,
        const std::string& ifaceName,
        antlr4::ParserRuleContext* errCtx) {
        auto* compiler = Compiler(errCtx);
        if (val == nullptr || ifaceName.empty()) return val;

        if (val->getType() == compiler->GetFatPtrType())
        {
            std::string srcIface = compiler->ResolveFatInterfaceSrcName(val,
                rightNV.TypeAndValue.IsInterface ? rightNV.TypeAndValue.TypeName : std::string());
            return compiler->ReboxInterfaceIfNeeded(val, srcIface, ifaceName);
        }

        std::string srcName = rightNV.TypeAndValue.TypeName;
        // Proven BEFORE the implements early-out: a primitive satisfies it never, so a guard
        // placed after it can only ever see class sources (see RejectPrimitiveShapedInterfaceUpcast).
        if (RejectPrimitiveShapedInterfaceUpcast(errCtx, rightNV.TypeAndValue, ifaceName)) return val;
        if (srcName.empty() || !compiler->StructImplementsInterface(srcName, ifaceName)) return val;
        // A FIELD / array-ELEMENT destination: TransferPointerOwnershipOnStore owns the
        // bookkeeping, so this must not adopt (see BoxConcreteIntoInterface's adoptsOwnership).
        return BoxConcreteIntoInterface(errCtx, val, rightNV.TypeAndValue.Pointer, srcName,
                                        ifaceName, &rightNV, false);
    }

bool MainListener::RejectValueIntoArrayViewField(
        const LLVMBackend::NamedVariable& rightNV,
        const LLVMBackend::TypeAndValue& fieldType,
        const std::string& displayTypeName,
        const std::string& fieldName,
        antlr4::ParserRuleContext* errCtx) {
        auto* compiler = Compiler(errCtx);
        if (!fieldType.IsArrayView) return false;

        const LLVMBackend::TypeAndValue& src = rightNV.TypeAndValue;
        // ELEMENT axis, ahead of the single-object one. A fixed array, a view or a counted
        // `new T[n]` is a real element source only when its elements ARE the field's interface:
        // 'CA[2]' indexes thin objects, so binding it forges a fat view over them. Master reached
        // this same guard inside BoxConcreteIntoInterface; excluding views from the boxing arm
        // took it away, so spell it here with the identical message.
        LLVMBackend::TypeAndValue shaped = src;
        const bool rawHeapArray = compiler->FindRawArrayResult(rightNV.Primary) != nullptr;
        if (rawHeapArray && !shaped.IsArrayView && shaped.ConstArraySize == 0)
            shaped.IsArrayView = true;
        // Elements already OF the field's interface are the ACCEPTED case; only a different
        // element type is rejected. A raw `new IA[n]` result names the interface without
        // carrying IsInterface, so match on the resolved NAME rather than on the flag.
        if (fieldType.IsInterface
            && compiler->ResolveTypeAlias(shaped.TypeName) != compiler->ResolveTypeAlias(fieldType.TypeName)
            && RejectPointerShapedInterfaceUpcast(errCtx, shaped, fieldType.TypeName))
            return true;
        // Element IDENTITY, not element name: no rebox is emitted per view element, so an 'IB[]'
        // bound here would keep the source's vtables. Same predicate and text as the '=' door.
        if (fieldType.IsInterface && RejectArrayViewElementMismatch(errCtx, fieldType, rightNV))
            return true;
        // ELEMENT axis for a plain 'T[]' field: an array, a view or a counted `new T[n]` whose
        // element differs strides and loads with the wrong shape, exactly as at the '=' door.
        if (!fieldType.IsInterface && !src.IsSimd && !src.TypeName.empty()
            && (shaped.IsArrayView || src.ConstArraySize != 0))
        {
            LLVMBackend::NamedVariable elemNV = rightNV;
            elemNV.TypeAndValue = shaped;
            elemNV.TypeAndValue.IsArrayView = true;
            if (RejectArrayViewElementMismatch(errCtx, fieldType, elemNV))
                return true;
        }
        // Primitive axis for an UNNAMED source, ahead of the unnamed early-out below: the null
        // spellings still bind, a bare non-zero literal is a number the field's elements index.
        if (src.TypeName.empty() && !src.IsArrayView && src.ConstArraySize == 0 && !src.IsSimd
            && !rawHeapArray && RejectPrimitiveValueIntoArrayView(errCtx, fieldType, rightNV))
            return true;
        // A view, a fixed array and a counted `new T[n]` are real element sources; a null or
        // otherwise unnamed source names no type at all and is a null view. None is a single object.
        if (src.IsArrayView || src.ConstArraySize != 0 || src.TypeName.empty() || src.IsSimd)
            return false;
        if (rawHeapArray) return false;
        // Primitive axis of the same door, both field kinds: '{ v = 0 }' is a legal null view,
        // any other primitive value is a number the field's elements would be indexed through.
        if (RejectPrimitiveValueIntoArrayView(errCtx, fieldType, rightNV)) return true;
        // Prove it before rejecting: an INTERFACE field takes only a source that IS the interface
        // or implements it, and a plain 'T[]' field only a class/struct of its own element type -
        // a primitive source keeps its own rules ('{ p = 0 }' is a legal null view).
        if (fieldType.IsInterface)
        {
            if (!src.IsInterface
                && !compiler->StructImplementsInterface(src.TypeName, fieldType.TypeName))
                return false;
        }
        else
        {
            const std::string srcType = compiler->ResolveTypeAlias(src.TypeName);
            if (LLVMBackend::IsPrimitiveTypeName(srcType) || src.IsFunctionPointer) return false;
            // A DIFFERENT struct or interface value is one object of the wrong shape - proven
            // wrong twice over - so it takes the same message, which already names both types.
            if (srcType != compiler->ResolveTypeAlias(fieldType.TypeName)
                && !src.IsInterface && !compiler->IsDataStructure(srcType))
                return false;
        }

        compiler->LogErrorMessage(
            "cannot brace-initialize array-view field '{}.{}' ('{}') from '{}' - a view indexes "
            "whole '{}' elements, so its source must be an array view of '{}' (or a null one), "
            "not a single object",
            { displayTypeName, fieldName, SpellType(*compiler, fieldType),
              SpellType(*compiler, src), fieldType.TypeName, fieldType.TypeName });
        return true;
    }

/*
 * Plan converting-constructors.md point 2: a C++ class's own `operator=(U)` for a non-T source
 * beats building `T(u)` - a standard conversion into U outranks the user-defined one into T.
 * Candidates are the declared overloads whose U is not a class (a class U would be a second
 * user-defined conversion) and an `operator=` template instantiated for this source, offered in
 * C++'s order: a non-template taking exactly the source type, else the template instantiation
 * (an exact deduction), else the non-templates needing a conversion. Viability is decided by
 * the REAL overload call run speculatively (errors suppressed, IR into a throwaway function), so
 * the probe and the call cannot disagree; a null result never leaves a diagnostic behind.
 */
MainListener::CxxAssignReturnKind MainListener::GetCxxAssignReturnKind(
        antlr4::ParserRuleContext* ctx,
        const std::string& typeName,
        const LLVMBackend::TypeAndValue& selectedReturn) {
    if (selectedReturn.TypeName == "void") return CxxAssignReturnKind::Void;
    auto* compiler = Compiler(ctx);
    const auto resolvedType = compiler->ResolveTypeAlias(typeName);
    const auto resolvedReturn = compiler->ResolveTypeAlias(selectedReturn.TypeName);
    if (!selectedReturn.Pointer && !selectedReturn.IsAlias
        && resolvedReturn == resolvedType)
        return CxxAssignReturnKind::ByValue;
    if (selectedReturn.IsAlias && !selectedReturn.Pointer
        && !selectedReturn.ElemPointer && resolvedReturn == resolvedType)
        return CxxAssignReturnKind::Reference;
    return CxxAssignReturnKind::Unsupported;
}

llvm::Value* MainListener::CxxAssignOperatorValue(
        antlr4::ParserRuleContext* ctx,
        const LLVMBackend::NamedVariable& namedVar,
        const std::string& typeName,
        llvm::Value* direct,
        const LLVMBackend::TypeAndValue& selectedReturn,
        LLVMBackend::NamedVariable* assignmentResult,
        llvm::AllocaInst* slot) {
    if (assignmentResult == nullptr) return direct;
    auto* compiler = Compiler(ctx);
    const auto returnKind = GetCxxAssignReturnKind(ctx, typeName, selectedReturn);
    if (returnKind == CxxAssignReturnKind::Void || returnKind == CxxAssignReturnKind::Unsupported)
    {
        if (returnKind == CxxAssignReturnKind::Void)
            LogErrorContext(ctx, std::format(
                "operator= of C++ class '{}' returns void, so the assignment has no value",
                compiler->DisplayCxxClassName(typeName)));
        else
            LogErrorContext(ctx, std::format(
                "cannot use the value of C++ operator= for '{}': return type '{}' is unsupported",
                compiler->DisplayCxxClassName(typeName), selectedReturn.TypeName));
        // `direct` is a void call, a scalar or the global probe's placeholder - never load it.
        compiler->lastCxxRetTemp_ = nullptr;
        compiler->lastCxxRetValue_ = nullptr;
        return nullptr;
    }

    if (returnKind == CxxAssignReturnKind::ByValue)
    {
        llvm::Value* resultStorage = compiler->lastCxxRetTemp_;
        bool isOwnedTemp = resultStorage != nullptr;
        if (resultStorage == nullptr && direct != nullptr && direct->getType()->isPointerTy())
            resultStorage = direct;
        if (resultStorage == nullptr) resultStorage = slot;
        if (resultStorage == nullptr)
        {
            LLVMBackend::TypeAndValue classType;
            classType.TypeName = typeName;
            resultStorage = compiler->AllocaAtEntry(compiler->GetType(classType), nullptr,
                                                     "cxx.assign.result");
            isOwnedTemp = true;
        }
        if (direct != resultStorage && !direct->getType()->isPointerTy())
            compiler->builder->CreateStore(direct, resultStorage);
        if (isOwnedTemp && resultStorage != compiler->lastCxxRetTemp_
            && resultStorage != direct && resultStorage != slot)
            compiler->RegisterOwnedStructTemp(resultStorage, typeName);
        LLVMBackend::TypeAndValue classType;
        classType.TypeName = typeName;
        llvm::Value* loaded = compiler->CreateLoad(compiler->GetType(classType), resultStorage);
        *assignmentResult = {};
        assignmentResult->TypeAndValue = namedVar.TypeAndValue;
        assignmentResult->TypeAndValue.VariableName.clear();
        assignmentResult->TypeAndValue.IsMove = false;
        assignmentResult->TypeAndValue.IsAlias = false;
        assignmentResult->Primary = loaded;
        assignmentResult->BaseType = loaded->getType();
        assignmentResult->Storage = resultStorage;
        assignmentResult->IsRvalue = true;
        compiler->lastCxxRetTemp_ = (resultStorage == direct && direct->getType()->isPointerTy())
            ? nullptr : llvm::dyn_cast<llvm::AllocaInst>(resultStorage);
        compiler->lastCxxRetValue_ = loaded;
        return loaded;
    }

    compiler->lastCxxRetTemp_ = nullptr;
    compiler->lastCxxRetValue_ = nullptr;
    llvm::Value* loaded = compiler->CreateLoad(namedVar.BaseType, direct);
    *assignmentResult = {};
    assignmentResult->TypeAndValue = namedVar.TypeAndValue;
    assignmentResult->TypeAndValue.VariableName.clear();
    assignmentResult->TypeAndValue.IsMove = false;
    assignmentResult->TypeAndValue.IsAlias = true;
    assignmentResult->Primary = loaded;
    assignmentResult->BaseType = loaded->getType();
    assignmentResult->Storage = direct;
    assignmentResult->CallerName = namedVar.CallerName;
    assignmentResult->FieldName = namedVar.FieldName;
    assignmentResult->MovePathKey = namedVar.MovePathKey;
    assignmentResult->MovePathHasDynamicIndex = namedVar.MovePathHasDynamicIndex;
    return loaded;
}

llvm::Value* MainListener::TryDirectCxxAssignOperator(
        antlr4::ParserRuleContext* ctx, const LLVMBackend::NamedVariable& destination,
        const std::string& typeName, const LLVMBackend::NamedVariable& rhsNV,
        std::string& refusal, bool probeOnly,
        LLVMBackend::TypeAndValue* selectedReturn) {
        auto* compiler = Compiler(ctx);
        refusal.clear();
        if (destination.Storage == nullptr || destination.BaseType == nullptr) return nullptr;
        const std::string opName = "operator=";
        const std::string resolved = compiler->ResolveTypeAlias(typeName);
        LLVMBackend::NamedVariable source = rhsNV;
        auto* right = LoadNamedVariable(source);
        if (right == nullptr) return nullptr;
        LLVMBackend::NamedVariable probeArg = rhsNV;
        probeArg.Primary = right;
        if (probeArg.BaseType == nullptr) probeArg.BaseType = right->getType();
        // The source's own type; an unnamed literal carries it only in the lowered type.
        LLVMBackend::TypeAndValue sourceType = probeArg.TypeAndValue;
        if (sourceType.TypeName.empty() && !sourceType.Pointer && probeArg.BaseType != nullptr)
            sourceType.TypeName = probeArg.BaseType->isFloatingPointTy()
                ? (probeArg.BaseType->isFloatTy() ? "float" : "double")
                : probeArg.BaseType->isIntegerTy(1) ? "bool"
                : probeArg.BaseType->isIntegerTy(8) ? "char"
                : probeArg.BaseType->isIntegerTy() ? "int" : "";

        auto isThisClass = [&](const LLVMBackend::FunctionSymbol& candidate) {
            return !candidate.Parameters.empty()
                && compiler->ResolveTypeAlias(candidate.Parameters[0].TypeName) == resolved;
        };
        auto classParameter = [&](const LLVMBackend::TypeAndValue& param) {
            const bool byReference = param.Pointer && (param.IsAlias || param.IsRvalueRef)
                && !param.ElemPointer;
            return (!param.Pointer || byReference)
                && (compiler->IsCxxRecord(param.TypeName)
                    || compiler->dataStructures.count(param.TypeName) != 0);
        };
        // U is exactly the source type (a by-value or const-reference U).
        auto exactParameter = [&](const LLVMBackend::TypeAndValue& param) {
            const bool byReference = param.Pointer && (param.IsAlias || param.IsRvalueRef)
                && !param.ElemPointer;
            const bool pointer = byReference ? false : param.Pointer;
            return !sourceType.TypeName.empty()
                && compiler->ResolveTypeAlias(param.TypeName)
                       == compiler->ResolveTypeAlias(sourceType.TypeName)
                && pointer == sourceType.Pointer
                && (byReference ? false : param.ElemPointer) == sourceType.ElemPointer;
        };
        auto arithmetic = [](const LLVMBackend::TypeAndValue& tv) {
            return !tv.Pointer
                && (tv.IsInteger() != -1 || tv.IsFloatingPoint() != -1 || tv.TypeName == "bool");
        };
        // The direct candidates, by kind: declared non-templates, or template instantiations.
        // Copies: offering a subset rebuilds the table, so pointers into it would dangle.
        auto collect = [&](bool templates) {
            std::vector<LLVMBackend::FunctionSymbol> found;
            std::set<std::string> declaredLinkages;
            if (auto record = compiler->cxxRecordEntries_.find(typeName);
                record != compiler->cxxRecordEntries_.end())
                for (const auto& member : record->second.members)
                    if (member.name == opName && !member.isImplicit && !member.isDeleted
                        && member.bindRefusal.empty())
                        declaredLinkages.insert(member.linkageName);
            auto candidates = compiler->functionTable.find(opName);
            if (candidates == compiler->functionTable.end()) return found;
            for (const auto& candidate : candidates->second)
            {
                const bool isTemplate = candidate.UniqueName.starts_with("__cflat_tpl_");
                if (!candidate.IsCxx || !isThisClass(candidate) || candidate.Parameters.size() != 2
                    || isTemplate != templates
                    || (!isTemplate && declaredLinkages.count(candidate.UniqueName) == 0)
                    || compiler->ResolveTypeAlias(candidate.Parameters[1].TypeName) == resolved
                    || (classParameter(candidate.Parameters[1])
                        && !exactParameter(candidate.Parameters[1])))
                    continue;
                found.push_back(candidate);
            }
            return found;
        };

        /*
         * Offer ONLY `offered` for the duration of `body`. The restore re-inserts just the hidden
         * candidates at their original positions, by key: the call may insert into the table or
         * update an entry it binds, and neither change may be rolled back.
         */
        auto withOffered = [&](const std::set<std::string>& offered, auto&& body) {
            std::vector<std::pair<size_t, LLVMBackend::FunctionSymbol>> hidden;
            {
                auto& table = compiler->functionTable[opName];
                std::vector<LLVMBackend::FunctionSymbol> kept;
                for (size_t i = 0; i < table.size(); ++i)
                    if (isThisClass(table[i]) && offered.count(table[i].UniqueName) == 0)
                        hidden.emplace_back(i, table[i]);
                    else
                        kept.push_back(table[i]);
                table = std::move(kept);
            }
            auto restore = [&]() {
                auto& table = compiler->functionTable[opName];
                for (auto& [index, symbol] : hidden)
                    table.insert(table.begin() + std::min(index, table.size()), std::move(symbol));
            };
            try
            {
                auto result = body();
                restore();
                return result;
            }
            catch (...)
            {
                restore();
                throw;
            }
        };
        auto emitCall = [&]() -> llvm::Value* {
            auto* left = compiler->CreateLoad(destination.BaseType, destination.Storage);
            return CxxAssignSretScope::EmitOperatorCall(compiler, [&]() {
                return TryBinaryOperatorOverload(
                    left, "=", right, ctx, destination.BaseType,
                    rhsNV.TypeAndValue.DepthIsAboutThisValue() ? rhsNV.TypeAndValue.PointerDepth : 0,
                    rhsNV.TypeAndValue.ElemPointer, destination.Storage, rhsNV.Storage,
                    /*reportMissing*/ false, /*allowReversed*/ false, /*lhsIsRvalue*/ false,
                    rhsNV.IsRvalue, typeName, rhsNV.TypeAndValue.TypeName,
                    0, false, {}, {}, false, {}, {}, false, {}, {}, false, {}, rhsNV.CxxArithIdentity, rhsNV.IsExplicitMove,
                    false, destination.TypeAndValue.IsCxxNullptrT,
                    rhsNV.TypeAndValue.IsCxxNullptrT, false, false,
                    compiler->CxxOperandObjectCv(destination), compiler->CxxOperandObjectCv(rhsNV));
            });
        };
        /*
         * The real call, run once under suppressed diagnostics into a detached block. Success
         * keeps it: the block is spliced in at the insertion point (or branched to when the call
         * split it). Failure deletes every block it created, so probe and call never diverge.
         */
        auto speculate = [&](const std::set<std::string>& offered) -> llvm::Value* {
            auto savedIP = compiler->builder->saveIP();
            auto savedDebugLoc = compiler->builder->getCurrentDebugLocation();
            auto* savedRetTemp = compiler->lastCxxRetTemp_;
            auto* savedRetValue = compiler->lastCxxRetValue_;
            const auto savedReturnType = compiler->lastCallReturnType;
            const bool savedSuppress = compiler->suppressErrors_;
            auto* origin = savedIP.getBlock();
            if (origin == nullptr || compiler->currentFunction == nullptr) return nullptr;
            auto* scratch = llvm::BasicBlock::Create(*compiler->context, "cxx.assign",
                                                     compiler->currentFunction);
            compiler->builder->SetInsertPoint(scratch);
            compiler->suppressErrors_ = true;
            auto ledgers = compiler->SnapshotDetectionLedgers();
            const auto mark = compiler->MarkOwnedTemps();
            auto createdBlocks = [&]() {
                std::vector<llvm::BasicBlock*> created;
                bool after = false;
                for (auto& block : *compiler->currentFunction)
                {
                    if (&block == scratch) after = true;
                    if (after) created.push_back(&block);
                }
                return created;
            };
            auto discard = [&]() {
                compiler->DiscardOwnedTempsSince(mark);
                compiler->RestoreDetectionLedgers(ledgers);
                compiler->suppressErrors_ = savedSuppress;
                compiler->lastCxxRetTemp_ = savedRetTemp;
                compiler->lastCxxRetValue_ = savedRetValue;
                compiler->lastCallReturnType = savedReturnType;
                compiler->builder->restoreIP(savedIP);
                compiler->builder->SetCurrentDebugLocation(savedDebugLoc);
                const auto created = createdBlocks();
                for (auto* block : created) block->dropAllReferences();
                for (auto* block : created) block->eraseFromParent();
            };
            llvm::Value* result = nullptr;
            try
            {
                result = withOffered(offered, emitCall);
            }
            catch (const SpeculativeEvalAbort&) { result = nullptr; }
            catch (...)
            {
                discard();
                throw;
            }
            if (result == nullptr)
            {
                discard();
                return nullptr;
            }
            const LLVMBackend::TypeAndValue selected = compiler->lastCallReturnType;
            if (selectedReturn != nullptr) *selectedReturn = selected;
            if (probeOnly)
            {
                discard();
                return llvm::UndefValue::get(compiler->builder->getInt1Ty());
            }
            compiler->suppressErrors_ = savedSuppress;
            const auto created = createdBlocks();
            // getTerminator() asserts on an open block; these ask whether one is still open.
            if (created.size() == 1 && cflat_llvm::GetTerminatorOrNull(scratch) == nullptr)
            {
                // One straight-line block: move its instructions to the insertion point.
                origin->splice(savedIP.getPoint(), scratch);
                // Temps the call registered (a by-value result) were keyed to `scratch`.
                compiler->RekeyOwnedTempsSince(mark, scratch, origin);
                scratch->eraseFromParent();
                compiler->builder->restoreIP(savedIP);
                return result;
            }
            if (savedIP.getPoint() == origin->end()
                && cflat_llvm::GetTerminatorOrNull(origin) == nullptr)
            {
                // The call split its block: enter it from the insertion point, stay at its end.
                llvm::BranchInst::Create(scratch, origin);
                return result;
            }
            discard();
            return withOffered(offered, emitCall);
        };
        auto names = [](const std::vector<LLVMBackend::FunctionSymbol>& symbols) {
            std::set<std::string> out;
            for (const auto& symbol : symbols) out.insert(symbol.UniqueName);
            return out;
        };

        const auto nonTemplates = collect(/*templates*/ false);
        std::vector<LLVMBackend::FunctionSymbol> exact;
        for (const auto& candidate : nonTemplates)
            if (exactParameter(candidate.Parameters[1])) exact.push_back(candidate);

        if (!exact.empty())
            if (auto* result = speculate(names(exact))) return result;
        bool templateInstantiated = false;
        if (compiler->IsCxxRecord(typeName)
            && compiler->HasCxxFunctionTemplateMember(typeName, opName))
        {
            LLVMBackend::NamedVariable receiver;
            receiver.Primary = destination.Storage;
            receiver.Storage = destination.Storage;
            receiver.BaseType = destination.Storage->getType();
            receiver.TypeAndValue.TypeName = typeName;
            receiver.TypeAndValue.Pointer = true;
            std::vector<LLVMBackend::NamedVariable> templateArguments = { receiver, probeArg };
            templateArguments[1].TypeAndValue.VariableName.clear();
            std::string registeredName, templateError;
            if (compiler->RequestCxxFunctionTemplate(opName, typeName, {}, templateArguments, {},
                                                     registeredName, templateError))
            {
                const auto instantiated = names(collect(/*templates*/ true));
                templateInstantiated = !instantiated.empty();
                if (templateInstantiated)
                    if (auto* result = speculate(instantiated)) return result;
            }
        }
        if (!nonTemplates.empty())
            if (auto* result = speculate(names(nonTemplates))) return result;

        // C++ would call an arithmetic `operator=(U)` for an arithmetic source (a standard
        // conversion) that CFlat's call rules refuse: say so rather than reroute through T(u).
        if (!templateInstantiated && arithmetic(sourceType))
            for (const auto& candidate : nonTemplates)
            {
                const auto& p = candidate.Parameters[1];
                if (!arithmetic(p)) continue;
                // A mutable reference takes no converted source in C++ either: not a refusal.
                const std::string spelling = compiler->CxxReferenceParameterSpelling(candidate, 1);
                if (p.IsRvalueRef || p.IsCxxRefToPointer
                    || (p.IsAlias && !p.CxxConstRef() && !spelling.starts_with("const ")))
                    continue;
                const std::string to = spelling.empty() ? SpellType(*compiler, p) : spelling;
                refusal = std::format(
                    "C++ resolves this assignment to '{}.operator=({})', which needs a "
                    "conversion CFlat does not make implicitly ('{}' to '{}'); cast the source to "
                    "'{}' to call it, or spell the conversion out as '{}(<source>)'",
                    compiler->DisplayCxxClassName(typeName), to, sourceType.TypeName, to, to,
                    compiler->DisplayCxxClassName(typeName));
                break;
            }
        return nullptr;
}

bool MainListener::EmitForeignCxxValueIntoSlot(
        const LLVMBackend::TypeAndValue& destType,
        llvm::Value* destination,
        LLVMBackend::NamedVariable& sourceNV,
        llvm::Value* sourceValue,
        const LLVMBackend::OwnedTempMark& ownedTempMark,
        const char* context,
        antlr4::ParserRuleContext* errCtx,
        bool intoFreshSlot,
        bool replaceExistingAfterConversion) {
        auto* compiler = Compiler(errCtx);
        if (destination == nullptr || sourceValue == nullptr
            || destType.Pointer || destType.ElemPointer || destType.IsArrayView)
            return false;
        const bool foreignNontrivial = compiler->IsForeignNontrivialCxxClass(destType.TypeName);
        const auto* classInfo = foreignNontrivial ? nullptr
            : compiler->GetCxxClassInfo(destType.TypeName);
        const bool trivialCtorTemplateCopy = !foreignNontrivial
            && compiler->IsCxxTriviallyCopyableRecord(destType.TypeName)
            && classInfo != nullptr && classInfo->hasCtorTemplate;
        if (!foreignNontrivial && !trivialCtorTemplateCopy) return false;

        auto* retTemp = compiler->lastCxxRetTemp_;
        const bool outermostTemp = retTemp != nullptr
            && ((sourceNV.Primary != nullptr && sourceNV.Primary == compiler->lastCxxRetValue_)
                || sourceNV.Storage == retTemp);
        const bool sameClass = !sourceNV.TypeAndValue.Pointer
            && sourceNV.TypeAndValue.TypeName == destType.TypeName;
        if (outermostTemp)
        {
            compiler->EmitCxxCopyOrMoveConstruct(destType.TypeName, destination, retTemp,
                                                 /*useMove*/ true, context);
            // The returned temporary was selected by this store. Its moved-from C++ object still
            // owes a destructor, so flush only the temporaries created by this RHS.
            compiler->FlushOwnedTempsSince(ownedTempMark, nullptr, nullptr);
            compiler->lastCxxRetTemp_ = nullptr;
            compiler->lastCxxRetValue_ = nullptr;
            return true;
        }
        // A C++ value ternary joins the selected sret slots with a storage PHI. The branch
        // temporaries are still individually tracked, so move from the selected slot and flush
        // those temporaries after the destination has taken the value.
        auto* valuePhi = llvm::dyn_cast<llvm::PHINode>(sourceValue);
        auto* storagePhi = llvm::dyn_cast_or_null<llvm::PHINode>(sourceNV.Storage);
        bool ownedCxxTempJoin = sameClass && valuePhi != nullptr && storagePhi != nullptr
            && valuePhi->getNumIncomingValues() == storagePhi->getNumIncomingValues();
        for (unsigned i = 0; ownedCxxTempJoin && i < valuePhi->getNumIncomingValues(); ++i)
        {
            LLVMBackend::NamedVariable arm;
            arm.Primary = valuePhi->getIncomingValue(i);
            arm.Storage = storagePhi->getIncomingValue(i);
            arm.TypeAndValue.TypeName = destType.TypeName;
            ownedCxxTempJoin = compiler->IsOwnedTempValue(arm);
        }
        if (ownedCxxTempJoin)
        {
            compiler->EmitCxxCopyOrMoveConstruct(destType.TypeName, destination,
                                                 sourceNV.Storage,
                                                 /*useMove*/ true, context);
            compiler->FlushOwnedTempsSince(ownedTempMark, nullptr, nullptr);
            compiler->lastCxxRetTemp_ = nullptr;
            compiler->lastCxxRetValue_ = nullptr;
            return true;
        }
        if (retTemp == nullptr && sameClass && sourceNV.Storage != nullptr)
        {
            compiler->EmitCxxCopyOrMoveConstruct(destType.TypeName, destination,
                                                 sourceNV.Storage,
                                                 sourceNV.IsRvalue || sourceNV.CxxParamLastUse,
                                                 context);
            if (sourceNV.IsElementAccess && sourceNV.IsRvalue)
                DestroyForeignCxxRelocationSource(sourceNV);
            // A moved-from C++ object remains alive and still needs its destructor. A CFlat move
            // parameter aliases caller storage, so only its last-use cleanup belongs to the caller.
            if (sourceNV.CxxParamLastUse && !sourceNV.IsElementAccess)
            {
                const std::string sourceName = sourceNV.CallerName.empty()
                    ? sourceNV.TypeAndValue.VariableName
                    : sourceNV.CallerName;
                if (!sourceName.empty())
                    compiler->MarkVariableMoved(sourceName);
            }
            return true;
        }
        if (retTemp == nullptr && sameClass && sourceValue->getType() == compiler->GetType(destType))
        {
            compiler->builder->CreateStore(sourceValue, destination);
            return true;
        }
        // R5: a raw T* into a `unique T*` slot on a C++ class (std::unique_ptr<T>) adopts it.
        {
            auto adoptSource = sourceNV;
            adoptSource.Primary = sourceValue;
            if (compiler->IsRawPointerForCxxUniquePtr(destType.TypeName, adoptSource)
                && compiler->TryAdoptRawPointerIntoCxxUniquePtr(destType.TypeName, destination,
                                                                adoptSource, context, &destType))
                return true;
        }
        // Plan converting-constructors.md point 1: a brace element / slot initializer `u` is
        // `T(u)`.
        if (!sameClass)
        {
            std::string refusal;
            LLVMBackend::NamedVariable converted = sourceNV;
            if (converted.Primary == nullptr) converted.Primary = sourceValue;
            // Copy-init (declaration, brace element, field): a fresh slot takes the converting ctor
            // in place like C++; otherwise build the owned `T(u)` temporary and move it in.
            switch (compiler->ConvertThroughCxxConvertingCtor(converted, destType.TypeName,
                                                              intoFreshSlot ? destination : nullptr,
                                                              refusal,
                                                              /*directInit*/ false,
                                                              /*allowScalarNarrowing*/ true))
            {
            case LLVMBackend::CxxConvertingCtorResult::Converted:
                if (!intoFreshSlot)
                {
                    if (replaceExistingAfterConversion)
                        if (auto* dtor = compiler->GetOrCreateFullDestructor(destType.TypeName))
                            compiler->builder->CreateCall(dtor->getFunctionType(), dtor,
                                                          { destination });
                    compiler->EmitCxxCopyOrMoveConstruct(destType.TypeName, destination,
                                                         converted.Storage, /*useMove*/ true,
                                                         context);
                }
                compiler->FlushOwnedTempsSince(ownedTempMark, nullptr, nullptr);
                return true;
            case LLVMBackend::CxxConvertingCtorResult::Refused:
                LogErrorContext(errCtx, std::format(
                    "cannot initialize C++ class '{}' from this expression; {}",
                    compiler->DisplayCxxClassName(destType.TypeName), refusal));
                return true;
            default:
                break;
            }
        }

        LogErrorContext(errCtx, std::format(
            "cannot initialize C++ class '{}' from this expression; use '{}(args)', a '{}' lvalue, "
            "or 'move <{}> lvalue'",
            compiler->DisplayCxxClassName(destType.TypeName),
            compiler->DisplayCxxClassName(destType.TypeName),
            compiler->DisplayCxxClassName(destType.TypeName),
            compiler->DisplayCxxClassName(destType.TypeName)));
        return true;
}

void MainListener::DestroyForeignCxxRelocationSource(
        const LLVMBackend::NamedVariable& sourceNV) {
    auto* compiler = Compiler();
    const bool dereferencedStorage = sourceNV.Storage != nullptr
        && llvm::isa<llvm::LoadInst>(sourceNV.Storage);
    if ((!sourceNV.IsElementAccess && !sourceNV.FieldPathThroughPointer
            && !dereferencedStorage)
        || !sourceNV.IsRvalue || sourceNV.Storage == nullptr
        || sourceNV.TypeAndValue.Pointer
        || !compiler->IsForeignNontrivialCxxClass(sourceNV.TypeAndValue.TypeName))
        return;
    if (auto* dtor = compiler->GetFullDestructorForDelete(sourceNV.TypeAndValue.TypeName))
        compiler->builder->CreateCall(dtor->getFunctionType(), dtor, { sourceNV.Storage });
}

bool MainListener::EmitForeignCxxFixedArrayIntoSlot(
        const LLVMBackend::DeclTypeAndValue& field,
        llvm::Value* destination,
        llvm::Value* sourceSlot,
        const char* context,
        antlr4::ParserRuleContext* errCtx) {
        auto* compiler = Compiler(errCtx);
        if (destination == nullptr || field.Pointer
            || field.ElemPointer || field.IsArrayView || field.ConstArraySize == 0
            || !compiler->IsForeignNontrivialCxxClass(field.TypeName))
            return false;

        auto* arrTy = llvm::dyn_cast<llvm::ArrayType>(compiler->GetType(field));
        auto* dtor = compiler->GetOrCreateFullDestructor(field.TypeName);
        if (arrTy == nullptr || dtor == nullptr) return false;

        uint64_t count = field.ConstArraySize;
        for (uint64_t dim : field.ConstInnerDimensions) count *= dim;
        llvm::Value* zero = compiler->builder->getInt32(0);
        std::vector<uint64_t> dimensions;
        dimensions.reserve(field.ConstInnerDimensions.size() + 1);
        dimensions.push_back(field.ConstArraySize);
        dimensions.insert(dimensions.end(), field.ConstInnerDimensions.begin(),
                          field.ConstInnerDimensions.end());

        for (uint64_t flatIndex = 0; flatIndex < count; ++flatIndex)
        {
            uint64_t remaining = flatIndex;
            std::vector<llvm::Value*> indices{ zero };
            for (size_t dim = 0; dim < dimensions.size(); ++dim)
            {
                uint64_t stride = 1;
                for (size_t trailing = dim + 1; trailing < dimensions.size(); ++trailing)
                    stride *= dimensions[trailing];
                uint64_t index = remaining / stride;
                remaining %= stride;
                indices.push_back(compiler->builder->getInt32((uint32_t)index));
            }

            auto* destinationElem = compiler->builder->CreateInBoundsGEP(
                arrTy, destination, indices, "fieldarrelem");
            if (sourceSlot == nullptr)
            {
                compiler->builder->CreateCall(dtor->getFunctionType(), dtor, { destinationElem });
                continue;
            }
            auto* sourceElem = compiler->builder->CreateInBoundsGEP(
                arrTy, sourceSlot, indices, "fieldsrcarrelem");
            compiler->builder->CreateCall(dtor->getFunctionType(), dtor, { destinationElem });
            compiler->EmitCxxCopyOrMoveConstruct(field.TypeName, destinationElem, sourceElem,
                                                 /*useMove*/ true, context);
            compiler->builder->CreateCall(dtor->getFunctionType(), dtor, { sourceElem });
        }
        return true;
}

bool MainListener::EmitOneFieldInit(
        llvm::Value* structPtr,
        const LLVMBackend::StructData& sd,
        const std::string& typeName,
        const std::string& fieldName,
        LLVMBackend::NamedVariable& rightNV,
        const LLVMBackend::OwnedTempMark& ownedTempMark,
        antlr4::ParserRuleContext* errCtx,
        bool destinationIsFresh,
        bool deferForeignReplacementDrop) {
        auto* compiler = Compiler(errCtx);
        const std::string displayTypeName = SpellType(*compiler,
            LLVMBackend::TypeAndValue{ .TypeName = typeName });

        int fieldIdx = -1;
        LLVMBackend::DeclTypeAndValue fieldType;
        for (int i = 0; i < (int)sd.StructFields.size(); i++)
        {
            if (sd.StructFields[i].VariableName == fieldName)
            {
                fieldIdx = i;
                fieldType = sd.StructFields[i];
                break;
            }
        }
        if (fieldIdx < 0)
        {
            LogErrorContext(errCtx, std::format("'{}' has no field named '{}'",
                SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = typeName }), fieldName));
            return false;
        }
        compiler->RejectImplicitIntegerPointeePointerConversion(rightNV.TypeAndValue, fieldType);
        if ((fieldType.TypeName == "string" || fieldType.TypeName == "string_view")
            && !fieldType.Pointer
            && RejectStackCharBufferEscape(rightNV, errCtx))
            return false;
        const bool isForeignCxxField = !fieldType.Pointer && !fieldType.ElemPointer
            && !fieldType.IsArrayView && fieldType.ConstArraySize == 0
            && compiler->IsForeignNontrivialCxxClass(fieldType.TypeName);

        // Ahead of every store rule: a single object bound to a VIEW field is not one element of
        // it, and the store below would leave the slot pointing at something that is not an array.
        if (RejectValueIntoArrayViewField(rightNV, fieldType, displayTypeName, fieldName, errCtx))
            return false;
        // A const C++ object's address into a mutable record pointer field (clang refuses).
        RejectConstRecordPointeeStore(errCtx, fieldType, rightNV, "field", fieldName);

        RejectRawHeapArrayIntoUniqueField(rightNV, fieldType, fieldName, errCtx);

        // Brace leg of the code-value store gate. This is a SEPARATE lowering path from the `=`
        // assignment site, so `h.p = w` being rejected says nothing about `Holder h = { p = w }`.
        RejectCodeValueIntoDataSlot(errCtx, rightNV, fieldType, "brace-initialize",
                                    std::format("field '{}.{}' of", displayTypeName, fieldName));

        // Brace initialization is a separate field-store path from `field = value`.
        // An alias string from a temporary must not escape through either spelling.
        // A string taken from a TEMPORARY binds by IMPLICIT COPY here as at the `=` twin (the copy
        // itself needs the loaded value, so it is emitted just after LoadNamedVariable below); only
        // a NAMED `alias` string local - a borrow the user spelled - stays rejected. The
        // FromOwningTempField leg is the brace sibling of the `=` temp-FIELD store, which used to
        // slip through here entirely and store a borrowed view into the fresh field.
        bool braceStringTempCopy = false;
        if (fieldType.TypeName == "string" && !fieldType.Pointer && !fieldType.IsAlias
            && !rightNV.TypeAndValue.IsMove
            && !rightNV.MovableTempField
            && (rightNV.TypeAndValue.IsAlias
                || (rightNV.FromOwningTempField && rightNV.TypeAndValue.TypeName == "string")))
        {
            const bool namedAliasLocal = rightNV.Storage != nullptr
                && !rightNV.IsElementAccess && !rightNV.CallerName.empty()
                && !rightNV.FromOwningTempField;
            if (namedAliasLocal)
                // Same template as the `=` twin, so the two spellings share one localized message.
                LogErrorContext(errCtx, std::format(
                    "cannot store {} into a longer-lived location; "
                    "its buffer is owned elsewhere and would be freed out from under the field. "
                    "Use '.copy()' for an independent copy.", "an 'alias' string local"));
            else
                braceStringTempCopy = true;
        }

        // Brace leg of the `=` path's owning-temp-FIELD rules (MainListener_Expressions ~2557).
        // An owning VALUE field read off a temporary has its buffer owned by that temporary, so a
        // second owner double-frees (a MOVE temp transfers instead - handled just below; a
        // `string` binds by implicit copy - handled just above).
        if (!braceStringTempCopy && rightNV.FromOwningTempField && !rightNV.MovableTempField
            && !rightNV.TypeAndValue.Pointer && !rightNV.TypeAndValue.IsMove
            && rightNV.TypeAndValue.TypeName != "string"
            && compiler->IsOwningValueType(rightNV.TypeAndValue.TypeName)
            && compiler->IsOwningValueType(fieldType.TypeName))
        {
            // Same wording as the `=` twin, so the two spellings share one localized message.
            LogErrorContext(errCtx, std::format(
                "cannot store '{}.{}' taken from a temporary into a longer-lived location; its buffer "
                "is owned elsewhere and would double-free. Use '.copy()' for an independent copy, or "
                "bind the whole call result to a local first and assign from that.",
                SpellType(*compiler, LLVMBackend::TypeAndValue{
                    .TypeName = rightNV.OwningStructName }), rightNV.FieldName));
            return false;
        }

        // Brace leg of the `=` path's implied move out of a MOVE temp's owning field. The temp's
        // own destructor still runs at end of statement, so the source field must be zeroed or the
        // one buffer is freed twice. Only the source is nulled - brace-init builds a FRESH slot.
        bool braceMoveTempField = rightNV.MovableTempField
            && !rightNV.TypeAndValue.Pointer && !rightNV.TypeAndValue.IsMove
            && rightNV.MoveTempStructType != nullptr && rightNV.MoveTempStructAlloca != nullptr
            && rightNV.TypeAndValue.TypeName != "string"
            && compiler->IsOwningValueType(rightNV.TypeAndValue.TypeName)
            && compiler->IsOwningValueType(fieldType.TypeName);

        // `unique` field initialized here: this is a second field-store path, so it needs the same
        // two source rejections the `=` path applies (Trap A, and field-to-field). No reassign-free
        // is needed - both callers construct a fresh slot, so there is no old pointee to release.
        // The two borrow legs use the same owning-slot predicate as the `=` path, so a field made
        // owning by generic substitution (`Box<unique Item*>::t`) is seen by both store paths.
        if (IsOwningUniqueField(fieldType))
        {
            std::string fieldDesc = std::format("unique field '{}.{}'", displayTypeName, fieldName);
            if (rightNV.IsBorrowed && !rightNV.TypeAndValue.IsMove)
                RejectBorrowIntoUniqueField(rightNV, fieldDesc, errCtx);
            // A '?:' join carries no IsBorrowed, so the ledger is the only surviving provenance;
            // same reject as the `=` path, keeping the two field-store paths in lockstep.
            std::string braceBorrowMoveOrigin;
            if (!rightNV.TypeAndValue.IsMove
                && compiler->IsMovedBorrowedPtrValue(rightNV.Primary, &braceBorrowMoveOrigin))
            {
                LLVMBackend::NamedVariable borrowSrc = {};
                borrowSrc.CallerName     = braceBorrowMoveOrigin;
                borrowSrc.BorrowedOrigin = braceBorrowMoveOrigin;
                RejectBorrowIntoUniqueField(borrowSrc, fieldDesc, errCtx);
            }
        }
        // Source and destination gates kept in lockstep with the `=` path: a generic-substituted
        // NAMED-LOCAL source, a source read off an owning temporary, and a fat-interface slot.
        bool braceSrcIsUniqueFieldRead = IsUniqueFieldRead(rightNV) || IsUniqueTempFieldRead(rightNV);
        // `fieldType.Pointer` stands in for the `=` path's isPointerTy() test on the stored value:
        // IsOwningUniquePointerField answers TRUE for any written `unique`, fat interface included.
        bool braceDestOwnsPointee = IsOwningUniquePointerField(fieldType) && fieldType.Pointer;
        bool braceDestOwnsValue = IsOwningUniqueField(fieldType)
            && !fieldType.Pointer && !fieldType.IsInterface;
        // A TEMPORARY's `unique` field has no addressable slot to null, so it stays a reject here
        // too; a real field source takes the implicit move applied just after the load below.
        if ((braceDestOwnsPointee || braceDestOwnsValue) && IsUniqueTempFieldRead(rightNV))
            RejectUniqueTempFieldToField(
                rightNV, std::format("unique field '{}.{}'", displayTypeName, fieldName), errCtx);
        // Belt-and-braces: the source gates above all throw, so this leg runs only for the
        // cast/join spellings that reach an owning destination with every declared fact stripped.
        bool braceSrcGateFired = braceSrcIsUniqueFieldRead
            && (braceDestOwnsPointee || braceDestOwnsValue
                || IsOwningUniqueInterfaceField(fieldType));
        if (!braceSrcGateFired)
        {
            bool braceDestOwns = braceDestOwnsPointee || braceDestOwnsValue
                || IsOwningUniqueInterfaceField(fieldType);
            // A brace temporary passed as a call argument dies with the statement, like its field.
            GuardOwningTempUniqueFieldEscape(
                rightNV, std::format("{}field '{}.{}'", braceDestOwns ? "unique " : "",
                                     displayTypeName, fieldName), errCtx,
                /*storeSite*/ !inCallArgument_);
        }

        if (rightNV.IsBonded || rightNV.ContainsBondedClosure || compiler->lastCallIsBonded)
        {
            const auto& bondedSources = rightNV.IsBonded
                ? rightNV.BondedSources : compiler->lastCallBondedSources;
            // The holder is the BRACE DESTINATION (recovered from structPtr), never the
            // source's own field path - passing rightNV re-marks the source holder instead.
            LLVMBackend::NamedVariable braceDestination{};
            if (rightNV.ContainsBondedClosure
                || !AllowBondedClosureFieldStore(compiler, braceDestination, structPtr,
                                                  bondedSources, errCtx))
                LogErrorContext(errCtx,
                    "bonded value cannot be stored in a struct field or through a pointer - bond lifetime would be untrackable");
        }

        llvm::Value* val = LoadNamedVariable(rightNV);
        if (!val) return false;

        // R5: `{ f = new T() }` into a `unique T*` field on a C++ class (std::unique_ptr<T>).
        if (!fieldType.Pointer && val->getType()->isPointerTy())
        {
            auto adoptSource = rightNV;
            adoptSource.Primary = val;
            if (auto* adopted = compiler->AdoptRawPointerAsCxxUniquePtrValue(
                    fieldType.TypeName, adoptSource,
                    std::format("field '{}.{}'", displayTypeName, fieldName)))
            {
                val = adopted;
                // The slot store below now sees the adopted class value, not the raw pointer.
                rightNV.TypeAndValue = LLVMBackend::TypeAndValue{ .TypeName = fieldType.TypeName };
                rightNV.Storage = nullptr;
                rightNV.Primary = adopted;
            }
        }

        if (!fieldType.Pointer && compiler->IsCoreUniqueType(fieldType.TypeName)
            && RejectFieldAllocAlignMismatch(
                fieldType, fieldType.AllocAlignValue, rightNV, val,
                std::format("field '{}.{}'", displayTypeName, fieldName), errCtx))
            return false;

        if ((val->getType()->isPointerTy()
                || (val->getType() == compiler->GetFatPtrType()
                    && compiler->IsCoreUniqueType(fieldType.TypeName)
                    && compiler->IsInterfaceType(MangledGenericArgument(
                        *compiler, fieldType.TypeName))))
            && !fieldType.Pointer
            && compiler->IsCoreUniqueType(fieldType.TypeName))
        {
            auto ctorArg = rightNV;
            ctorArg.Primary = val;
            ctorArg.BaseType = val->getType();
            val = compiler->AdjustCxxPointerForUniqueAdoption(
                ctorArg, fieldType, val,
                std::format("field '{}.{}'", displayTypeName, fieldName));
            ctorArg.Primary = val;
            ctorArg.BaseType = val->getType();
            ShapeCoreUniqueCtorArg(compiler, ctorArg, fieldType.TypeName, val);
            ctorArg.IsOwning = ctorArg.IsOwning || compiler->lastOwningResult
                || compiler->lastCallReturnsOwned || compiler->IsOwnedNewTemp(val)
                || compiler->IsOwningPtrTempValue(val) || compiler->IsMovedOutPtrValue(val);
            val = compiler->CreateCoreUniqueFromRawPointerCall(ctorArg, fieldType);
            compiler->ConsumeOwnedNewTemp(rightNV.Primary);
            compiler->lastOwningResult = false;
            compiler->lastCallReturnsOwned = false;
        }

        if (RejectImplicitPrimitiveToPointer(
                errCtx, fieldType, rightNV, val, "brace-initialize",
                std::format("field '{}.{}'", displayTypeName, fieldName)))
            return false;
        if (RejectImplicitPointerToNumber(
                errCtx, fieldType, rightNV, val, "brace-initialize",
                std::format("field '{}.{}'", displayTypeName, fieldName)))
            return false;
        if (RejectConstDiscardingConversion(errCtx, rightNV, val, fieldType))
            return false;

        // `{ f = cppRefCall() }` binds the referent and `{ f = derivedPtr }` shifts to the base
        // subobject: the brace twin of the declaration-initializer and `=` legs.
        {
            const std::string braceDest = std::format("field '{}.{}'", displayTypeName, fieldName);
            if (val != nullptr && val->getType()->isStructTy())
                if (auto* refAddr = compiler->CxxReferenceResultAsPointer(
                        fieldType, rightNV, braceDest))
                    val = refAddr;
            if (val != nullptr)
                val = compiler->AdjustCxxPointerForStore(
                    fieldType, rightNV.TypeAndValue, val, braceDest);
        }

        // A string LITERAL is a 'const char*', never a 'T*' - the brace leg of the same gate the
        // declarator and `=` paths apply, so no spelling can seed a struct pointer with characters.
        if (RejectStringLiteralIntoStructPointer(
                errCtx, fieldType, val, std::format("field '{}.{}'", displayTypeName, fieldName)))
            return false;

        // The implicit copy decided above. Emitted here because the deep copy needs the LOADED
        // {ptr,len,owned} aggregate; it also retires rightNV's borrow facts, so the alias/owning
        // rejects further down see an ordinary owned string temp.
        if (braceStringTempCopy)
            AdoptImplicitStringTempCopy(rightNV, val, errCtx);

        // The implied move decided above; emitted here because zeroing needs the loaded value's
        // type, and it must land after the read so the copy still sees the live buffer.
        if (braceMoveTempField && val->getType()->isStructTy())
            compiler->ClearMovableTempField(rightNV.MoveTempStructType,
                rightNV.MoveTempStructAlloca, rightNV.MoveTempFieldIndex, val->getType(),
                rightNV.TernaryTempAlreadyRegistered);

        // The brace-init leg of the 2026-08-10 implicit-move ruling, kept in lockstep with the `=`
        // path. The destination slot is freshly constructed here, so only the source is nulled.
        if (braceSrcIsUniqueFieldRead && !IsUniqueTempFieldRead(rightNV)
            && (braceDestOwnsPointee || braceDestOwnsValue
                || IsOwningUniqueInterfaceField(fieldType)))
        {
            // Same borrowed-by-value-parameter reject the `=` path applies: nulling the callee's
            // copy leaves the caller's struct still freeing the pointee (double free).
            if (RejectConsumeOfBorrowedByValueParamField(compiler, rightNV, errCtx))
                return false;
            if (RejectJoinedSourceIntoUniqueField(compiler, rightNV, errCtx))
                return false;
            EmitImplicitUniqueFieldMove(rightNV, val, nullptr, std::string(), errCtx);
        }

        // Same provable stack-address reject the `=` path applies, with the same scalar-pointer
        // shape gate; `&local` carries no borrow provenance for the legs above to see.
        if (IsOwningUniqueField(fieldType)
            && ((fieldType.Pointer && fieldType.ConstArraySize == 0 && !fieldType.IsInterface)
                || braceDestOwnsValue)
            && LLVMBackend::IsProvableNonHeapAddress(val))
            RejectNonHeapAddressIntoUnique(
                std::format("unique field '{}.{}'", displayTypeName, fieldName), errCtx);

        // Same allocation-alignment agreement the `=` path demands: this store reaches the very
        // same synthesized free site, so a mismatch here corrupts the heap identically.
        RejectFieldAllocAlignMismatch(
            fieldType, fieldType.AllocAlignValue, rightNV, val,
            std::format("field '{}.{}'", displayTypeName, fieldName), errCtx);

        /*
         * The `=` path's three remaining ownership rules for a field store, applied here in its
         * order. A closure is clone-safe and takes the auto-clone; it must NOT reach the rejects
         * (the `=` path returns early instead, and the owning-value reject would otherwise fire on
         * a `__closure_fat_ptr`, which has a full destructor). No reassign-free accompanies the
         * clone: both callers construct a FRESH slot, so there is no old env to release.
         */
        bool isClosureVal = (val->getType() == compiler->GetClosureFatPtrType());
        if (isClosureVal)
            val = CloneClosureFromNamedSource(rightNV, val, errCtx);
        else
        {
            if (RejectNonOwningStructJoinStore(val, fieldType.TypeName, "a struct field", errCtx))
                return false;
            if (RejectAliasStoreIntoField(rightNV, val, errCtx)) return false;
            // Brace-init always targets a FRESH slot, so a self-assign is impossible here. There is no
            // second copy path in brace-init, so the copied flag is unused.
            bool braceCopied = false;
            if (!isForeignCxxField
                && RejectOwningValueCopyIntoField(rightNV, val, false, braceCopied, errCtx))
                return false;
        }

        // Coerce char* string literals to the string struct type
        if (fieldType.TypeName == "string" && !fieldType.Pointer
            && val->getType() == cflat_llvm::PointerTo(compiler->builder->getInt8Ty()))
        {
            auto* c = llvm::dyn_cast<llvm::Constant>(val);
            if (c && compiler->IsStringLiteralConstant(c))
                val = compiler->WrapStringLiteralAsString(val);
            else if (compiler->GetFunction("operator string"))
            {
                LLVMBackend::NamedVariable argNV;
                argNV.Primary = val;
                argNV.BaseType = val->getType();
                val = compiler->CreateOverloadedFunctionCall("operator string", { argNV });
            }
        }
        // A string-STRUCT source stored into an owning string field: deep-copy so the
        // field owns an INDEPENDENT buffer. A plain store would alias the source's
        // buffer, which BOTH the field destructor AND the source's owner would free
        // (double-free). Mirrors the param-to-field deep-copy the factory constructors
        // rely on (e.g. text(string s) { t.text = s; }). copy() leaves the source
        // intact; the copy result is owned by the field, so it is dropped from the
        // owned-string temp-flush list below.
        // A string source that ALREADY hands us sole ownership of its buffer is transferred, not
        // copied - exactly as the `=` path does. Two shapes qualify: a `move`d source (Storage is
        // nulled below via the shared helper) and a transferred owned temp (Storage == nullptr with
        // the owning flag - a `move`d local whose storage ParseMoveExpression already zeroed, or an
        // operator+/copy() result). Deep-copying either would orphan the one buffer (a leak): the
        // moved-from source is already suppressed, so nothing else frees the original. A plain owned
        // string LOCAL (Storage != nullptr, not a move) still deep-copies - it keeps its buffer, so
        // the field needs an independent one (the known-benign divergence from the `=` path).
        else if (fieldType.TypeName == "string" && !fieldType.Pointer
                 && val->getType()->isStructTy()
                 && !(rightNV.TypeAndValue.IsMove
                      || (rightNV.Storage == nullptr && rightNV.IsOwningString))
                 && compiler->GetFunction("copy"))
        {
            LLVMBackend::NamedVariable srcNV;
            srcNV.Primary = val;
            srcNV.BaseType = val->getType();
            srcNV.TypeAndValue.TypeName = "string";
            if (auto* copied = compiler->CreateOverloadedFunctionCall("copy", { srcNV }))
                val = copied;
        }
        // Box a thin concrete source, or re-box an already-fat one to the FIELD's interface exactly
        // as the `=` path does. Without the rebox the field keeps the SOURCE's vtable and its slot 0
        // answers the field interface's method 0 - a silently wrong call, no crash, no error.
        // A VIEW destination is excluded exactly as at the sibling call / assignment doors: an
        // 'IA[]' slot is thin, so boxing here would store 16 bytes into 8.
        else if (fieldType.IsInterface && !fieldType.IsArrayView)
        {
            val = CoerceInitValueToInterface(rightNV, val, fieldType.TypeName, errCtx);
        }
        // Closure destination, spelled OR generic-substituted: the same two conversions the `=`
        // path applies. Without them a fat lambda literal lands in a thin slot and fails the verifier.
        else if (!fieldType.Pointer && fieldType.ConstArraySize == 0)
        {
            const LLVMBackend::TypeAndValue* clo = fieldType.IsFunctionPointer
                ? static_cast<const LLVMBackend::TypeAndValue*>(&fieldType)
                : compiler->GetEncodedClosureType(fieldType.TypeName);
            if (clo != nullptr)
                compiler->RejectLiteralZeroFunctionSlot(val, rightNV);
            if (clo != nullptr && clo->IsThinFnPtr()
                && val->getType() == compiler->GetClosureFatPtrType())
            {
                compiler->UnregisterOwnedClosureTemp(val);
                if (compiler->ClosureIsStaticallyNonCapturing(val))
                    val = compiler->CoerceClosureFatToThin(val, *clo);
                else
                {
                    LogErrorContext(errCtx, compiler->DescribeCapturingClosureToThin(rightNV.LambdaCaptureNames));
                    val = llvm::UndefValue::get(compiler->BuildThinFnPtrType(*clo));
                }
            }
            else if (clo != nullptr && !clo->IsThinFnPtr()
                     && !val->getType()->isStructTy() && val->getType()->isPointerTy())
                // Brace-init knows the INSTANTIATION name, which is mangled; render it as source
                // the user could write, or hand back the raw name when it cannot be rendered.
                val = compiler->WidenToClosureFatChecked(val, rightNV, {},
                    std::format("'{}.{}'", SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = typeName }), fieldName));
            else if (clo != nullptr && clo->IsThinFnPtr()
                     && !val->getType()->isStructTy() && val->getType()->isPointerTy())
            {
                // The thin sibling of the widen above: brace-init into a thin function<> field
                // (spelled or generic-encoded) fed a raw pointer; a named function takes the field's signature.
                compiler->CheckThinFnPtrAssignProvenance(val, rightNV,
                    std::format("'{}.{}'", SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = typeName }), fieldName));
                val = compiler->BareFunctionToThinValue(val, clo);
            }
        }

        // Numeric field brace-init: convert the source to the field's own type with the `=` path's
        // conversion (widen, then cast), so a float source into a double slot is extended and a
        // wider source into a narrower slot is truncated. A raw store of a mismatched width leaves
        // the rest of a wider field stale, or writes past a narrower one. A bool field is excluded:
        // it is a truth test, handled just below.
        bool rightIsUnsigned = rightNV.TypeAndValue.IsUnsignedInteger() != -1;
        if (val != nullptr && !val->getType()->isIntegerTy(1)
            && (val->getType()->isIntegerTy() || val->getType()->isFloatingPointTy())
            && !fieldType.Pointer && fieldType.ConstArraySize == 0)
        {
            auto* fieldLLVMType = compiler->GetType(fieldType);
            // The range check runs before the same-LLVM-type shortcut: '{ f = (u32)4294967295 }'
            // into an int field has equal types but does not fit.
            if (fieldLLVMType != nullptr && !fieldLLVMType->isIntegerTy(1)
                && (fieldLLVMType->isIntegerTy() || fieldLLVMType->isFloatingPointTy()))
            {
                // Named brace fields ('{ f = v }'): T119 constant fit applies to this store.
                LLVMBackend::NativeBraceScope braceScope(*compiler);
                // The loaded value is no longer a constant; the literal source is checked here.
                compiler->RefuseNarrowingBraceConstant(rightNV.Primary, rightIsUnsigned, fieldLLVMType,
                    fieldType.IsUnsignedInteger() != -1);
                if (fieldLLVMType != val->getType())
                    val = compiler->ConvertScalarToType(val, fieldLLVMType, rightIsUnsigned,
                        fieldType.IsUnsignedInteger() != -1);
            }
        }

        // A 'bool' field brace-init is a truth test like the '=' and cast paths. Upconvert only
        // widens, so without this the source bytes land raw on the one-byte i1 slot.
        if (val != nullptr && !fieldType.Pointer && fieldType.ConstArraySize == 0
            && !val->getType()->isIntegerTy(1)
            && (val->getType()->isIntegerTy() || val->getType()->isFloatingPointTy()
                || val->getType()->isPointerTy()))
        {
            auto* boolFieldType = compiler->GetType(fieldType);
            if (boolFieldType != nullptr && boolFieldType->isIntegerTy(1))
                val = compiler->CoerceToBoolCondition(val);
        }

        llvm::Value* destination = nullptr;
        if (sd.IsUnion)
        {
            // Union: all fields alias at offset 0. Store through the raw pointer with the explicit field type.
            auto* fieldLLVMType = compiler->GetType(fieldType);
            compiler->CreateAssignment(val, structPtr, rightIsUnsigned, fieldLLVMType);
            destination = structPtr;
        }
        else
        {
            auto* gep = compiler->CreateCxxFieldGEP(sd, structPtr, (unsigned)fieldIdx, fieldName + "_init");
            // The aggregate seed already constructed this field. Release that value before the
            // named override replaces it, matching the ordinary assignment drop-old path.
            const bool deferDropUntilConverted = deferForeignReplacementDrop
                && !destinationIsFresh && isForeignCxxField
                && rightNV.TypeAndValue.TypeName != fieldType.TypeName
                && !val->getType()->isStructTy();
            if (!destinationIsFresh && fieldType.IsUnique
                && fieldType.Pointer)
                compiler->EmitUniqueFieldDelete(
                    *compiler->builder, gep,
                    compiler->GetFullDestructorForDelete(fieldType.TypeName), val);
            else if (!destinationIsFresh && fieldType.IsFatInterfaceValue()
                && fieldType.IsUnique)
            {
                auto* oldFat = compiler->builder->CreateLoad(compiler->GetFatPtrType(), gep);
                compiler->DeleteInterfaceValue(oldFat, fieldType.TypeName, nullptr);
            }
            else if (!destinationIsFresh && !deferDropUntilConverted && !fieldType.Pointer
                     && (fieldType.TypeName == "string"
                         || compiler->IsOwningValueType(fieldType.TypeName))
                     && (val->getType()->isStructTy()
                         || (isForeignCxxField && rightNV.TypeAndValue.TypeName != fieldType.TypeName)))
            {
                // A converting-constructor source (plan converting-constructors.md) is scalar
                // but still replaces the seed object, so the seed is released first too.
                if (auto* dtor = compiler->GetOrCreateFullDestructor(fieldType.TypeName))
                    compiler->builder->CreateCall(dtor->getFunctionType(), dtor, { gep });
            }
            if (isForeignCxxField
                && EmitForeignCxxValueIntoSlot(fieldType, gep, rightNV, val, ownedTempMark,
                                               "into a struct field", errCtx, destinationIsFresh,
                                               deferDropUntilConverted))
            {
                destination = gep;
                if (destinationIsFresh)
                    compiler->NoteUnwindPartial(LLVMBackend::UnwindPartialEntry::Kind::Slot,
                                                gep, fieldType.TypeName);
                return true;
            }
            compiler->builder->CreateStore(val, gep);
            destination = gep;
        }

        // In a by-value brace argument, this aggregate's cleanup takes over an owning field temp.
        if (inCallArgument_ && !fieldType.Pointer && !fieldType.IsAlias
            && !fieldType.IsArrayView && fieldType.ConstArraySize == 0
            && !compiler->IsCxxRecord(fieldType.TypeName)
            && compiler->GetType(fieldType) == val->getType()
            && compiler->IsOwningValueType(fieldType.TypeName))
        {
            if (rightNV.TernaryTempAlreadyRegistered)
            {
                if (auto* join = llvm::dyn_cast<llvm::PHINode>(rightNV.Primary))
                    compiler->AdoptTernaryArmStructTemps(join);
            }
            else if (compiler->IsProducedTempValue(rightNV.Primary))
                compiler->UnregisterOwnedStructTempsHolding(rightNV.Primary);
            else if (compiler->IsOwnedStructTempSince(ownedTempMark, rightNV.Storage))
                compiler->UnregisterOwnedStructTemp(rightNV.Storage);
        }

        // The field now holds this pointer, so account for it exactly as the `=` path does: refcount
        // a new-allocated local's escape, or null the source. Without this both the source local and
        // the field's destructor free the one pointee.
        TransferPointerOwnershipOnStore(rightNV, destination, fieldType.IsInterface, errCtx);

        // Closure (Lambda<>) field: the destination now owns the closure env, so drop it from the
        // owned-closure temp-flush list (otherwise the env is freed twice - by the end-of-full-
        // expression flush AND by the owner's destructor). Mirrors the field-assignment path's
        // UnregisterOwnedClosureTemp for the temp case; a clone result is not registered (no-op).
        if (isClosureVal)
            compiler->UnregisterOwnedClosureTemp(val);
        // The field now owns the stored string buffer (a fresh copy, or a transferred
        // owned temp); drop it from the owned-string temp-flush list so it is not freed
        // twice (flush + field destructor).
        if (fieldType.TypeName == "string" && !fieldType.Pointer && val->getType()->isStructTy())
            compiler->UnregisterOwnedStringTemp(val);
        // A `move`d owning-string source: null its `_ptr` so the moved-from source's suppressed
        // destructor stays a no-op and the one buffer is owned solely by this field.
        TransferMoveStringOwnershipOnStore(rightNV, errCtx);
        if (destinationIsFresh && !fieldType.Pointer && !fieldType.ElemPointer
            && !fieldType.IsArrayView && fieldType.ConstArraySize == 0
            && compiler->HasNonTrivialDestructor(fieldType.TypeName))
            compiler->NoteUnwindPartial(LLVMBackend::UnwindPartialEntry::Kind::Slot,
                                        destination, fieldType.TypeName);
        return true;
    }

void MainListener::LogPointerBraceInitReject(
        antlr4::ParserRuleContext* ctx,
        const std::string& what,
        const std::string& typeName,
        const std::string& typeText,
        bool canAllocate,
        bool isUnique) {
        LLVMBackend::TypeAndValue type;
        type.TypeName = typeName;
        const std::string shownType = SpellType(*Compiler(ctx), type);
        // Carry 'unique' into the remedy so the suggestion is still a legal declaration.
        std::string qual = isUnique ? "unique " : "";
        std::string remedy = canAllocate
            ? std::format("allocate one first ('{}{}* p = new {}();') and assign through the "
                          "pointer instead", qual, shownType, shownType)
            : "assign an address to it instead";
        LogErrorContext(ctx, std::format(
            "cannot apply a brace initializer to the POINTER {} of type '{}' - the braces would "
            "write into the pointer itself, which holds an address and not a '{}'; {}",
            what, typeText, shownType, remedy));
    }

void MainListener::LogNonAggregateBraceInitReject(
        antlr4::ParserRuleContext* ctx,
        const std::string& name,
        const std::string& typeName) {
        const std::string shownType = SpellType(*Compiler(ctx),
            LLVMBackend::TypeAndValue{ .TypeName = typeName });
        LogErrorContext(ctx, std::format(
            "brace initializer with values is not supported on '{}' - '{}' is not "
            "a struct/union/class or a recognized container; assign it after declaration instead",
            name, shownType));
    }

void MainListener::LogArrayViewFieldBraceInitReject(
        antlr4::ParserRuleContext* ctx,
        const std::string& name,
        const std::string& typeName,
        bool elemPointer) {
        std::string base = typeName;
        if (base.ends_with("[]")) base.resize(base.size() - 2);
        std::string star = elemPointer ? "*" : "";
        const std::string shownType = base + star + "[]";
        const std::string fixedType = base + star + "[N]";
        LogErrorContext(ctx, std::format(
            "cannot apply a brace initializer with values to view field '{}' of type '{}' - "
            "a view field owns no storage for the list to write into, and storage built for it "
            "would need a lifetime tied to the containing object, which a field cannot provide; "
            "declare it as '{}' with a fixed size instead",
            name, shownType, fixedType));
    }

std::string MainListener::DescribePointerDeclType(const LLVMBackend::TypeAndValue& tv) const {
        if (tv.ConstArraySize > 0) return DescribeArrayShape(tv);
        // A simd type's TypeName is its ELEMENT ('float'), so spell the vector back out.
        LLVMBackend::TypeAndValue baseType = tv;
        baseType.Pointer = false;
        baseType.ElemPointer = false;
        baseType.PointerDepth = 0;
        baseType.IsArrayView = false;
        std::string base = SpellType(*compilerLLVM, baseType);
        if (tv.IsSimd) base = std::format("simd<{},{}>", base, tv.SimdLanes);
        return base + std::string(FixedArrayElementStars(tv), '*');
    }

std::string MainListener::CodeValueDestSpelling(const LLVMBackend::TypeAndValue& dest) const {
        if (dest.IsArrayView && dest.ConstArraySize == 0)
        {
            LLVMBackend::TypeAndValue baseType = dest;
            baseType.Pointer = false;
            baseType.ElemPointer = false;
            baseType.PointerDepth = 0;
            baseType.IsArrayView = false;
            return SpellType(*compilerLLVM, baseType)
                + std::string(dest.ElemPointer ? 1 : 0, '*') + "[]";
        }
        return DescribePointerDeclType(dest);
    }

std::string MainListener::CodeValueCastAdvice(const LLVMBackend::TypeAndValue& dest) const {
        if (!dest.Pointer || dest.IsArrayView || dest.ConstArraySize > 0) return {};
        return CodeValueDestSpelling(dest);
    }

// A bare function name is initially resolved to one overload; rebind it before slot checks or stores.
void MainListener::BindNamedFunctionToFuncPtrSlot(LLVMBackend* compiler,
        LLVMBackend::NamedVariable& value, const LLVMBackend::TypeAndValue& slot,
        antlr4::ParserRuleContext* site)
{
        auto* function = llvm::dyn_cast_or_null<llvm::Function>(value.Primary);
        if (function == nullptr || value.CallerName.empty()) return;
        if (!slot.IsFunctionPointer) return;
        if (compiler->NamedFunctionOverloadForFuncPtr(value.CallerName, slot) == nullptr
            && compiler->HasOverloadSetForFunction(function))
            LogErrorContext(site, std::format("no overload of '{}' matches function type '{}'",
                value.CallerName, compiler->FuncPtrSpellingOf(slot)));
        if (auto* bound = compiler->GetFunctionForFuncPtr(value.CallerName,
                static_cast<int>(slot.FuncPtrParams.size()), &slot.FuncPtrParams,
                &slot, /*compareReturnOwnership*/ true))
            value.Primary = bound;
}

llvm::Value* MainListener::ParseFieldDefaultInitializer(
        const std::string& structName,
        const LLVMBackend::TypeAndValue& field,
        CFlatParser::AssignmentExpressionContext* ae,
        bool* srcIsUnsigned,
        llvm::Value* intoSlot,
        bool* constructedInPlace) {
        auto* compiler = Compiler(ae);
        const std::string fieldDefaultOwner = structName;
        llvm::SaveAndRestore<std::string> fieldDefaultOwnerScope(
            fieldDefaultAggregateOwner_, fieldDefaultOwner);
        llvm::SaveAndRestore<std::string> fieldDefaultNameScope(
            fieldDefaultAggregateFieldName_, field.VariableName);
        std::string fieldOwner = lambdaEnclosingAggregateOwner_;
        if (!fieldDefaultOwner.empty()) fieldOwner = fieldDefaultOwner;
        llvm::SaveAndRestore<std::string> fieldOwnerScope(
            lambdaEnclosingAggregateOwner_, std::move(fieldOwner));
        const auto fieldInitTemps = compiler->MarkOwnedTemps();
        if (field.AllocAlignValue > LLVMBackend::kDefaultNewAlign
            && AsDirectNew(ae) != nullptr)
            compiler->pendingInitAllocAlign = field.AllocAlignValue;
        DeclExpectedTypeScope fieldExpectedScope(&declExpectedType, field, true);
        auto nv = ParseAssignmentExpressionNamed(ae);
        if (!structName.empty())
            if (auto* identifier = GenericFunctionValueIdentifier(ae);
                identifier != nullptr && llvm::isa_and_nonnull<llvm::Function>(nv.Primary))
            {
                const auto* resolved = compiler->FindSymbolForFunction(
                    llvm::cast<llvm::Function>(nv.Primary));
                if (resolved != nullptr && !resolved->IsMethod)
                {
                    const std::string& memberName = identifier->getText();
                    const std::string staticMemberName = structName + "." + memberName;
                    auto staticMembers = compiler->functionTable.find(staticMemberName);
                    const bool hasStaticMember = staticMembers != compiler->functionTable.end()
                        && std::any_of(staticMembers->second.begin(), staticMembers->second.end(),
                            [](const auto& symbol) { return !symbol.IsMethod; });
                    if (hasStaticMember && field.IsFunctionPointer)
                    {
                        nv.Primary = compiler->GetFunctionForFuncPtr(staticMemberName,
                            static_cast<int>(field.FuncPtrParams.size()), &field.FuncPtrParams,
                            &field, /*compareReturnOwnership*/ true);
                        nv.CallerName = staticMemberName;
                    }
                    else
                    {
                        bool memberMethodFound = false;
                        for (const auto& [_, overloads] : compiler->functionTable)
                            for (const auto& symbol : overloads)
                                if (symbol.IsMethod && symbol.SourceName == memberName
                                    && !symbol.Parameters.empty()
                                    && symbol.Parameters.front().TypeName == structName
                                    && symbol.Parameters.front().Pointer)
                                {
                                    memberMethodFound = true;
                                    break;
                                }
                        if (memberMethodFound)
                            LogErrorContext(ae,
                                "reference to non-static member function must be called");
                    }
                }
            }
        BindNamedFunctionToFuncPtrSlot(compiler, nv, field, ae);
        if (field.IsFunctionPointer)
            if (auto* namedFunction = llvm::dyn_cast_or_null<llvm::Function>(nv.Primary))
            {
                const auto actual = compiler->FuncPtrSigOfBoundFunction(nv.CallerName, namedFunction);
                if (actual.IsFunctionPointer)
                {
                    const std::string mismatch = compiler->DescribeFuncPtrBindMismatch(
                        nv.CallerName, actual, field, /*compareReturnOwnership*/ true);
                    if (!mismatch.empty()) LogErrorContext(ae, mismatch);
                }
            }
        if (std::string why = compiler->FuncPtrValueOwnershipReason(nv, field); !why.empty())
            LogErrorContext(ae, std::format("incompatible function pointer initializer: {}", why));
        // A field default initializer is a field store (`cppi.CrPair* p = &crConst;` is refused).
        RejectConstRecordPointeeStore(ae, field, nv, "field", field.VariableName);
        compiler->RejectImplicitIntegerPointeePointerConversion(nv.TypeAndValue, field);
        llvm::Value* val = LoadNamedVariable(nv);
        // A scalar initializer of a C++ class field (`IntC c = 5;`) is a converting constructor
        // call, the same one a local `IntC c = 5;` makes; a plain store would leave the field unset.
        if (val != nullptr && !field.Pointer && !field.IsAlias && !field.IsArrayView
            && field.ConstArraySize == 0 && compiler->IsCxxRecord(field.TypeName)
            && !nv.TypeAndValue.Pointer
            && (val->getType()->isIntegerTy() || val->getType()->isFloatingPointTy()))
        {
            LLVMBackend::NamedVariable source = nv;
            source.Primary = val;
            // A literal is typed the way the local and new-expression paths type it.
            bool suffixedLiteral = false;
            if (source.LiteralIdentity.empty())
                source.LiteralIdentity = LLVMBackend::LiteralIdentityForOverload(
                    ae->getText(), &suffixedLiteral);
            TypeUntypedCtorArg(source.TypeAndValue, val,
                               suffixedLiteral ? source.LiteralIdentity : std::string(), nullptr);
            llvm::Value* slot = intoSlot;
            llvm::Type* classType = compiler->GetType(field);
            if (slot == nullptr && classType != nullptr)
                slot = compiler->AllocaAtEntry(classType, nullptr, "field.cxx.init");
            // A tie CFlat cannot judge, or a candidate it drops, is clang's: the same test a local runs.
            std::string why;
            bool nullPointerCandidate = false;
            std::vector<LLVMBackend::NamedVariable> ctorVars{ source };
            const std::vector<LLVMBackend::TypeAndValue> ctorTypes{ source.TypeAndValue };
            const auto* listed = compiler->SelectCxxConstructor(field.TypeName, ctorTypes, why,
                                                               false, &ctorVars);
            const bool clangDecides = compiler->CxxConstructorNeedsClangResolution(
                    field.TypeName, listed, ctorTypes, &ctorVars, /*allowExplicit*/ true,
                    &nullPointerCandidate)
                || (listed == nullptr && why.find("more than one") != std::string::npos);
            std::string refusal;
            auto converted = LLVMBackend::CxxConvertingCtorResult::NotApplicable;
            if (slot != nullptr && clangDecides)
            {
                std::string wrapperName;
                std::string wrapperError;
                if (compiler->RequestCxxVariadicConstructor(field.TypeName, { source }, wrapperName,
                                                            wrapperError, /*copyInit*/ true))
                {
                    LLVMBackend::NamedVariable self;
                    self.Primary = slot;
                    self.BaseType = slot->getType();
                    self.TypeAndValue.TypeName = field.TypeName;
                    self.TypeAndValue.Pointer = true;
                    self.IsRvalue = true;
                    compiler->CreateOverloadedFunctionCall(wrapperName, { self, source });
                    converted = LLVMBackend::CxxConvertingCtorResult::Converted;
                }
                else if (!wrapperError.empty())
                {
                    refusal = wrapperError;
                    converted = LLVMBackend::CxxConvertingCtorResult::Refused;
                }
            }
            // Same conversion call as a local `T t = u;` (convertInto): scalar narrowing allowed.
            if (slot != nullptr && converted == LLVMBackend::CxxConvertingCtorResult::NotApplicable)
                converted = compiler->ConvertThroughCxxConvertingCtor(source, field.TypeName, slot,
                                                                      refusal,
                                                                      /*directInit*/ false,
                                                                      /*allowScalarNarrowing*/ true);
            if (converted == LLVMBackend::CxxConvertingCtorResult::Refused)
                LogErrorContext(ae, refusal);
            if (converted == LLVMBackend::CxxConvertingCtorResult::NotApplicable)
            {
                // Never a plain store into a C++ record: the local path refuses the same way.
                LogErrorContext(ae, std::format(
                    "cannot initialize C++ class '{}' from this expression; use '{}(args)', "
                    "'= default', a '{}' lvalue, or 'move <{}> lvalue'",
                    compiler->DisplayCxxClassName(field.TypeName),
                    compiler->DisplayCxxClassName(field.TypeName),
                    compiler->DisplayCxxClassName(field.TypeName),
                    compiler->DisplayCxxClassName(field.TypeName)));
            }
            if (slot != nullptr && classType != nullptr)
            {
                if (converted != LLVMBackend::CxxConvertingCtorResult::NotApplicable
                    && intoSlot != nullptr)
                {
                    if (constructedInPlace != nullptr) *constructedInPlace = true;
                    return nullptr;
                }
                return compiler->builder->CreateLoad(classType, slot);
            }
            return val;
        }
        RejectRawHeapArrayIntoUniqueField(nv, field, field.VariableName, ae);
        // R5: `unique T* f = new T();` on a C++ class (std::unique_ptr<T>) adopts the pointer.
        if (val != nullptr && !field.Pointer && val->getType()->isPointerTy())
        {
            auto adoptSource = nv;
            adoptSource.Primary = val;
            if (auto* adopted = compiler->AdoptRawPointerAsCxxUniquePtrValue(
                    field.TypeName, adoptSource, std::format("field of '{}'", structName)))
                val = adopted;
        }
        if (val != nullptr && !field.Pointer
            && compiler->IsCoreUniqueType(field.TypeName)
            && (val->getType()->isPointerTy()
                || (val->getType() == compiler->GetFatPtrType()
                    && compiler->IsInterfaceType(MangledGenericArgument(
                        *compiler, field.TypeName)))))
        {
            auto ctorArg = nv;
            ctorArg.Primary = val;
            ctorArg.BaseType = val->getType();
            val = compiler->AdjustCxxPointerForUniqueAdoption(
                ctorArg, field, val, std::format("field of '{}'", structName));
            ctorArg.Primary = val;
            ctorArg.BaseType = val->getType();
            ShapeCoreUniqueCtorArg(compiler, ctorArg, field.TypeName, val);
            ctorArg.IsOwning = ctorArg.IsOwning || compiler->lastOwningResult
                || compiler->lastCallReturnsOwned || compiler->IsOwnedNewTemp(val)
                || compiler->IsOwningPtrTempValue(val) || compiler->IsMovedOutPtrValue(val);
            val = compiler->CreateCoreUniqueFromRawPointerCall(ctorArg, field);
            compiler->ConsumeOwnedNewTemp(nv.Primary);
            compiler->lastOwningResult = false;
            compiler->lastCallReturnsOwned = false;
        }
        if (srcIsUnsigned != nullptr)
            *srcIsUnsigned = nv.TypeAndValue.IsUnsignedInteger() != -1;
        if (RejectImplicitPrimitiveToPointer(
                ae, field, nv, val, "default-initialize",
                std::format("field '{}.{}'", structName, field.VariableName)))
            return val;
        if (RejectImplicitPointerToNumber(
                ae, field, nv, nv.Primary, "default-initialize",
                std::format("field '{}.{}'", structName, field.VariableName)))
            return val;
        if (val != nullptr && RejectConstDiscardingConversion(ae, nv, val, field))
            return val;
        RejectCodeValueIntoDataSlot(ae, nv, field, "default-initialize",
            std::format("field '{}.{}' of", structName, field.VariableName));
        // The field-DEFAULT twin of the declaration-initializer and `=` legs: a C++ reference
        // result binds the referent, a derived pointer shifts to the base subobject.
        {
            const std::string defaultDest = std::format("field '{}.{}'", structName,
                                                        field.VariableName);
            if (val != nullptr && val->getType()->isStructTy())
                if (auto* refAddr = compiler->CxxReferenceResultAsPointer(field, nv, defaultDest))
                    val = refAddr;
            if (val != nullptr)
                val = compiler->AdjustCxxPointerForStore(field, nv.TypeAndValue, val, defaultDest);
        }
        // A string LITERAL is a 'const char*', never a 'T*' - the field-DEFAULT leg of the same
        // gate the declarator, `=`, brace-init, return and argument sites apply.
        RejectStringLiteralIntoStructPointer(ae, field, val,
            std::format("field '{}.{}'", structName, field.VariableName));
        // Aggregate field defaults use a shared path instead of the ordinary declaration upcast.
        // Box a concrete pointer here before the aggregate is assembled, otherwise an interface
        // field initialized with `new Impl()` is returned as a null fat value and its later
        // dispatch check reports a false null-interface proof.
        if (val != nullptr && field.IsFatInterfaceValue() && val->getType()->isPointerTy()
            && nv.TypeAndValue.Pointer
            && compiler->StructImplementsInterface(nv.TypeAndValue.TypeName, field.TypeName))
        {
            val = BoxConcreteIntoInterface(ae, val, true, nv.TypeAndValue.TypeName,
                                           field.TypeName, &nv,
                                           field.IsUnique);
            // The aggregate now carries the pointer (as a box), so an owning `new` temporary
            // must not also be released by end-of-expression cleanup. A non-unique interface
            // field intentionally remains a non-owning reference; its object is not field-owned.
            compiler->ConsumeOwnedNewTemp(nv.Primary);
        }
        // Thin sibling of the widen elsewhere: a default initializer into a thin
        // function<> field fed a raw pointer (spelled or generic-encoded).
        const LLVMBackend::TypeAndValue* clo = field.IsFunctionPointer
            ? &field : compiler->GetEncodedClosureType(field.TypeName);
        if (val && clo)
            compiler->RejectLiteralZeroFunctionSlot(val, nv);
        if (val && clo && !field.Pointer && field.ConstArraySize == 0
            && val->getType()->isPointerTy() && !val->getType()->isStructTy())
        {
            if (clo->IsThinFnPtr())
            {
                compiler->CheckThinFnPtrAssignProvenance(val, nv,
                    std::format("'{}.{}'", structName, field.VariableName));
                val = compiler->BareFunctionToThinValue(val, clo);
            }
            // Fat sibling: reject a provable data pointer first (LogError throws, so a
            // rejected source never reaches the widen), then widen a legal bare/thin source.
            else
            {
                std::string destDesc = std::format("'{}.{}'", structName, field.VariableName);
                compiler->CheckFatClosureAssignProvenance(val, nv, destDesc);
                val = compiler->WidenBareOrThinToClosureFat(val, *clo);
            }
        }
        // A field whose declared type is a VALUE struct with an implicit conversion from a raw
        // char pointer converts HERE, the way the declarator converts a local of the same type.
        // Without it the aggregate assembler sees a type mismatch on a struct-typed field and
        // default-constructs the field type, silently dropping the declared initializer.
        if (val != nullptr && !field.Pointer && field.ConstArraySize == 0 && !field.IsArrayView
            && !field.IsFunctionPointer && !field.IsFatInterfaceValue()
            && !compiler->IsCoreUniqueType(field.TypeName)
            && val->getType() == cflat_llvm::PointerTo(compiler->builder->getInt8Ty())
            && compiler->GetDataStructure(field.TypeName).StructType != nullptr
            && compiler->GetFunction("operator " + field.TypeName) != nullptr)
        {
            val = compiler->CoerceCharPointerToString(val);
        }
        compiler->FlushOwnedTempsSince(fieldInitTemps, val);
        return val;
    }

CFlatParser::InitializerListContext* MainListener::FieldDefaultBraceList(
        const LLVMBackend::DeclTypeAndValue& field) {
        if (field.Initializer != nullptr)
            return field.Initializer->initializerList();
        return field.BraceInitializer;
}

bool MainListener::FieldHasEmptyCxxBraceInitializer(
        const LLVMBackend::DeclTypeAndValue& field) {
        const bool emptyAssignmentBrace = field.Initializer != nullptr
            && field.Initializer->LeftBrace() != nullptr
            && field.Initializer->initializerList() == nullptr;
        const bool emptyBareBrace = field.HasEmptyBareBraceInitializer;
        const bool emptyBraceList = field.BraceInitializer != nullptr
            && field.BraceInitializer->fieldInit().empty();
        if ((!emptyAssignmentBrace && !emptyBareBrace && !emptyBraceList)
            || field.Pointer || field.ElemPointer
            || field.IsAlias || field.IsArrayView || field.ConstArraySize != 0
            || !field.ConstInnerDimensions.empty())
            return false;
        auto* context = field.Initializer != nullptr
            ? static_cast<antlr4::ParserRuleContext*>(field.Initializer)
            : field.BraceInitializer != nullptr
                ? static_cast<antlr4::ParserRuleContext*>(field.BraceInitializer)
                : field.BraceInitializerContext;
        return Compiler(context)->IsCxxRecord(field.TypeName);
}

bool MainListener::RejectNestedNamedBraceInitializer(
        CFlatParser::InitializerListContext* list, const std::string& message) {
        if (list == nullptr) return false;
        bool rejected = false;
        for (auto* fi : list->fieldInit())
        {
            if (fi->Identifier() != nullptr && fi->initializerList() != nullptr)
            {
                LogErrorContext(fi, message);
                rejected = true;
            }
            if (fi->initializerList() != nullptr
                && RejectNestedNamedBraceInitializer(fi->initializerList(), message))
                rejected = true;
        }
        return rejected;
    }

bool MainListener::FieldInitIsBraceElement(CFlatParser::FieldInitContext* fi) {
        return fi != nullptr && fi->Identifier() == nullptr && fi->assignmentExpression().empty();
    }

/*
 * One fixed-array slot written as a brace (`T[N] a = { {f = v}, ... }`). The slot is CONSTRUCTED
 * the way the scalar spelling constructs a local - default constructor if the type has one, else
 * zeroed - so the element type's own field defaults survive a partial list; the list itself then
 * goes through EmitFieldInitializer, which is the single place every field-store rule lives.
 */
void MainListener::EmitBraceElementIntoFixedSlot(
        llvm::Value* elemPtr,
        llvm::StructType* elemStructTy,
        const std::string& elemTypeName,
        CFlatParser::FieldInitContext* fi,
        bool rejectFrameBoundReferences) {
        auto* compiler = Compiler(fi);
        auto* list = fi->initializerList();
        const bool emptyList = list == nullptr || list->fieldInit().empty();
        if (emptyList && compiler->GetFunction(elemTypeName) == nullptr
            && HasUninitializedForeignCxxAggregateReferenceMember(elemTypeName, fi))
        {
            LogErrorContext(fi, std::format(
                "reference member must be initialized in C++ aggregate array element '{}'",
                elemTypeName));
            return;
        }
        if (list != nullptr && !list->fieldInit().empty()
            && compiler->IsForeignCxxAggregateClass(elemTypeName)
            && HasForeignCxxAggregateReferenceMember(elemTypeName, fi))
        {
            TryEmitForeignCxxAggregateBraceInit(
                elemTypeName, list, fi, elemPtr, true,
                false, nullptr, rejectFrameBoundReferences);
            return;
        }
        if (list != nullptr && !list->fieldInit().empty()
            && EmitFreshBraceInitializeAggregate(elemPtr, elemTypeName, list, false))
            return;
        // forceRoot: the GetFunction guard is an exact-key lookup, so a namespace walk here
        // would call a same-named sibling type's constructor.
        llvm::Value* seed = compiler->GetFunction(elemTypeName)
            ? compiler->CreateOverloadedFunctionCall(elemTypeName, {}, true)
            : nullptr;
        // A C++ class has no CFlat constructor FUNCTION, so `seed` is null for it and the zero
        // store below left the slot unconstructed (a null vptr on a polymorphic element) while
        // scope exit still destroyed it. Its own default constructor runs here instead.
        if (seed == nullptr && compiler->CxxElementNeedsDefaultConstruction(elemTypeName))
        {
            // An empty `{}` element of a list-constructor-only class is that constructor, n == 0.
            std::string ctorError;
            if (emptyList && compiler->CxxEmptyBraceUsesListCtor(elemTypeName))
                EmitCxxEmptyBraceConstruct(elemPtr, elemTypeName, fi, true);
            else if (!compiler->EmitCxxArrayDefaultConstruction(
                    elemTypeName, elemPtr, elemStructTy, compiler->builder->getInt64(1), ctorError))
                LogErrorContext(fi, ctorError);
        }
        else if (seed != nullptr && seed->getType() == elemStructTy)
            compiler->CreateAssignment(seed, elemPtr);
        else
            compiler->builder->CreateStore(llvm::Constant::getNullValue(elemStructTy), elemPtr);

        if (list != nullptr && !list->fieldInit().empty())
        {
            EmitFieldInitializer(elemPtr, elemTypeName, list);
        }
    }

/*
 * Build the value of a field whose default is a brace list. Mirrors the local declarator:
 * seed the slot with the field type's OWN default (so its scalar field defaults survive a
 * partial list), then let the named field-inits / container add() calls overwrite on top.
 */
/*
 * Value-initializes the C++ class `typeName` at `slot` from an empty brace: the default
 * constructor when present, else the initializer_list constructor with n == 0 (clang's `{}`).
 * copyListInit refuses an explicit constructor, as copy-list-initialization does.
 */
bool MainListener::EmitCxxEmptyBraceConstruct(
        llvm::Value* slot,
        const std::string& typeName,
        antlr4::ParserRuleContext* braceContext,
        bool copyListInit) {
        auto* compiler = Compiler(braceContext);
        std::vector<LLVMBackend::NamedVariable> elements;
        std::string wrapperName;
        std::string constructorError;
        if (!compiler->RequestCxxDirectBraceConstructor(
                typeName, elements, "{}", copyListInit,
                wrapperName, constructorError))
        {
            if (constructorError.empty())
                constructorError = std::format("C++ class '{}' has no constructor accepting an empty brace list",
                    compiler->DisplayCxxClassName(typeName));
            LogErrorContext(braceContext, constructorError);
            return false;
        }
        LLVMBackend::NamedVariable self;
        self.Primary = slot;
        self.BaseType = slot->getType();
        self.TypeAndValue.TypeName = typeName;
        self.TypeAndValue.Pointer = true;
        self.IsRvalue = true;
        std::vector<LLVMBackend::NamedVariable> wrapperArguments{ self };
        compiler->SetCurrentDebugLocation(braceContext->getStart()->getLine());
        compiler->CreateOverloadedFunctionCall(wrapperName, wrapperArguments);
        return true;
}

bool MainListener::IsCxxEmptyBraceArrayInit(
        const LLVMBackend::TypeAndValue& tv,
        CFlatParser::InitializerListContext* list) {
        if (list == nullptr || list->fieldInit().empty()) return false;
        auto* compiler = Compiler(list);
        if (!compiler->IsCxxRecord(tv.TypeName) || tv.Pointer || tv.ElemPointer || tv.IsAlias
            || tv.IsArrayView || tv.ConstArraySize == 0 || !tv.ConstInnerDimensions.empty())
            return false;
        for (auto* fi : list->fieldInit())
        {
            if (!FieldInitIsBraceElement(fi)) return false;
            auto* nested = fi->initializerList();
            if (nested != nullptr && !nested->fieldInit().empty()) return false;
        }
        return true;
}

llvm::Value* MainListener::ParseFieldDefaultBraceInitializer(
        const std::string& structName,
        const LLVMBackend::DeclTypeAndValue& field,
        CFlatParser::InitializerListContext* list,
        llvm::Value* inPlaceDest) {
        auto* braceContext = list != nullptr
            ? static_cast<antlr4::ParserRuleContext*>(list)
            : field.Initializer != nullptr
                ? static_cast<antlr4::ParserRuleContext*>(field.Initializer)
                : field.BraceInitializerContext;
        if (braceContext == nullptr) return nullptr;
        auto* compiler = Compiler(braceContext);
        const bool isFieldDeclarationDefault = (list != nullptr
                && FieldDefaultBraceList(field) == list)
            || (list == nullptr && FieldHasEmptyCxxBraceInitializer(field));
        const bool emptyCxxBrace = compiler->IsCxxRecord(field.TypeName)
            && !field.Pointer && !field.ElemPointer && !field.IsAlias && !field.IsArrayView
            && field.ConstArraySize == 0 && field.ConstInnerDimensions.empty()
            && ((list != nullptr && list->fieldInit().empty())
                || (list == nullptr && FieldHasEmptyCxxBraceInitializer(field)));
        if (emptyCxxBrace)
        {
            auto* fieldType = compiler->GetDataStructure(field.TypeName).StructType;
            if (fieldType == nullptr) return nullptr;
            const bool constructInPlace = inPlaceDest != nullptr
                && compiler->pendingCxxSretDest_ == inPlaceDest
                && compiler->pendingCxxSretTypeName_ == field.TypeName;
            llvm::Value* slot = constructInPlace ? inPlaceDest : compiler->CreateAlloca(fieldType);
            const bool copyListInit = field.Initializer != nullptr
                && field.Initializer->LeftBrace() != nullptr;
            if (!EmitCxxEmptyBraceConstruct(slot, field.TypeName, braceContext, copyListInit))
                return nullptr;
            if (constructInPlace)
            {
                compiler->pendingCxxSretDest_ = nullptr;
                compiler->pendingCxxSretTypeName_.clear();
                compiler->pendingCxxSretReturn_ = false;
                compiler->pendingCxxSretForFixedArray_ = false;
                return nullptr;
            }
            return compiler->builder->CreateLoad(
                fieldType, slot, structName + "_" + field.VariableName + "_braceinit");
        }
        if (list != nullptr && !list->fieldInit().empty()
            && !field.Pointer && !field.ElemPointer && !field.IsAlias && !field.IsArrayView
            && field.ConstArraySize == 0 && field.ConstInnerDimensions.empty()
            && compiler->IsForeignCxxAggregateClass(field.TypeName)
            && HasForeignCxxAggregateReferenceMember(field.TypeName, braceContext))
        {
            auto* fieldType = compiler->GetDataStructure(field.TypeName).StructType;
            if (fieldType == nullptr) return nullptr;
            const bool constructInPlace = inPlaceDest != nullptr
                && compiler->pendingCxxSretDest_ == inPlaceDest
                && compiler->pendingCxxSretTypeName_ == field.TypeName;
            llvm::Value* slot = constructInPlace ? inPlaceDest
                : compiler->CreateAlloca(fieldType);
            const bool copyListInit = field.Initializer != nullptr
                && field.Initializer->LeftBrace() != nullptr;
            if (!TryEmitForeignCxxAggregateBraceInit(
                field.TypeName, list, braceContext, slot, copyListInit,
                false, nullptr, isFieldDeclarationDefault))
                return nullptr;
            if (constructInPlace)
            {
                compiler->pendingCxxSretDest_ = nullptr;
                compiler->pendingCxxSretTypeName_.clear();
                compiler->pendingCxxSretReturn_ = false;
                compiler->pendingCxxSretForFixedArray_ = false;
                return nullptr;
            }
            return compiler->builder->CreateLoad(
                fieldType, slot, structName + "_" + field.VariableName + "_braceinit");
        }
        const std::string displayStructName = SpellType(*compiler,
            LLVMBackend::TypeAndValue{ .TypeName = structName });
        // A 'T[]' VIEW field: the local spelling infers backing storage a field cannot outlive,
        // so a list with values is rejected ('{}' parses to no initializerList and never gets here).
        if (field.IsArrayView) {
            if (!list->fieldInit().empty())
                LogArrayViewFieldBraceInitReject(list,
                    std::format("{}.{}", SpellType(*compiler,
                        LLVMBackend::TypeAndValue{ .TypeName = structName }), field.VariableName),
                    SpellType(*compiler, field), field.ElemPointer);
            return nullptr;
        }
        if (field.ConstArraySize > 0)
            return EmitFieldDefaultFixedArrayBrace(
                structName, field, list, nullptr, isFieldDeclarationDefault);

        bool isCoreUniqueField = compiler->IsCoreUniqueType(field.TypeName)
            && !field.Pointer;
        if (isCoreUniqueField)
        {
            std::string pointee = MangledGenericArgument(*compiler, field.TypeName);
            const bool hasAlignedWrapperType = field.AllocAlignValue != 0;
            const std::string pointeeSpelling = "unique "
                + SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = pointee })
                + (compiler->IsInterfaceType(pointee) ? "" : "*");
            const std::string typeText = hasAlignedWrapperType
                ? SpellType(*compiler, field) : pointeeSpelling;
            LogPointerBraceInitReject(list,
                std::format("field '{}.{}'", displayStructName, field.VariableName),
                hasAlignedWrapperType ? field.TypeName : pointee, typeText,
                hasAlignedWrapperType ? false : CanSuggestAllocation(list, field), true);
            return nullptr;
        }

        bool isContainer = MangledBase(field.TypeName) == "list"
            || MangledBase(field.TypeName) == "array"
            || MangledBase(field.TypeName) == "dictionary";
        auto* fieldType = compiler->GetDataStructure(field.TypeName).StructType;
        // Mirrors the two declarator scopes, in their order: a non-struct non-container type
        // first (a POINTER's TypeName is the pointee, so it is a struct and falls through).
        if (!isContainer && fieldType == nullptr)
            LogNonAggregateBraceInitReject(list,
                std::format("{}.{}", SpellType(*compiler,
                    LLVMBackend::TypeAndValue{ .TypeName = structName }), field.VariableName),
                SpellType(*compiler, field));
        if (field.Pointer || field.ElemPointer)
            LogPointerBraceInitReject(list,
                std::format("field '{}.{}'", displayStructName, field.VariableName),
                SpellType(*compiler, field), DescribePointerDeclType(field),
                CanSuggestAllocation(list, field),
                field.IsUnique);
        if (fieldType == nullptr) return nullptr;

        // An armed sret slot: the seed constructor builds straight into it, so the list edits the
        // field in place. A separate temp here would be dropped by the caller (initializedInPlace).
        const bool seedInPlace = inPlaceDest != nullptr
            && compiler->pendingCxxSretDest_ == inPlaceDest
            && compiler->GetFunction(field.TypeName) != nullptr;
        const bool freshBraceInit = CanFreshBraceInitializeAggregate(field.TypeName, list);
        llvm::Value* slot = (seedInPlace || (freshBraceInit && inPlaceDest != nullptr))
            ? inPlaceDest : compiler->CreateAlloca(fieldType);
        if (freshBraceInit)
        {
            if (!EmitFreshBraceInitializeAggregate(slot, field.TypeName, list))
                return nullptr;
            if (seedInPlace)
            {
                compiler->pendingCxxSretDest_ = nullptr;
                compiler->pendingCxxSretTypeName_.clear();
                compiler->pendingCxxSretReturn_ = false;
                compiler->pendingCxxSretForFixedArray_ = false;
            }
            else if (inPlaceDest != nullptr && compiler->pendingCxxSretDest_ == inPlaceDest)
            {
                compiler->pendingCxxSretDest_ = nullptr;
                compiler->pendingCxxSretTypeName_.clear();
                compiler->pendingCxxSretReturn_ = false;
                compiler->pendingCxxSretForFixedArray_ = false;
            }
        }
        else if (seedInPlace)
            compiler->CreateOverloadedFunctionCall(field.TypeName, {}, true);
        else
        {
            llvm::Value* seed = nullptr;
            // forceRoot: the GetFunction guard is an exact-key lookup, so a namespace walk here
            // would call a same-named sibling type's constructor.
            if (compiler->GetFunction(field.TypeName))
                seed = compiler->CreateOverloadedFunctionCall(field.TypeName, {}, true);
            if (seed == nullptr || seed->getType() != fieldType)
                seed = llvm::Constant::getNullValue(fieldType);
            compiler->builder->CreateStore(seed, slot);
        }

        if (!freshBraceInit && !TryEmitContainerInitializer(slot, field, list))
            EmitFieldInitializer(slot, field.TypeName, list);
        if (seedInPlace || (freshBraceInit && inPlaceDest != nullptr))
            return nullptr;
        return compiler->builder->CreateLoad(
            fieldType, slot, structName + "_" + field.VariableName + "_braceinit");
    }

/*
 * The fixed-array arm of a field default's brace list. Mirrors the LOCAL declarator's
 * array-brace split (MainListener_Declarations.cpp ~2828) arm for arm, but writes into a
 * slot alloca and hands the loaded '[N x T]' aggregate back for the CreateInsertValue.
 */
llvm::Value* MainListener::EmitFieldDefaultFixedArrayBrace(
        const std::string& structName,
        const LLVMBackend::DeclTypeAndValue& field,
        CFlatParser::InitializerListContext* list,
        llvm::Value** outSlot,
        bool rejectFrameBoundReferences) {
        auto* compiler = Compiler(list);
        const std::string displayStructName = SpellType(*compiler,
            LLVMBackend::TypeAndValue{ .TypeName = structName });
        auto* arrTy = llvm::dyn_cast<llvm::ArrayType>(compiler->GetType(field));
        if (arrTy == nullptr) return nullptr;

        // Positional ({v0, v1}) vs value-init ({} / {field=v}) - an element carrying no
        // field name is what distinguishes them, exactly as the declarator decides it.
        bool positional = false;
        for (auto* fi : list->fieldInit())
            if (fi->Identifier() == nullptr
                && (fi->assignmentExpression().size() == 1 || FieldInitIsBraceElement(fi)))
            { positional = true; break; }
        bool emptyList = list->fieldInit().empty();

        std::string path = std::format("{}.{}", structName, field.VariableName);
        const std::string displayPath = displayStructName + "." + field.VariableName;
        auto* slot = compiler->AllocaAtEntry(arrTy, nullptr, path + "_arrbrace");
        if (outSlot != nullptr) *outSlot = slot;
        compiler->builder->CreateStore(llvm::Constant::getNullValue(arrTy), slot);

        auto structData = compiler->GetDataStructure(field.TypeName);
        if (positional)
        {
            EmitPositionalFixedArrayIntoSlot(
                path, field, list, slot, false, rejectFrameBoundReferences);
        }
        else if (!emptyList && field.Pointer)
        {
            // 'S*[N] a = {f=v}' - the seed is an 'S' memcpy'd over each POINTER slot, so the
            // field values would become the element addresses.
            LogPointerBraceInitReject(list, std::format("array element of field '{}'", displayPath),
                SpellType(*compiler, field), DescribePointerDeclType(field),
                CanSuggestAllocation(list, field),
                field.IsUnique);
        }
        else if (!emptyList && structData.StructType == nullptr)
        {
            LogErrorContext(list, std::format(
                "array value-initializer '= {{}}' requires a struct element type, '{}' is not a struct",
                SpellType(*compiler, field)));
        }
        else if (!field.Pointer && structData.StructType != nullptr)
        {
            // Value-init: build one default-constructed element, apply the named field
            // overrides onto it, then memcpy that seed into every slot.
            EmitFieldDefaultArraySplat(field, list, slot, arrTy, structData.StructType);
        }
        return compiler->builder->CreateLoad(arrTy, slot, path + "_braceinit");
    }

/*
 * Value-init an array whose element type OWNS a resource, one slot at a time. A single seed
 * memcpy'd over every slot aliases one resource into all N and double-frees at teardown, so
 * each slot gets its own construction and its own copy of the named overrides. Shared by the
 * local declarator arm and the field-default arm; the walk is the one the `= default` arm and
 * the destruction path already use, so the traversals cannot drift.
 */
/*
 * Run the default constructor of one fixed-array element and store it in the slot. A lowered
 * struct (a C++ field that is not trivially relocatable) is constructed in the slot itself through
 * the sret destination, never stored as a relocated value.
 */
static llvm::Value* ConstructDefaultArrayElement(LLVMBackend* compiler, const std::string& typeName,
                                                 llvm::Value* elemPtr, bool forceRoot)
{
        const bool nativeCtorSret = compiler->IsNativeValueConstructor(
            typeName, LLVMBackend::TypeAndValue{ .TypeName = typeName }, {});
        const bool armElemSret = (compiler->IsLoweredCFlatOnlyStruct(typeName)
            && compiler->ReturnsViaInPlaceSret(typeName)) || nativeCtorSret;
        if (armElemSret)
        {
            compiler->pendingCxxSretDest_ = elemPtr;
            compiler->pendingCxxSretTypeName_ = typeName;
            compiler->pendingCxxSretForFixedArray_ = true;
        }
        llvm::Value* elem = compiler->CreateOverloadedFunctionCall(typeName, {}, forceRoot);
        const bool builtInPlace = armElemSret && compiler->pendingCxxSretDest_ == nullptr;
        if (armElemSret)
        {
            compiler->pendingCxxSretDest_ = nullptr;
            compiler->pendingCxxSretTypeName_.clear();
            compiler->pendingCxxSretForFixedArray_ = false;
        }
        // A zero-offset GEP of a global array can fold back to the GlobalVariable; carry the
        // constructor's element type so CreateAssignment does not mistake the slot for the array.
        if (elem != nullptr && !builtInPlace)
        {
            LLVMBackend::NativeBraceScope braceScope(*compiler);
            compiler->CreateAssignment(elem, elemPtr, false, elem->getType());
        }
        return elem;
}

void MainListener::EmitArrayValueInitSlots(
        llvm::Value* arrAlloc,
        llvm::StructType* elemTy,
        const LLVMBackend::TypeAndValue& tv,
        CFlatParser::InitializerListContext* list,
        bool forceRoot,
        antlr4::ParserRuleContext* emptyBraceCtx) {
        auto* compiler = Compiler();
        if (arrAlloc == nullptr || elemTy == nullptr) return;

        // Total slots: the outer extent times every inner dimension (T[N][M] is N*M elements).
        uint64_t n = tv.ConstArraySize;
        for (uint64_t d : tv.ConstInnerDimensions) n *= d;
        if (n == 0) return;

        antlr4::ParserRuleContext* errorContext = list != nullptr ? list : emptyBraceCtx;
        if (errorContext != nullptr && compiler->GetFunction(tv.TypeName) == nullptr
            && HasUninitializedForeignCxxAggregateReferenceMember(tv.TypeName, errorContext))
        {
            LogErrorContext(errorContext, std::format(
                "reference member must be initialized in C++ aggregate array element '{}'",
                tv.TypeName));
            return;
        }

        // `T[N] a = {};` on a C++-class element value-initializes each element, which for a
        // nontrivial default constructor means calling it - not storing N zeroed slots.
        if (compiler->GetFunction(tv.TypeName) == nullptr
            && compiler->CxxElementNeedsDefaultConstruction(tv.TypeName))
        {
            // A list-only class has no default constructor: each empty `{}` element takes the
            // initializer_list constructor with n == 0, as the global and positional forms do.
            const bool emptyList = list == nullptr || list->fieldInit().empty();
            std::string ctorError;
            if (emptyList && compiler->CxxEmptyBraceUsesListCtor(tv.TypeName))
            {
                compiler->EmitArrayConstructionWalk(
                    arrAlloc, elemTy, n, tv.TypeName,
                    [&](llvm::Value* elemPtr)
                    {
                        EmitCxxEmptyBraceConstruct(elemPtr, tv.TypeName, emptyBraceCtx, true);
                    });
            }
            else if (!compiler->EmitCxxArrayDefaultConstruction(
                    tv.TypeName, arrAlloc, elemTy, compiler->builder->getInt64(n), ctorError))
            {
                if (list != nullptr) LogErrorContext(list, ctorError);
                else compiler->LogErrorMessage("{}", { ctorError });
            }
            // A named-field list still applies per element, exactly as before the ctor ran; all
            // N elements are built by then.
            if (list != nullptr && !list->fieldInit().empty())
            {
                LLVMBackend::UnwindPartialScope builtElements(*compiler);
                compiler->NoteUnwindArrayPrefix(arrAlloc, elemTy, compiler->builder->getInt64(n),
                                                tv.TypeName);
                compiler->EmitFixedArrayElementWalk(
                    *compiler->builder, arrAlloc, elemTy, n,
                    [&](llvm::Value* elemPtr) { EmitFieldInitializer(elemPtr, tv.TypeName, list); });
            }
            return;
        }

        compiler->EmitArrayConstructionWalk(
            arrAlloc, elemTy, n, tv.TypeName,
            [&](llvm::Value* elemPtr)
            {
                if (list != nullptr && !list->fieldInit().empty()
                    && EmitFreshBraceInitializeAggregate(elemPtr, tv.TypeName, list, false))
                    return;
                llvm::Value* elem = compiler->GetFunction(tv.TypeName)
                    ? ConstructDefaultArrayElement(compiler, tv.TypeName, elemPtr, forceRoot)
                    : nullptr;
                if (elem == nullptr)
                    compiler->builder->CreateStore(llvm::Constant::getNullValue(elemTy), elemPtr);
                // Built: the named overrides below may throw.
                compiler->NoteUnwindPartial(LLVMBackend::UnwindPartialEntry::Kind::Slot, elemPtr,
                                            tv.TypeName);
                if (list != nullptr && !list->fieldInit().empty())
                    EmitFieldInitializer(elemPtr, tv.TypeName, list);
            });
    }

void MainListener::EmitFieldDefaultArraySplat(
        const LLVMBackend::DeclTypeAndValue& field,
        CFlatParser::InitializerListContext* list,
        llvm::Value* slot,
        llvm::ArrayType* arrTy,
        llvm::StructType* elemTy) {
        auto* compiler = Compiler(list);
        if (elemTy == nullptr) return;

        // A C++ element joins the per-slot path: the seed-and-memcpy arm below neither runs the
        // class's default constructor nor copy-constructs the replicas.
        if (compiler->IsOwningValueType(field.TypeName)
            || compiler->CxxElementNeedsDefaultConstruction(field.TypeName)
            || compiler->HasForeignNontrivialCxxField(field.TypeName))
        {
            EmitArrayValueInitSlots(slot, elemTy, field, list, true, list);
            return;
        }

        auto* seedAlloc = compiler->AllocaAtEntry(elemTy, nullptr, "fieldarrayseed");
        compiler->builder->CreateStore(llvm::Constant::getNullValue(elemTy), seedAlloc);
        // forceRoot: the GetFunction guard is an exact-key lookup, so a namespace walk here
        // would call a same-named sibling type's constructor.
        if (compiler->GetFunction(field.TypeName))
        {
            llvm::Value* seedVal = compiler->CreateOverloadedFunctionCall(field.TypeName, {}, true);
            if (seedVal && seedVal->getType() == elemTy)
                compiler->builder->CreateStore(seedVal, seedAlloc);
        }
        if (!list->fieldInit().empty())
            EmitFieldInitializer(seedAlloc, field.TypeName, list);

        const auto& dl = compiler->module->getDataLayout();
        uint64_t elemBytes = dl.getTypeAllocSize(elemTy);
        llvm::Value* zero = compiler->builder->getInt32(0);
        for (uint64_t i = 0; i < field.ConstArraySize; i++)
        {
            llvm::Value* idx = compiler->builder->getInt32((uint32_t)i);
            auto* elemPtr = compiler->builder->CreateInBoundsGEP(arrTy, slot, { zero, idx }, "arrelem");
            compiler->builder->CreateMemCpy(elemPtr, llvm::MaybeAlign(), seedAlloc, llvm::MaybeAlign(), elemBytes);
        }
    }

void MainListener::RejectCodeValueIntoDataSlot(antlr4::ParserRuleContext* ctx,
                                     const LLVMBackend::NamedVariable& src,
                                     const LLVMBackend::TypeAndValue& dest,
                                     const std::string& role,
                                     const std::string& what) {
        auto* compiler = Compiler(ctx);
        if (!compiler->CodeValueIntoDataDestination(src, dest)) return;
        LogErrorContext(ctx, compiler->DescribeCodeValueIntoData(
            CodeValueDestSpelling(dest), role, CodeValueCastAdvice(dest), what));
    }

bool MainListener::RejectStringLiteralIntoStructPointer(antlr4::ParserRuleContext* ctx,
                                     const LLVMBackend::TypeAndValue& destTV,
                                     llvm::Value* right,
                                     const std::string& destDesc) {
        auto* compiler = Compiler(ctx);
        if (!compiler->IsStringLiteralIntoStructPointer(destTV, right)) return false;
        LogErrorContext(ctx, compiler->DescribeStringLiteralIntoStructPointer(destTV, destDesc));
        return true;
    }

bool MainListener::RejectImplicitPrimitiveToPointer(
    antlr4::ParserRuleContext* ctx, const LLVMBackend::TypeAndValue& target,
    const LLVMBackend::NamedVariable& rhsNV, llvm::Value* value, const std::string& action,
    const std::string& destination) {
        auto* compiler = Compiler(ctx);
        if (!compiler->IsImplicitPrimitiveToPointer(target, rhsNV, value)) return false;
        LogErrorContext(ctx, compiler->DescribeImplicitPrimitiveToPointer(
            target, rhsNV, value, action, destination));
        return true;
    }

void MainListener::RejectNewPointerSlotDepthMismatch(
    antlr4::ParserRuleContext* ctx, const LLVMBackend::NamedVariable& sourceNV,
    const LLVMBackend::TypeAndValue& target) {
        const auto& source = sourceNV.TypeAndValue;
        if (!source.Pointer || !source.ElemPointer || source.IsArrayView || !target.Pointer)
            return;
        auto* compiler = Compiler(ctx);
        if (!sourceNV.IsOwning && !compiler->IsOwnedNewTemp(sourceNV.Primary)) return;
        const int sourceDepth = source.PointerDepth > 0 ? source.PointerDepth : 2;
        const int targetDepth = target.PointerDepth > 0 ? target.PointerDepth
            : (target.ElemPointer ? 2 : 1);
        if (sourceDepth == targetDepth || source.TypeName != target.TypeName) return;
        LogErrorContext(ctx, std::format("cannot convert '{}' to '{}' implicitly: the pointer depths differ",
            SpellType(*compiler, source), SpellType(*compiler, target)));
    }

bool MainListener::RejectConstDiscardingConversion(
    antlr4::ParserRuleContext* ctx, const LLVMBackend::NamedVariable& source, llvm::Value* value,
    const LLVMBackend::TypeAndValue& target) {
        std::string from, to;
        if (!Compiler(ctx)->ConversionDiscardsConst(value, source.TypeAndValue, target, from, to))
            return false;
        LogErrorContext(ctx, std::format("cannot convert '{}' to '{}': the conversion discards const. "
            "Use an explicit cast if the object is really writable", from, to));
        return true;
    }

bool MainListener::RejectImplicitPointerToNumber(
    antlr4::ParserRuleContext* ctx, const LLVMBackend::TypeAndValue& target,
    const LLVMBackend::NamedVariable& rhsNV, llvm::Value* value, const std::string& action,
    const std::string& destination) {
        auto* compiler = Compiler(ctx);
        if (!compiler->IsImplicitPointerToNumber(target, rhsNV, value)) return false;
        LogErrorContext(ctx, compiler->DescribeImplicitPointerToNumber(
            target, rhsNV, value, action, destination));
        return true;
    }

bool MainListener::CanSuggestAllocation(antlr4::ParserRuleContext* ctx, const LLVMBackend::TypeAndValue& tv) {
        return !tv.ElemPointer && tv.TypeName != "void"
            && Compiler(ctx)->GetDataStructure(tv.TypeName).StructType != nullptr;
    }

void MainListener::LogEmptyBraceOnPointerReject(
        antlr4::ParserRuleContext* ctx,
        const std::string& what,
        const LLVMBackend::TypeAndValue& tv) {
        std::string alloc = CanSuggestAllocation(ctx, tv)
            ? std::format(", or 'new {}()' for a pointer to a real object",
                SpellType(*Compiler(ctx), tv))
            : "";
        LogErrorContext(ctx, std::format(
            "empty brace initializer '{{}}' is AMBIGUOUS on the POINTER {} of type '{}' - it could "
            "mean the null pointer, or a pointer to a default-constructed '{}'; write '= default' "
            "(valid for any type, including a generic parameter substituted to a pointer) or "
            "'nullptr'{}",
            what, DescribePointerDeclType(tv), SpellType(*Compiler(ctx), tv), alloc));
    }

bool MainListener::EmitDeferredLoweredMoveIntoField(
        llvm::Value* slot,
        const std::string& typeName,
        const LLVMBackend::NamedVariable& rightNV,
        CFlatParser::MoveExpressionContext* moveSite,
        bool destinationIsFresh,
        const LLVMBackend::OwnedTempMark& ownedTempMark) {
    auto* compiler = Compiler();
    const auto& srcTV = rightNV.TypeAndValue;
    llvm::Value* producedSlot = rightNV.Primary != nullptr
        ? compiler->LoweredSretTempOf(rightNV.Primary, compiler->GetType(srcTV)) : nullptr;
    const bool moveCallTemp = moveSite != nullptr && producedSlot != nullptr
        && (rightNV.Storage == nullptr || rightNV.Storage == producedSlot);
    const bool namedMove = srcTV.IsMove && rightNV.Storage != nullptr && !moveCallTemp;
    if (slot == nullptr || (!moveCallTemp && !namedMove)
        || (rightNV.Storage != nullptr && rightNV.Storage == slot)
        || srcTV.Pointer || srcTV.ElemPointer || srcTV.IsArrayView || srcTV.ConstArraySize != 0
        || compiler->ResolveTypeAlias(srcTV.TypeName) != compiler->ResolveTypeAlias(typeName))
        return false;
    // An update replaces the seeded value, so its destructor runs first (as in the element path).
    if (!destinationIsFresh)
        if (auto* dtor = compiler->GetOrCreateFullDestructor(typeName))
            compiler->builder->CreateCall(dtor->getFunctionType(), dtor, { slot });
    // Re-arm the moved-from source first, so a throwing move ctor still unwinds it.
    if (namedMove)
    {
        const std::string movedName = compiler->FindVariableNameByStorage(rightNV.Storage);
        if (!movedName.empty()) compiler->MarkVariableNotExplicitlyMovedNull(movedName);
        compiler->EmitCxxCopyOrMoveConstruct(typeName, slot, rightNV.Storage,
            /*useMove*/ true, "into a struct field");
    }
    else
    {
        auto sourceNV = rightNV;
        sourceNV.TypeAndValue.IsMove = true;
        sourceNV.IsExplicitMove = true;
        sourceNV.IsRvalue = true;
        auto* sourceValue = sourceNV.Primary != nullptr ? sourceNV.Primary : LoadNamedVariable(sourceNV);
        if (sourceValue == nullptr || !EmitLoweredValueIntoSlot(compiler, typeName, slot,
                sourceNV, sourceValue, moveSite, "into a struct field"))
            return false;
    }
    compiler->FlushOwnedTempsSince(ownedTempMark, nullptr, nullptr);
    if (destinationIsFresh && compiler->HasNonTrivialDestructor(typeName))
        compiler->NoteUnwindPartial(LLVMBackend::UnwindPartialEntry::Kind::Slot, slot, typeName);
    return true;
}

void MainListener::EmitFieldInitializer(
        llvm::Value* structPtr,
        const std::string& typeName,
        CFlatParser::InitializerListContext* ctx,
        bool destinationIsFresh,
        const std::string* onlyField,
        bool deferForeignReplacementDrop,
        llvm::Value* completedObjectSlot,
        const std::string* completedObjectType) {
    auto* compiler = Compiler(ctx);
        bool completedObjectActive = completedObjectSlot != nullptr
            && completedObjectType != nullptr;
        bool completedObjectSuspended = false;
        size_t completedObjectUnwindMark = 0;
        auto suspendCompletedObject = [&](const std::string* replacedField,
                                           const std::string* retainedField = nullptr,
                                           llvm::Value* retainedSlot = nullptr) {
            if (!completedObjectActive) return;
            auto& entries = compiler->unwindPartial_;
            for (auto entry = entries.rbegin(); entry != entries.rend(); ++entry)
                if (entry->K == LLVMBackend::UnwindPartialEntry::Kind::Slot
                    && entry->V == completedObjectSlot
                    && entry->TypeName == *completedObjectType)
                {
                    entries.erase(std::next(entry).base());
                    completedObjectActive = false;
                    completedObjectSuspended = true;
                    completedObjectUnwindMark = entries.size();
                    auto objectIt = compiler->dataStructures.find(*completedObjectType);
                    if (objectIt == compiler->dataStructures.end() || objectIt->second.IsUnion)
                        return;
                    const auto& objectData = objectIt->second;
                    for (unsigned i = 0; i < objectData.StructFields.size(); ++i)
                    {
                        const auto& field = objectData.StructFields[i];
                        if (replacedField != nullptr && field.VariableName == *replacedField)
                            continue;
                        if (field.IsAlias || field.Pointer || field.ElemPointer
                            || field.IsArrayView || field.IsSimd || field.IsBitfield
                            || field.IsPadding || field.IsCflatOwned)
                            continue;
                        auto* fieldSlot = retainedField != nullptr
                                && field.VariableName == *retainedField && retainedSlot != nullptr
                            ? retainedSlot
                            : compiler->CreateCxxFieldGEP(
                                objectData, completedObjectSlot, i, field.VariableName + "_unwind");
                        if (field.ConstArraySize > 0)
                        {
                            auto* fieldTy = compiler->GetType(field);
                            llvm::Type* elemTy = nullptr;
                            const uint64_t count = LLVMBackend::PeelFixedArrayType(fieldTy, elemTy);
                            if (count > 0 && elemTy != nullptr)
                                compiler->NoteUnwindArrayPrefix(fieldSlot, elemTy,
                                    compiler->builder->getInt64(count), field.TypeName);
                        }
                        else
                            compiler->NoteUnwindPartial(
                                LLVMBackend::UnwindPartialEntry::Kind::Slot, fieldSlot, field.TypeName);
                    }
                    return;
                }
        };
        auto resumeCompletedObject = [&]() {
            if (!completedObjectSuspended) return;
            auto& entries = compiler->unwindPartial_;
            if (entries.size() > completedObjectUnwindMark)
                entries.resize(completedObjectUnwindMark);
            compiler->NoteUnwindPartial(LLVMBackend::UnwindPartialEntry::Kind::Slot,
                                        completedObjectSlot, *completedObjectType);
            completedObjectActive = true;
            completedObjectSuspended = false;
        };
    auto spellTypeName = [&](const std::string& name) {
        return SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = name });
    };
    auto it = compiler->dataStructures.find(typeName);
        if (it == compiler->dataStructures.end())
        {
            LogErrorContext(ctx, std::format("Field initializer: '{}' is not a known struct type",
                spellTypeName(typeName)));
            return;
        }
        const auto& sd = it->second;

        std::unordered_set<std::string> promotedFieldInitializers;
        for (auto* fi : ctx->fieldInit())
        {
            if (fi->Identifier() == nullptr) continue;
            const std::string fieldName = fi->Identifier()->getText();
            if (onlyField != nullptr && fieldName != *onlyField) continue;
            const bool directField = std::any_of(sd.StructFields.begin(), sd.StructFields.end(),
                [&](const auto& field) { return field.VariableName == fieldName; });
            if (directField) continue;

            std::vector<unsigned> path;
            if (!FindTransparentAnonFieldPath(typeName, fieldName, path) || path.empty()) continue;
            const unsigned anonymousIndex = path.front();
            if (anonymousIndex >= sd.StructFields.size()) continue;
            const auto& anonymousField = sd.StructFields[anonymousIndex];
            llvm::Value* nestedDestination = sd.IsUnion ? structPtr
                : compiler->CreateCxxFieldGEP(sd, structPtr, anonymousIndex,
                    anonymousField.VariableName + "_designated_init");
            promotedFieldInitializers.insert(fieldName);
            EmitFieldInitializer(nestedDestination, anonymousField.TypeName, ctx,
                destinationIsFresh, &fieldName, deferForeignReplacementDrop);
        }

        std::unordered_set<std::string> seen;
        for (auto* fi : ctx->fieldInit())
        {
            if (onlyField != nullptr
                && (fi->Identifier() == nullptr || fi->Identifier()->getText() != *onlyField))
                continue;
            // Struct field initializers are named-only ({field = value}). Positional
            // ({value}) and key:value ({k: v}) forms are reserved for array/list/dictionary
            // targets and are routed away before reaching here; reject them for structs.
            if (fi->Identifier() == nullptr)
            {
                LogErrorContext(fi, std::format(
                    "positional initializers are not supported for struct type '{}'; use 'field = value'",
                    spellTypeName(typeName)));
                continue;
            }
            std::string fieldName = fi->Identifier()->getText();
            if (promotedFieldInitializers.count(fieldName) != 0) continue;
            if (!seen.insert(fieldName).second)
            {
                LogErrorContext(fi, std::format("Duplicate field initializer for '{}'", fieldName));
                continue;
            }

            // `field = {}`: no initializerList and no expression, so it gets its own arm. Like clang,
            // a C++ class is value-initialized in place; other fields take their default value.
            if (fi->initializerList() == nullptr && fi->assignmentExpression().empty())
            {
                // Bitfields are not in StructFields: clear the window in the storage word (value 0).
                if (auto emptyBit = std::find_if(sd.Bitfields.begin(), sd.Bitfields.end(),
                        [&](const auto& b) { return b.Name == fieldName; });
                    emptyBit != sd.Bitfields.end())
                {
                    const auto& storageField = sd.StructFields[emptyBit->StorageFieldIndex];
                    auto* storageTy = compiler->GetType(storageField);
                    auto* wordTy = compiler->GetBitfieldWordType(storageTy);
                    auto* storagePtr = sd.IsUnion ? structPtr : compiler->builder->CreateStructGEP(
                        sd.StructType, structPtr, emptyBit->StorageFieldIndex, fieldName + "_bf_empty");
                    const unsigned w = emptyBit->BitWidth;
                    const uint64_t valMask = (w == 64) ? ~uint64_t(0) : ((uint64_t(1) << w) - 1);
                    const uint64_t windowMask = valMask << emptyBit->BitOffset;
                    auto* word = compiler->LoadBitfieldWord(storagePtr, storageTy);
                    auto* cleared = compiler->builder->CreateAnd(word,
                        cflat_llvm::GetIntTruncated(wordTy, ~windowMask));
                    compiler->StoreBitfieldWord(storagePtr, storageTy, cleared);
                    continue;
                }
                int emptyFieldIndex = -1;
                for (int i = 0; i < (int)sd.StructFields.size(); ++i)
                    if (sd.StructFields[i].VariableName == fieldName)
                    {
                        emptyFieldIndex = i;
                        break;
                    }
                if (emptyFieldIndex < 0)
                {
                    LogErrorContext(fi, std::format("'{}' has no field named '{}'",
                        spellTypeName(typeName), fieldName));
                    continue;
                }
                const auto& emptyField = sd.StructFields[emptyFieldIndex];
                const std::string emptyFieldPath = std::format("'{}.{}'",
                    spellTypeName(typeName), fieldName);
                llvm::Value* emptyDestination = sd.IsUnion ? structPtr
                    : compiler->CreateCxxFieldGEP(sd, structPtr, (unsigned)emptyFieldIndex,
                                                   fieldName + "_empty_init");
                const bool emptyCxxClass = !emptyField.Pointer && !emptyField.ElemPointer
                    && !emptyField.IsAlias && !emptyField.IsArrayView
                    && emptyField.ConstArraySize == 0 && emptyField.ConstInnerDimensions.empty()
                    && compiler->IsCxxRecord(emptyField.TypeName);
                if (emptyCxxClass)
                {
                    // Copy-list-initialization of the member: an explicit constructor is refused.
                    if (!EmitCxxEmptyBraceConstruct(emptyDestination, emptyField.TypeName, fi, true))
                        continue;
                    if (destinationIsFresh)
                        compiler->NoteUnwindPartial(LLVMBackend::UnwindPartialEntry::Kind::Slot,
                                                    emptyDestination, emptyField.TypeName);
                    continue;
                }
                // A CFlat struct with a C++ member lacking a usable default: refuse like an omitted field.
                std::string unavailableType;
                if (!emptyField.Pointer && !emptyField.ElemPointer && !emptyField.IsAlias
                    && !emptyField.IsArrayView
                    && compiler->GetDataStructure(emptyField.TypeName).StructType != nullptr
                    && compiler->HasUnavailableNestedCxxDefault(emptyField.TypeName, &unavailableType))
                {
                    const auto* info = compiler->GetCxxClassInfo(unavailableType);
                    LogErrorContext(fi, std::format(
                        "C++ class '{}' has no default constructor cflat can call{}",
                        compiler->DisplayCxxClassName(unavailableType),
                        info != nullptr && info->hasDeletedDefaultCtor ? " (it is deleted)" : ""));
                    continue;
                }
                if (emptyField.ConstArraySize > 0 && !emptyField.Pointer && !emptyField.ElemPointer
                    && !emptyField.IsAlias && !emptyField.IsArrayView)
                {
                    // Build each slot in place: a temp copy would leave self-pointers at a dead address.
                    if (compiler->IsCxxRecord(emptyField.TypeName) && emptyField.ConstInnerDimensions.empty())
                    {
                        auto* arrTy = llvm::dyn_cast_or_null<llvm::ArrayType>(compiler->GetType(emptyField));
                        bool built = arrTy != nullptr;
                        for (uint64_t slot = 0; built && slot < arrTy->getNumElements(); ++slot)
                        {
                            auto* slotPtr = compiler->builder->CreateConstInBoundsGEP2_64(
                                arrTy, emptyDestination, 0, slot);
                            built = EmitCxxEmptyBraceConstruct(slotPtr, emptyField.TypeName, fi, true);
                            if (built && destinationIsFresh)
                                compiler->NoteUnwindPartial(LLVMBackend::UnwindPartialEntry::Kind::Slot,
                                                            slotPtr, emptyField.TypeName);
                        }
                        if (!built)
                            continue;
                    }
                    else
                    {
                        GlobalScopeGuard emptyFieldScope(global_scope);
                        EmitFixedArrayDefaultInit(emptyDestination, emptyField, fi);
                        llvm::Type* elemTy = nullptr;
                        const uint64_t count = compiler->PeelFixedArrayType(
                            compiler->GetType(emptyField), elemTy);
                        if (destinationIsFresh)
                            compiler->NoteUnwindArrayPrefix(emptyDestination, elemTy,
                                compiler->builder->getInt64(count), emptyField.TypeName);
                    }
                    continue;
                }
                llvm::Value* emptyValue = nullptr;
                {
                    GlobalScopeGuard emptyFieldScope(global_scope);
                    emptyValue = GenerateDefaultValue(emptyField);
                }
                if (emptyValue == nullptr)
                {
                    LogErrorContext(fi, std::format(
                        "empty brace initializer is not supported for field {} of type '{}'",
                        emptyFieldPath, spellTypeName(emptyField.TypeName)));
                    continue;
                }
                compiler->builder->CreateStore(emptyValue, emptyDestination);
                continue;
            }

            if (auto* nested = fi->initializerList())
            {
                int nestedFieldIndex = -1;
                for (int i = 0; i < (int)sd.StructFields.size(); ++i)
                    if (sd.StructFields[i].VariableName == fieldName)
                    {
                        nestedFieldIndex = i;
                        break;
                    }
                if (nestedFieldIndex < 0)
                {
                    LogErrorContext(fi, std::format("'{}' has no field named '{}'",
                        spellTypeName(typeName), fieldName));
                    continue;
                }

                const auto& nestedField = sd.StructFields[nestedFieldIndex];
                std::string nestedFieldPath = std::format("'{}.{}'", spellTypeName(typeName), fieldName);
                if (nestedField.IsBitfield)
                {
                    LogErrorContext(fi, std::format(
                        "nested named brace initializer is not supported for bitfield {}", nestedFieldPath));
                    continue;
                }
                if (nestedField.Pointer)
                {
                    LogErrorContext(fi, std::format(
                        "nested named brace initializer is not supported for pointer field {}", nestedFieldPath));
                    continue;
                }
                if (nestedField.IsArrayView)
                {
                    LogErrorContext(fi, std::format(
                        "nested named brace initializer is not supported for array-view field {}", nestedFieldPath));
                    continue;
                }

                llvm::Value* nestedDestination = sd.IsUnion
                    ? structPtr
                    : compiler->CreateCxxFieldGEP(sd, structPtr,
                        (unsigned)nestedFieldIndex, fieldName + "_nested_init");
                if (nestedField.ConstArraySize > 0)
                {
                    const bool foreignCxxArray = !nestedField.Pointer && !nestedField.ElemPointer
                        && !nestedField.IsArrayView
                        && compiler->IsForeignNontrivialCxxClass(nestedField.TypeName);
                    bool positionalArray = false;
                    bool allPositionalElements = true;
                    for (auto* nestedInit : nested->fieldInit())
                    {
                        if (nestedInit->Identifier() == nullptr
                            && (nestedInit->assignmentExpression().size() == 1
                                || FieldInitIsBraceElement(nestedInit)))
                            positionalArray = true;
                        else
                            allPositionalElements = false;
                    }
                    const bool constructedCflatElements = !nestedField.Pointer
                        && !nestedField.ElemPointer && !nestedField.IsArrayView
                        && !compiler->IsCxxRecord(nestedField.TypeName)
                        && CflatStructNeedsCtorConstruction(nestedField.TypeName);
                    if (constructedCflatElements && positionalArray && allPositionalElements)
                    {
                        // Existing aggregates replace live elements; fresh ones construct each
                        // selected element directly and default only the unlisted tail.
                        EmitPositionalFixedArrayIntoSlot(
                            std::format("{}.{}", typeName, fieldName),
                            nestedField, nested, nestedDestination,
                            /*updateExistingElements*/ !destinationIsFresh);
                        continue;
                    }
                    if (foreignCxxArray && positionalArray)
                    {
                        // A fresh aggregate skipped the field's declaration initializer, so its
                        // array elements are not constructed yet. Reassignment still has to
                        // destroy the existing elements before the positional values replace them.
                        if (destinationIsFresh || EmitForeignCxxFixedArrayIntoSlot(
                                nestedField, nestedDestination, nullptr,
                                "into a fixed-array field", fi))
                            EmitPositionalFixedArrayIntoSlot(
                                std::format("{}.{}", typeName, fieldName),
                                nestedField, nested, nestedDestination);
                        continue;
                    }
                    llvm::Value* nestedSlot = nullptr;
                    auto nestedValue = EmitFieldDefaultFixedArrayBrace(
                        typeName, nestedField, nested, &nestedSlot);
                    if (nestedValue != nullptr)
                    {
                        if (!foreignCxxArray
                            || !EmitForeignCxxFixedArrayIntoSlot(
                                nestedField, nestedDestination, nestedSlot,
                                "into a fixed-array field", fi))
                        {
                            LLVMBackend::NativeBraceScope braceScope(*compiler);
                            compiler->CreateAssignment(nestedValue, nestedDestination, false,
                                compiler->GetType(nestedField));
                        }
                    }
                    continue;
                }

                if (!nestedField.Pointer && !nestedField.ElemPointer
                    && !nestedField.IsAlias && !nestedField.IsArrayView
                    && compiler->IsForeignCxxAggregateClass(nestedField.TypeName)
                    && HasForeignCxxAggregateReferenceMember(nestedField.TypeName, fi))
                {
                    suspendCompletedObject(nullptr, &fieldName, nestedDestination);
                    TryEmitForeignCxxAggregateBraceInit(
                        nestedField.TypeName, nested, fi, nestedDestination, true,
                        false, nullptr, false);
                    if (compiler->HasNonTrivialDestructor(nestedField.TypeName))
                        compiler->NoteUnwindPartial(
                            LLVMBackend::UnwindPartialEntry::Kind::Slot,
                            nestedDestination, nestedField.TypeName);
                    resumeCompletedObject();
                    continue;
                }

                bool isContainer = MangledBase(nestedField.TypeName) == "list"
                    || MangledBase(nestedField.TypeName) == "array"
                    || MangledBase(nestedField.TypeName) == "dictionary";
                if (isContainer)
                {
                    TryEmitContainerInitializer(nestedDestination, nestedField, nested);
                    continue;
                }

                auto nestedData = compiler->GetDataStructure(nestedField.TypeName);
                if (nestedData.StructType != nullptr)
                {
                    const bool nestedFresh = destinationIsFresh
                        && !compiler->IsCxxRecord(nestedField.TypeName)
                        && explicitNoArgConstructors_.count(nestedField.TypeName) == 0;
                    suspendCompletedObject(nullptr, &fieldName, nestedDestination);
                    const bool builtFresh = nestedFresh && EmitFreshBraceInitializeAggregate(
                        nestedDestination, nestedField.TypeName, nested, true);
                    if (builtFresh)
                        compiler->NoteUnwindPartial(LLVMBackend::UnwindPartialEntry::Kind::Slot,
                                                    nestedDestination, nestedField.TypeName);
                    else
                        EmitFieldInitializer(nestedDestination, nestedField.TypeName, nested,
                                             nestedFresh, nullptr, false,
                                             nestedDestination, &nestedField.TypeName);
                    resumeCompletedObject();
                    continue;
                }

                LogErrorContext(fi, std::format(
                    "nested named brace initializer requires a struct or container field {}", nestedFieldPath));
                continue;
            }

            // Occurrence-scope this ONE field initializer (see codeValueDataCasts_): a brace list
            // evaluates multiple fields in one statement, the same collision shape as call args.
            size_t savedCastOcc = compiler->BeginCastOccurrence();
            size_t thisCastOcc = compiler->CurrentCastOccurrence();

            // Bitfield seeded-init: `Point p = {ready: 1}`. Resolve through the
            // side-table; emit a RMW into the storage slot. No address-of, no GEP
            // of the field name itself.
            const LLVMBackend::BitfieldInfo* bfHit = nullptr;
            for (const auto& b : sd.Bitfields)
                if (b.Name == fieldName) { bfHit = &b; break; }
            if (bfHit)
            {
                auto rightNV = ParseAssignmentExpressionNamed(fi->assignmentExpression(0));
                llvm::Value* val = LoadNamedVariable(rightNV);
                compiler->EndCastOccurrence(savedCastOcc);
                if (!val) continue;

                const auto& storageField = sd.StructFields[bfHit->StorageFieldIndex];
                auto* storageTy = compiler->GetType(storageField);
                auto* wordTy = compiler->GetBitfieldWordType(storageTy);
                // Union bitfield storage words all alias the union's offset 0.
                auto* storagePtr = sd.IsUnion ? structPtr : compiler->builder->CreateStructGEP(
                    sd.StructType, structPtr, bfHit->StorageFieldIndex, fieldName + "_bf");

                unsigned w = bfHit->BitWidth;
                unsigned off = bfHit->BitOffset;
                uint64_t valMask = (w == 64) ? ~uint64_t(0) : ((uint64_t(1) << w) - 1);
                uint64_t windowMask = valMask << off;
                llvm::Value* valAsStorage = nullptr;
                if (bfHit->TypeName == "bool")
                    valAsStorage = compiler->builder->CreateZExt(compiler->CoerceToBoolCondition(val), wordTy);
                else
                {
                    // Brace constants are range-checked against the DECLARED type, not the width.
                    LLVMBackend::TypeAndValue declared{ .TypeName = bfHit->TypeName };
                    if (llvm::Type* declaredLLVM = compiler->GetType(declared);
                        declaredLLVM != nullptr && declaredLLVM->isIntegerTy())
                    {
                        LLVMBackend::NativeBraceScope braceScope(*compiler);
                        compiler->RefuseNarrowingBraceConstant(rightNV.Primary,
                            rightNV.TypeAndValue.IsUnsignedInteger() != -1, declaredLLVM, bfHit->IsUnsigned);
                    }
                    compiler->RefuseImplicitFloatToInt(val, wordTy, bfHit->IsUnsigned);
                    valAsStorage = compiler->CreateCast(val, wordTy, !bfHit->IsUnsigned, bfHit->IsUnsigned);
                }
                auto* valMasked = compiler->builder->CreateAnd(valAsStorage, cflat_llvm::GetIntTruncated(wordTy, valMask));
                auto* valShifted = compiler->builder->CreateShl(valMasked, cflat_llvm::GetIntTruncated(wordTy, off));
                auto* word = compiler->LoadBitfieldWord(storagePtr, storageTy);
                auto* cleared = compiler->builder->CreateAnd(word, cflat_llvm::GetIntTruncated(wordTy, ~windowMask));
                auto* newWord = compiler->builder->CreateOr(cleared, valShifted);
                compiler->StoreBitfieldWord(storagePtr, storageTy, newWord);
                continue;
            }

            // Thread the target field's closure signature into a lambda RHS so its return type is
            // inferred as `s.f = (int n) => ...` does. Spelled and ENCODED field types both.
            std::optional<DeclExpectedTypeScope> fieldExpectedScope;
            std::string stdFunctionField;
            const LLVMBackend::DeclTypeAndValue* initField = nullptr;
            unsigned initFieldIndex = 0;
            for (unsigned i = 0; i < sd.StructFields.size(); ++i)
            {
                const auto& field = sd.StructFields[i];
                if (field.VariableName != fieldName) continue;
                initField = &field;
                initFieldIndex = i;
                if (field.IsFunctionPointer) lambdaExpectedType = field;
                else if (const auto* enc = compiler->GetEncodedClosureType(field.TypeName))
                    lambdaExpectedType = *enc;
                else if (const auto* signature = compiler->StdFunctionClosureSignature(field.TypeName);
                         signature != nullptr && !field.Pointer)
                {
                    lambdaExpectedType = *signature;
                    stdFunctionField = field.TypeName;
                }
                // The FIELD's type is the context for an immediately-invoked literal in its
                // initializer, the same way a declarator's type is.
                fieldExpectedScope.emplace(&declExpectedType, field, true);
                if (field.AllocAlignValue > LLVMBackend::kDefaultNewAlign
                    && AsDirectNew(fi->assignmentExpression(0)) != nullptr)
                    compiler->pendingInitAllocAlign = field.AllocAlignValue;
                break;
            }
            if (initField != nullptr && !initField->Pointer
                && compiler->ResolveTypeAlias(initField->TypeName) == "string"
                && IsNullptrLiteralExpressionTree(fi->assignmentExpression(0)))
                LogErrorContext(fi->assignmentExpression(0), std::format(
                    "cannot initialize string '{}' from nullptr", fieldName));
            llvm::Value* fieldDestination = nullptr;
            bool armFieldSret = false;
            if (destinationIsFresh && initField != nullptr && !initField->Pointer
                && !initField->IsAlias && !initField->IsArrayView
                && initField->ConstArraySize == 0
                && (compiler->ReturnsViaInPlaceSret(initField->TypeName)
                    || compiler->IsNativeValueConstructor(initField->TypeName,
                        LLVMBackend::TypeAndValue{ .TypeName = initField->TypeName }, {}))
                && FieldInitializerIsSingleCall(fi->assignmentExpression(0)))
            {
                fieldDestination = sd.IsUnion ? structPtr
                    : compiler->CreateCxxFieldGEP(sd, structPtr, initFieldIndex,
                                                   fieldName + "_sret_init");
                compiler->pendingCxxSretDest_ = fieldDestination;
                compiler->pendingCxxSretTypeName_ = initField->TypeName;
                compiler->pendingCxxSretReturn_ = true;
                compiler->pendingCxxSretForFixedArray_ = true;
                armFieldSret = true;
            }
            // A fresh C++ class or [immovable] field initialized by a `?:` arms the field slot like a declaration:
            // each selected arm is built in place (C++17 guaranteed elision), never joined and moved.
            const bool armFieldTernary = !armFieldSret && destinationIsFresh && !sd.IsUnion
                && initField != nullptr && !initField->Pointer && !initField->IsAlias
                && !initField->IsArrayView && initField->ConstArraySize == 0
                && (compiler->IsForeignNontrivialCxxClass(initField->TypeName)
                    || compiler->IsImmovableCFlatStruct(initField->TypeName))
                && SoleTernaryExpression(fi->assignmentExpression(0)) != nullptr;
            if (armFieldTernary)
            {
                fieldDestination = compiler->CreateCxxFieldGEP(sd, structPtr, initFieldIndex,
                                                               fieldName + "_ternary_init");
                compiler->pendingCxxTernaryDeclDest_ = fieldDestination;
                compiler->pendingCxxTernaryDeclTypeName_ = initField->TypeName;
                compiler->pendingCxxTernaryDeclConsumed_ = false;
                compiler->pendingCxxTernaryDeclFailed_ = false;
            }
            auto ownedTempMark = compiler->MarkOwnedTemps();
            compiler->lastCxxRetTemp_ = nullptr;
            compiler->lastCxxRetValue_ = nullptr;
            // `f = move x` into a lowered CFlat struct with C++ members: the move is deferred to a
            // member-wise construct into the slot (T258, the named twin of the T253 element path).
            auto* fieldMoveExpr = TopLevelMoveExpression(fi->assignmentExpression(0));
            const bool loweredMoveField = fieldMoveExpr != nullptr && initField != nullptr
                && !sd.IsUnion && IsLoweredMoveFieldTarget(compiler, *initField);
            llvm::Value* loweredMoveSlot = loweredMoveField
                ? compiler->CreateCxxFieldGEP(sd, structPtr, initFieldIndex, fieldName + "_move_init")
                : nullptr;
            LLVMBackend::NamedVariable rightNV;
            if (loweredMoveField)
            {
                LoweredBraceMoveScope deferMove(*this, fieldMoveExpr, initField->TypeName, loweredMoveSlot);
                rightNV = ParseAssignmentExpressionNamed(fi->assignmentExpression(0));
            }
            else
                rightNV = ParseAssignmentExpressionNamed(fi->assignmentExpression(0));
            if (initField != nullptr)
                GuardRawPointerIntoOwningTempField(
                    rightNV, *initField, true, initField->IsAlias, fieldName, fi);
            const bool initializedInPlace = armFieldSret
                && compiler->pendingCxxSretDest_ == nullptr;
            if (armFieldSret)
            {
                compiler->pendingCxxSretDest_ = nullptr;
                compiler->pendingCxxSretTypeName_.clear();
                compiler->pendingCxxSretReturn_ = false;
                compiler->pendingCxxSretForFixedArray_ = false;
            }
            if (armFieldTernary)
            {
                const bool ternaryConsumed = compiler->pendingCxxTernaryDeclConsumed_;
                const bool ternaryFailed = compiler->pendingCxxTernaryDeclFailed_;
                compiler->pendingCxxTernaryDeclDest_ = nullptr;
                compiler->pendingCxxTernaryDeclTypeName_.clear();
                compiler->pendingCxxTernaryDeclConsumed_ = false;
                compiler->pendingCxxTernaryDeclFailed_ = false;
                if (ternaryFailed)
                {
                    compiler->lastCxxRetTemp_ = nullptr;
                    compiler->lastCxxRetValue_ = nullptr;
                    if (compiler->IsImmovableCFlatStruct(initField->TypeName))
                    {
                        LogErrorContext(fi, std::format(
                            "cannot initialize field '{}' from a value of type '{}': the type is [immovable], so the "
                            "value may not be copied or moved into the aggregate (initialize the field in place)",
                            fieldName, SpellDiagnosticType(*compiler, *initField)));
                        compiler->EndCastOccurrence(savedCastOcc);
                        continue;
                    }
                    LogErrorContext(fi, std::format(
                        "cannot initialize C++ class '{}' from this expression; use '{}(args)', a '{}' "
                        "lvalue, or 'move <{}> lvalue' in each arm",
                        compiler->DisplayCxxClassName(initField->TypeName),
                        compiler->DisplayCxxClassName(initField->TypeName),
                        compiler->DisplayCxxClassName(initField->TypeName),
                        compiler->DisplayCxxClassName(initField->TypeName)));
                    compiler->EndCastOccurrence(savedCastOcc);
                    continue;
                }
                if (ternaryConsumed)
                {
                    compiler->lastCxxRetTemp_ = nullptr;
                    compiler->lastCxxRetValue_ = nullptr;
                    compiler->EndCastOccurrence(savedCastOcc);
                    compiler->NoteUnwindPartial(LLVMBackend::UnwindPartialEntry::Kind::Slot,
                                                fieldDestination, initField->TypeName);
                    continue;
                }
            }
            if (initializedInPlace)
            {
                compiler->EndCastOccurrence(savedCastOcc);
                compiler->NoteUnwindPartial(LLVMBackend::UnwindPartialEntry::Kind::Slot,
                                            fieldDestination, initField->TypeName);
                continue;
            }
            // [immovable]: a brace-init field copies or moves its source into the new aggregate. A
            // borrowed field (pointer or alias) keeps the source in place; a fresh value is built in place.
            const auto declField = std::find_if(sd.StructFields.begin(), sd.StructFields.end(),
                [&](const auto& f) { return f.VariableName == fieldName; });
            if (declField != sd.StructFields.end() && !declField->Pointer && !declField->IsAlias
                && !rightNV.TypeAndValue.Pointer
                && !IsDefaultOnlyExpression(fi->assignmentExpression(0))
                && (rightNV.IsExplicitMove || !compiler->IsImmovableFreshOperand(rightNV))
                && compiler->IsImmovableType(rightNV.TypeAndValue.TypeName))
            {
                LogErrorContext(fi, std::format(
                    "cannot initialize field '{}' from a value of type '{}': the type is [immovable], so the "
                    "value may not be copied or moved into the aggregate (initialize the field in place)",
                    fieldName, SpellDiagnosticType(*compiler, rightNV.TypeAndValue)));
            }
            bool emittedLoweredMove = false;
            if (loweredMoveField)
            {
                suspendCompletedObject(&fieldName);
                emittedLoweredMove = EmitDeferredLoweredMoveIntoField(
                    loweredMoveSlot, initField->TypeName, rightNV, fieldMoveExpr,
                    destinationIsFresh, ownedTempMark);
                resumeCompletedObject();
            }
            if (emittedLoweredMove)
            {
                compiler->EndCastOccurrence(savedCastOcc);
                continue;
            }
            // A bare named function binds its function-pointer field through the same by-name
            // rule as a declaration (GetFunctionForFuncPtr refuses what cannot bind).
            if (initField != nullptr && initField->IsFunctionPointer
                && llvm::isa_and_nonnull<llvm::Function>(rightNV.Primary) && !rightNV.CallerName.empty())
            {
                if (auto* correctFn = compiler->GetFunctionForFuncPtr(rightNV.CallerName,
                        (int)initField->FuncPtrParams.size(), &initField->FuncPtrParams,
                        initField, /*compareReturnOwnership*/ true))
                    rightNV.Primary = correctFn;
            }
            // Variadic-ness and the bind rule are part of a function-pointer field's type, as for a declaration.
            // A generic-substituted closure field carries an encoded type name; its signature is the encoded one.
            LLVMBackend::TypeAndValue initSlotSig;
            if (initField != nullptr) initSlotSig = *initField;
            if (initField != nullptr && !initSlotSig.IsFunctionPointer && !initSlotSig.Pointer)
                if (const auto* enc = compiler->GetEncodedClosureType(initSlotSig.TypeName))
                    initSlotSig = *enc;
            LLVMBackend::TypeAndValue initSourceSig = rightNV.TypeAndValue;
            if (initSourceSig.FuncPtrReturnTypeName.empty() && !initSourceSig.Pointer)
                if (const auto* enc = compiler->GetEncodedClosureType(initSourceSig.TypeName))
                    initSourceSig = *enc;
            const bool initSourceIsNamedFunction =
                llvm::isa_and_nonnull<llvm::Function>(rightNV.Primary);
            if (initField != nullptr && initSlotSig.IsFunctionPointer
                && (initSourceSig.IsFunctionPointer || initSourceIsNamedFunction))
            {
                if (std::string why = compiler->FuncPtrValueOwnershipReason(
                        rightNV, initSlotSig); !why.empty())
                    LogErrorContext(fi, std::format(
                        "incompatible function pointer initializer: {}", why));
                if (initSourceSig.IsFunctionPointer)
                {
                    if (compiler->ClosureValueCanAdaptToSlot(rightNV, initSlotSig))
                        compiler->AdaptClosureValueToSlot(
                            LoadNamedVariable(rightNV), rightNV, initSlotSig);
                    if (std::string why = compiler->DescribeFuncPtrValueMismatch(
                            initSlotSig, rightNV); !why.empty())
                        LogErrorContext(fi, why);
                    else if (std::string constWhy = compiler->DescribeFuncPtrConstMismatch(
                            initSlotSig, rightNV); !constWhy.empty())
                        LogErrorContext(fi, constWhy);
                }
            }
            lambdaExpectedType = {};
            if (!stdFunctionField.empty())
            {
                // A std.function field takes a lambda through its function-pointer constructor.
                compiler->LowerNonCapturingClosureForCxx(
                    rightNV, compiler->StdFunctionClosureSignature(stdFunctionField));
                if (std::string refusal = compiler->CxxClosureArgumentRefusal(
                        rightNV, compiler->DisplayCxxClassName(stdFunctionField),
                        compiler->StdFunctionClosureSignature(stdFunctionField));
                    !refusal.empty())
                    LogErrorContext(fi, refusal);
            }
            rightNV.CastOccurrenceId = thisCastOcc;
            compiler->EndCastOccurrence(savedCastOcc);
            suspendCompletedObject(&fieldName);
            EmitOneFieldInit(structPtr, sd, typeName, fieldName, rightNV, ownedTempMark, fi,
                             destinationIsFresh, deferForeignReplacementDrop);
            resumeCompletedObject();
        }
    }

void MainListener::EmitConstructedFieldOverrides(
        llvm::Value* structPtr,
        const std::string& typeName,
        CFlatParser::InitializerListContext* ctx,
        bool retainUnwind,
        std::vector<std::pair<llvm::Value*, std::string>>* retainedUnwindSlots) {
        auto* compiler = Compiler(ctx);
        if (compiler->HasNonTrivialDestructor(typeName))
        {
            // The default constructor completed, so a later override may unwind through the full object.
            std::optional<LLVMBackend::UnwindPartialScope> completedObject;
            if (!retainUnwind) completedObject.emplace(*compiler);
            compiler->NoteUnwindPartial(LLVMBackend::UnwindPartialEntry::Kind::Slot,
                                        structPtr, typeName);
            if (retainUnwind && retainedUnwindSlots != nullptr)
                retainedUnwindSlots->emplace_back(structPtr, typeName);
            EmitFieldInitializer(structPtr, typeName, ctx, false, nullptr, false,
                                 structPtr, &typeName);
            return;
        }
        EmitFieldInitializer(structPtr, typeName, ctx);
}

void MainListener::CoerceElementToString(
        LLVMBackend* compiler,
        LLVMBackend::NamedVariable& nv,
        llvm::Value*& val,
        antlr4::ParserRuleContext* ctx) {
        if (!val || val->getType() != cflat_llvm::PointerTo(compiler->builder->getInt8Ty()))
            return;

        llvm::Value* strVal = nullptr;
        auto* c = llvm::dyn_cast<llvm::Constant>(val);
        if (c && compiler->IsStringLiteralConstant(c))
        {
            strVal = compiler->WrapStringLiteralAsString(val);
        }
        else if (compiler->GetFunction("operator string"))
        {
            LLVMBackend::NamedVariable argNV;
            argNV.Primary = val;
            argNV.BaseType = val->getType();
            argNV.TypeAndValue.TypeName = "char";
            argNV.TypeAndValue.Pointer = true;
            strVal = compiler->CreateOverloadedFunctionCall("operator string", { argNV });
        }
        if (!strVal)
            return;

        LLVMBackend::TypeAndValue strTV;
        strTV.TypeName = "string";
        auto* strTy = compiler->GetType(strTV);
        auto* strAlloca = compiler->AllocaAtEntry(strTy, nullptr, "initlist_str");
        compiler->builder->CreateStore(strVal, strAlloca);

        nv = {};
        nv.Storage = strAlloca;
        nv.BaseType = strTy;   // move-param handling dyn_casts BaseType; must not be null
        nv.TypeAndValue.TypeName = "string";
        val = strVal;
    }

void MainListener::EmitPositionalFixedArrayInit(
        const std::string& name,
        const LLVMBackend::TypeAndValue& tv,
        CFlatParser::InitializerListContext* initList,
        llvm::Value* arraySize,
        size_t line,
        std::vector<std::pair<std::string, llvm::AllocaInst*>>& allocList) {
        auto* compiler = Compiler(initList);
        auto* arrAlloc = compiler->CreateLocalVariable(tv, nullptr, arraySize, line);
        allocList.push_back(std::pair(name, llvm::dyn_cast<llvm::AllocaInst>(arrAlloc)));
        EmitPositionalFixedArrayIntoSlot(name, tv, initList, arrAlloc);
    }

void MainListener::EmitPositionalFixedArrayIntoSlot(
        const std::string& name,
        const LLVMBackend::TypeAndValue& tv,
        CFlatParser::InitializerListContext* initList,
        llvm::Value* arrAlloc,
        bool updateExistingElements,
        bool rejectFrameBoundReferences) {
        auto* compiler = Compiler(initList);
        // `arr = { name = {...} }` names an ARRAY slot, which has no name. A brace element one
        // level down is a struct element and is routed to EmitFieldInitializer below, so this
        // check stays at the array level instead of recursing.
        bool rejectedNamedBrace = false;
        for (auto* fi : initList->fieldInit())
            if (fi->Identifier() != nullptr && fi->initializerList() != nullptr)
            {
                LogErrorContext(fi,
                    "nested named brace initializer is not supported in positional fixed-array elements");
                rejectedNamedBrace = true;
            }
        if (rejectedNamedBrace) return;
        auto elements = initList->fieldInit();
        const auto initialElements = elements;
        bool multidim = !tv.ConstInnerDimensions.empty();
        // A STRUCT element type is the only one a braced element can construct.
        auto* elemStructTy = (FixedArrayElementStars(tv) == 0 && !tv.IsArrayView
                              && !compiler->IsInterfaceType(tv.TypeName))
            ? compiler->GetDataStructure(tv.TypeName).StructType
            : nullptr;
        uint64_t n = tv.ConstArraySize;
        for (uint64_t d : tv.ConstInnerDimensions) n *= d;

        auto* arrTy = llvm::cast<llvm::ArrayType>(compiler->GetType(tv));
        auto* elemTy = arrTy->getElementType();
        const bool cxxReferenceAggregateElements = elemStructTy != nullptr
            && compiler->IsForeignCxxAggregateClass(tv.TypeName)
            && HasForeignCxxAggregateReferenceMember(tv.TypeName, initList);
        std::function<size_t(const std::string&)> aggregateLeafCount;
        aggregateLeafCount = [&](const std::string& recordName) -> size_t {
            if (!compiler->IsForeignCxxAggregateClass(recordName)) return 1;
            const auto record = compiler->GetDataStructure(recordName);
            if (record.IsUnion) return record.StructFields.empty() ? 0 : 1;
            size_t count = 0;
            const size_t baseCount = compiler->CxxAggregateBaseCount(recordName);
            for (size_t baseIndex = 0; baseIndex < baseCount; ++baseIndex)
            {
                const std::string baseType = compiler->CxxAggregateBaseTypeAt(
                    recordName, baseIndex);
                if (!baseType.empty()) count += aggregateLeafCount(baseType);
            }
            for (const auto& field : record.StructFields)
            {
                const std::string fieldType = compiler->ResolveTypeAlias(field.TypeName);
                if (field.Pointer || field.ElemPointer || field.IsArrayView)
                {
                    ++count;
                    continue;
                }
                uint64_t arrayCount = field.ConstArraySize;
                for (uint64_t dimension : field.ConstInnerDimensions)
                    arrayCount *= dimension;
                if (arrayCount > 0)
                {
                    count += (size_t)arrayCount * aggregateLeafCount(fieldType);
                    continue;
                }
                count += aggregateLeafCount(fieldType);
            }
            return count;
        };
        const size_t elementLeafCount = cxxReferenceAggregateElements
            ? aggregateLeafCount(tv.TypeName) : 0;
        std::vector<std::pair<size_t, std::vector<CFlatParser::FieldInitContext*>>>
            referenceAggregateRows;
        std::vector<CFlatParser::FieldInitContext*> mappedArrayElements;
        bool hasMappedArrayElements = false;
        auto hasUninitializedReferenceMember = [&]() {
            return HasUninitializedForeignCxxAggregateReferenceMember(tv.TypeName, initList);
        };

        if (multidim)
        {
            uint64_t rowWidth = tv.ConstInnerDimensions.back();
            auto isEmptyBraceRow = [](CFlatParser::FieldInitContext* fi) {
                return fi->Identifier() == nullptr && fi->assignmentExpression().empty()
                    && fi->initializerList() == nullptr;
            };
            bool hasNested = false;
            for (auto* fi : elements)
                if (fi->initializerList() != nullptr || isEmptyBraceRow(fi))
                { hasNested = true; break; }

            bool hasBracedOuterElement = false;
            bool hasElidedOuterElement = false;
            for (auto* fi : elements)
            {
                hasBracedOuterElement |= fi->initializerList() != nullptr;
                hasElidedOuterElement |= !fi->assignmentExpression().empty();
            }
            if (!cxxReferenceAggregateElements && hasBracedOuterElement && hasElidedOuterElement)
            {
                LogErrorContext(initList,
                    "mixed braced and elided initializers are not supported for multi-dimensional fixed arrays");
                return;
            }

            // A char row can also be value-initialized with an empty brace.
            const bool charStringRows = tv.ConstInnerDimensions.size() == 1
                && elemTy->isArrayTy()
                && elemTy->getArrayElementType()->isIntegerTy(8)
                && std::all_of(elements.begin(), elements.end(), [&](auto* fi) {
                    return isEmptyBraceRow(fi) || fi->assignmentExpression().size() == 1;
                });
            if (charStringRows)
            {
                if (elements.size() > tv.ConstArraySize)
                {
                    LogErrorContext(initList, std::format(
                        "too many string rows for '{}[{}][{}]': got {} rows",
                        SpellType(*compiler, tv), tv.ConstArraySize, rowWidth, elements.size()));
                    return;
                }
                compiler->builder->CreateStore(llvm::Constant::getNullValue(arrTy), arrAlloc);
                llvm::Value* zero = compiler->builder->getInt32(0);
                for (size_t row = 0; row < elements.size(); row++)
                {
                    auto* fi = elements[row];
                    if (isEmptyBraceRow(fi)) continue;
                    if (fi->assignmentExpression().size() != 1)
                    {
                        LogErrorContext(fi, "multi-dimensional fixed-array initializers require nested braces or one string per char row");
                        return;
                    }
                    auto nv = ParseAssignmentExpressionNamed(fi->assignmentExpression(0));
                    auto* literal = llvm::dyn_cast_or_null<llvm::Constant>(LoadNamedVariable(nv));
                    auto* global = literal == nullptr ? nullptr
                        : llvm::dyn_cast<llvm::GlobalVariable>(literal->stripPointerCasts());
                    auto* bytes = global == nullptr ? nullptr
                        : llvm::dyn_cast<llvm::ConstantDataSequential>(global->getInitializer());
                    if (bytes == nullptr || !compiler->IsStringLiteralConstant(literal))
                    {
                        LogErrorContext(fi, "multi-dimensional char arrays require string literal rows");
                        return;
                    }
                    std::string text = bytes->getAsString().str();
                    if (!text.empty() && text.size() - 1 > rowWidth)
                    {
                        LogErrorContext(fi, std::format("string row has {} characters but '{}' has width {}",
                            text.size() - 1, SpellType(*compiler, tv), rowWidth));
                        return;
                    }
                    for (uint64_t col = 0; col < rowWidth; col++)
                    {
                        auto* colVal = compiler->builder->getInt32((uint32_t)col);
                        auto* rowVal = compiler->builder->getInt32((uint32_t)row);
                        auto* cell = compiler->builder->CreateInBoundsGEP(arrTy, arrAlloc,
                            {zero, rowVal, colVal}, "charrow");
                        uint8_t ch = col < text.size() ? (uint8_t)text[col] : 0;
                        compiler->builder->CreateStore(compiler->builder->getInt8(ch), cell);
                    }
                }
                return;
            }

            if (!hasNested && !cxxReferenceAggregateElements)
            {
                LogErrorContext(initList, std::format(
                    "flat initializer lists are not supported for '{}[{}]'; use nested braces matching each dimension",
                    SpellType(*compiler, tv), tv.ConstArraySize));
                return;
            }

            std::vector< CFlatParser::FieldInitContext* > flattened;
            const size_t dimensionCount = 1 + tv.ConstInnerDimensions.size();
            bool flattenFailed = false;
            std::function<void(CFlatParser::InitializerListContext*, size_t)> flatten =
                [&](CFlatParser::InitializerListContext* list, size_t depth) {
                    if (flattenFailed) return;
                    uint64_t expected = depth == 0 ? tv.ConstArraySize
                        : tv.ConstInnerDimensions[depth - 1];
                    if (list->fieldInit().size() > expected
                        && !(cxxReferenceAggregateElements
                            && depth + 1 == dimensionCount))
                    {
                        LogErrorContext(list, std::format(
                            "too many initializers at dimension {}: expected at most {}, got {}",
                            depth + 1, expected, list->fieldInit().size()));
                        flattenFailed = true;
                        return;
                    }
                    for (auto* fi : list->fieldInit())
                    {
                        if (depth + 1 < dimensionCount)
                        {
                            if (fi->initializerList() == nullptr)
                            {
                                if (isEmptyBraceRow(fi)) continue;
                                if (cxxReferenceAggregateElements) {
                                    flattened.push_back(fi);
                                    continue;
                                }
                                LogErrorContext(fi, "multi-dimensional fixed-array initializers require nested braces matching each dimension");
                                flattenFailed = true;
                                return;
                            }
                            flatten(fi->initializerList(), depth + 1);
                            if (flattenFailed) return;
                        }
                        else
                        {
                            // At the innermost dimension a brace IS the element for a struct
                            // element type ('P[2][2] a = { { {x=1}, ... } }'); for any other
                            // element type there is nothing a brace could construct.
                            if (fi->initializerList() != nullptr && elemStructTy == nullptr)
                            {
                                LogErrorContext(fi, "too many nested braces for fixed-array element");
                                flattenFailed = true;
                                return;
                            }
                            flattened.push_back(fi);
                        }
                    }
                };
            if (hasNested)
            {
                flatten(initList, 0);
                if (flattenFailed) return;
                elements = std::move(flattened);

                bool validRows = true;
                std::vector<std::pair<size_t,
                    std::vector<CFlatParser::FieldInitContext*>>> collectedRows;
                std::function<void(CFlatParser::InitializerListContext*, size_t, size_t)>
                    collectRows = [&](CFlatParser::InitializerListContext* list,
                                      size_t depth, size_t rowOrdinal) {
                        const size_t rank = 1 + tv.ConstInnerDimensions.size();
                        if (depth + 1 == rank)
                        {
                            collectedRows.emplace_back(rowOrdinal, list->fieldInit());
                            return;
                        }
                        const size_t expected = depth == 0
                            ? (size_t)tv.ConstArraySize
                            : (size_t)tv.ConstInnerDimensions[depth - 1];
                        size_t rowsPerElement = 1;
                        for (size_t dim = depth; dim + 1 < tv.ConstInnerDimensions.size(); ++dim)
                            rowsPerElement *= (size_t)tv.ConstInnerDimensions[dim];
                        const auto fields = list->fieldInit();
                        for (size_t i = 0; i < fields.size(); ++i)
                        {
                            if (isEmptyBraceRow(fields[i]))
                            {
                                for (size_t row = 0; row < rowsPerElement; ++row)
                                    collectedRows.emplace_back(rowOrdinal + i * rowsPerElement + row,
                                        std::vector<CFlatParser::FieldInitContext*>{});
                                continue;
                            }
                            if (fields[i]->initializerList() == nullptr)
                            {
                                validRows = false;
                                return;
                            }
                            collectRows(fields[i]->initializerList(), depth + 1,
                                rowOrdinal + i * rowsPerElement);
                            if (!validRows) return;
                        }
                        for (size_t i = fields.size(); i < expected; ++i)
                            for (size_t row = 0; row < rowsPerElement; ++row)
                                collectedRows.emplace_back(rowOrdinal + i * rowsPerElement + row,
                                    std::vector<CFlatParser::FieldInitContext*>{});
                    };
                collectRows(initList, 0, 0);
                if (validRows)
                {
                    if (cxxReferenceAggregateElements)
                        referenceAggregateRows = std::move(collectedRows);
                    else
                    {
                        mappedArrayElements.resize(n, nullptr);
                        size_t mappedCount = 0;
                        for (const auto& [rowOrdinal, row] : collectedRows)
                        {
                            for (size_t col = 0; col < row.size(); ++col)
                            {
                                const size_t slot = rowOrdinal * rowWidth + col;
                                if (slot >= mappedArrayElements.size())
                                {
                                    validRows = false;
                                    break;
                                }
                                mappedArrayElements[slot] = row[col];
                                ++mappedCount;
                            }
                            if (!validRows) break;
                        }
                        hasMappedArrayElements = validRows && mappedCount == elements.size();
                        if (!hasMappedArrayElements) mappedArrayElements.clear();
                    }
                }
            }
        }

        if (elements.size() > n && !cxxReferenceAggregateElements)
        {
            LogErrorContext(initList, std::format(
                "too many initializers for '{}[{}]': got {} elements",
                SpellType(*compiler, tv), n, elements.size()));
            return;
        }

        if (multidim)
        {
            while (elemTy->isArrayTy()) elemTy = elemTy->getArrayElementType();
        }
        // Zero-fill the whole array so unspecified trailing slots are well-defined.
        if (!updateExistingElements)
            compiler->builder->CreateStore(llvm::Constant::getNullValue(arrTy), arrAlloc);

        // The ELEMENT's type is what a stored value has to agree with, not the array's.
        LLVMBackend::TypeAndValue fixedElemTV = tv;
        int fixedElemTVStars = FixedArrayElementStars(tv);
        fixedElemTV.Pointer = fixedElemTVStars >= 1;
        fixedElemTV.ElemPointer = fixedElemTVStars >= 2;
        fixedElemTV.ConstArraySize = 0;
        fixedElemTV.IsArrayView = false;

        llvm::Value* zero = compiler->builder->getInt32(0);
        std::vector<llvm::Value*> elementPtrs;
        if (multidim)
        {
            // This caller needs every static element pointer; the shared walk may switch to a runtime loop.
            std::vector<uint64_t> dimensions;
            dimensions.reserve(tv.ConstInnerDimensions.size() + 1);
            dimensions.push_back(tv.ConstArraySize);
            dimensions.insert(dimensions.end(), tv.ConstInnerDimensions.begin(),
                              tv.ConstInnerDimensions.end());
            for (uint64_t flatIndex = 0; flatIndex < n; ++flatIndex)
            {
                uint64_t remaining = flatIndex;
                std::vector<llvm::Value*> indices{ zero };
                for (size_t dim = 0; dim < dimensions.size(); ++dim)
                {
                    uint64_t stride = 1;
                    for (size_t trailing = dim + 1; trailing < dimensions.size(); ++trailing)
                        stride *= dimensions[trailing];
                    uint64_t index = stride == 0 ? 0 : remaining / stride;
                    remaining = stride == 0 ? 0 : remaining % stride;
                    indices.push_back(compiler->builder->getInt32((uint32_t)index));
                }
                elementPtrs.push_back(compiler->builder->CreateInBoundsGEP(
                    arrTy, arrAlloc, indices, "arrelem"));
            }
        }
        // An unwind out of element i (or the value-initialized tail) destroys the elements
        // already built, newest first; element i itself never.
        LLVMBackend::UnwindPartialScope builtElements(*compiler);
        llvm::Value* builtElemPtr = nullptr;
        auto noteBuiltElement = [&]() {
            if (builtElemPtr != nullptr && elemStructTy != nullptr)
                compiler->NoteUnwindPartial(LLVMBackend::UnwindPartialEntry::Kind::Slot,
                                            builtElemPtr, tv.TypeName);
            builtElemPtr = nullptr;
        };
        uint64_t initializedElementCount = hasMappedArrayElements ? n : elements.size();
        const bool allAggregateElementsBraced = cxxReferenceAggregateElements
            && !elements.empty()
            && std::all_of(elements.begin(), elements.end(), [](auto* element) {
                return element->LeftBrace() != nullptr;
            });
        const bool hasUnbracedAggregateElement = cxxReferenceAggregateElements
            && std::any_of(elements.begin(), elements.end(), [](auto* element) {
                return element->LeftBrace() == nullptr;
            });
        const bool groupedReferenceAggregateElements = cxxReferenceAggregateElements
            && elementLeafCount > 0 && !elements.empty()
            && (allAggregateElementsBraced || hasUnbracedAggregateElement);
        if (groupedReferenceAggregateElements)
        {
            std::vector<std::vector<CFlatParser::FieldInitContext*>> aggregateElements;
            std::vector<size_t> aggregateElementIndices;
            const bool groupedByRows = multidim && !referenceAggregateRows.empty()
                && std::all_of(initialElements.begin(), initialElements.end(),
                    [](auto* row) { return row->LeftBrace() != nullptr; });
            if (groupedByRows)
            {
                const size_t rowWidth = (size_t)tv.ConstInnerDimensions.back();
                for (const auto& [rowOrdinal, row] : referenceAggregateRows)
                {
                    size_t sourceIndex = 0;
                    size_t rowElementIndex = 0;
                    while (sourceIndex < row.size())
                    {
                        auto* first = row[sourceIndex];
                        if (rowElementIndex >= rowWidth)
                        {
                            LogErrorContext(first, std::format(
                                "too many initializers at dimension {}: expected at most {}, got more",
                                tv.ConstInnerDimensions.size() + 1, rowWidth));
                            return;
                        }
                        if (first->LeftBrace() != nullptr)
                        {
                            aggregateElements.push_back({ first });
                            ++sourceIndex;
                        }
                        else
                        {
                            if (elementLeafCount > row.size() - sourceIndex)
                            {
                                LogErrorContext(first, std::format(
                                    "too few initializer clauses for C++ aggregate array element '{}'",
                                    SpellType(*compiler, fixedElemTV)));
                                return;
                            }
                            aggregateElements.emplace_back(
                                row.begin() + static_cast<std::ptrdiff_t>(sourceIndex),
                                row.begin() + static_cast<std::ptrdiff_t>(sourceIndex + elementLeafCount));
                            sourceIndex += elementLeafCount;
                        }
                        aggregateElementIndices.push_back(rowOrdinal * rowWidth + rowElementIndex);
                        ++rowElementIndex;
                    }
                    while (rowElementIndex < rowWidth)
                    {
                        aggregateElements.emplace_back();
                        aggregateElementIndices.push_back(rowOrdinal * rowWidth + rowElementIndex);
                        ++rowElementIndex;
                    }
                }
                initializedElementCount = referenceAggregateRows.size() * rowWidth;
            }
            else
            {
                size_t sourceIndex = 0;
                while (sourceIndex < elements.size())
                {
                    auto* first = elements[sourceIndex];
                    if (first->LeftBrace() != nullptr)
                    {
                        aggregateElements.push_back({ first });
                        ++sourceIndex;
                    }
                    else
                    {
                        if (elementLeafCount > elements.size() - sourceIndex)
                        {
                            LogErrorContext(first, std::format(
                                "too few initializer clauses for C++ aggregate array element '{}'",
                                SpellType(*compiler, fixedElemTV)));
                            return;
                        }
                        aggregateElements.emplace_back(
                            elements.begin() + static_cast<std::ptrdiff_t>(sourceIndex),
                            elements.begin() + static_cast<std::ptrdiff_t>(sourceIndex + elementLeafCount));
                        sourceIndex += elementLeafCount;
                    }
                    aggregateElementIndices.push_back(aggregateElements.size() - 1);
                }
                initializedElementCount = aggregateElements.size();
            }
            if (aggregateElements.size() > n
                || std::any_of(aggregateElementIndices.begin(), aggregateElementIndices.end(),
                    [&](size_t index) { return index >= n; }))
            {
                LogErrorContext(initList, std::format(
                    "too many initializers for '{}[{}]': expected at most {} elements",
                    SpellType(*compiler, tv), n, n));
                return;
            }
            auto* globalOwner = llvm::dyn_cast<llvm::GlobalVariable>(arrAlloc);
            for (size_t index = 0; index < aggregateElements.size(); ++index)
            {
                noteBuiltElement();
                const size_t elementIndex = aggregateElementIndices[index];
                auto* elemPtr = multidim
                    ? elementPtrs[elementIndex]
                    : compiler->builder->CreateInBoundsGEP(arrTy, arrAlloc,
                        { zero, compiler->builder->getInt32((uint32_t)elementIndex) }, "arrelem");
                auto& group = aggregateElements[index];
                if (group.empty())
                {
                    if (!updateExistingElements && hasUninitializedReferenceMember())
                    {
                        LogErrorContext(initList, std::format(
                            "reference member must be initialized in C++ aggregate array element '{}'",
                            SpellType(*compiler, fixedElemTV)));
                        return;
                    }
                    builtElemPtr = updateExistingElements ? nullptr : elemPtr;
                    if (!updateExistingElements
                        && compiler->CxxElementNeedsDefaultConstruction(tv.TypeName))
                    {
                        if (compiler->CxxEmptyBraceUsesListCtor(tv.TypeName))
                            EmitCxxEmptyBraceConstruct(elemPtr, tv.TypeName, initList, true);
                        else
                        {
                            std::string ctorError;
                            if (!compiler->EmitCxxArrayDefaultConstruction(tv.TypeName,
                                    elemPtr, elemStructTy, compiler->builder->getInt64(1),
                                    ctorError))
                                LogErrorContext(initList, ctorError);
                        }
                    }
                    continue;
                }
                auto* first = group.front();
                builtElemPtr = updateExistingElements ? nullptr : elemPtr;
                if (updateExistingElements)
                    if (auto* dtor = compiler->GetOrCreateFullDestructor(tv.TypeName))
                        compiler->builder->CreateCall(dtor->getFunctionType(), dtor, { elemPtr });
                if (group.size() == 1 && first->LeftBrace() != nullptr
                    && first->initializerList() != nullptr
                    && !first->initializerList()->fieldInit().empty())
                {
                    TryEmitForeignCxxAggregateBraceInit(tv.TypeName,
                        first->initializerList(), first, elemPtr, true, false,
                        globalOwner, rejectFrameBoundReferences);
                }
                else if (group.size() == 1 && first->LeftBrace() != nullptr)
                    EmitBraceElementIntoFixedSlot(elemPtr, elemStructTy, tv.TypeName,
                        first, rejectFrameBoundReferences);
                else
                    TryEmitForeignCxxAggregateBraceInitFields(tv.TypeName, group,
                        first, elemPtr, true, false, globalOwner,
                        rejectFrameBoundReferences);
            }
            noteBuiltElement();
        }
        const size_t elementLoopCount = hasMappedArrayElements ? (size_t)n : elements.size();
        for (size_t i = 0; !groupedReferenceAggregateElements && i < elementLoopCount; i++)
        {
            noteBuiltElement();
            auto* fi = hasMappedArrayElements ? mappedArrayElements[i] : elements[i];
            llvm::Value* idx = compiler->builder->getInt32((uint32_t)i);
            auto* elemPtr = multidim
                ? elementPtrs[i]
                : compiler->builder->CreateInBoundsGEP(arrTy, arrAlloc, { zero, idx }, "arrelem");
            builtElemPtr = updateExistingElements ? nullptr : elemPtr;
            if (fi == nullptr)
            {
                builtElemPtr = nullptr;
                if (!updateExistingElements && elemStructTy != nullptr)
                {
                    if (compiler->GetFunction(tv.TypeName) != nullptr)
                    {
                        ConstructDefaultArrayElement(compiler, tv.TypeName, elemPtr, false);
                        builtElemPtr = elemPtr;
                    }
                    else if (compiler->CxxElementNeedsDefaultConstruction(tv.TypeName))
                    {
                        if (hasUninitializedReferenceMember())
                        {
                            LogErrorContext(initList, std::format(
                                "reference member must be initialized in C++ aggregate array element '{}'",
                                SpellType(*compiler, fixedElemTV)));
                            return;
                        }
                        if (compiler->CxxEmptyBraceUsesListCtor(tv.TypeName))
                            EmitCxxEmptyBraceConstruct(elemPtr, tv.TypeName, initList, true);
                        else
                        {
                            std::string ctorError;
                            if (!compiler->EmitCxxArrayDefaultConstruction(
                                    tv.TypeName, elemPtr, elemStructTy,
                                    compiler->builder->getInt64(1), ctorError))
                                LogErrorContext(initList, ctorError);
                        }
                        builtElemPtr = elemPtr;
                    }
                }
                continue;
            }
            // A BRACED element carries no assignmentExpression at all (CFlat.g4 fieldInit's
            // '{' initializerList? '}' alternative), so it must be split off before the
            // expression read - that read is what walked a null context and crashed.
            if (FieldInitIsBraceElement(fi))
            {
                if (elemStructTy == nullptr)
                {
                    LogErrorContext(fi, std::format(
                        "a brace initializer is not allowed for element {} of '{}': element type "
                        "'{}' is not a struct, so there are no fields to initialize",
                        i, name,
                        [&]() {
                            auto elementTV = tv;
                            elementTV.Pointer = false;
                            elementTV.ElemPointer = false;
                            elementTV.PointerDepth = 0;
                            elementTV.IsArrayView = false;
                            elementTV.IsNullable = false;
                            return SpellType(*compiler, elementTV)
                                + std::string(FixedArrayElementStars(tv), '*');
                        }()));
                    continue;
                }
                llvm::Value* braceIdx = compiler->builder->getInt32((uint32_t)i);
                auto* braceElemPtr = multidim
                    ? elementPtrs[i]
                    : compiler->builder->CreateInBoundsGEP(arrTy, arrAlloc, { zero, braceIdx }, "arrelem");
                if (updateExistingElements)
                {
                    if (auto* elementList = fi->initializerList())
                    {
                        LLVMBackend::UnwindPartialScope elementOverride(*compiler);
                        EmitFieldInitializer(braceElemPtr, tv.TypeName, elementList, false,
                                             nullptr, /*deferForeignReplacementDrop*/ true);
                    }
                }
                else
                    EmitBraceElementIntoFixedSlot(braceElemPtr, elemStructTy, tv.TypeName, fi,
                                                  rejectFrameBoundReferences);
                continue;
            }
            auto* assignment = fi->assignmentExpression(0);
            const bool cxxElement = !fixedElemTV.Pointer && !fixedElemTV.ElemPointer
                && !fixedElemTV.IsArrayView
                && compiler->IsForeignNontrivialCxxClass(tv.TypeName);
            const bool loweredCxxElement = !fixedElemTV.Pointer && !fixedElemTV.ElemPointer
                && !fixedElemTV.IsArrayView && compiler->IsLoweredCFlatOnlyStruct(tv.TypeName);
            auto* moveExpr = TopLevelMoveExpression(assignment);

            // A C++ move keeps the source object alive; do not route it through CFlat's
            // destructive ParseMoveExpression before calling the C++ move constructor.
            if (cxxElement && moveExpr != nullptr)
            {
                auto* inner = moveExpr->unaryExpression();
                const std::string sourceName = inner != nullptr ? inner->getText() : std::string();
                if (IsBareIdentifierText(sourceName))
                {
                    auto* source = compiler->FindLiveNamedVariable(sourceName);
                    if (source != nullptr && source->Storage != nullptr
                        && !source->TypeAndValue.Pointer
                        && source->TypeAndValue.TypeName == tv.TypeName)
                    {
                        if (source->IsMoved || source->ExplicitlyMovedNull)
                        {
                            LogMovedUse(moveExpr, sourceName,
                                        IsPointeeMoveUse(*source) && source->CallerName == sourceName, true);
                        }
                        else
                        {
                            compiler->SetCurrentDebugLocation(moveExpr->getStart()->getLine());
                            compiler->EmitCxxCopyOrMoveConstruct(
                                tv.TypeName, elemPtr, source->Storage, /*useMove*/ true,
                                "into a fixed-array element");
                            compiler->MarkVariableMoved(sourceName);
                        }
                        continue;
                    }
                }
            }

            // A single C++ call can construct directly in the fresh array slot, matching the
            // scalar declaration path. More complex expressions use the temporary-then-move path.
            bool cxxSretArmed = false;
            if (cxxElement)
            {
                // Per element: a temporary left by an EARLIER element must not mark this one movable.
                compiler->lastCxxRetTemp_ = nullptr;
                compiler->lastCxxRetValue_ = nullptr;
            }
            if (cxxElement && moveExpr == nullptr)
            {
                const std::string text = assignment->getText();
                // An assignment shows the slot only to its own operator call (CxxAssignSretScope).
                const bool singleCall = (!text.empty() && text.back() == ')'
                        && std::count(text.begin(), text.end(), '(') == 1)
                    || SoleAssignmentExpression(assignment) != nullptr;
                if (singleCall)
                {
                    compiler->pendingCxxSretDest_ = elemPtr;
                    compiler->pendingCxxSretTypeName_ = tv.TypeName;
                    compiler->pendingCxxSretForFixedArray_ = true;
                    cxxSretArmed = true;
                }
            }
            // A native ctor that may capture `this` builds the element in its slot (one ctor call).
            else if (!cxxElement && moveExpr == nullptr && !fixedElemTV.Pointer
                && !fixedElemTV.ElemPointer && !fixedElemTV.IsArrayView
                && compiler->IsNativeValueConstructor(tv.TypeName,
                    LLVMBackend::TypeAndValue{ .TypeName = tv.TypeName }, {})
                && FieldInitializerIsSingleCall(assignment))
            {
                compiler->pendingCxxSretDest_ = elemPtr;
                compiler->pendingCxxSretTypeName_ = tv.TypeName;
                compiler->pendingCxxSretForFixedArray_ = true;
                cxxSretArmed = true;
            }
            auto elementOwnedTempMark = compiler->MarkOwnedTemps();
            LLVMBackend::NamedVariable nv;
            {
                DeclExpectedTypeScope elementExpectedScope(&declExpectedType, fixedElemTV, true);
                if (loweredCxxElement && moveExpr != nullptr)
                {
                    LoweredBraceMoveScope deferMove(*this, moveExpr, tv.TypeName, elemPtr);
                    nv = ParseAssignmentExpressionNamed(assignment);
                }
                else
                    nv = ParseAssignmentExpressionNamed(assignment);
            }
            BindNamedFunctionToFuncPtrSlot(compiler, nv, fixedElemTV, assignment);
            if (std::string why = compiler->FuncPtrValueOwnershipReason(nv, fixedElemTV); !why.empty())
                LogErrorContext(assignment, std::format(
                    "incompatible function pointer initializer: {}", why));
            const bool cxxSretConsumed = cxxSretArmed
                && compiler->pendingCxxSretDest_ == nullptr;
            if (cxxSretArmed)
            {
                compiler->pendingCxxSretDest_ = nullptr;
                compiler->pendingCxxSretTypeName_.clear();
                compiler->pendingCxxSretForFixedArray_ = false;
            }
            if (cxxSretConsumed) continue;
            if (loweredCxxElement && moveExpr != nullptr && nv.TypeAndValue.IsMove
                && nv.Storage != nullptr && nv.Storage != elemPtr)
            {
                if (updateExistingElements)
                    if (auto* dtor = compiler->GetOrCreateFullDestructor(tv.TypeName))
                        compiler->builder->CreateCall(dtor->getFunctionType(), dtor, { elemPtr });
                const std::string movedName = compiler->FindVariableNameByStorage(nv.Storage);
                if (!movedName.empty()) compiler->MarkVariableNotExplicitlyMovedNull(movedName);
                compiler->EmitCxxCopyOrMoveConstruct(tv.TypeName, elemPtr, nv.Storage,
                    /*useMove*/ true, "into a fixed-array element");
                compiler->FlushOwnedTempsSince(elementOwnedTempMark, nullptr, nullptr);
                noteBuiltElement();
                continue;
            }
            if (loweredCxxElement && moveExpr != nullptr && nv.Storage == nullptr)
            {
                if (updateExistingElements)
                    if (auto* dtor = compiler->GetOrCreateFullDestructor(tv.TypeName))
                        compiler->builder->CreateCall(dtor->getFunctionType(), dtor, { elemPtr });
                auto sourceNV = nv;
                sourceNV.TypeAndValue.IsMove = true;
                sourceNV.IsExplicitMove = true;
                sourceNV.IsRvalue = true;
                auto* sourceValue = sourceNV.Primary != nullptr
                    ? sourceNV.Primary : LoadNamedVariable(sourceNV);
                if (sourceValue == nullptr || !EmitLoweredValueIntoSlot(compiler, tv.TypeName,
                        elemPtr, sourceNV, sourceValue, moveExpr, "into a fixed-array element"))
                    continue;
                compiler->FlushOwnedTempsSince(elementOwnedTempMark, nullptr, nullptr);
                noteBuiltElement();
                continue;
            }
            llvm::Value* val = LoadNamedVariable(nv);
            if (!val) continue;

            compiler->RejectImplicitIntegerPointeePointerConversion(nv.TypeAndValue, fixedElemTV);

            if (RejectImplicitPrimitiveToPointer(
                    fi, fixedElemTV, nv, val, "brace-initialize",
                    std::format("element {} of '{}'", i, name)))
                continue;

            if (RejectImplicitPointerToNumber(
                    fi, fixedElemTV, nv, val, "brace-initialize",
                    std::format("element {} of '{}'", i, name)))
                continue;
            if (RejectConstDiscardingConversion(fi, nv, val, fixedElemTV))
                continue;

            // Aggregate leg of the code-value store gate: `Rec*[2] arr = { w, w };`.
            RejectCodeValueIntoDataSlot(fi, nv, fixedElemTV, "brace-initialize",
                                        std::format("element {} of '{}' of", i, name));
            // A string LITERAL is a 'const char*', never a 'T*' - the ELEMENT leg of the same gate
            // the scalar store sites apply. The element's type is what a stored value must agree
            // with, so this reads fixedElemTV, not the array's own type.
            RejectStringLiteralIntoStructPointer(fi, fixedElemTV, val,
                                        std::format("element {} of '{}'", i, name));

            // Thin sibling of the widen elsewhere: positional fixed-array brace-init into a
            // thin function<> element fed a raw pointer (spelled or generic-encoded).
            {
                const LLVMBackend::TypeAndValue* clo = fixedElemTV.IsFunctionPointer
                    ? &fixedElemTV : compiler->GetEncodedClosureType(fixedElemTV.TypeName);
                if (clo && clo->IsThinFnPtr() && !fixedElemTV.Pointer && fixedElemTV.ConstArraySize == 0
                    && val->getType()->isPointerTy() && !val->getType()->isStructTy())
                {
                    compiler->CheckThinFnPtrAssignProvenance(val, nv,
                        std::format("'{}[{}]'", name, i));
                    val = compiler->BareFunctionToThinValue(val, clo);
                }
            }

            // Array-aggregate leg of the temp-unique-field escape. This lowering is NOT
            // EmitOneFieldInit, so the struct brace-init leg could never see it.
            GuardOwningTempUniqueFieldEscape(
                nv, std::format("element {} of array '{}'", i, name), fi);

            if (tv.TypeName == "string"
                && val->getType() == cflat_llvm::PointerTo(compiler->builder->getInt8Ty()))
            {
                LLVMBackend::NamedVariable tmp = nv;
                CoerceElementToString(compiler, tmp, val, fi);
            }
            // Interface element: same boxing/rebox the struct-field brace path applies.
            if (!tv.ElemPointer && compiler->IsInterfaceType(tv.TypeName))
                val = CoerceInitValueToInterface(nv, val, tv.TypeName, fi);

            if (cxxElement)
            {
                EmitForeignCxxValueIntoSlot(fixedElemTV, elemPtr, nv, val,
                    elementOwnedTempMark, "into a fixed-array element", fi,
                    /*intoFreshSlot*/ true);
                continue;
            }
            ConsumeOwningBraceElementSource(nv, val, elemPtr, fixedElemTV, tv.TypeName, fi);
            // Update mode: the element is live (the aggregate's constructor built it), so destroy
            // it before the store, as the braced override does. val is already an owned value.
            if (updateExistingElements)
                if (auto* dtor = compiler->GetOrCreateFullDestructor(tv.TypeName))
                    compiler->builder->CreateCall(dtor->getFunctionType(), dtor, { elemPtr });
            {
                LLVMBackend::NativeBraceScope braceScope(*compiler);
                compiler->CreateAssignment(val, elemPtr,
                    nv.TypeAndValue.IsUnsignedInteger() != -1, elemTy, fixedElemTV.IsUnsignedInteger() != -1);
            }
        }
        noteBuiltElement();

        /*
         * C++ VALUE-INITIALIZES the slots past a short positional list, which for a class with a
         * nontrivial default constructor means calling it. The zero fill above left that tail
         * unconstructed while scope exit still destroyed all N. The tail is contiguous in the
         * flat layout (elementPtrs is built in row-major order), so one walk covers it.
         */
        if (elemStructTy != nullptr && initializedElementCount < n
            && compiler->GetFunction(tv.TypeName) == nullptr)
        {
            if (hasUninitializedReferenceMember())
            {
                LogErrorContext(initList, std::format(
                    "reference member must be initialized in C++ aggregate array element '{}'",
                    SpellType(*compiler, fixedElemTV)));
                return;
            }
            if (compiler->CxxElementNeedsDefaultConstruction(tv.TypeName))
            {
                uint64_t first = initializedElementCount;
                llvm::Value* tailBase = multidim
                    ? elementPtrs[first]
                    : compiler->builder->CreateInBoundsGEP(
                        arrTy, arrAlloc, { zero, compiler->builder->getInt32((uint32_t)first) },
                        "arrtail");
                std::string ctorError;
                if (compiler->CxxEmptyBraceUsesListCtor(tv.TypeName))
                {
                    // Each unlisted slot is value-initialized from `{}`: the list constructor, n == 0.
                    for (uint64_t slot = first; slot < n; ++slot)
                    {
                        llvm::Value* slotPtr = multidim
                            ? elementPtrs[slot]
                            : compiler->builder->CreateInBoundsGEP(
                                arrTy, arrAlloc, { zero, compiler->builder->getInt32((uint32_t)slot) },
                                "arrslot");
                        EmitCxxEmptyBraceConstruct(slotPtr, tv.TypeName, initList, true);
                    }
                }
                else if (!compiler->EmitCxxArrayDefaultConstruction(
                        tv.TypeName, tailBase, elemStructTy,
                        compiler->builder->getInt64(n - first), ctorError))
                {
                    LogErrorContext(initList, ctorError);
                    return;
                }
            }
        }
        /*
         * A native struct tail takes the element's own default, field defaults or CFlat constructor,
         * exactly as an omitted element does; the zero fill above would leave `0` where clang keeps
         * `7`. Update mode keeps the elements the containing aggregate already built.
         */
        if (!updateExistingElements && elemStructTy != nullptr && initializedElementCount < n
            && compiler->GetFunction(tv.TypeName) != nullptr)
        {
            uint64_t first = initializedElementCount;
            llvm::Value* tailBase = multidim
                ? elementPtrs[first]
                : compiler->builder->CreateInBoundsGEP(
                    arrTy, arrAlloc, { zero, compiler->builder->getInt32((uint32_t)first) }, "arrtail");
            compiler->EmitArrayConstructionWalk(
                tailBase, elemStructTy, n - first, tv.TypeName,
                [&](llvm::Value* elemPtr)
                {
                    ConstructDefaultArrayElement(compiler, tv.TypeName, elemPtr, false);
                });
        }
    }

/*
 * The element-CONSTRUCTION twin of the fixed-array-element assignment arm. A positional brace
 * list bit-copied an owning lvalue source into the slot, so the source and the new element both
 * destructed one resource (double free). Defer to T through the shared decision: a copyable owner
 * COPIES (source stays live), a non-copyable one MOVES and the source lvalue is zeroed, with
 * MarkVariableMoved for a named slot. NO drop-old - the slot was zero-filled by the caller and is
 * being constructed, not overwritten; that is the container-slot decision, not the assignment one.
 * `string` elements carry a runtime owned bit instead of a copy()/move classification, so they get
 * their own deep-copy leg, exactly as in the assignment arm.
 */
void MainListener::ConsumeOwningBraceElementSource(
        const LLVMBackend::NamedVariable& nv,
        llvm::Value*& val,
        llvm::Value* elemPtr,
        const LLVMBackend::TypeAndValue& elemTV,
        const std::string& elemTypeName,
        antlr4::ParserRuleContext* ctx) {
        auto* compiler = Compiler(ctx);
        if (val == nullptr || !val->getType()->isStructTy()) return;
        if (elemTV.Pointer || elemTV.ElemPointer || elemTV.IsArrayView) return;
        if (elemTypeName == "string")
        {
            // The slot is CONSTRUCTED (the caller zero-filled the array), so no drop-old: deep-copy
            // an LVALUE source so the element owns an independent buffer, and adopt a `move` /
            // owned temp as-is (dropping it from the temp-flush list, or the flush frees the
            // element's buffer at end of statement). Mirrors the struct-field brace-init arm.
            if (nv.Storage != nullptr && !nv.TypeAndValue.IsMove)
                val = compiler->EmitOwnedStringDeepCopy(val);
            compiler->UnregisterOwnedStringTemp(val);
            TransferMoveStringOwnershipOnStore(nv, ctx);
            return;
        }
        if (compiler->IsInterfaceType(elemTypeName)) return;

        // A source with STORAGE is an lvalue: a named slot (alloca/global) or an INDIRECT one
        // (`*ap`, `w.b`, `arr[i]`) whose Storage is the GEP / loaded address, and has no name.
        bool srcIsNamedSlot = nv.Storage != nullptr
            && (llvm::isa<llvm::AllocaInst>(nv.Storage)
                || llvm::isa<llvm::GlobalVariable>(nv.Storage))
            && !nv.CallerName.empty();
        bool srcIsOwningAssignLvalue = nv.Storage != nullptr
            && !SourceIsDanglingAliasBorrow(compiler, nv)
            && (srcIsNamedSlot || nv.TypeAndValue.TypeName != "string");
        if (!srcIsOwningAssignLvalue) return;
        // An explicit `move` source keeps its existing lowering (it already nulls the source), so
        // it is excluded here on the same predicate the assignment arm uses.
        if (nv.TypeAndValue.IsMove && !nv.IsOwningStruct) return;
        if (nv.TypeAndValue.Pointer || nv.TypeAndValue.IsArrayView
            || nv.TypeAndValue.IsInterfacePointer || nv.TypeAndValue.IsFunctionPointer) return;
        if (elemPtr == nv.Storage) return;
        // Same type on both sides (alias-resolved, per the `using` lesson): a mismatch must fall
        // through untouched to the existing cast diagnostic.
        if (compiler->ResolveTypeAlias(nv.TypeAndValue.TypeName)
            != compiler->ResolveTypeAlias(elemTypeName)) return;
        if (!compiler->IsOwningValueType(nv.TypeAndValue.TypeName)) return;

        AssignSourceKind kind;
        val = ClassifyOwningAssignSource(val, nv.TypeAndValue.TypeName,
                                         nv.TypeAndValue.IsMove, ctx, kind);
        if (kind != AssignSourceKind::Move) return;
        if (RejectConsumeOfBorrowedByValueParamField(compiler, nv, ctx)) return;
        if (RejectImplicitConsumeOfOutlivingOwner(compiler, nv, nv.TypeAndValue.IsMove, ctx)) return;
        compiler->builder->CreateStore(
            llvm::ConstantAggregateZero::get(val->getType()), nv.Storage);
        // Only a named slot has a spelling to report a later use of; an indirect lvalue
        // (field/element/deref) is consumed silently, exactly as `move w.b` already is.
        if (srcIsNamedSlot)
            compiler->MarkVariableMoved(nv.CallerName);
    }

std::string MainListener::DescribeInitializerPath(const std::string& callerName,
                                               const std::string& fieldName) {
        if (callerName.empty()) return "arr";
        if (fieldName.empty()) return callerName;
        return callerName + "." + fieldName;
    }

int MainListener::FixedArrayElementStars(const LLVMBackend::TypeAndValue& tv) {
        return (tv.Pointer ? 1 : 0) + (tv.ElemPointer ? 1 : 0);
    }

std::string MainListener::DescribeArrayShape(const LLVMBackend::TypeAndValue& tv) const {
        // A simd element keeps its own spelling: TypeName is only the LANE type, so without this
        // a simd<float,4>[2] reads as 'float[2]'. Wording only - the shape is unchanged.
        LLVMBackend::TypeAndValue elemType = tv;
        elemType.Pointer = false;
        elemType.ElemPointer = false;
        elemType.PointerDepth = 0;
        elemType.IsArrayView = false;
        std::string elem = SpellType(*compilerLLVM, elemType);
        if (tv.IsSimd) elem = std::format("simd<{},{}>", elem, tv.SimdLanes);
        std::string text = elem + std::string(FixedArrayElementStars(tv), '*');
        text += std::format("[{}]", tv.ConstArraySize);
        for (uint64_t d : tv.ConstInnerDimensions) text += std::format("[{}]", d);
        return text;
    }

std::string MainListener::DescribeArrayShape(const std::string& typeName, int stars, uint64_t n,
                                          const std::vector<uint64_t>& innerDims) {
        std::string text = typeName + std::string(stars, '*');
        text += std::format("[{}]", n);
        for (uint64_t d : innerDims) text += std::format("[{}]", d);
        return text;
    }

void MainListener::EmitFixedArrayValueCopy(antlr4::ParserRuleContext* ctx,
                                 const LLVMBackend::TypeAndValue& destTV,
                                 llvm::Value* destAlloc, llvm::ArrayType* destArrTy,
                                 llvm::Value* srcPtr,
                                 const std::string& srcTypeName, bool srcPointer,
                                 bool srcElemPointer, uint64_t srcConstArraySize,
                                 const std::vector<uint64_t>& srcInnerDims) {
        auto* compiler = Compiler(ctx);
        std::string destText = DescribeArrayShape(destTV);

        // A VIEW source is the only array-shaped thing with no compile-time extent, so the copy
        // cannot be sized. (The caller guarantees array-shaped, so this IS the view case.)
        if (srcConstArraySize == 0)
        {
            LogErrorContext(ctx, std::format(
                "cannot initialize fixed array '{}' from array-view '{}[]' - a view carries no "
                "compile-time extent, so the copy cannot be sized. Declare '{}[] {}' to bind the "
                "view instead, or copy the elements in a loop.",
                destText, SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = srcTypeName }),
                SpellType(*compiler, destTV), destTV.VariableName));
            return;
        }

        int destStars = FixedArrayElementStars(destTV);
        int srcStars = (srcPointer ? 1 : 0) + (srcElemPointer ? 1 : 0);
        LLVMBackend::TypeAndValue srcBase;
        srcBase.TypeName = srcTypeName;
        const std::string srcSpelling = SpellType(*compiler, srcBase);
        std::string srcText = DescribeArrayShape(srcSpelling, srcStars, srcConstArraySize,
                                                 srcInnerDims);
        if (srcTypeName != destTV.TypeName || srcStars != destStars
            || srcConstArraySize != destTV.ConstArraySize
            || srcInnerDims != destTV.ConstInnerDimensions)
        {
            LogErrorContext(ctx, std::format(
                "cannot initialize fixed array '{}' from '{}' - a fixed-array copy requires "
                "identical element type and extents", destText, srcText));
            return;
        }

        // A memcpy of elements that own a resource would hand the same pointer to two arrays'
        // scope-exit destructors (both run element-wise), so it is a double free. There is no
        // element-wise copy constructor to fall back on, so reject rather than emit one. An
        // array OF POINTERS (destStars > 0) is exempt: its elements are addresses, which the
        // language already lets two variables hold, and no destructor runs over them.
        if (destStars == 0 && compiler->IsOwningValueType(destTV.TypeName))
        {
            LogErrorContext(ctx, std::format(
                "cannot copy fixed array '{}' - element type '{}' owns a resource, so a bitwise "
                "copy would let both arrays destruct it. Copy the elements explicitly.",
                destText, SpellType(*compiler, destTV)));
            return;
        }

        uint64_t bytes = compiler->module->getDataLayout().getTypeAllocSize(destArrTy);
        compiler->builder->CreateMemCpy(destAlloc, llvm::MaybeAlign(), srcPtr,
                                        llvm::MaybeAlign(), bytes);
    }

void MainListener::EmitFixedArrayDefaultInit(llvm::Value* arrAlloc, const LLVMBackend::TypeAndValue& tv,
                                             antlr4::ParserRuleContext* where, bool forceRoot) {
        auto* compiler = Compiler(where);
        auto structData = compiler->GetDataStructure(tv.TypeName);
        auto* arrTy = llvm::dyn_cast_or_null<llvm::ArrayType>(compiler->GetType(tv));
        if (structData.StructType == nullptr || arrTy == nullptr) return;

        // Total slots: the outer extent times every inner dimension (T[N][M] is N*M structs).
        uint64_t n = tv.ConstArraySize;
        for (uint64_t d : tv.ConstInnerDimensions) n *= d;
        if (n == 0) return;

        // A foreign C++ element with a nontrivial default constructor: run that constructor once
        // per slot. The zero store below would skip it while scope exit still destroys all N.
        if (compiler->GetFunction(tv.TypeName) == nullptr
            && compiler->CxxElementNeedsDefaultConstruction(tv.TypeName))
        {
            std::string ctorError;
            if (!compiler->EmitCxxArrayDefaultConstruction(
                    tv.TypeName, arrAlloc, structData.StructType,
                    compiler->builder->getInt64(n), ctorError))
            {
                if (where != nullptr) LogErrorContext(where, ctorError);
                else compiler->LogErrorMessage("{}", { ctorError });
            }
            return;
        }

        // An owning default (its constructor allocates a resource) must be built independently in
        // each slot: a single seed replicated bitwise would alias one resource across all N slots
        // and double-free at teardown. The walk is shared with the destruction path so the two
        // traversals cannot drift.
        if (compiler->GetFunction(tv.TypeName) && compiler->IsOwningValueType(tv.TypeName))
        {
            compiler->EmitArrayConstructionWalk(
                arrAlloc, structData.StructType, n, tv.TypeName,
                [&](llvm::Value* elemPtr)
                {
                    ConstructDefaultArrayElement(compiler, tv.TypeName, elemPtr, forceRoot);
                });
            return;
        }

        // No default constructor: one store of the whole array, which lowers to a single memset.
        if (compiler->GetFunction(tv.TypeName) == nullptr)
        {
            compiler->builder->CreateStore(llvm::Constant::getNullValue(arrTy), arrAlloc);
            return;
        }

        // A construction that folds to a COMPILE-TIME CONSTANT has no side effect to run (the
        // fold declines on any callee that writes memory), so replicating it is observationally
        // identical to constructing N times - and keeps the single store for the common case.
        LLVMBackend::DeclTypeAndValue elemDecl;
        static_cast<LLVMBackend::TypeAndValue&>(elemDecl) = tv;
        elemDecl.ConstArraySize = 0;
        elemDecl.ConstInnerDimensions.clear();
        if (auto* folded = TryFoldGlobalDefaultConstruction(elemDecl))
        {
            if (folded->isNullValue())
                compiler->builder->CreateStore(llvm::Constant::getNullValue(arrTy), arrAlloc);
            else
                compiler->builder->CreateStore(
                    SplatConstantOverFixedArray(folded, arrTy), arrAlloc);
            return;
        }

        // Otherwise the element default is non-trivial: run it ONCE PER ELEMENT, the same rule
        // C++ applies to `S a[N]`. A single seed replicated bitwise runs its effects once.
        compiler->EmitArrayConstructionWalk(
            arrAlloc, structData.StructType, n, tv.TypeName,
            [&](llvm::Value* elemPtr)
            {
                ConstructDefaultArrayElement(compiler, tv.TypeName, elemPtr, forceRoot);
            });
    }

llvm::Constant* MainListener::CoerceConstantToArrayElement(
        LLVMBackend* compiler, llvm::Constant* c, llvm::Type* elemTy,
        const LLVMBackend::TypeAndValue& sourceTV) {
        if (c->getType() == elemTy) return c;
        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(c))
        {
            if (elemTy->isIntegerTy())
            {
                const bool sourceUnsigned = sourceTV.IsUnsignedInteger() != -1
                    || compiler->ResolveTypeAlias(sourceTV.TypeName) == "bool"
                    || ci->getBitWidth() == 1;
                const unsigned destBits = elemTy->getIntegerBitWidth();
                const llvm::APInt value = sourceUnsigned
                    ? ci->getValue().zextOrTrunc(destBits)
                    : ci->getValue().sextOrTrunc(destBits);
                return llvm::ConstantInt::get(elemTy, value);
            }
            if (elemTy->isFloatingPointTy())
            {
                const bool sourceUnsigned = sourceTV.IsUnsignedInteger() != -1
                    || compiler->ResolveTypeAlias(sourceTV.TypeName) == "bool"
                    || ci->getBitWidth() == 1;
                return LLVMBackend::ConvertIntegerConstantToFloatingPoint(
                    ci, elemTy, sourceUnsigned);
            }
        }
        else if (auto* cf = llvm::dyn_cast<llvm::ConstantFP>(c))
        {
            if (elemTy->isFloatingPointTy())
            {
                llvm::APFloat v = cf->getValueAPF();
                bool lost = false;
                v.convert(elemTy->getFltSemantics(), llvm::APFloat::rmNearestTiesToEven, &lost);
                return llvm::ConstantFP::get(elemTy->getContext(), v);
            }
        }
        else if (c->isNullValue())
        {
            return llvm::Constant::getNullValue(elemTy);
        }
        return nullptr;
    }

void MainListener::EmitGlobalFixedArrayInit(
        const std::string& name,
        const LLVMBackend::TypeAndValue& tv,
        CFlatParser::InitializerListContext* initList,
        bool positionalArray,
        CFlatParser::DirectDeclaratorContext* direct,
        antlr4::ParserRuleContext* errCtx, bool threadLocal,
        bool hiddenBacking, llvm::ArrayType* typeOverride,
        llvm::GlobalVariable** backingResult) {
        auto* compiler = Compiler(errCtx);
        if (backingResult != nullptr) *backingResult = nullptr;
        if (RejectNestedNamedBraceInitializer(initList,
                "nested named brace initializer is not supported in global fixed-array elements")) return;
        auto* arrTy = typeOverride != nullptr ? typeOverride
            : llvm::dyn_cast<llvm::ArrayType>(compiler->GetType(tv));
        if (arrTy == nullptr)
        {
            LogErrorContext(errCtx, "invalid global fixed-array initializer type");
            return;
        }
        auto* elemTy = arrTy->getElementType();
        uint64_t n = tv.ConstArraySize;

        bool emptyInit = (initList == nullptr || initList->fieldInit().empty());
        llvm::Constant* arrConst = nullptr;
        const bool cxxReferenceAggregateArray = !emptyInit && positionalArray
            && !tv.Pointer && !tv.ElemPointer && !tv.IsAlias && !tv.IsArrayView
            && compiler->IsForeignCxxAggregateClass(tv.TypeName)
            && HasForeignCxxAggregateReferenceMember(tv.TypeName, errCtx);
        // Every element `{}` of a C++ class: the startup walk builds each one through the empty-brace
        // route (the default ctor, else the list ctor), so the stored constant is only zero storage.
        // The outer `{}` of a C++-class array is the same thing: every element value-initialized at startup.
        const bool cxxEmptyBraceElems = emptyInit
            ? (n != 0 && compiler->IsCxxRecord(tv.TypeName) && !tv.Pointer && !tv.ElemPointer && !tv.IsAlias
               && !tv.IsArrayView && tv.ConstInnerDimensions.empty())
            : (positionalArray && IsCxxEmptyBraceArrayInit(tv, initList));

        if (hiddenBacking && n != 0 && cxxReferenceAggregateArray)
            LogErrorContext(errCtx, std::format(
                "a global array view of '{}' cannot initialize elements that need a runtime constructor",
                SpellType(*compiler, tv)));
        if (hiddenBacking && n != 0 && cxxEmptyBraceElems
            && compiler->CxxElementNeedsDefaultConstruction(tv.TypeName))
            LogErrorContext(errCtx, std::format(
                "a global array view of '{}' cannot initialize elements that need a runtime constructor",
                SpellType(*compiler, tv)));
        if (hiddenBacking && n != 0 && emptyInit
            && compiler->GetFunction(tv.TypeName) != nullptr)
            LogErrorContext(errCtx, std::format(
                "a global array view of '{}' cannot initialize elements that need a runtime constructor",
                SpellType(*compiler, tv)));

        if (cxxReferenceAggregateArray)
            arrConst = llvm::Constant::getNullValue(arrTy);
        else if (emptyInit)
        {
            if (compiler->IsCxxRecord(tv.TypeName))
            {
                std::string ctorError;
                compiler->TryBindCxxImplicitDefaultCtor(tv.TypeName, ctorError);
                if (!ctorError.empty()) LogErrorContext(errCtx, ctorError);
                if (compiler->FindCxxDefaultCtor(tv.TypeName) == nullptr
                    && !compiler->CxxEmptyBraceUsesListCtor(tv.TypeName))
                {
                    std::string ambiguityError;
                    if (compiler->CxxDefaultCtorAmbiguityError(tv.TypeName, ambiguityError))
                        LogErrorContext(errCtx, ambiguityError);
                    LogErrorContext(errCtx, std::format(
                        "C++ class '{}' has no default constructor cflat can call",
                        compiler->DisplayCxxClassName(tv.TypeName)));
                }
            }
            // Empty `{}` -> zero-init, same as `= default`.
            arrConst = llvm::Constant::getNullValue(arrTy);
        }
        else if (positionalArray)
        {
            if (!tv.ConstInnerDimensions.empty())
            {
                std::vector<uint64_t> dimensions{ tv.ConstArraySize };
                dimensions.insert(dimensions.end(), tv.ConstInnerDimensions.begin(),
                                 tv.ConstInnerDimensions.end());
                uint64_t totalElements = 1;
                for (uint64_t dimension : dimensions) totalElements *= dimension;

                std::vector<llvm::ArrayType*> arrayTypes;
                llvm::Type* leafTy = arrTy;
                for (size_t depth = 0; depth < dimensions.size(); ++depth)
                {
                    auto* dimensionTy = llvm::dyn_cast<llvm::ArrayType>(leafTy);
                    if (dimensionTy == nullptr)
                    {
                        LogErrorContext(errCtx, "invalid nested fixed-array initializer type");
                        return;
                    }
                    arrayTypes.push_back(dimensionTy);
                    leafTy = dimensionTy->getElementType();
                }

                auto elements = initList->fieldInit();
                auto isEmptyBraceRow = [](CFlatParser::FieldInitContext* fi) {
                    return fi->Identifier() == nullptr && fi->assignmentExpression().empty()
                        && fi->initializerList() == nullptr;
                };
                const bool hasNested = std::any_of(elements.begin(), elements.end(),
                    [&](auto* fi) { return fi->initializerList() != nullptr || isEmptyBraceRow(fi); });
                const bool hasBracedOuterElement = std::any_of(elements.begin(), elements.end(),
                    [](auto* fi) { return fi->initializerList() != nullptr; });
                const bool hasElidedOuterElement = std::any_of(elements.begin(), elements.end(),
                    [](auto* fi) { return !fi->assignmentExpression().empty(); });
                if (hasBracedOuterElement && hasElidedOuterElement)
                {
                    LogErrorContext(initList,
                        "mixed braced and elided initializers are not supported for multi-dimensional fixed arrays");
                    return;
                }

                const bool charStringRows = dimensions.size() == 2 && leafTy->isIntegerTy(8)
                    && std::all_of(elements.begin(), elements.end(), [&](auto* fi) {
                        return isEmptyBraceRow(fi) || fi->assignmentExpression().size() == 1;
                    });
                std::vector<CFlatParser::FieldInitContext*> leaves;
                std::vector<size_t> leafIndices;
                if (charStringRows)
                {
                    if (elements.size() > dimensions.front())
                    {
                        LogErrorContext(initList, std::format(
                            "too many string rows for '{}[{}][{}]': got {} rows",
                            SpellType(*compiler, tv), dimensions.front(), dimensions.back(), elements.size()));
                        return;
                    }
                }
                else
                {
                    if (!hasNested)
                    {
                        LogErrorContext(initList, std::format(
                            "flat initializer lists are not supported for '{}[{}]'; use nested braces matching each dimension",
                            SpellType(*compiler, tv), tv.ConstArraySize));
                        return;
                    }
                    std::vector<std::pair<size_t,
                        std::vector<CFlatParser::FieldInitContext*>>> rows;
                    bool validRows = true;
                    std::function<void(CFlatParser::InitializerListContext*, size_t, size_t)>
                        collectRows = [&](CFlatParser::InitializerListContext* list,
                                          size_t depth, size_t rowOrdinal) {
                            const auto fields = list->fieldInit();
                            const size_t expected = dimensions[depth];
                            if (fields.size() > expected)
                            {
                                validRows = false;
                                LogErrorContext(list, std::format(
                                    "too many initializers at dimension {}: expected at most {}, got {}",
                                    depth + 1, expected, fields.size()));
                                return;
                            }
                            if (depth + 1 == dimensions.size())
                            {
                                rows.emplace_back(rowOrdinal, fields);
                                return;
                            }
                            size_t rowsPerElement = 1;
                            for (size_t inner = depth + 1; inner + 1 < dimensions.size(); ++inner)
                                rowsPerElement *= dimensions[inner];
                            for (size_t i = 0; i < fields.size(); ++i)
                            {
                                if (isEmptyBraceRow(fields[i]))
                                {
                                    for (size_t row = 0; row < rowsPerElement; ++row)
                                        rows.emplace_back(rowOrdinal + i * rowsPerElement + row,
                                            std::vector<CFlatParser::FieldInitContext*>{});
                                    continue;
                                }
                                if (fields[i]->initializerList() == nullptr)
                                {
                                    validRows = false;
                                    LogErrorContext(fields[i],
                                        "multi-dimensional fixed-array initializers require nested braces matching each dimension");
                                    return;
                                }
                                collectRows(fields[i]->initializerList(), depth + 1,
                                    rowOrdinal + i * rowsPerElement);
                                if (!validRows) return;
                            }
                            for (size_t i = fields.size(); i < expected; ++i)
                                for (size_t row = 0; row < rowsPerElement; ++row)
                                    rows.emplace_back(rowOrdinal + i * rowsPerElement + row,
                                        std::vector<CFlatParser::FieldInitContext*>{});
                        };
                    collectRows(initList, 0, 0);
                    if (!validRows) return;
                    const size_t rowWidth = dimensions.back();
                    for (const auto& [rowOrdinal, row] : rows)
                        for (size_t col = 0; col < row.size(); ++col)
                        {
                            const size_t slot = rowOrdinal * rowWidth + col;
                            if (slot >= totalElements)
                            {
                                LogErrorContext(initList,
                                    "multi-dimensional fixed-array initializers exceed the array bounds");
                                return;
                            }
                            leaves.push_back(row[col]);
                            leafIndices.push_back(slot);
                        }
                }

                LLVMBackend::TypeAndValue globalElemTV = tv;
                int globalElemTVStars = FixedArrayElementStars(tv);
                globalElemTV.Pointer = globalElemTVStars >= 1;
                globalElemTV.ElemPointer = globalElemTVStars >= 2;
                globalElemTV.ConstArraySize = 0;
                globalElemTV.ConstInnerDimensions.clear();
                globalElemTV.IsArrayView = false;
                llvm::Constant* leafDefault = llvm::Constant::getNullValue(leafTy);
                if (leafTy->isStructTy() && leaves.size() < totalElements
                    && !compiler->IsCxxRecord(tv.TypeName))
                {
                    LLVMBackend::DeclTypeAndValue defaultType;
                    static_cast<LLVMBackend::TypeAndValue&>(defaultType) = globalElemTV;
                    leafDefault = TryFoldGlobalDefaultConstruction(defaultType);
                    if (leafDefault == nullptr || leafDefault->getType() != leafTy)
                    {
                        LogErrorContext(errCtx, std::format(
                            "native global '{}' of type '{}' has a default construction that could not be reduced to a compile-time constant; "
                            "it must be constructed in main or made foldable",
                            name, tv.TypeName));
                        return;
                    }
                }

                LLVMBackend::BuilderStateGuard savedState(compiler);
                auto* voidTy = llvm::FunctionType::get(compiler->builder->getVoidTy(), false);
                auto* tmpFn = llvm::Function::Create(
                    voidTy, llvm::Function::PrivateLinkage, "__global_arr_init_tmp", compiler->module.get());
                auto* tmpBB = llvm::BasicBlock::Create(*compiler->context, "entry", tmpFn);
                compiler->builder->SetInsertPoint(tmpBB);

                std::vector<llvm::Constant*> leafConstants(totalElements, leafDefault);
                CFlatParser::FieldInitContext* codeValueElem = nullptr;
                size_t codeValueIndex = 0;
                CFlatParser::FieldInitContext* braceElem = nullptr;
                CFlatParser::FieldInitContext* globalError = nullptr;
                std::string globalErrorMessage;
                bool ok = true;

                if (charStringRows)
                {
                    const size_t rowWidth = dimensions.back();
                    for (size_t row = 0; row < elements.size(); ++row)
                    {
                        auto* fi = elements[row];
                        if (isEmptyBraceRow(fi)) continue;
                        auto nv = ParseAssignmentExpressionNamed(fi->assignmentExpression(0));
                        auto* literal = llvm::dyn_cast_or_null<llvm::Constant>(LoadNamedVariable(nv));
                        auto* global = literal == nullptr ? nullptr
                            : llvm::dyn_cast<llvm::GlobalVariable>(literal->stripPointerCasts());
                        auto* bytes = global == nullptr ? nullptr
                            : llvm::dyn_cast<llvm::ConstantDataSequential>(global->getInitializer());
                        if (bytes == nullptr || !compiler->IsStringLiteralConstant(literal))
                        {
                            globalError = fi;
                            globalErrorMessage = "multi-dimensional char arrays require string literal rows";
                            ok = false;
                            break;
                        }
                        std::string text = bytes->getAsString().str();
                        if (!text.empty() && text.size() - 1 > rowWidth)
                        {
                            globalError = fi;
                            globalErrorMessage = std::format(
                                "string row has {} characters but '{}' has width {}",
                                text.size() - 1, SpellType(*compiler, tv), rowWidth);
                            ok = false;
                            break;
                        }
                        for (size_t col = 0; col < rowWidth; ++col)
                        {
                            const uint8_t ch = col < text.size() ? (uint8_t)text[col] : 0;
                            leafConstants[row * rowWidth + col] = llvm::ConstantInt::get(leafTy, ch);
                        }
                    }
                }
                else
                {
                    for (size_t i = 0; i < leaves.size(); ++i)
                    {
                        auto* fi = leaves[i];
                        if (FieldInitIsBraceElement(fi) && cxxEmptyBraceElems) continue;
                        if (FieldInitIsBraceElement(fi))
                        {
                            braceElem = fi;
                            ok = false;
                            break;
                        }
                        if (fi->assignmentExpression().size() != 1)
                        {
                            globalError = fi;
                            globalErrorMessage =
                                "multi-dimensional fixed-array initializers require nested braces matching each dimension";
                            ok = false;
                            break;
                        }
                        auto nv = ParseAssignmentExpressionNamed(fi->assignmentExpression(0));
                        llvm::Value* val = LoadNamedVariable(nv);
                        if (const auto* gclo = globalElemTV.IsFunctionPointer
                                ? &globalElemTV : compiler->GetEncodedClosureType(globalElemTV.TypeName);
                            gclo && gclo->IsThinFnPtr() && !globalElemTV.Pointer)
                            val = compiler->BareFunctionToThinValue(val, gclo);
                        compiler->RejectImplicitIntegerPointeePointerConversion(nv.TypeAndValue, globalElemTV);
                        RejectConstDiscardingConversion(fi, nv, val, globalElemTV);
                        if (compiler->CodeValueIntoDataDestination(nv, globalElemTV))
                        {
                            codeValueElem = fi;
                            codeValueIndex = leafIndices[i];
                            ok = false;
                            break;
                        }
                        if (val != nullptr && leafTy == llvm::StructType::getTypeByName(*compiler->context, "string")
                            && val->getType() == cflat_llvm::PointerTo(compiler->builder->getInt8Ty()))
                        {
                            auto* lit = llvm::dyn_cast<llvm::Constant>(val);
                            if (lit != nullptr && compiler->IsStringLiteralConstant(lit))
                                val = compiler->WrapStringLiteralAsString(val);
                        }
                        auto* c = llvm::dyn_cast_or_null<llvm::Constant>(val);
                        if (c)
                        {
                            LLVMBackend::NativeBraceScope braceScope(*compiler);
                            compiler->RefuseImplicitFloatToInt(c, leafTy,
                                globalElemTV.IsUnsignedInteger() != -1);
                            compiler->RefuseNarrowingBraceConstant(c,
                                nv.TypeAndValue.IsUnsignedInteger() != -1, leafTy,
                                globalElemTV.IsUnsignedInteger() != -1);
                            c = CoerceConstantToArrayElement(compiler, c, leafTy, nv.TypeAndValue);
                        }
                        if (c == nullptr)
                        {
                            globalError = fi;
                            globalErrorMessage = leafTy->isStructTy()
                                ? std::format("a global array of '{}' can only be initialized from compile-time constants; "
                                    "this element needs a runtime constructor - assign it at startup, or use "
                                    "'const char*[]' for a static table of literals", SpellType(*compiler, tv))
                                : "global array initializer elements must be compile-time constants";
                            ok = false;
                            break;
                        }
                        leafConstants[leafIndices[i]] = c;
                    }
                }

                tmpFn->eraseFromParent();
                savedState.restore();
                if (globalError != nullptr) LogErrorContext(globalError, globalErrorMessage);
                if (braceElem != nullptr)
                    LogErrorContext(braceElem, std::format(
                        "field initializers ('{{field = value}}') are not supported for elements of the "
                        "global array '{}' of type '{}' at global scope; assign the elements at startup, "
                        "or declare the array as a local",
                        name, SpellType(*compiler, tv)));
                if (codeValueElem != nullptr)
                    LogErrorContext(codeValueElem, compiler->DescribeCodeValueIntoData(
                        CodeValueDestSpelling(globalElemTV), "brace-initialize",
                        CodeValueCastAdvice(globalElemTV),
                        std::format("element {} of '{}' of", codeValueIndex, name)));
                if (!ok) return;

                std::vector<llvm::Constant*> level = std::move(leafConstants);
                for (size_t depth = dimensions.size(); depth-- > 0;)
                {
                    const size_t width = dimensions[depth];
                    std::vector<llvm::Constant*> next;
                    next.reserve(level.size() / width);
                    for (size_t offset = 0; offset < level.size(); offset += width)
                    {
                        std::vector<llvm::Constant*> children(
                            level.begin() + (std::ptrdiff_t)offset,
                            level.begin() + (std::ptrdiff_t)(offset + width));
                        next.push_back(llvm::ConstantArray::get(arrayTypes[depth], children));
                    }
                    level = std::move(next);
                }
                arrConst = level.front();
            }
            else
            {
            auto elements = initList->fieldInit();
            if (elements.size() > n)
            {
                LogErrorContext(initList, std::format(
                    "too many initializers for '{}[{}]': got {} elements",
                    SpellType(*compiler, tv), n, elements.size()));
                return;
            }

            // Evaluate element expressions inside a throwaway function so stray IR does
            // not corrupt the current insert block; each must fold to a constant.
            LLVMBackend::BuilderStateGuard savedState(compiler);
            auto* voidTy = llvm::FunctionType::get(compiler->builder->getVoidTy(), false);
            auto* tmpFn = llvm::Function::Create(
                voidTy, llvm::Function::PrivateLinkage, "__global_arr_init_tmp", compiler->module.get());
            auto* tmpBB = llvm::BasicBlock::Create(*compiler->context, "entry", tmpFn);
            compiler->builder->SetInsertPoint(tmpBB);

            std::vector<llvm::Constant*> elems;
            elems.reserve(n);
            bool ok = true;
            // The ELEMENT's type is what a stored value has to agree with, not the array's.
            LLVMBackend::TypeAndValue globalElemTV = tv;
            int globalElemTVStars = FixedArrayElementStars(tv);
            globalElemTV.Pointer = globalElemTVStars >= 1;
            globalElemTV.ElemPointer = globalElemTVStars >= 2;
            globalElemTV.ConstArraySize = 0;
            globalElemTV.IsArrayView = false;
            auto foldGlobalInteger = [&](llvm::Value* value) -> llvm::Constant* {
                if (auto* constant = llvm::dyn_cast_or_null<llvm::Constant>(value))
                    return constant;
                if (hiddenBacking && value != nullptr && value->getType()->isIntegerTy())
                {
                    uint64_t folded = 0;
                    if (TryFoldConstInt(value, folded, &constFoldableGlobals_,
                                        &compiler->ImportedEnumeratorGlobals()))
                        return llvm::ConstantInt::get(value->getType(), folded,
                            /*IsSigned=*/false, /*ImplicitTrunc=*/true);
                }
                return nullptr;
            };
            // Code-value store reject, RECORDED not raised: LogErrorContext throws, and the
            // builder is redirected into a throwaway function here, so unwinding from inside the
            // loop would leave the insert point in `tmpFn` and skip the restore below.
            CFlatParser::FieldInitContext* codeValueElem = nullptr;
            size_t codeValueIndex = 0;
            // A braced element ('{field = v}') needs a runtime field-initializer, which a global
            // cannot have - the scalar spelling 'P g = {a = 1};' is rejected at global scope for
            // the same reason. RECORDED, not raised: the builder is redirected into tmpFn here.
            CFlatParser::FieldInitContext* braceElem = nullptr;
            CFlatParser::FieldInitContext* nonconstantStructElem = nullptr;
            bool namedFieldStructElem = false;
            bool needsRuntimeConstructorStructElem = false;
            auto needsGlobalRuntimeConstructor = [&](auto&& self,
                                                      const LLVMBackend::TypeAndValue& type,
                                                      int depth) -> bool {
                if (depth > 32 || type.Pointer || type.ElemPointer || type.IsAlias
                    || type.IsArrayView || type.IsFunctionPointer)
                    return false;
                const std::string typeName = compiler->ResolveTypeAlias(type.TypeName);
                if (compiler->IsCxxRecord(typeName))
                    return compiler->CxxElementNeedsDefaultConstruction(typeName);
                if (explicitNoArgConstructors_.count(typeName) != 0
                    || !compiler->CFlatStructHasOnlyNoArgCtor(typeName))
                    return true;
                const auto data = compiler->GetDataStructure(typeName);
                if (data.StructType == nullptr || data.IsUnion) return false;
                for (const auto& field : data.StructFields)
                {
                    if (field.Pointer || field.ElemPointer || field.IsAlias || field.IsArrayView
                        || field.IsFunctionPointer || field.IsSimd || field.IsBitfield
                        || field.IsPadding || field.TypeName == typeName)
                        continue;
                    if (self(self, field, depth + 1)) return true;
                }
                return false;
            };
            auto foldStructBrace = [&](auto&& self,
                                       const LLVMBackend::TypeAndValue& sourceTV,
                                       CFlatParser::InitializerListContext* fieldsList)
                                       -> llvm::Constant* {
                if (needsGlobalRuntimeConstructor(needsGlobalRuntimeConstructor, sourceTV, 0))
                {
                    needsRuntimeConstructorStructElem = true;
                    return nullptr;
                }
                if (fieldsList != nullptr)
                {
                    for (auto* fieldInit : fieldsList->fieldInit())
                    {
                        if (fieldInit->Identifier() != nullptr)
                        {
                            namedFieldStructElem = true;
                            return nullptr;
                        }
                    }
                }
                LLVMBackend::DeclTypeAndValue structTV;
                static_cast<LLVMBackend::TypeAndValue&>(structTV) = sourceTV;
                structTV.IsArrayView = false;
                structTV.ConstArraySize = 0;
                structTV.ConstInnerDimensions.clear();
                auto* structTy = llvm::dyn_cast_or_null<llvm::StructType>(compiler->GetType(structTV));
                auto data = compiler->GetDataStructure(structTV.TypeName);
                if (structTy == nullptr || data.StructType != structTy || data.IsUnion
                    || data.CxxOffsetLayout || !data.Interfaces.empty()
                    || compiler->IsCxxRecord(structTV.TypeName)
                    || data.StructFields.size() != structTy->getNumElements())
                    return nullptr;

                auto* base = TryFoldGlobalDefaultConstruction(structTV);
                if (base == nullptr || base->getType() != structTy) return nullptr;
                std::vector<llvm::Constant*> values;
                values.reserve(structTy->getNumElements());
                for (unsigned i = 0; i < structTy->getNumElements(); ++i)
                {
                    auto* value = base->getAggregateElement(i);
                    if (value == nullptr) return nullptr;
                    values.push_back(value);
                }
                if (fieldsList == nullptr || fieldsList->fieldInit().empty()) return base;

                size_t storageIndex = 0;
                for (auto* fieldInit : fieldsList->fieldInit())
                {
                    while (storageIndex < data.StructFields.size()
                        && data.StructFields[storageIndex].IsPadding)
                        ++storageIndex;
                    if (storageIndex >= data.StructFields.size()
                        || data.StructFields[storageIndex].IsBitfield
                        || fieldInit->Identifier() != nullptr)
                        return nullptr;

                    const auto& fieldTV = data.StructFields[storageIndex];
                    auto* fieldTy = structTy->getElementType((unsigned)storageIndex);
                    llvm::Constant* fieldValue = nullptr;
                    if (fieldInit->initializerList() != nullptr)
                    {
                        fieldValue = self(self, fieldTV, fieldInit->initializerList());
                    }
                    else if (fieldInit->assignmentExpression().size() == 1)
                    {
                        LLVMBackend::NamedVariable nv;
                        {
                            DeclExpectedTypeScope expectedScope(&declExpectedType, fieldTV, true);
                            nv = ParseAssignmentExpressionNamed(fieldInit->assignmentExpression(0));
                        }
                        BindNamedFunctionToFuncPtrSlot(compiler, nv, fieldTV, fieldInit);
                        llvm::Value* value = LoadNamedVariable(nv);
                        if (value != nullptr && fieldTV.TypeName == "string"
                            && value->getType() == cflat_llvm::PointerTo(compiler->builder->getInt8Ty()))
                            if (auto* literal = llvm::dyn_cast<llvm::Constant>(value);
                                literal != nullptr && compiler->IsStringLiteralConstant(literal))
                                value = compiler->WrapStringLiteralAsString(value);
                        if (value != nullptr && compiler->CodeValueIntoDataDestination(nv, fieldTV))
                            return nullptr;
                        if (value != nullptr)
                        {
                            compiler->RejectImplicitIntegerPointeePointerConversion(nv.TypeAndValue, fieldTV);
                            RejectConstDiscardingConversion(fieldInit, nv, value, fieldTV);
                            fieldValue = foldGlobalInteger(value);
                        }
                        if (fieldValue != nullptr)
                        {
                            LLVMBackend::NativeBraceScope braceScope(*compiler);
                            compiler->RefuseImplicitFloatToInt(fieldValue, fieldTy,
                                fieldTV.IsUnsignedInteger() != -1);
                            compiler->RefuseNarrowingBraceConstant(fieldValue,
                                nv.TypeAndValue.IsUnsignedInteger() != -1, fieldTy,
                                fieldTV.IsUnsignedInteger() != -1);
                            fieldValue = CoerceConstantToArrayElement(
                                compiler, fieldValue, fieldTy, nv.TypeAndValue);
                        }
                    }
                    else
                    {
                        LLVMBackend::DeclTypeAndValue defaultField;
                        static_cast<LLVMBackend::TypeAndValue&>(defaultField) = fieldTV;
                        fieldValue = TryFoldGlobalDefaultConstruction(defaultField);
                    }
                    if (fieldValue == nullptr || fieldValue->getType() != fieldTy) return nullptr;
                    values[storageIndex++] = fieldValue;
                }
                return llvm::ConstantStruct::get(structTy, values);
            };
            for (size_t i = 0; i < elements.size(); i++)
            {
                auto* fi = elements[i];
                if (FieldInitIsBraceElement(fi) && cxxEmptyBraceElems)
                {
                    elems.push_back(llvm::Constant::getNullValue(elemTy));
                    continue;
                }
                if (hiddenBacking && FieldInitIsBraceElement(fi) && elemTy->isStructTy())
                {
                    namedFieldStructElem = false;
                    auto* value = foldStructBrace(foldStructBrace, globalElemTV, fi->initializerList());
                    if (value == nullptr)
                    {
                        if (namedFieldStructElem)
                            braceElem = fi;
                        else
                            nonconstantStructElem = fi;
                        ok = false;
                        break;
                    }
                    elems.push_back(value);
                    continue;
                }
                if (FieldInitIsBraceElement(fi))
                {
                    braceElem = fi;
                    ok = false;
                    break;
                }
                auto nv = ParseAssignmentExpressionNamed(fi->assignmentExpression(0));
                llvm::Value* val = LoadNamedVariable(nv);
                // A named function element of a thin function<> array becomes its thin constant
                // (a Function or thunk bitcast) before folding, so it can join the initializer.
                if (const auto* gclo = globalElemTV.IsFunctionPointer
                        ? &globalElemTV : compiler->GetEncodedClosureType(globalElemTV.TypeName);
                    gclo && gclo->IsThinFnPtr() && !globalElemTV.Pointer)
                    val = compiler->BareFunctionToThinValue(val, gclo);
                compiler->RejectImplicitIntegerPointeePointerConversion(nv.TypeAndValue, globalElemTV);
                RejectConstDiscardingConversion(fi, nv, val, globalElemTV);
                if (codeValueElem == nullptr
                    && compiler->CodeValueIntoDataDestination(nv, globalElemTV))
                {
                    codeValueElem = fi;
                    codeValueIndex = i;
                    ok = false;
                    break;
                }
                // A `string` element written as a literal is a borrowed {ptr,len} pair: fold it
                // exactly as a scalar `string g = "x";` global does instead of rejecting it.
                if (val != nullptr && elemTy == llvm::StructType::getTypeByName(*compiler->context, "string")
                    && val->getType() == cflat_llvm::PointerTo(compiler->builder->getInt8Ty()))
                {
                    auto* lit = llvm::dyn_cast<llvm::Constant>(val);
                    if (lit != nullptr && compiler->IsStringLiteralConstant(lit))
                        val = compiler->WrapStringLiteralAsString(val);
                }
                auto* c = foldGlobalInteger(val);
                if (c)
                {
                    LLVMBackend::NativeBraceScope braceScope(*compiler);
                    compiler->RefuseImplicitFloatToInt(c, elemTy, globalElemTV.IsUnsignedInteger() != -1);
                    compiler->RefuseNarrowingBraceConstant(c, nv.TypeAndValue.IsUnsignedInteger() != -1,
                        elemTy, globalElemTV.IsUnsignedInteger() != -1);
                    c = CoerceConstantToArrayElement(compiler, c, elemTy, nv.TypeAndValue);
                }
                if (!c)
                {
                    if (elemTy->isStructTy()
                        && needsGlobalRuntimeConstructor(needsGlobalRuntimeConstructor,
                                                         globalElemTV, 0))
                        LogErrorContext(fi, std::format(
                            "a global array of '{}' can only be initialized from compile-time constants; "
                            "this element needs a runtime constructor - assign it at startup, or use "
                            "'const char*[]' for a static table of literals", SpellType(*compiler, tv)));
                    LogErrorContext(fi, "global array initializer elements must be compile-time constants");
                    ok = false;
                    break;
                }
                elems.push_back(c);
            }
            // Zero-fill unspecified trailing slots, matching local positional-init.
            for (size_t i = elements.size(); i < n; i++)
                elems.push_back(llvm::Constant::getNullValue(elemTy));

            tmpFn->eraseFromParent();
            savedState.restore();

            if (braceElem != nullptr)
                LogErrorContext(braceElem, std::format(
                    "field initializers ('{{field = value}}') are not supported for elements of the "
                    "global array '{}' of type '{}' at global scope; assign the elements at startup, "
                    "or declare the array as a local",
                    name, SpellType(*compiler, tv)));
            if (nonconstantStructElem != nullptr)
            {
                LLVMBackend::DeclTypeAndValue elemType;
                static_cast<LLVMBackend::TypeAndValue&>(elemType) = globalElemTV;
                elemType.ConstArraySize = 0;
                elemType.ConstInnerDimensions.clear();
                const bool needsRuntimeConstruction =
                    needsRuntimeConstructorStructElem
                    || compiler->CxxElementNeedsDefaultConstruction(tv.TypeName)
                    || TryFoldGlobalDefaultConstruction(elemType) == nullptr;
                if (needsRuntimeConstruction)
                    LogErrorContext(nonconstantStructElem, std::format(
                        "a global array of '{}' can only be initialized from compile-time constants; "
                        "this element needs a runtime constructor - assign it at startup, or use "
                        "'const char*[]' for a static table of literals", SpellType(*compiler, tv)));
                LogErrorContext(nonconstantStructElem,
                    "global array initializer elements must be compile-time constants");
            }
            if (codeValueElem != nullptr)
                LogErrorContext(codeValueElem, compiler->DescribeCodeValueIntoData(
                    CodeValueDestSpelling(globalElemTV), "brace-initialize",
                    CodeValueCastAdvice(globalElemTV),
                    std::format("element {} of '{}' of", codeValueIndex, name)));
            if (!ok) return;
            arrConst = llvm::ConstantArray::get(arrTy, elems);
            }
        }
        else
        {
            LogErrorContext(errCtx, std::format(
                "global array initializer for '{}' must be positional '{{v0, v1, ...}}' or empty '{{}}'; struct field-init '{{field=v}}' at global scope is not supported",
                SpellType(*compiler, tv)));
            return;
        }

        auto* globalArr = hiddenBacking
            ? new llvm::GlobalVariable(*compiler->module, arrTy, false,
                llvm::GlobalValue::InternalLinkage, arrConst,
                UniqueHiddenGlobalName(compiler, "backing"))
            : compiler->CreateGlobalVariable(tv, arrConst, threadLocal);
        if (hiddenBacking && threadLocal)
            globalArr->setThreadLocalMode(llvm::GlobalVariable::GeneralDynamicTLSModel);
        if (backingResult != nullptr) *backingResult = globalArr;
        const size_t pendingConstructionsBefore = pendingGlobalDefaultConstructions_.size()
            + pendingGlobalCxxConstructions_.size();
        if (cxxReferenceAggregateArray && globalArr != nullptr
            && !globalArr->isDeclaration())
        {
            LLVMBackend::DeclTypeAndValue declTv;
            static_cast<LLVMBackend::TypeAndValue&>(declTv) = tv;
            declTv.threadLocal = threadLocal;
            QueuePendingGlobalConstruction(pendingGlobalCxxConstructions_,
                {globalArr, declTv, direct});
            pendingGlobalCxxConstructions_.back().BraceInitializer = initList;
            pendingGlobalCxxConstructions_.back().CopyListInit = true;
        }
        // `S*[2] a = {&tls, ...}`: a thread_local address is per thread, so the array starts zero and
        // the value is stored at its turn in the ordered (module or per-thread) initializer.
        if (globalArr != nullptr && !globalArr->isDeclaration() && globalArr->hasInitializer()
            && ConstantNamesThreadLocal(globalArr->getInitializer()))
        {
            if (hiddenBacking)
                LogErrorContext(errCtx,
                    "global array-view initializer elements cannot refer to thread_local storage");
            llvm::Constant* value = globalArr->getInitializer();
            globalArr->setInitializer(llvm::Constant::getNullValue(globalArr->getValueType()));
            LLVMBackend::DeclTypeAndValue declTv;
            static_cast<LLVMBackend::TypeAndValue&>(declTv) = tv;
            declTv.threadLocal = threadLocal;
            QueuePendingGlobalConstruction(pendingGlobalCxxConstructions_, {globalArr, declTv, direct});
            pendingGlobalCxxConstructions_.back().StoreValue = value;
        }
        // Every element is value-initialized at startup (a zero constant is only the storage).
        if (cxxEmptyBraceElems && globalArr != nullptr && !hiddenBacking)
        {
            LLVMBackend::DeclTypeAndValue declTv;
            static_cast<LLVMBackend::TypeAndValue&>(declTv) = tv;
            declTv.threadLocal = threadLocal;
            QueuePendingGlobalConstruction(pendingGlobalCxxConstructions_, {globalArr, declTv, direct, false, true, true});
        }
        // A native struct with a default (field defaults or a CFlat constructor): `T[N] g = {};`
        // default-constructs each element like `T[N] g;`, so the zero constant above is replaced.
        if (!hiddenBacking && emptyInit && globalArr != nullptr && !compiler->IsCxxRecord(tv.TypeName)
            && !tv.Pointer && !tv.ElemPointer && !tv.IsAlias && !tv.IsArrayView
            && compiler->GetFunction(tv.TypeName) != nullptr)
        {
            LLVMBackend::DeclTypeAndValue declTv;
            static_cast<LLVMBackend::TypeAndValue&>(declTv) = tv;
            declTv.threadLocal = threadLocal;
            QueuePendingGlobalConstruction(pendingGlobalDefaultConstructions_, {globalArr, declTv, direct});
        }

        if (!hiddenBacking && pendingConstructionsBefore == pendingGlobalDefaultConstructions_.size()
                + pendingGlobalCxxConstructions_.size())
            MarkConstGlobalReadOnly(globalArr, tv);

        if (hiddenBacking) return;

        // Register the global for LSP/--symbol, matching the scalar global path.
        if (auto* s = compiler->GetSymbolSink())
        {
            int dl = (int)direct->getStart()->getLine();
            int dc = (int)direct->getStart()->getCharPositionInLine();
            s->RegisterVariable(name, tv.TypeName, compiler->GetSourceFilePath(), dl, dc,
                                SpellTypeForSymbolDump(*compiler, tv));
            s->Register(SymbolKind::Variable, name, compiler->GetSourceFilePath(), dl, dc,
                        std::format("{}[{}] {}", SpellTypeForSymbolDump(*compiler, tv), n, name), {}, name);
        }
    }

void MainListener::EmitGlobalArrayViewInferredInit(
        const std::string& name,
        const LLVMBackend::TypeAndValue& tv,
        CFlatParser::InitializerListContext* initList,
        CFlatParser::DirectDeclaratorContext* direct,
        antlr4::ParserRuleContext* errCtx, bool threadLocal) {
        auto* compiler = Compiler(errCtx);
        std::vector<CFlatParser::FieldInitContext*> elements;
        if (initList != nullptr) elements = initList->fieldInit();

        if (elements.size() > (size_t)INT32_MAX)
            LogErrorContext(initList != nullptr ? static_cast<antlr4::ParserRuleContext*>(initList) : errCtx,
                "array size exceeds the maximum supported element count");
        for (auto* fi : elements)
            if (fi->Identifier() != nullptr
                || (fi->assignmentExpression().size() != 1 && !FieldInitIsBraceElement(fi)))
                LogErrorContext(fi, std::format(
                    "'{}' uses a positional initializer list; named or 'key: value' elements are not allowed",
                    SpellType(*compiler, tv)));

        LLVMBackend::TypeAndValue backingTV = tv;
        backingTV.IsArrayView = false;
        backingTV.Pointer = tv.ElemPointer;
        backingTV.ElemPointer = false;
        backingTV.ConstArraySize = elements.size();
        backingTV.ConstInnerDimensions.clear();
        llvm::Type* elementTy = compiler->GetType(backingTV);
        auto* backingArrayTy = elements.empty()
            ? llvm::ArrayType::get(elementTy, 0)
            : llvm::dyn_cast<llvm::ArrayType>(compiler->GetType(backingTV));
        if (backingArrayTy == nullptr)
        {
            LogErrorContext(errCtx, "invalid global array-view backing type");
            return;
        }

        llvm::GlobalVariable* backing = nullptr;
        EmitGlobalFixedArrayInit(name, backingTV, initList, !elements.empty(), direct, errCtx,
            threadLocal, true, backingArrayTy, &backing);
        if (backing == nullptr) return;

        auto* indexTy = compiler->builder->getInt32Ty();
        llvm::Constant* zero = llvm::ConstantInt::get(indexTy, 0);
        llvm::Constant* indices[] = { zero, zero };
        auto* dataPtr = llvm::ConstantExpr::getInBoundsGetElementPtr(
            backingArrayTy, backing, indices);

        auto* viewGlobal = threadLocal
            ? compiler->CreateGlobalVariable(tv, nullptr, true)
            : compiler->CreateGlobalVariable(tv, dataPtr, false);
        if (viewGlobal == nullptr) return;
        if (threadLocal)
        {
            LLVMBackend::DeclTypeAndValue declTV;
            static_cast<LLVMBackend::TypeAndValue&>(declTV) = tv;
            declTV.threadLocal = true;
            QueuePendingGlobalConstruction(pendingGlobalCxxConstructions_,
                {viewGlobal, declTV, direct});
            pendingGlobalCxxConstructions_.back().StoreValue = dataPtr;
        }

        auto* lengthTy = compiler->builder->getInt64Ty();
        auto* lengthGlobal = llvm::dyn_cast_or_null<llvm::GlobalVariable>(
            compiler->GetGlobalVariableNV(name).RawArrayLengthStorage);
        if (lengthGlobal == nullptr)
            lengthGlobal = new llvm::GlobalVariable(*compiler->module, lengthTy, false,
                llvm::GlobalValue::InternalLinkage,
                llvm::ConstantInt::get(lengthTy, elements.size()),
                UniqueHiddenGlobalName(compiler, "length"));
        else
            lengthGlobal->setInitializer(llvm::ConstantInt::get(lengthTy, elements.size()));
        if (threadLocal)
            lengthGlobal->setThreadLocalMode(llvm::GlobalVariable::GeneralDynamicTLSModel);
        viewGlobal->setMetadata("cflat.array-view.length",
            llvm::MDNode::get(*compiler->context, llvm::ValueAsMetadata::get(lengthGlobal)));
        compiler->SetRawArrayLengthKnown(name, true);
        compiler->SetViewOfFixedArrayStorage(name, true, "brace initializer backing storage");

        if (auto* sink = compiler->GetSymbolSink())
        {
            const int line = (int)direct->getStart()->getLine();
            const int column = (int)direct->getStart()->getCharPositionInLine();
            sink->RegisterVariable(name, tv.TypeName, compiler->GetSourceFilePath(), line, column,
                                   SpellTypeForSymbolDump(*compiler, tv));
            sink->Register(SymbolKind::Variable, name, compiler->GetSourceFilePath(), line, column,
                           std::format("{} {}", SpellTypeForSymbolDump(*compiler, tv), name), {}, name);
        }
    }

void MainListener::AdmitConstFloatingGlobal(LLVMBackend* compiler,
                                            const std::string& sourceName,
                                            llvm::GlobalVariable* global)
{
    if (compiler == nullptr || global == nullptr || !global->hasInitializer()
        || !llvm::isa<llvm::ConstantFP>(global->getInitializer()))
        return;
    const std::string llvmName = global->getName().str();
    if (!compiler->IsConstGlobalFP(sourceName) && !compiler->IsConstGlobalFP(llvmName)) return;
    constFoldableGlobals_.insert(sourceName);
    constFoldableGlobals_.insert(llvmName);
}

bool MainListener::TryFoldConstInt(llvm::Value* v, uint64_t& out,
                                const std::unordered_set<std::string>* constGlobals,
                                const std::unordered_set<std::string>* extraGlobals) {
        // Values are carried SIGN-extended to 64 bits (getSExtValue) so the signed ICmp cases
        // below can reinterpret `out` as int64_t correctly for negative constants. For same-width
        // operands, sign-extension preserves the unsigned ordering too, so the unsigned predicates
        // stay correct; ZExt casts re-zero the upper bits from the source width (see below).
        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(v))
        {
            out = (uint64_t)ci->getSExtValue();
            return true;
        }
        if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(v))
        {
            if (auto* gv = llvm::dyn_cast<llvm::GlobalVariable>(ld->getPointerOperand()))
                if (gv->hasInitializer())
                    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(gv->getInitializer()))
                    {
                        if (constGlobals != nullptr
                            && constGlobals->count(std::string(gv->getName())) == 0
                            && (extraGlobals == nullptr
                                || extraGlobals->count(std::string(gv->getName())) == 0))
                            return false;
                        out = (uint64_t)ci->getSExtValue();
                        return true;
                    }
            return false;
        }
        auto fpValue = [&](llvm::Value* value) -> std::optional<llvm::APFloat> {
            if (auto* cf = llvm::dyn_cast<llvm::ConstantFP>(value)) return cf->getValueAPF();
            if (auto* load = llvm::dyn_cast<llvm::LoadInst>(value))
                if (auto* global = llvm::dyn_cast<llvm::GlobalVariable>(load->getPointerOperand()))
                    if (global->hasInitializer())
                        if (auto* cf = llvm::dyn_cast<llvm::ConstantFP>(global->getInitializer()))
                        {
                            if (constGlobals != nullptr
                                && constGlobals->count(std::string(global->getName())) == 0
                                && (extraGlobals == nullptr
                                    || extraGlobals->count(std::string(global->getName())) == 0))
                                return std::nullopt;
                            return cf->getValueAPF();
                        }
            return std::nullopt;
        };
        if (auto* bo = llvm::dyn_cast<llvm::BinaryOperator>(v))
        {
            uint64_t l = 0, r = 0;
            if (!bo->getType()->isIntegerTy()
                || !TryFoldConstInt(bo->getOperand(0), l, constGlobals, extraGlobals)
                || !TryFoldConstInt(bo->getOperand(1), r, constGlobals, extraGlobals))
                return false;
            // Fold at the instruction's own width and signedness (leaves are carried sign-extended
            // to 64 bits), then carry the W-bit result sign-extended again.
            const unsigned W = bo->getType()->getIntegerBitWidth();
            auto widen = [W](uint64_t x) {
                llvm::APInt wide(64, x);
                return W <= 64 ? wide.trunc(W) : wide.sext(W);
            };
            const llvm::APInt a = widen(l), b = widen(r);
            llvm::APInt res(W, 0);
            switch (bo->getOpcode())
            {
                case llvm::Instruction::Add:  res = a + b; break;
                case llvm::Instruction::Sub:  res = a - b; break;
                case llvm::Instruction::Mul:  res = a * b; break;
                // Division by zero and INT_MIN / -1 are undefined: not a constant.
                case llvm::Instruction::SDiv:
                    if (b.isZero() || (a.isMinSignedValue() && b.isAllOnes())) return false;
                    res = a.sdiv(b); break;
                case llvm::Instruction::SRem:
                    if (b.isZero() || (a.isMinSignedValue() && b.isAllOnes())) return false;
                    res = a.srem(b); break;
                case llvm::Instruction::UDiv: if (b.isZero()) return false; res = a.udiv(b); break;
                case llvm::Instruction::URem: if (b.isZero()) return false; res = a.urem(b); break;
                // An over-shift (count >= W) is poison: not a constant.
                case llvm::Instruction::Shl:
                    if (b.uge(W)) return false;
                    res = a.shl((unsigned)b.getZExtValue()); break;
                // Right over-shifts keep their historical deterministic fold: LShr -> 0, AShr ->
                // the sign fill.
                case llvm::Instruction::LShr:
                    res = b.uge(W) ? llvm::APInt(W, 0) : a.lshr((unsigned)b.getZExtValue()); break;
                case llvm::Instruction::AShr:
                    res = a.ashr(b.uge(W) ? W - 1 : (unsigned)b.getZExtValue()); break;
                case llvm::Instruction::And:  res = a & b; break;
                case llvm::Instruction::Or:   res = a | b; break;
                case llvm::Instruction::Xor:  res = a ^ b; break;
                default: return false;
            }
            if (W > 64 && !res.isSignedIntN(64)) return false;
            out = (uint64_t)res.sextOrTrunc(64).getSExtValue();
            return true;
        }
        if (auto* cast = llvm::dyn_cast<llvm::CastInst>(v))
        {
            if ((cast->getOpcode() == llvm::Instruction::FPToSI
                    || cast->getOpcode() == llvm::Instruction::FPToUI)
                && cast->getDestTy()->isIntegerTy())
            {
                auto value = fpValue(cast->getOperand(0));
                if (!value) return false;
                const bool isUnsigned = cast->getOpcode() == llvm::Instruction::FPToUI;
                const unsigned destBits = cast->getDestTy()->getIntegerBitWidth();
                llvm::APSInt converted(destBits, isUnsigned);
                bool isExact = false;
                const auto status = value->convertToInteger(
                    converted, llvm::APFloat::rmTowardZero, &isExact);
                if (status & llvm::APFloat::opInvalidOp) return false;
                if (isUnsigned)
                {
                    if (destBits > 64 && converted.getActiveBits() > 64) return false;
                    out = converted.zextOrTrunc(64).getZExtValue();
                }
                else
                {
                    if (destBits > 64 && !converted.isSignedIntN(64)) return false;
                    out = (uint64_t)converted.sextOrTrunc(64).getSExtValue();
                }
                return true;
            }
            // SExt passes the sign-extended operand through unchanged (SExt of a value
            // already sign-extended from its own width is a no-op at 64 bits).
            if (cast->getOpcode() == llvm::Instruction::SExt)
                return TryFoldConstInt(cast->getOperand(0), out, constGlobals, extraGlobals);
            // Trunc keeps the low bits of the destination width, carried sign-extended.
            if (cast->getOpcode() == llvm::Instruction::Trunc && cast->getDestTy()->isIntegerTy())
            {
                if (!TryFoldConstInt(cast->getOperand(0), out, constGlobals, extraGlobals)) return false;
                const unsigned dstBits = cast->getDestTy()->getIntegerBitWidth();
                if (dstBits < 64)
                    out = (uint64_t)llvm::APInt(64, out).trunc(dstBits).sext(64).getZExtValue();
                return true;
            }
            // ZExt must discard the sign bits the leaf fold added: re-zero above the source width.
            if (cast->getOpcode() == llvm::Instruction::ZExt)
            {
                if (!TryFoldConstInt(cast->getOperand(0), out, constGlobals, extraGlobals)) return false;
                unsigned srcBits = cast->getSrcTy()->getIntegerBitWidth();
                if (srcBits < 64) out &= ((uint64_t)1 << srcBits) - 1;
                return true;
            }
            return false;
        }
        if (auto* cmp = llvm::dyn_cast<llvm::ICmpInst>(v))
        {
            uint64_t l = 0, r = 0;
            if (!TryFoldConstInt(cmp->getOperand(0), l, constGlobals, extraGlobals) || !TryFoldConstInt(cmp->getOperand(1), r, constGlobals, extraGlobals))
                return false;
            bool res = false;
            // Signed predicates reinterpret the raw bits as int64_t, matching the uint64
            // arithmetic convention used above for the other opcodes.
            switch (cmp->getPredicate())
            {
                case llvm::CmpInst::ICMP_EQ:  res = (l == r); break;
                case llvm::CmpInst::ICMP_NE:  res = (l != r); break;
                case llvm::CmpInst::ICMP_UGT: res = (l > r); break;
                case llvm::CmpInst::ICMP_UGE: res = (l >= r); break;
                case llvm::CmpInst::ICMP_ULT: res = (l < r); break;
                case llvm::CmpInst::ICMP_ULE: res = (l <= r); break;
                case llvm::CmpInst::ICMP_SGT: res = ((int64_t)l >  (int64_t)r); break;
                case llvm::CmpInst::ICMP_SGE: res = ((int64_t)l >= (int64_t)r); break;
                case llvm::CmpInst::ICMP_SLT: res = ((int64_t)l <  (int64_t)r); break;
                case llvm::CmpInst::ICMP_SLE: res = ((int64_t)l <= (int64_t)r); break;
                default: return false;
            }
            out = res ? 1 : 0;
            return true;
        }
        if (auto* cmp = llvm::dyn_cast<llvm::FCmpInst>(v))
        {
            // Floating globals fold only when admitted, as the integer LoadInst path requires.
            auto lhs = fpValue(cmp->getOperand(0));
            auto rhs = fpValue(cmp->getOperand(1));
            if (!lhs || !rhs) return false;
            const auto relation = lhs->compare(*rhs);
            const bool unordered = relation == llvm::APFloat::cmpUnordered;
            const bool equal = relation == llvm::APFloat::cmpEqual;
            const bool less = relation == llvm::APFloat::cmpLessThan;
            const bool greater = relation == llvm::APFloat::cmpGreaterThan;
            bool res = false;
            switch (cmp->getPredicate())
            {
                case llvm::CmpInst::FCMP_FALSE: res = false; break;
                case llvm::CmpInst::FCMP_OEQ: res = !unordered && equal; break;
                case llvm::CmpInst::FCMP_OGT: res = !unordered && greater; break;
                case llvm::CmpInst::FCMP_OGE: res = !unordered && (greater || equal); break;
                case llvm::CmpInst::FCMP_OLT: res = !unordered && less; break;
                case llvm::CmpInst::FCMP_OLE: res = !unordered && (less || equal); break;
                case llvm::CmpInst::FCMP_ONE: res = !unordered && !equal; break;
                case llvm::CmpInst::FCMP_ORD: res = !unordered; break;
                case llvm::CmpInst::FCMP_UNO: res = unordered; break;
                case llvm::CmpInst::FCMP_UEQ: res = unordered || equal; break;
                case llvm::CmpInst::FCMP_UGT: res = unordered || greater; break;
                case llvm::CmpInst::FCMP_UGE: res = unordered || greater || equal; break;
                case llvm::CmpInst::FCMP_ULT: res = unordered || less; break;
                case llvm::CmpInst::FCMP_ULE: res = unordered || less || equal; break;
                case llvm::CmpInst::FCMP_UNE: res = unordered || !equal; break;
                case llvm::CmpInst::FCMP_TRUE: res = true; break;
                default: return false;
            }
            out = res ? 1 : 0;
            return true;
        }
        if (auto* sel = llvm::dyn_cast<llvm::SelectInst>(v))
        {
            uint64_t c = 0;
            if (!TryFoldConstInt(sel->getCondition(), c, constGlobals, extraGlobals))
                return false;
            return TryFoldConstInt(c ? sel->getTrueValue() : sel->getFalseValue(), out, constGlobals, extraGlobals);
        }
        if (auto* phi = llvm::dyn_cast<llvm::PHINode>(v))
        {
            // A ternary join (two arms, one decider): fold only when the decider's condition folds,
            // then take the arm on the edge that condition selects.
            if (phi->getNumIncomingValues() != 2) return false;
            llvm::CondBrInst* decider = nullptr;
            bool takenWhenTrue[2] = {false, false};
            for (unsigned i = 0; i < 2; ++i)
            {
                // Walk single-predecessor edges up to the conditional branch that picks this arm.
                llvm::BasicBlock* blk = phi->getIncomingBlock(i);
                llvm::CondBrInst* found = nullptr;
                if (!llvm::isa<llvm::UncondBrInst>(blk->getTerminator())) return false;
                while (blk != nullptr && found == nullptr)
                {
                    llvm::BasicBlock* pred = blk->getSinglePredecessor();
                    if (pred == nullptr) return false;
                    if (auto* cb = llvm::dyn_cast<llvm::CondBrInst>(pred->getTerminator()))
                    {
                        if (cb->getSuccessor(0) == phi->getParent() || cb->getSuccessor(1) == phi->getParent())
                            return false;
                        if (cb->getSuccessor(0) == blk) takenWhenTrue[i] = true;
                        else if (cb->getSuccessor(1) == blk) takenWhenTrue[i] = false;
                        else return false;
                        found = cb;
                    }
                    else if (llvm::isa<llvm::UncondBrInst>(pred->getTerminator())) blk = pred;
                    else return false;
                }
                if (found == nullptr || (decider != nullptr && decider != found)) return false;
                decider = found;
            }
            if (takenWhenTrue[0] == takenWhenTrue[1]) return false;
            uint64_t c = 0;
            if (!TryFoldConstInt(decider->getCondition(), c, constGlobals, extraGlobals))
                return false;
            const unsigned taken = ((c != 0) == takenWhenTrue[0]) ? 0 : 1;
            return TryFoldConstInt(phi->getIncomingValue(taken), out, constGlobals, extraGlobals);
        }
        return false;
    }

llvm::ConstantInt* MainListener::EvalGlobalArrayDim(CFlatParser::AssignmentExpressionContext* expr) {
        auto* folded = TryFoldArrayDim(expr);
        auto* i64Ty = llvm::Type::getInt64Ty(*Compiler(expr)->context);
        if (folded == nullptr)
        {
            LogErrorContext(expr, "global array size must be a compile-time constant");
            return llvm::ConstantInt::get(i64Ty, 1);
        }
        return folded;
    }

void MainListener::ValidateArrayDimValue(antlr4::ParserRuleContext* expr, llvm::Value* value) {
        auto* ci = llvm::dyn_cast_or_null<llvm::ConstantInt>(value);
        if (ci == nullptr) return;
        const int64_t size = ci->getSExtValue();
        if (size <= 0)
            LogErrorContext(expr, "array size must be greater than zero");
        if (size > INT32_MAX)
            LogErrorContext(expr, "array size exceeds the maximum supported element count");
    }

void MainListener::ValidateArrayTotalElements(antlr4::ParserRuleContext* expr, uint64_t outer,
                                              const std::vector<uint64_t>& inner) {
        // Each dimension is range-checked on its own; a product past INT32_MAX (the single-dimension
        // cap) overflows the byte size (LLVM "invalid number of bytes"). A zero or oversized
        // dimension was already diagnosed, so it is skipped here to avoid a second error.
        if (inner.empty() || outer == 0 || outer > (uint64_t)INT32_MAX) return;
        uint64_t total = outer;
        for (uint64_t d : inner)
        {
            if (d == 0 || d > (uint64_t)INT32_MAX) return;
            if (total > (uint64_t)INT32_MAX / d)
            {
                LogErrorContext(expr, "array size exceeds the maximum supported element count");
                return;
            }
            total *= d;
        }
    }

llvm::Value* MainListener::FoldArrayDimValue(antlr4::ParserRuleContext* expr, llvm::Value* value) {
        // A function name, pointer or float is not a size (it reached the alloca and failed module
        // verification); size 1 keeps the declaration well-formed after the error, as EvalGlobalArrayDim does.
        if (value != nullptr && !value->getType()->isIntegerTy())
        {
            LogErrorContext(expr, std::format("array dimension '{}' must have an integer type",
                                              expr->getText()));
            return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*Compiler(expr)->context), 1);
        }
        uint64_t folded = 0;
        // Only admitted (immutable) globals fold; a mutable global keeps the run-time size.
        if (!TryFoldConstInt(value, folded, &constFoldableGlobals_,
                             &Compiler(expr)->ImportedEnumeratorGlobals()))
        {
            ValidateArrayDimValue(expr, value);
            return value;
        }
        auto* i64Ty = llvm::Type::getInt64Ty(*Compiler(expr)->context);
        auto* constant = llvm::ConstantInt::get(i64Ty, folded);
        ValidateArrayDimValue(expr, constant);
        return constant;
    }

llvm::ConstantInt* MainListener::TryFoldArrayDim(CFlatParser::AssignmentExpressionContext* expr,
                                                 bool allowEmission) {
        auto* compiler = Compiler(expr);
        // Macros and imported enumerators fold here (and only here), see IsFoldingArrayDim.
        compiler->SetFoldingArrayDim(true);
        auto constFold = FoldCompileTimeInt(compiler, expr);
        compiler->SetFoldingArrayDim(false);
        if (constFold)
        {
            if (*constFold <= 0)
                LogErrorContext(expr, "array size must be greater than zero");
            if (*constFold > INT32_MAX)
                LogErrorContext(expr, "array size exceeds the maximum supported element count");
            auto* i64Ty = llvm::Type::getInt64Ty(*compiler->context);
            return llvm::ConstantInt::get(i64Ty, (uint64_t)*constFold);
        }
        if (!allowEmission) return nullptr;
        LLVMBackend::BuilderStateGuard savedState(compiler);
        auto* voidTy = llvm::FunctionType::get(compiler->builder->getVoidTy(), false);
        auto* tmpFn = llvm::Function::Create(
            voidTy, llvm::Function::PrivateLinkage, "__global_arr_dim_tmp", compiler->module.get());
        auto* tmpBB = llvm::BasicBlock::Create(*compiler->context, "entry", tmpFn);
        compiler->builder->SetInsertPoint(tmpBB);
        // `&&` and `?:` create blocks in currentFunction; at file scope it is null, which left the
        // blocks detached (a segfault in the next load). Attach them to the throwaway function.
        auto* savedFn = compiler->currentFunction;
        compiler->currentFunction = tmpFn;

        llvm::Value* sizeVal = ParseAssignmentExpression(expr);
        uint64_t folded = 0;
        bool ok = sizeVal && TryFoldConstInt(sizeVal, folded, &constFoldableGlobals_,
                                             &compiler->ImportedEnumeratorGlobals());

        compiler->currentFunction = savedFn;
        tmpFn->eraseFromParent();
        savedState.restore();

        if (!ok) return nullptr;
        const int64_t signedFolded = (int64_t)folded;
        if (signedFolded <= 0)
            LogErrorContext(expr, "array size must be greater than zero");
        if (signedFolded > INT32_MAX)
            LogErrorContext(expr, "array size exceeds the maximum supported element count");
        auto* i64Ty = llvm::Type::getInt64Ty(*compiler->context);
        return llvm::ConstantInt::get(i64Ty, folded);
    }

void MainListener::EmitArrayViewInferredInit(
        const std::string& name,
        const LLVMBackend::TypeAndValue& tv,
        CFlatParser::InitializerListContext* initList,
        size_t line,
        std::vector<std::pair<std::string, llvm::AllocaInst*>>& allocList) {
        auto* compiler = Compiler(initList);
        if (RejectNestedNamedBraceInitializer(initList,
                "nested named brace initializer is not supported for inferred array-view elements")) return;
        std::vector<CFlatParser::FieldInitContext*> elements;
        if (initList != nullptr)
            elements = initList->fieldInit();
        if (elements.empty())
        {
            compiler->LogError(std::format(
                "cannot infer the length of '{}' from an empty initializer list; use an explicit size '{}[N]'",
                SpellType(*compiler, tv), SpellType(*compiler, tv)));
            return;
        }

        // Every element must be positional (no `field =` or `key: value`).
        for (auto* fi : elements)
        {
            if (fi->Identifier() != nullptr
                || (fi->assignmentExpression().size() != 1 && !FieldInitIsBraceElement(fi)))
            {
                LogErrorContext(fi, std::format(
                    "'{}' uses a positional initializer list; named or 'key: value' elements are not allowed",
                    SpellType(*compiler, tv)));
                return;
            }
        }

        // The element type is T (or T* when the declaration was T*[]): strip the
        // array-view-ness off a copy of the declared type.
        LLVMBackend::TypeAndValue elemTV = tv;
        elemTV.IsArrayView = false;
        elemTV.Pointer = tv.ElemPointer;
        elemTV.ElemPointer = false;
        elemTV.ConstArraySize = 0;
        llvm::Type* elemTy = compiler->GetType(elemTV);

        uint64_t n = elements.size();
        auto* arrTy = llvm::ArrayType::get(elemTy, n);
        auto* backing = compiler->AllocaAtEntry(arrTy, nullptr, "arrview_backing");

        llvm::Value* zero = compiler->builder->getInt32(0);
        for (size_t i = 0; i < elements.size(); i++)
        {
            auto* fi = elements[i];
            llvm::Value* idx = compiler->builder->getInt32((uint32_t)i);
            auto* elemPtr = compiler->builder->CreateInBoundsGEP(
                arrTy, backing, { zero, idx }, "arrview_elem");
            if (FieldInitIsBraceElement(fi))
            {
                auto* elemStructTy = llvm::dyn_cast<llvm::StructType>(elemTy);
                if (elemStructTy == nullptr)
                {
                    LogErrorContext(fi, std::format(
                        "nested brace initializer elements of '{}' require a struct element type",
                        SpellType(*compiler, tv)));
                    continue;
                }
                EmitBraceElementIntoFixedSlot(elemPtr, elemStructTy, elemTV.TypeName, fi);
                continue;
            }
            LLVMBackend::NamedVariable nv;
            {
                DeclExpectedTypeScope elementExpectedScope(&declExpectedType, elemTV, true);
                nv = ParseAssignmentExpressionNamed(fi->assignmentExpression(0));
            }
            BindNamedFunctionToFuncPtrSlot(compiler, nv, elemTV, fi);
            if (std::string why = compiler->FuncPtrValueOwnershipReason(nv, elemTV); !why.empty())
                LogErrorContext(fi, std::format(
                    "incompatible function pointer initializer: {}", why));
            llvm::Value* val = LoadNamedVariable(nv);
            if (!val) continue;

            if (RejectImplicitPrimitiveToPointer(
                    fi, elemTV, nv, val, "brace-initialize",
                    std::format("element {} of '{}'", i, name)))
                continue;

            if (RejectImplicitPointerToNumber(
                    fi, elemTV, nv, val, "brace-initialize",
                    std::format("element {} of '{}'", i, name)))
                continue;
            if (RejectConstDiscardingConversion(fi, nv, val, elemTV))
                continue;

            // Aggregate leg of the code-value store gate: `Rec*[] v = { w, w };`.
            RejectCodeValueIntoDataSlot(fi, nv, elemTV, "brace-initialize",
                                        std::format("element {} of '{}' of", i, name));
            // A string LITERAL is a 'const char*', never a 'T*' - the ELEMENT leg of the same gate
            // the scalar store sites apply. The element's type is what a stored value must agree
            // with, so this reads elemTV, not the array's own type.
            RejectStringLiteralIntoStructPointer(fi, elemTV, val,
                                        std::format("element {} of '{}'", i, name));

            // Thin sibling of the widen elsewhere: array-view brace-init into a thin
            // function<> element fed a raw pointer (spelled or generic-encoded).
            {
                const LLVMBackend::TypeAndValue* clo = elemTV.IsFunctionPointer
                    ? &elemTV : compiler->GetEncodedClosureType(elemTV.TypeName);
                if (clo && clo->IsThinFnPtr() && !elemTV.Pointer && elemTV.ConstArraySize == 0
                    && val->getType()->isPointerTy() && !val->getType()->isStructTy())
                {
                    compiler->CheckThinFnPtrAssignProvenance(val, nv,
                        std::format("'{}[{}]'", name, i));
                    val = compiler->BareFunctionToThinValue(val, clo);
                }
            }

            // Array-VIEW twin of the fixed-array leg above; a separate lowering reached by
            // neither the fixed path nor EmitOneFieldInit.
            GuardOwningTempUniqueFieldEscape(
                nv, std::format("element {} of array '{}'", i, name), fi);

            if (tv.TypeName == "string"
                && val->getType() == cflat_llvm::PointerTo(compiler->builder->getInt8Ty()))
            {
                LLVMBackend::NamedVariable tmp = nv;
                CoerceElementToString(compiler, tmp, val, fi);
            }
            // Interface element: same boxing/rebox the struct-field brace path applies.
            if (!elemTV.Pointer && compiler->IsInterfaceType(elemTV.TypeName))
                val = CoerceInitValueToInterface(nv, val, elemTV.TypeName, fi);

            compiler->CreateAssignment(val, elemPtr,
                nv.TypeAndValue.IsUnsignedInteger() != -1, elemTy, elemTV.IsUnsignedInteger() != -1);
        }

        // Create the T* local and point it at element 0 of the backing array.
        auto* alloc = compiler->CreateLocalVariable(tv, nullptr, nullptr, line);
        allocList.push_back(std::pair(name, llvm::dyn_cast<llvm::AllocaInst>(alloc)));
        auto* elem0 = compiler->builder->CreateInBoundsGEP(arrTy, backing, { zero, zero }, "arrview_data");
        compiler->builder->CreateStore(elem0, alloc);
        auto& view = compiler->GetOrCreateStackVariable(name);
        compiler->StoreRawArrayLength(view, compiler->builder->getInt64(n));
        compiler->SetRawArrayLengthStateForStorage(view.Storage,
            LLVMBackend::RawArrayLengthState{ true, true, {} });
        compiler->SetViewOfFixedArrayStorage(name, true, "brace initializer backing storage");
    }

bool MainListener::TryEmitContainerInitializer(
        llvm::Value* alloc,
        const LLVMBackend::TypeAndValue& tv,
        CFlatParser::InitializerListContext* initList) {
        auto* compiler = Compiler(initList);
        CallPostfixScope initializerPostfix(this);
        const std::string& typeName = tv.TypeName;
        const std::string displayTypeName = SpellType(*compiler, tv);

        const bool isList  = MangledBase(typeName) == "list";
        const bool isArray = MangledBase(typeName) == "array";
        const bool isDict  = MangledBase(typeName) == "dictionary";
        if (!isList && !isArray && !isDict)
            return false;
        if (RejectNestedNamedBraceInitializer(initList,
                "nested named brace initializer is not supported in container elements")) return true;

        auto elements = initList->fieldInit();

        // self NV referencing the (already default-constructed) container storage.
        // Rebuilt per call since CreateOverloadedFunctionCall may consume/null move args.
        auto makeSelf = [&]() {
            LLVMBackend::NamedVariable selfNV;
            selfNV.Storage = alloc;
            selfNV.TypeAndValue.TypeName = typeName;
            return selfNV;
        };

        // Parse one element expression into a NamedVariable, coercing string literals to
        // `string` when the target element type is string. Returns false on a parse error.
        auto parseElement = [&](CFlatParser::AssignmentExpressionContext* ax,
                                const std::string& elemType,
                                LLVMBackend::NamedVariable& outNV) -> bool {
            outNV = ParseAssignmentExpressionNamed(ax);
            llvm::Value* val = LoadNamedVariable(outNV);
            if (!val)
                return false;
            if (elemType == "string")
                CoerceElementToString(compiler, outNV, val, ax);
            // Overload resolution falls back to BaseType when TypeName is empty (e.g. a bare
            // literal expression); make sure one of them is populated or it dereferences null.
            else if (outNV.TypeAndValue.TypeName.empty() && outNV.BaseType == nullptr)
                outNV.BaseType = val->getType();
            // Elements are forwarded positionally into add()/set(). A bare-identifier element
            // (e.g. `list<int> a = { x }`) parses with VariableName set to the identifier, which
            // MatchFunction would treat as a NAMED argument ("named argument 'x' does not match
            // any parameter"). Clear it so every element is passed positionally. (Same idiom as
            // the forwarding-wrapper path above.)
            outNV.TypeAndValue.VariableName = "";
            return true;
        };

        if (isDict)
        {
            // dictionary<K,V>: every element must be `key: value`.
            // Extract K and V from the mangled name `dictionary$K$V` when it splits cleanly
            // into exactly two parts (covers primitives and string); otherwise skip coercion.
            std::string keyType, valType;
            {
                TypeSpelling dictionary;
                if (DemangleType(*compiler, typeName, dictionary)
                    && dictionary.base == "dictionary" && dictionary.args.size() == 2)
                {
                    keyType = MangleType(*compiler, dictionary.args[0]);
                    valType = MangleType(*compiler, dictionary.args[1]);
                }
            }
            for (auto* fi : elements)
            {
                if (fi->Identifier() != nullptr || fi->assignmentExpression().size() != 2)
                {
                    LogErrorContext(fi, std::format(
                        "dictionary initializer requires 'key: value' pairs for type '{}'",
                        SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = typeName })));
                    continue;
                }
                LLVMBackend::NamedVariable keyNV, valNV;
                if (!parseElement(fi->assignmentExpression(0), keyType, keyNV)) continue;
                if (!parseElement(fi->assignmentExpression(1), valType, valNV)) continue;
                initializerPostfix.Flush();
                compiler->CreateOverloadedFunctionCall("set", { makeSelf(), keyNV, valNV });
            }
            return true;
        }

        // list<T> / array<T>: every element must be positional.
        TypeSpelling containerSpelling;
        if (!DemangleType(*compiler, typeName, containerSpelling)
            || containerSpelling.args.size() != 1)
            return true;
        const std::string elemType = MangleType(*compiler, containerSpelling.args[0]);

        for (auto* fi : elements)
        {
            if (fi->Identifier() != nullptr || fi->assignmentExpression().size() != 1)
            {
                LogErrorContext(fi, std::format(
                    "'{}' uses a positional initializer list; named or 'key: value' elements are not allowed",
                    displayTypeName));
                return true;
            }
        }

        if (isArray)
        {
            // array<T> needs an explicit allocation before set(): x.init(N).
            LLVMBackend::NamedVariable countNV;
            countNV.Primary = compiler->builder->getInt32((uint32_t)elements.size());
            countNV.TypeAndValue.TypeName = "int";
            compiler->CreateOverloadedFunctionCall("init", { makeSelf(), countNV });

            for (size_t i = 0; i < elements.size(); i++)
            {
                LLVMBackend::NamedVariable elemNV;
                if (!parseElement(elements[i]->assignmentExpression(0), elemType, elemNV)) continue;
                LLVMBackend::NamedVariable idxNV;
                idxNV.Primary = compiler->builder->getInt32((uint32_t)i);
                idxNV.TypeAndValue.TypeName = "int";
                initializerPostfix.Flush();
                compiler->CreateOverloadedFunctionCall("set", { makeSelf(), idxNV, elemNV });
            }
            return true;
        }

        // list<T>: append each element in order.
        for (auto* fi : elements)
        {
            LLVMBackend::NamedVariable elemNV;
            if (!parseElement(fi->assignmentExpression(0), elemType, elemNV)) continue;
            initializerPostfix.Flush();
            compiler->CreateOverloadedFunctionCall("add", { makeSelf(), elemNV });
        }
        return true;
    }

void MainListener::ArmArrayNewDesugar(antlr4::tree::ParseTree* rhs, const LLVMBackend::TypeAndValue& target) {
        arrayNewDesugarCtx = nullptr;
        if (rhs == nullptr || target.Pointer || target.IsArrayView) return;
        auto* ne = AsDirectNew(rhs);
        if (ne == nullptr || ne->assignmentExpression() == nullptr) return;
        auto* compiler = Compiler();
        // `auto p = new T[n];` deduces the owning `array<T>`, not a raw `T*`: a raw pointer with
        // a hidden count is the escape hatch and has to be spelled `T* p = new T[n];`.
        if (target.TypeName == "auto")
        {
            std::string elem = NewExpressionElementTypeName(ne);
            std::string mangled = MangledGenericName("array", { elem });
            QueueGenericInstantiation("array", { elem }, mangled);
            arrayNewDesugarCtx = ne;
            arrayNewDesugarTarget = {};
            arrayNewDesugarTarget.TypeName = mangled;
            return;
        }
        if (!compiler->IsCoreArrayType(target.TypeName)) return;
        arrayNewDesugarCtx = ne;
        arrayNewDesugarTarget = target;
        arrayNewDesugarTarget.VariableName.clear();
    }


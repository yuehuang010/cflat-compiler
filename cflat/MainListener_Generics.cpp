#include "MainListener.h"

void MainListener::InstantiateGenericInterface(const std::string& baseName, const std::string& mangledName,
                                     const std::unordered_map<std::string, std::string>& substitutions,
                                     const std::unordered_map<std::string, std::vector<std::string>>& packSubstitutions) {
        if (instantiatedInterfaces.count(mangledName)) return;
        instantiatedInterfaces.insert(mangledName);

        auto templateIt = genericInterfaceTemplates.find(baseName);
        if (templateIt == genericInterfaceTemplates.end()) return;

        auto* ctx = compilerLLVM->MaterializeGenericInterface(baseName);
        if (!ctx) return;

        TemplateNamespaceScope nsScope(compilerLLVM, baseName);

        // Apply substitutions to instantiate the interface methods
        auto savedSubst = activeTypeSubstitutions;
        auto savedPackSubst = activePackSubstitutions;
        for (const auto& [k, v] : substitutions)
            activeTypeSubstitutions[k] = v;
        for (const auto& [k, v] : packSubstitutions)
            activePackSubstitutions[k] = v;

        // Resolve and materialize generic parents after the child substitutions are active.
        // BaseSpecifierName intentionally drops the parent's type arguments, so passing that
        // bare name to CreateInterfaceDefinition loses the concrete parent instance.
        std::vector<std::string> parentNames;
        for (auto* spec : ctx->baseSpecifier())
        {
            std::string parentBase = Compiler()->ResolveGenericBaseAlias(BaseSpecifierName(spec));
            if (spec->genericTypeParameters() == nullptr)
            {
                parentNames.push_back(Compiler()->ResolveInterfaceName(parentBase));
                continue;
            }

            std::vector<std::string> parentArgs;
            for (auto* entry : spec->genericTypeParameters()->typeParameterList()->typeParameterEntry())
                parentArgs.push_back(ResolveTypeArgEntry(entry));
            std::string parentMangled = MangledGenericName(parentBase, parentArgs);
            parentNames.push_back(parentMangled);

            auto parentIt = genericInterfaceTemplates.find(parentBase);
            if (parentIt != genericInterfaceTemplates.end())
            {
                std::unordered_map<std::string, std::string> parentSubst;
                std::unordered_map<std::string, std::vector<std::string>> parentPackSubst;
                const auto& parentParams = genericInterfaceTypeParams[parentBase];
                auto packIt = genericInterfacePackIndex.find(parentBase);
                size_t packIndex = packIt != genericInterfacePackIndex.end()
                    ? packIt->second : std::string::npos;
                if (packIndex == std::string::npos)
                {
                    for (size_t i = 0; i < parentParams.size() && i < parentArgs.size(); i++)
                        parentSubst[parentParams[i]] = parentArgs[i];
                }
                else
                {
                    for (size_t i = 0; i < packIndex && i < parentArgs.size(); i++)
                        parentSubst[parentParams[i]] = parentArgs[i];
                    parentPackSubst[parentParams[packIndex]] =
                        std::vector<std::string>(parentArgs.begin() + packIndex, parentArgs.end());
                }
                InstantiateGenericInterface(parentBase, parentMangled, parentSubst, parentPackSubst);
            }
        }

        // Entered after the substitutions are installed: this instantiation must re-evaluate the
        // if-const conditions under its own T, not reuse another instantiation's branch selection.
        ResolvedInterfaceMembersScope memberScope(resolvedInterfaceMembers_, (const void*)ctx);

        std::vector<LLVMBackend::InterfaceMethod> methods;
        for (auto method : InterfaceMethods(ctx))
        {
            if (RejectVariadicInterfaceMethod(mangledName, method)) continue;
            LLVMBackend::InterfaceMethod m;
            m.ReturnType = ParseDeclarationSpecifiers(method->declarationSpecifiers());
            m.Name = getInterfaceMethodName(method);
            auto declParams = ParseParameterTypeList(method->parameterTypeList());
            for (const auto& p : declParams)
            {
                LLVMBackend::TypeAndValue tv = p;
                m.Parameters.push_back(tv);
            }
            methods.push_back(std::move(m));
        }

        auto fields = ParseInterfaceFields(ctx);
        activeTypeSubstitutions = savedSubst;
        activePackSubstitutions = savedPackSubst;
        Compiler()->CreateInterfaceDefinition(mangledName, parentNames, methods, fields);
    }

std::string MainListener::GenericMethodOwner(const std::string& baseName, CFlatParser::FunctionDefinitionContext* tmplCtx) {
        size_t dot = baseName.rfind('.');
        if (dot == std::string::npos || tmplCtx == nullptr || isFunctionStatic(tmplCtx))
            return {};
        std::string owner = baseName.substr(0, dot);
        // Struct nesting and namespace nesting share ONE dotted key space, so the prefix alone
        // cannot say which this is. The RECORDED declaring namespace can: when it equals the
        // prefix, "Ov.f" is a FREE template in `namespace Ov`, never a method on `struct Ov`.
        if (Compiler()->IsGenericFunctionKeyInNamespace(baseName, owner))
            return {};
        return Compiler()->IsDataStructure(owner) ? owner : std::string{};
    }

std::string MainListener::GenericMethodTemplateKey(const std::string& receiverType, const std::string& methodName) {
        if (receiverType.empty() || methodName.empty())
            return {};
        std::string key = receiverType + "." + methodName;
        return genericFunctionTemplates.count(key) ? key : std::string{};
    }

bool MainListener::IsFollowedByDot(CFlatParser::PostfixExpressionContext* ctx, antlr4::tree::ParseTree* child) {
        const auto& children = ctx->children;
        for (size_t i = 0; i + 1 < children.size(); i++)
        {
            if (children[i] != child)
                continue;
            auto* next = dynamic_cast<antlr4::tree::TerminalNode*>(children[i + 1]);
            return next != nullptr && next->getSymbol()->getType() == CFlatParser::Dot;
        }
        return false;
    }

std::string MainListener::InstantiateGenericFunction(const std::string& baseName, const std::vector<std::string>& typeArgs) {
        std::string mangledName = MangledGenericName(baseName, typeArgs);
        if (instantiatedGenericFunctions.count(mangledName)) return mangledName;
        instantiatedGenericFunctions.insert(mangledName);

        auto templateIt = genericFunctionTemplates.find(baseName);
        if (templateIt == genericFunctionTemplates.end()) return {};
        auto* tmplCtx = compilerLLVM->MaterializeGenericFunction(baseName);
        if (!tmplCtx) return {};

        // The body is re-walked long after the declaring namespace's scope closed, so install it:
        // without this a sibling name inside the body resolves globally (or not at all).
        TemplateNamespaceScope nsScope(compilerLLVM, baseName);

        const auto& typeParams = genericFunctionTypeParams[baseName];
        auto packIt = genericFunctionPackIndex.find(baseName);
        size_t packIdx = packIt != genericFunctionPackIndex.end()
            ? packIt->second : std::string::npos;
        if ((packIdx == std::string::npos && typeParams.size() != typeArgs.size())
            || (packIdx != std::string::npos && typeArgs.size() < packIdx))
            return {};

        if (!CheckConstraints(baseName, typeParams, typeArgs, genericFunctionConstraints, tmplCtx))
            return {};

        auto savedSubst = activeTypeSubstitutions;
        auto savedPackSubst = activePackSubstitutions;
        if (packIdx == std::string::npos)
        {
            for (size_t i = 0; i < typeParams.size(); i++)
                activeTypeSubstitutions[typeParams[i]] = typeArgs[i];
        }
        else
        {
            for (size_t i = 0; i < packIdx; i++)
                activeTypeSubstitutions[typeParams[i]] = typeArgs[i];
            activePackSubstitutions[typeParams[packIdx]] =
                std::vector<std::string>(typeArgs.begin() + packIdx, typeArgs.end());
            if (!activePackSubstitutions[typeParams[packIdx]].empty())
                activeTypeSubstitutions[typeParams[packIdx]] =
                    activePackSubstitutions[typeParams[packIdx]].front();
        }

        // A generic member method is keyed "Owner.method". An instance method must be
        // emitted as a member (implicit `this` param) so its body can reach the owner's
        // fields; a static one keeps the free-function shape.
        std::string ownerStruct = GenericMethodOwner(baseName, tmplCtx);

        // Save the current IRBuilder insertion point so that emitting a new
        // function definition mid-block does not corrupt the caller's block.
        auto* instCompiler = Compiler(tmplCtx);
        LLVMBackend::BuilderStateGuard savedState(instCompiler);
        // Isolate the outer function's local-variable stack: if we are
        // instantiating mid-emission of another function, its frames are still
        // on stackNamedVariable, and the generic body's identifier lookup would
        // otherwise reach past its own frame and bind to the outer's locals
        // (e.g. parameter 'a' shadowed by outer 'int a' -> cross-function load
        // and "Instruction does not dominate all uses" verifier error).
        auto savedStack = std::move(instCompiler->stackNamedVariable);
        instCompiler->stackNamedVariable.clear();
        ParseFunctionDefinition(tmplCtx, ownerStruct, {}, mangledName, DeclaringNamespaceOf(compilerLLVM, baseName));
        instCompiler->stackNamedVariable = std::move(savedStack);
        savedState.restore();

        activeTypeSubstitutions = savedSubst;
        activePackSubstitutions = savedPackSubst;
        return mangledName;
    }

std::string MainListener::InferAndInstantiateGenericFunction(const std::string& funcName, const std::string& receiverType) {
        auto templateIt = genericFunctionTemplates.find(funcName);
        if (templateIt == genericFunctionTemplates.end()) return {};

        auto* funcCtx = compilerLLVM->MaterializeGenericFunction(funcName);
        if (!funcCtx) return {};
        const auto& typeParams = genericFunctionTypeParams[funcName];

        auto* paramTypeList = funcCtx->parameterTypeList();
        if (!paramTypeList || !paramTypeList->parameterList()) return {};

        auto paramDecls = paramTypeList->parameterList()->parameterDeclaration();
        if (paramDecls.empty()) return {};

        // Examine the first parameter's type specifier to determine the base interface name
        for (auto* declSpec : paramDecls[0]->declarationSpecifiers()->declarationSpecifier())
        {
            auto* typeSpec = declSpec->typeSpecifier();
            if (!typeSpec || !typeSpec->genericIdentifier()) continue;

            auto* genId = typeSpec->genericIdentifier();
            if (!genId->Identifier()) continue;

            std::string baseName = genId->Identifier()->getText();
            std::string prefix = baseName + "__";

            if (receiverType.size() <= prefix.size() || receiverType.substr(0, prefix.size()) != prefix)
                continue;

            std::string suffix = receiverType.substr(prefix.size());

            // With a single type parameter the entire suffix is the type argument.
            // With multiple type parameters split on "__" (works for simple non-generic args).
            std::vector<std::string> typeArgs;
            if (typeParams.size() == 1)
            {
                typeArgs = { suffix };
            }
            else
            {
                size_t pos = 0;
                while (pos < suffix.size())
                {
                    size_t next = suffix.find("__", pos);
                    if (next == std::string::npos)
                    {
                        typeArgs.push_back(suffix.substr(pos));
                        break;
                    }
                    typeArgs.push_back(suffix.substr(pos, next - pos));
                    pos = next + 2;
                }
            }

            if (typeArgs.size() == typeParams.size())
                return InstantiateGenericFunction(funcName, typeArgs);
        }
        return {};
    }

std::string MainListener::TryInferAndInstantiateFromArgs(const std::string& funcName,
                                               const std::vector<LLVMBackend::NamedVariable>& args) {
        auto templateIt = genericFunctionTemplates.find(funcName);
        if (templateIt == genericFunctionTemplates.end()) return {};

        auto* funcCtx = compilerLLVM->MaterializeGenericFunction(funcName);
        if (!funcCtx) return {};
        const auto& typeParams = genericFunctionTypeParams[funcName];
        if (typeParams.empty()) return {};

        auto* paramTypeList = funcCtx->parameterTypeList();
        if (!paramTypeList || !paramTypeList->parameterList()) return {};
        auto paramDecls = paramTypeList->parameterList()->parameterDeclaration();

        std::unordered_map<std::string, std::string> inferred;
        for (size_t i = 0; i < paramDecls.size() && i < args.size(); i++)
        {
            std::string paramTypeName;
            for (auto* ds : paramDecls[i]->declarationSpecifiers()->declarationSpecifier())
            {
                auto* ts = ds->typeSpecifier();
                if (!ts || !ts->genericIdentifier()) continue;
                auto* gid = ts->genericIdentifier();
                if (gid->Identifier()) { paramTypeName = gid->Identifier()->getText(); break; }
            }
            for (const auto& tp : typeParams)
            {
                if (paramTypeName == tp)
                {
                    std::string argType = args[i].TypeAndValue.TypeName;
                    // Free-function calls drop TypeName for signed-int args (call site only
                    // preserves it for unsigned ints). Fall back to the LLVM BaseType so
                    // numeric literals like 3 and (i64)10 still infer cleanly.
                    if (argType.empty() && args[i].BaseType != nullptr)
                        argType = Compiler()->LlvmTypeToTypeName(args[i].BaseType);
                    if (!argType.empty()) inferred[tp] = argType;
                    break;
                }
            }
        }

        if (inferred.size() != typeParams.size()) return {};

        std::vector<std::string> typeArgs;
        for (const auto& tp : typeParams)
        {
            auto it = inferred.find(tp);
            if (it == inferred.end()) return {};
            typeArgs.push_back(it->second);
        }
        return InstantiateGenericFunction(funcName, typeArgs);
    }

LLVMBackend::TypeAndValue MainListener::BuildFuncPtrAliasType(CFlatParser::FunctionPointerSpecifierContext* fpSpec) {
        LLVMBackend::TypeAndValue tv;
        tv.IsFunctionPointer = true;
        // `function` = thin C ptr ("__c_fn_ptr"); `Lambda` = fat ("__closure_fat_ptr"). IsThinFnPtr() derives from this.
        tv.TypeName = (fpSpec->Function() != nullptr) ? "__c_fn_ptr" : "__closure_fat_ptr";
        if (fpSpec->typeSpecifier() != nullptr)
        {
            // Resolve generic signature types (gap b) so `using Cb = Lambda<list<string>()>` stores
            // the mangled "list__string" and queues the instantiation.
            bool retPtr = fpSpec->pointer() != nullptr;
            int retStars = PointerDepthOf(fpSpec->pointer());
            tv.FuncPtrReturnTypeName = ResolveSigComponentCodegen(fpSpec->typeSpecifier(), retPtr);
            tv.FuncPtrReturnPointer  = retPtr;
            if (auto* qualifier = fpSpec->functionReturnQualifier(); qualifier != nullptr)
            {
                std::string text = qualifier->getText();
                if (text != "move" && text != "alias")
                    LogErrorContext(qualifier, "function return qualifier must be 'move' or 'alias'");
                tv.FuncPtrReturnOwned = text == "move";
                tv.FuncPtrReturnAlias = text == "alias";
            }
            tv.FuncPtrReturnPointerDepth = ReconcilePointerDepth(retPtr, retStars);
            tv.FuncPtrReturnResolvedKey = SigComponentResolvedKey(tv.FuncPtrReturnTypeName);
            if (fpSpec->functionPointerParamList() != nullptr)
                for (auto* param : fpSpec->functionPointerParamList()->functionPointerParam())
                {
                    LLVMBackend::TypeAndValue::FuncPtrParam p;
                    bool pPtr = param->pointer() != nullptr;
                    int pStars = PointerDepthOf(param->pointer());
                    p.TypeName = ResolveSigComponentCodegen(param->typeSpecifier(), pPtr);
                    p.Pointer  = pPtr;
                    p.IsMove   = param->Move() != nullptr;
                    p.PointerDepth = ReconcilePointerDepth(pPtr, pStars);
                    p.ResolvedTypeKey = SigComponentResolvedKey(p.TypeName);
                    tv.FuncPtrParams.push_back(p);
                }
        }
        return tv;
    }

void MainListener::QueueInstantiateGenericType(CFlatParser::DeclarationSpecifiersContext* declSpec) {
        for (auto declSpecItem : declSpec->declarationSpecifier())
        {
            auto typeSpec = declSpecItem->typeSpecifier();
            if (!typeSpec) continue;

            // Tuple type sugar: (T1, T2) -> tuple<T1, T2>
            if (typeSpec->tupleTypeSpecifier() != nullptr)
            {
                auto* tts = typeSpec->tupleTypeSpecifier();
                // Pack-only form (T...) resolved during instantiation - skip queuing here
                if (tts->tupleTypePackEntry() != nullptr)
                    break;
                std::vector<std::string> typeArgs;
                for (auto* entry : tts->tupleTypeEntry())
                    typeArgs.push_back(TupleEntryArgName(Compiler(declSpec), entry));
                std::string mangledName = MangledGenericName("tuple", typeArgs);
                tupleTypeArgs[mangledName] = typeArgs;
                if (!instantiatedGenerics.count(mangledName))
                {
                    pendingInstantiations.push_back({"tuple", typeArgs, mangledName});
                    instantiatedGenerics.insert(mangledName);
                }
                break;
            }

            std::string baseName;
            auto* genParams = GenericSpecOf(typeSpec, baseName);
            if (genParams == nullptr)
                continue;

            // This is a generic type instantiation
            baseName = Compiler()->ResolveGenericBaseAlias(baseName);
            std::vector<std::string> typeArgs;
            // Resolve via ResolveTypeArgEntry so active type-parameter substitutions are
            // applied (e.g. channel<T> -> channel__int inside an instantiated generic body).
            // Without this, the literal "T" is queued (channel__T), which later fails to
            // instantiate with "unknown type 'T'". No-op for already-concrete args.
            for (auto* entry : genParams->typeParameterList()->typeParameterEntry())
                typeArgs.push_back(ResolveTypeArgEntry(entry));

            std::string mangledName = MangledGenericName(baseName, typeArgs);

            // Queue the instantiation instead of doing it immediately
            if (!instantiatedGenerics.count(mangledName))
            {
                pendingInstantiations.push_back({baseName, typeArgs, mangledName});
                instantiatedGenerics.insert(mangledName);
            }
            break;
        }
    }

std::string MainListener::MangledGenericName(const std::string& baseName, const std::vector<std::string>& typeArgs) {
        std::string name = baseName;
        for (const auto& arg : typeArgs)
            name += "__" + MangleTypeArg(Compiler(), arg);
        return name;
    }

void MainListener::ProcessPendingInstantiations() {
        while (!pendingInstantiations.empty())
        {
            // Interface instantiations drain FIRST: a struct layout naming Container<int> in a
            // field (list<Container<int>>._data) needs interfaceTable to already hold it.
            size_t pick = pendingInstantiations.size() - 1;
            for (size_t i = pendingInstantiations.size(); i-- > 0; )
                if (Compiler()->IsGenericInterfaceTemplateName(pendingInstantiations[i].templateName))
                {
                    pick = i;
                    break;
                }
            auto pending = pendingInstantiations[pick];
            pendingInstantiations.erase(pendingInstantiations.begin() + pick);

            // Now safe to instantiate
            auto structIt = genericStructTemplates.find(pending.templateName);
            auto classIt = genericClassTemplates.find(pending.templateName);
            auto ifaceIt = genericInterfaceTemplates.find(pending.templateName);
            if (structIt == genericStructTemplates.end() && classIt == genericClassTemplates.end())
            {
                if (ifaceIt != genericInterfaceTemplates.end())
                {
                    // It's a generic interface - instantiate it (pack-aware)
                    std::unordered_map<std::string, std::string> ifaceSubst;
                    std::unordered_map<std::string, std::vector<std::string>> ifacePackSubst;
                    const auto& ifaceTypeParams = genericInterfaceTypeParams[pending.templateName];
                    auto packIdxIt = genericInterfacePackIndex.find(pending.templateName);
                    size_t packIdx = (packIdxIt != genericInterfacePackIndex.end()) ? packIdxIt->second : std::string::npos;
                    if (packIdx == std::string::npos)
                    {
                        for (size_t i = 0; i < ifaceTypeParams.size() && i < pending.typeArgs.size(); i++)
                            ifaceSubst[ifaceTypeParams[i]] = pending.typeArgs[i];
                    }
                    else
                    {
                        for (size_t i = 0; i < packIdx && i < pending.typeArgs.size(); i++)
                            ifaceSubst[ifaceTypeParams[i]] = pending.typeArgs[i];
                        ifacePackSubst[ifaceTypeParams[packIdx]] =
                            std::vector<std::string>(pending.typeArgs.begin() + packIdx, pending.typeArgs.end());
                    }
                    InstantiateGenericInterface(pending.templateName, pending.mangledName, ifaceSubst, ifacePackSubst);
                }
                else if (compilerLLVM->InstantiateWinrtGenericInterface(
                             pending.templateName, pending.typeArgs, pending.mangledName))
                {
                    // Resolved as an imported parameterized WinRT interface (IVector<int>, ...):
                    // a concrete COM vtable + thin pointer + derived PIID were synthesized.
                }
                else if (Compiler()->IsVerbose())
                {
                    std::cout << "[verbose]   skip instantiation '" << pending.mangledName
                              << "': template '" << pending.templateName << "' not found\n";
                }
                continue;
            }

            if (Compiler()->IsVerbose())
                std::cout << "[verbose]   instantiate generic: " << pending.mangledName << "\n";

            const auto& typeParams = genericStructTypeParams[pending.templateName];

            // Materialize the template context (lazy-parses cached source on first use),
            // then verify where-clause constraints before instantiating.
            bool isStruct = structIt != genericStructTemplates.end();
            auto* structCtx = isStruct ? compilerLLVM->MaterializeGenericStruct(pending.templateName) : nullptr;
            auto* classCtx  = isStruct ? nullptr : compilerLLVM->MaterializeGenericClass(pending.templateName);
            auto* ctxForError = isStruct
                ? (antlr4::ParserRuleContext*)structCtx
                : (antlr4::ParserRuleContext*)classCtx;
            if (!ctxForError) continue;
            const auto& constraintMap = isStruct
                ? genericStructConstraints : genericClassConstraints;
            if (!CheckConstraints(pending.templateName, typeParams, pending.typeArgs, constraintMap, ctxForError))
                continue;

            // Set up type substitutions for this instantiation
            auto savedSubst = activeTypeSubstitutions;
            auto savedPackSubst = activePackSubstitutions;

            auto packMapIt = genericStructPackIndex.find(pending.templateName);
            size_t packIdx = (packMapIt != genericStructPackIndex.end()) ? packMapIt->second : std::string::npos;

            if (packIdx == std::string::npos)
            {
                // Non-variadic: 1:1 mapping
                for (size_t i = 0; i < typeParams.size() && i < pending.typeArgs.size(); i++)
                    activeTypeSubstitutions[typeParams[i]] = pending.typeArgs[i];
            }
            else
            {
                // Fixed params before the pack
                for (size_t i = 0; i < packIdx && i < pending.typeArgs.size(); i++)
                    activeTypeSubstitutions[typeParams[i]] = pending.typeArgs[i];
                // Pack param absorbs remaining type args
                activePackSubstitutions[typeParams[packIdx]] =
                    std::vector<std::string>(pending.typeArgs.begin() + packIdx, pending.typeArgs.end());
            }

            {
                TemplateNamespaceScope nsScope(compilerLLVM, pending.templateName);
                if (isStruct)
                    ParseStructDefinition(structCtx, pending.mangledName);
                else
                    ParseClassDefinition(classCtx, pending.mangledName);
            }

            activeTypeSubstitutions = savedSubst;
            activePackSubstitutions = savedPackSubst;
        }
    }

void MainListener::PreDeclareInstantiationMembers(
        LLVMBackend* compiler,
        const std::vector<CFlatParser::FunctionDefinitionContext*>& functionList,
        const std::string& baseName,
        const std::string& structName,
        const LLVMBackend::TypeAndValue& returnType) {
        for (auto func : functionList)
        {
            if (IsReturnBlockFunction(func)) continue;
            if (func->genericTypeParameters() != nullptr) continue;

            std::string funcName = getFunctionName(func);

            // Constructor overload - pre-declare without this* parameter.
            // A zero-arg ctor has a null parameterTypeList; ParseParameterTypeList
            // returns empty for null, so do not gate the ctor branch on it.
            if (!FunctionDeclaresReturnType(func) && funcName == baseName)
            {
                auto* declParamList = func->parameterTypeList();
                auto declParams = this->ParseParameterTypeList(declParamList);
                bool declVarargs = declParamList && declParamList->Ellipsis() != nullptr;
                std::vector<LLVMBackend::TypeAndValue> ctorAllParams(declParams.begin(), declParams.end());
                // Sink inference on the monomorphized ctor params - this is where a generic-class
                // instantiation registers its FunctionSymbol, so ParseFunctionDefinition (which
                // sees alreadyDeclared) cannot set the flag later. The if-const evaluator lets a
                // move inside a live if-const branch mark the param a sink for this instantiation.
                ApplyOwningSinkInference(compiler, func, ctorAllParams, SinkIfConstEvaluator());
                compiler->CreateFunctionDeclaration(structName, returnType, ctorAllParams, false, declVarargs);
                continue;
            }

            bool isOperatorFunc = (funcName == "operator new"
                                || funcName == "operator delete"
                                || funcName == "operator string");
            bool isStaticFunc = isFunctionStatic(func);
            bool isStaticLike = isOperatorFunc || isStaticFunc;
            std::string declName = isStaticLike ? (structName + "." + funcName) : funcName;

            auto declReturnType = this->getFunctionReturnType(func);
            auto* declParamList = func->parameterTypeList();
            auto declParams = this->ParseParameterTypeList(declParamList);
            bool declVarargs = declParamList && declParamList->Ellipsis() != nullptr;

            if (!isStaticLike)
            {
                LLVMBackend::DeclTypeAndValue thisParam;
                thisParam.TypeName = structName;
                thisParam.VariableName = structName + "__";
                thisParam.Pointer = true;
                declParams.insert(declParams.begin(), thisParam);
            }

            std::vector<LLVMBackend::TypeAndValue> declAllParams(declParams.begin(), declParams.end());
            // Owning-value move-sink inference on the monomorphized method params: a
            // generic-class instantiation registers its FunctionSymbol HERE, so the flag must be
            // set now (ParseFunctionDefinition sees alreadyDeclared and does not re-register). The
            // if-const evaluator lets a move inside a live if-const branch mark the param a sink.
            ApplyOwningSinkInference(compiler, func, declAllParams, SinkIfConstEvaluator());
            bool declReturnsOwned = ComputeReturnsOwned(declReturnType, declName, declAllParams);
            compiler->CreateFunctionDeclaration(declName, declReturnType, declAllParams, declReturnType.external, declVarargs, declReturnsOwned, !isStaticLike);
        }
    }

bool MainListener::EnsureArenaChannelInstantiated(LLVMBackend* compiler) {
        // ForwardRefScanner may have registered an opaque shell (flagged in instantiatedGenerics) before
        // the full body is available; force re-queuing here so sizeof() is valid when the ctor needs it.
        auto isSized = [&]() {
            auto it = compiler->dataStructures.find(kArenaChannelType);
            return it != compiler->dataStructures.end() && it->second.StructType
                && it->second.StructType->isSized();
        };
        if (isSized()) return true;
        if (!genericClassTemplates.count("arena_channel")) return false;
        pendingInstantiations.push_back({"arena_channel", {"IMessage"}, kArenaChannelType});
        instantiatedGenerics.insert(kArenaChannelType);
        ProcessPendingInstantiations();
        return isSized();
    }

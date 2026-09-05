#include "MainListener.h"

namespace {
// The drain body has early 'continue's, so the previous origin is restored by a scope guard.
struct ActiveOriginScope
{
    GenericTemplateState::ActiveInstantiationOrigin* slot_;
    GenericTemplateState::ActiveInstantiationOrigin saved_;
    ActiveOriginScope(GenericTemplateState::ActiveInstantiationOrigin* slot,
                      GenericTemplateState::ActiveInstantiationOrigin next)
        : slot_(slot), saved_(std::move(*slot)) { *slot_ = std::move(next); }
    ~ActiveOriginScope() { *slot_ = std::move(saved_); }
    ActiveOriginScope(const ActiveOriginScope&) = delete;
    ActiveOriginScope& operator=(const ActiveOriginScope&) = delete;
};
}



bool MainListener::ValidateGenericArgumentKinds(const std::string& templateName,
    const std::vector<std::string>& typeParams, const std::vector<std::string>& valueParams,
    const std::vector<std::string>& typeArgs)
{
    bool valid = true;
    for (size_t i = 0; i < typeParams.size() && i < typeArgs.size(); i++)
    {
        std::string valueType = i < valueParams.size() ? valueParams[i] : std::string{};
        bool expectsValue = !valueType.empty();
        bool isValue = IsDecimalIntegerSpelling(typeArgs[i]);
        if (expectsValue != isValue)
        {
            Compiler()->LogError(std::format(
                "generic argument for {} parameter '{}' in template '{}' has the wrong kind; expected a {}",
                expectsValue ? "value" : "type", typeParams[i], templateName,
                expectsValue ? "compile-time value" : "type"));
            valid = false;
            continue;
        }
        if (!expectsValue) continue;

        int64_t value = ParseValueArg(typeArgs[i]);
        auto* llvmType = Compiler()->GetType(LLVMBackend::TypeAndValue{ .TypeName = valueType });
        unsigned bits = llvmType != nullptr && llvmType->isIntegerTy()
            ? llvmType->getIntegerBitWidth() : 0;
        bool unsignedType = valueType.starts_with("u");
        bool inRange = bits != 0;
        if (inRange && valueType == "bool") inRange = value == 0 || value == 1;
        else if (inRange && unsignedType)
            inRange = value >= 0 && (bits == 64 || static_cast<uint64_t>(value) < (uint64_t{1} << bits));
        else if (inRange && bits < 64)
        {
            int64_t minValue = -(int64_t{1} << (bits - 1));
            int64_t maxValue = (int64_t{1} << (bits - 1)) - 1;
            inRange = value >= minValue && value <= maxValue;
        }
        if (!inRange)
        {
            Compiler()->LogError(std::format(
                "value argument '{}' is outside the range of '{}' for parameter '{}' in template '{}'",
                typeArgs[i], valueType, typeParams[i], templateName));
            valid = false;
        }
    }
    return valid;
}


void MainListener::InstantiateGenericInterface(const std::string& baseName, const std::string& mangledName,
                                     const std::unordered_map<std::string, std::string>& substitutions,
                                     const std::unordered_map<std::string, std::vector<std::string>>& packSubstitutions,
                                     const std::unordered_map<std::string, std::string>& valueSubstitutions) {
        // The INSTANCE exists from here on, so its implementor set is enumerable even though the
        // template's never is. Recorded before the dedupe return - a repeat call is still an
        // instantiation of the same concrete name.
        compilerLLVM->RecordCertainInstantiatedInterface(mangledName);
        if (instantiatedInterfaces.count(mangledName)) return;
        instantiatedInterfaces.insert(mangledName);

        auto templateIt = genericInterfaceTemplates.find(baseName);
        if (templateIt == genericInterfaceTemplates.end()) return;

        auto* ctx = compilerLLVM->MaterializeGenericInterface(baseName);
        if (!ctx) return;

        TemplateNamespaceScope nsScope(compilerLLVM, baseName);

        // Apply substitutions to instantiate the interface methods
        auto savedSubst = activeTypeSubstitutions;
        auto savedValueSubst = activeValueSubstitutions;
        auto savedPackSubst = activePackSubstitutions;
        GenericValueMacroScope valueMacros(compilerLLVM);
        // Same shadow rule as the struct/function paths: a name this interface binds must not
        // keep an enclosing instantiation's binding of the OTHER kind.
        for (const auto& [k, v] : substitutions)
        {
            activePackSubstitutions.erase(k);
            activeValueSubstitutions.erase(k);
            activeTypeSubstitutions[k] = v;
        }
        for (const auto& [k, v] : packSubstitutions)
        {
            activeTypeSubstitutions.erase(k);
            activeValueSubstitutions.erase(k);
            activePackSubstitutions[k] = v;
        }
        for (const auto& [k, v] : valueSubstitutions)
        {
            activeValueSubstitutions[k] = v;
            valueMacros.Bind(k, ParseValueArg(v));
        }

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
                const auto& parentValueParams = genericInterfaceValueParams[parentBase];
                auto packIt = genericInterfacePackIndex.find(parentBase);
                size_t packIndex = packIt != genericInterfacePackIndex.end()
                    ? packIt->second : std::string::npos;
                std::unordered_map<std::string, std::string> parentValueSubst;
                if (packIndex == std::string::npos)
                {
                    for (size_t i = 0; i < parentParams.size() && i < parentArgs.size(); i++)
                    {
                        if (i < parentValueParams.size() && !parentValueParams[i].empty())
                            parentValueSubst[parentParams[i]] = parentArgs[i];
                        else
                            parentSubst[parentParams[i]] = parentArgs[i];
                    }
                }
                else
                {
                    for (size_t i = 0; i < packIndex && i < parentArgs.size(); i++)
                        parentSubst[parentParams[i]] = parentArgs[i];
                    parentPackSubst[parentParams[packIndex]] =
                        std::vector<std::string>(parentArgs.begin() + packIndex, parentArgs.end());
                }
                InstantiateGenericInterface(parentBase, parentMangled, parentSubst, parentPackSubst,
                                            parentValueSubst);
            }
        }

        // Entered after the substitutions are installed: this instantiation must re-evaluate the
        // if-const conditions under its own T, not reuse another instantiation's branch selection.
        ResolvedInterfaceMembersScope memberScope(resolvedInterfaceMembers_, (const void*)ctx);

        std::vector<LLVMBackend::InterfaceMethod> methods;
        auto implementationUsesCoreUnique = [&](const std::string& methodName, size_t paramIndex) {
            auto functionIt = compilerLLVM->functionTable.find(methodName);
            if (functionIt == compilerLLVM->functionTable.end()) return false;
            for (const auto& [implName, data] : compilerLLVM->dataStructures)
            {
                if (std::find(data.Interfaces.begin(), data.Interfaces.end(), mangledName)
                    == data.Interfaces.end())
                    continue;
                for (const auto& symbol : functionIt->second)
                {
                    if (symbol.Parameters.size() <= paramIndex + 1
                        || symbol.Parameters[0].TypeName != implName)
                        continue;
                    if (compilerLLVM->IsCoreUniqueType(symbol.Parameters[paramIndex + 1].TypeName))
                        return true;
                }
            }
            return false;
        };
        for (auto method : InterfaceMethods(ctx))
        {
            if (RejectVariadicInterfaceMethod(mangledName, method)) continue;
            LLVMBackend::InterfaceMethod m;
            m.ReturnType = ParseDeclarationSpecifiers(method->declarationSpecifiers());
            m.Name = getInterfaceMethodName(method);
            auto declParams = ParseParameterTypeList(method->parameterTypeList());
            for (const auto& p : declParams)
            {
                auto tv = p;
                if (compilerLLVM->IsCoreUniqueType(tv.TypeName)
                    && tv.Pointer && !tv.ElemPointer && !tv.IsInterface
                    && implementationUsesCoreUnique(m.Name, m.Parameters.size()))
                {
                    std::string uniqueType = MangledGenericName("unique", { tv.TypeName });
                    QueueGenericInstantiation("unique", { tv.TypeName }, uniqueType);
                    tv.TypeName = uniqueType;
                    tv.Pointer = false;
                    tv.ElemPointer = false;
                    tv.PointerDepth = 0;
                    tv.IsMove = true;
                }
                m.Parameters.push_back(tv);
            }
            methods.push_back(std::move(m));
        }

        auto fields = ParseInterfaceFields(ctx);
        activeTypeSubstitutions = savedSubst;
        activeValueSubstitutions = savedValueSubst;
        activePackSubstitutions = savedPackSubst;
        valueMacros.Restore();
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

std::string MainListener::InstantiateGenericFunction(const std::string& baseName,
                                                    const std::vector<std::string>& spelledArgs) {
        std::vector<std::string> typeArgs = spelledArgs;
        std::string mangledName = MangledGenericName(baseName, typeArgs);
        FillGenericValueDefaults(*Compiler(), baseName,
                                 genericFunctionTypeParams[baseName].size(), typeArgs);
        if (instantiatedGenericFunctions.count(mangledName)) return mangledName;
        instantiatedGenericFunctions.insert(mangledName);

        auto templateIt = genericFunctionTemplates.find(baseName);
        if (templateIt == genericFunctionTemplates.end()) return {};
        auto* tmplCtx = compilerLLVM->MaterializeGenericFunction(baseName);
        if (!tmplCtx) return {};

        // Generic FUNCTIONS do not go through pendingInstantiations, so publish the same origin
        // here: a body type that fails on a substituted array view is reported at the call.
        GenericTemplateState::ActiveInstantiationOrigin fnOrigin;
        {
            const auto& outer = compilerLLVM->gts.activeInstantiationOrigin;
            fnOrigin.templateName = baseName;
            if (outer.valid)
            {
                // Instantiated from inside another template body: keep the site the user wrote.
                fnOrigin.file = outer.file;
                fnOrigin.line = outer.line;
                fnOrigin.column = outer.column;
            }
            else
            {
                fnOrigin.file = compilerLLVM->sourceFileName;
                fnOrigin.line = compilerLLVM->currentLine;
                fnOrigin.column = compilerLLVM->currentColumn;
            }
            fnOrigin.valid = !fnOrigin.file.empty();
            for (const auto& arg : typeArgs)
                if (arg.size() > 2 && arg.ends_with("[]")) fnOrigin.viewArgs.push_back(arg);
        }
        ActiveOriginScope fnOriginScope(&compilerLLVM->gts.activeInstantiationOrigin, std::move(fnOrigin));

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
        auto savedValueSubst = activeValueSubstitutions;
        auto savedPackSubst = activePackSubstitutions;
        GenericValueMacroScope valueMacros(compilerLLVM);
        const auto& valueParams = genericFunctionValueParams[baseName];
        if (!ValidateGenericArgumentKinds(baseName, typeParams, valueParams, typeArgs))
            return {};
        // Same shadow rule as the struct drain: this instantiation OWNS its parameter names.
        for (const auto& tp : typeParams)
        {
            activeTypeSubstitutions.erase(tp);
            activeValueSubstitutions.erase(tp);
            activePackSubstitutions.erase(tp);
        }
        if (packIdx == std::string::npos)
        {
            for (size_t i = 0; i < typeParams.size(); i++)
            {
                if (i < valueParams.size() && !valueParams[i].empty())
                {
                    activeValueSubstitutions[typeParams[i]] = typeArgs[i];
                    valueMacros.Bind(typeParams[i], ParseValueArg(typeArgs[i]));
                }
                else activeTypeSubstitutions[typeParams[i]] = typeArgs[i];
            }
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

        if (!CheckValueConstraints(baseName, tmplCtx->whereClause(), typeParams, valueParams, typeArgs))
        {
            activeTypeSubstitutions = savedSubst;
            activeValueSubstitutions = savedValueSubst;
            activePackSubstitutions = savedPackSubst;
            valueMacros.Restore();
            return {};
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
        // Tail of the module's function list before the body walk: anything appended below is
        // this instantiation's, and must be sealed if the walk throws (expect_error resumes).
        auto& functionList = instCompiler->module->getFunctionList();
        llvm::Function* lastBefore = functionList.empty() ? nullptr : &functionList.back();
        try
        {
            if (!ownerStruct.empty())
            {
                LLVMBackend::AliasScopeGuard aggregateAliasScope(instCompiler, ownerStruct);
                ParseFunctionDefinition(tmplCtx, ownerStruct, {}, mangledName, DeclaringNamespaceOf(compilerLLVM, baseName));
            }
            else
                ParseFunctionDefinition(tmplCtx, ownerStruct, {}, mangledName, DeclaringNamespaceOf(compilerLLVM, baseName));
        }
        catch (...)
        {
            /*
             * The body walk was abandoned (an expect_error match, or a real diagnostic in batch
             * mode). Nothing below unwinds on its own: the outer function's locals were MOVED out
             * of stackNamedVariable, and the half-built instantiation is still in the module.
             * Every body sealed here is also unregistered and renamed out of the way, and
             * mangledName leaves instantiatedGenericFunctions, so a later good request re-emits
             * this instantiation instead of binding to the sealed body (which traps at runtime).
             */
            auto it = lastBefore != nullptr
                ? std::next(llvm::Module::iterator(lastBefore)) : functionList.begin();
            bool sealedAny = false;
            bool completeAny = false;
            for (; it != functionList.end(); ++it)
            {
                if (it->isDeclaration()) continue;
                if (instCompiler->SealAbandonedFunction(&*it))
                {
                    sealedAny = true;
                    instCompiler->DiscardAbandonedFunction(&*it);
                }
                // A nested instantiation that COMPLETED before this body threw is appended here
                // too, and keeps its registration.
                else completeAny = true;
            }
            // Only give up the name when this body was really abandoned.
            if (sealedAny || !completeAny) instantiatedGenericFunctions.erase(mangledName);
            instCompiler->stackNamedVariable = std::move(savedStack);
            activeTypeSubstitutions = savedSubst;
            activeValueSubstitutions = savedValueSubst;
            activePackSubstitutions = savedPackSubst;
            valueMacros.Restore();
            throw;   // BuilderStateGuard's destructor restores the caller's insert point.
        }
        instCompiler->stackNamedVariable = std::move(savedStack);
        savedState.restore();

        activeTypeSubstitutions = savedSubst;
        activeValueSubstitutions = savedValueSubst;
        activePackSubstitutions = savedPackSubst;
        valueMacros.Restore();
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
            TypeSpelling receiverSpelling;
            if (MangledBase(receiverType) != baseName
                || !DemangleType(*Compiler(), receiverType, receiverSpelling)
                || receiverSpelling.args.size() != typeParams.size())
                continue;
            std::vector<std::string> typeArgs;
            for (const auto& arg : receiverSpelling.args)
                typeArgs.push_back(MangleType(*Compiler(), arg));
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
        // A binding read off the argument's DECLARED spelling outranks the LLVM-type fallback,
        // and two declared bindings that disagree infer nothing (the overload error stands).
        std::unordered_map<std::string, bool> declaredBinding;
        for (size_t i = 0; i < paramDecls.size() && i < args.size(); i++)
        {
            std::string paramTypeName;
            int paramStars = 0;
            bool paramView = false;
            for (auto* ds : paramDecls[i]->declarationSpecifiers()->declarationSpecifier())
            {
                auto* ts = ds->typeSpecifier();
                if (!ts || !ts->genericIdentifier()) continue;
                auto* gid = ts->genericIdentifier();
                if (gid->Identifier())
                {
                    paramTypeName = gid->Identifier()->getText();
                    // The parameter's OWN decoration ('T[] v', 'T*[] v', 'T* p') is not part of
                    // the binding: it is stripped off the argument's spelling below.
                    paramStars = PointerDepthOf(ds->pointer());
                    paramView = IsArrayViewArg(ds);
                    break;
                }
            }
            for (const auto& tp : typeParams)
            {
                if (paramTypeName != tp)
                    continue;
                const auto& tv = args[i].TypeAndValue;
                // The call site drops TypeName for primitives, so the declared name arrives on
                // the side channel; the reference-kind flags are propagated either way.
                std::string base = tv.TypeName.empty() ? args[i].InferSourceTypeName : tv.TypeName;
                bool declared = false;
                std::string argType;
                if (!base.empty())
                {
                    // Rebuild the argument's written spelling from its flags: a view carries its
                    // ELEMENT's stars ('int*[]'), a plain pointer its own depth.
                    int argStars = tv.IsArrayView
                        ? (tv.ElemPointer ? std::max(tv.PointerDepth, 1) : 0)
                        : (tv.Pointer ? std::max(tv.PointerDepth, 1) : 0);
                    bool argView = tv.IsArrayView;
                    if (paramView)
                    {
                        // A view parameter binds from a view argument or from a FIXED array,
                        // which decays to one at the call (`First(plain)` with `int[3] plain`).
                        if (!argView && tv.ConstArraySize == 0) break;
                        argView = false;
                    }
                    else if (argView && paramStars > 0)
                    {
                        // 'T*' over a view argument decays to the ELEMENT pointer (q09 ruling).
                        argView = false;
                        argStars += 1;
                    }
                    if (argStars < paramStars) break;
                    argStars -= paramStars;
                    argType = base + std::string(argStars, '*') + (argView ? "[]" : "");
                    declared = true;
                }
                // Free-function calls drop TypeName for signed-int args. Fall back to the LLVM
                // BaseType so literals infer; NOT for a view, whose opaque pointer recovers as
                // a bogus 'i8' ELEMENT name that then fails the element-match gate.
                if (argType.empty() && args[i].BaseType != nullptr
                    && !args[i].TypeAndValue.IsArrayView)
                    argType = Compiler()->LlvmTypeToTypeName(args[i].BaseType);
                if (argType.empty()) break;
                auto prev = inferred.find(tp);
                if (prev == inferred.end())
                {
                    inferred[tp] = argType;
                    declaredBinding[tp] = declared;
                }
                else if (declared && !declaredBinding[tp])
                {
                    prev->second = argType;   // a declared spelling replaces the fallback
                    declaredBinding[tp] = true;
                }
                else if (declared && declaredBinding[tp] && prev->second != argType)
                {
                    // Two declared arguments disagree. Keep the LAST one (pre-fix behaviour) so
                    // the call still reports the detailed overload mismatch, not "not known".
                    prev->second = argType;
                }
                else if (!declared && !declaredBinding[tp])
                {
                    prev->second = argType;   // fallback only: last one wins, as before
                }
                break;
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
            // the mangled "list$string" and queues the instantiation.
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
                    CanonicalizeUniqueSigComponent(compilerLLVM, p.TypeName, p.Pointer,
                                                   p.PointerDepth);
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
                    QueuePendingInstantiation("tuple", typeArgs, mangledName);
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
                QueuePendingInstantiation(baseName, typeArgs, mangledName, genParams);
                instantiatedGenerics.insert(mangledName);
            }
            break;
        }
    }

std::string MainListener::MangledGenericName(const std::string& baseName, const std::vector<std::string>& typeArgs) {
        return MangleGenericInstance(*Compiler(), baseName, typeArgs);
    }

void MainListener::QueuePendingInstantiation(const std::string& templateName,
    const std::vector<std::string>& typeArgs, const std::string& mangledName,
    antlr4::ParserRuleContext* site)
{
        PendingInstantiation record{ templateName, typeArgs, mangledName };
        const auto& active = compilerLLVM->gts.activeInstantiationOrigin;
        if (active.valid)
        {
            // Queued from inside a template body: the user never wrote this site, so keep
            // pointing at the outer argument they did write.
            record.originFile = active.file;
            record.originLine = active.line;
            record.originColumn = active.column;
        }
        else if (site != nullptr)
        {
            record.originFile = sourceFileName;
            record.originLine = site->getStart()->getLine();
            record.originColumn = site->getStart()->getCharPositionInLine();
        }
        pendingInstantiations.push_back(std::move(record));
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

            // Publish this record's origin for the duration of its instantiation: a body type
            // lookup that fails on a substituted array view is reported at the user's argument.
            GenericTemplateState::ActiveInstantiationOrigin origin;
            origin.valid = !pending.originFile.empty();
            origin.templateName = pending.templateName;
            origin.file = pending.originFile;
            origin.line = pending.originLine;
            origin.column = pending.originColumn;
            for (const auto& arg : pending.typeArgs)
                if (arg.size() > 2 && arg.ends_with("[]")) origin.viewArgs.push_back(arg);
            ActiveOriginScope originScope(&compilerLLVM->gts.activeInstantiationOrigin, std::move(origin));

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
                    const auto& ifaceValueParams = genericInterfaceValueParams[pending.templateName];
                    auto packIdxIt = genericInterfacePackIndex.find(pending.templateName);
                    size_t packIdx = (packIdxIt != genericInterfacePackIndex.end()) ? packIdxIt->second : std::string::npos;
                    std::unordered_map<std::string, std::string> ifaceValueSubst;
                    if (packIdx == std::string::npos)
                    {
                        for (size_t i = 0; i < ifaceTypeParams.size() && i < pending.typeArgs.size(); i++)
                        {
                            if (i < ifaceValueParams.size() && !ifaceValueParams[i].empty())
                                ifaceValueSubst[ifaceTypeParams[i]] = pending.typeArgs[i];
                            else ifaceSubst[ifaceTypeParams[i]] = pending.typeArgs[i];
                        }
                        if (!ValidateGenericArgumentKinds(pending.templateName,
                                                           ifaceTypeParams, ifaceValueParams, pending.typeArgs))
                            continue;
                    }
                    else
                    {
                        for (size_t i = 0; i < packIdx && i < pending.typeArgs.size(); i++)
                            ifaceSubst[ifaceTypeParams[i]] = pending.typeArgs[i];
                        ifacePackSubst[ifaceTypeParams[packIdx]] =
                            std::vector<std::string>(pending.typeArgs.begin() + packIdx, pending.typeArgs.end());
                    }
                    InstantiateGenericInterface(pending.templateName, pending.mangledName,
                        ifaceSubst, ifacePackSubst, ifaceValueSubst);
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
            const auto& valueParams = genericStructValueParams[pending.templateName];
            // Omitted TRAILING value arguments come from the declared defaults. The mangled name
            // was canonicalized the same way (a spelled default is stripped), so the filled list
            // and the name always describe the same instantiation.
            if (!FillGenericValueDefaults(*Compiler(), pending.templateName,
                                          typeParams.size(), pending.typeArgs)
                && TemplateHasGenericDefaults(*Compiler(), pending.templateName))
            {
                Compiler()->LogError(std::format(
                    "generic '{}' expects {} type argument(s), but {} were provided and the "
                    "remaining parameter(s) have no default",
                    pending.templateName, typeParams.size(), pending.typeArgs.size()));
                continue;
            }

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
            if (!ValidateGenericArgumentKinds(pending.templateName, typeParams,
                                               valueParams, pending.typeArgs))
                continue;

            // Set up type substitutions for this instantiation
            auto savedSubst = activeTypeSubstitutions;
            auto savedValueSubst = activeValueSubstitutions;
            auto savedPackSubst = activePackSubstitutions;
            GenericValueMacroScope valueMacros(compilerLLVM);

            auto packMapIt = genericStructPackIndex.find(pending.templateName);
            size_t packIdx = (packMapIt != genericStructPackIndex.end()) ? packMapIt->second : std::string::npos;

            // A record drained while ANOTHER instantiation is mid-flight (tuple<T...> imports
            // tuple.cb and re-enters here) must not inherit that one's binding for a parameter
            // name it declares itself: a stale pack "T" expands this template's "T v" field.
            for (const auto& tp : typeParams)
            {
                activeTypeSubstitutions.erase(tp);
                activeValueSubstitutions.erase(tp);
                activePackSubstitutions.erase(tp);
            }
            if (packIdx == std::string::npos)
            {
                // Non-variadic: 1:1 mapping
                for (size_t i = 0; i < typeParams.size() && i < pending.typeArgs.size(); i++)
                {
                    if (i < valueParams.size() && !valueParams[i].empty())
                    {
                        activeValueSubstitutions[typeParams[i]] = pending.typeArgs[i];
                        valueMacros.Bind(typeParams[i], ParseValueArg(pending.typeArgs[i]));
                    }
                    else activeTypeSubstitutions[typeParams[i]] = pending.typeArgs[i];
                }
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

            auto* whereClause = isStruct ? structCtx->whereClause() : classCtx->whereClause();
            if (!CheckValueConstraints(pending.templateName, whereClause, typeParams, valueParams,
                                       pending.typeArgs))
            {
                activeTypeSubstitutions = savedSubst;
                activeValueSubstitutions = savedValueSubst;
                activePackSubstitutions = savedPackSubst;
                valueMacros.Restore();
                continue;
            }

            {
                TemplateNamespaceScope nsScope(compilerLLVM, pending.templateName);
                if (isStruct)
                    ParseStructDefinition(structCtx, pending.mangledName);
                else
                    ParseClassDefinition(classCtx, pending.mangledName);
            }

            activeTypeSubstitutions = savedSubst;
            activeValueSubstitutions = savedValueSubst;
            activePackSubstitutions = savedPackSubst;
            valueMacros.Restore();
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
            const std::string channelType = ArenaChannelTypeName(*compiler);
            auto it = compiler->dataStructures.find(channelType);
            return it != compiler->dataStructures.end() && it->second.StructType
                && it->second.StructType->isSized();
        };
        if (isSized()) return true;
        if (!genericClassTemplates.count("arena_channel")) return false;
        const std::string channelType = ArenaChannelTypeName(*compiler);
        QueuePendingInstantiation("arena_channel", {"IMessage"}, channelType);
        instantiatedGenerics.insert(channelType);
        ProcessPendingInstantiations();
        return isSized();
    }

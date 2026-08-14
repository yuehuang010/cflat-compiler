#include "MainListener.h"

LLVMBackend* ForwardRefScanner::Compiler(antlr4::ParserRuleContext* ctx) {
        compilerLLVM->SetSourceLocation(ctx->getStart()->getLine(), ctx->getStart()->getCharPositionInLine());
        return compilerLLVM;
    }

std::vector<CFlatParser::InterfaceMemberContext*> ForwardRefScanner::ResolveInterfaceMembers(CFlatParser::InterfaceDefinitionContext* ctx) {
        std::vector<CFlatParser::InterfaceMemberContext*> out;
        for (auto* m : ctx->interfaceMember())
            if (!m->ifConstInterfaceMember()) out.push_back(m);
        return out;
    }

LLVMBackend::DeclTypeAndValue ForwardRefScanner::ParseDeclarationSpecifiers(CFlatParser::DeclarationSpecifiersContext* declSpecs) {
        // A null context here means a caller tried to scan a declaration with no return-type
        // specifier as an ordinary member function. Diagnose rather than dereference it.
        if (declSpecs == nullptr)
        {
            compilerLLVM->LogError("expected a return type here");
            return {};
        }
        auto* compiler = Compiler(declSpecs);
        LLVMBackend::DeclTypeAndValue declType;
        // Reject `T[][]` / `T[][M]` / `T[N][]` before any branch consumes the brackets - every
        // branch below drops the empty pairs and would silently parse a narrower type.
        for (auto declSpec : declSpecs->declarationSpecifier())
            if (HasUnsizedMultiDim(declSpec))
                compiler->LogError(UnsizedMultiDimMessage(
                    declSpec->typeSpecifier() != nullptr ? declSpec->typeSpecifier()->getText() : "T"));
        // `long long` arrives as two `long` typeSpecifiers; count them before the loop breaks
        // out on the first one, so the pair can canonicalize to i64.
        int longSpecCount = 0;
        for (auto declSpec : declSpecs->declarationSpecifier())
            if (declSpec->typeSpecifier() != nullptr && declSpec->typeSpecifier()->getText() == "long")
                longSpecCount++;
        for (auto declSpec : declSpecs->declarationSpecifier())
        {
            auto typeSpec = declSpec->typeSpecifier();
            auto storageSpec = declSpec->storageClassSpecifier();
                if (typeSpec != nullptr)
                {
                    // 'move', 'alias', 'bond', 'unique' and 'manifest' are soft keywords parsed as Identifiers
                    // in typeSpecifier context
                    if (typeSpec->getText() == "move")
                    {
                        declType.IsMove = true;
                        continue;  // not a type; look for the actual type in next specifier
                    }
                    if (typeSpec->getText() == "alias")
                    {
                        declType.IsAlias = true;
                        continue;  // not a type; look for the actual type in next specifier
                    }
                    if (typeSpec->getText() == "bond")
                    {
                        declType.IsBond = true;
                        continue;  // not a type; look for the actual type in next specifier
                    }
                    // Position validity is checked in the MainListener copy, which is the pass
                    // whose diagnostics expect_error observes.
                    if (typeSpec->getText() == "unique")
                    {
                        declType.IsUnique = true;
                        continue;  // not a type; look for the actual type in next specifier
                    }
                    if (typeSpec->getText() == "manifest")
                        continue;  // collected by the main pass; not a runtime type
                    // tuple type sugar: (T1, T2) -> tuple<T1, T2>
                    if (typeSpec->tupleTypeSpecifier() != nullptr)
                    {
                        auto* tts = typeSpec->tupleTypeSpecifier();
                        // Pack-only form (T...) resolved during instantiation - skip forward-declare here
                        if (tts->tupleTypePackEntry() != nullptr)
                            break;
                        std::vector<std::string> typeArgs;
                        for (auto* entry : tts->tupleTypeEntry())
                            typeArgs.push_back(TupleEntryArgName(compiler, entry));
                        std::string mangledName = "tuple";
                        for (const auto& arg : typeArgs) mangledName += "__" + MangleTypeArg(compiler, arg);
                        compiler->CreateStructType(mangledName, {});
                        LLVMBackend::TypeAndValue rt{ .TypeName = mangledName };
                        compiler->CreateFunctionDeclaration(mangledName, rt, {});
                        compiler->gts.tupleTypeArgs[mangledName] = typeArgs;
                        declType.TypeName = mangledName;
                        declType.Pointer = declSpec->pointer() != nullptr;
                        declType.ArraySize = ArrayDimsOf(declSpec) ? ArrayDimsOf(declSpec)->assignmentExpression(0) : nullptr;
                        break;
                    }
                    // function pointer type: `function<...>` (thin C ptr) or `Lambda<...>` (fat closure)
                    if (typeSpec->functionPointerSpecifier() != nullptr)
                    {
                        auto* fpSpec = typeSpec->functionPointerSpecifier();
                        declType.IsFunctionPointer = true;
                        // `function` = thin C ptr ("__c_fn_ptr"); `Lambda` = fat ("__closure_fat_ptr").
                        declType.TypeName = (fpSpec->Function() != nullptr) ? "__c_fn_ptr" : "__closure_fat_ptr";
                        if (fpSpec->typeSpecifier() != nullptr)
                        {
                            // Resolve generic signature types (gap b) to match the main pass.
                            bool retPtr = fpSpec->pointer() != nullptr;
                            int retStars = PointerDepthOf(fpSpec->pointer());
                            declType.FuncPtrReturnTypeName = ResolveSigComponentScanner(fpSpec->typeSpecifier(), retPtr);
                            declType.FuncPtrReturnPointer = retPtr;
                            declType.FuncPtrReturnPointerDepth = ReconcilePointerDepth(retPtr, retStars);
                            declType.FuncPtrReturnResolvedKey = SigComponentResolvedKeyScanner(declType.FuncPtrReturnTypeName);
                            if (fpSpec->functionPointerParamList() != nullptr)
                            {
                                for (auto* param : fpSpec->functionPointerParamList()->functionPointerParam())
                                {
                                    LLVMBackend::TypeAndValue::FuncPtrParam p;
                                    bool pPtr = param->pointer() != nullptr;
                                    int pStars = PointerDepthOf(param->pointer());
                                    p.TypeName = ResolveSigComponentScanner(param->typeSpecifier(), pPtr);
                                    p.Pointer = pPtr;
                                    p.IsMove = param->Move() != nullptr;
                                    p.PointerDepth = ReconcilePointerDepth(pPtr, pStars);
                                    p.ResolvedTypeKey = SigComponentResolvedKeyScanner(p.TypeName);
                                    declType.FuncPtrParams.push_back(p);
                                }
                            }
                        }
                        // For bare 'function', signature inferred from initializer at declaration site
                        // This branch breaks out of the specifier loop, so nothing else consumes a
                        // trailing '[N]' or '*'; capture both here (as the alias branch below does).
                        declType.Pointer = declSpec->pointer() != nullptr;
                        if (auto* fpDimSpec = ArrayDimsOf(declSpec))
                        {
                            auto fpDims = fpDimSpec->assignmentExpression();
                            if (ArrayPtrOf(declSpec) != nullptr && !fpDims.empty())
                                compiler->LogError(FixedArrayPointerTypeMessage(typeSpec->getText()));
                            declType.ArraySize = fpDims.empty() ? nullptr : fpDims[0];
                            for (size_t di = 1; di < fpDims.size(); di++)
                                declType.ExtraArrayDims.push_back(fpDims[di]);
                            // '[]' array-view is a thin 'ptr' repr, so only a thin function
                            // pointer can take it; the main pass reports the fat-closure case.
                            if (fpDims.empty() && declType.IsThinFnPtr())
                            {
                                declType.IsArrayView = true;
                                declType.Pointer = true;
                            }
                        }
                        break;
                    }
                    // simd<T,N> builtin vector type. Record fields best-effort; the main pass reports
                    // any malformed lane count (avoid double-logging from the pre-scan).
                    if (typeSpec->simdTypeSpecifier() != nullptr)
                    {
                        auto* sd = typeSpec->simdTypeSpecifier();
                        uint64_t lanes = 0;
                        std::string err;
                        TryParseSimdLaneCount(sd->assignmentExpression()->getText(), lanes, err);
                        declType.TypeName = sd->typeSpecifier()->getText();
                        declType.IsSimd = true;
                        declType.SimdLanes = lanes;
                        RecordSimdPointerAndDims(declType, declSpec);
                        break;
                    }
                    // Pointer depth contributed by a pointer alias (using Handle = void*); peeled
                    // from the resolved alias string below and combined with the declarator's stars.
                    int aliasPtrDepth = 0;
                    // Set when the spec names a generic interface instantiation, whose interfaceTable
                    // entry does not exist yet during this pre-pass.
                    bool genericSpecIsInterface = false;
                    // grammar: some Identifier occurrences were refactored into a genericIdentifier rule
                    std::string baseName;
                    auto* genParams = GenericSpecOf(typeSpec, baseName);
                    if (genParams != nullptr)
                    {
                        // Generic type instantiation: Box<MyType> -> Box__MyType
                        baseName = compiler->ResolveGenericBaseAlias(baseName);
                        std::vector<std::string> typeArgs;
                    for (auto* entry : genParams->typeParameterList()->typeParameterEntry())
                    {
                        // Closure type args (gap a) encode to a symbol-safe name; a `unique`-qualified
                        // arg routes through ResolveForwardTypeArg so its canonical "unique T*" text
                        // (D10) matches the queueing path; others keep the raw getText() spelling.
                        if ((entry->typeSpecifier() && entry->typeSpecifier()->functionPointerSpecifier())
                            || TypeArgHasUnique(entry))
                            typeArgs.push_back(ResolveForwardTypeArg(entry));
                        else
                            typeArgs.push_back(ResolveTypeArgSpelling(compiler, entry->getText()));
                    }
                    std::string mangledName = baseName;
                    for (const auto& arg : typeArgs) mangledName += "__" + MangleTypeArg(compiler, arg);
                    // A generic INTERFACE instantiation is a fat pointer, not a struct: no shell,
                    // no default ctor. Mark it now - interfaceTable only fills in the main pass.
                    // The genericInterfaceInstances insert is deferred to the common tail below,
                    // past the LogErrors that can throw out of this function.
                    if (compiler->IsGenericInterfaceTemplateName(baseName))
                    {
                        genericSpecIsInterface = true;
                        // Re-mangle through ResolveForwardTypeArg so a nested generic argument
                        // (Container<Box<int>>) matches the main pass's Container__Box__int.
                        mangledName = baseName;
                        for (auto* entry : genParams->typeParameterList()->typeParameterEntry())
                            mangledName += "__" + MangleTypeArg(compiler, ResolveForwardTypeArg(entry));
                    }
                    // No template anywhere by this name: skip the shell, which would suppress the
                    // `unknown type` this declaration is owed. Main pass gates alike (isKnownTemplate).
                    else if (compiler->AnyGenericTypeTemplateNamed(baseName))
                    {
                        // Pre-declare opaque struct type and default constructor so that
                        // uses inside function bodies are resolvable before the full
                        // definition is emitted by ProcessPendingInstantiations().
                        compiler->CreateStructType(mangledName, {});
                        LLVMBackend::TypeAndValue returnType{ .TypeName = mangledName };
                        compiler->CreateFunctionDeclaration(mangledName, returnType, {});
                    }
                    declType.TypeName = mangledName;
                }
                else if (auto* fit = compiler->FindFunctionTypeAlias(typeSpec->getText());
                         fit != nullptr)
                {
                    // Function-type alias (using Cb = function<R(Args)> | Lambda<R(Args)>): expand
                    // into the stored signature, mirroring the functionPointerSpecifier branch above.
                    declType.IsFunctionPointer     = true;
                    declType.TypeName              = fit->IsThinFnPtr() ? "__c_fn_ptr" : "__closure_fat_ptr";
                    declType.FuncPtrReturnTypeName = fit->FuncPtrReturnTypeName;
                    declType.FuncPtrReturnPointer  = fit->FuncPtrReturnPointer;
                    declType.FuncPtrReturnOwned = fit->FuncPtrReturnOwned;
                    declType.FuncPtrReturnAlias = fit->FuncPtrReturnAlias;
                    declType.FuncPtrReturnPointerDepth = fit->FuncPtrReturnPointerDepth;
                    declType.FuncPtrParams         = fit->FuncPtrParams;
                    declType.Pointer               = declSpec->pointer() != nullptr;
                    // Like the functionPointerSpecifier branch, this one breaks out of the
                    // specifier loop, so a trailing '[N]' has to be captured right here.
                    if (auto* fpDimSpec = ArrayDimsOf(declSpec))
                    {
                        auto fpDims = fpDimSpec->assignmentExpression();
                        declType.ArraySize = fpDims.empty() ? nullptr : fpDims[0];
                        for (size_t di = 1; di < fpDims.size(); di++)
                            declType.ExtraArrayDims.push_back(fpDims[di]);
                        if (fpDims.empty() && declType.IsThinFnPtr())
                        {
                            declType.IsArrayView = true;
                            declType.Pointer = true;
                        }
                    }
                    break;
                }
                else
                {
                    std::string specText = typeSpec->getText();
                    if (specText == "long") specText = LongSpellingTypeName(longSpecCount);
                    declType.TypeName = compiler->ResolveQualifiedName(specText);
                    // Resolve type aliases (e.g. user-defined aliases)
                    declType.TypeName = compiler->ResolveTypeAlias(declType.TypeName);
                    // Peel an array alias's brackets (using Vec3 = float[3]) BEFORE the stars so
                    // "int*[3]" yields dims {3} over base "int*".
                    std::vector<uint64_t> aliasDims;
                    if (PeelAliasArrayDims(declType.TypeName, aliasDims))
                    {
                        declType.AliasArraySize = aliasDims[0];
                        declType.AliasInnerDims.assign(aliasDims.begin() + 1, aliasDims.end());
                    }
                    aliasPtrDepth = PeelAliasPointerStars(declType.TypeName);
                }
                // Combine alias pointer depth (using Handle = void*) with the declarator's stars.
                // The Pointer + ElemPointer model caps at 2 levels; the authoritative over-cap
                // error is emitted by the MainListener copy (this forward-ref copy clamps silently)
                // for the written-star spellings too - the pre-pass is opportunistic and must not
                // pre-empt the codegen pass's diagnostic ordering.
                {
                    int declStars = declSpec->pointer() != nullptr ? (int)declSpec->pointer()->Star().size() : 0;
                    int totalPtr = aliasPtrDepth + declStars;
                    if (totalPtr >= 1) declType.Pointer = true;
                    if (totalPtr >= 2) declType.ElemPointer = true;
                    // A POSITIVE depth claim (0 = none written here). Over the 2-level cap the depth
                    // is LOST, so claim nothing: a clamped 2 stepped down by '*' would falsely prove 1.
                    declType.PointerDepth = totalPtr > 2 ? 0 : totalPtr;
                }
                if (auto* dimSpec = ArrayDimsOf(declSpec))
                {
                    auto dims = dimSpec->assignmentExpression();
                    declType.ArraySize = dims.empty() ? nullptr : dims[0];
                    for (size_t di = 1; di < dims.size(); di++)
                        declType.ExtraArrayDims.push_back(dims[di]);
                    if (dims.empty())
                    {
                        // Preserve a declarator '*' as element pointer-ness for `T*[]`.
                        bool elementPointer = declType.Pointer;
                        declType.IsArrayView = true;
                        declType.Pointer = true;
                        declType.ElemPointer = elementPointer || declType.ElemPointer;
                    }
                }
                if (ArrayPtrOf(declSpec))
                {
                    if (declType.IsArrayView)
                        compiler->LogError(std::format("pointer to array-view '{}[]*' is not a valid type. "
                            "To return several results, return one 'T[]' and pass the rest as out-parameters: "
                            "a 'T[]' for arrays (each keeps its noalias contract) and a 'T*' for scalars.",
                            declType.TypeName));
                    else
                        compiler->LogError(std::format("pointer to fixed array '{}[N]*' is not a valid type; "
                            "pass '{}*' (a fixed array decays to a pointer to its first element).",
                            declType.TypeName, declType.TypeName));
                }
                declType.IsInterface = genericSpecIsInterface || compiler->IsInterfaceType(declType.TypeName);
                if (declType.IsInterface && declSpec->pointer() != nullptr)
                    compiler->LogError(std::format("pointer '*' is not allowed on interface type '{}'", declType.TypeName));
                // Past the throwing checks: this spec really does name a generic interface.
                if (genericSpecIsInterface)
                    compiler->gts.genericInterfaceInstances.insert(declType.TypeName);
                if (declType.IsInterface)
                {
                    declType.IsInterfacePointer = declSpec->pointer() != nullptr;
                    if (declType.IsInterfacePointer)
                        declType.Pointer = true;
                }
                if (declSpec->Question())
                {
                    if (declType.IsPrimitive())
                        compiler->LogError(std::format("nullable '?' is not allowed on primitive type '{}'", declType.TypeName));
                    else
                    {
                        declType.IsNullable = true;
                        declType.Pointer = true;
                    }
                }
                break;
            }
            else if (storageSpec)
            {
                declType.external = storageSpec->Extern() != nullptr;
                declType.threadLocal = storageSpec->ThreadLocal() != nullptr;
                // `static` on a LOCAL selects module-global storage + a run-once initializer.
                if (storageSpec->Static() != nullptr) declType.staticStorage = true;
            }
            else if (auto funcSpec = declSpec->functionSpecifier())
            {
                if (funcSpec->getText() == "stdcall")
                    declType.CallConv = LLVMBackend::CallingConv::Stdcall;
                else if (funcSpec->getText() == "cdecl")
                    declType.CallConv = LLVMBackend::CallingConv::Cdecl;
            }
            else if (auto* alignSpec = declSpec->alignmentSpecifier())
            {
                // ForwardRefScanner: the SLOT alignment (arg1) doesn't affect forward declarations,
                // but arg2 (allocation alignment) must be recorded onto the registered signature so
                // a return-type/param clause survives into the call-site checks. This pre-pass lacks
                // the constant folder, so it best-effort parses an integer literal; the codegen pass
                // does the full validation and error reporting.
                auto cexprs = alignSpec->constantExpression();
                size_t idx = (alignSpec->typeName() != nullptr) ? 0 : 1;
                if (cexprs.size() > idx)
                {
                    std::string txt = cexprs[idx]->getText();
                    uint64_t v = 0;
                    auto res = std::from_chars(txt.data(), txt.data() + txt.size(), v);
                    if (res.ec == std::errc() && res.ptr == txt.data() + txt.size()
                        && v > 0 && v <= 4096 && (v & (v - 1)) == 0)
                        declType.AllocAlignValue = v;
                }
            }
        }
        return declType;
    }

std::vector<LLVMBackend::DeclTypeAndValue> ForwardRefScanner::ParseParameterTypeList(CFlatParser::ParameterTypeListContext* paramTypeList) {
        std::vector<LLVMBackend::DeclTypeAndValue> params;
        if (paramTypeList == nullptr)
            return params;

        auto paramList = paramTypeList->parameterList();
        for (auto paramDecl : paramList->parameterDeclaration())
        {
            LLVMBackend::DeclTypeAndValue paramType = ParseDeclarationSpecifiers(paramDecl->declarationSpecifiers());
            if (auto declarer = paramDecl->declarator())
                if (auto directDeclarer = declarer->directDeclarator())
                    paramType.VariableName = getDirectDeclName(directDeclarer);
            // D4: a `unique` parameter is a synthesized move parameter (mirrors the codegen copy),
            // so the forward-declared signature agrees on the move flag.
            if (paramType.IsUnique
                && ((paramType.Pointer && !paramType.ElemPointer
                && !paramType.IsArrayView) || paramType.IsInterface))
                paramType.IsMove = true;
            // A parameter's `alignas(_, N)` allocation alignment now rides in declarationSpecifiers
            // (prefix), so ParseDeclarationSpecifiers above already recorded paramType.AllocAlignValue.
            if (paramType.IsMove && paramType.IsBond)
                Compiler(paramDecl)->LogError(std::format("parameter '{}': 'bond' and 'move' are mutually exclusive", paramType.VariableName));
            if (auto* lc = paramDecl->lockClause())
            {
                auto args = lc->lockArgList()->expression();
                std::string lockText = args.size() == 1 ? args[0]->getText() : std::string();
                if (args.size() == 1 && NormalizeLockText(lockText) == "this")
                {
                    paramType.LockThis = true;
                    paramType.LockThisMode = LockModeFromSuffix(LockTextMode(lockText));
                }
                else
                    Compiler(paramDecl)->LogError("lock(this) is the only supported form on function parameters");
            }
            paramType.DefaultValue = paramDecl->initializer();
            params.push_back(paramType);
        }
        return params;
    }

void ForwardRefScanner::ScanFunctionDefinition(CFlatParser::FunctionDefinitionContext* func, const std::string& structName, const std::string& namespaceName, const std::vector<std::string>& extraRequiredLocks) {
        auto* compiler = Compiler(func);
        // Return-block functions are inlined at call sites - no LLVM proto needed.
        if (IsReturnBlockFunction(func))
            return;

        // Generic function templates are instantiated on demand - skip pre-declaration.
        if (func->genericTypeParameters() != nullptr)
            return;

        const std::string rawFuncName = getFunctionName(func);
        std::string name = namespaceName.empty() ? rawFuncName : namespaceName + "." + rawFuncName;

        auto returnType = ParseDeclarationSpecifiers(func->declarationSpecifiers());
        // 'auto' return type is only supported on generic function templates (already
        // skipped above) and return-block functions. Reaching this point with auto
        // means the user wrote a non-generic, non-return-block 'auto fn(...)' which
        // we cannot infer in v1. Diagnose here so GetType does not fire its less
        // helpful "unknown type 'auto'" later in this scan.
        if (returnType.TypeName == "auto")
        {
            Compiler(func)->LogErrorMessage("'{}' return type is only supported on generic functions (e.g. {})",
                { "auto", "auto f<T>(T x)" });
            return;
        }
        auto* paramTypeList = func->parameterTypeList();
        auto params = ParseParameterTypeList(paramTypeList);
        bool varargs = paramTypeList && paramTypeList->Ellipsis() != nullptr;

        // Operator new/delete/string and static methods are scoped to struct but have no 'this' param
        bool isOperatorFunc = (rawFuncName == "operator new"
                            || rawFuncName == "operator delete"
                            || rawFuncName == "operator string");
        bool isStaticFunc = isFunctionStatic(func);
        if (!structName.empty() && (isOperatorFunc || isStaticFunc))
        {
            name = structName + "." + rawFuncName;
        }
        else if (!structName.empty())
        {
            LLVMBackend::DeclTypeAndValue thisParam;
            thisParam.TypeName = structName;
            thisParam.VariableName = structName + "__";
            thisParam.Pointer = true;
            params.insert(params.begin(), thisParam);
        }

        std::vector<LLVMBackend::TypeAndValue> allParams(params.begin(), params.end());

        ApplyOwningSinkInference(compiler, func, allParams);

        bool returnsOwned = ComputeReturnsOwned(returnType, name, allParams);

        compiler->CreateFunctionDeclaration(name, returnType, allParams, returnType.external, varargs, returnsOwned, false, returnType.CallConv);

        // An unannotated by-value struct return whose every path hands back a borrowed
        // parameter is an 'alias' (borrow) return - queue the inference so callers do not
        // free the result. The unique-ownership gate is applied when the queue is resolved.
        {
            std::string borrowedParam;
            if (ClassifyValueStructReturns(compiler, func, returnType, allParams, &borrowedParam)
                == ValueStructReturnKind::AllBorrowedParam)
                compiler->QueueAliasReturnInference(name, returnType.TypeName, allParams);
        }

        // Populate RequiredLocks from the function's lock clause and any extra locks
        // inherited from a positional lock group (extraRequiredLocks).
        {
            std::vector<std::string> locks;
            if (auto* lockClauseCtx = func->lockClause())
            {
                // Arrow-normalized but suffix-BEARING: the mode is what tells the body seeding
                // whether the clause grants a write. Every consumer strips it (StripLockModeSuffix).
                for (auto* exprCtx : lockClauseCtx->lockArgList()->expression())
                    locks.push_back(ArrowNormalizeLockText(exprCtx->getText()));
            }
            for (const auto& extra : extraRequiredLocks)
                locks.push_back(extra);
            if (!locks.empty())
                compiler->SetFunctionRequiredLocks(name, std::move(locks));
        }

        if (auto* s = compiler->GetSymbolSink())
        {
            // Qualify member methods with their owning type ("double vec3.dot(...)"),
            // mirroring how namespace functions already carry their namespace in `name`.
            std::string displayName = structName.empty() ? name : structName + "." + rawFuncName;
            std::string sig = returnType.TypeName + (returnType.Pointer ? "*" : "") + " " + displayName + "(";
            bool first = true;
            for (const auto& p : params)
            {
                // Skip implicit 'this' pointer (name convention: TypeName__)
                if (p.VariableName.ends_with("__"))
                    continue;
                if (!first) sig += ", ";
                first = false;
                sig += p.TypeName;
                if (p.Pointer) sig += "*";
                if (!p.VariableName.empty()) sig += " " + p.VariableName;
            }
            sig += ")";
            std::string doc = ExtractLeadingDoc(tokens_, func->getStart());
            s->Register(SymbolKind::Function, name, compiler->GetSourceFilePath(),
                        (int)func->getStart()->getLine(), (int)func->getStart()->getCharPositionInLine(),
                        sig, {}, doc);

            // Namespace free functions also register under the unqualified name so a bare
            // hover/completion on "square" resolves to "Math.square" (mirrors the struct alias
            // below). Without this the doc comment and signature are unreachable from hover.
            if (structName.empty() && !namespaceName.empty())
                s->Register(SymbolKind::Function, rawFuncName, compiler->GetSourceFilePath(),
                            (int)func->getStart()->getLine(), (int)func->getStart()->getCharPositionInLine(),
                            sig, {}, doc);

            // Record an unused-function candidate. Only free functions: struct methods
            // can be re-emitted (and called) in another TU under monomorphization, so a
            // method unused in this file is not provably dead. Operators are dispatched
            // by symbol, not name token, so they never appear "used" - skip them too.
            if (structName.empty() && !isOperatorFunc && func->directDeclarator())
            {
                auto* nameDecl = func->directDeclarator();
                UnusedCandidate cand;
                cand.name = name;
                cand.kind = SymbolKind::Function;
                cand.file = compiler->GetSourceFilePath();
                cand.line = (int)nameDecl->getStart()->getLine();
                cand.col  = (int)nameDecl->getStart()->getCharPositionInLine();
                cand.isExported = returnType.external;  // extern -> visible to other TUs
                s->RegisterCandidate(cand);
            }

            // Also register under "TypeName.method" for dot-completion prefix lookup.
            // Operators and statics already have the qualified name; only instance methods need this.
            if (!structName.empty() && !isOperatorFunc && !isStaticFunc)
            {
                std::string qualName = structName + "." + rawFuncName;
                s->Register(SymbolKind::Function, qualName, compiler->GetSourceFilePath(),
                            (int)func->getStart()->getLine(), (int)func->getStart()->getCharPositionInLine(),
                            sig, {}, doc);
            }
        }

        // Pre-declare overloads for default parameters
        int firstDefault = -1;
        for (int i = 0; i < (int)params.size(); i++)
        {
            if (params[i].DefaultValue != nullptr) { firstDefault = i; break; }
        }
        for (int cutoff = firstDefault; firstDefault >= 0 && cutoff < (int)params.size(); cutoff++)
        {
            std::vector<LLVMBackend::TypeAndValue> wrapperParams(params.begin(), params.begin() + cutoff);
            compiler->CreateFunctionDeclaration(name, returnType, wrapperParams, false, false);
        }
    }

void ForwardRefScanner::ScanInterfaceDefinition(CFlatParser::InterfaceDefinitionContext* ctx,
                                 const std::string& namespaceName) {
        // Generic interface templates are not pre-declared; they are instantiated on demand.
        auto* nameGid = ctx->genericIdentifier();
        if (nameGid && nameGid->genericTypeParameters() != nullptr)
            return;

        if (!nameGid || !nameGid->Identifier()) return;
        // Registered under the enclosing namespace like a struct or class, so two namespaces may
        // each declare their own "IV" without one silently overwriting the other.
        std::string name = nameGid->Identifier()->getText();
        if (!namespaceName.empty())
            name = namespaceName + "." + name;

        // Record the interface's type-level annotations now (raw, unvalidated) so [uuid] is
        // available before a [winrt] class implementing it is emitted. The main pass validates.
        Compiler(ctx)->SetTypeAnnotations(name, ExtractAnnotations(ctx->annotationList()));

        std::vector<std::string> parentNames;
        for (auto* spec : ctx->baseSpecifier())
            parentNames.push_back(Compiler(ctx)->ResolveInterfaceName(BaseSpecifierName(spec)));

        std::vector<LLVMBackend::InterfaceMethod> methods;
        for (auto method : InterfaceMethods(ctx))
        {
            LLVMBackend::InterfaceMethod m;
            m.ReturnType = ParseDeclarationSpecifiers(method->declarationSpecifiers());
            m.Name = getInterfaceMethodName(method);
            auto declParams = ParseParameterTypeList(method->parameterTypeList());
            for (const auto& p : declParams)
                m.Parameters.push_back(p);
            methods.push_back(std::move(m));
        }

        std::vector<LLVMBackend::TypeAndValue> fields;
        for (auto* f : InterfaceFields(ctx))
        {
            LLVMBackend::TypeAndValue tv = ParseDeclarationSpecifiers(f->declarationSpecifiers());
            tv.VariableName = f->directDeclarator()->getText();
            fields.push_back(std::move(tv));
        }

        Compiler(ctx)->CreateInterfaceDefinition(name, parentNames, methods, fields,
                                                 DefinitionSiteText(Compiler(ctx), ctx));

        if (auto* s = Compiler(ctx)->GetSymbolSink())
        {
            std::string sig = "interface " + name;
            if (!parentNames.empty())
            {
                sig += " : ";
                for (size_t i = 0; i < parentNames.size(); ++i)
                {
                    if (i > 0) sig += ", ";
                    sig += parentNames[i];
                }
            }
            std::vector<std::string> memberNames;
            for (const auto& m : methods)
                memberNames.push_back(m.Name);
            for (const auto& f : fields)
                memberNames.push_back(f.VariableName);
            s->Register(SymbolKind::Interface, name, Compiler(ctx)->GetSourceFilePath(),
                        (int)ctx->getStart()->getLine(), (int)ctx->getStart()->getCharPositionInLine(),
                        sig, memberNames, ExtractLeadingDoc(tokens_, ctx->getStart()));

            // Interface FIELDS are reached like struct fields (`b.title`), so they need the same
            // "Type.field" symbols that dot-completion, hover and go-to-definition resolve
            // against. The interface's field list is already flattened over its parents, so an
            // inherited field is registered under this name too - pointing at the parent's
            // declaration site, which is where it is actually written.
            std::unordered_map<std::string, CFlatParser::InterfaceFieldContext*> ownFields;
            for (auto* f : InterfaceFields(ctx))
                ownFields[f->directDeclarator()->getText()] = f;

            const auto* allFields = Compiler(ctx)->GetInterfaceFields(name);
            static const std::vector<LLVMBackend::TypeAndValue> kNoFields;
            for (const auto& f : allFields != nullptr ? *allFields : kNoFields)
            {
                if (f.VariableName.empty()) continue;
                std::string typeSig = f.TypeName + (f.Pointer ? "*" : "");
                std::string key = name + "." + f.VariableName;

                if (auto it = ownFields.find(f.VariableName); it != ownFields.end())
                {
                    auto* fc = it->second;
                    s->Register(SymbolKind::Field, key, Compiler(ctx)->GetSourceFilePath(),
                                (int)fc->getStart()->getLine(), (int)fc->getStart()->getCharPositionInLine(),
                                typeSig + " " + key, {}, ExtractLeadingDoc(tokens_, fc->getStart()));
                    continue;
                }
                // Inherited: reuse the parent's recorded location and doc. Copy them out before
                // registering - Register rehashes the symbol map and would invalidate the pointer.
                for (const auto& parent : parentNames)
                {
                    const SymbolDef* pd = s->Lookup(parent + "." + f.VariableName);
                    if (pd == nullptr) continue;
                    std::string pFile = pd->file, pDoc = pd->docComment;
                    int pLine = pd->line, pCol = pd->column;
                    s->Register(SymbolKind::Field, key, pFile, pLine, pCol,
                                typeSig + " " + parent + "." + f.VariableName, {}, pDoc);
                    break;
                }
            }
        }
    }

void ForwardRefScanner::ScanStructDefinition(CFlatParser::StructDefinitionContext* ctx, const std::string& namespaceName) {
        ScanStructOrClassDefinition(ctx, namespaceName);
        // Record struct type-level annotations now (raw, like the class scan) so [Capability]
        // is visible before any body is walked. The main pass overwrites with the validated set.
        std::string typeName = ctx->directDeclarator()->getText();
        if (!namespaceName.empty()) typeName = namespaceName + "." + typeName;
        auto anns = ExtractAnnotations(ctx->annotationList());
        auto* compiler = Compiler(ctx);
        compiler->SetTypeAnnotations(typeName, anns);

        // Pre-register [Capability(...)] on the struct shell so a lock() lowered in a body
        // written ABOVE the lock type still classifies it. The main pass validates the names.
        if (ctx->genericTypeParameters() == nullptr)
        {
            std::vector<std::string> capIfaces;
            for (const auto& ann : anns)
                if (ann.Name == "Capability")
                    for (const auto& ifaceName : ann.Values)
                        capIfaces.push_back(ifaceName);
            if (!capIfaces.empty())
                compiler->RegisterStructStaticInterfaces(typeName, capIfaces);
        }
    }

void ForwardRefScanner::ScanClassDefinition(CFlatParser::ClassDefinitionContext* ctx, const std::string& namespaceName) {
        ScanStructOrClassDefinition(ctx, namespaceName);
        // Record class type-level annotations now (raw, like the interface scan) so
        // annotationof(Class,...) is available regardless of source order. The main pass
        // overwrites this with the validated set.
        std::string typeName = ctx->directDeclarator()->getText();
        if (!namespaceName.empty()) typeName = namespaceName + "." + typeName;
        Compiler(ctx)->SetTypeAnnotations(typeName, ExtractAnnotations(ctx->annotationList()));
    }

void ForwardRefScanner::ScanGlobalLockGroup(CFlatParser::LockFieldGroupContext* ctx, const std::string& namespaceName) {
        auto groupArgs = ctx->lockClause()->lockArgList()->expression();
        if (groupArgs.empty()) return;
        // Same canonicalization as GetLockArgCanonical in MainListener (both passes must agree).
        std::vector<std::string> groupLocks = { NormalizeLockText(groupArgs[0]->getText()) };
        for (auto* func : ctx->functionDefinition())
            ScanFunctionDefinition(func, {}, namespaceName, groupLocks);
    }

void ForwardRefScanner::RegisterRenameAlias(CFlatParser::UsingDeclarationContext* ctx) {
        if (ctx->Identifier() == nullptr) return;  // `using "name" = T;` keys on a string literal
        // A pointer/array suffix means the alias is not a pure rename.
        if (ctx->pointer() != nullptr || ctx->arrayTypeSuffix() != nullptr) return;
        auto* typeSpec = ctx->typeSpecifier();
        if (typeSpec == nullptr) return;
        std::string target = compilerLLVM->ResolveQualifiedName(typeSpec->getText());
        std::string alias = ctx->Identifier()->getText();
        if (alias == target || !IsBareTypeName(target)) return;
        compilerLLVM->RegisterManglingAlias(alias, target);
    }

ForwardRefScanner::ForwardRefScanner(LLVMBackend* compiler) : compilerLLVM(compiler) {}

void ForwardRefScanner::SetTokens(antlr4::BufferedTokenStream* t) { tokens_ = t; }

void ForwardRefScanner::PreRegisterRenameAliases(antlr4::RuleContext* ctx) {
        for (auto* child : ctx->children)
        {
            auto* ruleCtx = dynamic_cast<antlr4::RuleContext*>(child);
            if (ruleCtx == nullptr) continue;
            switch (ruleCtx->getRuleIndex())
            {
            case CFlatParser::RuleUsingDeclaration:
                RegisterRenameAlias(static_cast<CFlatParser::UsingDeclarationContext*>(ruleCtx));
                break;
            case CFlatParser::RuleExternalDeclaration:
                PreRegisterRenameAliases(ruleCtx);
                break;
            case CFlatParser::RuleNamespaceDefinition:
            {
                auto* nsCtx = static_cast<CFlatParser::NamespaceDefinitionContext*>(ruleCtx);
                LLVMBackend::NamespaceScope nsScope(
                    compilerLLVM, NestedNamespaceName(compilerLLVM->GetCurrentNamespace(), nsCtx));
                PreRegisterRenameAliases(ruleCtx);
                break;
            }
            default:
                break;
            }
        }
    }

std::string ForwardRefScanner::ResolveForwardTypeArg(CFlatParser::TypeParameterEntryContext* entry) {
        bool isUnique = TypeArgHasUnique(entry);
        bool isAlias = TypeArgHasAlias(entry);
        auto* typeSpec = entry->typeSpecifier();
        std::string resolved;
        std::string innerBase;
        if (auto* innerParams = GenericSpecOf(typeSpec, innerBase))
        {
            resolved = Compiler(entry)->ResolveGenericBaseAlias(innerBase);
            for (auto* innerEntry : innerParams->typeParameterList()->typeParameterEntry())
                resolved += "__" + MangleTypeArg(compilerLLVM, ResolveForwardTypeArg(innerEntry));
        }
        else if (typeSpec && typeSpec->functionPointerSpecifier())
        {
            // Closure type as a generic argument (gap a): encode to a symbol-safe name so the
            // shell name (e.g. list____fatfn_1_3_int_3_int) matches the main pass.
            resolved = EncodeClosureScanner(typeSpec->functionPointerSpecifier());
        }
        else
        {
            // Same namespace walk as the main pass's ResolveTypeArgEntry (no active substitutions
            // during the scan), so a bare 'Item' inside 'namespace A' names A.Item in both passes.
            resolved = Compiler(entry)->ResolveTypeArgBaseName(
                typeSpec ? typeSpec->getText() : entry->getText());
        }
        // `T[]` array-view arg encodes as a "[]" suffix (mirrors "*" for a pointer); the bad
        // bracket forms are rejected in the main pass, so the forward scan just names them.
        // Stars are COUNTED here, exactly as the main pass counts them: a shell named Box__Cptr
        // for a Box<C**> use would not match the instantiation the codegen pass builds.
        if (entry->pointer() != nullptr)
            resolved += std::string(PointerDepthOf(entry->pointer()), '*');
        else if (IsArrayViewArg(entry))
            resolved += "[]";
        // Carry the `unique` / `alias` qualifier into the mangled/instantiation string; the main
        // pass validates position/type and emits diagnostics. The two are mutually exclusive.
        if (isUnique)
            resolved = std::string(kUniqueQualifierPrefix) + resolved;
        else if (isAlias)
            resolved = std::string(kAliasQualifierPrefix) + resolved;
        return resolved;
    }

std::string ForwardRefScanner::ResolveSigComponentScanner(CFlatParser::TypeSpecifierContext* ts, bool& outPointer) {
        (void)outPointer;
        if (ts == nullptr) return "void";
        if (ts->functionPointerSpecifier() != nullptr)
            return EncodeClosureScanner(ts->functionPointerSpecifier());
        if (ts->genericIdentifier() != nullptr && ts->genericIdentifier()->genericTypeParameters() != nullptr)
        {
            std::string mangled = ts->genericIdentifier()->Identifier()->getText();
            for (auto* entry : ts->genericIdentifier()->genericTypeParameters()->typeParameterList()->typeParameterEntry())
                mangled += "__" + MangleTypeArg(compilerLLVM, ResolveForwardTypeArg(entry));
            return mangled;
        }
        return ts->getText();
    }

std::string ForwardRefScanner::SigComponentResolvedKeyScanner(const std::string& name) {
        if (name.empty()) return "";
        std::string key = compilerLLVM->ResolveTypeArgBaseName(name);
        if (key != name) return key;                              // qualified by the walk
        if (name.find('.') != std::string::npos) return name;      // already qualified
        return compilerLLVM->GetCurrentNamespace().empty() ? name : "";  // global scope is unambiguous
    }

std::string ForwardRefScanner::EncodeClosureScanner(CFlatParser::FunctionPointerSpecifierContext* fpSpec) {
        bool isThin = fpSpec->Function() != nullptr;
        if (fpSpec->typeSpecifier() == nullptr)
            return isThin ? "__c_fn_ptr" : "__closure_fat_ptr";  // main pass reports the error
        bool retPtr = fpSpec->pointer() != nullptr;
        int retStars = PointerDepthOf(fpSpec->pointer());
        std::string retName = ResolveSigComponentScanner(fpSpec->typeSpecifier(), retPtr);
        std::vector<std::pair<std::string, int>> encParams;
        if (fpSpec->functionPointerParamList() != nullptr)
            for (auto* param : fpSpec->functionPointerParamList()->functionPointerParam())
            {
                bool pPtr = param->pointer() != nullptr;
                int pStars = PointerDepthOf(param->pointer());
                std::string pName = ResolveSigComponentScanner(param->typeSpecifier(), pPtr);
                encParams.push_back({ pName, ReconcilePointerDepth(pPtr, pStars) });
            }
        return BuildEncodedClosureName(compilerLLVM, isThin, retName, ReconcilePointerDepth(retPtr, retStars), encParams);
    }

std::optional<int64_t> ForwardRefScanner::ScannerFoldIfConst(antlr4::tree::ParseTree* node) {
        if (node == nullptr) return std::nullopt;
        if (auto* c = dynamic_cast<CFlatParser::ConditionalExpressionContext*>(node))
        {
            if (c->expression() != nullptr)
            {
                auto cond = ScannerFoldIfConst(c->logicalOrExpression());
                if (!cond) return std::nullopt;
                return (*cond != 0) ? ScannerFoldIfConst(c->expression())
                                    : ScannerFoldIfConst(c->conditionalExpression());
            }
            if (c->children.size() > 1) return std::nullopt;  // `??` null-coalescing
            return ScannerFoldIfConst(c->logicalOrExpression());
        }
        if (auto* o = dynamic_cast<CFlatParser::LogicalOrExpressionContext*>(node))
        {
            auto operands = o->logicalAndExpression();
            if (operands.size() == 1) return ScannerFoldIfConst(operands[0]);
            bool allKnownFalse = true;
            for (auto* op : operands)
            {
                auto v = ScannerFoldIfConst(op);
                if (v && *v != 0) return (int64_t)1;
                if (!v) allKnownFalse = false;
            }
            return allKnownFalse ? std::optional<int64_t>(0) : std::nullopt;
        }
        if (auto* a = dynamic_cast<CFlatParser::LogicalAndExpressionContext*>(node))
        {
            auto operands = a->inclusiveOrExpression();
            if (operands.size() == 1) return ScannerFoldIfConst(operands[0]);
            bool allKnownTrue = true;
            for (auto* op : operands)
            {
                auto v = ScannerFoldIfConst(op);
                if (v && *v == 0) return (int64_t)0;
                if (!v) allKnownTrue = false;
            }
            return allKnownTrue ? std::optional<int64_t>(1) : std::nullopt;
        }
        if (auto* e = dynamic_cast<CFlatParser::ExpressionContext*>(node))
            return ScannerFoldIfConst(e->assignmentExpression());
        if (auto* asn = dynamic_cast<CFlatParser::AssignmentExpressionContext*>(node))
        {
            if (asn->conditionalExpression() != nullptr)
                return ScannerFoldIfConst(asn->conditionalExpression());
            return std::nullopt;  // an actual assignment is not a constant
        }
        return ScannerFoldIfConstLeaf(node);
    }

std::optional<int64_t> ForwardRefScanner::ScannerFoldIfConstLeaf(antlr4::tree::ParseTree* node) {
        if (node == nullptr) return std::nullopt;

        // Left-associative binary chains: fold pairwise using the operator token between operands.
        auto foldChain = [&](const std::vector<antlr4::tree::ParseTree*>& ops,
                            antlr4::RuleContext* parent) -> std::optional<int64_t>
        {
            if (ops.empty()) return std::nullopt;
            auto acc = ScannerFoldIfConstLeaf(ops[0]);
            if (ops.size() == 1 || !acc) return acc;
            // Walk the parent's children to recover the operator tokens in source order.
            size_t opIdx = 0;
            for (size_t i = 0; i < parent->children.size(); i++)
            {
                auto* term = dynamic_cast<antlr4::tree::TerminalNode*>(parent->children[i]);
                if (term == nullptr) continue;
                std::string op = term->getText();
                if (op == "<" || op == ">")
                {
                    // '>' '>' is a shift written as two tokens; join it when adjacent.
                    if (i + 1 < parent->children.size())
                        if (auto* n2 = dynamic_cast<antlr4::tree::TerminalNode*>(parent->children[i + 1]))
                            if (op == ">" && n2->getText() == ">") { op = ">>"; i++; }
                }
                if (++opIdx > ops.size() - 1) break;
                auto rhs = ScannerFoldIfConstLeaf(ops[opIdx]);
                if (!rhs) return std::nullopt;
                int64_t l = *acc, r = *rhs, out = 0;
                // Every operand is already inside the 32-bit range (ParseScannerIntegerLiteral and
                // the macro leaf both guarantee it), so computing in int64 and rejecting an
                // out-of-range RESULT means no fold can differ from codegen's i32 arithmetic by a
                // wraparound - it becomes undecidable instead.
                if (op == "|")       out = l | r;
                else if (op == "^")  out = l ^ r;
                else if (op == "&")  out = l & r;
                else if (op == "==") out = (l == r) ? 1 : 0;
                else if (op == "!=") out = (l != r) ? 1 : 0;
                else if (op == "<")  out = (l < r) ? 1 : 0;
                else if (op == ">")  out = (l > r) ? 1 : 0;
                else if (op == "<=") out = (l <= r) ? 1 : 0;
                else if (op == ">=") out = (l >= r) ? 1 : 0;
                else if (op == "<<") { if (r < 0 || r > 31) return std::nullopt; out = l << r; }
                else if (op == ">>") { if (r < 0 || r > 31) return std::nullopt; out = l >> r; }
                else if (op == "+")  { if (ScannerAddOverflow(l, r, &out)) return std::nullopt; }
                else if (op == "-")  { if (ScannerSubOverflow(l, r, &out)) return std::nullopt; }
                else if (op == "*")  { if (ScannerMulOverflow(l, r, &out)) return std::nullopt; }
                else if (op == "/")  { if (r == 0) return std::nullopt; out = l / r; }
                else if (op == "%")  { if (r == 0) return std::nullopt; out = l % r; }
                else return std::nullopt;
                if (!InScannerInt32Range(out)) return std::nullopt;
                acc = out;
            }
            return acc;
        };

        if (auto* n = dynamic_cast<CFlatParser::InclusiveOrExpressionContext*>(node))
        {
            auto ops = n->exclusiveOrExpression();
            return foldChain({ops.begin(), ops.end()}, n);
        }
        if (auto* n = dynamic_cast<CFlatParser::ExclusiveOrExpressionContext*>(node))
        {
            auto ops = n->andExpression();
            return foldChain({ops.begin(), ops.end()}, n);
        }
        if (auto* n = dynamic_cast<CFlatParser::AndExpressionContext*>(node))
        {
            auto ops = n->equalityExpression();
            return foldChain({ops.begin(), ops.end()}, n);
        }
        if (auto* n = dynamic_cast<CFlatParser::EqualityExpressionContext*>(node))
        {
            auto ops = n->typeCheckExpression();
            return foldChain({ops.begin(), ops.end()}, n);
        }
        if (auto* n = dynamic_cast<CFlatParser::TypeCheckExpressionContext*>(node))
        {
            // `is` / `as` are not integer constants; a bare relational passes through.
            if (n->children.size() != 1) return std::nullopt;
            return ScannerFoldIfConstLeaf(n->relationalExpression());
        }
        if (auto* n = dynamic_cast<CFlatParser::RelationalExpressionContext*>(node))
        {
            auto ops = n->shiftExpression();
            return foldChain({ops.begin(), ops.end()}, n);
        }
        if (auto* n = dynamic_cast<CFlatParser::ShiftExpressionContext*>(node))
        {
            auto ops = n->additiveExpression();
            return foldChain({ops.begin(), ops.end()}, n);
        }
        if (auto* n = dynamic_cast<CFlatParser::AdditiveExpressionContext*>(node))
        {
            auto ops = n->multiplicativeExpression();
            return foldChain({ops.begin(), ops.end()}, n);
        }
        if (auto* n = dynamic_cast<CFlatParser::MultiplicativeExpressionContext*>(node))
        {
            auto ops = n->castExpression();
            return foldChain({ops.begin(), ops.end()}, n);
        }
        if (auto* n = dynamic_cast<CFlatParser::CastExpressionContext*>(node))
        {
            if (n->typeName() != nullptr) return std::nullopt;  // a cast is not folded here
            if (n->unaryExpression() != nullptr) return ScannerFoldIfConstLeaf(n->unaryExpression());
            return ParseScannerIntegerLiteral(n->getText());
        }
        if (auto* n = dynamic_cast<CFlatParser::UnaryExpressionContext*>(node))
        {
            if (n->postfixExpression() != nullptr) return ScannerFoldIfConstLeaf(n->postfixExpression());
            if (n->unaryOperator() != nullptr && n->castExpression() != nullptr)
            {
                auto v = ScannerFoldIfConstLeaf(n->castExpression());
                if (!v) return std::nullopt;
                std::string op = n->unaryOperator()->getText();
                if (op == "!") return (*v == 0) ? (int64_t)1 : (int64_t)0;
                if (op == "+") return *v;
                int64_t out = 0;
                if (op == "-")      { if (ScannerSubOverflow((int64_t)0, *v, &out)) return std::nullopt; }
                else if (op == "~") out = ~*v;
                else return std::nullopt;  // '&' / '*' are not integer constants
                return InScannerInt32Range(out) ? std::optional<int64_t>(out) : std::nullopt;
            }
            return std::nullopt;      // sizeof/alignof/new/delete/move/operator
        }
        if (auto* n = dynamic_cast<CFlatParser::PostfixExpressionContext*>(node))
        {
            // Only a bare primary folds - a call, index or member access does not.
            if (n->children.size() != 1) return std::nullopt;
            return ScannerFoldIfConstLeaf(n->primaryExpression());
        }
        if (auto* n = dynamic_cast<CFlatParser::PrimaryExpressionContext*>(node))
        {
            if (n->expression() != nullptr && n->children.size() == 3)
                return ScannerFoldIfConstLeaf(n->expression());   // redundant parens
            if (n->expression() != nullptr) return std::nullopt;  // nameof/typeof(expr)
            if (auto* gid = n->genericIdentifier())
            {
                if (gid->genericTypeParameters() != nullptr || gid->Identifier() == nullptr)
                    return std::nullopt;
                const auto& macros = compilerLLVM->compileTimeMacros;
                auto it = macros.find(gid->Identifier()->getText());
                if (it == macros.end()) return std::nullopt;
                auto* ci = llvm::dyn_cast_or_null<llvm::ConstantInt>(it->second.value);
                if (ci == nullptr || it->second.type != "int") return std::nullopt;
                int64_t mv = (int64_t)ci->getSExtValue();
                return InScannerInt32Range(mv) ? std::optional<int64_t>(mv) : std::nullopt;
            }
            return ParseScannerIntegerLiteral(n->getText());
        }
        if (auto* e = dynamic_cast<CFlatParser::ExpressionContext*>(node))
            return ScannerFoldIfConst(e);
        return std::nullopt;
    }

bool ForwardRefScanner::InScannerInt32Range(int64_t v) {
        return v >= (int64_t)INT32_MIN && v <= (int64_t)INT32_MAX;
    }

bool ForwardRefScanner::ScannerAddOverflow(int64_t a, int64_t b, int64_t* out) {
        *out = (int64_t)((uint64_t)a + (uint64_t)b);
        return ((a ^ *out) & (b ^ *out)) < 0;
    }

bool ForwardRefScanner::ScannerSubOverflow(int64_t a, int64_t b, int64_t* out) {
        *out = (int64_t)((uint64_t)a - (uint64_t)b);
        return ((a ^ b) & (a ^ *out)) < 0;
    }

bool ForwardRefScanner::ScannerMulOverflow(int64_t a, int64_t b, int64_t* out) {
        *out = (int64_t)((uint64_t)a * (uint64_t)b);
        if (a == 0) return false;
        if (a == -1 && b == INT64_MIN) return true;
        return *out / a != b;
    }

std::optional<int64_t> ForwardRefScanner::ParseScannerIntegerLiteral(const std::string& textIn) {
        if (textIn == "true") return (int64_t)1;
        if (textIn == "false") return (int64_t)0;
        std::string text = textIn;
        if (!text.empty() && (text.back() == 'u' || text.back() == 'U'
                           || text.back() == 'l' || text.back() == 'L'))
            return std::nullopt;   // suffixed literal - codegen's type differs from ours
        bool neg = false;
        size_t pos = 0;
        if (!text.empty() && text[0] == '-') { neg = true; pos = 1; }
        if (pos >= text.size()) return std::nullopt;
        int base = 10;
        if (text.size() > pos + 1 && text[pos] == '0')
        {
            char c = text[pos + 1];
            if (c == 'x' || c == 'X')      { base = 16; pos += 2; }
            else if (c == 'b' || c == 'B') { base = 2;  pos += 2; }
            else return std::nullopt;      // C octal (leading 0) - not folded here
        }
        if (pos >= text.size()) return std::nullopt;
        uint64_t v = 0;
        auto res = std::from_chars(text.data() + pos, text.data() + text.size(), v, base);
        if (res.ec != std::errc() || res.ptr != text.data() + text.size()) return std::nullopt;
        if (v > (uint64_t)INT32_MAX) return std::nullopt;
        int64_t out = neg ? -(int64_t)v : (int64_t)v;
        return InScannerInt32Range(out) ? std::optional<int64_t>(out) : std::nullopt;
    }

int ForwardRefScanner::ScannerDecideIfConst(CFlatParser::ExpressionContext* expr) {
        auto v = ScannerFoldIfConst(expr);
        if (!v) return -1;
        return (*v != 0) ? 1 : 0;
    }

void ForwardRefScanner::CollectGenericTemplateDeclsIfConst(CFlatParser::IfConstDeclarationContext* ctx, bool certain,
                                            bool ifConstUnfoldable, const std::string& ns) {
        int decision = ScannerDecideIfConst(ctx->expression());
        auto ifBlocks = ctx->ifConstBlock();
        if (ifBlocks.empty()) return;
        if (decision < 0)
        {
            // Unfoldable condition: not certain, AND the reason genuinely IS `if const` - the only
            // context permitted to claim `if const` as a cause downstream.
            for (auto* blk : ifBlocks) CollectGenericTemplateDecls(blk, false, /*ifConstUnfoldable*/ true, ns);
            if (auto* elseIf = ctx->ifConstDeclaration()) CollectGenericTemplateDeclsIfConst(elseIf, false, true, ns);
            return;
        }
        if (decision != 0)
        {
            CollectGenericTemplateDecls(ifBlocks[0], certain, ifConstUnfoldable, ns);
            return;
        }
        if (auto* elseIf = ctx->ifConstDeclaration()) CollectGenericTemplateDeclsIfConst(elseIf, certain, ifConstUnfoldable, ns);
        else if (ifBlocks.size() > 1) CollectGenericTemplateDecls(ifBlocks[1], certain, ifConstUnfoldable, ns);
    }

void ForwardRefScanner::CollectGenericTemplateDeclsIfConstMember(CFlatParser::IfConstMemberContext* ctx, bool certain,
                                                 bool ifConstUnfoldable, const std::string& ns,
                                                 const std::string& typePath, bool unkeyable) {
        int decision = ScannerDecideIfConst(ctx->expression());
        auto ifBlocks = ctx->ifConstMemberBlock();
        if (ifBlocks.empty()) return;
        if (decision < 0)
        {
            for (auto* blk : ifBlocks) CollectGenericTemplateDecls(blk, false, /*ifConstUnfoldable*/ true, ns, typePath, unkeyable);
            if (auto* elseIf = ctx->ifConstMember()) CollectGenericTemplateDeclsIfConstMember(elseIf, false, true, ns, typePath, unkeyable);
            return;
        }
        if (decision != 0)
        {
            CollectGenericTemplateDecls(ifBlocks[0], certain, ifConstUnfoldable, ns, typePath, unkeyable);
            return;
        }
        if (auto* elseIf = ctx->ifConstMember()) CollectGenericTemplateDeclsIfConstMember(elseIf, certain, ifConstUnfoldable, ns, typePath, unkeyable);
        else if (ifBlocks.size() > 1) CollectGenericTemplateDecls(ifBlocks[1], certain, ifConstUnfoldable, ns, typePath, unkeyable);
    }

void ForwardRefScanner::CollectGenericTemplateDecls(antlr4::RuleContext* ctx, bool certain, bool ifConstUnfoldable,
                                     const std::string& ns, const std::string& typePath,
                                     bool unkeyable) {
        auto* compiler = compilerLLVM;
        /*
         * Record every type DEFINITION name in scannedTypeNames (the type-argument accept set)
         * under the key it is ACTUALLY registered with. A type nested in a struct is keyed with the
         * outer struct's full name (ScanStructOrClassDefinition passes it as the nested scan's
         * `namespaceName`), so `typePath` accumulates the enclosing struct/class names - the key is
         * never re-derived from a dotted string. Suppressing nested types instead left their only
         * source as dataStructures, which fills in textual order during the main pass, so one
         * spelling in one namespace got two answers depending on line order.
         * `unkeyable` covers a body whose nested types have NO fixed key: inside a GENERIC template
         * the real key is per-instantiation (Box__int.Inner), not Box.Inner.
         * `certain` gates the whole thing: it is false inside an unfoldable `if const` arm (a dead
         * arm's type never becomes a real key) and inside an expect_error block (whose type may fail
         * to register while a claim here would redirect a later argument to a key that never
         * appears). An invented key is a false rejection, so both must stay out.
         */
        auto recordTypeName = [&](const std::string& name)
        {
            if (certain && !unkeyable && !name.empty())
                compiler->gts.scannedTypeNames.insert(QualifyName(QualifyName(ns, typePath), name));
        };
        for (auto* child : ctx->children)
        {
            auto* ruleCtx = dynamic_cast<antlr4::RuleContext*>(child);
            if (!ruleCtx) continue;
            switch (ruleCtx->getRuleIndex())
            {
            case CFlatParser::RuleNamespaceDefinition:
                // Every name declared below here is keyed qualified, matching the main pass.
                CollectGenericTemplateDecls(ruleCtx, certain, ifConstUnfoldable,
                                            NestedNamespaceName(ns, static_cast<CFlatParser::NamespaceDefinitionContext*>(ruleCtx)),
                                            typePath, unkeyable);
                continue;
            case CFlatParser::RuleIfConstDeclaration:
                CollectGenericTemplateDeclsIfConst(
                    static_cast<CFlatParser::IfConstDeclarationContext*>(ruleCtx), certain, ifConstUnfoldable, ns);
                continue;
            case CFlatParser::RuleIfConstMember:
                CollectGenericTemplateDeclsIfConstMember(
                    static_cast<CFlatParser::IfConstMemberContext*>(ruleCtx), certain, ifConstUnfoldable, ns,
                    typePath, unkeyable);
                continue;
            case CFlatParser::RuleExpectErrorDeclaration:
                // An expect_error block is compiled but its errors are swallowed, so a template
                // declared inside it must not veto (or claim) a name used outside the block. It is
                // NOT if-const-unfoldable: passing that on made the diagnostic blame `if const` on a
                // file containing none (scratch/rev6/g1_expect_error_false_ifconst_hint.cb).
                CollectGenericTemplateDecls(ruleCtx, false, /*ifConstUnfoldable*/ false, ns, typePath, unkeyable);
                continue;
            case CFlatParser::RuleStructDefinition:
            {
                auto* sd = static_cast<CFlatParser::StructDefinitionContext*>(ruleCtx);
                std::string tn = sd->directDeclarator() != nullptr ? sd->directDeclarator()->getText() : std::string{};
                recordTypeName(tn);
                if (sd->genericTypeParameters() != nullptr && !tn.empty())
                {
                    if (certain) RecordScannedGenericStructName(QualifyName(ns, tn));
                    else compiler->gts.scannedGenericStructNamesUncertain.insert(QualifyName(ns, tn));
                }
                CollectGenericTemplateDecls(ruleCtx, certain, ifConstUnfoldable, ns, QualifyName(typePath, tn),
                                            unkeyable || sd->genericTypeParameters() != nullptr);
                continue;
            }
            case CFlatParser::RuleClassDefinition:
            {
                auto* cd = static_cast<CFlatParser::ClassDefinitionContext*>(ruleCtx);
                std::string tn = cd->directDeclarator() != nullptr ? cd->directDeclarator()->getText() : std::string{};
                recordTypeName(tn);
                if (cd->genericTypeParameters() != nullptr && !tn.empty())
                {
                    if (certain) RecordScannedGenericStructName(QualifyName(ns, tn));
                    else compiler->gts.scannedGenericStructNamesUncertain.insert(QualifyName(ns, tn));
                }
                CollectGenericTemplateDecls(ruleCtx, certain, ifConstUnfoldable, ns, QualifyName(typePath, tn),
                                            unkeyable || cd->genericTypeParameters() != nullptr);
                continue;
            }
            case CFlatParser::RuleInterfaceDefinition:
            {
                auto* nameGid = static_cast<CFlatParser::InterfaceDefinitionContext*>(ruleCtx)->genericIdentifier();
                if (nameGid && nameGid->Identifier())
                    recordTypeName(nameGid->Identifier()->getText());
                if (nameGid && nameGid->Identifier() && nameGid->genericTypeParameters() != nullptr)
                {
                    std::string key = QualifyName(ns, nameGid->Identifier()->getText());
                    compiler->gts.scannedGenericInterfaceNames.insert(key);
                    // Only genuine if-const UNFOLDABILITY may claim `if const` as a cause. `certain`
                    // is ALSO false inside an expect_error block, which has nothing to do with
                    // `if const` - conflating the two reasons made the diagnostic blame `if const`
                    // on files containing none, so they must stay distinct.
                    if (ifConstUnfoldable)
                        compiler->gts.ifConstUncertainInterfaceNames.insert(key);
                }
                break;
            }
            case CFlatParser::RuleFunctionDefinition:
            {
                auto* fd = static_cast<CFlatParser::FunctionDefinitionContext*>(ruleCtx);
                auto* gp = fd->genericTypeParameters();
                if (gp != nullptr)
                {
                    std::string fn = getFunctionName(fd);
                    if (!fn.empty())
                    {
                        std::string key = QualifyName(QualifyName(ns, typePath), fn);
                        compiler->gts.genericFunctionTemplates[key] = fd;
                        compiler->gts.genericTemplateNamespace[key] = ns;
                        std::vector<std::string> params;
                        for (auto* entry : gp->typeParameterList()->typeParameterEntry())
                            params.push_back(entry->typeSpecifier() != nullptr
                                ? entry->typeSpecifier()->getText() : entry->getText());
                        compiler->gts.genericFunctionTypeParams[key] = params;
                        std::unordered_map<std::string, std::vector<std::string>> constraints;
                        if (auto* where = fd->whereClause(); where != nullptr)
                        {
                            for (auto* constraint : where->typeParameterConstraint())
                            {
                                auto ids = constraint->Identifier();
                                if (ids.size() >= 2)
                                    constraints[ids[0]->getText()].push_back(ids[1]->getText());
                            }
                        }
                        compiler->gts.genericFunctionConstraints[key] = std::move(constraints);
                        bool hasPack = !params.empty()
                            && !gp->typeParameterList()->typeParameterEntry().empty()
                            && gp->typeParameterList()->typeParameterEntry().back()->Ellipsis() != nullptr;
                        compiler->gts.genericFunctionPackIndex[key] =
                            hasPack ? params.size() - 1 : std::string::npos;
                    }
                }
                break;
            }
            }
            CollectGenericTemplateDecls(ruleCtx, certain, ifConstUnfoldable, ns, typePath, unkeyable);
        }
    }

std::string ForwardRefScanner::QualifyName(const std::string& ns, const std::string& name) {
        return ns.empty() ? name : ns + "." + name;
    }

std::string ForwardRefScanner::NestedNamespaceName(const std::string& ns, CFlatParser::NamespaceDefinitionContext* ctx) {
        std::string inner;
        for (auto* id : ctx->Identifier())
            inner += (inner.empty() ? "" : ".") + id->getText();
        return QualifyName(ns, inner);
    }

void ForwardRefScanner::RecordScannedGenericStructName(const std::string& name) {
        compilerLLVM->gts.scannedGenericStructNames.insert(name);
        compilerLLVM->RevokeGenericInterfaceInstances(name);
    }

void ForwardRefScanner::ScanGenericInterfaceTemplateNames(antlr4::RuleContext* ctx) {
        CollectGenericTemplateDecls(ctx, /*certain*/ true);
    }

void ForwardRefScanner::ScanGenericTypeUses(antlr4::RuleContext* ctx) {
        for (auto* child : ctx->children)
        {
            auto* ruleCtx = dynamic_cast<antlr4::RuleContext*>(child);
            if (!ruleCtx) continue;

            auto tryPreDeclare = [&](const std::string& spelledBase, CFlatParser::GenericTypeParametersContext* genericParams)
            {
                auto* compiler = Compiler(genericParams);
                // No evidence of a generic TYPE template by this name anywhere: pre-declaring an
                // opaque shell here would suppress the `unknown type` diagnostic the use is owed.
                if (!compiler->AnyGenericTypeTemplateNamed(spelledBase))
                    return;
                // Name the shell after the registered KEY, not the spelling: a bare use inside a
                // namespace would otherwise leave an opaque 'Box__int' that 'NS.Box__int' never completes.
                std::string baseName = compiler->ResolveGenericTemplateBase(spelledBase);
                std::string mangledName = baseName;
                for (auto* entry : genericParams->typeParameterList()->typeParameterEntry())
                    mangledName += "__" + MangleTypeArg(compiler, ResolveForwardTypeArg(entry));
                // A generic INTERFACE instantiation has no struct shell and no default ctor - the
                // main pass builds it in interfaceTable. Pre-declaring one shadows it as opaque.
                if (compiler->IsGenericInterfaceTemplateName(
                        compiler->ResolveGenericBaseAlias(spelledBase)))
                {
                    compiler->gts.genericInterfaceInstances.insert(mangledName);
                    return;
                }
                compiler->CreateStructType(mangledName, {});
                LLVMBackend::TypeAndValue returnType{ .TypeName = mangledName };
                compiler->CreateFunctionDeclaration(mangledName, returnType, {});
            };

            // Use getRuleIndex() (integer compare) instead of dynamic_cast for each node type
            // to avoid the RTTI hierarchy walk (FindSITargetTypeInstance) on every tree node.
            switch (ruleCtx->getRuleIndex())
            {
            case CFlatParser::RuleNamespaceDefinition:
            {
                // Make the enclosing namespace visible so a bare 'Box<int>' written inside
                // 'namespace NS' resolves to the NS.Box template key, as the main pass does.
                auto* nsCtx = static_cast<CFlatParser::NamespaceDefinitionContext*>(ruleCtx);
                LLVMBackend::NamespaceScope nsScope(
                    compilerLLVM, NestedNamespaceName(compilerLLVM->GetCurrentNamespace(), nsCtx));
                ScanGenericTypeUses(ruleCtx);
                continue;
            }
            case CFlatParser::RuleStructDefinition:
                // Skip generic template definitions - bodies contain unbound type parameters (e.g. T).
                if (static_cast<CFlatParser::StructDefinitionContext*>(ruleCtx)->genericTypeParameters() != nullptr)
                    continue;
                break;
            case CFlatParser::RuleClassDefinition:
                if (static_cast<CFlatParser::ClassDefinitionContext*>(ruleCtx)->genericTypeParameters() != nullptr)
                    continue;
                break;
            case CFlatParser::RuleFunctionDefinition:
                // Skip generic function template definitions for the same reason.
                if (static_cast<CFlatParser::FunctionDefinitionContext*>(ruleCtx)->genericTypeParameters() != nullptr)
                    continue;
                break;
            case CFlatParser::RuleTypeSpecifier:
                {
                    auto* typeSpec = static_cast<CFlatParser::TypeSpecifierContext*>(ruleCtx);
                    if (typeSpec->genericIdentifier() != nullptr && typeSpec->genericIdentifier()->genericTypeParameters() != nullptr && typeSpec->genericIdentifier()->Identifier() != nullptr)
                        tryPreDeclare(typeSpec->genericIdentifier()->Identifier()->getText(), typeSpec->genericIdentifier()->genericTypeParameters());
                    // The qualified spelling 'NS.Box<int>', only when it names a CFlat template key:
                    // an imported winmd generic is built elsewhere and must get no opaque shell.
                    if (std::string qBase; auto* qParams = GenericSpecOf(typeSpec, qBase))
                        if (typeSpec->qualifiedGenericIdentifier() != nullptr
                            && Compiler(typeSpec)->IsGenericTemplateKey(qBase))
                            tryPreDeclare(qBase, qParams);

                    // Tuple type sugar: (T1, T2) -> pre-declare tuple__T1__T2
                    if (typeSpec->tupleTypeSpecifier() != nullptr)
                    {
                        auto* tts = typeSpec->tupleTypeSpecifier();
                        // Pack-only form (T...) resolved during instantiation - skip forward-declare here
                        if (tts->tupleTypePackEntry() == nullptr)
                        {
                            std::vector<std::string> typeArgs;
                            for (auto* entry : tts->tupleTypeEntry())
                                typeArgs.push_back(TupleEntryArgName(Compiler(tts), entry));
                            std::string mangledName = "tuple";
                            for (const auto& arg : typeArgs)
                                mangledName += "__" + MangleTypeArg(Compiler(tts), arg);
                            auto* c = Compiler(tts);
                            c->CreateStructType(mangledName, {});
                            LLVMBackend::TypeAndValue rt{ .TypeName = mangledName };
                            c->CreateFunctionDeclaration(mangledName, rt, {});
                            c->gts.tupleTypeArgs[mangledName] = typeArgs;
                        }
                    }
                    break;
                }
            case CFlatParser::RulePrimaryExpression:
                {
                    auto* primaryExpr = static_cast<CFlatParser::PrimaryExpressionContext*>(ruleCtx);
                    if (primaryExpr->genericIdentifier() != nullptr && primaryExpr->genericIdentifier()->genericTypeParameters() != nullptr && primaryExpr->genericIdentifier()->Identifier() != nullptr)
                        tryPreDeclare(primaryExpr->genericIdentifier()->Identifier()->getText(), primaryExpr->genericIdentifier()->genericTypeParameters());
                    break;
                }
            }

            ScanGenericTypeUses(ruleCtx);
        }
    }

LLVMBackend::TypeAndValue ForwardRefScanner::BuildFuncPtrAliasType(CFlatParser::FunctionPointerSpecifierContext* fpSpec) {
        LLVMBackend::TypeAndValue tv;
        tv.IsFunctionPointer = true;
        // `function` = thin C ptr ("__c_fn_ptr"); `Lambda` = fat ("__closure_fat_ptr"). IsThinFnPtr() derives from this.
        tv.TypeName = (fpSpec->Function() != nullptr) ? "__c_fn_ptr" : "__closure_fat_ptr";
        if (fpSpec->typeSpecifier() != nullptr)
        {
            // Resolve generic signature types (gap b) to match the main pass.
            bool retPtr = fpSpec->pointer() != nullptr;
            int retStars = PointerDepthOf(fpSpec->pointer());
            tv.FuncPtrReturnTypeName = ResolveSigComponentScanner(fpSpec->typeSpecifier(), retPtr);
            tv.FuncPtrReturnPointer  = retPtr;
            tv.FuncPtrReturnPointerDepth = ReconcilePointerDepth(retPtr, retStars);
            tv.FuncPtrReturnResolvedKey = SigComponentResolvedKeyScanner(tv.FuncPtrReturnTypeName);
            if (fpSpec->functionPointerParamList() != nullptr)
                for (auto* param : fpSpec->functionPointerParamList()->functionPointerParam())
                {
                    LLVMBackend::TypeAndValue::FuncPtrParam p;
                    bool pPtr = param->pointer() != nullptr;
                    int pStars = PointerDepthOf(param->pointer());
                    p.TypeName = ResolveSigComponentScanner(param->typeSpecifier(), pPtr);
                    p.Pointer  = pPtr;
                    p.IsMove   = param->Move() != nullptr;
                    p.PointerDepth = ReconcilePointerDepth(pPtr, pStars);
                    p.ResolvedTypeKey = SigComponentResolvedKeyScanner(p.TypeName);
                    tv.FuncPtrParams.push_back(p);
                }
        }
        return tv;
    }

void ForwardRefScanner::ScanUsingDeclaration(CFlatParser::UsingDeclarationContext* ctx) {
        auto* compiler = Compiler(ctx);

        std::string alias;
        if (ctx->String())
            alias = ctx->String()->getText();
        else if (ctx->Identifier() != nullptr)
            alias = ctx->Identifier()->getText();

        auto* typeSpec = ctx->typeSpecifier();

        // Function-type alias (using Cb = function<R(Args)>): store the resolved signature so the
        // alias name expands to a closure type wherever it is used (pre-registered here so a
        // forward reference in a function signature resolves during the scan pass).
        if (auto* fpSpec = typeSpec->functionPointerSpecifier())
        {
            auto aliasKeys = compiler->ScopedNameCandidates(alias);
            compiler->functionTypeAliases[aliasKeys.empty() ? alias : aliasKeys.front()] = BuildFuncPtrAliasType(fpSpec);
            return;
        }
        std::string target = typeSpec->getText();
        // A pointer alias (using Handle = void*) stores its trailing stars in the alias string;
        // they are peeled back onto the pointer flags at the resolution site (GetType /
        // ParseDeclarationSpecifiers). Storage stays string-shaped - no descriptor struct.
        std::string suffix(ctx->pointer() != nullptr ? ctx->pointer()->Star().size() : 0, '*');

        // Generic RHS (using IL = list<int>): mangle to list__int, pre-declare the shell +
        // default ctor to enqueue the instantiation (mirrors the use-site path at
        // ParseDeclarationSpecifiers), then alias to the mangled name - still a plain string.
        // The forward-ref pass is opportunistic: it never errors (templates may not be scanned
        // yet); ParseUsingDeclaration is authoritative and emits the diagnostics.
        std::string baseName;
        if (auto* genParams = GenericSpecOf(typeSpec, baseName))
        {
            std::string mangledName = compiler->ResolveGenericBaseAlias(baseName);
            for (auto* entry : genParams->typeParameterList()->typeParameterEntry())
            {
                // Closure and `unique`-qualified args encode via ResolveForwardTypeArg so the shell
                // name matches the authoritative ParseUsingDeclaration; others keep raw getText().
                if ((entry->typeSpecifier() && entry->typeSpecifier()->functionPointerSpecifier())
                    || TypeArgHasUnique(entry))
                    mangledName += "__" + MangleTypeArg(compiler, ResolveForwardTypeArg(entry));
                else
                    mangledName += "__" + MangleTypeArg(compiler, ResolveTypeArgSpelling(compiler, entry->getText()));
            }
            // A generic interface instantiation gets no struct shell / default ctor (see
            // tryPreDeclare); the alias still names the mangled interface.
            if (compiler->IsGenericInterfaceTemplateName(compiler->ResolveGenericBaseAlias(baseName)))
                compiler->gts.genericInterfaceInstances.insert(mangledName);
            else
            {
                compiler->CreateStructType(mangledName, {});
                LLVMBackend::TypeAndValue returnType{ .TypeName = mangledName };
                compiler->CreateFunctionDeclaration(mangledName, returnType, {});
            }
            compiler->RegisterTypeAlias(alias, mangledName + suffix);
            return;
        }

        // An alias of an alias names whatever the RHS already names (one hop).
        target = compiler->ResolveQualifiedName(target);
        target = compiler->ResolveGenericBaseAlias(compiler->ResolveTypeAlias(target));
        std::string targetBase = target;
        std::vector<uint64_t> targetArrayDims;
        PeelAliasArrayDims(targetBase, targetArrayDims);
        int targetPointerDepth = PeelAliasPointerStars(targetBase);
        std::string targetDecorated = targetBase + std::string(targetPointerDepth, '*');
        for (uint64_t dim : targetArrayDims)
            targetDecorated += "[" + std::to_string(dim) + "]";

        // Alias of a generic BASE (using IVector = Windows.Foundation.Collections.IVector;) - the
        // use site supplies the <...> args. IsGenericTemplateKey also sees a same-TU template, whose
        // main-pass maps are still empty here. Authoritative handling is in ParseUsingDeclaration.
        if (suffix.empty() && (compiler->IsWinrtGenericBase(targetBase)
                               || compiler->IsGenericTemplateKey(targetBase)
                               || compiler->gts.scannedGenericStructNamesUncertain.count(targetBase) != 0))
        {
            compiler->RegisterGenericBaseAlias(alias, targetBase);
            return;
        }

        if (compiler->IsInterfaceType(targetBase) || compiler->dataStructures.count(targetBase) > 0
            || LLVMBackend::IsPrimitiveTypeName(targetBase) || compiler->IsWinrtFullName(targetBase))
            compiler->RegisterTypeAlias(alias, targetDecorated + suffix);
    }

void ForwardRefScanner::ScanProgramDefinition(CFlatParser::ProgramDefinitionContext* ctx) {
        auto* compiler = Compiler(ctx);
        std::string name = ctx->directDeclarator()->getText();

        // Register opaque struct shell and default constructor
        compiler->CreateStructType(name, {});
        LLVMBackend::TypeAndValue returnType{ .TypeName = name };
        compiler->CreateFunctionDeclaration(name, returnType, {});

        // Pre-declare trampoline: int __program_run_Name(void*)
        {
            LLVMBackend::TypeAndValue intReturn{ .TypeName = "int" };
            LLVMBackend::DeclTypeAndValue ctxParam;
            ctxParam.TypeName = "void";
            ctxParam.VariableName = "ctx";
            ctxParam.Pointer = true;
            compiler->CreateFunctionDeclaration("__program_run_" + name, intReturn, { ctxParam }, false, false, false, false);
        }

        // Pre-declare run(Name* this, list__string args) -> bool
        {
            LLVMBackend::TypeAndValue boolReturn{ .TypeName = "bool" };
            LLVMBackend::DeclTypeAndValue thisParam;
            thisParam.TypeName = name;
            thisParam.VariableName = name + "__";
            thisParam.Pointer = true;
            LLVMBackend::DeclTypeAndValue argsParam;
            argsParam.TypeName = "list__string";
            argsParam.VariableName = "args";
            argsParam.IsMove = true;  // run() takes ownership; caller's list is zeroed after the call
            compiler->CreateFunctionDeclaration("run", boolReturn, { thisParam, argsParam });
        }

        // Pre-declare WaitForExit(Name* this) -> void
        {
            LLVMBackend::TypeAndValue voidReturn{ .TypeName = "void" };
            LLVMBackend::DeclTypeAndValue thisParam;
            thisParam.TypeName = name;
            thisParam.VariableName = name + "__";
            thisParam.Pointer = true;
            compiler->CreateFunctionDeclaration("WaitForExit", voidReturn, { thisParam });
        }

        // Pre-declare WaitForExit(Name* this, stop_token token) -> bool
        {
            LLVMBackend::TypeAndValue boolReturn{ .TypeName = "bool" };
            LLVMBackend::DeclTypeAndValue thisParam;
            thisParam.TypeName     = name;
            thisParam.VariableName = name + "__";
            thisParam.Pointer      = true;
            LLVMBackend::DeclTypeAndValue tokenParam;
            tokenParam.TypeName     = "stop_token";
            tokenParam.VariableName = "token";
            compiler->CreateFunctionDeclaration("WaitForExit", boolReturn, { thisParam, tokenParam });
        }

        // Pre-declare WaitForExit(Name* this, int timeoutMs) -> bool
        {
            LLVMBackend::TypeAndValue boolReturn{ .TypeName = "bool" };
            LLVMBackend::DeclTypeAndValue thisParam;
            thisParam.TypeName     = name;
            thisParam.VariableName = name + "__";
            thisParam.Pointer      = true;
            LLVMBackend::DeclTypeAndValue msParam;
            msParam.TypeName     = "int";
            msParam.VariableName = "timeoutMs";
            compiler->CreateFunctionDeclaration("WaitForExit", boolReturn, { thisParam, msParam });
        }

        // Pre-declare Kill(Name* this) -> void
        {
            LLVMBackend::TypeAndValue voidReturn{ .TypeName = "void" };
            LLVMBackend::DeclTypeAndValue thisParam;
            thisParam.TypeName     = name;
            thisParam.VariableName = name + "__";
            thisParam.Pointer      = true;
            compiler->CreateFunctionDeclaration("Kill", voidReturn, { thisParam });
        }

        // Pre-declare RequestStop(Name* this) -> void
        {
            LLVMBackend::TypeAndValue voidReturn{ .TypeName = "void" };
            LLVMBackend::DeclTypeAndValue thisParam;
            thisParam.TypeName     = name;
            thisParam.VariableName = name + "__";
            thisParam.Pointer      = true;
            compiler->CreateFunctionDeclaration("RequestStop", voidReturn, { thisParam });
        }

        // Pre-declare exitCode(Name* this) -> int (IProcess method)
        {
            LLVMBackend::TypeAndValue intReturn{ .TypeName = "int" };
            LLVMBackend::DeclTypeAndValue thisParam;
            thisParam.TypeName     = name;
            thisParam.VariableName = name + "__";
            thisParam.Pointer      = true;
            compiler->CreateFunctionDeclaration("exitCode", intReturn, { thisParam });
        }

        // Store declared interfaces in programTable for VTable dispatch
        {
            std::vector<std::string> ifaces;
            for (auto* id : ctx->Identifier())
                ifaces.push_back(id->getText());
            compiler->programTable[name].Interfaces = ifaces;
        }

        // Pre-declare member functions (including user's main) and destructor
        for (auto func : ctx->functionDefinition())
        {
            // 'program' has no user-constructor concept - its instance is built by the
            // runtime entry trampoline, not a hand-written ctor. Reject it cleanly here.
            // A constructor is ONLY a function with no declarationSpecifiers - an ordinary
            // method that happens to share the program's name is NOT one.
            if (func->declarationSpecifiers() == nullptr && getFunctionName(func) == name)
                Compiler(func)->LogError(std::format(
                    "program '{}' does not support a user-defined constructor", name));
            else
                ScanFunctionDefinition(func, name);
        }
        for (auto dtor : ctx->destructorDefinition())
        {
            LLVMBackend::TypeAndValue voidReturn{ .TypeName = "void" };
            LLVMBackend::DeclTypeAndValue thisParam;
            thisParam.TypeName = name;
            thisParam.VariableName = name + "__";
            thisParam.Pointer = true;
            compiler->CreateFunctionDeclaration("~" + name, voidReturn, { thisParam });
        }
    }

void ForwardRefScanner::ScanImportedProgramDefinition(const std::string& name) {
        auto* compiler = compilerLLVM;

        compiler->CreateStructType(name, {});
        LLVMBackend::TypeAndValue returnType{ .TypeName = name };
        compiler->CreateFunctionDeclaration(name, returnType, {});

        // Pre-declare trampoline: int __program_run_Name(void*)
        {
            LLVMBackend::TypeAndValue intReturn{ .TypeName = "int" };
            LLVMBackend::DeclTypeAndValue ctxParam;
            ctxParam.TypeName = "void";
            ctxParam.VariableName = "ctx";
            ctxParam.Pointer = true;
            compiler->CreateFunctionDeclaration("__program_run_" + name, intReturn, { ctxParam }, false, false, false, false);
        }

        // Pre-declare run(Name* this, list__string args) -> bool
        {
            LLVMBackend::TypeAndValue boolReturn{ .TypeName = "bool" };
            LLVMBackend::DeclTypeAndValue thisParam;
            thisParam.TypeName = name;
            thisParam.VariableName = name + "__";
            thisParam.Pointer = true;
            LLVMBackend::DeclTypeAndValue argsParam;
            argsParam.TypeName = "list__string";
            argsParam.VariableName = "args";
            argsParam.IsMove = true;
            compiler->CreateFunctionDeclaration("run", boolReturn, { thisParam, argsParam });
        }

        // Pre-declare WaitForExit(Name* this) -> void
        {
            LLVMBackend::TypeAndValue voidReturn{ .TypeName = "void" };
            LLVMBackend::DeclTypeAndValue thisParam;
            thisParam.TypeName = name;
            thisParam.VariableName = name + "__";
            thisParam.Pointer = true;
            compiler->CreateFunctionDeclaration("WaitForExit", voidReturn, { thisParam });
        }

        // Pre-declare WaitForExit(Name* this, stop_token token) -> bool
        {
            LLVMBackend::TypeAndValue boolReturn{ .TypeName = "bool" };
            LLVMBackend::DeclTypeAndValue thisParam;
            thisParam.TypeName     = name;
            thisParam.VariableName = name + "__";
            thisParam.Pointer      = true;
            LLVMBackend::DeclTypeAndValue tokenParam;
            tokenParam.TypeName     = "stop_token";
            tokenParam.VariableName = "token";
            compiler->CreateFunctionDeclaration("WaitForExit", boolReturn, { thisParam, tokenParam });
        }

        // Pre-declare WaitForExit(Name* this, int timeoutMs) -> bool
        {
            LLVMBackend::TypeAndValue boolReturn{ .TypeName = "bool" };
            LLVMBackend::DeclTypeAndValue thisParam;
            thisParam.TypeName     = name;
            thisParam.VariableName = name + "__";
            thisParam.Pointer      = true;
            LLVMBackend::DeclTypeAndValue msParam;
            msParam.TypeName     = "int";
            msParam.VariableName = "timeoutMs";
            compiler->CreateFunctionDeclaration("WaitForExit", boolReturn, { thisParam, msParam });
        }

        // Pre-declare Kill(Name* this) -> void
        {
            LLVMBackend::TypeAndValue voidReturn{ .TypeName = "void" };
            LLVMBackend::DeclTypeAndValue thisParam;
            thisParam.TypeName     = name;
            thisParam.VariableName = name + "__";
            thisParam.Pointer      = true;
            compiler->CreateFunctionDeclaration("Kill", voidReturn, { thisParam });
        }

        // Pre-declare RequestStop(Name* this) -> void
        {
            LLVMBackend::TypeAndValue voidReturn{ .TypeName = "void" };
            LLVMBackend::DeclTypeAndValue thisParam;
            thisParam.TypeName     = name;
            thisParam.VariableName = name + "__";
            thisParam.Pointer      = true;
            compiler->CreateFunctionDeclaration("RequestStop", voidReturn, { thisParam });
        }
        // Pre-declare exitCode(Name* this) -> int (IProcess method)
        {
            LLVMBackend::TypeAndValue intReturn{ .TypeName = "int" };
            LLVMBackend::DeclTypeAndValue thisParam;
            thisParam.TypeName     = name;
            thisParam.VariableName = name + "__";
            thisParam.Pointer      = true;
            compiler->CreateFunctionDeclaration("exitCode", intReturn, { thisParam });
        }

        // All imported programs implicitly implement IProcess.
        compiler->programTable[name].Interfaces = { "IProcess" };
    }

void ForwardRefScanner::ScanAnnotationDefinition(CFlatParser::AnnotationDefinitionContext* ctx) {
        auto* compiler = Compiler(ctx);
        std::string name = ctx->Identifier()->getText();

        std::vector<std::string> fields;
        for (auto* decl : ctx->declaration())
        {
            if (auto* initList = decl->initDeclaratorList())
                for (auto* initDecl : initList->initDeclarator())
                    if (auto* dir = initDecl->declarator())
                        fields.push_back(getDirectDeclName(dir->directDeclarator()));
        }
        compiler->annotationRegistry[name] = fields;
    }

void ForwardRefScanner::ScanExternalDeclaration(CFlatParser::ExternalDeclarationContext* ctx, const std::string& namespaceName) {
        if (auto annDef = ctx->annotationDefinition())
            ScanAnnotationDefinition(annDef);
        else if (auto ns = ctx->namespaceDefinition())
            ScanNamespace(ns, namespaceName);
        else if (auto func = ctx->functionDefinition())
            ScanFunctionDefinition(func, {}, namespaceName);
        else if (auto dataStruct = ctx->structDefinition())
            ScanStructDefinition(dataStruct, namespaceName);
        else if (auto classDef = ctx->classDefinition())
            ScanClassDefinition(classDef, namespaceName);
        else if (auto iface = ctx->interfaceDefinition())
            ScanInterfaceDefinition(iface, namespaceName);
        else if (auto usingDecl = ctx->usingDeclaration())
            ScanUsingDeclaration(usingDecl);
        else if (auto progDef = ctx->programDefinition())
            ScanProgramDefinition(progDef);
        else if (auto imp = ctx->importDeclaration())
        {
            if (imp->children.size() >= 2 && imp->children[1]->getText() == "program")
            {
                std::string alias = imp->Identifier()->getText();
                ScanImportedProgramDefinition(alias);
            }
        }
        else if (auto* lfg = ctx->lockFieldGroup())
            ScanGlobalLockGroup(lfg, namespaceName);
        else if (auto expectErrDecl = ctx->expectErrorDeclaration())
        {
            // Bare-semicolon file-scope form (no inner declarations) is armed before
            // ProcessImports in LLVMBackend::Compile; leave that standing expectation
            // untouched here. Only the scoped-block form has declarations to scan.
            if (expectErrDecl->externalDeclaration().empty())
                return;
            // Set expectedError so errors during the scan are caught rather than aborting.
            std::string rawText = expectErrDecl->StringLiteral()->getText();
            compilerLLVM->expectedError = DequoteStringLiteral(rawText);
            try
            {
                for (auto* nested : expectErrDecl->externalDeclaration())
                    ScanExternalDeclaration(nested, namespaceName);
            }
            catch (const ExpectedErrorReceived&)
            {
            }
            compilerLLVM->expectedError.clear();
            return;
        }
        else if (auto* ifConst = ctx->ifConstDeclaration())
            MarkIfConstClassImplsUncertain(compilerLLVM, ifConst, namespaceName, {}, namespaceName);
        // if const declarations are otherwise skipped here; they are handled in MainListener
        // which has access to expression evaluation and can determine the taken branch
    }

void ForwardRefScanner::ScanNamespace(CFlatParser::NamespaceDefinitionContext* ctx, const std::string& parentNamespace) {
        std::string namespaceName;
        for (auto* id : ctx->Identifier())
            namespaceName += (namespaceName.empty() ? "" : ".") + id->getText();
        if (!parentNamespace.empty())
            namespaceName = parentNamespace + "." + namespaceName;

        // Member signatures may reference sibling types unqualified (a struct param
        // declared earlier in the namespace) - make the namespace visible to GetType.
        auto* compiler = Compiler(ctx);
        LLVMBackend::NamespaceScope nsScope(compiler, namespaceName);
        for (auto* extDecl : ctx->externalDeclaration())
            ScanExternalDeclaration(extDecl, namespaceName);
    }

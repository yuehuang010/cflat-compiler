#include "MainListener.h"

namespace
{
bool ShouldWarnImplicitFieldNarrowing(llvm::Value* value, llvm::Type* destinationType,
                                      const std::string& destinationTypeName)
{
    auto* constant = llvm::dyn_cast_or_null<llvm::ConstantInt>(value);
    if (constant == nullptr || destinationType == nullptr || !destinationType->isIntegerTy())
        return true;

    unsigned bits = destinationType->getIntegerBitWidth();
    std::string typeName = destinationTypeName;
    bool isUnsigned = typeName == "u8" || typeName == "u16"
        || typeName == "u32" || typeName == "u64";
    const llvm::APInt& integer = constant->getValue();
    return isUnsigned ? integer.isNegative() || integer.getActiveBits() > bits
                      : !integer.isSignedIntN(bits);
}
}

void MainListener::ParseStructDefinition(CFlatParser::StructDefinitionContext* ctx, const std::string& nameOverride, const std::string& namespaceName) {
        ResolvedMembersScope memberScope_(resolvedMembers_, (const void*)ctx);
        auto* compiler = Compiler(ctx);
        auto decl = ctx->directDeclarator();
        std::string baseName = decl->getText();
        std::string structName;

        // Apply nameOverride first (for generic instantiations), then namespace
        if (!nameOverride.empty())
        {
            structName = nameOverride;
        }
        else if (!namespaceName.empty())
        {
            structName = namespaceName + "." + baseName;
        }
        else
        {
            structName = baseName;
        }

        // If this is a generic template definition (not an instantiation), store it and return.
        if (nameOverride.empty() && ctx->genericTypeParameters() != nullptr)
        {
            if (Compiler()->gts.scannedGenericInterfaceNames.count(structName) != 0
                || genericInterfaceTemplates.count(structName) != 0)
                LogErrorContext(ctx, std::format(
                    "generic struct '{}' conflicts with a generic interface of the same name",
                    structName));
            auto typeParams = ParseGenericTypeParameters(ctx->genericTypeParameters());
            genericStructTemplates[structName] = ctx;
            Compiler()->gts.genericTemplateNamespace[structName] = Compiler()->GetCurrentNamespace();
            Compiler()->RevokeGenericInterfaceInstances(structName);
            genericStructTypeParams[structName] = typeParams;
            genericStructConstraints[structName] = ParseWhereClause(ctx->whereClause());
            // Record which param (if any) is variadic - always the last one
            {
                auto entries = ctx->genericTypeParameters()->typeParameterList()->typeParameterEntry();
                bool hasPack = !entries.empty() && entries.back()->Ellipsis() != nullptr;
                genericStructPackIndex[structName] = hasPack ? (typeParams.size() - 1) : std::string::npos;
            }
            return;
        }

        // Re-emission guard: if this struct was already fully emitted via a transitive import,
        // CreateFunctionDefinition's duplicate-skip leaves the builder out of scope - skip the walk.
        {
            auto sd = compiler->GetDataStructure(structName);
            if (sd.StructType != nullptr && !sd.StructType->isOpaque())
            {
                if (auto* existing = compiler->GetFunction(structName);
                    existing != nullptr && !existing->empty())
                {
                    if (compiler->IsVerbose())
                        std::cout << "[verbose]     skipping duplicate struct definition: " << structName << "\n";
                    return;
                }
            }
        }

        // Validate type-level annotations against the registry and record them for
        // annotationof(Type,"Ann"). [Capability(...)] is consumed once the members exist.
        auto structAnnotations = ParseAnnotationList(ctx->annotationList());
        compiler->SetTypeAnnotations(structName, structAnnotations);

        if (compiler->IsVerbose())
            std::cout << "[verbose]     parse decl list: " << structName << "\n";

        // Process nested struct/class definitions before fields so their types are available
        for (auto* nestedStruct : MemberStructDefinitions(ctx))
            ParseStructDefinition(nestedStruct, {}, structName);
        for (auto* nestedClass : MemberClassDefinitions(ctx))
            ParseClassDefinition(nestedClass, {}, structName);

        // Push scope so unqualified nested type names resolve (e.g. Inner -> Outer.Inner)
        structScopeStack.push_back(structName);

        // Arm the union-member rejection for `unique` (ValidateUniqueField). Placed after the
        // nested definitions above so an inner body is never judged by the enclosing body's kind.
        UnionFieldDeclGuard unionCtx(inUnionFieldDecl_, ctx->Union() != nullptr);

        auto declarationList = MemberDeclarations(ctx);
        std::vector<llvm::Type*> types;

        // Queue and instantiate generic types used in field declarations before
        // ParseDeclarationList resolves them to LLVM types. Only needed at top-level
        // (non-template) scope; template instantiations already have activeTypeSubstitutions
        // or activePackSubstitutions set and their generics are queued via ParseDeclarationSpecifiers.
        if (activeTypeSubstitutions.empty() && activePackSubstitutions.empty())
        {
            for (auto decl : declarationList)
                ScanAndQueueGenericTypeUses(decl);
            ProcessPendingInstantiations();
        }

        // Build field list, expanding pack fields (T... fieldName -> fieldName_0, fieldName_1, ...)
        std::vector<LLVMBackend::DeclTypeAndValue> declList;
        auto rejectFixedArrayMemberPrototype = [&](CFlatParser::DeclarationContext* decl) {
            auto* specs = decl->declarationSpecifiers();
            if (specs == nullptr || decl->initDeclaratorList() == nullptr) return;
            std::string element;
            for (auto* spec : specs->declarationSpecifier())
            {
                if (auto* dims = ArrayDimsOf(spec); dims != nullptr && !dims->assignmentExpression().empty())
                {
                    if (spec->typeSpecifier() != nullptr) element = spec->typeSpecifier()->getText();
                    break;
                }
            }
            if (element.empty()) return;
            for (auto* init : decl->initDeclaratorList()->initDeclarator())
            {
                auto* declarator = init->declarator();
                if (declarator == nullptr
                    || (declarator->parameterTypeList() == nullptr && declarator->children.size() <= 1)) continue;
                LogErrorContext(decl, std::format(
                    "member '{}' cannot return the fixed array '{}[N]' by value; return a struct with the array as a field or take an out-parameter",
                    declarator->directDeclarator()->getText(), element));
            }
        };
        for (auto* decl : declarationList)
        {
            rejectFixedArrayMemberPrototype(decl);
            std::string packParamName;
            if (decl->declarationSpecifiers())
            {
                for (auto* ds : decl->declarationSpecifiers()->declarationSpecifier())
                {
                    auto* ts = ds->typeSpecifier();
                    if (!ts || !ts->genericIdentifier() || ts->genericIdentifier()->genericTypeParameters()) continue;
                    auto* gid = ts->genericIdentifier();
                    if (!gid->Identifier()) continue;
                    std::string n = gid->Identifier()->getText();
                    if (activePackSubstitutions.count(n)) { packParamName = n; break; }
                }
            }

            if (packParamName.empty())
            {
                for (auto& f : ParseDeclarationList({decl}))
                    declList.push_back(f);
                continue;
            }

            std::string baseFieldName;
            if (auto* idl = decl->initDeclaratorList())
                if (!idl->initDeclarator().empty())
                    if (auto* d = idl->initDeclarator()[0]->declarator())
                        if (auto* dd = d->directDeclarator())
                            baseFieldName = getDirectDeclName(dd);

            auto& packTypes = activePackSubstitutions.at(packParamName);
            auto savedPackItemSubst = activeTypeSubstitutions;
            for (size_t i = 0; i < packTypes.size(); i++)
            {
                activeTypeSubstitutions[packParamName] = packTypes[i];
                auto expanded = ParseDeclarationList({decl});
                for (auto& f : expanded)
                {
                    f.VariableName = baseFieldName + "_" + std::to_string(i);
                    declList.push_back(f);
                }
            }
            activeTypeSubstitutions = savedPackItemSubst;
        }

        // Process lock field groups: each group annotates its fields with GuardedBy,
        // and registers member functions with the group's lock as a RequiredLock.
        for (auto* lfg : MemberLockFieldGroups(ctx))
        {
            // Also queue generic types used inside the group.
            if (activeTypeSubstitutions.empty() && activePackSubstitutions.empty())
                ScanAndQueueGenericTypeUses(lfg);

            // Extract the guardian name from the single lock arg expression.
            auto groupArgs = lfg->lockClause()->lockArgList()->expression();
            if (groupArgs.empty()) continue;
            std::string guardianName = GetLockArgCanonical(groupArgs[0]);

            for (auto* decl : lfg->declaration())
            {
                for (auto& f : ParseDeclarationList({decl}))
                {
                    f.GuardedBy = guardianName;
                    declList.push_back(f);
                }
            }
        }

        if (compiler->IsVerbose())
            std::cout << "[verbose]     decl list has " << declList.size() << " fields\n";

        // Reject two fields with the same name in one struct/union (C semantics). A duplicate would
        // overwrite the first field's entry in name->index lookups and silently shadow it. Anonymous
        // (C-interop) members carry an empty name and are skipped.
        {
            std::unordered_set<std::string> seenFields;
            for (const auto& f : declList)
                if (!f.VariableName.empty() && !seenFields.insert(f.VariableName).second)
                    LogErrorContext(ctx, std::format(
                        "redeclaration of field '{}' in '{}'", f.VariableName, structName));
        }

        // Bitfield packing: collapse runs of same-type bitfields into shared
        // storage slots BEFORE the constructor loop runs - the default ctor
        // emits one initializer per declList entry and the LLVM struct body
        // must match. The side-table goes into StructData.Bitfields.
        std::vector<LLVMBackend::BitfieldInfo> packedBitfields;
        bool anyBitfields = false;
        for (const auto& tv : declList) { if (tv.IsBitfield) { anyBitfields = true; break; } }
        if (anyBitfields)
            declList = compiler->PackBitfields(declList, packedBitfields);

        // Build the struct body before opening the constructor function so that
        // GetFunctionType can resolve the (sized) return type.  Initializer
        // expressions are evaluated later inside the constructor body.
        bool isUnion = (ctx->Union() != nullptr);

        if (compiler->IsVerbose())
            std::cout << "[verbose]     create struct type: " << structName << "\n";
        // Capture `struct alignas(N) S { ... }` BEFORE layout so the padding
        // member can be appended atomically.
        uint64_t userAlign = 0;
        if (auto* alignSpec = ctx->alignmentSpecifier())
            userAlign = ParseAlignmentSpecifier(alignSpec);
        llvm::StructType* structType;
        if (isUnion)
        {
            // A union body is one array: an over-aligned member cannot get a pad slot, it
            // just raises the union's alignment (all members start at offset 0).
            structType = compiler->CreateUnionType(structName, declList, userAlign);
        }
        else
        {
            // `alignas(N)` on a MEMBER: insert synthetic `__padN` slots so the member starts on
            // its boundary, and raise the struct's own alignment to the strictest member (which
            // also tail-pads sizeof). Runs after PackBitfields so indices stay in sync.
            uint64_t fieldAlign = 0;
            declList = compiler->PadFieldsForAlignment(declList, fieldAlign,
                anyBitfields ? &packedBitfields : nullptr);
            if (fieldAlign > userAlign) userAlign = fieldAlign;
            structType = compiler->CreateStructType(structName, declList, userAlign,
                anyBitfields ? &packedBitfields : nullptr);
            // A struct with zero fields still needs a sized (non-opaque) type
            // so that alloca/sizeof work correctly (e.g. when passed via interface).
            if (structType->isOpaque())
                structType->setBody(llvm::ArrayRef<llvm::Type*>());
        }
        if (compiler->IsVerbose())
            std::cout << "[verbose]     create default ctor: " << structName << "\n";
        LLVMBackend::TypeAndValue returnType{
            .TypeName = structName,
        };
        // Member functions of this struct. Pre-declare their signatures (for
        // instantiations) BEFORE the dependency flush below, so a sibling
        // instantiation pulled in by the flush can resolve calls back into this
        // type's methods to a forward declaration. See PreDeclareInstantiationMembers.
        auto functionList = MemberFunctionDefinitions(ctx);
        if (!nameOverride.empty())
            PreDeclareInstantiationMembers(compiler, functionList, baseName, structName, returnType);
        // Flush any nested generic instantiations queued while parsing field declarations,
        // so their constructors exist before this struct's default constructor calls them.
        {
            auto savedSubst = activeTypeSubstitutions;
            ProcessPendingInstantiations();
            activeTypeSubstitutions = savedSubst;
        }
        // If the user wrote an explicit no-arg constructor, skip the auto-generated one.
        // ParseConstructorDefinition will handle it later in the member function loop.
        bool hasBareNoArgCtor = [&]() {
            for (auto* f : MemberFunctionDefinitions(ctx))
                if (!FunctionDeclaresReturnType(f) && getFunctionName(f) == baseName && !f->parameterTypeList())
                    return true;
            return false;
        }();
        // An all-defaulted ctor is ALSO a no-arg ctor: its cutoff-0 wrapper claims the same
        // symbol, so emitting the synthetic one too collides (see AllParametersDefaulted).
        bool hasAllDefaultedCtor = !hasBareNoArgCtor && [&]() {
            for (auto* f : MemberFunctionDefinitions(ctx))
                if (!FunctionDeclaresReturnType(f) && getFunctionName(f) == baseName && AllParametersDefaulted(f->parameterTypeList()))
                    return true;
            return false;
        }();
        bool hasExplicitNoArgCtor = !isUnion && (hasBareNoArgCtor || hasAllDefaultedCtor);

        // Create default constructor (skipped when user provides an explicit no-arg ctor)
        if (!hasExplicitNoArgCtor)
        {
            auto funcDef = compiler->CreateFunctionDefinition(structName, returnType, {});

            if (isUnion)
            {
                EmitUnionDefaultConstructorBody(ctx, structName, structType, declList);
            }
            else
            {
                std::vector<llvm::Value*> initializers;
                for (auto& typeValue : declList)
                {
                    auto initializer = typeValue.Initializer;
                    llvm::Value* rvalue = nullptr;
                    if (auto* braceList = FieldDefaultBraceList(typeValue))
                    {
                        // Emitting a real function body; clear the stale file-scope global_scope
                        // so the brace list's stores and calls lower as ordinary instructions.
                        GlobalScopeGuard defaultCtorScope(global_scope);
                        rvalue = ParseFieldDefaultBraceInitializer(structName, typeValue, braceList);
                    }
                    else if (initializer != nullptr)
                    {
                        auto assignmentExpression = initializer->assignmentExpression();
                        if (assignmentExpression != nullptr)
                        {
                            rvalue = ParseFieldDefaultInitializer(
                                structName, typeValue, assignmentExpression);
                            if (typeValue.TypeName == "auto")
                            {
                                typeValue.TypeName = rvalue->getType()->getStructName();
                                // Re-finalise the struct body now that the auto field type is known.
                                structType = compiler->CreateStructType(structName, declList);
                            }
                        }
                        else if (initializer->Default() != nullptr)
                        {
                            // We are emitting the synthetic default-constructor body (a real
                            // function); global_scope is still true only because the file-scope
                            // struct walk has not finished. Clear it so a struct-typed field's
                            // `= default` recurses into that field type's own default constructor
                            // (running its field initializers) instead of zero-filling.
                            GlobalScopeGuard defaultCtorScope(global_scope);
                            rvalue = GenerateDefaultValue(typeValue);
                        }
                    }
                    if (rvalue == nullptr && compiler->GetType(typeValue)->isArrayTy())
                    {
                        GlobalScopeGuard defaultCtorScope(global_scope);
                        rvalue = GenerateDefaultValue(typeValue);
                    }
                    initializers.push_back(rvalue);
                }

                // Seed with zero (not undef) so fields lacking an explicit initializer read as
                // 0/null after `= default` / `= {}` instead of leaking stack garbage. Fields that
                // do have an initializer are overwritten by CreateInsertValue below, so a
                // fully-initialized struct optimizes to the same IR as the old undef seed.
                llvm::Value* structVal = llvm::Constant::getNullValue(structType);

                LLVMBackend::TypeAndValue myStruct;
                myStruct.TypeName = structName;
                myStruct.VariableName = "_" + structName;

                unsigned int structIndex = 0;

                for (auto rvalue : initializers)
                {
                    auto* destType = structType->getTypeAtIndex(structIndex);
                    // No explicit initializer on a struct-typed field - call its default ctor.
                    if (rvalue == nullptr && (destType->isStructTy() || destType->isArrayTy()))
                    {
                        std::string fieldTypeName = declList[structIndex].TypeName;
                        // forceRoot: the GetFunction guard is an exact-key lookup, so a namespace walk
                        // here would call a same-named sibling type's ctor (layer 3).
                        if (destType->isArrayTy())
                            rvalue = GenerateDefaultValue(declList[structIndex]);
                        else if (compiler->GetFunction(fieldTypeName))
                            rvalue = compiler->CreateOverloadedFunctionCall(fieldTypeName, {}, true);
                        else
                            rvalue = llvm::Constant::getNullValue(destType);
                    }
                    // Synthesized bitfield storage slot: always zero so packed
                    // bitfields read as 0 after `S s = default;`.
                    if (rvalue == nullptr && declList[structIndex].IsBitfieldStorage)
                    {
                        rvalue = llvm::Constant::getNullValue(destType);
                    }
                    if (rvalue != nullptr)
                    {
                        rvalue = compiler->Upconvert(rvalue, destType);
                        if (rvalue->getType() != destType)
                        {
                            if (destType->isStructTy())
                            {
                                // Initializer type doesn't match struct field type (e.g. integer 0 used for
                                // a struct-typed generic field).  Call the field's default constructor when
                                // one is available; otherwise zero-initialize the aggregate.
                                std::string fieldTypeName = declList[structIndex].TypeName;
                                // forceRoot: the GetFunction guard is an exact-key lookup, so a namespace walk
                                // here would call a same-named sibling type's ctor (layer 3).
                                if (compiler->GetFunction(fieldTypeName))
                                    rvalue = compiler->CreateOverloadedFunctionCall(fieldTypeName, {}, true);
                                else
                                    rvalue = llvm::Constant::getNullValue(destType);
                            }
                            else
                            {
                                // Narrowing field initializer (e.g. u8 r = 255 has i32 literal).
                                if (ShouldWarnImplicitFieldNarrowing(
                                        rvalue, destType, declList[structIndex].TypeName))
                                    compiler->LogWarning(std::format(
                                        "implicit narrowing to '{}' in field '{}' - use an explicit cast",
                                        declList[structIndex].TypeName,
                                        declList[structIndex].VariableName));
                                rvalue = compiler->CreateCast(rvalue, destType);
                            }
                        }
                        structVal = compiler->CreateInsertValue(structVal, rvalue, structIndex);
                    }

                    structIndex++;
                }

                // close constructor.
                compiler->CreateReturnCall(structVal);
            }

            // Pop the stack
            compiler->CreateBlockBreak(nullptr, true);
        } // end if (!hasExplicitNoArgCtor)

        // Register struct fields in LSP index for dot-completion
        if (auto* s = compiler->GetSymbolSink())
        {
            auto sd = compiler->GetDataStructure(structName);
            for (const auto& field : sd.StructFields)
            {
                if (field.VariableName.empty() || field.IsPadding) continue;
                std::string annSig;
                for (const auto& ann : field.Annotations)
                {
                    annSig += "[" + ann.Name;
                    if (!ann.Value.empty()) annSig += "(" + ann.Value + ")";
                    annSig += "] ";
                }
                std::string typeSig = field.TypeName;
                if (field.Pointer) typeSig += "*";
                if (field.ElemPointer) typeSig += "*";
                s->Register(SymbolKind::Field, structName + "." + field.VariableName,
                            compiler->GetSourceFilePath(),
                            (int)ctx->getStart()->getLine(),
                            (int)ctx->getStart()->getCharPositionInLine(),
                            annSig + typeSig + " " + structName + "." + field.VariableName);
            }
        }

        // Member function signatures were pre-declared above (before the flush).

        // Pre-register destructor so 'delete' inside static methods can call it, AND so a member
        // that constructs an instance of its own type (e.g. dictionary.copy() building a local
        // dictionary) forces .dtorfull with the user destructor already resolved. The scanner only
        // forward-declares the TEMPLATE's `~name`; a concrete instantiation's `~name__T` is not, so
        // declare it here when missing - otherwise .dtorfull bakes a null user-dtor and caches it,
        // leaking everything the hand-written destructor would have freed.
        if (!MemberDestructorDefinitions(ctx).empty())
        {
            llvm::Function* dtorFn = compiler->GetFunction("~" + structName);
            if (dtorFn == nullptr)
            {
                LLVMBackend::DeclTypeAndValue thisParam;
                thisParam.TypeName = structName;
                thisParam.VariableName = structName + "__";
                thisParam.Pointer = true;
                LLVMBackend::TypeAndValue voidReturn{ .TypeName = "void" };
                compiler->CreateFunctionDeclaration("~" + structName, voidReturn, { thisParam });
                dtorFn = compiler->GetFunction("~" + structName);
            }
            if (dtorFn != nullptr)
                compiler->RegisterDestructor(structName, dtorFn);
        }

        {
            GlobalScopeGuard scopeGuard(global_scope);
            for (auto func : functionList)
            {
                global_scope = false;
                std::string funcName = getFunctionName(func);
                if (compiler->IsVerbose())
                    std::cout << "[verbose]     parse member: " << structName << "." << funcName << "\n";
                // Constructor - same name as struct (no-arg or with parameters)
                if (!FunctionDeclaresReturnType(func) && funcName == baseName)
                {
                    // This ctor IS the type's no-arg ctor when every parameter is defaulted and
                    // no bare 'T()' was written - it must seed fields itself, not self-delegate.
                    bool suppliesNoArgCtor = !isUnion && !hasBareNoArgCtor
                        && AllParametersDefaulted(func->parameterTypeList());
                    ParseConstructorDefinition(func, structName, suppliesNoArgCtor);
                    continue;
                }
                // A generic member method - static or instance - is stored as a template keyed by
                // its owner ("Owner.method"). InstantiateGenericFunction re-derives the owner from
                // that key and emits an instance method with its implicit `this` parameter, so the
                // monomorphized body resolves the owner's fields like any other member.
                if (func->genericTypeParameters() != nullptr)
                {
                    std::string qualifiedName = structName + "." + funcName;
                    genericFunctionTemplates[qualifiedName] = func;
                    // Declaring NAMESPACE of the owner, recorded not derived: the key's last dot
                    // separates the owner, not a namespace, so only this tells the two apart.
                    Compiler()->gts.genericTemplateNamespace[qualifiedName] = Compiler()->GetCurrentNamespace();
                    genericFunctionTypeParams[qualifiedName] = ParseGenericTypeParameters(func->genericTypeParameters());
                    genericFunctionConstraints[qualifiedName] = ParseWhereClause(func->whereClause());
                }
                else if (funcName == "operator new" || funcName == "operator delete" || isFunctionStatic(func))
                {
                    ParseFunctionDefinition(func, {}, {}, structName + "." + funcName);
                }
                else
                    ParseFunctionDefinition(func, structName);
            }
        }

        // Parse functions declared inside positional lock groups.
        {
            GlobalScopeGuard scopeGuard(global_scope);
            for (auto* lfg : MemberLockFieldGroups(ctx))
            {
                for (auto* func : lfg->functionDefinition())
                {
                    global_scope = false;
                    // Same rejection as the scanner - a constructor is ONLY a function
                    // with no declarationSpecifiers (a same-named method is NOT one).
                    if (func->declarationSpecifiers() == nullptr && getFunctionName(func) == baseName)
                        Compiler(func)->LogError(std::format(
                            "constructor '{}' is not allowed inside a lock field group", baseName));
                    else
                        ParseFunctionDefinition(func, structName);
                }
            }
        }

        // Parse destructor
        {
            GlobalScopeGuard scopeGuard(global_scope);
            for (auto dtor : MemberDestructorDefinitions(ctx))
            {
                global_scope = false;
                ParseDestructorDefinition(dtor, structName);
            }
        }

        // [Capability(I, ...)]: static conformance. The interfaces go on the STATIC list - never
        // the nominal one - so no vtable is built and the type can never become a fat pointer.
        // Runs after the member functions so VerifyInterfaceImplementation can see them.
        {
            std::vector<std::string> capIfaces;
            for (const auto& ann : structAnnotations)
            {
                if (ann.Name != "Capability") continue;
                for (const auto& ifaceName : ann.Values)
                {
                    if (!compiler->HasInterface(ifaceName))
                    {
                        LogErrorContext(ctx, std::format(
                            "[Capability] on '{}': unknown interface '{}'", structName, ifaceName));
                        continue;
                    }
                    capIfaces.push_back(compiler->ResolveInterfaceName(ifaceName));
                }
            }
            if (!capIfaces.empty())
            {
                compiler->RegisterStructStaticInterfaces(structName, capIfaces);
                for (const auto& ifaceName : capIfaces)
                    compiler->VerifyInterfaceImplementation(structName, ifaceName);
            }
        }

        // Process any generic instantiations that were queued during this struct definition
        // ProcessPendingInstantiations();

        structScopeStack.pop_back();
    }

void MainListener::EmitUnionDefaultConstructorBody(
        antlr4::ParserRuleContext* ctx,
        const std::string& structName,
        llvm::StructType* structType,
        std::vector<LLVMBackend::DeclTypeAndValue>& declList) {
        auto* compiler = Compiler(ctx);

        // All members alias at offset 0, so only ONE default can be applied. Prefer the first
        // field carrying an EXPLICIT initializer; a bare `= default` is the repo-wide convention
        // and only contributes that field type's own default value.
        size_t chosen = declList.size();
        bool sawExplicit = false;
        for (size_t i = 0; i < declList.size(); i++)
        {
            auto& tv = declList[i];
            if (tv.TypeName == "auto" || tv.IsBitfieldStorage) continue;
            bool explicitInit = FieldDefaultBraceList(tv) != nullptr
                || (tv.Initializer != nullptr && tv.Initializer->assignmentExpression() != nullptr);
            if (!explicitInit) continue;
            if (sawExplicit)
            {
                // Point the caret at the OFFENDING field's own initializer, not at the `union`
                // keyword. LogErrorContext throws, so nothing after this runs.
                antlr4::ParserRuleContext* at = tv.Initializer != nullptr
                    ? static_cast<antlr4::ParserRuleContext*>(tv.Initializer)
                    : static_cast<antlr4::ParserRuleContext*>(FieldDefaultBraceList(tv));
                LogErrorContext(at != nullptr ? at : ctx, std::format(
                    "union '{}' gives field '{}' a default initializer, but field '{}' already has "
                    "one - members of a union share storage, so at most one may be initialized. "
                    "Write '= default' on all but one.",
                    structName, tv.VariableName, declList[chosen].VariableName));
            }
            sawExplicit = true;
            chosen = i;
        }
        if (!sawExplicit)
        {
            for (size_t i = 0; i < declList.size(); i++)
            {
                auto& tv = declList[i];
                if (tv.TypeName == "auto" || tv.IsBitfieldStorage) continue;
                if (tv.Initializer != nullptr && tv.Initializer->Default() != nullptr) { chosen = i; break; }
            }
        }

        llvm::Value* rvalue = nullptr;
        llvm::Type* fieldType = nullptr;
        if (chosen < declList.size())
        {
            auto& tv = declList[chosen];
            // Emitting a real function body; clear the stale file-scope global_scope so the
            // initializer's stores and calls lower as ordinary instructions.
            GlobalScopeGuard defaultCtorScope(global_scope);
            if (auto* braceList = FieldDefaultBraceList(tv))
                rvalue = ParseFieldDefaultBraceInitializer(structName, tv, braceList);
            else if (tv.Initializer != nullptr && tv.Initializer->assignmentExpression() != nullptr)
                rvalue = ParseFieldDefaultInitializer(structName, tv, tv.Initializer->assignmentExpression());
            else
                rvalue = GenerateDefaultValue(tv);
            fieldType = compiler->GetType(tv);
        }

        // A zero value covers every byte the chosen field does not; the field is then written at
        // offset 0 through a union-typed temp so its own LLVM type drives the store.
        if (rvalue == nullptr || fieldType == nullptr || !fieldType->isSized()
            || rvalue->getType() == structType)
        {
            compiler->CreateReturnCall(llvm::Constant::getNullValue(structType));
            return;
        }
        auto* slot = compiler->AllocaAtEntry(structType, nullptr, "uniondef");
        compiler->builder->CreateStore(llvm::Constant::getNullValue(structType), slot);
        compiler->CreateAssignment(rvalue, slot, false, fieldType);
        compiler->CreateReturnCall(compiler->CreateLoad(structType, slot));
    }

std::string MainListener::GetLockArgCanonical(CFlatParser::ExpressionContext* expr) {
        return NormalizeLockText(expr->getText());
    }

std::string MainListener::GetLockArgMode(CFlatParser::ExpressionContext* expr) {
        return LockTextMode(expr->getText());
    }

std::string MainListener::StripLockModeSuffix(const std::string& text) {
        return NormalizeLockText(text);
    }

std::vector<std::string> MainListener::LockSetAliases(const std::string& canonical) {
        std::vector<std::string> tokens = { canonical };
        for (size_t i = canonical.find('.'); i != std::string::npos; i = canonical.find('.', i + 1))
            if (compilerLLVM->IsNamespace(canonical.substr(0, i)))
                tokens.push_back(canonical.substr(i + 1));
        return tokens;
    }

std::string MainListener::GuardLockKey(const LLVMBackend::TypeAndValue& tv) {
        const std::string& parent = tv.ParentVariableName;
        return parent.empty() ? tv.GuardedBy : parent + "." + tv.GuardedBy;
    }

void MainListener::CheckGuardedWrite(antlr4::ParserRuleContext* ctx, const LLVMBackend::NamedVariable& target) {
        const std::string& guard = target.TypeAndValue.GuardedBy;
        if (guard.empty()) return;

        std::string key = GuardLockKey(target.TypeAndValue);
        auto it = currentLockSet.find(key);
        if (it == currentLockSet.end()) return;
        if (it->second == LockMode::Exclusive) return;

        const std::string& name = target.FieldName.empty()
            ? target.TypeAndValue.VariableName : target.FieldName;
        if (it->second == LockMode::Optimistic)
            LogErrorContext(ctx, std::format(
                "Field '{}' is guarded by '{}': cannot write it inside an optimistic read of '{}'.",
                name, guard, key));
        else
            LogErrorContext(ctx, std::format(
                "Field '{}' is guarded by '{}': cannot write it while holding '{}' in read mode.",
                name, guard, key));
    }

void MainListener::CheckCallSiteLocks(antlr4::ParserRuleContext* ctx,
                            const std::string& receiverText,
                            const std::vector<LLVMBackend::NamedVariable>& arguments) {
        const auto& requiredLocks = compilerLLVM->lastCallRequiredLocks;
        if (requiredLocks.empty()) return;

        const auto& paramNames = compilerLLVM->lastCallParameterNames;

        for (const auto& rawLock : requiredLocks)
        {
            std::string lock = StripLockModeSuffix(rawLock);

            // Split on the first '.' to get head (owner) and rest (field path).
            size_t dot = lock.find('.');
            std::string head = (dot == std::string::npos) ? lock : lock.substr(0, dot);
            std::string rest = (dot == std::string::npos) ? "" : lock.substr(dot + 1);

            std::string canonical;
            if (head == "this")
            {
                // this-relative lock: substitute with the receiver variable name.
                if (receiverText.empty()) continue; // no known receiver - skip
                canonical = rest.empty() ? receiverText : receiverText + "." + rest;
            }
            else
            {
                // Try to match head to a formal parameter name.
                bool found = false;
                for (size_t pi = 0; pi < paramNames.size(); pi++)
                {
                    if (paramNames[pi] != head) continue;
                    // arguments[pi] corresponds to paramNames[pi] (both include implicit this at index 0).
                    if (pi < arguments.size())
                    {
                        const std::string& argName = arguments[pi].CallerName;
                        if (argName.empty()) found = true; // complex expression - can't check
                        else
                            canonical = rest.empty() ? argName : argName + "." + rest;
                    }
                    found = true;
                    break;
                }
                if (!found)
                    canonical = lock; // global lock - no substitution
            }

            if (!canonical.empty() && currentLockSet.find(canonical) == currentLockSet.end())
            {
                LogErrorContext(ctx, std::format(
                    "must hold '{}' before calling this function.", canonical));
            }
        }
    }

llvm::Function* MainListener::FindMethodOf(const std::string& methodName, const std::string& firstParamType) {
        auto it = compilerLLVM->functionTable.find(methodName);
        if (it == compilerLLVM->functionTable.end()) return nullptr;
        for (const auto& sym : it->second)
        {
            if (!sym.Parameters.empty() && sym.Parameters[0].TypeName == firstParamType)
                return sym.Function;
        }
        return nullptr;
    }

void MainListener::EmitProgramSyntheticTeardown(const std::string& name, llvm::Value* thisArg) {
        auto* compiler = compilerLLVM;
        auto* dtorFn      = compiler->builder->GetInsertBlock()->getParent();
        auto* progType    = compiler->dataStructures[name].StructType;
        auto* fatTy       = compiler->GetFatPtrType();   // {i8*, i8*}
        auto* voidPtrType = compiler->builder->getInt8Ty()->getPointerTo();
        auto* freeFn      = compiler->GetFunction("free");
        if (!progType || !freeFn) return;

        unsigned allocatorIdx  = compiler->programTable[name].AllocatorFieldIndex;
        unsigned stopSrcIdx    = compiler->programTable[name].StopSourceFieldIndex;
        unsigned inboxArenaIdx = compiler->programTable[name].InboxArenaFieldIndex;

        auto* stopSrcDisposeFn = FindMethodOf("dispose", "stop_source");
        auto stopSrcIt = compiler->dataStructures.find("stop_source");
        auto* stopSrcType = stopSrcIt != compiler->dataStructures.end() ? stopSrcIt->second.StructType : nullptr;

        // Load self->_allocator (IAllocator fat-ptr)
        auto* dtor_allocFieldGEP = compiler->builder->CreateStructGEP(
            progType, thisArg, allocatorIdx, "alloc_field_gep");
        auto* allocFatPtr = compiler->builder->CreateLoad(fatTy, dtor_allocFieldGEP, "alloc_fat_ptr");

        // if (data ptr != null) { cleanup(); free(data); zero _allocator; }
        auto* allocDataPtr = compiler->builder->CreateExtractValue(allocFatPtr, {1u}, "alloc_data");
        auto* isNotNull    = compiler->builder->CreateICmpNE(
            allocDataPtr,
            llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(voidPtrType)),
            "alloc_not_null");
        auto* cleanupBlock = llvm::BasicBlock::Create(*compiler->context, "alloc_cleanup", dtorFn);
        auto* doneBlock    = llvm::BasicBlock::Create(*compiler->context, "dtor_done",     dtorFn);
        compiler->builder->CreateCondBr(isNotNull, cleanupBlock, doneBlock);

        compiler->builder->SetInsertPoint(cleanupBlock);
        // alloca a fat-ptr slot so CallInterfaceMethod can GEP into it
        auto* allocFatPtrSlot = compiler->builder->CreateAlloca(fatTy, nullptr, "alloc_fat_slot");
        compiler->builder->CreateStore(allocFatPtr, allocFatPtrSlot);
        compiler->CallInterfaceMethod(allocFatPtrSlot, "IAllocator", "cleanup", {});
        // The allocator object came from operator new (audited), but we free it via raw
        // free() below (deterministic CRT free, independent of the thread-local active
        // allocator). Notify the heap-audit oracle first so it does not flag the block as
        // a false-positive LEAK; no-op unless HeapAudit is enabled.
        if (compiler->GetFunction("__audit_note_free"))
        {
            LLVMBackend::NamedVariable noteArg;
            noteArg.Primary  = allocDataPtr;
            noteArg.BaseType = voidPtrType;
            compiler->CreateOverloadedFunctionCall("__audit_note_free", { noteArg });
        }
        // free the underlying memory (data ptr is already in allocDataPtr)
        compiler->builder->CreateCall(freeFn->getFunctionType(), freeFn, {allocDataPtr});
        compiler->builder->CreateStore(llvm::Constant::getNullValue(fatTy), dtor_allocFieldGEP);
        compiler->builder->CreateBr(doneBlock);

        compiler->builder->SetInsertPoint(doneBlock);

        // Dispose _stop_source (no-op if _state is null, safe to call after Kill())
        if (stopSrcDisposeFn && stopSrcType)
        {
            auto* stopSrcGEP = compiler->builder->CreateStructGEP(
                progType, thisArg, stopSrcIdx, "stop_src_gep");
            compiler->builder->CreateCall(
                stopSrcDisposeFn->getFunctionType(), stopSrcDisposeFn, {stopSrcGEP});
        }

        // Free the owned inbox shell; outbox is a BORROWED handle (self-loopback alias or consumer's inbox)
        // and is never freed. arena_channel::destroy() is safe on an uninitialized shell.
        if (inboxArenaIdx != (unsigned)-1)
        {
            auto* arenaTy = compiler->dataStructures.count(kArenaChannelType)
                            ? compiler->dataStructures[kArenaChannelType].StructType : nullptr;
            auto* arenaDestroyFn = FindMethodOf("destroy", kArenaChannelType);
            if (arenaTy && arenaDestroyFn)
            {
                auto* arenaPtrTy = arenaTy->getPointerTo();
                auto* inboxGEP = compiler->builder->CreateStructGEP(
                    progType, thisArg, inboxArenaIdx, "inbox_arena_gep");
                auto* inboxPtr = compiler->builder->CreateLoad(arenaPtrTy, inboxGEP, "inbox_arena_ptr");
                auto* inboxNotNull = compiler->builder->CreateICmpNE(
                    inboxPtr, llvm::ConstantPointerNull::get(arenaPtrTy), "inbox_not_null");
                auto* inboxCleanupBB = llvm::BasicBlock::Create(*compiler->context, "inbox_cleanup", dtorFn);
                auto* inboxDoneBB    = llvm::BasicBlock::Create(*compiler->context, "inbox_dtor_done", dtorFn);
                compiler->builder->CreateCondBr(inboxNotNull, inboxCleanupBB, inboxDoneBB);

                compiler->builder->SetInsertPoint(inboxCleanupBB);
                compiler->builder->CreateCall(
                    arenaDestroyFn->getFunctionType(), arenaDestroyFn, {inboxPtr});
                auto* inboxRaw = compiler->builder->CreateBitCast(inboxPtr, voidPtrType, "inbox_raw");
                compiler->builder->CreateCall(freeFn->getFunctionType(), freeFn, {inboxRaw});
                compiler->builder->CreateStore(llvm::Constant::getNullValue(arenaPtrTy), inboxGEP);
                compiler->builder->CreateBr(inboxDoneBB);

                compiler->builder->SetInsertPoint(inboxDoneBB);
            }
        }
    }

// A synthesized program member whose exact overload slot a user definition already filled would
// make CreateFunctionDefinition take its already-defined early return, which pushes no function
// scope; the emit that follows then pops a stackNamedVariable frame that was never pushed.
void MainListener::RejectIfProgramMemberSlotTaken(CFlatParser::ProgramDefinitionContext* ctx,
        const std::string& progName, const std::string& member, const std::string& signature,
        const LLVMBackend::TypeAndValue& returnType,
        const std::vector<LLVMBackend::TypeAndValue>& params) {
        auto* compiler = compilerLLVM;

        if (member == "run" && params.size() == 2 && params[1].TypeName == "list__string"
            && params[1].IsMove)
        {
            auto it = compiler->functionTable.find(member);
            if (it != compiler->functionTable.end())
            {
                for (const auto& sym : it->second)
                {
                    if (sym.Parameters.size() != params.size()
                        || sym.ReturnType.ToUniqueString() != returnType.ToUniqueString())
                        continue;
                    bool sameExceptMove = true;
                    for (size_t i = 0; i < params.size(); i++)
                    {
                        if (sym.Parameters[i].ToUniqueString() != params[i].ToUniqueString()
                            || (i == params.size() - 1 && sym.Parameters[i].IsMove))
                        {
                            sameExceptMove = false;
                            break;
                        }
                    }
                    if (sameExceptMove)
                    {
                        LogErrorContext(ctx, std::format(
                            "program '{}': 'bool run(list<string>)' differs from the reserved "
                            "'bool run(move list<string>)' only by 'move' and will not start the "
                            "program thread; add 'move' to the parameter",
                            progName));
                    }
                }
            }
        }

        std::string clashFile;
        size_t clashLine = 0;
        if (!compiler->OverloadSlotIsDefined(member, returnType, params, false, &clashFile, &clashLine))
            return;

        std::string message = std::format(
            "program '{}': '{}' is a reserved program member - the compiler synthesizes it, and the "
            "definition at {}({}) already has that exact signature. Rename it, or give it a "
            "different signature.",
            progName, signature, clashFile, clashLine);

        if (ctx != nullptr)
            LogErrorContext(ctx, std::move(message));
        else
            compiler->LogError(std::move(message));
    }

void MainListener::EmitProgramRunWrapper(const std::string& name, CFlatParser::ProgramDefinitionContext* ctx) {
        auto* compiler = compilerLLVM;

        // Resolve types - all must be concrete by this point (ProcessPendingInstantiations ran)
        auto* progType        = compiler->dataStructures[name].StructType;
        auto findStructType = [&](const std::string& name) -> llvm::StructType* {
            auto it = compiler->dataStructures.find(name);
            return it != compiler->dataStructures.end() ? it->second.StructType : nullptr;
        };
        auto* defAllocType    = findStructType("MallocAllocator");
        auto* listStringType  = findStructType("list__string");
        auto* stringStructType= findStructType("string");
        auto* threadType      = findStructType("Thread");
        auto* fatTy           = compiler->GetFatPtrType();   // {i8*, i8*}

        if (!progType || !defAllocType || !listStringType || !threadType)
        {
            compiler->LogError(std::format(
                "program '{}': missing required type (MallocAllocator={}, list__string={}, Thread={})",
                name,
                defAllocType  ? "ok" : "missing",
                listStringType ? "ok" : "missing",
                threadType    ? "ok" : "missing"));
            return;
        }

        auto* progPtrType    = progType->getPointerTo();
        auto* defAllocPtrTy  = defAllocType->getPointerTo();
        auto* voidPtrType    = compiler->builder->getInt8Ty()->getPointerTo();
        auto* i32Type        = llvm::Type::getInt32Ty(*compiler->context);
        auto* i64Type        = llvm::Type::getInt64Ty(*compiler->context);

        // __RunArgs_Name = { Name*, list__string }
        auto* runArgsType = llvm::StructType::create(
            *compiler->context, {progPtrType, listStringType}, "__RunArgs_" + name);
        compiler->programTable[name].RunArgsType = runArgsType;

        // Look up helper functions
        auto* mallocFn         = compiler->GetFunction("malloc");
        auto* freeFn           = compiler->GetFunction("free");
        auto* defAllocCtorFn   = compiler->GetFunction("MallocAllocator");
        auto* threadCtorFn     = compiler->GetFunction("Thread");
        auto* threadStartFn    = FindMethodOf("start", "Thread");
        auto* threadJoinFn     = FindMethodOf("join", "Thread");

        bool isImported = compiler->programTable[name].IsImportedProgram;

        // Find the 'main' function for this program.
        // For imported programs, MainFunction was already set by the pre-scan.
        // For regular programs, search the function table for a method with self as first param.
        llvm::Function* mainFn = nullptr;
        if (isImported)
        {
            mainFn = compiler->programTable[name].MainFunction;
        }
        else
        {
            auto it = compiler->functionTable.find("main");
            int mainCount = 0;
            if (it != compiler->functionTable.end())
            {
                for (const auto& sym : it->second)
                {
                    if (!sym.Parameters.empty() && sym.Parameters[0].TypeName == name)
                    {
                        ++mainCount;
                        mainFn = sym.Function;
                    }
                }
            }
            if (mainCount > 1)
            {
                compiler->LogError(std::format(
                    "program '{}': multiple 'main' methods defined; only one is allowed.", name));
                return;
            }
        }

        if (!mallocFn || !freeFn || !defAllocCtorFn
            || !threadCtorFn || !threadStartFn || !threadJoinFn || !mainFn)
        {
            compiler->LogError(std::format("program '{}': missing helper function for run() generation", name));
            return;
        }

        // Detect calling style from LLVM arg count. Regular methods have a leading 'self' arg;
        // imported programs are free functions (no self), so the arg counts differ by 1.
        auto mainArgCount = static_cast<unsigned>(mainFn->arg_size());
        bool isNoArgs   = isImported ? (mainArgCount == 0) : (mainArgCount == 1);
        bool isListArgs = isImported ? (mainArgCount == 1) : (mainArgCount == 2);
        bool isArgcArgv = isImported ? (mainArgCount == 2) : (mainArgCount == 3);

        if (!isNoArgs && !isListArgs && !isArgcArgv)
        {
            compiler->LogError(std::format(
                "program '{}': 'main' has unsupported signature (expected 0, 1, or 2 user params).", name));
            return;
        }

        if (isArgcArgv && !stringStructType)
        {
            compiler->LogError(std::format(
                "program '{}': main(int,char**) style requires 'string' type to be available.", name));
            return;
        }

        unsigned exitCodeIdx      = compiler->programTable[name].ExitCodeFieldIndex;
        unsigned threadIdx        = compiler->programTable[name].ThreadFieldIndex;
        unsigned allocatorIdx     = compiler->programTable[name].AllocatorFieldIndex;
        unsigned onStdoutIdx         = compiler->programTable[name].OnStdoutFieldIndex;
        unsigned onStdinIdx          = compiler->programTable[name].OnStdinFieldIndex;
        unsigned onStdinReturnIdx    = compiler->programTable[name].OnStdinReturnFieldIndex;
        unsigned stopSrcIdx          = compiler->programTable[name].StopSourceFieldIndex;
        unsigned trackHandlesIdx     = compiler->programTable[name].TrackHandlesFieldIndex;
        unsigned fpConfigIdx         = compiler->programTable[name].FpConfigFieldIndex;

        auto* stopSrcInitFn        = FindMethodOf("init",           "stop_source");
        auto* stopSrcRequestStopFn = FindMethodOf("request_stop",   "stop_source");
        // _stop_source dispose happens in ~Name() via EmitProgramSyntheticTeardown.
        auto* stopSrcType          = compiler->dataStructures.count("stop_source")
                                     ? compiler->dataStructures["stop_source"].StructType : nullptr;

        // __ProgramTLS field indices - must match struct __ProgramTLS in cruntime.cb
        constexpr int kPTLS_stdout_hook            = 0;
        constexpr int kPTLS_stdin_hook             = 4;
        constexpr int kPTLS_stdin_return_hook      = 8;
        constexpr int kPTLS_cached_stdin           = 9;
        constexpr int kPTLS_handle_tracker_enabled = 10;
        constexpr int kPTLS_handle_tracker_head    = 11;
        constexpr int kPTLS_stdin_active           = 12;

        // Look up the single thread-local TLS struct (declared in cruntime.cb)
        llvm::GlobalVariable* progTlsGlobal = nullptr;
        {
            auto it = compiler->globalNamedVariable.find("__prog_tls");
            if (it != compiler->globalNamedVariable.end()) progTlsGlobal = it->second;
        }
        if (!progTlsGlobal)
        {
            compiler->LogError(std::format(
                "program '{}': __prog_tls not found - cruntime.cb must be imported", name));
            return;
        }
        auto* progTlsType        = llvm::StructType::getTypeByName(*compiler->context, "__ProgramTLS");
        auto* hookFnPtrType      = progTlsType->getElementType(kPTLS_stdout_hook);
        auto* stdinHookFnPtrType = progTlsType->getElementType(kPTLS_stdin_hook);

        // Cast trampoline to the expected function pointer type: int(*)(void*)
        auto* trampolineFnTy = llvm::FunctionType::get(i32Type, {voidPtrType}, false);

        // SEH filter: always return EXCEPTION_EXECUTE_HANDLER (1) - catch everything.
        // Emitted once per module (deduped by name). Uses an isolated IRBuilder so the
        // main builder's insertion point is not disturbed.
        llvm::Function* sehFilterFn = compiler->module->getFunction("__cflat_seh_filter_always");
        if (!sehFilterFn)
        {
            auto* filterTy = llvm::FunctionType::get(i32Type, {voidPtrType, voidPtrType}, false);
            sehFilterFn = llvm::Function::Create(
                filterTy, llvm::Function::InternalLinkage,
                "__cflat_seh_filter_always", *compiler->module);
            auto* fEntry = llvm::BasicBlock::Create(*compiler->context, "entry", sehFilterFn);
            llvm::IRBuilder<> fb(fEntry);
            fb.CreateRet(fb.getInt32(1));
        }

        // ======================================================================
        // EMIT TRAMPOLINE: int __program_run_Name(void* ctx)
        // Runs on the spawned thread. Sets up allocator, calls main, stores
        // exitCode and allocator pointer into self, frees the args packet,
        // returns main's result. Allocator cleanup happens in ~Name().
        // ======================================================================
        {
            LLVMBackend::TypeAndValue intReturn;   intReturn.TypeName = "int";
            LLVMBackend::DeclTypeAndValue ctxParam;
            ctxParam.TypeName = "void";  ctxParam.VariableName = "ctx";  ctxParam.Pointer = true;
            RejectIfProgramMemberSlotTaken(ctx, name, "__program_run_" + name,
                "int __program_run_" + name + "(void*)", intReturn, {ctxParam});
            auto* trampolineFn = compiler->CreateFunctionDefinition(
                "__program_run_" + name, intReturn, {ctxParam}, false, false, 0, false, false);
            compiler->programTable[name].TrampolineFunction = trampolineFn;

            // Install Windows SEH personality so hardware faults in main() are caught.
            // Win32 does NOT set a personality: LLVM's x86 backend drops the catch handler
            // body when using _except_handler3 + catchpad/catchret, leaving broken EH tables.
            // POSIX has no SEH equivalent (a hardware fault is a thread-directed signal whose
            // default action kills the process); crash recovery is Win64-only and the test
            // guards the crash cases with if const (__WINDOWS__ ...). Both non-SEH targets fall
            // through to the plain-call path below.
            if (compiler->targetWindows_ && compiler->platformValue == 64)
            {
                llvm::Function* cshFn = compiler->module->getFunction("__C_specific_handler");
                if (!cshFn)
                {
                    auto* cshTy = llvm::FunctionType::get(i32Type, /*isVarArg=*/true);
                    cshFn = llvm::cast<llvm::Function>(
                        compiler->module->getOrInsertFunction("__C_specific_handler", cshTy).getCallee());
                    cshFn->setDLLStorageClass(llvm::GlobalValue::DLLImportStorageClass);
                }
                trampolineFn->setPersonalityFn(cshFn);
            }

            auto* ctxArg = trampolineFn->getArg(0);

            // Cast void* ctx to __RunArgs_Name*
            auto* argsPacket = compiler->builder->CreateBitCast(
                ctxArg, runArgsType->getPointerTo(), "args_packet");

            // Load self (Name*) from field 0
            auto* selfGEP  = compiler->builder->CreateStructGEP(runArgsType, argsPacket, 0, "self_gep");
            auto* self     = compiler->builder->CreateLoad(progPtrType, selfGEP, "self");

            // Pointer to list__string (field 1) - passed by value to main
            auto* argsGEP  = compiler->builder->CreateStructGEP(runArgsType, argsPacket, 1, "args_gep");

            // Load self->_allocator (IAllocator fat-ptr); user may have set it before run().
            auto* allocFieldGEP = compiler->builder->CreateStructGEP(
                progType, self, allocatorIdx, "alloc_field_gep");
            auto* existingFatPtr = compiler->builder->CreateLoad(fatTy, allocFieldGEP, "existing_alloc");

            // Check data ptr (field 1): null means user left _allocator unset -> use default.
            auto* existingDataPtr = compiler->builder->CreateExtractValue(existingFatPtr, {1u}, "existing_data");
            auto* isNull = compiler->builder->CreateICmpEQ(
                existingDataPtr,
                llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(voidPtrType)),
                "alloc_is_null");

            auto* defaultBlock = llvm::BasicBlock::Create(*compiler->context, "alloc_default", trampolineFn);
            auto* useBlock     = llvm::BasicBlock::Create(*compiler->context, "alloc_use",     trampolineFn);
            compiler->builder->CreateCondBr(isNull, defaultBlock, useBlock);

            // Default path: create a MallocAllocator on the heap and build its IAllocator fat-ptr.
            compiler->builder->SetInsertPoint(defaultBlock);
            auto* defAllocSize = compiler->GetTypeSizeBytes(defAllocType);
            auto* defAllocRaw  = compiler->builder->CreateCall(
                mallocFn->getFunctionType(), mallocFn, {defAllocSize}, "def_alloc_raw");
            auto* defAllocPtr  = compiler->builder->CreateBitCast(defAllocRaw, defAllocPtrTy, "def_alloc_ptr");
            auto* defAllocInit = compiler->builder->CreateCall(
                defAllocCtorFn->getFunctionType(), defAllocCtorFn, {}, "def_alloc_init");
            compiler->builder->CreateStore(defAllocInit, defAllocPtr);
            auto* defVtable    = compiler->GetOrCreateVTable("MallocAllocator", "IAllocator");
            auto* defFatPtr    = compiler->BuildInterfaceFatValue(defVtable, defAllocPtr);
            compiler->builder->CreateStore(defFatPtr, allocFieldGEP);
            compiler->builder->CreateBr(useBlock);

            // Merge: load the (possibly just-written) fat-ptr from self->_allocator.
            compiler->builder->SetInsertPoint(useBlock);
            auto* activeFatPtr = compiler->builder->CreateLoad(fatTy, allocFieldGEP, "active_alloc");

            // Set thread-local __active_allocator to the IAllocator fat-ptr.
            auto* activeAllocGlobal = compiler->globalNamedVariable["__active_allocator"];
            compiler->builder->CreateStore(activeFatPtr, activeAllocGlobal);

            // Arm the per-thread FP environment from self->_fpConfig (0 = no-op).
            // Runs on the program thread, mirroring the Thread trampoline's __fp_apply call.
            if (auto* fpApplyFn = compiler->GetFunction("__fp_apply"))
            {
                auto* fpCfgGEP = compiler->builder->CreateStructGEP(
                    progType, self, fpConfigIdx, "fp_config_gep");
                auto* fpCfgVal = compiler->builder->CreateLoad(
                    llvm::Type::getInt32Ty(*compiler->context), fpCfgGEP, "fp_config");
                compiler->builder->CreateCall(fpApplyFn->getFunctionType(), fpApplyFn, {fpCfgVal});
            }

            // Install stdout hook: load self->onStdout and store into __prog_tls.stdout_hook
            {
                auto* onStdoutGEP = compiler->builder->CreateStructGEP(
                    progType, self, onStdoutIdx, "on_stdout_gep");
                auto* onStdoutVal = compiler->builder->CreateLoad(
                    hookFnPtrType, onStdoutGEP, "on_stdout_fn");
                auto* stdoutHookGEP = compiler->builder->CreateStructGEP(
                    progTlsType, progTlsGlobal, kPTLS_stdout_hook, "stdout_hook_gep");
                compiler->builder->CreateStore(onStdoutVal, stdoutHookGEP);
            }

            // Install stdin hook: load self->onStdin and store into __prog_tls.stdin_hook
            {
                auto* onStdinGEP = compiler->builder->CreateStructGEP(
                    progType, self, onStdinIdx, "on_stdin_gep");
                auto* onStdinVal = compiler->builder->CreateLoad(
                    stdinHookFnPtrType, onStdinGEP, "on_stdin_fn");
                auto* stdinHookGEP = compiler->builder->CreateStructGEP(
                    progTlsType, progTlsGlobal, kPTLS_stdin_hook, "stdin_hook_gep");
                compiler->builder->CreateStore(onStdinVal, stdinHookGEP);
            }

            // Install stdin return hook: load self->onStdinReturn and store into __prog_tls.stdin_return_hook
            {
                auto* onStdinReturnGEP = compiler->builder->CreateStructGEP(
                    progType, self, onStdinReturnIdx, "on_stdin_return_gep");
                auto* stdinReturnHookType = progTlsType->getElementType(kPTLS_stdin_return_hook);
                auto* onStdinReturnVal = compiler->builder->CreateLoad(
                    stdinReturnHookType, onStdinReturnGEP, "on_stdin_return_fn");
                auto* stdinReturnHookGEP = compiler->builder->CreateStructGEP(
                    progTlsType, progTlsGlobal, kPTLS_stdin_return_hook, "stdin_return_hook_gep");
                compiler->builder->CreateStore(onStdinReturnVal, stdinReturnHookGEP);
            }

            // Eagerly init cached_stdin and activate the stdin fast path.
            // This pre-populates __prog_tls.cached_stdin so fgets avoids a lazy-init branch on every call,
            // and sets stdin_active so fgets can use a single-field guard instead of three separate checks.
            {
                // stdin FILE*: Windows reads it from the CRT's __acrt_iob_func(0); POSIX has no
                // such symbol, so route through the runtime's __std_iob(0) shim (cruntime.cb),
                // which returns the libc `stdin` global. Keeps the Windows path byte-identical.
                llvm::Value* stdinPtr = nullptr;
                if (compiler->targetWindows_)
                {
                    auto* ioFuncTy  = llvm::FunctionType::get(voidPtrType, {i32Type}, false);
                    auto* ioFuncFn  = compiler->module->getOrInsertFunction("__acrt_iob_func", ioFuncTy).getCallee();
                    stdinPtr = compiler->builder->CreateCall(
                        llvm::cast<llvm::Function>(ioFuncFn)->getFunctionType(), ioFuncFn,
                        {llvm::ConstantInt::get(i32Type, 0)}, "stdin_ptr");
                }
                else if (auto* iobFn = compiler->GetFunction("__std_iob"))
                {
                    stdinPtr = compiler->builder->CreateCall(
                        iobFn->getFunctionType(), iobFn,
                        {llvm::ConstantInt::get(i32Type, 0)}, "stdin_ptr");
                }
                else
                {
                    // __std_iob missing (runtime not imported) - leave cached_stdin null; the
                    // fgetc/scanf lazy-init path repopulates it on first use.
                    stdinPtr = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(voidPtrType));
                }
                auto* cachedStdinGEP = compiler->builder->CreateStructGEP(
                    progTlsType, progTlsGlobal, kPTLS_cached_stdin, "cached_stdin_gep");
                compiler->builder->CreateStore(stdinPtr, cachedStdinGEP);
                auto* stdinActiveGEP = compiler->builder->CreateStructGEP(
                    progTlsType, progTlsGlobal, kPTLS_stdin_active, "stdin_active_gep");
                compiler->builder->CreateStore(llvm::ConstantInt::getTrue(*compiler->context), stdinActiveGEP);
            }

            // Enable handle tracker: load self->trackHandles and store into __prog_tls.handle_tracker_enabled
            {
                auto* trackGEP = compiler->builder->CreateStructGEP(
                    progType, self, trackHandlesIdx, "track_handles_gep");
                // Field is int (i32) to avoid i1-in-ConstantStruct LLVM assertion; convert to i1 for the TLS field
                auto* trackI32 = compiler->builder->CreateLoad(
                    llvm::Type::getInt32Ty(*compiler->context), trackGEP, "track_handles_i32");
                auto* trackI1  = compiler->builder->CreateICmpNE(
                    trackI32,
                    llvm::ConstantInt::get(llvm::Type::getInt32Ty(*compiler->context), 0),
                    "track_handles_val");
                auto* enabledGEP = compiler->builder->CreateStructGEP(
                    progTlsType, progTlsGlobal, kPTLS_handle_tracker_enabled, "htrack_enabled_gep");
                compiler->builder->CreateStore(trackI1, enabledGEP);
            }

            auto* cleanupBB = llvm::BasicBlock::Create(*compiler->context, "seh_cleanup", trampolineFn);

            // Load list__string args by value from the packet
            llvm::Value* argsVal = compiler->builder->CreateLoad(listStringType, argsGEP, "args_val");

            // Re-home the args into the program allocator: the caller built them under the CRT
            // allocator; without adoption, freeing them under the program allocator corrupts the heap.
            if (isListArgs)
            {
                auto* adoptFn = compiler->GetFunction("__prog_adopt_args");
                if (adoptFn)
                    argsVal = compiler->builder->CreateCall(
                        adoptFn->getFunctionType(), adoptFn, {argsVal}, "adopted_args");
                else
                    compiler->LogError(std::format(
                        "program '{}': __prog_adopt_args not found - program.cb must be imported", name));
            }

            // exitCodeGEP must dominate all paths - compute in the entry block.
            auto* exitCodeGEP = compiler->builder->CreateStructGEP(
                progType, self, exitCodeIdx, "exit_code_gep");

            // ---- ArgcArgv conversion: list<string> -> argc + char** argv ----
            // Used only when main(int argc, char** argv) style is detected.
            // argvHolderAlloca stores the malloc'd argv array so cleanupBB can free it.
            llvm::AllocaInst* argvHolderAlloca = nullptr;
            llvm::Value* argc32Val  = nullptr;
            llvm::Value* argvPtrVal = nullptr;

            if (isArgcArgv)
            {
                // Alloca to hold argv ptr across SEH paths; initialized to null.
                argvHolderAlloca = compiler->AllocaAtEntry(voidPtrType, nullptr, "argv_holder");
                compiler->builder->CreateStore(
                    llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(voidPtrType)),
                    argvHolderAlloca);

                // list<string> layout: { string* _data, i32 _size, i32 _capacity }
                argc32Val        = compiler->builder->CreateExtractValue(argsVal, {1u}, "argc");
                auto* dataPtr    = compiler->builder->CreateExtractValue(argsVal, {0u}, "args_data");

                // malloc char*[argc+1]
                auto* argcPlus1    = compiler->builder->CreateAdd(argc32Val, compiler->builder->getInt32(1), "argc_p1");
                auto* argcPlus1_64 = compiler->builder->CreateZExt(argcPlus1, i64Type, "argc_p1_64");
                auto* ptrSize      = compiler->GetTypeSizeBytes(voidPtrType);
                auto* argvBytes    = compiler->builder->CreateMul(argcPlus1_64, ptrSize, "argv_bytes");
                auto* argvRaw      = compiler->builder->CreateCall(
                    mallocFn->getFunctionType(), mallocFn, {argvBytes}, "argv_raw");
                compiler->builder->CreateStore(argvRaw, argvHolderAlloca);
                argvPtrVal = compiler->builder->CreateBitCast(
                    argvRaw, voidPtrType->getPointerTo(), "argv_ptr");

                // Loop: argv[i] = data[i]._ptr  (string field 0 is i8* _ptr)
                auto* iAlloca   = compiler->AllocaAtEntry(i32Type, nullptr, "argv_i");
                compiler->builder->CreateStore(compiler->builder->getInt32(0), iAlloca);
                auto* loopCondBB = llvm::BasicBlock::Create(*compiler->context, "argv_cond", trampolineFn);
                auto* loopBodyBB = llvm::BasicBlock::Create(*compiler->context, "argv_body", trampolineFn);
                auto* loopDoneBB = llvm::BasicBlock::Create(*compiler->context, "argv_done", trampolineFn);
                compiler->builder->CreateBr(loopCondBB);

                compiler->builder->SetInsertPoint(loopCondBB);
                auto* iVal  = compiler->builder->CreateLoad(i32Type, iAlloca, "i");
                auto* check = compiler->builder->CreateICmpSLT(iVal, argc32Val, "loop_cond");
                compiler->builder->CreateCondBr(check, loopBodyBB, loopDoneBB);

                compiler->builder->SetInsertPoint(loopBodyBB);
                auto* i64Val    = compiler->builder->CreateZExt(iVal, i64Type, "i64");
                auto* elemPtr   = compiler->builder->CreateGEP(stringStructType, dataPtr, {i64Val}, "elem");
                auto* ptrFldGEP = compiler->builder->CreateStructGEP(stringStructType, elemPtr, 0, "elem_ptr");
                auto* charPtr   = compiler->builder->CreateLoad(voidPtrType, ptrFldGEP, "char_ptr");
                auto* argvSlot  = compiler->builder->CreateGEP(voidPtrType, argvPtrVal, {i64Val}, "argv_slot");
                compiler->builder->CreateStore(charPtr, argvSlot);
                auto* iNext = compiler->builder->CreateAdd(iVal, compiler->builder->getInt32(1), "i_next");
                compiler->builder->CreateStore(iNext, iAlloca);
                compiler->builder->CreateBr(loopCondBB);

                compiler->builder->SetInsertPoint(loopDoneBB);
                // Null-terminate: argv[argc] = nullptr
                auto* argc64    = compiler->builder->CreateZExt(argc32Val, i64Type, "argc64");
                auto* nullSlot  = compiler->builder->CreateGEP(voidPtrType, argvPtrVal, {argc64}, "null_slot");
                compiler->builder->CreateStore(
                    llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(voidPtrType)),
                    nullSlot);
            }

            // Build the argument list for the call/invoke based on main style.
            // Imported programs are free functions (no self pointer).
            std::vector<llvm::Value*> mainArgs;
            if (isImported)
            {
                if (isNoArgs)
                    mainArgs = {};
                else if (isListArgs)
                    mainArgs = {argsVal};
                else // ArgcArgv
                    mainArgs = {argc32Val, argvPtrVal};
            }
            else
            {
                if (isNoArgs)
                    mainArgs = {self};
                else if (isListArgs)
                    mainArgs = {self, argsVal};
                else // ArgcArgv
                    mainArgs = {self, argc32Val, argvPtrVal};
            }

            if (compiler->targetWindows_ && compiler->platformValue == 64)
            {
                // Win64: use SEH (invoke + catchswitch + catchpad) to catch hardware faults.
                auto* normalBB   = llvm::BasicBlock::Create(*compiler->context, "main_normal",  trampolineFn);
                auto* dispatchBB = llvm::BasicBlock::Create(*compiler->context, "seh_dispatch", trampolineFn);
                auto* catchBB    = llvm::BasicBlock::Create(*compiler->context, "seh_catch",    trampolineFn);

                // noinline: prevents the optimizer from inlining main() into this trampoline,
                // which would move null-dereference faults outside the invoke's protected region.
                mainFn->addFnAttr(llvm::Attribute::NoInline);

                // Invoke main() - normal return lands in normalBB, any fault unwinds to dispatchBB.
                auto* invokeInst = compiler->builder->CreateInvoke(
                    mainFn->getFunctionType(), mainFn,
                    normalBB, dispatchBB,
                    mainArgs, "main_result");

                // normalBB: main returned cleanly - store exit code, fall through to cleanup.
                compiler->builder->SetInsertPoint(normalBB);
                compiler->builder->CreateStore(invokeInst, exitCodeGEP);
                compiler->builder->CreateBr(cleanupBB);

                // dispatchBB: catchswitch routes all exceptions to catchBB.
                compiler->builder->SetInsertPoint(dispatchBB);
                auto* catchSwitch = compiler->builder->CreateCatchSwitch(
                    llvm::ConstantTokenNone::get(*compiler->context),
                    nullptr, 1, "cs");
                catchSwitch->addHandler(catchBB);

                // catchBB: catch everything (filter returns 1), store sentinel -1, rejoin cleanup.
                compiler->builder->SetInsertPoint(catchBB);
                auto* catchPad = compiler->builder->CreateCatchPad(
                    catchSwitch, {static_cast<llvm::Value*>(sehFilterFn)}, "cp");
                compiler->builder->CreateStore(
                    llvm::ConstantInt::get(i32Type, static_cast<uint64_t>(-1), /*isSigned=*/true),
                    exitCodeGEP);
                compiler->builder->CreateCatchRet(catchPad, cleanupBB);
            }
            else
            {
                // No SEH here (Win32 or any POSIX target). On Win32 LLVM's x86 backend drops
                // the catch-handler body (_except_handler3 + catchpad/catchret -> broken EH
                // tables); POSIX has no SEH at all. Fall back to a plain call - a hardware fault
                // in main() is not recoverable on these targets.
                auto* callResult = compiler->builder->CreateCall(
                    mainFn->getFunctionType(), mainFn, mainArgs, "main_result");
                compiler->builder->CreateStore(callResult, exitCodeGEP);
                compiler->builder->CreateBr(cleanupBB);
            }

            // cleanupBB: shared teardown - both normal and exception paths converge here.
            compiler->builder->SetInsertPoint(cleanupBB);

            // Auto-close a directly-piped output stream: `producer >> consumer` synthesizes a hidden stream
            // with _autoClose set; explicit `p >> s; s >> q` also sets _out but closes manually - leave it alone.
            {
                unsigned outIdx = compiler->programTable[name].OutFieldIndex;
                auto* streamTy  = compiler->dataStructures.count("stream")
                                  ? compiler->dataStructures["stream"].StructType : nullptr;
                auto* closeFn   = FindMethodOf("close", "stream");
                unsigned autoCloseIdx = (unsigned)-1;
                if (streamTy)
                {
                    const auto& sfields = compiler->dataStructures["stream"].StructFields;
                    for (unsigned i = 0; i < sfields.size(); ++i)
                        if (sfields[i].VariableName == "_autoClose") { autoCloseIdx = i; break; }
                }
                if (outIdx != (unsigned)-1 && streamTy && closeFn && autoCloseIdx != (unsigned)-1)
                {
                    auto* streamPtrTy = streamTy->getPointerTo();
                    auto* outGEP = compiler->builder->CreateStructGEP(progType, self, outIdx, "out_field_gep");
                    auto* outPtr = compiler->builder->CreateLoad(streamPtrTy, outGEP, "out_stream");
                    auto* outNotNull = compiler->builder->CreateICmpNE(
                        outPtr, llvm::ConstantPointerNull::get(streamPtrTy), "out_not_null");

                    auto* acChkBB  = llvm::BasicBlock::Create(*compiler->context, "autoclose_chk",  trampolineFn);
                    auto* acDoBB   = llvm::BasicBlock::Create(*compiler->context, "autoclose_do",   trampolineFn);
                    auto* acContBB = llvm::BasicBlock::Create(*compiler->context, "autoclose_cont", trampolineFn);
                    compiler->builder->CreateCondBr(outNotNull, acChkBB, acContBB);

                    compiler->builder->SetInsertPoint(acChkBB);
                    auto* acElemTy = streamTy->getStructElementType(autoCloseIdx);
                    auto* acGEP = compiler->builder->CreateStructGEP(streamTy, outPtr, autoCloseIdx, "autoclose_gep");
                    auto* acVal = compiler->builder->CreateLoad(acElemTy, acGEP, "autoclose");
                    auto* acTrue = compiler->builder->CreateICmpNE(
                        acVal, llvm::ConstantInt::get(acElemTy, 0), "autoclose_true");
                    compiler->builder->CreateCondBr(acTrue, acDoBB, acContBB);

                    compiler->builder->SetInsertPoint(acDoBB);
                    compiler->builder->CreateCall(closeFn->getFunctionType(), closeFn, {outPtr});
                    compiler->builder->CreateBr(acContBB);

                    compiler->builder->SetInsertPoint(acContBB);
                }
            }

            compiler->builder->CreateStore(llvm::Constant::getNullValue(fatTy), activeAllocGlobal);
            {
                auto* stdoutHookGEP = compiler->builder->CreateStructGEP(
                    progTlsType, progTlsGlobal, kPTLS_stdout_hook, "stdout_hook_gep");
                compiler->builder->CreateStore(
                    llvm::Constant::getNullValue(hookFnPtrType), stdoutHookGEP);
                auto* stdinHookGEP = compiler->builder->CreateStructGEP(
                    progTlsType, progTlsGlobal, kPTLS_stdin_hook, "stdin_hook_gep");
                compiler->builder->CreateStore(
                    llvm::Constant::getNullValue(stdinHookFnPtrType), stdinHookGEP);
                auto* stdinActiveGEP = compiler->builder->CreateStructGEP(
                    progTlsType, progTlsGlobal, kPTLS_stdin_active, "stdin_active_gep");
                compiler->builder->CreateStore(
                    llvm::ConstantInt::getFalse(*compiler->context), stdinActiveGEP);
            }

            // Handle tracker cleanup: disable tracker first (so fclose won't re-enter the list),
            // then walk the linked list and fclose any handles still open from a crash.
            {
                auto* enabledGEP = compiler->builder->CreateStructGEP(
                    progTlsType, progTlsGlobal, kPTLS_handle_tracker_enabled, "htrack_enabled_gep");
                compiler->builder->CreateStore(
                    llvm::ConstantInt::getFalse(*compiler->context), enabledGEP);
                auto* headFieldGEP = compiler->builder->CreateStructGEP(
                    progTlsType, progTlsGlobal, kPTLS_handle_tracker_head, "htrack_head_field_gep");

                auto* fcloseTy = llvm::FunctionType::get(i32Type, {voidPtrType}, false);
                auto* fcloseFn = compiler->module->getOrInsertFunction("fclose", fcloseTy).getCallee();

                auto* nodeTy = llvm::StructType::getTypeByName(*compiler->context, "__HandleNode");

                auto* htrackLoopBB  = llvm::BasicBlock::Create(*compiler->context, "htrack_loop",  trampolineFn);
                auto* htrackBodyBB  = llvm::BasicBlock::Create(*compiler->context, "htrack_body",  trampolineFn);
                auto* htrackCloseBB = llvm::BasicBlock::Create(*compiler->context, "htrack_close", trampolineFn);
                auto* htrackSkipBB  = llvm::BasicBlock::Create(*compiler->context, "htrack_skip",  trampolineFn);
                auto* htrackDoneBB  = llvm::BasicBlock::Create(*compiler->context, "htrack_done",  trampolineFn);
                compiler->builder->CreateBr(htrackLoopBB);

                // Loop header: load head, exit if null
                compiler->builder->SetInsertPoint(htrackLoopBB);
                auto* headVal  = compiler->builder->CreateLoad(voidPtrType, headFieldGEP, "htrack_head");
                auto* headNull = compiler->builder->CreateICmpEQ(
                    headVal,
                    llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(voidPtrType)),
                    "htrack_head_null");
                compiler->builder->CreateCondBr(headNull, htrackDoneBB, htrackBodyBB);

                // Loop body: load handle and next from node
                compiler->builder->SetInsertPoint(htrackBodyBB);
                auto* handleGEP = compiler->builder->CreateStructGEP(nodeTy, headVal, 0, "htrack_handle_gep");
                auto* handleVal = compiler->builder->CreateLoad(voidPtrType, handleGEP, "htrack_handle");
                auto* nextGEP   = compiler->builder->CreateStructGEP(nodeTy, headVal, 1, "htrack_next_gep");
                auto* nextVal   = compiler->builder->CreateLoad(voidPtrType, nextGEP, "htrack_next");
                auto* handleNull = compiler->builder->CreateICmpEQ(
                    handleVal,
                    llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(voidPtrType)),
                    "htrack_handle_null");
                compiler->builder->CreateCondBr(handleNull, htrackSkipBB, htrackCloseBB);

                // Close the handle
                compiler->builder->SetInsertPoint(htrackCloseBB);
                compiler->builder->CreateCall(fcloseTy, fcloseFn, {handleVal});
                compiler->builder->CreateBr(htrackSkipBB);

                // Advance head, free current node, continue loop
                compiler->builder->SetInsertPoint(htrackSkipBB);
                compiler->builder->CreateStore(nextVal, headFieldGEP);
                compiler->builder->CreateCall(freeFn->getFunctionType(), freeFn, {headVal});
                compiler->builder->CreateBr(htrackLoopBB);

                compiler->builder->SetInsertPoint(htrackDoneBB);
            }

            // Free argv array if ArgcArgv style (may be null on exception path before allocation).
            if (isArgcArgv && argvHolderAlloca)
            {
                auto* argvToFree = compiler->builder->CreateLoad(voidPtrType, argvHolderAlloca, "argv_to_free");
                compiler->builder->CreateCall(freeFn->getFunctionType(), freeFn, {argvToFree});
            }

            // Destruct the args list when main does NOT own it. Only main(move list<string>)
            // takes ownership (and frees it at scope exit); a no-arg or main(argc,argv) entry
            // never receives the list, so the packet's copy (_data buffer + owning string
            // elements) would otherwise leak. The active allocator is already nulled above, so
            // ~list__string frees under the CRT allocator the caller built the args with; the
            // argv array (which only borrows each element's _ptr) was just freed above.
            if (!isListArgs)
            {
                if (auto* argsDtor = compiler->GetOrCreateFullDestructor("list__string"))
                    compiler->builder->CreateCall(argsDtor->getFunctionType(), argsDtor, {argsGEP});
            }

            compiler->builder->CreateCall(freeFn->getFunctionType(), freeFn, {ctxArg});
            auto* finalExitCode = compiler->builder->CreateLoad(i32Type, exitCodeGEP, "final_exit");
            compiler->CreateReturnCall(finalExitCode);
            compiler->CreateBlockBreak(nullptr, true);
        }

        // ======================================================================
        // EMIT run(): bool run(Name* this, list__string args)
        // Allocates args packet, spawns thread into self->_thread, returns
        // whether the thread started. Does NOT join - caller uses WaitForExit().
        // ======================================================================
        {
            LLVMBackend::TypeAndValue boolReturn;  boolReturn.TypeName = "bool";
            LLVMBackend::DeclTypeAndValue thisParam;
            thisParam.TypeName = name;  thisParam.VariableName = name + "__";  thisParam.Pointer = true;
            LLVMBackend::DeclTypeAndValue argsParam;
            argsParam.TypeName = "list__string";  argsParam.VariableName = "args";
            argsParam.IsMove = true;  // run() takes ownership; caller's list is zeroed after the call

            RejectIfProgramMemberSlotTaken(ctx, name, "run", "bool run(move list<string>)",
                boolReturn, {thisParam, argsParam});
            auto* runFn = compiler->CreateFunctionDefinition("run", boolReturn, {thisParam, argsParam});
            compiler->programTable[name].RunFunction = runFn;

            auto* thisArg = runFn->getArg(0);   // Name*
            auto* argsArg = runFn->getArg(1);   // list__string by value (move)

            // Malloc the args packet (raw malloc - tracked alloc is per-thread)
            auto* pkgSize = compiler->GetTypeSizeBytes(runArgsType);
            auto* pkgRaw  = compiler->builder->CreateCall(
                mallocFn->getFunctionType(), mallocFn, {pkgSize}, "pkg_raw");
            auto* pkg = compiler->builder->CreateBitCast(pkgRaw, runArgsType->getPointerTo(), "pkg");

            // Store this -> pkg->self (field 0)
            auto* selfGEP = compiler->builder->CreateStructGEP(runArgsType, pkg, 0, "pkg_self_gep");
            compiler->builder->CreateStore(thisArg, selfGEP);

            // Store args -> pkg->args (field 1): use the original argument value (pre-alloca copy)
            auto* argsGEP = compiler->builder->CreateStructGEP(runArgsType, pkg, 1, "pkg_args_gep");
            compiler->builder->CreateStore(argsArg, argsGEP);

            // Zero run()'s args alloca so ~list__string is a no-op at scope exit.
            // Ownership of _data transfers to the packet; trampoline frees the packet on completion.
            {
                auto& runArgNV = compiler->stackNamedVariable.back().functionArgument["args"];
                if (runArgNV.Storage != nullptr)
                    compiler->builder->CreateStore(
                        llvm::ConstantAggregateZero::get(listStringType), runArgNV.Storage);
            }

            // Init _stop_source before spawning - gives main() a live token to check
            if (stopSrcInitFn && stopSrcType)
            {
                auto* stopSrcGEP = compiler->builder->CreateStructGEP(
                    progType, thisArg, stopSrcIdx, "stop_src_gep");
                compiler->builder->CreateCall(
                    stopSrcInitFn->getFunctionType(), stopSrcInitFn, {stopSrcGEP});
            }

            // Get &self->_thread (stored field; initialized by the program ctor)
            auto* threadFieldGEP = compiler->builder->CreateStructGEP(
                progType, thisArg, threadIdx, "thread_field");

            // Thread.start(&self->_thread, trampoline, pkg) -> bool
            // Thread.start() takes a THIN function<int(void*)> - a bare C function
            // pointer. Pass the trampoline bitcast to the param's thin signature.
            auto* trampolineFn  = compiler->programTable[name].TrampolineFunction;
            auto* startFnParamTy = threadStartFn->getFunctionType()->getParamType(1);
            auto* trampolineThin = compiler->builder->CreateBitCast(trampolineFn, startFnParamTy, "tramp_thin");
            // fpConfig arg is 0 here: the program thread arms its FP environment
            // from self->_fpConfig inside the trampoline (EmitProgramRunWrapper),
            // not via Thread.start. Pass an explicit 0 to match the 3-param
            // start(fn, ctx, fpConfig) signature.
            auto* startResult = compiler->builder->CreateCall(
                threadStartFn->getFunctionType(), threadStartFn,
                {threadFieldGEP, trampolineThin, pkgRaw, compiler->builder->getInt32(0)}, "start_result");

            // On start failure: free pkg, return false
            auto* successBlock = llvm::BasicBlock::Create(*compiler->context, "start_ok",   runFn);
            auto* failBlock    = llvm::BasicBlock::Create(*compiler->context, "start_fail", runFn);
            compiler->builder->CreateCondBr(startResult, successBlock, failBlock);

            compiler->builder->SetInsertPoint(failBlock);
            compiler->builder->CreateCall(freeFn->getFunctionType(), freeFn, {pkgRaw});
            compiler->builder->CreateRet(compiler->builder->getFalse());

            // On start success: return true (trampoline owns pkg from here)
            compiler->builder->SetInsertPoint(successBlock);
            compiler->builder->CreateRet(compiler->builder->getTrue());

            compiler->CreateBlockBreak(nullptr, true);
        }

        // ======================================================================
        // EMIT WaitForExit(): void WaitForExit(Name* this)
        // Blocks until the program thread exits. exitCode field is readable after.
        // ======================================================================
        {
            LLVMBackend::TypeAndValue voidReturn;  voidReturn.TypeName = "void";
            LLVMBackend::DeclTypeAndValue thisParam;
            thisParam.TypeName = name;  thisParam.VariableName = name + "__";  thisParam.Pointer = true;

            RejectIfProgramMemberSlotTaken(ctx, name, "WaitForExit", "void WaitForExit()",
                voidReturn, {thisParam});
            compiler->CreateFunctionDefinition("WaitForExit", voidReturn, {thisParam});

            auto* thisArg = compiler->builder->GetInsertBlock()->getParent()->getArg(0);

            // Get &self->_thread and join
            auto* threadFieldGEP = compiler->builder->CreateStructGEP(
                progType, thisArg, threadIdx, "thread_field");
            compiler->builder->CreateCall(
                threadJoinFn->getFunctionType(), threadJoinFn, {threadFieldGEP});

            compiler->CreateReturnCall(nullptr);
            compiler->CreateBlockBreak(nullptr, true);
        }

        // ======================================================================
        // EMIT WaitForExit(stop_token): bool WaitForExit(Name* this, stop_token token)
        // Polls until the thread exits or the token is cancelled.
        // Returns true if thread exited; false if cancelled (thread NOT joined).
        // ======================================================================
        {
            auto* stopTokenType = compiler->dataStructures.count("stop_token")
                                  ? compiler->dataStructures["stop_token"].StructType : nullptr;
            auto* waitOrStopFn  = stopTokenType
                                  ? FindMethodOf("__wait_thread_or_stop", "Thread") : nullptr;

            if (stopTokenType && waitOrStopFn)
            {
                LLVMBackend::TypeAndValue boolReturn;   boolReturn.TypeName = "bool";
                LLVMBackend::DeclTypeAndValue thisParam;
                thisParam.TypeName = name;  thisParam.VariableName = name + "__";  thisParam.Pointer = true;
                LLVMBackend::DeclTypeAndValue tokenParam;
                tokenParam.TypeName = "stop_token";  tokenParam.VariableName = "token";

                RejectIfProgramMemberSlotTaken(ctx, name, "WaitForExit", "bool WaitForExit(stop_token)",
                    boolReturn, {thisParam, tokenParam});
                auto* waitFn = compiler->CreateFunctionDefinition("WaitForExit", boolReturn, {thisParam, tokenParam});

                auto* thisArg  = waitFn->getArg(0);
                auto* tokenArg = waitFn->getArg(1);

                auto* threadFieldGEP = compiler->builder->CreateStructGEP(
                    progType, thisArg, threadIdx, "thread_field");

                auto* result = compiler->builder->CreateCall(
                    waitOrStopFn->getFunctionType(), waitOrStopFn,
                    {threadFieldGEP, tokenArg}, "wait_result");

                compiler->builder->CreateRet(result);
                compiler->CreateBlockBreak(nullptr, true);
            }
        }

        // ======================================================================
        // EMIT WaitForExit(int): bool WaitForExit(Name* this, int timeoutMs)
        // Single try_join call with the given timeout. Returns true if the thread
        // exited within the timeout; false if still running (handles intact).
        // ======================================================================
        {
            auto* threadTryJoinFn = FindMethodOf("try_join", "Thread");
            if (threadTryJoinFn)
            {
                LLVMBackend::TypeAndValue boolReturn;  boolReturn.TypeName = "bool";
                LLVMBackend::DeclTypeAndValue thisParam;
                thisParam.TypeName = name;  thisParam.VariableName = name + "__";  thisParam.Pointer = true;
                LLVMBackend::DeclTypeAndValue msParam;
                msParam.TypeName = "int";  msParam.VariableName = "timeoutMs";

                RejectIfProgramMemberSlotTaken(ctx, name, "WaitForExit", "bool WaitForExit(int)",
                    boolReturn, {thisParam, msParam});
                auto* waitFn = compiler->CreateFunctionDefinition("WaitForExit", boolReturn, {thisParam, msParam});

                auto* thisArg = waitFn->getArg(0);
                auto* msArg   = waitFn->getArg(1);

                auto* threadFieldGEP = compiler->builder->CreateStructGEP(
                    progType, thisArg, threadIdx, "thread_field");

                auto* result = compiler->builder->CreateCall(
                    threadTryJoinFn->getFunctionType(), threadTryJoinFn,
                    {threadFieldGEP, msArg}, "try_join_result");

                compiler->builder->CreateRet(result);
                compiler->CreateBlockBreak(nullptr, true);
            }
        }

        // ======================================================================
        // EMIT RequestStop(): void RequestStop(Name* this)
        // Signals the program's _stop_source so main() can observe it via
        // _stop_source.get_token().stop_requested(). Cooperative - main() must check.
        // ======================================================================
        {
            if (stopSrcRequestStopFn && stopSrcType)
            {
                LLVMBackend::TypeAndValue voidReturn;  voidReturn.TypeName = "void";
                LLVMBackend::DeclTypeAndValue thisParam;
                thisParam.TypeName = name;  thisParam.VariableName = name + "__";  thisParam.Pointer = true;

                RejectIfProgramMemberSlotTaken(ctx, name, "RequestStop", "void RequestStop()",
                    voidReturn, {thisParam});
                compiler->CreateFunctionDefinition("RequestStop", voidReturn, {thisParam});

                auto* thisArg = compiler->builder->GetInsertBlock()->getParent()->getArg(0);

                auto* stopSrcGEP = compiler->builder->CreateStructGEP(
                    progType, thisArg, stopSrcIdx, "stop_src_gep");
                compiler->builder->CreateCall(
                    stopSrcRequestStopFn->getFunctionType(), stopSrcRequestStopFn, {stopSrcGEP});

                compiler->CreateReturnCall(nullptr);
                compiler->CreateBlockBreak(nullptr, true);
            }
        }

        // ======================================================================
        // EMIT Kill(): void Kill(Name* this)
        // Signals RequestStop() first (cooperative), then forcibly terminates
        // via TerminateThread. Leaks allocator state and thread-held resources.
        // ======================================================================
        {
            auto* threadTerminateFn = FindMethodOf("terminate", "Thread");
            if (threadTerminateFn)
            {
                LLVMBackend::TypeAndValue voidReturn;  voidReturn.TypeName = "void";
                LLVMBackend::DeclTypeAndValue thisParam;
                thisParam.TypeName = name;  thisParam.VariableName = name + "__";  thisParam.Pointer = true;

                RejectIfProgramMemberSlotTaken(ctx, name, "Kill", "void Kill()", voidReturn, {thisParam});
                compiler->CreateFunctionDefinition("Kill", voidReturn, {thisParam});

                auto* thisArg = compiler->builder->GetInsertBlock()->getParent()->getArg(0);

                // Signal the stop token first - gives cooperative loops a chance to observe it.
                if (stopSrcRequestStopFn && stopSrcType)
                {
                    auto* stopSrcGEP = compiler->builder->CreateStructGEP(
                        progType, thisArg, stopSrcIdx, "stop_src_gep");
                    compiler->builder->CreateCall(
                        stopSrcRequestStopFn->getFunctionType(), stopSrcRequestStopFn, {stopSrcGEP});
                }

                auto* threadFieldGEP = compiler->builder->CreateStructGEP(
                    progType, thisArg, threadIdx, "thread_field");
                compiler->builder->CreateCall(
                    threadTerminateFn->getFunctionType(), threadTerminateFn, {threadFieldGEP});

                compiler->CreateReturnCall(nullptr);
                compiler->CreateBlockBreak(nullptr, true);
            }
        }

        // ======================================================================
        // EMIT ~Name(): void ~Name(Name* this)
        // The program always has exactly one compiler-owned destructor that ends with
        // the builtin field teardown (free _allocator, dispose _stop_source, free the
        // consumer-owned inbox arena_channel - see EmitProgramSyntheticTeardown).
        //
        // When the user wrote their own ~Name(), ParseProgramDestructorDefinition already
        // emitted it with the teardown appended at the end and set Destructor, so we skip
        // this block. Otherwise we synthesize a ~Name() that is just the teardown.
        // Null-checks make it safe to call even if run() was never called.
        // ======================================================================
        if (compiler->dataStructures[name].Destructor == nullptr)
        {
            LLVMBackend::TypeAndValue voidReturn;  voidReturn.TypeName = "void";
            LLVMBackend::DeclTypeAndValue thisParam;
            thisParam.TypeName = name;  thisParam.VariableName = name + "__";  thisParam.Pointer = true;

            auto* dtorFn = compiler->CreateFunctionDefinition("~" + name, voidReturn, {thisParam});
            compiler->RegisterDestructor(name, dtorFn);

            EmitProgramSyntheticTeardown(name, dtorFn->getArg(0));

            compiler->CreateReturnCall(nullptr);
            compiler->CreateBlockBreak(nullptr, true);
        }

        // ======================================================================
        // EMIT exitCode(): int exitCode(Name* this)
        // Returns the exitCode field - satisfies the IProcess interface contract.
        // ======================================================================
        {
            LLVMBackend::TypeAndValue intReturn;  intReturn.TypeName = "int";
            LLVMBackend::DeclTypeAndValue thisParam;
            thisParam.TypeName = name;  thisParam.VariableName = name + "__";  thisParam.Pointer = true;

            RejectIfProgramMemberSlotTaken(ctx, name, "exitCode", "int exitCode()", intReturn, {thisParam});
            auto* exitCodeFn = compiler->CreateFunctionDefinition("exitCode", intReturn, {thisParam});

            auto* thisArg = exitCodeFn->getArg(0);
            auto* exitCodeGEP = compiler->builder->CreateStructGEP(
                progType, thisArg, exitCodeIdx, "exit_code_gep");
            auto* exitCodeVal = compiler->builder->CreateLoad(i32Type, exitCodeGEP, "exit_code");
            compiler->builder->CreateRet(exitCodeVal);
            compiler->CreateBlockBreak(nullptr, true);
        }
    }

void MainListener::ParseImportedProgramDefinition(const std::string& name) {
        auto* compiler = compilerLLVM;

        if (compiler->IsVerbose())
            std::cout << "[verbose]     parse imported program: " << name << "\n";

        // No user-declared fields - only synthetic fields
        std::vector<LLVMBackend::DeclTypeAndValue> declList;

        unsigned exitCodeFieldIndex     = (unsigned)declList.size();
        unsigned threadFieldIndex       = exitCodeFieldIndex + 1;
        unsigned allocatorFieldIndex    = threadFieldIndex + 1;
        unsigned onStdoutFieldIndex      = allocatorFieldIndex + 1;
        unsigned onStdinFieldIndex       = onStdoutFieldIndex + 1;
        unsigned onStdinReturnFieldIndex = onStdinFieldIndex + 1;
        unsigned stopSrcFieldIndex       = onStdinReturnFieldIndex + 1;
        unsigned trackHandlesFieldIndex  = stopSrcFieldIndex + 1;
        unsigned useChannelFieldIndex    = trackHandlesFieldIndex + 1;
        unsigned fpConfigFieldIndex      = useChannelFieldIndex + 1;
        unsigned outFieldIndex           = (unsigned)-1;
        unsigned inStreamFieldIndex      = (unsigned)-1;
        unsigned inboxArenaFieldIndex    = (unsigned)-1;
        unsigned outboxFieldIndex        = (unsigned)-1;
        bool     hasStreamType           = compiler->dataStructures.count("stream") > 0;
        bool     hasArenaChannelType     = EnsureArenaChannelInstantiated(compiler);
        {
            LLVMBackend::DeclTypeAndValue exitCodeField;
            exitCodeField.TypeName     = "int";
            exitCodeField.VariableName = "exitCode";
            declList.push_back(exitCodeField);

            LLVMBackend::DeclTypeAndValue threadField;
            threadField.TypeName     = "Thread";
            threadField.VariableName = "_thread";
            declList.push_back(threadField);

            LLVMBackend::DeclTypeAndValue allocatorField;
            allocatorField.TypeName     = "IAllocator";
            allocatorField.VariableName = "_allocator";
            allocatorField.IsInterface  = true;
            allocatorField.Pointer      = true;
            declList.push_back(allocatorField);

            LLVMBackend::DeclTypeAndValue onStdoutField;
            onStdoutField.VariableName          = "onStdout";
            onStdoutField.IsFunctionPointer     = true;
            onStdoutField.FuncPtrReturnTypeName = "void";
            onStdoutField.FuncPtrParams         = {{"char", true}, {"int", false}};
            declList.push_back(onStdoutField);

            LLVMBackend::DeclTypeAndValue onStdinField;
            onStdinField.VariableName          = "onStdin";
            onStdinField.IsFunctionPointer     = true;
            onStdinField.FuncPtrReturnTypeName = "char";
            onStdinField.FuncPtrReturnPointer  = true;
            onStdinField.FuncPtrParams         = {};
            declList.push_back(onStdinField);

            LLVMBackend::DeclTypeAndValue onStdinReturnField;
            onStdinReturnField.VariableName          = "onStdinReturn";
            onStdinReturnField.IsFunctionPointer     = true;
            onStdinReturnField.FuncPtrReturnTypeName = "void";
            onStdinReturnField.FuncPtrParams         = {{"char", true}};
            declList.push_back(onStdinReturnField);

            LLVMBackend::DeclTypeAndValue stopSrcField;
            stopSrcField.TypeName     = "stop_source";
            stopSrcField.VariableName = "_stop_source";
            declList.push_back(stopSrcField);

            LLVMBackend::DeclTypeAndValue trackHandlesField;
            trackHandlesField.TypeName     = "int";
            trackHandlesField.VariableName = "trackHandles";
            declList.push_back(trackHandlesField);

            LLVMBackend::DeclTypeAndValue useChannelField;
            useChannelField.TypeName     = "int";   // int not bool - avoids i1-in-ConstantStruct assertion
            useChannelField.VariableName = "useChannel";
            declList.push_back(useChannelField);

            LLVMBackend::DeclTypeAndValue fpConfigField;
            fpConfigField.TypeName     = "int";   // per-thread FP environment knob applied on the program thread
            fpConfigField.VariableName = "_fpConfig";
            declList.push_back(fpConfigField);

            if (hasStreamType) {
                LLVMBackend::DeclTypeAndValue outField;
                outField.TypeName     = "stream";
                outField.VariableName = "_out";
                outField.Pointer      = true;
                outFieldIndex = (unsigned)declList.size();
                declList.push_back(outField);

                LLVMBackend::DeclTypeAndValue inField;
                inField.TypeName     = "stream";
                inField.VariableName = "_in";
                inField.Pointer      = true;
                inStreamFieldIndex = (unsigned)declList.size();
                declList.push_back(inField);
            }

            // inbox / outbox: program-owned arena_channel handles for `a >> b` rich piping
            // (see ParseProgramDefinition for the full rationale). Both default null.
            if (hasArenaChannelType) {
                LLVMBackend::DeclTypeAndValue inboxArenaField;
                inboxArenaField.TypeName     = kArenaChannelType;
                inboxArenaField.VariableName = "inbox";
                inboxArenaField.Pointer      = true;
                inboxArenaFieldIndex = (unsigned)declList.size();
                declList.push_back(inboxArenaField);

                LLVMBackend::DeclTypeAndValue outboxField;
                outboxField.TypeName     = kArenaChannelType;
                outboxField.VariableName = "outbox";
                outboxField.Pointer      = true;
                outboxFieldIndex = (unsigned)declList.size();
                declList.push_back(outboxField);
            }
        }

        auto* structType = compiler->CreateStructType(name, declList);
        if (structType->isOpaque())
            structType->setBody(llvm::ArrayRef<llvm::Type*>());

        // Create default constructor
        {
            LLVMBackend::TypeAndValue returnType;
            returnType.TypeName = name;
            compiler->CreateFunctionDefinition(name, returnType, {});

            std::vector<llvm::Value*> initializers;
            for (auto& typeValue : declList)
            {
                llvm::Value* rvalue = nullptr;
                auto* initializer = typeValue.Initializer;
                // Unreachable today: this emitter's declList is entirely synthetic; wired for symmetry.
                if (auto* braceList = FieldDefaultBraceList(typeValue))
                {
                    // Emitting a real function body; clear the stale file-scope global_scope
                    // so the brace list's stores and calls lower as ordinary instructions.
                    GlobalScopeGuard defaultCtorScope(global_scope);
                    rvalue = ParseFieldDefaultBraceInitializer(name, typeValue, braceList);
                }
                else if (initializer)
                {
                    if (auto* ae = initializer->assignmentExpression())
                        rvalue = ParseFieldDefaultInitializer(name, typeValue, ae);
                    else if (initializer->Default())
                    {
                        // Synthetic default-ctor body: clear the stale file-scope global_scope so a
                        // struct-typed field's `= default` runs that field's own default constructor
                        // (its field initializers) rather than zero-filling. See GenerateDefaultValue.
                        GlobalScopeGuard defaultCtorScope(global_scope);
                        rvalue = GenerateDefaultValue(typeValue);
                    }
                }
                if (rvalue == nullptr && compiler->GetType(typeValue)->isArrayTy())
                {
                    GlobalScopeGuard defaultCtorScope(global_scope);
                    rvalue = GenerateDefaultValue(typeValue);
                }
                initializers.push_back(rvalue);
            }

            // Seed with zero (not undef) so fields lacking an explicit initializer read as
            // 0/null after `= default` / `= {}` instead of leaking stack garbage. Fields that
            // do have an initializer are overwritten by CreateInsertValue below, so a
            // fully-initialized struct optimizes to the same IR as the old undef seed.
            llvm::Value* structVal = llvm::Constant::getNullValue(structType);
            unsigned int idx = 0;
            for (auto* rvalue : initializers)
            {
                if (rvalue)
                {
                    auto* destType = structType->getTypeAtIndex(idx);
                    rvalue = compiler->Upconvert(rvalue, destType);
                    if (rvalue->getType() != destType && destType->isStructTy())
                    {
                        std::string fieldTypeName = declList[idx].TypeName;
                        // forceRoot: the GetFunction guard is an exact-key lookup, so a namespace walk
                        // here would call a same-named sibling type's ctor (layer 3).
                        if (compiler->GetFunction(fieldTypeName))
                            rvalue = compiler->CreateOverloadedFunctionCall(fieldTypeName, {}, true);
                        else
                            rvalue = llvm::Constant::getNullValue(destType);
                    }
                    structVal = compiler->CreateInsertValue(structVal, rvalue, idx);
                }
                idx++;
            }

            // exitCode = -1
            {
                auto* minusOne = llvm::ConstantInt::getSigned(
                    llvm::Type::getInt32Ty(*compiler->context), -1);
                structVal = compiler->CreateInsertValue(structVal, minusOne, exitCodeFieldIndex);
            }

            // _thread = Thread()
            if (auto* threadCtorFn = compiler->GetFunction("Thread"))
            {
                auto* threadInitVal = compiler->builder->CreateCall(
                    threadCtorFn->getFunctionType(), threadCtorFn, {}, "thread_init");
                structVal = compiler->CreateInsertValue(structVal, threadInitVal, threadFieldIndex);
            }

            // _allocator = zero (null fat-ptr)
            {
                auto* fatTy = compiler->GetFatPtrType();
                structVal = compiler->CreateInsertValue(
                    structVal, llvm::Constant::getNullValue(fatTy), allocatorFieldIndex);
            }

            // onStdout = nullptr
            {
                auto& onStdoutDecl = declList[onStdoutFieldIndex];
                auto* fieldType = compiler->GetType(onStdoutDecl);
                structVal = compiler->CreateInsertValue(
                    structVal, llvm::Constant::getNullValue(fieldType), onStdoutFieldIndex);
            }

            // onStdin = nullptr
            {
                auto& onStdinDecl = declList[onStdinFieldIndex];
                auto* fieldType = compiler->GetType(onStdinDecl);
                structVal = compiler->CreateInsertValue(
                    structVal, llvm::Constant::getNullValue(fieldType), onStdinFieldIndex);
            }

            // onStdinReturn = nullptr
            {
                auto& onStdinReturnDecl = declList[onStdinReturnFieldIndex];
                auto* fieldType = compiler->GetType(onStdinReturnDecl);
                structVal = compiler->CreateInsertValue(
                    structVal, llvm::Constant::getNullValue(fieldType), onStdinReturnFieldIndex);
            }

            // _stop_source = stop_source()
            if (auto* stopSrcCtorFn = compiler->GetFunction("stop_source"))
            {
                auto* stopSrcInitVal = compiler->builder->CreateCall(
                    stopSrcCtorFn->getFunctionType(), stopSrcCtorFn, {}, "stop_src_zero");
                structVal = compiler->CreateInsertValue(structVal, stopSrcInitVal, stopSrcFieldIndex);
            }

            // trackHandles = 0
            structVal = compiler->CreateInsertValue(
                structVal,
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(*compiler->context), 0),
                trackHandlesFieldIndex);

            // useChannel = 0 (opt-in; `p1 >> p2` only wires the arena channel when both are set)
            structVal = compiler->CreateInsertValue(
                structVal,
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(*compiler->context), 0),
                useChannelFieldIndex);

            // _fpConfig = 0 (no-op; user sets `prog._fpConfig = FP_*` to arm the program thread)
            structVal = compiler->CreateInsertValue(
                structVal,
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(*compiler->context), 0),
                fpConfigFieldIndex);

            // _out / _in = nullptr (only when stream.cb is imported)
            if (outFieldIndex != (unsigned)-1)
            {
                auto* streamTy = compiler->GetDataStructure("stream").StructType;
                auto* nullStream = llvm::Constant::getNullValue(streamTy->getPointerTo());
                structVal = compiler->CreateInsertValue(structVal, nullStream, outFieldIndex);
                structVal = compiler->CreateInsertValue(structVal, nullStream, inStreamFieldIndex);
            }

            // Allocate a non-null arena_channel shell: send/recv are no-ops on the uninitialized shell,
            // but a non-null `this` prevents crashes if recv/send is called before `>>` wires the channel.
            if (inboxArenaFieldIndex != (unsigned)-1)
            {
                auto* arenaTy = compiler->GetDataStructure(kArenaChannelType).StructType;
                llvm::Value* shell = EmitArenaChannelShellAlloc(compiler);
                if (!shell) shell = llvm::Constant::getNullValue(arenaTy->getPointerTo());
                structVal = compiler->CreateInsertValue(structVal, shell, inboxArenaFieldIndex);
                structVal = compiler->CreateInsertValue(structVal, shell, outboxFieldIndex);
            }

            compiler->CreateReturnCall(structVal);
            compiler->CreateBlockBreak(nullptr, true);
        }

        ProcessPendingInstantiations();

        compiler->programTable[name].StructType          = structType;
        compiler->programTable[name].ConfigFields        = declList;
        compiler->programTable[name].ExitCodeFieldIndex  = exitCodeFieldIndex;
        compiler->programTable[name].ThreadFieldIndex    = threadFieldIndex;
        compiler->programTable[name].AllocatorFieldIndex = allocatorFieldIndex;
        compiler->programTable[name].OnStdoutFieldIndex       = onStdoutFieldIndex;
        compiler->programTable[name].OnStdinFieldIndex        = onStdinFieldIndex;
        compiler->programTable[name].OnStdinReturnFieldIndex  = onStdinReturnFieldIndex;
        compiler->programTable[name].StopSourceFieldIndex    = stopSrcFieldIndex;
        compiler->programTable[name].TrackHandlesFieldIndex  = trackHandlesFieldIndex;
        compiler->programTable[name].UseChannelFieldIndex    = useChannelFieldIndex;
        compiler->programTable[name].FpConfigFieldIndex      = fpConfigFieldIndex;
        compiler->programTable[name].OutFieldIndex           = outFieldIndex;
        compiler->programTable[name].InStreamFieldIndex      = inStreamFieldIndex;
        compiler->programTable[name].InboxArenaFieldIndex    = inboxArenaFieldIndex;
        compiler->programTable[name].OutboxFieldIndex        = outboxFieldIndex;
        // IsImportedProgram and MainFunction were already set by LLVMBackend.cpp pre-scan

        // All imported programs implicitly implement IProcess.
        compiler->programTable[name].Interfaces = { "IProcess" };

        EmitProgramRunWrapper(name);
    }

void MainListener::ParseProgramDefinition(CFlatParser::ProgramDefinitionContext* ctx) {
        auto* compiler = Compiler(ctx);
        std::string name = ctx->directDeclarator()->getText();

        if (compiler->IsVerbose())
            std::cout << "[verbose]     parse program: " << name << "\n";

        // Queue generic types used in field declarations and function parameters
        if (activeTypeSubstitutions.empty())
        {
            for (auto decl : ctx->declaration())
                ScanAndQueueGenericTypeUses(decl);
            for (auto func : ctx->functionDefinition())
                ScanAndQueueGenericTypeUses(func);
            ProcessPendingInstantiations();
        }

        auto declList = ParseDeclarationList(ctx->declaration());

        // Guard against user fields clashing with auto-injected synthetic fields
        for (auto& field : declList)
        {
            if (field.VariableName == "exitCode" || field.VariableName == "_thread"
                || field.VariableName == "_allocator" || field.VariableName == "onStdout"
                || field.VariableName == "onStdin" || field.VariableName == "onStdinReturn"
                || field.VariableName == "_stop_source" || field.VariableName == "inbox"
                || field.VariableName == "outbox" || field.VariableName == "useChannel"
                || field.VariableName == "trackHandles" || field.VariableName == "_out" || field.VariableName == "_in"
                || field.VariableName == "_fpConfig")
                compiler->LogError(std::format(
                    "program '{}': field name '{}' is reserved", name, field.VariableName));
        }

        // Inject synthetic fields after user-declared ones. trackHandles is i32 to avoid i1-in-ConstantStruct
        // LLVM assertion. stream and arena_channel fields only injected when their .cb is imported.
        unsigned exitCodeFieldIndex     = (unsigned)declList.size();
        unsigned threadFieldIndex       = exitCodeFieldIndex + 1;
        unsigned allocatorFieldIndex    = threadFieldIndex + 1;
        unsigned onStdoutFieldIndex      = allocatorFieldIndex + 1;
        unsigned onStdinFieldIndex       = onStdoutFieldIndex + 1;
        unsigned onStdinReturnFieldIndex = onStdinFieldIndex + 1;
        unsigned stopSrcFieldIndex       = onStdinReturnFieldIndex + 1;
        unsigned trackHandlesFieldIndex  = stopSrcFieldIndex + 1;
        unsigned useChannelFieldIndex    = trackHandlesFieldIndex + 1;
        unsigned fpConfigFieldIndex      = useChannelFieldIndex + 1;
        unsigned outFieldIndex           = (unsigned)-1;  // set below if stream.cb is imported
        unsigned inStreamFieldIndex      = (unsigned)-1;  // set below if stream.cb is imported
        unsigned inboxArenaFieldIndex    = (unsigned)-1;  // set below if arena_channel.cb is imported
        unsigned outboxFieldIndex        = (unsigned)-1;  // set below if arena_channel.cb is imported
        bool     hasStreamType           = compiler->dataStructures.count("stream") > 0;
        bool     hasArenaChannelType     = EnsureArenaChannelInstantiated(compiler);
        {
            LLVMBackend::DeclTypeAndValue exitCodeField;
            exitCodeField.TypeName     = "int";
            exitCodeField.VariableName = "exitCode";
            declList.push_back(exitCodeField);

            LLVMBackend::DeclTypeAndValue threadField;
            threadField.TypeName     = "Thread";
            threadField.VariableName = "_thread";
            declList.push_back(threadField);

            LLVMBackend::DeclTypeAndValue allocatorField;
            allocatorField.TypeName     = "IAllocator";
            allocatorField.VariableName = "_allocator";
            allocatorField.IsInterface  = true;
            allocatorField.Pointer      = true;
            declList.push_back(allocatorField);

            LLVMBackend::DeclTypeAndValue onStdoutField;
            onStdoutField.VariableName          = "onStdout";
            onStdoutField.IsFunctionPointer     = true;
            onStdoutField.FuncPtrReturnTypeName = "void";
            onStdoutField.FuncPtrParams         = {{"char", true}, {"int", false}};
            declList.push_back(onStdoutField);

            LLVMBackend::DeclTypeAndValue onStdinField;
            onStdinField.VariableName          = "onStdin";
            onStdinField.IsFunctionPointer     = true;
            onStdinField.FuncPtrReturnTypeName = "char";
            onStdinField.FuncPtrReturnPointer  = true;
            onStdinField.FuncPtrParams         = {};
            declList.push_back(onStdinField);

            LLVMBackend::DeclTypeAndValue onStdinReturnField;
            onStdinReturnField.VariableName          = "onStdinReturn";
            onStdinReturnField.IsFunctionPointer     = true;
            onStdinReturnField.FuncPtrReturnTypeName = "void";
            onStdinReturnField.FuncPtrParams         = {{"char", true}};
            declList.push_back(onStdinReturnField);

            LLVMBackend::DeclTypeAndValue stopSrcField;
            stopSrcField.TypeName     = "stop_source";
            stopSrcField.VariableName = "_stop_source";
            declList.push_back(stopSrcField);

            LLVMBackend::DeclTypeAndValue trackHandlesField;
            trackHandlesField.TypeName     = "int";
            trackHandlesField.VariableName = "trackHandles";
            declList.push_back(trackHandlesField);

            LLVMBackend::DeclTypeAndValue useChannelField;
            useChannelField.TypeName     = "int";   // int not bool - avoids i1-in-ConstantStruct assertion
            useChannelField.VariableName = "useChannel";
            declList.push_back(useChannelField);

            LLVMBackend::DeclTypeAndValue fpConfigField;
            fpConfigField.TypeName     = "int";   // per-thread FP environment knob applied on the program thread
            fpConfigField.VariableName = "_fpConfig";
            declList.push_back(fpConfigField);

            if (hasStreamType) {
                LLVMBackend::DeclTypeAndValue outField;
                outField.TypeName     = "stream";
                outField.VariableName = "_out";
                outField.Pointer      = true;
                outFieldIndex = (unsigned)declList.size();
                declList.push_back(outField);

                LLVMBackend::DeclTypeAndValue inField;
                inField.TypeName     = "stream";
                inField.VariableName = "_in";
                inField.Pointer      = true;
                inStreamFieldIndex = (unsigned)declList.size();
                declList.push_back(inField);
            }

            // inbox / outbox: program-owned arena_channel handles - the messaging mailbox and
            // the `a >> b` rich-piping endpoints. Consumer owns inbox (lazily allocated by >>);
            // producer's outbox is bound to the consumer's inbox. Both default null - programs
            // that never message pay nothing.
            if (hasArenaChannelType) {
                LLVMBackend::DeclTypeAndValue inboxArenaField;
                inboxArenaField.TypeName     = kArenaChannelType;
                inboxArenaField.VariableName = "inbox";
                inboxArenaField.Pointer      = true;
                inboxArenaFieldIndex = (unsigned)declList.size();
                declList.push_back(inboxArenaField);

                LLVMBackend::DeclTypeAndValue outboxField;
                outboxField.TypeName     = kArenaChannelType;
                outboxField.VariableName = "outbox";
                outboxField.Pointer      = true;
                outboxFieldIndex = (unsigned)declList.size();
                declList.push_back(outboxField);
            }
        }

        // Build struct type with user fields + synthetic fields
        auto* structType = compiler->CreateStructType(name, declList);
        if (structType->isOpaque())
            structType->setBody(llvm::ArrayRef<llvm::Type*>());

        // Create default constructor (same pattern as ParseStructDefinition)
        {
            LLVMBackend::TypeAndValue returnType;
            returnType.TypeName = name;
            compiler->CreateFunctionDefinition(name, returnType, {});

            std::vector<llvm::Value*> initializers;
            for (auto& typeValue : declList)
            {
                llvm::Value* rvalue = nullptr;
                auto* initializer = typeValue.Initializer;
                if (auto* braceList = FieldDefaultBraceList(typeValue))
                {
                    // Emitting a real function body; clear the stale file-scope global_scope
                    // so the brace list's stores and calls lower as ordinary instructions.
                    GlobalScopeGuard defaultCtorScope(global_scope);
                    rvalue = ParseFieldDefaultBraceInitializer(name, typeValue, braceList);
                }
                else if (initializer)
                {
                    if (auto* ae = initializer->assignmentExpression())
                        rvalue = ParseFieldDefaultInitializer(name, typeValue, ae);
                    else if (initializer->Default())
                    {
                        // Synthetic default-ctor body: clear the stale file-scope global_scope so a
                        // struct-typed field's `= default` runs that field's own default constructor
                        // (its field initializers) rather than zero-filling. See GenerateDefaultValue.
                        GlobalScopeGuard defaultCtorScope(global_scope);
                        rvalue = GenerateDefaultValue(typeValue);
                    }
                }
                initializers.push_back(rvalue);
            }

            // Seed with zero (not undef) so fields lacking an explicit initializer read as
            // 0/null after `= default` / `= {}` instead of leaking stack garbage. Fields that
            // do have an initializer are overwritten by CreateInsertValue below, so a
            // fully-initialized struct optimizes to the same IR as the old undef seed.
            llvm::Value* structVal = llvm::Constant::getNullValue(structType);
            unsigned int idx = 0;
            for (auto* rvalue : initializers)
            {
                auto* destType = structType->getTypeAtIndex(idx);
                // No explicit initializer on a struct-typed USER field - call its default ctor
                // (same fallback as ParseStructDefinition). Synthetic fields are written below.
                if (rvalue == nullptr && idx < exitCodeFieldIndex && destType->isStructTy())
                {
                    std::string fieldTypeName = declList[idx].TypeName;
                    // forceRoot: the GetFunction guard is an exact-key lookup, so a namespace walk
                    // here would call a same-named sibling type's ctor (layer 3).
                    if (compiler->GetFunction(fieldTypeName))
                        rvalue = compiler->CreateOverloadedFunctionCall(fieldTypeName, {}, true);
                    else
                        rvalue = llvm::Constant::getNullValue(destType);
                }
                if (rvalue)
                {
                    rvalue = compiler->Upconvert(rvalue, destType);
                    if (rvalue->getType() != destType)
                    {
                        if (destType->isStructTy())
                        {
                            // Initializer type doesn't match struct field type (same fallback as ParseStructDefinition).
                            std::string fieldTypeName = declList[idx].TypeName;
                            // forceRoot: the GetFunction guard is an exact-key lookup, so a namespace walk
                            // here would call a same-named sibling type's ctor (layer 3).
                            if (compiler->GetFunction(fieldTypeName))
                                rvalue = compiler->CreateOverloadedFunctionCall(fieldTypeName, {}, true);
                            else
                                rvalue = llvm::Constant::getNullValue(destType);
                        }
                        else
                        {
                            // Narrowing field initializer (e.g. u8 r = 255 has i32 literal).
                            if (ShouldWarnImplicitFieldNarrowing(rvalue, destType, declList[idx].TypeName))
                                compiler->LogWarning(std::format(
                                    "implicit narrowing to '{}' in field '{}' - use an explicit cast",
                                    declList[idx].TypeName,
                                    declList[idx].VariableName));
                            rvalue = compiler->CreateCast(rvalue, destType);
                        }
                    }
                    structVal = compiler->CreateInsertValue(structVal, rvalue, idx);
                }
                idx++;
            }

            // Synthetic field: exitCode = -1
            {
                auto* minusOne = llvm::ConstantInt::getSigned(
                    llvm::Type::getInt32Ty(*compiler->context), -1);
                structVal = compiler->CreateInsertValue(structVal, minusOne, exitCodeFieldIndex);
            }

            // Synthetic field: _thread = Thread()
            if (auto* threadCtorFn = compiler->GetFunction("Thread"))
            {
                auto* threadInitVal = compiler->builder->CreateCall(
                    threadCtorFn->getFunctionType(), threadCtorFn, {}, "thread_init");
                structVal = compiler->CreateInsertValue(structVal, threadInitVal, threadFieldIndex);
            }

            // Synthetic field: _allocator = zero (null IAllocator fat-ptr)
            {
                auto* fatTy = compiler->GetFatPtrType();
                structVal = compiler->CreateInsertValue(
                    structVal, llvm::Constant::getNullValue(fatTy), allocatorFieldIndex);
            }

            // Synthetic field: onStdout = nullptr (fat closure struct {i8*, i8*})
            {
                auto& onStdoutDecl = declList[onStdoutFieldIndex];
                auto* fieldType = compiler->GetType(onStdoutDecl);
                structVal = compiler->CreateInsertValue(
                    structVal, llvm::Constant::getNullValue(fieldType), onStdoutFieldIndex);
            }

            // Synthetic field: onStdin = nullptr (fat closure struct {i8*, i8*})
            {
                auto& onStdinDecl = declList[onStdinFieldIndex];
                auto* fieldType = compiler->GetType(onStdinDecl);
                structVal = compiler->CreateInsertValue(
                    structVal, llvm::Constant::getNullValue(fieldType), onStdinFieldIndex);
            }

            // Synthetic field: onStdinReturn = nullptr (fat closure struct {i8*, i8*})
            {
                auto& onStdinReturnDecl = declList[onStdinReturnFieldIndex];
                auto* fieldType = compiler->GetType(onStdinReturnDecl);
                structVal = compiler->CreateInsertValue(
                    structVal, llvm::Constant::getNullValue(fieldType), onStdinReturnFieldIndex);
            }

            // Synthetic field: _stop_source = stop_source() (zero-init; init() called in run())
            if (auto* stopSrcCtorFn = compiler->GetFunction("stop_source"))
            {
                auto* stopSrcInitVal = compiler->builder->CreateCall(
                    stopSrcCtorFn->getFunctionType(), stopSrcCtorFn, {}, "stop_src_zero");
                structVal = compiler->CreateInsertValue(structVal, stopSrcInitVal, stopSrcFieldIndex);
            }

            // Synthetic field: trackHandles = 0 (int, not bool - avoids i1-in-ConstantStruct assertion)
            structVal = compiler->CreateInsertValue(
                structVal,
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(*compiler->context), 0),
                trackHandlesFieldIndex);

            // Synthetic field: useChannel = 0 (opt-in arena-channel gate for `p1 >> p2`)
            structVal = compiler->CreateInsertValue(
                structVal,
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(*compiler->context), 0),
                useChannelFieldIndex);

            // Synthetic field: _fpConfig = 0 (no-op; user arms the program thread via `prog._fpConfig = FP_*`)
            structVal = compiler->CreateInsertValue(
                structVal,
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(*compiler->context), 0),
                fpConfigFieldIndex);

            // Synthetic fields: _out = nullptr, _in = nullptr (stream*; only present when stream.cb is imported)
            if (outFieldIndex != (unsigned)-1)
            {
                auto* streamTy = compiler->GetDataStructure("stream").StructType;
                auto* nullStream = llvm::Constant::getNullValue(streamTy->getPointerTo());
                structVal = compiler->CreateInsertValue(structVal, nullStream, outFieldIndex);
                structVal = compiler->CreateInsertValue(structVal, nullStream, inStreamFieldIndex);
            }

            // Allocate a non-null arena_channel shell: send/recv are no-ops on the uninitialized shell,
            // but a non-null `this` prevents crashes if recv/send is called before `>>` wires the channel.
            if (inboxArenaFieldIndex != (unsigned)-1)
            {
                auto* arenaTy = compiler->GetDataStructure(kArenaChannelType).StructType;
                llvm::Value* shell = EmitArenaChannelShellAlloc(compiler);
                if (!shell) shell = llvm::Constant::getNullValue(arenaTy->getPointerTo());
                structVal = compiler->CreateInsertValue(structVal, shell, inboxArenaFieldIndex);
                structVal = compiler->CreateInsertValue(structVal, shell, outboxFieldIndex);
            }

            compiler->CreateReturnCall(structVal);
            compiler->CreateBlockBreak(nullptr, true);
        }

        // Register class fields in LSP index for dot-completion
        if (auto* s = compiler->GetSymbolSink())
        {
            auto sd = compiler->GetDataStructure(name);
            for (const auto& field : sd.StructFields)
            {
                if (field.VariableName.empty() || field.IsPadding) continue;
                std::string annSig;
                for (const auto& ann : field.Annotations)
                {
                    annSig += "[" + ann.Name;
                    if (!ann.Value.empty()) annSig += "(" + ann.Value + ")";
                    annSig += "] ";
                }
                std::string typeSig = field.TypeName;
                if (field.Pointer) typeSig += "*";
                if (field.ElemPointer) typeSig += "*";
                s->Register(SymbolKind::Field, name + "." + field.VariableName,
                            compiler->GetSourceFilePath(),
                            (int)ctx->getStart()->getLine(),
                            (int)ctx->getStart()->getCharPositionInLine(),
                            annSig + typeSig + " " + name + "." + field.VariableName);
            }
        }

        // Parse member functions (includes user's main)
        {
            GlobalScopeGuard scopeGuard(global_scope);
            for (auto func : ctx->functionDefinition())
            {
                global_scope = false;
                // Same rejection as the scanner - a constructor is ONLY a function with
                // no declarationSpecifiers (a same-named method is NOT one).
                if (func->declarationSpecifiers() == nullptr && getFunctionName(func) == name)
                    Compiler(func)->LogError(std::format(
                        "program '{}' does not support a user-defined constructor", name));
                else
                    ParseFunctionDefinition(func, name);
            }
        }

        // Set programTable field indices BEFORE parsing the destructor: a user ~Name()
        // is emitted as one destructor with the builtin field teardown appended at the
        // end (ParseProgramDestructorDefinition -> EmitProgramSyntheticTeardown), and the
        // teardown reads these indices.
        compiler->programTable[name].StructType          = structType;
        compiler->programTable[name].ConfigFields        = declList;
        compiler->programTable[name].ExitCodeFieldIndex  = exitCodeFieldIndex;
        compiler->programTable[name].ThreadFieldIndex    = threadFieldIndex;
        compiler->programTable[name].AllocatorFieldIndex = allocatorFieldIndex;
        compiler->programTable[name].OnStdoutFieldIndex       = onStdoutFieldIndex;
        compiler->programTable[name].OnStdinFieldIndex        = onStdinFieldIndex;
        compiler->programTable[name].OnStdinReturnFieldIndex  = onStdinReturnFieldIndex;
        compiler->programTable[name].StopSourceFieldIndex    = stopSrcFieldIndex;
        compiler->programTable[name].TrackHandlesFieldIndex  = trackHandlesFieldIndex;
        compiler->programTable[name].UseChannelFieldIndex    = useChannelFieldIndex;
        compiler->programTable[name].FpConfigFieldIndex      = fpConfigFieldIndex;
        compiler->programTable[name].OutFieldIndex           = outFieldIndex;
        compiler->programTable[name].InStreamFieldIndex      = inStreamFieldIndex;
        compiler->programTable[name].InboxArenaFieldIndex    = inboxArenaFieldIndex;
        compiler->programTable[name].OutboxFieldIndex        = outboxFieldIndex;

        // A `program X : IFace` implements the interface's fields out of its config fields
        // (user fields come first, synthetics are appended), so verify them here - eagerly,
        // at the program definition, exactly as a class does.
        for (const auto& iface : compiler->programTable[name].Interfaces)
            compiler->VerifyInterfaceFields(name, iface, compiler->programTable[name].ConfigFields);

        // Parse the user destructor if present (at most one). Emitted as a single
        // compiler-owned ~Name() whose user body runs first, then the builtin teardown.
        {
            GlobalScopeGuard scopeGuard(global_scope);
            for (auto dtor : ctx->destructorDefinition())
            {
                global_scope = false;
                ParseProgramDestructorDefinition(dtor, name);
            }
        }

        // Flush instantiations (e.g. list__string from main's params) before emitting run()
        ProcessPendingInstantiations();

        // Emit auto-generated run(), WaitForExit(), and __program_run_Name trampoline
        EmitProgramRunWrapper(name, ctx);
    }

void MainListener::ParseClassDefinition(CFlatParser::ClassDefinitionContext* ctx, const std::string& nameOverride, const std::string& namespaceName) {
        ResolvedMembersScope memberScope_(resolvedMembers_, (const void*)ctx);
        auto* compiler = Compiler(ctx);
        auto decl = ctx->directDeclarator();
        std::string baseName = decl->getText();
        std::string structName;

        // Apply nameOverride first (for generic instantiations), then namespace
        if (!nameOverride.empty())
        {
            structName = nameOverride;
        }
        else if (!namespaceName.empty())
        {
            structName = namespaceName + "." + baseName;
        }
        else
        {
            structName = baseName;
        }

        // If this is a generic template definition (not an instantiation), store it and return.
        if (nameOverride.empty() && ctx->genericTypeParameters() != nullptr)
        {
            if (Compiler()->gts.scannedGenericInterfaceNames.count(structName) != 0
                || genericInterfaceTemplates.count(structName) != 0)
                LogErrorContext(ctx, std::format(
                    "generic class '{}' conflicts with a generic interface of the same name",
                    structName));
            auto typeParams = ParseGenericTypeParameters(ctx->genericTypeParameters());
            genericClassTemplates[structName] = ctx;
            Compiler()->gts.genericTemplateNamespace[structName] = Compiler()->GetCurrentNamespace();
            Compiler()->RevokeGenericInterfaceInstances(structName);
            genericStructTypeParams[structName] = typeParams;
            genericClassConstraints[structName] = ParseWhereClause(ctx->whereClause());
            return;
        }

        // Validate type-level annotations against the registry (errors on unknown) and keep them
        // for storage on the StructData below. winrt/uuid are ordinary annotations from com.cb.
        auto classAnnotations = ParseAnnotationList(ctx->annotationList());

        // [winrt] class: lower to a thin COM object (vtable ptr + refcount + fields) instead of
        // the fat-ptr interface path. Exactly one interface is supported in this milestone. The
        // vtable struct is created up front so the injected lpVtbl field type resolves.
        bool isWinrt = std::any_of(classAnnotations.begin(), classAnnotations.end(),
                                   [](const auto& a) { return a.Name == "winrt"; });
        std::string winrtIface;
        std::string winrtVtblName;
        if (isWinrt)
        {
            auto ids = ctx->baseSpecifier();
            if (ids.empty() || BaseSpecifierName(ids[0]).empty())
            {
                LogErrorContext(ctx, "[winrt] class '" + structName + "' must implement exactly one interface");
                return;
            }
            if (ids.size() > 1)
            {
                LogErrorContext(ctx, "[winrt] class '" + structName + "' may implement only one interface in this milestone");
                return;
            }
            // Passed through verbatim: a WinMD projected interface is registered under its own
            // fully qualified spelling and must not go through CFlat namespace resolution.
            winrtIface = BaseSpecifierName(ids[0]);
            winrtVtblName = compiler->CreateWinrtVtableStruct(structName, winrtIface);

            // The value-returning member-call sugar `recv->Method()` produces an HResult<T>, a
            // type never spelled in user source - so prime its instantiation here (we have the
            // method return types) and record the mangled name for EmitWinrtSlotCall to build.
            if (const auto* methods = compiler->FindInterface(winrtIface))
                for (const auto& m : *methods)
                {
                    if (m.ReturnType.TypeName == "void" && !m.ReturnType.Pointer) continue;
                    std::string arg = m.ReturnType.TypeName + (m.ReturnType.Pointer ? "*" : "");
                    std::string mangled = MangledGenericName("HResult", { arg });
                    compiler->winrtSlotHResultType_[structName + "::" + m.Name] = mangled;
                    if (!instantiatedGenerics.count(mangled))
                    {
                        pendingInstantiations.push_back({ "HResult", { arg }, mangled });
                        instantiatedGenerics.insert(mangled);
                    }
                }
            ProcessPendingInstantiations();
        }

        // Re-emission guard: if this struct was already fully emitted via a transitive import,
        // CreateFunctionDefinition's duplicate-skip leaves the builder out of scope - skip the walk.
        {
            auto sd = compiler->GetDataStructure(structName);
            if (sd.StructType != nullptr && !sd.StructType->isOpaque())
            {
                if (auto* existing = compiler->GetFunction(structName);
                    existing != nullptr && !existing->empty())
                {
                    if (compiler->IsVerbose())
                        std::cout << "[verbose]     skipping duplicate struct definition: " << structName << "\n";
                    return;
                }
            }
        }

        if (compiler->IsVerbose())
            std::cout << "[verbose]     parse decl list: " << structName << "\n";

        // Process nested struct/class definitions before fields so their types are available
        for (auto* nestedStruct : MemberStructDefinitions(ctx))
            ParseStructDefinition(nestedStruct, {}, structName);
        for (auto* nestedClass : MemberClassDefinitions(ctx))
            ParseClassDefinition(nestedClass, {}, structName);

        // Push scope so unqualified nested type names resolve (e.g. Inner -> Outer.Inner)
        structScopeStack.push_back(structName);

        auto declarationList = MemberDeclarations(ctx);
        std::vector<llvm::Type*> types;

        // Queue and instantiate generic types used in field declarations before
        // ParseDeclarationList resolves them to LLVM types. Only needed at top-level
        // (non-template) scope; template instantiations already have activeTypeSubstitutions
        // or activePackSubstitutions set and their generics are queued via ParseDeclarationSpecifiers.
        if (activeTypeSubstitutions.empty() && activePackSubstitutions.empty())
        {
            for (auto decl : declarationList)
                ScanAndQueueGenericTypeUses(decl);
            ProcessPendingInstantiations();
        }

        // Build field list, expanding pack fields (T... fieldName -> fieldName_0, fieldName_1, ...)
        std::vector<LLVMBackend::DeclTypeAndValue> declList;
        auto rejectFixedArrayMemberPrototype = [&](CFlatParser::DeclarationContext* decl) {
            auto* specs = decl->declarationSpecifiers();
            if (specs == nullptr || decl->initDeclaratorList() == nullptr) return;
            std::string element;
            for (auto* spec : specs->declarationSpecifier())
            {
                if (auto* dims = ArrayDimsOf(spec); dims != nullptr && !dims->assignmentExpression().empty())
                {
                    if (spec->typeSpecifier() != nullptr) element = spec->typeSpecifier()->getText();
                    break;
                }
            }
            if (element.empty()) return;
            for (auto* init : decl->initDeclaratorList()->initDeclarator())
            {
                auto* declarator = init->declarator();
                if (declarator == nullptr
                    || (declarator->parameterTypeList() == nullptr && declarator->children.size() <= 1)) continue;
                LogErrorContext(decl, std::format(
                    "member '{}' cannot return the fixed array '{}[N]' by value; return a struct with the array as a field or take an out-parameter",
                    declarator->directDeclarator()->getText(), element));
            }
        };
        for (auto* decl : declarationList)
        {
            rejectFixedArrayMemberPrototype(decl);
            std::string packParamName;
            if (decl->declarationSpecifiers())
            {
                for (auto* ds : decl->declarationSpecifiers()->declarationSpecifier())
                {
                    auto* ts = ds->typeSpecifier();
                    if (!ts || !ts->genericIdentifier() || ts->genericIdentifier()->genericTypeParameters()) continue;
                    auto* gid = ts->genericIdentifier();
                    if (!gid->Identifier()) continue;
                    std::string n = gid->Identifier()->getText();
                    if (activePackSubstitutions.count(n)) { packParamName = n; break; }
                }
            }

            if (packParamName.empty())
            {
                for (auto& f : ParseDeclarationList({decl}))
                    declList.push_back(f);
                continue;
            }

            std::string baseFieldName;
            if (auto* idl = decl->initDeclaratorList())
                if (!idl->initDeclarator().empty())
                    if (auto* d = idl->initDeclarator()[0]->declarator())
                        if (auto* dd = d->directDeclarator())
                            baseFieldName = getDirectDeclName(dd);

            auto& packTypes = activePackSubstitutions.at(packParamName);
            auto savedPackItemSubst = activeTypeSubstitutions;
            for (size_t i = 0; i < packTypes.size(); i++)
            {
                activeTypeSubstitutions[packParamName] = packTypes[i];
                auto expanded = ParseDeclarationList({decl});
                for (auto& f : expanded)
                {
                    f.VariableName = baseFieldName + "_" + std::to_string(i);
                    declList.push_back(f);
                }
            }
            activeTypeSubstitutions = savedPackItemSubst;
        }

        // Process lock field groups: each group annotates its fields with GuardedBy.
        for (auto* lfg : MemberLockFieldGroups(ctx))
        {
            if (activeTypeSubstitutions.empty() && activePackSubstitutions.empty())
                ScanAndQueueGenericTypeUses(lfg);

            auto groupArgs = lfg->lockClause()->lockArgList()->expression();
            if (groupArgs.empty()) continue;
            std::string guardianName = GetLockArgCanonical(groupArgs[0]);

            for (auto* decl : lfg->declaration())
            {
                for (auto& f : ParseDeclarationList({decl}))
                {
                    f.GuardedBy = guardianName;
                    declList.push_back(f);
                }
            }
        }

        // Prepend the COM header fields (vtable pointer @0, refcount @1) so the user fields
        // follow. `new` wires lpVtbl/refcount after the constructor; both are zero meanwhile.
        if (isWinrt)
        {
            LLVMBackend::DeclTypeAndValue lpVtbl;
            lpVtbl.VariableName = "lpVtbl";
            lpVtbl.TypeName = winrtVtblName;
            lpVtbl.Pointer = true;
            LLVMBackend::DeclTypeAndValue refcount;
            refcount.VariableName = "__refcount";
            refcount.TypeName = "u32";
            declList.insert(declList.begin(), refcount);
            declList.insert(declList.begin(), lpVtbl);
        }

        if (compiler->IsVerbose())
            std::cout << "[verbose]     decl list has " << declList.size() << " fields\n";

        if (compiler->IsVerbose())
            std::cout << "[verbose]     create struct type: " << structName << "\n";
        // Capture `class alignas(N) Foo { ... }` before layout so trailing
        // padding can be inserted atomically by CreateStructType.
        uint64_t userAlign = 0;
        if (auto* alignSpec = ctx->alignmentSpecifier())
            userAlign = ParseAlignmentSpecifier(alignSpec);
        // `alignas(N)` on a MEMBER: synthetic `__padN` slots align the member's slot, and the
        // strictest member alignment becomes the class's own (raising alignof and sizeof).
        uint64_t fieldAlign = 0;
        declList = compiler->PadFieldsForAlignment(declList, fieldAlign);
        if (fieldAlign > userAlign) userAlign = fieldAlign;
        auto structType = compiler->CreateStructType(structName, declList, userAlign);
        // A class with zero fields still needs a sized (non-opaque) type
        // so that alloca/sizeof work correctly (e.g. when passed via interface).
        if (structType->isOpaque())
            structType->setBody(llvm::ArrayRef<llvm::Type*>());
        // Record validated type-level annotations for annotationof(Type,"Ann") queries.
        compiler->SetTypeAnnotations(structName, classAnnotations);
        if (compiler->IsVerbose())
            std::cout << "[verbose]     create default ctor: " << structName << "\n";
        LLVMBackend::TypeAndValue returnType{
            .TypeName = structName,
        };
        // Member functions of this class. Pre-declare their signatures (for
        // instantiations) BEFORE the dependency flush below, so a sibling
        // instantiation pulled in by the flush can resolve calls back into this
        // type's methods to a forward declaration. See PreDeclareInstantiationMembers.
        auto functionList = MemberFunctionDefinitions(ctx);
        if (!nameOverride.empty())
            PreDeclareInstantiationMembers(compiler, functionList, baseName, structName, returnType);
        // Flush any nested generic instantiations queued while parsing field declarations,
        // so their constructors exist before this class's default constructor calls them.
        {
            auto savedSubst = activeTypeSubstitutions;
            ProcessPendingInstantiations();
            activeTypeSubstitutions = savedSubst;
        }
        // If the user wrote an explicit no-arg constructor, skip the auto-generated one.
        bool hasBareNoArgCtor = [&]() {
            for (auto* f : MemberFunctionDefinitions(ctx))
                if (!FunctionDeclaresReturnType(f) && getFunctionName(f) == baseName && !f->parameterTypeList())
                    return true;
            return false;
        }();
        // An all-defaulted ctor is ALSO a no-arg ctor: its cutoff-0 wrapper claims the same
        // symbol, so emitting the synthetic one too collides (see AllParametersDefaulted).
        bool hasAllDefaultedCtor = !hasBareNoArgCtor && [&]() {
            for (auto* f : MemberFunctionDefinitions(ctx))
                if (!FunctionDeclaresReturnType(f) && getFunctionName(f) == baseName && AllParametersDefaulted(f->parameterTypeList()))
                    return true;
            return false;
        }();
        bool hasExplicitNoArgCtor = hasBareNoArgCtor || hasAllDefaultedCtor;

        // Create default constructor (skipped when user provides an explicit no-arg ctor)
        if (!hasExplicitNoArgCtor)
        {
            auto funcDef = compiler->CreateFunctionDefinition(structName, returnType, {});

            std::vector<llvm::Value*> initializers;
            for (auto& typeValue : declList)
            {
                auto initializer = typeValue.Initializer;
                llvm::Value* rvalue = nullptr;
                if (auto* braceList = FieldDefaultBraceList(typeValue))
                {
                    // Emitting a real function body; clear the stale file-scope global_scope
                    // so the brace list's stores and calls lower as ordinary instructions.
                    GlobalScopeGuard defaultCtorScope(global_scope);
                    rvalue = ParseFieldDefaultBraceInitializer(structName, typeValue, braceList);
                }
                else if (initializer != nullptr)
                {
                    auto assignmentExpression = initializer->assignmentExpression();
                    if (assignmentExpression != nullptr)
                    {
                        rvalue = ParseFieldDefaultInitializer(
                            structName, typeValue, assignmentExpression);
                        if (typeValue.TypeName == "auto")
                        {
                            typeValue.TypeName = rvalue->getType()->getStructName();
                            structType = compiler->CreateStructType(structName, declList);
                        }
                    }
                    else if (initializer->Default() != nullptr)
                    {
                        // Synthetic default-ctor body: clear the stale file-scope global_scope so a
                        // struct-typed field's `= default` runs that field's own default constructor
                        // (its field initializers) rather than zero-filling. See GenerateDefaultValue.
                        GlobalScopeGuard defaultCtorScope(global_scope);
                        rvalue = GenerateDefaultValue(typeValue);
                    }
                }
                initializers.push_back(rvalue);
            }

            // Seed with zero (not undef) so fields lacking an explicit initializer read as
            // 0/null instead of leaking stack garbage. Mirrors the struct default ctor above.
            llvm::Value* structVal = llvm::Constant::getNullValue(structType);

            LLVMBackend::TypeAndValue myStruct;
            myStruct.TypeName = structName;
            myStruct.VariableName = "_" + structName;

            unsigned int structIndex = 0;

            for (auto rvalue : initializers)
            {
                auto* destType = structType->getTypeAtIndex(structIndex);
                // No explicit initializer on a struct-typed field - call its default ctor.
                if (rvalue == nullptr && (destType->isStructTy() || destType->isArrayTy()))
                {
                    std::string fieldTypeName = declList[structIndex].TypeName;
                    // forceRoot: the GetFunction guard is an exact-key lookup, so a namespace walk
                    // here would call a same-named sibling type's ctor (layer 3).
                    if (destType->isArrayTy())
                        rvalue = GenerateDefaultValue(declList[structIndex]);
                    else if (compiler->GetFunction(fieldTypeName))
                        rvalue = compiler->CreateOverloadedFunctionCall(fieldTypeName, {}, true);
                    else
                        rvalue = llvm::Constant::getNullValue(destType);
                }
                if (rvalue != nullptr)
                {
                    rvalue = compiler->Upconvert(rvalue, destType);
                    if (rvalue->getType() != destType)
                    {
                        if (destType->isStructTy())
                        {
                            std::string fieldTypeName = declList[structIndex].TypeName;
                            // forceRoot: the GetFunction guard is an exact-key lookup, so a namespace walk
                            // here would call a same-named sibling type's ctor (layer 3).
                            if (compiler->GetFunction(fieldTypeName))
                                rvalue = compiler->CreateOverloadedFunctionCall(fieldTypeName, {}, true);
                            else
                                rvalue = llvm::Constant::getNullValue(destType);
                        }
                        else
                        {
                            // Narrowing field initializer (e.g. u8 r = 255 has i32 literal).
                            if (ShouldWarnImplicitFieldNarrowing(
                                    rvalue, destType, declList[structIndex].TypeName))
                                compiler->LogWarning(std::format(
                                    "implicit narrowing to '{}' in field '{}' - use an explicit cast",
                                    declList[structIndex].TypeName,
                                    declList[structIndex].VariableName));
                            rvalue = compiler->CreateCast(rvalue, destType);
                        }
                    }
                    structVal = compiler->CreateInsertValue(structVal, rvalue, structIndex);
                }

                structIndex++;
            }

            compiler->CreateReturnCall(structVal);
            compiler->CreateBlockBreak(nullptr, true);
        } // end if (!hasExplicitNoArgCtor)

        // Register class fields in LSP index for dot-completion
        if (auto* s = compiler->GetSymbolSink())
        {
            auto sd = compiler->GetDataStructure(structName);
            for (const auto& field : sd.StructFields)
            {
                if (field.VariableName.empty() || field.IsPadding) continue;
                std::string annSig;
                for (const auto& ann : field.Annotations)
                {
                    annSig += "[" + ann.Name;
                    if (!ann.Value.empty()) annSig += "(" + ann.Value + ")";
                    annSig += "] ";
                }
                std::string typeSig = field.TypeName;
                if (field.Pointer) typeSig += "*";
                if (field.ElemPointer) typeSig += "*";
                s->Register(SymbolKind::Field, structName + "." + field.VariableName,
                            compiler->GetSourceFilePath(),
                            (int)ctx->getStart()->getLine(),
                            (int)ctx->getStart()->getCharPositionInLine(),
                            annSig + typeSig + " " + structName + "." + field.VariableName);
            }
        }

        // Member function signatures were pre-declared above (before the flush).

        // Pre-register destructor so 'delete' inside static methods can call it, AND so a member
        // that constructs an instance of its own type (e.g. dictionary.copy() building a local
        // dictionary) forces .dtorfull with the user destructor already resolved. The scanner only
        // forward-declares the TEMPLATE's `~name`; a concrete instantiation's `~name__T` is not, so
        // declare it here when missing - otherwise .dtorfull bakes a null user-dtor and caches it,
        // leaking everything the hand-written destructor would have freed.
        if (!MemberDestructorDefinitions(ctx).empty())
        {
            llvm::Function* dtorFn = compiler->GetFunction("~" + structName);
            if (dtorFn == nullptr)
            {
                LLVMBackend::DeclTypeAndValue thisParam;
                thisParam.TypeName = structName;
                thisParam.VariableName = structName + "__";
                thisParam.Pointer = true;
                LLVMBackend::TypeAndValue voidReturn{ .TypeName = "void" };
                compiler->CreateFunctionDeclaration("~" + structName, voidReturn, { thisParam });
                dtorFn = compiler->GetFunction("~" + structName);
            }
            if (dtorFn != nullptr)
                compiler->RegisterDestructor(structName, dtorFn);
        }

        // Pre-register interfaces before method bodies so StructImplementsInterface() returns true
        // for assignments inside the class itself (e.g. `IJSON result = this;` inside a class : IJSON).
        auto resolveImplsTypeArgs = [&](CFlatParser::GenericTypeParametersContext* gtp) -> std::vector<std::string>
        {
            std::vector<std::string> args;
            for (auto* entry : gtp->typeParameterList()->typeParameterEntry())
            {
                if (entry->Ellipsis() != nullptr)
                {
                    // Pack: expand T... -> [int, float, ...] via the pack substitution (keyed by the
                    // bare parameter name). A non-substituted element keeps its `*`/`unique` suffix.
                    std::string name = entry->typeSpecifier() ? entry->typeSpecifier()->getText() : entry->getText();
                    auto packIt = activePackSubstitutions.find(name);
                    if (packIt != activePackSubstitutions.end())
                        for (const auto& t : packIt->second)
                            args.push_back(t);
                    else
                        args.push_back(ResolveTypeArgEntry(entry));
                }
                else
                {
                    // Reconstruct the full type-arg spelling (element + `*`/`[]` + `unique`/`alias`)
                    // via the canonical path; the bare typeSpecifier text dropped the declarator
                    // suffix and `unique` on the explicit base-clause form (class PB : IB<R*>).
                    args.push_back(ResolveTypeArgEntry(entry));
                }
            }
            return args;
        };

        {
            std::vector<std::string> earlyIfaceNames;
            for (auto* spec : ctx->baseSpecifier())
            {
                std::string ifaceBaseName = BaseSpecifierName(spec);
                if (ifaceBaseName.empty()) continue;
                std::string ifaceName = ifaceBaseName;
                if (spec->genericTypeParameters() != nullptr)
                {
                    // A base clause names the TEMPLATE, so the spelling resolves through the
                    // generic key space (a bare 'IV<T>' inside namespace NS means NS.IV).
                    auto concreteTypeArgs = resolveImplsTypeArgs(spec->genericTypeParameters());
                    ifaceName = MangledGenericName(compiler->ResolveGenericBaseAlias(ifaceBaseName),
                                                   concreteTypeArgs);
                }
                else
                {
                    ifaceName = compiler->ResolveInterfaceName(ifaceBaseName);
                }
                earlyIfaceNames.push_back(ifaceName);
            }
            if (!earlyIfaceNames.empty() && !isWinrt)
                compiler->RegisterStructInterfaces(structName, earlyIfaceNames);
        }

        {
            GlobalScopeGuard scopeGuard(global_scope);
            for (auto func : functionList)
            {
                global_scope = false;
                std::string funcName = getFunctionName(func);
                if (compiler->IsVerbose())
                    std::cout << "[verbose]     parse member: " << structName << "." << funcName << "\n";
                // Constructor - same name as class (no-arg or with parameters)
                if (!FunctionDeclaresReturnType(func) && funcName == baseName)
                {
                    // This ctor IS the type's no-arg ctor when every parameter is defaulted and
                    // no bare 'T()' was written - it must seed fields itself, not self-delegate.
                    bool suppliesNoArgCtor = !hasBareNoArgCtor
                        && AllParametersDefaulted(func->parameterTypeList());
                    ParseConstructorDefinition(func, structName, suppliesNoArgCtor);
                    continue;
                }
                // A generic member method - static or instance - is stored as a template keyed by
                // its owner ("Owner.method"). InstantiateGenericFunction re-derives the owner from
                // that key and emits an instance method with its implicit `this` parameter, so the
                // monomorphized body resolves the owner's fields like any other member.
                if (func->genericTypeParameters() != nullptr)
                {
                    std::string qualifiedName = structName + "." + funcName;
                    genericFunctionTemplates[qualifiedName] = func;
                    // Declaring NAMESPACE of the owner, recorded not derived: the key's last dot
                    // separates the owner, not a namespace, so only this tells the two apart.
                    Compiler()->gts.genericTemplateNamespace[qualifiedName] = Compiler()->GetCurrentNamespace();
                    genericFunctionTypeParams[qualifiedName] = ParseGenericTypeParameters(func->genericTypeParameters());
                    genericFunctionConstraints[qualifiedName] = ParseWhereClause(func->whereClause());
                }
                else if (funcName == "operator new" || funcName == "operator delete" || isFunctionStatic(func))
                {
                    ParseFunctionDefinition(func, {}, {}, structName + "." + funcName);
                }
                else
                    ParseFunctionDefinition(func, structName);
            }
        }

        // Parse functions declared inside positional lock groups.
        {
            GlobalScopeGuard scopeGuard(global_scope);
            for (auto* lfg : MemberLockFieldGroups(ctx))
            {
                for (auto* func : lfg->functionDefinition())
                {
                    global_scope = false;
                    // Same rejection as the scanner - a constructor is ONLY a function
                    // with no declarationSpecifiers (a same-named method is NOT one).
                    if (func->declarationSpecifiers() == nullptr && getFunctionName(func) == baseName)
                        Compiler(func)->LogError(std::format(
                            "constructor '{}' is not allowed inside a lock field group", baseName));
                    else
                        ParseFunctionDefinition(func, structName);
                }
            }
        }

        // Parse destructor
        {
            GlobalScopeGuard scopeGuard(global_scope);
            for (auto dtor : MemberDestructorDefinitions(ctx))
            {
                global_scope = false;
                ParseDestructorDefinition(dtor, structName);
            }
        }

        // Record interfaces and verify implementations
        std::vector<std::string> ifaceNames;
        for (auto* spec : ctx->baseSpecifier())
        {
            std::string ifaceBaseName = BaseSpecifierName(spec);
            if (ifaceBaseName.empty()) continue;
            std::string ifaceName = compiler->ResolveInterfaceName(ifaceBaseName);

            if (spec->genericTypeParameters() != nullptr)
            {
                ifaceBaseName = compiler->ResolveGenericBaseAlias(ifaceBaseName);
                auto concreteTypeArgs = resolveImplsTypeArgs(spec->genericTypeParameters());
                ifaceName = MangledGenericName(ifaceBaseName, concreteTypeArgs);

                // Build substitution maps for the interface template's type params (pack-aware)
                std::unordered_map<std::string, std::string> ifaceSubstitutions;
                std::unordered_map<std::string, std::vector<std::string>> ifacePackSubstitutions;
                const auto& ifaceTypeParams = genericInterfaceTypeParams[ifaceBaseName];
                auto ifacePackIdxIt = genericInterfacePackIndex.find(ifaceBaseName);
                size_t ifacePackIdx = (ifacePackIdxIt != genericInterfacePackIndex.end())
                                      ? ifacePackIdxIt->second : std::string::npos;
                if (ifacePackIdx == std::string::npos)
                {
                    for (size_t i = 0; i < ifaceTypeParams.size() && i < concreteTypeArgs.size(); i++)
                        ifaceSubstitutions[ifaceTypeParams[i]] = concreteTypeArgs[i];
                }
                else
                {
                    for (size_t i = 0; i < ifacePackIdx && i < concreteTypeArgs.size(); i++)
                        ifaceSubstitutions[ifaceTypeParams[i]] = concreteTypeArgs[i];
                    ifacePackSubstitutions[ifaceTypeParams[ifacePackIdx]] =
                        std::vector<std::string>(concreteTypeArgs.begin() + ifacePackIdx, concreteTypeArgs.end());
                }

                InstantiateGenericInterface(ifaceBaseName, ifaceName, ifaceSubstitutions, ifacePackSubstitutions);
            }

            ifaceNames.push_back(ifaceName);
        }
        // A [winrt] class emits COM runtime functions + the static vtable instead of registering
        // a fat-ptr interface vtable. Runs after member functions/dtor so thunks can find them.
        if (isWinrt)
        {
            compiler->EmitWinrtRuntime(structName, winrtIface, winrtVtblName);
        }
        else
        {
            compiler->RegisterStructInterfaces(structName, ifaceNames);
            for (const auto& interfaceName : ifaceNames)
                compiler->VerifyInterfaceImplementation(structName, interfaceName);
        }

        // Process any generic instantiations that were queued during this class definition
        // ProcessPendingInstantiations();

        structScopeStack.pop_back();
    }

std::vector<std::string> MainListener::ParseGenericTypeParameters(CFlatParser::GenericTypeParametersContext* genericParams) {
        std::vector<std::string> typeParams;
        if (!genericParams)
            return typeParams;

        auto typeParamList = genericParams->typeParameterList();
        if (!typeParamList)
            return typeParams;

        bool seenPack = false;
        for (auto* entry : typeParamList->typeParameterEntry())
        {
            auto* typeSpec = entry->typeSpecifier();
            // Generic type parameters must be simple identifiers, not built-in types
            if (!typeSpec || !typeSpec->genericIdentifier() || !typeSpec->genericIdentifier()->Identifier())
            {
                LogErrorContext(entry, "Generic type parameter must be an identifier, not a built-in type");
                continue;
            }
            if (seenPack)
            {
                LogErrorContext(entry, "Only the last type parameter may be a pack (T...)");
                continue;
            }
            typeParams.push_back(typeSpec->genericIdentifier()->Identifier()->getText());
            if (entry->Ellipsis() != nullptr)
                seenPack = true;
        }

        return typeParams;
    }

std::unordered_map<std::string, std::vector<std::string>>
    MainListener::ParseWhereClause(CFlatParser::WhereClauseContext* wc) {
        std::unordered_map<std::string, std::vector<std::string>> result;
        if (!wc) return result;
        for (auto* constraint : wc->typeParameterConstraint())
        {
            auto ids = constraint->Identifier();
            if (ids.size() < 2) continue;
            result[ids[0]->getText()].push_back(ids[1]->getText());
        }
        return result;
    }

bool MainListener::CheckConstraints(
        const std::string& templateName,
        const std::vector<std::string>& typeParams,
        const std::vector<std::string>& typeArgs,
        const std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::string>>>& constraintMap,
        antlr4::ParserRuleContext* ctx) {
        auto cit = constraintMap.find(templateName);
        if (cit == constraintMap.end()) return true;
        for (size_t i = 0; i < typeParams.size() && i < typeArgs.size(); i++)
        {
            auto pit = cit->second.find(typeParams[i]);
            if (pit == cit->second.end()) continue;
            for (const auto& iface : pit->second)
            {
                if (!Compiler(ctx)->TypeImplementsInterface(typeArgs[i], iface))
                {
                    Compiler(ctx)->LogError(std::format(
                        "type '{}' does not implement '{}', required by constraint 'where {} : {}'",
                        typeArgs[i], iface, typeParams[i], iface));
                    return false;
                }
            }
        }
        return true;
    }

void MainListener::ParseConstructorDefinition(CFlatParser::FunctionDefinitionContext* func, const std::string& structName, bool suppliesNoArgCtor) {
        auto* compiler = Compiler(func);
        auto params = ParseParameterTypeList(func->parameterTypeList());
        size_t line = func->getStart()->getLine();
        bool varargs = func->parameterTypeList() && func->parameterTypeList()->Ellipsis() != nullptr;

        // Pre-scan body for generic instantiations (same as ParseFunctionDefinition)
        if (auto* blockItemList = func->compoundStatement()->blockItemList())
        {
            ScanAndQueueGenericTypeUses(blockItemList);
            ProcessPendingInstantiations();
        }

        LLVMBackend::DeclTypeAndValue returnType;
        returnType.TypeName = structName;
        std::vector<LLVMBackend::TypeAndValue> allParams(params.begin(), params.end());

        // Open constructor function - no this* parameter; returns the struct by value
        auto fn = compiler->CreateFunctionDefinition(structName, returnType, allParams, false, varargs, line);

        // CreateFunctionDefinition took its !fn->empty() early return, so no function scope was
        // pushed. Continuing would index one. Same guard as ParseFunctionDefinition.
        if (!fn->empty() && fn->getEntryBlock().getTerminator() != nullptr)
            return;

        compiler->InitializeBlock(&fn->front(), false);
        // Fresh straight-line for this function/lambda body; restore the enclosing walk's flag on
        // exit so a nested lambda's return does not leak into the surrounding expression.
        ReturnFlagGuard functionReturnFlagGuard(&straightLineReturned_);
        straightLineReturned_ = false;

        // Get the struct's LLVM type (without pointer)
        auto* structLLVMType = llvm::cast<llvm::StructType>(compiler->GetType(returnType, nullptr, false));

        // Alloca the struct so we can GEP into fields via 'this'
        auto* thisAlloca = compiler->AllocaAtEntry(structLLVMType, nullptr, structName + "__");

        // suppliesNoArgCtor: this ctor's own cutoff-0 wrapper IS structName(), so delegating to
        // it would be self-recursive exactly as in the bare no-arg case. Seed fields in line.
        if (allParams.empty() || suppliesNoArgCtor)
        {
            // No-arg constructor: calling structName() would be self-recursive.
            // Zero-initialize the struct, then apply field defaults from the data structure.
            compiler->builder->CreateStore(llvm::Constant::getNullValue(structLLVMType), thisAlloca);
            auto structData = compiler->GetDataStructure(structName);
            unsigned fieldIdx = 0;
            // Every field-default form the synthesized default constructors seed must be seeded
            // here too, or a user-written no-arg ctor silently leaves fields zeroed (see :271).
            for (const auto& field : structData.StructFields)
            {
                // A union's members all alias one storage slot, so StructFields outnumbers the
                // LLVM elements; indexing past the end would be out of range.
                if (fieldIdx >= structLLVMType->getNumElements())
                    break;
                auto* destType = structLLVMType->getTypeAtIndex(fieldIdx);
                llvm::Value* fieldVal = nullptr;
                bool fromBraceList = false;
                if (auto* braceList = FieldDefaultBraceList(field))
                {
                    // Same brace-list field default the synthesized ctors honour; a user-written
                    // no-arg ctor must seed its fields identically before its own body runs.
                    fieldVal = ParseFieldDefaultBraceInitializer(structName, field, braceList);
                    fromBraceList = true;
                }
                else if (field.Initializer != nullptr)
                {
                    auto* assignExpr = field.Initializer->assignmentExpression();
                    if (assignExpr != nullptr)
                    {
                        fieldVal = ParseFieldDefaultInitializer(structName, field, assignExpr);
                    }
                    else if (field.Initializer->Default() != nullptr)
                    {
                        // `= default` on a struct-typed field runs that field type's own default
                        // constructor (its field initializers), exactly as the synthetic ctor does.
                        fieldVal = GenerateDefaultValue(field);
                    }
                }
                // No initializer at all on a struct-typed field - call its default ctor, matching
                // the synthetic default-ctor path.
                if (fieldVal == nullptr && (destType->isStructTy() || destType->isArrayTy()))
                {
                    // forceRoot: the GetFunction guard is an exact-key lookup, so a namespace walk
                    // here would call a same-named sibling type's ctor (layer 3).
                    if (destType->isArrayTy())
                        fieldVal = GenerateDefaultValue(field);
                    else if (compiler->GetFunction(field.TypeName))
                        fieldVal = compiler->CreateOverloadedFunctionCall(field.TypeName, {}, true);
                }
                if (fieldVal != nullptr)
                {
                    if (!fromBraceList)
                        fieldVal = compiler->Upconvert(fieldVal, destType);
                    // Same type-mismatch arms the synthetic default ctor runs (:342-369); without
                    // them the store is dropped and the field keeps the seeding zero.
                    if (fieldVal->getType() != destType)
                    {
                        if (destType->isStructTy())
                        {
                            // Initializer type doesn't match a struct-typed field (e.g. integer 0 for
                            // a struct-typed generic field) - default-construct it instead.
                            // forceRoot: the GetFunction guard is an exact-key lookup, so a namespace walk
                            // here would call a same-named sibling type's ctor (layer 3).
                            if (compiler->GetFunction(field.TypeName))
                                fieldVal = compiler->CreateOverloadedFunctionCall(field.TypeName, {}, true);
                            else
                                fieldVal = llvm::Constant::getNullValue(destType);
                        }
                        else
                        {
                            // Narrowing field initializer (e.g. u8 r = 255 has i32 literal).
                            if (ShouldWarnImplicitFieldNarrowing(fieldVal, destType, field.TypeName))
                                compiler->LogWarning(std::format(
                                    "implicit narrowing to '{}' in field '{}' - use an explicit cast",
                                    field.TypeName, field.VariableName));
                            fieldVal = compiler->CreateCast(fieldVal, destType);
                        }
                    }
                    if (fieldVal->getType() == destType)
                    {
                        auto* fieldPtr = compiler->builder->CreateStructGEP(
                            structLLVMType, thisAlloca, fieldIdx, field.VariableName);
                        compiler->builder->CreateStore(fieldVal, fieldPtr);
                    }
                }
                fieldIdx++;
            }
        }
        else
        {
            // Parameterized constructor: delegate to the no-arg ctor for default initialization.
            auto* defaultVal = compiler->CreateOverloadedFunctionCall(structName, {});
            if (defaultVal)
                compiler->builder->CreateStore(defaultVal, thisAlloca);
        }

        // Register the alloca as the implicit 'this' pointer so member field access works
        LLVMBackend::TypeAndValue thisTv;
        thisTv.TypeName = structName;
        thisTv.VariableName = structName + "__";
        thisTv.Pointer = true;
        compiler->RegisterThisPointer(thisTv, thisAlloca, structLLVMType);

        // Parse user-written constructor body
        if (auto* blockItemList = func->compoundStatement()->blockItemList())
            ParseBlockItemList(blockItemList);

        // Load and return the (possibly mutated) struct by value
        auto* resultVal = compiler->CreateLoad(structLLVMType, thisAlloca);
        compiler->CreateReturnCall(resultVal);
        compiler->CreateBlockBreak(nullptr, true);
        compiler->ClearCurrentSubprogram();

        GenerateDefaultParamOverloads(structName, returnType, params, varargs, line);
    }

void MainListener::ParseDestructorDefinition(CFlatParser::DestructorDefinitionContext* ctx, const std::string& structName) {
        auto* compiler = Compiler(ctx);
        LLVMBackend::DeclTypeAndValue thisParam;
        thisParam.TypeName = structName;
        thisParam.VariableName = structName + "__";
        thisParam.Pointer = true;

        std::vector<LLVMBackend::TypeAndValue> params = { thisParam };

        LLVMBackend::TypeAndValue returnType;
        returnType.TypeName = "void";

        int line = static_cast<int>(ctx->getStart()->getLine());
        std::string fullName = "~" + structName;

        // A destructor can never be compiler-synthesized with a body, so an occupied slot here is
        // always a genuine user duplicate. Check BEFORE CreateFunctionDefinition: its own early
        // return for an already-defined body pushes no function scope, and continuing on would
        // pop a scope frame that was never pushed (the same-line underflow this guards against).
        std::string clashFile;
        size_t clashLine = 0;
        if (compiler->OverloadSlotIsDefined(fullName, returnType, params, false, &clashFile, &clashLine))
        {
            LogErrorContext(ctx, std::format(
                "redefinition of '{}' - the same overload is already defined at "
                "{}({}). Two parameter lists that differ only in a SPELLING of one type ('int' and "
                "'i32' name the same type) are one overload, not two.",
                fullName, clashFile, clashLine));
        }

        auto fn = compiler->CreateFunctionDefinition(fullName, returnType, params, false, false, line);
        compiler->RegisterDestructor(structName, fn);

        compiler->InitializeBlock(&fn->front(), false);
        // Fresh straight-line for this function/lambda body; restore the enclosing walk's flag on
        // exit so a nested lambda's return does not leak into the surrounding expression.
        ReturnFlagGuard functionReturnFlagGuard(&straightLineReturned_);
        straightLineReturned_ = false;

        auto blockItemList = ctx->compoundStatement()->blockItemList();
        if (blockItemList)
            ParseBlockItemList(blockItemList);

        compiler->CreateReturnCall(nullptr);
        compiler->CreateBlockBreak(nullptr, true);
        compiler->ClearCurrentSubprogram();
    }

void MainListener::ParseProgramDestructorDefinition(CFlatParser::DestructorDefinitionContext* ctx, const std::string& name) {
        auto* compiler = Compiler(ctx);
        LLVMBackend::DeclTypeAndValue thisParam;
        thisParam.TypeName = name;
        thisParam.VariableName = name + "__";
        thisParam.Pointer = true;

        std::vector<LLVMBackend::TypeAndValue> params = { thisParam };

        LLVMBackend::TypeAndValue returnType;
        returnType.TypeName = "void";

        int line = static_cast<int>(ctx->getStart()->getLine());
        std::string fullName = "~" + name;

        // Same guard as the struct/class destructor path: a destructor body can never be
        // compiler-synthesized, so an occupied slot here is always a genuine user duplicate.
        std::string clashFile;
        size_t clashLine = 0;
        if (compiler->OverloadSlotIsDefined(fullName, returnType, params, false, &clashFile, &clashLine))
        {
            LogErrorContext(ctx, std::format(
                "redefinition of '{}' - the same overload is already defined at "
                "{}({}). Two parameter lists that differ only in a SPELLING of one type ('int' and "
                "'i32' name the same type) are one overload, not two.",
                fullName, clashFile, clashLine));
        }

        auto fn = compiler->CreateFunctionDefinition(fullName, returnType, params, false, false, line);
        compiler->RegisterDestructor(name, fn);

        compiler->InitializeBlock(&fn->front(), false);
        // Fresh straight-line for this function/lambda body; restore the enclosing walk's flag on
        // exit so a nested lambda's return does not leak into the surrounding expression.
        ReturnFlagGuard functionReturnFlagGuard(&straightLineReturned_);
        straightLineReturned_ = false;

        auto blockItemList = ctx->compoundStatement()->blockItemList();
        if (blockItemList)
            ParseBlockItemList(blockItemList);

        // The builtin field teardown is appended at the end of the user body. If the body
        // fell through to an already-terminated block (an explicit `return;` at the tail),
        // the teardown would be both unreachable and emitted past a terminator, so reject it
        // with a clear message instead of producing invalid IR / silently skipping cleanup.
        if (compiler->builder->GetInsertBlock()->getTerminator() != nullptr)
        {
            LogErrorContext(ctx, std::format(
                "program '{}': ~{}() must not end with an explicit 'return;'. The builtin "
                "field cleanup (allocator, stop_source, inbox) is appended at the end of the "
                "destructor, so the body must fall off the end.", name, name));
        }
        else
        {
            // Builtin program field teardown runs at the end, after the user body falls through.
            EmitProgramSyntheticTeardown(name, fn->getArg(0));
        }

        compiler->CreateReturnCall(nullptr);
        compiler->CreateBlockBreak(nullptr, true);
        compiler->ClearCurrentSubprogram();
    }

void MainListener::LogErrorContext(antlr4::tree::TerminalNode* ctx, std::string errorMessage) {
        auto symbol = ctx->getSymbol();
        compilerLLVM->currentLine = static_cast<int>(symbol->getLine());
        compilerLLVM->currentColumn = static_cast<int>(symbol->getCharPositionInLine());
        compilerLLVM->LogError(std::move(errorMessage));
    }

void MainListener::LogErrorContext(antlr4::ParserRuleContext* ctx, std::string errorMessage) {
        compilerLLVM->currentLine = static_cast<int>(ctx->getStart()->getLine());
        compilerLLVM->currentColumn = static_cast<int>(ctx->getStart()->getCharPositionInLine());
        compilerLLVM->LogError(std::move(errorMessage));
    }

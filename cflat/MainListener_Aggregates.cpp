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
        || typeName == "u32" || typeName == "u64" || typeName == "u128"
        || typeName == "c8" || typeName == "c16" || typeName == "c32";
    const llvm::APInt& integer = constant->getValue();
    return isUnsigned ? integer.isNegative() || integer.getActiveBits() > bits
                      : !integer.isSignedIntN(bits);
}

std::string CppStructCxxName(const std::string& name)
{
    std::string out = "__cflat_user::" + name;
    for (size_t pos = 0; (pos = out.find('.', pos)) != std::string::npos; )
    {
        out.replace(pos, 1, "__");
        pos += 2;
    }
    return out;
}

std::string CppStructThunkStem(const std::string& name)
{
    std::string out = name;
    for (size_t pos = 0; (pos = out.find('.', pos)) != std::string::npos; )
    {
        out.replace(pos, 1, "__");
        pos += 2;
    }
    return out;
}
}

void MainListener::PrepareLaterCppStructDefinitions(
    CFlatParser::StructDefinitionContext* ctx, const std::string& namespaceName)
{
    auto* compiler = Compiler(ctx);
    auto* external = dynamic_cast<CFlatParser::ExternalDeclarationContext*>(ctx->parent);
    if (external == nullptr || external->parent == nullptr)
        return;

    std::vector<CFlatParser::ExternalDeclarationContext*> siblings;
    if (auto* parent = dynamic_cast<CFlatParser::TranslationUnitContext*>(external->parent))
        siblings = parent->externalDeclaration();
    else if (auto* parent = dynamic_cast<CFlatParser::NamespaceDefinitionContext*>(external->parent))
        siblings = parent->externalDeclaration();
    else if (auto* parent = dynamic_cast<CFlatParser::IfConstBlockContext*>(external->parent))
        siblings = parent->externalDeclaration();
    else if (auto* parent = dynamic_cast<CFlatParser::ExpectErrorDeclarationContext*>(external->parent))
        siblings = parent->externalDeclaration();
    if (siblings.empty())
        return;

    auto current = std::find(siblings.begin(), siblings.end(), external);
    if (current == siblings.end())
        return;
    for (++current; current != siblings.end(); ++current)
    {
        auto* future = (*current)->structDefinition();
        if (future == nullptr)
            continue;
        const auto annotations = ExtractAnnotations(future->annotationList());
        const bool isCppStruct = !BaseClauseIdentifiers(future).empty()
            || std::any_of(annotations.begin(), annotations.end(),
                           [](const auto& ann) { return ann.Name == "cpp"; });
        const std::string futureName = namespaceName.empty()
            ? future->directDeclarator()->getText()
            : namespaceName + "." + future->directDeclarator()->getText();
        if (!isCppStruct)
            continue;
        if (!compiler->HasTentativeCxxTypeFor(futureName))
            continue;
        if (!preparsedCppStructDefinitions_.insert(future).second)
            continue;
        ParseStructDefinition(future, {}, namespaceName);
    }
}

llvm::Value* MainListener::EmitAggregateFieldInitialization(
    const std::string& structName,
    llvm::StructType*& structType,
    std::vector<LLVMBackend::DeclTypeAndValue>& fields,
    size_t fieldCount)
{
    auto* compiler = Compiler();
    std::vector<llvm::Value*> initializers;
    std::vector<char> initializerUnsigned;
    // A field initializer that unwinds destroys the fields already built.
    LLVMBackend::UnwindPartialScope partialFields(*compiler);
    for (size_t fieldIndex = 0; fieldIndex < fields.size() && fieldIndex < fieldCount; ++fieldIndex)
    {
        auto& field = fields[fieldIndex];
        llvm::Value* rvalue = nullptr;
        bool fieldSrcUnsigned = false;
        if (auto* braceList = FieldDefaultBraceList(field))
        {
            GlobalScopeGuard fieldInitScope(global_scope);
            rvalue = ParseFieldDefaultBraceInitializer(structName, field, braceList);
        }
        else if (field.Initializer != nullptr)
        {
            auto* assignmentExpression = field.Initializer->assignmentExpression();
            if (assignmentExpression != nullptr)
            {
                rvalue = ParseFieldDefaultInitializer(
                    structName, field, assignmentExpression, &fieldSrcUnsigned);
                if (field.TypeName == "auto")
                {
                    field.TypeName = rvalue->getType()->getStructName();
                    structType = compiler->CreateStructType(structName, fields);
                }
            }
            else if (field.Initializer->Default() != nullptr)
            {
                GlobalScopeGuard fieldInitScope(global_scope);
                rvalue = GenerateDefaultValue(field);
            }
        }
        if (rvalue == nullptr)
        {
            auto* fieldType = compiler->GetType(field);
            if (fieldType != nullptr && fieldType->isArrayTy())
            {
                GlobalScopeGuard fieldInitScope(global_scope);
                rvalue = GenerateDefaultValue(field);
            }
        }
        initializers.push_back(rvalue);
        initializerUnsigned.push_back(fieldSrcUnsigned ? 1 : 0);
        compiler->NoteUnwindPartial(LLVMBackend::UnwindPartialEntry::Kind::Value, rvalue,
                                    field.TypeName);
    }

    llvm::Value* structValue = llvm::Constant::getNullValue(structType);
    for (unsigned index = 0; index < initializers.size(); ++index)
    {
        if (index >= structType->getNumElements())
            break;
        llvm::Value* rvalue = initializers[index];
        llvm::Value* const firstPassValue = rvalue;
        auto* destType = structType->getTypeAtIndex(index);
        auto& field = fields[index];
        if (rvalue == nullptr && (destType->isStructTy() || destType->isArrayTy()))
        {
            if (destType->isArrayTy())
                rvalue = GenerateDefaultValue(field);
            else if (compiler->GetFunction(field.TypeName))
                rvalue = compiler->CreateOverloadedFunctionCall(field.TypeName, {}, true);
            else
                rvalue = llvm::Constant::getNullValue(destType);
        }
        if (rvalue == nullptr)
            continue;

        rvalue = compiler->Upconvert(rvalue, destType,
            index < initializerUnsigned.size() && initializerUnsigned[index] != 0);
        if (rvalue->getType() != destType)
        {
            if (destType->isStructTy())
            {
                if (compiler->GetFunction(field.TypeName))
                    rvalue = compiler->CreateOverloadedFunctionCall(field.TypeName, {}, true);
                else
                    rvalue = llvm::Constant::getNullValue(destType);
            }
            else
            {
                if (ShouldWarnImplicitFieldNarrowing(rvalue, destType, field.TypeName))
                    compiler->LogWarning(std::format(
                        "implicit narrowing to '{}' in field '{}' - use an explicit cast",
                        SpellType(*compiler, field), field.VariableName));
                rvalue = compiler->CreateCast(rvalue, destType);
            }
        }
        if (rvalue->getType() == destType)
        {
            structValue = compiler->CreateInsertValue(structValue, rvalue, index);
            if (rvalue != firstPassValue)
                compiler->NoteUnwindPartial(LLVMBackend::UnwindPartialEntry::Kind::Value, rvalue,
                                            field.TypeName);
        }
    }
    return structValue;
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

        const auto rawAnnotations = ExtractAnnotations(ctx->annotationList());
        const auto baseClauses = BaseClauseIdentifiers(ctx);
        if (baseClauses.size() > 1)
        {
            Compiler(ctx)->LogErrorMessage("multiple bases are not supported yet");
            return;
        }
        const bool hasCppBase = !baseClauses.empty();
        const bool isCppStruct = hasCppBase
            || std::any_of(rawAnnotations.begin(), rawAnnotations.end(),
                           [](const auto& ann) { return ann.Name == "cpp"; });

        // A generic template is stored without emitting a C++ class. Its concrete instantiation
        // carries the substitutions and nameOverride used by the generated class path below.
        if (nameOverride.empty() && ctx->genericTypeParameters() != nullptr)
        {
            if (Compiler()->gts.scannedGenericInterfaceNames.count(structName) != 0
                || genericInterfaceTemplates.count(structName) != 0)
                LogErrorContext(ctx, std::format(
                    "generic struct '{}' conflicts with a generic interface of the same name",
                    SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = structName })));
            std::vector<std::string> valueParams;
            std::vector<std::string> valueDefaults;
            auto typeParams = ParseGenericTypeParameters(ctx->genericTypeParameters(), &valueParams,
                                                         &valueDefaults);
            genericStructTemplates[structName] = ctx;
            // Origin marker: a template DECLARED in a core library file. Read by
            // IsBorrowingContainerElementSink so a user type of the same name is not mistaken
            // for the core container.
            if (Compiler()->CurrentSourceIsCoreLibrary())
                Compiler()->gts.coreGenericTemplates.insert(structName);
            else
                Compiler()->gts.coreGenericTemplates.erase(structName);
            Compiler()->gts.genericTemplateNamespace[structName] = Compiler()->GetCurrentNamespace();
            Compiler()->RevokeGenericInterfaceInstances(structName);
            genericStructTypeParams[structName] = typeParams;
            genericStructValueParams[structName] = valueParams;
            Compiler()->gts.genericStructValueDefaults[structName] = valueDefaults;
            genericStructConstraints[structName] = ParseWhereClause(ctx->whereClause());
            ValidateGenericAggregateAliasNames(ctx, structName);
            // Record which param (if any) is variadic - always the last one
            {
                auto entries = ctx->genericTypeParameters()->typeParameterList()->typeParameterEntry();
                bool hasPack = !entries.empty() && entries.back()->Ellipsis() != nullptr;
                genericStructPackIndex[structName] = hasPack ? (typeParams.size() - 1) : std::string::npos;
            }
            return;
        }

        if (isCppStruct) compiler->RegisterCppStructName(structName);
        std::string cppBaseName;
        std::string cppBaseSpelling;
        size_t cppBaseOwnerGroup = static_cast<size_t>(-1);
        if (hasCppBase)
        {
            auto* base = baseClauses[0];
            const std::string baseSpelling = BaseSpecifierName(base);
            cppBaseName = baseSpelling;
            std::vector<std::string> typeArgs;
            std::string baseRequestName = baseSpelling;
            if (auto* generic = base->genericTypeParameters())
            {
                for (auto* entry : generic->typeParameterList()->typeParameterEntry())
                    typeArgs.push_back(ResolveTypeArgEntry(entry));
                // A C++ alias template substitutes into its own argument pattern; the request
                // must then name the TARGET, because the arguments are now the target's.
                std::string aliasError;
                if (compiler->ApplyCxxAliasPattern(cppBaseName, typeArgs, &aliasError))
                    baseRequestName = cppBaseName;
                else if (!aliasError.empty())
                {
                    Compiler(ctx)->LogErrorMessage("{}", { aliasError });
                    return;
                }
                else
                    cppBaseName = compiler->ResolveGenericBaseAlias(cppBaseName);
                cppBaseName = MangledGenericName(cppBaseName, typeArgs);
            }
            else
            {
                cppBaseName = compiler->ResolveTypeAlias(cppBaseName);
                baseRequestName = compiler->ResolveTypeAlias(baseRequestName);
            }
            compiler->RecordCppStructBase(structName, cppBaseName);
            std::string baseError;
            if (baseSpelling.empty()
                || !compiler->TryRequestCxxType(baseRequestName, typeArgs, cppBaseName, baseError)
                || !compiler->IsCxxRecord(cppBaseName))
            {
                Compiler(ctx)->LogErrorMessage(
                    "base '{}' of struct '{}' is not a C++ class", { baseSpelling, structName });
                return;
            }
            const auto* baseInfo = compiler->GetCxxClassInfo(cppBaseName);
            if (baseInfo == nullptr || !baseInfo->layoutRefusal.empty()
                || baseInfo->hasVirtualBases)
            {
                if (baseInfo != nullptr && !baseInfo->layoutRefusal.empty())
                    Compiler(ctx)->LogErrorMessage("{}", { baseInfo->layoutRefusal });
                else
                    Compiler(ctx)->LogErrorMessage("uses virtual inheritance, which is not supported yet");
                return;
            }
            if (!compiler->CxxSpellingForCflatType(cppBaseName, cppBaseSpelling))
            {
                Compiler(ctx)->LogErrorMessage(
                    "base '{}' of struct '{}' is not a C++ class", { baseSpelling, structName });
                return;
            }
            compiler->GetCxxTypeOwnerGroup(cppBaseName, cppBaseOwnerGroup);
        }

        // Re-emission guard: if this struct was already fully emitted via a transitive import,
        // CreateFunctionDefinition's duplicate-skip leaves the builder out of scope - skip the walk.
        {
            auto sd = compiler->GetDataStructure(structName);
            if (sd.StructType != nullptr && !sd.StructType->isOpaque())
            {
                if (auto* existing = compiler->GetFunction(structName);
                    existing != nullptr && compiler->FunctionHasDefinition(existing))
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
        if (hasCppBase && std::none_of(structAnnotations.begin(), structAnnotations.end(),
                                       [](const auto& ann) { return ann.Name == "cpp"; }))
            structAnnotations.push_back({ "cpp", {} });
        compiler->SetTypeAnnotations(structName, structAnnotations);

        if (compiler->IsVerbose())
            std::cout << "[verbose]     parse decl list: " << structName << "\n";

        LLVMBackend::AliasScopeGuard aliasScope(compiler);
        CollectAggregateAliases(ctx, structName);
        compiler->SaveAggregateAliasScope(structName);

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
                    if (spec->typeSpecifier() != nullptr)
                        element = CanonicalDeclarationTypeName(specs->declarationSpecifier(), spec->typeSpecifier()->getText());
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
                        "redeclaration of field '{}' in '{}'", f.VariableName,
                        SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = structName })));
        }

        // Bitfield packing: collapse runs of same-type bitfields into shared
        // storage slots BEFORE the constructor loop runs - the default ctor
        // emits one initializer per declList entry and the LLVM struct body
        // must match. The side-table goes into StructData.Bitfields.
        std::vector<LLVMBackend::BitfieldInfo> packedBitfields;
        bool anyBitfields = false;
        for (const auto& tv : declList) { if (tv.IsBitfield) { anyBitfields = true; break; } }
        if (anyBitfields)
            declList = compiler->PackBitfields(declList, packedBitfields, false);

        // Build the struct body before opening the constructor function so that
        // GetFunctionType can resolve the (sized) return type.  Initializer
        // expressions are evaluated later inside the constructor body.
        bool isUnion = (ctx->Union() != nullptr);
        if (isCppStruct && isUnion)
        {
            Compiler(ctx)->LogErrorMessage("[cpp] struct cannot be a union");
            structScopeStack.pop_back();
            return;
        }

        if (compiler->IsVerbose())
            std::cout << "[verbose]     create struct type: " << structName << "\n";
        // Capture `struct alignas(N) S { ... }` BEFORE layout so the padding
        // member can be appended atomically.
        uint64_t userAlign = 0;
        if (auto* alignSpec = ctx->alignmentSpecifier())
            userAlign = ParseAlignmentSpecifier(alignSpec);
        llvm::StructType* structType;
        llvm::StructType* literalType = nullptr;
        if (isUnion)
        {
            // A union body is one array: an over-aligned member cannot get a pad slot, it
            // just raises the union's alignment (all members start at offset 0).
            structType = compiler->CreateUnionType(structName, declList, userAlign);
        }
        else if (isCppStruct)
        {
            bool needsFieldInstantiation = false;
            for (const auto& field : declList)
            {
                auto fieldData = compiler->GetDataStructure(field.TypeName);
                if (compiler->IsCoreUniqueType(field.TypeName)
                    && (fieldData.StructType == nullptr || fieldData.StructType->isOpaque()))
                {
                    needsFieldInstantiation = true;
                    break;
                }
            }
            if (needsFieldInstantiation)
            {
                auto savedSubst = activeTypeSubstitutions;
                ProcessPendingInstantiations();
                activeTypeSubstitutions = savedSubst;
            }
            uint64_t fieldAlign = 0;
            declList = compiler->PadFieldsForAlignment(declList, fieldAlign,
                anyBitfields ? &packedBitfields : nullptr);
            if (fieldAlign > userAlign) userAlign = fieldAlign;
            if (declList.empty())
            {
                LLVMBackend::DeclTypeAndValue storage;
                storage.TypeName = "u8";
                storage.ConstArraySize = 1;
                declList.push_back(std::move(storage));
            }
            std::vector<llvm::Type*> literalFields;
            for (const auto& field : declList)
            {
                auto* fieldType = compiler->GetType(field);
                if (fieldType == nullptr || !fieldType->isSized())
                {
                    Compiler(ctx)->LogErrorMessage(
                        "[cpp] struct '{}' has an unsized field '{}'; use a pointer",
                        { structName, field.VariableName });
                    structScopeStack.pop_back();
                    return;
                }
                literalFields.push_back(fieldType);
            }
            if (userAlign > 1)
            {
                auto* natural = llvm::StructType::get(*compiler->context, literalFields);
                const uint64_t size = compiler->module->getDataLayout().getTypeAllocSize(natural);
                const uint64_t padded = llvm::alignTo(size, userAlign);
                if (padded > size)
                    literalFields.push_back(llvm::ArrayType::get(compiler->builder->getInt8Ty(),
                                                                   padded - size));
            }
            literalType = llvm::StructType::get(*compiler->context, literalFields);
            structType = compiler->GetDataStructure(structName).StructType;
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
            compiler->FlushPendingFunctionDeclarations();
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
        bool baseHasDefaultCtor = cppBaseName.empty();
        if (!cppBaseName.empty())
        {
            baseHasDefaultCtor = compiler->FindCxxDefaultCtor(cppBaseName) != nullptr;
            if (!baseHasDefaultCtor)
            {
                const auto* baseInfo = compiler->GetCxxClassInfo(cppBaseName);
                if (baseInfo != nullptr && !baseInfo->hasDeletedDefaultCtor
                    && (baseInfo->hasDefaultCtor
                        || (baseInfo->isAbstract && baseInfo->constructors.empty())))
                {
                    baseHasDefaultCtor = true;
                }
            }
        }
        const bool hasGeneratedNoArgCtor = !hasExplicitNoArgCtor && baseHasDefaultCtor;

        if (isCppStruct)
        {
            const std::string cxxName = CppStructCxxName(structName);
            const std::string shortName = CppStructThunkStem(structName);
            const uint64_t fieldBytes = compiler->module->getDataLayout().getTypeAllocSize(literalType);
            const uint64_t fieldAlign = std::max<uint64_t>(
                userAlign, std::max<uint64_t>(1,
                    compiler->module->getDataLayout().getABITypeAlign(literalType).value()));
            const bool hasGeneratedMove = cppBaseName.empty()
                || compiler->CxxBaseHasAccessibleMoveOrCopy(cppBaseName);
            uint64_t cxxFieldAlign = fieldAlign;
            if (!cppBaseName.empty())
            {
                LLVMBackend::TypeAndValue baseValue{ .TypeName = cppBaseName };
                if (auto* baseType = compiler->GetType(baseValue); baseType != nullptr)
                    cxxFieldAlign = std::max<uint64_t>(cxxFieldAlign,
                        compiler->module->getDataLayout().getABITypeAlign(baseType).value());
            }
            const std::string baseCxxName = cppBaseSpelling.empty() ? std::string()
                : (cppBaseSpelling.starts_with("::") ? cppBaseSpelling : "::" + cppBaseSpelling);
            std::string source = "namespace __cflat_user { struct " + shortName + "; }\n";
            source += "extern \"C\" {\n";
            auto appendParamSpelling = [&](const LLVMBackend::DeclTypeAndValue& param,
                                           std::string& out) -> bool {
                if (!compiler->CxxSpellingForCflatType(param.TypeName, out)) return false;
                const int depth = param.PointerDepth > 0
                    ? param.PointerDepth : (param.Pointer ? (param.ElemPointer ? 2 : 1) : 0);
                for (int i = 0; i < depth; ++i) out += " *";
                return true;
            };
            struct OverrideSourceInfo
            {
                CFlatParser::FunctionDefinitionContext* Function = nullptr;
                std::vector<LLVMBackend::DeclTypeAndValue> Params;
                LLVMBackend::CxxClassInfo::Method BaseMethod;
                std::string Name;
                std::string StableName;
            };
            auto sameOverrideType = [](const LLVMBackend::TypeAndValue& left,
                                       const LLVMBackend::TypeAndValue& right) {
                return left.TypeName == right.TypeName
                    && left.Pointer == right.Pointer
                    && left.ValuePointerDepth() == right.ValuePointerDepth()
                    && left.IsArrayView == right.IsArrayView
                    && left.IsRvalueRef == right.IsRvalueRef;
            };
            std::vector<OverrideSourceInfo> overrides;
            struct CppMethodSourceInfo
            {
                std::vector<LLVMBackend::DeclTypeAndValue> Params;
                std::string ReturnSpelling;
                std::string Name;
                std::string StableName;
            };
            std::vector<CppMethodSourceInfo> cppMethods;
            std::map<std::string, size_t> cppMethodOrdinals;
            std::set<std::string> overrideNames;
            std::map<std::string, size_t> overrideOrdinals;
            if (hasCppBase)
            {
            for (auto* func : functionList)
            {
                if (!FunctionDeclaresReturnType(func)) continue;
                const std::string methodName = getFunctionName(func);
                const bool isOverride = HasSoftDeclarationSpecifier(
                    func->declarationSpecifiers(), "override");
                auto candidates = compiler->FindCxxBaseMethods(cppBaseName, methodName);
                if (!isOverride)
                {
                    for (const auto& candidate : candidates)
                        if (candidate.raw.isVirtual)
                            Compiler(func)->LogErrorMessage(
                                "hides virtual method '{}.{}'; add override",
                                { compiler->DisplayCxxClassName(candidate.ownerType), methodName });
                    continue;
                }
                if (candidates.empty())
                {
                    Compiler(func)->LogErrorMessage(
                        "is marked override but base '{}' has no virtual method named {}",
                        { cppBaseName, methodName });
                    continue;
                }
                std::vector<LLVMBackend::CxxClassInfo::Method> virtuals;
                const LLVMBackend::CxxClassInfo::Method* unspellable = nullptr;
                for (const auto& candidate : candidates)
                    if (candidate.raw.isVirtual)
                    {
                        if (candidate.spellable) virtuals.push_back(candidate);
                        else if (unspellable == nullptr) unspellable = &candidate;
                    }
                if (virtuals.empty())
                {
                    if (unspellable != nullptr)
                    {
                        Compiler(func)->LogErrorMessage(
                            "does not override '{}.{}' (its C++ signature has no CFlat spelling)",
                        { compiler->DisplayCxxClassName(unspellable->ownerType), methodName });
                        continue;
                    }
                    Compiler(func)->LogErrorMessage(
                        "is marked override but '{}.{}' is not virtual",
                        { compiler->DisplayCxxClassName(candidates.front().ownerType), methodName });
                    continue;
                }
                auto params = ParseParameterTypeList(func->parameterTypeList());
                auto returnValue = getFunctionReturnType(func);
                const LLVMBackend::CxxClassInfo::Method* matched = nullptr;
                for (const auto& candidate : virtuals)
                {
                    if (candidate.params.size() != params.size() + 1
                        || !sameOverrideType(candidate.ret, returnValue)) continue;
                    bool same = true;
                    for (size_t i = 0; i < params.size(); ++i)
                        if (!sameOverrideType(candidate.params[i + 1], params[i])) same = false;
                    if (same) { matched = &candidate; break; }
                }
                if (matched == nullptr)
                {
                    if (unspellable != nullptr)
                    {
                        Compiler(func)->LogErrorMessage(
                            "does not override '{}.{}' (its C++ signature has no CFlat spelling)",
                            { compiler->DisplayCxxClassName(unspellable->ownerType), methodName });
                        continue;
                    }
                    const auto& expected = virtuals.front();
                    std::vector<LLVMBackend::TypeAndValue> expectedParams(
                        expected.params.begin() + 1, expected.params.end());
                    std::string candidatesText;
                    for (const auto& candidate : virtuals)
                    {
                        if (!candidatesText.empty()) candidatesText += "; ";
                        std::vector<LLVMBackend::TypeAndValue> candidateParams(
                            candidate.params.begin() + 1, candidate.params.end());
                        candidatesText += std::format("'{}.{}' expected {}({})",
                            compiler->DisplayCxxClassName(candidate.ownerType), methodName,
                            SpellType(*compiler, candidate.ret),
                            DescribeParameterTypes(candidateParams));
                    }
                    Compiler(func)->LogErrorMessage(
                        "does not override '{}.{}' (expected {}({}); candidates: {})",
                        { compiler->DisplayCxxClassName(expected.ownerType), methodName,
                          SpellType(*compiler, expected.ret),
                          DescribeParameterTypes(expectedParams), candidatesText });
                    continue;
                }
                if (matched->raw.isFinal)
                {
                    Compiler(func)->LogErrorMessage(
                        "overrides '{}.{}' which is final",
                        { compiler->DisplayCxxClassName(matched->ownerType), methodName });
                    continue;
                }
                const size_t ordinal = overrideOrdinals[methodName]++;
                const std::string stable = "__cflat_ovr_" + shortName + "_" + methodName
                    + "_" + std::to_string(ordinal);
                std::vector<LLVMBackend::TypeAndValue> overrideParams;
                overrideParams.reserve(params.size());
                for (const auto& param : params) overrideParams.push_back(param);
                compiler->RegisterCppStructOverrideName(structName, methodName,
                                                         overrideParams, stable);
                overrideNames.insert(methodName);
                overrides.push_back({ func, std::move(params), *matched, methodName, stable });
            }
            for (const auto& pure : compiler->FindCxxBaseVirtualMethods(cppBaseName))
            {
                if (!pure.raw.isPureVirtual) continue;
                bool implemented = false;
                for (const auto& overrideInfo : overrides)
                {
                    if (overrideInfo.Name != pure.raw.name
                        || overrideInfo.BaseMethod.params.size() != pure.params.size()
                        || !sameOverrideType(overrideInfo.BaseMethod.ret, pure.ret))
                        continue;
                    bool sameSignature = true;
                    for (size_t i = 0; i < pure.params.size(); ++i)
                        if (!sameOverrideType(overrideInfo.BaseMethod.params[i], pure.params[i]))
                            sameSignature = false;
                    if (sameSignature)
                    {
                        implemented = true;
                        break;
                    }
                }
                if (!implemented)
                {
                    if (!pure.spellable)
                        Compiler(ctx)->LogErrorMessage(
                            "does not override pure virtual method '{}.{}' (its C++ signature has no CFlat spelling)",
                            { compiler->DisplayCxxClassName(pure.ownerType), pure.raw.name });
                    else
                        Compiler(ctx)->LogErrorMessage(
                            "does not override pure virtual method '{}.{}'",
                            { compiler->DisplayCxxClassName(pure.ownerType), pure.raw.name });
                }
            }
            }
            for (auto* func : functionList)
            {
                if (!FunctionDeclaresReturnType(func)
                    || func->genericTypeParameters() != nullptr
                    || isFunctionStatic(func)
                    || HasSoftDeclarationSpecifier(func->declarationSpecifiers(), "override"))
                    continue;
                const std::string methodName = getFunctionName(func);
                const bool helperIdentifier = !methodName.empty()
                    && (std::isalpha((unsigned char)methodName.front())
                        || methodName.front() == '_')
                    && std::all_of(methodName.begin() + 1, methodName.end(),
                        [](unsigned char ch) { return std::isalnum(ch) || ch == '_'; });
                if (!helperIdentifier) continue;
                // Based on cppreference's C++ keywords table; the later-standard additions are gated below.
                static const std::set<std::string> cxxKeywords = {
                    "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand",
                    "bitor", "bool", "break", "case", "catch", "char", "char16_t",
                    "char32_t", "class", "compl", "const", "constexpr", "const_cast",
                    "continue", "decltype", "default", "delete", "do", "double",
                    "dynamic_cast", "else", "enum", "explicit", "export", "extern",
                    "false", "float", "for", "friend", "goto", "if", "inline", "int",
                    "long", "mutable", "namespace", "new", "noexcept", "not", "not_eq",
                    "nullptr", "operator", "or", "or_eq", "private", "protected", "public",
                    "register", "reinterpret_cast", "return", "short", "signed", "sizeof",
                    "static", "static_assert", "static_cast", "struct", "switch", "template",
                    "this", "thread_local", "throw", "true", "try", "typedef", "typeid",
                    "typename", "union", "unsigned", "using", "virtual", "void", "volatile",
                    "wchar_t", "while", "xor", "xor_eq", "char8_t", "concept", "consteval",
                    "constinit", "co_await", "co_return", "co_yield", "requires"
                };
                static const std::set<std::string> cxx20Keywords = {
                    "char8_t", "concept", "consteval", "constinit", "co_await", "co_return",
                    "co_yield", "requires"
                };
                static const std::set<std::string> cxx26Keywords = { "contract_assert" };
                if (cxxKeywords.count(methodName) != 0
                    && (compiler->cppStandard_ != "c++17" && compiler->cppStandard_ != "gnu++17"
                        || cxx20Keywords.count(methodName) == 0)
                    || ((compiler->cppStandard_ == "c++26" || compiler->cppStandard_ == "gnu++26")
                        && cxx26Keywords.count(methodName) != 0)) continue;
                std::vector<LLVMBackend::DeclTypeAndValue> params;
                if (func->parameterTypeList() != nullptr)
                    params = ParseParameterTypeList(func->parameterTypeList());
                std::string returnSpelling;
                const auto returnType = getFunctionReturnType(func);
                if ((!returnType.Pointer && !returnType.IsRvalueRef
                     && compiler->IsCxxRecord(returnType.TypeName))
                    || !compiler->CxxSpellingForCflatType(returnType.TypeName, returnSpelling))
                    continue;
                const int returnDepth = returnType.PointerDepth > 0 ? returnType.PointerDepth
                    : (returnType.Pointer ? (returnType.ElemPointer ? 2 : 1) : 0);
                for (int i = 0; i < returnDepth; ++i) returnSpelling += " *";
                if (returnType.IsRvalueRef) returnSpelling += " &&";
                bool spellable = true;
                for (auto& param : params)
                {
                    if (!param.Pointer && !param.IsRvalueRef
                        && compiler->IsCxxRecord(param.TypeName))
                    {
                        spellable = false;
                        break;
                    }
                    std::string spelling;
                    if (!appendParamSpelling(param, spelling))
                    {
                        spellable = false;
                        break;
                    }
                    const int depth = param.PointerDepth > 0 ? param.PointerDepth
                        : (param.Pointer ? (param.ElemPointer ? 2 : 1) : 0);
                    if (depth == 0 && param.IsRvalueRef) spelling += " &&";
                    else if (depth == 0 && param.IsCxxConstRef) spelling += " const&";
                }
                if (!spellable) continue;
                const size_t ordinal = cppMethodOrdinals[methodName]++;
                const std::string stable = "__cflat_mth_" + shortName + "_" + methodName
                    + "_" + std::to_string(ordinal);
                std::vector<LLVMBackend::TypeAndValue> methodParams;
                methodParams.reserve(params.size());
                for (const auto& param : params) methodParams.push_back(param);
                compiler->RegisterCppStructOverrideName(structName, methodName,
                                                         methodParams, stable);
                overrideNames.insert(methodName);
                cppMethods.push_back({ std::move(params), returnSpelling,
                                       methodName, stable });
            }
            struct ConstructorSourceInfo
            {
                CFlatParser::FunctionDefinitionContext* Function = nullptr;
                std::vector<LLVMBackend::DeclTypeAndValue> Params;
                size_t Index = 0;
                std::string BaseInitializer;
            };
            std::vector<ConstructorSourceInfo> constructors;
            auto isCppLiteral = [](const std::string& text) {
                if (text == "true" || text == "false") return true;
                if (text.size() >= 2
                    && ((text.front() == '"' && text.back() == '"')
                        || (text.front() == '\'' && text.back() == '\''))) return true;
                if (text.empty()) return false;
                size_t pos = (text[0] == '+' || text[0] == '-') ? 1 : 0;
                if (pos == text.size()) return false;
                auto integerSuffix = [](const std::string& suffix) {
                    return suffix.empty() || suffix == "u" || suffix == "U"
                        || suffix == "l" || suffix == "L" || suffix == "ll"
                        || suffix == "LL" || suffix == "ul" || suffix == "uL"
                        || suffix == "Ul" || suffix == "UL" || suffix == "lu"
                        || suffix == "lU" || suffix == "Lu" || suffix == "LU"
                        || suffix == "ull" || suffix == "uLL" || suffix == "Ull"
                        || suffix == "ULL" || suffix == "llu" || suffix == "llU"
                        || suffix == "LLu" || suffix == "LLU" || suffix == "z"
                        || suffix == "Z";
                };
                const bool hex = pos + 1 < text.size() && text[pos] == '0'
                    && (text[pos + 1] == 'x' || text[pos + 1] == 'X');
                if (hex) pos += 2;
                const size_t digitsBegin = pos;
                while (pos < text.size()
                       && (hex ? std::isxdigit((unsigned char)text[pos])
                               : std::isdigit((unsigned char)text[pos]))) ++pos;
                const bool beforeDot = pos != digitsBegin;
                bool dot = false;
                if (pos < text.size() && text[pos] == '.')
                {
                    dot = true;
                    ++pos;
                    const size_t afterDot = pos;
                    while (pos < text.size()
                           && (hex ? std::isxdigit((unsigned char)text[pos])
                                   : std::isdigit((unsigned char)text[pos]))) ++pos;
                    if (!beforeDot && pos == afterDot) return false;
                }
                if (!beforeDot && !dot) return false;
                bool exponent = false;
                const char exponentMarker = hex ? 'p' : 'e';
                if (pos < text.size()
                    && (text[pos] == exponentMarker
                        || text[pos] == static_cast<char>(exponentMarker - ('a' - 'A'))))
                {
                    exponent = true;
                    ++pos;
                    if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) ++pos;
                    const size_t exponentBegin = pos;
                    while (pos < text.size() && std::isdigit((unsigned char)text[pos])) ++pos;
                    if (pos == exponentBegin) return false;
                }
                const std::string suffix = text.substr(pos);
                if (hex && (dot || exponent))
                    return exponent && (suffix.empty() || suffix == "f" || suffix == "F"
                                        || suffix == "l" || suffix == "L");
                if (dot || exponent)
                    return suffix.empty() || suffix == "f" || suffix == "F"
                        || suffix == "l" || suffix == "L";
                return integerSuffix(suffix);
            };
            auto baseInitializerFor = [&](CFlatParser::FunctionDefinitionContext* func,
                                          const std::vector<LLVMBackend::DeclTypeAndValue>& params,
                                          std::string& result) {
                if (func->baseSpecifier() == nullptr) return true;
                auto* initializerBase = func->baseSpecifier();
                const std::string initializerSpelling = initializerBase->getText();
                std::string initializerName = BaseSpecifierName(initializerBase);
                std::vector<std::string> initializerArgs;
                if (auto* generic = initializerBase->genericTypeParameters())
                {
                    for (auto* entry : generic->typeParameterList()->typeParameterEntry())
                        initializerArgs.push_back(ResolveTypeArgEntry(entry));
                    compiler->ResolveGenericAliasSpelling(initializerName, initializerArgs);
                    initializerName = MangledGenericName(initializerName, initializerArgs);
                }
                if (initializerName != cppBaseName)
                {
                    Compiler(func)->LogErrorMessage(
                        "base initializer names '{}' but the base class is '{}'",
                        { initializerSpelling, baseClauses[0]->getText() });
                    return false;
                }
                std::set<std::string> parameterNames;
                for (const auto& param : params)
                    if (!param.VariableName.empty()) parameterNames.insert(param.VariableName);
                std::vector<std::string> args;
                if (auto* list = func->argumentExpressionList())
                    for (auto* arg : list->argumentNamedExpression())
                    {
                        auto* assignment = arg->assignmentExpression();
                        const std::string text = assignment == nullptr ? arg->getText()
                                                                       : assignment->getText();
                        if (parameterNames.count(text) == 0 && !isCppLiteral(text))
                        {
                            Compiler(func)->LogErrorMessage(
                                "base initializer arguments must be constructor parameters or literals");
                            return false;
                        }
                        args.push_back(text);
                    }
                result = " : " + baseCxxName + "(";
                for (size_t i = 0; i < args.size(); ++i)
                    result += (i == 0 ? "" : ", ") + args[i];
                result += ")";
                return true;
            };
            size_t ctorIndex = 0;
            if (hasGeneratedNoArgCtor)
            {
                source += "void __cflat_ctor_" + shortName + "_0(" + cxxName + "* dst) noexcept;\n";
                ctorIndex = 1;
            }
            for (auto* func : functionList)
            {
                if (FunctionDeclaresReturnType(func) || getFunctionName(func) != baseName)
                    continue;
                auto params = ParseParameterTypeList(func->parameterTypeList());
                source += "void __cflat_ctor_" + shortName + "_" + std::to_string(ctorIndex)
                       + "(" + cxxName + "* dst";
                bool valid = true;
                for (size_t i = 0; i < params.size(); ++i)
                {
                    std::string spelling;
                    if (!appendParamSpelling(params[i], spelling))
                    {
                        std::string displayType = params[i].TypeName;
                        if (auto* list = func->parameterTypeList()->parameterList())
                        {
                            const auto& declarations = list->parameterDeclaration();
                            if (i < declarations.size()
                                && declarations[i]->declarationSpecifiers() != nullptr)
                                displayType = declarations[i]->declarationSpecifiers()->getText();
                        }
                        Compiler(func)->LogErrorMessage(
                            "[cpp] struct constructor parameter '{}' has no C++ spelling",
                            { displayType });
                        valid = false;
                        break;
                    }
                    source += ", " + spelling + " "
                           + (params[i].VariableName.empty()
                              ? "p" + std::to_string(i) : params[i].VariableName);
                }
                source += ") noexcept;\n";
                if (!valid) break;
                std::string baseInitializer;
                if (!baseInitializerFor(func, params, baseInitializer)) break;
                constructors.push_back({ func, std::move(params), ctorIndex,
                                         std::move(baseInitializer) });
                ++ctorIndex;
            }
            if (hasGeneratedMove)
                source += "void __cflat_move_" + shortName + "(" + cxxName
                       + "* dst, " + cxxName + "* src) noexcept;\n";
            source += "void __cflat_dtor_" + shortName + "(" + cxxName + "* dst) noexcept;\n";
            for (const auto& overrideInfo : overrides)
            {
                const auto& raw = overrideInfo.BaseMethod.raw;
                source += raw.retType + " " + overrideInfo.StableName + "("
                    + (raw.isConst ? "const " : "") + cxxName + "*";
                for (size_t i = 1; i < raw.paramTypes.size(); ++i)
                    source += ", " + raw.paramTypes[i] + " "
                        + (i - 1 < overrideInfo.Params.size()
                           && !overrideInfo.Params[i - 1].VariableName.empty()
                           ? overrideInfo.Params[i - 1].VariableName
                           : "p" + std::to_string(i - 1));
                source += ");\n";
            }
            for (const auto& method : cppMethods)
            {
                source += method.ReturnSpelling + " " + method.StableName + "("
                    + cxxName + "* self";
                for (size_t i = 0; i < method.Params.size(); ++i)
                {
                    std::string spelling;
                    appendParamSpelling(method.Params[i], spelling);
                    if (method.Params[i].IsRvalueRef) spelling += " &&";
                    else if (method.Params[i].IsCxxConstRef) spelling += " const&";
                    source += ", " + spelling + " "
                        + (method.Params[i].VariableName.empty()
                           ? "p" + std::to_string(i) : method.Params[i].VariableName);
                }
                source += ");\n";
            }
            source += "}\nnamespace __cflat_user {\nstruct " + shortName + " final";
            if (!baseCxxName.empty()) source += " : public " + baseCxxName;
            source += " {\n";
            std::set<std::pair<std::string, std::string>> inheritedUsing;
            for (const auto& method : cppMethods)
                for (const auto& candidate : compiler->FindCxxBaseMethods(cppBaseName, method.Name))
                {
                    // `using` on a private base member is ill-formed; it is not visible anyway.
                    if (candidate.raw.access == cflat_cinterop::AccessPrivate) continue;
                    std::string ownerSpelling;
                    if (compiler->CxxSpellingForCflatType(candidate.ownerType, ownerSpelling))
                        inheritedUsing.emplace(ownerSpelling, method.Name);
                }
            for (const auto& [owner, name] : inheritedUsing)
                source += "    using " + owner + "::" + name + ";\n";
            source += "    alignas(" + std::to_string(cxxFieldAlign) + ") unsigned char __cflat_fields["
                   + std::to_string(std::max<uint64_t>(1, fieldBytes)) + "];\n";
            if (hasGeneratedNoArgCtor)
                source += "    " + shortName + "() { __cflat_ctor_" + shortName + "_0(this); }\n";
            for (const auto& ctor : constructors)
            {
                source += "    " + shortName + "(";
                for (size_t i = 0; i < ctor.Params.size(); ++i)
                {
                    if (i != 0) source += ", ";
                    std::string spelling;
                    appendParamSpelling(ctor.Params[i], spelling);
                    source += spelling + " "
                           + (ctor.Params[i].VariableName.empty()
                              ? "p" + std::to_string(i) : ctor.Params[i].VariableName);
                    if (ctor.Params[i].DefaultValue != nullptr)
                        source += " = " + ctor.Params[i].DefaultValue->getText();
                }
                source += ")" + ctor.BaseInitializer + " { __cflat_ctor_" + shortName + "_"
                       + std::to_string(ctor.Index) + "(this";
                for (size_t i = 0; i < ctor.Params.size(); ++i)
                    source += ", " + (ctor.Params[i].VariableName.empty()
                                      ? "p" + std::to_string(i) : ctor.Params[i].VariableName);
                source += "); }\n";
            }
            source += "    " + shortName + "(const " + shortName + "&) = delete;\n";
            source += "    " + shortName + "& operator=(const " + shortName + "&) = delete;\n";
            source += "    " + shortName + "(" + shortName + "&& src) noexcept";
            if (!baseCxxName.empty() && hasGeneratedMove)
                source += " : " + baseCxxName
                    + "(static_cast<" + baseCxxName + "&&>(src))";
            if (!hasGeneratedMove)
                source += " = delete;";
            else
                source += " { __cflat_move_" + shortName + "(this, &src); }";
            source += "\n";
            source += "    " + shortName + "& operator=(" + shortName + "&&) = delete;\n";
            source += "    ~" + shortName + "() { __cflat_dtor_" + shortName + "(this); }\n";
            for (const auto& overrideInfo : overrides)
            {
                const auto& raw = overrideInfo.BaseMethod.raw;
                source += "    " + raw.retType + " " + overrideInfo.Name + "(";
                for (size_t i = 1; i < raw.paramTypes.size(); ++i)
                    source += (i == 1 ? "" : ", ") + raw.paramTypes[i] + " "
                        + (i - 1 < overrideInfo.Params.size()
                           && !overrideInfo.Params[i - 1].VariableName.empty()
                           ? overrideInfo.Params[i - 1].VariableName
                           : "p" + std::to_string(i - 1));
                source += ")" + std::string(raw.isConst ? " const" : "")
                    + (raw.isNoexcept ? " noexcept" : "") + " override { ";
                if (raw.retType != "void") source += "return ";
                source += overrideInfo.StableName + "(this";
                for (size_t i = 0; i < overrideInfo.Params.size(); ++i)
                    source += ", " + (overrideInfo.Params[i].VariableName.empty()
                                      ? "p" + std::to_string(i)
                                      : overrideInfo.Params[i].VariableName);
                source += "); }\n";
            }
            for (const auto& method : cppMethods)
            {
                source += "    " + method.ReturnSpelling + " " + method.Name + "(";
                for (size_t i = 0; i < method.Params.size(); ++i)
                {
                    if (i != 0) source += ", ";
                    std::string spelling;
                    appendParamSpelling(method.Params[i], spelling);
                    if (method.Params[i].IsRvalueRef) spelling += " &&";
                    else if (method.Params[i].IsCxxConstRef) spelling += " const&";
                    source += spelling + " " + (method.Params[i].VariableName.empty()
                        ? "p" + std::to_string(i) : method.Params[i].VariableName);
                }
                source += ") { ";
                if (method.ReturnSpelling != "void") source += "return ";
                source += method.StableName + "(this";
                for (size_t i = 0; i < method.Params.size(); ++i)
                    source += ", " + (method.Params[i].VariableName.empty()
                        ? "p" + std::to_string(i) : method.Params[i].VariableName);
                source += "); }\n";
            }
            source += "};\n}\n";
            for (auto& field : declList)
                field.IsCflatOwned = true;
            compiler->RegisterGeneratedCxxOverrideNames(structName, overrideNames);
            std::string requestError;
            if (!compiler->RequestGeneratedCxxType(structName, cxxName, source, literalType,
                                                   declList, requestError, cppBaseOwnerGroup,
                                                   overrideNames))
            {
                if (requestError.empty())
                    Compiler(ctx)->LogErrorMessage("generated [cpp] struct could not be registered");
                else
                    Compiler(ctx)->LogErrorMessage("{}", { requestError });
                structScopeStack.pop_back();
                return;
            }
            compiler->RegisterCppStructProtectedBaseMembers(structName);
            for (const auto& [typeName, error] : compiler->RetryTentativeCxxTypesFor(structName))
                Compiler(ctx)->LogErrorMessage("C++ type '{}' could not be parsed: {}",
                                               { typeName, error });
            if (compiler->IsVerbose())
                std::cout << "[verbose] generated C++ for " << structName << ":\n" << source;
            if (hasGeneratedMove)
                EmitCppStructMoveThunk(ctx, structName);
            if (hasGeneratedNoArgCtor)
                EmitCppStructConstructorThunk(ctx, nullptr, structName,
                                              compiler->GetDataStructure(structName).StructFields,
                                              {}, 0);
            if (MemberDestructorDefinitions(ctx).empty())
                EmitCppStructDestructorThunk(ctx, structName);

            PrepareLaterCppStructDefinitions(ctx, namespaceName);
        }

        // Create default constructor (skipped when user provides an explicit no-arg ctor)
        if (!isCppStruct && !hasExplicitNoArgCtor)
        {
            auto funcDef = compiler->CreateFunctionDefinition(structName, returnType, {});

            if (isUnion)
            {
                EmitUnionDefaultConstructorBody(ctx, structName, structType, declList);
            }
            else
            {
                std::vector<llvm::Value*> initializers;
                std::vector<char> initializerUnsigned;
                // A field initializer that unwinds destroys the fields already built.
                LLVMBackend::UnwindPartialScope partialFields(*compiler);
                for (auto& typeValue : declList)
                {
                    auto initializer = typeValue.Initializer;
                    llvm::Value* rvalue = nullptr;
                    bool fieldSrcUnsigned = false;
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
                                structName, typeValue, assignmentExpression, &fieldSrcUnsigned);
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
                    initializerUnsigned.push_back(fieldSrcUnsigned ? 1 : 0);
                    compiler->NoteUnwindPartial(LLVMBackend::UnwindPartialEntry::Kind::Value,
                                                rvalue, typeValue.TypeName);
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
                    llvm::Value* const firstPassValue = rvalue;
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
                        rvalue = compiler->Upconvert(rvalue, destType,
                            structIndex < initializerUnsigned.size() && initializerUnsigned[structIndex] != 0);
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
                                        SpellType(*compiler, declList[structIndex]),
                                        declList[structIndex].VariableName));
                                rvalue = compiler->CreateCast(rvalue, destType);
                            }
                        }
                        structVal = compiler->CreateInsertValue(structVal, rvalue, structIndex);
                        if (rvalue != firstPassValue)
                            compiler->NoteUnwindPartial(LLVMBackend::UnwindPartialEntry::Kind::Value,
                                                        rvalue, declList[structIndex].TypeName);
                    }

                    structIndex++;
                }
                partialFields.Release();

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
                std::string typeSig = SpellType(*compiler, field);
                LLVMBackend::TypeAndValue ownerType;
                ownerType.TypeName = structName;
                const std::string displayFieldName = SpellType(*compiler, ownerType) + "." + field.VariableName;
                s->Register(SymbolKind::Field, structName + "." + field.VariableName,
                            compiler->GetSourceFilePath(),
                            (int)ctx->getStart()->getLine(),
                            (int)ctx->getStart()->getCharPositionInLine(),
                            annSig + typeSig + " " + displayFieldName, {}, displayFieldName);
            }
        }

        // Member function signatures were pre-declared above (before the flush).

        // Pre-register destructor so 'delete' inside static methods can call it, AND so a member
        // that constructs an instance of its own type (e.g. dictionary.copy() building a local
        // dictionary) forces .dtorfull with the user destructor already resolved. The scanner only
        // forward-declares the TEMPLATE's `~name`; a concrete instantiation's `~name__T` is not, so
        // declare it here when missing - otherwise .dtorfull bakes a null user-dtor and caches it,
        // leaking everything the hand-written destructor would have freed.
        if (!isCppStruct && !MemberDestructorDefinitions(ctx).empty())
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
            size_t cppCtorIndex = hasGeneratedNoArgCtor ? 1 : 0;
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
                    ParseConstructorDefinition(func, structName, suppliesNoArgCtor,
                        isCppStruct ? cppCtorIndex++ : SIZE_MAX, hasExplicitNoArgCtor);
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
                    std::vector<std::string> valueParams;
                    std::vector<std::string> valueDefaults;
                    genericFunctionTypeParams[qualifiedName] =
                        ParseGenericTypeParameters(func->genericTypeParameters(), &valueParams,
                                                   &valueDefaults);
                    genericFunctionValueParams[qualifiedName] = valueParams;
                    Compiler()->gts.genericFunctionValueDefaults[qualifiedName] = valueDefaults;
                    genericFunctionConstraints[qualifiedName] = ParseWhereClause(func->whereClause());
                }
                else if (funcName == "operator new" || funcName == "operator delete"
                         || funcName == "operator bool" || isFunctionStatic(func))
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
                            "[Capability] on '{}': unknown interface '{}'",
                            SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = structName }),
                            SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = ifaceName })));
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
                    SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = structName }),
                    tv.VariableName, declList[chosen].VariableName));
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

        bool unionFieldSrcUnsigned = false;
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
                rvalue = ParseFieldDefaultInitializer(structName, tv,
                    tv.Initializer->assignmentExpression(), &unionFieldSrcUnsigned);
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
        compiler->CreateAssignment(rvalue, slot, unionFieldSrcUnsigned, fieldType);
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

const LockMode* MainListener::FindHeldGuard(const std::string& receiverPath, const std::string& guard,
                                            std::string* matchedKey) const {
        auto probe = [&](const std::string& key) -> const LockMode* {
            auto it = currentLockSet.find(key);
            if (it == currentLockSet.end()) return nullptr;
            if (matchedKey != nullptr) *matchedKey = key;
            return &it->second;
        };
        if (receiverPath.empty() || receiverPath == "this")
        {
            if (const LockMode* bare = probe(guard)) return bare;
            return probe("this." + guard);
        }
        return probe(receiverPath + "." + guard);
    }

// The leading component of a lock key: 'inner.mtx' / 'inner[0].mtx' -> 'inner'.
static std::string LockKeyRoot(const std::string& path)
{
    size_t end = path.find_first_of(".[");
    return end == std::string::npos ? path : path.substr(0, end);
}

bool MainListener::LockRootIsImplicitThisField(const std::string& name) const {
        if (name.empty()) return false;
        // A local or a parameter of that name shadows the field, so the bare spelling names
        // something else entirely - same order the postfix implicit-this door uses.
        if (compilerLLVM->GetLocalVariable(name).Storage != nullptr) return false;
        if (compilerLLVM->GetFunctionArgument(name).GetValue() != nullptr) return false;
        return compilerLLVM->HasMemberVariable(name);
    }

bool MainListener::LockSetHoldsPath(const std::string& key) const {
        if (key.empty()) return false;
        if (currentLockSet.find(key) != currentLockSet.end()) return true;
        // Bridge the bare and 'this.'-qualified spellings only when the bare root really IS a
        // field of the enclosing struct; a shadowing local or parameter names a different object.
        if (key.starts_with("this."))
        {
            std::string bare = key.substr(5);
            if (!LockRootIsImplicitThisField(LockKeyRoot(bare))) return false;
            return currentLockSet.find(bare) != currentLockSet.end();
        }
        if (!LockRootIsImplicitThisField(LockKeyRoot(key))) return false;
        return currentLockSet.find("this." + key) != currentLockSet.end();
    }

void MainListener::CheckGuardedFieldRead(antlr4::ParserRuleContext* ctx, const std::string& fieldName,
                                        const std::string& guard, const std::string& receiverPath,
                                        const std::string& receiverName, std::string& heldKey) {
        if (IsSimpleLockPath(receiverPath))
        {
            if (FindHeldGuard(receiverPath, guard, &heldKey) != nullptr) return;
            LogErrorContext(ctx, std::format(
                "Field '{}' is guarded by '{}': must hold '{}' before accessing it.",
                fieldName, guard, GuardKeyForPath(receiverPath, guard)));
            return;
        }
        // Not a plain path (a call result, a computed receiver): keep the legacy key so the
        // verdict is exactly what it was before the canonical-path keying landed.
        if (receiverName.empty()) return;
        std::string requiredLock = receiverName + "." + guard;
        if (currentLockSet.find(requiredLock) != currentLockSet.end())
        {
            heldKey = requiredLock;
            return;
        }
        LogErrorContext(ctx, std::format(
            "Field '{}' is guarded by '{}': must hold '{}' before accessing it.",
            fieldName, guard, requiredLock));
    }

void MainListener::CheckGuardedWrite(antlr4::ParserRuleContext* ctx, const LLVMBackend::NamedVariable& target) {
        const std::string& guard = target.TypeAndValue.GuardedBy;
        if (guard.empty()) return;

        std::string key = target.GuardLockKey.empty()
            ? GuardLockKey(target.TypeAndValue) : target.GuardLockKey;
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
                            const std::vector<LLVMBackend::NamedVariable>& arguments,
                            const std::string& receiverPath) {
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
                // this-relative lock: substitute with the receiver's canonical source path, which
                // is the spelling lock(...) records; fall back to its root name when not a path.
                std::string base = IsSimpleLockPath(receiverPath) ? receiverPath : receiverText;
                if (base.empty()) continue; // no known receiver - skip
                canonical = rest.empty() ? base : base + "." + rest;
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
                        // The argument's canonical path ('&o.inner' -> 'o.inner') is the spelling
                        // lock(...) records; a non-path argument keeps the legacy root-name key.
                        const std::string& argPath = arguments[pi].CallerLockPath;
                        const std::string& argName = arguments[pi].CallerName;
                        const std::string& base = argPath.empty() ? argName : argPath;
                        if (base.empty()) found = true; // complex expression - can't check
                        else
                            canonical = rest.empty() ? base : base + "." + rest;
                    }
                    found = true;
                    break;
                }
                if (!found)
                    canonical = lock; // global lock - no substitution
            }

            if (!canonical.empty() && !LockSetHoldsPath(canonical))
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
        const std::string channelType = ArenaChannelTypeName(*compiler);
        auto* dtorFn      = compiler->builder->GetInsertBlock()->getParent();
        auto* progType    = compiler->dataStructures[name].StructType;
        auto* fatTy       = compiler->GetFatPtrType();   // {i8*, i8*}
        auto* voidPtrType = cflat_llvm::PointerTo(compiler->builder->getInt8Ty());
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
            auto* arenaTy = compiler->dataStructures.count(channelType)
                            ? compiler->dataStructures[channelType].StructType : nullptr;
            auto* arenaDestroyFn = FindMethodOf("destroy", channelType);
            if (arenaTy && arenaDestroyFn)
            {
                auto* arenaPtrTy = cflat_llvm::PointerTo(arenaTy);
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
        const std::string listStringTypeName = MangleGenericInstance(*compiler, "list", { "string" });

        if (member == "run" && params.size() == 2 && params[1].TypeName == listStringTypeName
            && params[1].IsMove)
        {
            auto it = compiler->functionTable.find(member);
            if (it != compiler->functionTable.end())
            {
                for (const auto& sym : it->second)
                {
                    if (sym.Parameters.size() != params.size()
                        || sym.ReturnType.ToUniqueString(*compiler) != returnType.ToUniqueString(*compiler))
                        continue;
                    bool sameExceptMove = true;
                    for (size_t i = 0; i < params.size(); i++)
                    {
                        if (sym.Parameters[i].ToUniqueString(*compiler) != params[i].ToUniqueString(*compiler)
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
        const std::string listStringTypeName = MangleGenericInstance(*compiler, "list", { "string" });

        // Resolve types - all must be concrete by this point (ProcessPendingInstantiations ran)
        auto* progType        = compiler->dataStructures[name].StructType;
        auto findStructType = [&](const std::string& name) -> llvm::StructType* {
            auto it = compiler->dataStructures.find(name);
            return it != compiler->dataStructures.end() ? it->second.StructType : nullptr;
        };
        auto* defAllocType    = findStructType("MallocAllocator");
        auto* listStringType  = findStructType(listStringTypeName);
        auto* stringStructType= findStructType("string");
        auto* threadType      = findStructType("Thread");
        auto* fatTy           = compiler->GetFatPtrType();   // {i8*, i8*}

        if (!progType || !defAllocType || !listStringType || !threadType)
        {
            compiler->LogError(std::format(
                "program '{}': missing required type (MallocAllocator={}, {}={}, Thread={})",
                name,
                defAllocType  ? "ok" : "missing",
                SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = listStringTypeName }),
                listStringType ? "ok" : "missing",
                threadType    ? "ok" : "missing"));
            return;
        }

        auto* progPtrType    = cflat_llvm::PointerTo(progType);
        auto* defAllocPtrTy  = cflat_llvm::PointerTo(defAllocType);
        auto* voidPtrType    = cflat_llvm::PointerTo(compiler->builder->getInt8Ty());
        auto* i32Type        = llvm::Type::getInt32Ty(*compiler->context);
        auto* i64Type        = llvm::Type::getInt64Ty(*compiler->context);

        // __RunArgs_Name = { Name*, list$string }
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
                trampolineFn->setPersonalityFn(compiler->GetTargetEhPersonality());

            auto* ctxArg = trampolineFn->getArg(0);

            // Cast void* ctx to __RunArgs_Name*
            auto* argsPacket = compiler->builder->CreateBitCast(
                ctxArg, cflat_llvm::PointerTo(runArgsType), "args_packet");

            // Load self (Name*) from field 0
            auto* selfGEP  = compiler->builder->CreateStructGEP(runArgsType, argsPacket, 0, "self_gep");
            auto* self     = compiler->builder->CreateLoad(progPtrType, selfGEP, "self");

            // Pointer to list$string (field 1) - passed by value to main
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

            // Load list$string args by value from the packet
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
                    argvRaw, cflat_llvm::PointerTo(voidPtrType), "argv_ptr");

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
            else if (compiler->cppInteropUsed_ && !compiler->targetWindows_
                     && compiler->symbolSink_ == nullptr)
            {
                llvm::Function* ehGuard = compiler->EnsureCxxProgramEhGuard(name);
                if (ehGuard == nullptr)
                {
                    auto* callResult = compiler->builder->CreateCall(
                        mainFn->getFunctionType(), mainFn, mainArgs, "main_result");
                    compiler->builder->CreateStore(callResult, exitCodeGEP);
                    compiler->builder->CreateBr(cleanupBB);
                }
                else
                {
                    // The C++ guard owns the personality and catch-all landing pad. Keep the
                    // CFlat trampoline as a normal cleanup function and pass main through it.
                    auto* ehFrameType = llvm::StructType::create(
                        *compiler->context,
                        {voidPtrType, voidPtrType, i32Type, voidPtrType},
                        "__ProgramEhFrame_" + name);
                    auto* ehFrame = compiler->AllocaAtEntry(ehFrameType, nullptr, "eh_frame");
                    auto* nullPtr = llvm::ConstantPointerNull::get(
                        llvm::cast<llvm::PointerType>(voidPtrType));

                    auto* frameSelfGEP = compiler->builder->CreateStructGEP(
                        ehFrameType, ehFrame, 0, "eh_frame_self");
                    llvm::Value* selfForMain = isImported ? static_cast<llvm::Value*>(nullPtr) : self;
                    compiler->builder->CreateStore(selfForMain, frameSelfGEP);

                    llvm::Value* argsForMain = nullPtr;
                    if (isListArgs)
                    {
                        auto* argsStorage = compiler->AllocaAtEntry(
                            listStringType, nullptr, "eh_args");
                        compiler->builder->CreateStore(argsVal, argsStorage);
                        argsForMain = argsStorage;
                    }
                    auto* frameArgsGEP = compiler->builder->CreateStructGEP(
                        ehFrameType, ehFrame, 1, "eh_frame_args");
                    compiler->builder->CreateStore(argsForMain, frameArgsGEP);

                    auto* frameArgcGEP = compiler->builder->CreateStructGEP(
                        ehFrameType, ehFrame, 2, "eh_frame_argc");
                    llvm::Value* argcForMain = isArgcArgv
                        ? argc32Val : static_cast<llvm::Value*>(compiler->builder->getInt32(0));
                    compiler->builder->CreateStore(
                        argcForMain, frameArgcGEP);

                    auto* frameArgvGEP = compiler->builder->CreateStructGEP(
                        ehFrameType, ehFrame, 3, "eh_frame_argv");
                    llvm::Value* argvForMain = isArgcArgv
                        ? argvPtrVal : static_cast<llvm::Value*>(nullPtr);
                    compiler->builder->CreateStore(
                        argvForMain, frameArgvGEP);

                    LLVMBackend::TypeAndValue intReturn;
                    intReturn.TypeName = "int";
                    LLVMBackend::DeclTypeAndValue frameParam;
                    frameParam.TypeName = "void";
                    frameParam.VariableName = "frame";
                    frameParam.Pointer = true;
                    RejectIfProgramMemberSlotTaken(ctx, name, "__program_main_" + name,
                        "int __program_main_" + name + "(void*)", intReturn, {frameParam});

                    auto* innerFn = llvm::Function::Create(
                        trampolineFnTy, llvm::Function::InternalLinkage,
                        "__program_main_" + name, *compiler->module);
                    auto* innerEntry = llvm::BasicBlock::Create(
                        *compiler->context, "entry", innerFn);
                    llvm::IRBuilder<> innerBuilder(innerEntry);
                    auto* innerFrame = innerBuilder.CreateBitCast(
                        innerFn->getArg(0), cflat_llvm::PointerTo(ehFrameType), "eh_frame");

                    auto* innerSelf = innerBuilder.CreateLoad(
                        voidPtrType, innerBuilder.CreateStructGEP(ehFrameType, innerFrame, 0), "self");
                    auto* innerArgsPtr = innerBuilder.CreateLoad(
                        voidPtrType, innerBuilder.CreateStructGEP(ehFrameType, innerFrame, 1), "args");
                    auto* innerArgc = innerBuilder.CreateLoad(
                        i32Type, innerBuilder.CreateStructGEP(ehFrameType, innerFrame, 2), "argc");
                    auto* innerArgv = innerBuilder.CreateLoad(
                        voidPtrType, innerBuilder.CreateStructGEP(ehFrameType, innerFrame, 3), "argv");

                    std::vector<llvm::Value*> innerMainArgs;
                    if (isImported)
                    {
                        if (isNoArgs)
                            innerMainArgs = {};
                        else if (isListArgs)
                            innerMainArgs = {innerBuilder.CreateLoad(
                                listStringType, innerArgsPtr, "args_val")};
                        else
                            innerMainArgs = {innerArgc, innerArgv};
                    }
                    else
                    {
                        if (isNoArgs)
                            innerMainArgs = {innerSelf};
                        else if (isListArgs)
                            innerMainArgs = {innerSelf, innerBuilder.CreateLoad(
                                listStringType, innerArgsPtr, "args_val")};
                        else
                            innerMainArgs = {innerSelf, innerArgc, innerArgv};
                    }
                    auto* innerResult = innerBuilder.CreateCall(
                        mainFn->getFunctionType(), mainFn, innerMainArgs, "main_result");
                    innerBuilder.CreateRet(innerResult);

                    // This call is C++ so clang owns the target personality and catch landing pad.
                    // A nonzero result means the C++ catch-all ran; converge on normal cleanup.
                    auto* guardResult = compiler->builder->CreateCall(
                        ehGuard->getFunctionType(), ehGuard, {innerFn, ehFrame, exitCodeGEP},
                        "eh_guard_result");
                    auto* guardNormalBB = llvm::BasicBlock::Create(
                        *compiler->context, "eh_guard_normal", trampolineFn);
                    auto* guardCatchBB = llvm::BasicBlock::Create(
                        *compiler->context, "eh_guard_catch", trampolineFn);
                    auto* guardCaught = compiler->builder->CreateICmpNE(
                        guardResult, compiler->builder->getInt32(0), "eh_caught");
                    compiler->builder->CreateCondBr(guardCaught, guardCatchBB, guardNormalBB);

                    compiler->builder->SetInsertPoint(guardCatchBB);
                    compiler->builder->CreateStore(
                        llvm::ConstantInt::get(i32Type, static_cast<uint64_t>(-1), true),
                        exitCodeGEP);
                    compiler->builder->CreateBr(cleanupBB);

                    compiler->builder->SetInsertPoint(guardNormalBB);
                    compiler->builder->CreateBr(cleanupBB);
                }
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
                    auto* streamPtrTy = cflat_llvm::PointerTo(streamTy);
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
            // ~list$string frees under the CRT allocator the caller built the args with; the
            // argv array (which only borrows each element's _ptr) was just freed above.
            if (!isListArgs)
            {
                if (auto* argsDtor = compiler->GetOrCreateFullDestructor(listStringTypeName))
                    compiler->builder->CreateCall(argsDtor->getFunctionType(), argsDtor, {argsGEP});
            }

            compiler->builder->CreateCall(freeFn->getFunctionType(), freeFn, {ctxArg});
            auto* finalExitCode = compiler->builder->CreateLoad(i32Type, exitCodeGEP, "final_exit");
            compiler->CreateReturnCall(finalExitCode);
            compiler->CreateBlockBreak(nullptr, true);
        }

        // ======================================================================
        // EMIT run(): bool run(Name* this, list$string args)
        // Allocates args packet, spawns thread into self->_thread, returns
        // whether the thread started. Does NOT join - caller uses WaitForExit().
        // ======================================================================
        {
            LLVMBackend::TypeAndValue boolReturn;  boolReturn.TypeName = "bool";
            LLVMBackend::DeclTypeAndValue thisParam;
            thisParam.TypeName = name;  thisParam.VariableName = name + "__";  thisParam.Pointer = true;
            LLVMBackend::DeclTypeAndValue argsParam;
            argsParam.TypeName = listStringTypeName;  argsParam.VariableName = "args";
            argsParam.IsMove = true;  // run() takes ownership; caller's list is zeroed after the call

            RejectIfProgramMemberSlotTaken(ctx, name, "run", "bool run(move list<string>)",
                boolReturn, {thisParam, argsParam});
            auto* runFn = compiler->CreateFunctionDefinition("run", boolReturn, {thisParam, argsParam});
            compiler->programTable[name].RunFunction = runFn;

            auto* thisArg = runFn->getArg(0);   // Name*
            auto* argsArg = runFn->getArg(1);   // list$string by value (move)

            // Malloc the args packet (raw malloc - tracked alloc is per-thread)
            auto* pkgSize = compiler->GetTypeSizeBytes(runArgsType);
            auto* pkgRaw  = compiler->builder->CreateCall(
                mallocFn->getFunctionType(), mallocFn, {pkgSize}, "pkg_raw");
            auto* pkg = compiler->builder->CreateBitCast(pkgRaw, cflat_llvm::PointerTo(runArgsType), "pkg");

            // Store this -> pkg->self (field 0)
            auto* selfGEP = compiler->builder->CreateStructGEP(runArgsType, pkg, 0, "pkg_self_gep");
            compiler->builder->CreateStore(thisArg, selfGEP);

            // Store args -> pkg->args (field 1): use the original argument value (pre-alloca copy)
            auto* argsGEP = compiler->builder->CreateStructGEP(runArgsType, pkg, 1, "pkg_args_gep");
            compiler->builder->CreateStore(argsArg, argsGEP);

            // Zero run()'s args alloca so ~list$string is a no-op at scope exit.
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
                inboxArenaField.TypeName     = ArenaChannelTypeName(*compiler);
                inboxArenaField.VariableName = "inbox";
                inboxArenaField.Pointer      = true;
                inboxArenaFieldIndex = (unsigned)declList.size();
                declList.push_back(inboxArenaField);

                LLVMBackend::DeclTypeAndValue outboxField;
                outboxField.TypeName     = ArenaChannelTypeName(*compiler);
                outboxField.VariableName = "outbox";
                outboxField.Pointer      = true;
                outboxFieldIndex = (unsigned)declList.size();
                declList.push_back(outboxField);
            }
        }

        auto* structType = compiler->CreateStructType(name, declList);
        if (structType->isOpaque())
            structType->setBody(llvm::ArrayRef<llvm::Type*>());
        compiler->FlushPendingFunctionDeclarations();

        // Create default constructor
        {
            LLVMBackend::TypeAndValue returnType;
            returnType.TypeName = name;
            compiler->CreateFunctionDefinition(name, returnType, {});

            std::vector<llvm::Value*> initializers;
            std::vector<char> initializerUnsigned;
            for (auto& typeValue : declList)
            {
                llvm::Value* rvalue = nullptr;
                bool fieldSrcUnsigned = false;
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
                        rvalue = ParseFieldDefaultInitializer(name, typeValue, ae, &fieldSrcUnsigned);
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
                initializerUnsigned.push_back(fieldSrcUnsigned ? 1 : 0);
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
                    rvalue = compiler->Upconvert(rvalue, destType,
                        idx < initializerUnsigned.size() && initializerUnsigned[idx] != 0);
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
                auto* nullStream = llvm::Constant::getNullValue(cflat_llvm::PointerTo(streamTy));
                structVal = compiler->CreateInsertValue(structVal, nullStream, outFieldIndex);
                structVal = compiler->CreateInsertValue(structVal, nullStream, inStreamFieldIndex);
            }

            // Allocate a non-null arena_channel shell: send/recv are no-ops on the uninitialized shell,
            // but a non-null `this` prevents crashes if recv/send is called before `>>` wires the channel.
            if (inboxArenaFieldIndex != (unsigned)-1)
            {
                auto* arenaTy = compiler->GetDataStructure(
                    ArenaChannelTypeName(*compiler)).StructType;
                llvm::Value* shell = EmitArenaChannelShellAlloc(compiler);
                if (!shell) shell = llvm::Constant::getNullValue(cflat_llvm::PointerTo(arenaTy));
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
        if (ctx->children.empty() || ctx->children[0]->getText() != "program")
            compiler->LogError("expected 'program' at the start of a program definition");
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
                inboxArenaField.TypeName     = ArenaChannelTypeName(*compiler);
                inboxArenaField.VariableName = "inbox";
                inboxArenaField.Pointer      = true;
                inboxArenaFieldIndex = (unsigned)declList.size();
                declList.push_back(inboxArenaField);

                LLVMBackend::DeclTypeAndValue outboxField;
                outboxField.TypeName     = ArenaChannelTypeName(*compiler);
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
        compiler->FlushPendingFunctionDeclarations();

        // Create default constructor (same pattern as ParseStructDefinition)
        {
            LLVMBackend::TypeAndValue returnType;
            returnType.TypeName = name;
            compiler->CreateFunctionDefinition(name, returnType, {});

            // User fields share the struct default ctor's field walk (a field initializer that
            // unwinds destroys the fields already built); synthetic fields are written below.
            llvm::Value* structVal = EmitAggregateFieldInitialization(name, structType, declList,
                                                                      exitCodeFieldIndex);

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
                auto* nullStream = llvm::Constant::getNullValue(cflat_llvm::PointerTo(streamTy));
                structVal = compiler->CreateInsertValue(structVal, nullStream, outFieldIndex);
                structVal = compiler->CreateInsertValue(structVal, nullStream, inStreamFieldIndex);
            }

            // Allocate a non-null arena_channel shell: send/recv are no-ops on the uninitialized shell,
            // but a non-null `this` prevents crashes if recv/send is called before `>>` wires the channel.
            if (inboxArenaFieldIndex != (unsigned)-1)
            {
                auto* arenaTy = compiler->GetDataStructure(
                    ArenaChannelTypeName(*compiler)).StructType;
                llvm::Value* shell = EmitArenaChannelShellAlloc(compiler);
                if (!shell) shell = llvm::Constant::getNullValue(cflat_llvm::PointerTo(arenaTy));
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
                std::string typeSig = SpellType(*compiler, field);
                LLVMBackend::TypeAndValue ownerType;
                ownerType.TypeName = name;
                const std::string displayFieldName = SpellType(*compiler, ownerType) + "." + field.VariableName;
                s->Register(SymbolKind::Field, name + "." + field.VariableName,
                            compiler->GetSourceFilePath(),
                            (int)ctx->getStart()->getLine(),
                            (int)ctx->getStart()->getCharPositionInLine(),
                            annSig + typeSig + " " + displayFieldName, {}, displayFieldName);
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

        // Flush instantiations (e.g. list$string from main's params) before emitting run()
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
                    SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = structName })));
            std::vector<std::string> valueParams;
            std::vector<std::string> valueDefaults;
            auto typeParams = ParseGenericTypeParameters(ctx->genericTypeParameters(), &valueParams,
                                                         &valueDefaults);
            genericClassTemplates[structName] = ctx;
            // Origin marker: a template DECLARED in a core library file. Read by
            // IsBorrowingContainerElementSink so a user type of the same name is not mistaken
            // for the core container.
            if (Compiler()->CurrentSourceIsCoreLibrary())
                Compiler()->gts.coreGenericTemplates.insert(structName);
            else
                Compiler()->gts.coreGenericTemplates.erase(structName);
            Compiler()->gts.genericTemplateNamespace[structName] = Compiler()->GetCurrentNamespace();
            Compiler()->RevokeGenericInterfaceInstances(structName);
            genericStructTypeParams[structName] = typeParams;
            genericStructValueParams[structName] = valueParams;
            Compiler()->gts.genericStructValueDefaults[structName] = valueDefaults;
            genericClassConstraints[structName] = ParseWhereClause(ctx->whereClause());
            ValidateGenericAggregateAliasNames(ctx, structName);
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
                LogErrorContext(ctx, "[winrt] class '"
                    + SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = structName })
                    + "' must implement exactly one interface");
                return;
            }
            if (ids.size() > 1)
            {
                LogErrorContext(ctx, "[winrt] class '"
                    + SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = structName })
                    + "' may implement only one interface in this milestone");
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
                        QueuePendingInstantiation("HResult", { arg }, mangled);
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
                    existing != nullptr && compiler->FunctionHasDefinition(existing))
                {
                    if (compiler->IsVerbose())
                        std::cout << "[verbose]     skipping duplicate struct definition: " << structName << "\n";
                    return;
                }
            }
        }

        if (compiler->IsVerbose())
            std::cout << "[verbose]     parse decl list: " << structName << "\n";

        LLVMBackend::AliasScopeGuard aliasScope(compiler);
        CollectAggregateAliases(ctx, structName);
        compiler->SaveAggregateAliasScope(structName);

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
                    if (spec->typeSpecifier() != nullptr)
                        element = CanonicalDeclarationTypeName(specs->declarationSpecifier(), spec->typeSpecifier()->getText());
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
        compiler->FlushPendingFunctionDeclarations();
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

            // The struct default ctor's field walk: a field initializer that unwinds destroys the
            // fields already built.
            llvm::Value* structVal = EmitAggregateFieldInitialization(structName, structType, declList);

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
                std::string typeSig = SpellType(*compiler, field);
                LLVMBackend::TypeAndValue ownerType;
                ownerType.TypeName = structName;
                const std::string displayFieldName = SpellType(*compiler, ownerType) + "." + field.VariableName;
                s->Register(SymbolKind::Field, structName + "." + field.VariableName,
                            compiler->GetSourceFilePath(),
                            (int)ctx->getStart()->getLine(),
                            (int)ctx->getStart()->getCharPositionInLine(),
                            annSig + typeSig + " " + displayFieldName, {}, displayFieldName);
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
                    std::string name = entry->typeSpecifier()
                        ? CanonicalTemplateTypeArgument(entry) : entry->getText();
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
                    ParseConstructorDefinition(func, structName, suppliesNoArgCtor, SIZE_MAX,
                                               hasExplicitNoArgCtor);
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
                    std::vector<std::string> valueParams;
                    std::vector<std::string> valueDefaults;
                    genericFunctionTypeParams[qualifiedName] =
                        ParseGenericTypeParameters(func->genericTypeParameters(), &valueParams,
                                                   &valueDefaults);
                    genericFunctionValueParams[qualifiedName] = valueParams;
                    Compiler()->gts.genericFunctionValueDefaults[qualifiedName] = valueDefaults;
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
                std::unordered_map<std::string, std::string> ifaceValueSubstitutions;
                std::unordered_map<std::string, std::vector<std::string>> ifacePackSubstitutions;
                const auto& ifaceTypeParams = genericInterfaceTypeParams[ifaceBaseName];
                const auto& ifaceValueParams = genericInterfaceValueParams[ifaceBaseName];
                auto ifacePackIdxIt = genericInterfacePackIndex.find(ifaceBaseName);
                size_t ifacePackIdx = (ifacePackIdxIt != genericInterfacePackIndex.end())
                                      ? ifacePackIdxIt->second : std::string::npos;
                if (ifacePackIdx == std::string::npos)
                {
                    for (size_t i = 0; i < ifaceTypeParams.size() && i < concreteTypeArgs.size(); i++)
                    {
                        if (i < ifaceValueParams.size() && !ifaceValueParams[i].empty())
                            ifaceValueSubstitutions[ifaceTypeParams[i]] = concreteTypeArgs[i];
                        else ifaceSubstitutions[ifaceTypeParams[i]] = concreteTypeArgs[i];
                    }
                }
                else
                {
                    for (size_t i = 0; i < ifacePackIdx && i < concreteTypeArgs.size(); i++)
                        ifaceSubstitutions[ifaceTypeParams[i]] = concreteTypeArgs[i];
                    ifacePackSubstitutions[ifaceTypeParams[ifacePackIdx]] =
                        std::vector<std::string>(concreteTypeArgs.begin() + ifacePackIdx, concreteTypeArgs.end());
                }

                InstantiateGenericInterface(ifaceBaseName, ifaceName, ifaceSubstitutions,
                    ifacePackSubstitutions, ifaceValueSubstitutions);
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

std::vector<std::string> MainListener::ParseGenericTypeParameters(
    CFlatParser::GenericTypeParametersContext* genericParams, std::vector<std::string>* valueTypes,
    std::vector<std::string>* valueDefaults) {
        std::vector<std::string> typeParams;
        if (valueTypes != nullptr) valueTypes->clear();
        if (valueDefaults != nullptr) valueDefaults->clear();
        if (!genericParams)
            return typeParams;

        auto typeParamList = genericParams->typeParameterList();
        if (!typeParamList)
            return typeParams;

        bool seenPack = false;
        for (auto* entry : typeParamList->typeParameterEntry())
        {
            if (IsValueParameterDeclaration(entry))
            {
                if (ValueParameterTypeSpelling(entry).empty())
                {
                    LogErrorContext(entry, "value parameter must use an integral type: char, short, int, long, bool, i8, i16, i32, i64, i128, u8, u16, u32, u64, u128");
                    continue;
                }
                if (seenPack)
                {
                    LogErrorContext(entry, "Only the last type parameter may be a pack (T...)");
                    continue;
                }
                typeParams.push_back(entry->valueParameterDeclaration()->Identifier()->getText());
                if (valueTypes != nullptr) valueTypes->push_back(ValueParameterTypeSpelling(entry));
                if (valueDefaults != nullptr)
                {
                    std::string defaultSpelling;
                    if (auto* defExpr = entry->valueParameterDeclaration()->shiftExpression())
                    {
                        auto folded = FoldCompileTimeInt(Compiler(entry), defExpr);
                        if (!folded)
                            LogErrorContext(entry, std::format(
                                "default for value parameter '{}' must be a compile-time constant "
                                "(got '{}')", typeParams.back(), defExpr->getText()));
                        else
                            defaultSpelling = std::to_string(*folded);
                    }
                    valueDefaults->push_back(defaultSpelling);
                }
                continue;
            }
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
            if (valueTypes != nullptr) valueTypes->push_back("");
            if (valueDefaults != nullptr) valueDefaults->push_back("");
            if (entry->Ellipsis() != nullptr)
            {
                seenPack = true;
                // The pack machinery binds every parameter through the TYPE substitution map, so
                // a value parameter alongside it would silently never be bound. Say so here
                // rather than let the body fail with "Undefined variable N".
                if (valueTypes != nullptr)
                    for (const auto& vt : *valueTypes)
                        if (!vt.empty())
                        {
                            LogErrorContext(entry, "a compile-time value parameter cannot be "
                                "combined with a parameter pack (T...) in the same generic");
                            break;
                        }
            }
        }

        // A default is only meaningful on a TRAILING parameter: a use site fills omitted
        // arguments from the right, so a defaulted parameter followed by a non-defaulted one
        // could never be omitted.
        if (valueDefaults != nullptr)
        {
            bool seenDefault = false;
            for (size_t i = 0; i < valueDefaults->size(); i++)
            {
                if (!(*valueDefaults)[i].empty()) { seenDefault = true; continue; }
                if (seenDefault)
                {
                    LogErrorContext(genericParams, std::format(
                        "generic parameter '{}' has no default but follows a parameter that does; "
                        "only trailing parameters may carry a default", typeParams[i]));
                    valueDefaults->assign(valueDefaults->size(), std::string{});
                    break;
                }
            }
        }

        return typeParams;
    }

std::unordered_map<std::string, std::vector<std::string>>
    MainListener::ParseWhereClause(CFlatParser::WhereClauseContext* wc) {
        std::unordered_map<std::string, std::vector<std::string>> result;
        if (!wc) return result;
        for (auto* constraint : wc->typeParameterConstraint())
        {
            if (constraint->assignmentExpression() != nullptr) continue;
            auto* target = constraint->genericIdentifier();
            auto* typeParam = constraint->Identifier();
            if (target == nullptr || typeParam == nullptr) continue;
            result[typeParam->getText()].push_back(target->getText());
        }
        return result;
    }

bool MainListener::CheckValueConstraints(
    const std::string& templateName,
    CFlatParser::WhereClauseContext* whereClause,
    const std::vector<std::string>& typeParams,
    const std::vector<std::string>& valueParams,
    const std::vector<std::string>& typeArgs) {
        if (whereClause == nullptr) return true;

        auto findValueParam = [&](antlr4::tree::ParseTree* node) {
            std::string found;
            auto visit = [&](auto&& self, antlr4::tree::ParseTree* current) -> void {
                if (current == nullptr || !found.empty()) return;
                if (auto* terminal = dynamic_cast<antlr4::tree::TerminalNode*>(current))
                {
                    for (size_t i = 0; i < typeParams.size() && i < valueParams.size(); i++)
                        if (!valueParams[i].empty() && terminal->getText() == typeParams[i])
                        {
                            found = typeParams[i];
                            return;
                        }
                }
                for (auto* child : current->children)
                    self(self, child);
            };
            visit(visit, node);
            return found;
        };

        std::string instantiation = templateName + "<";
        for (size_t i = 0; i < typeArgs.size(); i++)
        {
            if (i != 0) instantiation += ", ";
            instantiation += typeArgs[i];
        }
        instantiation += ">";

        for (auto* constraint : whereClause->typeParameterConstraint())
        {
            auto* predicate = constraint->assignmentExpression();
            if (predicate == nullptr) continue;

            auto folded = FoldCompileTimeInt(Compiler(), predicate);
            std::string constraintText = "where " + CollapsedSourceText(constraint);
            if (!folded)
            {
                Compiler()->LogError(std::format(
                    "constraint '{}' on '{}' does not fold to a compile-time constant",
                    constraintText, templateName));
                return false;
            }
            if (*folded != 0) continue;

            std::string valueName = findValueParam(predicate);
            std::string value = "0";
            for (size_t i = 0; i < typeParams.size() && i < valueParams.size(); i++)
                if (!valueName.empty() && typeParams[i] == valueName && i < typeArgs.size())
                    value = typeArgs[i];
            Compiler()->LogError(std::format(
                "constraint '{}' is not satisfied by '{}' ({} = {})",
                constraintText, instantiation, valueName.empty() ? "value" : valueName, value));
            return false;
        }
        return true;
    }

bool MainListener::CheckConstraints(
        const std::string& templateName,
        const std::vector<std::string>& typeParams,
        const std::vector<std::string>& typeArgs,
        const std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::string>>>& constraintMap,
        antlr4::ParserRuleContext* ctx) {
        auto cit = constraintMap.find(templateName);
        if (cit == constraintMap.end()) return true;

        std::unordered_map<std::string, std::string> substitutions;
        for (size_t i = 0; i < typeParams.size() && i < typeArgs.size(); i++)
            substitutions[typeParams[i]] = typeArgs[i];

        auto splitGeneric = [](const std::string& spelling, std::string& base,
                               std::vector<std::string>& args) {
            size_t lt = spelling.find('<');
            if (lt == std::string::npos || spelling.empty() || spelling.back() != '>')
                return false;
            base = spelling.substr(0, lt);
            args = SplitTopLevelTypeArgs(spelling.substr(lt + 1, spelling.size() - lt - 2));
            return !base.empty();
        };

        std::function<std::string(const std::string&)> substituteType;
        substituteType = [&](const std::string& spelling) {
            auto it = substitutions.find(spelling);
            if (it != substitutions.end()) return it->second;

            std::string base;
            std::vector<std::string> args;
            if (!splitGeneric(spelling, base, args)) return spelling;

            std::string result = base + "<";
            for (size_t i = 0; i < args.size(); i++)
            {
                if (i > 0) result += ",";
                result += substituteType(args[i]);
            }
            return result + ">";
        };

        std::function<std::string(const std::string&)> mangleType;
        mangleType = [&](const std::string& spelling) {
            std::string substituted = substituteType(spelling);
            std::string base;
            std::vector<std::string> args;
            if (!splitGeneric(substituted, base, args))
                return MangleTypeArg(Compiler(), substituted);

            std::vector<std::string> mangledArgs;
            for (const auto& arg : args)
            {
                std::string nestedBase;
                std::vector<std::string> nestedArgs;
                if (splitGeneric(arg, nestedBase, nestedArgs))
                    mangledArgs.push_back(mangleType(arg));
                else
                    mangledArgs.push_back(MangleTypeArg(Compiler(), arg));
            }
            return MangledGenericName(base, mangledArgs);
        };

        for (size_t i = 0; i < typeParams.size() && i < typeArgs.size(); i++)
        {
            auto pit = cit->second.find(typeParams[i]);
            if (pit == cit->second.end()) continue;
            for (const auto& iface : pit->second)
            {
                std::string sourceIface = substituteType(iface);
                std::string concreteIface = iface;
                std::string base;
                std::vector<std::string> args;
                if (splitGeneric(sourceIface, base, args))
                {
                    std::vector<std::string> mangledArgs;
                    for (const auto& arg : args)
                        mangledArgs.push_back(mangleType(arg));
                    concreteIface = MangledGenericName(base, mangledArgs);
                }

                if (!Compiler()->TypeImplementsInterface(typeArgs[i], concreteIface))
                {
                    Compiler()->LogError(std::format(
                        "type '{}' does not implement '{}', required by constraint 'where {} : {}'",
                        typeArgs[i], sourceIface, typeParams[i], iface));
                    return false;
                }
            }
        }
        return true;
    }

void MainListener::EmitCppStructConstructorThunk(
    antlr4::ParserRuleContext* ctx, CFlatParser::BlockItemListContext* body,
    const std::string& structName,
    const std::vector<LLVMBackend::DeclTypeAndValue>& fields,
    const std::vector<LLVMBackend::DeclTypeAndValue>& params,
    size_t ctorIndex)
{
        auto* compiler = Compiler(ctx);
        LLVMBackend::DeclTypeAndValue thisParam;
        thisParam.TypeName = structName;
        thisParam.VariableName = structName + "__";
        thisParam.Pointer = true;
        std::vector<LLVMBackend::TypeAndValue> allParams{ thisParam };
        allParams.insert(allParams.end(), params.begin(), params.end());
        LLVMBackend::TypeAndValue voidReturn{ .TypeName = "void" };
        const std::string thunkName = "__cflat_ctor_" + CppStructThunkStem(structName)
            + "_" + std::to_string(ctorIndex);
        auto fn = compiler->CreateFunctionDefinition(thunkName, voidReturn, allParams,
                                                      true, false, ctx->getStart()->getLine());
        if (fn->isMaterializable()
            || (!fn->empty() && cflat_llvm::GetTerminatorOrNull(&fn->getEntryBlock()) != nullptr))
            return;
        compiler->InitializeBlock(&fn->front(), false);
        LLVMBackend::AliasScopeGuard functionAliasScope(compiler);
        ReturnFlagGuard functionReturnFlagGuard(&straightLineReturned_);
        straightLineReturned_ = false;
        GlobalScopeGuard functionScope(global_scope);
        global_scope = false;

        auto* structType = compiler->GetDataStructure(structName).StructType;
        auto* dstArg = compiler->FindLiveNamedVariable(structName + "__");
        llvm::Value* dst = nullptr;
        if (dstArg != nullptr)
            dst = dstArg->Storage != nullptr
                ? compiler->CreateLoad(dstArg->BaseType, dstArg->Storage) : dstArg->Primary;
        if (structType == nullptr || dst == nullptr)
        {
            compiler->LogErrorMessage("C++ struct constructor thunk has no destination storage");
            compiler->CreateReturnCall(nullptr);
            compiler->CreateBlockBreak(nullptr, true);
            compiler->ClearCurrentSubprogram();
            return;
        }
        compiler->RegisterThisPointer(thisParam, dst, structType);
        uint64_t fieldStart = 0;
        uint64_t fieldBytes = 0;
        if (compiler->GetGeneratedCxxFieldBlock(structName, fieldStart, fieldBytes))
        {
            auto* bytePtr = compiler->builder->CreateBitCast(
                dst, llvm::PointerType::get(compiler->builder->getInt8Ty(), 0));
            auto* blockPtr = compiler->builder->CreateInBoundsGEP(
                compiler->builder->getInt8Ty(), bytePtr,
                compiler->builder->getInt64(fieldStart), "cflat_ctor_fields");
            compiler->builder->CreateMemSet(blockPtr, compiler->builder->getInt8(0),
                compiler->builder->getInt64(fieldBytes), llvm::Align(1));
        }

        // The C++ constructor never completes if a field initializer or the body throws, so C++
        // runs no destructor thunk: an unwind destroys the CFlat fields already built here.
        LLVMBackend::UnwindPartialScope builtFields(*compiler);
        for (size_t fieldIndex = 0; fieldIndex < fields.size(); ++fieldIndex)
        {
            if (fieldIndex >= structType->getNumElements()) break;
            const auto& field = fields[fieldIndex];
            if (!field.IsCflatOwned || field.IsPadding) continue;
            auto* destinationType = structType->getTypeAtIndex((unsigned)fieldIndex);
            auto* fieldPtr = compiler->builder->CreateStructGEP(structType, dst,
                (unsigned)fieldIndex, field.VariableName);
            auto* fieldInit = field.Initializer;
            const bool defaultOnly = fieldInit == nullptr
                || fieldInit->Default() != nullptr;
            if (defaultOnly && !destinationType->isArrayTy()
                && EmitNontrivialCxxDefaultAt(fieldPtr, field))
            {
                compiler->NoteUnwindPartial(LLVMBackend::UnwindPartialEntry::Kind::Slot,
                                            fieldPtr, field.TypeName);
                continue;
            }
            llvm::Value* value = nullptr;
            bool valueUnsigned = false;
            bool fromBraceList = false;
            if (auto* brace = FieldDefaultBraceList(field))
            {
                value = ParseFieldDefaultBraceInitializer(structName, field, brace);
                fromBraceList = true;
            }
            else if (field.Initializer != nullptr)
            {
                if (auto* assignment = field.Initializer->assignmentExpression())
                    value = ParseFieldDefaultInitializer(structName, field, assignment,
                                                         &valueUnsigned);
                else if (field.Initializer->Default() != nullptr)
                    value = GenerateDefaultValue(field);
            }
            if (value == nullptr && (destinationType->isStructTy() || destinationType->isArrayTy()))
                value = GenerateDefaultValue(field);
            if (value == nullptr) continue;
            if (!fromBraceList)
                value = compiler->Upconvert(value, destinationType, valueUnsigned);
            if (value->getType() != destinationType)
            {
                if (destinationType->isStructTy())
                    value = GenerateDefaultValue(field);
                else
                    value = compiler->CreateCast(value, destinationType);
            }
            if (value != nullptr && value->getType() == destinationType)
            {
                compiler->builder->CreateStore(value, fieldPtr);
                if (destinationType->isArrayTy())
                {
                    llvm::Type* elemTy = nullptr;
                    const uint64_t n = compiler->PeelFixedArrayType(destinationType, elemTy);
                    compiler->NoteUnwindArrayPrefix(fieldPtr, elemTy,
                                                    compiler->builder->getInt64(n), field.TypeName);
                }
                else if (destinationType->isStructTy())
                    compiler->NoteUnwindPartial(LLVMBackend::UnwindPartialEntry::Kind::Slot,
                                                fieldPtr, field.TypeName);
            }
        }

        if (body != nullptr) ParseBlockItemList(body);
        compiler->CreateReturnCall(nullptr);
        compiler->CreateBlockBreak(nullptr, true);
        compiler->ClearCurrentSubprogram();
}

void MainListener::EmitCppStructDestructorThunk(
    antlr4::ParserRuleContext* ctx, const std::string& structName)
{
        auto* compiler = Compiler(ctx);
        LLVMBackend::DeclTypeAndValue thisParam;
        thisParam.TypeName = structName;
        thisParam.VariableName = structName + "__";
        thisParam.Pointer = true;
        LLVMBackend::TypeAndValue voidReturn{ .TypeName = "void" };
        const std::string thunkName = "__cflat_dtor_" + CppStructThunkStem(structName);
        auto fn = compiler->CreateFunctionDefinition(thunkName, voidReturn, { thisParam },
                                                      true, false, ctx->getStart()->getLine());
        if (fn->isMaterializable()
            || (!fn->empty() && cflat_llvm::GetTerminatorOrNull(&fn->getEntryBlock()) != nullptr))
            return;
        compiler->InitializeBlock(&fn->front(), false);
        LLVMBackend::AliasScopeGuard functionAliasScope(compiler);
        ReturnFlagGuard functionReturnFlagGuard(&straightLineReturned_);
        straightLineReturned_ = false;
        GlobalScopeGuard functionScope(global_scope);
        global_scope = false;
        auto* dstArg = compiler->FindLiveNamedVariable(structName + "__");
        llvm::Value* dst = dstArg != nullptr
            ? (dstArg->Storage != nullptr
                ? compiler->CreateLoad(dstArg->BaseType, dstArg->Storage) : dstArg->Primary)
            : nullptr;
        auto* structType = compiler->GetDataStructure(structName).StructType;
        if (dst == nullptr || structType == nullptr)
            compiler->LogErrorMessage("C++ struct destructor thunk has no destination storage");
        else
        {
            compiler->RegisterThisPointer(thisParam, dst, structType);
            if (auto* userDtor = compiler->GetFunction("~" + structName))
                compiler->builder->CreateCall(userDtor->getFunctionType(), userDtor, { dst });
            if (cflat_llvm::GetTerminatorOrNull(compiler->builder->GetInsertBlock()) == nullptr)
                compiler->EmitCflatOwnedFieldsDestruction(*compiler->builder, structName, dst);
        }
        compiler->CreateReturnCall(nullptr);
        compiler->CreateBlockBreak(nullptr, true);
        compiler->ClearCurrentSubprogram();
}

void MainListener::EmitCppStructMoveThunk(antlr4::ParserRuleContext* ctx,
                                          const std::string& structName)
{
        auto* compiler = Compiler(ctx);
        LLVMBackend::DeclTypeAndValue dstParam;
        dstParam.TypeName = structName;
        dstParam.VariableName = "dst";
        dstParam.Pointer = true;
        LLVMBackend::DeclTypeAndValue srcParam = dstParam;
        srcParam.VariableName = "src";
        LLVMBackend::TypeAndValue voidReturn{ .TypeName = "void" };
        const std::string thunkName = "__cflat_move_" + CppStructThunkStem(structName);
        auto fn = compiler->CreateFunctionDefinition(thunkName, voidReturn,
                                                      { dstParam, srcParam }, true, false,
                                                      ctx->getStart()->getLine());
        if (fn->isMaterializable()
            || (!fn->empty() && cflat_llvm::GetTerminatorOrNull(&fn->getEntryBlock()) != nullptr))
            return;
        compiler->InitializeBlock(&fn->front(), false);
        LLVMBackend::AliasScopeGuard functionAliasScope(compiler);
        auto getPointer = [&](const char* name) -> llvm::Value* {
            auto* arg = compiler->FindLiveNamedVariable(name);
            if (arg == nullptr) return nullptr;
            return arg->Storage != nullptr
                ? compiler->CreateLoad(arg->BaseType, arg->Storage) : arg->Primary;
        };
        auto* dst = getPointer("dst");
        auto* src = getPointer("src");
        auto* structType = compiler->GetDataStructure(structName).StructType;
        if (dst == nullptr || src == nullptr || structType == nullptr)
        {
            compiler->LogErrorMessage("C++ struct move thunk has no source or destination storage");
        }
        else
        {
            uint64_t fieldStart = 0;
            uint64_t fieldBytes = 0;
            if (compiler->GetGeneratedCxxFieldBlock(structName, fieldStart, fieldBytes))
            {
                auto* dstBytes = compiler->builder->CreateBitCast(
                    dst, llvm::PointerType::get(compiler->builder->getInt8Ty(), 0));
                auto* srcBytes = compiler->builder->CreateBitCast(
                    src, llvm::PointerType::get(compiler->builder->getInt8Ty(), 0));
                auto* dstBlock = compiler->builder->CreateInBoundsGEP(
                    compiler->builder->getInt8Ty(), dstBytes,
                    compiler->builder->getInt64(fieldStart), "cflat_move_dst");
                auto* srcBlock = compiler->builder->CreateInBoundsGEP(
                    compiler->builder->getInt8Ty(), srcBytes,
                    compiler->builder->getInt64(fieldStart), "cflat_move_src");
                auto size = llvm::ConstantInt::get(compiler->builder->getInt64Ty(), fieldBytes);
                compiler->builder->CreateMemCpy(dstBlock, llvm::Align(1), srcBlock,
                                                 llvm::Align(1), size);
                compiler->builder->CreateMemSet(srcBlock, compiler->builder->getInt8(0),
                                                size, llvm::Align(1));
            }
        }
        compiler->CreateReturnCall(nullptr);
        compiler->CreateBlockBreak(nullptr, true);
        compiler->ClearCurrentSubprogram();
}

void MainListener::ParseConstructorDefinition(CFlatParser::FunctionDefinitionContext* func,
                                               const std::string& structName,
                                               bool suppliesNoArgCtor,
                                               size_t cppCtorIndex,
                                               bool hasExplicitNoArgCtor) {
        auto* compiler = Compiler(func);
        if (func->baseSpecifier() != nullptr && !compiler->HasTypeAnnotation(structName, "cpp"))
        {
            Compiler(func)->LogErrorMessage("base initializer is only valid in a [cpp] struct");
            return;
        }
        auto params = ParseParameterTypeList(func->parameterTypeList());
        if (cppCtorIndex != SIZE_MAX)
        {
            LLVMBackend::CppStructAccessScope cppStructAccessScope(compiler, structName);
            auto fields = compiler->GetDataStructure(structName).StructFields;
            EmitCppStructConstructorThunk(func, func->compoundStatement()->blockItemList(),
                                           structName, fields, params, cppCtorIndex);
            return;
        }
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
        if (fn->isMaterializable()
            || (!fn->empty() && cflat_llvm::GetTerminatorOrNull(&fn->getEntryBlock()) != nullptr))
            return;

        compiler->InitializeBlock(&fn->front(), false);
        LLVMBackend::AliasScopeGuard functionAliasScope(compiler);
        // Fresh straight-line for this function/lambda body; restore the enclosing walk's flag on
        // exit so a nested lambda's return does not leak into the surrounding expression.
        ReturnFlagGuard functionReturnFlagGuard(&straightLineReturned_);
        straightLineReturned_ = false;

        // Get the struct's LLVM type (without pointer)
        auto* structLLVMType = llvm::cast<llvm::StructType>(compiler->GetType(returnType, nullptr, false));

        // Alloca the struct so we can GEP into fields via 'this'
        auto* thisAlloca = compiler->AllocaAtEntry(structLLVMType, nullptr, structName + "__");

        // An unwind out of a field initializer destroys the fields already built; one out of
        // the body destroys every member but never runs the user ~T (construction never ended).
        LLVMBackend::UnwindPartialScope partialCtor(*compiler);
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
                bool fieldValSrcUnsigned = false;
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
                        fieldVal = ParseFieldDefaultInitializer(structName, field, assignExpr,
                            &fieldValSrcUnsigned);
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
                        fieldVal = compiler->Upconvert(fieldVal, destType, fieldValSrcUnsigned);
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
                                    SpellType(*compiler, field), field.VariableName));
                            fieldVal = compiler->CreateCast(fieldVal, destType);
                        }
                    }
                    if (fieldVal->getType() == destType)
                    {
                        auto* fieldPtr = compiler->builder->CreateStructGEP(
                            structLLVMType, thisAlloca, fieldIdx, field.VariableName);
                        compiler->builder->CreateStore(fieldVal, fieldPtr);
                        compiler->NoteUnwindPartial(LLVMBackend::UnwindPartialEntry::Kind::Value,
                                                    fieldVal, field.TypeName);
                    }
                }
                fieldIdx++;
            }
        }
        else if (hasExplicitNoArgCtor)
        {
            auto fields = compiler->GetDataStructure(structName).StructFields;
            auto* fieldValue = EmitAggregateFieldInitialization(
                structName, structLLVMType, fields);
            if (fieldValue)
                compiler->builder->CreateStore(fieldValue, thisAlloca);
        }
        else
        {
            // With no user no-arg ctor, the synthesized ctor remains the field-init path.
            auto* defaultVal = compiler->CreateOverloadedFunctionCall(structName, {});
            if (defaultVal)
                compiler->builder->CreateStore(defaultVal, thisAlloca);
        }

        partialCtor.Release();
        compiler->NoteUnwindPartial(LLVMBackend::UnwindPartialEntry::Kind::Members, thisAlloca,
                                    structName);

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
        if (compiler->HasTypeAnnotation(structName, "cpp"))
        {
            LLVMBackend::DeclTypeAndValue thisParam;
            thisParam.TypeName = structName;
            thisParam.VariableName = structName + "__";
            thisParam.Pointer = true;
            LLVMBackend::TypeAndValue returnType{ .TypeName = "void" };
            const std::string fullName = "~" + structName;
            auto fn = compiler->CreateFunctionDefinition(fullName, returnType, { thisParam },
                                                         false, false,
                                                         static_cast<int>(ctx->getStart()->getLine()));
            compiler->InitializeBlock(&fn->front(), false);
            LLVMBackend::AliasScopeGuard functionAliasScope(compiler);
            LLVMBackend::CppStructAccessScope cppStructAccessScope(compiler, structName);
            ReturnFlagGuard functionReturnFlagGuard(&straightLineReturned_);
            straightLineReturned_ = false;
            if (auto* body = ctx->compoundStatement()->blockItemList())
                ParseBlockItemList(body);
            compiler->CreateReturnCall(nullptr);
            compiler->CreateBlockBreak(nullptr, true);
            compiler->ClearCurrentSubprogram();
            EmitCppStructDestructorThunk(ctx, structName);
            return;
        }
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
        LLVMBackend::AliasScopeGuard functionAliasScope(compiler);
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
        LLVMBackend::AliasScopeGuard functionAliasScope(compiler);
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
        if (cflat_llvm::GetTerminatorOrNull(compiler->builder->GetInsertBlock()) != nullptr)
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

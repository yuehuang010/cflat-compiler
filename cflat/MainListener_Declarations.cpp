#include "MainListener.h"

LLVMBackend* MainListener::Compiler(antlr4::ParserRuleContext* ctx) {
        if (ctx)
            compilerLLVM->SetSourceLocation(ctx->getStart()->getLine(), ctx->getStart()->getCharPositionInLine());
        return compilerLLVM;
    }

static bool IsFunctionBodyDeclaration(
    CFlatParser::DeclarationSpecifiersContext* declSpecs)
{
        bool insideFunction = false;
        for (auto* parent = declSpecs->parent; parent != nullptr; parent = parent->parent)
        {
            if (dynamic_cast<CFlatParser::AggregateMemberContext*>(parent) != nullptr)
                return false;
            if (dynamic_cast<CFlatParser::ParameterDeclarationContext*>(parent) != nullptr)
                return false;
            if (dynamic_cast<CFlatParser::FunctionDefinitionContext*>(parent) != nullptr)
                insideFunction = true;
        }
        return insideFunction
            && dynamic_cast<CFlatParser::FunctionDefinitionContext*>(declSpecs->parent) == nullptr;
    }

static bool IsCoreUniqueArrayViewInstantiation(
    const LLVMBackend* compiler,
    const std::string& mangledName,
    const std::vector<std::string>& typeArgs) {
        return compiler != nullptr && compiler->IsCoreUniqueType(mangledName)
            && typeArgs.size() == 1 && typeArgs[0].ends_with("[]");
    }

static std::string CoreUniqueArrayViewMessage(const std::vector<std::string>& typeArgs) {
        return std::format(
            "'unique<{}>': array views are not supported - a view does not own its buffer",
            typeArgs.empty() ? std::string() : typeArgs[0]);
    }

bool MainListener::InGenericInstantiation() const {
        return !activeTypeSubstitutions.empty() || !activeValueSubstitutions.empty()
            || !activePackSubstitutions.empty();
}

void MainListener::RecordLoopExitMovedState() {
        if (loopBreakMovedStates_.empty()) return;
        loopBreakMovedStates_.back().push_back(Compiler()->SaveMovedState());
    }

void MainListener::ApplyVectorizeFpTier(LLVMBackend* compiler, VectorizeFpTier tier) {
        if (tier == VectorizeFpTier::None) return;
        llvm::FastMathFlags f = compiler->builder->getFastMathFlags();
        f.setAllowContract(true);
        if (tier == VectorizeFpTier::Reassoc)
            f.setAllowReassoc(true);
        compiler->builder->setFastMathFlags(f);
    }

std::string MainListener::ResolveTypeArgEntry(CFlatParser::TypeParameterEntryContext* entry) {
        if (entry->shiftExpression() != nullptr && entry->typeSpecifier() == nullptr)
        {
            auto folded = FoldCompileTimeInt(Compiler(entry), entry->shiftExpression());
            if (!folded)
            {
                LogErrorContext(entry, std::format("value argument '{}' does not fold to an integer",
                    entry->getText()));
                return "0";
            }
            return std::to_string(*folded);
        }
        auto* typeSpec = entry->typeSpecifier();
        // `unique` ownership qualifier (D10): a leading Identifier == "unique". Any other leading
        // Identifier is an unknown qualifier.
        bool isUnique = TypeArgHasUnique(entry);
        // `alias` as a generic type ARGUMENT is a pure-borrow element spelling (list<alias T*>):
        // behaviorally identical to a bare pointer element, but instantiated distinctly so a later
        // `delete` of a local bound to a get()/[] result can be rejected. Exclusive with `unique`.
        bool isAlias = TypeArgHasAlias(entry);
        if (isUnique && isAlias)
            LogErrorContext(entry, "'unique' and 'alias' cannot both qualify a generic type argument");
        else if (entry->Identifier() != nullptr && !isUnique && !isAlias)
            LogErrorContext(entry, std::format("unknown type qualifier '{}'; only 'unique' or 'alias' "
                "is allowed on a generic type argument", entry->Identifier()->getText()));
        // Written stars are COUNTED, not flagged: Box<C**> must reach MangleTypeArg as "C**" so it
        // mangles Box$.p$.p$C and becomes its own instantiation.
        int pointerDepth = PointerDepthOf(entry->pointer());
        bool hasPointer = pointerDepth > 0;
        // Now that the stars are COUNTED here, this spelling joins the others that count them: a
        // written depth of 3+ is refused rather than clamped (the type model holds two levels).
        if (pointerDepth > PointerDepthCap)
            LogErrorContext(entry, PointerDepthCapMessage(
                std::format("generic type argument '{}'", entry->getText()), pointerDepth));
        // `T[]` as a generic/tuple type arg is a noalias array-view member; `[N]` and `[]*`
        // are not valid in a type-argument position (reject with one clear message).
        bool hasArrayView = IsArrayViewArg(entry);
        // On a view the WRITTEN stars decorate the element (`int*[]` is a view of `int*`); a
        // substituted arg supplies its own element depth below. -1 = no substitution seen.
        int substElemPtrDepth = -1;
        if (IsBadArrayArg(entry))
            LogErrorContext(entry, "'T[]' is the only bracketed form valid as a generic or tuple "
                "type argument; a sized 'T[N]', an unsized multi-dimensional 'T[][]' and a "
                "pointer-to-array 'T[]*' are not");
        std::string resolved;

        std::string innerBase;
        if (auto* innerParams = GenericSpecOf(typeSpec, innerBase))
        {
            // Nested generic (e.g., Box<T>): recurse into each type argument
            innerBase = Compiler()->ResolveGenericBaseAlias(innerBase);
            std::vector<std::string> innerArgs;
            for (auto* innerEntry : innerParams->typeParameterList()->typeParameterEntry())
                innerArgs.push_back(ResolveTypeArgEntry(innerEntry));
            resolved = MangledGenericName(innerBase, innerArgs);
            if (IsCoreUniqueArrayViewInstantiation(Compiler(), resolved, innerArgs))
                LogErrorContext(entry, CoreUniqueArrayViewMessage(innerArgs));
            // A generic used as a type argument to another generic (e.g. list<int>
            // inside list<list<int>>) must itself be instantiated; otherwise the
            // outer instantiation's element type resolves to nothing and codegen
            // reports "unknown type '<inner>'". Recursion above queues the deepest
            // levels first, so inner types are registered before the outer.
            QueueGenericInstantiation(innerBase, innerArgs, resolved, entry);
            // A qualified base is an imported winmd generic - build it through the winmd path.
            if (innerBase.find('.') != std::string::npos)
                Compiler()->InstantiateWinrtGenericInterface(innerBase, innerArgs, resolved);
        }
        else if (typeSpec && typeSpec->functionPointerSpecifier())
        {
            // Closure type as a generic argument (gap a): encode to a symbol-safe name and register
            // the call descriptor + backing value type. A trailing '*' is carried through as a
            // pointer suffix for the THIN spelling only (see RejectFatClosurePointerArg).
            auto* fpSpec = typeSpec->functionPointerSpecifier();
            if (isUnique)
                LogErrorContext(entry, "'unique' on a generic type argument: a function pointer or closure "
                    "does not own an allocation");
            std::string encodedArg = EncodeClosureCodegen(fpSpec);
            // An array-view is a thin `ptr` repr, so a FAT closure - a by-value struct - cannot be
            // viewed this way. The declarator guard says so; this funnel skipped the suffix.
            if (hasArrayView && fpSpec->Function() == nullptr)
                LogErrorContext(entry, std::format(
                    "array-view '[]' is not supported on closure type '{}'; "
                    "use a fixed size '{}[N]' instead",
                    typeSpec->getText(), typeSpec->getText()));
            if (!hasPointer)
                return encodedArg;
            RejectFatClosurePointerArg(entry, fpSpec->Function() != nullptr, typeSpec->getText());
            return encodedArg + "*";
        }
        else
        {
            // Simple type or type parameter: look up in activeTypeSubstitutions
            resolved = typeSpec ? typeSpec->getText() : entry->getText();
            auto substIt = activeTypeSubstitutions.find(resolved);
            bool substituted = substIt != activeTypeSubstitutions.end();
            if (substIt != activeTypeSubstitutions.end())
            {
                // A substituted arg may itself carry "*"/"[]" (e.g. a pack param bound to "int[]")
                // or an "alias " prefix; peel it onto flags so it re-encodes once below.
                bool substAlias = false;
                resolved = substIt->second;
                // Stars WRITTEN on a view arg (`T*[]`) are element stars already; the peel adds
                // any the substituted spelling itself carries (`T[]` with T bound to "int*").
                bool writtenView = hasArrayView;
                int writtenStars = pointerDepth;
                substElemPtrDepth = hasArrayView ? pointerDepth : 0;
                PeelTypeArgSuffix(resolved, pointerDepth, hasArrayView, &substAlias,
                                  &substElemPtrDepth);
                // `B<T*>` with T bound to a view DECAYS to the ELEMENT pointer, exactly as the
                // declarator arm does: the star consumes the view's own thin-pointer repr.
                if (!writtenView && hasArrayView && writtenStars > 0)
                {
                    hasArrayView = false;
                    pointerDepth = substElemPtrDepth + writtenStars;
                    substElemPtrDepth = -1;
                    if (pointerDepth > PointerDepthCap)
                        LogErrorContext(entry, PointerDepthCapMessage(
                            std::format("generic type argument '{}'", entry->getText()),
                            pointerDepth));
                }
                hasPointer = pointerDepth > 0;
                isAlias = isAlias || substAlias;
            }
            else if (entry->pointer() == nullptr && entry->arrayTypeSuffix() == nullptr
                     && entry->Identifier() == nullptr && typeSpec != nullptr
                     && typeSpec->genericIdentifier() != nullptr
                     && typeSpec->genericIdentifier()->genericTypeParameters() == nullptr)
            {
                auto valueIt = activeValueSubstitutions.find(resolved);
                if (valueIt != activeValueSubstitutions.end())
                    return valueIt->second;
                std::string base = Compiler(entry)->ResolveTypeArgBaseName(resolved);
                // Fold a pure-rename `using` into the ARGUMENT, not just into the mangled name:
                // the instantiation is queued and drained after the alias scope is gone, so a
                // body-scope alias spelling would no longer name anything by then.
                base = Compiler(entry)->ResolveManglingAlias(base);
                // A body-scope `using` the mangler could not pre-register (inside a generic
                // template, whose body names unbound parameters) is only in the scoped alias map;
                // fold it too, but only when it renames a plain type name.
                if (std::string aliased = Compiler(entry)->ResolveTypeAlias(base);
                    aliased != base && IsBareTypeName(aliased))
                    base = aliased;
                // `using U = T;` in a generic body folds to the template's own parameter, which
                // only names a type through the enclosing instantiation's substitutions.
                if (auto subIt = activeTypeSubstitutions.find(base);
                    subIt != activeTypeSubstitutions.end() && IsBareTypeName(subIt->second))
                    base = subIt->second;
                if (Compiler(entry)->IsTypeArgTypeKey(base)
                    || Compiler(entry)->IsKnownTypeName(base))
                    resolved = base;
                else if (auto folded = FoldCompileTimeInt(Compiler(entry), typeSpec->genericIdentifier()))
                    return std::to_string(*folded);
            }
            // A function-type alias (using IntFn = Lambda<int(int)>) used as a generic arg resolves
            // to the SAME encoded closure type as the direct spelling (canonicalization / gap a).
            if (!hasArrayView)
                if (auto* fit = Compiler(entry)->FindFunctionTypeAlias(resolved);
                    fit != nullptr)
                {
                    if (isUnique)
                        LogErrorContext(entry, "'unique' on a generic type argument: a function pointer or closure "
                            "does not own an allocation");
                    std::string encodedAlias = EncodeClosureFromSig(Compiler(entry), *fit);
                    if (!hasPointer)
                        return encodedAlias;
                    RejectFatClosurePointerArg(entry, fit->IsThinFnPtr(), resolved);
                    return encodedAlias + "*";
                }
            // Resolve the ARGUMENT the way the template BASE is resolved. A SUBSTITUTED argument
            // is already resolved in the CALLER's scope - re-resolving rebinds it to this one.
            if (!substituted)
                resolved = Compiler(entry)->ResolveTypeArgBaseName(resolved);
            // A POINTER alias (`using IP = int*;`) names ONE type, so `V<IP>` and `V<IP[]>` must
            // key like `V<int*>` / `V<int*[]>`. ResolveManglingAlias above folds pure RENAMES
            // only, so fold the stars here the way the declarator does (ResolveTypeAlias +
            // PeelAliasPointerStars) and move them onto the element of a view.
            if (!substituted)
            {
                if (hasArrayView && substElemPtrDepth < 0) substElemPtrDepth = pointerDepth;
                int& aliasStarSink = hasArrayView ? substElemPtrDepth : pointerDepth;
                if (FoldPointerAliasArg(Compiler(entry), resolved, aliasStarSink))
                {
                    hasPointer = pointerDepth > 0;
                    if (aliasStarSink > PointerDepthCap)
                        LogErrorContext(entry, PointerDepthCapMessage(
                            std::format("generic type argument '{}'", entry->getText()),
                            aliasStarSink));
                }
            }
        }

        // The bare base (before the pointer/view suffix) drives the D1 type check below.
        std::string uniqueBase = resolved;
        // A view carries its ELEMENT's stars ("int*[]"), so a view of pointers keys apart from
        // a view of values; the view's own thin-pointer repr is never a written star.
        if (hasArrayView)
            resolved += std::string(
                substElemPtrDepth >= 0 ? substElemPtrDepth : pointerDepth, '*') + "[]";
        else if (hasPointer)
        {
            if (Compiler(entry)->IsInterfaceType(resolved))
                LogErrorContext(entry, std::format("pointer '*' is not allowed on interface type '{}'", resolved));
            // A type parameter already BOUND to a closure reaches here as an encoded name, so the
            // two closure branches above cannot see it; reject the fat case at this funnel too.
            RejectFatEncodedClosurePointerArg(entry, uniqueBase);
            resolved += std::string(pointerDepth, '*');
        }
        // D1: unique is only meaningful on a pointer or interface type. Valid unique arguments
        // have already become a real unique<X> instantiation above; invalid forms have thrown.
        if (isUnique)
        {
            if (hasArrayView)
                LogErrorContext(entry, std::format("'unique' on '{}': array views are not supported - a view does not own its buffer", entry->getText()));
            if (!hasPointer && !Compiler(entry)->IsInterfaceType(uniqueBase))
                LogErrorContext(entry, std::format("unique requires a pointer or interface type; "
                    "'{}' is neither", uniqueBase));
            // The pointer/interface spelling of a `unique` type ARGUMENT names the core
            // `unique<X>` value; unsupported or unresolved forms are rejected here.
            if (CanDesugarUniqueTypeArg(Compiler(entry), uniqueBase, hasPointer, pointerDepth,
                                        hasArrayView))
            {
                std::string uniqueType = MangledGenericName("unique", { uniqueBase });
                QueueGenericInstantiation("unique", { uniqueBase }, uniqueType);
                return uniqueType;
            }
            LogErrorContext(entry, std::format(
                "'unique' on generic type argument '{}' cannot be desugared", entry->getText()));
        }
        // alias mirrors D1: only meaningful on a pointer or interface element (a pure borrow),
        // and is carried as a leading prefix so this instantiation mangles distinctly.
        else if (isAlias)
        {
            if (!hasPointer && !Compiler(entry)->IsInterfaceType(uniqueBase))
                LogErrorContext(entry, std::format("alias requires a pointer or interface type; "
                    "'{}' is neither", uniqueBase));
            resolved = std::string(kAliasQualifierPrefix) + resolved;
        }
        return resolved;
    }

void MainListener::QueueGenericInstantiation(const std::string& baseName,
                                   const std::vector<std::string>& typeArgs,
                                   const std::string& mangledName,
                                   antlr4::ParserRuleContext* site) {
#ifndef NDEBUG
        TypeSpelling spelling;
        if (DemangleType(*Compiler(), mangledName, spelling))
        {
            std::vector<std::string> args;
            args.reserve(spelling.args.size());
            for (const auto& arg : spelling.args)
                args.push_back(MangleType(*Compiler(), arg));
            assert(MangleGenericInstance(*Compiler(), spelling.base, args) == mangledName);
        }
#endif
        if (IsCoreUniqueArrayViewInstantiation(Compiler(), mangledName, typeArgs))
            Compiler()->LogError(CoreUniqueArrayViewMessage(typeArgs));
        if (instantiatedGenerics.count(mangledName))
            return;
        bool isStructOrClass = genericStructTemplates.count(baseName) || genericClassTemplates.count(baseName);
        bool isInterface = genericInterfaceTemplates.count(baseName);
        if (!isStructOrClass && !isInterface)
            return;
        QueuePendingInstantiation(baseName, typeArgs, mangledName, site);
        instantiatedGenerics.insert(mangledName);
        // One predicate for "is this base an interface name", shared with the three scanner sites.
        if (Compiler()->IsGenericInterfaceTemplateName(baseName))
            Compiler()->gts.genericInterfaceInstances.insert(mangledName);
        if (isStructOrClass)
        {
            auto* c = Compiler();
            if (!c->GetDataStructure(mangledName).StructType)
            {
                c->CreateStructType(mangledName, {});
                LLVMBackend::TypeAndValue rt{ .TypeName = mangledName };
                c->CreateFunctionDeclaration(mangledName, rt, {});
            }
        }
    }

std::string MainListener::ResolveSigComponentCodegen(CFlatParser::TypeSpecifierContext* ts, bool& outPointer) {
        if (ts == nullptr) return "void";
        if (auto* inner = ts->functionPointerSpecifier(); inner != nullptr)
        {
            // Third site that reaches a closure shape: the declarator guard and the type-argument
            // funnel both reject a FAT closure pointer, so this one must agree.
            if (outPointer)
                RejectFatClosurePointerArg(ts, inner->Function() != nullptr, ts->getText());
            return EncodeClosureCodegen(inner);
        }
        if (ts->genericIdentifier() != nullptr && ts->genericIdentifier()->genericTypeParameters() != nullptr)
        {
            std::string baseName = ts->genericIdentifier()->Identifier()->getText();
            std::vector<std::string> innerArgs;
            for (auto* entry : ts->genericIdentifier()->genericTypeParameters()->typeParameterList()->typeParameterEntry())
                innerArgs.push_back(ResolveTypeArgEntry(entry));
            std::string mangled = MangledGenericName(baseName, innerArgs);
            QueueGenericInstantiation(baseName, innerArgs, mangled, ts);
            return mangled;
        }
        std::string name = ts->getText();
        auto substIt = activeTypeSubstitutions.find(name);
        if (substIt != activeTypeSubstitutions.end())
        {
            name = substIt->second;
            StripOwnershipQualifiers(name);  // resolve to the underlying type; not signature types
            while (!name.empty() && name.back() == '*') { name.pop_back(); outPointer = true; }
        }
        return name;
    }

std::string MainListener::SigComponentResolvedKey(const std::string& name) {
        return Compiler()->FuncPtrComponentDeclaringKey(name);
    }

void MainListener::RejectFatClosurePointerArg(antlr4::ParserRuleContext* ctx, bool isThin,
                                    const std::string& spelling) {
        if (isThin) return;
        LogErrorContext(ctx, std::format(
            "pointer '*' is not supported on closure type '{}'; "
            "pass the closure by value or use a fixed size '{}[N]' instead",
            spelling, spelling));
    }

std::string MainListener::ClosureArgSpelling(LLVMBackend* compiler, const std::string& encodedName) {
        const auto* sig = compiler->GetEncodedClosureType(encodedName);
        if (sig == nullptr) return encodedName;
        auto comp = [&](const std::string& n, bool ptr, int depth) {
            LLVMBackend::TypeAndValue tv;
            tv.TypeName = n;
            tv.Pointer = ptr;
            tv.PointerDepth = depth;
            return SpellType(*compiler, tv);
        };
        std::string s = sig->IsThinFnPtr() ? "function<" : "Lambda<";
        s += comp(sig->FuncPtrReturnTypeName, sig->FuncPtrReturnPointer, sig->FuncPtrReturnPointerDepth);
        s += "(";
        for (size_t i = 0; i < sig->FuncPtrParams.size(); i++)
        {
            if (i != 0) s += ", ";
            s += comp(sig->FuncPtrParams[i].TypeName, sig->FuncPtrParams[i].Pointer,
                      sig->FuncPtrParams[i].PointerDepth);
        }
        s += ")>";
        return s;
    }

void MainListener::RejectFatEncodedClosurePointerArg(antlr4::ParserRuleContext* ctx, const std::string& baseName) {
        auto* c = Compiler(ctx);
        const auto* sig = c->GetEncodedClosureType(baseName);
        bool isFat = (sig != nullptr && !sig->IsThinFnPtr()) || baseName == "__closure_fat_ptr";
        if (!isFat) return;
        RejectFatClosurePointerArg(ctx, false, ClosureArgSpelling(c, baseName));
    }

std::string MainListener::EncodeClosureCodegen(CFlatParser::FunctionPointerSpecifierContext* fpSpec) {
        bool isThin = fpSpec->Function() != nullptr;
        if (fpSpec->typeSpecifier() == nullptr)
        {
            LogErrorContext(fpSpec, "bare 'function'/'Lambda' has no signature; write "
                "'function<R(Args)>' or 'Lambda<R(Args)>' when used as a generic argument");
            return isThin ? "__c_fn_ptr" : "__closure_fat_ptr";
        }
        LLVMBackend::TypeAndValue sig;
        sig.IsFunctionPointer = true;
        sig.TypeName = isThin ? "__c_fn_ptr" : "__closure_fat_ptr";
        bool retPtr = fpSpec->pointer() != nullptr;
        int retStars = PointerDepthOf(fpSpec->pointer());
        std::string retName = ResolveSigComponentCodegen(fpSpec->typeSpecifier(), retPtr);
        // A signature component over the cap can never be matched: no function may declare a
        // 3-star return or parameter, so the closure type would be unpopulatable.
        if (retStars > PointerDepthCap)
            LogErrorContext(fpSpec, PointerDepthCapMessage(
                std::format("function type return '{}'", retName), retStars));
        sig.FuncPtrReturnTypeName = retName;
        sig.FuncPtrReturnPointer  = retPtr;
        if (auto* qualifier = fpSpec->functionReturnQualifier(); qualifier != nullptr)
        {
            std::string text = qualifier->getText();
            if (text != "move" && text != "alias")
                LogErrorContext(qualifier, "function return qualifier must be 'move' or 'alias'");
            sig.FuncPtrReturnOwned = text == "move";
            sig.FuncPtrReturnAlias = text == "alias";
        }
        sig.FuncPtrReturnPointerDepth = ReconcilePointerDepth(retPtr, retStars);
        sig.FuncPtrReturnResolvedKey = SigComponentResolvedKey(retName);
        std::vector<std::pair<std::string, int>> encParams;
        if (fpSpec->functionPointerParamList() != nullptr)
            for (auto* param : fpSpec->functionPointerParamList()->functionPointerParam())
            {
                bool pPtr = param->pointer() != nullptr;
                int pStars = PointerDepthOf(param->pointer());
                std::string pName = ResolveSigComponentCodegen(param->typeSpecifier(), pPtr);
                if (pStars > PointerDepthCap)
                    LogErrorContext(param, PointerDepthCapMessage(
                        std::format("function type parameter '{}'", pName), pStars));
                int pDepth = ReconcilePointerDepth(pPtr, pStars);
                encParams.push_back({ pName, pDepth });
                LLVMBackend::TypeAndValue::FuncPtrParam fp;
                fp.TypeName = pName; fp.Pointer = pPtr; fp.IsMove = param->Move() != nullptr;
                fp.PointerDepth = pDepth;
                fp.ResolvedTypeKey = SigComponentResolvedKey(pName);
                sig.FuncPtrParams.push_back(fp);
            }
        std::string encoded = BuildEncodedClosureName(Compiler(), isThin, retName,
            sig.FuncPtrReturnPointerDepth, encParams);
        Compiler(fpSpec)->RegisterEncodedClosureType(encoded, sig);
        return encoded;
    }

std::string MainListener::EncodeClosureFromSig(LLVMBackend* compiler, const LLVMBackend::TypeAndValue& sig) {
        std::vector<std::pair<std::string, int>> params;
        for (const auto& p : sig.FuncPtrParams)
            params.push_back({ p.TypeName, ReconcilePointerDepth(p.Pointer, p.PointerDepth) });
        std::string encoded = BuildEncodedClosureName(compiler, sig.IsThinFnPtr(), sig.FuncPtrReturnTypeName,
            ReconcilePointerDepth(sig.FuncPtrReturnPointer, sig.FuncPtrReturnPointerDepth), params);
        compiler->RegisterEncodedClosureType(encoded, sig);
        return encoded;
    }

LLVMBackend::DeclTypeAndValue MainListener::ParseDeclarationSpecifiers(CFlatParser::DeclarationSpecifiersContext* declSpecs) {
        // Same defensive guard as the ForwardRefScanner copy above.
        if (declSpecs == nullptr)
        {
            Compiler()->LogError("expected a return type here");
            return {};
        }
        LLVMBackend::DeclTypeAndValue declType;
        bool hasUniqueSpecifier = false;
        std::string typeName;
        auto declSpecList = declSpecs->declarationSpecifier();
        // Reject `T[][]` / `T[][M]` / `T[N][]` before any branch consumes the brackets - every
        // branch below drops the empty pairs and would silently parse a narrower type.
        for (auto declSpec : declSpecList)
            if (HasUnsizedMultiDim(declSpec))
                LogErrorContext(declSpec, UnsizedMultiDimMessage(
                    declSpec->typeSpecifier() != nullptr ? declSpec->typeSpecifier()->getText() : "T"));
        // Reject a written 3+ star declarator before any branch consumes the stars - the plain,
        // simd and generic branches would each clamp it to '**' with no diagnostic.
        for (auto declSpec : declSpecList)
        {
            int writtenStars = PointerDepthOf(declSpec->pointer());
            if (writtenStars > PointerDepthCap)
                LogErrorContext(declSpec, PointerDepthCapMessage(
                    std::format("declared type '{}'", declSpec->typeSpecifier() != nullptr
                        ? declSpec->typeSpecifier()->getText() : "T"), writtenStars));
        }
        // `long long` arrives as two `long` typeSpecifiers; count them before the loop breaks
        // out on the first one, so the pair can canonicalize to i64.
        int longSpecCount = 0;
        for (auto declSpec : declSpecList)
            if (declSpec->typeSpecifier() != nullptr && declSpec->typeSpecifier()->getText() == "long")
                longSpecCount++;

        for (auto declSpec : declSpecList)
        {
            auto typeSpec = declSpec->typeSpecifier();
            auto storageSpec = declSpec->storageClassSpecifier();
            if (typeSpec != nullptr)
            {
                // Pointer depth from a pointer alias (using Handle = void*); peeled off the
                // resolved alias string below and combined with the declarator's stars.
                int aliasPtrDepth = 0;
                // Star count contributed by an active type SUBSTITUTION (T bound to "C**" -> 2).
                // Real depth, not a flag, so the declarator combination below can prove a total.
                int substArgPtrDepth = 0;
                int substArgElemPtrDepth = 0;  // stars on a view ARG's element ("int*[]")
                // T bound to a '...[]' array-view ARGUMENT (P<int[]> -> T = "int[]").
                bool substArgIsArrayView = false;
                // Set when the spec names a generic interface instantiation, whose interfaceTable
                // entry is only built by the next ProcessPendingInstantiations.
                bool genericSpecIsInterface = false;
                // 'move', 'adopt', 'alias', 'bond', 'unique' and 'manifest' are soft keywords parsed as Identifiers
                // in typeSpecifier context
                if (typeSpec->getText() == "move")
                {
                    declType.IsMove = true;
                    continue;  // not a type; look for the actual type in next specifier
                }
                if (typeSpec->getText() == "adopt")
                {
                    declType.IsAdopt = true;
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
                if (typeSpec->getText() == "unique")
                {
                    // `unique` is now legal on a field (validated by ValidateUniqueField) and on a
                    // local/param (an owning location, validated at the end of this function once the
                    // pointer/interface shape is known - D1/D5). Return-type unique stays unsupported.
                    hasUniqueSpecifier = true;
                    continue;  // not a type; look for the actual type in next specifier
                }
                if (typeSpec->getText() == "manifest")
                    continue;  // collected by ParseDeclaration at file scope
                if (typeSpec->getText() == "application")
                    continue;  // collected by ParseDeclaration at file scope
                // tuple type sugar: (T1, T2) -> tuple<T1, T2>
                if (typeSpec->tupleTypeSpecifier() != nullptr)
                {
                    auto* tts = typeSpec->tupleTypeSpecifier();
                    std::vector<std::string> typeArgs;
                    if (tts->tupleTypePackEntry() != nullptr)
                    {
                        // (T...) - expand pack substitution
                        std::string packName = tts->tupleTypePackEntry()->typeSpecifier()->getText();
                        auto packIt = activePackSubstitutions.find(packName);
                        if (packIt != activePackSubstitutions.end())
                            typeArgs = packIt->second;
                        else
                            typeArgs.push_back(packName);
                    }
                    else
                    {
                        for (auto* entry : tts->tupleTypeEntry())
                        {
                            // `(T[], ...)` element is a noalias array-view member; `[N]`/`[]*` are not.
                            if (IsBadArrayArg(entry))
                                LogErrorContext(entry, "'T[]' is the only bracketed form valid as a "
                                    "tuple element; a sized 'T[N]', an unsized multi-dimensional "
                                    "'T[][]' and a pointer-to-array 'T[]*' are not");
                            std::string argName = entry->typeSpecifier()->getText();
                            int argDepth = PointerDepthOf(entry->pointer());
                            bool argView = IsArrayViewArg(entry);
                            // On a view the written stars decorate the ELEMENT: `(int*[], int)`.
                            int argElemDepth = argView ? argDepth : 0;
                            if (argView) argDepth = 0;
                            // Apply active type substitutions (e.g. T -> int inside generic body); a
                            // substituted arg may itself carry "*"/"[]" (e.g. T bound to "int[]").
                            auto substIt = activeTypeSubstitutions.find(argName);
                            if (substIt != activeTypeSubstitutions.end())
                            {
                                argName = substIt->second;
                                PeelTypeArgSuffix(argName, argDepth, argView, nullptr,
                                                  &argElemDepth);
                            }
                            else
                            {
                                // Same namespace walk as a generic type ARGUMENT (a substituted arg
                                // is already resolved in the caller's scope - never re-resolve it).
                                argName = Compiler(entry)->ResolveTypeArgBaseName(argName);
                                // One key per type: `(IP, int)` folds to `(int*, int)`.
                                FoldPointerAliasArg(Compiler(entry), argName,
                                                    argView ? argElemDepth : argDepth);
                            }
                            // `T*` on a substituted VIEW decays to the element pointer, exactly as
                            // the generic type-argument and declarator arms do.
                            if (argView && !IsArrayViewArg(entry) && PointerDepthOf(entry->pointer()) > 0)
                            {
                                argView = false;
                                argDepth = argElemDepth + PointerDepthOf(entry->pointer());
                                argElemDepth = 0;
                            }
                            if (argView) argName += std::string(argElemDepth, '*') + "[]";
                            else if (argDepth > 0) argName += std::string(argDepth, '*');
                            typeArgs.push_back(argName);
                        }
                    }
                    std::string mangledName = MangledGenericName("tuple", typeArgs);
                    declType.TypeName = mangledName;
                    tupleTypeArgs[mangledName] = typeArgs;
                    // Queue instantiation if inside a generic context and not already done
                    if (!instantiatedGenerics.count(mangledName) &&
                        (genericStructTemplates.count("tuple") || genericClassTemplates.count("tuple")))
                    {
                        QueuePendingInstantiation("tuple", typeArgs, mangledName);
                        instantiatedGenerics.insert(mangledName);
                        auto* c = Compiler();
                        if (!c->GetDataStructure(mangledName).StructType)
                        {
                            c->CreateStructType(mangledName, {});
                            LLVMBackend::TypeAndValue rt{ .TypeName = mangledName };
                            c->CreateFunctionDeclaration(mangledName, rt, {});
                        }
                    }
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
                        // Resolve return/param types the same way ordinary type positions resolve:
                        // substitution + generic mangling/queueing (gap b), so a generic inside the
                        // signature (e.g. Lambda<list<string>()>) stores the mangled "list$string".
                        bool retPtr = fpSpec->pointer() != nullptr;
                        int retStars = PointerDepthOf(fpSpec->pointer());
                        declType.FuncPtrReturnTypeName = ResolveSigComponentCodegen(fpSpec->typeSpecifier(), retPtr);
                        if (auto* qualifier = fpSpec->functionReturnQualifier(); qualifier != nullptr)
                        {
                            std::string text = qualifier->getText();
                            if (text != "move" && text != "alias")
                                LogErrorContext(qualifier, "function return qualifier must be 'move' or 'alias'");
                            declType.FuncPtrReturnOwned = text == "move";
                            declType.FuncPtrReturnAlias = text == "alias";
                        }
                        // Over the cap here too: no declarable function can supply this signature.
                                if (retStars > PointerDepthCap)
                                    LogErrorContext(fpSpec, PointerDepthCapMessage(
                                        std::format("function type return '{}'",
                                            SpellType(*Compiler(declSpecs), LLVMBackend::TypeAndValue{
                                                .TypeName = declType.FuncPtrReturnTypeName })), retStars));
                        declType.FuncPtrReturnPointer  = retPtr;
                        declType.FuncPtrReturnPointerDepth = ReconcilePointerDepth(retPtr, retStars);
                        declType.FuncPtrReturnResolvedKey = SigComponentResolvedKey(declType.FuncPtrReturnTypeName);
                        if (fpSpec->functionPointerParamList() != nullptr)
                        {
                            for (auto* param : fpSpec->functionPointerParamList()->functionPointerParam())
                            {
                                LLVMBackend::TypeAndValue::FuncPtrParam p;
                                bool pPtr = param->pointer() != nullptr;
                                int pStars = PointerDepthOf(param->pointer());
                                p.TypeName = ResolveSigComponentCodegen(param->typeSpecifier(), pPtr);
                                LLVMBackend::TypeAndValue pType;
                                pType.TypeName = p.TypeName;
                                pType.Pointer = pPtr;
                                pType.PointerDepth = pStars;
                                if (pStars > PointerDepthCap)
                                    LogErrorContext(param, PointerDepthCapMessage(
                                        std::format("function type parameter '{}'",
                                            SpellType(*Compiler(declSpecs), pType)), pStars));
                                p.Pointer  = pPtr;
                                p.IsMove   = param->Move() != nullptr;
                                p.PointerDepth = ReconcilePointerDepth(pPtr, pStars);
                                CanonicalizeUniqueSigComponent(Compiler(declSpecs), p.TypeName,
                                                               p.Pointer, p.PointerDepth);
                                p.ResolvedTypeKey = SigComponentResolvedKey(p.TypeName);
                                declType.FuncPtrParams.push_back(p);
                            }
                        }
                    }
                    // For bare 'function', signature inferred from initializer at declaration site
                    // This branch breaks out of the specifier loop, so nothing else consumes a
                    // trailing '[N]' or '*'; capture both here (as the alias branch below does).
                    declType.Pointer = declSpec->pointer() != nullptr;
                    // A fat closure is a by-value struct, so a pointer to one loads the struct
                    // as if it were the address itself and the module fails LLVM verification.
                    if (declType.Pointer && !declType.IsThinFnPtr())
                        LogErrorContext(declSpec, std::format(
                            "pointer '*' is not supported on closure type '{}'; "
                            "pass the closure by value or use a fixed size '{}[N]' instead",
                            typeSpec->getText(), typeSpec->getText()));
                    if (auto* fpDimSpec = ArrayDimsOf(declSpec))
                    {
                        auto fpDims = fpDimSpec->assignmentExpression();
                        if (ArrayPtrOf(declSpec) != nullptr && !fpDims.empty())
                            LogErrorContext(declSpec, FixedArrayPointerTypeMessage(typeSpec->getText()));
                        declType.ArraySize = fpDims.empty() ? nullptr : fpDims[0];
                        for (size_t di = 1; di < fpDims.size(); di++)
                            declType.ExtraArrayDims.push_back(fpDims[di]);
                        if (fpDims.empty())
                        {
                            // '[]' array-view is a thin 'ptr' repr, so a fat closure - which is
                            // a struct by value - cannot be viewed this way.
                            if (!declType.IsThinFnPtr())
                                LogErrorContext(declSpec, std::format(
                                    "array-view '[]' is not supported on closure type '{}'; "
                                    "use a fixed size '{}[N]' instead",
                                    typeSpec->getText(), typeSpec->getText()));
                            declType.IsArrayView = true;
                            declType.Pointer = true;
                        }
                    }
                    break;
                }
                // simd<T,N> builtin vector type (not a generic): record element type + lane count.
                if (typeSpec->simdTypeSpecifier() != nullptr)
                {
                    auto* sd = typeSpec->simdTypeSpecifier();
                    std::string elemType = sd->typeSpecifier()->getText();
                    // Resolve a generic type parameter (e.g. T -> float inside simd<T,8> in a template body).
                    auto substIt = activeTypeSubstitutions.find(elemType);
                    if (substIt != activeTypeSubstitutions.end())
                        elemType = substIt->second;
                    uint64_t lanes = 0;
                    std::string err;
                    if (!TryParseSimdLaneCount(Compiler(), sd->assignmentExpression(), lanes, err))
                        LogErrorContext(sd, err);
                    declType.TypeName = elemType;
                    declType.IsSimd = true;
                    declType.SimdLanes = lanes;
                    RecordSimdPointerAndDims(declType, declSpec);
                    if (auto* dims = ArrayDimsOf(declSpec);
                        dims != nullptr && dims->assignmentExpression().empty())
                        LogErrorContext(declSpec,
                            "array-view '[]' is not supported on simd types; use a fixed simd array");
                    // The simd branch leaves the specifier loop before the mainstream
                    // pointer-to-array guard, so state it here rather than fall out of
                    // declaration parsing and be re-read as a multiplication expression.
                    else if (ArrayPtrOf(declSpec) != nullptr && declType.ArraySize != nullptr)
                        LogErrorContext(declSpec, SimdPointerTypeMessage(
                            std::format("simd<{},{}>", elemType, lanes), true));
                    break;
                }
                std::string baseName;
                auto* genParams = GenericSpecOf(typeSpec, baseName);
                if (genParams != nullptr)
                {
                    // Generic type instantiation: Box<MyType> -> Box$MyType
                    baseName = Compiler(declSpecs)->ResolveGenericBaseAlias(baseName);
                    std::vector<std::string> typeArgs;
                    for (auto* entry : genParams->typeParameterList()->typeParameterEntry())
                    {
                        // `tuple` is a plain aggregate with no destructor, so a `unique` element would
                        // never be freed - reject it rather than silently drop the ownership.
                        if (baseName == "tuple" && TypeArgHasUnique(entry))
                            LogErrorContext(entry, "unique is not supported as a tuple element type");
                        typeArgs.push_back(ResolveTypeArgEntry(entry));
                    }
                    std::string mangledName = MangledGenericName(baseName, typeArgs);
                    if (IsCoreUniqueArrayViewInstantiation(Compiler(declSpecs), mangledName, typeArgs))
                        LogErrorContext(genParams, CoreUniqueArrayViewMessage(typeArgs));
                    declType.TypeName = mangledName;
                    // A generic INTERFACE instantiation is a fat pointer, not a struct. interfaceTable
                    // only gains it at the next drain, so mark it from the template name here. The
                    // genericInterfaceInstances insert waits for the common tail, past the LogErrors
                    // that can throw out of this function.
                    if (Compiler(declSpecs)->IsGenericInterfaceTemplateName(baseName))
                        genericSpecIsInterface = true;
                    // A namespace-qualified generic names an imported winmd template: build the
                    // concrete thin interface + PIID now so the declared type is a real struct.
                    if (baseName.find('.') != std::string::npos)
                        Compiler(declSpecs)->InstantiateWinrtGenericInterface(baseName, typeArgs, mangledName);
                    // Queue instantiation of nested generic types discovered during field/param parsing.
                    // Only do this when inside an active instantiation context (substitutions are set),
                    // to avoid treating unresolved type parameters (e.g. "T") as concrete types.
                    // Top-level explicit uses (e.g. hashset<int> in user code) are handled by
                    // ForwardRefScanner::ScanGenericTypeUses and ScanAndQueueGenericTypeUses.
                    if ((!activeTypeSubstitutions.empty() || !activeValueSubstitutions.empty())
                        && !instantiatedGenerics.count(mangledName))
                    {
                        bool isKnownTemplate = genericStructTemplates.count(baseName) || genericClassTemplates.count(baseName);
                        if (isKnownTemplate)
                        {
                            QueuePendingInstantiation(baseName, typeArgs, mangledName, genParams);
                            instantiatedGenerics.insert(mangledName);
                            auto* c = Compiler();
                            if (!c->GetDataStructure(mangledName).StructType)
                            {
                                c->CreateStructType(mangledName, {});
                                LLVMBackend::TypeAndValue rt{ .TypeName = mangledName };
                                c->CreateFunctionDeclaration(mangledName, rt, {});
                            }
                        }
                    }
                }
                else if (auto* fit = Compiler(declSpecs)->FindFunctionTypeAlias(typeSpec->getText());
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
                    // Same fat-closure pointer rejection as the functionPointerSpecifier branch.
                    if (declType.Pointer && !declType.IsThinFnPtr())
                        LogErrorContext(declSpec, std::format(
                            "pointer '*' is not supported on closure type '{}'; "
                            "pass the closure by value or use a fixed size '{}[N]' instead",
                            typeSpec->getText(), typeSpec->getText()));
                    // Like the functionPointerSpecifier branch, this one breaks out of the
                    // specifier loop, so a trailing '[N]' has to be captured right here.
                    if (auto* fpDimSpec = ArrayDimsOf(declSpec))
                    {
                        auto fpDims = fpDimSpec->assignmentExpression();
                        declType.ArraySize = fpDims.empty() ? nullptr : fpDims[0];
                        for (size_t di = 1; di < fpDims.size(); di++)
                            declType.ExtraArrayDims.push_back(fpDims[di]);
                        if (fpDims.empty())
                        {
                            if (!declType.IsThinFnPtr())
                                LogErrorContext(declSpec, std::format(
                                    "array-view '[]' is not supported on closure type '{}'; "
                                    "use a fixed size '{}[N]' instead",
                                    typeSpec->getText(), typeSpec->getText()));
                            declType.IsArrayView = true;
                            declType.Pointer = true;
                        }
                    }
                    break;
                }
                else
                {
                    typeName = typeSpec->getText();
                    if (typeName == "long") typeName = LongSpellingTypeName(longSpecCount);
                    // Apply active type parameter substitutions (e.g. T -> int inside a template body)
                    int& substPointerDepth = substArgPtrDepth;
                    bool& substArrayView = substArgIsArrayView;
                    auto substIt = activeTypeSubstitutions.find(typeName);
                    // A substituted name is ALREADY resolved in the caller's scope; re-resolving it
                    // here rebinds it to the template's declaring namespace (layer 3).
                    bool wasSubstituted = substIt != activeTypeSubstitutions.end();
                    if (substIt != activeTypeSubstitutions.end())
                    {
                        // A substituted type may carry a "*" (pointer), "[]" (noalias array-view),
                        // or "alias " suffix - e.g. a tuple pack param bound to "int[]". Strip only
                        // the alias marker here; a desugared unique type is already a core value.
                        typeName = substIt->second;
                        bool substAliasArg = false;
                        PeelTypeArgSuffix(typeName, substPointerDepth, substArrayView,
                                          &substAliasArg, &substArgElemPtrDepth);
                        // An `alias` element (list<alias X*>) is a pure borrow whose owner lives
                        // elsewhere, so ANY value of that element type is a borrow - not only the
                        // `alias T get` return but also `take()`'s plain-T return (take removes the
                        // slot but never transfers ownership the container never held). Tag every
                        // such value so a later `delete` of a bound local is rejected as a
                        // double-free. Never a move sink.
                        if (substAliasArg) declType.IsBorrowOfAliasElement = true;
                    }
                    // Resolve namespace-qualified type names (alias expansion + parent namespace search).
                    // forceRoot on a substituted name: the outward walk would re-bind it here.
                    typeName = Compiler(declSpecs)->ResolveQualifiedName(typeName, wasSubstituted);
                    // If still unresolved, try qualifying with the enclosing struct scope (e.g. Inner -> Outer.Inner).
                    // The !wasSubstituted guard is DEFENSIVE and inert today: removing it changed no leg or probe.
                    if (!wasSubstituted && !structScopeStack.empty() && !Compiler(declSpecs)->IsDataStructure(typeName))
                    {
                        std::string qualified = structScopeStack.back() + "." + typeName;
                        if (Compiler(declSpecs)->IsDataStructure(qualified))
                            typeName = qualified;
                    }
                    // Resolve type aliases (e.g. user-defined aliases)
                    typeName = Compiler(declSpecs)->ResolveTypeAlias(typeName);
                    // Peel an array alias's brackets (using Vec3 = float[3]) BEFORE the stars so
                    // "int*[3]" yields dims {3} over base "int*".
                    std::vector<uint64_t> aliasDims;
                    if (PeelAliasArrayDims(typeName, aliasDims))
                    {
                        declType.AliasArraySize = aliasDims[0];
                        declType.AliasInnerDims.assign(aliasDims.begin() + 1, aliasDims.end());
                    }
                    aliasPtrDepth = PeelAliasPointerStars(typeName);  // using Handle = void* -> depth 1
                    // An enum names a real type here: bind it to its registered (scoped) key and
                    // record the backing spelling that carries its width and signedness.
                    if (std::string enumKey = Compiler(declSpecs)->ResolveEnumTypeName(typeName);
                        !enumKey.empty())
                    {
                        typeName = enumKey;
                        declType.EnumBacking = Compiler(declSpecs)->GetEnumBackingType(enumKey);
                    }
                    declType.TypeName = typeName;
                    // A type-arg string carries its REAL star count, so the depth below is a proof:
                    // `Box<C**>`'s substituted `T x` is a C**, not a C*.
                    if (substPointerDepth > 0) declType.Pointer = true;
                    if (substArrayView)
                    {
                        // T bound to a `T[]` arg: the field/local is a noalias array-view (a thin
                        // pointer repr carrying the contract), exactly like a written `T[]`.
                        declType.IsArrayView = true;
                        declType.Pointer = true;
                        // T bound to `int*[]` is a view OF pointers - same shape a written
                        // `int*[]` declarator produces (Pointer + ElemPointer + IsArrayView).
                        if (substArgElemPtrDepth > 0) declType.ElemPointer = true;
                        // The view's own pointer repr is not a '*' the depth arithmetic may count.
                        substPointerDepth = 0;
                    }
                }
                bool hasExplicitPointer = declSpec->pointer() != nullptr;
                bool hasDblPointer = hasExplicitPointer && declSpec->pointer()->Star().size() >= 2;
                bool substPointer = declType.Pointer; // T was already a pointer (e.g. T=IMessage*)
                // `T*` with T bound to a view arg ('int[]') DECAYS to the ELEMENT pointer 'int*':
                // the star consumes the view's own thin-pointer repr and drops the noalias trait.
                if (substArgIsArrayView && hasExplicitPointer)
                {
                    declType.IsArrayView = false;
                    // A view OF pointers keeps its element depth, so `T*` over `int*[]` is an
                    // `int**`; the star arithmetic below re-adds the declarator's own levels.
                    declType.Pointer = substArgElemPtrDepth > 0;
                    declType.ElemPointer = false;
                    substArgPtrDepth = substArgElemPtrDepth;
                }
                if (aliasPtrDepth > 0)
                {
                    // A pointer alias is in play: combine its depth with the declarator's stars and
                    // any base-pointer-ness from a generic substitution. The Pointer + ElemPointer
                    // model caps at 2 levels - 3+ is a hard error (no silent truncation).
                    int declStars = hasExplicitPointer ? (int)declSpec->pointer()->Star().size() : 0;
                    int totalPtr = aliasPtrDepth + declStars + substArgPtrDepth;
                    // The cap counts a substitution as ONE level, exactly as it did before real
                    // depth existed: a deeper type ARGUMENT must not turn an instantiation into an
                    // error (list<C**> substitutes T=C** into its own `T* _data`).
                    int writtenPtr = aliasPtrDepth + declStars + (substArgPtrDepth > 0 ? 1 : 0);
                    if (writtenPtr > PointerDepthCap)
                        LogErrorContext(declSpec, PointerDepthCapMessage(
                            std::format("pointer alias resolving to '{}'",
                                SpellType(*Compiler(declSpecs), declType)), writtenPtr));
                    declType.Pointer = totalPtr >= 1;
                    declType.ElemPointer = totalPtr >= 2;
                    declType.PointerDepth = totalPtr > 2 ? 0 : totalPtr;
                }
                else
                {
                    // Pointer-to-pointer: explicit ** OR (substituted type is a pointer AND explicit *)
                    if (hasDblPointer || (declType.Pointer && hasExplicitPointer))
                        declType.ElemPointer = true;
                    declType.Pointer = hasExplicitPointer || declType.Pointer;
                    int declStars = hasExplicitPointer ? (int)declSpec->pointer()->Star().size() : 0;
                    // A substituted type argument contributes its REAL star count, so `T x` with
                    // T = C** is a proven C**. Over the 2-level cap the exact depth is LOST, so
                    // claim nothing (0): a clamped 2 stepped down by a '*' would falsely prove 1.
                    int totalPtr = declStars + substArgPtrDepth;
                    if (substArgPtrDepth >= 2) declType.ElemPointer = true;
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
                        // `T[]` is a thin noalias array-view. A preceding '*' belongs to the
                        // ELEMENT, so `T*[]` carries Pointer + ElemPointer rather than collapsing
                        // into the indistinguishable `T[]` shape.
                        bool elementPointer = declType.Pointer;
                        declType.IsArrayView = true;
                        declType.Pointer = true;
                        declType.ElemPointer = elementPointer || declType.ElemPointer;
                    }
                }
                LLVMBackend::TypeAndValue baseType = declType;
                baseType.Pointer = false;
                baseType.ElemPointer = false;
                baseType.PointerDepth = 0;
                baseType.IsArrayView = false;
                const std::string baseSpelling = SpellType(*Compiler(declSpecs), baseType);
                if (ArrayPtrOf(declSpec))
                {
                    if (declType.IsArrayView)
                        LogErrorContext(declSpec, std::format(
                            "pointer to array-view '{}[]*' is not a valid type. "
                            "To return several results, return one 'T[]' and pass the rest as out-parameters: "
                            "a 'T[]' for arrays (each keeps its noalias contract) and a 'T*' for scalars.",
                            baseSpelling));
                    else
                        LogErrorContext(declSpec, std::format(
                            "pointer to fixed array '{}[N]*' is not a valid type; "
                            "pass '{}*' (a fixed array decays to a pointer to its first element).",
                            baseSpelling, baseSpelling));
                }
                // A pointer to an aliased fixed array (Vec3* where Vec3 = float[3]) is the same
                // T[N]* ban, but the dimension lives on the alias, not the declarator - so the
                // ArrayPtrOf check above misses it. Fire the diagnostic through the alias.
                if (declType.AliasArraySize > 0 && hasExplicitPointer)
                    LogErrorContext(declSpec, std::format(
                        "pointer to fixed array '{}[N]*' is not a valid type; "
                        "pass '{}*' (a fixed array decays to a pointer to its first element).",
                        baseSpelling, baseSpelling));
                declType.IsInterface = genericSpecIsInterface
                                    || Compiler(declSpecs)->IsInterfaceType(declType.TypeName);
                if (declType.IsInterface && hasExplicitPointer && activeTypeSubstitutions.empty())
                    LogErrorContext(declSpec, std::format("pointer '*' is not allowed on interface type '{}'",
                        SpellType(*Compiler(declSpecs), baseType)));
                // Past the throwing checks: this spec really does name a generic interface.
                if (genericSpecIsInterface)
                    Compiler(declSpecs)->gts.genericInterfaceInstances.insert(declType.TypeName);
                if (declType.IsInterface)
                {
                    // IsInterfacePointer: this represents a pointer TO a fat-ptr, not the fat-ptr itself.
                    // True when T* where T=IFace (hasExplicitPointer), or T where T=IFace* (substPointer).
                    declType.IsInterfacePointer = hasExplicitPointer || substPointer;
                    if (declType.IsInterfacePointer)
                        declType.Pointer = true;
                }
                // Pointer-form and interface-form `unique` spell the core `unique<T>` value in
                // locals, params, fields, and function/interface return types. `unique void*`
                // stays on the builtin path permanently.
                const bool isParameterDecl = dynamic_cast<CFlatParser::ParameterDeclarationContext*>(
                    declSpecs->parent) != nullptr;
                const bool isReturnDecl =
                    dynamic_cast<CFlatParser::FunctionDefinitionContext*>(declSpecs->parent) != nullptr
                    || dynamic_cast<CFlatParser::InterfaceMethodContext*>(declSpecs->parent) != nullptr;
                bool hasExternSpecifier = false;
                for (auto candidate : declSpecList)
                    if (candidate->storageClassSpecifier() != nullptr
                        && candidate->storageClassSpecifier()->Extern() != nullptr)
                        hasExternSpecifier = true;
                const bool uniqueFatInterface = declType.IsFatInterfaceValue();
                // A FIELD's `[N]` sits on the declarator, not on these specifiers. Local and field
                // declarators may still desugar to fixed arrays of unique<T>.
                const bool uniqueArrayOk = inLocalDecl_ || inStructFieldDecl_;
                if (hasUniqueSpecifier
                    && !UniqueDeclPointeeIsUnbound(Compiler(declSpecs), declSpecs, declType.TypeName)
                    && (global_scope || inLocalDecl_ || isParameterDecl || inStructFieldDecl_ || isReturnDecl)
                    && (!isReturnDecl || !hasExternSpecifier)
                    && declType.TypeName != "void"
                    && !inUnionFieldDecl_
                    && !inInterfaceFieldDecl_
                    && (!inStructFieldDecl_ || declType.TypeName != "void")
                    && (uniqueFatInterface
                        || (!declType.IsInterface && declType.Pointer && !declType.ElemPointer))
                    && !declType.IsArrayView
                    && (declType.ArraySize == nullptr || uniqueArrayOk)
                    && declType.AliasArraySize == 0
                    && declType.ExtraArrayDims.empty()
                    && (declSpecs->parent == nullptr
                        || (uniqueArrayOk || !HasParameterArrayDeclarator(declSpecs))))
                {
                    std::string elementType = declType.TypeName;
                    // The declared allocation alignment travels in the TYPE: `alignas(0, N) unique
                    // T*` is `unique<T, N>`, so the deallocator that matches the allocation is
                    // part of the type rather than a side flag on the local.
                    std::vector<std::string> uniqueArgs{ elementType };
                    if (declType.AllocAlignValue != 0)
                        uniqueArgs.push_back(std::to_string(declType.AllocAlignValue));
                    std::string uniqueType = MangledGenericName("unique", uniqueArgs);
                    QueueGenericInstantiation("unique", uniqueArgs, uniqueType);
                    declType.TypeName = uniqueType;
                    declType.Pointer = false;
                    declType.ElemPointer = false;
                    declType.PointerDepth = 0;
                    hasUniqueSpecifier = false;
                    // The declared type is now the wrapper STRUCT, not the interface it stores.
                    declType.IsInterface = false;
                    declType.IsInterfacePointer = false;
                    if (isParameterDecl)
                        declType.IsMove = true;
                }
                if (declSpec->Question())
                {
                    if (declType.IsPrimitive())
                        LogErrorContext(declSpec, std::format("nullable '?' is not allowed on primitive type '{}'",
                            SpellType(*Compiler(declSpecs), declType)));
                    else
                    {
                        declType.IsNullable = true;
                        declType.Pointer = true;
                    }
                }
                // Direct `unique` on a local/param (D4/D5): it qualifies a single-indirection pointer
                // or an interface only (D1). A field is validated separately by ValidateUniqueField.
                if (hasUniqueSpecifier
                    && !inStructFieldDecl_)
                {
                    // A view never owns its buffer, so it can never be the thing a scope-exit
                    // teardown deletes. Same rule (and wording) as the field form.
                    if (declType.IsArrayView)
                        LogErrorContext(declSpec, std::format(
                            "'unique' on '{}[]': array views are not supported - a view does not "
                            "own its buffer", SpellType(*Compiler(declSpecs), declType)));
                    bool uniqueSingleIndirect = (declType.Pointer && !declType.ElemPointer
                        && !declType.IsArrayView) || declType.IsFatInterfaceValue();
                    if (!uniqueSingleIndirect && !declType.IsArrayView)
                        LogErrorContext(declSpec, std::format(
                            "unique requires a pointer or interface type; '{}' is neither",
                            SpellType(*Compiler(declSpecs), declType)));
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
            else if (auto alignSpec = declSpec->alignmentSpecifier())
            {
                uint64_t alignVal = ParseAlignmentSpecifier(alignSpec);
                if (alignVal != 0)
                    declType.UserAlignValue = alignVal;
                // arg2 = allocation alignment of the owned heap block (was `align_alloc(N)`).
                uint64_t allocAlign = ParseAllocAlignArg(alignSpec);
                if (allocAlign != 0)
                    declType.AllocAlignValue = allocAlign;
            }
        }

        // An explicitly spelled `unique<T, N>` must carry the same AllocAlignValue the
        // `alignas(0, N) unique T*` sugar leaves behind, or only the sugar seeds the inbound
        // allocation-alignment channel and a direct `new` on the explicit spelling is rejected.
        if (declType.AllocAlignValue == 0)
            declType.AllocAlignValue = CoreUniqueDeclaredAllocAlign(Compiler(), declType.TypeName);

        return declType;
    }

uint64_t MainListener::ValidateAlignValue(antlr4::ParserRuleContext* ctx, int64_t signedVal, const char* what) {
        if (signedVal <= 0)
        {
            LogErrorContext(ctx, std::format("{}: value must be positive (got {})", what, signedVal));
            return 0;
        }
        uint64_t alignVal = (uint64_t)signedVal;
        if (alignVal > 4096)
        {
            LogErrorContext(ctx, std::format("{}: value {} exceeds maximum of 4096", what, alignVal));
            return 0;
        }
        if ((alignVal & (alignVal - 1)) != 0)
        {
            LogErrorContext(ctx, std::format("{}: value {} is not a power of two", what, alignVal));
            return 0;
        }
        return alignVal;
    }

uint64_t MainListener::ParseAlignmentSpecifier(CFlatParser::AlignmentSpecifierContext* alignSpec) {
        if (auto* typeName = alignSpec->typeName())
        {
            LLVMBackend::DeclTypeAndValue dt;
            dt.TypeName = typeName->getText();
            llvm::Type* t = compilerLLVM->GetType(dt);
            if (t == nullptr || !t->isSized())
            {
                LogErrorContext(alignSpec, std::format("alignas: cannot resolve type '{}'",
                    SpellType(*compilerLLVM, dt)));
                return 0;
            }
            uint64_t alignVal = compilerLLVM->module->getDataLayout().getABITypeAlign(t).value();
            return ValidateAlignValue(alignSpec, (int64_t)alignVal, "alignas");
        }
        auto cexprs = alignSpec->constantExpression();
        if (cexprs.empty()) return 0;
        llvm::Value* condVal = ParseConditionalExpression(cexprs[0]->conditionalExpression());
        auto* ci = llvm::dyn_cast_or_null<llvm::ConstantInt>(condVal);
        if (ci == nullptr)
        {
            LogErrorContext(alignSpec, "alignas: argument must be a constant integer expression");
            return 0;
        }
        int64_t signedVal = ci->getSExtValue();
        // `alignas(0, N)` = natural slot (arg2 carries the alloc-align). A bare `alignas(0)` with no
        // arg2 is a likely mistake and is rejected below as a non-positive alignment.
        if (signedVal == 0 && cexprs.size() >= 2) return 0;
        return ValidateAlignValue(alignSpec, signedVal, "alignas");
    }

uint64_t MainListener::ParseAllocAlignArg(CFlatParser::AlignmentSpecifierContext* alignSpec) {
        if (alignSpec == nullptr) return 0;
        auto cexprs = alignSpec->constantExpression();
        size_t idx = (alignSpec->typeName() != nullptr) ? 0 : 1;
        if (cexprs.size() <= idx) return 0;
        llvm::Value* condVal = ParseConditionalExpression(cexprs[idx]->conditionalExpression());
        auto* ci = llvm::dyn_cast_or_null<llvm::ConstantInt>(condVal);
        if (ci == nullptr)
        {
            LogErrorContext(alignSpec, "alignas allocation alignment: argument must be a constant integer expression");
            return 0;
        }
        return ValidateAlignValue(alignSpec, ci->getSExtValue(), "alignas allocation alignment");
    }

CFlatParser::NewExpressionContext* MainListener::AsDirectNew(antlr4::tree::ParseTree* node) {
        while (node != nullptr)
        {
            if (auto* ne = dynamic_cast<CFlatParser::NewExpressionContext*>(node))
                return ne;
            if (node->children.size() != 1) return nullptr;
            node = node->children[0];
        }
        return nullptr;
    }

std::optional<uint64_t> MainListener::DirectEmbedByteCount(
        CFlatParser::AssignmentExpressionContext* expr) {
        if (expr == nullptr) return std::nullopt;
        std::string text = expr->getText();
        if (text.size() < 9 || text.rfind("embed(\"", 0) != 0 || text.back() != ')')
            return std::nullopt;
        std::string quoted = text.substr(6, text.size() - 7);
        if (quoted.size() < 2 || quoted.front() != '"' || quoted.back() != '"')
            return std::nullopt;
        auto* compiler = Compiler();
        std::string resolvedPath;
        std::string embedError;
        auto bytes = compiler->LoadEmbedFile(ProcessRawText(quoted, false),
                                             compiler->GetSourceFilePath(), resolvedPath, embedError);
        if (!bytes.has_value()) return std::nullopt;
        return (uint64_t)bytes->size();
    }

LLVMBackend::DeclTypeAndValue MainListener::getFunctionReturnType(CFlatParser::FunctionDefinitionContext* ctx) {
        auto declSpecs = ctx->declarationSpecifiers();

        return ParseDeclarationSpecifiers(declSpecs);
    }

/*
 * A core function loaded from the `--init` bitcode cache arrives LAZY: its body is on disk, not
 * in the module, so isMaterializable() is true and every guard below would read it as an opaque
 * call. Pull the body in so the fold sees exactly the IR the cold (parsed) path sees. Returns
 * false only when the callee is absent or its body cannot be read back.
 */
static bool MaterializeFoldCalleeIfLazy(llvm::Function* callee)
{
    if (callee == nullptr) return false;
    if (!callee->isMaterializable()) return true;
    if (auto error = callee->materialize())
    {
        llvm::consumeError(std::move(error));
        return false;
    }
    return true;
}

/*
 * Reduce a just-emitted default-construction value to a compile-time Constant by replaying
 * the constructor's own body. A synthesized (or constant-bodied user) no-arg constructor is a
 * single block that builds its result out of `insertvalue` over constants and calls to the
 * field types' own constructors, so a global can carry the same value its local twin gets.
 * Anything the walk does not recognise (a load, an allocation, a runtime call) yields nullptr
 * and the caller keeps the zero default.
 */
/*
 * Does running `callee` do anything besides produce its return value? The fold below replays the
 * VALUE, so any store, allocation, or unknown call inside the body would be dropped - and a
 * `= default` array must run its field initializers once per element (C++ semantics), not once.
 * Conservative: an unresolvable callee, a declaration, or a recursive call all count as having
 * effects, so the fold declines and the caller emits real per-element construction.
 */
static bool CalleeHasSideEffectsSafe(
    llvm::Function* callee,
    llvm::DenseSet<llvm::Function*>& visiting,
    llvm::DenseMap<llvm::Function*, bool>& memo)
{
    if (!MaterializeFoldCalleeIfLazy(callee)) return true;
    if (callee == nullptr || callee->isDeclaration() || callee->isMaterializable()
        || callee->isVarArg()) return true;
    auto memoIt = memo.find(callee);
    if (memoIt != memo.end()) return memoIt->second;
    if (!visiting.insert(callee).second) return true;
    bool hasEffects = false;
    for (llvm::BasicBlock& bb : *callee)
        for (llvm::Instruction& inst : bb)
        {
            if (auto* call = llvm::dyn_cast<llvm::CallBase>(&inst))
            {
                if (CalleeHasSideEffectsSafe(call->getCalledFunction(), visiting, memo))
                {
                    hasEffects = true;
                    break;
                }
            }
            else if (inst.mayWriteToMemory())
            {
                hasEffects = true;
                break;
            }
        }
    visiting.erase(callee);
    memo[callee] = hasEffects;
    return hasEffects;
}

struct FoldConstructedValueState
{
    static constexpr unsigned MaxConstructorFrames = 128;
    static constexpr unsigned MaxValueDepth = 512;
    llvm::DenseMap<const llvm::Function*, llvm::Constant*> FoldedFunctions;
    llvm::DenseSet<const llvm::Function*> ActiveFunctions;
    llvm::DenseSet<llvm::Function*> EffectVisiting;
    llvm::DenseMap<llvm::Function*, bool> EffectMemo;
    unsigned ConstructorFrames = 0;
    unsigned ValueDepth = 0;
};

static llvm::Constant* FoldConstructedValueToConstantImpl(
    llvm::Value* value,
    const llvm::DenseMap<const llvm::Value*, llvm::Constant*>& frameArgs,
    llvm::DenseMap<const llvm::Value*, llvm::Constant*>& frameMemo,
    FoldConstructedValueState& state);

/*
 * Memoizing front door for the fold walk. The memo is PER FRAME: the same instruction folds to
 * different constants under different constructor arguments, so a cross-frame memo would be
 * wrong. Within one frame it collapses shared sub-DAGs from exponential to linear, and the
 * depth cap bounds the native recursion the old flat depth-16 budget used to bound.
 */
static llvm::Constant* FoldConstructedValueToConstant(
    llvm::Value* value,
    const llvm::DenseMap<const llvm::Value*, llvm::Constant*>& frameArgs,
    llvm::DenseMap<const llvm::Value*, llvm::Constant*>& frameMemo,
    FoldConstructedValueState& state)
{
    if (value == nullptr) return nullptr;
    if (auto* c = llvm::dyn_cast<llvm::Constant>(value)) return c;
    if (state.ValueDepth >= FoldConstructedValueState::MaxValueDepth) return nullptr;
    auto memoIt = frameMemo.find(value);
    if (memoIt != frameMemo.end()) return memoIt->second;
    state.ValueDepth++;
    auto* folded = FoldConstructedValueToConstantImpl(value, frameArgs, frameMemo, state);
    state.ValueDepth--;
    frameMemo[value] = folded;
    return folded;
}

static llvm::Constant* FoldConstructedValueToConstantImpl(
    llvm::Value* value,
    const llvm::DenseMap<const llvm::Value*, llvm::Constant*>& frameArgs,
    llvm::DenseMap<const llvm::Value*, llvm::Constant*>& frameMemo,
    FoldConstructedValueState& state)
{
    if (auto* arg = llvm::dyn_cast<llvm::Argument>(value))
    {
        auto it = frameArgs.find(arg);
        return it == frameArgs.end() ? nullptr : it->second;
    }

    if (llvm::isa<llvm::InsertValueInst>(value))
    {
        struct InsertStep { llvm::InsertValueInst* Inst; };
        std::vector<InsertStep> steps;
        llvm::Value* aggregate = value;
        while (auto* iv = llvm::dyn_cast<llvm::InsertValueInst>(aggregate))
        {
            steps.push_back({iv});
            aggregate = iv->getAggregateOperand();
        }
        auto* folded = FoldConstructedValueToConstant(aggregate, frameArgs, frameMemo, state);
        if (folded == nullptr) return nullptr;
        for (auto it = steps.rbegin(); it != steps.rend(); ++it)
        {
            auto* element = FoldConstructedValueToConstant(
                it->Inst->getInsertedValueOperand(), frameArgs, frameMemo, state);
            if (element == nullptr) return nullptr;
            folded = llvm::ConstantFoldInsertValueInstruction(folded, element, it->Inst->getIndices());
            if (folded == nullptr) return nullptr;
        }
        return folded;
    }

    if (auto* ev = llvm::dyn_cast<llvm::ExtractValueInst>(value))
    {
        auto* agg = FoldConstructedValueToConstant(ev->getAggregateOperand(), frameArgs, frameMemo, state);
        if (agg == nullptr) return nullptr;
        return llvm::ConstantFoldExtractValueInstruction(agg, ev->getIndices());
    }

    if (auto* call = llvm::dyn_cast<llvm::CallBase>(value))
    {
        llvm::Function* callee = call->getCalledFunction();
        if (!MaterializeFoldCalleeIfLazy(callee)) return nullptr;
        if (callee == nullptr || callee->isDeclaration() || callee->isMaterializable()
            || callee->isVarArg()) return nullptr;
        if (callee->size() != 1) return nullptr;
        if (call->arg_size() != callee->arg_size()) return nullptr;
        if (call->arg_size() == 0)
        {
            auto memoIt = state.FoldedFunctions.find(callee);
            if (memoIt != state.FoldedFunctions.end()) return memoIt->second;
        }
        if (!state.ActiveFunctions.insert(callee).second) return nullptr;
        if (state.ConstructorFrames >= FoldConstructedValueState::MaxConstructorFrames)
        {
            state.ActiveFunctions.erase(callee);
            return nullptr;
        }
        // Replaying the VALUE is only equivalent to running the call when the call has no other
        // effect: a field initializer `int k = bump();` must run once per element, not once.
        if (CalleeHasSideEffectsSafe(callee, state.EffectVisiting, state.EffectMemo))
        {
            state.ActiveFunctions.erase(callee);
            return nullptr;
        }
        state.ConstructorFrames++;
        auto* ret = llvm::dyn_cast_or_null<llvm::ReturnInst>(cflat_llvm::GetTerminatorOrNull(&callee->back()));
        if (ret == nullptr || ret->getReturnValue() == nullptr)
        {
            state.ConstructorFrames--;
            state.ActiveFunctions.erase(callee);
            return nullptr;
        }
        // Every argument must itself fold, and it folds in the CALLER's frame.
        llvm::DenseMap<const llvm::Value*, llvm::Constant*> calleeArgs;
        unsigned i = 0;
        for (llvm::Argument& formal : callee->args())
        {
            auto* actual = FoldConstructedValueToConstant(call->getArgOperand(i), frameArgs, frameMemo, state);
            if (actual == nullptr)
            {
                state.ConstructorFrames--;
                state.ActiveFunctions.erase(callee);
                return nullptr;
            }
            calleeArgs[&formal] = actual;
            i++;
        }
        // The callee's body folds in its own frame: fresh args, fresh memo.
        llvm::DenseMap<const llvm::Value*, llvm::Constant*> calleeMemo;
        auto* folded = FoldConstructedValueToConstant(ret->getReturnValue(), calleeArgs, calleeMemo, state);
        state.ConstructorFrames--;
        state.ActiveFunctions.erase(callee);
        if (folded != nullptr && call->arg_size() == 0)
            state.FoldedFunctions[callee] = folded;
        return folded;
    }

    return nullptr;
}

/*
 * Global counterpart of the local declarator's default-construction call. The call itself
 * cannot stand as a global initializer, so emit it into a throwaway function (the same guard
 * the `= <expr>` global path uses), fold the result, and discard the IR either way.
 */
llvm::Constant* MainListener::TryFoldGlobalDefaultConstruction(
    const LLVMBackend::DeclTypeAndValue& typeValue)
{
        FoldConstructedValueState foldState;
        return TryFoldGlobalDefaultConstruction(typeValue, foldState);
    }

llvm::Constant* MainListener::TryFoldGlobalDefaultConstruction(
    const LLVMBackend::DeclTypeAndValue& typeValue, FoldConstructedValueState& foldState)
{
        auto* compiler = Compiler();
        if (compiler->GetDataStructure(typeValue.TypeName).StructType == nullptr) return nullptr;
        if (compiler->GetFunction(typeValue.TypeName) == nullptr) return nullptr;

        auto savedState = compiler->SaveBuilderState();
        auto* voidTy = llvm::FunctionType::get(compiler->builder->getVoidTy(), false);
        auto* tempFn = llvm::Function::Create(
            voidTy, llvm::Function::PrivateLinkage, "__global_default_init_tmp", compiler->module.get());
        auto* tempBB = llvm::BasicBlock::Create(*compiler->context, "entry", tempFn);
        compiler->builder->SetInsertPoint(tempBB);

        llvm::Constant* folded = nullptr;
        try
        {
            // The constructor call must lower as an ordinary instruction, not as file-scope IR.
            GlobalScopeGuard defaultCtorScope(global_scope);
            // forceRoot: an exact-key lookup, so a namespace walk cannot pick a sibling's ctor.
            llvm::Value* constructed = compiler->CreateOverloadedFunctionCall(typeValue.TypeName, {}, true);
            llvm::DenseMap<const llvm::Value*, llvm::Constant*> noArgs;
            llvm::DenseMap<const llvm::Value*, llvm::Constant*> rootMemo;
            folded = FoldConstructedValueToConstant(constructed, noArgs, rootMemo, foldState);
        }
        catch (...)
        {
            // LogError THROWS and expect_error resumes the walk, so the temp IR must go and the
            // builder must be restored here or the next statement emits into an erased block.
            tempFn->eraseFromParent();
            compiler->RestoreBuilderState(savedState);
            throw;
        }

        tempFn->eraseFromParent();
        compiler->RestoreBuilderState(savedState);
        // A fold that produced the wrong shape is not usable as this global's initializer.
        if (folded != nullptr && folded->getType() != compiler->GetType(typeValue)) return nullptr;
        return folded;
    }

void MainListener::ResolvePendingGlobalDefaultConstructions()
{
        auto pending = std::move(pendingGlobalDefaultConstructions_);
        pendingGlobalDefaultConstructions_.clear();
        FoldConstructedValueState foldState;
        for (const auto& item : pending)
        {
            if (item.Global == nullptr || item.Global->isDeclaration()) continue;
            auto* folded = TryFoldGlobalDefaultConstruction(item.TypeValue, foldState);
            if (folded != nullptr)
            {
                item.Global->setInitializer(folded);
                continue;
            }
            LogWarningContext(item.Context, std::format(
                "({}) global is zero-initialized: its default construction could not be reduced to a compile-time constant here.",
                SpellType(*compilerLLVM, item.TypeValue)));
        }
    }

// Replicates one element constant across a fixed-array type, recursing through every inner
// dimension. Returns null when the shapes do not line up, so the caller falls back to seeding.
llvm::Constant* MainListener::SplatConstantOverFixedArray(llvm::Constant* elemConst, llvm::Type* arrType) {
        auto* arrTy = llvm::dyn_cast_or_null<llvm::ArrayType>(arrType);
        if (elemConst == nullptr || arrTy == nullptr) return nullptr;

        llvm::Type* inner = arrTy->getElementType();
        llvm::Constant* fill = (inner == elemConst->getType())
            ? elemConst
            : SplatConstantOverFixedArray(elemConst, inner);
        if (fill == nullptr) return nullptr;

        std::vector<llvm::Constant*> elems((size_t)arrTy->getNumElements(), fill);
        return llvm::ConstantArray::get(arrTy, elems);
    }

llvm::Value* MainListener::GenerateDefaultValue(const LLVMBackend::DeclTypeAndValue& typeValue) {
        auto* compiler = Compiler();
        // Apply active type-parameter substitutions as a fallback in case the caller
        // hasn't already resolved them (e.g. "V" inside a generic method body).
        auto resolved = typeValue;
        auto substIt = activeTypeSubstitutions.find(resolved.TypeName);
        if (substIt != activeTypeSubstitutions.end())
        {
            resolved.TypeName = substIt->second;
            StripOwnershipQualifiers(resolved.TypeName);  // default-value resolution uses the bare type
            while (!resolved.TypeName.empty() && resolved.TypeName.back() == '*')
            {
                resolved.TypeName.pop_back();
                resolved.Pointer = true;
            }
        }

        auto* llvmType = compiler->GetType(resolved);
        if (!llvmType) return nullptr;

        // A fixed-array field default must construct each aggregate element, not just zero-fill
        // the enclosing array. The local declarator already uses EmitFixedArrayDefaultInit; use
        // the same element walk when a synthetic aggregate constructor asks for the field value.
        if (!resolved.Pointer && llvmType->isArrayTy() && !global_scope)
        {
            auto* array = llvm::cast<llvm::ArrayType>(llvmType);
            if (resolved.ConstArraySize == 0)
            {
                resolved.ConstArraySize = array->getNumElements();
                for (auto* inner = array->getElementType(); inner->isArrayTy();
                     inner = inner->getArrayElementType())
                    resolved.ConstInnerDimensions.push_back(
                        llvm::cast<llvm::ArrayType>(inner)->getNumElements());
            }
            auto* element = array->getElementType();
            while (element->isArrayTy()) element = element->getArrayElementType();
            if (element->isStructTy()
                && compiler->GetDataStructure(resolved.TypeName).StructType != nullptr
                && compiler->GetFunction(resolved.TypeName) != nullptr)
            {
                auto* slot = compiler->AllocaAtEntry(llvmType, nullptr, "arrayfielddefault");
                compiler->builder->CreateStore(llvm::Constant::getNullValue(llvmType), slot);
                EmitFixedArrayDefaultInit(slot, resolved);
                return compiler->builder->CreateLoad(llvmType, slot, "arrayfielddefault_value");
            }
        }

        if (!resolved.Pointer && llvmType->isStructTy())
        {
            auto structData = compiler->GetDataStructure(resolved.TypeName);
            // forceRoot: the guards above are EXACT-key lookups, so the default ctor must be the
            // one of that exact type - a namespace walk here would call a same-named sibling's.
            if (structData.StructType != nullptr && compiler->GetFunction(resolved.TypeName))
            {
                if (!global_scope)
                    return compiler->CreateOverloadedFunctionCall(resolved.TypeName, {}, true);
                // At global scope the call is not an initializer; take its constant value if it has one.
                if (auto* folded = TryFoldGlobalDefaultConstruction(resolved))
                    return folded;
            }
        }

        return llvm::Constant::getNullValue(llvmType);
    }

MainListener::MainListener(CFlatParser* parser, LLVMBackend* compilerLLVM, const std::string& filename)
        : genericStructTemplates(compilerLLVM->gts.genericStructTemplates)
        , genericClassTemplates(compilerLLVM->gts.genericClassTemplates)
        , genericStructTypeParams(compilerLLVM->gts.genericStructTypeParams)
        , genericStructValueParams(compilerLLVM->gts.genericStructValueParams)
        , instantiatedGenerics(compilerLLVM->gts.instantiatedGenerics)
        , genericStructConstraints(compilerLLVM->gts.genericStructConstraints)
        , genericClassConstraints(compilerLLVM->gts.genericClassConstraints)
        , genericStructPackIndex(compilerLLVM->gts.genericStructPackIndex)
        , genericClassPackIndex(compilerLLVM->gts.genericClassPackIndex)
        , genericFunctionPackIndex(compilerLLVM->gts.genericFunctionPackIndex)
        , genericInterfacePackIndex(compilerLLVM->gts.genericInterfacePackIndex)
        , genericInterfaceTemplates(compilerLLVM->gts.genericInterfaceTemplates)
        , genericInterfaceTypeParams(compilerLLVM->gts.genericInterfaceTypeParams)
        , genericInterfaceValueParams(compilerLLVM->gts.genericInterfaceValueParams)
        , instantiatedInterfaces(compilerLLVM->gts.instantiatedInterfaces)
        , genericFunctionTemplates(compilerLLVM->gts.genericFunctionTemplates)
        , genericFunctionTypeParams(compilerLLVM->gts.genericFunctionTypeParams)
        , genericFunctionValueParams(compilerLLVM->gts.genericFunctionValueParams)
        , instantiatedGenericFunctions(compilerLLVM->gts.instantiatedGenericFunctions)
        , genericFunctionConstraints(compilerLLVM->gts.genericFunctionConstraints)
        , pendingInstantiations(compilerLLVM->gts.pendingInstantiations)
        , tupleTypeArgs(compilerLLVM->gts.tupleTypeArgs) {
        this->parser = parser;
        this->compilerLLVM = compilerLLVM;
        this->sourceFileName = filename;
        // The forward-ref scan for this file is complete, so every struct's fields are
        // registered and the queued 'alias' return inference can be gated on ownership.
        compilerLLVM->ResolvePendingAliasReturnInference();
    }

void MainListener::SetImportNamespace(const std::string& ns) { importNamespace_ = ns; }

antlr4::BufferedTokenStream* MainListener::GetTokens() const {
        return parser ? dynamic_cast<antlr4::BufferedTokenStream*>(parser->getTokenStream()) : nullptr;
    }

void MainListener::ParseInterfaceDefinition(CFlatParser::InterfaceDefinitionContext* ctx,
                                  const std::string& namespaceName) {
        auto* nameGid = ctx->genericIdentifier();
        if (!nameGid || !nameGid->Identifier()) return;

        std::string baseName = nameGid->Identifier()->getText();
        // Qualified by the enclosing namespace, exactly like a struct or class, so two
        // namespaces may each declare their own "IV" - generic templates included.
        std::string name = namespaceName.empty() ? baseName : namespaceName + "." + baseName;

        // Validate type-level annotations (e.g. [uuid("...")]) against the registry and replace the
        // forward scan's raw record with the validated set, so an unknown annotation that errored
        // does not linger as queryable.
        Compiler(ctx)->SetTypeAnnotations(name, ParseAnnotationList(ctx->annotationList()));

        if (nameGid->genericTypeParameters() == nullptr)
        {
            for (auto* spec : ctx->baseSpecifier())
                if (spec->genericTypeParameters() != nullptr)
                    LogErrorContext(spec,
                        "generic arguments are not supported on a non-generic interface parent");
        }

        // Collect parent interface names, resolved to the names they are registered under.
        std::vector<std::string> parentNames;
        for (auto* spec : ctx->baseSpecifier())
            parentNames.push_back(Compiler(ctx)->ResolveInterfaceName(BaseSpecifierName(spec)));

        // Generic interface template - store for on-demand instantiation
        if (nameGid->genericTypeParameters() != nullptr)
        {
            if (Compiler()->gts.scannedGenericStructNames.count(name) != 0
                || genericStructTemplates.count(name) != 0
                || genericClassTemplates.count(name) != 0)
                LogErrorContext(ctx, std::format(
                    "generic interface '{}' conflicts with a generic struct/class template of the same name",
                    name));
            // Keyed on the namespace-QUALIFIED name, like a generic struct or class. The use site
            // resolves its spelled base to this key via LLVMBackend::ResolveGenericTemplateBase.
            std::vector<std::string> valueParams;
            std::vector<std::string> valueDefaults;
            auto typeParams = ParseGenericTypeParameters(nameGid->genericTypeParameters(), &valueParams,
                                                         &valueDefaults);
            genericInterfaceTemplates[name] = ctx;
            Compiler()->gts.genericTemplateNamespace[name] = Compiler()->GetCurrentNamespace();
            genericInterfaceTypeParams[name] = typeParams;
            compilerLLVM->gts.genericInterfaceValueParams[name] = valueParams;
            compilerLLVM->gts.genericInterfaceValueDefaults[name] = valueDefaults;
            {
                auto entries = nameGid->genericTypeParameters()->typeParameterList()->typeParameterEntry();
                bool hasPack = !entries.empty() && entries.back()->Ellipsis() != nullptr;
                genericInterfacePackIndex[name] = hasPack ? (typeParams.size() - 1) : std::string::npos;
            }
            return;
        }

        ResolvedInterfaceMembersScope memberScope(resolvedInterfaceMembers_, (const void*)ctx);

        std::vector<LLVMBackend::InterfaceMethod> methods;
        for (auto method : InterfaceMethods(ctx))
        {
            if (RejectVariadicInterfaceMethod(name, method)) continue;
            LLVMBackend::InterfaceMethod m;
            m.ReturnType = ParseDeclarationSpecifiers(method->declarationSpecifiers());
            bool fixedReturn = false;
            for (auto* spec : method->declarationSpecifiers()->declarationSpecifier())
                if (auto* dims = ArrayDimsOf(spec); dims != nullptr && !dims->assignmentExpression().empty())
                    fixedReturn = true;
            if (fixedReturn)
                LogErrorContext(method, std::format(
                    "interface method '{}.{}' cannot return a fixed-size array by value; return a struct with the array as a field or take an out-parameter",
                    name, getInterfaceMethodName(method)));
            m.Name = getInterfaceMethodName(method);
            auto declParams = ParseParameterTypeList(method->parameterTypeList());
            for (const auto& p : declParams)
            {
                LLVMBackend::TypeAndValue tv = p;
                m.Parameters.push_back(tv);
            }
            methods.push_back(std::move(m));
        }

        Compiler(ctx)->CreateInterfaceDefinition(name, parentNames, methods, ParseInterfaceFields(ctx),
                                                 DefinitionSiteText(Compiler(ctx), ctx));
    }

bool MainListener::RejectVariadicInterfaceMethod(const std::string& ifaceName,
                                       CFlatParser::InterfaceMethodContext* method) {
        auto* paramList = method->parameterTypeList();
        if (paramList == nullptr || paramList->Ellipsis() == nullptr) return false;
        LogErrorContext(method, std::format(
            "interface method '{}.{}' cannot be variadic - '...' is not dispatchable through a "
            "vtable; declare a fixed-parameter overload instead",
            ifaceName, getInterfaceMethodName(method)));
        return true;
    }

std::vector<LLVMBackend::TypeAndValue> MainListener::ParseInterfaceFields(CFlatParser::InterfaceDefinitionContext* ctx) {
        StructFieldDeclGuard fieldCtx(inStructFieldDecl_);
        InterfaceFieldDeclGuard interfaceFieldCtx(inInterfaceFieldDecl_);
        // An interface body is never a union body - disarmed explicitly because a generic
        // interface can be instantiated from inside one (a union member typed `IFoo<int>`).
        UnionFieldDeclGuard notUnion(inUnionFieldDecl_, false);

        std::vector<LLVMBackend::TypeAndValue> fields;
        for (auto* f : InterfaceFields(ctx))
        {
            LLVMBackend::DeclTypeAndValue tv = ParseDeclarationSpecifiers(f->declarationSpecifiers());
            tv.VariableName = f->directDeclarator()->getText();
            // Interface fields retain the contract marker: unlike class fields, they cannot
            // desugar to a wrapper without losing the ABI-level ownership agreement.
            if (HasSoftDeclarationSpecifier(f->declarationSpecifiers(), "unique"))
                tv.IsUnique = true;
            if (tv.IsUnique)
                ValidateUniqueField(tv, f);
            else
                ValidateAllocAlignField(tv, f);
            fields.push_back(std::move(tv));
        }
        return fields;
    }

std::string MainListener::DeclaringNamespaceOf(LLVMBackend* compiler, const std::string& key) {
        auto it = compiler->gts.genericTemplateNamespace.find(key);
        return it != compiler->gts.genericTemplateNamespace.end() ? it->second : std::string{};
    }

void MainListener::ParseUsingDeclaration(CFlatParser::UsingDeclarationContext* ctx) {
        auto* compiler = Compiler(ctx);

        // The alias name may be a plain Identifier or the 'string' keyword token.
        std::string alias;
        if (ctx->String())
            alias = ctx->String()->getText();
        else if (ctx->Identifier() != nullptr)
            alias = ctx->Identifier()->getText();

        if (compiler->AliasInCurrentScope(alias))
            LogErrorContext(ctx, compiler->LocalizeMessage(
                "alias '{}' is already defined in this scope", { alias }));

        auto* typeSpec = ctx->typeSpecifier();

        // Function-type alias (using Cb = function<R(Args)>): a closure type carries a call
        // signature, not a plain type name, so it is stored structurally in functionTypeAliases
        // and expanded at every use site (ParseDeclarationSpecifiers). Must be handled before the
        // type/namespace dispatch below, which only recognizes named types.
        if (auto* fpSpec = typeSpec->functionPointerSpecifier())
        {
            compiler->RegisterFunctionTypeAlias(alias, BuildFuncPtrAliasType(fpSpec));
            if (auto* s = compiler->GetSymbolSink())
                s->Register(SymbolKind::TypeAlias, alias, compiler->GetSourceFilePath(),
                            (int)ctx->getStart()->getLine(), (int)ctx->getStart()->getCharPositionInLine(),
                            "using " + alias + " = " + typeSpec->getText(), {},
                            ExtractLeadingDoc(GetTokens(), ctx->getStart()));
            return;
        }

        std::string target = typeSpec->getText();
        if (auto subst = activeTypeSubstitutions.find(target); subst != activeTypeSubstitutions.end())
            target = subst->second;
        // A pointer alias (using Handle = void*) stores its trailing stars in the alias string;
        // the stars are peeled back onto the pointer flags at the resolution site (GetType /
        // ParseDeclarationSpecifiers). Storage stays string-shaped - no descriptor struct.
        std::string suffix(ctx->pointer() != nullptr ? ctx->pointer()->Star().size() : 0, '*');
        // An alias RHS over the cap names a type no declarator could ever spell; reject it here
        // rather than only at the (possibly absent) use site.
        if ((int)suffix.size() > PointerDepthCap)
            LogErrorContext(ctx, PointerDepthCapMessage(
                std::format("using alias '{}' = '{}'", alias, target), (int)suffix.size()));

        // An array alias (using Vec3 = float[3]) folds each bracketed size NOW (the alias string
        // cannot keep a live parse node) and bakes the integers into the stored string ("float[3]").
        // The sizes are re-parsed onto ConstArraySize / ConstInnerDimensions at the resolution site.
        std::string arraySuffix;
        if (auto* dimSpec = ArrayDimsOf(ctx))
        {
            if (ArrayPtrOf(ctx) != nullptr)
                compiler->LogError(std::format(
                    "using alias '{}': pointer to fixed array 'T[N]*' is not a valid type", alias));
            // `using M = int[][3];` would otherwise fold to "int[3]" - the empty pair is dropped.
            if (DimSpecIsUnsizedMultiDim(dimSpec))
                compiler->LogError(std::format("using alias '{}': {}", alias,
                    UnsizedMultiDimMessage(SpellType(*compiler,
                        LLVMBackend::TypeAndValue{ .TypeName = target }))));
            auto dims = dimSpec->assignmentExpression();
            if (dims.empty())
                compiler->LogError(std::format(
                    "using alias '{}': array-view 'T[]' aliases are not supported; use a fixed size 'T[N]'",
                    alias));
            for (auto* d : dims)
            {
                auto* v = ParseAssignmentExpression(d);
                auto* ci = llvm::dyn_cast_or_null<llvm::ConstantInt>(v);
                if (ci == nullptr)
                    compiler->LogError(std::format("using alias '{}': array size must be a compile-time constant", alias));
                else if (ci->getZExtValue() == 0)
                    compiler->LogError(std::format("using alias '{}': array size must be greater than zero", alias));
                else
                    arraySuffix += "[" + std::to_string(ci->getZExtValue()) + "]";
            }
        }
        suffix += arraySuffix;  // pointer stars precede array brackets ("int*[3]")

        // An alias of an alias (using PVStatics = IPropertyValueStatics;) names whatever the RHS
        // already names. One hop - enough for the qualified-WinMD-name -> short-alias -> shorter
        // -alias chains that consuming a .winmd produces.
        target = compiler->ResolveQualifiedName(target);
        target = compiler->ResolveGenericBaseAlias(compiler->ResolveTypeAlias(target));

        // Alias targets may already carry decoration from an earlier alias (e.g. Q = P where
        // P = int*). Validate the undecorated base, then put every inherited decoration back into
        // the canonical string stored for the new alias.
        std::string targetBase = target;
        std::vector<uint64_t> targetArrayDims;
        PeelAliasArrayDims(targetBase, targetArrayDims);
        int targetPointerDepth = PeelAliasPointerStars(targetBase);
        std::string targetDecorated = targetBase + std::string(targetPointerDepth, '*');
        for (uint64_t dim : targetArrayDims)
            targetDecorated += "[" + std::to_string(dim) + "]";

        // A name that only becomes a type once its <...> arguments are supplied.
        auto IsGenericTemplateName = [&](const std::string& n) {
            return genericStructTemplates.count(n) != 0 || genericClassTemplates.count(n) != 0
                || genericInterfaceTemplates.count(n) != 0 || compiler->IsWinrtGenericBase(n);
        };

        // Generic RHS (using IL = list<int>): mangle to list$int and pre-declare the shell +
        // default ctor to enqueue the instantiation, then alias to the mangled name. The base
        // must name a generic template, else the RHS is malformed - LogError rather than
        // silently degrading to a namespace alias (the historical bug).
        std::string baseName;
        if (auto* genParams = GenericSpecOf(typeSpec, baseName))
        {
            baseName = compiler->ResolveGenericBaseAlias(baseName);
            std::vector<std::string> typeArgs;
            for (auto* entry : genParams->typeParameterList()->typeParameterEntry())
                typeArgs.push_back(ResolveTypeArgEntry(entry));
            std::string mangledName = MangledGenericName(baseName, typeArgs);

            if (genericStructTemplates.count(baseName) != 0
                || genericClassTemplates.count(baseName) != 0
                || genericInterfaceTemplates.count(baseName) != 0)
            {
                // CFlat generic: enqueue the instantiation (shell + default ctor created
                // immediately, body emitted by the next ProcessPendingInstantiations).
                QueueGenericInstantiation(baseName, typeArgs, mangledName, genParams);
            }
            // A deferred winmd generic interface (e.g. IAsyncOperationWithProgress<string,
            // HttpProgress>): instantiate the concrete thin interface + PIID on demand, exactly as
            // a cast or iidof use-site does, so the alias names a real type instead of erroring.
            else if (!compiler->InstantiateWinrtGenericInterface(baseName, typeArgs, mangledName))
            {
                compiler->LogError(std::format("using alias '{}' = '{}': '{}' is not a generic type",
                                               alias, SpellType(*compiler,
                                                   LLVMBackend::TypeAndValue{ .TypeName = target }),
                                               SpellType(*compiler,
                                                   LLVMBackend::TypeAndValue{ .TypeName = baseName })));
                return;
            }
            compiler->RegisterTypeAlias(alias, mangledName + suffix);
            if (auto* s = compiler->GetSymbolSink())
                s->Register(SymbolKind::TypeAlias, alias, compiler->GetSourceFilePath(),
                            (int)ctx->getStart()->getLine(), (int)ctx->getStart()->getCharPositionInLine(),
                            "using " + alias + " = " + target + suffix, {},
                            ExtractLeadingDoc(GetTokens(), ctx->getStart()));
            return;
        }

        // Alias of a GENERIC BASE (using IReference = Windows.Foundation.IReference;) so a body can
        // keep writing the familiar `IReference<int>`. Only meaningful with no suffix - a base is
        // not a type, so `T*` / `T[N]` decoration on it is meaningless.
        if (suffix.empty() && IsGenericTemplateName(targetBase))
        {
            compiler->RegisterGenericBaseAlias(alias, targetBase);
            if (auto* s = compiler->GetSymbolSink())
                s->Register(SymbolKind::TypeAlias, alias, compiler->GetSourceFilePath(),
                            (int)ctx->getStart()->getLine(), (int)ctx->getStart()->getCharPositionInLine(),
                            "using " + alias + " = " + target, {},
                            ExtractLeadingDoc(GetTokens(), ctx->getStart()));
            return;
        }

        // `using D = Dir;` where Dir is an enum: alias the enum's registered (scoped) key, so the
        // alias resolves as a type and not as a namespace alias onto the member scope.
        if (std::string enumKey = compiler->ResolveEnumTypeName(targetBase); !enumKey.empty())
        {
            targetBase = enumKey;
            targetDecorated = enumKey + std::string(targetPointerDepth, '*');
            for (uint64_t dim : targetArrayDims)
                targetDecorated += "[" + std::to_string(dim) + "]";
            compiler->RegisterTypeAlias(alias, targetDecorated + suffix);
            return;
        }

        // If the target names a known type (interface, struct, primitive, or an imported winmd
        // type - delegates/enums/runtime classes have no CFlat struct but are nameable), register a
        // type alias. Otherwise treat it as a namespace alias.
        if (compiler->IsInterfaceType(targetBase) || compiler->GetDataStructure(targetBase).StructType != nullptr
            || LLVMBackend::IsPrimitiveTypeName(targetBase) || compiler->IsWinrtFullName(targetBase))
        {
            // A winmd projection must never silently displace a CFlat type of the same short name -
            // that is the collision the qualified registration exists to prevent. An alias is the
            // user explicitly claiming the name, so an outright clash is an error, not a shadow.
            if (compiler->IsWinrtFullName(targetBase) && alias != targetBase
                && (compiler->IsInterfaceType(alias) || compiler->GetDataStructure(alias).StructType != nullptr))
            {
                compiler->LogError(std::format(
                    "using alias '{}' = '{}': '{}' already names a type in this program; "
                    "pick a different alias or spell the WinMD type fully qualified",
                    alias, targetDecorated, alias));
                return;
            }
            compiler->RegisterTypeAlias(alias, targetDecorated + suffix);
            if (auto* s = compiler->GetSymbolSink())
                s->Register(SymbolKind::TypeAlias, alias, compiler->GetSourceFilePath(),
                            (int)ctx->getStart()->getLine(), (int)ctx->getStart()->getCharPositionInLine(),
                            "using " + alias + " = " + targetDecorated + suffix, {},
                            ExtractLeadingDoc(GetTokens(), ctx->getStart()));
        }
        else if (!suffix.empty())
        {
            // A pointer alias whose base is neither a type nor (a namespace can't take a '*')
            // is malformed - the RHS looks like a type but resolves to nothing.
            compiler->LogError(std::format("using alias '{}' = '{}': '{}' is not a known type",
                                           alias, SpellType(*compiler,
                                               LLVMBackend::TypeAndValue{ .TypeName = targetDecorated + suffix }),
                                           SpellType(*compiler,
                                               LLVMBackend::TypeAndValue{ .TypeName = targetBase })));
        }
        else if (global_scope)
            compiler->RegisterNamespaceAlias(alias, targetBase);
        else
            compiler->RegisterLocalNamespaceAlias(alias, targetBase);
    }

void MainListener::ParseAnnotationDefinition(CFlatParser::AnnotationDefinitionContext* ctx) {
        auto* compiler = Compiler(ctx);
        std::string name = ctx->Identifier()->getText();

        // Validate: no duplicate declaration
        if (compiler->annotationRegistry.count(name))
        {
            // Already registered by ForwardRefScanner - nothing to emit (no LLVM type).
            return;
        }

        // Fallback registration in case ForwardRefScanner missed it (shouldn't happen).
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

void MainListener::ParseExternalDeclaration(CFlatParser::ExternalDeclarationContext* ctx, const std::string& namespaceName) {
        auto func = ctx->functionDefinition();
        auto dataStruct = ctx->structDefinition();
        auto classDef = ctx->classDefinition();
        auto decl = ctx->declaration();
        auto iface = ctx->interfaceDefinition();
        auto ns = ctx->namespaceDefinition();
        auto usingDecl = ctx->usingDeclaration();
        auto ifConst = ctx->ifConstDeclaration();

        if (auto annDef = ctx->annotationDefinition())
        {
            ParseAnnotationDefinition(annDef);
            return;
        }

        if (iface != nullptr)
        {
            ParseInterfaceDefinition(iface, namespaceName);
        }
        else if (ns != nullptr)
        {
            ParseNamespaceDefinition(ns, namespaceName);
        }
        else if (usingDecl != nullptr)
        {
            ParseUsingDeclaration(usingDecl);
        }
        else if (ifConst != nullptr)
        {
            ParseIfConstDeclaration(ifConst, namespaceName);
        }
        else if (decl != nullptr)
        {
            ParseDeclaration(decl, namespaceName);
        }
        else if (func != nullptr)
        {
            global_scope = false;
            ParseFunctionDefinition(func, {}, namespaceName);
            global_scope = true;
        }
        else if (dataStruct != nullptr)
        {
            ParseStructDefinition(dataStruct, {}, namespaceName);
        }
        else if (classDef != nullptr)
        {
            ParseClassDefinition(classDef, {}, namespaceName);
        }
        else if (auto progDef = ctx->programDefinition())
        {
            ParseProgramDefinition(progDef);
        }
        else if (auto* lfg = ctx->lockFieldGroup())
        {
            ParseGlobalLockGroup(lfg, namespaceName);
        }
        else if (auto imp = ctx->importDeclaration())
        {
            if (imp->children.size() >= 2 && imp->children[1]->getText() == "program")
            {
                std::string alias = ImportProgramAlias(imp);
                ParseImportedProgramDefinition(alias);
            }
        }
        else if (auto expectErrDecl = ctx->expectErrorDeclaration())
        {
            // Bare file-scope form is armed before imports in LLVMBackend::Compile. A namespace
            // form is armed here because its members are processed under the namespace scope.
            if (expectErrDecl->externalDeclaration().empty())
            {
                // A namespace has no function scope-exit handler, so remember its bare form
                // and let ParseNamespaceDefinition report a missing error after its members.
                if (!namespaceName.empty())
                {
                    std::string rawText = expectErrDecl->StringLiteral()->getText();
                    compilerLLVM->expectedError = ProcessRawText(rawText);
                    compilerLLVM->expectedErrorScopeDepth = compilerLLVM->stackNamedVariable.size();
                }
            }
            else
            {
                // Scoped block form at file scope: expect_error("msg") { functionDef / structDef / ... }
                std::string rawText = expectErrDecl->StringLiteral()->getText();
                compilerLLVM->expectedError = ProcessRawText(rawText);
                compilerLLVM->expectedErrorScopeDepth = SIZE_MAX;
                auto ownedTempMark = compilerLLVM->MarkOwnedTemps();
                std::unordered_set<std::string> typesBefore;
                for (const auto& [name, _] : compilerLLVM->dataStructures)
                    typesBefore.insert(name);
                std::unordered_set<std::string> globalsBefore;
                for (const auto& global : compilerLLVM->module->globals())
                    globalsBefore.insert(global.getName().str());

                bool errorReceived = false;
                try
                {
                    for (auto* extDecl : expectErrDecl->externalDeclaration())
                        ParseExternalDeclaration(extDecl, namespaceName);
                }
                catch (const ExpectedErrorReceived&)
                {
                    errorReceived = true;
                    // ForwardRefScanner may have queued generic work for this failed declaration
                    // before the scoped expectation ran. Let later source uses queue it again.
                    for (const auto& pending : pendingInstantiations)
                        instantiatedGenerics.erase(pending.mangledName);
                    pendingInstantiations.clear();
                    // The declaration that failed inside the expectation is not a usable type.
                    // Roll back shells created by this block so they cannot shadow outer names.
                    for (auto it = compilerLLVM->dataStructures.begin(); it != compilerLLVM->dataStructures.end(); )
                    {
                        if (!typesBefore.count(it->first)
                            && it->second.StructType != nullptr && it->second.StructType->isOpaque())
                            it = compilerLLVM->dataStructures.erase(it);
                        else
                            ++it;
                    }
                    auto sizeFailedAggregateShell = [&](const std::string& baseName) {
                        std::string typeName = namespaceName.empty() ? baseName : namespaceName + "." + baseName;
                        auto it = compilerLLVM->dataStructures.find(typeName);
                        if (it != compilerLLVM->dataStructures.end() && it->second.StructType != nullptr
                            && it->second.StructType->isOpaque())
                        {
                            it->second.StructFields.clear();
                            it->second.StructType->setBody(llvm::ArrayRef<llvm::Type*>());
                        }
                    };
                    for (auto* extDecl : expectErrDecl->externalDeclaration())
                    {
                        if (auto* failedStruct = extDecl->structDefinition())
                            sizeFailedAggregateShell(failedStruct->directDeclarator()->getText());
                        if (auto* failedClass = extDecl->classDefinition())
                            sizeFailedAggregateShell(failedClass->directDeclarator()->getText());
                    }
                    // A global whose type is the shell just rolled back keeps an unsized
                    // initializer, which the LLVM verifier rejects. Roll those back too - the
                    // failed declaration never produced a usable global.
                    std::vector<llvm::GlobalVariable*> unsizedGlobals;
                    for (auto& global : compilerLLVM->module->globals())
                        if (!globalsBefore.count(global.getName().str())
                            && !global.getValueType()->isSized())
                            unsizedGlobals.push_back(&global);
                    for (auto* global : unsizedGlobals)
                    {
                        const std::string name = global->getName().str();
                        global->replaceAllUsesWith(llvm::PoisonValue::get(global->getType()));
                        global->eraseFromParent();
                        compilerLLVM->globalNamedVariable.erase(name);
                        compilerLLVM->globalVariableTypes.erase(name);
                        compilerLLVM->globalDeclSite.erase(name);
                    }
                    // The rest of the block never ran, so its `if const` arms were never decided.
                    ForgetIfConstGuardedImpls(compilerLLVM, expectErrDecl);
                    compilerLLVM->AbortFunctionBlocks(0);
                    compilerLLVM->DiscardOwnedTempsSince(ownedTempMark);
                    compilerLLVM->ClearCurrentSubprogram();
                    compilerLLVM->RestoreFileScopeExpectedError();
                    // The throw unwound past the `global_scope = true` restore in the
                    // function branch; recovery resumes at file scope, so re-assert it.
                    global_scope = true;
                }

                if (!errorReceived && !compilerLLVM->expectedError.empty())
                {
                    std::cout << std::format("FAIL: expected error '{}' did not occur\n",
                                              compilerLLVM->expectedError);
                    compilerLLVM->expectedError.clear();
                    if (compilerLLVM->diagnosticSink_)
                        throw CompilerAbortException{ "expected error did not occur", compilerLLVM->sourceFileName, 0, 0 };
                    else
                        compilerLLVM->FailCompilation("expected error did not occur");
                }
            }
        }

        // Process any generic instantiations queued while parsing the above item.
        // This is the only safe point: the IRBuilder has no active function/block.
        ProcessPendingInstantiations();
    }

void MainListener::ParseGlobalLockGroup(CFlatParser::LockFieldGroupContext* ctx, const std::string& namespaceName) {
        auto groupArgs = ctx->lockClause()->lockArgList()->expression();
        if (groupArgs.size() != 1)
        {
            LogErrorContext(ctx, "a lock group takes exactly one guardian, e.g. 'lock(g_mtx) { ... }'.");
            return;
        }
        std::string guardianName = GetLockArgCanonical(groupArgs[0]);

        compilerLLVM->pendingGlobalGuardedBy = guardianName;
        try
        {
            for (auto* decl : ctx->declaration())
                ParseDeclaration(decl, namespaceName);
        }
        catch (...)
        {
            compilerLLVM->pendingGlobalGuardedBy.clear();
            throw;
        }
        compilerLLVM->pendingGlobalGuardedBy.clear();

        for (auto* func : ctx->functionDefinition())
        {
            global_scope = false;
            ParseFunctionDefinition(func, {}, namespaceName);
            global_scope = true;
        }
    }

bool MainListener::DeclSpecHasConst(CFlatParser::DeclarationSpecifiersContext* declSpec) {
        if (declSpec == nullptr) return false;
        for (auto* ds : declSpec->declarationSpecifier())
            if (auto* q = ds->typeQualifier())
                if (q->getText() == "const") return true;
        return false;
    }

void MainListener::AppendResolvedMembers(const std::vector<CFlatParser::AggregateMemberContext*>& members,
                               std::vector<CFlatParser::AggregateMemberContext*>& out) {
        for (auto* m : members)
        {
            if (auto* ifConst = m->ifConstMember()) AppendIfConstMembers(ifConst, out);
            else out.push_back(m);
        }
    }

void MainListener::AppendIfConstMembers(CFlatParser::IfConstMemberContext* ctx,
                              std::vector<CFlatParser::AggregateMemberContext*>& out) {
        int decision = DecideIfConstCondition(ctx->expression());
        if (decision < 0)
        {
            LogErrorContext(ctx, "'if const' condition must be a compile-time constant expression");
            return;
        }

        bool taken = decision != 0;
        auto ifBlocks = ctx->ifConstMemberBlock();
        if (ifBlocks.empty())
            return;

        CFlatParser::IfConstMemberBlockContext* branchBlock = nullptr;
        if (taken)
        {
            branchBlock = ifBlocks[0];
        }
        else if (auto* elseIf = ctx->ifConstMember())
        {
            // Chained `else if const` - the else arm is another if const, not a brace block. Taking
            // the else PATH peels exactly the level the scanner appended for it (nested=true).
            RetractIfConstArmGuardedImpls(compilerLLVM, elseIf, /*nested=*/true);
            AppendIfConstMembers(elseIf, out);
            return;
        }
        else if (ifBlocks.size() > 1)
        {
            branchBlock = ifBlocks[1];
        }

        if (branchBlock)
        {
            // Taken arm - see the matching retraction in ParseIfConstDeclaration.
            RetractIfConstArmGuardedImpls(compilerLLVM, branchBlock);
            AppendResolvedMembers(branchBlock->aggregateMember(), out);
        }
    }

void MainListener::AppendResolvedInterfaceMembers(const std::vector<CFlatParser::InterfaceMemberContext*>& members,
                                        std::vector<CFlatParser::InterfaceMemberContext*>& out) {
        for (auto* m : members)
        {
            if (auto* ifConst = m->ifConstInterfaceMember()) AppendIfConstInterfaceMembers(ifConst, out);
            else out.push_back(m);
        }
    }

void MainListener::AppendIfConstInterfaceMembers(CFlatParser::IfConstInterfaceMemberContext* ctx,
                                       std::vector<CFlatParser::InterfaceMemberContext*>& out) {
        int decision = DecideIfConstCondition(ctx->expression());
        if (decision < 0)
        {
            LogErrorContext(ctx, "'if const' condition must be a compile-time constant expression");
            return;
        }

        bool taken = decision != 0;
        auto ifBlocks = ctx->ifConstInterfaceBlock();
        if (ifBlocks.empty())
            return;

        CFlatParser::IfConstInterfaceBlockContext* branchBlock = nullptr;
        if (taken)
        {
            branchBlock = ifBlocks[0];
        }
        else if (auto* elseIf = ctx->ifConstInterfaceMember())
        {
            // Chained `else if const` - the else arm is another if const, not a brace block.
            AppendIfConstInterfaceMembers(elseIf, out);
            return;
        }
        else if (ifBlocks.size() > 1)
        {
            branchBlock = ifBlocks[1];
        }

        if (branchBlock)
            AppendResolvedInterfaceMembers(branchBlock->interfaceMember(), out);
    }

const std::vector<CFlatParser::InterfaceMemberContext*>& MainListener::ResolveInterfaceMembers(CFlatParser::InterfaceDefinitionContext* ctx) {
        auto it = resolvedInterfaceMembers_.find((const void*)ctx);
        if (it != resolvedInterfaceMembers_.end()) return it->second;

        std::vector<CFlatParser::InterfaceMemberContext*> out;
        AppendResolvedInterfaceMembers(ctx->interfaceMember(), out);
        return resolvedInterfaceMembers_.emplace((const void*)ctx, std::move(out)).first->second;
    }

void MainListener::ParseIfConstDeclaration(CFlatParser::IfConstDeclarationContext* ctx, const std::string& namespaceName) {
        auto expression = ctx->expression();
        int decision = DecideIfConstCondition(expression);
        if (decision < 0)
        {
            LogErrorContext(ctx, "'if const' condition must be a compile-time constant expression");
            return;
        }

        bool taken = decision != 0;
        auto ifBlocks = ctx->ifConstBlock();

        if (ifBlocks.empty())
            return;

        CFlatParser::IfConstBlockContext* branchBlock = nullptr;
        if (taken)
        {
            branchBlock = ifBlocks[0];
        }
        else if (auto* elseIf = ctx->ifConstDeclaration())
        {
            // Chained `else if const` - the else arm is another if const, not a brace block. Taking
            // the else PATH peels exactly the level the scanner appended for it (nested=true).
            RetractIfConstArmGuardedImpls(compilerLLVM, elseIf, /*nested=*/true);
            ParseIfConstDeclaration(elseIf, namespaceName);
            return;
        }
        else if (ifBlocks.size() > 1)
        {
            branchBlock = ifBlocks[1];
        }

        if (!branchBlock)
            return;

        // This arm IS taken, so its classes are live: withdraw the scanner's guarded-implementor
        // note for them before any diagnostic can claim they are absent from this build.
        RetractIfConstArmGuardedImpls(compilerLLVM, branchBlock);

        auto branchDecls = branchBlock->externalDeclaration();

        // Process any imports in the taken branch before forward-scanning it.
        for (auto* extDecl : branchDecls)
        {
            if (auto* imp = extDecl->importDeclaration())
            {
                // Point diagnostics at this import statement (see ProcessImports): a not-found
                // error has no readable file behind it and would otherwise report (0,0).
                Compiler()->SetSourceLocation(imp->getStart()->getLine(), imp->getStart()->getCharPositionInLine());
                if (auto error = ValidateImportLeadingMarker(imp); !error.empty()) {
                    Compiler()->LogError(error);
                    continue;
                }
                if (auto error = ValidateImportSoftKeywords(imp); !error.empty()) {
                    Compiler()->LogError(error);
                    continue;
                }
                // `import framework "X";` / `import framework { ... };` inside an if-const
                // branch. Dispatch before the importGroup routing since it reuses importGroup.
                if (imp->children.size() >= 2 && imp->children[1]->getText() == "framework")
                {
                    if (auto* grp = imp->importGroup())
                        for (auto* lit : grp->StringLiteral())
                            Compiler()->AddFrameworkImport(DequoteStringLiteral(lit->getText()));
                    else if (auto* lit = imp->StringLiteral())
                        Compiler()->AddFrameworkImport(DequoteStringLiteral(lit->getText()));
                    continue;
                }
                // A `framework "X"` clause on a header/package/group import (S3): link the
                // framework in addition to binding the header. Standalone form handled above.
                if (auto* fc = imp->frameworkClause())
                    for (auto* lit : fc->StringLiteral())
                        Compiler()->AddFrameworkImport(DequoteStringLiteral(lit->getText()));
                // `import package-nuget importGroup from "id[/version]";` inside an if-const
                // branch. Dispatch on the keyword BEFORE the plain importGroup routing, since
                // package-nuget now also carries an importGroup; a multi-entry nuget group is
                // ONE package TU, not several plain imports.
                if (imp->children.size() >= 2 && imp->children[1]->getText() == "package-nuget")
                {
                    std::vector<std::string> nugetFiles;
                    if (auto* grp = imp->importGroup())
                        for (auto* lit : grp->StringLiteral())
                            nugetFiles.push_back(DequoteStringLiteral(lit->getText()));
                    std::string packageSpec;
                    if (auto* fc = imp->fromClause())
                        if (fc->StringLiteral())
                        {
                            std::string fr = fc->StringLiteral()->getText();
                            if (fr.size() >= 2) packageSpec = DequoteStringLiteral(fr);
                        }
                    std::vector<std::string> nugetDefines;
                    for (auto* dc : imp->defineClause())
                        if (dc->StringLiteral())
                        {
                            std::string dr = dc->StringLiteral()->getText();
                            if (dr.size() >= 2) nugetDefines.push_back(DequoteStringLiteral(dr));
                        }
                    // Optional `pri "..."` clause: deploy the named .pri as <exe>.pri.
                    std::string nugetPri;
                    if (auto* pc = imp->priClause())
                        if (pc->StringLiteral())
                        {
                            std::string pr = pc->StringLiteral()->getText();
                            if (pr.size() >= 2) nugetPri = DequoteStringLiteral(pr);
                        }
                    Compiler()->CompileNugetImport(nugetFiles, packageSpec, nugetDefines, nugetPri);
                    continue;
                }
                // Grouped import `import { "a", "b" };` inside an if-const branch - header
                // entries share one TU; .cb/.c route individually (see CompileImportGroup).
                if (auto* grp = imp->importGroup())
                {
                    auto lits = grp->StringLiteral();
                    if (lits.size() > 1)
                    {
                        std::vector<std::string> entries;
                        for (auto* lit : lits)
                        {
                            std::string gr = lit->getText();
                            if (gr.size() >= 2) entries.push_back(DequoteStringLiteral(gr));
                        }
                        std::vector<std::string> grpLibs;
                        if (auto* lc = imp->libClause())
                            for (auto* lit : lc->StringLiteral())
                            {
                                std::string lr = lit->getText();
                                if (lr.size() >= 2) grpLibs.push_back(DequoteStringLiteral(lr));
                            }
                        std::vector<std::string> grpDefines;
                        for (auto* dc : imp->defineClause())
                            if (dc->StringLiteral())
                            {
                                std::string dr = dc->StringLiteral()->getText();
                                if (dr.size() >= 2) grpDefines.push_back(DequoteStringLiteral(dr));
                            }
                        Compiler()->CompileImportGroup(Compiler()->currentSourceFilePath_, entries,
                                                       grpLibs, grpDefines, imp->cacheClause() != nullptr);
                        continue;
                    }
                }
                std::string importFilename;
                if (auto* grp = imp->importGroup())
                    importFilename = DequoteStringLiteral(grp->StringLiteral(0)->getText());
                else
                    importFilename = DequoteStringLiteral(imp->StringLiteral()->getText());
                // `import package-vcpkg "header" from "port";` inside an if-const branch.
                if (imp->children.size() >= 2 && imp->children[1]->getText() == "package-vcpkg")
                {
                    std::string portSpec;
                    if (auto* fc = imp->fromClause())
                        if (fc->StringLiteral())
                        {
                            std::string fr = fc->StringLiteral()->getText();
                            if (fr.size() >= 2) portSpec = DequoteStringLiteral(fr);
                        }
                    std::vector<std::string> vcpkgDefines;
                    for (auto* dc : imp->defineClause())
                        if (dc->StringLiteral())
                        {
                            std::string dr = dc->StringLiteral()->getText();
                            if (dr.size() >= 2) vcpkgDefines.push_back(DequoteStringLiteral(dr));
                        }
                    Compiler()->CompileVcpkgImport(Compiler()->RootVcpkgImportPath(Compiler()->currentSourceFilePath_), importFilename, portSpec, vcpkgDefines);
                    continue;
                }
                std::string ns = ImportNamespace(imp);
                std::vector<std::string> explicitLibs;
                if (auto* lc = imp->libClause())
                    for (auto* lit : lc->StringLiteral())
                    {
                        std::string lr = lit->getText();
                        if (lr.size() >= 2) explicitLibs.push_back(DequoteStringLiteral(lr));
                    }
                std::vector<std::string> extraDefines;
                for (auto* dc : imp->defineClause())
                    if (dc->StringLiteral())
                    {
                        std::string dr = dc->StringLiteral()->getText();
                        if (dr.size() >= 2) extraDefines.push_back(DequoteStringLiteral(dr));
                    }
                Compiler()->CompileImportedFile(Compiler()->currentSourceFilePath_, importFilename, ns, "", explicitLibs, extraDefines);
            }
        }

        // First pass: forward ref scan the taken branch to register symbols
        ForwardRefScanner scanner(Compiler());
        scanner.SetTokens(GetTokens());
        for (auto* extDecl : branchDecls)
            scanner.ScanExternalDeclaration(extDecl, namespaceName);

        // Second pass: generate code for the taken branch
        for (auto* extDecl : branchDecls)
            ParseExternalDeclaration(extDecl, namespaceName);
    }

void MainListener::ParseNamespaceDefinition(CFlatParser::NamespaceDefinitionContext* ctx, const std::string& parentNamespace) {
        std::string namespaceName;
        for (auto* id : ctx->Identifier())
            namespaceName += (namespaceName.empty() ? "" : ".") + id->getText();
        if (!parentNamespace.empty())
            namespaceName = parentNamespace + "." + namespaceName;
        auto* compiler = Compiler(ctx);
        // Register every prefix so qualified lookup can walk "os" -> "os.windows".
        std::string prefix;
        for (size_t pos = 0; (pos = namespaceName.find('.', pos)) != std::string::npos; pos++)
            compiler->RegisterNamespace(namespaceName.substr(0, pos));
        compiler->RegisterNamespace(namespaceName);

        if (auto* s = compiler->GetSymbolSink())
        {
            std::string doc = ExtractLeadingDoc(GetTokens(), ctx->getStart());
            s->Register(SymbolKind::Namespace, namespaceName, compiler->GetSourceFilePath(),
                        (int)ctx->getStart()->getLine(), (int)ctx->getStart()->getCharPositionInLine(),
                        "namespace " + namespaceName, {}, doc);
            // Also register under the unqualified name so Lookup("Inner") finds "Outer.Inner".
            size_t dot = namespaceName.rfind('.');
            if (dot != std::string::npos)
                s->Register(SymbolKind::Namespace, namespaceName.substr(dot + 1), compiler->GetSourceFilePath(),
                            (int)ctx->getStart()->getLine(), (int)ctx->getStart()->getCharPositionInLine(),
                            "namespace " + namespaceName, {}, doc);
        }

        // Member declarations (extern signatures, struct fields) may reference sibling
        // types unqualified - make the namespace visible to GetType while parsing them.
        LLVMBackend::NamespaceScope nsScope(compiler, namespaceName);
        for (auto* extDecl : ctx->externalDeclaration())
            ParseExternalDeclaration(extDecl, namespaceName);

        // A bare namespace expectation has no enclosing function to check its scope exit.
        if (!compilerLLVM->expectedError.empty()
            && compilerLLVM->expectedErrorScopeDepth != SIZE_MAX)
        {
            std::cout << std::format("FAIL: expected error '{}' did not occur\n",
                                      compilerLLVM->expectedError);
            compilerLLVM->expectedError.clear();
            if (compilerLLVM->diagnosticSink_)
                throw CompilerAbortException{ "expected error did not occur", compilerLLVM->sourceFileName, 0, 0 };
            compilerLLVM->FailCompilation("expected error did not occur");
        }
    }

void MainListener::enterExternalDeclaration(CFlatParser::ExternalDeclarationContext* ctx) {
        // Skip nodes nested inside a namespace - they are handled by ParseNamespaceDefinition.
        if (dynamic_cast<CFlatParser::NamespaceDefinitionContext*>(ctx->parent))
            return;

        // Skip nodes nested inside an if const block - they are handled by ParseIfConstDeclaration.
        if (dynamic_cast<CFlatParser::IfConstBlockContext*>(ctx->parent))
            return;

        // Skip nodes nested inside an expect_error block - handled by ParseExternalDeclaration's
        // expectErrorDeclaration branch, which processes them manually after setting expectedError.
        if (dynamic_cast<CFlatParser::ExpectErrorDeclarationContext*>(ctx->parent))
            return;

        ParseExternalDeclaration(ctx);
    }

void MainListener::ParseFunctionDefinition(CFlatParser::FunctionDefinitionContext* func, const std::string& structName, const std::string& namespaceName, const std::string& nameOverride, const std::string& bodyNamespace) {
        auto* compiler = Compiler(func);
        if (HasSoftDeclarationSpecifier(func->declarationSpecifiers(), "manifest"))
        {
            LogErrorContext(func, "manifest is only valid on file-scope variable declarations");
            return;
        }
        if (HasSoftDeclarationSpecifier(func->declarationSpecifiers(), "application"))
        {
            LogErrorContext(func, "application is only valid on file-scope variable declarations");
            return;
        }
        // Create Function Definition
        auto name = nameOverride.empty() ? ::getFunctionName(func) : nameOverride;
        if (!namespaceName.empty())
            name = namespaceName + "." + name;
        auto returnType = this->getFunctionReturnType(func);
        CFlatParser::ParameterTypeListContext* paramTypeList = func->parameterTypeList();
        auto params = this->ParseParameterTypeList(paramTypeList);
        size_t line = func->getStart()->getLine();
        bool varargs = paramTypeList && paramTypeList->Ellipsis() != nullptr;

        // If this is a generic function template definition (not an instantiation), store it and return.
        if (nameOverride.empty() && func->genericTypeParameters() != nullptr)
        {
            std::vector<std::string> valueParams;
            std::vector<std::string> valueDefaults;
            auto typeParams = ParseGenericTypeParameters(func->genericTypeParameters(), &valueParams,
                                                         &valueDefaults);
            genericFunctionTemplates[name] = func;
            // Record the declaring namespace, never re-derive it from the key: a call site's bare
            // spelling resolves through it (ResolveGenericFunctionBase) and the body is lowered in it.
            Compiler()->gts.genericTemplateNamespace[name] = namespaceName;
            genericFunctionTypeParams[name] = typeParams;
            compilerLLVM->gts.genericFunctionValueParams[name] = valueParams;
            compilerLLVM->gts.genericFunctionValueDefaults[name] = valueDefaults;
            genericFunctionConstraints[name] = ParseWhereClause(func->whereClause());
            auto entries = func->genericTypeParameters()->typeParameterList()->typeParameterEntry();
            bool hasPack = !entries.empty() && entries.back()->Ellipsis() != nullptr;
            genericFunctionPackIndex[name] = hasPack ? typeParams.size() - 1 : std::string::npos;
            return;
        }

        // A lock(this.read) / lock(this.optimistic) parameter grants a non-exclusive mode over
        // `this`, so the enclosing type must actually carry that capability.
        for (const auto& p : params)
        {
            if (!p.LockThis || p.LockThisMode == LockMode::Exclusive) continue;
            std::string modeText = (p.LockThisMode == LockMode::Shared) ? "read" : "optimistic";
            const char* wantIface = CapabilityForLockMode(modeText);
            if (!structName.empty() && compiler->TypeHasCapability(structName, wantIface)) continue;
            LogErrorContext(func, std::format(
                "lock: type '{}' does not support '{}' mode (missing [Capability({})]).",
                structName.empty() ? "<free function>" : SpellType(*compiler,
                    LLVMBackend::TypeAndValue{ .TypeName = structName }), modeText, wantIface));
        }

        if (!structName.empty())
        {
            LLVMBackend::DeclTypeAndValue typeValue;
            typeValue.TypeName = structName;
            typeValue.VariableName = structName + "__";
            typeValue.Pointer = true;
            params.insert(params.begin(), typeValue);
        }

        // Return-block function: store the inner block for inlining at call sites.
        if (IsReturnBlockFunction(func))
        {
            auto* blockBody = func->compoundStatement()->blockItemList()->blockItem()[0]
                ->statement()->jumpStatement()->compoundStatement();
            compiler->RegisterReturnBlock(name, blockBody, params, returnType);
            return;
        }

        std::vector<LLVMBackend::TypeAndValue> allParams(params.begin(), params.end());

        // Owning-value move-sink inference on the concrete (monomorphized) param list. For a
        // non-generic method ScanFunctionDefinition already set this on the pre-declared symbol,
        // so this is redundant there; for a generic-CLASS-method instantiation it is the ONLY
        // place the sink flag reaches the emitted FunctionSymbol (no forward-scan pre-declaration).
        // Pass the if-const evaluator so a move inside a LIVE if-const branch marks the param a sink.
        ApplyOwningSinkInference(Compiler(), func, allParams, SinkIfConstEvaluator());

        // Record unused-parameter candidates. Restricted to free, non-extern functions:
        // a method's params are constrained by interface/override conformance, and an
        // extern function's signature is part of an external contract - flagging either
        // would be noise. A leading underscore is the opt-out convention (`_unused`).
        if (auto* s = compiler->GetSymbolSink();
            s && structName.empty() && !returnType.external && paramTypeList && paramTypeList->parameterList()
            && !InGenericInstantiation())
        {
            for (auto* paramDecl : paramTypeList->parameterList()->parameterDeclaration())
            {
                auto* declr = paramDecl->declarator();
                auto* dir   = declr ? declr->directDeclarator() : nullptr;
                if (!dir) continue;
                std::string pname = getDirectDeclName(dir);
                if (pname.empty() || pname[0] == '_') continue;
                UnusedCandidate cand;
                cand.name = pname;
                cand.kind = SymbolKind::Variable;
                cand.file = compiler->GetSourceFilePath();
                cand.line = (int)dir->getStart()->getLine();
                cand.col  = (int)dir->getStart()->getCharPositionInLine();
                s->RegisterCandidate(cand);
            }
        }

        // 'auto' return type: only supported on generic instantiations, where param
        // types are concrete and the body can be emitted under an auto-capture pass
        // that records each 'return expr;' value type instead of emitting 'ret'.
        // After body emission the function is rebuilt with the unified return type.
        bool isAutoReturn = (returnType.TypeName == "auto");
        if (isAutoReturn)
        {
            if (nameOverride.empty())
            {
                Compiler(func)->LogErrorMessage("'{}' return type is only supported on generic functions (e.g. {})",
                    { "auto", "auto f<T>(T x)" });
                return;
            }
            // Substitute a placeholder return type so CreateFunctionDefinition can build
            // an LLVM function. The placeholder is replaced after body emission.
            returnType.TypeName = "i64";
            returnType.Pointer  = false;
            returnType.IsMove   = false;
            returnType.IsNullable = false;
        }

        // Struct fields are registered during this walk, not the forward-ref scan, so the
        // queued 'alias' return inference becomes evaluable only now.
        compiler->ResolvePendingAliasReturnInference();

        // Ownership of an unannotated by-value STRUCT return that transitively owns a 'unique'
        // pointer. All paths returning a borrowed parameter means the result is a borrow, so
        // infer 'alias'; paths that disagree give the caller no answer at all, so reject.
        // Copyable returns are untouched - a copy duplicates no ownership.
        if (compiler->TypeOwnsUniquePointer(returnType.TypeName))
        {
            std::string borrowedParam;
            const std::string returnTypeSpelling = SpellType(*compiler, returnType);
            auto returnKind = ClassifyValueStructReturns(Compiler(), func, returnType, allParams, &borrowedParam);
            if (returnKind == ValueStructReturnKind::Mixed)
            {
                LogErrorContext(func, std::format(
                    "function returns borrowed parameter '{}' on one path and an owned value on another, "
                    "so the caller cannot know whether to free the result. Make all returns borrowed "
                    "(declare 'alias {}'), or all returns owned (declare 'move {}' and take ownership in "
                    "with a 'move {} {}' parameter).",
                    borrowedParam, returnTypeSpelling, returnTypeSpelling,
                    returnTypeSpelling, borrowedParam));
                return;
            }
            if (returnKind == ValueStructReturnKind::AllBorrowedParam)
                returnType.IsAlias = true;
        }

        bool returnsOwned = ComputeReturnsOwned(returnType, name, allParams);

        // Pre-scan parameter types, return type, and function body to queue and emit any
        // generic struct instantiations before the function's IR block is opened.
        // At this point no basic block is active, so it is safe to emit new functions.
        // Parameter types must be scanned so that e.g. list<string>* params instantiate
        // list$string before the body references its methods.
        ScanAndQueueGenericTypeUses(func->declarationSpecifiers());
        if (auto* paramTypeList = func->parameterTypeList())
            ScanAndQueueGenericTypeUses(paramTypeList);
        if (auto* blockItemList = func->compoundStatement()->blockItemList())
            ScanAndQueueGenericTypeUses(blockItemList);
        ProcessPendingInstantiations();

        /*
         * Returning a fixed array BY VALUE is unimplemented, not merely mis-lowered: every
         * spelling probed (1-D, 2-D, alias, string-literal body, never-called) emitted `ret ptr`
         * against an `[N x T]` return type and failed module verification. Rejected rather than
         * implemented - a real by-value array return needs an sret-style hidden out-parameter and
         * a copy at every call site, which is a feature, not a defect fix. C forbids it outright.
         */
        if ((returnType.ArraySize != nullptr || returnType.AliasArraySize > 0)
            && !returnType.IsArrayView && !returnType.Pointer)
        {
            // A simd type's TypeName is its ELEMENT ('float'), so spell the vector back out; the
            // dimension carried here is the ARRAY's, not the lane count.
            LLVMBackend::TypeAndValue elementType = returnType;
            elementType.Pointer = false;
            elementType.ElemPointer = false;
            elementType.PointerDepth = 0;
            elementType.IsArrayView = false;
            std::string elem = returnType.IsSimd
                ? std::format("simd<{},{}>", SpellType(*compiler, elementType), returnType.SimdLanes)
                : SpellType(*compiler, elementType);
            LogErrorContext(func, std::format(
                "function '{}' cannot return the fixed array '{}[N]' by value - CFlat has no "
                "by-value array return, and the size was being dropped silently (the function "
                "returned a single '{}'). Return a struct with the array as a field, or take an "
                "out-parameter ('{}* out') and fill it in.",
                name, elem, elem, elem));
        }

        size_t bodyLine = 0;
        if (auto* body = func->compoundStatement())
            bodyLine = body->getStart()->getLine();
        auto fn = compiler->CreateFunctionDefinition(name, returnType, allParams, returnType.external, varargs, line, returnsOwned, !structName.empty(), returnType.CallConv, bodyLine);

        // CreateFunctionDefinition returns the existing function (without setting up
        // a fresh entry block) when a matching definition was already emitted by a
        // transitive import. Detect that here and skip body emission - re-emitting
        // into the live function's blocks would corrupt its IR.
        if (fn->isMaterializable()
            || (!fn->empty() && cflat_llvm::GetTerminatorOrNull(&fn->getEntryBlock()) != nullptr))
            return;

        compiler->InitializeBlock(&fn->front(), false);
        LLVMBackend::AliasScopeGuard functionAliasScope(compiler);
        // Fresh straight-line for this function/lambda body; restore the enclosing walk's flag on
        // exit so a nested lambda's return does not leak into the surrounding expression.
        ReturnFlagGuard functionReturnFlagGuard(&straightLineReturned_);
        straightLineReturned_ = false;

        // Make the enclosing namespace visible to body resolution so an unqualified
        // sibling reference (bare "helper" inside "namespace N") resolves to "N.helper".
        // RAII: a LogError inside the body throws on the batch/LSP paths, and a skipped restore
        // would steer the next file's generic-template resolution.
        LLVMBackend::NamespaceScope nsScope(compiler, bodyNamespace.empty() ? namespaceName : bodyNamespace);

        currentFunctionIsVariadic = varargs;

        if (isAutoReturn)
        {
            // The placeholder mangled name encodes the placeholder return type, which
            // may happen to collide with the eventual post-inference mangled name
            // (e.g. if T=i64, both placeholder and inferred return are i64). Rename
            // the LLVM function to a guaranteed-unique pending name so finalization
            // can freely create the real function under the inferred mangled name.
            fn->setName(fn->getName().str() + "$auto_pending");
            compiler->BeginAutoReturnCapture();
        }

        // Seed the lock-set from the function's RequiredLocks (covers both lock clauses and
        // positional group membership). For this.X entries, also insert the bare form X so
        // self-access checks (which use the guardian bare name) pass inside the method body.
        currentLockSet.clear();
        if (const auto* sym = compiler->GetFunctionSymbol(fn))
        {
            for (const auto& rawLock : sym->RequiredLocks)
            {
                std::string canonical = StripLockModeSuffix(rawLock);
                LockMode heldMode = LockModeFromSuffix(LockTextMode(rawLock));
                currentLockSet[canonical] = heldMode;
                if (canonical.starts_with("this."))
                    currentLockSet[canonical.substr(5)] = heldMode;
            }
        }

        // Record stack depth after createFunctionBlock pushed the function's frame.
        // Used to identify bare-semicolon expect_error that was set inside this function.
        size_t funcDepth = compilerLLVM->stackNamedVariable.size();

        auto blockItemList = func->compoundStatement()->blockItemList();

        bool expectErrorHandled = false;
        if (blockItemList)
        {
            try
            {
                ParseBlockItemList(blockItemList);
            }
            catch (const ExpectedErrorReceived&)
            {
                // Handle only if the expect_error was set at this function's entry depth
                // (bare-semicolon form inside this function body).
                // File-scope scoped-block form sets expectedErrorScopeDepth = SIZE_MAX - re-throw.
                if (!compilerLLVM->expectedError.empty() &&
                    compilerLLVM->expectedErrorScopeDepth == funcDepth)
                {
                    // The rest of the body never ran; defensive no-op while the grammar puts no
                    // classDefinition in a function body (see the file-scope catch for the real case).
                    ForgetIfConstGuardedImpls(compilerLLVM, blockItemList);
                    compilerLLVM->AbortFunctionBlocks(funcDepth - 1);
                    compilerLLVM->RestoreFileScopeExpectedError();
                    compiler->ClearCurrentSubprogram();
                    // The body was aborted mid-way: its CFG is partial, so its null-state log
                    // must be dropped rather than analyzed by the end-of-module sweep.
                    compiler->DiscardNullDerefEvents(fn);
                    // Same reasoning for pending return-dangle checks: a partial CFG cannot
                    // answer the existential use-list question soundly.
                    compiler->DiscardPendingReturnDangleChecks(fn);
                    // Same reasoning for pending null-interface dispatches: the anchor's block
                    // was never finished, so "the last write before the dispatch" is unanswerable.
                    compiler->DiscardPendingNullIfaceDispatch(fn);
                    expectErrorHandled = true;
                }
                else
                {
                    throw;
                }
            }
        }

        if (!expectErrorHandled)
        {
            // An unreachable trailing block (e.g. the dead exit of a `while (true)`
            // loop with no break) does not fall through, so it must not be flagged
            // as a missing return. It is also left without a terminator, so close it
            // with 'unreachable' below for module validity.
            bool blockUnreachable = compiler->IsCurrentBlockUnreachable();
            auto* currentBlock = compilerLLVM->builder->GetInsertBlock();
            if (!blockUnreachable && currentBlock == expectErrorRecoveryBlock_ && currentBlock->empty())
                blockUnreachable = true;
            if (returnType.TypeName != "void" && !compiler->IsBlockTerminated() && !blockUnreachable)
                LogErrorContext(func, std::format("Function '{}' with non-void return type is missing a return statement.",
                    SpellFunctionSymbol(*compiler, name)));

            // if return is void, then this might need a implicit return;
            if (returnType.TypeName == "void")
            {
                compiler->CreateReturnCall(nullptr);
            }

            // Pop the stack
            compiler->CreateBlockBreak(nullptr, true);

            if (!compiler->IsBlockTerminated() && blockUnreachable)
                compiler->builder->CreateUnreachable();

            compiler->ClearCurrentSubprogram();

            // The body's CFG is complete - solve the MAY-null fixpoint here so the error still
            // lands inside an enclosing scoped expect_error rather than at end-of-module.
            compiler->RunDeferredEndOfBodyChecks(fn);

            // Same reasoning, for the deferred interface-return-dangle existential check: the
            // slot's use-list is only complete once the body is fully lowered.

            // Same reasoning again for the definitely-null interface dispatch: the proof reads
            // the dispatch's own block, which is only complete now.

            // Local using aliases are registered while the body is walked. Re-run sink inference
            // now so a cast through an alias declared in this function updates the emitted symbol.
            ApplyOwningSinkInference(Compiler(), func, allParams, SinkIfConstEvaluator());
            auto symIt = compiler->functionTable.find(name);
            if (symIt != compiler->functionTable.end())
            {
                for (auto& sym : symIt->second)
                {
                    if (sym.Function != fn) continue;
                    sym.Parameters = allParams;
                    sym.ReturnType = returnType;
                    sym.ReturnsOwned = returnsOwned;
                    sym.ReturnsAlias = returnType.IsAlias;
                    break;
                }
            }
        }

        if (isAutoReturn)
        {
            auto sites = compiler->EndAutoReturnCapture();
            compiler->FinalizeAutoReturnFunction(name, fn, sites, allParams, varargs, returnsOwned, !structName.empty());
            return;  // auto functions do not participate in default-param overload generation in v1
        }

        GenerateDefaultParamOverloads(name, returnType, params, varargs, line);
    }

std::vector<LLVMBackend::AnnotationValue> MainListener::ParseAnnotationList(CFlatParser::AnnotationListContext* annList) {
        std::vector<LLVMBackend::AnnotationValue> result;
        if (!annList) return result;

        auto* compiler = Compiler(annList);
        for (auto* ann : annList->annotation())
        {
            std::string annName = ann->Identifier()->getText();

            // Validate: annotation must be declared
            auto regIt = compiler->annotationRegistry.find(annName);
            if (regIt == compiler->annotationRegistry.end())
            {
                LogErrorContext(ann, "Unknown annotation '" + annName + "': no annotation declaration found");
                continue;
            }

            auto argValues = AnnotationArgTexts(ann);
            bool hasArg = ann->annotationArgList() != nullptr;
            bool expectsArg = !regIt->second.empty();

            if (hasArg && !expectsArg)
            {
                LogErrorContext(ann, "Annotation '" + annName + "' does not accept arguments");
                continue;
            }
            if (!hasArg && expectsArg)
            {
                LogErrorContext(ann, "Annotation '" + annName + "' requires an argument");
                continue;
            }

            LLVMBackend::AnnotationValue av;
            av.Name = annName;
            av.Value = argValues.empty() ? std::string{} : argValues.front();
            av.Values = std::move(argValues);
            result.push_back(std::move(av));
        }
        return result;
    }

std::vector<LLVMBackend::DeclTypeAndValue> MainListener::ParseDeclarationList(std::vector<CFlatParser::DeclarationContext*> ctx) {
        std::vector<LLVMBackend::DeclTypeAndValue> result;
        auto* compiler = Compiler(ctx.empty() ? nullptr : ctx.front());

        // Parse a fixed-array dimension expression to a compile-time constant.
        // Returns nullopt (and logs) when the expression is not a ConstantInt.
        auto parseConstDim = [&](CFlatParser::AssignmentExpressionContext* expr,
                                 antlr4::ParserRuleContext* errCtx,
                                 const char* msg) -> std::optional<uint64_t>
        {
            auto* sizeVal = ParseAssignmentExpression(expr);
            if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(sizeVal))
                return ci->getZExtValue();
            LogErrorContext(errCtx, msg);
            return std::nullopt;
        };

        if (ctx.size() > 0)
        {
            for (auto decl : ctx)
            {
                auto direct = decl->declarationSpecifiers();
                if (HasSoftDeclarationSpecifier(direct, "manifest"))
                {
                    LogErrorContext(direct, "manifest declarations are only allowed at file scope");
                    continue;
                }
                if (HasSoftDeclarationSpecifier(direct, "application"))
                {
                    LogErrorContext(direct, "application declarations are only allowed at file scope");
                    continue;
                }
                LLVMBackend::DeclTypeAndValue typeAndValue;
                {
                    StructFieldDeclGuard fieldCtx(inStructFieldDecl_);
                    typeAndValue = ParseDeclarationSpecifiers(direct);
                }
                auto annotations = ParseAnnotationList(decl->annotationList());

                auto initDeclList = decl->initDeclaratorList()->initDeclarator();
                for (auto initDecl : initDeclList)
                {
                    auto declarator = initDecl->declarator();
                    auto initializer = initDecl->initializer();

                    auto* directDecl = declarator->directDeclarator();
                    std::string name = getDirectDeclName(directDecl);
                    // Fixed-size array field - two forms, mirroring local declarations:
                    //   C-style:    T buf[N]   - size on the directDeclarator
                    //   Type-first: T[N] buf   - size on the declarationSpecifier (typeAndValue.ArraySize)
                    typeAndValue.ConstArraySize = 0;  // reset per-declarator
                    typeAndValue.ConstInnerDimensions.clear();
                    if (directDecl->assignmentExpression())
                    {
                        // C-style fixed-size array field: char buf[N]
                        if (auto sz = parseConstDim(directDecl->assignmentExpression(), directDecl, "array size must be a compile-time constant"))
                            typeAndValue.ConstArraySize = *sz;
                    }
                    else if (typeAndValue.ArraySize != nullptr)
                    {
                        // Type-first fixed-size array field: u8[N] data (optionally multi-dim u8[N][M]).
                        if (auto sz = parseConstDim(typeAndValue.ArraySize, directDecl, "array size must be a compile-time constant"))
                            typeAndValue.ConstArraySize = *sz;
                        for (auto* dimExpr : typeAndValue.ExtraArrayDims)
                        {
                            if (auto dim = parseConstDim(dimExpr, dimExpr, "array dimension must be a compile-time constant"))
                                typeAndValue.ConstInnerDimensions.push_back(*dim);
                        }
                    }
                    else if (typeAndValue.AliasArraySize > 0)
                    {
                        // Size came from an array alias (using Bytes = u8[8]); shared by every declarator.
                        typeAndValue.ConstArraySize = typeAndValue.AliasArraySize;
                        if (!typeAndValue.AliasInnerDims.empty())
                            typeAndValue.ConstInnerDimensions = typeAndValue.AliasInnerDims;
                    }
                    typeAndValue.VariableName = name;
                    typeAndValue.Initializer = initializer;
                    // Bare-brace field default (`Inner i { x = 1 };`): the list hangs on initDecl,
                    // not on an initializer, so record it or the spelling is silently dropped.
                    typeAndValue.BraceInitializer = initDecl->initializerList();
                    typeAndValue.Annotations = annotations;

                    // Bitfield: `int flags : 3 = 0;` - the constantExpression after ':'
                    // is the declared width in bits. Set IsBitfield + BitWidth here;
                    // the packing pass (LayoutBitfields in LLVMBackend.h) fills in
                    // BitOffset and StorageFieldIndex before CreateStructType.
                    if (auto* widthExpr = initDecl->constantExpression())
                    {
                        llvm::Value* widthVal = ParseConditionalExpression(widthExpr->conditionalExpression());
                        auto* widthCi = llvm::dyn_cast_or_null<llvm::ConstantInt>(widthVal);
                        if (!widthCi)
                        {
                            LogErrorContext(widthExpr, "bitfield width must be a compile-time constant integer");
                        }
                        else
                        {
                            int64_t w = widthCi->getSExtValue();
                            if (w < 0)
                                LogErrorContext(widthExpr, "bitfield width must be non-negative");
                            else
                                typeAndValue.BitWidth = (unsigned)w;
                            typeAndValue.IsBitfield = true;
                        }
                    }

                    bool spelledUnique = HasSoftDeclarationSpecifier(direct, "unique");
                    bool isCoreUniqueField = !typeAndValue.Pointer
                        && (compiler->IsCoreUniqueType(typeAndValue.TypeName)
                            || MangledBase(typeAndValue.TypeName) == "unique");
                    if (typeAndValue.IsUnique || isCoreUniqueField || spelledUnique)
                        ValidateUniqueField(typeAndValue, declarator);
                    else
                        ValidateAllocAlignField(typeAndValue, declarator);

                    result.push_back(typeAndValue);  // Copy
                    if (!inUnionFieldDecl_ && !structScopeStack.empty())
                    {
                        std::string cycleField;
                        if (compiler->HasUniqueDestructionCycle(structScopeStack.back(), result, &cycleField))
                            LogErrorContext(declarator, std::format(
                                "'unique' on field '{}.{}' forms a destruction cycle back to '{}': the synthesized destructor would recurse without bound. Drop 'unique' and write a destructor with an iterative teardown.",
                                structScopeStack.back(), cycleField, structScopeStack.back()));
                    }
                }
            }
        }

        return result;
    }

void MainListener::ValidateUniqueField(const LLVMBackend::DeclTypeAndValue& f, antlr4::ParserRuleContext* ctx) {
        std::string owner = structScopeStack.empty() ? "" : structScopeStack.back() + ".";
        std::string field = "field '" + owner + f.VariableName + "'";
        // Checked ahead of the field-shape rejections below: no union member can ever be 'unique',
        // so a shape complaint would only steer the user to fix the shape and land back here.
        if (inUnionFieldDecl_)
            LogErrorContext(ctx, "'unique' on " + field + ": a union member cannot be 'unique' - the synthesized destructor cannot know which member is active, so it would free whichever bits the union currently holds. Drop 'unique' and free the pointer from code that knows the active member.");
        // The core wrapper enforces its single-pointer shape; old shape checks are builtin-only.
        bool pendingCoreUnique = !f.Pointer && MangledBase(f.TypeName) == "unique"
            && Compiler(ctx)->gts.coreGenericTemplates.count("unique") != 0;
        if ((Compiler(ctx)->IsCoreUniqueType(f.TypeName) || pendingCoreUnique)
            && !f.Pointer)
            return;
        // `unique T* f[N]` is allowed since 2026-07-20: the synthesized destructor walks the array
        // and releases each slot (EmitOwningUniqueArrayCleanup, shared with the array-LOCAL path).
        if (f.IsArrayView)
            LogErrorContext(ctx, "'unique' on " + field + ": array views are not supported - a view does not own its buffer");
        if (f.IsSimd)
            LogErrorContext(ctx, "'unique' on " + field + ": simd is not a pointer type");
        if (f.IsBitfield)
            LogErrorContext(ctx, "'unique' on " + field + ": a bitfield is not a pointer type");
        if (f.IsFunctionPointer)
            LogErrorContext(ctx, "'unique' on " + field + ": a function pointer or closure does not own an allocation");
        // A boxed interface VALUE is a {i8*,i8*} fat pointer, not a single-indirection pointer, but
        // the synthesized destructor releases it through the vtable dtor slot - so allow it.
        bool ifaceValue = f.IsFatInterfaceValue() && !f.ElemPointer;
        if ((!f.Pointer || f.ElemPointer) && !ifaceValue)
            LogErrorContext(ctx, "'unique' on " + field + " requires a single-indirection pointer type such as 'unique Node* n'");
    }

void MainListener::ValidateAllocAlignField(const LLVMBackend::DeclTypeAndValue& f, antlr4::ParserRuleContext* ctx) {
        // Only N > kDefaultNewAlign actually selects the aligned deallocator. A `unique` or array-view
        // field owns its allocation; a non-pointer / double-pointer field routes no scalar delete.
        if (f.AllocAlignValue <= LLVMBackend::kDefaultNewAlign) return;
        if (f.IsUnique || f.IsArrayView) return;
        if (!f.Pointer || f.ElemPointer) return;
        std::string owner = structScopeStack.empty() ? "" : structScopeStack.back() + ".";
        LogErrorContext(ctx, std::format(
            "alignas(0, {}) on pointer field '{}{}' selects aligned deallocation but the field is not "
            "'unique'; the compiler cannot guarantee the block was allocated aligned, so a 'delete' of "
            "this field would free a plain 'new' block through the aligned deallocator and corrupt the "
            "heap. Mark the field 'unique' so the compiler owns the allocation, or remove the "
            "alloc-alignment clause.",
            f.AllocAlignValue, owner, f.VariableName));
    }

// True when a pointer value roots at a stack allocation - the address of a local, of one of its
// fields, or of an element of a local array. Globals, heap results and loads root elsewhere.
bool MainListener::PointsIntoStackFrame(llvm::Value* value)
{
        if (value == nullptr || !value->getType()->isPointerTy()) return false;
        value = value->stripPointerCasts();
        while (auto* gep = llvm::dyn_cast<llvm::GEPOperator>(value))
            value = gep->getPointerOperand()->stripPointerCasts();
        return llvm::isa<llvm::AllocaInst>(value);
    }

bool MainListener::IsProgramLifetimeStorage(
        const LLVMBackend::NamedVariable& nv) const {
        if (nv.IsStaticLocal) return true;
        llvm::Value* storage = nv.Storage;
        for (int depth = 0; storage != nullptr && depth < 64; ++depth)
        {
            storage = storage->stripPointerCasts();
            if (llvm::isa<llvm::GlobalVariable>(storage)) return true;
            auto* gep = llvm::dyn_cast<llvm::GEPOperator>(storage);
            if (gep == nullptr) return false;
            storage = gep->getPointerOperand();
        }
        return false;
    }

void MainListener::RejectLambdaReferenceCaptureEscape(
        bool destinationHasProgramLifetime,
        const LLVMBackend::NamedVariable& source,
        antlr4::ParserRuleContext* ctx) {
        if (!destinationHasProgramLifetime || source.LambdaReferenceCaptureNames.empty()) return;
        std::string captures;
        for (const auto& name : source.LambdaReferenceCaptureNames)
        {
            if (!captures.empty()) captures += ", ";
            captures += "'" + name + "'";
        }
        LogErrorContext(ctx, std::format(
            "cannot store a lambda with a by-reference capture of non-static local {} into "
            "program-lifetime storage; the closure would access a dead stack frame",
            captures));
    }

void MainListener::OpenStaticLocalGuard(StaticLocalGuard& guard, const std::string& varName) {
        auto* compiler = Compiler();
        auto* bb = compiler->builder->GetInsertBlock();
        auto* fn = bb != nullptr ? bb->getParent() : nullptr;
        if (fn == nullptr) return;

        auto* i8Ty = compiler->builder->getInt8Ty();
        // A plain (non-atomic) once flag: `static` locals are single-thread-initialized by design;
        // no __cxa_guard-style machinery is built here.
        guard.Flag = new llvm::GlobalVariable(
            *compiler->module, i8Ty, false, llvm::GlobalValue::InternalLinkage,
            compiler->builder->getInt8(0), fn->getName().str() + ".once." + varName);
        guard.PreBB = bb;
        guard.InitBB = llvm::BasicBlock::Create(*compiler->context, "statinit", fn);
        guard.ContBB = llvm::BasicBlock::Create(*compiler->context, "statdone", fn);
        guard.FlagLoad = compiler->builder->CreateLoad(i8Ty, guard.Flag, "statflag");
        guard.FlagCmp = llvm::cast<llvm::Instruction>(compiler->builder->CreateICmpEQ(
            guard.FlagLoad, compiler->builder->getInt8(0), "statfirst"));
        guard.CondBr = compiler->builder->CreateCondBr(guard.FlagCmp, guard.InitBB, guard.ContBB);
        compiler->builder->SetInsertPoint(guard.InitBB);
        // Set BEFORE the initializer runs, so a call that recurses into this function observes
        // "already initialized" instead of re-entering the initializer.
        guard.FlagStore = compiler->builder->CreateStore(compiler->builder->getInt8(1), guard.Flag);
    }

void MainListener::CloseStaticLocalGuard(StaticLocalGuard& guard, const std::string& varName) {
        auto* compiler = Compiler();
        compiler->ClearStaticLocalRequest();
        if (guard.Flag == nullptr) return;

        llvm::GlobalVariable* storage = nullptr;
        if (!compiler->stackNamedVariable.empty())
        {
            auto& frame = compiler->stackNamedVariable.back().namedVariable;
            auto it = frame.find(varName);
            if (it != frame.end() && it->second.IsStaticLocal)
                storage = llvm::dyn_cast_or_null<llvm::GlobalVariable>(it->second.Storage);
        }

        // Constant fast path: the whole initializer folded to a single store of a Constant (or to
        // nothing at all), so it becomes the global's own initializer and the guard is unwound.
        bool fold = false;
        llvm::StoreInst* valueStore = nullptr;
        if (storage != nullptr && compiler->builder->GetInsertBlock() == guard.InitBB
            && cflat_llvm::GetTerminatorOrNull(guard.InitBB) == nullptr)
        {
            if (guard.InitBB->size() == 1)
            {
                fold = true;
            }
            else if (guard.InitBB->size() == 2)
            {
                auto* st = llvm::dyn_cast<llvm::StoreInst>(&guard.InitBB->back());
                auto* c = st != nullptr ? llvm::dyn_cast<llvm::Constant>(st->getValueOperand()) : nullptr;
                if (st != nullptr && c != nullptr && st->getPointerOperand() == storage
                    && c->getType() == storage->getValueType())
                {
                    valueStore = st;
                    fold = true;
                }
            }
        }

        if (fold)
        {
            if (valueStore != nullptr)
            {
                storage->setInitializer(llvm::cast<llvm::Constant>(valueStore->getValueOperand()));
                valueStore->eraseFromParent();
            }
            guard.FlagStore->eraseFromParent();
            guard.CondBr->eraseFromParent();
            guard.InitBB->eraseFromParent();
            guard.ContBB->eraseFromParent();
            guard.FlagCmp->eraseFromParent();
            guard.FlagLoad->eraseFromParent();
            guard.Flag->eraseFromParent();
            compiler->builder->SetInsertPoint(guard.PreBB);
        }
        else
        {
            if (compiler->IsInsertBlockLive())
                compiler->builder->CreateBr(guard.ContBB);
            compiler->builder->SetInsertPoint(guard.ContBB);
        }
        guard = StaticLocalGuard{};
    }

MainListener::StaticLocalGuardScope::~StaticLocalGuardScope() {
        // Closed even while unwinding from a LogError throw: an expect_error compile keeps going
        // and verifies the module, so an unclosed guard would strand a terminator-less block.
        if (Owner == nullptr) return;
        try { Owner->CloseStaticLocalGuard(Guard, Name); } catch (...) { }
    }

std::vector<std::pair<std::string, llvm::AllocaInst*>> MainListener::ParseForDeclaration(CFlatParser::ForDeclarationContext* ctx) {
        auto declSpec = ctx->declarationSpecifiers();
        auto initDecl = ctx->initDeclaratorList();
        return ParseDeclaration(declSpec, initDecl);
    }

std::vector<std::pair<std::string, llvm::AllocaInst*>> MainListener::ParseDeclaration(CFlatParser::DeclarationContext* ctx, const std::string& namespaceName) {
        // Handle enum declarations which use the enumSpecifier alternative in the grammar
        if (auto enumSpec = ctx->enumSpecifier())
        {
            ParseEnumSpecifier(enumSpec, namespaceName);
            return {};
        }

        auto declSpec = ctx->declarationSpecifiers();
        auto initDecl = ctx->initDeclaratorList();
        return ParseDeclaration(declSpec, initDecl, namespaceName);
    }

void MainListener::ParseEnumSpecifier(CFlatParser::EnumSpecifierContext* ctx, const std::string& namespaceName) {
        if (!ctx) return;
        // Codegen-pass folder for an initializer the pre-pass could not evaluate statically.
        std::function<bool(CFlatParser::ConditionalExpressionContext*, llvm::APInt&)> dynamicEval =
            [this](CFlatParser::ConditionalExpressionContext* cond, llvm::APInt& out) -> bool {
                llvm::Value* v = ParseConditionalExpression(cond);
                auto* ci = llvm::dyn_cast_or_null<llvm::ConstantInt>(v);
                if (ci == nullptr) return false;
                // Natural width, no extension: the caller knows the backing signedness.
                out = ci->getValue();
                return true;
            };
        auto* compiler = Compiler(ctx);
        compiler->RegisterEnumSpecifier(ctx, namespaceName, &constFoldableGlobals_, dynamicEval);

        // An enum is a type and its members are named constants, so index both: --symbol,
        // hover and completion see `Dir` and `Dir.Back` the way they see a struct and its fields.
        auto* sink = compiler->GetSymbolSink();
        auto* id = ctx->Identifier();
        if (sink == nullptr || id == nullptr) return;
        std::string scoped = compiler->ResolveEnumTypeName(id->getText());
        if (scoped.empty()) scoped = id->getText();
        std::string backing = compiler->GetEnumBackingType(scoped);
        if (backing.empty()) backing = "int";
        std::vector<std::string> memberNames;
        auto* list = ctx->enumeratorList();
        if (list != nullptr)
            for (auto* e : list->enumerator())
                memberNames.push_back(e->enumerationConstant()->getText());
        const std::string file = compiler->GetSourceFilePath();
        sink->Register(SymbolKind::TypeAlias, scoped, file,
                       (int)ctx->getStart()->getLine(), (int)ctx->getStart()->getCharPositionInLine(),
                       "enum " + scoped + " : " + backing, memberNames,
                       ExtractLeadingDoc(GetTokens(), ctx->getStart()));
        if (list != nullptr)
            for (auto* e : list->enumerator())
            {
                std::string member = e->enumerationConstant()->getText();
                sink->Register(SymbolKind::Variable, scoped + "." + member, file,
                               (int)e->getStart()->getLine(), (int)e->getStart()->getCharPositionInLine(),
                               backing + " " + scoped + "." + member);
            }
    }

CFlatParser::MoveExpressionContext* MainListener::TopLevelMoveExpression(antlr4::tree::ParseTree* node) {
        while (node != nullptr)
        {
            if (auto* mv = dynamic_cast<CFlatParser::MoveExpressionContext*>(node))
                return mv;
            if (node->children.size() != 1) return nullptr;
            node = node->children[0];
        }
        return nullptr;
    }

bool MainListener::IsBareIdentifierText(const std::string& text) {
        if (text.empty() || (!std::isalpha((unsigned char)text[0]) && text[0] != '_'))
            return false;
        for (char c : text)
            if (!std::isalnum((unsigned char)c) && c != '_') return false;
        return true;
    }

void MainListener::ReleaseOwningLocalNow(antlr4::ParserRuleContext* ctx, LLVMBackend::NamedVariable* nv,
                               const std::string& name) {
        auto* compiler = Compiler(ctx);
        compiler->SetCurrentDebugLocation(ctx->getStart()->getLine());
        if (nv->IsMoved || nv->MovedIntoInterface)
        {
            LogErrorContext(ctx, std::format(
                "cannot discard '{}': it was already moved", name));
            return;
        }
        if (!compiler->OwnsDroppableResource(*nv))
            return;
        compiler->DropValue(*nv);
        if (nv->Storage != nullptr && nv->BaseType != nullptr)
            compiler->builder->CreateStore(llvm::Constant::getNullValue(nv->BaseType), nv->Storage);
        compiler->StoreRawArrayLength(*nv, nullptr);
        nv->IsOwning = false;
        nv->RefCountStorage = nullptr;
        compiler->MarkVariableMoved(name);
        if (compiler->IsCoreUniqueType(nv->TypeAndValue.TypeName))
            compiler->MarkVariableExplicitlyMovedNull(name);
    }

void MainListener::ReleaseOwningGlobalNow(antlr4::ParserRuleContext* ctx, const std::string& name) {
        auto* compiler = Compiler(ctx);
        compiler->SetCurrentDebugLocation(ctx->getStart()->getLine());
        auto nv = compiler->GetGlobalVariableNV(name);
        if (nv.Storage == nullptr)
            return;
        if (!compiler->OwnsDroppableResource(nv))
            return;
        compiler->DropValue(nv);
        if (nv.BaseType != nullptr)
            compiler->builder->CreateStore(llvm::Constant::getNullValue(nv.BaseType), nv.Storage);
    }

void MainListener::ApplyMovedSlotOwnership(LLVMBackend::NamedVariable& nv,
                                        const LLVMBackend::TypeAndValue& elemOwnershipType) {
        bool unique = elemOwnershipType.IsUnique
            || MangledBase(elemOwnershipType.TypeName) == "unique";
        bool thinPtr = elemOwnershipType.Pointer && !elemOwnershipType.ElemPointer
            && !elemOwnershipType.IsInterface;
        bool uniqueIface = elemOwnershipType.IsFatInterfaceValue();
        if (unique && (thinPtr || uniqueIface))
        {
            nv.IsOwning = true;
        }
        else if ((thinPtr || uniqueIface) && !unique)
        {
            nv.IsOwning = false;
            nv.IsNewAllocated = false;
        }
    }


bool MainListener::ValidateApplicationIcons(antlr4::ParserRuleContext* ctx,
    cflat::appres::AppInfoData& out)
{
    using namespace cflat::appres;
    if (out.Icons.empty())
    {
        out.IconKind = AppIconKind::None;
        return true;
    }

    // A single-entry list may be a whole .ico / .icns container; every other shape is a PNG set.
    if (out.Icons.size() == 1)
    {
        if (LooksLikeIco(out.Icons[0].Bytes))
        {
            // Split now so the .res writer emits one RT_ICON per contained image.
            std::vector<AppIconImage> split;
            std::string splitError;
            if (!SplitIco(out.Icons[0].Bytes, split, splitError))
            {
                LogErrorContext(ctx, std::format("application icon: {}", splitError));
                return false;
            }
            out.Icons = std::move(split);
            out.IconKind = AppIconKind::Ico;
            return true;
        }
        if (LooksLikeIcns(out.Icons[0].Bytes)) { out.IconKind = AppIconKind::Icns; return true; }
    }

    static const std::set<uint32_t> allowed = { 16, 32, 48, 64, 128, 256, 512, 1024 };
    std::set<uint32_t> seen;
    for (size_t i = 0; i < out.Icons.size(); ++i)
    {
        uint32_t width = 0;
        uint32_t height = 0;
        if (!ProbePng(out.Icons[i].Bytes, width, height))
        {
            LogErrorContext(ctx, std::format(
                "application icon entry {} is not a PNG; an icon list holds PNG images, or a "
                "single .ico / .icns file", i));
            return false;
        }
        if (width != height)
        {
            LogErrorContext(ctx, std::format(
                "application icon entry {} is {}x{}; icon images must be square", i, width, height));
            return false;
        }
        if (allowed.count(width) == 0)
        {
            LogErrorContext(ctx, std::format(
                "application icon entry {} is {}x{}; the size must be one of "
                "16, 32, 48, 64, 128, 256, 512, 1024", i, width, height));
            return false;
        }
        if (!seen.insert(width).second)
        {
            LogErrorContext(ctx, std::format(
                "application icon entry {} repeats the {}x{} size; every icon size must be distinct",
                i, width, height));
            return false;
        }
        out.Icons[i].Width = width;
        out.Icons[i].Height = height;
    }
    out.IconKind = AppIconKind::PngSet;
    return true;
}

/*
 * The `application` literal walk. Deliberately its own walker rather than a third mode of
 * FoldConstLiteral: the leaves here are typed data (byte spans from embed, a parsed version
 * quad, an icon list) instead of the JSON/XML text those two modes produce.
 */
bool MainListener::FoldApplicationLiteral(antlr4::ParserRuleContext* ctx, const std::string& typeName,
    CFlatParser::InitializerListContext* init, cflat::appres::AppInfoData& out)
{
    auto* compiler = Compiler(ctx);
    if (typeName != "AppInfo")
    {
        LogErrorContext(ctx, std::format(
            "an application declaration must be typed 'AppInfo' from core/application.cb, not '{}'",
            typeName));
        return false;
    }
    if (init == nullptr)
    {
        LogErrorContext(ctx, "an application declaration requires a brace initializer");
        return false;
    }

    // One named field's single expression; nullptr when the field is a brace list instead.
    auto soleExpression = [&](CFlatParser::FieldInitContext* field)
        -> CFlatParser::AssignmentExpressionContext* {
        if (field->Colon() != nullptr || field->assignmentExpression().size() != 1) return nullptr;
        return field->assignmentExpression(0);
    };
    auto stringField = [&](CFlatParser::FieldInitContext* field, const std::string& name,
                           std::string& value) {
        auto* expression = soleExpression(field);
        std::string raw = expression == nullptr ? std::string() : expression->getText();
        if (raw.size() < 2 || raw.front() != '"' || raw.back() != '"')
        {
            LogErrorContext(field, std::format(
                "application field '{}' must be a string literal", name));
            return false;
        }
        value = ProcessRawText(raw);
        // The VS_VERSIONINFO block length is a u16, so an over-long field would silently truncate.
        static const size_t kMaxAppStringBytes = 32000;
        if (value.size() > kMaxAppStringBytes)
        {
            LogErrorContext(field, std::format(
                "application field '{}' is {} bytes; the limit is {} bytes", name, value.size(),
                kMaxAppStringBytes));
            return false;
        }
        return true;
    };
    auto boolField = [&](CFlatParser::FieldInitContext* field, const std::string& name, bool& value) {
        auto* expression = soleExpression(field);
        std::string raw = expression == nullptr ? std::string() : expression->getText();
        if (raw != "true" && raw != "false")
        {
            LogErrorContext(field, std::format(
                "application field '{}' must be 'true' or 'false'", name));
            return false;
        }
        value = raw == "true";
        return true;
    };

    bool sawName = false;
    for (auto* field : init->fieldInit())
    {
        if (field->Identifier() == nullptr)
        {
            LogErrorContext(field, "an application initializer requires named fields");
            return false;
        }
        const std::string name = field->Identifier()->getText();
        if (name == "name")
        {
            if (!stringField(field, name, out.Name)) return false;
            sawName = !out.Name.empty();
        }
        else if (name == "identifier") { if (!stringField(field, name, out.Identifier)) return false; }
        else if (name == "description") { if (!stringField(field, name, out.Description)) return false; }
        else if (name == "company") { if (!stringField(field, name, out.Company)) return false; }
        else if (name == "copyright") { if (!stringField(field, name, out.Copyright)) return false; }
        else if (name == "language")
        {
            auto* expression = soleExpression(field);
            std::string raw = expression == nullptr ? std::string() : expression->getText();
            long parsed = 0;
            size_t consumed = 0;
            try { parsed = std::stol(raw, &consumed); } catch (...) { consumed = 0; }
            if (consumed != raw.size() || parsed < 0 || parsed > 0xFFFF)
            {
                LogErrorContext(field,
                    "application field 'language' must be a decimal LanguageId between 0 and 65535");
                return false;
            }
            out.Language = static_cast<int>(parsed);
        }
        else if (name == "type")
        {
            auto* expression = soleExpression(field);
            std::string raw = expression == nullptr ? std::string() : expression->getText();
            if (raw == "FileType.application") out.Type = cflat::appres::AppFileType::Application;
            else if (raw == "FileType.dll") out.Type = cflat::appres::AppFileType::Dll;
            else if (raw == "FileType.driver") out.Type = cflat::appres::AppFileType::Driver;
            else
            {
                LogErrorContext(field, std::format(
                    "application field 'type' must name a FileType member "
                    "(FileType.application, FileType.dll, FileType.driver), got '{}'", raw));
                return false;
            }
        }
        else if (name == "version")
        {
            auto* nested = field->initializerList();
            if (nested == nullptr)
            {
                LogErrorContext(field, "application field 'version' must be a brace initializer");
                return false;
            }
            for (auto* versionField : nested->fieldInit())
            {
                if (versionField->Identifier() == nullptr)
                {
                    LogErrorContext(versionField, "an application initializer requires named fields");
                    return false;
                }
                const std::string versionName = versionField->Identifier()->getText();
                if (versionName == "file")
                { if (!stringField(versionField, "version.file", out.Version.File)) return false; }
                else if (versionName == "product")
                { if (!stringField(versionField, "version.product", out.Version.Product)) return false; }
                else if (versionName == "prerelease")
                { if (!boolField(versionField, "version.prerelease", out.Version.Prerelease)) return false; }
                else if (versionName == "debug")
                { if (!boolField(versionField, "version.debug", out.Version.Debug)) return false; }
                else
                {
                    LogErrorContext(versionField, std::format(
                        "no field named '{}' in AppVersion", versionName));
                    return false;
                }
            }
        }
        else if (name == "icon")
        {
            auto* nested = field->initializerList();
            if (nested == nullptr)
            {
                LogErrorContext(field, "application field 'icon' must be a brace list of AppIcon entries");
                return false;
            }
            size_t index = 0;
            for (auto* entry : nested->fieldInit())
            {
                auto* entryInit = entry->initializerList();
                if (entry->Identifier() != nullptr || entryInit == nullptr)
                {
                    LogErrorContext(entry, std::format(
                        "application icon entry {} must be a brace initializer "
                        "of the form '{{ image = embed(\"icon.png\") }}'", index));
                    return false;
                }
                cflat::appres::AppIconImage image;
                bool sawImage = false;
                for (auto* iconField : entryInit->fieldInit())
                {
                    if (iconField->Identifier() == nullptr
                        || iconField->Identifier()->getText() != "image")
                    {
                        LogErrorContext(iconField, std::format(
                            "application icon entry {} accepts only the field 'image'", index));
                        return false;
                    }
                    auto* expression = soleExpression(iconField);
                    std::string raw = expression == nullptr ? std::string() : expression->getText();
                    if (!raw.starts_with("embed(") || !raw.ends_with(")"))
                    {
                        LogErrorContext(iconField, std::format(
                            "application icon entry {} must be 'image = embed(\"icon.png\")'", index));
                        return false;
                    }
                    std::string pathLiteral = raw.substr(6, raw.size() - 7);
                    if (pathLiteral.size() < 2 || pathLiteral.front() != '"'
                        || pathLiteral.back() != '"')
                    {
                        LogErrorContext(iconField, std::format(
                            "application icon entry {} must embed a string literal path", index));
                        return false;
                    }
                    std::string literalPath = ProcessRawText(pathLiteral, false);
                    std::string resolvedPath;
                    std::string embedError;
                    auto bytes = compiler->LoadEmbedFile(literalPath,
                        compiler->GetSourceFilePath(), resolvedPath, embedError);
                    if (!bytes.has_value())
                    {
                        LogErrorContext(iconField, std::format(
                            "embed(\"{}\") cannot be read: {}", literalPath, embedError));
                        return false;
                    }
                    compiler->RecordEmbedDependency(resolvedPath);
                    image.Bytes = std::move(*bytes);
                    sawImage = true;
                }
                if (!sawImage)
                {
                    LogErrorContext(entry, std::format(
                        "application icon entry {} has no 'image'", index));
                    return false;
                }
                out.Icons.push_back(std::move(image));
                ++index;
            }
        }
        else
        {
            LogErrorContext(field, std::format("no field named '{}' in AppInfo", name));
            return false;
        }
    }

    if (!sawName)
    {
        LogErrorContext(ctx, "an application declaration requires a non-empty 'name'");
        return false;
    }

    uint16_t quad[4] = { 0, 0, 0, 0 };
    std::string versionError;
    if (!out.Version.File.empty()
        && !cflat::appres::ParseVersionQuad(out.Version.File, quad, versionError))
    {
        LogErrorContext(ctx, std::format("application version.file '{}' is not valid: {}",
            out.Version.File, versionError));
        return false;
    }
    if (!out.Version.Product.empty()
        && !cflat::appres::ParseVersionQuad(out.Version.Product, quad, versionError))
    {
        LogErrorContext(ctx, std::format("application version.product '{}' is not valid: {}",
            out.Version.Product, versionError));
        return false;
    }

    if (!ValidateApplicationIcons(ctx, out)) return false;
    return true;
}

std::vector<std::pair<std::string, llvm::AllocaInst*>> MainListener::ParseDeclaration(CFlatParser::DeclarationSpecifiersContext* declSpec, CFlatParser::InitDeclaratorListContext* initDecl, const std::string& namespaceName) {
        auto* compiler = Compiler(declSpec);
        std::vector<std::pair<std::string, llvm::AllocaInst*>> allocList;

        size_t line = declSpec->getStart()->getLine();
        const bool isManifest = HasSoftDeclarationSpecifier(declSpec, "manifest");
        const bool isApplication = HasSoftDeclarationSpecifier(declSpec, "application");
        const bool spelledBuiltinUnique = HasSoftDeclarationSpecifier(declSpec, "unique");
        auto typeAndValue = [&]() {
            LocalDeclGuard localDecl(inLocalDecl_,
                !global_scope && IsFunctionBodyDeclaration(declSpec));
            return ParseDeclarationSpecifiers(declSpec);
        }();
        // A generic parameter can itself be bound to a core unique wrapper. It is already a
        // concrete value type, but its ordinary copy-shaped helper bodies must not be mistaken
        // for a direct `unique` declaration diagnostic.
        const bool genericCoreUniqueTypeArg = !spelledBuiltinUnique
            && activeTypeSubstitutions.count(declSpec->getText()) != 0
            && !typeAndValue.Pointer
            && compiler->IsCoreUniqueType(typeAndValue.TypeName);
        if (isManifest)
        {
            if (!global_scope)
            {
                LogErrorContext(declSpec, "manifest declarations are only allowed at file scope");
                return allocList;
            }
            for (auto* manifestDecl : initDecl->initDeclarator())
            {
                auto* initializer = manifestDecl->initializer();
                auto* initList = manifestDecl->initializerList();
                if (initList == nullptr && initializer != nullptr)
                    initList = initializer->initializerList();
                if (manifestDecl->declarator()->parameterTypeList() != nullptr || initList == nullptr)
                {
                    LogErrorContext(manifestDecl,
                        "manifest initializer must be a compile-time literal");
                    continue;
                }
                std::vector<LLVMBackend::ManifestFragment::Leaf> manifestLeaves;
                auto folded = FoldConstLiteral(manifestDecl, "xml_const",
                    typeAndValue.TypeName, initList, &manifestLeaves);
                if (folded)
                    compiler->RecordManifestFragment(compiler->GetSourceFilePath(),
                        manifestDecl->getStart()->getLine(), *folded, std::move(manifestLeaves));
            }
            return allocList;
        }
        if (isApplication)
        {
            if (!global_scope)
            {
                LogErrorContext(declSpec, "application declarations are only allowed at file scope");
                return allocList;
            }
            for (auto* appDecl : initDecl->initDeclarator())
            {
                auto* initializer = appDecl->initializer();
                auto* initList = appDecl->initializerList();
                if (initList == nullptr && initializer != nullptr)
                    initList = initializer->initializerList();
                if (appDecl->declarator()->parameterTypeList() != nullptr || initList == nullptr)
                {
                    LogErrorContext(appDecl,
                        "application initializer must be a compile-time literal");
                    continue;
                }
                // The one-declaration rule is checked BEFORE the walk so a second declaration
                // names both locations rather than failing on whatever it embeds.
                if (compiler->GetApplicationInfo().has_value())
                {
                    LogErrorContext(appDecl, std::format(
                        "a program may declare only one application; already declared at {}:{}",
                        compiler->GetApplicationSourceFile(), compiler->GetApplicationLine()));
                    continue;
                }
                cflat::appres::AppInfoData info;
                if (!FoldApplicationLiteral(appDecl, typeAndValue.TypeName, initList, info))
                    continue;
                compiler->RecordApplicationInfo(compiler->GetSourceFilePath(),
                    appDecl->getStart()->getLine(), std::move(info));
            }
            return allocList;
        }

        // Queue any pending generic instantiation for this declaration's type.
        // Actual instantiation happens later in ProcessPendingInstantiations() at top-level scope.
        QueueInstantiateGenericType(declSpec);
        // The desugared local calls unique<T>'s constructor immediately, so its deferred body and
        // overload set must be available before this declaration evaluates its initializer.
        if (compiler->IsCoreUniqueType(typeAndValue.TypeName))
        {
            auto savedState = compiler->SaveBuilderState();
            auto savedStack = std::move(compiler->stackNamedVariable);
            auto savedExpectedError = compiler->expectedError;
            auto savedExpectedErrorScopeDepth = compiler->expectedErrorScopeDepth;
            compiler->stackNamedVariable.clear();
            compiler->expectedError.clear();
            compiler->expectedErrorScopeDepth = SIZE_MAX;
            try
            {
                ProcessPendingInstantiations();
            }
            catch (...)
            {
                compiler->stackNamedVariable = std::move(savedStack);
                compiler->expectedError = std::move(savedExpectedError);
                compiler->expectedErrorScopeDepth = savedExpectedErrorScopeDepth;
                compiler->RestoreBuilderState(savedState);
                throw;
            }
            compiler->stackNamedVariable = std::move(savedStack);
            compiler->expectedError = std::move(savedExpectedError);
            compiler->expectedErrorScopeDepth = savedExpectedErrorScopeDepth;
            compiler->RestoreBuilderState(savedState);
        }

        auto initDeclarVec = initDecl->initDeclarator();

        if (typeAndValue.TypeName.empty() && !typeAndValue.IsFunctionPointer)
        {
            LogErrorContext(declSpec, "Declaration has an empty type name.");
            return allocList;
        }

        llvm::Value* arraySize = nullptr;
        if (typeAndValue.ArraySize)
        {
            // A global array dimension can reference a `const` int or be a const-expr
            // (int[BOARD_W*BOARD_H]); evaluating it the local way would emit a load into
            // the program-init block and leave the size unfolded. EvalGlobalArrayDim folds
            // it without leaking IR. Locals keep the live value (supports VLA-style sizing).
            if (global_scope)
                arraySize = EvalGlobalArrayDim(typeAndValue.ArraySize);
            else
                arraySize = ParseAssignmentExpression(typeAndValue.ArraySize);
        }

        // Evaluate inner dimensions for multi-dim arrays: T[N][M] -> ConstInnerDimensions = {M}
        typeAndValue.ConstInnerDimensions.clear();
        for (auto* dimExpr : typeAndValue.ExtraArrayDims)
        {
            auto* dimVal = global_scope ? EvalGlobalArrayDim(dimExpr)
                                        : ParseAssignmentExpression(dimExpr);
            if (auto* ci = llvm::dyn_cast_or_null<llvm::ConstantInt>(dimVal))
                typeAndValue.ConstInnerDimensions.push_back(ci->getZExtValue());
            else
                LogErrorContext(dimExpr, "array dimension must be a compile-time constant");
        }
        // Inner dims from an array alias (using Mat = float[2][3]) when the declarator carries
        // no brackets of its own. The sizes were folded at registration (see ParseDeclarationSpecifiers).
        if (typeAndValue.ConstInnerDimensions.empty() && !typeAndValue.AliasInnerDims.empty())
            typeAndValue.ConstInnerDimensions = typeAndValue.AliasInnerDims;
        const auto parsedTypeAndValue = typeAndValue;

        for (auto initDecl : initDeclarVec)
        {
            typeAndValue = parsedTypeAndValue;
            /*
            declarator
            : directDeclarator
            | directDeclarator '[' typeQualifierList? assignmentExpression? ']'
            | directDeclarator '[' 'static' typeQualifierList? assignmentExpression ']'
            | directDeclarator '[' typeQualifierList 'static' assignmentExpression ']'
            | directDeclarator '[' typeQualifierList? '*' ']'
            | directDeclarator '(' parameterTypeList ')'
            | directDeclarator '(' identifierList? ')'
            ;
            */
            auto declarator = initDecl->declarator();
            auto direct = declarator->directDeclarator();
            std::string declaratorName = direct != nullptr ? getDirectDeclName(direct) : std::string();
            // unique<T> owns exactly ONE object - it has no element count to free an array with.
            // Catch the direct `new T[n]` initializer here, before the wrapper's constructor
            // overload resolution reports the mismatch in wrapper terms.
            if (!global_scope && compiler->IsCoreUniqueType(typeAndValue.TypeName)
                && !typeAndValue.Pointer)
            {
                auto* initAssign = initDecl->initializer() != nullptr
                    ? initDecl->initializer()->assignmentExpression() : nullptr;
                if (RawNewArrayValueOf(initAssign) != nullptr)
                    LogErrorContext(initAssign, std::format(
                        "cannot store a heap array in unique local '{}': unique<T> does not own "
                        "arrays - use 'array<T>'", declaratorName));
            }
            auto paramTypeList = declarator->parameterTypeList();
            // A declarator with parens but no paramTypeList is a zero-parameter function:
            // matches grammar alternative `directDeclarator '(' identifierList? ')'`
            bool hasParens = declarator->children.size() > 1;

            if (paramTypeList != nullptr || hasParens)
            {
                // If there is parameter list (or empty parens), then it is a function.
                auto declParams = paramTypeList ? ParseParameterTypeList(paramTypeList) : std::vector<LLVMBackend::DeclTypeAndValue>{};
                std::vector<LLVMBackend::TypeAndValue> allParams(declParams.begin(), declParams.end());

                bool ellipsis = paramTypeList && paramTypeList->Ellipsis() != nullptr;
                // A declaration at namespace scope registers under its qualified name
                // (os.windows.Sleep) so lookup is namespaced, but an extern keeps its
                // bare linkage symbol (Sleep) - the import library only knows that name.
                std::string declName = direct->getText();
                std::string linkName;
                if (!namespaceName.empty())
                {
                    if (typeAndValue.external)
                        linkName = declName;
                    declName = namespaceName + "." + declName;
                }
                /*
                 * Same reject as the function DEFINITION path (see ParseFunctionDefinition): a
                 * prototype has no body, so it never reached that arm and the '[N]' was dropped
                 * silently - the symbol bound as a single element while the callee returns N.
                 * This is the only registration site for prototypes; ForwardRefScanner does not
                 * scan file-scope `declaration` nodes.
                 */
                if ((typeAndValue.ArraySize != nullptr || typeAndValue.AliasArraySize > 0)
                    && !typeAndValue.IsArrayView && !typeAndValue.Pointer)
                {
                    // A simd type's TypeName is its ELEMENT ('float'), so spell the vector back out;
                    // the dimension carried here is the ARRAY's, not the lane count.
                    LLVMBackend::TypeAndValue elementType = typeAndValue;
                    elementType.Pointer = false;
                    elementType.ElemPointer = false;
                    elementType.PointerDepth = 0;
                    elementType.IsArrayView = false;
                    std::string elem = typeAndValue.IsSimd
                        ? std::format("simd<{},{}>", SpellType(*compiler, elementType), typeAndValue.SimdLanes)
                        : SpellType(*compiler, elementType);
                    LogErrorContext(direct, std::format(
                        "function '{}' cannot return the fixed array '{}[N]' by value - CFlat has no "
                        "by-value array return, and the size was being dropped silently (the function "
                        "returned a single '{}'). Return a struct with the array as a field, or take an "
                        "out-parameter ('{}* out') and fill it in.",
                        declName, elem, elem, elem));
                }

                compiler->CreateFunctionDeclaration(declName, typeAndValue, allParams, typeAndValue.external, ellipsis, false, false, typeAndValue.CallConv, linkName);

                // Register the declaration in the symbol index so extern-bound C
                // runtime functions (atoi, strcmp, memcpy, ...) are discoverable
                // via --symbol, hover, and completion like any defined function.
                // The signature format mirrors the definition-site registration in
                // ForwardRefScanner, so a declaration followed by a definition
                // dedupes instead of listing as a phantom overload.
                if (auto* s = compiler->GetSymbolSink())
                {
                    std::string sig = SpellType(*compiler, typeAndValue) + " " + declName + "(";
                    bool first = true;
                    for (const auto& p : declParams)
                    {
                        if (!first) sig += ", ";
                        first = false;
                        sig += SpellType(*compiler, p);
                        if (!p.VariableName.empty()) sig += " " + p.VariableName;
                    }
                    if (ellipsis) sig += first ? "..." : ", ...";
                    sig += ")";
                    s->Register(SymbolKind::Function, declName,
                                compiler->GetSourceFilePath(),
                                (int)direct->getStart()->getLine(),
                                (int)direct->getStart()->getCharPositionInLine(), sig);
                }

                // Declare overloads for each suffix of omitted default parameters
                int firstDefault = -1;
                for (int i = 0; i < (int)declParams.size(); i++)
                {
                    if (declParams[i].DefaultValue != nullptr) { firstDefault = i; break; }
                }
                for (int cutoff = firstDefault; firstDefault >= 0 && cutoff < (int)declParams.size(); cutoff++)
                {
                    std::vector<LLVMBackend::TypeAndValue> wrapperParams(declParams.begin(), declParams.begin() + cutoff);
                    compiler->CreateFunctionDeclaration(declName, typeAndValue, wrapperParams, typeAndValue.external, false, false, false, typeAndValue.CallConv, linkName);
                }
            }
            else if (direct != nullptr)
            {
                auto braceInitializer = initDecl->initializer();
                auto braceInitializerList = initDecl->initializerList();
                if (braceInitializerList == nullptr && braceInitializer != nullptr)
                    braceInitializerList = braceInitializer->initializerList();
                // Pointer-form unique brace initializers stay on the builtin diagnostic path;
                // core unique<T> has a struct initializer syntax with different semantics.
                bool hasBraceInitializer = braceInitializerList != nullptr
                    || (braceInitializer != nullptr && braceInitializer->LeftBrace() != nullptr)
                    || initDecl->LeftBrace() != nullptr;
                if (compiler->IsCoreUniqueType(typeAndValue.TypeName)
                    && (hasBraceInitializer || direct->assignmentExpression() != nullptr))
                {
                    typeAndValue.TypeName = MangledGenericArgument(
                        *compiler, typeAndValue.TypeName);
                    typeAndValue.Pointer = true;
                    typeAndValue.ElemPointer = false;
                    typeAndValue.PointerDepth = 1;
                    typeAndValue.IsUnique = true;
                }
                auto identList = declarator->identifierList();
                std::string name = getDirectDeclName(direct);
                // `_` is the reserved discard target (`_ = expr`), never a binding. Rejecting the
                // declaration keeps `_ = ...` unambiguously a discard, not a hidden assignment to
                // a user variable that silently never updates.
                if (name == "_")
                    LogErrorContext(direct, "'_' is reserved as the discard target; it cannot name a variable.");
                // A namespace-scope global registers under its qualified name (Cfg.W),
                // mirroring namespace functions and enum members: qualified access
                // (Cfg.W) resolves through the global table, and two namespaces can
                // each declare the same global name without colliding in the IR.
                if (!namespaceName.empty())
                    name = namespaceName + "." + name;
                // Fixed-size array local - two forms:
                //   C-style:    T arr[N]  - size in directDeclarator
                //   Type-first: T[N] arr  - size in declarationSpecifier (arraySize already evaluated)
                typeAndValue.ConstArraySize = 0;  // reset per-declarator
                if (direct->assignmentExpression() && typeAndValue.ArraySize == nullptr)
                {
                    // Pointer arrays (char* arr[N]) can't use type-first form - grammar
                    // doesn't support 'char*[N]' - so only error on non-pointer types.
                    if (!typeAndValue.Pointer)
                        LogErrorContext(direct, std::format(
                            "C-style array declaration '{}[{}]' is not allowed; use '[{}] {}'",
                            name, direct->assignmentExpression()->getText(),
                            direct->assignmentExpression()->getText(), name));
                    auto* sizeVal = ParseAssignmentExpression(direct->assignmentExpression());
                    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(sizeVal))
                        typeAndValue.ConstArraySize = ci->getZExtValue();
                }
                else if (arraySize != nullptr)
                {
                    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(arraySize))
                        typeAndValue.ConstArraySize = ci->getZExtValue();
                }
                else if (typeAndValue.AliasArraySize > 0)
                {
                    // Size came from an array alias (using Vec3 = float[3]); the declarator has no
                    // brackets of its own. The size lives on the type, shared by every declarator.
                    typeAndValue.ConstArraySize = typeAndValue.AliasArraySize;
                }
                typeAndValue.VariableName = name;

                // `static` local: module-global storage, initializer runs at most once. The guard
                // opens HERE - before this declarator's IR - and closes on every path out of it.
                StaticLocalGuardScope staticScope;
                if (typeAndValue.staticStorage && !global_scope && compiler->IsInsertBlockLive())
                {
                    staticScope.Owner = this;
                    staticScope.Name = name;
                    OpenStaticLocalGuard(staticScope.Guard, name);
                    compiler->RequestStaticLocalStorage(name, typeAndValue.TypeName);
                }
                bool isStaticLocal = staticScope.Owner != nullptr;

                if (identList != nullptr)
                {
                    // TODO
                    LogErrorContext(identList, "Not Yet Implemented.");
                }

                llvm::Value* right = nullptr;
                bool pendingGlobalDefaultConstruction = false;
                LLVMBackend::NamedVariable interfaceSourceNV;
                bool haveInterfaceSourceNV = false;
                LLVMBackend::NamedVariable initializerSourceNV;
                bool haveInitializerSourceNV = false;
                bool srcIsUnsigned = false;
                bool srcIsBorrowed = false;
                std::string srcBorrowedOrigin;
                std::string srcBorrowedUniqueField; // "Struct.field" when the borrow came from a `unique` field
                bool srcBorrowedUniqueFieldViaCall = false; // proven through a call, so `move <origin>` is unspellable
                std::string srcBorrowedField;       // RHS field name, when the borrow came through a field read
                // Field-path provenance of the RHS, kept past rightNV's scope so the decl-init
                // consume arm can ask whether the source roots at a borrowed by-value parameter.
                LLVMBackend::NamedVariable srcFieldPathNV;
                bool srcIsOwningMove = false;       // RHS is an owning pointer (move param or alias thereof via cast)
                std::string srcOwningName;          // name of the original owning source, for nulling on transfer
                // RHS is a plain read of a live OWNING local (or of another such copy). See
                // NamedVariable::BorrowsOwningLocal - the copy owns nothing, so deleting it double-frees.
                bool srcBorrowsOwningLocal = false;
                std::string srcOwningLocalOrigin;
                llvm::Value* srcOwningLocalStorage = nullptr;
                // Concrete type inferred from the initializer, used to resolve an 'auto'
                // declaration's TypeName (LLVM opaque pointers cannot recover the pointee
                // from the value type alone, so typeof/nameof need the name captured here).
                std::string srcInferredTypeName;
                bool srcInferredPointer = false;
                bool srcInferredElemPointer = false;
                // Fixed-array shape of the initializer (`int[3] a` -> 3). Drives the `auto`
                // array-view deduction and the fixed-array-to-fixed-array copy below.
                uint64_t srcConstArraySize = 0;
                std::vector<uint64_t> srcConstInnerDimensions;
                bool srcIsArrayView = false;
                bool srcIsSimd = false;
                // Raw AST text of the initializer, used as the delete-guard's source name for a
                // GLOBAL read: CallerName is unset there, and the LLVM symbol may carry a '.N' suffix.
                std::string srcRhsExprText;
                // True when the initializer is itself array-shaped (a fixed array or a view).
                // A fixed-array destination only takes the copy path for such a source; a scalar
                // pointer / `new` / string-literal RHS keeps its pre-existing diagnostic.
                bool srcIsArrayShaped = false;
                // Managed-value copy at decl-init (string-redesign Phase 1): captured from the
                // RHS NamedVariable so the store site can route `ManagedType b = a;` through
                // a.copy() instead of a shallow alias. srcStorage non-null + !srcIsMove + a
                // CallerName marks a named lvalue source whose buffer is still owned elsewhere.
                llvm::Value* srcStorage = nullptr;
                // Non-null when the source is a UNION member: Storage is the union alloca, so any
                // type-inferred load off it reads the whole union instead of the member.
                llvm::Type* srcUnionFieldType = nullptr;
                llvm::Type* srcBaseType = nullptr;
                // The initializer's RESULT VALUE, before any decl-site coercion. Ownership
                // adoption is answered from this by value identity, never from a sticky flag.
                llvm::Value* srcPrimary = nullptr;
                bool srcIsMove = false;
                bool coreUniqueImplicitDefault = false;
                bool srcMovedFromSlot = false;
                std::string srcCallerName;
                std::vector<std::string> rhsLambdaCaptureNames;
                std::vector<std::string> rhsLambdaReferenceCaptureNames;
                // True when the initializer is (or transitively aliases) an owning string FIELD,
                // e.g. `string t = b.name;`. Such a local borrows a heap buffer some struct still
                // owns, so storing it into another field would double-free - tracked so the
                // field-store reject can catch the laundered field-to-field copy.
                bool srcBorrowsOwnedString = false;
                // True when the initializer READS a `string` element of a fixed array
                // (`string q = dst[0];`). Such a source has no FieldName, so the field-borrow
                // taint above never sees it, yet the element owns its buffer just as a field does.
                bool srcIsOwningArrayStringElem = false;
                bool srcIsRawHeapStringElem = false;
                bool srcPtrCopyIsRawNewArray = false;
                bool srcPointsToBorrowedByValueParam = false;
                llvm::Value* srcRawArrayLength = nullptr;
                // True when the initializer reads an element of a user `T[]` VIEW. The GEP has a
                // container slot's single-index shape, so only this provenance separates them.
                bool srcIsViewElem = false;
                // True when the initializer is an `alias` (borrow) return (`Token t = toks.get(0)`).
                // The local shallow-aliases storage it does not own, so its scope-exit destructor
                // must be suppressed (IsAliasBorrow) - otherwise it double-frees the source's buffer.
                bool srcIsAlias = false;
                // A non-pointer alias declaration is a reference binding. Keep the RHS slot instead
                // of copying its loaded value into the declaration's temporary alloca.
                llvm::Value* aliasStorage = nullptr;
                bool bindAliasReference = false;
                // True when the initializer borrows an element the owning container frees
                // (`B* g = l.get(0)` on a `list<unique B*>`). A later `delete g` double-frees.
                bool srcBorrowsOwnedElement = false;
                bool srcCoreUniqueElementBorrow = false;
                std::string srcOwnedElementContainer;
                // True when the borrowed element's owner lives outside the container (list<alias X*>);
                // selects the alias delete message instead of the unique-element one.
                bool srcElementExternallyOwned = false;
                // Implied move of a `move`-temp's owning field (`string s = makeToken().text`): the temp
                // owns the field, so the local adopts it and the source field in the temp is zeroed.
                bool srcMovableTempField = false;
                llvm::Value* srcMoveTempStructAlloca = nullptr;
                llvm::Type* srcMoveTempStructType = nullptr;
                unsigned srcMoveTempFieldIndex = 0;
                auto initializer = initDecl->initializer();

                // Bare-brace form: T[N] arr {} - LeftBrace/initializerList are on initDecl directly.
                // Equivalent to T[N] arr = {} / T[N] arr = {field=v,...}.
                bool barebraceInit = initDecl->LeftBrace() != nullptr;
                CFlatParser::InitializerListContext* barebraceList = barebraceInit ? initDecl->initializerList() : nullptr;

                /*
                 * `u8[] a = embed("...")` sizes its own destination: the asset's byte count IS the
                 * extent, so the declaration is a fixed 'u8[N]', not a length-inferred view. The
                 * size must be known BEFORE storage is created, so the path is read here from the
                 * initializer's syntax; the expression itself re-reads it and folds the bytes.
                 */
                if (typeAndValue.IsArrayView && typeAndValue.ConstArraySize == 0
                    && initializer != nullptr)
                {
                    if (auto embedSize = DirectEmbedByteCount(initializer->assignmentExpression()))
                    {
                        typeAndValue.IsArrayView = false;
                        typeAndValue.Pointer = false;
                        typeAndValue.ConstArraySize = *embedSize;
                    }
                }

                // Empty '{}' on a POINTER target: rejected as AMBIGUOUS (null pointer vs pointer
                // to an empty object) in every spelling. A NON-pointer '{}' has ONE reading, so
                // it seeds instead - that one-reading-vs-two split is why this arm exists.
                bool emptyBraceToken = (barebraceInit && barebraceList == nullptr)
                    || (initializer != nullptr && initializer->LeftBrace() != nullptr
                        && initializer->initializerList() == nullptr);
                // Array VIEWS are the ONLY exemption; they keep their "cannot infer the length"
                // message. 'simd' is NOT exempt - it sets Pointer from its own '*' (~3816).
                if (emptyBraceToken && typeAndValue.Pointer && !typeAndValue.IsArrayView)
                {
                    std::string role = typeAndValue.ConstArraySize > 0
                        ? std::format("array element of '{}'", name)
                        : std::format("declaration '{}'", name);
                    LogEmptyBraceOnPointerReject(direct, role, typeAndValue);
                }

                // Array value-initializer: T[N] arr = {} / T[N] arr {} / T[N] arr = {field=v,...}
                // Seed-once pattern: construct one element, then memcpy into every slot.
                if (typeAndValue.ConstArraySize > 0 &&
                    ((initializer != nullptr && initializer->LeftBrace() != nullptr) || barebraceInit))
                {
                    auto* arrInitList = barebraceInit ? barebraceList
                                      : initializer->initializerList();

                    // Positional fixed-array init: T[N] x = {v0, v1, ...}. Distinguished from
                    // the value-init forms ({} / {field=v}) by elements that carry no field name.
                    bool positionalArray = false;
                    if (arrInitList)
                        for (auto* fi : arrInitList->fieldInit())
                            if (fi->Identifier() == nullptr
                                && (fi->assignmentExpression().size() == 1 || FieldInitIsBraceElement(fi)))
                            { positionalArray = true; break; }

                    bool emptyArrInit = (arrInitList == nullptr || arrInitList->fieldInit().empty());

                    if (global_scope)
                    {
                        EmitGlobalFixedArrayInit(name, typeAndValue, arrInitList, positionalArray, direct, initDecl);
                    }
                    else if (positionalArray)
                    {
                        EmitPositionalFixedArrayInit(name, typeAndValue, arrInitList, arraySize, line, allocList);
                    }
                    else if (emptyArrInit && (typeAndValue.Pointer
                             || compiler->GetDataStructure(typeAndValue.TypeName).StructType == nullptr))
                    {
                        // Empty `{}` on a primitive/pointer element type is zero-init,
                        // equivalent to `= default`. (Struct empty-{} still seeds below.)
                        auto* arrAlloc = compiler->CreateLocalVariable(typeAndValue, nullptr, arraySize, line);
                        allocList.push_back(std::pair(name, llvm::dyn_cast<llvm::AllocaInst>(arrAlloc)));
                        auto* arrTy = llvm::cast<llvm::ArrayType>(compiler->GetType(typeAndValue));
                        compiler->builder->CreateStore(llvm::Constant::getNullValue(arrTy), arrAlloc);
                    }
                    else
                    {
                        auto structData = compiler->GetDataStructure(typeAndValue.TypeName);
                        if (typeAndValue.Pointer)
                        {
                            LLVMBackend::TypeAndValue baseType = typeAndValue;
                            baseType.Pointer = false;
                            baseType.ElemPointer = false;
                            baseType.PointerDepth = 0;
                            baseType.IsArrayView = false;
                            // 'S*[N] a = {field=v}' - the seed built below is an 'S', memcpy'd over
                            // each POINTER slot, so the field values become the element addresses.
                            LogPointerBraceInitReject(initDecl, std::format("array element of '{}'", name),
                                SpellType(*compiler, baseType), DescribePointerDeclType(typeAndValue),
                                CanSuggestAllocation(initDecl, typeAndValue));
                        }
                        else if (structData.StructType == nullptr)
                        {
                            LogErrorContext(initDecl, std::format("array value-initializer '= {{}}' requires a struct element type, '{}' is not a struct",
                                SpellType(*compiler, typeAndValue)));
                        }
                        else
                        {
                            auto* elemTy = structData.StructType;
                            auto* initList = barebraceInit ? barebraceList
                                           : initializer->initializerList();

                            {
                                // Every element is built INDEPENDENTLY, whatever its ownership:
                                // an owning element replicated bitwise aliases one resource into
                                // every slot and double-frees, and a non-owning one with a field
                                // initializer or a named override must run it once per element
                                // (the C++ rule for `S a[N]`), not once for the whole array.
                                auto* arrAlloc = compiler->CreateLocalVariable(typeAndValue, nullptr, arraySize, line);
                                allocList.push_back(std::pair(name, llvm::dyn_cast<llvm::AllocaInst>(arrAlloc)));
                                EmitArrayValueInitSlots(arrAlloc, elemTy, typeAndValue, initList, false);
                            }
                        }
                    }
                    continue;
                }

                // Length-inferred array-view: T[] x = {v0, v1, ...}. `T[]` is a thin T*, so
                // the brace list both provides the backing storage and infers its extent.
                if (typeAndValue.IsArrayView &&
                    ((initializer != nullptr && initializer->LeftBrace() != nullptr) || barebraceInit))
                {
                    // initializerList is optional in the grammar, so empty `{}` yields null here.
                    auto* arrInitList = barebraceInit ? barebraceList
                                      : initializer->initializerList();
                    if (global_scope)
                        LogErrorContext(initDecl, "array-view initializer '= {}' is not allowed at global scope");
                    else if (isStaticLocal)
                        // The brace list's backing array is a stack alloca, so the static view would
                        // dangle after the first call. A sized 'T[N]' static owns its own storage.
                        LogErrorContext(initDecl, std::format(
                            "a 'static' array view cannot be initialized from a brace list - its "
                            "backing storage is the stack frame of the first call, so '{}' would "
                            "dangle afterwards. Declare it with an explicit size ('{}[N] {}') so the "
                            "static owns the elements.", name, SpellType(*compiler, typeAndValue), name));
                    else
                        EmitArrayViewInferredInit(name, typeAndValue, arrInitList, line, allocList);
                    continue;
                }

                // At global scope, any RHS evaluation emits IR into whatever block the builder
                // currently points at (the tail of the last generated function), corrupting it.
                // Guard against this by redirecting into a throwaway block; if the result isn't
                // a compile-time constant we emit an error and discard the temp IR.
                llvm::Function* globalInitTempFn = nullptr;
                LLVMBackend::BuilderState globalInitSavedState;
                if (global_scope && initializer != nullptr && initializer->assignmentExpression() != nullptr)
                {
                    globalInitSavedState = compiler->SaveBuilderState();
                    auto* voidTy = llvm::FunctionType::get(compiler->builder->getVoidTy(), false);
                    globalInitTempFn = llvm::Function::Create(
                        voidTy, llvm::Function::PrivateLinkage, "__global_init_tmp", compiler->module.get());
                    auto* tmpBB = llvm::BasicBlock::Create(*compiler->context, "entry", globalInitTempFn);
                    compiler->builder->SetInsertPoint(tmpBB);
                }

                if (initializer != nullptr)
                {
                    auto assignmentExpression = initializer->assignmentExpression();
                    if (assignmentExpression != nullptr)
                        srcRhsExprText = assignmentExpression->getText();
                    // Inbound alloc-align channel: an align-declared local whose initializer is a
                    // DIRECT `new` hands the declared allocation alignment down so the bare `new`
                    // allocates aligned. Gated on AsDirectNew so an indirect `new` (ternary/cast/
                    // call-arg) does NOT silently inherit - the post-check below then errors.
                    if (assignmentExpression != nullptr
                        && typeAndValue.AllocAlignValue > LLVMBackend::kDefaultNewAlign
                        && AsDirectNew(assignmentExpression) != nullptr)
                        compiler->pendingInitAllocAlign = typeAndValue.AllocAlignValue;
                    if (assignmentExpression != nullptr)
                        ArmArrayNewDesugar(assignmentExpression, typeAndValue);
                    if (assignmentExpression != nullptr)
                    {
                        std::optional<DeclExpectedTypeScope> coreUniqueExpectedScope;
                        if (compiler->IsCoreUniqueType(typeAndValue.TypeName)
                            && compiler->IsInterfaceType(MangledGenericArgument(
                                *compiler, typeAndValue.TypeName)))
                            coreUniqueExpectedScope.emplace(&declExpectedType, typeAndValue);
                        if (typeAndValue.IsFatInterfaceValue())
                        {
                            // An interface declaration is a destination too: an immediately-
                            // invoked literal in its RHS must see this interface, not an outer
                            // lambda's return type.
                            DeclExpectedTypeScope declExpectedScope(&declExpectedType, typeAndValue);
                            // For interface declarations, preserve NamedVariable type info
                            // so we can do the class->interface fat-struct upcast when needed.
                            auto rightNV = ParseAssignmentExpressionNamed(assignmentExpression);
                            ApplyCallResultBorrowProvenance(compiler, rightNV);
                            interfaceSourceNV = rightNV;
                            haveInterfaceSourceNV = true;
                            if (!global_scope && typeAndValue.IsAlias && !typeAndValue.Pointer
                                && rightNV.Storage != nullptr)
                            {
                                aliasStorage = rightNV.Storage;
                                bindAliasReference = true;
                            }
                            // Interface decl-init is its OWN branch, so the escape gate in the
                            // else below never sees `IShape s = makeIBox().t;` (it dangled).
                            if (!global_scope)
                                GuardOwningTempUniqueFieldEscape(
                                    rightNV, "an interface local", assignmentExpression);
                            right = LoadNamedVariable(rightNV);
                            srcPrimary = rightNV.Primary;
                            // A core unique<T> local is an owner wrapper, not an implementor
                            // object. A plain interface destination borrows its pointee, so expose
                            // the wrapper's _p slot before the normal concrete-to-interface box.
                            if (right != nullptr && !rightNV.TypeAndValue.Pointer
                                && compiler->IsCoreUniqueType(rightNV.TypeAndValue.TypeName))
                            {
                                bool coreFieldBorrow =
                                    IsUniqueFieldRead(rightNV) || rightNV.IsUniqueFieldAlias;
                                std::string coreFieldOwner = coreFieldBorrow
                                    ? DescribeUniqueFieldAccess(rightNV) : std::string();
                                auto* uniqueStorage = rightNV.Storage;
                                if (uniqueStorage == nullptr)
                                {
                                    uniqueStorage = compiler->CreateAlloca(right->getType());
                                    compiler->builder->CreateStore(right, uniqueStorage);
                                }
                                const auto& uniqueData = compiler->GetDataStructure(
                                    rightNV.TypeAndValue.TypeName);
                                // The interface arm stores the fat value in its interface-typed
                                // field; unwrapping it yields the interface directly, and a MOVED
                                // source hands the boxed object over rather than lending it.
                                bool interfaceArm = false;
                                LLVMBackend::TypeAndValue armField;
                                for (size_t i = 0; i < uniqueData.StructFields.size(); i++)
                                    if (uniqueData.StructFields[i].VariableName == "_p")
                                    {
                                        interfaceArm = uniqueData.StructFields[i].IsInterface;
                                        armField = uniqueData.StructFields[i];
                                        right = compiler->CreateLoad(compiler->CreateStructGEP(
                                            right->getType(), uniqueStorage, (uint32_t)i));
                                        break;
                                    }
                                bool transfersOwnership = rightNV.TypeAndValue.IsMove
                                    || rightNV.IsExplicitMove;
                                if (interfaceArm)
                                {
                                    armField.VariableName.clear();
                                    rightNV.TypeAndValue = armField;
                                }
                                else
                                {
                                    rightNV.TypeAndValue.TypeName = MangledGenericArgument(
                                        *compiler, rightNV.TypeAndValue.TypeName);
                                    rightNV.TypeAndValue.Pointer = true;
                                }
                                rightNV.TypeAndValue.IsMove = interfaceArm && transfersOwnership;
                                rightNV.IsOwning = interfaceArm && transfersOwnership;
                                rightNV.IsBorrowed = !rightNV.IsOwning;
                                if (!rightNV.IsOwning)
                                {
                                    rightNV.BorrowedOrigin = rightNV.CallerName;
                                    if (coreFieldBorrow)
                                        rightNV.BorrowedUniqueField = coreFieldOwner;
                                }
                                rightNV.Primary = right;
                                rightNV.Storage = nullptr;
                                rightNV.BaseType = right->getType();
                                interfaceSourceNV = rightNV;
                            }
                            // Derived-interface -> parent-interface: re-box (a derived vtable is not
                            // layout-compatible with the parent's once field slots exist). A '?:'
                            // result has no NamedVariable TypeName, so fall back to its join ledger.
                            if (right && right->getType() == compiler->GetFatPtrType())
                            {
                                std::string srcIface = compiler->ResolveFatInterfaceSrcName(right,
                                    rightNV.TypeAndValue.IsInterface ? rightNV.TypeAndValue.TypeName : std::string());
                                right = compiler->ReboxInterfaceIfNeeded(
                                    right, srcIface, typeAndValue.TypeName);
                            }
                            if (right && right->getType() != compiler->GetFatPtrType())
                            {
                                std::string structName = rightNV.TypeAndValue.TypeName;
                                // Catch the case where the RHS is a known type that doesn't implement the interface.
                                bool rhsIsKnownType = !structName.empty() &&
                                    (compiler->dataStructures.count(structName) || compiler->programTable.count(structName));
                                // A '?:' / '??' join result carries no TypeName: box each arm in
                                // its own branch. The arms keep their ownership (see the helper).
                                std::string ternaryArmFailure;
                                std::string joinSpelling = "?:";
                                llvm::Value* ternaryFat = structName.empty()
                                    ? UpcastPointerJoinToInterface(right, typeAndValue.TypeName,
                                                                   &ternaryArmFailure, &joinSpelling)
                                    : nullptr;
                                if (ternaryFat != nullptr)
                                    right = ternaryFat;
                                else if (!ternaryArmFailure.empty())
                                    LogErrorContext(assignmentExpression, std::format(
                                        "cannot convert '{}' arm to interface '{}': {}",
                                        joinSpelling, SpellType(*compiler, typeAndValue), ternaryArmFailure));
                                else if (rhsIsKnownType && !compiler->StructImplementsInterface(structName, typeAndValue.TypeName))
                                {
                                    LogErrorContext(assignmentExpression,
                                        std::format("'{}' does not implement interface '{}'",
                                            SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = structName }),
                                            SpellType(*compiler, typeAndValue)));
                                }
                                else if (!structName.empty()
                                         && compiler->StructImplementsInterface(structName, typeAndValue.TypeName))
                                {
                                    // Shared boxing helper: shape rejection, data-pointer selection
                                    // and the owning-source transfer all live there now.
                                    right = BoxConcreteIntoInterface(
                                        assignmentExpression, right, rightNV.TypeAndValue.Pointer,
                                        structName, typeAndValue.TypeName, &rightNV);
                                }
                                // Provably unboxable: a primitive-element array / view / 'T**'.
                                // The helper throws; the empty body keeps control out of the store.
                                else if (RejectPrimitiveShapedInterfaceUpcast(
                                             assignmentExpression, rightNV.TypeAndValue,
                                             typeAndValue.TypeName))
                                {
                                }
                                else if (typeAndValue.TypeName == "string" &&
                                         right->getType() == cflat_llvm::PointerTo(compiler->builder->getInt8Ty()))
                                {
                                    // A raw i8*/char* assigned to a string variable.
                                    // If it is a compile-time string literal constant (length known at
                                    // compile time), wrap it directly in a string struct on the caller's stack.
                                    // Otherwise call user-defined operator string(char*) for runtime values.
                                    auto* c = llvm::dyn_cast<llvm::Constant>(right);
                                    if (c && compiler->IsStringLiteralConstant(c))
                                    {
                                        right = compiler->WrapStringLiteralAsString(right);
                                    }
                                    else if (compiler->GetFunction("operator string"))
                                    {
                                        LLVMBackend::NamedVariable argNV;
                                        argNV.Primary = right;
                                        argNV.BaseType = right->getType();
                                        argNV.TypeAndValue.TypeName = "char";
                                        argNV.TypeAndValue.Pointer = true;
                                        right = compiler->CreateOverloadedFunctionCall("operator string", { argNV });
                                    }
                                    else
                                    {
                                        right = compiler->WrapStringLiteralAsString(right);
                                    }
                                }
                            }
                            // nullptr constant assigned to an interface variable - produce null fat pointer {null, null}
                            if (llvm::isa_and_nonnull<llvm::ConstantPointerNull>(right))
                                right = llvm::Constant::getNullValue(compiler->GetFatPtrType());
                        }
                        else
                        {
                            // Thread expected function-pointer type into lambda expression parsing.
                            if (typeAndValue.IsFunctionPointer)
                                lambdaExpectedType = typeAndValue;
                            // A non-funcptr destination still supplies the return type an
                            // IMMEDIATELY-INVOKED literal in this initializer needs; the postfix
                            // path consults it so the literal stops inheriting an enclosing one.
                            DeclExpectedTypeScope declExpectedScope(&declExpectedType, typeAndValue);
                            std::string genericFuncCallerName;
                            bool rhsIsFuncPtr = false;
                            bool rhsIsThinFnPtr = false;
                            // Hoisted for the thin function<> provenance gate below, which needs the
                            // RHS's declared-pointer shape after rightNV itself goes out of scope.
                            bool rhsIsPointer = false;
                            bool rhsFuncPtrReturnOwned = false;
                            bool rhsFuncPtrReturnAlias = false;
                            std::vector<LLVMBackend::TypeAndValue::FuncPtrParam> rhsFuncPtrParams;
                            // Captured-variable names of an RHS lambda literal, hoisted out of the
                            // rightNV scope so the fat->thin narrowing gate below can name them.
                            {
                                auto rightNV = ParseAssignmentExpressionNamed(assignmentExpression);
                                ApplyCallResultBorrowProvenance(compiler, rightNV);
                                initializerSourceNV = rightNV;
                                haveInitializerSourceNV = true;
                                if (!global_scope && typeAndValue.IsAlias && !typeAndValue.Pointer
                                    && rightNV.Storage != nullptr)
                                {
                                    aliasStorage = rightNV.Storage;
                                    bindAliasReference = true;
                                }
                                right = LoadNamedVariable(rightNV);
                                // A moved core unique interface wrapper can be reboxed into a
                                // different core unique interface wrapper; do not bitcast structs.
                                if (right != nullptr && !typeAndValue.Pointer
                                    && !rightNV.TypeAndValue.Pointer
                                    && compiler->IsCoreUniqueType(typeAndValue.TypeName)
                                    && compiler->IsCoreUniqueType(rightNV.TypeAndValue.TypeName)
                                    && compiler->IsInterfaceType(MangledGenericArgument(
                                        *compiler, typeAndValue.TypeName))
                                    && compiler->IsInterfaceType(MangledGenericArgument(
                                        *compiler, rightNV.TypeAndValue.TypeName)))
                                {
                                    const std::string sourceInterface =
                                        MangledGenericArgument(
                                            *compiler, rightNV.TypeAndValue.TypeName);
                                    const std::string destinationInterface =
                                        MangledGenericArgument(*compiler, typeAndValue.TypeName);
                                    if (sourceInterface != destinationInterface)
                                    {
                                        auto* wrapperStorage = rightNV.Storage;
                                        if (wrapperStorage == nullptr)
                                        {
                                            wrapperStorage = compiler->CreateAlloca(right->getType());
                                            compiler->builder->CreateStore(right, wrapperStorage);
                                        }
                                        llvm::Value* interfaceValue = nullptr;
                                        const auto& sourceData = compiler->GetDataStructure(
                                            rightNV.TypeAndValue.TypeName);
                                        for (size_t i = 0; i < sourceData.StructFields.size(); i++)
                                            if (sourceData.StructFields[i].IsInterface)
                                            {
                                                interfaceValue = compiler->CreateLoad(
                                                    compiler->CreateStructGEP(
                                                        right->getType(), wrapperStorage,
                                                        (uint32_t)i));
                                                break;
                                            }
                                        auto* reboxed = compiler->ReboxInterfaceIfNeeded(
                                            interfaceValue, sourceInterface, destinationInterface);
                                        if (reboxed == interfaceValue)
                                            right = GenerateDefaultValue(typeAndValue);
                                        else
                                            right = reboxed;
                                    }
                                }
                                if (right != nullptr && !typeAndValue.Pointer
                                    && !InGenericInstantiation()
                                    && compiler->IsCoreUniqueType(typeAndValue.TypeName)
                                    && rightNV.TypeAndValue.TypeName == typeAndValue.TypeName
                                    && !rightNV.TypeAndValue.IsMove
                                    && !rightNV.IsExplicitMove
                                    && !compiler->lastCallReturnsOwned)
                                {
                                    const std::string sourceName =
                                        LLVMBackend::BorrowedSourceName(rightNV);
                                    if (sourceName.empty())
                                        LogErrorContext(assignmentExpression, std::format(
                                            "cannot initialize {} '{}' from a borrowed value - the source still "
                                            "owns it; use 'new', a 'move' expression, or a move-returning call",
                                            SpellType(*compiler, typeAndValue), name));
                                    else
                                        LogErrorContext(assignmentExpression, std::format(
                                            "cannot initialize {} '{}' from borrowed value '{}' - the source still "
                                            "owns it; use 'new', a 'move' expression, or a move-returning call",
                                            SpellType(*compiler, typeAndValue), name,
                                            sourceName));
                                }
                                if (right != nullptr
                                    && !typeAndValue.Pointer
                                    && compiler->IsCoreUniqueType(typeAndValue.TypeName)
                                    && compiler->IsInterfaceType(MangledGenericArgument(
                                        *compiler, typeAndValue.TypeName))
                                    && right->getType()->isStructTy()
                                    && compiler->StructImplementsInterface(
                                        rightNV.TypeAndValue.TypeName,
                                        MangledGenericArgument(*compiler, typeAndValue.TypeName)))
                                {
                                    const std::string sourceName =
                                        LLVMBackend::BorrowedSourceName(rightNV);
                                    if (sourceName.empty())
                                        LogErrorContext(assignmentExpression, std::format(
                                            "cannot initialize {} '{}' from a borrowed value - the source is a stack value; "
                                            "use 'new', a 'move' expression, or a move-returning call",
                                            SpellType(*compiler, typeAndValue), name));
                                    else
                                        LogErrorContext(assignmentExpression, std::format(
                                            "cannot initialize {} '{}' from borrowed value '{}' - the source is a stack value; "
                                            "use 'new', a 'move' expression, or a move-returning call",
                                            SpellType(*compiler, typeAndValue), name,
                                            sourceName));
                                }
                                // A core unique value is an observer when it initializes a plain
                                // raw pointer local. Extract `_p` directly so legacy pointer locals
                                // keep their borrow semantics without copying the wrapper.
                                // A `unique X*` destination the desugar deliberately left on the
                                // builtin spelling still takes a core unique<X> source: unwrap `_p`
                                // and let the move/borrow classification below decide ownership.
                                if (right != nullptr && typeAndValue.Pointer
                                    && !rightNV.TypeAndValue.Pointer
                                    && compiler->IsCoreUniqueType(rightNV.TypeAndValue.TypeName))
                                {
                                    srcCoreUniqueElementBorrow = rightNV.TypeAndValue.IsAlias
                                        && !rightNV.TypeAndValue.IsMove;
                                    // Judge a JOINED escape on the WRAPPER: extracting `_p` below
                                    // severs the ledger the join is the only carrier of. A direct
                                    // field read keeps its own provenance and its own diagnostic.
                                    if (!global_scope && compiler != nullptr
                                        && !compiler->IsLedgeredOwningTempUniqueField(rightNV.Primary)
                                        && compiler->JoinCarriesOwningTempUniqueField(rightNV.Primary))
                                        GuardOwningTempUniqueFieldEscape(
                                            rightNV, "a local", assignmentExpression);
                                    bool movingUnique = rightNV.TypeAndValue.IsMove
                                        || rightNV.IsExplicitMove;
                                bool coreFieldBorrow = !movingUnique
                                    && (IsUniqueFieldRead(rightNV) || rightNV.IsUniqueFieldAlias
                                        || rightNV.BorrowedUniqueFieldViaCall);
                                std::string coreFieldOwner = coreFieldBorrow
                                    ? (rightNV.BorrowedUniqueField.empty()
                                        ? DescribeUniqueFieldAccess(rightNV)
                                        : rightNV.BorrowedUniqueField)
                                    : std::string();
                                    auto* uniqueStorage = rightNV.Storage;
                                    if (uniqueStorage == nullptr)
                                    {
                                        uniqueStorage = compiler->CreateAlloca(right->getType());
                                        compiler->builder->CreateStore(right, uniqueStorage);
                                    }
                                    const auto& uniqueData = compiler->GetDataStructure(
                                        rightNV.TypeAndValue.TypeName);
                                    for (size_t i = 0; i < uniqueData.StructFields.size(); i++)
                                        if (uniqueData.StructFields[i].VariableName == "_p")
                                        {
                                            right = compiler->CreateLoad(compiler->CreateStructGEP(
                                                right->getType(), uniqueStorage, (uint32_t)i));
                                            break;
                                        }
                                    auto pointeeName = MangledGenericArgument(
                                        *compiler, rightNV.TypeAndValue.TypeName);
                                    rightNV.TypeAndValue.TypeName = pointeeName;
                                    rightNV.TypeAndValue.Pointer = true;
                                    rightNV.TypeAndValue.IsMove = movingUnique;
                                    rightNV.IsOwning = movingUnique;
                                    rightNV.IsBorrowed = !movingUnique;
                                    rightNV.BorrowedOrigin = movingUnique ? std::string() : rightNV.CallerName;
                                    if (coreFieldBorrow)
                                        rightNV.BorrowedUniqueField = coreFieldOwner;
                                    rightNV.Primary = right;
                                    rightNV.Storage = nullptr;
                                    rightNV.BaseType = right->getType();
                                    initializerSourceNV = rightNV;
                                }
                                // The same observer conversion feeds a plain interface local:
                                // box the pointee, never the unique<T> wrapper itself.
                                if (right != nullptr && typeAndValue.IsInterface
                                    && !typeAndValue.IsUnique
                                    && !rightNV.TypeAndValue.Pointer
                                    && compiler->IsCoreUniqueType(rightNV.TypeAndValue.TypeName))
                                {
                                    auto* uniqueStorage = rightNV.Storage;
                                    if (uniqueStorage == nullptr)
                                    {
                                        uniqueStorage = compiler->CreateAlloca(right->getType());
                                        compiler->builder->CreateStore(right, uniqueStorage);
                                    }
                                    const auto& uniqueData = compiler->GetDataStructure(
                                        rightNV.TypeAndValue.TypeName);
                                    for (size_t i = 0; i < uniqueData.StructFields.size(); i++)
                                        if (uniqueData.StructFields[i].VariableName == "_p")
                                        {
                                            right = compiler->CreateLoad(compiler->CreateStructGEP(
                                                right->getType(), uniqueStorage, (uint32_t)i));
                                            break;
                                        }
                                    rightNV.TypeAndValue.TypeName = MangledGenericArgument(
                                        *compiler, rightNV.TypeAndValue.TypeName);
                                    rightNV.TypeAndValue.Pointer = true;
                                    rightNV.TypeAndValue.IsMove = false;
                                    rightNV.IsOwning = false;
                                    rightNV.IsBorrowed = true;
                                    rightNV.BorrowedOrigin = rightNV.CallerName;
                                    rightNV.Primary = right;
                                    rightNV.Storage = nullptr;
                                    rightNV.BaseType = right->getType();
                                    initializerSourceNV = rightNV;
                                }
                                if (right && rightNV.TypeAndValue.IsAlias
                                    && !rightNV.TypeAndValue.Pointer
                                    && rightNV.Primary != nullptr
                                    && (rightNV.TypeAndValue.IsFunctionPointer
                                        || compiler->GetEncodedClosureType(
                                            rightNV.TypeAndValue.TypeName) != nullptr))
                                    right = compiler->CreateLoad(
                                        compiler->GetType(rightNV.TypeAndValue), rightNV.Primary);
                                // A `move`-temp's owning field can be MOVED into the local (the temp owns it):
                                // capture the source so the store site adopts it and zeros the temp's field.
                                // POINTER fields excluded as at the `=` twin: the aggregate-zero
                                // store on a pointer field emits IR that crashes codegen.
                                srcMovableTempField = rightNV.MovableTempField
                                    && !rightNV.TypeAndValue.Pointer
                                    && compiler->IsOwningValueType(rightNV.TypeAndValue.TypeName);
                                srcMoveTempStructAlloca = rightNV.MoveTempStructAlloca;
                                srcMoveTempStructType = rightNV.MoveTempStructType;
                                srcMoveTempFieldIndex = rightNV.MoveTempFieldIndex;
                                // Binding an owning field of a NON-move (alias) temp to a local REJECTED: its
                                // buffer is owned elsewhere and would be freed out from under the local.
                                // A `string` field instead binds by IMPLICIT COPY (see the ruling on
                                // AdoptImplicitStringTempCopy) - the reject stays for every other owner.
                                // A DECLARED `alias` local is excluded: the user spelled a borrow,
                                // and a borrow of a temporary is exactly the dangle this rejects.
                                if (rightNV.FromOwningTempField && !srcMovableTempField
                                    && !typeAndValue.IsAlias
                                    && !(rightNV.IsAliasBorrow && rightNV.TypeAndValue.Pointer)
                                    && rightNV.TypeAndValue.TypeName == "string"
                                    && AdoptImplicitStringTempCopy(rightNV, right, assignmentExpression))
                                {
                                    initializerSourceNV = rightNV;
                                }
                                // The whole-ELEMENT sibling (`string a = fields.get(0);`), which has no
                                // field access to mark it and used to bind a borrow that dangled once
                                // the container was cleared. Same copy, same `alias` exclusion.
                                else if (!srcMovableTempField && !typeAndValue.IsAlias
                                    && IsImplicitCopyableStringTemp(rightNV)
                                    && AdoptImplicitStringTempCopy(rightNV, right, assignmentExpression))
                                {
                                    initializerSourceNV = rightNV;
                                }
                                // The reference ABI keeps a direct alias return's Storage, so the
                                // whole-element copy arm above can no longer rely on a null Storage
                                // to identify that temporary. Preserve the old safe value binding for
                                // a non-alias string destination; a named alias local remains a borrow.
                                else if (!srcMovableTempField && !typeAndValue.IsAlias
                                    && (rightNV.TypeAndValue.IsAlias || rightNV.IsAliasBorrow)
                                    && llvm::isa_and_nonnull<llvm::CallBase>(rightNV.Storage)
                                    && rightNV.TypeAndValue.TypeName == "string"
                                    && !rightNV.TypeAndValue.IsMove)
                                {
                                    right = compiler->EmitOwnedStringDeepCopy(right);
                                    initializerSourceNV = rightNV;
                                }
                                else if (rightNV.FromOwningTempField && !srcMovableTempField
                                    && !(rightNV.IsAliasBorrow && rightNV.TypeAndValue.Pointer)
                                    && compiler->IsOwningValueType(rightNV.TypeAndValue.TypeName))
                                {
                                    if (rightNV.OwningStructName.empty() || rightNV.FieldName.empty())
                                        LogErrorContext(assignmentExpression,
                                            "cannot bind a field taken from a temporary to a local; its buffer is "
                                            "owned elsewhere and would be freed out from under the local. Bind the "
                                            "whole call result first (e.g. `auto t = ...;`) or use '.copy()' for "
                                            "an independent copy.");
                                    else
                                        LogErrorContext(assignmentExpression, std::format(
                                            "cannot bind '{}.{}' taken from a temporary to a local; its buffer is owned "
                                            "elsewhere and would be freed out from under the local. Bind the whole call "
                                            "result first (e.g. `auto t = ...;`) or use '.copy()' for an independent copy.",
                                            rightNV.OwningStructName, rightNV.FieldName));
                                }
                                // Same escape with a dtor-LESS pointee. Skipped at global scope,
                                // where the compile-time-constant message is the truer one.
                                if (!global_scope)
                                    GuardOwningTempUniqueFieldEscape(rightNV, "a local", assignmentExpression);
                                srcIsUnsigned = rightNV.TypeAndValue.IsUnsignedInteger() != -1;
                                genericFuncCallerName = rightNV.CallerName;
                                // A copy of a holder carrying a bonded closure escapes the tracking:
                                // the copy could then be returned or stored past the captured local.
                                if (rightNV.ContainsBondedClosure)
                                    LogErrorContext(assignmentExpression,
                                        "cannot copy a holder containing a bonded closure - the copy would outlive its captured local");
                                srcIsBorrowed = rightNV.IsBorrowed;
                                srcBorrowedOrigin = rightNV.BorrowedOrigin.empty()
                                    ? rightNV.CallerName
                                    : rightNV.BorrowedOrigin;
                                srcBorrowedUniqueField = rightNV.BorrowedUniqueField;
                                srcBorrowedUniqueFieldViaCall = rightNV.BorrowedUniqueFieldViaCall;
                                srcBorrowedField = rightNV.FieldName;
                                srcFieldPathNV.OwningStructName = rightNV.OwningStructName;
                                srcFieldPathNV.FieldName = rightNV.FieldName;
                                srcFieldPathNV.FieldPathText = rightNV.FieldPathText;
                                srcFieldPathNV.FieldPathRoot = rightNV.FieldPathRoot;
                                srcFieldPathNV.RootIsBorrowedByValueParam =
                                    rightNV.RootIsBorrowedByValueParam;
                                srcFieldPathNV.RootIsAliasBorrowLocal =
                                    rightNV.RootIsAliasBorrowLocal;
                                // The borrowed POINTER-parameter root travels on these three, and
                                // the guard needs all of them (IsElementAccess is the slot carve-out).
                                srcFieldPathNV.IsBorrowed = rightNV.IsBorrowed;
                                srcFieldPathNV.BorrowedOrigin = rightNV.BorrowedOrigin;
                                srcFieldPathNV.IsElementAccess = rightNV.IsElementAccess;
                                srcFieldPathNV.PointsToBorrowedByValueParam =
                                    rightNV.PointsToBorrowedByValueParam;
                                // The field's own type decides owning-VALUE vs pointer, so it has
                                // to travel too - the guard reads it, not the root's type.
                                srcFieldPathNV.TypeAndValue.TypeName = rightNV.TypeAndValue.TypeName;
                                srcFieldPathNV.TypeAndValue.Pointer = rightNV.TypeAndValue.Pointer;
                                srcFieldPathNV.TypeAndValue.ElemPointer = rightNV.TypeAndValue.ElemPointer;
                                srcFieldPathNV.TypeAndValue.ParentVariableName =
                                    rightNV.TypeAndValue.ParentVariableName;
                                // Trap B: a plain copy of a `unique` field does not null the field, so the
                                // field's synthesized destructor still frees the pointee. Treat the local as
                                // a borrow of the field, which routes a later `delete` into the existing
                                // "cannot delete" check while leaving reads alone. `move b.p` extracts
                                // ownership instead: it returns a fresh NamedVariable with no Storage (and
                                // sets lastOwningResult), so it is not a field read and never lands here.
                                // A cast severs Storage and clears IsUnique on the type, so accept the
                                // carried IsUniqueFieldAlias as an alternative to the direct field read -
                                // `(T*)b.p` must stay a borrow. OwningStructName/FieldName/CallerName
                                // survive the cast, so the diagnostics name the same owner as the un-cast form.
                                if (!srcIsBorrowed
                                    && !rightNV.TypeAndValue.IsMove
                                    && ((rightNV.TypeAndValue.IsUnique
                                            && rightNV.TypeAndValue.Pointer
                                            && rightNV.Storage != nullptr)
                                        || rightNV.IsUniqueFieldAlias))
                                {
                                    srcIsBorrowed = true;
                                    srcBorrowedUniqueField = DescribeUniqueFieldAccess(rightNV);
                                    srcBorrowedOrigin = DescribeUniqueFieldAccess(rightNV);
                                }
                                // Trap B through a CALL: the callee provably returns a live `unique`
                                // field, so the result borrows it exactly as a direct `b.p` read does.
                                if (!srcIsBorrowed && !rightNV.TypeAndValue.IsMove
                                    && rightNV.TypeAndValue.Pointer)
                                {
                                    const auto* fieldBorrow =
                                        compiler->FindUniqueFieldBorrowResult(rightNV.Primary);
                                    if (fieldBorrow != nullptr)
                                    {
                                        srcIsBorrowed = true;
                                        srcBorrowedUniqueField = fieldBorrow->FieldOwner;
                                        srcBorrowedOrigin = fieldBorrow->FieldOwner;
                                        srcBorrowedUniqueFieldViaCall = true;
                                    }
                                }
                                // Trigger transfer only when the RHS is an owning pointer whose
                                // source link was severed (Storage cleared) - typically by a cast.
                                // Direct refs ('T* q = p') keep Storage set and follow today's
                                // alias semantics in ParseDeclaration; we don't disturb them.
                                bool severedOwningSource = rightNV.IsOwning
                                    && rightNV.TypeAndValue.Pointer
                                    && rightNV.Storage == nullptr
                                    && !rightNV.CallerName.empty();
                                // A reinterpreting cast without `move` is a VIEW (borrow), like `T* q = p`.
                                // Exception: a live `new`-allocated local has no other owner, so it keeps transferring.
                                auto srcRef = compiler->FindVariableStorage(rightNV.CallerName);
                                const auto* srcVar = srcRef.Storage != nullptr
                                    ? compiler->FindVariableByStorage(srcRef.Storage) : nullptr;
                                bool srcIsLiveNewLocal = srcVar != nullptr && srcVar->IsNewAllocated;
                                srcIsOwningMove = severedOwningSource
                                    && (rightNV.TypeAndValue.IsMove || !srcIsLiveNewLocal);
                                srcOwningName = rightNV.CallerName;
                                if (severedOwningSource && !srcIsOwningMove)
                                {
                                    srcBorrowsOwningLocal = true;
                                    srcOwningLocalOrigin = rightNV.CallerName;
                                    srcOwningLocalStorage = srcRef.Storage;
                                }
                                // A plain copy of a live OWNING local: the source still frees the
                                // pointee at its own scope exit, so the copy owns nothing. Resolved by
                                // STORAGE IDENTITY (a spelling that erased CallerName still resolves,
                                // and a shadowing name cannot be mistaken for another binding).
                                if (!rightNV.TypeAndValue.IsMove && rightNV.TypeAndValue.Pointer
                                    && llvm::isa_and_nonnull<llvm::AllocaInst>(rightNV.Storage))
                                {
                                    const auto* srcBind = compiler->FindVariableByStorage(rightNV.Storage);
                                    if (srcBind != nullptr && srcBind->IsOwning)
                                    {
                                        srcBorrowsOwningLocal = true;
                                        srcOwningLocalOrigin =
                                            compiler->FindVariableNameByStorage(rightNV.Storage);
                                        srcOwningLocalStorage = rightNV.Storage;
                                    }
                                    // One hop further (`T* d = b;`): carry the origin, unless the
                                    // source was rebound since - then its declaration fact is stale.
                                    else if (srcBind != nullptr && srcBind->BorrowsOwningLocal
                                             && !srcBind->PointerRebound
                                             && !srcBind->OwningLocalOrigin.empty())
                                    {
                                        srcBorrowsOwningLocal = true;
                                        srcOwningLocalOrigin = srcBind->OwningLocalOrigin;
                                        srcOwningLocalStorage = srcBind->OwningLocalStorage;
                                    }
                                }
                                srcInferredTypeName = rightNV.TypeAndValue.TypeName;
                                srcInferredPointer = rightNV.TypeAndValue.Pointer;
                                srcInferredElemPointer = rightNV.TypeAndValue.ElemPointer;
                                srcConstArraySize = rightNV.TypeAndValue.ConstArraySize;
                                srcConstInnerDimensions = rightNV.TypeAndValue.ConstInnerDimensions;
                                srcIsArrayView = rightNV.TypeAndValue.IsArrayView;
                                srcIsSimd = rightNV.TypeAndValue.IsSimd;
                                srcIsArrayShaped = srcConstArraySize > 0 || srcIsArrayView;
                                srcStorage = rightNV.Storage;
                                srcIsViewElem = rightNV.IsViewElement;
                                srcUnionFieldType = rightNV.UnionFieldType;
                                srcBaseType = rightNV.BaseType;
                                srcPrimary = rightNV.Primary;
                                srcIsMove = rightNV.TypeAndValue.IsMove;
                                // Borrow of an element the owning container frees (list<unique X*>
                                // .get). Capture it so the local is tagged for the delete check.
                                bool coreUniqueElementBorrow = srcCoreUniqueElementBorrow
                                    || (rightNV.TypeAndValue.IsAlias
                                        && compiler->IsCoreUniqueType(rightNV.TypeAndValue.TypeName)
                                        && !rightNV.TypeAndValue.IsMove);
                                srcBorrowsOwnedElement = rightNV.TypeAndValue.IsBorrowOfAliasElement
                                    || coreUniqueElementBorrow;
                                srcElementExternallyOwned = rightNV.TypeAndValue.IsBorrowOfAliasElement;
                                srcOwnedElementContainer = rightNV.TypeAndValue.ParentVariableName.empty()
                                    ? rightNV.CallerName : rightNV.TypeAndValue.ParentVariableName;
                                // One hop off a source BINDING that already borrows the element: the
                                // flags above read the ACCESSOR result, so a plain copy carried none.
                                // The CONTAINER still owns whatever the source was rebound to since,
                                // so a rebound/'??='d source is the stale direction and is dropped.
                                if (!srcBorrowsOwnedElement && !rightNV.TypeAndValue.IsMove
                                    && rightNV.TypeAndValue.Pointer
                                    && llvm::isa_and_nonnull<llvm::AllocaInst>(rightNV.Storage))
                                {
                                    const auto* elemBind = compiler->FindVariableByStorage(rightNV.Storage);
                                    if (elemBind != nullptr && elemBind->BorrowsOwnedElement
                                        && !elemBind->IsOwning && !elemBind->PointerRebound
                                        && !elemBind->CoalesceRebound)
                                    {
                                        srcBorrowsOwnedElement = true;
                                        srcElementExternallyOwned = elemBind->BorrowedElementExternallyOwned;
                                        srcOwnedElementContainer = elemBind->OwnedElementContainer;
                                    }
                                }
                                // Alias if the RHS is an `alias` call result OR a read of an
                                // alias-borrow local (chained `Token t2 = t1;`) - both borrow.
                                srcIsAlias = rightNV.TypeAndValue.IsAlias || rightNV.IsAliasBorrow;
                                srcCallerName = rightNV.CallerName;
                                // Taint a string local initialized from an owning string FIELD
                                // (`string t = b.name`) or from another already-tainted borrow
                                // (`string t2 = t`). A `.copy()` / `operator+` result (IsMove)
                                // owns an independent buffer and is NOT a borrow.
                                srcBorrowsOwnedString = NamedVarIsString(rightNV)
                                    && !rightNV.TypeAndValue.IsMove
                                    && ((!rightNV.FieldName.empty() && !rightNV.OwningStructName.empty())
                                        || rightNV.BorrowsOwnedString);
                                srcIsOwningArrayStringElem =
                                    IsOwningArrayStringElementRead(rightNV, right);
                                srcIsRawHeapStringElem =
                                    IsRawHeapStringElementRead(rightNV, right);
                                // Plain pointer copy from an already-tagged whole binding (`T* q = p`)
                                // carries the `new T[n]` provenance across, but a field/element is one slot.
                                const bool rhsIsWholeRawArrayBinding = rightNV.FieldName.empty()
                                    && rightNV.OwningStructName.empty() && !rightNV.IsElementAccess
                                    && (rightNV.Storage == nullptr
                                        || llvm::isa<llvm::AllocaInst>(rightNV.Storage)
                                        || llvm::isa<llvm::GlobalVariable>(rightNV.Storage));
                                srcPtrCopyIsRawNewArray = rhsIsWholeRawArrayBinding
                                    && rightNV.AllocatedByRawNewArray;
                                // Both the `&w` producer and a plain copy of an already-tainted
                                // pointer local carry the fact forward.
                                srcPointsToBorrowedByValueParam =
                                    rightNV.PointsToBorrowedByValueParam;
                                srcRawArrayLength = rhsIsWholeRawArrayBinding
                                    ? compiler->LoadRawArrayLength(rightNV) : nullptr;
                                rhsIsFuncPtr = rightNV.TypeAndValue.IsFunctionPointer;
                                rhsFuncPtrReturnOwned = rightNV.TypeAndValue.FuncPtrReturnOwned;
                                rhsFuncPtrReturnAlias = rightNV.TypeAndValue.FuncPtrReturnAlias;
                                rhsIsThinFnPtr = rightNV.TypeAndValue.IsThinFnPtr();
                                rhsIsPointer = rightNV.TypeAndValue.Pointer;
                                rhsFuncPtrParams = rightNV.TypeAndValue.FuncPtrParams;
                                rhsLambdaCaptureNames = rightNV.LambdaCaptureNames;
                                rhsLambdaReferenceCaptureNames = rightNV.LambdaReferenceCaptureNames;
                                RejectLambdaReferenceCaptureEscape(
                                    isStaticLocal, rightNV, assignmentExpression);
                                // int[] v = rawIntPtr; is the laundering door - reject it.
                                RejectRawPointerToArrayView(assignmentExpression, typeAndValue, rightNV);
                                // int[] v = ptrView; binds a view whose ELEMENT differs - reject.
                                RejectArrayViewElementMismatch(assignmentExpression, typeAndValue, rightNV);
                                // Declarator-init leg of the code-value store gate: `Rec* r = w;`
                                // stored a code address in a data pointer and wrote through it.
                                if (compiler->CodeValueIntoDataDestination(rightNV, typeAndValue))
                                    LogErrorContext(assignmentExpression,
                                        compiler->DescribeCodeValueIntoData(
                                            CodeValueDestSpelling(typeAndValue), "initialize",
                                            CodeValueCastAdvice(typeAndValue)));
                            }
                            lambdaExpectedType = {};
                            // Implicit char* -> string coercion: string s = "hello" or string s = charPtr.
                            if (right && typeAndValue.TypeName == "string" && !typeAndValue.Pointer
                                && right->getType() == cflat_llvm::PointerTo(compiler->builder->getInt8Ty()))
                            {
                                auto* c = llvm::dyn_cast<llvm::Constant>(right);
                                if (c && compiler->IsStringLiteralConstant(c))
                                    right = compiler->WrapStringLiteralAsString(right);
                                else if (compiler->GetFunction("operator string"))
                                {
                                    LLVMBackend::NamedVariable argNV;
                                    argNV.Primary = right;
                                    argNV.BaseType = right->getType();
                                    argNV.TypeAndValue.TypeName = "char";
                                    argNV.TypeAndValue.Pointer = true;
                                    right = compiler->CreateOverloadedFunctionCall("operator string", { argNV });
                                }
                                else
                                    right = compiler->WrapStringLiteralAsString(right);
                            }
                            // A primitive assigned to a string is no longer an implicit conversion
                            // (it mirrors the rejected `(string)primitive` cast). Direct the user to
                            // value.toString() instead of emitting an invalid scalar->struct cast.
                            else if (right && typeAndValue.TypeName == "string" && !typeAndValue.Pointer
                                && (right->getType()->isIntegerTy() || right->getType()->isFloatingPointTy()))
                            {
                                LogErrorContext(assignmentExpression, "cannot assign a primitive value to "
                                    "'string'; call 'value.toString()' instead (string interpolation \"{x}\" "
                                    "and '+' concatenation still convert automatically)");
                            }
                            // Bare 'function' type inference: infer signature from the assigned function value.
                            if (right && typeAndValue.IsFunctionPointer && typeAndValue.FuncPtrReturnTypeName.empty())
                            {
                                std::string funcName = assignmentExpression->getText();
                                auto inferred = compiler->MakeFuncPtrTypeAndValue(funcName);
                                if (inferred.IsFunctionPointer)
                                {
                                    typeAndValue.FuncPtrReturnTypeName = inferred.FuncPtrReturnTypeName;
                                    typeAndValue.FuncPtrReturnPointer = inferred.FuncPtrReturnPointer;
                                    typeAndValue.FuncPtrReturnOwned = inferred.FuncPtrReturnOwned;
                                    typeAndValue.FuncPtrReturnAlias = inferred.FuncPtrReturnAlias;
                                    typeAndValue.FuncPtrParams = inferred.FuncPtrParams;
                                }
                            }
                            // The overload actually BOUND below, kept so the inferred-sink adoption
                            // reads the same one the call will run (never a by-name re-lookup).
                            const llvm::Function* boundNamedFn = nullptr;
                            std::string boundNamedFnName;
                            // Wrap named function in closure fat struct when declaring function<T>.
                            if (right && typeAndValue.IsFunctionPointer && !right->getType()->isStructTy())
                            {
                                // Re-resolve by name to avoid picking a struct method that shares the
                                // same plain key in functionTable (e.g. atomic_counter::add vs add).
                                // Only when the RHS is a bare named function (right is already an
                                // llvm::Function); a same-named local/param of function-pointer type
                                // loads to a Value and must shadow the global function, not be replaced.
                                std::string funcName = assignmentExpression->getText();
                                int expectedParams = (int)typeAndValue.FuncPtrParams.size();
                                if (llvm::isa<llvm::Function>(right))
                                    if (auto* correctFn = compiler->GetFunctionForFuncPtr(funcName, expectedParams, &typeAndValue.FuncPtrParams, &typeAndValue))
                                        right = correctFn;
                                if (auto* fn = llvm::dyn_cast<llvm::Function>(right))
                                {
                                    boundNamedFn = fn;
                                    boundNamedFnName = funcName;
                                    auto boundSig = compiler->FuncPtrSigOfBoundFunction(funcName, fn);
                                    if (boundSig.IsFunctionPointer)
                                        typeAndValue.FuncPtrReturnOwned = boundSig.FuncPtrReturnOwned;
                                        typeAndValue.FuncPtrReturnAlias = boundSig.FuncPtrReturnAlias;
                                    VerifyFuncPtrAssignmentMoveFlags(funcName, typeAndValue, assignmentExpression);
                                    // thin `function<T>`: bare fn ptr; fat `Lambda<T>`: closure fat struct.
                                    right = typeAndValue.IsThinFnPtr()
                                          ? compiler->MakeThinFnPtrValue(fn, typeAndValue)
                                          : compiler->WrapBareValueAsFatStruct(fn);
                                }
                            }
                            // Fallback for generic function pointers (e.g. function<int(void*)> fp = wrap<int>):
                            // the RHS parses to an empty namedVar with CallerName set to the mangled function name.
                            if (!right && typeAndValue.IsFunctionPointer && !genericFuncCallerName.empty())
                            {
                                int expectedParams = (int)typeAndValue.FuncPtrParams.size();
                                llvm::Function* genericFn = compiler->GetFunctionForFuncPtr(genericFuncCallerName, expectedParams, &typeAndValue.FuncPtrParams, &typeAndValue);
                                if (genericFn)
                                {
                                    boundNamedFn = genericFn;
                                    boundNamedFnName = genericFuncCallerName;
                                    auto boundSig = compiler->FuncPtrSigOfBoundFunction(genericFuncCallerName, genericFn);
                                    if (boundSig.IsFunctionPointer)
                                        typeAndValue.FuncPtrReturnOwned = boundSig.FuncPtrReturnOwned;
                                        typeAndValue.FuncPtrReturnAlias = boundSig.FuncPtrReturnAlias;
                                    VerifyFuncPtrAssignmentMoveFlags(genericFuncCallerName, typeAndValue, assignmentExpression);
                                    right = typeAndValue.IsThinFnPtr()
                                          ? compiler->MakeThinFnPtrValue(genericFn, typeAndValue)
                                          : compiler->WrapBareValueAsFatStruct(genericFn);
                                }
                            }
                            // Fat closure -> thin function<T>: allowed only when provably non-capturing
                            // (env is a compile-time null). A capturing lambda literal or a stored Lambda<>
                            // value cannot carry captures across the C ABI, so reject it with guidance
                            // instead of dropping the env (a thin ptr over freed/absent state -> crash).
                            if (right && typeAndValue.IsFunctionPointer && typeAndValue.IsThinFnPtr()
                                && right->getType() == compiler->GetClosureFatPtrType())
                            {
                                compiler->UnregisterOwnedClosureTemp(right);
                                if (compiler->ClosureIsStaticallyNonCapturing(right))
                                    right = compiler->CoerceClosureFatToThin(right, typeAndValue);
                                else
                                {
                                    LogErrorContext(assignmentExpression,
                                        compiler->DescribeCapturingClosureToThin(rhsLambdaCaptureNames));
                                    right = llvm::UndefValue::get(compiler->BuildThinFnPtrType(typeAndValue));
                                }
                            }
                            // Decl-init spelling of the thin gate ('function<T> f = vp;'); rightNV
                            // is out of scope here, so rhsProvenance carries only the two flags read.
                            if (right && typeAndValue.IsFunctionPointer && typeAndValue.IsThinFnPtr()
                                && !typeAndValue.Pointer && right->getType()->isPointerTy())
                            {
                                LLVMBackend::NamedVariable rhsProvenance;
                                rhsProvenance.TypeAndValue.IsFunctionPointer = rhsIsFuncPtr;
                                rhsProvenance.TypeAndValue.Pointer = rhsIsPointer;
                                rhsProvenance.TypeAndValue.TypeName = srcInferredTypeName;
                                compiler->CheckThinFnPtrAssignProvenance(right, rhsProvenance,
                                    std::format("'{}'", name));
                            }
                            // Widen a thin function<T> value to a fat Lambda<T>: store {code, null}, no
                            // thunk. Mirrors the call-arg widen; keeps thin -> Lambda -> toFunction lossless.
                            if (right && typeAndValue.IsFunctionPointer && !typeAndValue.IsThinFnPtr()
                                && rhsIsThinFnPtr && !right->getType()->isStructTy())
                            {
                                right = compiler->WidenThinToFat(right);
                            }

                            // Funcptr-to-funcptr declaration init: per-param IsMove flags must agree.
                            if (right && typeAndValue.IsFunctionPointer && rhsIsFuncPtr
                                && right->getType()->isStructTy()
                                && typeAndValue.FuncPtrParams.size() == rhsFuncPtrParams.size())
                            {
                                for (size_t i = 0; i < typeAndValue.FuncPtrParams.size(); i++)
                                {
                                    if (!compiler->FuncPtrParamMoveAgrees(
                                            typeAndValue.FuncPtrParams[i], rhsFuncPtrParams[i]))
                                    {
                                        LogErrorContext(assignmentExpression, std::format(
                                            "incompatible function pointer initializer: parameter {} differs in 'move' modifier - 'move' is part of the function-pointer type",
                                            i + 1));
                                        break;
                                    }
                                }
                            }
                            // The declared spelling carries no INFERRED sink, so adopt the
                            // initializer's - otherwise the call site never nulls the caller.
                            if (right && typeAndValue.IsFunctionPointer)
                            {
                                if (rhsIsFuncPtr)
                                {
                                    AdoptInferredParamSinks(typeAndValue, rhsFuncPtrParams);
                                    typeAndValue.FuncPtrReturnOwned = rhsFuncPtrReturnOwned;
                                    typeAndValue.FuncPtrReturnAlias = rhsFuncPtrReturnAlias;
                                }
                                else if (boundNamedFn != nullptr)
                                {
                                    // A bare NAMED function initializer carries its facts in the
                                    // function table; read them off the overload actually bound.
                                    auto named = compiler->FuncPtrSigOfBoundFunction(
                                        boundNamedFnName, boundNamedFn);
                                    if (named.IsFunctionPointer)
                                    {
                                        AdoptInferredParamSinks(typeAndValue, named.FuncPtrParams);
                                        typeAndValue.FuncPtrReturnOwned = named.FuncPtrReturnOwned;
                                    }
                                }
                            }

                            // The pointer spelling of a local `unique X*` was rewritten to the
                            // core value `unique<X>`. Adopt a pointer-producing initializer through
                            // its move constructor; nullptr is the value's default construction.
                            const bool coreUniqueIfaceArm =
                                compiler->IsCoreUniqueType(typeAndValue.TypeName)
                                && compiler->IsInterfaceType(MangledGenericArgument(
                                    *compiler, typeAndValue.TypeName));
                            if (right && compiler->IsCoreUniqueType(typeAndValue.TypeName)
                                && !typeAndValue.Pointer && !typeAndValue.IsArrayView
                                && (right->getType()->isPointerTy()
                                    || (coreUniqueIfaceArm
                                        && right->getType() == compiler->GetFatPtrType())))
                            {
                                auto movedArmOwns = [&](auto&& self, antlr4::tree::ParseTree* node) -> bool {
                                    if (node == nullptr) return false;
                                    std::string text = node->getText();
                                    size_t movedText = text.find("move");
                                    size_t colonText = movedText == std::string::npos
                                        ? std::string::npos : text.find(':', movedText);
                                    if (movedText != std::string::npos && colonText != std::string::npos)
                                    {
                                        std::string sourceName = text.substr(movedText + 4,
                                            colonText - movedText - 4);
                                        if (auto* source = compiler->FindLiveNamedVariable(sourceName))
                                            if (source->IsOwning
                                                || source->TypeAndValue.IsUnique
                                                || compiler->IsCoreUniqueType(source->TypeAndValue.TypeName)
                                                || compiler->IsVariableOwning(sourceName))
                                                return true;
                                    }
                                    if (auto* move = dynamic_cast<CFlatParser::MoveExpressionContext*>(node))
                                    {
                                        std::string sourceName = move->unaryExpression() != nullptr
                                            ? move->unaryExpression()->getText() : std::string();
                                        if (auto* source = compiler->FindLiveNamedVariable(sourceName))
                                            if (source->IsOwning
                                                || source->TypeAndValue.IsUnique
                                                || compiler->IsCoreUniqueType(source->TypeAndValue.TypeName)
                                                || compiler->IsVariableOwning(sourceName))
                                                return true;
                                    }
                                    for (auto* child : node->children)
                                        if (self(self, child)) return true;
                                    return false;
                                };
                                bool initializerIsBorrowed = initializerSourceNV.IsBorrowed
                                    || initializerSourceNV.IsAliasBorrow
                                    || initializerSourceNV.TypeAndValue.IsAlias
                                    || initializerSourceNV.BorrowsOwnedElement
                                    || initializerSourceNV.BorrowsOwningLocal;
                                bool initializerOwns = compiler->lastOwningResult
                                    || compiler->lastCallReturnsOwned
                                    || initializerSourceNV.IsOwning
                                    || initializerSourceNV.IsOwningStruct
                                    || initializerSourceNV.TypeAndValue.IsMove
                                    || initializerSourceNV.IsExplicitMove
                                    || compiler->IsOwnedNewTemp(right)
                                    || compiler->IsOwningPtrTempValue(right)
                                    || compiler->IsMovedOutPtrValue(right)
                                    || movedArmOwns(movedArmOwns, initializer)
                                    || (initializer != nullptr
                                        && initializer->getText().find("move") != std::string::npos
                                        && compiler->IsMovedOutPtrValue(right));
                                initializerIsBorrowed = initializerIsBorrowed || !initializerOwns;
                                // A raw pointer binding can retain its original borrow bit after
                                // an owning rebind. The move dataflow ledger is authoritative for
                                // that case, so do not make the core constructor reject a fresh owner.
                                if (initializerIsBorrowed && initializerOwns
                                    && !initializerSourceNV.CallerName.empty()
                                    && (compiler->IsVariableOwning(initializerSourceNV.CallerName)
                                        || compiler->BorrowProofRetiredByRebind(initializerSourceNV)))
                                    initializerIsBorrowed = false;
                                bool initializerIsNull = llvm::isa<llvm::ConstantPointerNull>(right)
                                    || IsAllNullPhi(right);
                                if (initializerIsBorrowed && !initializerIsNull
                                    && !genericCoreUniqueTypeArg)
                                {
                                    const std::string sourceName =
                                        LLVMBackend::BorrowedSourceName(initializerSourceNV);
                                    if (sourceName.empty())
                                        LogErrorContext(initDecl, std::format(
                                            "cannot initialize {} '{}' from a borrowed value - the source still owns it; "
                                            "use 'new', a 'move' expression, or a move-returning call",
                                            SpellType(*compiler, typeAndValue), name));
                                    else
                                        LogErrorContext(initDecl, std::format(
                                            "cannot initialize {} '{}' from borrowed value '{}' - the source still owns it; "
                                            "use 'new', a 'move' expression, or a move-returning call",
                                            SpellType(*compiler, typeAndValue), name,
                                            sourceName));
                                    right = GenerateDefaultValue(typeAndValue);
                                }
                                if (initializerIsNull)
                                    right = GenerateDefaultValue(typeAndValue);
                                else
                                {
                                    // A '?:' / '??' join carries no TypeName: box each arm into the
                                    // interface here, so the ctor sees one owning fat value.
                                    if (coreUniqueIfaceArm && right->getType()->isPointerTy()
                                        && initializerSourceNV.TypeAndValue.TypeName.empty())
                                    {
                                        std::string armFailure;
                                        std::string joinSpelling = "?:";
                                        std::string iface = MangledGenericArgument(
                                            *compiler, typeAndValue.TypeName);
                                        if (auto* fat = UpcastPointerJoinToInterface(
                                                right, iface, &armFailure, &joinSpelling, true))
                                            right = fat;
                                        else if (!armFailure.empty())
                                            LogErrorContext(assignmentExpression, std::format(
                                                "cannot convert '{}' arm to interface '{}': {}",
                                                joinSpelling, iface, armFailure));
                                    }
                                    LLVMBackend::NamedVariable ctorArg;
                                    ctorArg.Primary = right;
                                    ctorArg.BaseType = right->getType();
                                    ctorArg.Storage = initializerSourceNV.Storage;
                                    ctorArg.CallerName = initializerSourceNV.CallerName;
                                    ctorArg.FieldName = initializerSourceNV.FieldName;
                                    ctorArg.OwningStructName = initializerSourceNV.OwningStructName;
                                    bool movedContainerSlot = compiler->lastMovedFromContainerSlot;
                                    ctorArg.IsExplicitMove = initializerSourceNV.IsExplicitMove;
                                    ctorArg.TypeAndValue.IsMove = initializerSourceNV.TypeAndValue.IsMove
                                        || movedContainerSlot;
                                    ctorArg.IsBorrowed = movedContainerSlot
                                        ? false : initializerIsBorrowed;
                                    ctorArg.BorrowedOrigin = movedContainerSlot
                                        ? std::string() : initializerSourceNV.BorrowedOrigin;
                                    ctorArg.IsOwning = initializerSourceNV.IsOwning
                                        || movedContainerSlot
                                        || (initializerOwns && !initializerIsBorrowed);
                                    ctorArg.IsOwningStruct = initializerSourceNV.IsOwningStruct;
                                    ctorArg.AllocAlignment = initializerSourceNV.AllocAlignment;
                                    ctorArg.TypeAndValue.AllocAlignValue =
                                        initializerSourceNV.TypeAndValue.AllocAlignValue;
                                    // A moved container slot can retain the slot's pointer depth
                                    // witness even though its loaded LLVM value is one raw T*.
                                    // The unique constructor consumes that actual value, so present
                                    // its canonical pointee shape to overload resolution.
                                    ctorArg.TypeAndValue.TypeName =
                                        initializerSourceNV.TypeAndValue.TypeName;
                                    ShapeCoreUniqueCtorArg(compiler, ctorArg,
                                                           typeAndValue.TypeName, right);
                                    right = compiler->CreateOverloadedFunctionCall(
                                        typeAndValue.TypeName, { ctorArg }, true);
                                    compiler->lastOwningResult = false;
                                    compiler->lastCallReturnsOwned = false;
                                }
                            }

                            // A string LITERAL is a 'const char*', never a 'T*': catch it here or
                            // the slot points at character data and the first member access faults.
                            if (RejectStringLiteralIntoStructPointer(
                                    assignmentExpression, typeAndValue, right,
                                    std::format("variable '{}'", name)))
                                right = nullptr;

                            // Pointer variable assigned a struct value: catch the mismatch here
                            // with a clear message rather than letting LLVM assert inside CreateCast.
                            if (right && typeAndValue.Pointer && !typeAndValue.IsFunctionPointer
                                && right->getType()->isStructTy())
                            {
                                LogErrorContext(assignmentExpression, std::format(
                                    "cannot initialize pointer '{}' with a value of type '{}' - the right-hand side must be a pointer (call getPtr() or use '&')",
                                    name, SpellType(*compiler, typeAndValue)));
                                right = nullptr;
                            }

                            // Value-typed local assigned a pointer (`Foo f = new Foo();`): catch the
                            // mismatch here rather than emitting an invalid pointer->struct bitcast.
                            // A fixed-array destination with an ARRAY-SHAPED source
                            // (`Foo[3] b = a;`) is exempt - its RHS is a decayed array pointer,
                            // handled by EmitFixedArrayValueCopy below. `Foo[3] b = new Foo();`
                            // is NOT exempt: the RHS is a scalar `Foo*` and this message is the
                            // accurate one for it.
                            if (right && !typeAndValue.Pointer && !typeAndValue.IsFunctionPointer
                                && !(typeAndValue.ConstArraySize > 0 && srcIsArrayShaped)
                                && right->getType()->isPointerTy()
                                && compiler->GetDataStructure(typeAndValue.TypeName).StructType != nullptr)
                            {
                                LogErrorContext(assignmentExpression, std::format(
                                    "cannot initialize value of type '{}' with a pointer of type '{}*'; "
                                    "declare it as '{}* {} = ...;' or drop the 'new'",
                                    SpellType(*compiler, typeAndValue), SpellType(*compiler, typeAndValue),
                                    SpellType(*compiler, typeAndValue), name));
                                right = nullptr;
                            }

                        }
                    }
                    else if (initializer->Default() != nullptr)
                    {
                        // `S[N] a = default;` must default-CONSTRUCT each element:
                        // GenerateDefaultValue hands back a zeroinitializer for the whole ARRAY
                        // type, skipping every field initializer the element declares.
                        bool fixedArrayOfStruct = !global_scope && !typeAndValue.Pointer
                            && typeAndValue.ConstArraySize > 0
                            && compiler->GetDataStructure(typeAndValue.TypeName).StructType != nullptr;
                        if (fixedArrayOfStruct)
                        {
                            // Preferred: fold one element's construction and replicate it as a
                            // CONSTANT array, keeping the single whole-array store this spelling
                            // has always emitted. The null-interface dataflow proof reads that
                            // stored aggregate, so a per-element memcpy would hide a null element.
                            auto elemTypeAndValue = typeAndValue;
                            elemTypeAndValue.ConstArraySize = 0;
                            elemTypeAndValue.ConstInnerDimensions.clear();
                            if (auto* elemConst = TryFoldGlobalDefaultConstruction(elemTypeAndValue))
                                right = SplatConstantOverFixedArray(
                                    elemConst, compiler->GetType(typeAndValue));
                            // A construction that does not fold - an allocation is never constant,
                            // so an owned resource per slot lands here - falls through to seeding.
                        }
                        else
                        {
                            right = GenerateDefaultValue(typeAndValue);
                        }
                    }
                    else if (initializer->LeftBrace() != nullptr)
                    {
                        // Field initializer: MyStruct s = { field=val, ... }, and the EMPTY form
                        // 'T x = {}'. Gated on the brace TOKEN, not on initializerList(): the list
                        // rule requires >= 1 element, so '{}' yields a null list and this arm used
                        // to match nothing, leaving 'right' null and the target never written
                        // ('int x = {}' read undef). The bare-brace arm below has always gated on
                        // the token, which is why 'T x {}' seeded correctly.
                        right = GenerateDefaultValue(typeAndValue);
                    }
                }
                else if (barebraceInit)
                {
                    // Bare-brace scalar form ('S ls {1,2};' / 'S ls {a=1,b=2};', no '='). A
                    // fixed-array or array-view target already consumed barebraceInit above and
                    // `continue`d before reaching here, so this is the scalar struct/union/class
                    // case - the exact counterpart of the 'initializerList() != nullptr' arm just
                    // above, reached through 'initDecl' instead of 'initializer' because the
                    // brace list has no '=' in front of it. Route it the same way so the
                    // EmitFieldInitializer / global-guard code below (which falls back to
                    // barebraceList when 'initializer' is null) sees a non-null 'right' instead of
                    // discarding the brace values as a bare declaration with no initializer would
                    // (silently at local scope; with a non-blocking "not initialized on the stack"
                    // note but still no real diagnostic at global scope).
                    right = GenerateDefaultValue(typeAndValue);
                }

                if (globalInitTempFn != nullptr)
                {
                    // Check that the initializer reduced to a compile-time constant.
                    if (right != nullptr && llvm::dyn_cast_or_null<llvm::Constant>(right) == nullptr)
                    {
                        LogErrorContext(initializer->assignmentExpression(),
                            "global variable initializer must be a compile-time constant");
                        right = nullptr;
                    }
                    // Discard the temp function (and all IR emitted into it) then restore the builder.
                    globalInitTempFn->eraseFromParent();
                    globalInitTempFn = nullptr;
                    compiler->RestoreBuilderState(globalInitSavedState);
                }

                if (right == nullptr && typeAndValue.IsNullable)
                {
                    // Nullable pointer defaults to null when no initializer is provided.
                    llvm::Type* ptrTy = compiler->GetType(typeAndValue);
                    right = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy));
                }

                // An `extern` global with no initializer is a declaration of a symbol defined
                // elsewhere (another TU or a C library); skip default-init / the not-initialized
                // warning - there is nothing to construct here.
                bool externDeclOnly = global_scope && typeAndValue.external && right == nullptr;

                // `T arr[N];` with no initializer: every element gets the same default construction
                // the scalar branch below performs. Emitted after the alloca exists, since the
                // default is written through the array's slots rather than into `right`.
                bool needsArrayDefaultInit = right == nullptr && !typeAndValue.Pointer
                    && typeAndValue.ConstArraySize > 0 && !externDeclOnly && !global_scope
                    && compiler->GetDataStructure(typeAndValue.TypeName).StructType != nullptr;

                if (right == nullptr && !typeAndValue.Pointer && typeAndValue.ConstArraySize == 0 && !externDeclOnly)
                {
                    auto structData = compiler->GetDataStructure(typeAndValue.TypeName);
                    if (structData.StructType != nullptr)
                    {
                        // Auto-initialize using the default constructor when available. A global
                        // takes the constant value of that same construction when it has one; the
                        // note below is reserved for the cases that genuinely stay zeroed.
                        if (!global_scope && compiler->GetFunction(typeAndValue.TypeName))
                            right = compiler->CreateOverloadedFunctionCall(typeAndValue.TypeName, {});
                        else if (global_scope)
                        {
                            right = TryFoldGlobalDefaultConstruction(typeAndValue);
                            if (right == nullptr)
                            {
                                // A defined outer constructor can still call a field constructor
                                // emitted later, so defer every candidate until the module walk ends.
                                pendingGlobalDefaultConstruction = true;
                            }
                        }
                        else
                            LogWarningContext(direct, std::format(
                                "({}) struct and class is not initialized on the stack.",
                                SpellType(*compiler, typeAndValue)));
                    }
                }
                coreUniqueImplicitDefault = !global_scope && initializer == nullptr && !barebraceInit
                    && !typeAndValue.Pointer && typeAndValue.TypeName.size() > 8
                    && compiler->IsCoreUniqueType(typeAndValue.TypeName)
                    && compiler->IsInterfaceType(MangledGenericArgument(
                        *compiler, typeAndValue.TypeName));

                /*
                 * `auto x = <fixed array>` deduces the ARRAY VIEW `T[]`, i.e. a borrow. CFlat is
                 * borrow-by-default and `auto` introduces no new storage, so it must not copy.
                 * Without this the binding kept TypeName "auto" with no shape at all: the local
                 * was never materialised, `x[i]` did not index, and every downstream guard that
                 * decides on the array shape (interface boxing among them) was handed a source
                 * that no longer says "array". A multi-dimensional source is left alone - its
                 * decayed element is a row, not a `T`, so `T[]` would be the wrong deduction.
                 * File scope is left alone too: `auto` is rejected there for every other source
                 * ("unknown type 'auto'"), and this is not the change that lifts that.
                 */
                /*
                 * A MULTI-DIMENSIONAL fixed array (`T[N][M]`) has no view to deduce to either:
                 * its decayed element is a ROW, so `T[]` is the wrong deduction, and the `T[][]`
                 * spelling is rejected outright (a thin view carries no row stride). Reject
                 * rather than leave the shapeless binding that indexed nothing and read garbage.
                 */
                if (typeAndValue.TypeName == "auto" && right != nullptr && !global_scope
                    && !typeAndValue.Pointer && !typeAndValue.IsSimd
                    && typeAndValue.ConstArraySize == 0
                    && srcConstArraySize > 0 && !srcConstInnerDimensions.empty()
                    && !srcInferredPointer && !srcIsArrayView && !srcIsSimd
                    && !srcInferredTypeName.empty() && srcInferredTypeName != "auto"
                    && right->getType()->isPointerTy())
                {
                    std::string srcDims = std::format("[{}]", srcConstArraySize);
                    for (uint64_t d : srcConstInnerDimensions) srcDims += std::format("[{}]", d);
                    std::string srcPath = DescribeInitializerPath(srcCallerName, srcBorrowedField);
                    const std::string srcTypeSpelling = SpellType(*compiler,
                        LLVMBackend::TypeAndValue{ .TypeName = srcInferredTypeName });
                    // The row-bind remedy only reaches a `T[]` when the source is exactly 2-D;
                    // a row of a 3-D array is itself multi-dimensional and has no view either.
                    std::string rowRemedy = srcConstInnerDimensions.size() == 1
                        ? std::format("bind one row ('{}[] row = {}[i];'), or ",
                                      srcTypeSpelling, srcPath)
                        : "or ";
                    LogErrorContext(direct, std::format(
                        "cannot deduce 'auto' from multi-dimensional fixed array '{0}{1}' - a "
                        "multi-dimensional array has no array-view type (a thin view carries no "
                        "row stride). Copy it ('{0}{1} b = {2};'), {3}index the array directly.",
                        srcTypeSpelling, srcDims, srcPath, rowRemedy));
                }

                if (typeAndValue.TypeName == "auto" && right != nullptr && !global_scope
                    && !typeAndValue.Pointer && !typeAndValue.IsSimd
                    && typeAndValue.ConstArraySize == 0
                    && srcConstArraySize > 0 && srcConstInnerDimensions.empty()
                    && !srcIsArrayView && !srcIsSimd
                    && !srcInferredTypeName.empty() && srcInferredTypeName != "auto"
                    && right->getType()->isPointerTy())
                {
                    typeAndValue.TypeName = srcInferredTypeName;
                    typeAndValue.ElemPointer = srcInferredPointer || srcInferredElemPointer;
                    typeAndValue.Pointer = true;
                    typeAndValue.IsArrayView = true;
                    typeAndValue.IsInterface = compiler->IsInterfaceType(typeAndValue.TypeName);
                }

                // Resolve an 'auto' declaration to the initializer's concrete type so the
                // variable carries a real TypeName (drives typeof/nameof reflection and the
                // LSP). Guarded by an LLVM-type equality check: only adopt the inferred name
                // when the name-based type matches the value's actual type, leaving simd /
                // function-pointer / array-view / generic 'auto' vars (which infer their
                // LLVM type directly from the value) untouched.
                if (typeAndValue.TypeName == "auto" && right != nullptr
                    && !srcInferredTypeName.empty() && srcInferredTypeName != "auto")
                {
                    LLVMBackend::TypeAndValue probe = typeAndValue;
                    probe.TypeName = srcInferredTypeName;
                    probe.Pointer = srcInferredPointer;
                    probe.ElemPointer = srcInferredElemPointer;
                    if (compiler->GetType(probe) == right->getType())
                    {
                        typeAndValue.TypeName = srcInferredTypeName;
                        typeAndValue.Pointer = srcInferredPointer;
                        typeAndValue.ElemPointer = srcInferredElemPointer;
                    }
                }

                if (global_scope)
                {
                    // Same-scope (file-scope) redeclaration of a global is rejected (C semantics),
                    // mirroring the local check. An `extern` declaration may legitimately precede a
                    // definition (or repeat), so only flag when a prior real DEFINITION already exists
                    // and this declaration is not itself an extern declaration. A re-registration at
                    // the SAME source line is benign (a core file analyzed as a root - via a temp-copy
                    // path, as the LSP does - is also pulled in via the auto-import graph, so its
                    // globals register twice from the same line); only the same name at a DIFFERENT
                    // line is a true in-source redeclaration.
                    if (!externDeclOnly)
                    {
                        int curLine = (int)direct->getStart()->getLine();
                        auto prevSite = compiler->globalDeclSite.find(name);
                        bool sameLine = prevSite != compiler->globalDeclSite.end()
                            && prevSite->second == curLine;
                        auto existingGlobal = compiler->globalNamedVariable.find(name);
                        if (!sameLine
                            && existingGlobal != compiler->globalNamedVariable.end()
                            && existingGlobal->second != nullptr
                            && !existingGlobal->second->isDeclaration())
                            LogErrorContext(direct, std::format(
                                "redeclaration of global '{}' in the same scope; use a different name or assign to the existing variable", name));
                        compiler->globalDeclSite[name] = curLine;
                    }
                    auto constant = llvm::dyn_cast_or_null<llvm::Constant>(right);

                    /*
                     * A scalar struct/union/class global with a non-empty brace-list initializer
                     * ('{1,2}' or '{a=1,b=2}') has no path to a real Constant here: the fixed-array
                     * and array-view brace forms are handled above and `continue` before reaching
                     * this block. Left alone, `right` is just the zero default value computed
                     * earlier, so the global silently lands as a zeroinitializer with every brace
                     * value discarded - including for generic containers (list/array/dictionary),
                     * which desugar to add()/set() CALLS at local scope (TryEmitContainerInitializer)
                     * that this Constant-only global path cannot emit. Reject every case rather than
                     * silently losing the values; only the WORDING differs by shape. Route positional
                     * struct/union/class lists to the same diagnostic the LOCAL declarator gives
                     * (EmitFieldInitializer); the named form ('{field = value}') DOES work locally but
                     * not here, so it gets its own honest message. Containers get a third message,
                     * since for THEM the positional form is exactly what local supports and
                     * 'field = value' would be a false remedy.
                     *
                     * isContainerType is a mangled-name-prefix test (matches
                     * TryEmitContainerInitializer's own discriminator) used ONLY to pick which
                     * message is the least misleading; it can never flip accept vs. reject; a type
                     * whose prefix this misses still hits the struct/union/class branch below (any
                     * false positive would need a mangled name accidentally starting with
                     * "list$"/"array$"/"dictionary$", which the mangler does not produce for
                     * anything else) and gets the (slightly less precise but still true) struct
                     * wording instead.
                     *
                     * The brace list may arrive with no '=' in front of it ('S gs {1,2};' -
                     * barebraceList, captured above) instead of through 'initializer'; both spell
                     * the identical construct, so both must reach this guard or the bare form
                     * would silently zero exactly like the filed bug, one character away.
                     */
                    auto* scalarInitList = (initializer != nullptr) ? initializer->initializerList() : barebraceList;
                    if (!externDeclOnly && scalarInitList != nullptr)
                    {
                        auto scalarElements = scalarInitList->fieldInit();
                        const std::string& scalarTypeName = typeAndValue.TypeName;
                        bool isContainerType = MangledBase(scalarTypeName) == "list"
                            || MangledBase(scalarTypeName) == "array"
                            || MangledBase(scalarTypeName) == "dictionary";
                        bool scalarIsAggregate =
                            compiler->GetDataStructure(scalarTypeName).StructType != nullptr;
                        if (!scalarElements.empty() && scalarIsAggregate)
                        {
                            bool hasPositional = false;
                            for (auto* fi : scalarElements)
                                if (fi->Identifier() == nullptr) { hasPositional = true; break; }
                            // A pointer declarator ('S* gp = {a=1};') hits this same guard - GetDataStructure
                            // looks up by name, not by pointer-ness - so say "pointer to struct" instead of
                            // "struct type" for one; the message would otherwise misdescribe the declaration.
                            const char* kindWord = typeAndValue.Pointer ? "pointer to struct type" : "struct type";
                            const std::string scalarTypeSpelling = SpellType(*compiler, typeAndValue);
                            if (isContainerType)
                                LogErrorContext(direct, std::format(
                                    "brace initializers are not supported for '{}' at global scope - "
                                    "it requires runtime construction (add()/set() calls) that a "
                                    "global's compile-time constant cannot hold; declare it '= default' "
                                    "and populate it in a function", scalarTypeSpelling));
                            else if (hasPositional)
                                LogErrorContext(direct, std::format(
                                    "positional initializers are not supported for {} '{}'; use 'field = value'",
                                    kindWord, scalarTypeSpelling));
                            else
                                LogErrorContext(direct, std::format(
                                    "field initializers ('{{field = value}}') are not supported for {} '{}' "
                                    "at global scope; assign fields individually after declaration, or use "
                                    "'= default' to zero-initialize", kindWord, scalarTypeSpelling));
                        }
                        else if (!scalarElements.empty() && !isContainerType)
                        {
                            /*
                             * The type is neither an aggregate nor a container, so the branch above
                             * never fired and the list was silently DISCARDED: `right` stays the
                             * type's zero default and the global lands as a zeroinitializer. That
                             * covers INTERFACE-typed globals (`I gi = { a = 1 };` - the fat value's
                             * boxing machinery is never reached, since a brace list is not an
                             * assignmentExpression, and an interface is not in the StructData table
                             * at all), and equally primitives, `char*`, `function<>` and `simd`.
                             * The LOCAL declarator already rejects every one of these with this
                             * message, so the two scopes now agree. Fixed arrays and array views
                             * never reach here - both `continue` out earlier.
                             */
                            LogNonAggregateBraceInitReject(direct, name, scalarTypeName);
                        }
                    }

                    /*
                     * Global counterpart of the local `T[N] b = <pointer>` reject below. A global's
                     * initializer is a Constant, so the bad `ptr` -> `[N x T]` conversion is never
                     * cast: it lands in the module and fails verification with no source location.
                     * Brace lists and `= default` produce an aggregate Constant and are unaffected,
                     * and `= nullptr` is coerced to a zeroinitializer by CreateGlobalVariable.
                     */
                    bool initIsNull = constant != nullptr && constant->isNullValue();
                    if (!externDeclOnly && typeAndValue.ConstArraySize > 0
                        && !typeAndValue.IsArrayView && !initIsNull
                        && right != nullptr && right->getType()->isPointerTy()
                        && llvm::isa_and_nonnull<llvm::ArrayType>(compiler->GetType(typeAndValue)))
                    {
                        // `embed("...")` folds to a private constant [N x i8] global; a global
                        // destination of the same shape adopts those bytes directly.
                        auto* embedGlobal = constant == nullptr ? nullptr
                            : llvm::dyn_cast<llvm::GlobalVariable>(constant->stripPointerCasts());
                        auto* embedBytes = (embedGlobal == nullptr || !embedGlobal->isConstant()
                                            || !embedGlobal->hasInitializer())
                            ? nullptr
                            : llvm::dyn_cast<llvm::ConstantDataArray>(embedGlobal->getInitializer());
                        if (embedBytes != nullptr
                            && embedBytes->getType() == compiler->GetType(typeAndValue)
                            && DirectEmbedByteCount(initializer == nullptr ? nullptr
                                                    : initializer->assignmentExpression()).has_value())
                        {
                            constant = embedBytes;
                        }
                        else
                        {
                        bool fromStringLiteral = constant != nullptr
                            && compiler->IsStringLiteralConstant(constant);
                        if (fromStringLiteral)
                        {
                            auto* literalGlobal = llvm::dyn_cast<llvm::GlobalVariable>(
                                constant->stripPointerCasts());
                            auto* literalBytes = literalGlobal == nullptr ? nullptr
                                : llvm::dyn_cast<llvm::ConstantDataSequential>(literalGlobal->getInitializer());
                            auto* arrayTy = llvm::cast<llvm::ArrayType>(compiler->GetType(typeAndValue));
                            if (literalBytes == nullptr || !arrayTy->getElementType()->isIntegerTy(8))
                                LogErrorContext(direct, std::format(
                                    "cannot initialize global fixed array '{} {}' from this string literal",
                                    DescribeArrayShape(typeAndValue), name));
                            std::string bytes = literalBytes->getAsString().str();
                            size_t visible = bytes.empty() ? 0 : bytes.size() - 1;
                            if (visible > typeAndValue.ConstArraySize)
                                LogErrorContext(direct, std::format(
                                    "too many initializers for '{}[{}]': string literal has {} characters",
                                    SpellType(*compiler, typeAndValue), typeAndValue.ConstArraySize, visible));
                            std::vector<llvm::Constant*> elements;
                            elements.reserve((size_t)typeAndValue.ConstArraySize);
                            for (size_t i = 0; i < typeAndValue.ConstArraySize; ++i)
                            {
                                uint8_t ch = i < bytes.size() ? (uint8_t)bytes[i] : 0;
                                elements.push_back(llvm::ConstantInt::get(arrayTy->getElementType(), ch));
                            }
                            constant = llvm::ConstantArray::get(arrayTy, elements);
                        }
                        else
                            LogErrorContext(direct, std::format(
                                "cannot initialize global fixed array '{} {}' from this expression - "
                                "a global's initializer must be a compile-time constant of the array's "
                                "own shape. Use a brace list ('{{...}}'), '= default' to zero it, or "
                                "fill '{}' element by element at the start of main.",
                                DescribeArrayShape(typeAndValue), name, name));
                        }
                    }

                    auto* global = compiler->CreateGlobalVariable(typeAndValue, constant,
                        typeAndValue.threadLocal, typeAndValue.UserAlignValue, externDeclOnly,
                        srcIsUnsigned);
                    if (pendingGlobalDefaultConstruction && !externDeclOnly)
                        pendingGlobalDefaultConstructions_.push_back({global, typeAndValue, direct});
                    if (!externDeclOnly && DeclSpecHasConst(declSpec)
                        && typeAndValue.TypeName == "string" && initializer != nullptr
                        && initializer->assignmentExpression() != nullptr)
                    {
                        std::string raw = initializer->assignmentExpression()->getText();
                        if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"'
                            && !HasInterpolation(raw))
                            compiler->RecordCompileTimeStringConstant(name, ProcessRawText(raw));
                    }
                    // A const-qualified global scalar with a constant-int initializer is foldable
                    // in an `if const` condition; a mutable global is not (see constFoldableGlobals_).
                    if (!externDeclOnly && DeclSpecHasConst(declSpec)
                        && llvm::isa_and_nonnull<llvm::ConstantInt>(constant))
                        constFoldableGlobals_.insert(typeAndValue.VariableName);
                    // Record the variable's declaration site so LSP go-to-definition
                    // on the variable name lands on the global itself, not on its
                    // (possibly unregistered, e.g. generic-instantiation) type.
                    if (auto* s = compiler->GetSymbolSink())
                    {
                        s->RegisterVariable(name, typeAndValue.TypeName,
                                            compiler->GetSourceFilePath(),
                                            (int)direct->getStart()->getLine(),
                                            (int)direct->getStart()->getCharPositionInLine(),
                                            SpellType(*compiler, typeAndValue));
                        // Also register as a named symbol so globals (including
                        // namespace consts like Math.PI) show up in --symbol
                        // queries and in their namespace's member listing.
                        s->Register(SymbolKind::Variable, name,
                                    compiler->GetSourceFilePath(),
                                    (int)direct->getStart()->getLine(),
                                    (int)direct->getStart()->getCharPositionInLine(),
                                    std::format("{} {}", SpellType(*compiler, typeAndValue), name),
                                    {}, name);
                    }
                }
                else
                {
                    if (typeAndValue.threadLocal)
                        LogErrorContext(direct, "thread_local is only allowed on global variables.");
                    // Same-scope redeclaration is rejected (C semantics). Silently overwriting the
                    // scope-map entry would drop the shadowed local's scope-exit destructor and leak
                    // an owning value (string/list/closure). Shadowing in a NESTED block is fine - that
                    // is a separate scope frame; parameters live in a separate map (functionArgument),
                    // so a local re-using a parameter name is not flagged here.
                    if (compiler->stackNamedVariable.back().namedVariable.count(name))
                        LogErrorContext(direct, std::format(
                            "redeclaration of '{}' in the same scope; use a different name or assign to the existing variable", name));
                    auto alloc = compiler->CreateLocalVariable(typeAndValue, right ? right->getType() : nullptr, arraySize, line, typeAndValue.UserAlignValue);
                    allocList.push_back(std::pair(name, llvm::dyn_cast<llvm::AllocaInst>(alloc)));
                    if (!typeAndValue.Pointer && compiler->IsCoreUniqueType(typeAndValue.TypeName))
                        compiler->GetOrCreateStackVariable(name).IsOwning = true;
                    if (typeAndValue.IsFunctionPointer)
                    {
                        auto& local = compiler->GetOrCreateStackVariable(name);
                        local.LambdaCaptureNames = rhsLambdaCaptureNames;
                        local.LambdaReferenceCaptureNames = rhsLambdaReferenceCaptureNames;
                    }

                    /*
                     * A `static` local outlives the frame, so a pointer / array view it holds must
                     * not address a non-static local. Only the DIRECTLY enumerable shapes are
                     * rejected (the initializer value roots at an alloca): `&local`, `&local.f`,
                     * `&local[i]`, and a view of a local fixed array. A copy destination
                     * (`static T[N] c = src;`) is unaffected - it owns its own storage.
                     * Best-effort by construction: an address that reaches the static through a
                     * parameter, through `this`, or as a call result roots at a load or a call,
                     * not at an alloca, and is NOT detected. Closing that needs escape analysis.
                     */
                    if (isStaticLocal && right != nullptr && typeAndValue.Pointer
                        && typeAndValue.ConstArraySize == 0 && PointsIntoStackFrame(right))
                        LogErrorContext(direct, std::format(
                            "a 'static' local may not hold the address of a non-static local - it "
                            "outlives the frame, so '{}' would dangle once the first call returns. "
                            "Point it at a global, at another 'static' local, or at a heap "
                            "allocation ('new').", name));

                    // A view bound directly from a fixed array's decayed storage (ConstArraySize
                    // proves it - 'new T[n]' and another 'T[]' both carry IsArrayView instead)
                    // came from stack/global storage, not the heap; tag it so 'delete' can reject
                    // the free() of a non-heap address. Every other RHS shape (parameter, field,
                    // call result, conditional join) leaves the flag at its default false.
                    if (typeAndValue.IsArrayView)
                    {
                        bool boundFromFixedArray = srcConstArraySize > 0 && !srcIsArrayView;
                        // Name the source for the diagnostic: 'callerName[.field]' for a local
                        // or field read. A plain (or field-of-global) GLOBAL read never sets
                        // CallerName (GetGlobalVariableNV doesn't), so fall back to the RHS's own
                        // AST text - never the LLVM symbol name: a name collision with a runtime
                        // declaration (e.g. a global 'read' vs. the libc '@read' the runtime
                        // imports) gets a '.N' uniquifying suffix that does not exist in source.
                        std::string srcDesc = boundFromFixedArray
                            ? (!srcCallerName.empty()
                                ? DescribeInitializerPath(srcCallerName, srcBorrowedField)
                                : srcRhsExprText)
                            : std::string();
                        compiler->SetViewOfFixedArrayStorage(name, boundFromFixedArray, srcDesc);
                        auto& viewNV = compiler->GetOrCreateStackVariable(name);
                        if (boundFromFixedArray)
                            compiler->StoreRawArrayLength(
                                viewNV, compiler->builder->getInt64(srcConstArraySize));
                        else if (srcIsArrayView || compiler->IsRawArrayResult(srcPrimary))
                            compiler->StoreRawArrayLength(viewNV, srcRawArrayLength);
                        viewNV.OwningStructName = srcFieldPathNV.OwningStructName;
                        viewNV.FieldName = srcFieldPathNV.FieldName;
                        viewNV.FieldPathText = srcFieldPathNV.FieldPathText;
                        viewNV.FieldPathRoot = srcFieldPathNV.FieldPathRoot;
                        viewNV.RootIsBorrowedByValueParam =
                            srcFieldPathNV.RootIsBorrowedByValueParam;
                        viewNV.RootIsAliasBorrowLocal = srcFieldPathNV.RootIsAliasBorrowLocal;
                    }

                    // Declaration leg of the borrowed-box tag: `right` is the fat value this local
                    // is about to receive. See TagInterfaceBoxProvenance.
                    if (typeAndValue.IsFatInterfaceValue())
                    {
                        // Initializing from an interface local that ADOPTED its box transfers the
                        // box, so exactly one of the two destructs it (mirrors 'string b = a').
                        bool adoptsSourceBox = haveInterfaceSourceNV
                            && interfaceSourceNV.OwnsInterfaceBox
                            && !interfaceSourceNV.CallerName.empty()
                            && interfaceSourceNV.FieldName.empty()
                            && !typeAndValue.Pointer
                            && !typeAndValue.IsUnique
                            && !typeAndValue.IsAlias;
                        if (adoptsSourceBox)
                        {
                            compiler->SetVariableOwning(interfaceSourceNV.CallerName, false);
                            compiler->SetVariableOwnsInterfaceBox(interfaceSourceNV.CallerName, false);
                        }
                        if (haveInterfaceSourceNV)
                        {
                            auto& destNV = compiler->GetOrCreateStackVariable(name);
                            destNV.IsAdoptable = interfaceSourceNV.IsAdoptable;
                            if (adoptsSourceBox)
                            {
                                destNV.IsOwning = true;
                                destNV.OwnsInterfaceBox = true;
                            }
                        }
                        TagInterfaceBoxProvenance(
                            name, right, haveInterfaceSourceNV ? &interfaceSourceNV : nullptr);
                    }

                    if (bindAliasReference)
                    {
                            auto& nv = compiler->GetOrCreateStackVariable(name);
                        nv.Storage = aliasStorage;
                        nv.BaseType = compiler->GetType(typeAndValue);
                        nv.IsAliasBorrow = true;
                        nv.IsOwning = false;
                        nv.IsOwningString = false;
                        RecordAliasBorrowDeclBlock(compiler, nv);
                    }
                    else if (needsArrayDefaultInit)
                        EmitFixedArrayDefaultInit(alloc, typeAndValue);

                    /*
                     * An owning pointer local with no initializer left its slot uninitialized, so a
                     * later assignment's drop-old and the scope-exit release both freed garbage.
                     * Zero it, exactly as the `= nullptr` spelling does; the array type's own zero
                     * value covers every slot of `unique T*[N]` and of `unique I[N]`. The fat
                     * interface value `unique I` has the same defect and the same remedy - its zero
                     * is a null { vtable, data } pair, and the store carries the decl-splat witness
                     * tag so the never-initialised interface diagnostic still sees no initializer.
                     * A `static` local's storage is a module global zeroed once at definition and
                     * must not be re-zeroed per call, and a run-time-sized allocation has no
                     * constant whole-slot zero.
                     */
                    bool coreUniqueValue = !typeAndValue.Pointer
                        && compiler->IsCoreUniqueType(typeAndValue.TypeName);
                    bool coreUniqueInterface = coreUniqueValue
                        && typeAndValue.TypeName.size() > 8
                        && compiler->IsInterfaceType(MangledGenericArgument(
                            *compiler, typeAndValue.TypeName));
                    if (right == nullptr
                        && (typeAndValue.IsUnique || coreUniqueValue)
                        && !isStaticLocal
                        && (typeAndValue.Pointer || typeAndValue.IsFatInterfaceValue()
                            || coreUniqueInterface)
                        && !needsArrayDefaultInit
                        && (arraySize == nullptr || typeAndValue.ConstArraySize > 0))
                    {
                        if (auto* slot = llvm::dyn_cast_or_null<llvm::AllocaInst>(alloc))
                        {
                            auto* splat = compiler->builder->CreateStore(
                                llvm::Constant::getNullValue(slot->getAllocatedType()), slot);
                            // Witness tag: the null-interface proofs must not read this
                            // compiler-emitted splat as an initialization. See StoreWritesInterfaceLoc.
                            splat->setMetadata(LLVMBackend::kIfaceDeclSplatMD,
                                               llvm::MDNode::get(*compiler->context, {}));
                        }
                    }

                    // Record an unused-local candidate. RAII locals (a type with a
                    // destructor, e.g. `lock`) are exempt: the declaration itself is the
                    // point even when the name is never read again.
                    if (auto* s = compiler->GetSymbolSink(); s && !InGenericInstantiation())
                    {
                        UnusedCandidate cand;
                        cand.name = name;
                        cand.kind = SymbolKind::Variable;
                        cand.file = compiler->GetSourceFilePath();
                        cand.line = (int)direct->getStart()->getLine();
                        cand.col  = (int)direct->getStart()->getCharPositionInLine();
                        cand.hasDestructor = compiler->TypeHasDestructor(typeAndValue.TypeName);
                        s->RegisterCandidate(cand);
                    }

                    if (right != nullptr && !bindAliasReference)
                    {
                        // Consume the container-element-slot move signal HERE, at the point common to
                        // both the interface and non-interface init branches (each parsed the RHS in
                        // its own branch), and BEFORE CreateAssignment could parse a nested element
                        // move (brace init). ParseAssignmentExpressionNamed reset it at RHS entry, so
                        // it reflects only THIS initializer's top-level `move <element slot>`.
                        srcMovedFromSlot = compiler->lastMovedFromContainerSlot;
                        compiler->lastMovedFromContainerSlot = false;

                        // simd<T,N> from a scalar splats across all lanes - but only when the declared
                        // SLOT lowers to a vector (`simd<T,N>*` and `[N]` carry IsSimd and do not).
                        if (typeAndValue.IsSimd && !right->getType()->isVectorTy()
                            && llvm::isa_and_nonnull<llvm::FixedVectorType>(compiler->GetType(typeAndValue)))
                            right = compiler->SplatToSimd(right, typeAndValue, srcIsUnsigned);

                        /*
                         * Closure decl-init from a FIELD / ELEMENT (Option A clone). The
                         * whole-variable block below only accepts an alloca/global source, so
                         * `Lambda<...> h = t.get;` (Storage is a GEP, or the base pointer when the
                         * closure is field 0) fell through and shallow-stored the source's TAGGED
                         * OWNED env: h's scope-exit dtor and the struct's field dtor then free the
                         * same block - double free. Clone instead, so each side owns an independent
                         * env; cloning a borrowed/null env is a no-op, and a `move` source or a
                         * temp/call result (null Storage) already transferred ownership.
                         */
                        if (right->getType()->isStructTy()
                            && srcStorage != nullptr
                            && !llvm::isa<llvm::AllocaInst>(srcStorage)
                            && !llvm::isa<llvm::GlobalVariable>(srcStorage)
                            && !srcIsMove
                            && srcInferredTypeName == "__closure_fat_ptr"
                            && typeAndValue.TypeName == "__closure_fat_ptr")
                        {
                            LLVMBackend::NamedVariable srcNV;
                            srcNV.BaseType = compiler->GetClosureFatPtrType();
                            if (srcUnionFieldType != nullptr)
                                srcNV.Primary = compiler->CreateLoad(srcNV.BaseType, srcStorage);
                            else
                                srcNV.Storage = srcStorage;
                            srcNV.TypeAndValue.TypeName = "__closure_fat_ptr";
                            if (auto* cloned = compiler->CreateOverloadedFunctionCall("copy", { srcNV }))
                                right = cloned;
                        }

                        // Owning-value MOVE at decl-init (string-redesign FINAL MODEL): `OwningType
                        // b = a;` where a is a plain named local/global of a value type that owns
                        // resources (has a full destructor). A shallow struct store aliases a's
                        // buffers into b, double-freeing at teardown; instead MOVE - the shallow
                        // store below transfers a's bits, and we mark a moved so its scope-exit
                        // destructor is suppressed and any later use of a is rejected. `b = a.copy()`
                        // is the explicit way to duplicate. Excludes move/temporary sources (a move
                        // already transfers; a call result / .copy() owns an independent buffer with
                        // null srcStorage). An INDIRECT lvalue source (field / deref / fixed-array
                        // element) takes the same decision - see srcIsIndirectOwningLvalue below.
                        bool didDeepCopyBorrowString = false;
                        auto* srcInitGep = llvm::dyn_cast_or_null<llvm::GetElementPtrInst>(srcStorage);
                        bool srcIsNamedSlot = srcStorage != nullptr
                            && (llvm::isa<llvm::AllocaInst>(srcStorage)
                                || llvm::isa<llvm::GlobalVariable>(srcStorage))
                            && !srcCallerName.empty();
                        // Excluded: a SINGLE-index GEP (container/view slot - `T tmp = _data[i]` is
                        // the borrow list sort shuffles), and string/closure (own paths run below).
                        // A `T[]` VIEW element (single-index GEP + srcIsViewElem) joins the set:
                        // its slot is LIVE storage the new local does not own, exactly like a
                        // fixed-array element. A container's raw `T*` slot carries no such
                        // provenance and keeps its borrow (the plan's LOAD-BEARING INVARIANT).
                        bool srcIsIndirectOwningLvalue = srcStorage != nullptr && !srcIsNamedSlot
                            && typeAndValue.TypeName != "__closure_fat_ptr"
                            && typeAndValue.TypeName != "string"
                            && (srcInitGep == nullptr
                                || (srcInitGep->getNumIndices() == 2
                                    && (srcInitGep->getSourceElementType()->isStructTy()
                                        || srcInitGep->getSourceElementType()->isArrayTy()))
                                || (srcInitGep->getNumIndices() == 1 && srcIsViewElem)
                                // A raw `new T[n]` heap element is also a ONE-index GEP, so it is
                                // admitted on the same alloca-backed-local provenance the string
                                // arms use - never on the GEP shape, which container internals share.
                                || (srcInitGep->getNumIndices() == 1
                                    && RawHeapBaseIsNewArrayLocal(srcInitGep->getPointerOperand())));
                        if (right->getType()->isStructTy()
                            && (srcIsNamedSlot || srcIsIndirectOwningLvalue)
                            && !srcIsMove
                            && !srcIsAlias
                            && srcInferredTypeName == typeAndValue.TypeName
                            && compiler->IsOwningValueType(typeAndValue.TypeName))
                        {
                            if (typeAndValue.TypeName == "__closure_fat_ptr")
                            {
                                // Closures (Option A) are CLONE-by-default, not move: cloning a
                                // borrowed/null env is a no-op and cloning an owned env yields an
                                // independent block, so `b = a` deep-copies the env and leaves a
                                // valid (no use-after-move). `right` is replaced with the clone so
                                // the CreateAssignment below stores an independent env into b.
                                LLVMBackend::NamedVariable srcNV;
                                srcNV.BaseType = compiler->GetClosureFatPtrType();
                                if (srcUnionFieldType != nullptr)
                                    srcNV.Primary = compiler->CreateLoad(srcNV.BaseType, srcStorage);
                                else
                                    srcNV.Storage = srcStorage;
                                srcNV.TypeAndValue.TypeName = "__closure_fat_ptr";
                                if (auto* cloned = compiler->CreateOverloadedFunctionCall("copy", { srcNV }))
                                    right = cloned;
                            }
                            else
                            {
                                // THE FLIP via the shared decision: a copyable owner COPIES (source
                                // stays LIVE), a non-copyable owner MOVES. A Move here transfers a's
                                // loaded {fields} into b (the CreateAssignment below), so ZERO a's
                                // storage - a's always-run scope-exit destructor then no-ops on the
                                // moved-out value (cflat value-type move = zero source, not skip dtor)
                                // and a is marked moved for the use-after-move diagnostic.
                                AssignSourceKind kind;
                                right = ClassifyOwningAssignSource(right, typeAndValue.TypeName, false, direct, kind);
                                if (kind == AssignSourceKind::Move && haveInitializerSourceNV
                                    && RejectImplicitConsumeOfOutlivingOwner(
                                        compiler, initializerSourceNV, false, direct))
                                    return allocList;
                                // Bails like the five sibling sites rather than relying on
                                // LogErrorContext's throw to skip the consume below.
                                bool rejectedBorrowedField = kind == AssignSourceKind::Move
                                    && RejectConsumeOfBorrowedByValueParamField(
                                        compiler, srcFieldPathNV, direct);
                                if (kind == AssignSourceKind::Move && !rejectedBorrowedField)
                                {
                                    compiler->builder->CreateStore(
                                        llvm::ConstantAggregateZero::get(right->getType()), srcStorage);
                                    if (compiler->IsCoreUniqueType(srcInferredTypeName)
                                        && !srcCallerName.empty())
                                        compiler->RecordNullSet(srcCallerName);
                                    // Only a named slot has a spelling to report a later use of; an
                                    // indirect lvalue is consumed silently, as `move w.b` already is.
                                    if (srcIsNamedSlot)
                                        compiler->MarkVariableMoved(srcCallerName);
                                }
                                // `string` frees its local via the IsOwningString gate; a copied or
                                // moved-in local owns its buffer, so mark it owning.
                                if (typeAndValue.TypeName == "string")
                                    compiler->GetOrCreateStackVariable(name).IsOwningString = true;
                            }
                        }
                        // String BORROW bound to an owning local via a DIRECT read of an owning string
                        // FIELD the local does not own: `string x = obj.name`. srcBorrowsOwnedString
                        // flags this statically (FieldName + OwningStructName known). The struct keeps
                        // sole ownership, so ALWAYS deep-copy - x must hold an independent owned buffer
                        // or it dangles once obj frees/reassigns the field (setter store, `delete obj`).
                        // x is flagged owning so the borrow taint below is suppressed and x is treated
                        // as a true owner (its unconditional owned-bit-gated dtor frees the copy).
                        //
                        // A plain function/method RESULT (`string x = obj.label()`) is deliberately NOT
                        // handled here: the runtime OWNED bit means only "a heap buffer exists", not
                        // "ownership transfers to you", so it cannot distinguish a returned field borrow
                        // from a returned owned temp. Binding a borrowed return to a plain owning local
                        // is an inherent ambiguity - the caller must use `.copy()` for an independent
                        // string (exactly what the owning-string-into-field diagnostics recommend).
                        else if (typeAndValue.TypeName == "string"
                            && !typeAndValue.Pointer
                            && right->getType()->isStructTy()
                            && initializer != nullptr
                            && initializer->assignmentExpression() != nullptr
                            && !srcIsMove
                            && !srcIsAlias
                            && !srcMovableTempField
                            && srcBorrowsOwnedString)
                        {
                            right = compiler->EmitOwnedStringDeepCopy(right);
                            compiler->GetOrCreateStackVariable(name).IsOwningString = true;
                            didDeepCopyBorrowString = true;
                        }
                        // The ELEMENT twin of the field arm above (`string q = dst[0];`). A fixed-array or view
                        // `string` element has no FieldName, so srcBorrowsOwnedString is false and the
                        // plain store bit-copied the {ptr,len,owned} pair: the local and the element both
                        // owned - and freed - the same buffer (rc 133). Deep-copy so the local owns an
                        // independent one, exactly as a field source is deep-copied. Both halves are
                        // gated on the REPRESENTATION: the `%string` named type on the destination
                        // ALLOCA and on the source value, never on a spelled TypeName.
                        else if (right->getType()->isStructTy()
                            && initializer != nullptr
                            && initializer->assignmentExpression() != nullptr
                            && !srcIsMove
                            && !srcIsAlias
                            && !srcMovableTempField
                            && srcIsOwningArrayStringElem
                            && !typeAndValue.Pointer
                            && compiler->GetTypeFromStorage(alloc)
                                == llvm::StructType::getTypeByName(*compiler->context, "string"))
                        {
                            right = compiler->EmitOwnedStringDeepCopy(right);
                            compiler->GetOrCreateStackVariable(name).IsOwningString = true;
                        }
                        // Raw `new string[n]` twin of the arm above: the read BORROWS, so clear the
                        // owned bit. Same whole-`%string`-local destination gate as that arm.
                        else if (right->getType()->isStructTy()
                            && srcIsRawHeapStringElem
                            && !srcIsMove
                            && !typeAndValue.Pointer
                            && compiler->GetTypeFromStorage(alloc)
                                == llvm::StructType::getTypeByName(*compiler->context, "string"))
                        {
                            right = compiler->ClearStringOwnedBit(right);
                        }

                        /*
                         * `T[N] b = a;` (fixed array from an ARRAY-SHAPED source) is a COPY: the
                         * declared type allocates its own storage, so it cannot alias `a`. The RHS
                         * has already decayed to a pointer to its first element, so the plain
                         * store below would cast that pointer to the `[N x T]` aggregate - the
                         * invalid bitcast this branch exists to replace. `= nullptr` zeroes the
                         * storage; any other pointer-shaped source is rejected by the last arm.
                         */
                        auto* destArrTy = llvm::dyn_cast_or_null<llvm::ArrayType>(
                            compiler->GetTypeFromStorage(alloc));
                        bool destIsFixedArray = destArrTy != nullptr
                            && typeAndValue.ConstArraySize > 0
                            && right->getType()->isPointerTy();
                        auto* rightConst = llvm::dyn_cast<llvm::Constant>(right);
                        if (destIsFixedArray && rightConst != nullptr && rightConst->isNullValue())
                        {
                            // `T[N] b = nullptr;` zeroes the storage. Emit the zeroinitializer
                            // directly rather than letting a null pointer fold through a cast.
                            compiler->builder->CreateStore(
                                llvm::Constant::getNullValue(destArrTy), alloc);
                        }
                        else if (destIsFixedArray && rightConst != nullptr
                            && compiler->IsStringLiteralConstant(rightConst))
                        {
                            // `char[N] b = "text";`: lower the literal's GLOBAL byte initializer
                            // into the destination aggregate. A pointer-to-array cast reads pointer
                            // bits and is invalid IR; this is the same zero-fill + byte-store result
                            // as a positional brace initializer, folded into one constant store.
                            auto* literalGlobal = llvm::dyn_cast<llvm::GlobalVariable>(
                                rightConst->stripPointerCasts());
                            auto* literalBytes = literalGlobal == nullptr ? nullptr
                                : llvm::dyn_cast<llvm::ConstantDataSequential>(literalGlobal->getInitializer());
                            if (literalBytes == nullptr || !destArrTy->getElementType()->isIntegerTy(8))
                            {
                                LogErrorContext(direct, std::format(
                                    "cannot initialize fixed array '{}' from this string literal",
                                    DescribeArrayShape(typeAndValue)));
                            }
                            std::string bytes = literalBytes->getAsString().str();
                            // The pooled global includes its terminating NUL. C permits an exact
                            // fit without it, but not fewer slots than the visible bytes.
                            size_t visible = bytes.empty() ? 0 : bytes.size() - 1;
                            if (visible > typeAndValue.ConstArraySize)
                                LogErrorContext(direct, std::format(
                                    "too many initializers for '{}[{}]': string literal has {} characters",
                                    SpellType(*compiler, typeAndValue), typeAndValue.ConstArraySize, visible));
                            std::vector<llvm::Constant*> elements;
                            elements.reserve((size_t)typeAndValue.ConstArraySize);
                            for (size_t i = 0; i < typeAndValue.ConstArraySize; ++i)
                            {
                                uint8_t ch = i < bytes.size() ? (uint8_t)bytes[i] : 0;
                                elements.push_back(llvm::ConstantInt::get(
                                    destArrTy->getElementType(), ch));
                            }
                            compiler->builder->CreateStore(
                                llvm::ConstantArray::get(destArrTy, elements), alloc);
                        }
                        else if (destIsFixedArray && srcIsArrayShaped)
                        {
                            EmitFixedArrayValueCopy(direct, typeAndValue, alloc, destArrTy, right,
                                                    srcInferredTypeName, srcInferredPointer,
                                                    srcInferredElemPointer, srcConstArraySize,
                                                    srcConstInnerDimensions);
                        }
                        else if (destIsFixedArray)
                        {
                            /*
                             * Pointer-shaped initializer that is neither a string literal, a null,
                             * nor an array-shaped source: a plain `T*` expression, or a '?:' / '??'
                             * join. Neither carries an array length for EmitFixedArrayValueCopy to
                             * size a copy from, and the store below would cast it to '[N x T]'.
                             */
                            std::string srcText = initializer != nullptr ? initializer->getText()
                                                                         : std::string();
                            bool isJoin = srcText.find('?') != std::string::npos;
                            LogErrorContext(direct, std::format(
                                "cannot initialize fixed array '{}' from a pointer-valued expression "
                                "- a pointer carries no array length, so there is nothing to size a "
                                "copy from.{} Declare '{}' as a pointer or an array view ('T[]') to "
                                "borrow the source instead of copying it, or copy the elements into "
                                "'{}' individually.",
                                DescribeArrayShape(typeAndValue),
                                isJoin ? " A '?:' or '" "?" "?" "' join yields a pointer for this reason too."
                                       : "",
                                name, name));
                        }
                        else if (!bindAliasReference)
                        {
                            auto* initStore = compiler->CreateAssignment(right, alloc, srcIsUnsigned);
                            if (coreUniqueImplicitDefault && initStore != nullptr)
                                initStore->setMetadata(
                                    LLVMBackend::kIfaceDeclSplatMD,
                                    llvm::MDNode::get(*compiler->context, {}));
                        }

                        // Implied move of a `move`-temp's owning field: the local now owns the buffer, so
                        // zero the source in the temp and mark a string local owning (not a borrow).
                        if (srcMovableTempField)
                        {
                            auto* srcGep = compiler->builder->CreateStructGEP(
                                srcMoveTempStructType, srcMoveTempStructAlloca, srcMoveTempFieldIndex, "movedfld");
                            compiler->builder->CreateStore(
                                llvm::ConstantAggregateZero::get(right->getType()), srcGep);
                            if (typeAndValue.TypeName == "string")
                                compiler->GetOrCreateStackVariable(name).IsOwningString = true;
                        }

                        // A closure local now owns its env (its scope-exit dtor frees it), so drop
                        // the value from the owned-closure temp-flush list to avoid a double-free
                        // (covers a lambda literal RHS, a clone, or a call result uniformly).
                        if (typeAndValue.TypeName == "__closure_fat_ptr")
                            compiler->UnregisterOwnedClosureTemp(right);

                        // `alias` (borrow) initializer: the local shallow-aliases storage the source
                        // still owns. Suppress its scope-exit destructor so it does not double-free,
                        // and leave it non-owning. A persist of this local is rejected at the store
                        // sites (see the FromOwningTempField / IsAliasBorrow escape checks). Excludes
                        // `string`/`__closure_fat_ptr` - their runtime owned bit already makes a borrow
                        // local's destructor a no-op, so the `alias` machinery is for owning STRUCTS.
                        if (srcIsAlias && compiler->IsOwningValueType(typeAndValue.TypeName)
                            && typeAndValue.TypeName != "string"
                            && typeAndValue.TypeName != "__closure_fat_ptr")
                        {
                            auto& nv = compiler->GetOrCreateStackVariable(name);
                            nv.IsAliasBorrow = true;
                            // The source may carry the owner's field path through an alias-return
                            // call. This local is a shallow copy with its own borrow identity, so
                            // downstream field access must resolve the root against this binding.
                            nv.FieldPathRoot.clear();
                            nv.RootIsBorrowedByValueParam = false;
                            nv.RootIsAliasBorrowLocal = false;
                            nv.IsOwningString = false;
                            nv.IsOwning = false;
                            RecordAliasBorrowDeclBlock(compiler, nv);
                        }

                        /*
                         * MIXED '?:' join of an owning-value STRUCT (`Box k = c ? makeBox() : borrowed;`).
                         * The phi is a pure SSA value - no Storage, no CallerName - so the move-vs-copy
                         * decision above never sees it and the shallow CreateAssignment just ran. `k` is a
                         * fresh local of an owning-value type, so its scope-exit full destructor would
                         * DELETE whatever the phi selected, including a borrow arm's live pointee its real
                         * owner deletes again. Which arm ran is not knowable, so borrow (suppress the
                         * destructor) exactly as the `alias` case above does - the untaken owning arm
                         * leaks, which is the trade already shipped for mixed pointer/interface joins.
                         */
                        if (!typeAndValue.Pointer && right->getType()->isStructTy()
                            && compiler->IsNonOwningStructJoin(right)
                            && compiler->IsOwningValueType(typeAndValue.TypeName))
                        {
                            auto& nv = compiler->GetOrCreateStackVariable(name);
                            nv.IsAliasBorrow = true;
                            nv.IsOwningString = false;
                            nv.IsOwning = false;
                            RecordAliasBorrowDeclBlock(compiler, nv);
                        }

                        // Apply field initializer overrides after the default value is stored.
                        // A brace list targeting a generic container (list/array/dictionary) is
                        // desugared into add/init+set/set calls; otherwise it is a struct field-init.
                        // The bare-brace spelling ('S ls {a=1};', no '=') carries its list on
                        // barebraceList instead of 'initializer', which is null for that form.
                        auto* localInitList = (initializer != nullptr) ? initializer->initializerList() : barebraceList;
                        if (localInitList != nullptr)
                        {
                            bool localIsContainer = MangledBase(typeAndValue.TypeName) == "list"
                                || MangledBase(typeAndValue.TypeName) == "array"
                                || MangledBase(typeAndValue.TypeName) == "dictionary";
                            if (!localIsContainer && compiler->GetDataStructure(typeAndValue.TypeName).StructType == nullptr)
                            {
                                // A brace list with values on a non-struct, non-container declaration
                                // ('int x {5};' / 'int x = {5};' / 'bool b {true};') - EmitFieldInitializer's
                                // own message ("'int' is not a known struct type") describes a struct-
                                // field-init failure, not what the user wrote here, so name the real
                                // construct instead. Brace-VALUE-init on a primitive is not implemented
                                // (a language feature, not this fix's job); reject rather than read
                                // whatever 'right' happened to be seeded to. A POINTER-typed declaration
                                // ('S* p {a=1};') does NOT reach this branch: TypeName is the pointee
                                // ('S'), which IS a known struct, so it takes the pointer reject below.
                                LogNonAggregateBraceInitReject(direct, name, typeAndValue.TypeName);
                            }
                            else if (typeAndValue.Pointer)
                            {
                                // 'alloc' is the POINTER VARIABLE's own 8 bytes, so both paths below
                                // write through it. Fixed arrays / 'T[]' views 'continue' above, not here.
                                LogPointerBraceInitReject(direct, std::format("declaration '{}'", name),
                                    typeAndValue.TypeName, DescribePointerDeclType(typeAndValue),
                                    CanSuggestAllocation(direct, typeAndValue), typeAndValue.IsUnique);
                            }
                            else if (!TryEmitContainerInitializer(alloc, typeAndValue, localInitList))
                                EmitFieldInitializer(alloc, typeAndValue.TypeName, localInitList);
                        }

                        // Taint a string local that borrows an owning string field, so storing it
                        // into another field is rejected as a laundered field-to-field copy. An implied
                        // move (above) makes the local a true owner, not a borrow, so skip the taint.
                        if (typeAndValue.TypeName == "string" && srcBorrowsOwnedString
                            && !srcMovableTempField && !didDeepCopyBorrowString)
                            compiler->SetVariableBorrowsOwnedString(name, true);

                        // Propagate ownership: if the RHS was a heap-allocating string call,
                        // mark this local as owning so the destructor frees the buffer on scope exit.
                        if (typeAndValue.TypeName == "string" && compiler->lastCallReturnsOwned)
                        {
                            auto& nv = compiler->GetOrCreateStackVariable(name);
                            nv.IsOwningString = true;
                            // The named local now owns this buffer and frees it on scope
                            // exit; drop it from the unnamed-temporary cleanup list so it
                            // is not also freed at end-of-expression (double free).
                            compiler->UnregisterOwnedStringTemp(right);
                            compiler->lastCallReturnsOwned = false;
                        }

                        // Record `new T[n]` provenance on a raw `T*` local: a DIRECT new-array
                        // initializer, or a plain copy of an already-tagged pointer local. A `T[]`
                        // view bound from a counted `move T*` CALL result records it too, so the
                        // count that crossed the return drives its scope-exit / delete path.
                        auto* initNewArr = initializer && initializer->assignmentExpression()
                            ? AsDirectNew(initializer->assignmentExpression()) : nullptr;
                        const bool viewFromCountedCall = typeAndValue.IsArrayView
                            && initNewArr == nullptr && srcPtrCopyIsRawNewArray
                            && srcRawArrayLength != nullptr;
                        if (typeAndValue.Pointer && (!typeAndValue.IsArrayView || viewFromCountedCall))
                        {
                            bool isRawArray = (initNewArr != nullptr
                                && initNewArr->assignmentExpression() != nullptr)
                                || srcPtrCopyIsRawNewArray;
                            auto& local = compiler->GetOrCreateStackVariable(name);
                            local.AllocatedByRawNewArray = isRawArray;
                            compiler->StoreRawArrayLength(
                                local, isRawArray ? srcRawArrayLength : nullptr);
                            // `Wrap* p = &w;` carries the by-value parameter's borrow contract onto
                            // `p`, so a later `p.b` still roots at the parameter.
                            local.PointsToBorrowedByValueParam = srcPointsToBorrowedByValueParam;
                        }


                        // Propagate pointer ownership: if the RHS was a move-returning pointer call,
                        // mark this local as owning so it is freed on scope exit.
                        if (typeAndValue.Pointer && compiler->lastCallReturnsOwned)
                        {
                            compiler->GetOrCreateStackVariable(name).IsOwning = true;
                            compiler->lastCallReturnsOwned = false;
                        }

                        // Same for a `move <interface>`-returning call: the fat-ptr local owns the
                        // boxed heap object. Keep the existing ownership flag propagation for unique
                        // locals and mark only a direct plain-local move result for auto cleanup.
                        bool interfaceMoveResult = !typeAndValue.Pointer
                            && compiler->lastCallReturnsOwned
                            && compiler->IsInterfaceType(typeAndValue.TypeName);
                        bool plainInterfaceMoveResult = interfaceMoveResult
                            && !typeAndValue.IsUnique
                            && compiler->lastCallReturnType.IsMove
                            && !srcRhsExprText.empty()
                            // The RHS must BE a call; lastCallReturnType.IsMove above already
                            // proves it returned an owned box, so a dotted callee (this._card())
                            // or a '.' in an argument/lambda body must not disqualify it.
                            && srcRhsExprText.find('(') != std::string::npos
                            && !InGenericInstantiation();
                        if (interfaceMoveResult)
                        {
                            auto& local = compiler->GetOrCreateStackVariable(name);
                            local.IsOwning = true;
                            local.OwnsInterfaceBox = plainInterfaceMoveResult;
                            compiler->lastCallReturnsOwned = false;
                        }

                        /*
                         * Propagate new-allocation: mark local as owning its heap pointer.
                         * IsNewAllocated is set alongside IsOwning to enable refcount-on-field-escape
                         * (move params have IsOwning but not IsNewAllocated).
                         * lastOwningResult is a sticky per-expression channel that a `new` ANYWHERE in
                         * the initializer sets, including in a call's ARGUMENT list, so
                         * `T* r = borrows(p, new T());` adopted the call's BORROW return and freed a
                         * pointee its real owner freed again. For a POINTER destination the channel is
                         * therefore only trusted when it agrees with the initializer's RESULT VALUE:
                         * the initializer is a direct `new` / top-level `move` (whose result is the
                         * owned thing by construction), or the result value is itself ledgered owned
                         * (an owning `new` or move-returning call that reached the result through a
                         * transparent wrapper such as a '?:' arm, or a pointer a `move` detached).
                         * Non-pointer destinations (owning struct, string, interface fat ptr) run their
                         * own release paths and keep the channel's plain reading.
                         */
                        auto* initAssignExprOwn = initializer ? initializer->assignmentExpression() : nullptr;
                        bool initResultOwns = compiler->lastOwningResult
                            && (!typeAndValue.Pointer
                                || (initAssignExprOwn != nullptr
                                    && (AsDirectNew(initAssignExprOwn) != nullptr
                                        || TopLevelMoveExpression(initAssignExprOwn) != nullptr))
                                        || compiler->IsOwnedNewTemp(srcPrimary)
                                        || compiler->RawArrayResultOwns(srcPrimary)
                                        || compiler->IsOwningPtrTempValue(srcPrimary)
                                || compiler->IsMovedOutPtrValue(srcPrimary));
                        /*
                         * `move` of a BORROWED pointer transfers nothing - the real owner still frees
                         * the pointee, so a destination that ADOPTS here frees it twice. A `unique`
                         * destination is rejected below; a PLAIN `T*` destination carries no ownership
                         * assertion at all, so it simply must not adopt - it stays the borrow it was,
                         * and the srcIsBorrowed clause further down tags it so a later `delete` is
                         * rejected exactly as `T* d = p; delete d;` already is. An element-SLOT move
                         * (`move n->values[i]`) nulls the one real slot even through a borrowed base,
                         * so it does transfer; ownership is re-derived below.
                         */
                        bool destUniqueLoc = (typeAndValue.IsUnique
                                && ((typeAndValue.Pointer && !typeAndValue.ElemPointer)
                                    || typeAndValue.IsInterface))
                            || (!genericCoreUniqueTypeArg && !typeAndValue.Pointer
                                && compiler->IsCoreUniqueType(typeAndValue.TypeName));
                        bool borrowMoveKeepsBorrow = srcIsBorrowed && !srcMovedFromSlot
                            && typeAndValue.Pointer && !typeAndValue.ElemPointer
                            && !typeAndValue.IsUnique && !destUniqueLoc;
                        if (initResultOwns)
                        {
                            // ParseMoveExpression sets lastOwningResult unconditionally and records the
                            // true provenance in IsBorrowed, so consult that; the `=` path has the same gate.
                            if (srcIsBorrowed && !srcMovedFromSlot && destUniqueLoc)
                            {
                                bool srcIsField = !srcBorrowedField.empty();
                                std::string srcDesc = srcIsField
                                    ? std::format("{}.{}", srcCallerName, srcBorrowedField)
                                    : srcBorrowedOrigin;
                                RejectBorrowIntoUniqueLocal(srcDesc, BorrowedOriginRoot(srcBorrowedOrigin),
                                                            srcIsField, name, true, initDecl,
                                                            srcBorrowedUniqueFieldViaCall
                                                                ? srcBorrowedUniqueField : std::string());
                            }
                            if (!borrowMoveKeepsBorrow)
                            {
                                auto& nv = compiler->GetOrCreateStackVariable(name);
                                nv.IsOwning = true;
                                nv.IsNewAllocated = true;
                                // Carry per-site over-alignment so scope-exit / delete free via __delete_aligned.
                                nv.AllocAlignment = haveInitializerSourceNV
                                    && initializerSourceNV.AllocAlignment != 0
                                    ? initializerSourceNV.AllocAlignment : compiler->lastAllocAlignment;
                            }
                            compiler->lastOwningResult = false;
                            compiler->lastAllocAlignment = 0;
                        }
                        // Borrow provenance for the DECLARATION door: `T* p = &x;` binds an
                        // address-of result, which never owns. Carried on the binding so a later
                        // '?:' arm can be PROVEN a borrow instead of merely unproven.
                        // A plain COPY of a binding that already proved an address-of origin
                        // carries the proof; a `move` source sets IsOwning above and does not.
                        if (typeAndValue.Pointer && haveInitializerSourceNV
                            && (compiler->IsBorrowedAddressValue(initializerSourceNV.Primary)
                                || initializerSourceNV.PointsToBorrowedAddress))
                            compiler->GetOrCreateStackVariable(name).PointsToBorrowedAddress = true;
                        // A channel the value-identity gate rejected is stale (a `new` from an argument
                        // list); retire it here so no later declaration or return reads it as owned.
                        compiler->lastOwningResult = false;
                        compiler->lastAllocAlignment = 0;

                        // Move OUT of a container element slot (`T tmp = move _data[i]`): the pointer/
                        // interface element read demoted `unique`-ness away (a slot read hands out a
                        // borrow), so the ownership channels above cannot tell an owning element from a
                        // borrowed one - they mark EVERY moved pointer local owning. Re-derive the
                        // dropped local's ownership from the DESTINATION type, which DOES carry the
                        // element's ownership via its desugared core unique type. This is keyed
                        // strictly to element-slot sources (lastMovedFromContainerSlot), so a move of a named
                        // local / param / field stays source-keyed. Value struct / string destinations
                        // are left untouched (their drop is already correct via the struct/runtime-bit
                        // path). Fixes: a bare `T*` element over-owning (spurious delete -> double-free)
                        // and a `unique <interface>` element under-owning (drop frees nothing -> leak).
                        if (srcMovedFromSlot)
                        {
                            // Re-derive the dropped local's ownership from the DESTINATION type (the
                            // shared recovery the `_ = move _data[i]` discard form also uses). An owning
                            // element frees once (EmitOwningPtrCleanup for a thin ptr, the vtable dtor
                            // for a fat one); a bare borrow element clears the spuriously-set ownership.
                            auto& nv = compiler->GetOrCreateStackVariable(name);
                            ApplyMovedSlotOwnership(nv, typeAndValue);
                        }

                        // Return-result alloc-align channel: a call whose return type declared
                        // `alignas(_, N)` hands back an N-aligned block; stamp the receiving local so
                        // an explicit `delete[]` (or scope-exit free) routes to __delete_aligned.
                        if (compiler->lastCallReturnsAllocAlign > 0)
                        {
                            compiler->GetOrCreateStackVariable(name).AllocAlignment =
                                compiler->lastCallReturnsAllocAlign;
                            compiler->lastCallReturnsAllocAlign = 0;
                        }

                        // Safety boundary: an align-declared local promises a block of exactly its
                        // allocation alignment. Record the declared value on the local (so a later
                        // direct assignment inherits it), then verify the initializer delivered a
                        // matching block - else a free would use the wrong alignment. A direct `new`
                        // inherited it above; a matching aligned source (return/move) already stamped
                        // it. Anything else could not carry the alignment: error rather than corrupt.
                        if (typeAndValue.AllocAlignValue > LLVMBackend::kDefaultNewAlign)
                        {
                            auto& nv = compiler->GetOrCreateStackVariable(name);
                            nv.TypeAndValue.AllocAlignValue = typeAndValue.AllocAlignValue;
                            auto* initAssignExpr = initializer ? initializer->assignmentExpression() : nullptr;
                            // A `unique<T, N>` local carries the alignment in its TYPE: the wrapper's
                            // destructor picks the matching deallocator off N, and the direct `new`
                            // above already inherited N. There is no separate block-alignment field
                            // on the wrapper VALUE to cross-check, so this raw-pointer guard does
                            // not apply.
                            if (compiler->IsCoreUniqueType(typeAndValue.TypeName)
                                && !typeAndValue.Pointer)
                                initAssignExpr = nullptr;
                            if (initAssignExpr != nullptr
                                && nv.AllocAlignment != typeAndValue.AllocAlignValue)
                            {
                                if (AsDirectNew(initAssignExpr) != nullptr)
                                    LogErrorContext(initAssignExpr, std::format(
                                        "allocation alignment mismatch: the 'new' allocates {}-aligned but '{}' is "
                                        "declared 'alignas(_, {})'. The clauses must agree so the free site recovers "
                                        "the correct alignment.",
                                        nv.AllocAlignment, name, typeAndValue.AllocAlignValue));
                                else
                                    LogErrorContext(initAssignExpr, std::format(
                                        "cannot infer allocation alignment here - annotate the 'new' with "
                                        "'alignas(0, {})'. '{}' is declared 'alignas(_, {})', but the initializer is "
                                        "not a direct 'new', so the block alignment cannot be threaded to the "
                                        "allocation and a free would use the wrong alignment.",
                                        typeAndValue.AllocAlignValue, name, typeAndValue.AllocAlignValue));
                            }
                        }

                        // Propagate bond: if the RHS was a bonded call result, tag this local.
                        if (compiler->lastCallIsBonded)
                        {
                            auto& nv = compiler->GetOrCreateStackVariable(name);
                            nv.IsBonded = true;
                            nv.BondByAddress = compiler->lastCallBondByAddress;
                            nv.BondedSources = compiler->lastCallBondedSources;
                            nv.BondDeclBlock = compiler->builder->GetInsertBlock();
                            nv.BondDeclFunction = nv.BondDeclBlock != nullptr
                                ? nv.BondDeclBlock->getParent() : nullptr;
                            compiler->lastCallIsBonded = false;
                            compiler->lastCallBondByAddress = false;
                            compiler->lastCallBondedSources.clear();
                        }

                        // A closure field read carries its bond on the initializer value, not
                        // through the call-result side channel used above.
                        if (haveInitializerSourceNV && initializerSourceNV.IsBonded)
                        {
                            auto& nv = compiler->GetOrCreateStackVariable(name);
                            nv.IsBonded = true;
                            nv.BondByAddress = initializerSourceNV.BondByAddress;
                            nv.BondedSources = initializerSourceNV.BondedSources;
                            nv.BondDeclBlock = compiler->builder->GetInsertBlock();
                            nv.BondDeclFunction = nv.BondDeclBlock != nullptr
                                ? nv.BondDeclBlock->getParent() : nullptr;
                        }

                        // Propagate borrow: if the RHS is a borrowed pointer (non-move param or
                        // a local that already aliases one, possibly via cast), this local also
                        // aliases the borrowed origin and must not be deleted. IsOwning/IsNewAllocated
                        // sources override this (handled above by clearing in those branches).
                        if (srcIsBorrowed && typeAndValue.Pointer && !compiler->lastOwningResult)
                        {
                            auto& nv = compiler->GetOrCreateStackVariable(name);
                            if (!nv.IsOwning && !nv.IsNewAllocated)
                            {
                                nv.IsBorrowed = true;
                                nv.BorrowedOrigin = srcBorrowedOrigin;
                                nv.BorrowedUniqueField = srcBorrowedUniqueField;
                                nv.BorrowedUniqueFieldViaCall = srcBorrowedUniqueFieldViaCall;
                                nv.BorrowedThroughField = !srcBorrowedField.empty();
                            }
                        }

                        // Tag a plain copy of a live OWNING local so a later `delete` of it (raw or
                        // through an interface box) is rejected. A genuine transfer (move/new) makes
                        // this local IsOwning/IsNewAllocated above and is skipped.
                        if (srcBorrowsOwningLocal && typeAndValue.Pointer
                            && !srcOwningLocalOrigin.empty() && srcOwningLocalStorage != nullptr)
                        {
                            auto& nv = compiler->GetOrCreateStackVariable(name);
                            if (!nv.IsOwning && !nv.IsNewAllocated)
                            {
                                nv.BorrowsOwningLocal = true;
                                nv.OwningLocalOrigin = srcOwningLocalOrigin;
                                nv.OwningLocalStorage = srcOwningLocalStorage;
                            }
                        }

                        // Tag a local that borrows a container-owned element (list<unique X*>.get)
                        // so a later `delete` of it is rejected. A genuine transfer (move/new) sets
                        // lastOwningResult and is skipped; the tag is delete-only (see BorrowsOwnedElement).
                        if (srcBorrowsOwnedElement && typeAndValue.Pointer && !compiler->lastOwningResult)
                        {
                            auto& nv = compiler->GetOrCreateStackVariable(name);
                            if (!nv.IsOwning && !nv.IsNewAllocated)
                                compiler->SetVariableBorrowsOwnedElement(
                                    name, true, srcOwnedElementContainer, srcElementExternallyOwned);
                        }

                        // Tag a local declared from a '?:' / '??' JOIN whose every non-null arm
                        // proves another owner. A join carries no source binding, so no clause above
                        // can fire; the arms are still in hand HERE. Recorded in the same pair a
                        // later `p = q;` refreshes, so any reassignment retires or replaces it.
                        // Asked BEFORE the map is indexed: every other clause here is already gated
                        // by a source fact, and `operator[]` would insert on a miss.
                        if (typeAndValue.Pointer && !compiler->lastOwningResult)
                        {
                            std::vector<llvm::Value*> joinSlots;
                            std::string joinOwner = JoinArmsKeepOwner(right, &joinSlots);
                            if (!joinOwner.empty() && !joinSlots.empty())
                            {
                                auto& nv = compiler->GetOrCreateStackVariable(name);
                                if (!nv.IsOwning && !nv.IsNewAllocated && !nv.IsBorrowed
                                    && !nv.BorrowsOwningLocal && !nv.BorrowsOwnedElement)
                                {
                                    nv.JoinKeepsOwner = true;
                                    nv.JoinKeepsOwnerSource = joinOwner;
                                    nv.JoinKeepsOwnerSlots = joinSlots;
                                }
                            }
                        }

                        // Propagate move ownership: if the RHS is a cast (or similar) over an
                        // owning pointer source - the source link was severed (Storage cleared)
                        // but CallerName still names the original - transfer ownership to this
                        // local: tag it owning, null the source alloca, mark source moved.
                        // Without this, both the source's scope-exit cleanup and any 'delete'
                        // on this local would free the same pointer (double-free).
                        // Move params are treated like new-allocated for this purpose.
                        if (srcIsOwningMove && typeAndValue.Pointer && !compiler->lastOwningResult
                            && !compiler->IsCoreUniqueType(srcInferredTypeName))
                        {
                            auto& nv = compiler->GetOrCreateStackVariable(name);
                            if (!nv.IsOwning)
                            {
                                nv.IsOwning = true;
                                nv.IsNewAllocated = true;
                                auto ref = compiler->FindVariableStorage(srcOwningName);
                                if (ref.Storage != nullptr)
                                {
                                    if (auto* ptrTy = llvm::dyn_cast<llvm::PointerType>(ref.BaseType))
                                        compiler->builder->CreateStore(
                                            llvm::ConstantPointerNull::get(ptrTy), ref.Storage);
                                }
                                compiler->MarkVariableMoved(srcOwningName);
                            }
                        }

                        // D5: a `unique` local is an owning location; it must be initialized from an
                        // owned source (new, a `move` expression, or a move-returning call), which the
                        // branches above leave owning, or from nullptr (owns nothing). A borrowed/plain
                        // source would create a second owner of one pointer - reject it. A `unique`
                        // INTERFACE local additionally rejects stack boxing (`unique IShape s = t;`):
                        // only heap `new` sources are legal (no silent heap-promotion of a stack value).
                        // A raw null pointer, or the null fat-ptr an interface `= nullptr` upcasts
                        // to (a null aggregate, not a ConstantPointerNull), owns nothing - allowed.
                        bool srcIsNullLiteral = right != nullptr
                            && ((llvm::isa<llvm::Constant>(right)
                                    && llvm::cast<llvm::Constant>(right)->isNullValue())
                                || IsAllNullPhi(right));
                        if (initializer && destUniqueLoc
                            && !srcIsMove && !srcIsNullLiteral)
                        {
                            auto& nv = compiler->GetOrCreateStackVariable(name);
                            if (!nv.IsOwning && !nv.IsNewAllocated)
                            {
                                const std::string sourceName =
                                    LLVMBackend::BorrowedSourceName(initializerSourceNV);
                                if (sourceName.empty())
                                    LogErrorContext(initDecl, std::format(
                                        "cannot initialize unique '{}' from a borrowed value - the source still "
                                        "owns it; use 'new', a 'move' expression, or a move-returning call, or "
                                        "drop 'unique'", name));
                                else
                                    LogErrorContext(initDecl, std::format(
                                        "cannot initialize unique '{}' from borrowed value '{}' - the source still "
                                        "owns it; use 'new', a 'move' expression, or a move-returning call, or "
                                        "drop 'unique'", name, sourceName));
                            }
                        }

                    }
                }
            }
        }

        return allocList;
    }

std::string MainListener::DescribeUniqueFieldOwner(const LLVMBackend::NamedVariable& nv) {
        std::string field = nv.FieldName.empty() ? nv.TypeAndValue.VariableName : nv.FieldName;
        std::string owner = nv.OwningStructName;
        if (owner.empty() && compilerLLVM)
            owner = SplitEnclosingStruct(compilerLLVM->GetCurrentFunctionName(), compilerLLVM);
        if (compilerLLVM && !owner.empty())
            owner = SpellType(*compilerLLVM, LLVMBackend::TypeAndValue{ .TypeName = owner });
        if (field.empty()) return owner;
        return owner.empty() ? field : owner + "." + field;
    }

llvm::Value* MainListener::ResolveConstantOffsetRoot(llvm::Value* addr, const llvm::DataLayout& dl,
                                                  llvm::APInt& offset, bool& allConstant) {
        allConstant = (addr != nullptr);
        if (addr == nullptr) return nullptr;
        addr = addr->stripPointerCasts();
        while (auto* gep = llvm::dyn_cast<llvm::GEPOperator>(addr))
        {
            if (!gep->accumulateConstantOffset(dl, offset)) { allConstant = false; break; }
            addr = gep->getPointerOperand()->stripPointerCasts();
        }
        return addr;
    }

llvm::Value* MainListener::StripAllGeps(llvm::Value* addr) {
        if (addr == nullptr) return nullptr;
        addr = addr->stripPointerCasts();
        while (auto* gep = llvm::dyn_cast<llvm::GEPOperator>(addr))
            addr = gep->getPointerOperand()->stripPointerCasts();
        return addr;
    }

bool MainListener::AddressRootIsStackOrGlobal(llvm::Value* addr) {
        addr = StripAllGeps(addr);
        return addr != nullptr
            && (llvm::isa<llvm::AllocaInst>(addr) || llvm::isa<llvm::GlobalVariable>(addr));
    }

bool MainListener::ProvablyDifferentObjects(llvm::Value* a, llvm::Value* b) {
        if (a == nullptr || b == nullptr || a == b) return false;
        llvm::Value* rootA = StripAllGeps(a);
        llvm::Value* rootB = StripAllGeps(b);
        if (rootA == nullptr || rootB == nullptr || rootA == rootB) return false;
        return (llvm::isa<llvm::AllocaInst>(rootA) || llvm::isa<llvm::GlobalVariable>(rootA))
            && (llvm::isa<llvm::AllocaInst>(rootB) || llvm::isa<llvm::GlobalVariable>(rootB));
    }

bool MainListener::SameLoadedPointer(llvm::Value* a, llvm::Value* b) {
        auto* la = llvm::dyn_cast_or_null<llvm::LoadInst>(a);
        auto* lb = llvm::dyn_cast_or_null<llvm::LoadInst>(b);
        if (la == nullptr || lb == nullptr || !la->isSimple() || !lb->isSimple()) return false;
        if (la->getParent() == nullptr || la->getParent() != lb->getParent()) return false;
        if (compilerLLVM == nullptr || compilerLLVM->module == nullptr) return false;
        const llvm::DataLayout& dl = compilerLLVM->module->getDataLayout();
        llvm::Value* pa = la->getPointerOperand();
        llvm::Value* pb = lb->getPointerOperand();
        if (!pa->getType()->isPointerTy() || !pb->getType()->isPointerTy()) return false;
        unsigned bits = dl.getIndexTypeSizeInBits(pa->getType());
        if (bits != dl.getIndexTypeSizeInBits(pb->getType())) return false;
        llvm::APInt offA(bits, 0), offB(bits, 0);
        bool okA = false, okB = false;
        llvm::Value* rootA = ResolveConstantOffsetRoot(pa, dl, offA, okA);
        llvm::Value* rootB = ResolveConstantOffsetRoot(pb, dl, offB, okB);
        if (!okA || !okB || rootA == nullptr || rootA != rootB || offA != offB) return false;
        // No store, call, or other memory writer may sit between the two reads.
        const llvm::Instruction* first = la->comesBefore(lb) ? la : lb;
        const llvm::Instruction* last  = (first == la) ? lb : la;
        for (auto it = std::next(first->getIterator()); &*it != last; ++it)
            if (it->mayWriteToMemory()) return false;
        return true;
    }

bool MainListener::ProvablyDifferentSlots(llvm::Value* a, llvm::Value* b) {
        if (a == nullptr || b == nullptr || a == b) return false;
        // Two different globals (or two different locals) named by a receiver kind whose
        // CallerName is empty - a file-scope struct - are otherwise indistinguishable here.
        if (ProvablyDifferentObjects(a, b)) return true;
        if (compilerLLVM == nullptr || compilerLLVM->module == nullptr) return false;
        const llvm::DataLayout& dl = compilerLLVM->module->getDataLayout();
        if (!a->getType()->isPointerTy() || !b->getType()->isPointerTy()) return false;
        unsigned bits = dl.getIndexTypeSizeInBits(a->getType());
        if (bits != dl.getIndexTypeSizeInBits(b->getType())) return false;
        llvm::APInt offA(bits, 0), offB(bits, 0);
        bool okA = false, okB = false;
        llvm::Value* rootA = ResolveConstantOffsetRoot(a, dl, offA, okA);
        llvm::Value* rootB = ResolveConstantOffsetRoot(b, dl, offB, okB);
        if (!okA || !okB || rootA == nullptr) return false;
        if (rootA != rootB && !SameLoadedPointer(rootA, rootB)) return false;
        return offA != offB;
    }

std::string MainListener::DescribeUniqueFieldAccess(const LLVMBackend::NamedVariable& nv) {
        return LLVMBackend::DescribeUniqueFieldAccess(nv);
    }

bool MainListener::IsUniqueFieldRead(const LLVMBackend::NamedVariable& nv) {
        // A cast severs Storage (the GEP-shape test below can't match) and clears IsUnique on the
        // type, so the carried alias flag is the only surviving provenance. `move b.p` yields a
        // fresh NamedVariable with the flag unset, so it stays legal.
        if (nv.IsUniqueFieldAlias && !nv.TypeAndValue.IsMove) return true;
        if (nv.TypeAndValue.IsMove) return false;
        // A `?:`/`??` result has no field-shaped Storage. Its value ledger records the source
        // field slots and is consumed by EmitImplicitUniqueFieldMove at the destination.
        if (compilerLLVM != nullptr && compilerLLVM->IsUniqueFieldReadValue(nv.Primary))
            return true;
        // Ownership gate only - the GEP-shape test below is deliberately NOT widened (it
        // over-matches borrows through casts; see the IsUniqueFieldAlias carve-out above).
        bool ownsPointee = (IsOwningUniquePointerField(nv.TypeAndValue) && nv.TypeAndValue.Pointer)
            || (IsOwningUniqueField(nv.TypeAndValue));
        if (!ownsPointee && !IsOwningUniqueInterfaceField(nv.TypeAndValue)) return false;
        if (nv.IsInterfaceField) return true;
        auto* gep = llvm::dyn_cast_or_null<llvm::GetElementPtrInst>(nv.Storage);
        return gep != nullptr && gep->getNumIndices() == 2
            && gep->getSourceElementType()->isStructTy();
    }

bool MainListener::IsUniqueTempFieldRead(const LLVMBackend::NamedVariable& nv) {
        if (!nv.MovableTempField && !nv.FromOwningTempField) return false;
        if (nv.TypeAndValue.IsMove) return false;
        return (IsOwningUniquePointerField(nv.TypeAndValue) && nv.TypeAndValue.Pointer)
            || IsOwningUniqueField(nv.TypeAndValue);
    }

bool MainListener::IsImplicitCopyableStringTemp(const LLVMBackend::NamedVariable& nv) {
        if (nv.TypeAndValue.TypeName != "string") return false;
        if (nv.TypeAndValue.Pointer || nv.TypeAndValue.IsMove) return false;
        // The borrow marker an `alias` accessor result carries; a `.copy()` result is owned and
        // never has it, so an explicit copy is never copied a second time.
        if (!nv.TypeAndValue.IsAlias) return false;
        if (nv.MovableTempField) return false;
        // A NAMED `alias` string local (`alias string a = ...; ... = a;`) keeps its reject.
        const bool namedAliasLocal = nv.Storage != nullptr
            && !nv.IsElementAccess && !nv.CallerName.empty();
        return !namedAliasLocal;
    }

bool MainListener::AdoptImplicitStringTempCopy(LLVMBackend::NamedVariable& nv, llvm::Value*& value,
                                               antlr4::ParserRuleContext* ctx) {
        auto* compiler = Compiler(ctx);
        if (compiler == nullptr || value == nullptr) return false;
        // By REPRESENTATION, not by spelling: a temp field's NamedVariable can carry the declared
        // name while a joined/laundered value carries none, and only the LLVM type is reliable.
        auto* strTy = llvm::StructType::getTypeByName(*compiler->context, "string");
        if (strTy == nullptr || value->getType() != strTy) return false;
        if (nv.TypeAndValue.Pointer || nv.TypeAndValue.IsMove) return false;
        auto* copied = compiler->EmitOwnedStringDeepCopy(value);
        if (copied == value) return false;   // no 'operator new' in this module - keep the reject
        value = copied;
        // Retire every borrow/temp fact: the value is now an independently OWNED string temp, so
        // downstream rejects must not fire and the copy paths must not copy it a second time.
        nv.Primary = copied;
        nv.Storage = nullptr;
        nv.FromOwningTempField = false;
        nv.OwningTempParent = false;
        nv.MovableTempField = false;
        nv.IsBorrowed = false;
        nv.BorrowsOwnedString = false;
        // An OWNED temp with no Storage: the two field-store paths both read that pair as
        // "already hands us sole ownership", which is exactly what the copy is, and so neither
        // emits a second deep copy on top of it.
        nv.IsOwningString = true;
        nv.IsElementAccess = false;
        nv.BorrowedOrigin.clear();
        nv.OwningStructName.clear();
        nv.FieldName.clear();
        nv.FieldPathText.clear();
        nv.FieldPathRoot.clear();
        nv.CallerName.clear();
        nv.TypeAndValue.IsAlias = false;
        nv.TypeAndValue.TypeName = "string";
        return true;
    }

bool MainListener::IsOwningTempUniqueFieldEscape(const LLVMBackend::NamedVariable& nv) {
        if (nv.TypeAndValue.IsMove) return false;
        if (nv.IsAliasBorrow && !nv.RootIsAliasBorrowLocal) return false;
        if (nv.IsUniqueFieldAlias && nv.FromOwningTempField && nv.OwningTempParent
            && nv.TypeAndValue.Pointer)
            return true;
        if (DeclaredOwningTempUniqueFieldRead(nv)) return true;
        return compilerLLVM != nullptr
            && compilerLLVM->JoinCarriesOwningTempUniqueField(nv.Primary);
    }

bool MainListener::DeclaredOwningTempUniqueFieldRead(const LLVMBackend::NamedVariable& nv) {
        if (!nv.FromOwningTempField || !nv.OwningTempParent) return false;
        if (nv.TypeAndValue.IsMove) return false;
        return (IsOwningUniquePointerField(nv.TypeAndValue) && nv.TypeAndValue.Pointer)
            || IsOwningUniqueField(nv.TypeAndValue)
            || IsOwningUniqueInterfaceField(nv.TypeAndValue);
    }

void MainListener::RejectOwningTempUniqueFieldEscape(const LLVMBackend::NamedVariable& rightNV,
                                           const std::string& destDesc,
                                           antlr4::ParserRuleContext* ctx) {
        // A value LAUNDERED through a borrowing callee is a bare CallInst with no field
        // provenance on it, so name the callee and the field the ledger recorded at the call.
        if (compilerLLVM != nullptr)
            if (const auto* laundered =
                    compilerLLVM->FindLaunderedTempUniqueField(rightNV.Primary))
            {
                LogErrorContext(ctx, LLVMBackend::DescribeLaunderedTempUniqueFieldEscape(
                    laundered->CalleeName, laundered->Access, destDesc));
                return;
            }
        // A join arrives as a PHI and a cast severs Storage, so neither carries a name to quote;
        // say so rather than printing an empty one.
        std::string access = DescribeUniqueFieldAccess(rightNV);
        if (access.empty())
        {
            LogErrorContext(ctx, std::format(
                "cannot store a unique field of a temporary, reached through a cast or a "
                "'?:' / '" "?" "?" "' join, into {} - the temporary's synthesized destructor frees the "
                "pointee at the end of this statement, leaving it dangling. Bind the whole call "
                "result to a local first and read the field from that local.",
                destDesc));
            return;
        }
        LogErrorContext(ctx, std::format(
            "cannot store unique field '{}' of a temporary into {} - the temporary's synthesized "
            "destructor frees the pointee at the end of this statement, leaving it dangling. "
            "'move' needs an addressable source, so bind the whole call result to a local first "
            "and read the field from that local.",
            access, destDesc));
    }

void MainListener::GuardOwningTempUniqueFieldEscape(const LLVMBackend::NamedVariable& nv,
                                           const std::string& destDesc,
                                           antlr4::ParserRuleContext* ctx) {
        if (IsOwningTempUniqueFieldEscape(nv))
        {
            RejectOwningTempUniqueFieldEscape(nv, destDesc, ctx);
            return;   // LogErrorContext throws; this only says so
        }
        if (compilerLLVM == nullptr || nv.TypeAndValue.IsMove || ctx == nullptr) return;
        compilerLLVM->RecordDeferredTempUniqueFieldEscape(nv.Primary, destDesc,
            compilerLLVM->sourceFileName,
            (size_t)ctx->getStart()->getLine(), (size_t)ctx->getStart()->getCharPositionInLine());
    }

bool MainListener::IsOwningUniquePointerField(const LLVMBackend::TypeAndValue& tv) {
        if (tv.IsUnique) return true;
        return false;
    }

bool MainListener::IsOwningUniqueField(const LLVMBackend::TypeAndValue& tv) const {
        if (IsOwningUniquePointerField(tv)) return true;
        return compilerLLVM != nullptr && compilerLLVM->IsCoreUniqueType(tv.TypeName)
            && !tv.Pointer && !tv.ElemPointer && !tv.IsAlias
            && !tv.IsArrayView && !tv.IsSimd
            && tv.ConstArraySize == 0;
}

void MainListener::RejectRawHeapArrayIntoUniqueField(
        const LLVMBackend::NamedVariable& source,
        const LLVMBackend::TypeAndValue& fieldType,
        const std::string& fieldName,
        antlr4::ParserRuleContext* ctx) {
        if (!compilerLLVM->HasRawNewArrayProvenance(source)
            || fieldType.ConstArraySize != 0 || !IsOwningUniqueField(fieldType))
            return;
        const std::string name = fieldName.empty() ? fieldType.VariableName : fieldName;
        LogErrorContext(ctx, std::format(
            "cannot store a heap array in unique field '{}': a unique field frees a single object - "
            "hold the array in a local or a container", name));
    }

bool MainListener::IsOwningUniqueInterfaceField(const LLVMBackend::TypeAndValue& tv) {
        return tv.IsUnique
            && tv.IsInterface && !tv.Pointer
            && !tv.IsAlias && !tv.IsArrayView
            && !tv.IsSimd && tv.ConstArraySize == 0;
    }

void MainListener::RejectBorrowIntoUniqueField(const LLVMBackend::NamedVariable& rightNV,
                                     const std::string& fieldDesc,
                                     antlr4::ParserRuleContext* ctx) {
        std::string src = rightNV.CallerName.empty() ? rightNV.BorrowedOrigin : rightNV.CallerName;
        std::string origin = !rightNV.BorrowedUniqueField.empty()
            ? rightNV.BorrowedUniqueField
            : (rightNV.BorrowedOrigin.empty() ? src : rightNV.BorrowedOrigin);
        // The borrow aliases another `unique` field, so the source field's synthesized
        // destructor owns the pointee - storing it here would make two owners of one pointer.
        if (!rightNV.BorrowedUniqueField.empty() && rightNV.BorrowedUniqueFieldViaCall)
            LogErrorContext(ctx, std::format(
                "cannot store '{}' into {} - it was returned as a borrow of unique field '{}', whose "
                "synthesized destructor already frees it, and two 'unique' fields cannot own one "
                "pointer. Drop 'unique' from the destination field if it only borrows.",
                src, fieldDesc, rightNV.BorrowedUniqueField));
        else if (!rightNV.BorrowedUniqueField.empty())
            LogErrorContext(ctx, std::format(
                "cannot store '{}' into {} - it aliases '{}', a unique field whose synthesized "
                "destructor already frees it, and two 'unique' fields cannot own one pointer. "
                "Use 'move {}' to transfer ownership out of the source field (which nulls it).",
                src, fieldDesc, origin, origin));
        else if (rightNV.FieldName.empty())
            LogErrorContext(ctx, std::format(
                "cannot store borrowed parameter '{}' into {} - the caller still owns it and will "
                "free it on scope exit, leaving the field dangling. Declare the parameter 'move {}' "
                "to transfer ownership, or drop 'unique' from the field if it only borrows.",
                src, fieldDesc, origin));
        else
            LogErrorContext(ctx, std::format(
                "cannot store '{}.{}' into {} - it is reached through borrowed parameter '{}', whose "
                "owner still frees it, and two 'unique' fields cannot own one pointer. Use 'move' to "
                "transfer ownership, or drop 'unique' from the field if it only borrows.",
                src, rightNV.FieldName, fieldDesc, origin));
    }

void MainListener::RejectNonHeapAddressIntoUnique(const std::string& destDesc,
                                        antlr4::ParserRuleContext* ctx) {
        LogErrorContext(ctx, std::format(
            "cannot store the address of a stack or global value into {} - a 'unique' location "
            "owns its pointee and its synthesized destructor frees it, but neither is "
            "heap-allocated and freeing it is undefined. Use 'new' to allocate on the heap, or "
            "drop 'unique' if it only borrows.", destDesc));
    }

void MainListener::RejectBorrowIntoUniqueLocal(const std::string& srcDesc, const std::string& originName,
                                     bool srcIsField, const std::string& localName,
                                     bool isInit, antlr4::ParserRuleContext* ctx,
                                     const std::string& uniqueFieldViaCall) {
        // A borrow returned by an ACCESSOR names no parameter and no spellable source expression -
        // the one true owner is the `unique` field the callee read, so say exactly that.
        // Assignment only: the DECL-INIT spelling is caught earlier by the borrowed-value reject
        // (`cannot initialize unique 'o' from a borrowed value`), so `isInit` never reaches here.
        if (!uniqueFieldViaCall.empty())
        {
            LogErrorContext(ctx, std::format(
                "cannot assign to unique local '{}' from a borrow of unique field '{}' - that "
                "field's synthesized destructor already frees the pointee, so this would free it "
                "twice. Drop 'unique' from '{}' if it only borrows.",
                localName, uniqueFieldViaCall, localName));
            return;
        }
        const char* srcKind = srcIsField ? "" : "borrowed parameter ";
        std::string lead = isInit
            ? std::format("cannot initialize unique local '{}' from {}'{}'",
                          localName, srcKind, srcDesc)
            : std::format("cannot assign {}'{}' to unique local '{}'",
                          srcKind, srcDesc, localName);
        if (srcIsField)
            LogErrorContext(ctx, std::format(
                "{} - it is reached through borrowed parameter '{}', whose owner still frees it, so "
                "this would free it twice. Drop 'unique' from '{}' if it only borrows.",
                lead, originName, localName));
        else
            LogErrorContext(ctx, std::format(
                "{} - the caller still owns it and frees it on scope exit, so this would free it "
                "twice. Declare the parameter 'move {}' to transfer ownership, or drop 'unique' "
                "from '{}' if it only borrows.",
                lead, originName, localName));
    }

std::string MainListener::FormatUniqueTempFieldToField(const LLVMBackend::NamedVariable& rightNV,
                                             const std::string& fieldDesc) {
        std::string access = DescribeUniqueFieldAccess(rightNV);
        if (rightNV.OwningTempParent)
            return std::format(
                "cannot store unique field '{}' of a temporary into {} - the temporary's "
                "synthesized destructor frees it at the end of this statement, and two 'unique' "
                "fields cannot own one pointer. 'move' needs an addressable source, so bind the "
                "whole call result to a local first and move the field out of that local instead.",
                access, fieldDesc);
        return std::format(
            "cannot store unique field '{}' borrowed from a temporary into {} - the container "
            "the temporary was borrowed from still owns it and frees it at its own teardown, "
            "so two 'unique' fields would own one pointer. Drop 'unique' from the destination "
            "field if it only borrows, or store an independent copy of the pointee.",
            access, fieldDesc);
    }

void MainListener::RejectUniqueTempFieldToField(const LLVMBackend::NamedVariable& rightNV,
                                      const std::string& fieldDesc,
                                      antlr4::ParserRuleContext* ctx) {
        LogErrorContext(ctx, FormatUniqueTempFieldToField(rightNV, fieldDesc));
    }

/*
 * The 2026-08-10 implicit-move lowering: null the SOURCE `unique` field so the destination
 * becomes its sole owner. Emitted BEFORE the drop-old that releases the destination's previous
 * pointee, which is what makes an unprovable self-assign (`arr[i].p = arr[j].p` with i==j) a
 * safe no-op - the drop-old then reads a null slot and frees nothing.
 */
void MainListener::EmitImplicitUniqueFieldMove(LLVMBackend::NamedVariable& rightNV,
                                      llvm::Value* right, llvm::Value* destination,
                                      const std::string& destIfaceName) {
        if (compilerLLVM == nullptr || right == nullptr) return;
        // A joined field read has no single Storage. Clear each source in its own predecessor,
        // before the destination drop at the join, so the selected arm transfers ownership and a
        // runtime self-join sees a null old destination rather than freeing its live value.
        if (rightNV.Storage == nullptr)
        {
            if (const auto* join = compilerLLVM->FindUniqueFieldReadJoin(rightNV.Primary))
            {
                auto savedIP = compilerLLVM->builder->saveIP();
                for (const auto& source : join->Sources)
                {
                    if (source.Storage == nullptr || source.Block == nullptr
                        || cflat_llvm::GetTerminatorOrNull(source.Block) == nullptr)
                        continue;
                    compilerLLVM->builder->SetInsertPoint(cflat_llvm::GetTerminatorOrNull(source.Block));
                    if (right->getType()->isPointerTy())
                        compilerLLVM->builder->CreateStore(
                            llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(right->getType())),
                            source.Storage);
                    else if (right->getType()->isStructTy())
                        compilerLLVM->builder->CreateStore(
                            llvm::ConstantAggregateZero::get(right->getType()), source.Storage);
                }
                compilerLLVM->builder->restoreIP(savedIP);
                rightNV.TypeAndValue.IsMove = true;
                return;
            }
            return;
        }
        bool isFat = false;
        if (auto* ptrTy = llvm::dyn_cast<llvm::PointerType>(right->getType()))
            compilerLLVM->builder->CreateStore(
                llvm::ConstantPointerNull::get(ptrTy), rightNV.Storage);
        else if (right->getType()->isStructTy())
        {
            compilerLLVM->builder->CreateStore(
                llvm::ConstantAggregateZero::get(right->getType()), rightNV.Storage);
            isFat = true;
        }
        else
            return;
        // A fat-interface FIELD destination has no drop-old anywhere else, so release the old box
        // here or the move leaks it. Runs after the zeroing, so a self-assign frees nothing.
        if (isFat && destination != nullptr && !destIfaceName.empty())
        {
            auto* oldFat = compilerLLVM->builder->CreateLoad(
                compilerLLVM->GetFatPtrType(), destination, "uqif.old");
            compilerLLVM->DeleteInterfaceValue(oldFat, destIfaceName, nullptr);
        }
        // Present the source as a moved value from here on: Storage is detached so no later leg
        // re-reads the nulled slot, and IsMove is what the borrow/ownership legs already key on.
        rightNV.TypeAndValue.IsMove = true;
        rightNV.Storage = nullptr;
    }

bool MainListener::RejectFieldAllocAlignMismatch(
        const LLVMBackend::TypeAndValue& fieldTV,
        uint64_t fieldAlign,
        const LLVMBackend::NamedVariable& rightNV,
        llvm::Value* right,
        const std::string& fieldDesc,
        antlr4::ParserRuleContext* ctx,
        bool slotIsAuthoritativeHolder,
        const char* destNoun) {
        if (fieldAlign == 0)
            fieldAlign = fieldTV.AllocAlignValue;
        uint64_t rhsAllocAlign = rightNV.AllocAlignment != 0
            ? rightNV.AllocAlignment : rightNV.TypeAndValue.AllocAlignValue;
        bool rhsOverAligned = rhsAllocAlign > LLVMBackend::kDefaultNewAlign;
        bool fieldHasClause = fieldAlign > LLVMBackend::kDefaultNewAlign;
        // A freshly-allocated array-view (`new T[n]...`) stored into a field must carry the
        // block alignment because the field stores only the raw buffer pointer.
        bool rhsFreshBuffer = rightNV.TypeAndValue.IsArrayView;
        // Determinate over-aligned provenance into an unclaused slot is unrecoverable (the free reads
        // the SLOT type). Core unique<T, ALIGN> is the authority on its own raw slot, so it opts out.
        bool rhsAlignIsKnown = rhsFreshBuffer
            || (!slotIsAuthoritativeHolder
                && (rightNV.AllocatedByRawNewArray || rightNV.AllocAlignKnown));
        bool rhsNonNullPtr  = right != nullptr && !llvm::isa<llvm::ConstantPointerNull>(right);
        // Desugared unique fields still need the same source-alignment proof before the raw
        // pointer is wrapped by unique<T, N>. Interface fields retain the legacy marker.
        bool coreUniqueField = !fieldTV.Pointer
            && compilerLLVM->IsCoreUniqueType(fieldTV.TypeName);
        if (rhsNonNullPtr && (fieldTV.IsUnique || coreUniqueField))
        {
            LLVMBackend::TypeAndValue pointeeTV = fieldTV;
            if (coreUniqueField)
                pointeeTV.TypeName = MangledGenericArgument(*compilerLLVM, fieldTV.TypeName);
            if (ElementTypeIsOverAligned(pointeeTV))
                return false;
        }
        bool rhsIndirectPtr = fieldHasClause && rhsNonNullPtr && !rhsFreshBuffer;
        if (!(((rhsAlignIsKnown || fieldHasClause) && rhsOverAligned)
              || (fieldHasClause && (rhsFreshBuffer || rhsIndirectPtr)))
            || fieldAlign == rhsAllocAlign)
            return false;
        // An unclaused slot only corrupts when it actually receives a live over-aligned block.
        if (!fieldHasClause && !(rhsOverAligned && rhsAlignIsKnown && rhsNonNullPtr))
            return false;

        if (!fieldHasClause)
            LogErrorContext(ctx, std::format(
                "cannot store an over-aligned buffer ('new T[n] alignas(0, {})') into {}: that alignment "
                "is a property of the allocation, not of the type, so a free through the {} cannot "
                "recover it. Declare the {} 'alignas(0, {})' so the block alignment is recorded, or "
                "over-align the ELEMENT TYPE instead ('struct alignas({}) Chunk {{ ... }};').",
                rhsAllocAlign, fieldDesc, destNoun, destNoun, rhsAllocAlign, rhsAllocAlign));
        else if (!rhsOverAligned && rhsFreshBuffer)
            LogErrorContext(ctx, std::format(
                "alignment mismatch storing into {}: the {} is declared 'alignas(0, {})' but the "
                "value was allocated without a matching allocation-alignment clause (ordinary alignment). "
                "Allocate it with 'new T[n] alignas(0, {})' so the free site recovers the correct alignment.",
                fieldDesc, destNoun, fieldAlign, fieldAlign));
        else if (!rhsOverAligned)
            // Scalar `new T` takes no alignment clause, so the source must inherit the
            // field's - directly, or through an align-declared local.
            LogErrorContext(ctx, std::format(
                "alignment mismatch storing into {}: the {} is declared 'alignas(0, {})' but the "
                "value was allocated without a matching allocation-alignment clause (ordinary alignment). "
                "Store the 'new' directly into the {}, or declare the source 'alignas(0, {})' so its "
                "'new' inherits the alignment, or over-align the pointee TYPE instead "
                "('struct alignas({}) T {{ ... }};') and drop the {}'s clause.",
                fieldDesc, destNoun, fieldAlign, destNoun, fieldAlign, fieldAlign, destNoun));
        else
            LogErrorContext(ctx, std::format(
                "alignment mismatch storing into {}: the {} is declared 'alignas(0, {})' but the "
                "value was allocated 'alignas(0, {})'. The two must agree so the free site recovers "
                "the correct alignment.",
                fieldDesc, destNoun, fieldAlign, rhsAllocAlign));
        return true;
    }

bool MainListener::RejectLocalAllocAlignMismatch(
        const LLVMBackend::NamedVariable& namedVar,
        const LLVMBackend::NamedVariable& rightNV,
        llvm::Value* right,
        antlr4::ParserRuleContext* ctx) {
        uint64_t localAlign = namedVar.TypeAndValue.AllocAlignValue;
        uint64_t sourceAlign = std::max(rightNV.AllocAlignment,
                                        rightNV.TypeAndValue.AllocAlignValue);
        // Known ordinary: the RHS's alignment is DETERMINATE and ordinary - either a raw
        // `new T[n]` binding, or a move whose source slot carried its alignment provenance.
        bool rhsAlignIsKnown = rightNV.AllocatedByRawNewArray || rightNV.AllocAlignKnown;
        if (localAlign <= LLVMBackend::kDefaultNewAlign)
        {
            bool rhsIsKnownOrdinary = rightNV.AllocAlignment <= LLVMBackend::kDefaultNewAlign
                && rightNV.TypeAndValue.AllocAlignValue <= LLVMBackend::kDefaultNewAlign
                && rhsAlignIsKnown;
            if (namedVar.AllocAlignment > LLVMBackend::kDefaultNewAlign
                && rhsIsKnownOrdinary && right != nullptr
                && !llvm::isa<llvm::ConstantPointerNull>(right)
                && right->getType()->isPointerTy()
                && !ElementTypeIsOverAligned(namedVar.TypeAndValue))
            {
                const std::string& name = namedVar.CallerName;
                const uint64_t trackedAlign = namedVar.AllocAlignment;
                LogErrorContext(ctx, std::format(
                    "alignment mismatch assigning to '{}': the local's inferred allocation alignment "
                    "is {}, but the value was allocated without a matching allocation-alignment clause "
                    "(ordinary alignment). Declare the local 'alignas(0, {})' to make the rule explicit, "
                    "or allocate the source with 'new T[n] alignas(0, {})'.",
                    name, trackedAlign, trackedAlign, trackedAlign));
                return true;
            }
            if (sourceAlign <= LLVMBackend::kDefaultNewAlign) return false;
            // The binding already TRACKS this exact block alignment (declaration-time inference,
            // or the inbound channel that makes a direct `new` inherit it), so its free agrees.
            if (namedVar.AllocAlignment == sourceAlign) return false;
            // A type-wide over-alignment selects the matching allocator/deallocator pair from
            // the static pointee type, so it does not need per-binding allocation provenance.
            if (right == nullptr || llvm::isa<llvm::ConstantPointerNull>(right)
                || !right->getType()->isPointerTy()
                || ElementTypeIsOverAligned(namedVar.TypeAndValue))
                return false;

            const std::string& name = namedVar.CallerName;
            const std::string spelled = SpellType(*Compiler(), LLVMBackend::TypeAndValue{
                .TypeName = namedVar.TypeAndValue.TypeName });
            LogErrorContext(ctx, std::format(
                "cannot assign an over-aligned block (block alignment {}) to unclaused binding '{}': "
                "declare the binding 'alignas(0, {})' (for a parameter: 'alignas(0, {}) {}* {}', after "
                "any 'move'), or over-align the pointee TYPE instead.",
                sourceAlign, name, sourceAlign, sourceAlign, spelled, name));
            return true;
        }
        if (localAlign == rightNV.AllocAlignment) return false;
        // The tracked block alignment (AllocAlignment) is only ever populated by a `new` /
        // `move` / reassignment that ran through the alignment channel - a plain read of a
        // parameter, global, or ternary-join arm never touches it and leaves it at 0 even when
        // the source's own DECLARED type carries a matching 'alignas' clause. Accept on that
        // declared clause too, so a correctly-aligned parameter/global source is not rejected
        // just because its tracked field was never (re)established.
        if (localAlign == rightNV.TypeAndValue.AllocAlignValue) return false;
        // A null store carries no block, and an over-aligned pointee TYPE routes both `new` and
        // `delete` off the static type, so neither can reach a mismatched free site.
        if (right == nullptr || llvm::isa<llvm::ConstantPointerNull>(right)) return false;
        if (!right->getType()->isPointerTy()) return false;
        if (ElementTypeIsOverAligned(namedVar.TypeAndValue)) return false;
        // Fire only on POSITIVE provenance: the RHS must actually be known to carry a different
        // block alignment - a tracked over-aligned block, a declared 'alignas' clause (already
        // proven different above), or a raw allocation whose ordinary (0) alignment is itself
        // known (e.g. `new T[n]` with no clause, moved into a clause-bearing local). A bare
        // named-variable read whose tracked field was simply never established (the ternary-join
        // arms above, or any other untracked source) fails open - the same as master, which has
        // no data to disagree with.
        bool rhsAlignKnown = rightNV.AllocAlignment > LLVMBackend::kDefaultNewAlign
            || rightNV.TypeAndValue.AllocAlignValue > LLVMBackend::kDefaultNewAlign
            || rhsAlignIsKnown;
        if (!rhsAlignKnown) return false;

        const std::string& name = namedVar.CallerName;
        if (rightNV.AllocAlignment <= LLVMBackend::kDefaultNewAlign)
            LogErrorContext(ctx, std::format(
                "alignment mismatch assigning to '{}': the local is declared 'alignas(0, {})' but the "
                "value was allocated without a matching allocation-alignment clause (ordinary alignment). "
                "Allocate it with 'new T[n] alignas(0, {})', or declare the source 'alignas(0, {})' so "
                "its 'new' inherits the alignment, or over-align the pointee TYPE instead "
                "('struct alignas({}) T {{ ... }};') and drop the local's clause.",
                name, localAlign, localAlign, localAlign, localAlign));
        else
            LogErrorContext(ctx, std::format(
                "alignment mismatch assigning to '{}': the local is declared 'alignas(0, {})' but the "
                "value was allocated 'alignas(0, {})'. The two must agree so the free site recovers "
                "the correct alignment.",
                name, localAlign, rightNV.AllocAlignment));
        return true;
    }

void MainListener::TransferPointerOwnershipOnStore(
        const LLVMBackend::NamedVariable& rightNV,
        llvm::Value* destination,
        bool destIsInterface,
        antlr4::ParserRuleContext* ctx) {
        auto* compiler = Compiler(ctx);

        // Lazy refcount: if a new-allocated local is assigned to a non-local destination
        // (struct field, heap object), create a refcount on first escape and increment it.
        if (rightNV.IsNewAllocated && rightNV.TypeAndValue.Pointer
            && !rightNV.CallerName.empty()
            && destination != nullptr
            && !llvm::isa<llvm::AllocaInst>(destination)
            && !llvm::isa<llvm::GlobalVariable>(destination))
        {
            // Fetch the live RefCountStorage (rightNV is a copy; look up the actual NV).
            llvm::Value* refAlloca = rightNV.RefCountStorage;
            if (refAlloca == nullptr)
            {
                // First escape: emit the refcount alloca at function entry (initialized to 1).
                auto savedIP = compiler->builder->saveIP();
                auto* fn = compiler->builder->GetInsertBlock()->getParent();
                auto* entryBB = &fn->getEntryBlock();
                compiler->builder->SetInsertPoint(entryBB, entryBB->begin());
                refAlloca = compiler->builder->CreateAlloca(compiler->builder->getInt32Ty(), nullptr, "refcount");
                compiler->builder->CreateStore(compiler->builder->getInt32(1), refAlloca);
                compiler->builder->restoreIP(savedIP);
                compiler->SetVariableRefCountStorage(rightNV.CallerName, refAlloca);
            }
            // Increment for this escape.
            auto* cur = compiler->builder->CreateLoad(compiler->builder->getInt32Ty(), refAlloca);
            compiler->builder->CreateStore(
                compiler->builder->CreateAdd(cur, compiler->builder->getInt32(1), "refinc"),
                refAlloca);
        }

        // Transfer ownership: null the source alloca so EmitDestructorsForScope
        // won't free the pointer we just stored elsewhere.
        // Move params (IsOwning && !IsNewAllocated): null for any destination type.
        // New-allocated locals (IsOwning && IsNewAllocated): null only for local destinations;
        //   struct-field escapes use refcount above so both sides validly hold the pointer.
        // When the RHS came through a cast that cleared rightNV.Storage, fall back to
        // looking up the original variable by CallerName so the source is still nulled.
        // Array-view RHS is excluded: `int[]` is a non-owning alias, so `view = view`
        // must rebind the pointer only. Nulling/moving the source orphans the buffer
        // because the destination view never frees it (the owner is the `new T[n]`
        // variable, deleted explicitly). Mirrors the struct-move guard above.
        if (rightNV.IsOwning
            && rightNV.TypeAndValue.Pointer
            && !rightNV.TypeAndValue.IsArrayView
            && (!rightNV.IsNewAllocated
                || destination == nullptr
                || llvm::isa<llvm::AllocaInst>(destination)
                || llvm::isa<llvm::GlobalVariable>(destination)))
        {
            llvm::Value* srcStorage = rightNV.Storage;
            llvm::Type*  srcBaseTy  = rightNV.BaseType;
            if (srcStorage == nullptr && !rightNV.CallerName.empty())
            {
                auto ref = compiler->FindVariableStorage(rightNV.CallerName);
                srcStorage = ref.Storage;
                srcBaseTy  = ref.BaseType;
            }
            // Self-assign (`a = a`): source slot == destination slot. The value was already
            // stored back into the same slot, so nulling it would zero the live pointer and
            // segfault on the next deref. Skip the transfer - the slot still owns its pointee.
            // Mirrors the struct-move `destination != rightNV.Storage` self-assign guard.
            if (srcStorage != nullptr && srcStorage != destination)
            {
                if (auto* ptrTy = llvm::dyn_cast<llvm::PointerType>(srcBaseTy))
                {
                    compiler->builder->CreateStore(
                        llvm::ConstantPointerNull::get(ptrTy), srcStorage);
                    const bool sourceIsWholeRawArrayBinding = !rightNV.IsElementAccess
                        && rightNV.FieldName.empty() && rightNV.OwningStructName.empty()
                        && (llvm::isa<llvm::AllocaInst>(srcStorage)
                            || llvm::isa<llvm::GlobalVariable>(srcStorage));
                    if (sourceIsWholeRawArrayBinding)
                        compiler->StoreRawArrayLength(rightNV, nullptr);
                    // Moving a field (`x = node->left`) marks only that field, not the base.
                    if (!rightNV.FieldName.empty())
                        compiler->MarkVariableFieldMoved(rightNV.CallerName, rightNV.FieldName);
                    else
                    {
                        compiler->MarkVariableMoved(rightNV.CallerName);
                        // Boxing into an interface ('live = sc') transfers ownership to a handle
                        // that is not auto-destructed; flag the source so a later 'delete sc' is
                        // rejected (it would be a no-op and leak).
                        if (destIsInterface)
                            compiler->MarkVariableMovedIntoInterface(rightNV.CallerName);
                    }
                }
            }
        }
    }

llvm::Value* MainListener::CloneClosureFromNamedSource(
        const LLVMBackend::NamedVariable& rightNV,
        llvm::Value* right,
        antlr4::ParserRuleContext* ctx) {
        auto* compiler = Compiler(ctx);
        bool namedSource = rightNV.Storage != nullptr
            && !rightNV.TypeAndValue.IsMove
            && rightNV.TypeAndValue.TypeName == "__closure_fat_ptr";
        if (!namedSource)
            return right;
        // Fresh arg NV (Storage + closure type only) so the user-side CallerName /
        // named-argument metadata on rightNV does not confuse copy() overload resolution.
        LLVMBackend::NamedVariable cloneArg;
        cloneArg.BaseType = compiler->GetClosureFatPtrType();
        // A union member's Storage is the union alloca, so a type-inferred load off it reads the
        // whole union; hand the copy a value loaded with the closure type instead.
        if (rightNV.UnionFieldType != nullptr)
            cloneArg.Primary = compiler->CreateLoad(cloneArg.BaseType, rightNV.Storage);
        else
            cloneArg.Storage = rightNV.Storage;
        cloneArg.TypeAndValue.TypeName = "__closure_fat_ptr";
        if (auto* cloned = compiler->CreateOverloadedFunctionCall("copy", { cloneArg }))
            return cloned;
        return right;
    }

llvm::Value* MainListener::EmitCopyableOwnerCopy(
        const LLVMBackend::NamedVariable& rightNV,
        llvm::Value* right,
        antlr4::ParserRuleContext* ctx) {
        auto* compiler = Compiler(ctx);
        if (rightNV.TypeAndValue.TypeName == "string")
            return compiler->EmitOwnedStringDeepCopy(right);
        // Pass the loaded by-value struct (Primary), not the source Storage: copy()/the memberwise
        // synth take `self` BY VALUE. A fresh arg NV (value + type only, no CallerName) keeps
        // overload resolution from being confused by the source's named-argument metadata.
        LLVMBackend::NamedVariable srcNV;
        srcNV.Primary  = right;
        srcNV.BaseType = right->getType();
        srcNV.TypeAndValue.TypeName = rightNV.TypeAndValue.TypeName;
        if (auto* copied = compiler->CreateOverloadedFunctionCall("copy", { srcNV }))
            return copied;
        return right;
    }

llvm::Value* MainListener::ClassifyOwningAssignSource(
        llvm::Value* right, const std::string& destTypeName, bool srcIsMove,
        antlr4::ParserRuleContext* ctx, AssignSourceKind& outKind) {
        auto* compiler = Compiler(ctx);
        if (compiler->IsCopyableType(destTypeName) && !srcIsMove)
        {
            outKind = AssignSourceKind::Copy;
            LLVMBackend::NamedVariable copySrc;
            copySrc.TypeAndValue.TypeName = destTypeName;
            return EmitCopyableOwnerCopy(copySrc, right, ctx);
        }
        outKind = AssignSourceKind::Move;
        return right;
    }

bool MainListener::SourceIsDanglingAliasBorrow(LLVMBackend* compiler,
        const LLVMBackend::NamedVariable& nv) {
        return (nv.TypeAndValue.IsAlias || nv.IsAliasBorrow)
            && nv.TypeAndValue.TypeName != "string"
            && nv.TypeAndValue.TypeName != "__closure_fat_ptr"
            && (nv.TypeAndValue.Pointer
                || compiler->IsOwningValueType(nv.TypeAndValue.TypeName));
    }

void MainListener::ApplyCallResultBorrowProvenance(LLVMBackend* compiler,
        LLVMBackend::NamedVariable& nv) {
        if (compiler == nullptr || nv.IsBorrowed || nv.TypeAndValue.IsMove) return;
        bool coreUniqueValue = !nv.TypeAndValue.Pointer
            && compiler->IsCoreUniqueType(nv.TypeAndValue.TypeName);
        if ((!nv.TypeAndValue.Pointer && !coreUniqueValue) || nv.Primary == nullptr) return;
        const auto* fieldBorrow = compiler->FindUniqueFieldBorrowResult(nv.Primary);
        if (fieldBorrow == nullptr) return;
        nv.IsBorrowed = true;
        nv.BorrowedOrigin = fieldBorrow->FieldOwner;
        nv.BorrowedUniqueField = fieldBorrow->FieldOwner;
        nv.BorrowedUniqueFieldViaCall = true;
    }

bool MainListener::BorrowAdoptionIsUnsound(LLVMBackend* compiler,
        const LLVMBackend::NamedVariable& nv) {
        if (!SourceIsDanglingAliasBorrow(compiler, nv)) return false;
        if (nv.IsAliasBorrow) return true;
        // A non-pointer alias call result now carries the live source slot in Storage. It is
        // still a borrowed value at an owning adoption site, even though that Storage is a GEP.
        if (nv.TypeAndValue.IsAlias && !nv.TypeAndValue.Pointer && !nv.IsAliasBorrow)
            return true;
        return nv.Storage == nullptr
            || llvm::isa<llvm::AllocaInst>(nv.Storage)
            || llvm::isa<llvm::GlobalVariable>(nv.Storage);
}

void MainListener::RecordAliasBorrowDeclBlock(LLVMBackend* compiler,
        LLVMBackend::NamedVariable& nv) {
        auto* here = compiler->builder != nullptr ? compiler->builder->GetInsertBlock() : nullptr;
        nv.AliasBorrowDeclBlock = here;
        nv.AliasBorrowDeclFunction = here != nullptr ? here->getParent() : nullptr;
    }

bool MainListener::IsAliasBorrowLocalBinding(const LLVMBackend::NamedVariable& nv) {
        if (!nv.IsAliasBorrow) return false;
        // A by-reference lambda capture is IsAliasBorrow too, but its Storage IS the outer owner's
        // address. A declared non-pointer alias binding has the same reference shape, while its
        // Storage may be a field GEP instead of an own slot; both must retain the borrow-root fact
        // for field-consume diagnostics.
        return (llvm::isa_and_nonnull<llvm::AllocaInst>(nv.Storage)
                || llvm::isa_and_nonnull<llvm::GlobalVariable>(nv.Storage))
            || (nv.TypeAndValue.IsAlias && !nv.TypeAndValue.Pointer
                && !nv.CallerName.empty() && nv.Storage != nullptr);
}

bool MainListener::DestinationIsAliasBorrowLocal(LLVMBackend* compiler, llvm::Value* destination) {
        if (compiler == nullptr || destination == nullptr) return false;
        if (!llvm::isa<llvm::AllocaInst>(destination) && !llvm::isa<llvm::GlobalVariable>(destination))
            return false;
        const auto* bind = compiler->FindVariableByStorage(destination);
        return bind != nullptr && IsAliasBorrowLocalBinding(*bind);
    }

void MainListener::RetireAliasBorrowOnRebind(LLVMBackend* compiler, llvm::Value* destination) {
        if (compiler == nullptr || destination == nullptr || compiler->builder == nullptr) return;
        auto* here = compiler->builder->GetInsertBlock();
        if (here == nullptr) return;
        for (auto& frame : compiler->stackNamedVariable)
            for (auto& [varName, nv] : frame.namedVariable)
            {
                if (nv.Storage != destination || !IsAliasBorrowLocalBinding(nv)) continue;
                // Retire ONLY when this store sits in the block the binding was declared in: every
                // path that reaches scope exit then ran it, so the local really owns what it holds.
                if (nv.AliasBorrowDeclBlock != here
                    || nv.AliasBorrowDeclFunction != here->getParent())
                    return;
                nv.IsAliasBorrow = false;
                nv.AliasBorrowDeclBlock = nullptr;
                nv.AliasBorrowDeclFunction = nullptr;
                return;
            }
    }

bool MainListener::RejectAliasBorrowAdoption(
        const LLVMBackend::NamedVariable& rightNV,
        llvm::Value* right,
        const char* destKind,
        antlr4::ParserRuleContext* ctx) {
        auto* compiler = Compiler(ctx);
        if (right == nullptr || !BorrowAdoptionIsUnsound(compiler, rightNV)) return false;

        LogErrorContext(ctx, std::format(
            "cannot store an 'alias' value '{}' into {}; it borrows storage it does not own and "
            "would dangle. Use '.copy()' for an independent owned copy.",
            rightNV.CallerName.empty() ? SpellType(*compiler, rightNV.TypeAndValue) : rightNV.CallerName,
            destKind));
        return true;
    }

bool MainListener::RejectAliasStoreIntoField(
        const LLVMBackend::NamedVariable& rightNV,
        llvm::Value* right,
        antlr4::ParserRuleContext* ctx) {
        auto* compiler = Compiler(ctx);
        if (!(right && SourceIsDanglingAliasBorrow(compiler, rightNV)))
            return false;

        // Let the dedicated unique-field gate below name this more specific ownership hazard.
        if (IsUniqueTempFieldRead(rightNV)) return false;

        // An opaque void pointer member is copied by value, even when its containing object is
        // borrowed. Typed pointers can point into owned storage, so keep them on the reject set.
        if (!rightNV.FieldName.empty()
            && rightNV.TypeAndValue.TypeName == "void"
            && rightNV.TypeAndValue.Pointer
            && !rightNV.TypeAndValue.ElemPointer
            && !rightNV.TypeAndValue.IsArrayView
            && rightNV.TypeAndValue.ConstArraySize == 0
            && !IsOwningUniquePointerField(rightNV.TypeAndValue)
            && !IsOwningUniqueInterfaceField(rightNV.TypeAndValue))
            return false;

        if (rightNV.FromOwningTempField && rightNV.TypeAndValue.TypeName != "string")
        {
            // A raw, non-owning pointer read out of the temp (a `void*` OS handle, a back-pointer)
            // is not a buffer anyone frees, so copying it into a field can never dangle. Owning
            // unique pointers took the IsUniqueTempFieldRead exit above and never reach here.
            if (rightNV.TypeAndValue.Pointer
                && !IsOwningUniquePointerField(rightNV.TypeAndValue)
                && !IsOwningUniqueInterfaceField(rightNV.TypeAndValue))
                return false;
            LogErrorContext(ctx, std::format(
                "cannot store '{}.{}' taken from a temporary into a field; its buffer is owned "
                "elsewhere and would be freed. Use '.copy()' for an independent owned copy.",
                rightNV.OwningStructName, rightNV.FieldName));
            return true;
        }

        LogErrorContext(ctx, std::format(
            "cannot store an 'alias' value '{}' into a field; it borrows storage it does not own "
            "and would dangle. Use '.copy()' for an independent owned copy.",
            rightNV.CallerName.empty() ? SpellType(*compiler, rightNV.TypeAndValue) : rightNV.CallerName));
        return true;
    }

bool MainListener::RejectMixedOwnershipTernaryJoin(
        llvm::Value* trueValue, llvm::Value* falseValue, llvm::Value* joined,
        antlr4::ParserRuleContext* trueCtx, antlr4::ParserRuleContext* falseCtx,
        antlr4::ParserRuleContext* ctx) {
        auto* compiler = Compiler(ctx);
        if (joined == nullptr || !compiler->IsOwningValueStructValue(joined)) return false;
        bool trueOwns = compiler->TernaryArmJoinsOwning(trueValue);
        bool falseOwns = compiler->TernaryArmJoinsOwning(falseValue);
        if (trueOwns == falseOwns) return false;
        // Original source slice, not getText(): the parse tree drops whitespace, so `move x`
        // would otherwise be quoted back at the user as 'movex'.
        auto armText = [](antlr4::ParserRuleContext* armCtx) {
            std::string text("?");
            if (armCtx != nullptr && armCtx->start != nullptr && armCtx->stop != nullptr
                && armCtx->start->getInputStream() != nullptr)
                text = armCtx->start->getInputStream()->getText(antlr4::misc::Interval(
                    armCtx->start->getStartIndex(), armCtx->stop->getStopIndex()));
            else if (armCtx != nullptr)
                text = armCtx->getText();
            if (text.size() > 40) text = text.substr(0, 37) + "...";
            return text;
        };
        // Fixed wording, owning arm first, so the catalog string carries no English fragments.
        antlr4::ParserRuleContext* owningCtx = trueOwns ? trueCtx : falseCtx;
        antlr4::ParserRuleContext* borrowCtx = trueOwns ? falseCtx : trueCtx;
        LogErrorContext(ctx, compiler->LocalizeMessage(
            "'?:' arms differ in ownership: '{}' is an owning value and '{}' is a borrow of an "
            "existing value. Either 'move' the borrowed side into a fresh value so both arms own, "
            "or make both arms borrows.",
            { armText(owningCtx), armText(borrowCtx) }));
        return true;
    }

bool MainListener::RejectMixedOwnershipPointerTernaryJoin(
        llvm::Value* trueValue, llvm::Value* falseValue, llvm::Value* joined,
        llvm::Value* trueStorage, llvm::Value* falseStorage,
        antlr4::ParserRuleContext* trueCtx, antlr4::ParserRuleContext* falseCtx,
        antlr4::ParserRuleContext* ctx) {
        auto* compiler = Compiler(ctx);
        if (joined == nullptr || !joined->getType()->isPointerTy()) return false;
        // A null arm carries no block at all: `c ? buf : nullptr` allocates nothing on either
        // side, so it is neither an owning nor a borrowing mismatch. TernaryArmJoinsOwning scores
        // null OWNING (it is safe to adopt), which is the opposite of what this rule needs.
        auto isNullArm = [](llvm::Value* arm) {
            auto* c = llvm::dyn_cast_or_null<llvm::Constant>(arm);
            return c != nullptr && c->isNullValue();
        };
        if (isNullArm(trueValue) || isNullArm(falseValue)) return false;
        bool trueOwns = compiler->TernaryArmJoinsOwning(trueValue);
        bool falseOwns = compiler->TernaryArmJoinsOwning(falseValue);
        if (trueOwns == falseOwns) return false;
        // Prove the BORROW side too. TernaryArmJoinsOwning answers the owning-temp ledgers, so its
        // `false` covers "unrecorded" as well as "borrows" - reading it as a borrow claim rejects
        // `T* owner = new T(); c ? owner : new T();`, which the corpus relies on.
        llvm::Value* borrowValue   = trueOwns ? falseValue   : trueValue;
        llvm::Value* borrowStorage = trueOwns ? falseStorage : trueStorage;
        if (!compiler->TernaryArmIsProvenBorrow(borrowValue, borrowStorage)) return false;
        // Original source slice, not getText(): the parse tree drops whitespace, so `move x`
        // would otherwise be quoted back at the user as 'movex'.
        auto armText = [](antlr4::ParserRuleContext* armCtx) {
            std::string text("?");
            if (armCtx != nullptr && armCtx->start != nullptr && armCtx->stop != nullptr
                && armCtx->start->getInputStream() != nullptr)
                text = armCtx->start->getInputStream()->getText(antlr4::misc::Interval(
                    armCtx->start->getStartIndex(), armCtx->stop->getStopIndex()));
            else if (armCtx != nullptr)
                text = armCtx->getText();
            if (text.size() > 40) text = text.substr(0, 37) + "...";
            return text;
        };
        antlr4::ParserRuleContext* owningCtx = trueOwns ? trueCtx : falseCtx;
        antlr4::ParserRuleContext* borrowCtx = trueOwns ? falseCtx : trueCtx;
        LogErrorContext(ctx, compiler->LocalizeMessage(
            "'?:' arms differ in ownership: '{}' allocates a value the join cannot own and '{}' "
            "is a borrow of an existing value, so the allocating arm leaks whenever it is taken. "
            "Either 'move' the borrowed side into a fresh value so both arms own, or allocate "
            "into a named owner before the '?:' and make both arms borrows.",
            { armText(owningCtx), armText(borrowCtx) }));
        return true;
    }

bool MainListener::RejectNonOwningStructJoinStore(llvm::Value* right, const std::string& destTypeName,
                                        const char* destKind, antlr4::ParserRuleContext* ctx) {
        auto* compiler = Compiler(ctx);
        if (right == nullptr || !right->getType()->isStructTy()) return false;
        if (!compiler->IsNonOwningStructJoin(right)) return false;
        if (destTypeName.empty() || !compiler->IsOwningValueType(destTypeName)) return false;
        const std::string destTypeSpelling = SpellType(*compiler,
            LLVMBackend::TypeAndValue{ .TypeName = destTypeName });
        // Mixed-ownership joins are rejected at the join itself now, so what still reaches here is
        // a join of two BORROWED arms landing in an owning destination.
        LogErrorContext(ctx, compiler->LocalizeMessage(
            "cannot store a '?:' that joins two borrowed '{}' arms into {}; the destination's "
            "destructor would free a value its existing owner still frees. Assign each arm in its "
            "own branch, 'move' each arm into a fresh owned value, declare the function 'alias' to "
            "hand the borrow through, or write a 'copy()' method for '{}' and copy the arm into an "
            "independent owned value.",
            { destTypeSpelling, destKind, destTypeSpelling }));
        return true;
    }

bool MainListener::RejectOwningValueCopyIntoField(
        const LLVMBackend::NamedVariable& rightNV,
        llvm::Value*& right,
        bool isSelfAssign,
        bool& outCopied,
        antlr4::ParserRuleContext* ctx) {
        auto* compiler = Compiler(ctx);
        outCopied = false;
        // A desugared unique field uses the ordinary value representation, but a plain field
        // assignment is still the language's implicit ownership move, not a memberwise copy.
        bool coreUniqueFieldRead = compiler->IsCoreUniqueType(rightNV.TypeAndValue.TypeName)
            && !rightNV.TypeAndValue.Pointer && !rightNV.TypeAndValue.IsMove
            && (!rightNV.FieldName.empty() || !rightNV.OwningStructName.empty()
                || llvm::isa_and_nonnull<llvm::GetElementPtrInst>(rightNV.Storage));
        if (coreUniqueFieldRead) return false;
        if (!(right && right->getType()->isStructTy()
            && !rightNV.TypeAndValue.Pointer
            && !rightNV.TypeAndValue.IsArrayView
            && !rightNV.TypeAndValue.IsMove
            && rightNV.TypeAndValue.TypeName != "string"  // string members are never auto-destructed (leak, not double-free)
            && right->getType() != llvm::StructType::getTypeByName(*compiler->context, "string")
            // The RHS must be an addressable lvalue (variable / parameter / field) whose backing
            // buffer is still owned by that source - that is what makes the shallow copy an alias.
            // A function-call result, '.copy()', or any temporary has null Storage (ownership was
            // transferred to us, nothing else frees it) and is therefore safe - this is the
            // recommended fix, so it must NOT be flagged. (Call results carry the callee name in
            // CallerName, so CallerName alone cannot distinguish them.)
            && rightNV.Storage != nullptr
            && !rightNV.CallerName.empty()
            && compiler->GetOrCreateFullDestructor(rightNV.TypeAndValue.TypeName) != nullptr))
            return false;

        // Copyable owner: COPY into the field (independent duplicate), leave the source live and
        // proceed with the store. The copy is produced before any old-field destruct at the caller.
        // A self-assign stores the same bits back (the self-store skips the old-field free), so it
        // must NOT copy - a copy would orphan (leak) the field's current buffer.
        if (compiler->IsCopyableType(rightNV.TypeAndValue.TypeName))
        {
            if (!isSelfAssign)
            {
                right = EmitCopyableOwnerCopy(rightNV, right, ctx);
                outCopied = true;
            }
            return false;
        }

        // Render the mangled generic name back to source spelling for the message
        // (e.g. "list$int" -> "list<int>", "dictionary$string$int" -> "dictionary<string, int>").
        std::string displayType = SpellType(*compiler, rightNV.TypeAndValue);
        LogErrorContext(ctx, std::format(
            "copying owning value '{}' by value into a struct field aliases its backing buffer "
            "and will double-free at teardown; use '.copy()' for an independent copy or 'move' "
            "to transfer ownership", displayType));
        return true;
    }

void MainListener::TransferMoveStringOwnershipOnStore(
        const LLVMBackend::NamedVariable& rightNV,
        antlr4::ParserRuleContext* ctx) {
        auto* compiler = Compiler(ctx);
        if (!(rightNV.IsOwningString && rightNV.Storage != nullptr && rightNV.TypeAndValue.IsMove))
            return;
        if (auto* strTy = llvm::StructType::getTypeByName(*compiler->context, "string"))
        {
            auto* ptrField = compiler->builder->CreateStructGEP(strTy, rightNV.Storage, 0);
            compiler->builder->CreateStore(
                llvm::ConstantPointerNull::get(compiler->builder->getPtrTy()), ptrField);
        }
    }

bool MainListener::NamedVarIsString(const LLVMBackend::NamedVariable& nv) {
        if (nv.TypeAndValue.TypeName == "string") return true;
        if (auto* st = llvm::dyn_cast_or_null<llvm::StructType>(nv.BaseType))
            return st->getName() == "string";
        return false;
    }

// The READ twin of `destIsFixedArrayElem`: a `string` element whose slot is LIVE storage the
// reader does not own. Two shapes qualify. A FIXED array (`dst[0]`, `dst[0][0]`, `w.arr[0]`, a
// global's element) is a TWO-index GEP over an array type. A `T[]` VIEW element is a SINGLE-index
// GEP, identical in shape to Part 6's container slot - which must stay a borrow - so it is
// admitted only on the positive `IsViewElement` provenance, never on the GEP alone. A raw `T*`
// heap array carries neither and keeps its borrow. Keyed on the REPRESENTATION (the `%string`
// named type by identity), because an element read off a temp-producing expression carries no
// TypeName at all. A `move` source already transferred its buffer.
bool MainListener::IsOwningArrayStringElementRead(
        const LLVMBackend::NamedVariable& nv, llvm::Value* value) {
        if (value == nullptr || nv.Storage == nullptr || !nv.IsElementAccess) return false;
        if (nv.TypeAndValue.IsMove || nv.TypeAndValue.Pointer || nv.TypeAndValue.IsArrayView)
            return false;
        // GEPOperator, not GetElementPtrInst: a constant index off a GLOBAL array folds the
        // access into a ConstantExpr GEP that the instruction cast would miss.
        auto* gep = llvm::dyn_cast<llvm::GEPOperator>(nv.Storage);
        if (gep == nullptr) return false;
        bool fixedArrayElem = gep->getNumIndices() == 2
            && gep->getSourceElementType()->isArrayTy();
        bool viewElem = gep->getNumIndices() == 1 && nv.IsViewElement;
        if (!fixedArrayElem && !viewElem)
            return false;
        return value->getType()
            == llvm::StructType::getTypeByName(*compilerLLVM->context, "string");
    }

// A `string` element read out of a raw `new string[n]` array: a single-index GEP whose base is a
// LOAD from an alloca-backed LOCAL `string*`. The allocation does not own its elements (its
// destructive `delete` is rejected), so the read must be a guaranteed BORROW - the caller clears
// the runtime owned bit a slot may still carry. A CONTAINER's `_data[i]` loads its base from a
// struct field, never from an alloca, so `list`/`dictionary` reads keep their exact bit-copy.
bool MainListener::IsRawHeapStringElementRead(
        const LLVMBackend::NamedVariable& nv, llvm::Value* value) {
        if (value == nullptr || nv.Storage == nullptr || !nv.IsElementAccess) return false;
        if (nv.IsViewElement || nv.TypeAndValue.IsArrayView || nv.TypeAndValue.Pointer)
            return false;
        auto* gep = llvm::dyn_cast<llvm::GEPOperator>(nv.Storage);
        if (gep == nullptr || gep->getNumIndices() != 1) return false;
        auto* baseLoad = llvm::dyn_cast<llvm::LoadInst>(gep->getPointerOperand());
        if (baseLoad == nullptr || !llvm::isa<llvm::AllocaInst>(baseLoad->getPointerOperand()))
            return false;
        if (!RawHeapBaseIsNewArrayLocal(gep->getPointerOperand())) return false;
        return value->getType()
            == llvm::StructType::getTypeByName(*compilerLLVM->context, "string");
    }

// The POSITIVE admission for both raw-heap arms: the GEP's base is a LOAD of a local that holds a
// `new T[n]` allocation. Anything else - a decayed fixed array, a join, a parameter - answers false
// and keeps the plain store, so an unenumerated binding costs a copy and never a dangling borrow.
bool MainListener::RawHeapBaseIsNewArrayLocal(llvm::Value* gepBase) {
        auto* baseLoad = llvm::dyn_cast_or_null<llvm::LoadInst>(gepBase);
        if (baseLoad == nullptr) return false;
        const auto* bind = compilerLLVM->FindVariableByStorage(baseLoad->getPointerOperand());
        return bind != nullptr && bind->AllocatedByRawNewArray;
    }

/*
 * A `return` source that reads an owning VALUE out of another object's storage: a FIELD path
 * (`w.b`, `w.a.b`, `arr[0].b`), a fixed-array ELEMENT (a two-index GEP over an array type), or a
 * `T[]` VIEW element (a single-index GEP, admitted only on the positive `IsViewElement`
 * provenance - never on the GEP shape alone, since the WHOLE view field is also a single-index
 * GEP and must stay a borrow). Mirrors `IsOwningArrayStringElementRead`'s view-element arm, but
 * for an owning STRUCT return type instead of `string`, which has its own runtime owned bit and
 * its own arm above.
 */
bool MainListener::ReturnSourceIsIndirectOwningLvalue(
        const LLVMBackend::NamedVariable& nv, llvm::Value* value) {
        auto* compiler = compilerLLVM;
        if (value == nullptr || nv.Storage == nullptr) return false;
        if (nv.TypeAndValue.Pointer || nv.TypeAndValue.ElemPointer
            || nv.TypeAndValue.IsArrayView || nv.TypeAndValue.IsInterface
            || nv.TypeAndValue.IsInterfacePointer || nv.TypeAndValue.IsFunctionPointer)
            return false;
        if (nv.TypeAndValue.TypeName.empty() || nv.TypeAndValue.TypeName == "string")
            return false;
        bool aliasesFrameOwner = false;
        if (nv.IsAliasBorrow && nv.RootIsAliasBorrowLocal)
        {
            auto rootStorage = compiler->FindVariableStorage(FieldPathRootName(nv)).Storage;
            if (rootStorage != nullptr && llvm::isa<llvm::AllocaInst>(rootStorage))
                for (const auto& frame : compiler->stackNamedVariable)
                    for (const auto& [name, binding] : frame.namedVariable)
                        if (binding.Storage == rootStorage && !binding.IsAliasBorrow
                            && !binding.IsBorrowedOwningValue)
                            aliasesFrameOwner = true;
        }
        if (SourceIsDanglingAliasBorrow(compiler, nv) && !aliasesFrameOwner) return false;
        if (!compiler->IsOwningValueType(nv.TypeAndValue.TypeName)) return false;
        // Same type on both sides (alias-resolved). This arm rewrites the returned value, so a
        // mismatch must fall through to the existing return-type diagnostics untouched.
        if (compiler->ResolveTypeAlias(nv.TypeAndValue.TypeName)
            != compiler->ResolveTypeAlias(compiler->currentFunctionReturnTypeName))
            return false;
        // A whole named local is `movableLocalReturn`'s job; only an indirect lvalue lands here.
        auto* gep = llvm::dyn_cast<llvm::GEPOperator>(nv.Storage);
        if (gep == nullptr) return false;
        if (!nv.FieldName.empty()) return true;
        // An IMPLICIT self-field read (`return b;` inside a method) carries no FieldName at all -
        // GetMemberVariable deliberately omits it - so admit by GEP SHAPE as the store arms do by
        // Storage alone: a two-index access into a struct (a field) or an array (an element).
        if (gep->getNumIndices() == 2
            && (gep->getSourceElementType()->isStructTy()
                || gep->getSourceElementType()->isArrayTy()))
            return true;
        return gep->getNumIndices() == 1 && nv.IsViewElement;
    }

// Walks a field / element GEP chain down to the object it addresses, and answers true only when
// that object is an alloca of this frame. A global, or anything reached through a loaded pointer,
// outlives the return and must keep borrowing.
bool MainListener::FieldPathRootIsFrameLocal(llvm::Value* storage) {
        // The step budget is a belt-and-braces bound on an acyclic use-def walk, so exhausting it
        // answers "copy": a spurious copy costs an allocation, a spurious borrow returns free()d bytes.
        for (int depth = 0; storage != nullptr && depth < 256; ++depth)
        {
            if (llvm::isa<llvm::AllocaInst>(storage)) return true;
            auto* gep = llvm::dyn_cast<llvm::GEPOperator>(storage);
            if (gep == nullptr) return false;
            storage = gep->getPointerOperand();
        }
        return true;
    }

LLVMBackend::NamedVariable MainListener::FinishAssignmentExpressionNamed(
        LLVMBackend::NamedVariable nv, bool savedOwned) {
        if (nv.Primary != nullptr && (nv.TypeAndValue.IsAlias || nv.IsAliasBorrow))
            compilerLLVM->RegisterAliasValue(nv.Primary);
        if (nv.Primary != nullptr && nv.FromOwningTempField && !nv.OwningTempParent)
            compilerLLVM->RegisterTempFieldValue(nv.Primary);
        if (const auto* raw = compilerLLVM->FindRawArrayResult(nv.Primary))
        {
            nv.AllocatedByRawNewArray = true;
            nv.RawArrayLength = raw->Count;
            nv.AllocAlignment = raw->AllocAlign;
            nv.IsOwning = raw->Owns;
        }
        if (compilerLLVM->lastCallReturnsAllocAlign > 0)
            nv.AllocAlignment = compilerLLVM->lastCallReturnsAllocAlign;
        bool exprOwned = compilerLLVM->lastCallReturnsOwned;
        if (exprOwned && NamedVarIsString(nv))
            nv.IsOwningString = true;
        // A plain owned-string VARIABLE read (not a call result) must also carry its owning
        // status, so passing it directly as a `move string` argument TRANSFERS the buffer rather
        // than tripping the defensive heap-copy in CreateOverloadedFunctionCall - that copy would
        // clone the buffer for the callee while the `move` still consumes the source, orphaning
        // the source's original buffer (a leak hit by list/dictionary/hashset add(move s)).
        // Gated on the source variable's record actually owning, and on a whole-variable read
        // (no FieldName) so a struct field access is left to the field-store machinery.
        else if (!nv.IsOwningString && nv.FieldName.empty() && !nv.CallerName.empty()
                 && NamedVarIsString(nv) && compilerLLVM->IsVariableOwningString(nv.CallerName))
            nv.IsOwningString = true;
        // Carry the field-borrow taint on a whole-variable string read (`a.name = tmp` where
        // `tmp` was `string tmp = b.name`) so the field-store reject can catch the laundered
        // field-to-field copy. A struct-field access (FieldName set) is left to the direct
        // field-to-field reject; a call result has no CallerName.
        if (nv.FieldName.empty() && !nv.CallerName.empty() && NamedVarIsString(nv)
            && compilerLLVM->IsVariableBorrowingOwnedString(nv.CallerName))
            nv.BorrowsOwnedString = true;
        // A produced call/join is the result of THIS expression. If its call was borrowing,
        // do not restore an owned-return bit left by an outer receiver call (for example,
        // `makeHolder()->borrowKid()`); that stale bit would make a plain pointer local own the
        // borrowed field. Preserve the saved channel only for a non-produced binding read.
        compilerLLVM->lastCallReturnsOwned = exprOwned
            ? true : (compilerLLVM->IsProducedTempValue(nv.Primary) ? false : savedOwned);
        return nv;
    }

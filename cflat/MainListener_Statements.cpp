#include "MainListener.h"

MainListener::RangeForContext* MainListener::FindActiveRangeForVariable(
        const LLVMBackend::NamedVariable& nv) {
        std::string name = nv.CallerName.empty() ? nv.TypeAndValue.VariableName : nv.CallerName;
        for (auto it = rangeForStack_.rbegin(); it != rangeForStack_.rend(); ++it)
            if (it->variableName == name)
                return &*it;
        return nullptr;
    }

bool MainListener::IsActiveRangeCollectionElement(
        const LLVMBackend::NamedVariable& nv) const {
        if (!nv.IsElementAccess || nv.Storage == nullptr) return false;
        auto contains = [](llvm::Value* value, llvm::Value* base) {
            for (int depth = 0; value != nullptr && depth < 64; ++depth)
            {
                if (value == base) return true;
                auto* gep = llvm::dyn_cast<llvm::GEPOperator>(value);
                if (gep == nullptr) return false;
                value = gep->getPointerOperand();
            }
            return false;
        };
        for (auto it = rangeForStack_.rbegin(); it != rangeForStack_.rend(); ++it)
        {
            if (it->elementStorage != nullptr && nv.Storage == it->elementStorage)
                continue;
            if (contains(nv.Storage, it->collectionStorage)
                || contains(nv.Storage, it->collectionValue))
                return true;
        }
        return false;
    }

void MainListener::ParseBlockItemList(CFlatParser::BlockItemListContext* ctx) {
        auto blockItems = ctx->blockItem();

        // Unreachable-code hint (LSP only): a direct return/break/continue ends the
        // block, so the items after it are dead - but only up to the next label. A
        // `case:`/`default:`/goto label is a fresh entry point, so a switch body (whose
        // arms each end in `break;`) has no dead code at all. Report each dead run and
        // let normal codegen proceed unchanged (the loop below reopens a dead block).
        if (auto* compiler = Compiler(ctx); compiler->HasHintRegionSink())
        {
            auto isLabel = [](CFlatParser::BlockItemContext* item)
            {
                auto* stmt = item->statement();
                return stmt != nullptr && stmt->labeledStatement() != nullptr;
            };

            for (size_t i = 0; i + 1 < blockItems.size(); ++i)
            {
                auto* stmt = blockItems[i]->statement();
                if (!stmt || stmt->jumpStatement() == nullptr) continue;

                size_t deadEnd = i + 1;
                while (deadEnd < blockItems.size() && !isLabel(blockItems[deadEnd]))
                    ++deadEnd;

                if (deadEnd > i + 1)
                {
                    auto* startTok = blockItems[i + 1]->getStart();
                    auto* stopTok  = blockItems[deadEnd - 1]->getStop();
                    int endCol = (int)stopTok->getCharPositionInLine()
                               + (int)stopTok->getText().size();
                    compiler->ReportHintRegion(
                        (int)startTok->getLine(), (int)startTok->getCharPositionInLine(),
                        (int)stopTok->getLine(), endCol,
                        "unreachable code");
                }
                // Resume past the label that revives the block (it can never itself be a
                // direct jump item, so skipping it as a candidate loses nothing).
                i = deadEnd;
            }
        }

        for (const auto& blockItem : blockItems)
        {
            auto decl = blockItem->declaration();
            auto statement = blockItem->statement();
            auto usingDecl = blockItem->usingDeclaration();
            auto destructuring = blockItem->destructuringDeclaration();

            auto* compiler = Compiler(blockItem);
            expectErrorRecoveryBlock_ = nullptr;

            // Dead code after a return/break/continue (typically one nested in a compound
            // block or an `if const` arm, which inline into this block): the insert block is
            // already terminated, so reopen emission in a fresh unreachable block. A
            // `case:`/`default:` label switches blocks itself - a block injected here would be
            // left empty and unterminated - so leave those to the labeled-statement path.
            if (compiler->IsBlockTerminated()
                && !(statement != nullptr && statement->labeledStatement() != nullptr))
                compiler->ReopenAfterTerminator();

            if (decl != nullptr)
            {
                compiler->SetCurrentDebugLocation(decl->getStart()->getLine());
                ParseDeclaration(decl);
            }
            else if (statement != nullptr)
            {
                ParseStatement(statement);
            }
            else if (usingDecl != nullptr)
            {
                compiler->SetCurrentDebugLocation(usingDecl->getStart()->getLine());
                ParseUsingDeclaration(usingDecl);
            }
            else if (destructuring != nullptr)
            {
                compiler->SetCurrentDebugLocation(destructuring->getStart()->getLine());
                ParseDestructuringDeclaration(destructuring);
            }

            // End of a full expression / statement: free owned temporaries not claimed by a named
            // local or move parameter (e.g. the unnamed `a + b` of a chained concat). Like C++.
            compiler->FlushOwnedTemps();
        }
    }

void MainListener::EnsureTupleInstantiated(const std::string& mangledName) {
        if (!instantiatedGenerics.count(mangledName) && !genericStructTemplates.count("tuple"))
            return;  // tuple.cb not imported - nothing to instantiate from
        auto it = tupleTypeArgs.find(mangledName);
        if (it == tupleTypeArgs.end())
            return;
        if (!instantiatedGenerics.count(mangledName))
        {
            QueuePendingInstantiation("tuple", it->second, mangledName);
            instantiatedGenerics.insert(mangledName);
        }
        LLVMBackend::BuilderStateGuard savedState(Compiler());
        ProcessPendingInstantiations();
    }

void MainListener::ParseDestructuringDeclaration(CFlatParser::DestructuringDeclarationContext* ctx) {
        auto* compiler = Compiler(ctx);

        // Evaluate the RHS once
        auto rhsNV = ParseAssignmentExpressionNamed(ctx->assignmentExpression());
        std::string rhsType = rhsNV.TypeAndValue.TypeName;

        // Verify the RHS is a tuple type through the shared mangling owner.
        if (MangledBase(rhsType) != "tuple")
        {
            LogErrorContext(ctx, std::format("Destructuring requires a tuple type, got '{}'",
                SpellType(*compiler, rhsNV.TypeAndValue)));
            return;
        }

        // A tuple reached only via a return type (its producer not yet code-generated, e.g. defined
        // below this destructure) has a registered shell but no fields yet. Instantiate them now
        // from the type args recorded when the shell was named.
        if (compiler->GetDataStructure(rhsType).StructFields.empty())
            EnsureTupleInstantiated(rhsType);

        auto* structType = compiler->GetDataStructure(rhsType).StructType;
        if (!structType)
        {
            LogErrorContext(ctx, std::format("Tuple type '{}' is not fully instantiated",
                SpellType(*compiler, rhsNV.TypeAndValue)));
            return;
        }

        // Load the tuple value into a temporary alloca so we can GEP its fields
        llvm::Value* tupleAlloca = rhsNV.Storage;
        if (!tupleAlloca)
        {
            // RHS was a value not stored - create a temp alloca
            tupleAlloca = compiler->CreateAlloca(structType);
            compiler->builder->CreateStore(LoadNamedVariable(rhsNV), tupleAlloca);
        }

        const auto& structData = compiler->GetDataStructure(rhsType);
        auto entries = ctx->destructuringEntry();

        if (entries.size() != structData.StructFields.size())
        {
            LogErrorContext(ctx, std::format("Destructuring arity mismatch: {} variables for {} fields",
                entries.size(), structData.StructFields.size()));
            return;
        }

        // Declare each variable and load from the corresponding item_i field
        for (size_t i = 0; i < entries.size(); i++)
        {
            auto* entry = entries[i];
            std::string varName = entry->Identifier()->getText();

            std::string fieldName = "item_" + std::to_string(i);
            unsigned fieldIdx = 0;
            for (const auto& f : structData.StructFields)
            {
                if (f.VariableName == fieldName) break;
                fieldIdx++;
            }

            // `_` is the discard wildcard: skip binding this slot entirely. The value stays in
            // the tuple, so the tuple's own destructor frees it (no extra var -> no double-free).
            if (entry->declarationSpecifiers() == nullptr && varName == "_")
                continue;

            // Two entry forms: `T name` (explicit type) or a bare `name` whose type is inferred
            // from the corresponding tuple field. A bare identifier other than `_` always infers.
            LLVMBackend::TypeAndValue declType = entry->declarationSpecifiers() != nullptr
                ? static_cast<LLVMBackend::TypeAndValue>(ParseDeclarationSpecifiers(entry->declarationSpecifiers()))
                : static_cast<LLVMBackend::TypeAndValue>(structData.StructFields[fieldIdx]);

            auto* gep = compiler->CreateStructGEP(structType, tupleAlloca, fieldIdx);
            auto* fieldLLVMType = compiler->GetType(structData.StructFields[fieldIdx]);
            auto* fieldVal = compiler->builder->CreateLoad(fieldLLVMType, gep);

            // Allocate and store into the new variable
            auto* alloca = compiler->CreateAlloca(fieldLLVMType);
            compiler->builder->CreateStore(fieldVal, alloca);

            declType.VariableName = varName;
            LLVMBackend::NamedVariable namedVar;
            namedVar.TypeAndValue = declType;
            namedVar.Storage = alloca;
            namedVar.BaseType = fieldLLVMType;
            compiler->SetStackVariable(varName, namedVar);
            compiler->RecordMoveGenBind(varName); // fresh tuple-destructure binding
        }
    }

void MainListener::CollectCasesFromStatement(CFlatParser::StatementContext* stmt, SwitchContext& ctx) {
        auto* compiler = Compiler(stmt);
        if (!stmt) return;
        auto labeled = stmt->labeledStatement();
        if (!labeled) return;

        bool hasArrow = labeled->FatArrow() != nullptr;

        // Reject mixing C-style (:) and arm-style (=>) cases in the same switch.
        if (hasArrow && !ctx.caseMap.empty() && !ctx.isArmStyle)
            LogErrorContext(labeled, "cannot mix ':' and '=>' cases in the same switch");
        if (!hasArrow && !ctx.caseMap.empty() && ctx.isArmStyle)
            LogErrorContext(labeled, "cannot mix ':' and '=>' cases in the same switch");
        if (hasArrow)
            ctx.isArmStyle = true;

        if (labeled->Case())
        {
            // Arm-style type pointer case: case TypeName* optVar => body
            if (labeled->typeSpecifier() && labeled->pointer())
            {
                std::string typeName = labeled->typeSpecifier()->getText();
                std::string resolvedTypeName = compiler->ResolveQualifiedName(typeName);
                if (compiler->HasInterface(typeName))
                    resolvedTypeName = compiler->ResolveInterfaceName(typeName);
                else
                    resolvedTypeName = compiler->ResolveTypeAlias(resolvedTypeName);
                std::string boundVar = labeled->Identifier() ? labeled->Identifier()->getText() : "";
                bool isStruct = compiler->dataStructures.count(resolvedTypeName) > 0;
                bool isInterface = compiler->HasInterface(resolvedTypeName);
                if (!isStruct && !isInterface)
                    LogErrorContext(labeled, std::format("'{}' is not a known struct or interface type",
                        SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = typeName })));
                if (!ctx.isTypeSwitch && !ctx.caseMap.empty())
                    LogErrorContext(labeled, "cannot mix type cases with constant cases in a switch");
                ctx.isTypeSwitch = true;
                ctx.caseMap[labeled] = { nullptr, nullptr, compiler->CreateBasicBlock("switchTypeCase"), true, resolvedTypeName, boundVar, true };
                return;
            }

            // C-style or arm-style value/type case via constantExpression
            auto constExpr = labeled->constantExpression();
            if (!constExpr)
            {
                LogErrorContext(labeled, "case must have an expression");
                return;
            }

            std::string exprText = constExpr->getText();
            std::string resolvedExprText = compiler->ResolveQualifiedName(exprText);
            if (compiler->HasInterface(exprText))
                resolvedExprText = compiler->ResolveInterfaceName(exprText);
            else
                resolvedExprText = compiler->ResolveTypeAlias(resolvedExprText);
            bool isStruct = compiler->dataStructures.count(resolvedExprText) > 0;
            bool isInterface = compiler->HasInterface(resolvedExprText);

            // _ is a soft wildcard in arm-style switches - register as both default and caseMap entry
            // so ParseStatement can locate this labeled node and emit the arm body with auto-jump.
            if (hasArrow && exprText == "_")
            {
                ctx.defaultBlock = compiler->CreateBasicBlock("switchDefault");
                ctx.caseMap[labeled] = { nullptr, nullptr, ctx.defaultBlock, false, "", "", true };
                return;
            }

            if (isStruct || isInterface)
            {
                if (!ctx.isTypeSwitch && !ctx.caseMap.empty())
                    LogErrorContext(labeled, "cannot mix type cases with constant cases in a switch");
                ctx.isTypeSwitch = true;
                ctx.caseMap[labeled] = { nullptr, nullptr, compiler->CreateBasicBlock("switchTypeCase"), true, resolvedExprText, "", hasArrow };
                if (!hasArrow) CollectCasesFromStatement(labeled->statement(), ctx);
            }
            else
            {
                if (ctx.isTypeSwitch)
                    LogErrorContext(labeled, "cannot mix constant cases with type cases in a switch");

                auto typedVal = ParseConditionalExpression(constExpr->conditionalExpression());
                llvm::Value* rawVal = typedVal.value;
                auto* val = llvm::dyn_cast<llvm::ConstantInt>(rawVal);
                // An enum member (or a const global) reads back as a load from a constant global,
                // not a ConstantInt; fold it with the same lookup `if const` uses.
                if (!val && rawVal != nullptr && rawVal->getType()->isIntegerTy())
                {
                    uint64_t folded = 0;
                    if (TryFoldConstInt(rawVal, folded, &constFoldableGlobals_))
                    {
                        val = llvm::cast<llvm::ConstantInt>(llvm::ConstantInt::get(
                            llvm::cast<llvm::IntegerType>(rawVal->getType()), folded, true));
                        // The label's signedness is the DECLARED type of the global it came from
                        // (an enum resolves to its backing type), not the load's IR type.
                        if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(rawVal))
                            if (auto* gv = llvm::dyn_cast<llvm::GlobalVariable>(ld->getPointerOperand()))
                            {
                                auto nv = compiler->GetGlobalVariableNV(std::string(gv->getName()));
                                LLVMBackend::TypeAndValue probe;
                                probe.TypeName = compiler->ResolveFuncPtrTypeSpelling(nv.TypeAndValue.TypeName);
                                if (probe.IsUnsignedInteger() != -1)
                                    typedVal.isUnsigned = true;
                            }
                        rawVal = val;
                    }
                }
                llvm::Constant* strLit = nullptr;
                if (!val)
                {
                    strLit = llvm::dyn_cast<llvm::Constant>(rawVal);
                    if (strLit && compiler->IsStringLiteralConstant(strLit))
                        ctx.isStringSwitch = true;
                    else
                        LogErrorContext(labeled, "case value must be a constant integer or string literal");
                }
                // An unsuffixed hex literal in [2^31, 2^32) (or a decimal past INT64_MAX) folds to a
                // signed value keeping its bit pattern; a label that IS that literal is the unsigned pattern.
                bool labelIsUnsigned = typedVal.isUnsigned
                    || (val && val->isNegative() && lastNumberLiteralIsBitPattern
                        && val->getBitWidth() == lastNumberLiteralWidth
                        && val->getValue().getZExtValue() == lastNumberLiteralBits);
                ctx.caseMap[labeled] = { val, strLit, compiler->CreateBasicBlock("switchCase"), false, "", "", hasArrow, labelIsUnsigned };
                if (!hasArrow) CollectCasesFromStatement(labeled->statement(), ctx);
            }
        }
        else if (labeled->Default())
        {
            ctx.defaultBlock = compiler->CreateBasicBlock("switchDefault");
            if (!hasArrow) CollectCasesFromStatement(labeled->statement(), ctx);
        }
    }

CFlatParser::PostfixExpressionContext* MainListener::FindFirstCall(antlr4::tree::ParseTree* node) {
        if (node == nullptr) return nullptr;
        if (auto* pf = dynamic_cast<CFlatParser::PostfixExpressionContext*>(node))
            if (!pf->argumentExpressionList().empty())
                return pf;
        for (auto* child : node->children)
            if (auto* found = FindFirstCall(child))
                return found;
        return nullptr;
    }

void MainListener::ScanComparisons(antlr4::tree::ParseTree* node, bool& hasRelational, bool& hasEquality) {
        if (node == nullptr) return;
        if (auto* rel = dynamic_cast<CFlatParser::RelationalExpressionContext*>(node))
            if (rel->shiftExpression().size() > 1) hasRelational = true;   // < <= > >=
        if (auto* eq = dynamic_cast<CFlatParser::EqualityExpressionContext*>(node))
            if (eq->typeCheckExpression().size() > 1) hasEquality = true;  // == !=
        for (auto* child : node->children)
            ScanComparisons(child, hasRelational, hasEquality);
    }

bool MainListener::ConditionIsSentinel(antlr4::tree::ParseTree* node) {
        bool hasRelational = false, hasEquality = false;
        ScanComparisons(node, hasRelational, hasEquality);
        return hasEquality && !hasRelational;
    }

void MainListener::ParseControlledBody(CFlatParser::StatementContext* body) {
        ParseStatement(body);
        if (body != nullptr && body->compoundStatement() == nullptr)
            Compiler(body)->FlushOwnedTemps();
    }

llvm::AllocaInst* MainListener::FrameLocalDataPointer(llvm::Value* data,
                                            std::unordered_set<const llvm::Value*>& seen) {
        if (data == nullptr || !seen.insert(data).second) return nullptr;
        data = data->stripPointerCasts();
        // An element/field address (`arr[0] as IShape`) is frame-local iff its base is.
        while (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(data))
            data = gep->getPointerOperand()->stripPointerCasts();
        // A '?:' arrives as a join of the two arms' data pointers. It dangles if EITHER arm
        // does, so the first frame-local incoming decides it.
        if (auto* phi = llvm::dyn_cast<llvm::PHINode>(data))
        {
            for (unsigned i = 0; i < phi->getNumIncomingValues(); i++)
                if (auto* slot = FrameLocalDataPointer(phi->getIncomingValue(i), seen)) return slot;
            return nullptr;
        }
        if (auto* sel = llvm::dyn_cast<llvm::SelectInst>(data))
        {
            if (auto* slot = FrameLocalDataPointer(sel->getTrueValue(), seen)) return slot;
            return FrameLocalDataPointer(sel->getFalseValue(), seen);
        }
        return llvm::dyn_cast<llvm::AllocaInst>(data);
    }

void MainListener::CollectFatValueFields(llvm::Value* fatValue, unsigned index,
                               std::vector<llvm::Value*>& out,
                               std::unordered_set<const llvm::Value*>& seen) {
        if (fatValue == nullptr || !seen.insert(fatValue).second) return;
        if (auto* phi = llvm::dyn_cast<llvm::PHINode>(fatValue))
        {
            for (unsigned i = 0; i < phi->getNumIncomingValues(); i++)
                CollectFatValueFields(phi->getIncomingValue(i), index, out, seen);
            return;
        }
        if (auto* sel = llvm::dyn_cast<llvm::SelectInst>(fatValue))
        {
            CollectFatValueFields(sel->getTrueValue(), index, out, seen);
            CollectFatValueFields(sel->getFalseValue(), index, out, seen);
            return;
        }
        llvm::Value* agg = fatValue;
        while (auto* iv = llvm::dyn_cast<llvm::InsertValueInst>(agg))
        {
            if (iv->getNumIndices() == 1 && iv->getIndices()[0] == index)
            {
                out.push_back(iv->getInsertedValueOperand());
                return;
            }
            agg = iv->getAggregateOperand();
        }
        // The base of the chain folds to a constant once the vtable is inserted into undef,
        // so the vtable half usually lives here rather than in an insertvalue.
        if (auto* c = llvm::dyn_cast<llvm::Constant>(agg))
            if (auto* elem = c->getAggregateElement(index);
                elem != nullptr && !llvm::isa<llvm::UndefValue>(elem))
                out.push_back(elem);
    }

llvm::AllocaInst* MainListener::FrameLocalDataOfFatValue(llvm::Value* fatValue) {
        std::vector<llvm::Value*> dataPtrs;
        std::unordered_set<const llvm::Value*> seenFat, seenData;
        CollectFatValueFields(fatValue, 1u, dataPtrs, seenFat);
        for (auto* data : dataPtrs)
            if (auto* slot = FrameLocalDataPointer(data, seenData)) return slot;
        return nullptr;
    }

std::string MainListener::VTableOwnerName(llvm::GlobalVariable* vtable, LLVMBackend* compiler) {
        for (auto& [sName, sd] : compiler->dataStructures)
            for (auto& [iName, vt] : sd.VTables) if (vt == vtable) return sName;
        for (auto& [pName, pd] : compiler->programTable)
            for (auto& [iName, vt] : pd.VTables) if (vt == vtable) return pName;
        return "";
    }

std::string MainListener::BoxedStructNameOfFatValue(llvm::Value* fatValue, LLVMBackend* compiler) {
        std::vector<llvm::Value*> vtables;
        std::unordered_set<const llvm::Value*> seen;
        CollectFatValueFields(fatValue, 0u, vtables, seen);
        std::string found;
        for (auto* v : vtables)
        {
            auto* g = llvm::dyn_cast<llvm::GlobalVariable>(v->stripPointerCasts());
            std::string name = g != nullptr ? VTableOwnerName(g, compiler) : std::string();
            if (name.empty() || (!found.empty() && found != name)) return "";
            found = name;
        }
        return found;
    }

// The declared return type as the USER spelled it, for a return diagnostic. The bare
// TypeName drops the pointer / array-view suffix, which reads as a contradiction on `void*`.
static std::string CurrentReturnTypeSpelling(const LLVMBackend* compiler)
{
        if (compiler->currentFunctionReturnTypeName.empty()) return "a value";
        const auto& tv = compiler->currentFunctionReturnTV;
        // A closure/function-pointer return type is stored under its LOWERED name
        // ("__closure_fat_ptr"), which is not writable source - name the writable KIND instead,
        // the same elided spelling the lambda-inference diagnostic uses.
        if (tv.IsFunctionPointer) return tv.IsThinFnPtr() ? "function<...>" : "Lambda<...>";
        return SpellType(*compiler, tv);
    }

static std::string CurrentReturnBaseTypeSpelling(const LLVMBackend* compiler)
{
        if (compiler->currentFunctionReturnTypeName.empty()) return "a value";
        auto tv = compiler->currentFunctionReturnTV;
        tv.Pointer = false;
        tv.ElemPointer = false;
        tv.PointerDepth = 0;
        tv.IsArrayView = false;
        return SpellType(*compiler, tv);
    }

std::string MainListener::ReturnUpcastStructName(const LLVMBackend::NamedVariable& nv) {
        if (!nv.TypeAndValue.TypeName.empty()) return nv.TypeAndValue.TypeName;
        if (auto* st = llvm::dyn_cast_or_null<llvm::StructType>(nv.BaseType)) return st->getName().str();
        return {};
    }

void MainListener::LogInterfaceReturnDangle(antlr4::ParserRuleContext* ctx, const std::string& structName,
                                  const std::string& ifaceName) {
        if (structName.empty())
        {
            LogErrorContext(ctx, std::format("cannot return a local value as interface '{}' - "
                "the interface fat pointer would dangle once this function returns; "
                "allocate the object on the heap and return the pointer",
                SpellType(*Compiler(ctx), LLVMBackend::TypeAndValue{ .TypeName = ifaceName })));
            return;
        }
        LLVMBackend::TypeAndValue structType{ .TypeName = structName };
        LLVMBackend::TypeAndValue interfaceType{ .TypeName = ifaceName };
        LogErrorContext(ctx, std::format("cannot return local value '{}' as interface '{}' - "
            "the interface fat pointer would dangle once this function returns; "
            "allocate on the heap ('new {}') and return the pointer",
            SpellType(*Compiler(ctx), structType), SpellType(*Compiler(ctx), interfaceType),
            SpellType(*Compiler(ctx), structType)));
    }

/*
 * The whole `return <expr>;` lowering: ownership classification, the escape / dangle
 * gates, owned-temp flushing and the interface upcast. An expression-body lambda
 * (`=> expr`) is `=> { return expr; }`, so it calls this instead of a bare
 * CreateReturnCall - otherwise none of the above runs inside its body.
 */
void MainListener::EmitReturnExpression(antlr4::ParserRuleContext* errCtx,
                                        CFlatParser::AssignmentExpressionContext* assignExpr,
                                        const std::string& retText,
                                        bool defaultValue) {
        auto* compiler = Compiler(errCtx);
        // Evaluate via NV path so we can inspect bond info alongside ownership.
        LLVMBackend::NamedVariable returnNV;
        // Thread a function<> return type into a returned lambda literal so the
        // lambda's invoker is typed from the function's declared return type
        // (`return () => {...};`) instead of defaulting to void. Mirrors the
        // declaration/argument lambda-context threading.
        // Capture the function-pointer return type BEFORE parsing the return
        // expression - lowering a returned lambda calls createFunctionBlock, which
        // overwrites currentFunctionReturnTV with the lambda's own return type.
        LLVMBackend::TypeAndValue returnFnPtrTV;
        if (compiler->currentFunctionReturnTV.IsFunctionPointer)
        {
            lambdaExpectedType = compiler->currentFunctionReturnTV;
            returnFnPtrTV = compiler->currentFunctionReturnTV;
        }
        // Inbound alloc-align channel for `return new T[n];`: a function whose return
        // type declares `alignas(_, N)` hands the allocation alignment down to a DIRECT
        // `new` in the return, exactly as a decl-init does. Indirect return shapes are
        // rejected by the check below rather than silently under-aligned.
        if (assignExpr != nullptr
            && compiler->currentFunctionReturnTV.AllocAlignValue > LLVMBackend::kDefaultNewAlign
            && AsDirectNew(assignExpr) != nullptr)
            compiler->pendingInitAllocAlign = compiler->currentFunctionReturnTV.AllocAlignValue;
        // The enclosing function's RETURN type is the context for an immediately-invoked literal
        // in the returned expression, exactly as a declarator's type is for its initializer.
        std::optional<DeclExpectedTypeScope> returnExpectedScope;
        returnExpectedScope.emplace(&declExpectedType, compiler->currentFunctionReturnTV);
        if (assignExpr != nullptr)
            ArmArrayNewDesugar(assignExpr, compiler->currentFunctionReturnTV);
        if (assignExpr != nullptr)
            returnNV = ParseAssignmentExpressionNamed(assignExpr, ResultUse::ReturnOperand);
        else if (defaultValue)
        {
            // `return default;` - the same emitter `T x = default;` and a `default` ternary arm
            // use, so a struct runs its field initializers instead of being zero-filled.
            LLVMBackend::DeclTypeAndValue dtv;
            static_cast<LLVMBackend::TypeAndValue&>(dtv) = compiler->currentFunctionReturnTV;
            returnNV.TypeAndValue = compiler->currentFunctionReturnTV;
            returnNV.Primary = GenerateDefaultValue(dtv);
        }
        returnExpectedScope.reset();
        arrayNewDesugarCtx = nullptr;
        compiler->pendingInitAllocAlign = 0;  // one-shot
        lambdaExpectedType = {};

        if (compiler->HasRawNewArrayProvenance(returnNV)
            && returnNV.TypeAndValue.Pointer
            && !compiler->currentFunctionReturnTV.Pointer
            && compiler->IsCoreUniqueType(compiler->currentFunctionReturnTV.TypeName))
        {
            std::string sourceName;
            if (assignExpr != nullptr)
                if (auto* move = TopLevelMoveExpression(assignExpr);
                    move != nullptr && move->unaryExpression() != nullptr)
                    sourceName = move->unaryExpression()->getText();
            if (sourceName.empty())
                sourceName = returnNV.CallerName.empty()
                    ? returnNV.TypeAndValue.VariableName : returnNV.CallerName;
            if (sourceName.empty()) sourceName = retText;
            std::string pointee = MangledGenericArgument(
                *compiler, compiler->currentFunctionReturnTV.TypeName);
            std::string shownPointee = SpellType(*compiler,
                LLVMBackend::TypeAndValue{ .TypeName = pointee });
            LogErrorContext(errCtx, std::format(
                "cannot return owning heap array '{}' as '{}': unique<T> does not own arrays - "
                "return 'move {}*' instead",
                sourceName,
                SpellType(*compiler, compiler->currentFunctionReturnTV),
                shownPointee));
        }

        // A raw heap array handed back through a bare `T*` / `T[]` return loses its element count
        // at the boundary (only `move T*` carries one), so the caller can never free it correctly.
        // `alias T*` is the documented hand-managed spelling and stays legal.
        if (returnNV.TypeAndValue.Pointer
            && compiler->currentFunctionReturnTV.Pointer
            && !compiler->currentFunctionReturnTV.IsAlias
            && !compiler->ReturnCarriesRawArrayCount(compiler->currentFunctionReturnTV)
            && (compiler->HasRawNewArrayProvenance(returnNV)
                || compiler->FindRawArrayResult(returnNV.Primary) != nullptr))
        {
            std::string sourceName = returnNV.CallerName.empty()
                ? returnNV.TypeAndValue.VariableName : returnNV.CallerName;
            if (sourceName.empty()) sourceName = "from 'new'";
            else sourceName = "'" + sourceName + "'";
            // The suggestions name the ELEMENT type: spelling the return type here would carry
            // its '[]' / '*' declarator into 'array<Y[]>' and 'move Y[][]'.
            std::string elemShown = CurrentReturnBaseTypeSpelling(compiler);
            LogErrorContext(errCtx, compiler->LocalizeMessage(
                "cannot return the heap array {} as '{}': the element count is lost at the return - "
                "return 'array<{}>' or 'move {}[]' (which carries the count) instead",
                { sourceName, CurrentReturnTypeSpelling(compiler), elemShown, elemShown }));
        }

        if (returnNV.ContainsBondedClosure)
            LogErrorContext(errCtx,
                "cannot return a holder containing a bonded closure - the closure would outlive its captured local");

        // GetFunctionType hands an `extern` (C ABI) function the by-value return shape, so the
        // reference ABI holds only where the emitted function really returns a pointer. Comparing
        // against the ABI type keeps this site and GetFunctionType from drifting apart.
        auto* currentFn = compiler->builder->GetInsertBlock() != nullptr
            ? compiler->builder->GetInsertBlock()->getParent() : nullptr;
        const bool aliasRefReturn = compiler->currentFunctionReturnTV.IsAlias
            && !compiler->currentFunctionReturnTV.Pointer
            && currentFn != nullptr
            && currentFn->getReturnType()
                == compiler->GetFunctionReturnABIType(compiler->currentFunctionReturnTV);

        // A non-pointer alias return is a reference to the source slot. It must have storage,
        // and that storage must outlive this frame; returning a by-value snapshot would silently
        // restore the old broken ABI and returning an alloca would dangle immediately.
        if (aliasRefReturn)
        {
            if (returnNV.Storage == nullptr)
                LogErrorContext(errCtx,
                    "cannot return an 'alias' value without addressable storage; return a live "
                    "lvalue rather than a temporary value");
            if (PointsIntoStackFrame(returnNV.Storage))
            {
                // An alias LOCAL bound to a frame-local owner is still frame storage, so the
                // borrow marker cannot excuse it - only a caller-owned by-value param can.
                if (!IsBorrowedByValueParamBinding(compiler, returnNV))
                {
                    if (NamedVarIsString(returnNV))
                        LogErrorContext(errCtx,
                            "cannot return an 'alias' string value that borrows frame-local storage; "
                            "the buffer would dangle when the function returns");
                    else
                        LogErrorContext(errCtx,
                            "cannot return an 'alias' value that borrows frame-local storage; "
                            "the referenced object would dangle when the function returns");
                }
            }
        }

        // Implicit move on `return <local>;` (C++-style, narrow). Triggers only when the
        // return expression is a BARE IDENTIFIER naming a local (or by-value parameter)
        // whose owning value-struct type matches the function's return type; then it is
        // treated exactly as `return move <local>;` - snapshot the value, zero the source
        // so its scope-exit destructor is a no-op, and hand ownership to the caller. A
        // borrowing local (alias / borrowed field / borrowed by-value param) is excluded so
        // the alias path still wins and no second owner is created (which would double-free).
        if (assignExpr != nullptr)
        {
            std::string retName = retText;
            LLVMBackend::NamedVariable localNV = compiler->GetScopedLocalOrArgument(retName);
            bool movableLocalReturn =
                !retName.empty()
                && !localNV.TypeAndValue.TypeName.empty()
                && !localNV.TypeAndValue.Pointer && !localNV.TypeAndValue.ElemPointer
                && localNV.Storage != nullptr
                && compiler->IsDataStructure(localNV.TypeAndValue.TypeName)
                && compiler->IsOwningValueType(localNV.TypeAndValue.TypeName)
                && localNV.TypeAndValue.TypeName == compiler->currentFunctionReturnTypeName
                && !compiler->currentFunctionReturnTV.Pointer
                && !localNV.IsAliasBorrow && !localNV.BorrowsOwnedString
                && localNV.BorrowedUniqueField.empty()
                && !compiler->IsBorrowStringParamStorage(localNV.Storage)
                && !IsBorrowedStructParameter(compiler, retName);
            if (movableLocalReturn)
            {
                llvm::Value* snapshot = LoadNamedVariable(localNV);
                if (llvm::Type* st = compiler->GetType(localNV.TypeAndValue))
                    compiler->builder->CreateStore(
                        llvm::ConstantAggregateZero::get(st), localNV.Storage);
                compiler->lastOwningResult = true;
                returnNV.Primary = snapshot;
                returnNV.Storage = nullptr;
                returnNV.CallerName.clear();
                returnNV.IdentifierLine = 0;
            }
        }

        // Returning a raw `T*` as a `T[]` return type would forge the noalias
        // contract (a view must span a whole allocation). Decay T[]->T* is fine.
        if (compiler->currentFunctionReturnIsArrayView
            && returnNV.TypeAndValue.Pointer && !returnNV.TypeAndValue.IsArrayView)
            LogErrorContext(errCtx, "cannot return a raw pointer 'T*' as an array-view 'T[]' - "
                "a view must span a whole allocation (from 'new T[n]' or another 'T[]'); "
                "the 'T[] -> T*' decay is one-way");
        // Return leg of the code-value store gate: `return w;` from a `Rec*`
        // function handed the caller a code address to write through.
        if (compiler->CodeValueIntoDataDestination(
                returnNV, compiler->currentFunctionReturnTV))
        {
            const auto& retTV = compiler->currentFunctionReturnTV;
            LogErrorContext(errCtx, compiler->DescribeCodeValueIntoData(
                CodeValueDestSpelling(retTV), "return", CodeValueCastAdvice(retTV)));
        }
        // A desugared unique<T> local returned through an owning interface return releases its
        // raw pointee for the existing interface boxing path. The wrapper slot is nulled first,
        // so its normal scope-exit destructor remains harmless.
        if (compiler->currentFunctionReturnTV.IsInterface
            && !returnNV.TypeAndValue.Pointer
            && compiler->IsCoreUniqueType(returnNV.TypeAndValue.TypeName)
            && returnNV.Storage != nullptr)
        {
            const auto& uniqueData = compiler->GetDataStructure(returnNV.TypeAndValue.TypeName);
            for (size_t i = 0; i < uniqueData.StructFields.size(); i++)
                if (uniqueData.StructFields[i].VariableName == "_p")
                {
                    auto* pointerField = compiler->CreateStructGEP(
                        returnNV.BaseType, returnNV.Storage, (uint32_t)i);
                    auto* raw = compiler->CreateLoad(pointerField);
                    compiler->builder->CreateStore(
                        llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(raw->getType())),
                        pointerField);
                    compiler->RecordNullSet(returnNV.CallerName);
                    compiler->lastOwningResult = true;
                    returnNV.Primary = raw;
                    returnNV.TypeAndValue.TypeName = MangledGenericArgument(
                        *compiler, returnNV.TypeAndValue.TypeName);
                    returnNV.TypeAndValue.Pointer = true;
                    returnNV.TypeAndValue.IsMove = true;
                    returnNV.IsOwning = true;
                    break;
                }
        }
        bool coreUniqueRawReturn = !aliasRefReturn
            && compiler->IsCoreUniqueToRawPointer(returnNV, compiler->currentFunctionReturnTV);
        auto right = coreUniqueRawReturn
            ? compiler->CreateCoreUniqueRawPointerCall(returnNV, compiler->currentFunctionReturnTV)
            : (aliasRefReturn ? returnNV.Storage : LoadNamedVariable(returnNV));
        if (coreUniqueRawReturn)
            returnNV.TypeAndValue = compiler->currentFunctionReturnTV;

        // A pointer-form `unique T*` return was desugared to a core unique<T> value by the
        // declaration parser. Adopt a raw pointer only after proving that this expression owns
        // it; CreateCoreUniqueFromRawPointerCall deliberately treats an unmarked rvalue call as
        // an ownership handoff, which is wrong for a borrowed plain-pointer call result.
        const bool coreUniqueValueReturn = !aliasRefReturn
            && !compiler->currentFunctionReturnTV.Pointer
            && compiler->IsCoreUniqueType(compiler->currentFunctionReturnTV.TypeName);
        const bool rawPointerReturn = coreUniqueValueReturn
            && right != nullptr && right->getType()->isPointerTy()
            && (returnNV.TypeAndValue.Pointer
                || llvm::isa<llvm::ConstantPointerNull>(right));
        if (rawPointerReturn)
        {
            const bool returnedNull = llvm::isa<llvm::ConstantPointerNull>(right);
            const bool sourceBorrowed = returnNV.IsBorrowed
                || returnNV.IsAliasBorrow
                || returnNV.TypeAndValue.IsAlias
                || returnNV.BorrowsOwnedElement
                || returnNV.BorrowsOwningLocal;
            const bool sourceOwned = compiler->lastCallReturnsOwned
                || compiler->lastOwningResult
                || returnNV.IsOwning
                || returnNV.IsExplicitMove
                || returnNV.TypeAndValue.IsMove
                || compiler->IsOwnedNewTemp(right)
                || compiler->IsOwningPtrTempValue(right)
                || compiler->IsMovedOutPtrValue(right)
                || (!returnNV.CallerName.empty()
                    && compiler->IsVariableOwning(returnNV.CallerName));
            if (!returnedNull && (sourceBorrowed || !sourceOwned))
            {
                const std::string sourceName = LLVMBackend::BorrowedSourceName(returnNV).empty()
                    ? retText : LLVMBackend::BorrowedSourceName(returnNV);
                LogErrorContext(errCtx, std::format(
                    "cannot return borrowed value '{}' as '{}'; the return type owns the "
                    "pointee - use 'new', a move source, or a move-returning call",
                    sourceName.empty() ? "this expression" : sourceName,
                    SpellType(*compiler, compiler->currentFunctionReturnTV)));
            }
            if (returnedNull)
            {
                LLVMBackend::DeclTypeAndValue defaultType;
                static_cast<LLVMBackend::TypeAndValue&>(defaultType) =
                    compiler->currentFunctionReturnTV;
                right = GenerateDefaultValue(defaultType);
            }
            else
            {
                auto ctorArg = returnNV;
                ctorArg.Primary = right;
                ctorArg.BaseType = right->getType();
                ShapeCoreUniqueCtorArg(
                    compiler, ctorArg, compiler->currentFunctionReturnTV.TypeName, right);
                right = compiler->CreateCoreUniqueFromRawPointerCall(
                    ctorArg, compiler->currentFunctionReturnTV);
                compiler->ConsumeOwnedNewTemp(returnNV.Primary);
            }
            returnNV.TypeAndValue = compiler->currentFunctionReturnTV;
            returnNV.Primary = right;
            returnNV.Storage = nullptr;
            returnNV.CallerName.clear();
            returnNV.FieldName.clear();
            returnNV.IsOwning = true;
            returnNV.IsExplicitMove = false;
        }
        // A conditional of desugared unique<T> values yields the wrapper by value. Unwrap its
        // selected pointee for the existing owning-interface return boxing path.
        if (compiler->currentFunctionReturnTV.IsInterface
            && right != nullptr && right->getType()->isStructTy()
            && compiler->IsCoreUniqueType(returnNV.TypeAndValue.TypeName))
        {
            const auto& uniqueData = compiler->GetDataStructure(returnNV.TypeAndValue.TypeName);
            for (size_t i = 0; i < uniqueData.StructFields.size(); i++)
                if (uniqueData.StructFields[i].VariableName == "_p")
                {
                    right = compiler->builder->CreateExtractValue(
                        right, {(unsigned)i}, "unique_return_ptr");
                    returnNV.TypeAndValue.TypeName = MangledGenericArgument(
                        *compiler, returnNV.TypeAndValue.TypeName);
                    returnNV.TypeAndValue.Pointer = true;
                    returnNV.TypeAndValue.IsMove = true;
                    returnNV.IsOwning = true;
                    compiler->lastOwningResult = true;
                    break;
                }
        }
        // A string LITERAL is a 'const char*', never a 'T*' - the RETURN leg of the same gate the
        // declarator, `=`, brace-init, field-default and argument sites apply.
        RejectStringLiteralIntoStructPointer(errCtx, compiler->currentFunctionReturnTV, right,
                                             "the return value");
        // Coerce the returned value to the function-pointer return type (thin vs
        // fat): a named function, a thin function<> value, or a fat closure value.
        if (right && returnFnPtrTV.IsFunctionPointer)
        {
            // The declared RETURN type is the crossing here: it cannot adopt an inferred sink,
            // so a consuming closure leaving through it must spell the sink.
            int lostSink = compiler->FindLostClosureSinkParam(returnFnPtrTV, returnNV.TypeAndValue);
            if (lostSink >= 0)
                LogErrorContext(errCtx, compiler->DescribeLostClosureSink(
                    returnFnPtrTV, (size_t)lostSink, "the declared return type"));
            right = compiler->CoerceToFuncPtrReturn(right, returnFnPtrTV, returnNV);
        }
        ProcessPlusPlus();

        /*
         * VOID-ness of the `ret` operand and of the function must agree, or the module
         * verifier rejects the whole module with no source location at all. C keeps exactly
         * one crossing legal - `void f() { return g(); }` on a VOID g - which evaluates the
         * call for its effects and then returns nothing; right is nulled so the CreateRetVoid
         * at the bottom emits it, while every flush below still runs for the argument temps.
         * Only a PROVEN mismatch is rejected. An 'auto' return type is emitted against an i64
         * PLACEHOLDER and unified after the body, so its LLVM return type answers nothing -
         * the void-typed expression is still proven, but the message must say 'auto'.
         */
        bool autoReturn = compiler->IsAutoReturnCaptureActive();
        bool abiReturnsValue = compiler->currentFunctionAbiRecipe.hasLowering
            && compiler->currentFunctionAbiRecipe.retSlot.kind != LLVMBackend::AbiSlot::Direct;
        bool fnReturnsVoid = !autoReturn && compiler->currentFunction != nullptr
            && compiler->currentFunction->getReturnType()->isVoidTy() && !abiReturnsValue;
        // Void-ness reaches here two ways. A void CALL is a void-typed value. A void CLOSURE call
        // and a `(void)` cast both yield no LLVM value at all, so their void-ness rides the
        // NamedVariable instead (`int f() { return g(); }` on a `Lambda<void()>` g). The
        // NamedVariable arm resolves through `using V = void;` - the mirror of the call-site
        // gate, which would otherwise see the alias spelling and hand a null operand to the ret.
        bool returnExprIsVoid =
            (right != nullptr && right->getType()->isVoidTy())
            || (right == nullptr && !returnNV.TypeAndValue.Pointer
                && compiler->ResolveTypeAlias(returnNV.TypeAndValue.TypeName) == "void");
        if (fnReturnsVoid && returnExprIsVoid)
        {
            right = nullptr;
        }
        else if (autoReturn && returnExprIsVoid)
        {
            // Auto inference treats `return voidExpr;` exactly like bare `return;`: both
            // contribute a void site. Do not retain a void CallInst as the captured value,
            // because finalization replaces a null site with CreateRetVoid.
            right = nullptr;
        }
        else if (fnReturnsVoid && right != nullptr)
        {
            // A lambda that no function<>/Lambda<> context reached has "void" as an INFERRED
            // fallback, not a declaration - saying it "returns void" would be false. This
            // LogErrorContext throws, so the specialized wording wins over the general one.
            if (lambdaReturnInferredFn_ != nullptr
                && lambdaReturnInferredFn_ == compiler->currentFunction)
                LogErrorContext(errCtx, std::format(
                    "cannot infer the return type of lambda '{}': its body returns a value "
                    "but no 'function<...>' or 'Lambda<...>' type reaches it here; bind the "
                    "lambda to a typed variable or parameter and call it through that",
                    lambdaReturnInferredName_));
            LogErrorContext(errCtx,
                "cannot return a value from a function whose return type is 'void' - "
                "drop the value ('return;'), or declare a non-void return type");
        }
        else if (returnExprIsVoid)
        {
            LogErrorContext(errCtx, std::format(
                "cannot return a 'void' expression from a function that returns '{}' - "
                "the expression produces no value; call it as a statement and return a value",
                autoReturn ? "auto" : CurrentReturnTypeSpelling(compiler)));
        }

        // Keep representation-changing return paths below (string wrapping, interface boxing,
        // closure coercion) intact, but reject scalar/aggregate crossings that Upconvert cannot
        // repair before they become a locationless LLVM return-type verifier failure.
        if (!autoReturn && right != nullptr && compiler->currentFunction != nullptr)
        {
            auto* returnTy = compiler->currentFunction->getReturnType();
            if (abiReturnsValue)
                returnTy = compiler->currentFunctionAbiRecipe.retSlot.structTy;
            auto* stringTy = llvm::StructType::getTypeByName(*compiler->context, "string");
            bool aggregateFromScalar = returnTy->isStructTy() && returnTy != stringTy
                && !right->getType()->isStructTy() && !right->getType()->isPointerTy();
            bool scalarFromPointer = returnTy->isIntegerTy() && right->getType()->isPointerTy();
            if (aggregateFromScalar || scalarFromPointer)
                LogErrorContext(errCtx, std::format(
                    "cannot return a value of type '{}' from a function that returns '{}'",
                    returnNV.TypeAndValue.TypeName.empty() ? std::string("this expression")
                                                             : SpellType(*compiler, returnNV.TypeAndValue),
                    CurrentReturnTypeSpelling(compiler)));
        }

        /*
         * A '?:' join of concrete implementer pointers carries no NamedVariable
         * TypeName, so the interface-return upcast further down cannot see a class
         * to box and a bare `ptr` would reach the `ret`. Box each arm in its own
         * branch HERE - before the ownership and dangle checks - so every one of
         * them inspects the fat pointer, exactly as they already do for the
         * `return c ? (x as I) : (y as I);` spelling. The helper bails to nullptr
         * for anything that is not a pointer '?:' targeting this interface, so the
         * normal path is untouched; it only reports an armFailure for a join that
         * genuinely targets the interface and cannot be boxed, which today reaches
         * the verifier as a raw type mismatch with no source location.
         */
        if (auto* fatTy = compiler->GetFatPtrType();
            right != nullptr && fatTy != nullptr && compiler->currentFunction != nullptr
            && compiler->currentFunction->getReturnType() == fatTy
            && right->getType()->isPointerTy()
            && !returnNV.TypeAndValue.IsInterface
            && (ReturnUpcastStructName(returnNV).empty()
                || compiler->FindNullCoalesceJoin(right) != nullptr
                || llvm::isa<llvm::PHINode>(right)))
        {
            std::string ternaryArmFailure;
            std::string joinSpelling = "?:";
            bool ternaryArmNotOwned = false;
            const std::string& ifaceName = compiler->currentFunctionReturnTypeName;
            if (auto* fat = UpcastPointerJoinToInterface(
                    right, ifaceName, &ternaryArmFailure, &joinSpelling,
                    compiler->currentFunctionReturnsOwned, &ternaryArmNotOwned))
                right = fat;
            else if (ternaryArmNotOwned)
                LogErrorContext(errCtx, "function declares 'move' return type but returned expression is not owned - value must come from 'new', a move parameter, or another move-returning function");
            else if (!ternaryArmFailure.empty())
                LogErrorContext(errCtx, std::format(
                    "cannot convert '{}' arm to interface '{}': {}",
                    joinSpelling,
                    SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = ifaceName }),
                    ternaryArmFailure));
        }

        // String ownership transfer (runtime-bit model). Classify the returned
        // value once, before the owned-return flags below are cleared.
        //  - A whole string LOCAL read (its NamedVariable.Storage is an alloca, not
        //    a field/element GEP) that does not itself borrow an owned field is
        //    MOVED to the caller by CreateReturnCall (which zeroes the source), so
        //    its OWNED bit must stay set.
        //  - A genuinely-owned temp / new / owned call result keeps its bit too.
        //  - Anything else returned by a plain (non-`move`) string function is a
        //    BORROW of a location someone else owns (a field/element read such as
        //    list.get's `return _data[i]`, or a local that borrows an owned field).
        //    Clear the OWNED bit so the caller's always-run string destructor does
        //    not free a buffer the real owner still holds.
        bool returnExprOwned = compiler->IsOwningValue(right)
            || compiler->lastCallReturnsOwned
            || compiler->lastOwningResult
            || returnNV.IsOwningString;
        bool returnIsWholeLocal = returnNV.FieldName.empty()
            && returnNV.Storage != nullptr
            && llvm::isa<llvm::AllocaInst>(returnNV.Storage)
            && !returnNV.BorrowsOwnedString
            && !returnNV.IsRangeForBorrow;
        // A direct call result carries an authoritative runtime OWNED bit: the callee
        // already cleared it for a borrow return (list.get's `return _data[i]`) or left
        // it set for an owned return (`return "k" + s;`). Re-clearing here would orphan
        // a buffer the caller must free - the leak in `return makeOwned();`. Genuine
        // borrows are loads from storage (field/element GEP), never a CallInst.
        bool returnIsCallResult = right != nullptr && compiler->IsProducedTempValue(right);
        // A borrow string PARAMETER (`string echo(string s){return s;}`) has an
        // alloca Storage so it looks like a movable whole-local, but the frame does
        // not own its buffer - returning it must clear the OWNED bit so the caller
        // gets a BORROW, not a second owner of the caller's buffer (double-free).
        bool returnIsBorrowStringParam =
            compiler->IsBorrowStringParamStorage(returnNV.Storage);
        bool clearReturnedStringBorrowBit =
            NamedVarIsString(returnNV)
            && !compiler->currentFunctionReturnsOwned
            && !returnExprOwned
            && (!returnIsWholeLocal || returnIsBorrowStringParam)
            && !returnIsCallResult;
        // Same borrow classification for a STRUCT taken from a collection the
        // function does not own (e.g. `alias T get` returning `_data[i]`). The
        // shallow copy handed back carries the OWNED bit of every owning string
        // (or nested-struct) field; a copy that escapes alias compile-time tracking
        // - notably a for-in loop variable - is destructed and would free buffers
        // the container still owns. Clear those field bits so any such destruct is
        // a no-op (the container keeps its single owner). A whole-local move-out or
        // a `move`-returning function keeps its bits so ownership transfers.
        bool clearReturnedStructBorrowBits =
            right != nullptr && right->getType()->isStructTy()
            && !NamedVarIsString(returnNV)
            && compiler->IsOwningValueType(returnNV.TypeAndValue.TypeName)
            && !compiler->currentFunctionReturnsOwned
            && !returnExprOwned
            && !returnIsWholeLocal
            && !returnIsCallResult;

        if (compiler->currentFunctionReturnsOwned && right != nullptr
            && !llvm::isa<llvm::Constant>(right)
            && right->getType()->isPointerTy())
        {
            bool returnIsOwned = compiler->IsOwningValue(right)
                || compiler->lastCallReturnsOwned
                || compiler->lastOwningResult;
            // A `move` of a BORROW sets lastOwningResult like any real transfer, so
            // the test above passes it; the ledger is the surviving provenance.
            std::string returnedBorrowOrigin;
            if (compiler->IsMovedBorrowedPtrValue(right, &returnedBorrowOrigin))
            {
                // `move <origin>` is the remedy only when the value IS the parameter
                // (possibly copied). Through a FIELD it names a different object.
                std::string remedy = compiler->IsMovedBorrowedThroughField(right)
                    ? std::format("Move the FIELD itself once '{}' is owned here, or "
                                  "drop 'move' from the return type.",
                                  returnedBorrowOrigin)
                    : std::format("Declare the source parameter 'move {}' to take "
                                  "ownership, or drop 'move' from the return type.",
                                  returnedBorrowOrigin);
                LogErrorContext(errCtx, std::format(
                    "function declares a 'move' return type, but the returned expression "
                    "moves a value that only borrows through parameter '{}' - the move "
                    "nulls this frame's copy while the caller still owns the pointee, so "
                    "the caller would free it twice. {}",
                    returnedBorrowOrigin, remedy));
            }
            if (!returnIsOwned)
                LogErrorContext(errCtx, "function declares 'move' return type but returned expression is not owned - value must come from 'new', a move parameter, or another move-returning function");
        }

        // Same ownership rule as above, widened to BY-VALUE STRUCT returns. A
        // borrowed by-value struct parameter is not owned, so handing it back as a
        // 'move' result makes the caller a second owner of the same unique pointee.
        // A 'move S' struct-VALUE return is deliberately not `currentFunctionReturnsOwned`
        // (see the NOTE on ComputeReturnsOwned), so key off the declared return type.
        if (compiler->currentFunctionReturnTV.IsMove
            && !compiler->currentFunctionReturnTV.Pointer
            && right != nullptr
            && compiler->IsOwningValueType(returnNV.TypeAndValue.TypeName)
            && IsBorrowedStructParameter(compiler, returnNV.CallerName))
        {
            LogErrorContext(errCtx, "function declares 'move' return type but returned expression is not owned - value must come from 'new', a move parameter, or another move-returning function");
        }

        // Plain 'return h' of a 'move' owning-struct PARAMETER copies its owning bits
        // into the caller's return slot without releasing the parameter, so both the
        // returned value and the expiring parameter free the same storage (double-free).
        // An explicit 'return move h' detaches the source (returnExprOwned, empty
        // CallerName), so it is not flagged. An owned LOCAL returned plain transfers
        // correctly and is not a parameter, so it is excluded too.
        if (compiler->currentFunctionReturnTV.IsMove
            && !compiler->currentFunctionReturnTV.Pointer
            && right != nullptr
            && !returnExprOwned
            && IsMoveOwningStructParameter(compiler, returnNV.CallerName))
        {
            LogErrorContext(errCtx, std::format(
                "cannot return 'move' parameter '{}' by a plain 'return' - it copies the "
                "parameter's owning fields without releasing it, so both the returned value "
                "and the expiring parameter free the same storage (double-free). Write "
                "'return move {}' to transfer ownership to the caller.",
                returnNV.CallerName, returnNV.CallerName));
        }

        // `return new T();` where the declared return type is the VALUE struct 'T'.
        // The operand is a 'T*' against a struct return type - LLVM rejects the
        // module with no source location, so diagnose it here instead.
        if (right != nullptr && returnNV.TypeAndValue.Pointer
            && compiler->currentFunction != nullptr
            && compiler->currentFunction->getReturnType()->isStructTy()
            && !returnNV.TypeAndValue.TypeName.empty()
            && returnNV.TypeAndValue.TypeName == compiler->currentFunctionReturnTypeName)
        {
            LogErrorContext(errCtx, std::format(
                "cannot return pointer '{}' from a function declared to return value type '{}' - "
                "declare the return type '{}*' (or 'move {}*' to transfer ownership to the caller), "
                "or dereference the pointer to return a copy.",
                SpellType(*compiler, returnNV.TypeAndValue),
                CurrentReturnTypeSpelling(compiler),
                CurrentReturnTypeSpelling(compiler),
                CurrentReturnTypeSpelling(compiler)));
        }

        if (ReturnLeaksOwnershipIntoInterface(right, compiler))
        {
            LogErrorContext(errCtx, std::format(
                "returning a heap object boxed into interface '{}' from a non-'move' function "
                "transfers ownership the caller cannot see - it will leak. Declare the return "
                "type 'move {}' so the caller knows to 'delete' it.",
                CurrentReturnTypeSpelling(compiler),
                CurrentReturnTypeSpelling(compiler)));
        }

        // A FRESH ALLOCATION handed back through a BARE pointer return type gives the
        // caller no signal that it owns the result, so a forgotten 'delete' is a silent
        // leak. Require the intent to be explicit: 'move T*' transfers ownership (the
        // caller adopts with `unique T* x = f();`), 'alias T*' opts out of ownership
        // tracking and the caller frees by hand. Scoped to the DIRECT `return new T();`
        // form - indirect provenance (`T* h = new T(); return h;`) is not tracked here.
        if (assignExpr != nullptr && AsDirectNew(assignExpr) != nullptr
            && compiler->currentFunctionReturnTV.Pointer
            && !compiler->currentFunctionReturnTV.IsMove
            && !compiler->currentFunctionReturnTV.IsAlias
            && !compiler->currentFunctionReturnsOwned
            && !compiler->currentFunctionReturnIsArrayView
            && compiler->currentFunction != nullptr
            && compiler->currentFunction->getReturnType()->isPointerTy())
        {
            bool lambdaInvoker = compiler->currentFunction != nullptr
                && compiler->currentFunction->getName().contains("__lambda_");
            if (lambdaInvoker)
            {
                LogErrorContext(errCtx, std::format(
                    "returning a fresh allocation from a lambda with the bare pointer return type '{}*' "
                    "gives the caller no ownership signal - declare a named function with return type "
                    "'move {}*' and call it from the lambda, or use a named function with return type "
                    "'alias {}*' for manual ownership",
                    CurrentReturnBaseTypeSpelling(compiler),
                    CurrentReturnBaseTypeSpelling(compiler),
                    CurrentReturnBaseTypeSpelling(compiler)));
            }
            else LogErrorContext(errCtx, std::format(
                "returning a fresh allocation from a function whose return type is the bare "
                "pointer '{}*' gives the caller no signal that it owns the result - a forgotten "
                "'delete' is a silent leak. Declare the return type 'move {}*' to transfer "
                "ownership to the caller (which adopts it with 'unique {}* x = f();'), or "
                "'alias {}*' to opt out of ownership tracking and manage the lifetime by hand.",
                CurrentReturnBaseTypeSpelling(compiler), CurrentReturnBaseTypeSpelling(compiler),
                CurrentReturnBaseTypeSpelling(compiler), CurrentReturnBaseTypeSpelling(compiler)));
        }

        // The mirror image: an INTERFACE VALUE returned from a function whose declared
        // return type is not that interface (e.g. a `View*` factory whose body now
        // returns an `IView`). The ret operand would be a fat pointer against a
        // pointer return type - LLVM module verification fails with no source location.
        if (right != nullptr && compiler->GetFatPtrType() != nullptr
            && right->getType() == compiler->GetFatPtrType()
            && compiler->currentFunction != nullptr
            && compiler->currentFunction->getReturnType() != compiler->GetFatPtrType())
        {
            LogErrorContext(errCtx, std::format(
                "cannot return interface value '{}' from a function declared to return '{}' - "
                "declare the return type as the interface (e.g. 'move {}')",
                returnNV.TypeAndValue.TypeName.empty() ? "<interface>"
                    : SpellType(*compiler, returnNV.TypeAndValue),
                CurrentReturnTypeSpelling(compiler),
                returnNV.TypeAndValue.TypeName.empty() ? "<interface>"
                    : SpellType(*compiler, returnNV.TypeAndValue)));
        }

        /*
         * BORROW PROVENANCE for this function's callers: does EVERY pointer return read a live
         * `unique` FIELD? A `return nullptr;` owns nothing and is NEUTRAL, exactly as a provably
         * null join arm is. Recorded here, where the return's binding still carries the field
         * flags; consumed at the call site, where the result becomes a borrow of that field.
         */
        if (compiler->builder != nullptr && compiler->builder->GetInsertBlock() != nullptr
            && !(right != nullptr && llvm::isa<llvm::ConstantPointerNull>(right)))
        {
            bool returnsCoreUniqueAsRawPointer =
                compiler->IsCoreUniqueToRawPointer(returnNV, compiler->currentFunctionReturnTV);
            bool returnsCoreUniqueValue = !returnNV.TypeAndValue.Pointer
                && compiler->IsCoreUniqueType(returnNV.TypeAndValue.TypeName);
            bool provesFieldBorrow = IsUniqueFieldRead(returnNV)
                && (returnNV.TypeAndValue.Pointer || returnsCoreUniqueAsRawPointer
                    || returnsCoreUniqueValue);
            compiler->RecordUniqueFieldBorrowReturn(
                compiler->builder->GetInsertBlock()->getParent(), provesFieldBorrow,
                provesFieldBorrow ? DescribeUniqueFieldOwner(returnNV) : std::string());
        }

        // Bond return check: bonded value may only be returned if all its sources
        // are 'bond' parameters of the current function (not locals).
        auto checkBondSources = [&](const std::vector<std::string>& sources) {
            for (const auto& source : sources)
            {
                auto funcArg = compiler->GetFunctionArgument(source);
                if (funcArg.GetValue() == nullptr || !funcArg.TypeAndValue.IsBond)
                    LogErrorContext(errCtx, std::format("returning bonded value whose source '{}' is not a 'bond' parameter - bonded values cannot escape their source's scope", source));
            }
        };
        if (returnNV.IsBonded)
            checkBondSources(returnNV.BondedSources);
        if (compiler->lastCallIsBonded)
            checkBondSources(compiler->lastCallBondedSources);

        // Returning an owning field of a by-value temp (`return makeToken().text;`).
        if (returnNV.FromOwningTempField
            && compiler->IsOwningValueType(returnNV.TypeAndValue.TypeName))
        {
            // A `move`-temp OWNS the field: move it out as the return value (Phase B).
            // Keep the owned bit (ownership transfers to the caller) and zero the source
            // field so the temp's full-dtor (FlushOwnedStructTemps below) skips it. Works
            // for a string field (clears its owned bit) or an owning struct field (the
            // aggregate-zero recursively clears every owning subfield's owned bit). An
            // ALIAS temp (plain accessor) is still rejected - you cannot move out of a borrow.
            // POINTER fields are excluded here as at the two sibling implied-move sites:
            // the aggregate-zero store below emits IR that crashes codegen on one.
            if (returnNV.MovableTempField && right != nullptr
                && !returnNV.TypeAndValue.Pointer
                && right->getType() == returnNV.BaseType)
            {
                clearReturnedStringBorrowBit = false;
                // Same exclusion for a moved-out owning STRUCT field (e.g.
                // `return makeOuterTok(n).inner;`): the buffer's ownership transfers
                // to the caller, so its owning-field bits must stay set, not be
                // cleared as a borrow - clearing would orphan the buffer (leak).
                clearReturnedStructBorrowBits = false;
                auto* srcGep = compiler->builder->CreateStructGEP(
                    returnNV.MoveTempStructType, returnNV.MoveTempStructAlloca,
                    returnNV.MoveTempFieldIndex, "movedfld");
                compiler->builder->CreateStore(
                    llvm::ConstantAggregateZero::get(right->getType()), srcGep);
            }
            // A `string` field is returned by IMPLICIT COPY instead (see
            // AdoptImplicitStringTempCopy); the reject stays for every other owning type.
            else if (returnNV.TypeAndValue.TypeName == "string"
                && AdoptImplicitStringTempCopy(returnNV, right, errCtx))
            {
                // The copy owns its buffer, so the returned value must keep its owned bit.
                clearReturnedStringBorrowBit = false;
            }
            else
            {
                LogErrorContext(errCtx, std::format(
                    "cannot return '{}.{}' taken from a temporary; its buffer is owned elsewhere and "
                    "would be freed. Use '.copy()' for an independent copy.",
                    returnNV.OwningStructName, returnNV.FieldName));
            }
        }

        // Same escape with a dtor-LESS pointee, which the type-name gate above
        // cannot see. After it, so a dtor-bearing pointee keeps its wording.
        GuardOwningTempUniqueFieldEscape(returnNV, "the return value", errCtx);

        // Returning a whole `alias` (borrow) value from a non-`alias` function hands
        // the caller a value whose always-run destructor frees a buffer the real owner
        // still holds. Allowed only when the function itself is declared `alias` (the
        // borrow passes through). `.copy()` makes an independent owned value. `string`
        // and `__closure_fat_ptr` are excluded - they carry a runtime owned bit that
        // already clears on a borrow return (the string-redesign borrow path), so the
        // `alias` compile-time machinery is only for owning STRUCTS with no runtime bit.
        // A borrow can only dangle when the caller could destruct it: a pointer (the
        // pointee is freed) or an owning value type (its destructor frees buffers the
        // real owner still holds). An alias of a primitive or a POD struct hands back a
        // plain value copy with nothing to free - e.g. `dict<string,u64>.get(k)`.
        // PointsToAliasBorrow is the address-of form: `&rows.get(0)` is storable, but handing it
        // across the return boundary from a non-alias function is the escape this rejects.
        // A root that is an alias-borrow LOCAL excuses a borrowed POINTER (per the ruling, `&` over
        // an alias lvalue is a plain borrowed pointer) or a value with nothing to free. It does NOT
        // excuse an OWNING value type: that hands the caller a second destructor for the same
        // buffers, unless the indirect-owning-lvalue arm below rewrites it into an owned copy/move.
        const bool aliasBorrowRootExcused = returnNV.RootIsAliasBorrowLocal
            && (returnNV.TypeAndValue.Pointer
                || !compiler->IsOwningValueType(returnNV.TypeAndValue.TypeName)
                || (right != nullptr && assignExpr != nullptr
                    && right->getType()->isStructTy()
                    && !returnNV.FromOwningTempField
                    && !returnNV.IsClosureValueCapture
                    && ReturnSourceIsIndirectOwningLvalue(returnNV, right)));
        if (!compiler->currentFunctionReturnTV.IsAlias
            && (SourceIsDanglingAliasBorrow(compiler, returnNV) || returnNV.PointsToAliasBorrow)
            && !aliasBorrowRootExcused)
        {
            LogErrorContext(errCtx, std::format(
                "cannot return an 'alias' value '{}'; it borrows storage it does not own and "
                "would dangle. Declare the function 'alias', or use '.copy()' for an owned copy.",
                returnNV.CallerName.empty() ? SpellType(*compiler, returnNV.TypeAndValue)
                                            : returnNV.CallerName));
        }

        // Same hazard through a suppressed (mixed) '?:' join of an owning-value
        // struct: the caller adopts and destroys bits a live borrow still owns.
        // An `alias` function passes the borrow through, exactly as above.
        if (!compiler->currentFunctionReturnTV.IsAlias)
            RejectNonOwningStructJoinStore(right, compiler->currentFunctionReturnTypeName,
                                           "a return slot", errCtx);

        // The allocation alignment of an over-aligned `new T[n]` is not in the element
        // type, so the caller recovers it only when the RETURN TYPE declares the same
        // `alignas(_, N)` clause (threaded to the caller via lastCallReturnsAllocAlign).
        // A matching clause is allowed; a missing/mismatched one would free the block
        // wrong. (Alignment carried by the TYPE returns freely, untagged, as in C++.)
        if (returnNV.AllocAlignment > LLVMBackend::kDefaultNewAlign
            && compiler->currentFunctionReturnTV.AllocAlignValue != returnNV.AllocAlignment)
        {
            if (compiler->currentFunctionReturnTV.AllocAlignValue == 0)
                LogErrorContext(errCtx, std::format(
                    "cannot return the over-aligned buffer '{}' ('new T[n] alignas(0, {})'): that "
                    "alignment is a property of the allocation, not of the type, so the caller cannot "
                    "recover it from the return type and would free the block as an ordinary "
                    "allocation. Declare the return type 'alignas(0, {})' so the block alignment is "
                    "recorded, or over-align the ELEMENT TYPE instead.",
                    returnNV.CallerName.empty() ? SpellType(*compiler, returnNV.TypeAndValue)
                                                : returnNV.CallerName,
                    returnNV.AllocAlignment, returnNV.AllocAlignment));
            else
                LogErrorContext(errCtx, std::format(
                    "allocation alignment mismatch on return: the return type is declared "
                    "'alignas(0, {})' but the returned buffer was allocated {}-aligned. The two must "
                    "agree so the caller frees with the correct alignment.",
                    compiler->currentFunctionReturnTV.AllocAlignValue, returnNV.AllocAlignment));
        }

        /*
         * Returning a closure read from a FIELD / ELEMENT (`return p->get;`), the
         * mirror of the decl-init clone. A whole-variable closure local returns by
         * move (its storage is handed to the caller and its dtor suppressed), but a
         * field read has no such transfer: the raw TAGGED OWNED env would be handed
         * to the caller while the struct still owns it, and both free it. Clone so
         * the caller gets an independent env; a `move` source already transferred.
         */
        if (right != nullptr
            && right->getType()->isStructTy()
            && returnNV.TypeAndValue.TypeName == "__closure_fat_ptr"
            && !returnNV.TypeAndValue.IsMove
            && returnNV.Storage != nullptr
            && !llvm::isa<llvm::AllocaInst>(returnNV.Storage)
            && !llvm::isa<llvm::GlobalVariable>(returnNV.Storage))
        {
            LLVMBackend::NamedVariable srcNV;
            srcNV.Storage  = returnNV.Storage;
            srcNV.BaseType = compiler->GetClosureFatPtrType();
            srcNV.TypeAndValue.TypeName = "__closure_fat_ptr";
            if (auto* cloned = compiler->CreateOverloadedFunctionCall("copy", { srcNV }))
                right = cloned;
        }

        /*
         * Returning a lambda body's unpacked BY-VALUE capture of an owning value type. The
         * closure ENV owns that buffer and its cleanup fn frees it exactly once, so the local
         * is only a borrow (IsClosureValueCapture, set at the capture unpack). Handing it back
         * raw makes the caller a second owner of the env's buffer - or, once the owned bit is
         * cleared, a borrower of storage the env may free first. Copy so the caller gets an
         * independent owner, which is what `.copy()` did by hand. An `alias` function passes
         * the borrow through deliberately, and a `move` source already transferred.
         */
        if (right != nullptr
            && returnNV.IsClosureValueCapture
            && !returnNV.TypeAndValue.IsMove
            && !compiler->currentFunctionReturnTV.IsAlias
            && returnNV.Storage != nullptr
            && right->getType()->isStructTy()
            && compiler->IsOwningValueType(returnNV.TypeAndValue.TypeName))
        {
            LLVMBackend::NamedVariable srcNV;
            srcNV.Storage  = returnNV.Storage;
            srcNV.BaseType = right->getType();
            srcNV.TypeAndValue.TypeName = returnNV.TypeAndValue.TypeName;
            if (auto* copied = compiler->CreateOverloadedFunctionCall("copy", { srcNV }))
            {
                right = copied;
                // The copy is a fresh buffer nobody else owns, so the caller must own it: a
                // FIELD read (`() => box.s`) classified as a borrow above and would leak it.
                clearReturnedStringBorrowBit = false;
                clearReturnedStructBorrowBits = false;
            }
        }

        /*
         * Returning a `string` read through a BY-REFERENCE capture (`() => box.s`). The OUTER
         * frame owns that buffer and destroys it when its scope closes, so handing the caller the
         * borrow returns freed bytes (an empty string under MallocScribble). Deep-copy and keep
         * the OWNED bit, exactly as the by-value-capture arm above does for the env's buffer. The
         * non-string owning field of such a capture is rejected earlier as an `alias` return, and
         * an `alias` function passes the borrow through deliberately.
         */
        if (right != nullptr
            && returnNV.IsClosureRefCapture
            && !returnNV.TypeAndValue.IsMove
            && !compiler->currentFunctionReturnTV.IsAlias
            && NamedVarIsString(returnNV)
            && right->getType() == llvm::StructType::getTypeByName(*compiler->context, "string"))
        {
            right = compiler->EmitOwnedStringDeepCopy(right);
            clearReturnedStringBorrowBit = false;
        }

        /*
         * Returning a fixed-array `string` element (`return dst[0];`). The frame destroys the
         * array on the way out, so handing the caller the element's own {ptr,len,owned} pair both
         * double-freed the buffer (rc 133) and left the caller reading freed bytes - a WRONG VALUE,
         * not merely a double free. Deep-copy so the caller owns an independent buffer, and keep
         * the OWNED bit set (the borrow classification above would otherwise clear it and leak the
         * copy). An `alias` function passes the borrow through deliberately. Representation-gated
         * on both halves: the `%string` named type on the value and on the function's return type.
         */
        if (right != nullptr
            && !compiler->currentFunctionReturnTV.IsAlias
            && compiler->currentFunction != nullptr
            && compiler->currentFunction->getReturnType()
                == llvm::StructType::getTypeByName(*compiler->context, "string")
            && IsOwningArrayStringElementRead(returnNV, right))
        {
            right = compiler->EmitOwnedStringDeepCopy(right);
            clearReturnedStringBorrowBit = false;
        }

        /*
         * Returning a whole `string` ELEMENT borrowed from a container (`return fields.get(0);`).
         * The accessor hands back an `alias` view of a buffer the container owns, so the caller
         * was left reading freed bytes the moment the container was cleared - a silent wrong
         * value. Copy, and keep the OWNED bit so the caller owns the copy. An `alias` function
         * passes the borrow through deliberately.
         */
        if (right != nullptr
            && !compiler->currentFunctionReturnTV.IsAlias
            && compiler->currentFunction != nullptr
            && compiler->currentFunction->getReturnType()
                == llvm::StructType::getTypeByName(*compiler->context, "string")
            && IsImplicitCopyableStringTemp(returnNV)
            && AdoptImplicitStringTempCopy(returnNV, right, errCtx))
        {
            clearReturnedStringBorrowBit = false;
        }

        // A range-for variable is a borrow held in a reusable local slot. Returning that slot
        // must copy the current element before the collection's frame-local storage is destroyed.
        if (right != nullptr && returnNV.IsRangeForBorrow
            && !compiler->currentFunctionReturnTV.IsAlias
            && compiler->currentFunction != nullptr
            && compiler->currentFunction->getReturnType()
                == llvm::StructType::getTypeByName(*compiler->context, "string")
            && returnNV.TypeAndValue.TypeName == "string")
        {
            right = compiler->EmitOwnedStringDeepCopy(right);
            clearReturnedStringBorrowBit = false;
        }

        if (right != nullptr && returnNV.IsRangeForBorrow
            && !compiler->currentFunctionReturnTV.IsAlias
            && !NamedVarIsString(returnNV)
            && compiler->IsOwningValueType(returnNV.TypeAndValue.TypeName))
        {
            right = EmitCopyableOwnerCopy(returnNV, right, errCtx);
            clearReturnedStructBorrowBits = false;
        }

        /*
         * Returning an owning FIELD PATH or fixed-array ELEMENT (`return w.b;`). The six store
         * arms consume such a source through ClassifyOwningAssignSource; the return path had no
         * such arm at all, so it copied the field's bits and left the field live - the returned
         * value and the still-live struct both owned the same pointee (rc 133). Take the SAME
         * decision here: a copyable owner COPIES (source stays live), a non-copyable owner MOVES
         * by zeroing the source lvalue. An `alias` function passes the borrow through.
         */
        if (right != nullptr && assignExpr != nullptr
            && right->getType()->isStructTy()
            && !compiler->currentFunctionReturnTV.IsAlias
            && !returnNV.FromOwningTempField
            && !returnNV.IsClosureValueCapture
            && ReturnSourceIsIndirectOwningLvalue(returnNV, right))
        {
            AssignSourceKind kind;
            llvm::Value* toReturn = ClassifyOwningAssignSource(
                right, returnNV.TypeAndValue.TypeName, returnNV.TypeAndValue.IsMove, errCtx, kind);
            // The same guard the six store arms run after their classify: consuming a field of a
            // borrowed by-value parameter nulls only the callee's copy of it (double free).
            // q11 point 2: the same rule the store arms apply - returning an owning value out of
            // a global / `static` local consumes storage that is never re-initialized, so the
            // next call reads it empty. Explicit `move` remains the sanctioned spelling.
            if (kind != AssignSourceKind::Move
                || !RejectConsumeOfBorrowedByValueParamField(compiler, returnNV, errCtx))
            if (kind != AssignSourceKind::Move
                || !RejectImplicitConsumeOfOutlivingOwner(
                        compiler, returnNV, returnNV.TypeAndValue.IsMove, errCtx))
            {
                if (kind == AssignSourceKind::Move)
                    compiler->builder->CreateStore(
                        llvm::ConstantAggregateZero::get(toReturn->getType()), returnNV.Storage);
                right = toReturn;
                // The value is this frame's to hand over now; the borrow classification computed
                // above would otherwise clear its owning bits and orphan the pointee.
                clearReturnedStructBorrowBits = false;
                clearReturnedStringBorrowBit = false;
            }
        }

        /*
         * Returning an owning `string` FIELD of a struct that dies with this frame
         * (`B b; b.s = "ab" + "cd"; return b.s;`). The borrow classification above hands the
         * caller the field's {ptr,len} while the frame's destructor frees that buffer, so the
         * caller read back an empty string - a SILENT wrong value, not an abort. Deep-copy so
         * the caller owns an independent buffer. Gated on the borrow classification still being
         * live, which is what keeps the temp-field, closure-capture and fixed-array-element arms
         * above (each of which already took responsibility) from copying a second time.
         */
        if (right != nullptr
            && clearReturnedStringBorrowBit
            && !compiler->currentFunctionReturnTV.IsAlias
            && !returnNV.TypeAndValue.IsMove
            && !returnNV.FieldName.empty()
            && returnNV.Storage != nullptr
            && compiler->currentFunction != nullptr
            && compiler->currentFunction->getReturnType()
                == llvm::StructType::getTypeByName(*compiler->context, "string")
            && right->getType() == llvm::StructType::getTypeByName(*compiler->context, "string")
            && FieldPathRootIsFrameLocal(returnNV.Storage))
        {
            right = compiler->EmitOwnedStringDeepCopy(right);
            clearReturnedStringBorrowBit = false;
        }

        // Clear flags consumed by the return check.
        compiler->lastOwningResult = false;
        compiler->lastAllocAlignment = 0;
        compiler->lastCallReturnsAllocAlign = 0;
        compiler->lastCallReturnsOwned = false;
        compiler->lastCallIsBonded = false;
        compiler->lastCallBondByAddress = false;
        compiler->lastCallBondedSources.clear();
        // The returned value (e.g. the final string of `return a + b + c;`)
        // is moved to the caller - drop it from the temporary cleanup list
        // so we do not free a buffer the caller now owns. Any other owned-
        // string intermediates of the return expression must be freed before
        // the ret terminates the block.
        compiler->UnregisterOwnedStringTemp(right);
        compiler->FlushOwnedStringTemps();
        // Same for a returned closure literal (`return () => {...};`): the closure
        // env is moved to the caller, so drop it from the temp list before flushing
        // or we would free an env the caller now owns (double free on its teardown).
        compiler->UnregisterOwnedClosureTemp(right);
        compiler->FlushOwnedClosureTemps();
        // Owning temp whose field was extracted in the return expr (returning an OWNING
        // field is rejected upstream, so this never frees a buffer the caller now owns).
        compiler->FlushOwnedStructTemps();
        // Borrow returns: hand the caller a non-owning copy (see the classification
        // above). Done after the temp-flush so the unregister logic above still sees
        // the original loaded value; a borrow is never a registered owned temp.
        if (clearReturnedStringBorrowBit)
            right = compiler->ClearStringOwnedBit(right);
        else if (clearReturnedStructBorrowBits)
            right = compiler->ClearStructOwnedBits(right, returnNV.TypeAndValue.TypeName);
        // Pass the returned local's storage so a by-value struct return whose
        // full-destructor frees members (e.g. owned string fields) moves
        // ownership to the caller instead of being destructed here. The struct
        // return value is materialized field-wise (and a struct read leaves
        // Storage null, keeping the value in Primary), so resolve the alloca by
        // name - CreateReturnCall's LoadInst-based detection cannot see it.
        llvm::Value* retStorage = returnNV.Storage;
        if (retStorage == nullptr && !returnNV.CallerName.empty())
            retStorage = compiler->FindVariableStorage(returnNV.CallerName).Storage;

        // Interface return upcast. When the function's declared return type is an
        // interface (LLVM { ptr vtable, ptr data } fat pointer), box a concrete
        // implementer here - mirroring the assignment/parameter upcast. Without
        // this the `ret` operand is a bare pointer and module verification fails.
        // An operand that is ALREADY a fat pointer still enters when its data half
        // is this frame's storage (`return sq as IShape;`): the boxing happened
        // upstream, but the dangle it creates is this path's to reject.
        std::string interfaceReturnStructName;
        llvm::AllocaInst* returnedFatFrameSlot =
            right != nullptr && right->getType() == compiler->GetFatPtrType()
                ? FrameLocalDataOfFatValue(right) : nullptr;
        /*
         * DEFERRED existential check for the shape the walk above cannot see:
         * `IShape r = loc as IShape; return r;` arrives here as a plain load of
         * r's slot, with returnedFatFrameSlot null (the walk stops at loads by
         * design). Record the slot now - the answer is resolved once this
         * function's body is fully lowered and its CFG is complete
         * (RunInterfaceReturnDangleCheck, called from the same hook as
         * RunNullDerefDataflow), not here.
         */
        if (right != nullptr && compiler->currentFunction != nullptr
            && compiler->currentFunction->getReturnType() == compiler->GetFatPtrType()
            && right->getType() == compiler->GetFatPtrType()
            && returnedFatFrameSlot == nullptr)
        {
            if (auto* load = llvm::dyn_cast<llvm::LoadInst>(right))
                if (auto* slot = llvm::dyn_cast<llvm::AllocaInst>(
                        load->getPointerOperand()->stripPointerCasts()))
                {
                    const auto* slotNV = compiler->FindVariableByStorage(slot);
                    compiler->RecordPendingReturnDangleCheck(
                        slot, static_cast<int>(errCtx->getStart()->getLine()),
                        static_cast<int>(errCtx->getStart()->getCharPositionInLine()),
                        compiler->currentFunctionReturnTypeName,
                        slotNV != nullptr && slotNV->InterfaceBoxFrameStorage,
                        slotNV == nullptr || slotNV->InterfaceBoxReturnProvenanceUnknown,
                        slotNV == nullptr ? std::string()
                            : slotNV->InterfaceBoxFrameStorageClassName);
                }
        }
        if (auto* fatTy = compiler->GetFatPtrType();
            right != nullptr && fatTy != nullptr && compiler->currentFunction != nullptr
            && compiler->currentFunction->getReturnType() == fatTy
            && ((right->getType() != fatTy && !returnNV.TypeAndValue.IsInterface)
                || returnedFatFrameSlot != nullptr))
        {
            const std::string& ifaceName = compiler->currentFunctionReturnTypeName;
            std::string structName = ReturnUpcastStructName(returnNV);

            if (returnedFatFrameSlot != nullptr)
            {
                // Already boxed against this frame's storage. The implements/shape
                // checks below cannot apply (the operand is no longer the class),
                // and returnNV describes the fat pointer rather than what was boxed
                // - so name the class from the vtable the value carries, or say
                // nothing about it when no single class answers.
                LogInterfaceReturnDangle(errCtx, BoxedStructNameOfFatValue(right, compiler), ifaceName);
            }
            else if (!structName.empty() && !compiler->StructImplementsInterface(structName, ifaceName))
            {
                LogErrorContext(errCtx, std::format("'{}' does not implement interface '{}'",
                    SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = structName }),
                    SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = ifaceName })));
            }
            else if (!structName.empty()
                     && RejectPointerShapedInterfaceUpcast(
                            errCtx, returnNV.TypeAndValue, ifaceName))
            {
                // Diagnosed: a pointer/view-shaped binding cannot carry the vtable.
                // Empty body only because LogErrorContext throws; otherwise this
                // must not fall through to CreateReturnCall with an unboxed ptr.
            }
            else if (!structName.empty())
            {
                if (returnNV.TypeAndValue.Pointer)
                {
                    // Concrete implementer pointer: defer the boxing to
                    // CreateReturnCall so the owning local's scope-exit free is
                    // suppressed (ownership moves to the caller).
                    interfaceReturnStructName = structName;
                }
                else
                {
                    // Concrete implementer VALUE: the interface fat pointer must
                    // carry a data pointer that outlives this call. A by-value
                    // local lives in this frame, so boxing it would return a
                    // dangling pointer. Reject with a clear diagnostic steering to
                    // a heap allocation (rather than emitting UB / a verifier dump).
                    LogInterfaceReturnDangle(errCtx, structName, ifaceName);
                }
            }
            else if (llvm::isa<llvm::Constant>(right) && right->getType()->isPointerTy())
            {
                // `return nullptr;` for an interface return -> null fat pointer.
                right = llvm::ConstantAggregateZero::get(fatTy);
            }
            else if (right->getType()->isPointerTy())
            {
                // Backstop against a raw pointer reaching the module verifier.
                // Any nameless pointer expression (`c + 0`) gets here, not just a join.
                LogErrorContext(errCtx, std::format(
                    "cannot convert this expression to interface '{}': its concrete "
                    "class cannot be determined; bind it to a local variable of the "
                    "class type first", SpellType(*compiler,
                        LLVMBackend::TypeAndValue{ .TypeName = ifaceName })));
            }
        }

        // Derived-interface -> parent-interface RETURN (`IElement f(){ IButton b = ...;
        // return b; }`). The returned value is already a fat pointer, but a derived
        // vtable is not layout-compatible with the parent's once field-offset slots
        // exist, so rebuild it through the typedesc chain - the same rebox the
        // assignment and call-argument paths do. A same-interface return is a no-op.
        // The ownership checks above are untouched: they fire on a returned concrete
        // POINTER (the boxing case), which this is not.
        if (auto* fatTy = compiler->GetFatPtrType();
            right != nullptr && fatTy != nullptr && right->getType() == fatTy)
        {
            // A '?:' result carries no NamedVariable TypeName; fall back to the
            // per-value ledger a ternary join stamps (PropagateFatInterfaceJoin).
            std::string srcIface = compiler->ResolveFatInterfaceSrcName(right,
                returnNV.TypeAndValue.IsInterface ? returnNV.TypeAndValue.TypeName : std::string());
            right = compiler->ReboxInterfaceIfNeeded(
                right, srcIface, compiler->currentFunctionReturnTypeName);
        }

        if (auto* rawCountOut = compiler->CurrentRawArrayReturnCountArgument())
        {
            llvm::Value* count = compiler->LoadRawArrayLength(returnNV);
            if (count == nullptr) count = compiler->RawArrayCountOf(right);
            compiler->builder->CreateStore(
                count != nullptr
                    ? compiler->Upconvert(count, compiler->builder->getInt64Ty())
                    : compiler->builder->getInt64(-1),
                rawCountOut);
        }

        compiler->CreateReturnCall(right, retStorage, interfaceReturnStructName,
                                   returnNV.TypeAndValue.IsUnsignedInteger() != -1);
    }

void MainListener::ParseStatement(CFlatParser::StatementContext* statement) {
        auto* compiler = Compiler(statement);
        compiler->SetCurrentDebugLocation(statement->getStart()->getLine());

        auto jump = statement->jumpStatement();
        auto expressStatement = statement->expressionStatement();
        auto iterationStatement = statement->iterationStatement();
        auto vectorizeStatement = statement->vectorizeStatement();
        auto selectionStatement = statement->selectionStatement();

        // `vectorize <loop>` is handled by the iteration path below with the
        // request flag armed. Only counted `for` and `while` can be vectorized;
        // do-while and foreach (which lowers to count()/get() calls) are rejected
        // up front with a clear message rather than a cryptic LLVM remark.
        if (vectorizeStatement != nullptr)
        {
            iterationStatement = vectorizeStatement->iterationStatement();
            if (iterationStatement->Do() ||
                (iterationStatement->declarationSpecifiers() && iterationStatement->In()))
            {
                LogErrorContext(vectorizeStatement,
                    "vectorize supports counted 'for' and 'while' loops only");
                return;
            }
            // Optional FP-math tier flag: 'contract' or 'reassoc' (tiers, not composable;
            // reassoc implies contract). Anything else is rejected.
            vectorizeFpTier_ = VectorizeFpTier::None;
            if (auto* flagId = vectorizeStatement->Identifier())
            {
                const std::string flag = flagId->getText();
                if (flag == "contract")
                    vectorizeFpTier_ = VectorizeFpTier::Contract;
                else if (flag == "reassoc")
                    vectorizeFpTier_ = VectorizeFpTier::Reassoc;
                else
                {
                    LogErrorContext(vectorizeStatement,
                        "vectorize flag must be 'contract' or 'reassoc'");
                    return;
                }
            }
            vectorizeActive_ = true;
            vectorizeLine_ = static_cast<int>(vectorizeStatement->getStart()->getLine());

            // Gather AST facts about this loop so a failed vectorization (detected
            // post-optimization) can report a precise, well-located reason.
            LLVMBackend::VectorizeLoopInfo vinfo;
            vinfo.line = vectorizeLine_;
            vinfo.col  = static_cast<int>(vectorizeStatement->getStart()->getCharPositionInLine());
            vinfo.isWhile = iterationStatement->While() != nullptr && iterationStatement->Do() == nullptr;
            if (vinfo.isWhile)
            {
                auto* cond = iterationStatement->expression();
                if (cond != nullptr)
                {
                    vinfo.condText = cond->getText();
                    vinfo.condLine = static_cast<int>(cond->getStart()->getLine());
                    vinfo.condCol  = static_cast<int>(cond->getStart()->getCharPositionInLine());
                    // A counted loop compares against a bound (< <= > >=); an
                    // equality/sentinel condition (== / !=, e.g. `p != nullptr`)
                    // is the classic non-countable pointer-chase.
                    vinfo.conditionCounted = !ConditionIsSentinel(cond);
                }
            }
            if (auto* call = FindFirstCall(iterationStatement->statement()))
            {
                vinfo.hasCall = true;
                vinfo.callName = call->primaryExpression() ? call->primaryExpression()->getText() : std::string("a function");
                vinfo.callLine = static_cast<int>(call->getStart()->getLine());
                vinfo.callCol  = static_cast<int>(call->getStart()->getCharPositionInLine());
            }
            compiler->AddVectorizeLoopInfo(vinfo);
        }
        auto compoundStatement = statement->compoundStatement();
        auto labeledStatement = statement->labeledStatement();
        auto expectErrorStmt = statement->expectErrorStatement();

        if (labeledStatement != nullptr && !switchStack.empty())
        {
            auto& ctx = switchStack.back();
            llvm::BasicBlock* targetBlock = nullptr;

            if (labeledStatement->Case())
            {
                auto it = ctx.caseMap.find(labeledStatement);
                if (it != ctx.caseMap.end())
                    targetBlock = it->second.block;
            }
            else if (labeledStatement->Default())
            {
                targetBlock = ctx.defaultBlock;
            }

            if (targetBlock)
            {
                compiler->CreateJump(targetBlock);    // fallthrough if no terminator yet
                compiler->SwitchToBlock(targetBlock);
            }

            // Arm-style: inject bound variable, execute body in its own scope, auto-jump to resume.
            // `default =>` and `case _ =>` arms are detected via FatArrow token.
            auto it = ctx.caseMap.find(labeledStatement);
            bool isDefaultArm = labeledStatement->FatArrow() != nullptr
                             && (labeledStatement->Default()
                                 || (labeledStatement->Case()
                                     && labeledStatement->constantExpression()
                                     && labeledStatement->constantExpression()->getText() == "_"));
            bool isArm = (it != ctx.caseMap.end() && it->second.isArmStyle) || isDefaultArm;
            bool hasBound = isArm && it != ctx.caseMap.end() && it->second.isTypeCase && !it->second.boundVarName.empty();

            if (isArm)
            {
                compiler->InitializeBlock(nullptr, true);

                if (hasBound && ctx.condValue)
                {
                    // Extract the data pointer (field 1) from the interface fat ptr and bind it.
                    auto* dataPtr = compiler->builder->CreateExtractValue(ctx.condValue, { 1u }, "typecase_data");
                    auto* alloca = compiler->AllocaAtEntry(dataPtr->getType(), nullptr, it->second.boundVarName);
                    compiler->builder->CreateStore(dataPtr, alloca);

                    LLVMBackend::NamedVariable nv;
                    nv.Storage  = alloca;
                    nv.Primary  = dataPtr;
                    nv.TypeAndValue.TypeName = it->second.typeCaseName;
                    nv.TypeAndValue.Pointer  = true;
                    nv.IsOwning    = false;
                    compiler->SetStackVariable(it->second.boundVarName, nv);
                    compiler->RecordMoveGenBind(it->second.boundVarName); // fresh type-case arm binding
                }

                ParseStatement(labeledStatement->statement());
                compiler->CreateBlockBreak(ctx.resumeBlock, true);
                return;
            }

            ParseStatement(labeledStatement->statement());
            return;
        }

        if (jump != nullptr)
        {
            if (jump->Return())
            {
                // Mark the straight-line as returned so the if/else move-merge treats this branch's
                // moves as dead-path even if a (dead) statement follows the return.
                straightLineReturned_ = true;
                if (jump->Default() != nullptr)
                {
                    // return default; - the declared return type's default VALUE, built by the
                    // shared emitter so a struct runs its field initializers (see the `default`
                    // primaryExpression). A void / 'auto' / untyped return has nothing to default
                    // to, so it keeps the flat zero (or no value at all).
                    auto* retTy = compiler->currentFunction->getReturnType();
                    const auto& retTV = compiler->currentFunctionReturnTV;
                    bool abiReturnsValue = compiler->currentFunctionAbiRecipe.hasLowering
                        && compiler->currentFunctionAbiRecipe.retSlot.kind
                            != LLVMBackend::AbiSlot::Direct;
                    bool typedReturn = (!retTy->isVoidTy() || abiReturnsValue)
                        && !compiler->IsAutoReturnCaptureActive()
                        && !retTV.TypeName.empty() && retTV.TypeName != "auto";
                    if (typedReturn)
                        EmitReturnExpression(jump, nullptr, "", true);
                    else
                        compiler->CreateReturnCall(
                            retTy->isVoidTy() ? nullptr : llvm::Constant::getNullValue(retTy));
                }
                else if (auto* blockBody = jump->compoundStatement())
                {
                    compiler->InitializeBlock(nullptr, true);
                    if (auto* blockItems = blockBody->blockItemList())
                        ParseBlockItemList(blockItems);
                    compiler->CreateBlockBreak(nullptr, true);
                }
                else
                {
                    auto express = jump->expression();
                    if (express != nullptr)
                        EmitReturnExpression(jump, express->assignmentExpression(), express->getText());
                    else
                    {
                        // Mirror of the void-ness check in EmitReturnExpression: a value-less
                        // `return;` emits a CreateRetVoid, which the module verifier rejects
                        // (with no source location) in a function that returns a value.
                        // An 'auto' function is emitted against an i64 placeholder and can still
                        // infer void, so its LLVM return type proves nothing here.
                        if (compiler->currentFunction != nullptr
                            && !compiler->IsAutoReturnCaptureActive()
                            && !compiler->currentFunction->getReturnType()->isVoidTy())
                            LogErrorContext(jump, std::format(
                                "cannot 'return' without a value from a function that returns "
                                "'{}' - return a value, or 'return default;' for the type's default value",
                                CurrentReturnTypeSpelling(compiler)));
                        compiler->CreateReturnCall(nullptr);
                    }
                }
                return;
            }
            else if (jump->Continue())
            {
                RecordLoopExitMovedState();
                straightLineJumped_ = true;
                compiler->CreateContinueCall();
                return;
            }
            else if (jump->Break())
            {
                RecordLoopExitMovedState();
                straightLineJumped_ = true;
                compiler->CreateBreakCall();
                return;
            }
        }
        else if (expressStatement != nullptr)
        {
            auto express = expressStatement->expression();
            if (express != nullptr)
            {
                // A discarded full expression (`makePlain(2);`). Evaluate via the Named path so we
                // can see whether the result is an unclaimed owning-struct temp and register it for
                // end-of-full-expression destruction (FlushOwnedTemps at the block-item boundary).
                if (auto* assign = express->assignmentExpression())
                {
                    bool bareExpr = assign->assignmentOperator() == nullptr;
                    auto resultNV = ParseAssignmentExpressionNamed(assign, ResultUse::Discard);
                    if (bareExpr) DiagnoseDiscardedOwningReturn(assign, resultNV);
                    ProcessPlusPlus();
                    RegisterDiscardedOwningStructTemp(resultNV);
                    return;
                }
                ParseExpression(express);
                return;
            }
        }
        else if (iterationStatement != nullptr)
        {
            // A loop falls through to its resume (it may run zero times or exit normally), so a
            // return in its body does not mark the enclosing straight-line as returned.
            ReturnFlagGuard returnFlagGuard(&straightLineReturned_);
            ReturnFlagGuard jumpFlagGuard(&straightLineJumped_);
            LoopMovedStateGuard loopMovedGuard(this, compiler);
            // Consume the vectorize request immediately so it binds only to this
            // loop level (a nested loop in the body re-reads a cleared flag).
            bool doVectorize = vectorizeActive_;
            int  vectorizeLine = vectorizeLine_;
            VectorizeFpTier fpTier = vectorizeFpTier_;
            vectorizeActive_ = false;
            vectorizeFpTier_ = VectorizeFpTier::None;

            /*
            iterationStatement
                : While '(' expression ')' statement
                | Do statement While '(' expression ')' ';'
                | For '(' forCondition ')' statement
                ;
            forCondition
                : (forDeclaration | expression?) ';' forExpression? ';' forExpression?
                ;
            */

            // Check Do before While: do-while contains the 'while' keyword, so While() returns
            // non-null for both rules. Do() is unambiguous and must be tested first.
            if (iterationStatement->Do())
            {
                auto expression = iterationStatement->expression();
                auto innerStatement = iterationStatement->statement();

                auto blockInner = compiler->CreateBasicBlock("doWhileInner");
                auto blockCondition = compiler->CreateBasicBlock("doWhileCondition");
                auto blockResume = compiler->CreateBasicBlock("doWhileResume");

                compiler->CreateBlockBreak(blockInner, false);

                compiler->InitializeBlock(blockInner, true, blockCondition, blockResume, blockResume);
                ParseControlledBody(innerStatement);
                compiler->CreateContinueCall();

                compiler->InitializeBlock(blockCondition, false);
                auto condition = ParseExpression(expression);
                // Free owned-string temps produced inside the condition (e.g. `do {...} while
                // (s.toString() != "x")`) here, in the condition block. The block-item flush runs
                // in the post-loop block, where the dominance guard would drop them and leak.
                compiler->FlushOwnedTemps();
                compiler->CreateConditionJump(condition, blockInner, blockResume);

                // resume
                compiler->InitializeBlock(blockResume, false);

                // pop the stack
                compiler->CreateBlockBreak(nullptr, true);
                return;
            }
            else if (iterationStatement->While())
            {
                auto expression = iterationStatement->expression();
                auto innerStatement = iterationStatement->statement();

                auto blockCondition = compiler->CreateBasicBlock("whileCondition");
                auto blockInner = compiler->CreateBasicBlock("whileInner");
                auto blockResume = compiler->CreateBasicBlock("whileResume");

                compiler->CreateBlockBreak(blockCondition, false);

                compiler->InitializeBlock(blockCondition, true, blockCondition, blockResume, blockResume);
                auto condition = ParseExpression(expression);
                // Free owned-string temps from the condition in the condition block (see do-while).
                // Runs each iteration after the guard is evaluated.
                compiler->FlushOwnedTemps();

                // A constant-true guard (`while (true)` / `while (1)`) can only be
                // left via `break`. Branch unconditionally into the body so the
                // resume block keeps a predecessor only when a break targets it.
                // CreateConditionJump would instead emit a cond-br that always
                // lists blockResume as a successor, making the dead exit look like
                // a live fall-through path and wrongly demanding a trailing return.
                // When no break targets it, blockResume is left predecessor-less;
                // IsCurrentBlockUnreachable() then recognizes it as dead at the end
                // of the enclosing function/lambda (see the missing-return checks).
                if (compiler->IsConstantTruthy(condition))
                    compiler->CreateBlockBreak(blockInner, false);  // unconditional br to body
                else
                    compiler->CreateConditionJump(condition, blockInner, blockResume);

                compiler->InitializeBlock(blockInner, false);
                {
                    int prevBody = currentVectorizeBodyLine_;
                    if (doVectorize) currentVectorizeBodyLine_ = vectorizeLine;
                    // Apply the FP-math tier to the body's FP ops; save/restore so nothing
                    // outside the lexical body inherits the flags (and outer scopes compose).
                    llvm::FastMathFlags prevFMF = compiler->builder->getFastMathFlags();
                    ApplyVectorizeFpTier(compiler, fpTier);
                    ParseControlledBody(innerStatement);
                    compiler->builder->setFastMathFlags(prevFMF);
                    currentVectorizeBodyLine_ = prevBody;
                }
                compiler->CreateContinueCall();

                // The back-edge to blockCondition is now the terminator of the
                // current block (the loop latch); stamp the vectorize hint on it.
                if (doVectorize)
                    compiler->AttachVectorizeHintToCurrentLatch(vectorizeLine);

                // resume
                compiler->InitializeBlock(blockResume, false);

                // pop the stack
                compiler->CreateBlockBreak(nullptr, true);

                return;
            }
            else if (iterationStatement->For())
            {
                // Classic for-loop: for (init; cond; inc) statement
                if (iterationStatement->forCondition())
                {
                    auto forCondition = iterationStatement->forCondition();
                    auto declaration = forCondition->forDeclaration();
                    auto expressionCtx = forCondition->expression();
                    auto forIncrementCtx = forCondition->forExpression();
                    auto compareCtx = forCondition->assignmentExpression();
                    auto innerStatement = iterationStatement->statement();

                    auto blockInit = compiler->CreateBasicBlock("forInit");
                    auto blockCondition = compiler->CreateBasicBlock("forCondition");
                    auto blockInner = compiler->CreateBasicBlock("forInner");
                    auto blockIncrement = compiler->CreateBasicBlock("forIncrement");
                    auto blockResume = compiler->CreateBasicBlock("forResume");

                    compiler->CreateBlockBreak(blockInit, false);

                    // Init => Condition => Inner => Increment => Condition

                    // initialization
                    compiler->InitializeBlock(blockInit, true, blockIncrement, blockResume, blockResume);
                    if (declaration)
                        ParseForDeclaration(declaration);
                    if (expressionCtx)
                    {
                        // for-init is a discard position too: a bare owning-return call must error
                        // and any owned temp must be freed here (mirrors the statement/for-update).
                        auto* initAssign = expressionCtx->assignmentExpression();
                        bool bareExpr = initAssign->assignmentOperator() == nullptr;
                        auto nv = ParseAssignmentExpressionNamed(initAssign, ResultUse::Discard);
                        if (bareExpr) DiagnoseDiscardedOwningReturn(initAssign, nv);
                        ProcessPlusPlus();
                        RegisterDiscardedOwningStructTemp(nv);
                        compiler->FlushOwnedTemps();
                    }

                    compiler->CreateBlockBreak(blockCondition, false);

                    // Condition
                    compiler->InitializeBlock(blockCondition, false);
                    auto condition = ParseAssignmentExpression(compareCtx);
                    // Free owned-string temps from the condition in the condition block (see do-while).
                    compiler->FlushOwnedTemps();
                    compiler->CreateConditionJump(condition, blockInner, blockResume);

                    // Inner statement
                    compiler->InitializeBlock(blockInner, false);
                    {
                        int prevBody = currentVectorizeBodyLine_;
                        if (doVectorize) currentVectorizeBodyLine_ = vectorizeLine;
                        // Apply the FP-math tier to every FP op the body emits (see while-form).
                        llvm::FastMathFlags prevFMF = compiler->builder->getFastMathFlags();
                        ApplyVectorizeFpTier(compiler, fpTier);
                        ParseControlledBody(innerStatement);
                        compiler->builder->setFastMathFlags(prevFMF);
                        currentVectorizeBodyLine_ = prevBody;
                    }
                    compiler->CreateContinueCall();

                    // Increment
                    compiler->InitializeBlock(blockIncrement, false);

                    auto assignments = forIncrementCtx->assignmentExpression();
                    for (auto assign : assignments)
                    {
                        // A for-update is a discard position too (its value is unused).
                        bool bareExpr = assign->assignmentOperator() == nullptr;
                        auto nv = ParseAssignmentExpressionNamed(assign, ResultUse::Discard);
                        if (bareExpr) DiagnoseDiscardedOwningReturn(assign, nv);
                        ProcessPlusPlus();
                        RegisterDiscardedOwningStructTemp(nv);
                    }
                    // Flush HERE, not in the condition block: the increment block does not
                    // dominate the condition, so a temp registered here would never be freed.
                    compiler->FlushOwnedTemps();

                    compiler->CreateBlockBreak(blockCondition, false);

                    // The increment block's back-edge to blockCondition is the
                    // loop latch terminator; stamp the vectorize hint on it.
                    if (doVectorize)
                        compiler->AttachVectorizeHintToCurrentLatch(vectorizeLine);

                    // resume
                    compiler->InitializeBlock(blockResume, false);

                    // pop the stack
                    compiler->CreateBlockBreak(nullptr, true);

                    return;
                }
                // Range-based for: for (T x in collection) statement
                else if (iterationStatement->declarationSpecifiers() && iterationStatement->In())
                {
                    auto declSpecCtx = iterationStatement->declarationSpecifiers();
                    auto varNameTok  = iterationStatement->Identifier();
                    auto collExprCtx = iterationStatement->expression();
                    auto bodyStmt    = iterationStatement->statement();

                    std::string varName = varNameTok->getText();

                    auto blockInit      = compiler->CreateBasicBlock("forRangeInit");
                    auto blockCond      = compiler->CreateBasicBlock("forRangeCond");
                    auto blockInner     = compiler->CreateBasicBlock("forRangeInner");
                    auto blockIncrement = compiler->CreateBasicBlock("forRangeIncrement");
                    auto blockResume    = compiler->CreateBasicBlock("forRangeResume");

                    compiler->CreateBlockBreak(blockInit, false);

                    // Push scope; continue -> increment, break/else -> resume
                    compiler->InitializeBlock(blockInit, true, blockIncrement, blockResume, blockResume);

                    // Evaluate the collection expression (needs typed NamedVariable for dispatch)
                    auto collNV = ParseAssignmentExpressionNamed(collExprCtx->assignmentExpression());

                    // Spill into alloca if the collection was returned by value (no storage)
                    if (collNV.Storage == nullptr && collNV.Primary != nullptr)
                    {
                        llvm::Type* ty = collNV.BaseType;
                        if (!ty) ty = compiler->GetType(collNV.TypeAndValue);
                        auto spill = compiler->CreateAlloca(ty);
                        compiler->CreateAssignment(collNV.Primary, spill);
                        collNV.Storage = spill;
                        collNV.Primary = nullptr;
                    }

                    // Parsed once for every leg below (WinRT, fixed array, interface, container)
                    // so the by-value ownership guard that follows covers all of them.
                    auto elemType = ParseDeclarationSpecifiers(declSpecCtx);
                    elemType.VariableName = varName;

                    // A by-value loop var bit-copies the element; an owning raw pointer has no
                    // owned bit to clear, so the once-only dtor at forRangeResume double-frees.
                    const bool elemIsCoreUnique = compiler->IsCoreUniqueType(elemType.TypeName);
                    const bool elemIsUniqueAttribute =
                        compiler->HasTypeAnnotation(elemType.TypeName, "unique");
                    const std::string shownElemType = SpellType(*compiler, elemType);
                    std::string ownedFieldPath;
                    if (!elemType.Pointer && !elemType.IsArrayView && !elemType.IsInterface
                        && !compiler->IsInterfaceType(elemType.TypeName)
                        && compiler->TypeOwnsUniquePointer(elemType.TypeName, &ownedFieldPath))
                    {
                        if (elemIsCoreUnique || elemIsUniqueAttribute)
                        {
                            compiler->LogUniqueCopyError(elemType.TypeName);
                            return;
                        }
                        LogErrorContext(iterationStatement, std::format(
                            "cannot bind loop variable '{}' of '{}' by value in a range 'for': field "
                            "'{}.{}' is 'unique', so '{}' owns a raw pointer that a by-value copy would "
                            "share. The loop variable is destructed once when the loop exits and would "
                            "free the pointee the collection still owns. Iterate by index and read the "
                            "element in place instead (for (int i = 0; i < N; i++) ... coll[i]).",
                            varName, shownElemType, shownElemType, ownedFieldPath,
                            shownElemType));
                        return;
                    }

                    // Imported winmd interface receiver -> iterate through the WinRT IIterable<T> /
                    // IIterator<T> COM protocol instead of the count()/get() index path below.
                    if (compiler->IsWinrtThinInterface(collNV.TypeAndValue.TypeName))
                    {
                        std::string collTypeName = collNV.TypeAndValue.TypeName;

                        std::string elemArg = elemType.TypeName;

                        // Synthesize IIterable<T> and IIterator<T> (COM vtable + thin ptr + PIID).
                        std::string iterableName = MangledGenericName(LLVMBackend::kWinrtIIterable, { elemArg });
                        std::string iteratorName = MangledGenericName(LLVMBackend::kWinrtIIterator, { elemArg });
                        bool haveIterable = compiler->InstantiateWinrtGenericInterface(LLVMBackend::kWinrtIIterable, { elemArg }, iterableName);
                        bool haveIterator = compiler->InstantiateWinrtGenericInterface(LLVMBackend::kWinrtIIterator, { elemArg }, iteratorName);
                        auto* iidGlobal = haveIterable ? compiler->EmitIidGlobalFor(iterableName) : nullptr;
                        if (!haveIterable || !haveIterator || !iidGlobal)
                        {
                            LogErrorContext(iterationStatement,
                                "foreach over a WinRT interface needs IIterable<T>/IIterator<T> "
                                "(import \"Windows.Foundation.winmd\") and a scalar/named element type");
                            return;
                        }

                        auto* i8PtrTy = cflat_llvm::PointerTo(compiler->builder->getInt8Ty());
                        auto* i8Ty    = compiler->builder->getInt8Ty();
                        auto* nullPtr = llvm::ConstantPointerNull::get(i8PtrTy);

                        // The live interface pointer (load it out of storage if it is a variable).
                        llvm::Value* objVal = collNV.Primary;
                        if (!objVal) objVal = compiler->builder->CreateLoad(i8PtrTy, collNV.Storage);

                        auto iterableAlloca = compiler->CreateAlloca(i8PtrTy);
                        auto iteratorAlloca = compiler->CreateAlloca(i8PtrTy);
                        auto hasAlloca      = compiler->CreateAlloca(i8Ty);
                        compiler->builder->CreateStore(nullPtr, iterableAlloca);
                        compiler->builder->CreateStore(nullPtr, iteratorAlloca);

                        auto elemAlloca = compiler->CreateLocalVariable(elemType);

                        // QueryInterface the receiver for IIterable<T> (IVector<T> only *requires*
                        // it; an object that already is IIterable answers with itself + AddRef).
                        auto* iidPtr = compiler->builder->CreateBitCast(iidGlobal, i8PtrTy);
                        auto* ppvIterable = compiler->builder->CreateBitCast(iterableAlloca, i8PtrTy);
                        auto* hrQI = compiler->EmitWinrtThinSlotCall(objVal, collTypeName, "QueryInterface", { iidPtr, ppvIterable });
                        auto* iterableV = compiler->builder->CreateLoad(i8PtrTy, iterableAlloca);
                        auto* qiOk = compiler->builder->CreateAnd(
                            compiler->builder->CreateICmpEQ(hrQI, compiler->builder->getInt32(0)),
                            compiler->builder->CreateICmpNE(iterableV, nullPtr));

                        auto blockFirst = compiler->CreateBasicBlock("forIterFirst");
                        compiler->CreateConditionJump(qiOk, blockFirst, blockResume);

                        // First(&iterator) -> the IIterator<T>; bail to cleanup if it fails.
                        compiler->InitializeBlock(blockFirst, false);
                        auto* ppvIter = compiler->builder->CreateBitCast(iteratorAlloca, i8PtrTy);
                        auto* hrFirst = compiler->EmitWinrtThinSlotCall(iterableV, iterableName, "First", { ppvIter });
                        auto* iterV0 = compiler->builder->CreateLoad(i8PtrTy, iteratorAlloca);
                        auto* firstOk = compiler->builder->CreateAnd(
                            compiler->builder->CreateICmpEQ(hrFirst, compiler->builder->getInt32(0)),
                            compiler->builder->CreateICmpNE(iterV0, nullPtr));
                        compiler->CreateConditionJump(firstOk, blockCond, blockResume);

                        // Condition: get_HasCurrent.
                        compiler->InitializeBlock(blockCond, false);
                        auto* iterC = compiler->builder->CreateLoad(i8PtrTy, iteratorAlloca);
                        auto* ppHas = compiler->builder->CreateBitCast(hasAlloca, i8PtrTy);
                        compiler->EmitWinrtThinSlotCall(iterC, iteratorName, "get_HasCurrent", { ppHas });
                        auto* hasV = compiler->builder->CreateICmpNE(
                            compiler->builder->CreateLoad(i8Ty, hasAlloca), compiler->builder->getInt8(0));
                        compiler->CreateConditionJump(hasV, blockInner, blockResume);

                        // Body: bind the element via get_Current, run the loop body.
                        compiler->InitializeBlock(blockInner, false);
                        auto* iterI = compiler->builder->CreateLoad(i8PtrTy, iteratorAlloca);
                        auto* elemOut = compiler->builder->CreateBitCast(elemAlloca, i8PtrTy);
                        compiler->EmitWinrtThinSlotCall(iterI, iteratorName, "get_Current", { elemOut });
                        compiler->RecordMoveGenBind(varName); // element is a fresh binding each iteration
                        ParseControlledBody(bodyStmt);
                        compiler->CreateContinueCall();

                        // Increment slot (continue target): MoveNext, then re-test HasCurrent.
                        compiler->InitializeBlock(blockIncrement, false);
                        auto* iterM = compiler->builder->CreateLoad(i8PtrTy, iteratorAlloca);
                        auto* ppHas2 = compiler->builder->CreateBitCast(hasAlloca, i8PtrTy);
                        compiler->EmitWinrtThinSlotCall(iterM, iteratorName, "MoveNext", { ppHas2 });
                        compiler->CreateBlockBreak(blockCond, false);

                        // Resume: null-safe Release of the iterator and the QI'd iterable.
                        compiler->InitializeBlock(blockResume, false);
                        auto releaseIfNonNull = [&](llvm::AllocaInst* slotAlloca, const std::string& thinNm) {
                            auto* p = compiler->builder->CreateLoad(i8PtrTy, slotAlloca);
                            auto* nn = compiler->builder->CreateICmpNE(p, nullPtr);
                            auto relBB  = compiler->CreateBasicBlock("forIterRel");
                            auto contBB = compiler->CreateBasicBlock("forIterRelCont");
                            compiler->CreateConditionJump(nn, relBB, contBB);
                            compiler->InitializeBlock(relBB, false);
                            compiler->EmitWinrtThinSlotCall(p, thinNm, "Release", {});
                            compiler->CreateBlockBreak(contBB, false);
                            compiler->InitializeBlock(contBB, false);
                        };
                        releaseIfNonNull(iteratorAlloca, iteratorName);
                        releaseIfNonNull(iterableAlloca, iterableName);
                        compiler->CreateBlockBreak(nullptr, true);
                        return;
                    }

                    bool isFaceType   = compiler->IsInterfaceType(collNV.TypeAndValue.TypeName);
                    bool isFixedArray = collNV.BaseType && llvm::isa<llvm::ArrayType>(collNV.BaseType);
                    bool isArrayView  = collNV.TypeAndValue.IsArrayView;
                    bool isDirectContainer = MangledBase(collNV.TypeAndValue.TypeName) == "list"
                        || MangledBase(collNV.TypeAndValue.TypeName) == "array";
                    int dataFieldIndex = -1;
                    if (isDirectContainer && !isFaceType && !isFixedArray && !isArrayView && collNV.BaseType
                        && collNV.BaseType->isStructTy())
                    {
                        auto data = compiler->GetDataStructure(collNV.TypeAndValue.TypeName);
                        for (size_t i = 0; i < data.StructFields.size(); ++i)
                            if ((MangledBase(collNV.TypeAndValue.TypeName) == "list"
                                    ? data.StructFields[i].VariableName == "_data"
                                    : data.StructFields[i].VariableName == "_ptr")
                                && data.StructFields[i].Pointer)
                            {
                                dataFieldIndex = (int)i;
                                break;
                            }
                    }

                    // Call count() once and cache it
                    llvm::Value* countVal = nullptr;
                    llvm::Value* ifacePtr = nullptr;
                    if (isFixedArray)
                    {
                        auto* arrTy = llvm::cast<llvm::ArrayType>(collNV.BaseType);
                        countVal = compiler->builder->getInt32((uint32_t)arrTy->getNumElements());
                    }
                    else if (isArrayView)
                    {
                        countVal = compiler->LoadRawArrayLength(collNV);
                        if (countVal == nullptr)
                            LogErrorContext(iterationStatement,
                                "cannot range-for over an array view whose length is unknown");
                        if (countVal != nullptr && countVal->getType() != compiler->builder->getInt32Ty())
                            countVal = compiler->builder->CreateIntCast(
                                countVal, compiler->builder->getInt32Ty(), false, "viewcount");
                    }
                    else if (isFaceType)
                    {
                        ifacePtr = collNV.Storage;
                        if (!ifacePtr)
                        {
                            auto fatTy = compiler->GetFatPtrType();
                            ifacePtr = compiler->CreateAlloca(fatTy);
                            compiler->CreateAssignment(collNV.Primary, ifacePtr);
                        }
                        countVal = compiler->CallInterfaceMethod(ifacePtr, collNV.TypeAndValue.TypeName, "count", {});
                    }
                    else
                    {
                        LLVMBackend::NamedVariable selfArg = collNV;
                        selfArg.TypeAndValue.VariableName = "";
                        countVal = compiler->CreateOverloadedFunctionCall("count", { selfArg });
                    }

                    auto* i32Ty = compiler->builder->getInt32Ty();

                    auto countAlloca = compiler->CreateAlloca(i32Ty);
                    compiler->builder->CreateStore(countVal, countAlloca);

                    auto indexAlloca = compiler->CreateAlloca(i32Ty);
                    compiler->builder->CreateStore(compiler->builder->getInt32(0), indexAlloca);

                    // Pre-allocate the element variable in the init block (one alloca for all iterations)
                    auto elemAlloca = compiler->CreateLocalVariable(elemType);
                    compiler->GetOrCreateStackVariable(varName).IsRangeForBorrow = true;

                    compiler->CreateBlockBreak(blockCond, false);

                    // Condition: i < count
                    compiler->InitializeBlock(blockCond, false);
                    auto iVal   = compiler->CreateLoad(indexAlloca);
                    auto cntVal = compiler->CreateLoad(countAlloca);
                    auto cond   = compiler->builder->CreateICmpSLT(iVal, cntVal);
                    compiler->CreateConditionJump(cond, blockInner, blockResume);

                    // Inner block: load element, run body
                    compiler->InitializeBlock(blockInner, false);

                    LLVMBackend::NamedVariable indexNV;
                    indexNV.Primary  = compiler->CreateLoad(indexAlloca);
                    indexNV.BaseType = i32Ty;
                    indexNV.TypeAndValue.TypeName = "int";

                    llvm::Value* elemVal = nullptr;
                    llvm::Value* elemStorage = nullptr;
                    llvm::Value* rangeCollectionValue = nullptr;
                    if (isFixedArray)
                    {
                        auto* arrTy = llvm::cast<llvm::ArrayType>(collNV.BaseType);
                        llvm::Value* zero   = compiler->builder->getInt64(0);
                        llvm::Value* idxI64 = compiler->builder->CreateZExt(
                            compiler->CreateLoad(indexAlloca), compiler->builder->getInt64Ty(), "idxi64");
                        auto* elemPtr = compiler->builder->CreateGEP(
                            arrTy, collNV.Storage, {zero, idxI64}, "arrayelemptr");
                        elemStorage = elemPtr;
                        elemVal = compiler->builder->CreateLoad(arrTy->getElementType(), elemPtr, "arrayelem");
                        // Bind the loop variable as a BORROW - its alloca is hoisted and destructed
                        // once, so an owning bit-copy double-frees; matches the container `get`.
                        elemVal = compiler->ClearStringOwnedBit(elemVal);
                        elemVal = compiler->ClearStructOwnedBits(elemVal, elemType.TypeName);
                    }
                    else if (isArrayView)
                    {
                        auto viewElemType = elemType;
                        viewElemType.Pointer = false;
                        viewElemType.ElemPointer = false;
                        viewElemType.IsArrayView = false;
                        viewElemType.ConstArraySize = 0;
                        auto* viewElemLLVMType = compiler->GetType(viewElemType);
                        auto* viewValue = compiler->builder->CreateLoad(
                            compiler->GetType(collNV.TypeAndValue), collNV.Storage, "viewbase");
                        rangeCollectionValue = viewValue;
                        auto* idxI64 = compiler->builder->CreateZExt(
                            compiler->CreateLoad(indexAlloca), compiler->builder->getInt64Ty(), "idxi64");
                        elemStorage = compiler->builder->CreateGEP(
                            viewElemLLVMType, viewValue, idxI64, "viewelemptr");
                        elemVal = compiler->builder->CreateLoad(
                            viewElemLLVMType, elemStorage, "viewelem");
                        elemVal = compiler->ClearStringOwnedBit(elemVal);
                        elemVal = compiler->ClearStructOwnedBits(elemVal, elemType.TypeName);
                    }
                    else if (dataFieldIndex >= 0)
                    {
                        auto data = compiler->GetDataStructure(collNV.TypeAndValue.TypeName);
                        const auto& dataField = data.StructFields[(size_t)dataFieldIndex];
                        auto* dataSlot = compiler->CreateStructGEP(
                            collNV.BaseType, collNV.Storage, (uint32_t)dataFieldIndex);
                        auto* dataPtr = compiler->builder->CreateLoad(
                            compiler->GetType(dataField), dataSlot, "rangeptr");
                        auto* elemLLVMType = compiler->GetType(elemType);
                        auto* idxI64 = compiler->builder->CreateZExt(
                            compiler->CreateLoad(indexAlloca), compiler->builder->getInt64Ty(), "idxi64");
                        elemStorage = compiler->builder->CreateGEP(
                            elemLLVMType, dataPtr, idxI64, "rangeelemptr");
                        elemVal = compiler->builder->CreateLoad(
                            elemLLVMType, elemStorage, "rangeelem");
                        elemVal = compiler->ClearStringOwnedBit(elemVal);
                        elemVal = compiler->ClearStructOwnedBits(elemVal, elemType.TypeName);
                    }
                    else if (isFaceType)
                    {
                        elemVal = compiler->CallInterfaceMethod(ifacePtr, collNV.TypeAndValue.TypeName, "get", { indexNV });
                        if (compiler->lastCallReturnType.IsAlias
                            && !compiler->lastCallReturnType.Pointer && elemVal != nullptr)
                        {
                            elemVal = compiler->builder->CreateLoad(
                                compiler->GetType(elemType), elemVal, "rangealiasvalue");
                            elemVal = compiler->ClearStringOwnedBit(elemVal);
                            elemVal = compiler->ClearStructOwnedBits(elemVal, elemType.TypeName);
                        }
                    }
                    else
                    {
                        LLVMBackend::NamedVariable selfArg = collNV;
                        selfArg.TypeAndValue.VariableName = "";
                        elemVal = compiler->CreateOverloadedFunctionCall("get", { selfArg, indexNV });
                    }

                    auto& rangeVariable = compiler->GetOrCreateStackVariable(varName);
                    if (elemStorage != nullptr)
                    {
                        // Keep reads and writes of the loop variable on the live element slot. The
                        // fallback alloca is only needed for accessor legs that return a value.
                        rangeVariable.Storage = elemStorage;
                        rangeVariable.Primary = nullptr;
                        rangeVariable.BaseType = compiler->GetType(elemType);
                    }
                    else if (elemVal)
                    {
                        compiler->CreateAssignment(elemVal, elemAlloca);
                    }

                    RangeForContext rangeCtx;
                    rangeCtx.variableName = varName;
                    rangeCtx.collection = collNV;
                    rangeCtx.indexStorage = indexAlloca;
                    rangeCtx.collectionStorage = collNV.Storage;
                    rangeCtx.collectionValue = rangeCollectionValue;
                    rangeCtx.elementStorage = elemStorage;
                    rangeCtx.elementBaseType = compiler->GetType(elemType);
                    rangeCtx.elementType = elemType;
                    rangeCtx.interfaceStorage = ifacePtr;
                    rangeCtx.isFixedArray = isFixedArray;
                    rangeCtx.isArrayView = isArrayView;
                    rangeCtx.isInterface = isFaceType;
                    rangeForStack_.push_back(std::move(rangeCtx));
                    compiler->RecordMoveGenBind(varName); // element is a fresh binding each iteration
                    ParseControlledBody(bodyStmt);
                    rangeForStack_.pop_back();
                    compiler->CreateContinueCall();

                    // Increment block: i++
                    compiler->InitializeBlock(blockIncrement, false);
                    compiler->CreateIncrement(indexAlloca, 1);
                    compiler->CreateBlockBreak(blockCond, false);

                    // Resume
                    compiler->InitializeBlock(blockResume, false);
                    compiler->CreateBlockBreak(nullptr, true);

                    return;
                }
            }
        }
        else if (selectionStatement)
        {
            /*
            selectionStatement
                : 'if' '(' expression ')' statement ('else' statement)?
                | 'if' 'const' '(' expression ')' statement ('else' statement)?
                | 'switch' '(' expression ')' statement
                ;
            */

            if (selectionStatement->If() && selectionStatement->Const())
            {
                // if const (...) - compile-time conditional
                auto expression = selectionStatement->expression();
                auto innerStatement = selectionStatement->statement();

                int decision = DecideIfConstCondition(expression);
                if (decision < 0)
                {
                    LogErrorContext(selectionStatement, "'if const' condition must be a compile-time constant expression");
                    return;
                }

                bool taken = decision != 0;
                if (taken)
                    ParseStatement(innerStatement[0]);
                else if (innerStatement.size() > 1)
                    ParseStatement(innerStatement[1]);
                return;
            }
            else if (selectionStatement->If())
            {
                auto expression = selectionStatement->expression();
                auto innerStatement = selectionStatement->statement();

                // Parse condition value before CreateBlock
                auto blockCondition = compiler->CreateBasicBlock("ifCondition");
                auto blockTrue = compiler->CreateBasicBlock("ifTrue");
                auto blockResume = compiler->CreateBasicBlock("ifResume");
                llvm::BasicBlock* blockElse = selectionStatement->Else() == nullptr ? nullptr : compiler->CreateBasicBlock("ifFalse");
                auto blockFalse = blockElse ? blockElse : blockResume;

                compiler->CreateBlockBreak(blockCondition, false);

                compiler->InitializeBlock(blockCondition, true, nullptr, nullptr, blockFalse);
                auto condition = ParseExpression(expression);
                // Free owned-string temps produced inside the condition (e.g. `if (s.toString()
                // == x)`) here, while still in the condition block. The block-item flush runs in
                // the post-if merge block, where the dominance guard would drop them and leak.
                compiler->FlushOwnedTemps();
                compiler->CreateConditionJump(condition, blockTrue, blockFalse);

                auto preIfMovedState = compiler->SaveMovedState();

                // Only a `return` branch's moves truly vanish (the path exits the function). A
                // break/continue branch REJOINS later code (post-loop / next iteration), so its
                // moves must survive as maybe-moved - keep merging those. Track "this branch
                // returned" via straightLineReturned_ (set by the return handler), which is immune
                // to dead code after a return that would hide the ret terminator from a block check.
                bool enclosingReturned = straightLineReturned_;
                bool enclosingJumped = straightLineJumped_;

                compiler->InitializeBlock(blockTrue, false);
                straightLineReturned_ = false;
                straightLineJumped_ = false;
                ParseControlledBody(innerStatement[0]);
                bool thenReturned = straightLineReturned_ || straightLineJumped_;
                auto thenMovedState = compiler->SaveMovedState();

                compiler->CreateBlockBreak(blockResume, true);

                bool elseReturned = false;
                if (blockElse != nullptr)
                {
                    // Restore pre-branch moved state so else doesn't inherit if-branch moves.
                    compiler->RestoreMovedState(preIfMovedState);

                    // else statement
                    compiler->InitializeBlock(blockElse, true);
                    straightLineReturned_ = false;
                    straightLineJumped_ = false;
                    ParseControlledBody(innerStatement[1]);
                    elseReturned = straightLineReturned_ || straightLineJumped_;
                    auto elseMovedState = compiler->SaveMovedState();
                    compiler->CreateBlockBreak(blockResume, true);

                    // Drop an EXITING branch's moves (return/break/continue never reach resume).
                    // A break/continue path's moves are re-merged at loop exit, so they are not
                    // lost. If both fall through, a var is moved if moved in either.
                    if (thenReturned && !elseReturned)
                        compiler->RestoreMovedState(elseMovedState);
                    else if (!thenReturned && elseReturned)
                        compiler->RestoreMovedState(thenMovedState);
                    else if (thenReturned && elseReturned)
                        compiler->RestoreMovedState(preIfMovedState);
                    else
                        compiler->MergeMovedStates(thenMovedState, elseMovedState);
                }
                else if (thenReturned)
                {
                    // No else: the resume path is the condition-false path, which never ran the
                    // then-branch, so an exiting (return/break/continue) then-branch contributes
                    // no moves here. A break/continue's moves rejoin at loop exit instead.
                    compiler->RestoreMovedState(preIfMovedState);
                }

                // The enclosing straight-line has unconditionally returned iff it already had, or
                // this if returns on every path (both branches return; a no-else if falls through).
                straightLineReturned_ = enclosingReturned
                    || (blockElse != nullptr && thenReturned && elseReturned);
                straightLineJumped_ = enclosingJumped
                    || (blockElse != nullptr && thenReturned && elseReturned);

                // resume
                compiler->InitializeBlock(blockResume, false);
                return;
            }
            else if (selectionStatement->Switch())
            {
                // Conservatively treat a switch as falling through (a return in one case must not
                // mark the enclosing straight-line as returned); restore the flag after the body.
                ReturnFlagGuard returnFlagGuard(&straightLineReturned_);
                auto expression = selectionStatement->expression();
                auto body = selectionStatement->statement(0)->compoundStatement();

                SwitchContext switchCtx;
                switchCtx.resumeBlock = compiler->CreateBasicBlock("switchResume");

                // Pre-scan: collect all case/default labels and create their blocks
                if (body && body->blockItemList())
                {
                    for (auto blockItem : body->blockItemList()->blockItem())
                    {
                        auto stmt = blockItem->statement();
                        if (stmt) CollectCasesFromStatement(stmt, switchCtx);
                    }
                }

                // caseMap is keyed by parser-context pointers; sort by source position so
                // unordered-map bucket order cannot affect the generated IR.
                std::vector<CFlatParser::LabeledStatementContext*> orderedCaseLabels;
                orderedCaseLabels.reserve(switchCtx.caseMap.size());
                for (const auto& caseEntry : switchCtx.caseMap)
                    orderedCaseLabels.push_back(caseEntry.first);
                std::sort(orderedCaseLabels.begin(), orderedCaseLabels.end(),
                    [](auto* lhs, auto* rhs) {
                        return lhs->getStart()->getStartIndex() < rhs->getStart()->getStartIndex();
                    });

                auto switchDefault = switchCtx.defaultBlock ? switchCtx.defaultBlock : switchCtx.resumeBlock;

                // Owned-string temporaries produced while evaluating the scrutinee (the scrutinee
                // result of `switch (s.toString())`, plus any borrowed operands of a `switch (a + b)`
                // concat) are temporaries nothing else frees. They cannot be flushed before the
                // dispatch - the string-switch strcmp chain reads the scrutinee's buffer - and
                // matched cases branch away before any common post-dispatch point. So collect every
                // temp registered during scrutinee evaluation, pull them off the pending list (the
                // merge-block flush would otherwise drop them via the dominance guard and leak), and
                // re-register them at resumeBlock below, where every case/default path converges.
                // (A `return` inside a case leaks on that path - the same documented across-branch
                // temp limitation that applies elsewhere.)
                size_t pendingBefore = compiler->pendingOwnedStringTemps.size();
                // Same evaluation ParseExpression performs, but keeping the operand's signedness:
                // case labels are converted into the operand's type and that needs its signedness.
                llvm::Value* condVal = nullptr;
                bool condIsUnsigned = false;
                auto* condAssignCtx = expression->assignmentExpression();
                if (condAssignCtx != nullptr && condAssignCtx->conditionalExpression() != nullptr)
                {
                    auto condTyped = ParseConditionalExpression(condAssignCtx->conditionalExpression());
                    condVal = condTyped.value;
                    condIsUnsigned = condTyped.isUnsigned;
                    ProcessPlusPlus();
                }
                else
                {
                    condVal = ParseExpression(expression);
                }
                switchCtx.condValue = condVal;

                std::vector<llvm::Value*> scrutineeTemps;
                for (size_t t = pendingBefore; t < compiler->pendingOwnedStringTemps.size(); t++)
                    scrutineeTemps.push_back(compiler->pendingOwnedStringTemps[t].first);
                compiler->pendingOwnedStringTemps.resize(pendingBefore);
                // A bare owned-call scrutinee (`switch (call())`) is not registered by any operator
                // site, so add condVal explicitly when it is an owned string temp not already collected.
                auto* condStrTy = llvm::StructType::getTypeByName(*compiler->context, "string");
                if (compiler->lastCallReturnsOwned && condVal && condStrTy != nullptr
                    && condVal->getType() == condStrTy
                    && std::find(scrutineeTemps.begin(), scrutineeTemps.end(), condVal) == scrutineeTemps.end())
                    scrutineeTemps.push_back(condVal);
                compiler->lastCallReturnsOwned = false;

                if (switchCtx.isTypeSwitch)
                {
                    // Type switch: dispatch on the concrete type behind an interface fat pointer
                    auto fatTy = compiler->GetFatPtrType();
                    auto ptrTy = cflat_llvm::PointerTo(compiler->builder->getInt8Ty());

                    // Validate: switch expression must be an interface-typed value (fat pointer)
                    if (condVal->getType() != fatTy && condVal->getType() != cflat_llvm::PointerTo(fatTy))
                        LogErrorContext(expression, "type switch expression must be interface-typed (fat pointer)");

                    // Extract dataPtr (field 1 of fat pointer)
                    llvm::Value* dataPtr;
                    if (condVal->getType()->isStructTy())
                    {
                        dataPtr = compiler->builder->CreateExtractValue(condVal, {1u});
                    }
                    else
                    {
                        auto dpField = compiler->builder->CreateStructGEP(fatTy, condVal, 1);
                        dataPtr = compiler->builder->CreateLoad(ptrTy, dpField);
                    }

                    // Load type descriptor from vtable[0]
                    llvm::Value* loadedDesc = LoadTypeDescFromInterface(condVal, expression);

                    // Emit linear dispatch chain: for each type case, check if it matches
                    for (auto* labeledCtx : orderedCaseLabels)
                    {
                        auto& entry = switchCtx.caseMap.at(labeledCtx);
                        if (!entry.isTypeCase) continue;  // skip non-type cases (shouldn't happen)

                        auto* nextCheck = compiler->CreateBasicBlock("typeswitch_next");

                        if (auto dsIt = compiler->dataStructures.find(entry.typeCaseName); dsIt != compiler->dataStructures.end())
                        {
                            // Concrete struct case: single type descriptor comparison
                            auto& sd = dsIt->second;
                            if (!sd.typeDescriptor)
                            {
                                LogErrorContext(expression, std::format("struct '{}' has no type descriptor",
                                    SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = entry.typeCaseName })));
                                continue;
                            }
                            auto* cmp = compiler->builder->CreateICmpEQ(loadedDesc, sd.typeDescriptor);
                            compiler->builder->CreateCondBr(cmp, entry.block, nextCheck);
                        }
                        else if (compiler->programTable.count(entry.typeCaseName))
                        {
                            // Concrete program case: single type descriptor comparison
                            auto& pd = compiler->programTable[entry.typeCaseName];
                            if (!pd.typeDescriptor)
                            {
                                LogErrorContext(expression, std::format("program '{}' has no type descriptor",
                                    SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = entry.typeCaseName })));
                                continue;
                            }
                            auto* cmp = compiler->builder->CreateICmpEQ(loadedDesc, pd.typeDescriptor);
                            compiler->builder->CreateCondBr(cmp, entry.block, nextCheck);
                        }
                        else if (compiler->HasInterface(entry.typeCaseName))
                        {
                            // Interface case: match if the concrete type implements this interface
                            // Emit: if (typedesc == any_implementing_struct_typedesc) goto case block
                            llvm::BasicBlock* anyMatchedBlock = entry.block;
                            auto* nextStruct = nextCheck;

                            // Enumerate all classes/structs that implement this interface
                            for (auto& [sName, sd] : compiler->dataStructures)
                            {
                                if (!compiler->StructImplementsInterface(sName, entry.typeCaseName)) continue;
                                if (!sd.typeDescriptor) continue;

                                auto* matchBlock = compiler->CreateBasicBlock("typeswitch_match");
                                auto* cmpNextStruct = compiler->CreateBasicBlock("typeswitch_cmp_next");

                                auto* cmp = compiler->builder->CreateICmpEQ(loadedDesc, sd.typeDescriptor);
                                compiler->builder->CreateCondBr(cmp, matchBlock, cmpNextStruct);

                                compiler->SwitchToBlock(matchBlock);
                                compiler->builder->CreateBr(anyMatchedBlock);

                                compiler->SwitchToBlock(cmpNextStruct);
                                nextStruct = cmpNextStruct;
                            }
                            // Also enumerate programs that implement this interface
                            for (auto& [pName, pd] : compiler->programTable)
                            {
                                if (!compiler->StructImplementsInterface(pName, entry.typeCaseName)) continue;
                                if (!pd.typeDescriptor) continue;

                                auto* matchBlock = compiler->CreateBasicBlock("typeswitch_match");
                                auto* cmpNextStruct = compiler->CreateBasicBlock("typeswitch_cmp_next");

                                auto* cmp = compiler->builder->CreateICmpEQ(loadedDesc, pd.typeDescriptor);
                                compiler->builder->CreateCondBr(cmp, matchBlock, cmpNextStruct);

                                compiler->SwitchToBlock(matchBlock);
                                compiler->builder->CreateBr(anyMatchedBlock);

                                compiler->SwitchToBlock(cmpNextStruct);
                                nextStruct = cmpNextStruct;
                            }
                            // Fall through nextStruct to nextCheck
                            compiler->builder->CreateBr(nextCheck);
                        }
                        else
                        {
                            LogErrorContext(expression, std::format("'{}' is not a known struct or interface type",
                                SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = entry.typeCaseName })));
                        }

                        compiler->SwitchToBlock(nextCheck);
                    }
                    // Fall through to default case
                    compiler->CreateJump(switchDefault);
                }
                else if (switchCtx.isStringSwitch)
                {
                    // String switch: emit if-else chain using strcmp on _ptr (field 0)
                    auto* strPtr = compiler->builder->CreateExtractValue(condVal, { 0u });
                    auto* strcmpFn = compiler->GetOrDeclareStrcmp();
                    auto* i32Zero = compiler->builder->getInt32(0);

                    for (auto* labeledCtx : orderedCaseLabels)
                    {
                        auto& entry = switchCtx.caseMap.at(labeledCtx);
                        if (!entry.strLiteral) continue;
                        auto* nextBlock = compiler->CreateBasicBlock("switchCmp");
                        auto* cmpResult = compiler->builder->CreateCall(strcmpFn, { strPtr, entry.strLiteral });
                        auto* isEqual = compiler->builder->CreateICmpEQ(cmpResult, i32Zero);
                        compiler->builder->CreateCondBr(isEqual, entry.block, nextBlock);
                        compiler->SwitchToBlock(nextBlock);
                    }
                    compiler->CreateJump(switchDefault);
                }
                else
                {
                    auto switchInst = compiler->CreateSwitchInst(condVal, switchDefault, (unsigned)switchCtx.caseMap.size());
                    std::map<std::string, CFlatParser::LabeledStatementContext*> seenCaseValues;
                    for (auto* labeledCtx : orderedCaseLabels)
                    {
                        auto& entry = switchCtx.caseMap.at(labeledCtx);
                        if (!entry.value)  // null value = wildcard (_) arm, handled via defaultBlock
                            continue;
                        if (condVal->getType()->isIntegerTy())
                        {
                            auto wide = compiler->WidenCaseValue(entry.value, entry.valueIsUnsigned);
                            if (!compiler->CaseValueFits(wide, entry.valueIsUnsigned, condVal->getType(), condIsUnsigned))
                                LogErrorContext(labeledCtx, compiler->LocalizeMessage(
                                    "case label '{}' does not fit the switch operand type '{}'",
                                    { llvm::toString(wide, 10, true),
                                      LLVMBackend::SpellIntegerType(condVal->getType(), condIsUnsigned) }));
                        }
                        auto* coerced = compiler->CoerceCaseValue(entry.value, condVal->getType(), entry.valueIsUnsigned);
                        // Two labels that convert to the same operand-typed constant would make an
                        // invalid SwitchInst, so diagnose them here instead of failing verification.
                        std::string key = llvm::toString(coerced->getValue(), 16, false);
                        if (seenCaseValues.count(key) > 0)
                            LogErrorContext(labeledCtx, compiler->LocalizeMessage(
                                "duplicate case label '{}' in switch",
                                { llvm::toString(coerced->getValue(), 10, !condIsUnsigned) }));
                        seenCaseValues[key] = labeledCtx;
                        switchInst->addCase(coerced, entry.block);
                    }
                }

                // Push scope: break -> resumeBlock, no continue (propagates to outer loop)
                compiler->InitializeBlock(nullptr, true, nullptr, switchCtx.resumeBlock, nullptr);

                switchStack.push_back(switchCtx);

                if (body && body->blockItemList())
                    ParseBlockItemList(body->blockItemList());

                // Fallthrough at end of switch body -> resume
                compiler->CreateBlockBreak(switchCtx.resumeBlock, true);

                switchStack.pop_back();

                compiler->InitializeBlock(switchCtx.resumeBlock, false);
                // Free the scrutinee temporaries now that the dispatch is done and all paths have
                // converged here. Registered against resumeBlock so the block-item flush (which runs
                // next, still positioned in resumeBlock) emits the destructors; each temp is computed
                // in the switch-entry block, which dominates resumeBlock.
                for (auto* t : scrutineeTemps)
                    compiler->RegisterOwnedStringTemp(t);
                return;
            }
        }
        else if (compoundStatement)
        {
            compiler->InitializeBlock(nullptr, true);
            auto blockList = compoundStatement->blockItemList();
            if (blockList)
                ParseBlockItemList(blockList);
            compiler->CreateBlockBreak(nullptr, true);
            return;
        }
        else if (expectErrorStmt)
        {
            std::string rawText = expectErrorStmt->StringLiteral()->getText();
            compilerLLVM->expectedError = ProcessRawText(rawText);

            if (auto* cs = expectErrorStmt->compoundStatement())
            {
                // Scoped block form: expect_error("msg") { ... } - error must occur inside the braces.
                compilerLLVM->expectedErrorScopeDepth = SIZE_MAX;  // manual check after block
                size_t savedDepth = compilerLLVM->stackNamedVariable.size();
                auto* entryBB = compilerLLVM->builder->GetInsertBlock();
                bool entryWasUnterminated = entryBB != nullptr && cflat_llvm::GetTerminatorOrNull(entryBB) == nullptr;
                auto ownedTempMark = compiler->MarkOwnedTemps();
                // Rewind point for the per-function pending logs (see PendingAnalysisMark).
                auto* markedFn = entryBB != nullptr ? entryBB->getParent() : nullptr;
                auto pendingMark = compilerLLVM->MarkPendingAnalyses(markedFn);
                compiler->InitializeBlock(nullptr, true);
                bool errorReceived = false;
                try
                {
                    if (auto* blockList = cs->blockItemList())
                        ParseBlockItemList(blockList);
                }
                catch (const ExpectedErrorReceived&)
                {
                    errorReceived = true;
                    // The rest of the block never ran (see the file-scope catch). Today the grammar
                    // puts no classDefinition in a function body, so this is a defensive no-op.
                    ForgetIfConstGuardedImpls(compilerLLVM, cs);
                    // Pop any extra nested frames without destructors (error path).
                    while (compilerLLVM->stackNamedVariable.size() > savedDepth)
                        compilerLLVM->stackNamedVariable.pop_back();
                    // The swallowed diagnostic already answered everything this block queued;
                    // leaving it queued makes the end-of-module sweep report it a second time.
                    compilerLLVM->RewindPendingAnalyses(markedFn, pendingMark);
                    compilerLLVM->DiscardOwnedTempsSince(ownedTempMark);
                    // Terminate abandoned blocks and reconnect the live path to recovery.
                    if (auto* bb = compilerLLVM->builder->GetInsertBlock())
                    {
                        auto* function = bb->getParent();
                        auto* resume = llvm::BasicBlock::Create(
                            *compilerLLVM->context, "after_expect_error", function);
                        if (!compiler->IsBlockTerminated())
                            compilerLLVM->builder->CreateUnreachable();
                        // A throwing diagnostic can unwind through a guarded expression whose
                        // sibling arm is not the current insert block (for example `?.`). The
                        // expectation harness continues compilation, so close every abandoned
                        // block in this function before creating its recovery continuation.
                        for (auto& abandoned : *function)
                        {
                            if (&abandoned == resume)
                                continue;
                            if (cflat_llvm::GetTerminatorOrNull(&abandoned) == nullptr)
                            {
                                compilerLLVM->builder->SetInsertPoint(&abandoned);
                                compilerLLVM->builder->CreateUnreachable();
                            }
                        }
                        if (entryWasUnterminated && entryBB->getParent() == function)
                        {
                            if (auto* terminator = cflat_llvm::GetTerminatorOrNull(entryBB))
                                terminator->eraseFromParent();
                            compilerLLVM->builder->SetInsertPoint(entryBB);
                            compilerLLVM->builder->CreateBr(resume);
                        }
                        compilerLLVM->builder->SetInsertPoint(resume);
                        expectErrorRecoveryBlock_ = resume;
                    }
                    compilerLLVM->RestoreFileScopeExpectedError();
                }

                if (!errorReceived)
                {
                    compiler->CreateBlockBreak(nullptr, true);
                    if (!compilerLLVM->expectedError.empty())
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
            else
            {
                // Bare-semicolon form: expect_error("msg"); - error must occur before the enclosing scope exits.
                compilerLLVM->expectedErrorScopeDepth = compilerLLVM->stackNamedVariable.size();
            }
            return;
        }

        else if (auto* lockStmt = statement->lockStatement())
        {
            // lock (expr1, expr2, ...) { body }
            // Acquire all mutexes in order, parse body, release in reverse order on scope exit.
            auto* lockClauseCtx = lockStmt->lockClause();
            auto lockArgs = lockClauseCtx->lockArgList()->expression();

            struct AcquiredLock
            {
                std::string canonical;
                std::string acquireMethod;
                std::string releaseMethod;
                std::string typeName;
                LockMode heldMode = LockMode::Exclusive;
                llvm::Value* mutexPtr = nullptr; // address release() is called on - the mutex itself
            };
            std::vector<AcquiredLock> acquired;

            for (auto* exprCtx : lockArgs)
            {
                // Determine mode: strip trailing .read/.write soft keyword.
                std::string mode = GetLockArgMode(exprCtx);
                std::string canonical = GetLockArgCanonical(exprCtx);

                // Evaluate the base expression (without .read/.write suffix).
                // For .read/.write, we need the base expression's assignmentExpression context.
                // Since getText() strips whitespace, we reconstruct the base by truncating.
                // The simplest approach: evaluate the full expression but intercept if mode set.
                LLVMBackend::NamedVariable mutexNV;
                if (mode.empty())
                {
                    mutexNV = ParseAssignmentExpressionNamed(exprCtx->assignmentExpression());
                }
                else
                {
                    // `rw.read` / `rw.write`: the mode suffix is a soft keyword, not a real field.
                    // Evaluate the postfix chain minus the trailing '.'/'read'/'write' children.
                    auto* pfx = tryGetPostfixExpression(exprCtx);
                    if (pfx == nullptr || pfx->children.size() < 3)
                    {
                        LogErrorContext(lockStmt, std::format(
                            "lock: '{}' mode requires a simple lock target, e.g. 'lock(rw.{})'.", mode, mode));
                        return;
                    }
                    mutexNV = ParsePostfixExpression(pfx, true, 2);
                }

                // Spill into alloca if returned by value (no storage pointer).
                if (mutexNV.Storage == nullptr && mutexNV.Primary != nullptr)
                {
                    llvm::Type* ty = mutexNV.BaseType ? mutexNV.BaseType : compiler->GetType(mutexNV.TypeAndValue);
                    auto* spill = compiler->CreateAlloca(ty);
                    compiler->CreateAssignment(mutexNV.Primary, spill);
                    mutexNV.Storage = spill;
                    mutexNV.Primary = nullptr;
                }

                if (!mutexNV.Storage)
                {
                    LogErrorContext(lockStmt, std::format("lock: expression '{}' must be a lock variable.", canonical));
                    return;
                }

                std::string mutexTypeName = mutexNV.TypeAndValue.TypeName;

                // A dynamically-dispatched lock cannot be named: the lock-set analysis
                // canonicalizes targets by name, so an interface-typed value is rejected.
                if (compiler->HasInterface(mutexTypeName))
                {
                    LogErrorContext(lockStmt, std::format(
                        "lock: '{}' is an interface value of type '{}'; lock() requires a concrete lock type.",
                        canonical, SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = mutexTypeName })));
                    return;
                }

                // Classify the receiver against the capability table. "write" and "" are both
                // the exclusive mode; "read" selects the shared row; "optimistic" demands
                // IOptimisticLockable, which is a checking capability with no scoped lowering.
                std::string wantMode = (mode == "read" || mode == "optimistic") ? mode : "";
                const char* wantIface = CapabilityForLockMode(wantMode);
                if (!compiler->TypeHasCapability(mutexTypeName, wantIface))
                {
                    if (wantMode.empty())
                        LogErrorContext(lockStmt, std::format(
                            "lock: type '{}' is not a lock type (missing [Capability(ILockable)]).",
                            SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = mutexTypeName })));
                    else
                        LogErrorContext(lockStmt, std::format(
                            "lock: type '{}' does not support '{}' mode (missing [Capability({})]).",
                            SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = mutexTypeName }), wantMode, wantIface));
                    return;
                }

                const CapabilitySpec* spec = nullptr;
                for (const auto& cap : kCapabilities)
                {
                    if (wantMode != cap.Mode) continue;
                    if (!compiler->TypeHasCapability(mutexTypeName, cap.Iface)) continue;
                    spec = &cap;
                    break;
                }
                if (spec == nullptr)
                {
                    // Only 'optimistic' reaches here: it validates, it does not acquire, so there
                    // is no acquire/release pair for a scoped block to bracket the body with.
                    LogErrorContext(lockStmt, std::format(
                        "lock: '{}' mode has no scoped form; call '{}.read(() => {{ ... }})' instead.",
                        wantMode, canonical));
                    return;
                }
                std::string acquireMethod = spec->Acquire;
                std::string releaseMethod = spec->Release;

                // For a pointer lock arg (mutex*), Storage is the address of the pointer field
                // (mutex**); releasing that would unlock the wrong address and leak the lock forever.
                llvm::Value* mutexPtr = mutexNV.Storage;
                if (mutexNV.TypeAndValue.Pointer)
                    mutexPtr = compiler->CreateLoad(compiler->builder->getPtrTy(), mutexNV.Storage);

                // Call acquire / acquire_read.
                LLVMBackend::NamedVariable selfArg = mutexNV;
                selfArg.TypeAndValue.VariableName = "";
                compiler->CreateOverloadedFunctionCall(acquireMethod, { selfArg });

                llvm::Function* unlockFn = FindMethodOf(releaseMethod, mutexTypeName);
                if (!unlockFn)
                {
                    LogErrorContext(lockStmt, std::format(
                        "lock: type '{}' has no '{}' method.",
                        SpellType(*compiler, LLVMBackend::TypeAndValue{ .TypeName = mutexTypeName }), releaseMethod));
                    return;
                }

                acquired.push_back({ canonical, acquireMethod, releaseMethod, mutexTypeName,
                                     LockModeFromSuffix(mode), mutexPtr });
            }

            // Push the lock scope and register a cleanup for every acquired mutex, so
            // scope exit releases all of them (in reverse) instead of only the first.
            compiler->InitializeBlock(nullptr, true);
            for (const auto& lk : acquired)
            {
                compiler->stackNamedVariable.back().lockCleanups.push_back(
                    LLVMBackend::StackState::LockCleanup{
                        .UnlockFn = FindMethodOf(lk.releaseMethod, lk.typeName),
                        .MutexPtr = lk.mutexPtr,
                    });
            }

            // Update lock-set for static analysis. Save each token's prior state so a nested
            // `lock (m.read)` inside an outer `lock (m)` restores the outer mode on exit
            // instead of dropping the lock from the set entirely.
            std::vector<std::pair<std::string, std::optional<LockMode>>> savedTokens;
            for (const auto& lk : acquired)
            {
                for (auto& tok : LockSetAliases(lk.canonical))
                {
                    auto it = currentLockSet.find(tok);
                    savedTokens.emplace_back(tok,
                        it == currentLockSet.end() ? std::optional<LockMode>{} : std::optional<LockMode>{ it->second });
                    currentLockSet[tok] = lk.heldMode;
                }
            }

            // Parse the body.
            auto* blockList = lockStmt->compoundStatement()->blockItemList();
            if (blockList)
                ParseBlockItemList(blockList);

            // Restore the lock-set.
            for (auto it = savedTokens.rbegin(); it != savedTokens.rend(); ++it)
            {
                if (it->second.has_value())
                    currentLockSet[it->first] = *it->second;
                else
                    currentLockSet.erase(it->first);
            }

            // Close the scope - EmitDestructorsForScope will call unlock().
            compiler->CreateBlockBreak(nullptr, true);
            return;
        }

        LogErrorContext(statement, "Unhandled statement type.");
        return;
    }

void MainListener::GenerateDefaultParamOverloads(
        const std::string& name,
        const LLVMBackend::DeclTypeAndValue& returnType,
        const std::vector<LLVMBackend::DeclTypeAndValue>& params,
        bool varargs,
        size_t line) {
        auto* compiler = Compiler();
        int firstDefault = -1;
        for (int i = 0; i < (int)params.size(); i++)
        {
            if (params[i].DefaultValue != nullptr)
            {
                firstDefault = i;
                break;
            }
        }

        if (firstDefault < 0)
            return;

        for (int cutoff = firstDefault; cutoff < (int)params.size(); cutoff++)
        {
            std::vector<LLVMBackend::TypeAndValue> wrapperParams(params.begin(), params.begin() + cutoff);

            /*
             * The slot this wrapper needs can already hold a body. CreateFunctionDefinition would
             * take its !fn->empty() early return, which pushes NO function scope - and the emission
             * below would then write into a foreign, terminated block and pop a scope frame it never
             * pushed, corrupting the scope stack (nondeterministic SIGABRT / SIGSEGV / "declared with
             * no enclosing scope"). Decide BEFORE the call instead.
             */
            std::string clashFile;
            size_t clashLine = 0;
            if (compiler->OverloadSlotIsDefined(name, returnType, wrapperParams, false, &clashFile, &clashLine))
            {
                // Line 0 is a compiler-synthesized definition (e.g. a union's default constructor):
                // yield to it silently, exactly as a duplicate synthesized body does elsewhere.
                if (clashLine == 0)
                    continue;
                compiler->LogError(std::format(
                    "'{}' with its parameter defaults filled in matches an overload that is already "
                    "defined at {}({}) - a call could mean either. Remove one of the two definitions, "
                    "or drop the default from a parameter so the two no longer overlap.",
                    name, clashFile, clashLine));
            }

            auto wrapperFn = compiler->CreateFunctionDefinition(name, returnType, wrapperParams, false, false, line);
            compiler->InitializeBlock(&wrapperFn->front(), false);
            // Fresh straight-line for the wrapper body; restore the enclosing walk's flag on exit.
            ReturnFlagGuard wrapperReturnFlagGuard(&straightLineReturned_);
            straightLineReturned_ = false;

            // Clear each arg's VariableName: MatchFunction treats a non-empty VariableName as a
            // NAMED argument, which hard-errors on same-named methods of unrelated types instead of rejecting.
            std::vector<LLVMBackend::NamedVariable> callArgs;

            for (int i = 0; i < cutoff; i++)
            {
                auto arg = compiler->GetFunctionArgument(params[i].VariableName);
                arg.TypeAndValue.VariableName = "";   // forward positionally, never as a named arg
                // A 'move' parameter must forward AS a move, or the wrapper binds the borrow
                // overload and its own scope-exit teardown frees what the callee kept.
                if (params[i].IsMove)
                    arg.IsExplicitMove = true;
                callArgs.push_back(arg);
            }

            for (int i = cutoff; i < (int)params.size(); i++)
            {
                auto* initCtx = params[i].DefaultValue;
                llvm::Value* defaultVal = nullptr;
                bool defaultIsUnsigned = false;
                if (auto* ae = initCtx->assignmentExpression())
                {
                    // Default arguments are lowered in a synthesized forwarding wrapper, but a
                    // lambda literal still needs the declared parameter's function signature.
                    // Restore on LogError unwind so the wrapper cannot leak its context.
                    LambdaExpectedTypeRestoreGuard lambdaExpectedTypeRestore(&lambdaExpectedType);
                    DeclExpectedTypeScope defaultExpectedScope(&declExpectedType, params[i]);
                    if (params[i].IsFunctionPointer)
                        lambdaExpectedType = params[i];
                    else if (const auto* encoded = compiler->GetEncodedClosureType(params[i].TypeName))
                        lambdaExpectedType = *encoded;
                    // Parameter-DEFAULT leg of the code-value store gate: `f(Rec* p = ro)` put a
                    // code address in the omitted argument's slot and the body wrote through it.
                    auto defNV = ParseAssignmentExpressionNamed(ae);
                    defaultVal = LoadNamedVariable(defNV);
                    defaultIsUnsigned = defNV.TypeAndValue.IsUnsignedInteger() != -1;
                    RejectCodeValueIntoDataSlot(ae, defNV, params[i], "default-initialize",
                        std::format("parameter '{}' of", params[i].VariableName));
                    // Thin-function sibling of the gate above: a data pointer default for a
                    // thin 'function<>' destination would be called as code (7th ungated site).
                    if (defaultVal && !params[i].Pointer && params[i].ConstArraySize == 0
                        && defaultVal->getType()->isPointerTy() && !defaultVal->getType()->isStructTy())
                    {
                        const LLVMBackend::TypeAndValue* thinDest = nullptr;
                        if (params[i].IsFunctionPointer)
                            thinDest = &params[i];
                        else
                            thinDest = compiler->GetEncodedClosureType(params[i].TypeName);
                        if (thinDest && thinDest->IsThinFnPtr())
                            compiler->CheckThinFnPtrAssignProvenance(defaultVal, defNV,
                                std::format("'{}'", params[i].VariableName));
                        // Fat sibling: the parameter default never widens (see the doc
                        // comment), so a provable data pointer needs its own reject here.
                        else if (thinDest)
                            compiler->CheckFatClosureAssignProvenance(defaultVal, defNV,
                                std::format("'{}'", params[i].VariableName));
                    }
                }
                else if (initCtx->Default())
                {
                    defaultVal = GenerateDefaultValue(params[i]);
                }
                else if (initCtx->LeftBrace() != nullptr)
                {
                    // Gated on the brace TOKEN so the EMPTY form ('int f(int x = {})') reaches
                    // this arm too - initializerList() is null for '{}' and it used to miss.
                    auto* initList = initCtx->initializerList();
                    /*
                     * An array VIEW is Pointer-flagged but is not a pointer target. Exempt it
                     * exactly as the declarator guard does: the empty form gets the declarator's
                     * own length message instead of naming a pointer that is not there, and the
                     * non-empty form - which the declarator supports by building backing storage,
                     * and which a parameter default cannot - says so rather than borrowing the
                     * pointer wording. Both LogErrorContext calls throw, so neither falls through.
                    */
                    if (params[i].IsArrayView)
                    {
                        LLVMBackend::TypeAndValue parameterBase = params[i];
                        parameterBase.Pointer = false;
                        parameterBase.ElemPointer = false;
                        parameterBase.PointerDepth = 0;
                        parameterBase.IsArrayView = false;
                        const std::string parameterBaseSpelling = SpellType(*compiler, parameterBase);
                        if (initList == nullptr)
                            LogErrorContext(initCtx, std::format(
                                "cannot infer the length of '{}[]' from an empty initializer list; "
                                "use an explicit size '{}[N]'",
                                parameterBaseSpelling, parameterBaseSpelling));
                        LogErrorContext(initCtx, std::format(
                            "a brace list is not supported as the default for the array-view "
                            "parameter '{}' of type '{}[]' - it would need backing storage the "
                            "default cannot own; use '= default' and fill it in the body",
                            params[i].VariableName, parameterBaseSpelling));
                    }

                    if (params[i].Pointer)
                    {
                        // Non-empty: field-inits through a POINTER alloca, so the caller gets the
                        // field bytes as 'p'. Empty: ambiguous between null and a default object.
                        std::string role = std::format("parameter default for '{}'", params[i].VariableName);
                        if (initList != nullptr)
                            LogPointerBraceInitReject(initCtx, role, params[i].TypeName,
                                DescribePointerDeclType(params[i]),
                                CanSuggestAllocation(initCtx, params[i]));
                        else
                            LogEmptyBraceOnPointerReject(initCtx, role, params[i]);
                    }

                    // Field initializer default: build the struct, apply overrides, pass by value.
                    defaultVal = GenerateDefaultValue(params[i]);
                    if (defaultVal && initList != nullptr)
                    {
                        auto* alloca = compiler->CreateAlloca(defaultVal->getType());
                        compiler->CreateAssignment(defaultVal, alloca);
                        EmitFieldInitializer(alloca, params[i].TypeName, initList);
                        defaultVal = compiler->CreateLoad(alloca);
                    }
                }
                // An unsupported default-initializer spelling (notably an empty '= {}') leaves
                // defaultVal null, which the call below dereferences - diagnose instead of crashing.
                if (!defaultVal)
                    LogErrorContext(initCtx, std::format(
                        "cannot build the default value for parameter '{}' of type '{}' - this default "
                        "initializer form is not supported; use '= default' or an explicit expression",
                        params[i].VariableName, SpellType(*compiler, params[i])));

                // A default value is a declaration initializer, so convert it into the
                // parameter's own slot exactly as an initializer does - the forwarding call
                // below claims the parameter's type, and an unconverted value (an i8 '2' for a
                // 'bool', an i16 '300' for a 'u8') reached the module verifier instead.
                if (defaultVal != nullptr && !params[i].Pointer && defaultVal->getType()->isIntegerTy())
                {
                    auto* slotType = compiler->GetType(params[i]);
                    if (slotType != nullptr && slotType->isIntegerTy() && slotType != defaultVal->getType())
                    {
                        unsigned slotBits = slotType->getIntegerBitWidth();
                        unsigned valueBits = defaultVal->getType()->getIntegerBitWidth();
                        if (slotBits == 1)
                            defaultVal = compiler->CoerceToBoolCondition(defaultVal);
                        else if (slotBits < valueBits)
                            defaultVal = compiler->builder->CreateTrunc(defaultVal, slotType);
                        else
                            defaultVal = compiler->Upconvert(defaultVal, slotType, defaultIsUnsigned);
                    }
                }

                LLVMBackend::NamedVariable namedVar;
                namedVar.Primary = defaultVal;
                namedVar.BaseType = defaultVal ? defaultVal->getType() : nullptr;
                namedVar.TypeAndValue.TypeName = params[i].TypeName;
                namedVar.TypeAndValue.Pointer = params[i].Pointer;
                // Naming the PARAMETER type would sign-extend an unsigned default into a wider
                // slot; carry the default expression's own unsigned width instead.
                if (defaultIsUnsigned && defaultVal != nullptr && defaultVal->getType()->isIntegerTy()
                    && namedVar.TypeAndValue.IsUnsignedInteger() == -1)
                {
                    unsigned bits = defaultVal->getType()->getIntegerBitWidth();
                    if      (bits == 8)  namedVar.TypeAndValue.TypeName = "u8";
                    else if (bits == 16) namedVar.TypeAndValue.TypeName = "u16";
                    else if (bits == 32) namedVar.TypeAndValue.TypeName = "u32";
                    else if (bits == 64) namedVar.TypeAndValue.TypeName = "u64";
                }
                callArgs.push_back(namedVar);
            }

            if (returnType.TypeName == "void")
            {
                compiler->CreateOverloadedFunctionCall(name, callArgs);
                compiler->CreateReturnCall(nullptr);
            }
            else
            {
                auto result = compiler->CreateOverloadedFunctionCall(name, callArgs);
                compiler->CreateReturnCall(result);
            }

            compiler->CreateBlockBreak(nullptr, true);
            compiler->ClearCurrentSubprogram();
        }
    }

int MainListener::EvaluateIfConstForSink(CFlatParser::ExpressionContext* expr) {
        if (expr == nullptr) return -1;
        // Runs PRE-BODY, when the builder points at a FOREIGN, already-terminated function block.
        // Route through the shared evaluator with forceScratch=true so every emitted leaf lands in
        // a throwaway function - never leaking instructions past that foreign block's terminator -
        // and suppress=true so an ill-formed condition (e.g. a not-yet-in-scope local) is silently
        // "cannot decide" here, to be re-evaluated and reported at real body codegen.
        auto v = EvalIfConstConstant(expr, /*forceScratch*/ true, /*suppress*/ true);
        if (!v) return -1;
        return (*v != 0) ? 1 : 0;
    }

IfConstEvaluator MainListener::SinkIfConstEvaluator() {
        return [this](CFlatParser::ExpressionContext* e) { return EvaluateIfConstForSink(e); };
    }

std::optional<int64_t> MainListener::EmitAndFoldIfConstLeaf(antlr4::tree::ParseTree* node, bool forceScratch, bool suppress) {
        if (node == nullptr) return std::nullopt;
        auto* compiler = Compiler();

        // Statement scope (a LIVE insert block that is the function body currently being emitted):
        // emit into it directly, exactly as the pre-evaluator code did. The dead leaf IR left
        // behind is harmless (a plain if-const decision, later DCE'd). forceScratch overrides this
        // for the owning-sink pre-body scan, where the "current" block is a FOREIGN, already-
        // terminated function block; emitting there would leak instructions past its terminator.
        // Liveness (not just non-null) is required: at FILE / member / interface scope the builder
        // still points at the last, already-terminated block of the previously emitted function.
        if (!forceScratch && compiler->IsInsertBlockLive())
        {
            llvm::Value* v = EmitIfConstLeafValue(node);
            uint64_t folded = 0;
            if (v && TryFoldConstInt(v, folded, &constFoldableGlobals_))
                return (int64_t)folded;
            return std::nullopt;
        }

        // No usable insert block - null, or terminated (declaration / member / interface scope, or
        // dead code after a return) - or forceScratch: emit into a throwaway function
        // (mirrors EvalGlobalArrayDim) so the builder has a valid, private
        // block. This is the crash fix - a global/enum load can no longer dereference a null insert
        // block - and it also keeps the owning-sink scan from corrupting a foreign function.
        auto savedState = compiler->SaveBuilderState();
        auto* savedFn = compiler->currentFunction;
        auto* voidTy = llvm::FunctionType::get(compiler->builder->getVoidTy(), false);
        auto* tmpFn = llvm::Function::Create(
            voidTy, llvm::Function::PrivateLinkage, "__if_const_eval_tmp", compiler->module.get());
        auto* tmpBB = llvm::BasicBlock::Create(*compiler->context, "entry", tmpFn);
        compiler->builder->SetInsertPoint(tmpBB);
        compiler->currentFunction = tmpFn;
        bool savedSuppress = compiler->suppressErrors_;
        if (suppress) compiler->suppressErrors_ = true;

        std::optional<int64_t> result = std::nullopt;
        try
        {
            llvm::Value* v = EmitIfConstLeafValue(node);
            uint64_t folded = 0;
            if (v && TryFoldConstInt(v, folded, &constFoldableGlobals_))
                result = (int64_t)folded;
        }
        catch (const SpeculativeEvalAbort&) {}
        catch (...)
        {
            compiler->suppressErrors_ = savedSuppress;
            compiler->currentFunction = savedFn;
            tmpFn->eraseFromParent();
            compiler->RestoreBuilderState(savedState);
            throw;
        }
        compiler->suppressErrors_ = savedSuppress;
        compiler->currentFunction = savedFn;
        tmpFn->eraseFromParent();
        compiler->RestoreBuilderState(savedState);
        return result;
    }

llvm::Value* MainListener::EmitIfConstLeafValue(antlr4::tree::ParseTree* node) {
        if (auto* i = dynamic_cast<CFlatParser::InclusiveOrExpressionContext*>(node))
            return ParseInclusiveOrExpression(i);
        if (auto* a = dynamic_cast<CFlatParser::LogicalAndExpressionContext*>(node))
            return ParseLogicalAndExpression(a);
        if (auto* o = dynamic_cast<CFlatParser::LogicalOrExpressionContext*>(node))
            return ParseLogicalOrExpression(o);
        if (auto* c = dynamic_cast<CFlatParser::ConditionalExpressionContext*>(node))
            return ParseConditionalExpression(c);
        if (auto* asn = dynamic_cast<CFlatParser::AssignmentExpressionContext*>(node))
            return ParseAssignmentExpression(asn);
        if (auto* e = dynamic_cast<CFlatParser::ExpressionContext*>(node))
            return ParseExpression(e);
        return nullptr;
    }

std::optional<int64_t> MainListener::EvalIfConstConstant(antlr4::tree::ParseTree* node, bool forceScratch, bool suppress) {
        if (node == nullptr) return std::nullopt;

        if (auto* c = dynamic_cast<CFlatParser::ConditionalExpressionContext*>(node))
        {
            // Ternary `cond ? a : b` short-circuits to one arm; `x ?? y` is not an integer const.
            if (c->expression() != nullptr)
            {
                auto cond = EvalIfConstConstant(c->logicalOrExpression(), forceScratch, suppress);
                if (!cond) return std::nullopt;
                return (*cond != 0) ? EvalIfConstConstant(c->expression(), forceScratch, suppress)
                                    : EvalIfConstConstant(c->conditionalExpression(), forceScratch, suppress);
            }
            if (c->children.size() > 1) return std::nullopt;  // `??` null-coalescing
            return EvalIfConstConstant(c->logicalOrExpression(), forceScratch, suppress);
        }
        if (auto* o = dynamic_cast<CFlatParser::LogicalOrExpressionContext*>(node))
        {
            auto operands = o->logicalAndExpression();
            if (operands.size() == 1) return EvalIfConstConstant(operands[0], forceScratch, suppress);
            // OR: any known-true wins; all-known-false is false; otherwise undecidable.
            bool allKnownFalse = true;
            for (auto* op : operands)
            {
                auto v = EvalIfConstConstant(op, forceScratch, suppress);
                if (v && *v != 0) return (int64_t)1;
                if (!v) allKnownFalse = false;
            }
            return allKnownFalse ? std::optional<int64_t>(0) : std::nullopt;
        }
        if (auto* a = dynamic_cast<CFlatParser::LogicalAndExpressionContext*>(node))
        {
            auto operands = a->inclusiveOrExpression();
            if (operands.size() == 1) return EvalIfConstConstant(operands[0], forceScratch, suppress);
            // AND: any known-false wins; all-known-true is true; otherwise undecidable.
            bool allKnownTrue = true;
            for (auto* op : operands)
            {
                auto v = EvalIfConstConstant(op, forceScratch, suppress);
                if (v && *v == 0) return (int64_t)0;
                if (!v) allKnownTrue = false;
            }
            return allKnownTrue ? std::optional<int64_t>(1) : std::nullopt;
        }
        if (auto* e = dynamic_cast<CFlatParser::ExpressionContext*>(node))
            return EvalIfConstConstant(e->assignmentExpression(), forceScratch, suppress);
        if (auto* asn = dynamic_cast<CFlatParser::AssignmentExpressionContext*>(node))
        {
            if (asn->conditionalExpression() != nullptr)
                return EvalIfConstConstant(asn->conditionalExpression(), forceScratch, suppress);
            return std::nullopt;  // an actual assignment is not a constant
        }
        // No short-circuit control flow at or below this node: emit-and-fold the leaf.
        return EmitAndFoldIfConstLeaf(node, forceScratch, suppress);
    }

int MainListener::DecideIfConstCondition(CFlatParser::ExpressionContext* expr) {
        auto v = EvalIfConstConstant(expr, /*forceScratch*/ false, /*suppress*/ false);
        if (!v) return -1;
        return (*v != 0) ? 1 : 0;
    }

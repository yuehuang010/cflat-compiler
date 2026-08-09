#include "MainListener.h"

LLVMBackend::NamedVariable MainListener::ParseAssignmentExpressionNamed(CFlatParser::AssignmentExpressionContext* ctx,
                                                              bool discardResult) {
        // Snapshot and clear the owned-return flag so FinishAssignmentExpressionNamed
        // can tell whether THIS expression (not a stale prior call) ended in an
        // owned-string-returning call.
        bool savedOwned = compilerLLVM->lastCallReturnsOwned;
        compilerLLVM->lastCallReturnsOwned = false;
        // Clear any stale new/move owning flag left by a PRIOR statement (e.g. a bare
        // `items.add(new T())` whose `new` result was moved into the callee, not consumed
        // by a declaration). Only a new/move parsed WITHIN this RHS may mark the target
        // owning; without this, the next pointer/interface declaration in the same scope
        // (notably `T* d = expr as U;`, whose cast produces no owning signal of its own)
        // falsely inherits ownership and double-frees at scope exit. Mirrors the existing
        // assignment-path reset (see operatorText == "=" below).
        compilerLLVM->lastOwningResult = false;
        compilerLLVM->lastAllocAlignment = 0;
        compilerLLVM->lastCallReturnsAllocAlign = 0;
        // Only a `move <element slot>` parsed WITHIN this RHS may key the container-slot ownership
        // override (see ParseDeclaration); clear any value left by a prior statement.
        compilerLLVM->lastMovedFromContainerSlot = false;

        auto* condCtx = ctx->conditionalExpression();
        if (condCtx && !ctx->assignmentOperator()
            && !condCtx->Question() && !condCtx->QuestionQuestion())
        {
            auto* lor = condCtx->logicalOrExpression();
            if (lor)
            {
                auto las = lor->logicalAndExpression();
                if (las.size() == 1)
                {
                    auto ios = las[0]->inclusiveOrExpression();
                    if (ios.size() == 1)
                    {
                        auto eos = ios[0]->exclusiveOrExpression();
                        if (eos.size() == 1)
                        {
                            auto ands = eos[0]->andExpression();
                            if (ands.size() == 1)
                            {
                                auto eqs = ands[0]->equalityExpression();
                                if (eqs.size() == 1)
                                {
                                    auto tcs = eqs[0]->typeCheckExpression();
                                    // Guard: skip the fast path if the typeCheckExpression has 'is'/'as'
                                    // operators - those must be handled by ParseConditionalExpression.
                                    if (tcs.size() == 1 && tcs[0]->typeSpecifier().empty())
                                    {
                                        auto* relCtx = tcs[0]->relationalExpression();
                                        auto rels = relCtx ? relCtx->shiftExpression() : std::vector<CFlatParser::ShiftExpressionContext*>{};
                                        if (rels.size() == 1)
                                        {
                                            auto shs = rels[0]->additiveExpression();
                                            if (shs.size() == 1)
                                            {
                                                auto adds = shs[0]->multiplicativeExpression();
                                                if (adds.size() == 1)
                                                {
                                                    auto muls = adds[0]->castExpression();
                                                    if (muls.size() == 1)
                                                    {
                                                        // Pure single-child passthrough all the way down:
                                                        // this fast path IS the only shape where a bare
                                                        // return-block call reaches the inliner, so forward
                                                        // discardResult unchanged. Any operator/value context
                                                        // takes the fallback below with discardResult=false.
                                                        return FinishAssignmentExpressionNamed(
                                                            ParseCastExpression(muls[0], false, discardResult), savedOwned);
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        // Fall back: call ParseConditionalExpression directly (when no assignment) so we can
        // recover the isUnsigned flag from TypedValue and synthesize the TypeName for Upconvert.
        {
            LLVMBackend::NamedVariable result;
            auto* condCtx = ctx->conditionalExpression();
            if (condCtx && !ctx->assignmentOperator())
            {
                auto tv = ParseConditionalExpression(condCtx);
                result.Primary = tv.value;
                if (result.Primary)
                {
                    result.BaseType = result.Primary->getType();
                    if (tv.isUnsigned && result.Primary->getType()->isIntegerTy())
                    {
                        unsigned bits = result.Primary->getType()->getIntegerBitWidth();
                        if      (bits == 8)  result.TypeAndValue.TypeName = "u8";
                        else if (bits == 16) result.TypeAndValue.TypeName = "u16";
                        else if (bits == 32) result.TypeAndValue.TypeName = "u32";
                        else if (bits == 64) result.TypeAndValue.TypeName = "u64";
                    }
                    // A '?:' join of two interface values yields a phi/select with no NamedVariable
                    // of its own to carry IsInterface/TypeName - it is a bare fat {vtable,data}
                    // struct. Recover the interface identity from the per-value ledger that
                    // PropagateFatInterfaceJoin stamped during the join, so a by-value call argument
                    // (which reads TypeAndValue, not the ledger) matches an interface parameter
                    // instead of showing the raw "__iface_fat_ptr" struct name to overload resolution.
                    else if (result.BaseType == compilerLLVM->GetFatPtrType())
                    {
                        std::string ifaceName = compilerLLVM->FindFatInterfaceValueTypeName(result.Primary);
                        if (!ifaceName.empty() && ifaceName != LLVMBackend::kAmbiguousFatInterface)
                        {
                            result.TypeAndValue.IsInterface = true;
                            result.TypeAndValue.TypeName = ifaceName;
                        }
                    }
                }
            }
            else
            {
                result.Primary = ParseAssignmentExpression(ctx);
                if (result.Primary) result.BaseType = result.Primary->getType();
            }
            return FinishAssignmentExpressionNamed(result, savedOwned);
        }
    }

bool MainListener::RejectPointerShapedInterfaceUpcast(antlr4::ParserRuleContext* errCtx,
                                            const LLVMBackend::TypeAndValue& src,
                                            const std::string& interfaceName) {
        if (interfaceName.empty()) return false;
        std::string shape = compilerLLVM->DescribePointerShapedInterfaceSource(src);
        if (shape.empty()) return false;
        LogErrorContext(errCtx, compilerLLVM->FormatPointerShapedInterfaceUpcastError(
            shape, src.TypeName, interfaceName));
        return true;
    }

bool MainListener::RejectPrimitiveShapedInterfaceUpcast(antlr4::ParserRuleContext* errCtx,
                                              const LLVMBackend::TypeAndValue& src,
                                              const std::string& interfaceName) {
        if (interfaceName.empty() || compilerLLVM->interfaceTable.count(interfaceName) == 0)
            return false;
        if (!LLVMBackend::IsPrimitiveTypeName(compilerLLVM->ResolveTypeAlias(src.TypeName)))
            return false;
        return RejectPointerShapedInterfaceUpcast(errCtx, src, interfaceName);
    }

LLVMBackend::InterfaceBoxSource MainListener::ClassifyInterfaceBoxSource(llvm::Value* dataPtr,
                                                              const LLVMBackend::NamedVariable* srcNV,
                                                              bool ownershipTransferred) {
        using Source = LLVMBackend::InterfaceBoxSource;
        if (dataPtr == nullptr) return Source::Unknown;
        llvm::Value* base = dataPtr->stripPointerCasts();
        while (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(base))
            base = gep->getPointerOperand()->stripPointerCasts();
        // Storage shape decides the FRAME case before ownership does: a by-value class local with
        // an owning binding still lives in this frame, and a box over it dies with the frame.
        if (llvm::isa<llvm::AllocaInst>(base)) return Source::FrameStorage;
        if (ownershipTransferred || (srcNV != nullptr && srcNV->IsOwning)
            || compilerLLVM->IsOwningValue(dataPtr))
            return Source::Heap;
        if (srcNV != nullptr && !srcNV->CallerName.empty()
            && compilerLLVM->IsFunctionParameter(srcNV->CallerName))
            return Source::Parameter;
        if (llvm::isa<llvm::GlobalVariable>(base)) return Source::Global;
        if (llvm::isa<llvm::Argument>(base)) return Source::Parameter;
        return Source::Unknown;
    }

bool MainListener::RetireOwningSourceOfBoxedValue(llvm::Value* value) {
        auto* compiler = compilerLLVM;
        auto* load = llvm::dyn_cast_or_null<llvm::LoadInst>(value);
        if (load == nullptr || !load->getType()->isPointerTy()) return false;
        if (!compiler->IsOwningValue(load)) return false;
        auto* slot = load->getPointerOperand();
        compiler->builder->CreateStore(
            llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(load->getType())), slot);
        std::string name = compiler->FindVariableNameByStorage(slot);
        if (!name.empty())
        {
            compiler->MarkVariableMoved(name);
            compiler->MarkVariableMovedIntoInterface(name);
        }
        return true;
    }

llvm::Value* MainListener::BoxConcreteIntoInterface(antlr4::ParserRuleContext* errCtx, llvm::Value* srcValue,
                                          bool sourceIsPointer, const std::string& structName,
                                          const std::string& interfaceName,
                                          const LLVMBackend::NamedVariable* srcNV,
                                          bool adoptsOwnership) {
        auto* compiler = compilerLLVM;
        if (!compiler->StructImplementsInterface(structName, interfaceName))
        {
            LogErrorContext(errCtx, std::format("'{}' does not implement interface '{}'",
                                                structName, interfaceName));
            return nullptr;
        }
        if (srcNV != nullptr
            && RejectPointerShapedInterfaceUpcast(errCtx, srcNV->TypeAndValue, interfaceName))
            return nullptr;

        // A pointer source IS the data pointer; a value source needs its address, preferring the
        // binding's own storage over a spill.
        llvm::Value* dataPtr = srcValue;
        if (!sourceIsPointer)
            dataPtr = srcNV != nullptr && srcNV->Storage != nullptr
                ? srcNV->Storage : AddressOfClassValueOperand(srcValue, compiler);

        auto* vtable = compiler->GetOrCreateVTable(structName, interfaceName);
        llvm::Value* fat = compiler->BuildInterfaceFatValue(vtable, dataPtr);

        // Ownership transfer: null the owning source and mark it moved so its scope-exit free
        // cannot double-free the object the interface box now owns.
        bool transferred = false;
        if (adoptsOwnership && srcNV != nullptr && srcNV->IsOwning && sourceIsPointer
            && srcNV->Storage != nullptr)
        {
            if (auto* ptrTy = llvm::dyn_cast<llvm::PointerType>(srcNV->BaseType))
            {
                compiler->builder->CreateStore(llvm::ConstantPointerNull::get(ptrTy), srcNV->Storage);
                transferred = true;
                if (!srcNV->CallerName.empty())
                {
                    compiler->MarkVariableMoved(srcNV->CallerName);
                    compiler->MarkVariableMovedIntoInterface(srcNV->CallerName);
                }
            }
        }
        // The binding-erased spelling: same transfer, recovered from the VALUE instead of a name.
        if (adoptsOwnership && !transferred && sourceIsPointer)
            transferred = RetireOwningSourceOfBoxedValue(dataPtr);

        LLVMBackend::InterfaceBoxRecord record;
        record.FatValue = fat;
        record.DataPointer = dataPtr;
        record.SourceClassName = structName;
        record.InterfaceName = interfaceName;
        record.Source = ClassifyInterfaceBoxSource(dataPtr, srcNV, transferred);
        record.OwnershipTransferred = transferred;
        record.SourceKeepsOwner = ClassifyBoxedSourceKeepsOwner(dataPtr, srcNV, transferred);
        if (record.SourceKeepsOwner) record.SourceDisplayName = DescribeBoxedSourceOwner(dataPtr, srcNV);
        compiler->RegisterInterfaceBox(record);
        return fat;
    }

bool MainListener::BindingKeepsOwnershipOfBoxedObject(const LLVMBackend::NamedVariable* nv) const {
        if (nv == nullptr || nv->Storage == nullptr) return false;
        // A `unique` field's synthesized destructor frees it. Field-specific and proven by the
        // field itself, so it is asked before the direct-binding gate below excludes field reads.
        if (nv->IsUniqueFieldAlias || nv->TypeAndValue.IsUnique) return true;
        // Direct bindings only below: a FIELD read's Storage is a GEP and its CallerName names the
        // BASE object, so without this gate a plain field read is blamed on the enclosing parameter.
        if (!llvm::isa<llvm::AllocaInst>(nv->Storage)) return false;
        if (nv->IsOwning) return true;
        // Refreshed by the same '=' that sets PointerRebound, so both outrank it. A '??=' returns
        // before that refresh, so its element fact may be stale and is not a proof on its own -
        // the join proof below carries the case where the '??=' RHS proved an owner too.
        if (nv->BorrowsOwnedElement && !nv->CoalesceRebound) return true;
        if (nv->InheritedKeepsOwner) return true;
        // Recorded where the join's arms were in hand, and refreshed by every later '=', so it
        // outranks the retirement below for the same reason InheritedKeepsOwner does. Every ARM's
        // liveness is re-asked here too, or nulling an arm would leave a stale false rejection.
        if (JoinArmsStillKeepOwner(*nv)) return true;
        // A rebound pointer no longer points at whatever its declaration established an owner for.
        if (nv->PointerRebound) return false;
        // A local that aliases a borrowed parameter. Retired above by any reassignment, which is
        // what makes it safe to consult here - `IS s = b;` must not launder what `delete b;` rejects.
        // Same pair of conditions the raw-delete guard uses, so the two spellings agree exactly.
        if (nv->IsBorrowed && !nv->BorrowedOrigin.empty()) return true;
        // A plain copy of an OWNING local, established at ITS declaration - so it belongs below the
        // retirement with the other declaration-time clauses. The raw `delete b;` guard uses the
        // same pair of conditions, so the boxed and raw spellings reject exactly the same set.
        if (compilerLLVM->OwningLocalCopyStillAliases(*nv)) return true;
        // Identity, never spelling: a local SHADOWING a parameter's name is a different binding.
        return !nv->TypeAndValue.IsMove
            && compilerLLVM->IsFunctionParameterStorage(nv->Storage);
    }

std::string MainListener::DescribeBoxedSourceOwner(llvm::Value* dataPtr,
                                         const LLVMBackend::NamedVariable* srcNV) const {
        const LLVMBackend::NamedVariable* nv = ProvingBindingForBoxedSource(dataPtr, srcNV);
        if (nv == nullptr) return {};
        // A container's element: the CONTAINER frees it, never the local holding the borrow, so
        // an unnameable container falls back to a phrase rather than blaming the local.
        // Gated exactly as in BindingKeepsOwnershipOfBoxedObject: after a '??=' the element fact is
        // not the proof, so naming the container alone would under-name a join the guard fired on.
        if (nv->BorrowsOwnedElement && !nv->CoalesceRebound)
            return nv->OwnedElementContainer.empty()
                ? std::string("its container") : std::format("'{}'", nv->OwnedElementContainer);
        // Carried across a `p = q;` store from the RHS binding's own proof; already rendered there.
        if (nv->InheritedKeepsOwner && !nv->InheritedKeepsOwnerSource.empty())
            return nv->InheritedKeepsOwnerSource;
        if (JoinArmsStillKeepOwner(*nv) && !nv->JoinKeepsOwnerSource.empty())
            return nv->JoinKeepsOwnerSource;
        if (!nv->FieldName.empty())
            return nv->OwningStructName.empty()
                ? std::format("'{}'", nv->FieldName)
                : std::format("'{}.{}'", nv->OwningStructName, nv->FieldName);
        // A bare self-field read (`u` inside the struct's own method) comes from GetMemberVariable,
        // which leaves FieldName and OwningStructName empty; the enclosing method names the owner.
        if (nv->IsUniqueFieldAlias || nv->TypeAndValue.IsUnique)
        {
            std::string field = nv->TypeAndValue.VariableName.empty()
                ? nv->CallerName : nv->TypeAndValue.VariableName;
            std::string owner = compilerLLVM->GetCurrentFunctionName();
            if (auto dot = owner.find('.'); dot != std::string::npos) owner = owner.substr(0, dot);
            else if (!owner.empty() && owner[0] == '~') owner = owner.substr(1);
            else owner.clear();
            if (field.empty()) return "its owner";
            return owner.empty() || !compilerLLVM->IsDataStructure(owner)
                ? std::format("'{}'", field) : std::format("'{}.{}'", owner, field);
        }
        // A borrow: name the ORIGIN, never the local holding it - the caller's parameter (or the
        // `unique` field) is what frees the object, exactly as the raw-delete diagnostic reports it.
        if (!nv->BorrowedUniqueField.empty()) return std::format("'{}'", nv->BorrowedUniqueField);
        if (nv->IsBorrowed && !nv->BorrowedOrigin.empty())
            return std::format("'{}'", nv->BorrowedOrigin);
        // A plain copy of an owning local: name the OWNER, never the copy holding it.
        if (compilerLLVM->OwningLocalCopyStillAliases(*nv))
            return std::format("'{}'", nv->OwningLocalOrigin);
        if (!nv->CallerName.empty()) return std::format("'{}'", nv->CallerName);
        auto* load = llvm::dyn_cast_or_null<llvm::LoadInst>(dataPtr);
        std::string name = load == nullptr
            ? std::string() : compilerLLVM->FindVariableNameByStorage(load->getPointerOperand());
        return name.empty() ? std::string() : std::format("'{}'", name);
    }

std::string MainListener::DescribeAssignedSourceOwner(const LLVMBackend::NamedVariable& rightNV) const {
        if (rightNV.Storage == nullptr || !llvm::isa<llvm::AllocaInst>(rightNV.Storage)) return {};
        const auto* srcNV = compilerLLVM->FindVariableByStorage(rightNV.Storage);
        if (!BindingKeepsOwnershipOfBoxedObject(srcNV)) return {};
        std::string owner = DescribeBoxedSourceOwner(nullptr, srcNV);
        return owner.empty() ? std::string("the binding it was assigned from") : owner;
    }

std::string MainListener::JoinArmsKeepOwner(llvm::Value* joined,
                                  std::vector<llvm::Value*>* slotsOut) const {
        if (slotsOut != nullptr) slotsOut->clear();
        std::vector<llvm::Value*> arms;
        if (auto* phi = llvm::dyn_cast_or_null<llvm::PHINode>(joined))
        {
            if (!phi->getType()->isPointerTy()) return {};
            for (unsigned i = 0; i < phi->getNumIncomingValues(); i++)
                arms.push_back(phi->getIncomingValue(i));
        }
        else if (auto* load = llvm::dyn_cast_or_null<llvm::LoadInst>(joined))
        {
            const auto* join = compilerLLVM->FindNullCoalesceJoin(load);
            if (join == nullptr) return {};
            for (const auto& arm : join->Arms) arms.push_back(arm.Value);
        }
        else return {};

        std::string owner;
        bool proved = false;
        for (auto* arm : arms)
        {
            if (arm == nullptr) return {};
            // The literal and a binding PROVABLY parked at null are the same neutral arm.
            if (JoinArmIsProvablyNull(arm)) continue;
            // A PROVEN `move` arm detached its source, so that binding frees nothing and cannot be
            // the other owner. Keyed on the arm VALUE - the source NamedVariable keeps IsOwning.
            if (compilerLLVM->IsMovedOutPtrValue(arm)) return {};
            auto* load = llvm::dyn_cast<llvm::LoadInst>(arm);
            if (load == nullptr || !load->getType()->isPointerTy()) return {};
            const auto* nv = compilerLLVM->FindVariableByStorage(load->getPointerOperand());
            if (!BindingKeepsOwnershipOfBoxedObject(nv)) return {};
            if (owner.empty()) owner = DescribeBoxedSourceOwner(arm, nullptr);
            if (slotsOut != nullptr) slotsOut->push_back(load->getPointerOperand());
            proved = true;
        }
        if (!proved) return {};
        return owner.empty() ? std::string("the binding each arm was joined from") : owner;
    }

bool MainListener::JoinArmIsProvablyNull(llvm::Value* arm, int depth) const {
        if (arm == nullptr) return false;
        if (llvm::isa<llvm::ConstantPointerNull>(arm)) return true;
        if (depth > 3) return false;
        auto* load = llvm::dyn_cast<llvm::LoadInst>(arm);
        if (load == nullptr || !load->getType()->isPointerTy()) return false;
        auto* slot = llvm::dyn_cast<llvm::AllocaInst>(load->getPointerOperand());
        // An escaping slot can be written through a pointer no store here accounts for, so the
        // store survey below would not be a survey of every reaching definition.
        if (slot == nullptr || !compilerLLVM->AllocaIsLoadStoreOnly(slot)) return false;
        bool sawStore = false;
        for (llvm::User* u : slot->users())
        {
            auto* store = llvm::dyn_cast<llvm::StoreInst>(u);
            if (store == nullptr || store->getPointerOperand() != slot) continue;
            sawStore = true;
            if (!JoinArmIsProvablyNull(store->getValueOperand(), depth + 1)) return false;
        }
        // No store at all is "unknown", not "null" - a parameter slot or an unwritten alloca.
        return sawStore;
    }

bool MainListener::JoinArmsStillKeepOwner(const LLVMBackend::NamedVariable& nv, int depth) const {
        if (!nv.JoinKeepsOwner || nv.JoinKeepsOwnerSlots.empty()) return false;
        if (depth > 4) return false;
        for (auto* slot : nv.JoinKeepsOwnerSlots)
        {
            if (slot == nullptr || slot == nv.Storage) return false;
            const auto* arm = compilerLLVM->FindVariableByStorage(slot);
            if (arm == nullptr || arm->PointerRebound) return false;
            // A join OF joins re-asks the inner arms and stops there: re-entering
            // BindingKeepsOwnershipOfBoxedObject at depth 0 would loop on a cyclic arm graph.
            if (arm->JoinKeepsOwner)
            {
                if (!JoinArmsStillKeepOwner(*arm, depth + 1)) return false;
                continue;
            }
            if (!BindingKeepsOwnershipOfBoxedObject(arm)) return false;
        }
        return true;
    }

const LLVMBackend::NamedVariable* MainListener::ProvingBindingForBoxedSource(
        llvm::Value* dataPtr, const LLVMBackend::NamedVariable* srcNV) const {
        if (BindingKeepsOwnershipOfBoxedObject(srcNV)) return srcNV;
        auto* load = llvm::dyn_cast_or_null<llvm::LoadInst>(dataPtr);
        if (load == nullptr || !load->getType()->isPointerTy()) return nullptr;
        const auto* slotNV = compilerLLVM->FindVariableByStorage(load->getPointerOperand());
        return BindingKeepsOwnershipOfBoxedObject(slotNV) ? slotNV : nullptr;
    }

bool MainListener::ClassifyBoxedSourceKeepsOwner(llvm::Value* dataPtr,
                                       const LLVMBackend::NamedVariable* srcNV,
                                       bool ownershipTransferred) const {
        return !ownershipTransferred && ProvingBindingForBoxedSource(dataPtr, srcNV) != nullptr;
    }

bool MainListener::InterfaceBoxValueIsProvablyBorrowed(llvm::Value* fatValue,
                                             std::vector<std::string>& sourceNames) {
        if (fatValue == nullptr) return false;
        auto* compiler = compilerLLVM;
        if (auto* record = compiler->FindInterfaceBoxByFatValue(fatValue))
        {
            if (!record->SourceKeepsOwner) return false;
            if (!record->SourceDisplayName.empty())
                sourceNames.push_back(record->SourceDisplayName);
            return true;
        }

        auto* phi = llvm::dyn_cast<llvm::PHINode>(fatValue);
        if (phi == nullptr) return false;
        bool sawBox = false;
        for (unsigned i = 0; i < phi->getNumIncomingValues(); i++)
        {
            llvm::Value* arm = phi->getIncomingValue(i);
            if (auto* constant = llvm::dyn_cast<llvm::Constant>(arm);
                constant != nullptr && constant->isNullValue())
                continue;
            const auto* record = compiler->FindInterfaceBoxByFatValue(arm);
            if (record == nullptr) return false;
            // Neutral, exactly like the null-literal arm skipped above: it owns nothing, so it
            // neither proves another owner nor blocks the arms that do.
            if (record->SourceProvablyNull) continue;
            if (!record->SourceKeepsOwner) return false;
            sawBox = true;
            const std::string& name = record->SourceDisplayName;
            if (!name.empty()
                && std::find(sourceNames.begin(), sourceNames.end(), name) == sourceNames.end())
                sourceNames.push_back(name);
        }
        return sawBox;
    }

std::string MainListener::DescribeInterfaceBoxOwners(const std::vector<std::string>& names) const {
        std::string text;
        for (size_t i = 0; i < names.size(); i++)
        {
            if (i > 0) text += (i + 1 == names.size()) ? " or " : ", ";
            text += names[i];   // already quoted (or a phrase) by DescribeBoxedSourceOwner
        }
        return text;
    }

void MainListener::TagInterfaceBoxProvenance(const std::string& varName, llvm::Value* fatValue) {
        if (varName.empty() || fatValue == nullptr) return;
        if (auto* constant = llvm::dyn_cast<llvm::Constant>(fatValue);
            constant != nullptr && constant->isNullValue())
            return;
        std::vector<std::string> sourceNames;
        bool borrowed = InterfaceBoxValueIsProvablyBorrowed(fatValue, sourceNames);
        compilerLLVM->SetInterfaceBoxIsBorrowed(varName, borrowed,
                                                DescribeInterfaceBoxOwners(sourceNames));
    }

bool MainListener::FatValueOwnsHeapBox(llvm::Value* fatValue) {
        if (fatValue == nullptr) return false;
        if (auto* rec = compilerLLVM->FindInterfaceBoxByFatValue(fatValue))
            if (rec->Source == LLVMBackend::InterfaceBoxSource::Heap) return true;
        std::vector<llvm::Value*> dataPtrs;
        std::unordered_set<const llvm::Value*> seen;
        CollectFatValueFields(fatValue, 1u, dataPtrs, seen);
        for (auto* data : dataPtrs)
            if (compilerLLVM->FindInterfaceBoxByDataPointer(
                    data, LLVMBackend::InterfaceBoxSource::Heap) != nullptr)
                return true;
        return false;
    }

bool MainListener::ReturnLeaksOwnershipIntoInterface(llvm::Value* right, LLVMBackend* compiler) {
        if (right == nullptr || compiler->currentFunctionReturnsOwned) return false;
        auto* fatTy = compiler->GetFatPtrType();
        if (fatTy == nullptr || compiler->currentFunction == nullptr
            || compiler->currentFunction->getReturnType() != fatTy)
            return false;
        if (right->getType() == fatTy)
            return FrameLocalDataOfFatValue(right) == nullptr && FatValueOwnsHeapBox(right);
        return right->getType()->isPointerTy() && !llvm::isa<llvm::Constant>(right)
            && (compiler->IsOwningValue(right) || compiler->lastCallReturnsOwned
                || compiler->lastOwningResult);
    }

std::vector<MainListener::InterfaceJoinArm> MainListener::CollectPointerJoinArms(llvm::Value* value) const {
        std::vector<InterfaceJoinArm> arms;
        if (auto* phi = llvm::dyn_cast_or_null<llvm::PHINode>(value))
        {
            if (!phi->getType()->isPointerTy()) return arms;
            for (unsigned i = 0; i < phi->getNumIncomingValues(); i++)
                arms.push_back({ phi->getIncomingValue(i), phi->getIncomingBlock(i) });
            return arms;
        }
        if (auto* load = llvm::dyn_cast_or_null<llvm::LoadInst>(value))
        {
            if (!load->getType()->isPointerTy()) return arms;
            if (const auto* join = compilerLLVM->FindNullCoalesceJoin(load))
                for (const auto& arm : join->Arms) arms.push_back({ arm.Value, arm.Block });
        }
        return arms;
    }

bool MainListener::NestedJoinArmsBoxable(llvm::Value* value, const std::string& interfaceName,
                               std::string* armFailure, int depth) {
        if (depth > kMaxNestedJoinDepth) return false;
        auto arms = CollectPointerJoinArms(value);
        if (arms.empty()) return false;
        for (const auto& arm : arms)
        {
            if (arm.Value == nullptr || arm.Block == nullptr) return false;
            if (arm.Block->getTerminator() == nullptr) return false;
            if (llvm::isa<llvm::ConstantPointerNull>(arm.Value)) continue;
            std::string name = compilerLLVM->ResolvePointerElementTypeName(arm.Value);
            if (name.empty())
            {
                if (!NestedJoinArmsBoxable(arm.Value, interfaceName, armFailure, depth + 1))
                    return false;
                continue;
            }
            if (!compilerLLVM->StructImplementsInterface(name, interfaceName))
            {
                if (armFailure != nullptr)
                    *armFailure = std::format("'{}' does not implement it", name);
                return false;
            }
        }
        return true;
    }

bool MainListener::CollectJoinArmClasses(llvm::Value* value, std::vector<std::string>& out, int depth) {
        if (depth > kMaxNestedJoinDepth) return false;
        auto arms = CollectPointerJoinArms(value);
        if (arms.empty()) return false;
        for (const auto& arm : arms)
        {
            if (arm.Value == nullptr) return false;
            if (llvm::isa<llvm::ConstantPointerNull>(arm.Value)) continue;
            std::string name = compilerLLVM->ResolvePointerElementTypeName(arm.Value);
            if (!name.empty())
            {
                if (!compilerLLVM->IsDataStructure(name)) return false;
                out.push_back(name);
                continue;
            }
            if (!CollectJoinArmClasses(arm.Value, out, depth + 1)) return false;
        }
        return true;
    }

llvm::Value* MainListener::BoxInterfaceJoinArms(const std::vector<InterfaceJoinArm>& arms,
                                      llvm::Value* joinValue, llvm::Instruction* joinPoint,
                                      const std::string& interfaceName, std::string* armFailure,
                                      bool transferArmOwnership, bool* armNotOwned) {
        auto* compiler = compilerLLVM;
        unsigned count = static_cast<unsigned>(arms.size());
        if (count == 0 || joinPoint == nullptr || interfaceName.empty()) return nullptr;
        // Resolve every arm first: a partial rewrite would leave half-boxed IR behind.
        std::vector<std::string> armTypes(count);
        std::vector<bool> armIsNestedJoin(count, false);
        for (unsigned i = 0; i < count; i++)
        {
            llvm::Value* incoming = arms[i].Value;
            if (incoming == nullptr || arms[i].Block == nullptr) return nullptr;
            if (llvm::isa<llvm::ConstantPointerNull>(incoming)) continue;
            armTypes[i] = compiler->ResolvePointerElementTypeName(incoming);
            if (armTypes[i].empty())
            {
                // The arm is itself a JOIN (a chained '??', or either spelling nested in the
                // other), so it has no class of its own - box it recursively in its own block.
                std::string nestedFailure;
                if (NestedJoinArmsBoxable(incoming, interfaceName, &nestedFailure)
                    && arms[i].Block->getTerminator() != nullptr)
                {
                    armIsNestedJoin[i] = true;
                    continue;
                }
                if (armFailure != nullptr)
                    *armFailure = nestedFailure.empty()
                        ? "the arm's concrete class cannot be determined; bind the arm to a "
                          "local variable of the class type first"
                        : nestedFailure;
                return nullptr;
            }
            if (!compiler->StructImplementsInterface(armTypes[i], interfaceName))
            {
                if (armFailure != nullptr)
                    *armFailure = std::format("'{}' does not implement it", armTypes[i]);
                return nullptr;
            }
            if (arms[i].Block->getTerminator() == nullptr) return nullptr;
            /*
             * A 'move' return transfers EVERY arm, so an arm that owns nothing would hand the
             * caller a second owner of a live borrow (double free). The whole-expression check on
             * the return path cannot see this: boxing turns the operand into a struct and that
             * check is gated on a pointer operand. Ask per arm instead - but PROVABLY, since the
             * accept side is the safe one here: only a load off a live binding that declares
             * itself non-owning is rejected. A call result, a phi, a field GEP or an unresolvable
             * slot all answer "cannot tell" and are accepted, which is what keeps this from
             * reopening the false rejection that the whole-expression check used to produce.
             */
            if (transferArmOwnership && !compiler->TernaryArmJoinsOwning(incoming)
                && !compiler->IsMovedOutPtrValue(incoming)
                && compiler->IsProvablyNonOwningPointerLoad(incoming))
            {
                if (armNotOwned != nullptr) *armNotOwned = true;
                return nullptr;
            }
        }

        auto* savedBlock = compiler->builder->GetInsertBlock();
        auto savedPoint = compiler->builder->GetInsertPoint();
        auto* fatTy = compiler->GetFatPtrType();
        std::vector<llvm::Value*> boxed(count, nullptr);
        for (unsigned i = 0; i < count; i++)
        {
            if (armIsNestedJoin[i])
            {
                // Boxed at the NESTED join's own join point, so its fat phi lands in the block
                // that really branches here - arms[i].Block is that block, and it dominates it.
                std::string nestedFailure;
                bool nestedNotOwned = false;
                llvm::Value* nested = UpcastPointerJoinToInterface(
                    arms[i].Value, interfaceName, &nestedFailure, nullptr, transferArmOwnership,
                    &nestedNotOwned);
                if (nested == nullptr)
                {
                    compiler->builder->SetInsertPoint(savedBlock, savedPoint);
                    // Propagate the INNER verdict: a not-owned inner arm must reach the caller as
                    // the ownership diagnostic, not as the useless "bind it to a local" remedy.
                    if (nestedNotOwned)
                    {
                        if (armNotOwned != nullptr) *armNotOwned = true;
                    }
                    else if (armFailure != nullptr)
                        *armFailure = nestedFailure.empty()
                            ? "the arm's concrete class cannot be determined; bind the arm "
                              "to a local variable of the class type first"
                            : nestedFailure;
                    return nullptr;
                }
                boxed[i] = nested;
                continue;
            }
            if (armTypes[i].empty())
            {
                boxed[i] = llvm::Constant::getNullValue(fatTy);
                continue;
            }
            compiler->builder->SetInsertPoint(arms[i].Block->getTerminator());
            auto* vtable = compiler->GetOrCreateVTable(armTypes[i], interfaceName);
            llvm::Value* armData = arms[i].Value;
            boxed[i] = compiler->BuildInterfaceFatValue(vtable, armData);

            // Ownership transfer for an escaping box (see transferArmOwnership). Null the arm's
            // OWNING source in the arm's OWN block, so only the untaken arm is freed at scope exit.
            bool transferred = false;
            if (transferArmOwnership)
                if (auto* load = llvm::dyn_cast<llvm::LoadInst>(armData);
                    load != nullptr && load->getType()->isPointerTy() && compiler->IsOwningValue(load))
                {
                    compiler->builder->CreateStore(
                        llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(load->getType())),
                        load->getPointerOperand());
                    transferred = true;
                }
            // NO MarkVariableMoved here, unlike BoxConcreteIntoInterface: the transfer is
            // RUNTIME-conditional, so a compile-time flag false-rejects the not-taken path.

            // Ledger the box with the same provenance classification the single-value site records,
            // so the guards that ask WHERE a boxed object lives can answer a join too.
            LLVMBackend::InterfaceBoxRecord record;
            record.FatValue = boxed[i];
            record.DataPointer = armData;
            record.SourceClassName = armTypes[i];
            record.InterfaceName = interfaceName;
            record.Source = ClassifyInterfaceBoxSource(armData, nullptr, transferred);
            record.OwnershipTransferred = transferred;
            // A PROVEN `move` arm detached its source here, so no other binding frees it. Same
            // demotion, and the same value keying, JoinArmsKeepOwner applies to the raw spelling.
            bool armMovedOut = compiler->IsMovedOutPtrValue(armData);
            record.SourceKeepsOwner = !armMovedOut
                && ClassifyBoxedSourceKeepsOwner(armData, nullptr, transferred);
            // The arm keeps its real vtable (the IR is unchanged); only the LEDGER calls it neutral.
            record.SourceProvablyNull = JoinArmIsProvablyNull(armData);
            if (record.SourceKeepsOwner) record.SourceDisplayName = DescribeBoxedSourceOwner(armData, nullptr);
            compiler->RegisterInterfaceBox(record);
        }
        compiler->builder->SetInsertPoint(joinPoint);
        auto* fatPhi = compiler->builder->CreatePHI(fatTy, count, "ternary_iface");
        for (unsigned i = 0; i < count; i++)
            fatPhi->addIncoming(boxed[i], arms[i].Block);
        compiler->builder->SetInsertPoint(savedBlock, savedPoint);

        /*
         * Carry the per-arm ownership verdict onto the boxed join. The ledgers track the RAW thin
         * arms, and the receiver now adopts a different value (fatPhi), so the verdict must travel
         * with it or a join with a live BORROW arm is destroyed by its receiver and again by its
         * real owner. The diagnostic entry rides along either way (a suppressed one stays visible
         * to the no-discard check); the thin join's own entries are retired so nothing misfires off
         * a value that only feeds the boxing.
         */
        bool armsAllOwn = true;
        for (unsigned i = 0; i < count; i++)
            if (!compiler->TernaryArmJoinsOwning(arms[i].Value)) armsAllOwn = false;
        compiler->PropagateOwnedReturnTemp(joinValue, fatPhi);
        if (armsAllOwn)
        {
            if (compiler->IsMovedOutPtrValue(joinValue)) compiler->RegisterMovedOutPtrValue(fatPhi);
        }
        else
        {
            compiler->SuppressCallerRelease(fatPhi);
            compiler->SuppressCallerRelease(joinValue);
            compiler->ClearOwnedResultChannels();
        }
        return fatPhi;
    }

llvm::Value* MainListener::UpcastTernaryPhiToInterface(llvm::Value* right, const std::string& interfaceName,
                                             std::string* armFailure,
                                             bool transferArmOwnership,
                                             bool* armNotOwned) {
        auto* phi = llvm::dyn_cast_or_null<llvm::PHINode>(right);
        if (phi == nullptr || interfaceName.empty() || !phi->getType()->isPointerTy()) return nullptr;
        std::vector<InterfaceJoinArm> arms;
        for (unsigned i = 0; i < phi->getNumIncomingValues(); i++)
            arms.push_back({ phi->getIncomingValue(i), phi->getIncomingBlock(i) });
        return BoxInterfaceJoinArms(arms, phi, phi, interfaceName, armFailure, transferArmOwnership,
                                    armNotOwned);
    }

llvm::Value* MainListener::UpcastNullCoalesceToInterface(llvm::Value* right, const std::string& interfaceName,
                                               std::string* armFailure,
                                               bool transferArmOwnership,
                                               bool* armNotOwned) {
        auto* load = llvm::dyn_cast_or_null<llvm::LoadInst>(right);
        if (load == nullptr || interfaceName.empty() || !load->getType()->isPointerTy()) return nullptr;
        const auto* join = compilerLLVM->FindNullCoalesceJoin(load);
        if (join == nullptr) return nullptr;
        std::vector<InterfaceJoinArm> arms;
        for (const auto& arm : join->Arms) arms.push_back({ arm.Value, arm.Block });
        return BoxInterfaceJoinArms(arms, load, load, interfaceName, armFailure, transferArmOwnership,
                                    armNotOwned);
    }

llvm::Value* MainListener::UpcastPointerJoinToInterface(llvm::Value* right, const std::string& interfaceName,
                                              std::string* armFailure,
                                              std::string* joinSpelling,
                                              bool transferArmOwnership,
                                              bool* armNotOwned) {
        if (auto* fat = UpcastTernaryPhiToInterface(right, interfaceName, armFailure,
                                                   transferArmOwnership, armNotOwned))
            return fat;
        if (armNotOwned != nullptr && *armNotOwned)
        {
            if (joinSpelling != nullptr) *joinSpelling = "?:";
            return nullptr;
        }
        if (armFailure != nullptr && !armFailure->empty())
        {
            if (joinSpelling != nullptr) *joinSpelling = "?:";
            return nullptr;
        }
        auto* fat = UpcastNullCoalesceToInterface(right, interfaceName, armFailure,
                                                  transferArmOwnership, armNotOwned);
        if (fat == nullptr && joinSpelling != nullptr
            && ((armNotOwned != nullptr && *armNotOwned)
                || (armFailure != nullptr && !armFailure->empty())))
            *joinSpelling = "??";
        return fat;
    }

llvm::Value* MainListener::BoxPointerJoinArgument(
        const std::vector<const LLVMBackend::TypeAndValue*>& paramsAtPosition,
        llvm::Value* argValue, std::string& ifaceNameOut) {
        auto* compiler = compilerLLVM;
        if (argValue == nullptr || !argValue->getType()->isPointerTy()) return nullptr;

        // Every LEAF arm's concrete class, so "implements" can be asked of all of them at once.
        // Recurses through nested joins, whose result carries no class of its own.
        std::vector<std::string> armClasses;
        if (!CollectJoinArmClasses(argValue, armClasses)) return nullptr;
        if (armClasses.empty()) return nullptr;

        std::string target;
        for (const auto* param : paramsAtPosition)
        {
            if (param == nullptr) continue;
            // ANY pointer parameter can take the raw join and would win the call it is
            // about to be pre-empted out of - void*, char*, int* included. Never box past one.
            if (param->Pointer) return nullptr;
            if (!param->IsInterface || param->TypeName.empty()) continue;
            bool allImplement = true;
            for (const auto& cls : armClasses)
                if (!compiler->StructImplementsInterface(cls, param->TypeName)) allImplement = false;
            if (!allImplement) continue;
            if (!target.empty() && target != param->TypeName) return nullptr;
            target = param->TypeName;
        }
        if (target.empty()) return nullptr;

        std::string armFailure;
        llvm::Value* fat = UpcastPointerJoinToInterface(argValue, target, &armFailure);
        if (fat == nullptr) return nullptr;
        ifaceNameOut = target;
        return fat;
    }

bool MainListener::IsAllNullPhi(llvm::Value* value) const {
        auto* phi = llvm::dyn_cast_or_null<llvm::PHINode>(value);
        if (phi == nullptr || phi->getNumIncomingValues() == 0) return false;
        for (unsigned i = 0; i < phi->getNumIncomingValues(); i++)
        {
            auto* constant = llvm::dyn_cast<llvm::Constant>(phi->getIncomingValue(i));
            if (constant == nullptr || !constant->isNullValue()) return false;
        }
        return true;
    }

bool MainListener::RejectRawPointerToArrayView(antlr4::ParserRuleContext* ctx,
                                     const LLVMBackend::TypeAndValue& target,
                                     const LLVMBackend::TypeAndValue& rhs) {
        if (target.IsArrayView && rhs.Pointer && !rhs.IsArrayView)
        {
            LogErrorContext(ctx, "cannot bind a raw pointer 'T*' to an array-view 'T[]' - a view "
                "must span a whole allocation (it comes only from 'new T[n]' or another 'T[]'); "
                "the 'T[] -> T*' decay is one-way");
            return true;
        }
        return false;
    }

llvm::Value* MainListener::ParseAssignmentExpression(CFlatParser::AssignmentExpressionContext* ctx) {
        auto* compiler = Compiler(ctx);
        auto condCtx = ctx->conditionalExpression();
        auto assignmentOp = ctx->assignmentOperator();
        auto unaryCtx = ctx->unaryExpression();

        if (condCtx != nullptr)
        {
            return ParseConditionalExpression(condCtx);
        }
        else if (assignmentOp != nullptr)
        {
            auto operatorText = ctx->assignmentOperator()->getText();
            auto assignCtx = ctx->assignmentExpression();

            // `_ = expr` is an explicit discard: evaluate the RHS for its side effects, drop the
            // result, and - like a bare-statement temp - destruct an owning-struct rvalue at the
            // end of the full expression. `_` is never an lvalue, so only the plain `=` form is
            // meaningful; a compound op (`_ += x`) would have to read `_` and is rejected.
            if (unaryCtx != nullptr && unaryCtx->getText() == "_")
            {
                if (operatorText != "=")
                    LogErrorContext(unaryCtx,
                        "'_' is a discard target; only '_ = expr' is allowed, not compound assignment.");

                // `_ = move <bare local>;` explicitly releases the owning local NOW: it runs the
                // same per-type teardown as scope exit, then the local is moved-from (a later read
                // is a compile error). `_ = move <bare global>;` gets the same NOW-release, minus
                // the compile-time moved-from tracking (globals have no stackNamedVariable frame to
                // hold it) - the nulled storage still makes a later read see it as moved-out at
                // runtime. Only the top-level bare-identifier move form is special; `_ = move
                // obj.field`, `_ = <call>()`, `_ = expr` keep the generic discard below.
                if (auto* mv = TopLevelMoveExpression(assignCtx))
                {
                    auto* inner = mv->unaryExpression();
                    std::string name = inner ? inner->getText() : std::string();
                    if (IsBareIdentifierText(name))
                    {
                        if (auto* nv = compiler->FindLiveNamedVariable(name))
                        {
                            ReleaseOwningLocalNow(unaryCtx, nv, name);
                            return nullptr;
                        }
                        if (compiler->GetGlobalVariable(name) != nullptr)
                        {
                            ReleaseOwningGlobalNow(unaryCtx, name);
                            return nullptr;
                        }
                    }
                }

                auto rhsNV = ParseAssignmentExpressionNamed(assignCtx);

                // `_ = move <container element slot>;` (e.g. `_ = move _data[i]`): the slot read
                // demoted an owning element to a borrow and there is no destination type to
                // re-derive from, so recover the element's true ownership from ElementOwningUnique
                // (stamped on the buffer field at instantiation, carried onto the element by the
                // subscript). ParseMoveExpression detached the value (Storage null) and already
                // zeroed the slot, so materialize the value into a temp and run the SAME DropValue
                // teardown the `T tmp = move _data[i]` form gets: an owning ptr/iface/value/string
                // element frees exactly once, a bare borrow element frees nothing. This branch fully
                // owns the slot-move discard - it must NOT fall through to the struct-temp helper
                // (which mishandles a detached pointer element as an owning value and over-frees).
                bool slotMove = compiler->lastMovedFromContainerSlot;
                compiler->lastMovedFromContainerSlot = false;
                if (slotMove && rhsNV.Primary != nullptr && rhsNV.BaseType != nullptr)
                {
                    LLVMBackend::NamedVariable slotNV;
                    slotNV.TypeAndValue = rhsNV.TypeAndValue;
                    slotNV.BaseType     = rhsNV.BaseType;
                    ApplyMovedSlotOwnership(slotNV, rhsNV.TypeAndValue);
                    auto* tmp = compiler->CreateAlloca(rhsNV.BaseType);
                    compiler->builder->CreateStore(rhsNV.Primary, tmp);
                    slotNV.Storage = tmp;
                    if (compiler->OwnsDroppableResource(slotNV))
                        compiler->DropValue(slotNV);
                    return rhsNV.Primary;
                }

                // `_ = <call>();` on an owning-POINTER return (`move R*`): the thing being dropped
                // is the returned POINTER, so free THAT (dtor + delete) at end-of-full-expression.
                if (const auto* owned = compiler->FindOwnedReturnEntry(rhsNV.Primary);
                    owned != nullptr && owned->IsOwningPtr)
                {
                    compiler->RegisterOwnedPtrTemp(rhsNV.Primary);
                    return rhsNV.Primary;
                }

                RegisterDiscardedOwningStructTemp(rhsNV);
                return rhsNV.Primary;
            }

            auto namedVar = ParseUnaryExpression(unaryCtx);
            auto destination = namedVar.Storage;

            // Guarded-field WRITE check: the lock must be held exclusively. Runs on the resolved
            // LHS only, so a guarded field READ inside the LHS (e.g. an index 'a[n->count]') is
            // not mistaken for a write.
            CheckGuardedWrite(unaryCtx, namedVar);

            // Storing through a temp spill (`mk().vals[0] = x`) writes into a shallow copy that dies
            // with the full expression - the write is unobservable, so reject it rather than emit a
            // silently discarded store. The READ form is legal (the spill exists for it).
            if (namedVar.IsTempSpillStorage)
            {
                LogErrorContext(unaryCtx,
                    "cannot assign into an inline array field of a temporary value - the slot lives in "
                    "a copy that dies at the end of the expression, so the store is discarded. Bind the "
                    "call result to a local first, then assign into it.");
            }

            /*
             * Assignment to a WHOLE fixed array (`b = ...` where b is `T[N]`, not `b[i] = ...`).
             * Unsupported, and every spelling was already broken: a non-literal RHS reached an
             * invalid `ptr` -> `[N x T]` bitcast and failed module verification, while a string
             * literal folded that cast into a ConstantExpr the verifier does not check - which
             * then miscompiled to garbage, or crashed the compiler in SelectionDAG on an indexed
             * read. Rejected here rather than implemented: whole-array assignment is a feature
             * with its own axes (compound operators, field arrays, globals).
             */
            if (namedVar.TypeAndValue.ConstArraySize > 0
                && !namedVar.TypeAndValue.IsArrayView && !namedVar.TypeAndValue.IsSimd
                && namedVar.BaseType != nullptr && llvm::isa<llvm::ArrayType>(namedVar.BaseType))
            {
                std::string rhsText = assignCtx != nullptr ? assignCtx->getText() : std::string();
                // The LHS as written, not CallerName - on a field receiver ('u.a') CallerName is
                // just the base variable, and the suggested remedy would name the wrong thing.
                std::string lhsName = unaryCtx != nullptr && !unaryCtx->getText().empty()
                    ? unaryCtx->getText() : namedVar.CallerName;
                if (!rhsText.empty() && rhsText.front() == '"')
                    LogErrorContext(unaryCtx, std::format(
                        "cannot assign a string literal to fixed array '{}' - CFlat has no "
                        "C-style character-array assignment. Use 'char* p = ...' to point at the "
                        "literal, 'string s = ...' for a managed string, or copy the bytes into "
                        "'{}' element by element.",
                        DescribeArrayShape(namedVar.TypeAndValue), lhsName));
                else
                    LogErrorContext(unaryCtx, std::format(
                        "assignment to a whole fixed array '{}' is not supported - assign its "
                        "elements ('{}[i] = ...'), or copy at the DECLARATION instead "
                        "('{} dst = <source>;'), which is supported.",
                        DescribeArrayShape(namedVar.TypeAndValue), lhsName,
                        DescribeArrayShape(namedVar.TypeAndValue)));
            }

            // The left side must resolve to an addressable storage location. A null Storage
            // means the LHS produced a value, not an lvalue - storing through it would
            // dereference null below (in isDerefStorage's isa<> checks). Reject it with a
            // diagnostic instead. A simd<T,N> lane subscript (`v[i]`) is the common offender:
            // it lowers to an extractelement value (lanes are read-only), so call that out.
            // Bitfield writes are exempt: they legitimately carry null Storage and route
            // the store through BitfieldStorage in bitfieldAssign below.
            if (destination == nullptr && namedVar.BitfieldStorage == nullptr)
            {
                if (namedVar.Primary && llvm::isa<llvm::ExtractElementInst>(namedVar.Primary))
                    LogErrorContext(unaryCtx, "simd<T,N> lane write 'v[i] = ...' is not supported; lanes are read-only. Build a new vector value instead.");
                else
                    LogErrorContext(unaryCtx, "Left side of assignment is not an addressable lvalue.");
            }

            // For through-pointer dereferences (*p), Storage is a raw loaded ptr (not alloca/gep/global)
            // and BaseType holds the pointee type. All loads/stores through `destination` must use it.
            // UnionFieldType is set for union field access - it overrides the load/store type so that
            // reinterpret semantics work correctly even with LLVM opaque pointers.
            auto isDerefStorage = [&]() {
                return namedVar.BaseType
                    && !llvm::isa<llvm::AllocaInst>(destination)
                    && !llvm::isa<llvm::GlobalVariable>(destination)
                    && !llvm::isa<llvm::GetElementPtrInst>(destination);
            };
            auto derefLoad = [&]() -> llvm::Value* {
                // Bitfield: the extracted value was already computed at field-access time.
                if (namedVar.BitfieldStorage)
                    return namedVar.Primary;
                if (namedVar.UnionFieldType)
                    return compiler->CreateLoad(namedVar.UnionFieldType, destination);
                return isDerefStorage()
                    ? compiler->CreateLoad(namedVar.BaseType, destination)
                    : compiler->CreateLoad(destination);
            };
            // Bitfield read-modify-write: ((word & ~window) | ((val & valMask) << off)).
            // Returns the truncated/sign-extended bitfield value so the assignment
            // expression's result matches the bitfield's declared type.
            auto bitfieldAssign = [&](llvm::Value* val) -> llvm::Value* {
                auto* storageTy = namedVar.BitfieldStorageType;
                unsigned storageBits = (unsigned)storageTy->getIntegerBitWidth();
                unsigned w = namedVar.BitfieldWidth;
                unsigned off = namedVar.BitfieldOffset;
                uint64_t valMask = (w == 64) ? ~uint64_t(0) : ((uint64_t(1) << w) - 1);
                uint64_t windowMask = valMask << off;

                // Cast the incoming value to the storage word's integer width.
                // Third arg of CreateCast is isSigned (used only when widening); flip
                // BitfieldUnsigned to get sign-extension for signed bitfields.
                auto* valAsStorage = compiler->CreateCast(val, storageTy, !namedVar.BitfieldUnsigned);
                auto* valMasked = compiler->builder->CreateAnd(valAsStorage, llvm::ConstantInt::get(storageTy, valMask));
                auto* valShifted = compiler->builder->CreateShl(valMasked, llvm::ConstantInt::get(storageTy, off));
                auto* word = compiler->CreateLoad(storageTy, namedVar.BitfieldStorage);
                auto* cleared = compiler->builder->CreateAnd(word, llvm::ConstantInt::get(storageTy, ~windowMask));
                auto* newWord = compiler->builder->CreateOr(cleared, valShifted);
                compiler->builder->CreateStore(newWord, namedVar.BitfieldStorage);

                // Recompute the extracted bitfield value (post-store) so this expression's
                // result is the value actually stored, with width/sign correctly applied.
                llvm::Value* result;
                if (namedVar.BitfieldUnsigned)
                {
                    result = valMasked;  // already masked to width, zero-extended
                }
                else
                {
                    unsigned leftShift = storageBits - w;
                    auto* shl = compiler->builder->CreateShl(valMasked, llvm::ConstantInt::get(storageTy, leftShift));
                    result = compiler->builder->CreateAShr(shl, llvm::ConstantInt::get(storageTy, leftShift));
                }
                return result;
            };
            auto derefAssign = [&](llvm::Value* val, bool isUnsigned) -> llvm::Value* {
                if (namedVar.BitfieldStorage)
                    return bitfieldAssign(val);

                /*
                 * Storing a non-array value into `[N x T]` storage. The whole-VARIABLE spelling is
                 * rejected above with its own wording; this catches every other receiver, of which
                 * a ROW of a multidimensional array (`b[0] = ...`) is the reachable one - indexing
                 * strips the outer dimension, so the whole-array guard never sees it. A string
                 * literal source here folded the bad cast into a ConstantExpr, which miscompiled
                 * to garbage or crashed the compiler in SelectionDAG on a later indexed read.
                 * The type is the INDEXED storage's (the GEP's), never the union FIELD's - a
                 * subscripted union field targets one element, not the whole field.
                 */
                auto* rowDestTy = isDerefStorage() ? namedVar.BaseType
                                                   : compiler->GetTypeFromStorage(destination);
                if (llvm::isa_and_nonnull<llvm::ArrayType>(rowDestTy)
                    && val != nullptr && val->getType() != rowDestTy)
                {
                    std::string lhsText = unaryCtx != nullptr ? unaryCtx->getText() : std::string("<lhs>");
                    LogErrorContext(unaryCtx, std::format(
                        "cannot assign to '{}' as a whole - it names {}, and CFlat has no "
                        "whole-array assignment. Copy the elements ('{}[i] = ...'), or bind an "
                        "array view ('T[] row = {};') and write through that.",
                        lhsText,
                        LLVMBackend::DescribeAggregateStorageShape(
                            rowDestTy, namedVar.TypeAndValue.TypeName),
                        lhsText, lhsText));
                }

                if (namedVar.UnionFieldType)
                    return compiler->CreateAssignment(val, destination, isUnsigned, namedVar.UnionFieldType);
                return isDerefStorage()
                    ? compiler->CreateAssignment(val, destination, isUnsigned, namedVar.BaseType)
                    : compiler->CreateAssignment(val, destination, isUnsigned);
            };

            /*
             * Null-coalescing assignment: x ??= rhs  ->  if (x == 0/null) x = rhs. Only the null
             * test and the branch are emitted here; the STORE and every piece of bookkeeping that
             * goes with it are the plain-'=' tail below, entered with the builder positioned in the
             * assign arm. That routing is the whole point: this handler used to emit its own store
             * and RETURN, skipping the ownership transfer, the moved-flag revival, the bond and
             * borrow refreshes and the thin-function<> provenance gate the tail runs.
             */
            llvm::BasicBlock* coalesceResume = nullptr;
            std::string coalesceLhsOwner;
            if (operatorText == "??=")
            {
                auto* lhs = derefLoad();
                // The null test IS a guard read of the destination, and derefLoad bypasses
                // LoadNamedVariable, which is where that event is normally logged. Without it the
                // null dataflow cannot see the guard and reports the desugared-equivalent
                // `if (a == nullptr) { a = ...; }` shape as a moved-variable dereference.
                compiler->RecordNullRead(namedVar.CallerName);

                // Is this an ownership-tracked pointer binding? Same gate the plain '=' retirement
                // uses: bare identifier, its own alloca, not a field store.
                bool tracksOwnership = namedVar.TypeAndValue.Pointer && namedVar.FieldName.empty()
                    && !namedVar.CallerName.empty() && namedVar.Storage != nullptr
                    && llvm::isa<llvm::AllocaInst>(namedVar.Storage);
                // The LHS's PRE-STORE proof, taken before anything below can mutate the binding.
                if (tracksOwnership)
                {
                    const auto* lhsNV = compiler->FindVariableByStorage(namedVar.Storage);
                    if (BindingKeepsOwnershipOfBoxedObject(lhsNV))
                        coalesceLhsOwner = DescribeBoxedSourceOwner(nullptr, lhsNV);
                }

                auto* assignBlock = compiler->CreateBasicBlock("nullcoalasgn_assign");
                coalesceResume    = compiler->CreateBasicBlock("nullcoalasgn_resume");
                // Jump to the assign block only when lhs is null/zero.
                compiler->CreateConditionJump(lhs, coalesceResume, assignBlock);
                compiler->SwitchToBlock(assignBlock);
                operatorText = "=";  // the tail IS the store path from here on
            }
            // Closes the '??=' assign arm and yields the destination's value at the join. Identity
            // for a plain '=', which has no arm to close.
            auto finishStore = [&](llvm::Value* v) -> llvm::Value* {
                if (coalesceResume == nullptr) return v;
                compiler->CreateJump(coalesceResume);
                compiler->SwitchToBlock(coalesceResume);
                return derefLoad();
            };

            // Thread expected function-pointer type into lambda RHS (for f = (x) => {...} reassignment)
            if (operatorText == "=" && namedVar.TypeAndValue.IsFunctionPointer)
                lambdaExpectedType = namedVar.TypeAndValue;
            // A generic-substituted closure field carries an ENCODED type name, not IsFunctionPointer;
            // recover its call descriptor so the lambda RHS infers the same return type.
            else if (operatorText == "=" && !namedVar.TypeAndValue.Pointer)
                if (const auto* enc = compiler->GetEncodedClosureType(namedVar.TypeAndValue.TypeName))
                    lambdaExpectedType = *enc;

            // Bond source reassignment check: error if LHS is a live bond source.
            // Only fire when destination is the variable's own alloca/global - field assignments
            // (GEP destinations) mutate the struct in place and do not invalidate any bond.
            // Must run before RHS evaluation so the error fires on the assignment statement.
            if (operatorText == "=" && !namedVar.CallerName.empty()
                && (llvm::isa<llvm::AllocaInst>(destination) || llvm::isa<llvm::GlobalVariable>(destination)))
            {
                auto borrower = compiler->FindActiveBondBorrower(namedVar.CallerName);
                if (!borrower.empty())
                    LogErrorContext(ctx, std::format("cannot reassign '{}' while '{}' holds a bonded reference to it - assign null to '{}' first to break the bond", namedVar.CallerName, borrower, borrower));
            }

            // Inbound alloc-align channel for direct assignment to an align-declared target (a field
            // whose clause was stamped on read, or a local carrying its declared clause). A DIRECT
            // `new` RHS inherits the target's allocation alignment; the store-check below then
            // verifies agreement. Indirect RHS shapes are left to error on mismatch, never inferred.
            uint64_t targetAllocAlign = namedVar.AllocAlignment > 0
                ? namedVar.AllocAlignment
                : namedVar.TypeAndValue.AllocAlignValue;
            if (operatorText == "=" && targetAllocAlign > LLVMBackend::kDefaultNewAlign
                && AsDirectNew(assignCtx) != nullptr)
                compiler->pendingInitAllocAlign = targetAllocAlign;

            auto rightNV = ParseAssignmentExpressionNamed(assignCtx);
            compiler->pendingInitAllocAlign = 0;  // one-shot; consumed by the `new` above if direct
            lambdaExpectedType = {};
            /*
             * Hoisted above the rebind block below, which records it: this is the ONLY discriminator
             * that separates "pointed at a fresh owner" from "pointed at another borrow", and
             * PointerRebound alone cannot tell those apart. Full rationale at the `unique T*`
             * reassignment further down, which reads the same bool.
             */
            bool srcIsOwnedPtrRhs = AsDirectNew(assignCtx) != nullptr
                || TopLevelMoveExpression(assignCtx) != nullptr
                || compiler->IsOwningPtrTempValue(rightNV.Primary)
                || compiler->IsOwnedNewTemp(rightNV.Primary)
                || compiler->IsMovedOutPtrValue(rightNV.Primary);
            // Reassignment / field store into an array-view: `a = rawIntPtr;` or `s.view = p;`
            // would launder a raw pointer into the noalias contract - reject (decay is one-way).
            if (operatorText == "=")
            {
                RejectRawPointerToArrayView(ctx, namedVar.TypeAndValue, rightNV.TypeAndValue);
                // Permanently clear the declaration-time provenance flag on ANY reassignment,
                // regardless of what this RHS is. Recomputing it from THIS RHS would be
                // WALK-ORDER over the AST, not control flow: a reassignment inside one branch
                // must not poison (or clear) a delete reachable only from a different branch -
                // see SetViewOfFixedArrayStorage. Scoped to a plain bare-identifier LHS (field
                // stores are never tracked in the first place).
                if (namedVar.TypeAndValue.IsArrayView && namedVar.FieldName.empty()
                    && !namedVar.CallerName.empty() && namedVar.Storage != nullptr
                    && llvm::isa<llvm::AllocaInst>(namedVar.Storage))
                    compiler->SetViewOfFixedArrayStorage(namedVar.CallerName, false);
                // Retire the declaration-time "someone else frees this" facts about a POINTER
                // binding that has just been pointed elsewhere - UNLESS the RHS binding proves an
                // owner of its own, in which case the proof is carried across. `p = q;` between two
                // borrowed parameters leaves p a borrow; retiring there laundered a double free.
                if (namedVar.TypeAndValue.Pointer && namedVar.FieldName.empty()
                    && !namedVar.CallerName.empty() && namedVar.Storage != nullptr
                    && llvm::isa<llvm::AllocaInst>(namedVar.Storage))
                {
                    /*
                     * '??=' is itself a JOIN: afterwards the binding holds either its OLD referent
                     * (arm not taken) or the RHS's (arm taken). So the fact survives only when BOTH
                     * sides prove an owner, and the diagnostic then names both - mirroring the
                     * '?:' / '??' rule rather than either side alone. Taking the RHS alone
                     * false-rejects `c = new Ci(); c ??= q;`; taking neither laundered `p ??= q`
                     * between two borrowed parameters, which the raw `delete p;` rejects.
                     */
                    std::string assignedOwner = DescribeAssignedSourceOwner(rightNV);
                    // A cast erases the RHS binding's Storage; the boxed VALUE still names the slot,
                    // which is what the pre-tail '??=' handler asked. Coalesce path only.
                    if (coalesceResume != nullptr && assignedOwner.empty()
                        && ProvingBindingForBoxedSource(rightNV.Primary, nullptr) != nullptr)
                        assignedOwner = DescribeBoxedSourceOwner(rightNV.Primary, nullptr);
                    if (coalesceResume != nullptr)
                    {
                        std::string joined;
                        if (!coalesceLhsOwner.empty() && !assignedOwner.empty())
                        {
                            std::vector<std::string> owners{coalesceLhsOwner};
                            if (assignedOwner != coalesceLhsOwner) owners.push_back(assignedOwner);
                            joined = DescribeInterfaceBoxOwners(owners);
                        }
                        // '??=' keeps the OLD referent when the arm is not taken, so no store
                        // through it can prove the binding now holds an owner - never retire.
                        compiler->MarkPointerRebound(namedVar.CallerName, joined,
                                                     /*coalesceJoin*/ true,
                                                     /*reboundToOwnedValue*/ false);
                    }
                    else
                    {
                        /*
                         * srcIsOwnedPtrRhs minus its purely SYNTACTIC leg. `b = move p` off a BORROW
                         * carries the `move` token and owns nothing - ParseMoveExpression ledgers it
                         * in movedBorrowedPtrValues_, not movedOutPtrValues_ - so trusting the token
                         * here retired the borrow proof on a value that transfers nothing. Only the
                         * value-identity legs may prove ownership for a RETIREMENT; narrowed here
                         * rather than in srcIsOwnedPtrRhs, whose other consumer sits behind its own
                         * IsBorrowed gate and must keep the syntactic leg for a genuine transfer.
                         */
                        bool reboundToOwnedValue = srcIsOwnedPtrRhs
                            && !compiler->IsMovedBorrowedPtrValue(rightNV.Primary);
                        compiler->MarkPointerRebound(namedVar.CallerName, assignedOwner,
                                                     /*coalesceJoin*/ false, reboundToOwnedValue);
                    }
                    // A JOIN RHS carries no source binding, so no clause above can see it; ask its
                    // arms directly. After MarkPointerRebound, which retires any earlier join.
                    std::vector<llvm::Value*> storeJoinSlots;
                    std::string storeJoinOwner =
                        JoinArmsKeepOwner(rightNV.Primary, &storeJoinSlots);
                    compiler->SetJoinKeepsOwner(namedVar.CallerName, storeJoinOwner, storeJoinSlots);
                }
            }
            // An assignment to an existing variable (not a declaration) does not transfer
            // ownership - consuming lastOwningResult here would be wrong (the new-allocated
            // value is either moved via items.add(move p) or managed by the caller).
            // Reset the flag so it doesn't leak into the next declaration in this scope.
            // The over-alignment tag rides the same signal and must be reset with it, or a
            // later owning declaration adopts it and aligned-frees an ordinary block.
            // Capture whether the RHS is an OWNED source (direct `new`, a move-returning call,
            // or a `move` expression) before the flags are cleared: the D5 assignment reject for
            // a `unique` interface local (below, after the upcast) needs it to tell a heap-owned
            // source from a stack box / borrow.
            // lastOwningResult is a sticky global that a `new` ANYWHERE in the RHS sets, including
            // as a call argument, so `k = pick(g, new Sq())` adopted a borrow (double-free). Trust
            // it only when the RHS itself is a direct `new` or a top-level `move` - it stays the
            // ownership signal there, since a `move` of a BORROWED interface never sets it.
            // The value-identity leg (IsOwnedNewTemp) sees an owning `new` that reached the RESULT
            // through a transparent wrapper (a '?:' arm); a `new` in ARGUMENT position never does.
            bool srcIsOwnedForUniqueIface =
                (compiler->lastOwningResult
                    && (AsDirectNew(assignCtx) != nullptr || TopLevelMoveExpression(assignCtx) != nullptr))
                || compiler->lastCallReturnsOwned
                || compiler->IsOwnedNewTemp(rightNV.Primary)
                || rightNV.TypeAndValue.IsMove;
            // Same question for a `unique T*` LOCAL reassignment, but answered ONLY from
            // properties of THIS RHS. lastOwningResult is deliberately excluded: it is a sticky
            // global that a `new` anywhere in the RHS sets - including as a call ARGUMENT - so
            // `b = addr(new R())` would inherit it and adopt a pointer the call merely borrowed
            // (freeing a global). The syntactic tests below see the RHS shape itself, and
            // lastCallReturnsOwned is derived from the resolved callee's declared `move`/unique
            // return type, so an ordinary borrow-returning call cannot fake ownership.
            // The IsOwnedNewTemp leg asks the same question BY VALUE IDENTITY instead of by shape,
            // so a `new` that reached the RESULT through a '?:' arm adopts; one in ARGUMENT
            // position produces a different value (the borrow-returning call's result) and does not.
            // The owning-RETURN leg is asked BY VALUE IDENTITY too (IsOwningPtrTempValue), not from
            // the bare lastCallReturnsOwned side-channel: that flag fires for any owning call in the
            // RHS, including an unselected '?:' arm, so a suppressed mixed join adopted a borrow.
            // The IsMovedOutPtrValue leg keeps this in step with the DECLARATION path: a non-mixed
            // '?:' join of `move` arms owns, but is in no other ledger, so `b = c ? move a : nullptr;`
            // left the moved-out object owned by nobody (leak). A MIXED join never reaches the
            // ledger, so it still borrows here.
            compiler->lastOwningResult = false;
            compiler->lastAllocAlignment = 0;
            compiler->lastCallReturnsAllocAlign = 0;
            auto right = LoadNamedVariable(rightNV);

            // A plain assignment moves the RHS value into the named lvalue; if it was an
            // owned-string temporary (e.g. `s = a + b + c;`) the lvalue now holds it, so
            // drop it from the unnamed-temporary cleanup list - freeing it at end of
            // expression would leave the lvalue dangling (UAF / double free). Intermediate
            // concat temporaries stay queued and are still freed. Only for plain '='; a
            // compound '+=' RHS is a true intermediate that must be freed.
            if (operatorText == "=" && right != nullptr)
                compiler->UnregisterOwnedStringTemp(right);

            // Ownership of an owned heap string returned by the RHS (e.g. copy() /
            // operator+) transfers to the assigned-to string local: mark it owning so
            // the scope-exit destructor frees the buffer. Crucially, also CONSUME the
            // lastCallReturnsOwned flag here. Unlike the declaration path (which clears
            // it), a plain assignment used to leave it set, so the next declaration in
            // an adjacent scope (e.g. `string b = "literal";` in the else branch after
            // `name = a.copy();` in the if branch) would falsely inherit ownership and
            // its scope-exit destructor would free a string literal -> heap corruption.
            // Only a plain string LOCAL is tracked for scope-exit destruction. For a string
            // FIELD assignment (`obj.field = expr.copy()`), CallerName names the base struct,
            // not the field - marking it IsOwningString would mis-tag the whole struct (and,
            // for a returned value struct, block the by-value move-out in CreateReturnCall).
            // The field's buffer is owned via the containing struct's destructor instead, so
            // skip the mark for field accesses (FieldName non-empty) - just consume the flag.
            if (operatorText == "=" && compiler->lastCallReturnsOwned)
            {
                if (NamedVarIsString(namedVar) && !namedVar.CallerName.empty()
                    && namedVar.FieldName.empty())
                    compiler->MarkVariableOwningString(namedVar.CallerName);
                compiler->lastCallReturnsOwned = false;
            }

            // Implicit char* -> string coercion on assignment: `s = "lit";` / `s = charPtr;`
            // and the struct-field form `r.name = buf;`. Mirrors the declaration initializer
            // path (ParseDeclaration) so assigning a raw c-string to an EXISTING string runs
            // through operator string(const char*) - which sets _ptr AND _len = strlen(s) -
            // instead of storing the bare pointer with _len left 0. The resulting wrap is
            // non-owning, identical to the initializer form. Fires only when the LHS is a
            // non-pointer string and the RHS loaded to a raw i8*; string-to-string and all
            // other assignments are untouched.
            if (operatorText == "=" && right && NamedVarIsString(namedVar)
                && !namedVar.TypeAndValue.Pointer
                && right->getType() == compiler->builder->getInt8Ty()->getPointerTo())
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

            // Wrap named function in closure fat struct when assigning to a function<T> variable.
            if (operatorText == "=" && namedVar.TypeAndValue.IsFunctionPointer
                && right && !right->getType()->isStructTy())
            {
                // Re-resolve by name to avoid picking a struct method that shares the
                // same plain key in functionTable (e.g. atomic_counter::add vs add).
                // Only when the RHS is a bare named function (right is already an
                // llvm::Function); a same-named local/param of function-pointer type
                // loads to a Value and must shadow the global function, not be replaced.
                std::string funcName = assignCtx->getText();
                int expectedParams = (int)namedVar.TypeAndValue.FuncPtrParams.size();
                if (llvm::isa<llvm::Function>(right))
                    if (auto* correctFn = compiler->GetFunctionForFuncPtr(funcName, expectedParams, &namedVar.TypeAndValue.FuncPtrParams, &namedVar.TypeAndValue))
                        right = correctFn;
                if (auto* fn = llvm::dyn_cast<llvm::Function>(right))
                {
                    VerifyFuncPtrAssignmentMoveFlags(funcName, namedVar.TypeAndValue, ctx);
                    right = namedVar.TypeAndValue.IsThinFnPtr()
                          ? compiler->MakeThinFnPtrValue(fn, namedVar.TypeAndValue)
                          : compiler->WrapBareValueAsFatStruct(fn);
                }
            }
            // Fallback for generic function pointer reassignment (e.g. fn = wrap<int>):
            // the RHS produces a null value with CallerName set to the mangled function name.
            // This mirrors the same fallback in ParseDeclaration for the initial assignment.
            if (!right && operatorText == "=" && namedVar.TypeAndValue.IsFunctionPointer
                && !rightNV.CallerName.empty())
            {
                int expectedParams = (int)namedVar.TypeAndValue.FuncPtrParams.size();
                llvm::Function* genericFn = compiler->GetFunctionForFuncPtr(rightNV.CallerName, expectedParams, &namedVar.TypeAndValue.FuncPtrParams, &namedVar.TypeAndValue);
                if (genericFn)
                {
                    VerifyFuncPtrAssignmentMoveFlags(rightNV.CallerName, namedVar.TypeAndValue, ctx);
                    right = namedVar.TypeAndValue.IsThinFnPtr()
                          ? compiler->MakeThinFnPtrValue(genericFn, namedVar.TypeAndValue)
                          : compiler->WrapBareValueAsFatStruct(genericFn);
                }
            }
            // Fat closure -> thin function<T> on reassignment (mirrors the decl path): allowed only
            // when provably non-capturing (env is a compile-time null); otherwise reject with guidance.
            if (operatorText == "=" && right && namedVar.TypeAndValue.IsFunctionPointer
                && namedVar.TypeAndValue.IsThinFnPtr()
                && right->getType() == compiler->GetClosureFatPtrType())
            {
                compiler->UnregisterOwnedClosureTemp(right);
                if (compiler->ClosureIsStaticallyNonCapturing(right))
                    right = compiler->CoerceClosureFatToThin(right, namedVar.TypeAndValue);
                else
                {
                    LogErrorContext(ctx, compiler->DescribeCapturingClosureToThin(rightNV.LambdaCaptureNames));
                    right = llvm::UndefValue::get(compiler->BuildThinFnPtrType(namedVar.TypeAndValue));
                }
            }
            // A thin function<T> destination fed a raw pointer not handled above (see
            // CheckThinFnPtrAssignProvenance's doc comment for why the FAT twin needs no gate here).
            if (operatorText == "=" && right && namedVar.TypeAndValue.IsFunctionPointer
                && namedVar.TypeAndValue.IsThinFnPtr() && !namedVar.TypeAndValue.Pointer
                && right->getType()->isPointerTy())
            {
                compiler->CheckThinFnPtrAssignProvenance(right, rightNV,
                    namedVar.FieldName.empty()
                        ? (namedVar.CallerName.empty()
                              ? (namedVar.TypeAndValue.VariableName.empty()
                                    ? std::string("the destination")
                                    : std::format("'{}'", namedVar.TypeAndValue.VariableName))
                              : std::format("'{}'", namedVar.CallerName))
                        : std::format("'{}.{}'", namedVar.CallerName, namedVar.FieldName));
            }
            // Widen a thin function<T> value to a fat Lambda<T>: store {code, null}, no thunk
            // (mirrors the decl path). Keeps thin -> Lambda -> toFunction lossless.
            if (operatorText == "=" && right && namedVar.TypeAndValue.IsFunctionPointer
                && !namedVar.TypeAndValue.IsThinFnPtr() && rightNV.TypeAndValue.IsThinFnPtr()
                && !right->getType()->isStructTy())
            {
                right = compiler->WidenThinToFat(right);
            }
            // Same two conversions for a GENERIC-SUBSTITUTED closure field, whose type is an
            // ENCODED name rather than IsFunctionPointer. The destination decides the repr.
            if (operatorText == "=" && right && !namedVar.TypeAndValue.IsFunctionPointer
                && !namedVar.TypeAndValue.Pointer && !namedVar.TypeAndValue.IsArrayView
                && namedVar.TypeAndValue.ConstArraySize == 0)
            {
                if (const auto* enc = compiler->GetEncodedClosureType(namedVar.TypeAndValue.TypeName))
                {
                    if (enc->IsThinFnPtr() && right->getType() == compiler->GetClosureFatPtrType())
                    {
                        compiler->UnregisterOwnedClosureTemp(right);
                        if (compiler->ClosureIsStaticallyNonCapturing(right))
                            right = compiler->CoerceClosureFatToThin(right, *enc);
                        else
                        {
                            LogErrorContext(ctx, compiler->DescribeCapturingClosureToThin(rightNV.LambdaCaptureNames));
                            right = llvm::UndefValue::get(compiler->BuildThinFnPtrType(*enc));
                        }
                    }
                    else if (enc->IsThinFnPtr() && !right->getType()->isStructTy()
                             && right->getType()->isPointerTy())
                        // Same thin gate as the spelled destination above, for the
                        // generic-encoded element (Box<function<...>>.item = vp).
                        compiler->CheckThinFnPtrAssignProvenance(right, rightNV,
                            namedVar.FieldName.empty()
                                ? (namedVar.CallerName.empty() ? std::string("the destination")
                                                                : std::format("'{}'", namedVar.CallerName))
                                : std::format("'{}.{}'", namedVar.CallerName, namedVar.FieldName));
                    else if (!enc->IsThinFnPtr() && !right->getType()->isStructTy()
                             && right->getType()->isPointerTy())
                        right = compiler->WidenToClosureFatChecked(right, rightNV, {},
                            namedVar.FieldName.empty()
                                ? std::format("'{}'", namedVar.CallerName)
                                : std::format("'{}.{}'", namedVar.CallerName, namedVar.FieldName));
                }
            }
            // Funcptr-to-funcptr assignment: per-param IsMove flags must agree on both sides.
            if (operatorText == "=" && namedVar.TypeAndValue.IsFunctionPointer
                && rightNV.TypeAndValue.IsFunctionPointer
                && right && right->getType()->isStructTy())
            {
                const auto& lhsP = namedVar.TypeAndValue.FuncPtrParams;
                const auto& rhsP = rightNV.TypeAndValue.FuncPtrParams;
                if (lhsP.size() == rhsP.size())
                {
                    for (size_t i = 0; i < lhsP.size(); i++)
                    {
                        if (lhsP[i].IsMove != rhsP[i].IsMove)
                        {
                            LogErrorContext(ctx, std::format(
                                "incompatible function pointer assignment: parameter {} differs in 'move' modifier - 'move' is part of the function-pointer type",
                                i + 1));
                            break;
                        }
                    }
                }
            }

            // Bond escape check: bonded value cannot be assigned to a variable in a wider scope
            // than its bond source, and cannot be stored into struct fields (GEP destinations).
            if (operatorText == "=" && (rightNV.IsBonded || compiler->lastCallIsBonded))
            {
                const auto& bondedSources = rightNV.IsBonded ? rightNV.BondedSources : compiler->lastCallBondedSources;
                if (!llvm::isa<llvm::AllocaInst>(destination) && !llvm::isa<llvm::GlobalVariable>(destination))
                {
                    // Struct field or heap dereference - bonded values cannot be stored there.
                    LogErrorContext(ctx, "bonded value cannot be stored in a struct field or through a pointer - bond lifetime would be untrackable");
                }
                else if (!namedVar.CallerName.empty())
                {
                    size_t lhsDepth = compiler->FindVariableScopeDepth(namedVar.CallerName);
                    for (const auto& source : bondedSources)
                    {
                        size_t srcDepth = compiler->FindVariableScopeDepth(source);
                        if (srcDepth != SIZE_MAX && lhsDepth < srcDepth)
                            LogErrorContext(ctx, std::format("bonded value cannot be assigned to '{}' - '{}' is in a wider scope than its bond source '{}'", namedVar.CallerName, namedVar.CallerName, source));
                    }
                }
                compiler->lastCallIsBonded = false;
                compiler->lastCallBondByAddress = false;
                compiler->lastCallBondedSources.clear();
            }

            // Interface upcast: struct* -> fat pointer when assigning to an interface variable.
            // Mirrors the same logic in the declaration initializer path (ParseDeclaration).
            if (operatorText == "=" && namedVar.TypeAndValue.IsInterface
                && right && right->getType() != compiler->GetFatPtrType())
            {
                // nullptr assigned to a non-pointer interface variable - produce null fat pointer {null, null}.
                // IsInterfacePointer (IFoo*) stores a pointer-to-fat-ptr, not a fat-ptr; skip this conversion.
                if (!namedVar.TypeAndValue.IsInterfacePointer && llvm::isa_and_nonnull<llvm::ConstantPointerNull>(right))
                    right = llvm::Constant::getNullValue(compiler->GetFatPtrType());
                std::string structName = rightNV.TypeAndValue.TypeName;
                // A '?:' / '??' join result carries no TypeName: box each arm in its own branch.
                // No ownership transfer: a join into a plain interface local is a BORROW by design.
                if (structName.empty() && !namedVar.TypeAndValue.IsInterfacePointer)
                {
                    std::string ternaryArmFailure;
                    std::string joinSpelling = "?:";
                    auto* ternaryFat = UpcastPointerJoinToInterface(
                        right, namedVar.TypeAndValue.TypeName, &ternaryArmFailure, &joinSpelling);
                    if (ternaryFat != nullptr)
                        right = ternaryFat;
                    else if (!ternaryArmFailure.empty())
                        LogErrorContext(ctx, std::format(
                            "cannot convert '{}' arm to interface '{}': {}",
                            joinSpelling, namedVar.TypeAndValue.TypeName, ternaryArmFailure));
                }
                if (!structName.empty()
                    && compiler->StructImplementsInterface(structName, namedVar.TypeAndValue.TypeName))
                {
                    // A FIELD store keeps its own bookkeeping (TransferPointerOwnershipOnStore
                    // refcounts an escaping `new`), so only a plain slot adopts here.
                    bool destAdopts = llvm::isa_and_nonnull<llvm::AllocaInst>(destination)
                        || llvm::isa_and_nonnull<llvm::GlobalVariable>(destination);
                    right = BoxConcreteIntoInterface(
                        ctx, right, rightNV.TypeAndValue.Pointer, structName,
                        namedVar.TypeAndValue.TypeName, &rightNV, destAdopts);
                }
                // Same provable rejection as the decl-init spelling; a fat-ptr DESTINATION only,
                // since an 'IFoo*' slot stores a pointer and never bitcasts to the fat struct.
                else if (namedVar.TypeAndValue.IsFatInterfaceValue())
                {
                    RejectPrimitiveShapedInterfaceUpcast(
                        ctx, rightNV.TypeAndValue, namedVar.TypeAndValue.TypeName);
                }
            }
            // Derived-interface -> parent-interface assignment: re-box through the typedesc chain.
            // A '?:' RHS carries no NamedVariable TypeName, so fall back to its join ledger.
            else if (operatorText == "=" && namedVar.TypeAndValue.IsFatInterfaceValue()
                     && right && right->getType() == compiler->GetFatPtrType())
            {
                std::string srcIface = compiler->ResolveFatInterfaceSrcName(right,
                    rightNV.TypeAndValue.IsInterface ? rightNV.TypeAndValue.TypeName : std::string());
                right = compiler->ReboxInterfaceIfNeeded(
                    right, srcIface, namedVar.TypeAndValue.TypeName);
            }

            // Assignment leg of the borrowed-box tag. Scoped to a plain bare-identifier LHS with
            // alloca storage - a field store is never tracked. See TagInterfaceBoxProvenance.
            if (operatorText == "=" && namedVar.TypeAndValue.IsFatInterfaceValue()
                && namedVar.FieldName.empty() && !namedVar.CallerName.empty()
                && llvm::isa_and_nonnull<llvm::AllocaInst>(destination))
                TagInterfaceBoxProvenance(namedVar.CallerName, right);

            // D5 (assignment leg): a `unique` interface local owns a heap-boxed object, and its
            // scope-exit teardown frees the fat-ptr data pointer. Reassigning it from a borrowed
            // value or a stack box would either create a second owner (leak / double-free) or point
            // the owning slot at a STACK address that teardown then frees (heap corruption). Only a
            // heap `new`, a `move`, a move-returning call, or nullptr is legal - mirror decl-init
            // (ParseDeclaration). A null value (raw null or the null fat-ptr from the nullptr upcast
            // above) owns nothing and is allowed.
            bool destUniqueIface = namedVar.TypeAndValue.IsUnique
                && namedVar.TypeAndValue.IsFatInterfaceValue();
            bool srcIsNullForUniqueIface = right != nullptr
                && ((llvm::isa<llvm::Constant>(right)
                        && llvm::cast<llvm::Constant>(right)->isNullValue())
                    || IsAllNullPhi(right));
            if (operatorText == "=" && destUniqueIface
                && !srcIsOwnedForUniqueIface && !srcIsNullForUniqueIface)
            {
                LogErrorContext(ctx, std::format(
                    "cannot assign a borrowed value to unique interface '{}' - the source still owns "
                    "it (or is a stack value), so this would leak or free a stack address at scope "
                    "exit; assign 'new', a 'move' expression, a move-returning call, or 'nullptr'",
                    namedVar.CallerName));
                return finishStore(right);
            }

            // A valid owning reassignment to a `unique` interface LOCAL frees the OLD pointee first
            // (its scope-exit teardown only sees the final value), then adopts ownership of the new
            // one. `s = nullptr` frees the old and stores null (release-early). Self-assign cannot
            // occur - the reject above only admits new / move / move-call / null.
            if (operatorText == "=" && destUniqueIface
                && (llvm::isa<llvm::AllocaInst>(destination) || llvm::isa<llvm::GlobalVariable>(destination)))
            {
                auto* oldFat = compiler->builder->CreateLoad(compiler->GetFatPtrType(), destination);
                compiler->DeleteInterfaceValue(oldFat, namedVar.TypeAndValue.TypeName, nullptr);
                if (srcIsOwnedForUniqueIface && !namedVar.CallerName.empty())
                {
                    compiler->SetVariableOwning(namedVar.CallerName, true);
                    compiler->ConsumeOwnedNewTemp(rightNV.Primary);
                }
            }

            // Assignment leg of the code-value store gate - this one site also carries the field,
            // element and global stores, which all reach ParseAssignment with the slot's own type.
            if (right && compiler->CodeValueIntoDataDestination(rightNV, namedVar.TypeAndValue))
            {
                const auto& destTV = namedVar.TypeAndValue;
                // A compound operator asks a different question, and on a non-pointer destination
                // ('string +=' is concatenation) the offset wording would be false.
                LogErrorContext(ctx, operatorText == "="
                    ? compiler->DescribeCodeValueIntoData(
                        CodeValueDestSpelling(destTV), "assign", CodeValueCastAdvice(destTV))
                    : compiler->DescribeCodeValueAsCompoundOperand(
                        CodeValueDestSpelling(destTV), operatorText,
                        destTV.Pointer && !destTV.IsArrayView));
                return finishStore(right);
            }

            // Pointer variable assigned a struct value: catch the mismatch here
            // with a clear message rather than letting LLVM assert inside CreateCast.
            // Interface targets are exempt: an interface slot legitimately holds a fat-ptr
            // struct value (produced by the upcast above), even when Pointer is set.
            if (operatorText == "=" && right && namedVar.TypeAndValue.Pointer && !namedVar.TypeAndValue.IsFunctionPointer
                && !namedVar.TypeAndValue.IsInterface
                && right->getType()->isStructTy())
            {
                LogErrorContext(ctx, std::format(
                    "cannot assign a value of type '{}' to pointer variable '{}' - the right-hand side must be a pointer (call getPtr() or use '&')",
                    rightNV.TypeAndValue.TypeName, namedVar.CallerName));
                return finishStore(right);
            }

            bool rhsUnsigned = rightNV.TypeAndValue.IsUnsignedInteger() != -1;
            if (operatorText != "=")
            {
                auto left = derefLoad();
                bool lhsUnsigned = namedVar.TypeAndValue.IsUnsignedInteger() != -1;

                // Pointer compound arithmetic: use element-typed GEP (C semantics).
                if (namedVar.TypeAndValue.Pointer
                    && (operatorText == "+=" || operatorText == "-=")
                    && right && right->getType()->isIntegerTy())
                {
                    auto elemTV = namedVar.TypeAndValue;
                    elemTV.ElemPointer ? (elemTV.ElemPointer = false) : (elemTV.Pointer = false, elemTV.IsInterfacePointer = false);
                    auto* et = compiler->GetType(elemTV);
                    auto* idx = compiler->Upconvert(right, compiler->builder->getInt64Ty());
                    if (operatorText == "-=")
                        idx = compiler->builder->CreateNeg(idx, "neg");
                    right = compiler->CreateGEP(et, left, idx, "ptrarith");
                }
                else
                {
                    right = compiler->CreateOperation(operatorText, left, right, lhsUnsigned, rhsUnsigned);
                }
            }

            // X4 guard: copying a named owning-value into a struct field aliases its buffer (double-free).
            // Single-index GEP excluded - those are container-internal element stores that must not be flagged.
            auto* destGep = llvm::dyn_cast_or_null<llvm::GetElementPtrInst>(destination);
            bool destIsStructField = (destGep && destGep->getNumIndices() == 2
                && destGep->getSourceElementType()->isStructTy())
                || namedVar.IsInterfaceField;  // reached via the interface's byte-offset slot

            if (operatorText == "=" && destIsStructField)
                RejectFieldAllocAlignMismatch(
                    namedVar.TypeAndValue, namedVar.AllocAlignment, rightNV, right,
                    namedVar.FieldName.empty()
                        ? std::string("a field")
                        : std::format("field '{}'", namedVar.FieldName),
                    ctx);

            // Closure store (Option A): closures are clone-safe, so named-source assignment auto-clones.
            // Old dest env freed for alloca/global only - skipped for struct-field GEP (slot may be uninitialized).
            // Keyed on the REPRESENTATION, not the spelling: a generic-substituted fat field
            // carries an ENCODED name but is the same `__closure_fat_ptr` struct and owns an env.
            bool destIsClosureFat = namedVar.TypeAndValue.TypeName == "__closure_fat_ptr"
                || (!namedVar.TypeAndValue.Pointer
                    && compiler->IsFatEncodedClosureType(namedVar.TypeAndValue.TypeName));
            if (operatorText == "=" && right && right->getType()->isStructTy() && destIsClosureFat)
            {
                right = CloneClosureFromNamedSource(rightNV, right, ctx);
                // Free the old env when overwriting a KNOWN-INITIALIZED destination slot, so the
                // prior env is not orphaned. Safe slots: a whole-variable closure local/global
                // (alloca/global), and a struct FIELD of a stack/global value-type instance - its
                // fields are default/zero-initialized, so an unset field reads as null and the env
                // dtor (null/tag-guarded) is a no-op. A field reached through a HEAP pointer (e.g. a
                // raw-malloc'd thread/process packet) may be uninitialized garbage, so it is NOT
                // freed here - freeing a garbage env would corrupt the heap.
                // The clone above reads the SOURCE env first, so a self-assign (`f = f`) is safe:
                // the independent clone exists before the old env is freed.
                bool destSlotInitialized =
                    llvm::isa<llvm::AllocaInst>(destination) || llvm::isa<llvm::GlobalVariable>(destination);
                if (!destSlotInitialized && destIsStructField)
                {
                    auto* base = destGep->getPointerOperand()->stripPointerCasts();
                    destSlotInitialized = llvm::isa<llvm::AllocaInst>(base) || llvm::isa<llvm::GlobalVariable>(base);
                }
                // A UNION member is NOT a known-initialized closure slot: every arm names the same
                // bytes, so the current contents may be an unrelated arm's value. Read as a closure
                // an odd integer arm looks like an OWNED (tagged) env and the dtor makes a wild
                // indirect call through it. Suppress the free here; the arm's old env leaks.
                if (namedVar.UnionFieldType != nullptr)
                    destSlotInitialized = false;
                if (destSlotInitialized)
                    if (auto* dtor = compiler->GetOrCreateFullDestructor("__closure_fat_ptr"))
                        compiler->builder->CreateCall(dtor->getFunctionType(), dtor, { destination });
                derefAssign(right, rhsUnsigned);
                // The destination now owns this closure env, so drop the stored value from the
                // owned-closure temp-flush list to avoid a double-free (no-op for a clone result).
                compiler->UnregisterOwnedClosureTemp(right);
                return finishStore(right);
            }

            // Field-identity of the two sides, computed up front so the field-store rejects/copies
            // below can all recognize a self-assign. Ownership is a runtime property, so a
            // field-to-field of a NON-copyable owner cannot be safely duplicated (see the rejects).
            auto* srcFieldGep = llvm::dyn_cast_or_null<llvm::GetElementPtrInst>(rightNV.Storage);
            bool srcIsStructField = (srcFieldGep && srcFieldGep->getNumIndices() == 2
                && srcFieldGep->getSourceElementType()->isStructTy())
                || rightNV.IsInterfaceField;
            // Names cannot tell two receivers apart (an element's CallerName is the CONTAINER), so
            // only this proof can. DIAGNOSTICS may use it as-is; anything that EMITS a copy or a
            // destruct must use the -Initialized form, which also requires both slots to root in
            // stack/global storage. A slot reached through a heap pointer may hold garbage, and
            // deep-copying or destructing that is a SIGSEGV, so those paths keep the old no-op.
            bool provablyDifferentSlots = ProvablyDifferentSlots(destination, rightNV.Storage);
            bool provablyDifferentInitializedSlots = provablyDifferentSlots
                && AddressRootIsStackOrGlobal(destination)
                && AddressRootIsStackOrGlobal(rightNV.Storage);
            bool selfFieldAssign = !namedVar.FieldName.empty()
                && namedVar.FieldName == rightNV.FieldName
                && namedVar.CallerName == rightNV.CallerName
                && !provablyDifferentSlots;
            // The store-side twin of selfFieldAssign, for the copy/destruct paths below.
            bool sameFieldStore = !namedVar.FieldName.empty()
                && namedVar.FieldName == rightNV.FieldName
                && namedVar.CallerName == rightNV.CallerName
                && !provablyDifferentInitializedSlots;

            // Self-assign of a `unique` field is one owner, not two, so neither reject below may
            // fire on it. selfFieldAssign misses the bare `item = item` inside a method: that
            // NamedVariable comes from GetMemberVariable, which deliberately leaves FieldName
            // empty. Both sides then name a field of the same `this`, so the declared field name
            // identifies the slot (each access re-loads `this`, so the GEPs never compare equal).
            bool selfUniqueFieldAssign = selfFieldAssign
                || (namedVar.FieldName.empty() && rightNV.FieldName.empty()
                    && !namedVar.TypeAndValue.VariableName.empty()
                    && namedVar.TypeAndValue.VariableName == rightNV.TypeAndValue.VariableName);
            // Store-side twin, for the one helper below that COPIES as well as rejects.
            bool selfUniqueFieldStore = selfUniqueFieldAssign || sameFieldStore;

            // A suppressed (mixed) '?:' join of an owning-value struct: neither an existing owning
            // local nor a field can carry the suppression, so reject before any store path runs.
            if (operatorText == "="
                && RejectNonOwningStructJoinStore(right, namedVar.TypeAndValue.TypeName,
                                                  destIsStructField ? "a struct field"
                                                                    : "an owning variable", ctx))
                return finishStore(right);

            // Storing a whole `alias` (borrow) value into a struct field - rejected ahead of the
            // generic owning-value reject below so the borrow keeps its precise message.
            if (operatorText == "=" && destIsStructField
                && RejectAliasStoreIntoField(rightNV, right, ctx))
                return finishStore(right);

            // Same hazard with an owning LOCAL/GLOBAL destination (`other = k`): it destructs, so
            // it must sit ahead of the move/destruct blocks below, which only ask for a named slot.
            if (operatorText == "=" && !destIsStructField && namedVar.FieldName.empty()
                && destination != nullptr
                && (llvm::isa<llvm::AllocaInst>(destination)
                    || llvm::isa<llvm::GlobalVariable>(destination))
                && compiler->IsOwningValueType(namedVar.TypeAndValue.TypeName)
                && !namedVar.TypeAndValue.Pointer
                && destination != rightNV.Storage
                && RejectAliasBorrowAdoption(rightNV, right, "an owning variable", ctx))
                return finishStore(right);

            // Copying a named owning value into a struct field by value. THE FLIP copies a copyable
            // owner in place (proceeds); a non-copyable owner is rejected. A self-assign is a no-op
            // (no copy - copying would leak the old buffer the self-store never frees). `rejectCopied`
            // records whether it produced the copy, so the field-to-field block below does not copy again.
            bool rejectCopied = false;
            if (operatorText == "=" && destIsStructField
                && RejectOwningValueCopyIntoField(rightNV, right, selfUniqueFieldStore, rejectCopied, ctx))
                return finishStore(right);

            // A PROVABLE stack address into a scalar `unique` field: the shape gate keeps the
            // pointer-worded message off whole-array and interface fields, where it would be untrue.
            if (operatorText == "=" && right && destIsStructField
                && (namedVar.TypeAndValue.IsUnique || namedVar.TypeAndValue.IsUniqueTypeArg)
                && namedVar.TypeAndValue.Pointer
                && namedVar.TypeAndValue.ConstArraySize == 0
                && !namedVar.TypeAndValue.IsInterface
                && !selfUniqueFieldAssign
                && LLVMBackend::IsProvableNonHeapAddress(right))
            {
                RejectNonHeapAddressIntoUnique(
                    std::format("unique field '{}'", DescribeUniqueFieldOwner(namedVar)), ctx);
                return finishStore(right);
            }

            // Storing a BORROW into a `unique` field. The field declares that it owns the pointee and
            // its synthesized destructor deletes it, but the borrow's real owner frees it as well.
            // This is Trap A from internal/plan/ownership-move-alias-discipline.md, diagnosable here
            // only because `unique` makes the ownership claim explicit. A `move` RHS, a `new` result,
            // `nullptr`, and ownership-transferring call results are not borrows and never reach here;
            // `h->slot = h->slot` is a self-assign, not a second owner, so it is excluded too.
            if (operatorText == "=" && right && destIsStructField
                && IsOwningUniquePointerField(namedVar.TypeAndValue)
                && rightNV.IsBorrowed
                && !rightNV.TypeAndValue.IsMove
                && !selfUniqueFieldAssign)
            {
                RejectBorrowIntoUniqueField(rightNV,
                    std::format("unique field '{}'", DescribeUniqueFieldOwner(namedVar)), ctx);
                return finishStore(right);
            }

            // Storing a plain COPY of a live OWNING local into a `unique` field. The copy carries no
            // IsBorrowed, so the clause above cannot see it, and the field's synthesized destructor
            // then frees a pointee the source frees again at scope exit. The DIRECT spelling
            // (`h.f = c;`) transfers ownership out of `c`, so it is both legal and the remedy.
            // Re-asked for liveness, so rebinding either end retires it (that copy is then the
            // sole owner and rejecting the store would LEAK).
            if (operatorText == "=" && right && destIsStructField
                && IsOwningUniquePointerField(namedVar.TypeAndValue)
                && !rightNV.TypeAndValue.IsMove
                && !selfUniqueFieldAssign
                && llvm::isa_and_nonnull<llvm::AllocaInst>(rightNV.Storage))
            {
                const auto* copyBind = compiler->FindVariableByStorage(rightNV.Storage);
                std::string srcName = rightNV.CallerName.empty()
                    ? rightNV.TypeAndValue.VariableName : rightNV.CallerName;
                if (srcName.empty()) srcName = "<expr>";
                if (copyBind != nullptr && compiler->OwningLocalCopyStillAliases(*copyBind))
                {
                    LogErrorContext(ctx, std::format(
                        "cannot store '{}' into unique field '{}' - '{}' copies '{}', which still owns "
                        "the object and frees it at scope exit, so the field's synthesized destructor "
                        "double-frees it. Store '{}' directly (that transfers ownership out of it), or "
                        "use 'move {}'.",
                        srcName, DescribeUniqueFieldOwner(namedVar), srcName,
                        copyBind->OwningLocalOrigin, copyBind->OwningLocalOrigin,
                        copyBind->OwningLocalOrigin));
                    return finishStore(right);
                }
                // The same store off a local bound from a proving '?:' / '??' JOIN. Its own delete
                // is rejected above; the field's synthesized destructor is the same second free.
                if (copyBind != nullptr && !copyBind->IsOwning
                    && JoinArmsStillKeepOwner(*copyBind)
                    && !copyBind->JoinKeepsOwnerSource.empty())
                {
                    LogErrorContext(ctx, std::format(
                        "cannot store '{}' into unique field '{}' - every arm of the join '{}' was "
                        "bound from holds an object {} already frees, so the field's synthesized "
                        "destructor double-frees it. Store an object this frame owns instead.",
                        srcName, DescribeUniqueFieldOwner(namedVar), srcName,
                        copyBind->JoinKeepsOwnerSource));
                    return finishStore(right);
                }
            }

            // Same store through a '?:' join that laundered a borrowed `move`: the joined value
            // carries no IsBorrowed, yet the field adopted a live pointee and double-freed (abort).
            std::string fieldBorrowMoveOrigin;
            if (operatorText == "=" && right && destIsStructField
                && IsOwningUniquePointerField(namedVar.TypeAndValue)
                && !rightNV.TypeAndValue.IsMove
                && !selfUniqueFieldAssign
                && compiler->IsMovedBorrowedPtrValue(rightNV.Primary, &fieldBorrowMoveOrigin))
            {
                LLVMBackend::NamedVariable borrowSrc = {};
                borrowSrc.CallerName     = fieldBorrowMoveOrigin;
                borrowSrc.BorrowedOrigin = fieldBorrowMoveOrigin;
                RejectBorrowIntoUniqueField(borrowSrc,
                    std::format("unique field '{}'", DescribeUniqueFieldOwner(namedVar)), ctx);
                return finishStore(right);
            }

            // Direct `unique`-field-to-`unique`-field copy (`c.p = a.p`) - two owners of one
            // pointee. Checked after Trap A so a source reached through a borrowed parameter keeps
            // the more precise borrow message. `move a.p` clears Storage, so it is not a field read
            // and stays legal (it nulls the source field); self-assign is excluded above.
            // A fat-interface slot fails both pointer-shaped tests, so it gets its own
            // destination leg (IsUniqueTempFieldRead / IsOwningUniqueInterfaceField).
            bool destOwnsUniquePointee = right && right->getType()->isPointerTy()
                && IsOwningUniquePointerField(namedVar.TypeAndValue);
            bool destOwnsUniqueInterface = right && right->getType()->isStructTy()
                && IsOwningUniqueInterfaceField(namedVar.TypeAndValue);
            if (operatorText == "=" && destIsStructField
                && (destOwnsUniquePointee || destOwnsUniqueInterface)
                && (IsUniqueFieldRead(rightNV) || IsUniqueTempFieldRead(rightNV))
                && !selfUniqueFieldAssign)
            {
                std::string destDesc = std::format("unique field '{}'", DescribeUniqueFieldOwner(namedVar));
                // Temp source first: it is the more specific shape, and only its message names a
                // remedy that exists (`move <temp>.<field>` is rejected as unaddressable).
                std::string srcText = assignCtx != nullptr ? assignCtx->getText() : std::string();
                if (IsUniqueTempFieldRead(rightNV))
                    RejectUniqueTempFieldToField(rightNV, destDesc, ctx);
                else if (destOwnsUniqueInterface)
                    RejectUniqueInterfaceFieldToField(rightNV, destDesc, ctx);
                else
                    RejectUniqueFieldToUniqueField(rightNV, destDesc, ctx, srcText);
                return finishStore(right);
            }

            // Same store between two INTERFACE receivers: neither side carries a caller name, so
            // the reject above reads them as a self-assign. Record it and settle at end of body.
            if (operatorText == "=" && destIsStructField
                && namedVar.IsInterfaceField && rightNV.IsInterfaceField
                && (destOwnsUniquePointee || destOwnsUniqueInterface)
                && (IsUniqueFieldRead(rightNV) || IsUniqueTempFieldRead(rightNV))
                && !rightNV.TypeAndValue.IsMove
                && selfUniqueFieldAssign)
                RecordInterfaceFieldToFieldStore(
                    namedVar, rightNV, destination, destOwnsUniqueInterface,
                    assignCtx != nullptr ? assignCtx->getText() : std::string(), ctx);

            // A plain (non-move) by-value `string` PARAMETER stored into a struct field: deep-copy it.
            // The parameter is a by-value copy of the caller's argument, but the CALLER still owns and
            // frees the underlying buffer (e.g. an owning concat temp passed as the argument is freed
            // at the caller's end-of-statement cleanup). A shallow store would alias that buffer into
            // the field, leaving it dangling - and silently corrupted once the freed block is reused -
            // after the caller frees the temp. A true move is impossible from here (the callee cannot
            // reach the caller's temp cleanup), so an independent owned copy is the only safe transfer;
            // it is exactly what the owning-string-into-field reject below recommends ('.copy()').
            // `move` params (handled by the move-string transfer below) and owned temps / call results
            // (null Storage, already transferred to us) keep their existing zero-copy paths.
            if (operatorText == "=" && right && right->getType()->isStructTy()
                && destIsStructField
                && NamedVarIsString(namedVar)
                && rightNV.TypeAndValue.TypeName == "string"
                && !rightNV.TypeAndValue.IsMove
                && rightNV.FieldName.empty()
                && rightNV.Storage != nullptr
                && !rightNV.CallerName.empty()
                && !sameFieldStore)
            {
                auto argNV = compiler->GetFunctionArgument(rightNV.CallerName);
                bool srcIsByValueStringParam =
                    argNV.Storage != nullptr
                    && argNV.Storage == rightNV.Storage
                    && argNV.TypeAndValue.TypeName == "string"
                    && !argNV.TypeAndValue.IsMove
                    && !argNV.IsOwningString;
                if (srcIsByValueStringParam)
                    right = compiler->EmitOwnedStringDeepCopy(right);
            }

            // Implied move of a `move`-temp's owning field (`dest = makeToken().text`): the temp OWNS it,
            // so free dest's old value, store the field, and zero the source so the temp dtor skips it.
            // POINTER fields are excluded: this branch was written for owning VALUE types, and on a
            // `unique Item*` field it destructs the destination SLOT's address (crashes codegen).
            if (operatorText == "=" && right && rightNV.MovableTempField
                && !rightNV.TypeAndValue.Pointer
                && compiler->IsOwningValueType(rightNV.TypeAndValue.TypeName))
            {
                if (auto* dtor = compiler->GetOrCreateFullDestructor(namedVar.TypeAndValue.TypeName))
                    compiler->builder->CreateCall(dtor->getFunctionType(), dtor, { destination });
                compiler->builder->CreateStore(right, destination);
                auto* srcGep = compiler->builder->CreateStructGEP(
                    rightNV.MoveTempStructType, rightNV.MoveTempStructAlloca, rightNV.MoveTempFieldIndex, "movedfld");
                compiler->builder->CreateStore(
                    llvm::ConstantAggregateZero::get(right->getType()), srcGep);
                return finishStore(right);
            }

            // Storing an owning field of a NON-move (alias) temp (`x = aliasTok().text`) into a longer-
            // lived slot REJECTED: the buffer is owned elsewhere, so a 2nd owner double-frees. (Move-temps
            // took the implied-move branch above.)
            if (operatorText == "=" && rightNV.FromOwningTempField
                && compiler->IsOwningValueType(rightNV.TypeAndValue.TypeName))
            {
                LogErrorContext(ctx, std::format(
                    "cannot store '{}.{}' taken from a temporary into a longer-lived location; its buffer "
                    "is owned elsewhere and would double-free. Use '.copy()' for an independent copy, or "
                    "bind the whole call result to a local first and assign from that.",
                    rightNV.OwningStructName, rightNV.FieldName));
                return finishStore(right);
            }

            // Same escape into a BORROWING destination (field, local, global, array element) with
            // a dtor-LESS pointee. After the gate above, so a dtor-bearing pointee keeps its wording.
            if (operatorText == "=" && IsOwningTempUniqueFieldEscape(rightNV))
            {
                RejectOwningTempUniqueFieldEscape(rightNV, "a longer-lived location", ctx);
                return finishStore(right);
            }

            // Assigning a named string LOCAL into a string field. THE FLIP: a NAMED OWNED source
            // (IsOwningString, not a field-borrow) COPIES into the field (independent buffer),
            // leaving the source live; the old field value is destructed below before the copy is
            // stored. A pure field-BORROW source (BorrowsOwnedString, not owning) stays REJECTED -
            // copying it would silently turn a deliberate borrow into an owned duplicate (e.g.
            // xml.cb's `b._rootTag = rt`).
            if (operatorText == "=" && right && right->getType()->isStructTy()
                && destIsStructField
                && NamedVarIsString(namedVar)
                && rightNV.TypeAndValue.TypeName == "string"
                && (rightNV.IsOwningString || rightNV.BorrowsOwnedString)
                && rightNV.Storage != nullptr
                && !rightNV.TypeAndValue.IsMove
                && !sameFieldStore)
            {
                if (rightNV.IsOwningString && !rightNV.BorrowsOwnedString)
                    right = compiler->EmitOwnedStringDeepCopy(right);
                else
                {
                    LogErrorContext(ctx, std::format(
                        "assigning a non-owning string '{}' into a struct field; use 'move' to transfer "
                        "ownership or '.copy()' for an independent copy", rightNV.CallerName));
                    return finishStore(right);
                }
            }

            // Field-to-field copy of an owning value. THE FLIP: a copyable owner COPIES field-to-field
            // (independent duplicate), leaving the source field live; the old destination field is
            // destructed below before the copy is stored. A NON-copyable owner (owns a `unique`, no
            // copy()) stays REJECTED - it has no safe generic duplicate.
            if (operatorText == "=" && right && right->getType()->isStructTy()
                && destIsStructField && srcIsStructField
                && !sameFieldStore
                && !rightNV.TypeAndValue.IsMove
                && rightNV.TypeAndValue.TypeName == namedVar.TypeAndValue.TypeName
                && compiler->IsOwningValueType(namedVar.TypeAndValue.TypeName))
            {
                if (compiler->IsCopyableType(namedVar.TypeAndValue.TypeName))
                {
                    // RejectOwningValueCopyIntoField above already copied this copyable owner
                    // (rejectCopied) - copying again would orphan (leak) the first copy. Only copy
                    // here when it bailed (e.g. a source with no CallerName it could not classify).
                    if (!rejectCopied)
                        right = EmitCopyableOwnerCopy(rightNV, right, ctx);
                }
                else
                {
                    LogErrorContext(ctx, std::format(
                        "field-to-field copy of owning value '{}' aliases its backing buffer and will "
                        "double-free at teardown; use '.copy()' for an independent copy or 'move' to "
                        "transfer ownership", namedVar.TypeAndValue.TypeName));
                    return finishStore(right);
                }
            }

            // Owning-value MOVE at reassignment: a shallow store aliases owned buffers (double-free).
            // Destruct the old destination first, then move source bits into it and zero the source.
            // Interface targets are excluded: the upcast above replaced `right` with a fat-ptr that
            // ALIASES the source's storage (dataPtr = &source), so zeroing the source would corrupt
            // the object the interface now points at (it reads back default/zero fields). An owning-
            // value bound to an interface is a borrow of the source storage - the source keeps
            // ownership and frees at scope exit (heap-`new` is the way to hand an interface an
            // independently-owned object). This mirrors the pointer source case, which is likewise
            // excluded by `!rightNV.TypeAndValue.Pointer`.
            if (operatorText == "=" && right && right->getType()->isStructTy()
                && !namedVar.TypeAndValue.IsInterface
                && (llvm::isa<llvm::AllocaInst>(destination) || llvm::isa<llvm::GlobalVariable>(destination))
                && rightNV.Storage != nullptr
                && (llvm::isa<llvm::AllocaInst>(rightNV.Storage) || llvm::isa<llvm::GlobalVariable>(rightNV.Storage))
                && !rightNV.CallerName.empty()
                // A `move` PARAMETER is an owner (IsOwningStruct) and destructs at function exit,
                // so assigning it into an owning slot must TRANSFER just like an owned local.
                && (!rightNV.TypeAndValue.IsMove || rightNV.IsOwningStruct)
                && !rightNV.TypeAndValue.Pointer
                && !rightNV.TypeAndValue.IsArrayView
                && !rightNV.TypeAndValue.IsInterfacePointer
                && !rightNV.TypeAndValue.IsFunctionPointer
                && destination != rightNV.Storage
                && compiler->IsOwningValueType(rightNV.TypeAndValue.TypeName))
            {
                // THE FLIP via the shared decision: a copyable owner COPIES (source stays live), a
                // non-copyable owner MOVES. The copy/right value is produced BEFORE the old
                // destination is destructed (self-assign is guarded by destination != rightNV.Storage
                // above). On a Move, zero the moved-out source so its always-run scope-exit destructor
                // is a no-op (cflat value-type move = zero the source) and mark it moved.
                AssignSourceKind kind;
                llvm::Value* toStore = ClassifyOwningAssignSource(
                    right, rightNV.TypeAndValue.TypeName, rightNV.TypeAndValue.IsMove, ctx, kind);
                if (auto* dtor = compiler->GetOrCreateFullDestructor(namedVar.TypeAndValue.TypeName))
                    compiler->builder->CreateCall(dtor->getFunctionType(), dtor, { destination });
                compiler->builder->CreateStore(toStore, destination);
                if (kind == AssignSourceKind::Move)
                {
                    compiler->builder->CreateStore(
                        llvm::ConstantAggregateZero::get(toStore->getType()), rightNV.Storage);
                    compiler->MarkVariableMoved(rightNV.CallerName);
                }
                // The destination is now live again (it may have been moved-from earlier).
                if (!namedVar.CallerName.empty() && namedVar.FieldName.empty())
                    compiler->MarkVariableUnmoved(namedVar.CallerName);
                return finishStore(toStore);
            }

            // Owning-value MOVE through a pointer-deref destination (`*pc = *pa`): a shallow store
            // would alias owned buffers (double-free) and orphan the destination's old value (leak).
            // A deref lvalue is neither an alloca/global (destIsLocalOwningVar) nor a 2-index
            // struct-field GEP (destIsStructField), so both paths above skip it. Container element
            // stores are single-index GEPs, which isDerefStorage() excludes (it rejects every GEP),
            // so they never reach here. Destruct the old destination, move the source bits in, then
            // zero the source so its scope-exit dtor is a no-op. Use-after-move through a pointer is
            // left unenforced - a deref source (`*pa`) has no name to MarkVariableMoved.
            if (operatorText == "=" && right && destination
                && !namedVar.BitfieldStorage && !namedVar.UnionFieldType
                && right->getType()->isStructTy()
                && isDerefStorage()
                && !namedVar.TypeAndValue.IsInterface
                && !namedVar.TypeAndValue.Pointer
                && !namedVar.TypeAndValue.IsInterfacePointer
                && !namedVar.TypeAndValue.IsFunctionPointer
                && !namedVar.TypeAndValue.IsArrayView
                && !rightNV.TypeAndValue.IsMove
                && destination != rightNV.Storage
                && compiler->IsOwningValueType(namedVar.TypeAndValue.TypeName))
            {
                // THE FLIP via the shared decision: a copyable owner COPIES into the deref lvalue
                // (source stays live - no zero, no mark-moved); a non-copyable owner MOVES. Produce
                // the value before destructing the old destination. The guard excludes an explicit
                // `move` source, so pass srcIsMove=false.
                AssignSourceKind kind;
                llvm::Value* toStore = ClassifyOwningAssignSource(
                    right, namedVar.TypeAndValue.TypeName, false, ctx, kind);
                if (auto* dtor = compiler->GetOrCreateFullDestructor(namedVar.TypeAndValue.TypeName))
                    compiler->builder->CreateCall(dtor->getFunctionType(), dtor, { destination });
                auto* assignResult = derefAssign(toStore, rhsUnsigned);
                if (kind == AssignSourceKind::Move)
                {
                    // Zero the moved-out source (a named local or `*pa`) so its dtor frees nothing.
                    if (rightNV.Storage != nullptr)
                        compiler->builder->CreateStore(
                            llvm::ConstantAggregateZero::get(toStore->getType()), rightNV.Storage);
                    // Enforce use-after-move only for a plain named source; a deref source has no name.
                    bool srcIsNamedVar = rightNV.Storage != nullptr
                        && (llvm::isa<llvm::AllocaInst>(rightNV.Storage)
                            || llvm::isa<llvm::GlobalVariable>(rightNV.Storage));
                    if (srcIsNamedVar && !rightNV.CallerName.empty())
                        compiler->MarkVariableMoved(rightNV.CallerName);
                }
                return finishStore(assignResult);
            }

            // Destruct old value of an owning-value-type struct FIELD or whole LOCAL variable before
            // overwriting (closes the reassignment LEAK). For a struct field this guards field stores;
            // for a local/global variable it covers `r = f()` where f returns an owned temp (the named-
            // variable-RHS move is already handled and returned above at the MOVE path, and an explicit
            // `move` RHS is nulled below, so only a temp/call-result RHS reaches here for a local dest).
            // Container element stores are excluded (would double-free if touched here).
            bool destIsLocalOwningVar = namedVar.FieldName.empty()
                && (llvm::isa<llvm::AllocaInst>(destination) || llvm::isa<llvm::GlobalVariable>(destination));
            // sameFieldStore, not selfFieldAssign: this EMITS a destruct, so it may only treat two
            // elements as distinct when both root in known-initialized stack/global storage.
            if (operatorText == "=" && right && right->getType()->isStructTy()
                && (destIsStructField || destIsLocalOwningVar)
                && destination != rightNV.Storage
                && !sameFieldStore
                && compiler->IsOwningValueType(namedVar.TypeAndValue.TypeName))
            {
                // NOTE: closes the reassignment LEAK but does NOT fix aliasing of an owning-value RHS -
                // ownership is a runtime property (_len owned bit), so auto copy/move is unsafe for string.
                if (auto* dtor = compiler->GetOrCreateFullDestructor(namedVar.TypeAndValue.TypeName))
                    compiler->builder->CreateCall(dtor->getFunctionType(), dtor, { destination });
            }

            // `unique T* field` reassignment: free the old pointee before overwriting it, or it
            // leaks. Shares EmitUniqueFieldDelete with the synthesized destructor so a field is
            // freed identically at teardown and at reassignment (a divergence between the two
            // would be a silent heap bug). Assigning `nullptr` frees the old pointee too - that is
            // the sanctioned "release early" idiom, and it is why `delete` on a unique field is
            // rejected as unnecessary. Passing `right` lets the helper skip the free on a
            // self-assign. The slot is trusted to be initialized on the same grounds the
            // synthesized destructor already trusts it: every construction path (stack local,
            // `new T()`, `new T[n]`, user-ctor entry) runs a default constructor that
            // zero-initializes the fields.
            bool consumedUniqueFieldSource = false;
            if (operatorText == "=" && right && right->getType()->isPointerTy()
                && destIsStructField
                && namedVar.TypeAndValue.IsUnique)
            {
                compiler->EmitUniqueFieldDelete(
                    *compiler->builder, destination,
                    compiler->GetFullDestructorForDelete(namedVar.TypeAndValue.TypeName),
                    namedVar.TypeAndValue.TypeName, namedVar.TypeAndValue.AllocAlignValue,
                    right);

                // Transfer, not alias: storing a named OWNING `unique T*` local into a unique
                // pointer field must CONSUME the source (null its slot + mark moved) so the field
                // is the sole owner - mirrors the unique-LOCAL reassign and the by-value move sink.
                // Otherwise TransferPointerOwnershipOnStore's refcount-alias path (new-allocated
                // local escaping to a field) leaves both handles live over one pointee - and the
                // field free never consults that refcount, so a UAF is one perturbation away. A
                // `new`/call temp (null Storage) is already a sole owner and left as-is; a store
                // back into the same slot is skipped so it does not dangle; the null-out below is
                // what makes the source's scope-exit free a no-op.
                llvm::Value* srcStorage = rightNV.Storage;
                llvm::Type*  srcBaseTy  = rightNV.BaseType;
                if (srcStorage == nullptr && !rightNV.CallerName.empty())
                {
                    auto ref = compiler->FindVariableStorage(rightNV.CallerName);
                    srcStorage = ref.Storage;
                    srcBaseTy  = ref.BaseType;
                }
                if (rightNV.IsOwning && rightNV.TypeAndValue.Pointer
                    && !rightNV.TypeAndValue.IsArrayView
                    && !rightNV.TypeAndValue.IsMove
                    && !rightNV.CallerName.empty()
                    && rightNV.FieldName.empty()
                    && srcStorage != nullptr && srcStorage != destination)
                {
                    if (auto* ptrTy = llvm::dyn_cast<llvm::PointerType>(srcBaseTy))
                    {
                        compiler->builder->CreateStore(
                            llvm::ConstantPointerNull::get(ptrTy), srcStorage);
                        compiler->MarkVariableMoved(rightNV.CallerName);
                        consumedUniqueFieldSource = true;
                    }
                }
            }

            /*
             * Same reject for an ELEMENT of a `unique T* f[N]` fixed array, field or local. The
             * synthesized teardown releases every slot (EmitOwningUniqueArrayCleanup for a local,
             * EmitUniqueArrayFieldRelease for a field), so a stack address in any slot is freed
             * exactly like the scalar case. The subscript zeroes ConstArraySize and produces a
             * two-index GEP over the ARRAY type, so neither the struct-field nor the local leg
             * sees it: destIsStructField requires a struct source element type, and the local leg
             * requires an alloca/global destination.
             */
            auto* destArrGep = llvm::dyn_cast_or_null<llvm::GetElementPtrInst>(destination);
            if (operatorText == "=" && right && right->getType()->isPointerTy()
                && namedVar.IsElementAccess
                && destArrGep && destArrGep->getNumIndices() == 2
                && destArrGep->getSourceElementType()->isArrayTy()
                && (namedVar.TypeAndValue.IsUnique || namedVar.TypeAndValue.IsUniqueTypeArg)
                && namedVar.TypeAndValue.Pointer
                && !namedVar.TypeAndValue.IsInterface
                && !namedVar.TypeAndValue.IsInterfacePointer
                && !namedVar.TypeAndValue.IsFunctionPointer
                && !namedVar.TypeAndValue.IsArrayView
                && LLVMBackend::IsProvableNonHeapAddress(right))
            {
                std::string slot = namedVar.FieldName.empty() && !namedVar.CallerName.empty()
                    ? namedVar.CallerName : DescribeUniqueFieldOwner(namedVar);
                RejectNonHeapAddressIntoUnique(
                    std::format("an element of unique array '{}'", slot), ctx);
                return finishStore(right);
            }

            // Storing a PROVABLE stack address into a `unique T*` LOCAL. The local's scope-exit
            // teardown frees the pointee, so this would free a stack address. `&local` carries no
            // borrow provenance, so the reassignment legs below never see it.
            if (operatorText == "=" && right && right->getType()->isPointerTy()
                && !destIsStructField
                && (llvm::isa<llvm::AllocaInst>(destination) || llvm::isa<llvm::GlobalVariable>(destination))
                && (namedVar.TypeAndValue.IsUnique || namedVar.TypeAndValue.IsUniqueTypeArg)
                && namedVar.TypeAndValue.Pointer
                && !namedVar.TypeAndValue.IsInterface
                && !namedVar.TypeAndValue.IsInterfacePointer
                && !namedVar.TypeAndValue.IsFunctionPointer
                && !namedVar.TypeAndValue.IsArrayView
                && LLVMBackend::IsProvableNonHeapAddress(right))
            {
                RejectNonHeapAddressIntoUnique(
                    std::format("unique local '{}'", namedVar.CallerName), ctx);
                return finishStore(right);
            }

            // `unique T* LOCAL` reassignment (`b = a`): free the old pointee before overwriting,
            // or the destination's prior object leaks - the field case just above closes this for
            // struct fields, but a thin unique LOCAL fell through to the plain store. Scope exit
            // frees a unique local via EmitOwningPtrCleanup ONLY when IsOwning is set, so we also
            // adopt ownership below. Reuse EmitUniqueFieldDelete for the drop-old so the store-site
            // free matches teardown (a null old pointee is a safe no-op). The outer
            // destination != rightNV.Storage guard - and passing `right` to skip on an equal
            // pointee - make self-assign `a = a` free nothing and keep its live pointer, mirroring
            // the transfer-side self-assign guard in TransferPointerOwnershipOnStore.
            if (operatorText == "=" && right && right->getType()->isPointerTy()
                && !destIsStructField && destIsLocalOwningVar
                && namedVar.TypeAndValue.IsUnique
                && namedVar.TypeAndValue.Pointer
                && !namedVar.TypeAndValue.IsInterface
                && !namedVar.TypeAndValue.IsInterfacePointer
                && !namedVar.TypeAndValue.IsFunctionPointer
                && !namedVar.TypeAndValue.IsArrayView
                && destination != rightNV.Storage)
            {
                // `move` of a BORROWED pointer parameter transfers nothing - the caller still owns
                // the pointee and frees it at its own scope exit. Adopting it into this owning slot
                // would free the caller's live object here and again there. ParseMoveExpression
                // carries IsBorrowed precisely so a store into an owning location can be rejected;
                // this is the LOCAL counterpart of RejectBorrowIntoUniqueField.
                if (rightNV.IsBorrowed)
                {
                    std::string origin = rightNV.BorrowedOrigin.empty()
                        ? rightNV.CallerName : rightNV.BorrowedOrigin;
                    bool srcIsField = !rightNV.FieldName.empty();
                    std::string srcDesc = srcIsField
                        ? std::format("{}.{}", rightNV.CallerName, rightNV.FieldName) : origin;
                    RejectBorrowIntoUniqueLocal(srcDesc, BorrowedOriginRoot(origin), srcIsField,
                                                namedVar.CallerName, false, ctx);
                    return finishStore(right);
                }

                // Same rejection when a '?:' join laundered the borrowed move: the joined VALUE
                // carries no IsBorrowed, yet every arm owned nothing. The DECLARATION form errors.
                std::string borrowMoveOrigin;
                if (compiler->IsMovedBorrowedPtrValue(rightNV.Primary, &borrowMoveOrigin))
                {
                    RejectBorrowIntoUniqueLocal(borrowMoveOrigin, BorrowedOriginRoot(borrowMoveOrigin),
                                                false, namedVar.CallerName, false, ctx);
                    return finishStore(right);
                }

                compiler->EmitUniqueFieldDelete(
                    *compiler->builder, destination,
                    compiler->GetFullDestructorForDelete(namedVar.TypeAndValue.TypeName),
                    namedVar.TypeAndValue.TypeName, namedVar.TypeAndValue.AllocAlignValue,
                    right);
                // When the RHS transfers ownership (owning local, move param, or `new` temp -
                // exactly what TransferPointerOwnershipOnStore nulls), the destination adopts it.
                // A NON-owning destination (`unique R* b = nullptr; b = a;`) would otherwise be
                // skipped by the Pointer && IsOwning scope-exit gate and leak the transferred
                // pointee. Mirrors init and the unique-interface-local reassignment above.
                // srcIsOwnedPtrRhs covers the DETACHED owned sources whose NamedVariable carries
                // no IsOwning at all - `b = move a`, `b = new R()`, `b = moveReturningCall()`: an
                // explicit `move` returns a Storage-less value, so only the RHS shape identifies it.
                if ((rightNV.IsOwning || srcIsOwnedPtrRhs) && !namedVar.CallerName.empty())
                {
                    compiler->SetVariableOwning(namedVar.CallerName, true);
                    compiler->ConsumeOwnedNewTemp(rightNV.Primary);
                }
            }

            // Destruct the old value of an owning-string LOCAL before overwriting it. Closes the
            // reassignment leak where a pre-declared string local reassigned in a loop
            // (`last = name.copy();`) dropped each prior owned buffer. The string dtor checks the
            // runtime owned bit, so freeing a currently-borrowed or empty local is a safe no-op.
            // Limited to plain locals/globals (struct fields are handled just above; container
            // element stores are excluded - they would double-free). Self-assign is guarded by
            // destination != rightNV.Storage.
            if (operatorText == "=" && right && right->getType()->isStructTy()
                && NamedVarIsString(namedVar)
                && namedVar.FieldName.empty()
                && (llvm::isa<llvm::AllocaInst>(destination) || llvm::isa<llvm::GlobalVariable>(destination))
                && destination != rightNV.Storage)
            {
                if (auto* dtor = compiler->GetOrCreateFullDestructor("string"))
                    compiler->builder->CreateCall(dtor->getFunctionType(), dtor, { destination });
            }

            // Part 6: single-index GEP (container element slot) store of an OWNING element type.
            // Lift the deliberate exclusion so `_data[i] = value` in generic container code defers to
            // T: COPY a copyable owner (source stays live), MOVE a non-copyable owner / unique
            // pointer (consume + zero + mark the source), or CLONE a closure. Gated to owning element
            // types, so raw non-owning arrays (primitives, borrows, bare pointers, interface values)
            // keep their plain store below. This does NOT drop the old slot value: a container
            // releases a live slot itself (list.set -> `_releaseAt` then `_placeAt`) and only ever
            // stores into an EMPTY slot here, so a drop-old would double-destruct the (default-
            // constructed) slot - `list<Val>` with a side-effecting dtor proved that. Returns early:
            // it does the source-consume itself, so the generic Transfer*OnStore helpers must not run.
            // Fire ONLY for a NAMED owning source (a by-value param / local, alloca- or
            // global-backed) - `_data[i] = value` / `_data[i] = move value` in collapsed container
            // code. An rvalue temp (`value.copy()`, `new T()`) and a GEP-source move
            // (`newData[i] = move _data[j]` in a grow loop) have no such storage and fall through to
            // the existing plain store, so un-collapsed containers are untouched by the lift.
            bool srcIsNamedLocal = rightNV.Storage != nullptr
                && (llvm::isa<llvm::AllocaInst>(rightNV.Storage)
                    || llvm::isa<llvm::GlobalVariable>(rightNV.Storage))
                && !rightNV.CallerName.empty();
            auto* destSlotGep = llvm::dyn_cast_or_null<llvm::GetElementPtrInst>(destination);
            bool destIsElemSlot = operatorText == "=" && right && srcIsNamedLocal
                && destSlotGep && destSlotGep->getNumIndices() == 1
                && !destIsStructField && !namedVar.IsInterfaceField
                && !namedVar.BitfieldStorage && !namedVar.UnionFieldType
                && !namedVar.TypeAndValue.IsArrayView;
            const std::string& slotElemType = namedVar.TypeAndValue.TypeName;

            if (destIsElemSlot && slotElemType == "__closure_fat_ptr"
                && right->getType()->isStructTy())
            {
                // Closure element: clone a named source (self-safe - the clone reads the source env
                // first), transfer a `move` source. No drop-old (empty slot; see the block header).
                if (!rightNV.TypeAndValue.IsMove)
                    right = CloneClosureFromNamedSource(rightNV, right, ctx);
                compiler->builder->CreateStore(right, destination);
                compiler->UnregisterOwnedClosureTemp(right);
                return finishStore(right);
            }

            if (destIsElemSlot && namedVar.TypeAndValue.IsUnique && namedVar.TypeAndValue.Pointer
                && right->getType()->isPointerTy())
            {
                // unique T* element: store the new pointer, then null + mark-moved a NAMED owning
                // source so exactly one owner frees. No drop-old (empty slot; see the block header).
                compiler->builder->CreateStore(right, destination);
                if (rightNV.Storage != nullptr && !rightNV.CallerName.empty()
                    && rightNV.FieldName.empty()
                    && (llvm::isa<llvm::AllocaInst>(rightNV.Storage)
                        || llvm::isa<llvm::GlobalVariable>(rightNV.Storage)))
                {
                    if (auto* ptrTy = llvm::dyn_cast<llvm::PointerType>(rightNV.BaseType))
                    {
                        compiler->builder->CreateStore(
                            llvm::ConstantPointerNull::get(ptrTy), rightNV.Storage);
                        compiler->MarkVariableMoved(rightNV.CallerName);
                    }
                }
                return finishStore(right);
            }

            if (destIsElemSlot && right->getType()->isStructTy()
                && rightNV.TypeAndValue.IsFatInterfaceValue()
                && (rightNV.TypeAndValue.IsUnique || rightNV.TypeAndValue.IsUniqueTypeArg))
            {
                // unique <interface> element: store the fat value, then zero + mark-moved a NAMED
                // owning source so exactly one owner frees (mirrors the unique-pointer arm for the
                // fat-ptr slot). Without the zero a plain (non-move) unique-interface sink param -
                // now owning at entry (8a) - is freed again at its scope exit. No drop-old (empty
                // slot; see the block header). A bare (borrow) interface element never reaches here.
                compiler->builder->CreateStore(right, destination);
                if (rightNV.Storage != nullptr && !rightNV.CallerName.empty()
                    && rightNV.FieldName.empty()
                    && (llvm::isa<llvm::AllocaInst>(rightNV.Storage)
                        || llvm::isa<llvm::GlobalVariable>(rightNV.Storage)))
                {
                    compiler->builder->CreateStore(
                        llvm::ConstantAggregateZero::get(right->getType()), rightNV.Storage);
                    compiler->MarkVariableMoved(rightNV.CallerName);
                }
                return finishStore(right);
            }

            if (destIsElemSlot && right->getType()->isStructTy()
                && (compiler->IsOwningValueType(slotElemType) || slotElemType == "string"))
            {
                // Owning value struct / string element: COPY a copyable owner (source stays live),
                // MOVE a non-copyable owner or an explicit `move`. No drop-old (empty slot; see the
                // block header). On a Move, zero the moved-out NAMED source so its dtor is a no-op.
                AssignSourceKind slotKind;
                llvm::Value* toStore = ClassifyOwningAssignSource(
                    right, slotElemType, rightNV.TypeAndValue.IsMove, ctx, slotKind);
                compiler->builder->CreateStore(toStore, destination);
                if (slotKind == AssignSourceKind::Move && rightNV.Storage != nullptr)
                {
                    compiler->builder->CreateStore(
                        llvm::ConstantAggregateZero::get(toStore->getType()), rightNV.Storage);
                    if (!rightNV.CallerName.empty()
                        && (llvm::isa<llvm::AllocaInst>(rightNV.Storage)
                            || llvm::isa<llvm::GlobalVariable>(rightNV.Storage)))
                        compiler->MarkVariableMoved(rightNV.CallerName);
                }
                return finishStore(toStore);
            }

            auto* assignResult = derefAssign(right, rhsUnsigned);
            // Array-view element store: tag the store with the view's alias scope (matches the load
            // side in TagViewElementAccess) so the vectorizer proves distinct views disjoint.
            if (namedVar.TypeAndValue.NoaliasScopeId >= 0)
                if (auto* st = llvm::dyn_cast_or_null<llvm::StoreInst>(assignResult))
                    compiler->AttachViewNoalias(st, namedVar.TypeAndValue.NoaliasScopeId);
            // Skip when the unique-field store above already consumed the source: it did the
            // transfer explicitly, and re-running here would take the aliasing refcount path.
            if (operatorText == "=" && !consumedUniqueFieldSource)
                TransferPointerOwnershipOnStore(
                    rightNV, destination, namedVar.TypeAndValue.IsInterface, ctx);
            // Transfer ownership for move string: null _ptr so string.dtor is a no-op
            // after the value has been moved to persistent storage (e.g. list::add).
            if (operatorText == "=")
                TransferMoveStringOwnershipOnStore(rightNV, ctx);
            // Reassignment to a moved variable (or field) makes it live again.
            if (operatorText == "=" && !namedVar.CallerName.empty())
            {
                if (!namedVar.FieldName.empty())
                    compiler->MarkVariableFieldUnmoved(namedVar.CallerName, namedVar.FieldName);
                else
                {
                    compiler->MarkVariableUnmoved(namedVar.CallerName);
                    compiler->MarkVariableNotExplicitlyMovedNull(namedVar.CallerName);
                    // --sanitize=ownership (M1): the local is live again - reset its origin slot.
                    compiler->ClearOwnMoveOrigin(destination);
                }
            }
            /*
             * The three refreshes below RETIRE a fact, and '??=' stores CONDITIONALLY - the old
             * referent survives when the arm is not taken - so a '??=' takes the JOIN: the fact is
             * kept unless the new RHS also carries it. Retiring unconditionally would launder a
             * container-owned element borrow (measured: the desugared `if (g == nullptr) { g = ...; }`
             * spelling clears the taint, and the resulting raw `delete g;` double-frees).
             */
            // Reassignment to a bonded variable breaks the bond (per design: bond is to the instance).
            if (operatorText == "=" && coalesceResume == nullptr && !namedVar.CallerName.empty())
                compiler->ClearVariableBond(namedVar.CallerName);
            // Refresh the field-borrow taint on a reassigned whole-variable string local: it now
            // borrows a field iff the new RHS does (so `t = "lit"` after `t = b.name` clears it).
            if (operatorText == "=" && !namedVar.CallerName.empty() && namedVar.FieldName.empty()
                && NamedVarIsString(namedVar))
            {
                bool rhsBorrowsField = NamedVarIsString(rightNV)
                    && !rightNV.TypeAndValue.IsMove
                    && ((!rightNV.FieldName.empty() && !rightNV.OwningStructName.empty())
                        || rightNV.BorrowsOwnedString);
                if (rhsBorrowsField || coalesceResume == nullptr)
                    compiler->SetVariableBorrowsOwnedString(namedVar.CallerName, rhsBorrowsField);
            }
            // Refresh the container-owned-element borrow taint on a reassigned pointer local: it
            // now borrows a container-owned element iff the new RHS does, so `g = new B()` after
            // `g = l.get(0)` clears it and a later `delete g` is allowed (item 2 false positive).
            if (operatorText == "=" && !namedVar.CallerName.empty() && namedVar.FieldName.empty()
                && namedVar.TypeAndValue.Pointer)
            {
                bool rhsBorrowsElem = (rightNV.TypeAndValue.IsBorrowOfUniqueElement
                    || rightNV.TypeAndValue.IsBorrowOfAliasElement)
                    && !compiler->lastOwningResult;
                bool rhsExternallyOwned = rightNV.TypeAndValue.IsBorrowOfAliasElement
                    && !compiler->lastOwningResult;
                std::string container = rightNV.TypeAndValue.ParentVariableName.empty()
                    ? rightNV.CallerName : rightNV.TypeAndValue.ParentVariableName;
                if (rhsBorrowsElem || coalesceResume == nullptr)
                    compiler->SetVariableBorrowsOwnedElement(namedVar.CallerName, rhsBorrowsElem, container, rhsExternallyOwned);
            }
            return finishStore(assignResult);
        }
        else if (unaryCtx)
        {
            auto namedVar = ParseUnaryExpression(unaryCtx);
            auto destination = LoadNamedVariable(namedVar);
            return destination;
        }

        LogErrorContext(ctx, "Unhandled assignment expression form.");
        return nullptr;
    }

llvm::Value* MainListener::BoxTernaryThinArmToInterface(llvm::Value* thinValue, const std::string& interfaceName,
                                              std::string& armFailure) {
        auto* compiler = compilerLLVM;
        auto* fatTy = compiler->GetFatPtrType();
        if (auto* c = llvm::dyn_cast<llvm::Constant>(thinValue); c != nullptr && c->isNullValue())
            return llvm::Constant::getNullValue(fatTy);

        std::string className = compiler->ResolvePointerElementTypeName(thinValue);
        if (className.empty())
        {
            armFailure = "the arm's concrete class cannot be determined; bind the arm to a "
                         "local variable of the class type first";
            return nullptr;
        }
        if (!compiler->StructImplementsInterface(className, interfaceName))
        {
            armFailure = std::format("'{}' does not implement it", className);
            return nullptr;
        }
        auto* vtable = compiler->GetOrCreateVTable(className, interfaceName);
        auto* boxed = compiler->BuildInterfaceFatValue(vtable, thinValue);
        // Ledger which interface this box was built against, so the sibling fat arm's interface
        // (both arms now agree) survives to the eventual phi and the receiver's rebox check.
        compiler->RegisterFatInterfaceValueTypeName(boxed, interfaceName);

        // Carry the ownership verdict onto the box: a fat arm is scored owning only via the
        // owning-RETURN ledger (a struct is never an owning-POINTER temp).
        if (compiler->TernaryArmJoinsOwning(thinValue))
        {
            if (compiler->IsMovedOutPtrValue(thinValue))
                compiler->RegisterMovedOutPtrValue(boxed);
            else
                compiler->RegisterOwnedReturnTemp(boxed, "?:", LLVMBackend::TypeAndValue{});
        }
        // thinValue's move-of-borrow provenance is dropped here: unreachable today (scored
        // non-owning above already), and the ledger refuses a struct (boxed) regardless.
        return boxed;
    }

bool MainListener::RejectCodeValueTernaryStringArm(CFlatParser::ConditionalExpressionContext* ctx,
                                         llvm::Value* armValue, size_t armOccurrence) {
        auto* compiler = Compiler(ctx);
        // Runs after both arms are parsed, so the ambient id is the enclosing slot's again - the
        // arm answers under the id IT evaluated under (see joinArmOccurrences_).
        if (!compiler->JoinArmCarriesCodeValue(armValue, armOccurrence)) return false;
        LogErrorContext(ctx, "cannot convert a function-pointer or closure VALUE in a '?:' arm to "
            "'string' - code is not a NUL-terminated buffer; write an explicit '(char*)' cast if "
            "the raw code address is what you want");
        return true;
    }

bool MainListener::UnifyTernaryArmTypes(CFlatParser::ConditionalExpressionContext* ctx,
                              llvm::Value*& trueValue, llvm::Value*& falseValue,
                              const std::function<void()>& atTrue,
                              const std::function<void()>& atFalse,
                              size_t trueOccurrence, size_t falseOccurrence) {
        auto* compiler = Compiler(ctx);
        if (!trueValue || !falseValue || trueValue->getType() == falseValue->getType())
            return true;

        auto* ft = falseValue->getType();
        auto* tt = trueValue->getType();
        auto* strTy = llvm::StructType::getTypeByName(*compiler->context, "string");
        if (ft->isIntegerTy() && tt->isIntegerTy())
        {
            unsigned fb = ft->getIntegerBitWidth();
            unsigned tb = tt->getIntegerBitWidth();
            if (fb < tb) { atFalse(); falseValue = compiler->Upconvert(falseValue, tt); }
            else         { atTrue();  trueValue  = compiler->Upconvert(trueValue,  ft); }
        }
        else if (ft->isFloatingPointTy() && tt->isFloatingPointTy())
        {
            // Widen the narrower branch (e.g. float literal 1.0 -> double).
            unsigned fb = ft->getScalarSizeInBits();
            unsigned tb = tt->getScalarSizeInBits();
            if (fb < tb) { atFalse(); falseValue = compiler->Upconvert(falseValue, tt); }
            else         { atTrue();  trueValue  = compiler->Upconvert(trueValue,  ft); }
        }
        else if (ft->isFloatingPointTy() && tt->isIntegerTy())
        {
            // Mixed int/float (e.g. cond ? 1 : 2.5): promote the integer side.
            atTrue();
            trueValue = compiler->Upconvert(trueValue, ft);
        }
        else if (ft->isIntegerTy() && tt->isFloatingPointTy())
        {
            atFalse();
            falseValue = compiler->Upconvert(falseValue, tt);
        }
        else if (strTy && tt == strTy && ft->isPointerTy())
        {
            // string vs char* literal (e.g. cond ? prefix : "0"): wrap the raw
            // pointer into a non-owning string so both branches are `string`.
            if (RejectCodeValueTernaryStringArm(ctx, falseValue, falseOccurrence)) return false;
            atFalse();
            falseValue = compiler->WrapStringLiteralAsString(falseValue);
        }
        else if (strTy && ft == strTy && tt->isPointerTy())
        {
            if (RejectCodeValueTernaryStringArm(ctx, trueValue, trueOccurrence)) return false;
            atTrue();
            trueValue = compiler->WrapStringLiteralAsString(trueValue);
        }
        else if (auto* fatTy = compiler->GetFatPtrType();
                 (ft == fatTy && tt->isPointerTy()) || (tt == fatTy && ft->isPointerTy()))
        {
            // One arm already carries the interface fat struct (e.g. `move` of an
            // interface-TYPED local, which evaluates directly to {vtable,data} since the local
            // itself is already fat-typed); the other is still a thin pointer (`new T()` /
            // `nullptr`). Learn which interface from the fat arm's ledgered TypeName - the fat
            // struct itself does not say which interface it is, since every interface shares
            // this one LLVM type - then box the thin arm into the SAME interface, in its own
            // arm block, mirroring UpcastTernaryPhiToInterface's per-arm boxing.
            bool trueIsFat = (tt == fatTy);
            llvm::Value*& fatValue  = trueIsFat ? trueValue : falseValue;
            llvm::Value*& thinValue = trueIsFat ? falseValue : trueValue;
            std::string interfaceName = compiler->FindFatInterfaceValueTypeName(fatValue);
            if (interfaceName.empty())
            {
                LogErrorContext(ctx, "cannot convert '?:' arms to a common interface type: the "
                    "fat arm's interface could not be determined");
                return false;
            }
            std::string armFailure;
            (trueIsFat ? atFalse : atTrue)();
            llvm::Value* boxed = BoxTernaryThinArmToInterface(thinValue, interfaceName, armFailure);
            if (boxed == nullptr)
            {
                LogErrorContext(ctx, std::format(
                    "cannot convert '?:' arm to interface '{}': {}", interfaceName, armFailure));
                return false;
            }
            thinValue = boxed;
        }
        else
        {
            auto describe = [](llvm::Type* t) -> std::string {
                if (auto* st = llvm::dyn_cast<llvm::StructType>(t))
                    return (st->hasName() ? st->getName().str() : std::string("struct"));
                if (t->isPointerTy()) return "pointer";
                if (t->isIntegerTy()) return "i" + std::to_string(t->getIntegerBitWidth());
                if (t->isFloatTy())   return "float";
                if (t->isDoubleTy())  return "double";
                return "value";
            };
            // ctx->expression() is the true branch; ctx->conditionalExpression() the false.
            LogErrorContext(ctx, std::format(
                "ternary branches have incompatible types '{}' and '{}'",
                describe(tt), describe(ft)));
            return false;
        }
        return true;
    }

llvm::Value* MainListener::AdoptTernaryStringArm(LLVMBackend* compiler, llvm::Value* value, bool& deepCopied) {
        auto* strTy = llvm::StructType::getTypeByName(*compiler->context, "string");
        if (strTy == nullptr || value == nullptr || value->getType() != strTy) return value;

        auto* owned = compiler->EmitOwnedStringDeepCopy(value);
        if (owned == value) return value;
        deepCopied = true;
        // A temp nothing else owns: already registered, or a plain (non-`move`) string-returning
        // CALL, which the call path does not register. A local/field read is neither - its owner frees it.
        if (compiler->IsPendingOwnedStringTemp(value) || llvm::isa<llvm::CallInst>(value))
        {
            compiler->UnregisterOwnedStringTemp(value);
            compiler->EmitOwnedStringTempFree(value);
        }
        return owned;
    }

void MainListener::FinishTernaryArm(LLVMBackend* compiler, llvm::Value*& value,
                          const LLVMBackend::OwnedTempMark& mark, bool& deepCopied) {
        value = AdoptTernaryStringArm(compiler, value, deepCopied);
        compiler->FlushOwnedTempsSince(mark, deepCopied ? nullptr : value);
    }

LLVMBackend::TypedValue MainListener::ParseTernaryBranches(
        CFlatParser::ConditionalExpressionContext* ctx,
        const LLVMBackend::TypedValue& condTv,
        CFlatParser::ExpressionContext* expressionTrueCtx,
        CFlatParser::ConditionalExpressionContext* expressionFalseCtx) {
        auto* compiler = Compiler(ctx);
        if (condTv.value == nullptr) return {};

        auto* trueBlock   = compiler->CreateBasicBlock("ternary_true");
        auto* falseBlock  = compiler->CreateBasicBlock("ternary_false");
        auto* resumeBlock = compiler->CreateBasicBlock("ternary_resume");

        // Coerces a non-bool condition to i1; leaves the insert point in trueBlock.
        compiler->CreateConditionJump(condTv.value, trueBlock, falseBlock);

        llvm::Value* trueValue  = nullptr;
        llvm::Value* falseValue = nullptr;
        llvm::BranchInst* trueBr  = nullptr;
        llvm::BranchInst* falseBr = nullptr;
        bool trueOwnedString  = false;
        bool falseOwnedString = false;
        LLVMBackend::OwnedTempMark trueMark  = compiler->MarkOwnedTemps();
        LLVMBackend::OwnedTempMark falseMark = trueMark;
        // Give each arm its OWN cast occurrence: both arms share the enclosing argument slot, so
        // without this a cast on one arm launders a bare sibling naming the same constant.
        size_t trueOcc  = compiler->CurrentCastOccurrence();
        size_t falseOcc = trueOcc;
        try
        {
            {
                LLVMBackend::CastOccurrenceScope armScope(compiler);
                trueOcc   = armScope.Id;
                trueValue = ParseExpression(expressionTrueCtx);
                FinishTernaryArm(compiler, trueValue, trueMark, trueOwnedString);
            }
            trueBr    = compiler->CreateJump(resumeBlock);

            compiler->SwitchToBlock(falseBlock);
            falseMark  = compiler->MarkOwnedTemps();
            {
                LLVMBackend::CastOccurrenceScope armScope(compiler);
                falseOcc   = armScope.Id;
                falseValue = ParseConditionalExpression(expressionFalseCtx);
                FinishTernaryArm(compiler, falseValue, falseMark, falseOwnedString);
            }
            falseBr    = compiler->CreateJump(resumeBlock);
        }
        catch (...)
        {
            // An error thrown mid-arm leaves half-emitted blocks; terminate them so the module
            // still verifies when the error is caught (expect_error) and compilation continues.
            if (compiler->IsInsertBlockLive()) compiler->builder->CreateBr(resumeBlock);
            for (auto* bb : { trueBlock, falseBlock })
            {
                if (bb->getTerminator() != nullptr) continue;
                compiler->SwitchToBlock(bb);
                compiler->builder->CreateBr(resumeBlock);
            }
            compiler->SwitchToBlock(resumeBlock);
            // Drop, without emitting anything, whatever the aborted arms registered: those
            // entries are keyed to arm blocks and would otherwise outlive this expression.
            compiler->DiscardOwnedTempsSince(falseMark);
            compiler->DiscardOwnedTempsSince(trueMark);
            throw;
        }

        // An arm whose own lowering terminated its block never reaches the join; the ternary can
        // then only yield the other arm, so no PHI is needed (and none is legal).
        if (trueBr == nullptr || falseBr == nullptr)
        {
            compiler->SwitchToBlock(resumeBlock);
            if (trueBr == nullptr && falseBr == nullptr) return {};
            llvm::Value* only = trueBr != nullptr ? trueValue : falseValue;
            compiler->PromoteCastOccurrence(only, trueBr != nullptr ? trueOcc : falseOcc);
            return { only, false };
        }

        if (trueValue == nullptr || falseValue == nullptr)
        {
            compiler->SwitchToBlock(resumeBlock);
            return {};
        }

        auto atTrue  = [&]() { compiler->builder->SetInsertPoint(trueBr); };
        auto atFalse = [&]() { compiler->builder->SetInsertPoint(falseBr); };
        if (!UnifyTernaryArmTypes(ctx, trueValue, falseValue, atTrue, atFalse, trueOcc, falseOcc))
        {
            compiler->SwitchToBlock(resumeBlock);
            return {};
        }

        if (trueValue->getType()->isVoidTy())
        {
            compiler->SwitchToBlock(resumeBlock);
            LogErrorContext(ctx, "ternary branches must produce a value; 'void' is not allowed");
            return {};
        }

        // An arm that only BECAME a string in unification is a wrapped char* literal, borrowing
        // nothing the flush could free, so copying after it is safe (see FinishTernaryArm).
        auto* strTy = llvm::StructType::getTypeByName(*compiler->context, "string");
        bool ownedString = false;
        if (strTy != nullptr && trueValue->getType() == strTy)
        {
            if (!trueOwnedString)  { atTrue();  trueValue  = AdoptTernaryStringArm(compiler, trueValue,  trueOwnedString); }
            if (!falseOwnedString) { atFalse(); falseValue = AdoptTernaryStringArm(compiler, falseValue, falseOwnedString); }
            ownedString = trueOwnedString && falseOwnedString;
        }

        auto* trueEnd  = trueBr->getParent();
        auto* falseEnd = falseBr->getParent();
        compiler->SwitchToBlock(resumeBlock);
        auto* phi = compiler->builder->CreatePHI(trueValue->getType(), 2, "ternary");
        phi->addIncoming(trueValue,  trueEnd);
        phi->addIncoming(falseValue, falseEnd);
        compiler->RegisterJoinArmCastOccurrence(phi, 0, trueOcc);
        compiler->RegisterJoinArmCastOccurrence(phi, 1, falseOcc);

        if (ownedString)
        {
            compiler->RegisterOwnedStringTemp(phi);
            compiler->lastCallReturnsOwned = true;
            return { phi, true };
        }

        // A ternary is a transparent wrapper: ownership rides out on the joined value.
        // Mixed owning/borrow POINTER joins are handled in PropagateTernaryOwnership.
        // A mixed join owns NOTHING the receiver may free, so the sticky per-expression ownership
        // side-channels must be cleared too: they carry no value identity, so a declaration would
        // otherwise adopt the phi whichever arm ran and double-free the borrow arm's live pointee.
        if (compiler->PropagateTernaryOwnership(trueValue, falseValue, phi))
            compiler->ClearOwnedResultChannels();
        compiler->PropagateFatInterfaceJoin(trueValue, falseValue, phi);
        return { phi, false };
    }

LLVMBackend::TypedValue MainListener::ParseConditionalExpression(CFlatParser::ConditionalExpressionContext* ctx) {
        auto* compiler = Compiler(ctx);
        auto logicCtx = ctx->logicalOrExpression();

        if (ctx->QuestionQuestion())
        {
            // Null-coalescing: lhs ?? rhs  ->  (lhs != null) ? lhs : rhs
            // Per-arm cast occurrence, exactly as ParseTernaryBranches scopes its two arms.
            llvm::Value* lhs = nullptr;
            size_t lhsOcc = compiler->CurrentCastOccurrence();
            {
                LLVMBackend::CastOccurrenceScope armScope(compiler);
                lhsOcc = armScope.Id;
                lhs = ParseLogicalOrExpression(logicCtx);
            }
            if (!lhs) return {};

            auto* resultAlloca = compiler->CreateAlloca(lhs->getType());

            auto* nullBlock = compiler->CreateBasicBlock("nullcoal_null");
            auto* notNullBlock = compiler->CreateBasicBlock("nullcoal_notnull");
            auto* resumeBlock = compiler->CreateBasicBlock("nullcoal_resume");

            compiler->CreateConditionJump(lhs, notNullBlock, nullBlock);
            // insert point is now notNullBlock (lhs is not null)
            compiler->CreateAssignment(lhs, resultAlloca);
            auto* lhsBr = compiler->CreateJump(resumeBlock);

            compiler->SwitchToBlock(nullBlock);
            llvm::Value* rhs = nullptr;
            size_t rhsOcc = compiler->CurrentCastOccurrence();
            {
                LLVMBackend::CastOccurrenceScope armScope(compiler);
                rhsOcc = armScope.Id;
                rhs = ParseConditionalExpression(ctx->conditionalExpression());
            }
            compiler->CreateAssignment(rhs, resultAlloca);
            auto* rhsBr = compiler->CreateJump(resumeBlock);

            compiler->SwitchToBlock(resumeBlock);
            auto* joined = compiler->CreateLoad(resultAlloca);
            // Ledger the arms for the interface-boxing path: this joins through a SLOT, so the
            // result is a plain load and the arms are unrecoverable from the IR afterwards.
            if (joined != nullptr && lhsBr != nullptr && rhsBr != nullptr
                && rhs != nullptr && joined->getType()->isPointerTy()
                && lhs->getType()->isPointerTy() && rhs->getType()->isPointerTy())
            {
                compiler->RegisterNullCoalesceJoin(
                    joined, { { lhs, lhsBr->getParent() }, { rhs, rhsBr->getParent() } });
                compiler->RegisterJoinArmCastOccurrence(joined, 0, lhsOcc);
                compiler->RegisterJoinArmCastOccurrence(joined, 1, rhsOcc);
            }
            return { joined, false };
        }

        // Grammar: logicalOrExpression ('?' expression ':' conditionalExpression)?
        // - so `expression` is the TRUE branch and `conditionalExpression` is the FALSE branch.
        auto expressionTrueCtx = ctx->expression();
        auto expressionFalseCtx = ctx->conditionalExpression();

        if (logicCtx != nullptr)
        {
            auto condTv = ParseLogicalOrExpression(logicCtx);

            // Both expression should exist or not exist.
            if ((expressionFalseCtx != nullptr) != (expressionTrueCtx != nullptr))
            {
                LogErrorContext(ctx, "Conditional expression requires both true and false branches.");
                return {};
            }
            else if (expressionFalseCtx != nullptr && (expressionTrueCtx != nullptr))
            {
                // In a function, branch so only the selected arm runs. A constant context (enum,
                // bitfield width, alignas, global init) has no live block and keeps the eager form.
                auto* insertBB = compiler->builder->GetInsertBlock();
                if (insertBB != nullptr && insertBB->getTerminator() == nullptr
                    && compiler->currentFunction != nullptr
                    && insertBB->getParent() == compiler->currentFunction)
                {
                    return ParseTernaryBranches(ctx, condTv, expressionTrueCtx, expressionFalseCtx);
                }

                // Eager fallback: BOTH arms execute unconditionally, so a deref only sound under
                // one arm's condition is unsafe here - suppress the same-block moved-null guard.
                llvm::Value* trueValue  = nullptr;
                llvm::Value* falseValue = nullptr;
                {
                    SuppressExplicitNullDerefGuardScope suppressGuard(compiler);
                    trueValue  = ParseExpression(expressionTrueCtx);
                    falseValue = ParseConditionalExpression(expressionFalseCtx);
                }

                // A branch that is a plain (non-`move`) string-returning CALL yields a fresh owned
                // temp the call path did not register (only `move` returns are). CreateSelect below
                // evaluates BOTH branches and the string case deep-copies the winner into a new owned
                // buffer, leaving each branch's temp unreferenced - so an unregistered plain-return
                // temp leaks. Register any call-result string branch for end-of-statement cleanup;
                // this reuses the operator-operand path's logic: a named local / field read lowers to
                // a load (not a CallInst) and is skipped, so it is never double-freed, and the free is
                // owned-bit-gated so a borrow return is a runtime no-op. `move` returns are already
                // registered and re-registration is idempotent (deduped).
                RegisterBorrowedStringOperandTemp(compiler, trueValue);
                RegisterBorrowedStringOperandTemp(compiler, falseValue);

                auto here = []() {};   // eager form: both arms already live in the current block
                if (!UnifyTernaryArmTypes(ctx, trueValue, falseValue, here, here,
                        compiler->CurrentCastOccurrence(), compiler->CurrentCastOccurrence()))
                    return {};

                // LLVM's select requires an i1 condition; a non-bool CFlat condition
                // (int, char, pointer, float) must be lowered the same way if/while do.
                auto* selectCond = compiler->CoerceToBoolCondition(condTv.value);
                auto* selectValue = compiler->CreateSelect(selectCond, falseValue, trueValue);

                // Owning-string ternary: a branch that reads an owning string FIELD or a named
                // owning local (`cond ? this.name : "-"`) yields a struct whose OWNED bit rides
                // along with the aliased buffer pointer. The select copies that struct verbatim,
                // so the ternary result shallow-aliases the branch's buffer; binding it to a local
                // (whose owned-bit-gated dtor then fires) frees a buffer the field/local still owns
                // (use-after-free). Deep-copy the selected string into an independent owned buffer
                // (always safe, mirrors the manual `"" + this.name` workaround and the direct
                // `string x = obj.name` deep-copy path), then hand it back as an owned temporary so
                // the standard owned-temp machinery frees it if it is not moved into a named local.
                auto* strTy = llvm::StructType::getTypeByName(*compiler->context, "string");
                if (strTy != nullptr && selectValue != nullptr && selectValue->getType() == strTy)
                {
                    auto* owned = compiler->EmitOwnedStringDeepCopy(selectValue);
                    if (owned != selectValue)
                    {
                        compiler->RegisterOwnedStringTemp(owned);
                        compiler->lastCallReturnsOwned = true;
                        return { owned, true };
                    }
                }
                // Eager form: both arms already ran, so only the selected one can be adopted
                // and the other leaks regardless. See PropagateTernaryOwnership.
                if (compiler->PropagateTernaryOwnership(trueValue, falseValue, selectValue))
                    compiler->ClearOwnedResultChannels();
                compiler->PropagateFatInterfaceJoin(trueValue, falseValue, selectValue);
                return { selectValue, false };
            }

            return condTv;
        }

        LogErrorContext(ctx, "Conditional expression has no logical-or sub-expression.");
        return {};
    }

LLVMBackend::TypedValue MainListener::ParseLogicalOrExpression(CFlatParser::LogicalOrExpressionContext* ctx) {
        auto* compiler = Compiler(ctx);
        auto logicCtxs = ctx->logicalAndExpression();

        if (logicCtxs.size() == 1)
        {
            return ParseLogicalAndExpression(logicCtxs[0]);
        }
        else if (logicCtxs.size() > 1)
        {
            llvm::Value* left = nullptr;

            // Always use resultStorage path. The elseBlock optimization was broken:
            // when the first || operand is true it jumped to blockFalse instead of blockTrue,
            // because only the false-destination is stored in the block context.
            LLVMBackend::TypeAndValue boolValue = { .TypeName = "bool",.VariableName = "", .Pointer = false };
            auto resultStorage = compiler->CreateAlloca(compiler->GetType(boolValue));
            auto resumeBlock = compiler->CreateBasicBlock("resumeOR");

            for (const auto& logicCtx : logicCtxs)
            {
                if (left == nullptr)
                {
                    left = ParseLogicalAndExpression(logicCtx);
                    compiler->CreateAssignment(left, resultStorage);
                }
                else
                {
                    auto falseBlock = compiler->CreateBasicBlock("falseOR");
                    auto branch = compiler->CreateConditionJump(left, resumeBlock, falseBlock);

                    compiler->InitializeBlock(falseBlock, false);
                    llvm::Value* right = ParseLogicalAndExpression(logicCtx);
                    left = compiler->CreateOperation(LLVMBackend::Operation::LogicalOr, left, right);
                    compiler->CreateAssignment(left, resultStorage);
                }
            }

            compiler->CreateBlockBreak(resumeBlock, false);

            compiler->InitializeBlock(resumeBlock, false);
            return { compiler->CreateLoad(resultStorage), false };
        }

        LogErrorContext(ctx, "Logical-OR expression has no operands.");
        return {};
    }

LLVMBackend::TypedValue MainListener::ParseLogicalAndExpression(CFlatParser::LogicalAndExpressionContext* ctx) {
        auto* compiler = Compiler(ctx);
        auto inclusiveCtxs = ctx->inclusiveOrExpression();

        if (inclusiveCtxs.size() == 1)
        {
            return ParseInclusiveOrExpression(inclusiveCtxs[0]);
        }
        else if (inclusiveCtxs.size() > 1)
        {
            llvm::Value* left = nullptr;

            // Always use resultStorage path. The elseBlock optimization was broken:
            // in non-condition contexts (e.g. bool x = a && b inside a loop body) the
            // elseBlock from an enclosing scope (loop exit) was incorrectly used as the
            // false-branch target, causing the loop to exit instead of continuing.
            LLVMBackend::TypeAndValue boolValue = { .TypeName = "bool",.VariableName = "", .Pointer = false };
            auto resultStorage = compiler->CreateAlloca(compiler->GetType(boolValue));
            auto resumeBlock = compiler->CreateBasicBlock("resumeAND");

            for (const auto& inclusiveCtx : inclusiveCtxs)
            {
                if (left == nullptr)
                {
                    left = ParseInclusiveOrExpression(inclusiveCtx);
                    compiler->CreateAssignment(left, resultStorage);
                }
                else
                {
                    auto trueBlock = compiler->CreateBasicBlock("trueAND");
                    auto branch = compiler->CreateConditionJump(left, trueBlock, resumeBlock);

                    compiler->InitializeBlock(trueBlock, false);
                    llvm::Value* right = ParseInclusiveOrExpression(inclusiveCtx);
                    left = compiler->CreateOperation(LLVMBackend::Operation::LogicalAnd, left, right);
                    compiler->CreateAssignment(left, resultStorage);
                }
            }

            compiler->CreateBlockBreak(resumeBlock, false);

            compiler->InitializeBlock(resumeBlock, false);
            return { compiler->CreateLoad(resultStorage), false };
        }

        LogErrorContext(ctx, "Logical-AND expression has no operands.");
        return {};
    }

LLVMBackend::TypedValue MainListener::ParseInclusiveOrExpression(CFlatParser::InclusiveOrExpressionContext* ctx) {
        auto exclusiveCtxs = ctx->exclusiveOrExpression();
        if (exclusiveCtxs.size() == 1)
            return ParseExclusiveOrExpression(exclusiveCtxs[0]);

        if (exclusiveCtxs.size() > 1)
        {
            auto lv = ParseExclusiveOrExpression(exclusiveCtxs[0]);
            llvm::Value* acc = lv.value;
            for (size_t i = 1; i < exclusiveCtxs.size(); i++)
            {
                auto rv = ParseExclusiveOrExpression(exclusiveCtxs[i]);
                acc = Compiler(ctx)->CreateOperation(LLVMBackend::Operation::BitwiseOr, acc, rv.value);
            }
            return { acc, lv.isUnsigned };
        }

        LogErrorContext(ctx, "Inclusive-OR expression has no operands.");
        return {};
    }

LLVMBackend::TypedValue MainListener::ParseExclusiveOrExpression(CFlatParser::ExclusiveOrExpressionContext* ctx) {
        auto andCtxs = ctx->andExpression();
        if (andCtxs.size() == 1)
            return ParseAndExpression(andCtxs[0]);

        if (andCtxs.size() > 1)
        {
            auto lv = ParseAndExpression(andCtxs[0]);
            llvm::Value* acc = lv.value;
            for (size_t i = 1; i < andCtxs.size(); i++)
            {
                auto rv = ParseAndExpression(andCtxs[i]);
                acc = Compiler(ctx)->CreateOperation(LLVMBackend::Operation::BitwiseXor, acc, rv.value);
            }
            return { acc, lv.isUnsigned };
        }

        LogErrorContext(ctx, "Exclusive-OR expression has no operands.");
        return {};
    }

LLVMBackend::TypedValue MainListener::ParseAndExpression(CFlatParser::AndExpressionContext* ctx) {
        auto nextCtxs = ctx->equalityExpression();
        if (nextCtxs.size() == 1)
            return ParseEqualityExpression(nextCtxs[0]);

        if (nextCtxs.size() > 1)
        {
            auto lv = ParseEqualityExpression(nextCtxs[0]);
            llvm::Value* acc = lv.value;
            for (size_t i = 1; i < nextCtxs.size(); i++)
            {
                auto rv = ParseEqualityExpression(nextCtxs[i]);
                acc = Compiler(ctx)->CreateOperation(LLVMBackend::Operation::BitwiseAnd, acc, rv.value);
            }
            return { acc, lv.isUnsigned };
        }

        LogErrorContext(ctx, "Bitwise-AND expression has no operands.");
        return {};
    }

void MainListener::LowerInterfaceNullCompare(antlr4::ParserRuleContext* ctx,
                                   LLVMBackend::TypedValue& lv, LLVMBackend::TypedValue& rv) {
        auto* compiler = Compiler(ctx);
        auto* fatTy = compiler->GetFatPtrType();
        auto isFat  = [&](const LLVMBackend::TypedValue& v)
            { return v.value != nullptr && v.value->getType() == fatTy; };
        auto isNull = [&](const LLVMBackend::TypedValue& v)
            { return llvm::isa_and_nonnull<llvm::ConstantPointerNull>(v.value); };

        if (isFat(lv) && isNull(rv))
            lv.value = compiler->builder->CreateExtractValue(lv.value, { 1u }, "iface_data");
        else if (isFat(rv) && isNull(lv))
            rv.value = compiler->builder->CreateExtractValue(rv.value, { 1u }, "iface_data");
    }

LLVMBackend::TypedValue MainListener::ParseEqualityExpression(CFlatParser::EqualityExpressionContext* ctx) {
        auto nextCtxs = ctx->typeCheckExpression();
        if (nextCtxs.size() == 1)
        {
            return ParseTypeCheckExpression(nextCtxs[0]);
        }
        else if (nextCtxs.size() == 2)
        {
            // Clear lastCallReturnsOwned before each operand: a prior operand would leave it set
            // and the next (possibly a named variable) would be mis-registered and double-freed.
            // Each owned-string operand that is a call result is a temporary the comparison only
            // borrows; register it for end-of-statement cleanup (see RegisterBorrowedStringOperandTemp).
            // An owning-POINTER operand (`makePtr() != nullptr`) is the same shape: the comparison
            // yields a bool, so the pointer cannot escape and the temp is freed at the same point.
            Compiler(ctx)->lastCallReturnsOwned = false;
            auto lv = ParseTypeCheckExpression(nextCtxs[0]);
            RegisterBorrowedStringOperandTemp(Compiler(ctx), lv.value);
            Compiler(ctx)->RegisterOwnedPtrTemp(lv.value);
            Compiler(ctx)->lastCallReturnsOwned = false;
            auto rv = ParseTypeCheckExpression(nextCtxs[1]);
            RegisterBorrowedStringOperandTemp(Compiler(ctx), rv.value);
            Compiler(ctx)->RegisterOwnedPtrTemp(rv.value);
            std::string op = ctx->children[1]->getText();

            // An interface value compared against nullptr tests its DATA pointer (fat-ptr field 1):
            // a failed `as <Interface>` yields a zeroed fat pointer, so `f != nullptr` is the
            // hit/miss test. Reduce the fat operand to its data pointer and compare pointers.
            LowerInterfaceNullCompare(ctx, lv, rv);

            auto* overload = TryBinaryOperatorOverload(lv, op, rv, ctx, lv.elemType);
            llvm::Value* result = overload ? overload
                                           : Compiler(ctx)->CreateOperation(op, lv, rv, lv.isUnsigned, rv.isUnsigned);
            return { result, false };  // == != result is bool, not unsigned
        }

        LogErrorContext(ctx, "Equality expression has unexpected operand count.");
        return {};
    }

LLVMBackend::TypedValue MainListener::TypedValueOfNamedOperand(LLVMBackend::NamedVariable& namedVar,
                                                     antlr4::ParserRuleContext* ctx) {
        bool isUnsigned = namedVar.TypeAndValue.IsUnsignedInteger() != -1;
        llvm::Type* elemType = nullptr;
        if (namedVar.TypeAndValue.Pointer)
        {
            auto elemTV = namedVar.TypeAndValue;
            elemTV.ElemPointer ? (elemTV.ElemPointer = false) : (elemTV.Pointer = false, elemTV.IsInterfacePointer = false);
            elemType = Compiler(ctx)->GetType(elemTV);
        }
        LLVMBackend::TypedValue result{ LoadNamedVariable(namedVar), isUnsigned };
        result.elemType = elemType;
        result.isArrayView = namedVar.TypeAndValue.IsArrayView;
        return result;
    }

CFlatParser::CastExpressionContext* MainListener::SoleCastOperandOf(CFlatParser::RelationalExpressionContext* relCtx) {
        if (relCtx == nullptr) return nullptr;
        auto shifts = relCtx->shiftExpression();
        if (shifts.size() != 1) return nullptr;
        auto adds = shifts[0]->additiveExpression();
        if (adds.size() != 1) return nullptr;
        auto muls = adds[0]->multiplicativeExpression();
        if (muls.size() != 1) return nullptr;
        auto casts = muls[0]->castExpression();
        return casts.size() == 1 ? casts[0] : nullptr;
    }

LLVMBackend::TypedValue MainListener::ParseTypeCheckExpression(CFlatParser::TypeCheckExpressionContext* ctx) {
        auto relCtx = ctx->relationalExpression();
        if (!relCtx)
        {
            LogErrorContext(ctx, "Type check expression has no operand.");
            return {};
        }

        // An 'is'/'as' operand is parsed through the cast level when it is a plain passthrough,
        // so the source binding survives; every other shape keeps today's relational path.
        auto typeSpecs = ctx->typeSpecifier();
        LLVMBackend::NamedVariable srcNV;
        const LLVMBackend::NamedVariable* srcBinding = nullptr;
        LLVMBackend::TypedValue tv;
        auto* castOperand = typeSpecs.empty() ? nullptr : SoleCastOperandOf(relCtx);
        if (castOperand != nullptr)
        {
            srcNV = ParseCastExpression(castOperand);
            tv = TypedValueOfNamedOperand(srcNV, castOperand);
            srcBinding = &srcNV;
        }
        else
        {
            tv = ParseRelationalExpression(relCtx);
        }
        llvm::Value* result = tv.value;
        llvm::Type* srcElemType = tv.elemType;

        if (typeSpecs.size() > 0)
        {
            for (size_t i = 0; i < typeSpecs.size(); i++)
            {
                std::string op = ctx->children[2 * i + 1]->getText();  // 'is' or 'as' token
                std::string targetTypeName = ParseTypeSpecifierName(typeSpecs[i]);

                // Name the source when a binding survived, so ClassifyCastSource can fill in
                // shape.TypeName for an interface-valued source (an LLVM fat pointer carries no
                // interface identity, so the name can only come from here).
                std::string srcTypeName = srcBinding != nullptr ? srcBinding->TypeAndValue.TypeName
                                                                : std::string{};
                if (op == "is")
                {
                    result = GenerateIsCheck(result, targetTypeName, ctx, srcElemType, srcTypeName);
                }
                else if (op == "as")
                {
                    result = GenerateSafeCast(result, targetTypeName, ctx, srcElemType, srcBinding,
                                              srcTypeName);
                }
                // Only the first operand's static type is known here; a chained is/as
                // operates on the previous op's result, whose element type isn't tracked.
                srcElemType = nullptr;
                srcBinding = nullptr;
            }
            return { result, false };  // is/as result is bool or pointer, not unsigned
        }

        return tv;
    }

llvm::Value* MainListener::LoadTypeDescFromInterface(llvm::Value* interfaceValue, antlr4::ParserRuleContext* ctx) {
        auto* compiler = Compiler(ctx);
        auto ptrTy = compiler->builder->getInt8Ty()->getPointerTo();

        llvm::Value* vtablePtr;
        if (interfaceValue->getType()->isStructTy())
        {
            vtablePtr = compiler->builder->CreateExtractValue(interfaceValue, {0u});
        }
        else
        {
            auto fatTy = compiler->GetFatPtrType();
            auto vtablePtrField = compiler->builder->CreateStructGEP(fatTy, interfaceValue, 0);
            vtablePtr = compiler->builder->CreateLoad(ptrTy, vtablePtrField);
        }

        // vtable[0] holds the type descriptor pointer
        auto typeDescField = compiler->builder->CreateGEP(ptrTy, vtablePtr, compiler->builder->getInt32(0));
        return compiler->builder->CreateLoad(ptrTy, typeDescField);
    }

std::string MainListener::ConcreteStructNameFromElemType(llvm::Type* elemType, LLVMBackend* compiler) {
        auto* st = elemType ? llvm::dyn_cast<llvm::StructType>(elemType) : nullptr;
        if (!st || !st->hasName()) return "";
        std::string name = st->getName().str();
        return compiler->dataStructures.count(name) ? name : "";
    }

std::string MainListener::ConcreteStructNameFromValue(llvm::Value* value, LLVMBackend* compiler) {
        auto* st = value ? llvm::dyn_cast<llvm::StructType>(value->getType()) : nullptr;
        if (!st || !st->hasName()) return "";
        std::string name = st->getName().str();
        return compiler->dataStructures.count(name) ? name : "";
    }

bool MainListener::ClassifyPointerShapedSource(llvm::Value* value, llvm::Type* elemType, LLVMBackend* compiler,
                                     LLVMBackend::TypeAndValue& shape) {
        llvm::Type* valueType = value->getType();
        llvm::ArrayType* arrayType = llvm::dyn_cast<llvm::ArrayType>(valueType);
        if (arrayType == nullptr && valueType->isPointerTy())
        {
            if (auto* gep = llvm::dyn_cast<llvm::GEPOperator>(value))
                arrayType = llvm::dyn_cast<llvm::ArrayType>(gep->getSourceElementType());
            else if (auto* global = llvm::dyn_cast<llvm::GlobalVariable>(value))
                arrayType = llvm::dyn_cast<llvm::ArrayType>(global->getValueType());
        }
        if (arrayType != nullptr)
        {
            // Unwrap inner dimensions to reach the class; the reported size stays the outer one.
            llvm::Type* inner = arrayType->getElementType();
            while (auto* nested = llvm::dyn_cast<llvm::ArrayType>(inner)) inner = nested->getElementType();
            std::string elem = ConcreteStructNameFromElemType(inner, compiler);
            // An array OF POINTERS names no class this way; fall through to the declared type.
            if (!elem.empty())
            {
                shape.TypeName = elem;
                shape.ConstArraySize = arrayType->getNumElements();
                return true;
            }
        }

        // Shapes no llvm type can name: a `T**`, and a global whose array ELEMENT is itself a
        // pointer. Both come from the binding's declared type instead.
        const llvm::Value* storage = nullptr;
        if (llvm::isa<llvm::GlobalVariable>(value)) storage = value;
        else if (auto* load = llvm::dyn_cast<llvm::LoadInst>(value))
        {
            // Gated on a POINTER elemType, which a plain `T*` and an array VIEW are not - both
            // were already classified as concrete pointers above.
            if (elemType != nullptr && elemType->isPointerTy()) storage = load->getPointerOperand();
        }
        if (storage != nullptr)
        {
            const auto* declared = compiler->FindDeclaredTypeAndValueForStorage(storage);
            // A REGISTERED class or a BUILTIN primitive element: both are named types whose
            // boxability is decidable, and both spellings must reject a primitive array in step.
            bool namedElement = declared != nullptr
                && (compiler->dataStructures.count(declared->TypeName)
                    || LLVMBackend::IsPrimitiveTypeName(compiler->ResolveTypeAlias(declared->TypeName)));
            if (namedElement
                && !compiler->DescribePointerShapedInterfaceSource(*declared).empty())
            {
                shape = *declared;  // copy out now; the pointer aliases live map storage
                return true;
            }
        }
        return false;
    }

MainListener::CastSourceKind MainListener::ClassifyCastSource(llvm::Value* value, llvm::Type* elemType, LLVMBackend* compiler,
                                      std::string& structName, LLVMBackend::TypeAndValue& shape,
                                      const std::string& srcTypeName) {
        structName.clear();
        shape = LLVMBackend::TypeAndValue{};
        if (value == nullptr) return CastSourceKind::Unknown;
        llvm::Type* valueType = value->getType();
        auto* fatTy = compiler->GetFatPtrType();

        // A genuine interface value: the fat aggregate itself, or a pointer to one. `shape` is
        // populated on BOTH arms - it used to be left default-constructed here, which silently gave
        // every caller an empty TypeName for the entire interface-valued input class.
        if (valueType == fatTy || (valueType->isPointerTy() && elemType == fatTy))
        {
            shape.TypeName = srcTypeName;   // empty when the caller could not name the source
            shape.IsInterface = true;
            shape.Pointer = (valueType != fatTy);
            return CastSourceKind::InterfaceValue;
        }

        structName = ConcreteStructNameFromElemType(elemType, compiler);
        if (!structName.empty()) return CastSourceKind::ConcretePointer;
        structName = ConcreteStructNameFromValue(value, compiler);
        if (!structName.empty()) return CastSourceKind::ConcreteValue;

        // An `arr[i]` element access is NOT pointer-shaped: it carries a class elemType and was
        // already classified as a concrete pointer above.
        if (ClassifyPointerShapedSource(value, elemType, compiler, shape))
            return CastSourceKind::PointerShaped;

        // A '?:' join of pointers carries no elemType; each arm has its own concrete class.
        if (valueType->isPointerTy())
            if (auto* phi = llvm::dyn_cast<llvm::PHINode>(value))
                if (phi->getNumIncomingValues() > 0) return CastSourceKind::TernaryPointerJoin;

        return CastSourceKind::Unknown;
    }

bool MainListener::ResolveTernaryArmClasses(llvm::Value* value, LLVMBackend* compiler,
                                  std::vector<std::string>& armTypes, std::string& failure) {
        auto* phi = llvm::cast<llvm::PHINode>(value);
        armTypes.assign(phi->getNumIncomingValues(), std::string());
        for (unsigned i = 0; i < phi->getNumIncomingValues(); i++)
        {
            llvm::Value* incoming = phi->getIncomingValue(i);
            if (llvm::isa<llvm::ConstantPointerNull>(incoming)) continue;
            armTypes[i] = compiler->ResolvePointerElementTypeName(incoming);
            if (armTypes[i].empty())
            {
                failure = "the arm's concrete class cannot be determined; bind the arm to a "
                          "local variable of the class type first";
                return false;
            }
        }
        return true;
    }

llvm::Value* MainListener::JoinTernaryArmPredicates(llvm::Value* value, const std::vector<bool>& answers,
                                          LLVMBackend* compiler) {
        // No arms means nothing can match; test this first, or the all-quantifiers below are
        // both vacuously true and an empty join would answer TRUE.
        if (answers.empty()) return compiler->builder->getInt1(false);
        bool allTrue = true, allFalse = true;
        for (bool a : answers) { if (a) allFalse = false; else allTrue = false; }
        if (allTrue) return compiler->builder->getInt1(true);
        if (allFalse) return compiler->builder->getInt1(false);

        auto* phi = llvm::cast<llvm::PHINode>(value);
        auto* savedBlock = compiler->builder->GetInsertBlock();
        auto savedPoint = compiler->builder->GetInsertPoint();
        compiler->builder->SetInsertPoint(phi);
        auto* answerPhi = compiler->builder->CreatePHI(compiler->builder->getInt1Ty(),
                                                       phi->getNumIncomingValues(), "ternary_is");
        for (unsigned i = 0; i < phi->getNumIncomingValues(); i++)
            answerPhi->addIncoming(compiler->builder->getInt1(answers[i]), phi->getIncomingBlock(i));
        compiler->builder->SetInsertPoint(savedBlock, savedPoint);
        return answerPhi;
    }

llvm::Value* MainListener::AddressOfClassValueOperand(llvm::Value* value, LLVMBackend* compiler) {
        if (auto* load = llvm::dyn_cast<llvm::LoadInst>(value))
            return load->getPointerOperand();
        auto* slot = compiler->CreateAlloca(value->getType());
        compiler->CreateAssignment(value, slot);
        return slot;
    }

llvm::Value* MainListener::GenerateIsCheck(llvm::Value* interfaceValue, const std::string& targetTypeNameIn,
                                  antlr4::ParserRuleContext* ctx, llvm::Type* srcElemType,
                                  const std::string& srcTypeNameIn) {
        auto* compiler = Compiler(ctx);
        // targetTypeName is LOAD-BEARING: every direct interfaceTable.find/count below used to
        // miss an aliased spelling that IsInterfaceType (which resolves aliases) would accept -
        // that was the bug (see internal/issue p1, now closed). targetTypeNameIn (unresolved) is
        // kept around and used in every LogErrorContext message below, so a rejected 'AliasIA'
        // is reported as 'AliasIA', not as the interface it resolves to.
        //
        // srcTypeName's resolution here is DEFENSIVE, not load-bearing: srcTypeNameIn always
        // arrives already resolved (the caller reads it off a NamedVariable.TypeAndValue.TypeName,
        // which ParseDeclarationSpecifiers resolves at declaration time), so this is a no-op today.
        // Kept so a future caller that stops pre-resolving does not silently reintroduce the same
        // asymmetry on the source side - Test/test_interface.cb's iface_alias_is_source /
        // iface_alias_as_source legs are a tripwire for exactly that, not current-bug coverage.
        std::string targetTypeName = compiler->ResolveTypeAlias(targetTypeNameIn);
        std::string srcTypeName = compiler->ResolveTypeAlias(srcTypeNameIn);

        std::string srcStructName;
        LLVMBackend::TypeAndValue srcShape;
        CastSourceKind srcKind = ClassifyCastSource(interfaceValue, srcElemType, compiler,
                                                    srcStructName, srcShape, srcTypeName);

        // An unrouted generic-interface SOURCE has no method table, so its type-descriptor slot is
        // null and dereferencing it crashes. Only the SOURCE: as a TARGET the name is never
        // dereferenced (the answer is a pure implementor lookup, correctly false), and rejecting it
        // there is a false rejection - see scratch/rev4/p/t13_iscls.cb.
        compiler->RecordInterfaceMaterialization(srcShape.TypeName, "the source of 'is'");

        // Interface target: shared helper, shared wording. Guard on its bool exactly as the other
        // call sites do, so control cannot reach the concrete-target message if it stopped throwing.
        if (srcKind == CastSourceKind::PointerShaped)
        {
            // Membership gates on the resolved name; the message spells what the user wrote.
            if (compiler->interfaceTable.count(targetTypeName)
                && RejectPointerShapedInterfaceUpcast(ctx, srcShape, targetTypeNameIn))
                return nullptr;
            LogErrorContext(ctx, std::format(
                "'is' needs an interface value or a single class instance, not '{}'",
                compiler->DescribePointerShapedInterfaceSource(srcShape)));
            return nullptr;
        }

        // A '?:' join has one concrete class per arm, so the answer is per arm too.
        if (srcKind == CastSourceKind::TernaryPointerJoin)
        {
            std::vector<std::string> armTypes;
            std::string failure;
            if (!ResolveTernaryArmClasses(interfaceValue, compiler, armTypes, failure))
            {
                LogErrorContext(ctx, std::format("cannot test '?:' arm against '{}': {}",
                                                 targetTypeNameIn, failure));
                return nullptr;
            }
            bool targetIsInterface = compiler->interfaceTable.count(targetTypeName) != 0;
            if (!targetIsInterface && !compiler->dataStructures.count(targetTypeName))
            {
                LogErrorContext(ctx, std::format("'{}' is not a known struct or interface type for 'is' check", targetTypeNameIn));
                return nullptr;
            }
            std::vector<bool> answers(armTypes.size(), false);
            for (size_t i = 0; i < armTypes.size(); i++)
            {
                if (armTypes[i].empty()) continue;
                answers[i] = targetIsInterface
                    ? compiler->StructImplementsInterface(armTypes[i], targetTypeName)
                    : armTypes[i] == targetTypeName;
            }
            return JoinTernaryArmPredicates(interfaceValue, answers, compiler);
        }

        if (srcKind == CastSourceKind::Unknown)
        {
            LogErrorContext(ctx, std::format(
                "'is' requires an interface value or a class instance on the left of '{}'; this "
                "expression is neither", targetTypeNameIn));
            return nullptr;
        }

        // Concrete source, pointer (e.g. `w is IW` where w is W*) or stack value (`s is IW`),
        // not a boxed interface value: no vtable/typedesc header to load, so the answer is
        // known at compile time.
        if (srcKind == CastSourceKind::ConcretePointer || srcKind == CastSourceKind::ConcreteValue)
        {
            if (compiler->interfaceTable.count(targetTypeName))
                return compiler->builder->getInt1(compiler->StructImplementsInterface(srcStructName, targetTypeName));
            if (compiler->dataStructures.count(targetTypeName))
                return compiler->builder->getInt1(srcStructName == targetTypeName);

            LogErrorContext(ctx, std::format("'{}' is not a known struct or interface type for 'is' check", targetTypeNameIn));
            return nullptr;
        }

        // Interface target: true when the runtime type is any implementor of that interface.
        // An OR-chain of typedesc compares - the same enumeration `switch` on an interface uses.
        if (compiler->interfaceTable.count(targetTypeName))
        {
            auto loadedDesc = LoadTypeDescFromInterface(interfaceValue, ctx);
            llvm::Value* result = compiler->builder->getInt1(false);
            for (auto& [sName, sd] : compiler->dataStructures)
            {
                if (!sd.typeDescriptor || !compiler->StructImplementsInterface(sName, targetTypeName)) continue;
                auto* cmp = compiler->builder->CreateICmpEQ(loadedDesc, sd.typeDescriptor);
                result = compiler->builder->CreateOr(result, cmp);
            }
            for (auto& [pName, pd] : compiler->programTable)
            {
                if (!pd.typeDescriptor || !compiler->StructImplementsInterface(pName, targetTypeName)) continue;
                auto* cmp = compiler->builder->CreateICmpEQ(loadedDesc, pd.typeDescriptor);
                result = compiler->builder->CreateOr(result, cmp);
            }
            return result;
        }

        auto targetIt = compiler->dataStructures.find(targetTypeName);
        if (targetIt == compiler->dataStructures.end())
        {
            LogErrorContext(ctx, std::format("'{}' is not a known struct type for 'is' check", targetTypeNameIn));
            return nullptr;
        }

        auto* typeDesc = targetIt->second.typeDescriptor;
        if (!typeDesc)
        {
            LogErrorContext(ctx, std::format("'{}' has no type descriptor", targetTypeNameIn));
            return nullptr;
        }

        auto loadedDesc = LoadTypeDescFromInterface(interfaceValue, ctx);
        return compiler->builder->CreateICmpEQ(loadedDesc, typeDesc);
    }

llvm::Value* MainListener::GenerateSafeCast(llvm::Value* interfaceValue, const std::string& targetTypeNameIn,
                                  antlr4::ParserRuleContext* ctx, llvm::Type* srcElemType,
                                  const LLVMBackend::NamedVariable* srcBinding,
                                  const std::string& srcTypeNameIn) {
        auto* compiler = Compiler(ctx);
        auto ptrTy = compiler->builder->getInt8Ty()->getPointerTo();
        // targetTypeName resolution is LOAD-BEARING (see the matching comment in
        // GenerateIsCheck); targetTypeNameIn (unresolved) is kept for every message below so a
        // rejected alias is reported by the name the user wrote. srcTypeName's resolution is
        // DEFENSIVE only - srcTypeNameIn always arrives pre-resolved already.
        std::string targetTypeName = compiler->ResolveTypeAlias(targetTypeNameIn);
        std::string srcTypeName = compiler->ResolveTypeAlias(srcTypeNameIn);

        std::string srcStructName;
        LLVMBackend::TypeAndValue srcShape;
        CastSourceKind srcKind = ClassifyCastSource(interfaceValue, srcElemType, compiler,
                                                    srcStructName, srcShape, srcTypeName);

        // An unrouted generic-interface SOURCE has no method table, so its type-descriptor slot is
        // null and dereferencing it crashes. Only the SOURCE: as a TARGET the name is never
        // dereferenced (the answer is a pure implementor lookup, correctly false), and rejecting it
        // there is a false rejection - see scratch/rev4/p/t13_iscls.cb.
        compiler->RecordInterfaceMaterialization(srcShape.TypeName, "the source of 'as'");
        bool srcIsValue = srcKind == CastSourceKind::ConcreteValue;

        // Not one object: same rejection, same wording, as the plain-assignment spelling. Guarded
        // on the helper's bool like every other call site (see its comment for why).
        if (srcKind == CastSourceKind::PointerShaped)
        {
            // Membership gates on the resolved name; the message spells what the user wrote.
            if (compiler->interfaceTable.count(targetTypeName)
                && RejectPointerShapedInterfaceUpcast(ctx, srcShape, targetTypeNameIn))
                return nullptr;
            LogErrorContext(ctx, std::format(
                "cannot cast '{}' to '{}' - index or dereference it first to get a single instance",
                compiler->DescribePointerShapedInterfaceSource(srcShape), targetTypeNameIn));
            return nullptr;
        }

        // A '?:' join carries no single concrete class: box each arm in its own block, exactly
        // as the plain-assignment path does.
        if (srcKind == CastSourceKind::TernaryPointerJoin)
        {
            if (compiler->interfaceTable.count(targetTypeName))
            {
                std::string armFailure;
                if (auto* fat = UpcastTernaryPhiToInterface(interfaceValue, targetTypeName, &armFailure))
                    return fat;
                LogErrorContext(ctx, armFailure.empty()
                    ? std::format("cannot convert '?:' result to interface '{}'", targetTypeNameIn)
                    : std::format("cannot convert '?:' arm to interface '{}': {}", targetTypeNameIn, armFailure));
                return nullptr;
            }
            LogErrorContext(ctx, std::format(
                "cannot cast a '?:' result to '{}'; 'as' performs a runtime-checked downcast only "
                "from an interface value - bind the '?:' to a local first", targetTypeNameIn));
            return nullptr;
        }

        if (srcKind == CastSourceKind::Unknown)
        {
            LogErrorContext(ctx, std::format(
                "'as' requires an interface value or a class instance on the left of '{}'; this "
                "expression is neither", targetTypeNameIn));
            return nullptr;
        }

        // CONCRETE source, pointer (e.g. `w as X` where w is W*) or stack value (`s as X`).
        // A concrete object carries no vtable/typedesc header, so every target is resolved
        // statically here; this branch always returns, keeping the runtime-checked downcast
        // below interface-source-only.
        if (srcKind == CastSourceKind::ConcretePointer || srcKind == CastSourceKind::ConcreteValue)
        {
            // Interface target: the shared boxing helper, so this spelling carries the same
            // implements/shape/ownership guards the plain-assignment spelling does.
            if (compiler->interfaceTable.count(targetTypeName))
            {
                return BoxConcreteIntoInterface(ctx, interfaceValue, !srcIsValue, srcStructName,
                                                targetTypeName, srcBinding);
            }

            if (compiler->dataStructures.count(targetTypeName))
            {
                // Same concrete type: identity cast, the pointer is already what was asked for.
                if (srcStructName == targetTypeName) return interfaceValue;

                // Different concrete type. There is no inheritance between concrete classes and
                // no runtime type info on the object, so this can never be a checked downcast.
                LogErrorContext(ctx, std::format(
                    "cannot cast '{}' to unrelated type '{}'; 'as' performs a runtime-checked downcast only from an interface value",
                    srcStructName, targetTypeNameIn));
                return nullptr;
            }

            LogErrorContext(ctx, std::format("'{}' is not a known struct or interface type for 'as' cast", targetTypeNameIn));
            return nullptr;
        }

        // Only InterfaceValue reaches here, so the source carries the vtable + typedesc header
        // the checks below load from. Interface target: runtime-checked interface-to-interface.
        if (compiler->interfaceTable.count(targetTypeName))
        {
            auto fatTy = compiler->GetFatPtrType();

            // Extract dataPtr from source fat pointer
            llvm::Value* dataPtr;
            if (interfaceValue->getType()->isStructTy())
                dataPtr = compiler->builder->CreateExtractValue(interfaceValue, {1u});
            else
            {
                auto dp = compiler->builder->CreateStructGEP(fatTy, interfaceValue, 1);
                dataPtr = compiler->builder->CreateLoad(ptrTy, dp);
            }

            // Load type descriptor from source
            llvm::Value* loadedDesc = LoadTypeDescFromInterface(interfaceValue, ctx);

            // Alloca for result: defaults to null fat ptr (aggregate zero)
            auto* resultAlloca = compiler->CreateAlloca(fatTy);
            compiler->builder->CreateStore(llvm::ConstantAggregateZero::get(fatTy), resultAlloca);

            auto* afterBlock = compiler->CreateBasicBlock("as_iface_after");

            // For each class that implements the target interface,
            // check if the concrete type matches and build the appropriate fat pointer
            for (auto& [sName, sd] : compiler->dataStructures)
            {
                if (!compiler->StructImplementsInterface(sName, targetTypeName)) continue;
                if (!sd.typeDescriptor) continue;

                auto* matchBlock = compiler->CreateBasicBlock("as_iface_match");
                auto* nextBlock = compiler->CreateBasicBlock("as_iface_next");

                auto* cmp = compiler->builder->CreateICmpEQ(loadedDesc, sd.typeDescriptor);
                compiler->builder->CreateCondBr(cmp, matchBlock, nextBlock);

                compiler->SwitchToBlock(matchBlock);
                auto* vtable = compiler->GetOrCreateVTable(sName, targetTypeName);
                auto fatVal = compiler->BuildInterfaceFatValue(vtable, dataPtr);
                compiler->builder->CreateStore(fatVal, resultAlloca);
                compiler->builder->CreateBr(afterBlock);

                compiler->SwitchToBlock(nextBlock);
            }
            // Also check programs that implement the target interface
            for (auto& [pName, pd] : compiler->programTable)
            {
                if (!compiler->StructImplementsInterface(pName, targetTypeName)) continue;
                if (!pd.typeDescriptor) continue;

                auto* matchBlock = compiler->CreateBasicBlock("as_iface_match");
                auto* nextBlock = compiler->CreateBasicBlock("as_iface_next");

                auto* cmp = compiler->builder->CreateICmpEQ(loadedDesc, pd.typeDescriptor);
                compiler->builder->CreateCondBr(cmp, matchBlock, nextBlock);

                compiler->SwitchToBlock(matchBlock);
                auto* vtable = compiler->GetOrCreateVTable(pName, targetTypeName);
                auto fatVal = compiler->BuildInterfaceFatValue(vtable, dataPtr);
                compiler->builder->CreateStore(fatVal, resultAlloca);
                compiler->builder->CreateBr(afterBlock);

                compiler->SwitchToBlock(nextBlock);
            }

            // No match: fall through (result stays null fat ptr)
            compiler->builder->CreateBr(afterBlock);
            compiler->SwitchToBlock(afterBlock);

            // Return the fat pointer value (aggregate)
            return compiler->builder->CreateLoad(fatTy, resultAlloca);
        }

        // Concrete target from an interface source: the runtime-checked downcast.
        auto targetIt = compiler->dataStructures.find(targetTypeName);
        if (targetIt == compiler->dataStructures.end())
        {
            LogErrorContext(ctx, std::format("'{}' is not a known struct or interface type for 'as' cast", targetTypeNameIn));
            return nullptr;
        }

        auto* typeDesc = targetIt->second.typeDescriptor;
        if (!typeDesc)
        {
            LogErrorContext(ctx, std::format("'{}' has no type descriptor", targetTypeNameIn));
            return nullptr;
        }

        // Extract data pointer (field 1)
        llvm::Value* dataPtr;
        if (interfaceValue->getType()->isStructTy())
            dataPtr = compiler->builder->CreateExtractValue(interfaceValue, {1u});
        else
        {
            auto fatTy = compiler->GetFatPtrType();
            auto dataPtrField = compiler->builder->CreateStructGEP(fatTy, interfaceValue, 1);
            dataPtr = compiler->builder->CreateLoad(ptrTy, dataPtrField);
        }

        auto loadedDesc = LoadTypeDescFromInterface(interfaceValue, ctx);
        auto typeMatches = compiler->builder->CreateICmpEQ(loadedDesc, typeDesc);

        // In opaque pointer mode, dataPtr is already the right pointer type - no bitcast needed
        auto nullPtr = llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(dataPtr->getType()));
        return compiler->builder->CreateSelect(typeMatches, dataPtr, nullPtr);
    }

LLVMBackend::TypedValue MainListener::ParseRelationalExpression(CFlatParser::RelationalExpressionContext* ctx) {
        auto nextCtxs = ctx->shiftExpression();
        if (nextCtxs.size() == 1)
        {
            return ParseShiftExpression(nextCtxs[0]);
        }
        else if (nextCtxs.size() == 2)
        {
            // See ParseEqualityExpression: a string operand returned from a call (e.g.
            // `s < other.toString()`) is an owned temp the relational operator only borrows.
            // Reset lastCallReturnsOwned before each operand and register each call-result
            // operand for end-of-statement cleanup (see RegisterBorrowedStringOperandTemp).
            // Owning-POINTER operands are registered here too - see ParseEqualityExpression.
            Compiler(ctx)->lastCallReturnsOwned = false;
            auto lv = ParseShiftExpression(nextCtxs[0]);
            RegisterBorrowedStringOperandTemp(Compiler(ctx), lv.value);
            Compiler(ctx)->RegisterOwnedPtrTemp(lv.value);
            Compiler(ctx)->lastCallReturnsOwned = false;
            auto rv = ParseShiftExpression(nextCtxs[1]);
            RegisterBorrowedStringOperandTemp(Compiler(ctx), rv.value);
            Compiler(ctx)->RegisterOwnedPtrTemp(rv.value);
            std::string op = ctx->children[1]->getText();

            auto* overload = TryBinaryOperatorOverload(lv, op, rv, ctx, lv.elemType);
            llvm::Value* result = overload ? overload
                                           : Compiler(ctx)->CreateOperation(op, lv, rv, lv.isUnsigned, rv.isUnsigned);
            return { result, false };  // comparison result is bool, not unsigned
        }

        LogErrorContext(ctx, "Relational expression has unexpected operand count.");
        return {};
    }

std::string MainListener::TryGetSimpleIdentifier(antlr4::ParserRuleContext* ctx) {
        if (!ctx) return "";
        auto& children = ctx->children;
        if (children.size() == 1)
        {
            if (auto* term = dynamic_cast<antlr4::tree::TerminalNode*>(children[0]))
            {
                if (term->getSymbol()->getType() == CFlatLexer::Identifier)
                    return term->getText();
            }
            if (auto* child = dynamic_cast<antlr4::ParserRuleContext*>(children[0]))
                return TryGetSimpleIdentifier(child);
        }
        return "";
    }

llvm::Function* MainListener::FindStreamMethodFn(LLVMBackend* compiler, const std::string& methodName) {
        auto it = compiler->functionTable.find(methodName);
        if (it == compiler->functionTable.end()) return nullptr;
        for (const auto& sym : it->second)
        {
            if (!sym.Parameters.empty() && sym.Parameters[0].TypeName == "stream")
                return sym.Function;
        }
        return nullptr;
    }

void MainListener::EmitProgramToStreamWire(const std::string& progName,
        llvm::Value* progStorage, llvm::Value* streamStorage,
        antlr4::ParserRuleContext* ctx) {
        auto* compiler = Compiler(ctx);
        auto* i8PtrTy  = compiler->builder->getInt8Ty()->getPointerTo();

        auto* writeBytesFn = FindStreamMethodFn(compiler, "write_bytes");
        if (!writeBytesFn)
        {
            LogErrorContext(ctx, "stream::write_bytes not found - import \"stream.cb\" before using >>.");
            return;
        }

        // Create/reuse __stream_write_shim(char* data, i32 len, i8* env) - casts env to stream*, calls write_bytes.
        // Env is the trailing param (closure ABI is env-last). The hook is non-owning: printf passes a
        // thread-local buffer; the shim copies it via write_bytes.
        llvm::Function* shim = compiler->module->getFunction("__stream_write_shim");
        if (!shim)
        {
            auto* charPtrTy = i8PtrTy;  // char* and i8* are the same in LLVM
            auto* i32Ty     = llvm::Type::getInt32Ty(*compiler->context);
            auto* shimTy    = llvm::FunctionType::get(
                compiler->builder->getVoidTy(), {charPtrTy, i32Ty, i8PtrTy}, false);
            shim = llvm::Function::Create(shimTy, llvm::Function::InternalLinkage,
                "__stream_write_shim", *compiler->module);
            auto* bb = llvm::BasicBlock::Create(*compiler->context, "entry", shim);
            llvm::IRBuilder<> b(bb);
            auto* streamTy = compiler->GetDataStructure("stream").StructType;
            auto* selfPtr  = b.CreateBitCast(shim->getArg(2), streamTy->getPointerTo(), "stream_self");
            b.CreateCall(writeBytesFn->getFunctionType(), writeBytesFn,
                {selfPtr, shim->getArg(0), shim->getArg(1)});
            b.CreateRetVoid();
        }

        // Build fat closure {shim_i8*, stream_i8*} and store into prog.onStdout.
        auto* shimI8 = compiler->builder->CreateBitCast(shim, i8PtrTy, "write_shim_i8");
        auto* envI8  = compiler->builder->CreateBitCast(streamStorage, i8PtrTy, "stream_env_i8");
        auto* fatTy  = compiler->GetClosureFatPtrType();
        llvm::Value* fat = llvm::UndefValue::get(fatTy);
        fat = compiler->builder->CreateInsertValue(fat, shimI8, {0u});
        fat = compiler->builder->CreateInsertValue(fat, envI8,  {1u});

        auto& pd  = compiler->programTable[progName];
        auto* gep = compiler->builder->CreateStructGEP(
            pd.StructType, progStorage, pd.OnStdoutFieldIndex, "on_stdout_gep");
        compiler->builder->CreateStore(fat, gep);

        // Also store the stream pointer into _out so programs can call _out.write_bytes() directly.
        if (pd.OutFieldIndex != (unsigned)-1)
        {
            auto* outGep = compiler->builder->CreateStructGEP(
                pd.StructType, progStorage, pd.OutFieldIndex, "out_gep");
            compiler->builder->CreateStore(streamStorage, outGep);
        }
    }

void MainListener::EmitStreamToProgramWire(llvm::Value* streamStorage,
        const std::string& progName, llvm::Value* progStorage,
        antlr4::ParserRuleContext* ctx) {
        auto* compiler = Compiler(ctx);
        auto* i8PtrTy  = compiler->builder->getInt8Ty()->getPointerTo();

        auto* readFn = FindStreamMethodFn(compiler, "read");
        if (!readFn)
        {
            LogErrorContext(ctx, "stream::read not found - import \"stream.cb\" before using >>.");
            return;
        }

        // Create/reuse __stream_read_shim(i8* env) -> char* - casts env to stream*, calls read.
        llvm::Function* shim = compiler->module->getFunction("__stream_read_shim");
        if (!shim)
        {
            auto* charPtrTy = i8PtrTy;
            auto* shimTy    = llvm::FunctionType::get(charPtrTy, {i8PtrTy}, false);
            shim = llvm::Function::Create(shimTy, llvm::Function::InternalLinkage,
                "__stream_read_shim", *compiler->module);
            auto* bb = llvm::BasicBlock::Create(*compiler->context, "entry", shim);
            llvm::IRBuilder<> b(bb);
            auto* streamTy = compiler->GetDataStructure("stream").StructType;
            auto* selfPtr  = b.CreateBitCast(shim->getArg(0), streamTy->getPointerTo(), "stream_self");
            auto* result   = b.CreateCall(readFn->getFunctionType(), readFn, {selfPtr}, "line");
            b.CreateRet(result);
        }

        auto* shimI8 = compiler->builder->CreateBitCast(shim, i8PtrTy, "read_shim_i8");
        auto* envI8  = compiler->builder->CreateBitCast(streamStorage, i8PtrTy, "stream_env_i8");
        auto* fatTy  = compiler->GetClosureFatPtrType();
        llvm::Value* fat = llvm::UndefValue::get(fatTy);
        fat = compiler->builder->CreateInsertValue(fat, shimI8, {0u});
        fat = compiler->builder->CreateInsertValue(fat, envI8,  {1u});

        auto& pd  = compiler->programTable[progName];
        auto* gep = compiler->builder->CreateStructGEP(
            pd.StructType, progStorage, pd.OnStdinFieldIndex, "on_stdin_gep");
        compiler->builder->CreateStore(fat, gep);

        // Wire return_buffer: when the consumer is done with a buffer, return it to the pool.
        auto* returnFn = FindStreamMethodFn(compiler, "return_buffer");
        if (returnFn)
        {
            // Create/reuse __stream_return_buffer_shim(char* buf, i8* env) - calls stream.return_buffer.
            // Env is the trailing param (closure ABI is env-last).
            llvm::Function* returnShim = compiler->module->getFunction("__stream_return_buffer_shim");
            if (!returnShim)
            {
                auto* charPtrTy = i8PtrTy;
                auto* shimTy    = llvm::FunctionType::get(
                    compiler->builder->getVoidTy(), {charPtrTy, i8PtrTy}, false);
                returnShim = llvm::Function::Create(shimTy, llvm::Function::InternalLinkage,
                    "__stream_return_buffer_shim", *compiler->module);
                auto* bb = llvm::BasicBlock::Create(*compiler->context, "entry", returnShim);
                llvm::IRBuilder<> b(bb);
                auto* streamTy = compiler->GetDataStructure("stream").StructType;
                auto* selfPtr  = b.CreateBitCast(returnShim->getArg(1), streamTy->getPointerTo(), "stream_self");
                b.CreateCall(returnFn->getFunctionType(), returnFn, {selfPtr, returnShim->getArg(0)});
                b.CreateRetVoid();
            }

            auto* returnShimI8 = compiler->builder->CreateBitCast(returnShim, i8PtrTy, "return_shim_i8");
            auto* returnEnvI8  = compiler->builder->CreateBitCast(streamStorage, i8PtrTy, "stream_env_i8_ret");
            llvm::Value* returnFat = llvm::UndefValue::get(fatTy);
            returnFat = compiler->builder->CreateInsertValue(returnFat, returnShimI8, {0u});
            returnFat = compiler->builder->CreateInsertValue(returnFat, returnEnvI8,  {1u});

            auto* retGep = compiler->builder->CreateStructGEP(
                pd.StructType, progStorage, pd.OnStdinReturnFieldIndex, "on_stdin_return_gep");
            compiler->builder->CreateStore(returnFat, retGep);
        }

        // Also store the stream pointer into _in so programs can call _in.read_buf() directly.
        if (pd.InStreamFieldIndex != (unsigned)-1)
        {
            auto* inGep = compiler->builder->CreateStructGEP(
                pd.StructType, progStorage, pd.InStreamFieldIndex, "in_gep");
            compiler->builder->CreateStore(streamStorage, inGep);
        }
    }

llvm::Value* MainListener::EmitArenaChannelShellAlloc(LLVMBackend* compiler) {
        auto dsIt = compiler->dataStructures.find(kArenaChannelType);
        if (dsIt == compiler->dataStructures.end()) return nullptr;
        auto* arenaTy    = dsIt->second.StructType;
        auto* arenaPtrTy = arenaTy->getPointerTo();
        auto* ctorFn     = compiler->GetFunction(kArenaChannelType);
        auto* mallocFn   = compiler->GetFunction("malloc");
        if (!ctorFn || !mallocFn) return llvm::Constant::getNullValue(arenaPtrTy);

        auto* arenaSize = compiler->GetTypeSizeBytes(arenaTy);
        auto* arenaRaw  = compiler->builder->CreateCall(
            mallocFn->getFunctionType(), mallocFn, {arenaSize}, "arena_shell_raw");
        auto* arenaPtr  = compiler->builder->CreateBitCast(arenaRaw, arenaPtrTy, "arena_shell_ptr");
        auto* arenaZero = compiler->builder->CreateCall(
            ctorFn->getFunctionType(), ctorFn, {}, "arena_shell_zero");
        compiler->builder->CreateStore(arenaZero, arenaPtr);
        return arenaPtr;
    }

void MainListener::EmitProgramToProgramArenaWire(const std::string& producerName, llvm::Value* producerStorage,
        const std::string& consumerName, llvm::Value* consumerStorage,
        antlr4::ParserRuleContext* ctx) {
        auto* compiler = Compiler(ctx);
        auto& producerPd = compiler->programTable[producerName];
        auto& consumerPd = compiler->programTable[consumerName];

        if (consumerPd.InboxArenaFieldIndex == (unsigned)-1 || producerPd.OutboxFieldIndex == (unsigned)-1)
        {
            LogErrorContext(ctx,
                "program-to-program '>>' requires arena_channel (import \"arena_channel.cb\").");
            return;
        }

        auto dsIt = compiler->dataStructures.find(kArenaChannelType);
        if (dsIt == compiler->dataStructures.end())
        {
            LogErrorContext(ctx, "arena_channel type not found for program '>>'.");
            return;
        }
        auto* arenaTy    = dsIt->second.StructType;
        auto* arenaPtrTy = arenaTy->getPointerTo();

        // Select the no-arg init() overload (idempotent; init(int, i64) also exists).
        llvm::Function* initFn = nullptr;
        if (auto it = compiler->functionTable.find("init"); it != compiler->functionTable.end())
            for (const auto& sym : it->second)
                if (sym.Parameters.size() == 1 && sym.Parameters[0].TypeName == kArenaChannelType)
                    { initFn = sym.Function; break; }

        if (!initFn)
        {
            LogErrorContext(ctx, "arena_channel init not found for program '>>'.");
            return;
        }

        // The consumer's inbox is a non-null shell allocated in its ctor. init() upgrades it to a
        // live ring (no-op if a previous `>>` already wired it - the fan-in case).
        auto* inboxGep = compiler->builder->CreateStructGEP(
            consumerPd.StructType, consumerStorage, consumerPd.InboxArenaFieldIndex, "inbox_gep");
        auto* inboxPtr = compiler->builder->CreateLoad(arenaPtrTy, inboxGep, "inbox_ptr");
        compiler->builder->CreateCall(initFn->getFunctionType(), initFn, {inboxPtr});

        // Rebind producer.outbox (its own self-loopback shell) to the consumer's live inbox.
        auto* outboxGep = compiler->builder->CreateStructGEP(
            producerPd.StructType, producerStorage, producerPd.OutboxFieldIndex, "outbox_gep");
        compiler->builder->CreateStore(inboxPtr, outboxGep);
    }

void MainListener::EmitProgramToProgramStreamWire(const std::string& producerName, llvm::Value* producerStorage,
        const std::string& consumerName, llvm::Value* consumerStorage,
        antlr4::ParserRuleContext* ctx) {
        auto* compiler = Compiler(ctx);
        auto streamDsIt = compiler->dataStructures.find("stream");
        if (streamDsIt == compiler->dataStructures.end()) return;  // stream.cb not imported - arena-only
        auto& streamDs = streamDsIt->second;

        auto* streamTy = streamDs.StructType;
        auto* ctorFn   = compiler->GetFunction("stream");
        auto* initFn   = FindStreamMethodFn(compiler, "init");
        if (!ctorFn || !initFn)
        {
            LogErrorContext(ctx, "stream ctor/init not found for program>>program piping - import \"stream.cb\".");
            return;
        }

        // Hidden stream local: alloca at entry, default-construct, init().
        auto* streamStorage = compiler->AllocaAtEntry(streamTy, nullptr, "pipe_stream");
        auto* zeroVal = compiler->builder->CreateCall(ctorFn->getFunctionType(), ctorFn, {}, "pipe_stream_zero");
        compiler->builder->CreateStore(zeroVal, streamStorage);
        compiler->builder->CreateCall(initFn->getFunctionType(), initFn, {streamStorage});

        // Mark _autoClose = true so the producer trampoline closes it after main() returns.
        unsigned autoCloseIdx = (unsigned)-1;
        const auto& fields = streamDs.StructFields;
        for (unsigned i = 0; i < fields.size(); ++i)
            if (fields[i].VariableName == "_autoClose") { autoCloseIdx = i; break; }
        if (autoCloseIdx != (unsigned)-1)
        {
            auto* acGep    = compiler->builder->CreateStructGEP(streamTy, streamStorage, autoCloseIdx, "autoclose_gep");
            auto* acElemTy = streamTy->getStructElementType(autoCloseIdx);
            compiler->builder->CreateStore(llvm::ConstantInt::get(acElemTy, 1), acGep);
        }

        // Register the hidden stream for scope-exit destruction (frees both buffers + the CVs).
        if (!compiler->stackNamedVariable.empty())
        {
            LLVMBackend::NamedVariable nv;
            nv.TypeAndValue.TypeName = "stream";
            nv.TypeAndValue.Pointer  = false;
            nv.Storage               = streamStorage;
            compiler->stackNamedVariable.back().namedVariable[
                "__pipe_stream_" + std::to_string(compiler->pipeStreamCounter++)] = nv;
        }

        // Reuse the explicit-form wiring for both directions.
        EmitProgramToStreamWire(producerName, producerStorage, streamStorage, ctx);
        EmitStreamToProgramWire(streamStorage, consumerName, consumerStorage, ctx);
    }

bool MainListener::HasOperatorOverloadForFirstParam(const std::string& opName, const std::string& typeName) {
        auto* compiler = Compiler();
        auto it = compiler->functionTable.find(opName);
        if (it == compiler->functionTable.end()) return false;
        for (const auto& sym : it->second)
            if (!sym.Parameters.empty() && sym.Parameters[0].TypeName == typeName)
                return true;
        return false;
    }

LLVMBackend::TypedValue MainListener::ParseShiftExpression(CFlatParser::ShiftExpressionContext* ctx) {
        auto nextCtxs = ctx->additiveExpression();
        if (nextCtxs.size() == 1)
        {
            return ParseAdditiveExpression(nextCtxs[0]);
        }
        else if (nextCtxs.size() == 2)
        {
            auto lv = ParseAdditiveExpression(nextCtxs[0]);
            auto rv = ParseAdditiveExpression(nextCtxs[1]);
            // '>>' is two tokens in the grammar (('>' '>')), so children[1] = '>' and children[2] = '>'.
            // '<<' is a single token, so children[1] = '<<'.
            std::string op = ctx->children[1]->getText();
            if (op == ">" && ctx->children.size() > 2 && ctx->children[2]->getText() == ">")
                op = ">>";

            auto* compiler = Compiler(ctx);
            std::string lhsName = TryGetSimpleIdentifier(nextCtxs[0]);
            std::string rhsName = TryGetSimpleIdentifier(nextCtxs[1]);

            auto lhsNV = lhsName.empty() ? LLVMBackend::NamedVariable{} : compiler->GetLocalVariable(lhsName);
            if (!lhsName.empty() && lhsNV.Storage == nullptr)
                lhsNV = compiler->GetGlobalVariableNV(lhsName);
            auto rhsNV = rhsName.empty() ? LLVMBackend::NamedVariable{} : compiler->GetLocalVariable(rhsName);
            if (!rhsName.empty() && rhsNV.Storage == nullptr)
                rhsNV = compiler->GetGlobalVariableNV(rhsName);

            const std::string& lhsType = lhsNV.TypeAndValue.TypeName;
            const std::string& rhsType = rhsNV.TypeAndValue.TypeName;

            if (op == ">>")
            {
                llvm::Value* lhsStorage = lhsNV.Storage;
                llvm::Value* rhsStorage = rhsNV.Storage;

                bool lhsIsProgram = !lhsType.empty() && compiler->programTable.count(lhsType) > 0;
                bool rhsIsProgram = !rhsType.empty() && compiler->programTable.count(rhsType) > 0;
                bool lhsIsStream  = lhsType == "stream";
                bool rhsIsStream  = rhsType == "stream";

                if (lhsIsProgram && rhsIsProgram && lhsStorage && rhsStorage)
                {
                    // Direct program>>program (the design's "stream always wired, channel additive"):
                    // the stdout->stdin stream is ALWAYS wired (auto-synthesized + auto-closed); the
                    // rich arena_channel is wired only when BOTH programs opted in via useChannel = true
                    // (a runtime branch, since `>>` runs once). useChannel defaults to 0, so programs
                    // that only pipe stdout pay nothing for an arena_channel they never touch.
                    EmitProgramToProgramStreamWire(lhsType, lhsStorage, rhsType, rhsStorage, ctx);

                    auto& lpd = compiler->programTable[lhsType];
                    auto& rpd = compiler->programTable[rhsType];
                    auto* i32Ty = llvm::Type::getInt32Ty(*compiler->context);
                    auto* lUseGEP = compiler->builder->CreateStructGEP(
                        lpd.StructType, lhsStorage, lpd.UseChannelFieldIndex, "l_usechannel_gep");
                    auto* rUseGEP = compiler->builder->CreateStructGEP(
                        rpd.StructType, rhsStorage, rpd.UseChannelFieldIndex, "r_usechannel_gep");
                    auto* lOn = compiler->builder->CreateICmpNE(
                        compiler->builder->CreateLoad(i32Ty, lUseGEP, "l_usechannel"),
                        llvm::ConstantInt::get(i32Ty, 0), "l_usechannel_on");
                    auto* rOn = compiler->builder->CreateICmpNE(
                        compiler->builder->CreateLoad(i32Ty, rUseGEP, "r_usechannel"),
                        llvm::ConstantInt::get(i32Ty, 0), "r_usechannel_on");
                    auto* bothOn = compiler->builder->CreateAnd(lOn, rOn, "usechannel_both");

                    auto* curFn   = compiler->builder->GetInsertBlock()->getParent();
                    auto* wireBB  = llvm::BasicBlock::Create(*compiler->context, "arena_wire",  curFn);
                    auto* afterBB = llvm::BasicBlock::Create(*compiler->context, "arena_after", curFn);
                    compiler->builder->CreateCondBr(bothOn, wireBB, afterBB);

                    compiler->builder->SetInsertPoint(wireBB);
                    EmitProgramToProgramArenaWire(lhsType, lhsStorage, rhsType, rhsStorage, ctx);
                    compiler->builder->CreateBr(afterBB);

                    compiler->builder->SetInsertPoint(afterBB);
                    return rv;  // return consumer so `a >> b >> c` could chain later
                }
                if (lhsIsProgram && rhsIsStream && lhsStorage && rhsStorage)
                {
                    EmitProgramToStreamWire(lhsType, lhsStorage, rhsStorage, ctx);
                    return rv;  // return stream value so `p1 >> s >> p2` can chain
                }
                if (lhsIsStream && rhsIsProgram && rhsStorage)
                {
                    llvm::Value* streamPtr = lhsStorage;
                    if (!streamPtr)
                    {
                        // Spill loaded value (chain case: `(p1 >> s) >> p2` produces a loaded stream)
                        auto* streamTy = compiler->GetDataStructure("stream").StructType;
                        streamPtr = compiler->AllocaAtEntry(streamTy, nullptr, "stream_spill");
                        compiler->builder->CreateStore(lv.value, streamPtr);
                    }
                    EmitStreamToProgramWire(streamPtr, rhsType, rhsStorage, ctx);
                    return rv;  // return program value
                }
            }

            // General operator overloading for '>>' / '<<' (e.g. channel<T>::operator>>).
            // Only dispatched when the LHS is a struct/class type with a matching operator
            // overload, so primitive integer bit-shifts fall through to CreateOperation.
            if (op == ">>" || op == "<<")
            {
                std::string opName = "operator" + op;
                if (!lhsType.empty() && compiler->IsDataStructure(lhsType)
                    && HasOperatorOverloadForFirstParam(opName, lhsType))
                {
                    LLVMBackend::NamedVariable la;
                    la.TypeAndValue = lhsNV.TypeAndValue;
                    la.TypeAndValue.VariableName = "";   // positional, not a named arg
                    la.Primary      = lv.value;
                    la.BaseType     = lv.value ? lv.value->getType() : nullptr;
                    la.CallerName   = lhsName;
                    LLVMBackend::NamedVariable ra;
                    ra.TypeAndValue = rhsNV.TypeAndValue;
                    ra.TypeAndValue.VariableName = "";   // positional, not a named arg
                    ra.Primary      = rv.value;
                    ra.BaseType     = rv.value ? rv.value->getType() : nullptr;
                    ra.CallerName   = rhsName;
                    auto* res = compiler->CreateOverloadedFunctionCall(opName, { la, ra });
                    return { res, false };
                }
            }

            auto result = compiler->CreateOperation(op, lv, rv, lv.isUnsigned, rv.isUnsigned);
            return { result, false };
        }

        LogErrorContext(ctx, "Shift expression has no operands.");
        return {};
    }

LLVMBackend::TypedValue MainListener::ParseAdditiveExpression(CFlatParser::AdditiveExpressionContext* ctx) {
        auto nextCtxs = ctx->multiplicativeExpression();

        if (nextCtxs.size() == 1)
        {
            return ParseMultiplicativeExpression(nextCtxs[0]);
        }
        else if (nextCtxs.size() > 1)
        {
            // Clear lastCallReturnsOwned before each operand: a preceding owned operator+ result would
            // otherwise leave the flag set and mis-register the next plain-variable operand (double-free).
            Compiler(ctx)->lastCallReturnsOwned = false;
            auto lv = ParseMultiplicativeExpression(nextCtxs[0]);
            llvm::Value* lvalue = lv.value;
            bool lu = lv.isUnsigned;
            llvm::Type* elemType = lv.elemType;
            TrackOwnedStringOperatorResult(Compiler(ctx), lvalue);
            // String concat (+) only borrows its operands: a plain (non-move) string-returning
            // call leaves lastCallReturnsOwned false, so TrackOwnedStringOperatorResult skips it.
            // Register call-result string operands directly so they are freed at end-of-statement.
            RegisterBorrowedStringOperandTemp(Compiler(ctx), lvalue);

            for (size_t i = 1; i < nextCtxs.size(); i++)
            {
                Compiler(ctx)->lastCallReturnsOwned = false;
                auto rv = ParseMultiplicativeExpression(nextCtxs[i]);
                llvm::Value* rvalue = rv.value;
                bool ru = rv.isUnsigned;
                TrackOwnedStringOperatorResult(Compiler(ctx), rvalue);
                RegisterBorrowedStringOperandTemp(Compiler(ctx), rvalue);
                std::string op = ctx->children[i * 2 - 1]->getText();

                if (lvalue->getType()->isPointerTy() && rvalue->getType()->isPointerTy() && op == "-")
                {
                    // ptr - ptr -> element count (C ptrdiff_t semantics)
                    auto* i64Ty = Compiler(ctx)->builder->getInt64Ty();
                    auto* byteDiff = Compiler(ctx)->builder->CreateSub(
                        Compiler(ctx)->builder->CreatePtrToInt(lvalue, i64Ty),
                        Compiler(ctx)->builder->CreatePtrToInt(rvalue, i64Ty),
                        "ptrdiff");
                    if (elemType)
                    {
                        auto* elemSize = Compiler(ctx)->GetTypeSizeBytes(elemType);
                        lvalue = Compiler(ctx)->builder->CreateSDiv(byteDiff, elemSize, "ptrdiff_elem");
                    }
                    else
                    {
                        lvalue = byteDiff;
                    }
                    elemType = nullptr;
                }
                else if (elemType && lvalue->getType()->isPointerTy()
                    && rvalue && rvalue->getType()->isIntegerTy()
                    && (op == "+" || op == "-"))
                {
                    if (lv.isArrayView || rv.isArrayView)
                        LogErrorContext(ctx, "pointer arithmetic is not allowed on an array-view 'T[]' - index it with 'a[i]' instead "
                            "(the view spans a whole allocation; arithmetic would create an aliasing pointer the noalias contract forbids)");
                    // Pointer arithmetic: ptr + int / ptr - int -> GEP
                    if (op == "-")
                        rvalue = Compiler(ctx)->builder->CreateNeg(rvalue, "neg");
                    lvalue = Compiler(ctx)->CreateGEP(elemType, lvalue, rvalue, "ptrarith");
                    // elemType stays the same - result is still a pointer to the same element type
                }
                else
                {
                    auto* overload = TryBinaryOperatorOverload(lvalue, op, rvalue, ctx);

                    // char* + char* concatenation: TryBinaryOperatorOverload dispatches off a struct lvalue
                    // and can't reach raw i8*; both must qualify as c-strings so int* + int* still errors.
                    auto isCStr = [&](llvm::Value* v, llvm::Type* et) -> bool {
                        if (et && et->isIntegerTy(8)) return true;
                        if (auto* c = llvm::dyn_cast<llvm::Constant>(v))
                            return Compiler(ctx)->stringLiteralLenByPtr.count(c) > 0;
                        return false;
                    };
                    if (overload == nullptr && op == "+"
                        && lvalue->getType()->isPointerTy() && rvalue->getType()->isPointerTy()
                        && isCStr(lvalue, elemType) && isCStr(rvalue, rv.elemType))
                    {
                        LLVMBackend::NamedVariable leftNV;
                        leftNV.Primary  = lvalue;
                        leftNV.BaseType = lvalue->getType();
                        leftNV.TypeAndValue.TypeName = "char";
                        leftNV.TypeAndValue.Pointer  = true;
                        LLVMBackend::NamedVariable rightNV;
                        rightNV.Primary  = rvalue;
                        rightNV.BaseType = rvalue->getType();
                        rightNV.TypeAndValue.TypeName = "char";
                        rightNV.TypeAndValue.Pointer  = true;
                        overload = Compiler(ctx)->CreateOverloadedFunctionCall("operator+", { leftNV, rightNV });
                        // Owned result: track it as an end-of-expression temporary so a
                        // bare `a + b` or a chained intermediate is freed (it is unregistered
                        // when bound to a named local / moved). See string-concat-temp-flush.md.
                        TrackOwnedStringOperatorResult(Compiler(ctx), overload);
                    }

                    lvalue = overload ? overload : Compiler(ctx)->CreateOperation(op, lvalue, rvalue, lu, ru);
                    lu = lu || ru;
                    elemType = nullptr;  // arithmetic result is no longer a pointer
                }
            }

            LLVMBackend::TypedValue result{ lvalue, lu };
            result.elemType = elemType;
            return result;
        }

        LogErrorContext(ctx, "Additive expression has no operands.");
        return {};
    }

void MainListener::VerifyFuncPtrAssignmentMoveFlags(const std::string& funcName,
                                          const LLVMBackend::TypeAndValue& funcPtrType,
                                          antlr4::ParserRuleContext* ctx) {
        if (!funcPtrType.IsFunctionPointer) return;
        // No move modifiers anywhere -> nothing to enforce.
        bool anyMove = false;
        for (const auto& p : funcPtrType.FuncPtrParams) if (p.IsMove) { anyMove = true; break; }
        if (!anyMove)
        {
            // Even when destination has no move flags, reject if every overload only has move params.
            // We only flag when the function table has *no* matching all-borrow overload.
        }
        if (!Compiler(ctx)->HasFunctionWithMoveFlags(funcName, funcPtrType.FuncPtrParams))
        {
            std::string expected;
            for (size_t i = 0; i < funcPtrType.FuncPtrParams.size(); i++)
            {
                if (i) expected += ", ";
                if (funcPtrType.FuncPtrParams[i].IsMove) expected += "move ";
                expected += funcPtrType.FuncPtrParams[i].TypeName;
                if (funcPtrType.FuncPtrParams[i].Pointer) expected += "*";
            }
            Compiler(ctx)->LogError(std::format(
                "function '{}' has no overload matching the 'move' modifiers of function pointer signature ({}) - 'move' is part of the function-pointer type and must agree on both sides",
                funcName, expected));
        }
    }

llvm::Value* MainListener::TagViewElementAccess(llvm::Value* loaded, const LLVMBackend::NamedVariable& namedVar) {
        if (namedVar.TypeAndValue.NoaliasScopeId >= 0)
            if (auto* inst = llvm::dyn_cast_or_null<llvm::Instruction>(loaded))
                Compiler()->AttachViewNoalias(inst, namedVar.TypeAndValue.NoaliasScopeId);
        return loaded;
    }

LLVMBackend::NamedVariable MainListener::LowerSpanElementAccess(
        antlr4::ParserRuleContext* ctx,
        const LLVMBackend::NamedVariable& structVar,
        llvm::Value* indexValue) {
        LLVMBackend::NamedVariable elem;
        if (indexValue == nullptr || structVar.Storage == nullptr
            || structVar.TypeAndValue.Pointer
            || !structVar.BaseType || !structVar.BaseType->isStructTy())
            return elem;

        auto* compiler = Compiler(ctx);
        int bufIndex = compiler->ArrayViewBufferFieldIndex(structVar.TypeAndValue.TypeName);
        if (bufIndex < 0)
            return elem;

        // Load the `_ptr` array-view field (the T* buffer).
        const auto& spanDS = compiler->GetDataStructure(structVar.TypeAndValue.TypeName);
        auto* bufGEP = compiler->CreateStructGEP(structVar.BaseType, structVar.Storage, (uint32_t)bufIndex);
        llvm::Value* bufPtr = compiler->CreateLoad(bufGEP);

        // Element TypeAndValue = the array-view field with the view-ness dropped (an indexed element
        // is a single slot, never a whole-allocation view), mirroring the IsArrayView subscript
        // branch in ParsePostfix. Key the alias scope to the receiver origin so distinct spans land
        // in distinct scopes; an unnamed (temporary) receiver yields scope -1 = no metadata.
        LLVMBackend::TypeAndValue elementTV = spanDS.StructFields[bufIndex];
        std::string originKey = structVar.TypeAndValue.VariableName;
        if (originKey.empty()) originKey = structVar.CallerName;
        int scopeId = compiler->GetOrMintViewScope(originKey);
        elementTV.IsArrayView = false;
        if (elementTV.ElemPointer)
            elementTV.ElemPointer = false;
        else
        {
            elementTV.Pointer = false;
            elementTV.IsInterfacePointer = false;
        }
        auto* elementType = compiler->GetType(elementTV);
        elementTV.NoaliasScopeId = scopeId;

        elem.Storage = compiler->CreateGEP(elementType, bufPtr, indexValue);
        elem.BaseType = elementType;
        elem.TypeAndValue = elementTV;
        return elem;
    }

llvm::Value* MainListener::LoadNamedVariable(LLVMBackend::NamedVariable& namedVar) {
        auto* compiler = Compiler();
        llvm::Value* result = LoadNamedVariableImpl(namedVar);
        if (result != nullptr && namedVar.TypeAndValue.IsFatInterfaceValue()
            && !namedVar.TypeAndValue.Pointer)
            compiler->RegisterFatInterfaceValueTypeName(result, namedVar.TypeAndValue.TypeName);
        // Ledger a CODE value by the same value identity (see codeValues_). Synchronous check:
        // namedVar is not a stamped argument, so ask under the ambient occurrence.
        if (result != nullptr && compiler->ArgumentIsCodeValue(namedVar, compiler->CurrentCastOccurrence()))
            compiler->RegisterCodeValue(result);
        // Mirror ledger for the closure-widen gate, which asks the opposite question: a join of
        // values PROVEN to be data must not widen into a fat closure's code slot (see dataValues_).
        if (result != nullptr && compiler->ArgumentIsDataValue(namedVar))
            compiler->RegisterDataValue(result);
        return result;
    }

llvm::Value* MainListener::LoadNamedVariableImpl(LLVMBackend::NamedVariable& namedVar) {
        auto* compiler = Compiler();
        // Every value read of a named local. A dereference site records its Deref event BEFORE
        // calling this, so the read below cannot swallow its own check (see RecordNullRead).
        compiler->RecordNullRead(namedVar.CallerName);
        if (namedVar.IdentifierLine > 0)
        {
            if (!namedVar.IsElementAccess)
                compiler->RecordMoveUse(namedVar.CallerName, namedVar.FieldName,
                                        namedVar.IdentifierLine, namedVar.IdentifierColumn);
            if (auto moved = compiler->MovedUseSubject(namedVar); !moved.empty())
            {
                compiler->currentLine = namedVar.IdentifierLine;
                compiler->currentColumn = namedVar.IdentifierColumn;
                compiler->LogError(std::format("use of moved variable '{}'", moved));
            }
        }
        if (namedVar.TypeAndValue.Pointer)
        {
            if (namedVar.Primary != nullptr)
                return namedVar.Primary;
            if (namedVar.Storage != nullptr)
            {
                // Pointer array decay: char*[3] used as char** -> GEP to first element.
                // Must check before the alloca load path below.
                if (namedVar.BaseType && llvm::isa<llvm::ArrayType>(namedVar.BaseType))
                {
                    auto* arrTy = llvm::cast<llvm::ArrayType>(namedVar.BaseType);
                    auto* zero = compiler->builder->getInt64(0);
                    return compiler->builder->CreateGEP(arrTy, namedVar.Storage, {zero, zero}, "arrptr");
                }
                if (llvm::isa<llvm::AllocaInst>(namedVar.Storage) ||
                    llvm::isa<llvm::GlobalVariable>(namedVar.Storage) ||
                    llvm::isa<llvm::GetElementPtrInst>(namedVar.Storage))
                    return TagViewElementAccess(compiler->CreateLoad(namedVar.Storage), namedVar);
                // Through-pointer dereference of a pointer type (e.g. *pp where pp is char**):
                // Storage is a raw loaded address - load again to get the pointer value.
                if (namedVar.BaseType && namedVar.BaseType->isPointerTy())
                    return TagViewElementAccess(compiler->CreateLoad(namedVar.BaseType, namedVar.Storage), namedVar);
                return namedVar.Storage;
            }
            return nullptr;
        }
        else
        {
            if (namedVar.Primary != nullptr)
                return namedVar.Primary;

            if (namedVar.Storage != nullptr)
            {
                // Array-to-pointer decay: fixed-size array used as a value decays to pointer to first element
                if (namedVar.BaseType && llvm::isa<llvm::ArrayType>(namedVar.BaseType))
                {
                    auto* arrTy = llvm::cast<llvm::ArrayType>(namedVar.BaseType);
                    auto* zero = compiler->builder->getInt64(0);
                    return compiler->builder->CreateGEP(arrTy, namedVar.Storage, {zero, zero}, "arrptr");
                }
                // For through-pointer dereferences (Storage is a raw loaded ptr, not an alloca/gep/global),
                // use BaseType to emit the correctly-typed load (opaque pointers carry no type info).
                if (namedVar.BaseType
                    && !llvm::isa<llvm::AllocaInst>(namedVar.Storage)
                    && !llvm::isa<llvm::GlobalVariable>(namedVar.Storage)
                    && !llvm::isa<llvm::GetElementPtrInst>(namedVar.Storage))
                    return TagViewElementAccess(compiler->CreateLoad(namedVar.BaseType, namedVar.Storage), namedVar);
                return TagViewElementAccess(compiler->CreateLoad(namedVar.Storage), namedVar);
            }
        }

        // namedVar is empty - caller's block was already terminated (e.g. by a return-block inline).
        return nullptr;
    }

llvm::Value* MainListener::TryUnaryOperatorOverload(
        llvm::Value* operand, const std::string& op,
        antlr4::ParserRuleContext* ctx) {
        if (!operand) return nullptr;
        auto* compiler = Compiler(ctx);

        auto* ty = operand->getType();
        if (!ty->isStructTy()) return nullptr;
        auto* structTy = llvm::cast<llvm::StructType>(ty);
        if (structTy->isLiteral() || !structTy->hasName()) return nullptr;
        std::string typeName = structTy->getName().str();
        if (typeName == "__iface_fat_ptr") return nullptr;

        std::string opName = "operator" + op;
        if (!compiler->GetFunction(opName)) return nullptr;

        // Determine whether to pass the operand by pointer or by value.
        bool usePointer = false;
        {
            auto funcSym = compiler->functionTable.find(opName);
            if (funcSym != compiler->functionTable.end())
            {
                for (const auto& candidate : funcSym->second)
                {
                    if (!candidate.Parameters.empty()
                        && candidate.Parameters[0].TypeName == typeName
                        && candidate.Parameters[0].Pointer)
                    {
                        usePointer = true;
                        break;
                    }
                }
            }
        }

        if (usePointer)
        {
            auto* tempAlloca = compiler->CreateAlloca(structTy);
            compiler->CreateAssignment(operand, tempAlloca);

            LLVMBackend::NamedVariable thisNV;
            thisNV.TypeAndValue.TypeName = typeName;
            thisNV.TypeAndValue.Pointer  = true;
            thisNV.Primary = tempAlloca;

            return compiler->CreateOverloadedFunctionCall(opName, { thisNV });
        }
        else
        {
            LLVMBackend::NamedVariable thisNV;
            thisNV.TypeAndValue.TypeName = typeName;
            thisNV.TypeAndValue.Pointer  = false;
            thisNV.Primary  = operand;
            thisNV.BaseType = structTy;

            return compiler->CreateOverloadedFunctionCall(opName, { thisNV });
        }
    }

void MainListener::TrackOwnedStringOperatorResult(LLVMBackend* compiler, llvm::Value* result) {
        if (result == nullptr || !compiler->lastCallReturnsOwned) return;
        auto* strTy = llvm::StructType::getTypeByName(*compiler->context, "string");
        if (strTy != nullptr && result->getType() == strTy)
            compiler->RegisterOwnedStringTemp(result);
    }

void MainListener::RegisterBorrowedStringOperandTemp(LLVMBackend* compiler, llvm::Value* operand) {
        if (operand == nullptr || !llvm::isa<llvm::CallInst>(operand)) return;
        auto* strTy = llvm::StructType::getTypeByName(*compiler->context, "string");
        if (strTy != nullptr && operand->getType() == strTy)
            compiler->RegisterOwnedStringTemp(operand);
    }

bool MainListener::IsComparisonOperatorText(const std::string& op) {
        return op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=";
    }

llvm::Value* MainListener::TryPointerLhsOperatorOverload(
        llvm::Value* lvalue, const std::string& op, llvm::Value* rvalue,
        antlr4::ParserRuleContext* ctx, llvm::Type* lhsElemType) {
        auto* compiler = Compiler(ctx);

        // LOAD-BEARING: the RHS must NOT be pointer-typed. Both-pointer operands (including
        // `p == nullptr`) always stay on the builtin address comparison and never dispatch.
        if (!rvalue || rvalue->getType()->isPointerTy()) return nullptr;
        if (!IsComparisonOperatorText(op)) return nullptr;

        std::string opName = "operator" + op;
        std::string pointee = ConcreteStructNameFromElemType(lhsElemType, compiler);

        if (!pointee.empty() && compiler->GetFunction(opName))
        {
            auto funcSym = compiler->functionTable.find(opName);
            if (funcSym != compiler->functionTable.end())
            {
                for (const auto& candidate : funcSym->second)
                {
                    if (candidate.Parameters.empty()) continue;
                    if (candidate.Parameters[0].TypeName != pointee || !candidate.Parameters[0].Pointer) continue;

                    LLVMBackend::NamedVariable thisNV;
                    thisNV.TypeAndValue.TypeName = pointee;
                    thisNV.TypeAndValue.Pointer  = true;
                    thisNV.Primary  = lvalue;
                    thisNV.BaseType = lhsElemType;

                    LLVMBackend::NamedVariable rightNV;
                    rightNV.Primary  = rvalue;
                    rightNV.BaseType = rvalue->getType();
                    if (auto* rst = llvm::dyn_cast<llvm::StructType>(rvalue->getType()))
                        if (!rst->isLiteral() && rst->hasName())
                            rightNV.TypeAndValue.TypeName = rst->getName().str();

                    auto* result = compiler->CreateOverloadedFunctionCall(opName, { thisNV, rightNV });
                    TrackOwnedStringOperatorResult(compiler, result);
                    return result;
                }
            }
        }

        // A struct-valued RHS can never reach a valid builtin comparison against a pointer,
        // so name the failure here instead of emitting a malformed icmp.
        if (rvalue->getType()->isStructTy())
        {
            std::string lhsName = pointee.empty() ? "pointer" : pointee + "*";
            auto* rst = llvm::cast<llvm::StructType>(rvalue->getType());
            std::string rhsName = (!rst->isLiteral() && rst->hasName()) ? rst->getName().str() : "struct";
            LogErrorContext(ctx, std::format(
                "no overload of '{}' matches operands '{}' and '{}'; overloaded operators dispatch on a struct value "
                "or struct pointer left operand with a matching declared overload", opName, lhsName, rhsName));
            return nullptr;
        }

        return nullptr;
    }

llvm::Value* MainListener::TryBinaryOperatorOverload(
        llvm::Value* lvalue, const std::string& op, llvm::Value* rvalue,
        antlr4::ParserRuleContext* ctx, llvm::Type* lhsElemType) {
        if (!lvalue) return nullptr;
        auto* compiler = Compiler(ctx);

        // If the LHS is a string literal (ptr to global constant), wrap it
        // as a %string struct so operator+(string, ...) can match.
        if (lvalue->getType()->isPointerTy())
        {
            if (auto* c = llvm::dyn_cast<llvm::Constant>(lvalue))
            {
                if (compiler->stringLiteralLenByPtr.count(c))
                    lvalue = compiler->WrapStringLiteralAsString(lvalue);
            }
        }

        auto* ty = lvalue->getType();

        if (ty->isPointerTy())
            return TryPointerLhsOperatorOverload(lvalue, op, rvalue, ctx, lhsElemType);

        if (!ty->isStructTy()) return nullptr;
        auto* structTy = llvm::cast<llvm::StructType>(ty);
        if (structTy->isLiteral() || !structTy->hasName()) return nullptr;
        std::string typeName = structTy->getName().str();
        // Fat-ptr types compare by extracted field - let CreateOperation handle it.
        if (typeName == "__iface_fat_ptr" || typeName == "__closure_fat_ptr") return nullptr;

        std::string opName = "operator" + op;
        if (!compiler->GetFunction(opName)) return nullptr;

        auto makeRightNV = [&]() {
            LLVMBackend::NamedVariable rightNV;
            rightNV.Primary  = rvalue;
            rightNV.BaseType = rvalue ? rvalue->getType() : nullptr;
            if (rvalue && rvalue->getType()->isStructTy())
            {
                auto* rst = llvm::cast<llvm::StructType>(rvalue->getType());
                if (!rst->isLiteral() && rst->hasName())
                    rightNV.TypeAndValue.TypeName = rst->getName().str();
            }
            else if (rvalue && rvalue->getType()->isPointerTy())
            {
                // If the RHS is a string literal (ptr to global constant), wrap it
                // as a %string struct so operator+(string, string) can match.
                if (auto* c = llvm::dyn_cast<llvm::Constant>(rvalue))
                {
                    if (compiler->stringLiteralLenByPtr.count(c))
                    {
                        rightNV.Primary  = compiler->WrapStringLiteralAsString(rvalue);
                        rightNV.BaseType = rightNV.Primary->getType();
                        rightNV.TypeAndValue.TypeName = "string";
                    }
                }
            }
            return rightNV;
        };

        // Determine whether to pass lvalue by pointer or by value by inspecting the
        // registered candidates - check if any candidate's first param is a pointer to
        // this struct type. If so use pointer dispatch; otherwise use value dispatch.
        bool usePointer = false;
        {
            auto funcSym = compiler->functionTable.find(opName);
            if (funcSym != compiler->functionTable.end())
            {
                for (const auto& candidate : funcSym->second)
                {
                    if (!candidate.Parameters.empty()
                        && candidate.Parameters[0].TypeName == typeName
                        && candidate.Parameters[0].Pointer)
                    {
                        usePointer = true;
                        break;
                    }
                }
            }
        }

        if (usePointer)
        {
            // By-pointer dispatch: conventional user-defined struct operators (T* this).
            auto* tempAlloca = compiler->CreateAlloca(structTy);
            compiler->CreateAssignment(lvalue, tempAlloca);

            LLVMBackend::NamedVariable thisNV;
            thisNV.TypeAndValue.TypeName = typeName;
            thisNV.TypeAndValue.Pointer  = true;
            thisNV.Primary = tempAlloca;

            auto* result = compiler->CreateOverloadedFunctionCall(opName, { thisNV, makeRightNV() });
            TrackOwnedStringOperatorResult(compiler, result);
            return result;
        }
        else
        {
            // By-value dispatch: built-in value types like string whose operators
            // are defined as operator+(T a, ...) rather than operator+(T* a, ...).
            LLVMBackend::NamedVariable thisNV;
            thisNV.TypeAndValue.TypeName = typeName;
            thisNV.TypeAndValue.Pointer  = false;
            thisNV.Primary  = lvalue;
            thisNV.BaseType = structTy;

            auto* result = compiler->CreateOverloadedFunctionCall(opName, { thisNV, makeRightNV() });
            TrackOwnedStringOperatorResult(compiler, result);
            return result;
        }

        return nullptr;
    }

LLVMBackend::TypedValue MainListener::ParseMultiplicativeExpression(CFlatParser::MultiplicativeExpressionContext* ctx) {
        auto nextCtxs = ctx->castExpression();

        if (nextCtxs.size() == 1)
        {
            auto namedVar = ParseCastExpression(nextCtxs[0]);
            return TypedValueOfNamedOperand(namedVar, nextCtxs[0]);
        }
        else if (nextCtxs.size() > 1)
        {
            auto firstNV = ParseCastExpression(nextCtxs[0]);
            bool lu = firstNV.TypeAndValue.IsUnsignedInteger() != -1;
            llvm::Value* lvalue = LoadNamedVariable(firstNV);

            for (size_t i = 1; i < nextCtxs.size(); i++)
            {
                auto rightNV = ParseCastExpression(nextCtxs[i]);
                bool ru = rightNV.TypeAndValue.IsUnsignedInteger() != -1;
                llvm::Value* rvalue = LoadNamedVariable(rightNV);
                std::string op = ctx->children[i * 2 - 1]->getText();

                auto* overload = TryBinaryOperatorOverload(lvalue, op, rvalue, ctx);
                lvalue = overload ? overload : Compiler(ctx)->CreateOperation(op, lvalue, rvalue, lu, ru);
                lu = lu || ru;
            }

            return { lvalue, lu };
        }

        LogErrorContext(ctx, "Multiplicative expression has no operands.");
        return {};
    }

/*
 * A cast operand can name no value at all, in which case ParseCastExpression used to reach
 * CreateCast with a null llvm::Value and SIGSEGV the compiler with zero diagnostic output.
 * Two outcomes are possible and both are handled here.
 *
 * A generic FUNCTION instantiation ('gid<double>') is the supportable one: the postfix walk
 * publishes it as an empty NamedVariable carrying only the mangled instantiation name, the
 * same convention the declarator and assignment paths already consume. It is a monomorphized
 * function symbol, so resolving it to its llvm::Function makes the cast behave exactly like a
 * cast of a plain named function - no gate is bypassed, the cast lands on the same
 * ParameterStoresData / RegisterCodeValueDataCast tail as '(void*)plainFn'.
 *
 * Everything else that reaches here (a type name, a namespace, a generic function template
 * with no type arguments) genuinely has no value, so it gets a located diagnostic.
 */
void MainListener::ResolveValuelessCastOperand(CFlatParser::CastExpressionContext* ctx,
                                               CFlatParser::CastExpressionContext* operandCtx,
                                               LLVMBackend::NamedVariable& namedVar,
                                               const LLVMBackend::TypeAndValue& destTypeName) {
        if (namedVar.Primary != nullptr || namedVar.Storage != nullptr)
            return;

        // Compiler() without a ctx: the ctx overload calls SetSourceLocation, which would move
        // the caret of every later diagnostic from the operand to the cast's '('.
        auto* compiler = Compiler();

        // A monomorphized instantiation's mangled name is unique, so it resolves exactly the way
        // a plain named function does (ParseIdentifier's GetFunctionForFuncPtr(name) arm).
        if (!namedVar.CallerName.empty())
        {
            namedVar.Primary = compiler->GetFunctionForFuncPtr(namedVar.CallerName);
            if (namedVar.Primary != nullptr)
            {
                // Instantiating the template left the caret inside the template BODY; point it at
                // the operand, which is where the plain named-function spelling leaves it.
                if (operandCtx != nullptr)
                    Compiler(operandCtx);
                return;
            }
        }

        std::string operandText = operandCtx != nullptr ? operandCtx->getText() : std::string();
        // The base name is the spelling before any type-argument list, so a template whose
        // arguments failed to form an instantiation is still recognized as one.
        std::string operandBase = operandText.substr(0, operandText.find('<'));
        bool isTemplate = !operandBase.empty()
            && genericFunctionTemplates.count(compiler->ResolveGenericFunctionBase(operandBase)) != 0;

        // A bare generic function template resolves to nothing until it is instantiated; name the
        // remedy, which the branch above compiles.
        if (isTemplate && operandBase == operandText)
        {
            LogErrorContext(ctx, std::format(
                "'{}' is a generic function template and has no value until it is instantiated; "
                "give it explicit type arguments (e.g. '{}<int>').", operandText, operandText));
        }
        // Type arguments WERE written but no instantiation came of them - wrong count, or an
        // argument that is not a type. Name that rather than claiming it is not a value.
        else if (isTemplate)
        {
            LogErrorContext(ctx, std::format(
                "'{}' did not instantiate the generic function template '{}', so it has no value; "
                "check the number and spelling of its type arguments.", operandText, operandBase));
        }
        else
        {
            // Spell the destination from the source text; a synthesized rendering would have to
            // guess at array-view, function-pointer and alias spellings.
            std::string destText = ctx->typeName() != nullptr ? ctx->typeName()->getText() : destTypeName.TypeName;
            LogErrorContext(ctx, std::format(
                "'{}' does not name a value, so it cannot be cast to '{}' - a type name, a namespace "
                "and an uninstantiated generic template are not values.",
                operandText, destText));
        }
    }

LLVMBackend::NamedVariable MainListener::ParseCastExpression(CFlatParser::CastExpressionContext* ctx, bool lvalue,
                                                   bool discardResult) {
        auto* compiler = Compiler(ctx);
        auto unaryCtx = ctx->unaryExpression();
        auto castExp = ctx->castExpression();
        auto typeName = ctx->typeName();

        if (unaryCtx != nullptr)
        {
            // Single-child passthrough: forward discardResult unchanged.
            return ParseUnaryExpression(unaryCtx, discardResult);
        }
        else if (castExp && typeName)
        {
            auto namedVar = ParseCastExpression(castExp);
            auto destTypeName = ParseTypeName(typeName);
            auto type = compiler->GetType(destTypeName);

            // Materialize a stored operand into Primary. A fixed-size array decays to
            // a pointer to its first element ((u8*)buf on char[N], (char**)a on
            // char*[N]); loading the whole [N x T] aggregate would feed an invalid
            // aggregate-to-pointer bitcast. Everything else loads with the source
            // type so we read the correct number of bytes - loading directly with the
            // destination type reads too many bytes from narrower fields (e.g., an
            // i64 load from a u32 field corrupts the adjacent field).
            auto materialize = [&](LLVMBackend::NamedVariable& nv)
            {
                if (nv.Primary != nullptr || nv.Storage == nullptr)
                    return;
                if (nv.BaseType && llvm::isa<llvm::ArrayType>(nv.BaseType))
                {
                    auto* arrTy = llvm::cast<llvm::ArrayType>(nv.BaseType);
                    auto* zero = compiler->builder->getInt64(0);
                    nv.Primary = compiler->builder->CreateGEP(arrTy, nv.Storage, {zero, zero}, "arrptr");
                }
                else
                {
                    auto srcType = compiler->GetType(nv.TypeAndValue);
                    nv.Primary = compiler->CreateLoad(srcType, nv.Storage);
                }
                nv.Storage = nullptr;
            };

            ResolveValuelessCastOperand(ctx, castExp, namedVar, destTypeName);

            // If the destination is a struct VALUE type and an operator overload
            // exists, call it (e.g. (string)charPtr calls operator string(char*)).
            // A POINTER (or array-view) destination is a pure reinterpret, never a
            // value conversion: '(string*)p' must not invoke 'operator string'. Doing
            // so produced a 'string' VALUE where a 'string*' was expected, which then
            // asserted the compiler downstream (e.g. an invalid bitcast in
            // 'delete[_] (string*)expr', or a 'no overload' error when no value
            // conversion matched the pointer operand).
            std::string opName = "operator " + destTypeName.TypeName;
            if (!destTypeName.Pointer && !destTypeName.IsArrayView
                && compiler->GetFunction(opName) != nullptr)
            {
                auto argNV = namedVar;
                argNV.TypeAndValue.VariableName = "";  // clear name so positional matching is used
                materialize(argNV);

                // A bare `(string)primitive` cast is no longer a public conversion - a
                // primitive's NamedVariable here is an integer/floating-point scalar value
                // (char*/IString operands are pointer/struct values and still cast). Direct
                // the user to value.toString(); interpolation "{x}" and string concatenation
                // keep working because they reach the internal operator string hook by name,
                // not through this cast path. (This also covers the former literal-cast crash.)
                if (destTypeName.TypeName == "string" && argNV.Primary != nullptr
                    && (argNV.Primary->getType()->isIntegerTy()
                        || argNV.Primary->getType()->isFloatingPointTy()))
                {
                    LogErrorContext(ctx, "cannot cast a primitive value to 'string'; call "
                        "'value.toString()' instead (string interpolation \"{x}\" and '+' "
                        "concatenation still convert automatically)");
                    namedVar.TypeAndValue = destTypeName;
                    return namedVar;
                }

                auto result = compiler->CreateOverloadedFunctionCall(opName, { argNV });
                namedVar.Primary = result;
                namedVar.Storage = nullptr;
                namedVar.TypeAndValue = destTypeName;
                return namedVar;
            }

            materialize(namedVar);

            // A fat `Lambda<...>` closure is a 16-byte {code, env} struct; it must be
            // CONSTRUCTED from a lambda/named function, never reinterpreted from a scalar.
            // Casting a raw pointer (or any non-closure value) to it would emit an illegal
            // 8B->16B bitcast that fails module verification. The thin `function<...>` cast
            // is the supported path for a raw code address (e.g. a COM vtable slot).
            if (destTypeName.IsFunctionPointer && !destTypeName.IsThinFnPtr()
                && namedVar.Primary != nullptr && !namedVar.Primary->getType()->isStructTy())
            {
                LogErrorContext(ctx, "cannot cast to a 'Lambda<...>' closure; a fat closure "
                    "must be built from a lambda or named function, not reinterpreted from a "
                    "pointer. Cast to the thin 'function<...>' to call a raw code address.");
                namedVar.TypeAndValue = destTypeName;
                return namedVar;
            }

            // Explicit `(T[])p` escape: re-establish the noalias array-view from a raw
            // pointer. `T*` and `T[]` share an identical representation, so this is a pure
            // reinterpret (no bit manipulation) - the sanctioned inverse of the implicit
            // `T[] -> T*` decay, by which the programmer asserts the pointer spans a whole,
            // distinct allocation. The result carries IsArrayView, so it passes the one-way
            // bind gate (RejectRawPointerToArrayView) at every binding site.
            if (destTypeName.IsArrayView)
            {
                if (!namedVar.TypeAndValue.Pointer)
                    LogErrorContext(ctx, "'(T[])' cast requires a pointer source 'T*'; "
                        "cannot cast a non-pointer to an array-view");
                namedVar.TypeAndValue = destTypeName;
                return namedVar;
            }

            bool srcIsSigned = namedVar.TypeAndValue.IsUnsignedInteger() == -1;
            // A cast off a temp's `unique` field is the "I mean this" spelling, and it drops the
            // ownership facts with the type; carry them to the RESULT so persist sites still see it.
            bool castOfTempUniqueField = IsOwningTempUniqueFieldEscape(namedVar);
            namedVar.Primary = compiler->CreateCast(namedVar.Primary, type, srcIsSigned);
            namedVar.TypeAndValue = destTypeName;
            if (castOfTempUniqueField)
                compiler->RegisterOwningTempUniqueField(namedVar.Primary);
            // A ptr->ptr cast is a no-op under opaque pointers, so the result IS the ledgered
            // code value; launder it or the explicit cast the rejection advises is itself refused.
            if (compiler->ParameterStoresData(destTypeName))
                compiler->RegisterCodeValueDataCast(namedVar.Primary);
            // The mirror launder: a cast TO a code type is the escape hatch the closure-widen
            // gate advises, and the no-op cast result is still the ledgered data value.
            else if (destTypeName.IsFunctionPointer)
                compiler->RegisterDataValueCodeCast(namedVar.Primary);
            return namedVar;
        }

        LogErrorContext(ctx, "Cast expression has no recognized form.");
        return {};
    }

LLVMBackend::TypeAndValue MainListener::ParseTypeName(CFlatParser::TypeNameContext* ctx) {
        auto specCtx = ctx->specifierQualifierList();
        auto abstractDecl = ctx->abstractDeclarator();

        LLVMBackend::TypeAndValue typeValue;

        if (specCtx)
        {
            auto typeQualfier = specCtx->typeQualifier();
            auto typeSpecs = specCtx->typeSpecifier();

            if (typeSpecs.size() > 0)
            {
                // TODO Collect all of them.
                auto* typeSpec = typeSpecs[0];
                std::string baseName;
                auto* genParams = GenericSpecOf(typeSpec, baseName);
                if (auto* fpSpec = typeSpec->functionPointerSpecifier())
                {
                    // Cast target `(function<R(Args)>)addr` / `(Lambda<...>)x`: build the
                    // resolved closure type so a raw code address can be reinterpreted as a
                    // thin C function pointer. Mirrors the declaration path (BuildFuncPtrAliasType).
                    typeValue = BuildFuncPtrAliasType(fpSpec);
                }
                else if (genParams != nullptr)
                {
                    // Generic cast: (channel<int>*) -> mangle to channel__int
                    baseName = compilerLLVM->ResolveGenericBaseAlias(baseName);
                    std::vector<std::string> typeArgs;
                    for (auto* entry : genParams->typeParameterList()->typeParameterEntry())
                        typeArgs.push_back(ResolveTypeArgEntry(entry));
                    typeValue.TypeName = MangledGenericName(baseName, typeArgs);
                    // A deferred winmd generic interface named directly in a cast (no `using` or
                    // forward-ref scan reached it) is instantiated on demand here. Idempotent/cached,
                    // and a no-op false for CFlat generics (already queued by the scanner).
                    if (compilerLLVM->GetDataStructure(typeValue.TypeName).StructType == nullptr)
                        compilerLLVM->InstantiateWinrtGenericInterface(baseName, typeArgs, typeValue.TypeName);
                }
                else
                {
                    typeValue.TypeName = typeSpec->getText();
                    // `(long long)x` / `sizeof(long long)`: two `long` specifiers, only [0] is read.
                    if (typeValue.TypeName == "long")
                    {
                        int longSpecCount = 0;
                        for (auto* ts : typeSpecs)
                            if (ts->getText() == "long") longSpecCount++;
                        typeValue.TypeName = LongSpellingTypeName(longSpecCount);
                    }
                    // Apply active type-parameter substitutions (e.g. T -> int inside a generic function body).
                    auto substIt = activeTypeSubstitutions.find(typeValue.TypeName);
                    if (substIt != activeTypeSubstitutions.end())
                        typeValue.TypeName = substIt->second;
                    // Expand a `using` alias so a cast through it names a real type, e.g.
                    // (StringOp*)p -> the instantiated thin-interface struct. One-hop, matching the
                    // declaration path (ParseDeclarationSpecifiers); covers generic and plain aliases.
                    typeValue.TypeName = compilerLLVM->ResolveTypeAlias(typeValue.TypeName);
                    // Peel any trailing stars baked into a substitution or pointer alias
                    // (using Handle = void*) onto the pointer flag.
                    if (PeelAliasPointerStars(typeValue.TypeName) > 0)
                        typeValue.Pointer = true;
                }
            }
        }
        else
        {
            // undefined type.
            LogErrorContext(ctx, "Type name has no specifier-qualifier list.");
            return typeValue;
        }

        if (abstractDecl && abstractDecl->pointer())
        {
            typeValue.Pointer = true;
        }

        // `(T[])` cast target: the noalias array-view. Empty brackets only - a sized
        // `(T[N])` is not a meaningful cast. Mirrors the declaration path: an array-view
        // is a pointer repr carrying the noalias contract.
        if (abstractDecl)
        {
            // `(T[]*)` / `(T[N]*)`: a trailing '*' after the brackets is never a valid type.
            // Redirect to the array-view cast (return-early avoids a duplicate sized-array error).
            if (ArrayPtrOf(abstractDecl))
            {
                LogErrorContext(ctx, "pointer to array-view '(T[]*)' is not a valid type; cast to "
                    "the array-view '(T[])' and take its address separately if you need a pointer");
                return typeValue;
            }
            if (auto* dimSpec = ArrayDimsOf(abstractDecl))
            {
                if (DimSpecIsUnsizedMultiDim(dimSpec))
                    LogErrorContext(ctx, UnsizedMultiDimMessage(typeValue.TypeName));
                if (!dimSpec->assignmentExpression().empty())
                    LogErrorContext(ctx, "a sized array '(T[N])' is not a valid cast target; "
                        "use '(T[])' for the noalias array-view");
                typeValue.IsArrayView = true;
                typeValue.Pointer = true;
            }
        }

        return typeValue;
    }

LLVMBackend::NamedVariable MainListener::ParseUnaryExpression(CFlatParser::UnaryExpressionContext* ctx,
                                                    bool discardResult) {
        auto* compiler = Compiler(ctx);
        auto postFixCtx = ctx->postfixExpression();
        auto castExpCtx = ctx->castExpression();
        auto unaryOperator = ctx->unaryOperator();
        auto typeNameCtx = ctx->typeName();

        // Handle sizeof/alignof as prefix: the parser may match "sizeof" and "(TypeName)"
        // separately, where the "(TypeName)" becomes a postfixExpression (function call syntax)
        if (postFixCtx != nullptr)
        {
            std::string text = ctx->getText();
            bool prefixSizeof = text.find("sizeof(") == 0;
            bool prefixAlignof = text.find("alignof(") == 0;

            if ((prefixSizeof || prefixAlignof) && typeNameCtx == nullptr)
            {
                // The parser matched sizeof/alignof as a prefix on a postfixExpression.
                // Extract the type name from the text (e.g., "sizeof(Point)" -> "Point")
                std::string postfixText = postFixCtx->getText();

                // Remove outer parentheses if present
                if (!postfixText.empty() && postfixText[0] == '(' && postfixText.back() == ')')
                {
                    postfixText = postfixText.substr(1, postfixText.length() - 2);
                }

                // Does this look like a type name? '(' ')' ',' count as type text only INSIDE
                // balanced generic brackets (Pair<int,float>, Box<function<int(int)>>).
                bool likelyType = !postfixText.empty() && (std::isalpha(postfixText[0]) || postfixText[0] == '_');
                int angleDepth = 0;
                bool usedBracketPunct = false;
                for (char c : postfixText)
                {
                    if (c == '<') { angleDepth++; continue; }
                    if (c == '>') { angleDepth--; continue; }
                    if (std::isalnum(c) || c == '_' || c == '.' || c == '*')
                        continue;
                    if (angleDepth > 0 && (c == '(' || c == ')' || c == ','))
                    {
                        usedBracketPunct = true;
                        continue;
                    }
                    likelyType = false;
                    break;
                }
                // Only the newly-admitted punctuation demands balanced brackets; an unbalanced
                // spelling like 'a<b' keeps its existing (bracket-free) classification.
                if (usedBracketPunct && angleDepth != 0)
                    likelyType = false;

                if (likelyType)
                {
                    // Try to parse as a type
                    LLVMBackend::TypeAndValue typeValue;
                    typeValue.TypeName = postfixText;

                    // Check for trailing * (pointer)
                    if (!typeValue.TypeName.empty() && typeValue.TypeName.back() == '*')
                    {
                        typeValue.Pointer = true;
                        typeValue.TypeName.pop_back();
                    }

                    // Apply active type-parameter substitutions (e.g. sizeof(T) inside a generic function body).
                    {
                        auto substIt = activeTypeSubstitutions.find(typeValue.TypeName);
                        if (substIt != activeTypeSubstitutions.end())
                        {
                            typeValue.TypeName = substIt->second;
                            while (!typeValue.TypeName.empty() && typeValue.TypeName.back() == '*')
                            {
                                typeValue.TypeName.pop_back();
                                typeValue.Pointer = true;
                            }
                        }
                    }

                    // sizeof(T) where T is a pack param returns the element count
                    if (prefixSizeof)
                    {
                        auto packIt = activePackSubstitutions.find(typeValue.TypeName);
                        if (packIt != activePackSubstitutions.end())
                        {
                            LLVMBackend::NamedVariable namedVar;
                            namedVar.Primary = llvm::ConstantInt::get(
                                llvm::Type::getInt32Ty(*compiler->context),
                                (int)packIt->second.size());
                            namedVar.TypeAndValue.TypeName = "int";
                            return namedVar;
                        }
                    }

                    // sizeof(var) / alignof(var): a bare identifier that names a
                    // variable (and not a type) measures the variable's declared
                    // storage - most useful for fixed arrays, where sizeof(buf) on
                    // 'char[128] buf' is 128. The type meaning wins on a name
                    // collision, matching C. Adopting the variable's TypeAndValue
                    // here lets the normal type path below compute the (padded)
                    // size or alignment.
                    if (!typeValue.Pointer
                        && postfixText.find('.') == std::string::npos
                        && postfixText.find('<') == std::string::npos
                        && !compiler->IsKnownTypeName(typeValue.TypeName))
                    {
                        auto varNV = compiler->GetLocalVariable(typeValue.TypeName);
                        if (varNV.Storage == nullptr && varNV.Primary == nullptr)
                            varNV = compiler->GetGlobalVariableNV(typeValue.TypeName);
                        if (varNV.Storage != nullptr || varNV.Primary != nullptr)
                            typeValue = varNV.TypeAndValue;
                    }

                    auto* llvmType = compiler->GetType(typeValue, nullptr, true);
                    if (llvmType && !llvmType->isVoidTy())  // Void is a valid type but let's use basic validity check
                    {
                        llvm::Value* result;
                        uint64_t effAlign = typeValue.Pointer
                            ? 0
                            : compiler->GetEffectiveAlignmentForType(typeValue.TypeName, llvmType);
                        if (prefixSizeof)
                        {
                            if (effAlign > 1)
                            {
                                uint64_t paddedSize = compiler->GetEffectiveAllocSize(llvmType, effAlign);
                                result = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*compiler->context), paddedSize);
                            }
                            else
                            {
                                result = compiler->GetTypeSizeBytes(llvmType);
                            }
                        }
                        else
                        {
                            if (effAlign > 1)
                                result = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*compiler->context), effAlign);
                            else
                                result = compiler->GetTypeAlignBytes(llvmType);
                        }

                        if (result)
                        {
                            LLVMBackend::NamedVariable namedVar;
                            namedVar.Primary = result;
                            namedVar.TypeAndValue.TypeName = "i64";
                            namedVar.Storage = nullptr;
                            return namedVar;
                        }
                    }
                }
            }

            // Single-child passthrough: forward discardResult unchanged.
            return ParsePostfixExpression(postFixCtx, false, 0, discardResult);
        }
        else if (auto* newCtx = ctx->newExpression())
        {
            return ParseNewExpression(newCtx);
        }
        else if (auto* delCtx = ctx->deleteExpression())
        {
            return ParseDeleteExpression(delCtx);
        }
        else if (auto* moveCtx = ctx->moveExpression())
        {
            return ParseMoveExpression(moveCtx);
        }
        else if (auto* opStrCtx = ctx->operatorStringExpression())
        {
            return ParseOperatorStringExpression(opStrCtx);
        }
        else if (unaryOperator && castExpCtx)
        {
            /* unaryOperator : '&' | '*'| '+'| '-'| '~'| '!'; */

            std::string opText = unaryOperator->getText();

            // For '!', suspend any active short-circuit else-block before
            // evaluating the operand.  Without this, !(A && B) in a while
            // condition would short-circuit the inner && directly to the
            // loop-exit block when A is false - bypassing the negation -
            // instead of continuing the loop as !(false && B) == true demands.
            llvm::BasicBlock* savedElse = nullptr;
            if (opText == "!")
                savedElse = compiler->ExchangeElseBlock(nullptr);

            auto namedVar = ParseCastExpression(castExpCtx);

            if (opText == "!")
                compiler->ExchangeElseBlock(savedElse);

            if (opText == "&")
            {
                if (namedVar.BitfieldStorage)
                {
                    LogErrorContext(ctx, std::format(
                        "cannot take the address of bitfield '{}' - bitfields are not byte-addressable",
                        namedVar.FieldName));
                }
                if (!namedVar.Storage)
                {
                    LogErrorContext(ctx, "Unable to get an Address-of an object without a Storage.");
                }

                namedVar.Primary = namedVar.Storage;
                namedVar.Storage = nullptr;
                // The address of an element (e.g. &a[i]) is a raw pointer to one slot, never a
                // whole-allocation array-view. Clear the flag so it cannot bind back into a `T[]`
                // (which would forge an offset view that overlaps the original - the escape hatch
                // the noalias contract must keep one-way). Mark a scalar operand's address as a
                // pointer so the array-view bind gate sees `&a[i]` as the raw `int*` it is.
                namedVar.TypeAndValue.IsArrayView = false;
                if (!namedVar.TypeAndValue.Pointer)
                    namedVar.TypeAndValue.Pointer = true;
                // The address of an element borrows, it does not own - clear IsOwning so taking
                // `&a[i]` does not move/consume the owning buffer `a` it points into.
                namedVar.IsOwning = false;
                // Move-dataflow: taking a variable's address (e.g. an out-parameter `receive(&v)`)
                // may reinitialize it, so treat it as a revive - the value is live again afterward.
                compiler->RecordMoveGenRevive(namedVar.CallerName);
                // The escaped address could rewrite the pointee (e.g. `fill(&a)`) - latch the
                // explicit-move-null deref guard off for good, per MarkVariableAddressEscaped.
                compiler->MarkVariableAddressEscaped(namedVar.CallerName);
            }
            else if (opText == "*")
            {
                if (!namedVar.Storage)
                {
                    LogErrorContext(ctx, "Unable to dereference an object without a Storage.");
                }

                if (!namedVar.Storage->getType()->isPointerTy())
                {
                    LogErrorContext(ctx, "Unable to dereference a non-Pointer.");
                }

                // Strip one pointer level from the CFlat type to get the pointee type.
                // For char** (ElemPointer=true): deref gives char* (keep Pointer, clear ElemPointer).
                // For char* (ElemPointer=false): deref gives char (clear Pointer).
                if (namedVar.TypeAndValue.ElemPointer)
                    namedVar.TypeAndValue.ElemPointer = false;
                else
                {
                    namedVar.TypeAndValue.Pointer = false;
                    namedVar.TypeAndValue.IsInterfacePointer = false;
                }
                auto* pointeeType = compiler->GetType(namedVar.TypeAndValue);
                llvm::Value* baseStorage = namedVar.Storage;
                // A `*p` dereference of an explicitly-moved-null thin pointer local is statically
                // null - reject it, same as the '->'/'.' guard.
                compiler->RecordNullDerefFor(namedVar, ctx->getStart()->getLine(),
                    ctx->getStart()->getCharPositionInLine());
                if (compiler->IsExplicitlyMovedNullHere(namedVar))
                    LogErrorContext(ctx, std::format(
                        "dereference of moved variable '{}' (it is null after the move)",
                        namedVar.CallerName));
                llvm::Value* loadedPtr = compiler->CreateLoad(namedVar.Storage);
                // --sanitize=ownership (M1): guard `*p` deref of a moved-from local.
                compiler->EmitOwnDerefGuard(baseStorage, loadedPtr,
                    ctx->getStart()->getLine(), ctx->getStart()->getCharPositionInLine());
                // The deref'd location: loadedPtr is the address; pointeeType is what it holds.
                // Storing it in Storage (not Primary) makes it usable as both lvalue and rvalue.
                namedVar.Storage  = loadedPtr;
                namedVar.Primary  = nullptr;
                namedVar.BaseType = pointeeType;
            }
            else if (opText == "!")
            {
                auto newValue = this->LoadNamedVariable(namedVar);
                if (auto* overload = TryUnaryOperatorOverload(newValue, "!", ctx))
                {
                    namedVar.Primary = overload;
                    namedVar.TypeAndValue = compiler->lastCallReturnType;
                }
                else
                {
                    // Logical negation yields a plain rvalue bool, whatever the operand was
                    // (int, pointer, float). Reset the type so a negated pointer/owning/view
                    // operand cannot carry those flags into the result.
                    namedVar.Primary = compiler->CreateLogicalNot(newValue);
                    namedVar.TypeAndValue = {};
                    namedVar.TypeAndValue.TypeName = "bool";
                    namedVar.BaseType = nullptr;
                    namedVar.IsOwning = false;
                    namedVar.BitfieldStorage = nullptr;
                }
                namedVar.Storage = nullptr;
            }
            else if (opText == "-")
            {
                auto newValue = this->LoadNamedVariable(namedVar);
                if (auto* overload = TryUnaryOperatorOverload(newValue, "-", ctx))
                {
                    namedVar.Primary = overload;
                    namedVar.TypeAndValue = compiler->lastCallReturnType;
                    namedVar.Storage = nullptr;
                }
                else
                {
                    // Fold negation of integer constants into the smallest fitting type.
                    // e.g. 32768 is i32, but -32768 fits in i16 (INT16_MIN).
                    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(newValue))
                    {
                        int64_t neg = -(int64_t)ci->getSExtValue();
                        if (neg >= std::numeric_limits<int8_t>::min() && neg <= std::numeric_limits<int8_t>::max())
                            namedVar.Primary = compiler->builder->getInt8((int8_t)neg);
                        else if (neg >= std::numeric_limits<int16_t>::min() && neg <= std::numeric_limits<int16_t>::max())
                            namedVar.Primary = compiler->builder->getInt16((int16_t)neg);
                        else if (neg >= (int64_t)std::numeric_limits<int32_t>::min() && neg <= (int64_t)std::numeric_limits<int32_t>::max())
                            namedVar.Primary = compiler->builder->getInt32((int32_t)neg);
                        else
                            namedVar.Primary = compiler->builder->getInt64(neg);
                    }
                    else
                    {
                        namedVar.Primary = compiler->CreateNeg(newValue);
                    }
                    namedVar.Storage = nullptr;
                }
            }
            else if (opText == "+")
            {
                auto newValue = this->LoadNamedVariable(namedVar);
                if (auto* overload = TryUnaryOperatorOverload(newValue, "+", ctx))
                {
                    namedVar.Primary = overload;
                    namedVar.TypeAndValue = compiler->lastCallReturnType;
                }
                else
                {
                    // unary + is a no-op: just load the value
                    namedVar.Primary = newValue;
                }
                namedVar.Storage = nullptr;
            }
            else if (opText == "~")
            {
                auto newValue = this->LoadNamedVariable(namedVar);
                if (auto* overload = TryUnaryOperatorOverload(newValue, "~", ctx))
                {
                    namedVar.Primary = overload;
                    namedVar.TypeAndValue = compiler->lastCallReturnType;
                }
                else
                {
                    namedVar.Primary = compiler->CreateNot(newValue);
                }
                namedVar.Storage = nullptr;
            }
            else
            {
                LogErrorContext(ctx, std::format("{} operator is not yet implemented.", opText));
                return namedVar;
            }

            return namedVar;
        }
        else if (auto* typeNameCtx = ctx->typeName())
        {
            // Handle sizeof(typeName) or alignof(typeName)
            std::string text = ctx->getText();

            // Check if this is a sizeof or alignof expression
            bool isSizeof = text.find("sizeof(") != std::string::npos;
            bool isAlignof = text.find("alignof(") != std::string::npos;

            if (isSizeof || isAlignof)
            {
                // Parse the type name to get its LLVM type
                auto typeValue = ParseTypeName(typeNameCtx);
                if (typeValue.TypeName.empty())
                {
                    LogErrorContext(ctx, "sizeof/alignof: could not determine type");
                    return {};
                }

                // sizeof(T) where T is a pack param returns the element count, not byte size
                if (isSizeof)
                {
                    auto packIt = activePackSubstitutions.find(typeValue.TypeName);
                    if (packIt != activePackSubstitutions.end())
                    {
                        LLVMBackend::NamedVariable namedVar;
                        namedVar.Primary = llvm::ConstantInt::get(
                            llvm::Type::getInt32Ty(*compiler->context),
                            (int)packIt->second.size());
                        namedVar.TypeAndValue.TypeName = "int";
                        return namedVar;
                    }
                }

                auto* llvmType = compiler->GetType(typeValue, nullptr, true);
                if (!llvmType)
                {
                    LogErrorContext(ctx, "sizeof/alignof: could not resolve type to LLVM type");
                    return {};
                }

                // Get the size or alignment value, honoring any struct-level alignas.
                uint64_t effAlign = typeValue.Pointer
                    ? 0
                    : compiler->GetEffectiveAlignmentForType(typeValue.TypeName, llvmType);
                llvm::Value* result;
                if (isSizeof)
                {
                    if (effAlign > 1)
                    {
                        uint64_t paddedSize = compiler->GetEffectiveAllocSize(llvmType, effAlign);
                        result = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*compiler->context), paddedSize);
                    }
                    else
                    {
                        result = compiler->GetTypeSizeBytes(llvmType);
                    }
                }
                else
                {
                    if (effAlign > 1)
                        result = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*compiler->context), effAlign);
                    else
                        result = compiler->GetTypeAlignBytes(llvmType);
                }

                // Return as a named variable with i64 type
                LLVMBackend::NamedVariable namedVar;
                namedVar.Primary = result;
                namedVar.TypeAndValue.TypeName = "i64";
                namedVar.Storage = nullptr;
                return namedVar;
            }
        }

        LogErrorContext(ctx, "Unary expression has no recognized form.");
        return {};
    }

std::string MainListener::ParseTypeSpecifierName(CFlatParser::TypeSpecifierContext* ctx) {
        std::string base;
        if (auto* genParams = GenericSpecOf(ctx, base))
        {
            // Generic type: Box<int> -> Box__int
            // Also apply type substitutions to arguments (e.g. Box<T> with T=int -> Box__int)
            base = Compiler(ctx)->ResolveGenericBaseAlias(base);
            std::vector<std::string> args;
            // ResolveTypeArgEntry applies active substitutions AND recursively
            // resolves/queues nested generics (e.g. list<int> inside list<list<int>>).
            for (auto* entry : genParams->typeParameterList()->typeParameterEntry())
                args.push_back(ResolveTypeArgEntry(entry));
            return MangledGenericName(base, args);
        }
        std::string name = ctx->getText();
        // Apply active type substitutions (for generic templates)
        auto it = activeTypeSubstitutions.find(name);
        // A substituted name is already resolved in the caller's scope - resolve it from the root
        // so the template's declaring namespace cannot rebind it (layer 3).
        bool wasSubstituted = it != activeTypeSubstitutions.end();
        if (it != activeTypeSubstitutions.end()) name = it->second;
        StripOwnershipQualifiers(name);  // resolve to the underlying LLVM type; storage markers only
        return Compiler(ctx)->ResolveQualifiedName(name, wasSubstituted);
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
            LogErrorContext(ctx, std::format("field initializer: unknown function '{}'", functionName));
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

bool MainListener::EmitOneFieldInit(
        llvm::Value* structPtr,
        const LLVMBackend::StructData& sd,
        const std::string& typeName,
        const std::string& fieldName,
        LLVMBackend::NamedVariable& rightNV,
        antlr4::ParserRuleContext* errCtx) {
        auto* compiler = Compiler(errCtx);

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
            LogErrorContext(errCtx, std::format("'{}' has no field named '{}'", typeName, fieldName));
            return false;
        }

        // Brace leg of the code-value store gate. This is a SEPARATE lowering path from the `=`
        // assignment site, so `h.p = w` being rejected says nothing about `Holder h = { p = w }`.
        RejectCodeValueIntoDataSlot(errCtx, rightNV, fieldType, "brace-initialize",
                                    std::format("field '{}.{}' of", typeName, fieldName));

        // `unique` field initialized here: this is a second field-store path, so it needs the same
        // two source rejections the `=` path applies (Trap A, and field-to-field). No reassign-free
        // is needed - both callers construct a fresh slot, so there is no old pointee to release.
        // The two borrow legs use the same owning-slot predicate as the `=` path, so a field made
        // owning by generic substitution (`Box<unique Item*>::t`) is seen by both store paths.
        if (IsOwningUniquePointerField(fieldType))
        {
            std::string fieldDesc = std::format("unique field '{}.{}'", typeName, fieldName);
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
        if (braceSrcIsUniqueFieldRead && (braceDestOwnsPointee || IsOwningUniqueInterfaceField(fieldType)))
        {
            std::string braceDesc = std::format("unique field '{}.{}'", typeName, fieldName);
            if (braceDestOwnsPointee && IsUniqueTempFieldRead(rightNV))
                RejectUniqueTempFieldToField(rightNV, braceDesc, errCtx);
            else if (braceDestOwnsPointee)
                RejectUniqueFieldToUniqueField(rightNV, braceDesc, errCtx);
            else
                RejectUniqueInterfaceFieldToField(rightNV, braceDesc, errCtx);
        }
        // Belt-and-braces: the source gates above all throw, so this leg runs only for the
        // cast/join spellings that reach an owning destination with every declared fact stripped.
        bool braceSrcGateFired = braceSrcIsUniqueFieldRead
            && (braceDestOwnsPointee || IsOwningUniqueInterfaceField(fieldType));
        if (!braceSrcGateFired && IsOwningTempUniqueFieldEscape(rightNV))
        {
            bool braceDestOwns = braceDestOwnsPointee || IsOwningUniqueInterfaceField(fieldType);
            RejectOwningTempUniqueFieldEscape(
                rightNV, std::format("{}field '{}.{}'", braceDestOwns ? "unique " : "",
                                     typeName, fieldName), errCtx);
        }

        llvm::Value* val = LoadNamedVariable(rightNV);
        if (!val) return false;

        // Same provable stack-address reject the `=` path applies, with the same scalar-pointer
        // shape gate; `&local` carries no borrow provenance for the legs above to see.
        if ((fieldType.IsUnique || fieldType.IsUniqueTypeArg)
            && fieldType.Pointer && fieldType.ConstArraySize == 0 && !fieldType.IsInterface
            && LLVMBackend::IsProvableNonHeapAddress(val))
            RejectNonHeapAddressIntoUnique(
                std::format("unique field '{}.{}'", typeName, fieldName), errCtx);

        // Same allocation-alignment agreement the `=` path demands: this store reaches the very
        // same synthesized free site, so a mismatch here corrupts the heap identically.
        RejectFieldAllocAlignMismatch(
            fieldType, fieldType.AllocAlignValue, rightNV, val,
            std::format("field '{}.{}'", typeName, fieldName), errCtx);

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
            if (RejectOwningValueCopyIntoField(rightNV, val, false, braceCopied, errCtx)) return false;
        }

        // Coerce char* string literals to the string struct type
        if (fieldType.TypeName == "string" && !fieldType.Pointer
            && val->getType() == compiler->builder->getInt8Ty()->getPointerTo())
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
        else if (fieldType.IsInterface)
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
                    std::format("'{}.{}'", compiler->DisplayNameOfMangledType(typeName), fieldName));
            else if (clo != nullptr && clo->IsThinFnPtr()
                     && !val->getType()->isStructTy() && val->getType()->isPointerTy())
                // The thin sibling of the widen above: brace-init into a thin function<> field
                // (spelled or generic-encoded) fed a raw pointer.
                compiler->CheckThinFnPtrAssignProvenance(val, rightNV,
                    std::format("'{}.{}'", compiler->DisplayNameOfMangledType(typeName), fieldName));
        }

        llvm::Value* destination = nullptr;
        if (sd.IsUnion)
        {
            // Union: all fields alias at offset 0. Store through the raw pointer with the explicit field type.
            auto* fieldLLVMType = compiler->GetType(fieldType);
            compiler->CreateAssignment(val, structPtr, false, fieldLLVMType);
            destination = structPtr;
        }
        else
        {
            auto* gep = compiler->builder->CreateStructGEP(sd.StructType, structPtr, (unsigned)fieldIdx, fieldName + "_init");
            compiler->builder->CreateStore(val, gep);
            destination = gep;
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
        return true;
    }

void MainListener::LogPointerBraceInitReject(
        antlr4::ParserRuleContext* ctx,
        const std::string& what,
        const std::string& typeName,
        const std::string& typeText,
        bool canAllocate,
        bool isUnique) {
        // Carry 'unique' into the remedy so the suggestion is still a legal declaration.
        std::string qual = isUnique ? "unique " : "";
        std::string remedy = canAllocate
            ? std::format("allocate one first ('{}{}* p = new {}();') and assign through the "
                          "pointer instead", qual, typeName, typeName)
            : "assign an address to it instead";
        LogErrorContext(ctx, std::format(
            "cannot apply a brace initializer to the POINTER {} of type '{}' - the braces would "
            "write into the pointer itself, which holds an address and not a '{}'; {}",
            what, typeText, typeName, remedy));
    }

void MainListener::LogNonAggregateBraceInitReject(
        antlr4::ParserRuleContext* ctx,
        const std::string& name,
        const std::string& typeName) {
        LogErrorContext(ctx, std::format(
            "brace initializer with values is not supported on '{}' - '{}' is not "
            "a struct/union/class or a recognized container; assign it after declaration instead",
            name, typeName));
    }

void MainListener::LogArrayViewFieldBraceInitReject(
        antlr4::ParserRuleContext* ctx,
        const std::string& name,
        const std::string& typeName,
        bool elemPointer) {
        std::string star = elemPointer ? "*" : "";
        LogErrorContext(ctx, std::format(
            "cannot apply a brace initializer with values to view field '{}' of type '{}{}[]' - "
            "a view field owns no storage for the list to write into, and storage built for it "
            "would need a lifetime tied to the containing object, which a field cannot provide; "
            "declare it as '{}{}[N]' with a fixed size instead",
            name, typeName, star, typeName, star));
    }

std::string MainListener::DescribePointerDeclType(const LLVMBackend::TypeAndValue& tv) {
        if (tv.ConstArraySize > 0) return DescribeArrayShape(tv);
        // A simd type's TypeName is its ELEMENT ('float'), so spell the vector back out.
        std::string base = tv.IsSimd ? std::format("simd<{},{}>", tv.TypeName, tv.SimdLanes)
                                     : tv.TypeName;
        return base + std::string(FixedArrayElementStars(tv), '*');
    }

std::string MainListener::CodeValueDestSpelling(const LLVMBackend::TypeAndValue& dest) {
        if (dest.IsArrayView && dest.ConstArraySize == 0)
            return dest.TypeName + std::string(dest.ElemPointer ? 1 : 0, '*') + "[]";
        return DescribePointerDeclType(dest);
    }

std::string MainListener::CodeValueCastAdvice(const LLVMBackend::TypeAndValue& dest) {
        if (!dest.Pointer || dest.IsArrayView || dest.ConstArraySize > 0) return {};
        return CodeValueDestSpelling(dest);
    }

llvm::Value* MainListener::ParseFieldDefaultInitializer(
        const std::string& structName,
        const LLVMBackend::TypeAndValue& field,
        CFlatParser::AssignmentExpressionContext* ae) {
        auto* compiler = Compiler(ae);
        auto nv = ParseAssignmentExpressionNamed(ae);
        llvm::Value* val = LoadNamedVariable(nv);
        RejectCodeValueIntoDataSlot(ae, nv, field, "default-initialize",
            std::format("field '{}.{}' of", structName, field.VariableName));
        // Thin sibling of the widen elsewhere: a default initializer into a thin
        // function<> field fed a raw pointer (spelled or generic-encoded).
        const LLVMBackend::TypeAndValue* clo = field.IsFunctionPointer
            ? &field : compiler->GetEncodedClosureType(field.TypeName);
        if (val && clo && !field.Pointer && field.ConstArraySize == 0
            && val->getType()->isPointerTy() && !val->getType()->isStructTy())
        {
            if (clo->IsThinFnPtr())
                compiler->CheckThinFnPtrAssignProvenance(val, nv,
                    std::format("'{}.{}'", structName, field.VariableName));
            // Fat sibling: reject a provable data pointer first (LogError throws, so a
            // rejected source never reaches the widen), then widen a legal bare/thin source.
            else
            {
                std::string destDesc = std::format("'{}.{}'", structName, field.VariableName);
                compiler->CheckFatClosureAssignProvenance(val, nv, destDesc);
                val = compiler->WidenBareOrThinToClosureFat(val);
            }
        }
        return val;
    }

CFlatParser::InitializerListContext* MainListener::FieldDefaultBraceList(
        const LLVMBackend::DeclTypeAndValue& field) {
        if (field.Initializer != nullptr)
            return field.Initializer->initializerList();
        return field.BraceInitializer;
    }

/*
 * Build the value of a field whose default is a brace list. Mirrors the local declarator:
 * seed the slot with the field type's OWN default (so its scalar field defaults survive a
 * partial list), then let the named field-inits / container add() calls overwrite on top.
 */
llvm::Value* MainListener::ParseFieldDefaultBraceInitializer(
        const std::string& structName,
        const LLVMBackend::DeclTypeAndValue& field,
        CFlatParser::InitializerListContext* list) {
        auto* compiler = Compiler(list);
        // A 'T[]' VIEW field: the local spelling infers backing storage a field cannot outlive,
        // so a list with values is rejected ('{}' parses to no initializerList and never gets here).
        if (field.IsArrayView) {
            if (!list->fieldInit().empty())
                LogArrayViewFieldBraceInitReject(list,
                    std::format("{}.{}", structName, field.VariableName),
                    field.TypeName, field.ElemPointer);
            return nullptr;
        }
        if (field.ConstArraySize > 0)
            return EmitFieldDefaultFixedArrayBrace(structName, field, list);

        bool isContainer = field.TypeName.rfind("list__", 0) == 0
            || field.TypeName.rfind("array__", 0) == 0
            || field.TypeName.rfind("dictionary__", 0) == 0;
        auto* fieldType = compiler->GetDataStructure(field.TypeName).StructType;
        // Mirrors the two declarator scopes, in their order: a non-struct non-container type
        // first (a POINTER's TypeName is the pointee, so it is a struct and falls through).
        if (!isContainer && fieldType == nullptr)
            LogNonAggregateBraceInitReject(list,
                std::format("{}.{}", structName, field.VariableName), field.TypeName);
        if (field.Pointer || field.ElemPointer)
            LogPointerBraceInitReject(list,
                std::format("field '{}.{}'", structName, field.VariableName),
                field.TypeName, DescribePointerDeclType(field),
                CanSuggestAllocation(list, field), field.IsUnique);
        if (fieldType == nullptr) return nullptr;

        llvm::Value* slot = compiler->CreateAlloca(fieldType);
        llvm::Value* seed = nullptr;
        // forceRoot: the GetFunction guard is an exact-key lookup, so a namespace walk here
        // would call a same-named sibling type's constructor.
        if (compiler->GetFunction(field.TypeName))
            seed = compiler->CreateOverloadedFunctionCall(field.TypeName, {}, true);
        if (seed == nullptr || seed->getType() != fieldType)
            seed = llvm::Constant::getNullValue(fieldType);
        compiler->builder->CreateStore(seed, slot);

        if (!TryEmitContainerInitializer(slot, field, list))
            EmitFieldInitializer(slot, field.TypeName, list);
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
        CFlatParser::InitializerListContext* list) {
        auto* compiler = Compiler(list);
        auto* arrTy = llvm::dyn_cast<llvm::ArrayType>(compiler->GetType(field));
        if (arrTy == nullptr) return nullptr;

        // Positional ({v0, v1}) vs value-init ({} / {field=v}) - an element carrying no
        // field name is what distinguishes them, exactly as the declarator decides it.
        bool positional = false;
        for (auto* fi : list->fieldInit())
            if (fi->Identifier() == nullptr && fi->assignmentExpression().size() == 1)
            { positional = true; break; }
        bool emptyList = list->fieldInit().empty();

        std::string path = std::format("{}.{}", structName, field.VariableName);
        auto* slot = compiler->AllocaAtEntry(arrTy, nullptr, path + "_arrbrace");
        compiler->builder->CreateStore(llvm::Constant::getNullValue(arrTy), slot);

        auto structData = compiler->GetDataStructure(field.TypeName);
        if (positional)
        {
            EmitPositionalFixedArrayIntoSlot(path, field, list, slot);
        }
        else if (!emptyList && field.Pointer)
        {
            // 'S*[N] a = {f=v}' - the seed is an 'S' memcpy'd over each POINTER slot, so the
            // field values would become the element addresses.
            LogPointerBraceInitReject(list, std::format("array element of field '{}'", path),
                field.TypeName, DescribePointerDeclType(field),
                CanSuggestAllocation(list, field), field.IsUnique);
        }
        else if (!emptyList && structData.StructType == nullptr)
        {
            LogErrorContext(list, std::format(
                "array value-initializer '= {{}}' requires a struct element type, '{}' is not a struct",
                field.TypeName));
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
void MainListener::EmitOwningArrayValueInitSlots(
        llvm::Value* arrAlloc,
        llvm::StructType* elemTy,
        const LLVMBackend::TypeAndValue& tv,
        CFlatParser::InitializerListContext* list,
        bool forceRoot) {
        auto* compiler = Compiler();
        if (arrAlloc == nullptr || elemTy == nullptr) return;

        // Total slots: the outer extent times every inner dimension (T[N][M] is N*M elements).
        uint64_t n = tv.ConstArraySize;
        for (uint64_t d : tv.ConstInnerDimensions) n *= d;
        if (n == 0) return;

        compiler->EmitFixedArrayElementWalk(
            *compiler->builder, arrAlloc, elemTy, n,
            [&](llvm::Value* elemPtr)
            {
                llvm::Value* elem = compiler->GetFunction(tv.TypeName)
                    ? compiler->CreateOverloadedFunctionCall(tv.TypeName, {}, forceRoot)
                    : nullptr;
                if (elem != nullptr)
                    compiler->CreateAssignment(elem, elemPtr);
                else
                    compiler->builder->CreateStore(llvm::Constant::getNullValue(elemTy), elemPtr);
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

        if (compiler->IsOwningValueType(field.TypeName))
        {
            EmitOwningArrayValueInitSlots(slot, elemTy, field, list, true);
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

bool MainListener::CanSuggestAllocation(antlr4::ParserRuleContext* ctx, const LLVMBackend::TypeAndValue& tv) {
        return !tv.ElemPointer && tv.TypeName != "void"
            && Compiler(ctx)->GetDataStructure(tv.TypeName).StructType != nullptr;
    }

void MainListener::LogEmptyBraceOnPointerReject(
        antlr4::ParserRuleContext* ctx,
        const std::string& what,
        const LLVMBackend::TypeAndValue& tv) {
        std::string alloc = CanSuggestAllocation(ctx, tv)
            ? std::format(", or 'new {}()' for a pointer to a real object", tv.TypeName)
            : "";
        LogErrorContext(ctx, std::format(
            "empty brace initializer '{{}}' is AMBIGUOUS on the POINTER {} of type '{}' - it could "
            "mean the null pointer, or a pointer to a default-constructed '{}'; write '= default' "
            "(valid for any type, including a generic parameter substituted to a pointer) or "
            "'nullptr'{}",
            what, DescribePointerDeclType(tv), tv.TypeName, alloc));
    }

void MainListener::EmitFieldInitializer(
        llvm::Value* structPtr,
        const std::string& typeName,
        CFlatParser::InitializerListContext* ctx) {
        auto* compiler = Compiler(ctx);
        auto it = compiler->dataStructures.find(typeName);
        if (it == compiler->dataStructures.end())
        {
            LogErrorContext(ctx, std::format("Field initializer: '{}' is not a known struct type", typeName));
            return;
        }
        const auto& sd = it->second;

        std::unordered_set<std::string> seen;
        for (auto* fi : ctx->fieldInit())
        {
            // Struct field initializers are named-only ({field = value}). Positional
            // ({value}) and key:value ({k: v}) forms are reserved for array/list/dictionary
            // targets and are routed away before reaching here; reject them for structs.
            if (fi->Identifier() == nullptr)
            {
                LogErrorContext(fi, std::format(
                    "positional initializers are not supported for struct type '{}'; use 'field = value'", typeName));
                continue;
            }
            std::string fieldName = fi->Identifier()->getText();
            if (!seen.insert(fieldName).second)
            {
                LogErrorContext(fi, std::format("Duplicate field initializer for '{}'", fieldName));
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
                auto* storagePtr = compiler->builder->CreateStructGEP(
                    sd.StructType, structPtr, bfHit->StorageFieldIndex, fieldName + "_bf");

                unsigned w = bfHit->BitWidth;
                unsigned off = bfHit->BitOffset;
                uint64_t valMask = (w == 64) ? ~uint64_t(0) : ((uint64_t(1) << w) - 1);
                uint64_t windowMask = valMask << off;
                auto* valAsStorage = compiler->CreateCast(val, storageTy, !bfHit->IsUnsigned);
                auto* valMasked = compiler->builder->CreateAnd(valAsStorage, llvm::ConstantInt::get(storageTy, valMask));
                auto* valShifted = compiler->builder->CreateShl(valMasked, llvm::ConstantInt::get(storageTy, off));
                auto* word = compiler->builder->CreateLoad(storageTy, storagePtr);
                auto* cleared = compiler->builder->CreateAnd(word, llvm::ConstantInt::get(storageTy, ~windowMask));
                auto* newWord = compiler->builder->CreateOr(cleared, valShifted);
                compiler->builder->CreateStore(newWord, storagePtr);
                continue;
            }

            // Thread the target field's closure signature into a lambda RHS so its return type is
            // inferred as `s.f = (int n) => ...` does. Spelled and ENCODED field types both.
            for (const auto& field : sd.StructFields)
            {
                if (field.VariableName != fieldName) continue;
                if (field.IsFunctionPointer) lambdaExpectedType = field;
                else if (const auto* enc = compiler->GetEncodedClosureType(field.TypeName))
                    lambdaExpectedType = *enc;
                break;
            }
            auto rightNV = ParseAssignmentExpressionNamed(fi->assignmentExpression(0));
            lambdaExpectedType = {};
            rightNV.CastOccurrenceId = thisCastOcc;
            compiler->EndCastOccurrence(savedCastOcc);
            EmitOneFieldInit(structPtr, sd, typeName, fieldName, rightNV, fi);
        }
    }

void MainListener::CoerceElementToString(
        LLVMBackend* compiler,
        LLVMBackend::NamedVariable& nv,
        llvm::Value*& val,
        antlr4::ParserRuleContext* ctx) {
        if (!val || val->getType() != compiler->builder->getInt8Ty()->getPointerTo())
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
        allocList.push_back(std::pair(name, arrAlloc));
        EmitPositionalFixedArrayIntoSlot(name, tv, initList, arrAlloc);
    }

void MainListener::EmitPositionalFixedArrayIntoSlot(
        const std::string& name,
        const LLVMBackend::TypeAndValue& tv,
        CFlatParser::InitializerListContext* initList,
        llvm::Value* arrAlloc) {
        auto* compiler = Compiler(initList);
        auto elements = initList->fieldInit();
        uint64_t n = tv.ConstArraySize;

        if (elements.size() > n)
        {
            LogErrorContext(initList, std::format(
                "too many initializers for '{}[{}]': got {} elements", tv.TypeName, n, elements.size()));
            return;
        }

        auto* arrTy = llvm::cast<llvm::ArrayType>(compiler->GetType(tv));
        auto* elemTy = arrTy->getElementType();
        // Zero-fill the whole array so unspecified trailing slots are well-defined.
        compiler->builder->CreateStore(llvm::Constant::getNullValue(arrTy), arrAlloc);

        // The ELEMENT's type is what a stored value has to agree with, not the array's.
        LLVMBackend::TypeAndValue fixedElemTV = tv;
        int fixedElemTVStars = FixedArrayElementStars(tv);
        fixedElemTV.Pointer = fixedElemTVStars >= 1;
        fixedElemTV.ElemPointer = fixedElemTVStars >= 2;
        fixedElemTV.ConstArraySize = 0;
        fixedElemTV.IsArrayView = false;

        llvm::Value* zero = compiler->builder->getInt32(0);
        for (size_t i = 0; i < elements.size(); i++)
        {
            auto* fi = elements[i];
            auto nv = ParseAssignmentExpressionNamed(fi->assignmentExpression(0));
            llvm::Value* val = LoadNamedVariable(nv);
            if (!val) continue;

            // Aggregate leg of the code-value store gate: `Rec*[2] arr = { w, w };`.
            RejectCodeValueIntoDataSlot(fi, nv, fixedElemTV, "brace-initialize",
                                        std::format("element {} of '{}' of", i, name));

            // Thin sibling of the widen elsewhere: positional fixed-array brace-init into a
            // thin function<> element fed a raw pointer (spelled or generic-encoded).
            {
                const LLVMBackend::TypeAndValue* clo = fixedElemTV.IsFunctionPointer
                    ? &fixedElemTV : compiler->GetEncodedClosureType(fixedElemTV.TypeName);
                if (clo && clo->IsThinFnPtr() && !fixedElemTV.Pointer && fixedElemTV.ConstArraySize == 0
                    && val->getType()->isPointerTy() && !val->getType()->isStructTy())
                    compiler->CheckThinFnPtrAssignProvenance(val, nv,
                        std::format("'{}[{}]'", name, i));
            }

            // Array-aggregate leg of the temp-unique-field escape. This lowering is NOT
            // EmitOneFieldInit, so the struct brace-init leg could never see it.
            if (IsOwningTempUniqueFieldEscape(nv))
                RejectOwningTempUniqueFieldEscape(
                    nv, std::format("element {} of array '{}'", i, name), fi);

            if (tv.TypeName == "string"
                && val->getType() == compiler->builder->getInt8Ty()->getPointerTo())
            {
                LLVMBackend::NamedVariable tmp = nv;
                CoerceElementToString(compiler, tmp, val, fi);
            }
            // Interface element: same boxing/rebox the struct-field brace path applies.
            if (!tv.ElemPointer && compiler->IsInterfaceType(tv.TypeName))
                val = CoerceInitValueToInterface(nv, val, tv.TypeName, fi);

            llvm::Value* idx = compiler->builder->getInt32((uint32_t)i);
            auto* elemPtr = compiler->builder->CreateInBoundsGEP(arrTy, arrAlloc, { zero, idx }, "arrelem");
            compiler->CreateAssignment(val, elemPtr, false, elemTy);
        }
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

std::string MainListener::DescribeArrayShape(const LLVMBackend::TypeAndValue& tv) {
        // A simd element keeps its own spelling: TypeName is only the LANE type, so without this
        // a simd<float,4>[2] reads as 'float[2]'. Wording only - the shape is unchanged.
        std::string elem = tv.IsSimd ? std::format("simd<{},{}>", tv.TypeName, tv.SimdLanes)
                                     : tv.TypeName;
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
                destText, srcTypeName, destTV.TypeName, destTV.VariableName));
            return;
        }

        int destStars = FixedArrayElementStars(destTV);
        int srcStars = (srcPointer ? 1 : 0) + (srcElemPointer ? 1 : 0);
        std::string srcText = DescribeArrayShape(srcTypeName, srcStars, srcConstArraySize,
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
                destText, destTV.TypeName));
            return;
        }

        uint64_t bytes = compiler->module->getDataLayout().getTypeAllocSize(destArrTy);
        compiler->builder->CreateMemCpy(destAlloc, llvm::MaybeAlign(), srcPtr,
                                        llvm::MaybeAlign(), bytes);
    }

void MainListener::EmitFixedArrayDefaultInit(llvm::Value* arrAlloc, const LLVMBackend::TypeAndValue& tv) {
        auto* compiler = Compiler();
        auto structData = compiler->GetDataStructure(tv.TypeName);
        auto* arrTy = llvm::dyn_cast_or_null<llvm::ArrayType>(compiler->GetType(tv));
        if (structData.StructType == nullptr || arrTy == nullptr) return;

        // Total slots: the outer extent times every inner dimension (T[N][M] is N*M structs).
        uint64_t n = tv.ConstArraySize;
        for (uint64_t d : tv.ConstInnerDimensions) n *= d;
        if (n == 0) return;

        // An owning default (its constructor allocates a resource) must be built independently in
        // each slot: a single seed replicated bitwise would alias one resource across all N slots
        // and double-free at teardown. The walk is shared with the destruction path so the two
        // traversals cannot drift.
        if (compiler->GetFunction(tv.TypeName) && compiler->IsOwningValueType(tv.TypeName))
        {
            compiler->EmitFixedArrayElementWalk(
                *compiler->builder, arrAlloc, structData.StructType, n,
                [&](llvm::Value* elemPtr)
                {
                    llvm::Value* elem = compiler->CreateOverloadedFunctionCall(tv.TypeName, {});
                    compiler->CreateAssignment(elem, elemPtr);
                });
            return;
        }

        llvm::Value* seedVal = compiler->GetFunction(tv.TypeName)
            ? compiler->CreateOverloadedFunctionCall(tv.TypeName, {})
            : nullptr;

        // No default constructor, or one that folds to all-zero: a single store of the whole
        // array, which lowers to one memset regardless of N.
        auto* seedConst = llvm::dyn_cast_or_null<llvm::Constant>(seedVal);
        if (seedVal == nullptr || (seedConst != nullptr && seedConst->isNullValue()))
        {
            compiler->builder->CreateStore(llvm::Constant::getNullValue(arrTy), arrAlloc);
            return;
        }

        // A non-owning non-trivial default (plain field initializers, no owned resource) is safe
        // to replicate bitwise: seed one element and copy it into every slot.
        auto* seedAlloc = compiler->AllocaAtEntry(structData.StructType, nullptr, "arrdefseed");
        compiler->CreateAssignment(seedVal, seedAlloc);
        uint64_t elemBytes = compiler->module->getDataLayout().getTypeAllocSize(structData.StructType);

        compiler->EmitFixedArrayElementWalk(
            *compiler->builder, arrAlloc, structData.StructType, n,
            [&](llvm::Value* elemPtr)
            {
                compiler->builder->CreateMemCpy(
                    elemPtr, llvm::MaybeAlign(), seedAlloc, llvm::MaybeAlign(), elemBytes);
            });
    }

llvm::Constant* MainListener::CoerceConstantToArrayElement(
        LLVMBackend* compiler, llvm::Constant* c, llvm::Type* elemTy) {
        if (c->getType() == elemTy) return c;
        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(c))
        {
            if (elemTy->isIntegerTy())
                return llvm::ConstantInt::get(elemTy, ci->getZExtValue());
            if (elemTy->isFloatingPointTy())
                return llvm::ConstantFP::get(elemTy, (double)ci->getSExtValue());
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
        antlr4::ParserRuleContext* errCtx) {
        auto* compiler = Compiler(errCtx);
        auto* arrTy = llvm::cast<llvm::ArrayType>(compiler->GetType(tv));
        auto* elemTy = arrTy->getElementType();
        uint64_t n = tv.ConstArraySize;

        bool emptyInit = (initList == nullptr || initList->fieldInit().empty());
        llvm::Constant* arrConst = nullptr;

        if (emptyInit)
        {
            // Empty `{}` -> zero-init, same as `= default`.
            arrConst = llvm::Constant::getNullValue(arrTy);
        }
        else if (positionalArray)
        {
            auto elements = initList->fieldInit();
            if (elements.size() > n)
            {
                LogErrorContext(initList, std::format(
                    "too many initializers for '{}[{}]': got {} elements", tv.TypeName, n, elements.size()));
                return;
            }

            // Evaluate element expressions inside a throwaway function so stray IR does
            // not corrupt the current insert block; each must fold to a constant.
            auto savedState = compiler->SaveBuilderState();
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
            // Code-value store reject, RECORDED not raised: LogErrorContext throws, and the
            // builder is redirected into a throwaway function here, so unwinding from inside the
            // loop would leave the insert point in `tmpFn` and skip the restore below.
            CFlatParser::FieldInitContext* codeValueElem = nullptr;
            size_t codeValueIndex = 0;
            for (size_t i = 0; i < elements.size(); i++)
            {
                auto* fi = elements[i];
                auto nv = ParseAssignmentExpressionNamed(fi->assignmentExpression(0));
                llvm::Value* val = LoadNamedVariable(nv);
                if (codeValueElem == nullptr
                    && compiler->CodeValueIntoDataDestination(nv, globalElemTV))
                {
                    codeValueElem = fi;
                    codeValueIndex = i;
                    ok = false;
                    break;
                }
                auto* c = llvm::dyn_cast_or_null<llvm::Constant>(val);
                if (c) c = CoerceConstantToArrayElement(compiler, c, elemTy);
                if (!c)
                {
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
            compiler->RestoreBuilderState(savedState);

            if (codeValueElem != nullptr)
                LogErrorContext(codeValueElem, compiler->DescribeCodeValueIntoData(
                    CodeValueDestSpelling(globalElemTV), "brace-initialize",
                    CodeValueCastAdvice(globalElemTV),
                    std::format("element {} of '{}' of", codeValueIndex, name)));
            if (!ok) return;
            arrConst = llvm::ConstantArray::get(arrTy, elems);
        }
        else
        {
            LogErrorContext(errCtx, std::format(
                "global array initializer for '{}' must be positional '{{v0, v1, ...}}' or empty '{{}}'; struct field-init '{{field=v}}' at global scope is not supported",
                tv.TypeName));
            return;
        }

        compiler->CreateGlobalVariable(tv, arrConst);

        // Register the global for LSP/--symbol, matching the scalar global path.
        if (auto* s = compiler->GetSymbolSink())
        {
            int dl = (int)direct->getStart()->getLine();
            int dc = (int)direct->getStart()->getCharPositionInLine();
            s->RegisterVariable(name, tv.TypeName, compiler->GetSourceFilePath(), dl, dc);
            s->Register(SymbolKind::Variable, name, compiler->GetSourceFilePath(), dl, dc,
                        std::format("{}[{}] {}", tv.TypeName, n, name));
        }
    }

bool MainListener::TryFoldConstInt(llvm::Value* v, uint64_t& out,
                                const std::unordered_set<std::string>* constGlobals) {
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
                            && constGlobals->count(std::string(gv->getName())) == 0)
                            return false;
                        out = (uint64_t)ci->getSExtValue();
                        return true;
                    }
            return false;
        }
        if (auto* bo = llvm::dyn_cast<llvm::BinaryOperator>(v))
        {
            uint64_t l = 0, r = 0;
            if (!TryFoldConstInt(bo->getOperand(0), l, constGlobals) || !TryFoldConstInt(bo->getOperand(1), r, constGlobals))
                return false;
            switch (bo->getOpcode())
            {
                case llvm::Instruction::Add:  out = l + r; return true;
                case llvm::Instruction::Sub:  out = l - r; return true;
                case llvm::Instruction::Mul:  out = l * r; return true;
                case llvm::Instruction::SDiv:
                case llvm::Instruction::UDiv: if (r == 0) return false; out = l / r; return true;
                case llvm::Instruction::SRem:
                case llvm::Instruction::URem: if (r == 0) return false; out = l % r; return true;
                case llvm::Instruction::Shl:  out = l << r; return true;
                // Right shifts are width-aware: leaves are carried sign-extended to 64 bits, so a
                // bare `>>` would fill from bit 63 (wrong for a narrower operand). Fold at the
                // instruction's own width W, then re-sign-extend the W-bit result to 64.
                case llvm::Instruction::LShr:
                case llvm::Instruction::AShr:
                {
                    unsigned W = bo->getType()->getIntegerBitWidth();
                    uint64_t mask = (W < 64) ? (((uint64_t)1 << W) - 1) : ~(uint64_t)0;
                    uint64_t signBitW = (uint64_t)1 << (W - 1);
                    bool isAShr = bo->getOpcode() == llvm::Instruction::AShr;
                    uint64_t res;
                    // Over-shift (r >= W, or a negative r that sign-extended to a huge value) is UB in
                    // C++ `>>` and poison at the source level. Fold it deterministically: LShr -> 0,
                    // AShr -> all-ones-in-W when the operand's sign bit is set, else 0.
                    if (r >= W)
                        res = (isAShr && (l & signBitW)) ? mask : 0;
                    else if (!isAShr)
                        res = (l & mask) >> r;                       // zero-fill within W bits
                    else
                    {
                        // Arithmetic shift: interpret l as a W-bit signed value (already sext'd) and
                        // shift, filling from the sign bit.
                        res = (uint64_t)((int64_t)l >> r);
                    }
                    // Sign-extend the W-bit result back to 64 so downstream signed compares are valid.
                    if (W < 64)
                    {
                        res &= mask;
                        if (res & signBitW) res |= ~mask;
                    }
                    out = res;
                    return true;
                }
                case llvm::Instruction::And:  out = l & r; return true;
                case llvm::Instruction::Or:   out = l | r; return true;
                case llvm::Instruction::Xor:  out = l ^ r; return true;
                default: return false;
            }
        }
        if (auto* cast = llvm::dyn_cast<llvm::CastInst>(v))
        {
            // Trunc / SExt pass the sign-extended operand through unchanged (SExt of a value
            // already sign-extended from its own width is a no-op at 64 bits).
            if (cast->getOpcode() == llvm::Instruction::Trunc ||
                cast->getOpcode() == llvm::Instruction::SExt)
                return TryFoldConstInt(cast->getOperand(0), out, constGlobals);
            // ZExt must discard the sign bits the leaf fold added: re-zero above the source width.
            if (cast->getOpcode() == llvm::Instruction::ZExt)
            {
                if (!TryFoldConstInt(cast->getOperand(0), out, constGlobals)) return false;
                unsigned srcBits = cast->getSrcTy()->getIntegerBitWidth();
                if (srcBits < 64) out &= ((uint64_t)1 << srcBits) - 1;
                return true;
            }
            return false;
        }
        if (auto* cmp = llvm::dyn_cast<llvm::ICmpInst>(v))
        {
            uint64_t l = 0, r = 0;
            if (!TryFoldConstInt(cmp->getOperand(0), l, constGlobals) || !TryFoldConstInt(cmp->getOperand(1), r, constGlobals))
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
        if (auto* sel = llvm::dyn_cast<llvm::SelectInst>(v))
        {
            uint64_t c = 0;
            if (!TryFoldConstInt(sel->getCondition(), c, constGlobals))
                return false;
            return TryFoldConstInt(c ? sel->getTrueValue() : sel->getFalseValue(), out, constGlobals);
        }
        return false;
    }

llvm::ConstantInt* MainListener::EvalGlobalArrayDim(CFlatParser::AssignmentExpressionContext* expr) {
        auto* compiler = Compiler(expr);
        auto savedState = compiler->SaveBuilderState();
        auto* voidTy = llvm::FunctionType::get(compiler->builder->getVoidTy(), false);
        auto* tmpFn = llvm::Function::Create(
            voidTy, llvm::Function::PrivateLinkage, "__global_arr_dim_tmp", compiler->module.get());
        auto* tmpBB = llvm::BasicBlock::Create(*compiler->context, "entry", tmpFn);
        compiler->builder->SetInsertPoint(tmpBB);

        llvm::Value* sizeVal = ParseAssignmentExpression(expr);
        uint64_t folded = 0;
        bool ok = sizeVal && TryFoldConstInt(sizeVal, folded);

        tmpFn->eraseFromParent();
        compiler->RestoreBuilderState(savedState);

        auto* i64Ty = llvm::Type::getInt64Ty(*compiler->context);
        if (!ok)
        {
            LogErrorContext(expr, "global array size must be a compile-time constant");
            return llvm::ConstantInt::get(i64Ty, 1);
        }
        return llvm::ConstantInt::get(i64Ty, folded);
    }

void MainListener::EmitArrayViewInferredInit(
        const std::string& name,
        const LLVMBackend::TypeAndValue& tv,
        CFlatParser::InitializerListContext* initList,
        size_t line,
        std::vector<std::pair<std::string, llvm::AllocaInst*>>& allocList) {
        auto* compiler = Compiler(initList);
        std::vector<CFlatParser::FieldInitContext*> elements;
        if (initList != nullptr)
            elements = initList->fieldInit();

        if (elements.empty())
        {
            compiler->LogError(std::format(
                "cannot infer the length of '{}[]' from an empty initializer list; use an explicit size '{}[N]'",
                tv.TypeName, tv.TypeName));
            return;
        }

        // Every element must be positional (no `field =` or `key: value`).
        for (auto* fi : elements)
        {
            if (fi->Identifier() != nullptr || fi->assignmentExpression().size() != 1)
            {
                LogErrorContext(fi, std::format(
                    "'{}[]' uses a positional initializer list; named or 'key: value' elements are not allowed",
                    tv.TypeName));
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
            auto nv = ParseAssignmentExpressionNamed(fi->assignmentExpression(0));
            llvm::Value* val = LoadNamedVariable(nv);
            if (!val) continue;

            // Aggregate leg of the code-value store gate: `Rec*[] v = { w, w };`.
            RejectCodeValueIntoDataSlot(fi, nv, elemTV, "brace-initialize",
                                        std::format("element {} of '{}' of", i, name));

            // Thin sibling of the widen elsewhere: array-view brace-init into a thin
            // function<> element fed a raw pointer (spelled or generic-encoded).
            {
                const LLVMBackend::TypeAndValue* clo = elemTV.IsFunctionPointer
                    ? &elemTV : compiler->GetEncodedClosureType(elemTV.TypeName);
                if (clo && clo->IsThinFnPtr() && !elemTV.Pointer && elemTV.ConstArraySize == 0
                    && val->getType()->isPointerTy() && !val->getType()->isStructTy())
                    compiler->CheckThinFnPtrAssignProvenance(val, nv,
                        std::format("'{}[{}]'", name, i));
            }

            // Array-VIEW twin of the fixed-array leg above; a separate lowering reached by
            // neither the fixed path nor EmitOneFieldInit.
            if (IsOwningTempUniqueFieldEscape(nv))
                RejectOwningTempUniqueFieldEscape(
                    nv, std::format("element {} of array '{}'", i, name), fi);

            if (tv.TypeName == "string"
                && val->getType() == compiler->builder->getInt8Ty()->getPointerTo())
            {
                LLVMBackend::NamedVariable tmp = nv;
                CoerceElementToString(compiler, tmp, val, fi);
            }
            // Interface element: same boxing/rebox the struct-field brace path applies.
            if (!elemTV.Pointer && compiler->IsInterfaceType(elemTV.TypeName))
                val = CoerceInitValueToInterface(nv, val, elemTV.TypeName, fi);

            llvm::Value* idx = compiler->builder->getInt32((uint32_t)i);
            auto* elemPtr = compiler->builder->CreateInBoundsGEP(arrTy, backing, { zero, idx }, "arrview_elem");
            compiler->CreateAssignment(val, elemPtr, false, elemTy);
        }

        // Create the T* local and point it at element 0 of the backing array.
        auto* alloc = compiler->CreateLocalVariable(tv, nullptr, nullptr, line);
        allocList.push_back(std::pair(name, alloc));
        auto* elem0 = compiler->builder->CreateInBoundsGEP(arrTy, backing, { zero, zero }, "arrview_data");
        compiler->builder->CreateStore(elem0, alloc);
    }

bool MainListener::TryEmitContainerInitializer(
        llvm::Value* alloc,
        const LLVMBackend::TypeAndValue& tv,
        CFlatParser::InitializerListContext* initList) {
        auto* compiler = Compiler(initList);
        const std::string& typeName = tv.TypeName;

        const bool isList  = typeName.rfind("list__", 0) == 0;
        const bool isArray = typeName.rfind("array__", 0) == 0;
        const bool isDict  = typeName.rfind("dictionary__", 0) == 0;
        if (!isList && !isArray && !isDict)
            return false;

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
            // Extract K and V from the mangled name `dictionary__K__V` when it splits cleanly
            // into exactly two parts (covers primitives and string); otherwise skip coercion.
            std::string keyType, valType;
            {
                std::string args = typeName.substr(12);  // strlen("dictionary__")
                size_t sep = args.find("__");
                if (sep != std::string::npos && args.find("__", sep + 2) == std::string::npos)
                {
                    keyType = args.substr(0, sep);
                    valType = args.substr(sep + 2);
                }
            }
            for (auto* fi : elements)
            {
                if (fi->Identifier() != nullptr || fi->assignmentExpression().size() != 2)
                {
                    LogErrorContext(fi, std::format(
                        "dictionary initializer requires 'key: value' pairs for type '{}'", typeName));
                    continue;
                }
                LLVMBackend::NamedVariable keyNV, valNV;
                if (!parseElement(fi->assignmentExpression(0), keyType, keyNV)) continue;
                if (!parseElement(fi->assignmentExpression(1), valType, valNV)) continue;
                compiler->CreateOverloadedFunctionCall("set", { makeSelf(), keyNV, valNV });
            }
            return true;
        }

        // list<T> / array<T>: every element must be positional.
        const std::string elemType = isList
            ? typeName.substr(6)   // strlen("list__")
            : typeName.substr(7);  // strlen("array__")

        for (auto* fi : elements)
        {
            if (fi->Identifier() != nullptr || fi->assignmentExpression().size() != 1)
            {
                LogErrorContext(fi, std::format(
                    "'{}' uses a positional initializer list; named or 'key: value' elements are not allowed",
                    typeName));
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
                compiler->CreateOverloadedFunctionCall("set", { makeSelf(), idxNV, elemNV });
            }
            return true;
        }

        // list<T>: append each element in order.
        for (auto* fi : elements)
        {
            LLVMBackend::NamedVariable elemNV;
            if (!parseElement(fi->assignmentExpression(0), elemType, elemNV)) continue;
            compiler->CreateOverloadedFunctionCall("add", { makeSelf(), elemNV });
        }
        return true;
    }

LLVMBackend::NamedVariable MainListener::ParseNewExpression(CFlatParser::NewExpressionContext* ctx) {
        auto* compiler = Compiler(ctx);
        std::string typeName = ParseTypeSpecifierName(ctx->typeSpecifier());
        bool isArray = ctx->assignmentExpression() != nullptr;

        // If type substitution produced a pointer type (e.g. T=Employee*), strip the '*'
        // and treat as pointer-to-element so GetType resolves the base type correctly.
        bool typeIsPtr = false;
        while (!typeName.empty() && typeName.back() == '*')
        {
            typeName.pop_back();
            typeIsPtr = true;
        }

        LLVMBackend::TypeAndValue typeInfo{ .TypeName = typeName, .Pointer = typeIsPtr };
        llvm::Type* elemType = compiler->GetType(typeInfo);

        // Compute allocation size
        if (!elemType || !elemType->isSized())
        {
            LogErrorContext(ctx, std::format("'new': cannot compute size of unsized or unresolved type '{}'", typeName));
            return {};
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
        if (!typeName.empty() && compiler->GetFunction(opNewName))
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

        llvm::Type* ptrTy = elemType->getPointerTo();
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

        // For array new of a class type: call default constructor for each element (like C++).
        // Skip when typeIsPtr - the element is a pointer (e.g. Point*), not a struct; calling
        // the struct ctor would store sizeof(Point)=8 bytes into a sizeof(ptr)=4-byte slot on
        // Win32, corrupting the heap (on Win64 they happen to be equal so the bug is silent).
        if (isArray && count && !typeIsPtr && compiler->GetFunction(typeName))
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
            llvm::Value* structVal = compiler->CreateOverloadedFunctionCall(typeName, {});
            if (structVal)
                compiler->builder->CreateStore(structVal, elemPtr);
            auto* next = compiler->builder->CreateAdd(idx2, compiler->builder->getInt64(1));
            compiler->builder->CreateStore(next, indexAlloca);
            compiler->builder->CreateBr(condBB);

            compiler->builder->SetInsertPoint(afterBB);
        }

        // For non-array new of a class type: call constructor and store result
        if (!isArray && compiler->GetFunction(typeName))
        {
            std::vector<LLVMBackend::NamedVariable> ctorArgs;
            auto argList = ctx->argumentExpressionList();
            if (argList != nullptr)
            {
                for (auto* namedArg : argList->argumentNamedExpression())
                {
                    // Occurrence-scope this ONE constructor argument (see codeValueDataCasts_),
                    // same as the direct-call argument loop.
                    size_t savedCastOcc = compiler->BeginCastOccurrence();
                    size_t thisCastOcc = compiler->CurrentCastOccurrence();
                    llvm::Value* argVal = ParseAssignmentExpression(namedArg->assignmentExpression());
                    compiler->EndCastOccurrence(savedCastOcc);
                    if (!argVal) break;
                    LLVMBackend::NamedVariable argVar;
                    argVar.Primary = argVal;
                    argVar.BaseType = argVal->getType();
                    argVar.CastOccurrenceId = thisCastOcc;
                    ctorArgs.push_back(argVar);
                }
            }
            llvm::Value* structVal = compiler->CreateOverloadedFunctionCall(typeName, ctorArgs);
            if (structVal)
                compiler->builder->CreateStore(structVal, typedPtr);
        }

        // Apply field initializer: new Type { field=val, ... }
        if (auto* initList = ctx->initializerList())
        {
            // Reachable only when substitution made the element a pointer ('new T{...}' with
            // T=S*): 'typedPtr' then addresses a POINTER slot, not an 'S'.
            if (typeIsPtr)
                // The element type IS 'typeName*' here; allocating one 'typeName' is the remedy.
                LogPointerBraceInitReject(ctx, std::format("element of 'new {}*'", typeName), typeName,
                    typeName + "*", true);
            EmitFieldInitializer(typedPtr, typeName, initList);
        }

        // [winrt] object: wire lpVtbl -> static vtable and set refcount = 1 after the ctor ran.
        bool isWinrtNew = !isArray && !typeIsPtr && compiler->IsWinrtClass(typeName);
        if (isWinrtNew)
            compiler->WireWinrtObject(typedPtr, typeName);

        LLVMBackend::NamedVariable result;
        result.TypeAndValue.TypeName = typeName;
        result.TypeAndValue.Pointer = true;
        // `new T[n]` yields a thin array-view: a fresh, whole, distinct allocation - the blessed
        // way to obtain an `int[]`. Decays implicitly to `T*` when stored into a pointer lvalue.
        result.TypeAndValue.IsArrayView = isArray;
        result.Primary = typedPtr;
        result.BaseType = ptrTy;
        // A free site does not need the alignment VALUE - only which deallocator to use, since
        // __delete_aligned recovers the raw block without it. So the tag is needed only when the
        // element TYPE alone would NOT already route there: an over-aligned type (`struct
        // alignas(64) T`) makes both `new` and `delete` pick the aligned pair off the static
        // type, exactly as in C++, so it escapes into fields/containers/returns untagged. Only a
        // per-site `alignas(N)` on an ordinarily-aligned type is invisible to every free site.
        bool typeAlreadyAligned = effAlign > LLVMBackend::kDefaultNewAlign;
        uint64_t siteExcess = (useAligned && !typeAlreadyAligned) ? allocAlign : 0;
        result.AllocAlignment = siteExcess;
        // A COM object's lifetime is refcounted via Release, not owning-pointer auto-free, so the
        // caller must NOT auto-delete it at scope exit.
        compiler->lastOwningResult = !isWinrtNew;
        compiler->lastAllocAlignment = siteExcess;
        // Ledger the result BY VALUE so an owning `new` reaching an assignment through a
        // transparent wrapper (a '?:' arm) is still recognized as owning. See ownedNewTemps_.
        if (!isWinrtNew && !isArray)
            compiler->RegisterOwnedNewTemp(typedPtr, typeName, siteExcess);
        // Ledger the concrete class too: a '?:' phi carries no TypeName, and the interface
        // upcast needs one per arm. See valueElementTypeNames_.
        if (!isArray)
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
                   && castCtx->typeName() != nullptr
                   && castCtx->castExpression() != nullptr)
            {
                if (!sawCast)
                {
                    auto destTV  = ParseTypeName(castCtx->typeName());
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
            typeName  = namedVar.TypeAndValue.TypeName;
            elemIsPtr = namedVar.TypeAndValue.ElemPointer;
            operandAllocAlign = namedVar.AllocAlignment;
            // An interface value is a {vtable, data} fat struct, not a raw pointer; flag it so
            // the free path below extracts the data pointer and dispatches the dtor virtually.
            isInterfaceOperand = namedVar.TypeAndValue.IsFatInterfaceValue() && !sawCast;

            // Error: 'delete' on a value-type local (a struct VALUE, not a pointer). Value
            // types are auto-destructed at scope exit, so this is always a user error; without
            // this check the operand flows into icmp/call/bitcast instructions that require a
            // pointer and LLVM's module verifier fails with no source location.
            if (!namedVar.TypeAndValue.Pointer && !isInterfaceOperand)
            {
                std::string displayType = typeName;
                if (size_t d = displayType.find("__"); d != std::string::npos)
                {
                    std::string base = displayType.substr(0, d);
                    std::string args = displayType.substr(d + 2);
                    std::string joined;
                    for (size_t pos = 0; pos <= args.size(); )
                    {
                        size_t sep = args.find("__", pos);
                        if (sep == std::string::npos) { joined += args.substr(pos); break; }
                        joined += args.substr(pos, sep - pos) + ", ";
                        pos = sep + 2;
                    }
                    displayType = base + "<" + joined + ">";
                }
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
                    "cannot delete array-view '{}' - it was bound from {}, not a heap allocation; "
                    "'delete' would hand free() a non-heap address. Only a view from 'new {}[n]' "
                    "can be deleted.",
                    name, boundFrom, namedVar.TypeAndValue.TypeName));
                return {};
            }

            // A cast target renames the element type; honor it for destructor selection.
            if (sawCast)
            {
                typeName  = castTypeName;
                elemIsPtr = castElemPtr;
            }

            // Error: deleting the DIRECT result of a call whose callee returns 'alias' (a borrow the
            // owner retains, e.g. list.get's `alias T get`). The owner still holds the pointer and
            // frees it at its own dtor - deleting it here double-frees. A call result has null Storage
            // (Primary holds the value); a named local has alloca Storage and is not flagged, so
            // 'T* p = ...; delete p;' stays allowed. Covers the parenthesized/cast-laundered forms too.
            if (namedVar.TypeAndValue.IsAlias && namedVar.Storage == nullptr)
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
            if (!namedVar.IsOwning
                && namedVar.BorrowsOwnedElement
                && namedVar.Storage != nullptr
                && llvm::isa<llvm::AllocaInst>(namedVar.Storage))
            {
                std::string name = namedVar.CallerName.empty()
                    ? namedVar.TypeAndValue.VariableName : namedVar.CallerName;
                std::string owner = namedVar.OwnedElementContainer.empty()
                    ? "its container" : "'" + namedVar.OwnedElementContainer + "'";
                if (namedVar.BorrowedElementExternallyOwned)
                {
                    // Container-agnostic: any 'alias'-element container (list<alias T*>,
                    // queue<alias T*>, ...) hands back pure borrows owned elsewhere.
                    LogErrorContext(ctx, std::format(
                        "cannot delete '{}' - {} declares its elements 'alias' (a pure borrow owned "
                        "elsewhere), so deleting this double-frees when the real owner releases it. Do "
                        "not delete a borrow, or use a 'unique'-element container if it should own them.",
                        name, owner));
                    return {};
                }
                LogErrorContext(ctx, std::format(
                    "cannot delete '{}' - it borrows an element that {} owns, whose "
                    "destructor already frees it, so this is a double-free. Remove and free through "
                    "the owner instead (e.g. the container's removeAt/erase), or do not delete a borrow.",
                    name, owner));
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
                && compiler->IsFunctionParameter(namedVar.CallerName))
            {
                LogErrorContext(ctx, std::format(
                    "cannot delete borrowed parameter '{}' - caller may own this pointer and "
                    "will free it on scope exit. Declare the parameter 'move {}' to take ownership.",
                    namedVar.CallerName, namedVar.CallerName));
                return {};
            }

            // Error: deleting a local that aliases a borrowed parameter (e.g. via cast).
            // 'Payload* p = (Payload*)raw; delete p;' is the laundered form of the case above.
            // Exempt field accesses (storage is a GEP, not an alloca): 'delete param->field'
            // is intentionally allowed - the field's lifetime is the caller's contract, just
            // like the simple-variable check above.
            if (!namedVar.IsOwning
                && namedVar.IsBorrowed
                && !namedVar.BorrowedOrigin.empty()
                && namedVar.Storage != nullptr
                && llvm::isa<llvm::AllocaInst>(namedVar.Storage))
            {
                std::string name = namedVar.CallerName.empty() ? namedVar.BorrowedOrigin : namedVar.CallerName;
                // Trap B: the borrow came from a `unique` field, not a parameter. The field's
                // synthesized destructor is the owner, so name it and point at `move` instead.
                if (!namedVar.BorrowedUniqueField.empty())
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
            if (namedVar.TypeAndValue.IsUnique)
            {
                // A `unique` LOCAL (alloca storage, no field name) is freed automatically at scope
                // exit, and reassignment frees the current pointee first. Distinguish it from the
                // field case (whose storage is a GEP off `this`) so the advice fits.
                bool isLocal = namedVar.Storage != nullptr
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
                    ? SplitEnclosingStruct(compiler->GetCurrentFunctionName(), compiler)
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
                && !llvm::isa<llvm::AllocaInst>(namedVar.Storage))
            {
                std::string enclosing = SplitEnclosingStruct(compiler->GetCurrentFunctionName(), compiler);
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
                if (llvm::isa<llvm::AllocaInst>(namedVar.Storage))
                {
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
                "'delete[]' is not allowed for struct type '{}' - use 'delete[n] ptr' to call destructors or 'delete[_] ptr' to free the raw buffer", typeName));
            return {};
        }

        // 1. Call the full destructor (user dtor + member fields) if needed (non-array only).
        // Guard on null: 'delete nullptr' must be a no-op (operator delete below already
        // null-checks). Without this, the destructor would dereference a null pointer - a
        // recursive tree teardown ('delete left; delete right;' with null leaf children)
        // would crash on the first null child.
        if (!isArray && !typeName.empty())
        {
            if (auto* dtor = compiler->GetFullDestructorForDelete(typeName))
            {
                auto* nullPtr = llvm::ConstantPointerNull::get(
                    llvm::cast<llvm::PointerType>(ptrVal->getType()));
                auto* isNull  = compiler->builder->CreateICmpEQ(ptrVal, nullPtr, "del_isnull");
                auto* dtorBB  = compiler->CreateBasicBlock("del_dtor");
                auto* contBB  = compiler->CreateBasicBlock("del_dtor_cont");
                compiler->builder->CreateCondBr(isNull, contBB, dtorBB);
                compiler->builder->SetInsertPoint(dtorBB);
                compiler->builder->CreateCall(dtor, { ptrVal });
                compiler->builder->CreateBr(contBB);
                compiler->builder->SetInsertPoint(contBB);
            }
        }

        // 1b. For delete[n]: call ~T() on each element using the caller-supplied count.
        // elemIsPtr suppresses destructor calls for pointer-element arrays (e.g. list<T*> buffer).
        llvm::Value* freeBase = ptrVal;
        if (hasSizeExpr && !isRawFree && compiler->IsDataStructure(typeName) && !elemIsPtr)
        {
            llvm::Function* elemDtor = compiler->GetFullDestructorForDelete(typeName);
            if (elemDtor)
            {
                auto* i64Ty = compiler->builder->getInt64Ty();
                llvm::Value* arrCount = ParseExpression(ctx->deleteArraySize()->expression());
                arrCount = compiler->Upconvert(arrCount, i64Ty);

                LLVMBackend::TypeAndValue typeInfo{ .TypeName = typeName };
                llvm::Type* elemType = compiler->GetType(typeInfo);

                auto* indexAlloca = compiler->builder->CreateAlloca(i64Ty, nullptr, "del_i");
                compiler->builder->CreateStore(
                    compiler->builder->CreateSub(arrCount, compiler->builder->getInt64(1), "del_start"),
                    indexAlloca);

                auto* condBB  = compiler->CreateBasicBlock("del_dtor_cond");
                auto* bodyBB  = compiler->CreateBasicBlock("del_dtor_body");
                auto* afterBB = compiler->CreateBasicBlock("del_dtor_after");
                compiler->builder->CreateBr(condBB);

                compiler->builder->SetInsertPoint(condBB);
                auto* idx = compiler->builder->CreateLoad(i64Ty, indexAlloca);
                compiler->builder->CreateCondBr(
                    compiler->builder->CreateICmpSGE(idx, compiler->builder->getInt64(0)),
                    bodyBB, afterBB);

                compiler->builder->SetInsertPoint(bodyBB);
                auto* idx2    = compiler->builder->CreateLoad(i64Ty, indexAlloca);
                auto* elemPtr = compiler->builder->CreateGEP(elemType, ptrVal, idx2, "del_elem");
                compiler->builder->CreateCall(elemDtor, { elemPtr });
                compiler->builder->CreateStore(
                    compiler->builder->CreateSub(idx2, compiler->builder->getInt64(1)), indexAlloca);
                compiler->builder->CreateBr(condBB);

                compiler->builder->SetInsertPoint(afterBB);
            }
        }

        // 2. Convert free base to void*
        auto* voidPtrTy = compiler->builder->getInt8Ty()->getPointerTo();
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
        if (!typeName.empty() && compiler->GetFunction(opDelName))
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
                compiler->builder->CreateStore(
                    llvm::ConstantPointerNull::get(ptrTy), srcAlloca);
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

bool MainListener::IsBorrowedStructParameter(LLVMBackend* compiler, const std::string& name) {
        if (compiler == nullptr || name.empty()) return false;
        if (!compiler->IsFunctionParameter(name)) return false;
        auto nv = compiler->GetScopedLocalOrArgument(name);
        if (nv.TypeAndValue.Pointer || nv.TypeAndValue.IsMove || nv.IsOwningStruct) return false;
        return compiler->IsDataStructure(nv.TypeAndValue.TypeName);
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

LLVMBackend::NamedVariable MainListener::ParseMoveExpression(CFlatParser::MoveExpressionContext* ctx) {
        auto* compiler = Compiler(ctx);
        auto argNV = ParseUnaryExpression(ctx->unaryExpression());

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
        if (!argNV.OwningStructName.empty()
            && IsBorrowedStructParameter(compiler, argNV.TypeAndValue.ParentVariableName))
        {
            const std::string& parent = argNV.TypeAndValue.ParentVariableName;
            LogErrorContext(ctx, std::format(
                "cannot 'move' field '{}.{}' out of borrowed by-value parameter '{}' - the move nulls "
                "only the callee's copy, so the caller's struct still frees the pointee (double-free). "
                "Declare the parameter 'move {}' to take ownership, or 'alias {}' to borrow explicitly.",
                argNV.OwningStructName, argNV.FieldName, parent, parent, parent));
            return {};
        }

        // Reject 'move param->field' (or any field whose parent traces to a borrowed
        // parameter): the caller still owns the parent struct and will see a nulled field
        // it never agreed to surrender. Require the parent parameter to be declared 'move'.
        // Element slots (n->values[i]) are exempt: IsBorrowed only tags pointer bases, so the
        // slot GEP nulls the one real object - recovery derives ownership from the element type.
        if (!argNV.OwningStructName.empty()
            && argNV.IsBorrowed
            && !argNV.BorrowedOrigin.empty()
            && !argNV.IsElementAccess)
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
            // 'move <origin>' is the right remedy only when the struct holding the field is OWNED.
            // If it is a borrowed by-value parameter, that advice is itself a double-free.
            if (IsBorrowedStructParameter(compiler, root))
                LogErrorContext(ctx, std::format(
                    "cannot 'move' '{}' - it aliases unique field '{}', and moving the alias does not "
                    "null the field, so the field's synthesized destructor still frees the pointee. "
                    "'move {}' is not a remedy here either: '{}' is a borrowed by-value parameter, so "
                    "it would null only the callee's copy. Declare the parameter 'move {}' to take "
                    "ownership, or 'alias {}' to borrow explicitly.",
                    name, argNV.BorrowedUniqueField, argNV.BorrowedOrigin, root, root, root));
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
                argNV.CallerName.empty() ? argNV.TypeAndValue.TypeName : argNV.CallerName));
            return {};
        }

        llvm::Value* ptrVal = LoadNamedVariable(argNV);

        // move on a named struct value type: capture the value, then zero the source storage
        // to leave it in a "moved-from" (default) state - enables safe delete[n] on the source.
        // Primitive value types (int, etc.) remain a no-op.
        // An interface fat-pointer ELEMENT (`move arr[i]`) takes no deferred transfer path (that one
        // would mark the whole array moved), and an interface is not a dataStructures entry, so the
        // slot below never zeroes. Zero the {i8*,i8*} here or the array teardown double-frees it.
        if (!argNV.TypeAndValue.Pointer && argNV.IsElementAccess && argNV.Storage
            && argNV.TypeAndValue.IsFatInterfaceValue())
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
            && !IsDirectCallArgument(ctx)
            && !argNV.CallerName.empty() && compiler->IsVariableOwning(argNV.CallerName))
        {
            compiler->builder->CreateStore(
                llvm::ConstantAggregateZero::get(compiler->GetFatPtrType()), argNV.Storage);
            // Same-block deref guard only (see ExplicitlyMovedNull). The local stays plain-readable
            // as a zeroed fat pointer, exactly as a moved thin `unique R*` stays readable as null.
            compiler->MarkVariableExplicitlyMovedNull(argNV.CallerName);
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

                llvm::Type* structType = compiler->GetType(argNV.TypeAndValue);
                if (structType)
                    compiler->builder->CreateStore(
                        llvm::ConstantAggregateZero::get(structType), argNV.Storage);
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
                result.BaseType     = argNV.BaseType;
                result.TypeAndValue = argNV.TypeAndValue;
                // CallerName is deliberately NOT carried: the source is already detached and
                // zeroed, and keeping it re-marks the origin in the move tracker (false positives).
                // Carry the owning flag so 'move s' directly as a 'move string' arg TRANSFERS the buffer;
                // without it, the defensive copy in CreateOverloadedFunctionCall orphans the original.
                if (NamedVarIsString(argNV))
                    result.IsOwningString = argNV.IsOwningString
                        || (!argNV.CallerName.empty() && compiler->IsVariableOwningString(argNV.CallerName));
                return result;
            }
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

        // Null the source (field GEP or local alloca) to transfer ownership.
        if (auto* ptrTy = llvm::dyn_cast<llvm::PointerType>(ptrVal->getType()))
            compiler->builder->CreateStore(llvm::ConstantPointerNull::get(ptrTy), argNV.Storage);

        // Explicit 'move a' of a whole thin pointer local: nulled but plain-readable (only a later
        // SAME-BLOCK deref errors - see MarkVariableExplicitlyMovedNull). Excludes a field-path move.
        if (argNV.FieldName.empty() && argNV.OwningStructName.empty() && !argNV.IsElementAccess
            && !argNV.CallerName.empty())
            compiler->MarkVariableExplicitlyMovedNull(argNV.CallerName);

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
            compiler->RegisterMovedOutPtrValue(ptrVal);
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
        if (argNV.IsElementAccess)
            compiler->lastMovedFromContainerSlot = true;

        LLVMBackend::NamedVariable result;
        result.Primary        = ptrVal;
        result.Storage        = nullptr;
        result.BaseType       = ptrVal ? ptrVal->getType() : nullptr;
        result.TypeAndValue   = argNV.TypeAndValue;
        result.AllocAlignment = argNV.AllocAlignment;
        // A 'move' of a BORROWED pointer parameter transfers nothing - the caller still owns the
        // pointee. Carry the provenance so a store into a `unique` field (a deferred delete, exactly
        // like the already-blocked `delete b`) is rejected, and so a PLAIN `T*` destination declines
        // to adopt, instead of double-freeing.
        result.IsBorrowed     = srcBorrowLive;
        result.BorrowedOrigin = srcBorrowLive ? argNV.BorrowedOrigin : std::string();
        return result;
    }

LLVMBackend::NamedVariable MainListener::ParseOperatorStringExpression(CFlatParser::OperatorStringExpressionContext* ctx) {
        auto* compiler = Compiler(ctx);

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
        std::string elemType = sd->typeSpecifier()->getText();
        auto substIt = activeTypeSubstitutions.find(elemType);
        if (substIt != activeTypeSubstitutions.end())
            elemType = substIt->second;
        uint64_t lanes = 0;
        std::string err;
        if (!TryParseSimdLaneCount(sd->assignmentExpression()->getText(), lanes, err))
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
        auto identifiers = ctx->Identifier();
        auto argLists = ctx->argumentExpressionList();
        if (identifiers.empty() || argLists.empty())
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
            for (int k = 0; k < intrin->Arity; k++)
                ops.push_back(requireSimdArg(ctx, argValue(k), k, vecTy, method));
            auto* fn = llvm::Intrinsic::getDeclaration(compiler->module.get(), intrin->Id, {vecTy});
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
            auto* maxFn = llvm::Intrinsic::getDeclaration(compiler->module.get(), llvm::Intrinsic::maxnum, {vecTy});
            auto* minFn = llvm::Intrinsic::getDeclaration(compiler->module.get(), llvm::Intrinsic::minnum, {vecTy});
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
            auto* csFn = llvm::Intrinsic::getDeclaration(compiler->module.get(), llvm::Intrinsic::copysign, {vecTy});
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
                    auto* fn = llvm::Intrinsic::getDeclaration(compiler->module.get(), llvm::Intrinsic::vector_reduce_fadd, {vecTy});
                    auto* start = llvm::ConstantFP::get(elemTy, 0.0);
                    auto* call = compiler->builder->CreateCall(fn, {start, v}, "simdreduceadd");
                    call->setHasAllowReassoc(true);
                    reduced = call;
                }
                else
                {
                    auto* fn = llvm::Intrinsic::getDeclaration(compiler->module.get(), llvm::Intrinsic::vector_reduce_add, {vecTy});
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
                auto* fn = llvm::Intrinsic::getDeclaration(compiler->module.get(), rid, {vecTy});
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
                    ? (llvm::Constant*)llvm::ConstantFP::get(elemTy, (double)i)
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
            auto* fn = llvm::Intrinsic::getDeclaration(compiler->module.get(), llvm::Intrinsic::masked_load,
                {vecTy, compiler->builder->getPtrTy()});
            result.Primary = compiler->builder->CreateCall(
                fn, {elemPtr, compiler->builder->getInt32((uint32_t)al.value()), mask, passthru}, "simdloadmasked");
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
            auto* fn = llvm::Intrinsic::getDeclaration(compiler->module.get(), llvm::Intrinsic::masked_store,
                {vecTy, compiler->builder->getPtrTy()});
            compiler->builder->CreateCall(fn, {vecVal, elemPtr, compiler->builder->getInt32((uint32_t)al.value()), mask});
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
            auto* fn = llvm::Intrinsic::getDeclaration(compiler->module.get(), llvm::Intrinsic::masked_gather, {vecTy, ptrVecTy});
            result.Primary = compiler->builder->CreateCall(
                fn, {ptrVec, compiler->builder->getInt32((uint32_t)al.value()), allTrue, passthru}, "simdgather");
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
            auto* fn = llvm::Intrinsic::getDeclaration(compiler->module.get(), llvm::Intrinsic::masked_scatter, {vecTy, ptrVecTy});
            compiler->builder->CreateCall(fn, {vecVal, ptrVec, compiler->builder->getInt32((uint32_t)al.value()), allTrue});
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

LLVMBackend::NamedVariable MainListener::EmitBitfieldAccess(
        LLVMBackend* compiler,
        llvm::Value* storagePtr,
        llvm::Type* storageTy,
        const LLVMBackend::BitfieldInfo& bf,
        const std::string& parentVariableName,
        const std::string& owningStructName) {
        auto* word = compiler->CreateLoad(storageTy, storagePtr);
        unsigned w = bf.BitWidth;
        unsigned off = bf.BitOffset;
        unsigned storageBits = (unsigned)word->getType()->getIntegerBitWidth();
        bool isUnsigned = bf.IsUnsigned || bf.TypeName == "bool";
        // Sign-aware extraction. Unsigned: (word >> off) & ((1<<w)-1). Signed: shift the
        // bitfield's MSB up to the word MSB, then arithmetic-shift right to sign-extend.
        llvm::Value* shifted;
        if (isUnsigned)
        {
            auto* shr = compiler->builder->CreateLShr(word, llvm::ConstantInt::get(word->getType(), off));
            uint64_t mask = (w == 64) ? ~uint64_t(0) : ((uint64_t(1) << w) - 1);
            shifted = compiler->builder->CreateAnd(shr, llvm::ConstantInt::get(word->getType(), mask));
        }
        else
        {
            unsigned leftShift = storageBits - w - off;
            auto* shl = compiler->builder->CreateShl(word, llvm::ConstantInt::get(word->getType(), leftShift));
            shifted = compiler->builder->CreateAShr(shl, llvm::ConstantInt::get(word->getType(), storageBits - w));
        }

        LLVMBackend::DeclTypeAndValue bfType{};
        bfType.TypeName = bf.TypeName;
        bfType.VariableName = bf.Name;
        bfType.IsBitfield = true;
        bfType.BitWidth = bf.BitWidth;
        bfType.BitOffset = bf.BitOffset;
        bfType.StorageFieldIndex = bf.StorageFieldIndex;

        LLVMBackend::NamedVariable nv{};
        nv.Primary = shifted;
        nv.BaseType = shifted->getType();
        nv.Storage = nullptr;  // bitfields have no addressable storage
        nv.TypeAndValue = bfType;
        nv.TypeAndValue.ParentVariableName = parentVariableName;
        nv.OwningStructName = owningStructName;
        nv.FieldName = bf.Name;
        nv.BitfieldStorage = storagePtr;
        nv.BitfieldStorageType = storageTy;
        nv.BitfieldOffset = bf.BitOffset;
        nv.BitfieldWidth = bf.BitWidth;
        nv.BitfieldUnsigned = isUnsigned;
        return nv;
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

        // DFS through anonymous (__anonN) members for fieldName. `chain` holds field indices root-to-leaf.
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
                if (f.VariableName.rfind("__anon", 0) != 0) continue;  // only transparent members
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
                ptr = compiler->CreateStructGEP(curType, ptr, (unsigned)chain[k]);
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
            out = EmitBitfieldAccess(compiler, storagePtr, storageTy, bitfieldHit,
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
            out.Storage = compiler->CreateStructGEP(curType, ptr, (unsigned)chain.back());
            if (llvm::isa<llvm::ArrayType>(leafLLVMType))
            {
                out.Primary = nullptr;
                out.BaseType = leafLLVMType;
            }
            else
            {
                out.Primary = compiler->CreateLoad(out.Storage);
                out.BaseType = out.Primary->getType();
            }
        }
        out.TypeAndValue = leafField;
        out.TypeAndValue.ParentVariableName = structVar.TypeAndValue.VariableName;
        out.OwningStructName = structVar.TypeAndValue.TypeName;
        out.FieldName = fieldName;
        // Preserve unique-field provenance across a later cast (see the sibling named-field read).
        if (leafField.IsUnique && leafField.Pointer)
            out.IsUniqueFieldAlias = true;
        out.IsBorrowed = structVar.IsBorrowed;
        out.BorrowedOrigin = structVar.BorrowedOrigin;
        return true;
    }

void MainListener::ClassifyPostfixCallResult(
        antlr4::ParserRuleContext* ctx,
        const LLVMBackend::NamedVariable& result,
        LLVMBackend::NamedVariable& structVar,
        LLVMBackend::NamedVariable& interfaceVar) {
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

std::string MainListener::NextMemberName(CFlatParser::PostfixExpressionContext* ctx,
                                      antlr4::tree::ParseTree* opNode) {
        const auto& children = ctx->children;
        for (size_t i = 0; i + 1 < children.size(); ++i)
        {
            if (children[i] != opNode) continue;
            auto* next = children[i + 1];
            if (next->getTreeType() != antlr4::tree::ParseTreeType::TERMINAL) return "";
            auto* term = dynamic_cast<antlr4::tree::TerminalNode*>(next);
            auto tok = term->getSymbol()->getType();
            if (tok == CFlatParser::Identifier || tok == CFlatParser::Move)
                return term->getText();
            return "";
        }
        return "";
    }


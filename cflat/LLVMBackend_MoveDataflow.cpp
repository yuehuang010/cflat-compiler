#pragma warning(push)
#pragma warning(disable: 4244 4267)
#include <llvm/IR/IRBuilder.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/GetElementPtrTypeIterator.h>
#include "TypeMangling.h"
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/Dominators.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/CFG.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Transforms/Utils/Mem2Reg.h>
#include <llvm/Transforms/Scalar/SROA.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Transforms/IPO/GlobalDCE.h>
#include <llvm/Transforms/Instrumentation/AddressSanitizer.h>
#include <llvm/Object/COFF.h>
#include <llvm/Object/Binary.h>
#include <llvm/Object/Archive.h>
#include <llvm/Object/COFFImportFile.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/TimeProfiler.h>
#include <llvm/Support/JSON.h>
#include <llvm/IR/DiagnosticInfo.h>
#include <llvm/IR/DiagnosticHandler.h>
#pragma warning(pop)
#include <antlr4-runtime.h>

#include "platform/GeneratedParser.h"
#include "LLVMBackend.h"
#include "MainListener.h"
#include "GrammarTreeListener.h"
#include <filesystem>
#include <optional>
#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <set>
#include <unordered_set>

#if defined(__APPLE__)
// Step 3 (macOS self-contained link): harvest libSystem's exported symbols from
// the live dyld shared cache to synthesize a linker stub, so -o needs no SDK.
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <dlfcn.h>
#include <cstring>
#include <sys/sysctl.h>
#endif

// ---- Definitions moved out of LLVMBackend.h (MoveDataflow) ----

void LLVMBackend::RecordMoveEvent(movedf::EventKind kind, const std::string& name,
                         const std::string& field, int line, int col)
{
        if (name.empty()) return;
        if (!builder) return;
        llvm::BasicBlock* bb = builder->GetInsertBlock();
        if (!bb || !bb->getParent()) return;
        moveEventLog_[bb->getParent()].push_back(
            movedf::Event{ bb, kind, name, field, line, col });
    }

void LLVMBackend::RecordMoveKill(const std::string& name)
{ RecordMoveEvent(movedf::EventKind::KillWhole, name, "", 0, 0); }

void LLVMBackend::RecordMoveKillField(const std::string& name, const std::string& field)
{ RecordMoveEvent(movedf::EventKind::KillField, name, field, 0, 0); }

void LLVMBackend::RecordMoveGenRevive(const std::string& name)
{ RecordMoveEvent(movedf::EventKind::GenReviveWhole, name, "", 0, 0); }

void LLVMBackend::RecordMoveGenReviveField(const std::string& name, const std::string& field)
{ RecordMoveEvent(movedf::EventKind::GenReviveField, name, field, 0, 0); }

void LLVMBackend::RecordMoveGenBind(const std::string& name)
{ RecordMoveEvent(movedf::EventKind::GenBind, name, "", 0, 0); RecordNullClear(name); }

void LLVMBackend::RecordMoveUse(const std::string& name, const std::string& field, int line, int col)
{ RecordMoveEvent(movedf::EventKind::Use, name, field, line, col); }

void LLVMBackend::RecordNullEvent(nulldf::EventKind kind, const std::string& name, int line, int col)
{
        if (name.empty() || !builder) return;
        llvm::BasicBlock* bb = builder->GetInsertBlock();
        if (!bb || !bb->getParent()) return;
        nullEventLog_[bb->getParent()].push_back(nulldf::Event{ bb, kind, name, line, col });
    }

void LLVMBackend::RecordNullSet(const std::string& name)
{ RecordNullEvent(nulldf::EventKind::SetNull, name, 0, 0); }

void LLVMBackend::RecordNullClear(const std::string& name)
{ RecordNullEvent(nulldf::EventKind::ClearNull, name, 0, 0); }

void LLVMBackend::RecordNullEscape(const std::string& name)
{ RecordNullEvent(nulldf::EventKind::Escape, name, 0, 0); }

void LLVMBackend::RecordNullRead(const std::string& name)
{ RecordNullEvent(nulldf::EventKind::Read, name, 0, 0); }

void LLVMBackend::RecordNullDeref(const std::string& name, int line, int col)
{ RecordNullEvent(nulldf::EventKind::Deref, name, line, col); }

void LLVMBackend::DiscardNullDerefEvents(llvm::Function* F)
{
        if (F) nullEventLog_.erase(F);
    }

/*
 * An expect_error block swallows the diagnostic and compilation CONTINUES, so a function body
 * abandoned mid-emission stays in the module with unterminated blocks. Every later CFG walk
 * (movedf/nulldf ComputeRpoIndex calls llvm::succ_begin, which dereferences the terminator) and
 * the module verifier then see malformed IR. Close each open block with `unreachable` and drop
 * the function's pending analysis logs - the swallowed diagnostic already answered them.
 */
bool LLVMBackend::SealAbandonedFunction(llvm::Function* F)
{
        if (F == nullptr || F->isDeclaration()) return false;
        bool sealed = false;
        for (auto& BB : *F)
            if (cflat_llvm::GetTerminatorOrNull(&BB) == nullptr)
            {
                new llvm::UnreachableInst(*context, &BB);
                sealed = true;
            }
        moveEventLog_.erase(F);
        nullEventLog_.erase(F);
        pendingReturnDangleChecks_.erase(F);
        pendingNullIfaceDispatch_.erase(F);
        return sealed;
    }

/*
 * A sealed body is dead but not removable: an outer abandoned body may already call it, and a
 * FunctionSymbol elsewhere may hold the pointer, so erasing it would dangle. Rename it out of the
 * way and drop its registrations instead - the symbol is then free for a later good emission, and
 * the renamed remnant is unreachable internal code the optimizer drops.
 */
void LLVMBackend::DiscardAbandonedFunction(llvm::Function* F)
{
        if (F == nullptr || F->isDeclaration()) return;
        std::string mangled = F->getName().str();
        for (auto& entry : functionTable)
        {
            auto& syms = entry.second;
            syms.erase(std::remove_if(syms.begin(), syms.end(),
                [F](const FunctionSymbol& sym) { return sym.Function == F; }), syms.end());
        }
        functionBodyOrigin_.erase(mangled);
        F->setName(mangled + ".abandoned");
        F->setLinkage(llvm::Function::InternalLinkage);
    }

// Sizes of the three per-function pending logs, so an abandoned region can rewind to them.
LLVMBackend::PendingAnalysisMark LLVMBackend::MarkPendingAnalyses(llvm::Function* F) const
{
        PendingAnalysisMark mark;
        if (F == nullptr) return mark;
        auto nullIt = nullEventLog_.find(F);
        if (nullIt != nullEventLog_.end()) mark.nullEvents = nullIt->second.size();
        auto dangleIt = pendingReturnDangleChecks_.find(F);
        if (dangleIt != pendingReturnDangleChecks_.end()) mark.returnDangleChecks = dangleIt->second.size();
        auto ifaceIt = pendingNullIfaceDispatch_.find(F);
        if (ifaceIt != pendingNullIfaceDispatch_.end()) mark.nullIfaceDispatch = ifaceIt->second.size();
        return mark;
    }

void LLVMBackend::RewindPendingAnalyses(llvm::Function* F, const PendingAnalysisMark& mark)
{
        if (F == nullptr) return;
        // Only ever shrinks: a log that somehow got smaller is left alone rather than grown.
        auto rewind = [](auto& table, llvm::Function* fn, size_t keep) {
            auto it = table.find(fn);
            if (it == table.end()) return;
            if (it->second.size() > keep) it->second.resize(keep);
            if (it->second.empty()) table.erase(it);
        };
        rewind(nullEventLog_, F, mark.nullEvents);
        rewind(pendingReturnDangleChecks_, F, mark.returnDangleChecks);
        rewind(pendingNullIfaceDispatch_, F, mark.nullIfaceDispatch);
    }

void LLVMBackend::RunNullDerefDataflow(llvm::Function* F)
{
        if (!F) return;
        // Prove F never returns BEFORE anything can throw, so a wrapper like
        // `void die() { printf(...); exit(1); }` is known by the time its callers are analyzed.
        // Order-dependent, and never in the false-positive direction: proving a wrapper noreturn
        // can only suppress a report or sharpen control dependence into a correct one.
        if (nulldf::FunctionNeverReturns(F, &provenNoReturn_)) provenNoReturn_.insert(F);
        auto it = nullEventLog_.find(F);
        if (it == nullEventLog_.end()) return;
        std::vector<nulldf::Event> events = std::move(it->second);
        nullEventLog_.erase(it);

        const char* off = std::getenv("CFLAT_NULL_DF_OFF");
        if (off && off[0] != '\0') return;

        auto diverged = nulldf::AnalyzeFunction(F, events, &provenNoReturn_);
        if (diverged.empty()) return;
        const nulldf::Divergence* earliest = nullptr;
        for (const auto& d : diverged)
            if (!earliest || d.line < earliest->line ||
                (d.line == earliest->line && d.col < earliest->col))
                earliest = &d;
        SetSourceLocation(earliest->line, earliest->col);
        LogError(std::format(
            "dereference of moved variable '{}' (it is null after the move)", earliest->name));
    }

static std::vector<llvm::StoreInst*> StoresReachingLoadLinear(llvm::LoadInst* load,
                                                              llvm::AllocaInst* slot)
{
        std::vector<llvm::StoreInst*> stores;
        if (load == nullptr || slot == nullptr) return stores;
        std::unordered_set<const llvm::StoreInst*> seenStores;
        auto nearestStore = [&](llvm::BasicBlock* block, llvm::Instruction* before) {
            auto current = before != nullptr ? before->getIterator() : block->end();
            while (current != block->begin())
            {
                --current;
                auto* store = llvm::dyn_cast<llvm::StoreInst>(&*current);
                if (store != nullptr
                    && store->getPointerOperand()->stripPointerCasts() == slot)
                {
                    if (seenStores.insert(store).second) stores.push_back(store);
                    return true;
                }
            }
            return false;
        };

        std::vector<std::pair<llvm::BasicBlock*, llvm::Instruction*>> pending;
        std::unordered_set<const llvm::BasicBlock*> visited;
        if (!nearestStore(load->getParent(), load))
            for (llvm::BasicBlock* predecessor : llvm::predecessors(load->getParent()))
                pending.emplace_back(predecessor, nullptr);
        for (size_t i = 0; i < pending.size(); ++i)
        {
            auto [block, before] = pending[i];
            if (block == nullptr || !visited.insert(block).second) continue;
            if (!nearestStore(block, before))
                for (llvm::BasicBlock* predecessor : llvm::predecessors(block))
                    pending.emplace_back(predecessor, nullptr);
        }
        return stores;
}

static bool RefineStoresByCorrelatedBranches(
    llvm::LoadInst* load, llvm::AllocaInst* slot,
    std::vector<llvm::StoreInst*>& stores)
{
        stores.clear();
        if (load == nullptr || slot == nullptr) return true;
        constexpr size_t kNodeBudget = 4096;
        size_t visitedNodes = 0;
        bool exhausted = false;
        struct BranchFact
        {
            const llvm::Value* Condition = nullptr;
            bool Truth = false;
            const llvm::ICmpInst* Comparison = nullptr;
            bool ComparisonTruth = false;
        };
        std::unordered_set<const llvm::BasicBlock*> path;
        std::unordered_set<const llvm::StoreInst*> seenStores;
        std::vector<BranchFact> branchFacts;
        std::unordered_map<const llvm::Value*, unsigned> storeCountBySlot;
        auto storesToSlot = [&](const llvm::Value* alloca) {
            auto [it, inserted] = storeCountBySlot.emplace(alloca, 0);
            if (inserted)
                for (const llvm::BasicBlock& basicBlock : *load->getFunction())
                    for (const llvm::Instruction& instruction : basicBlock)
                        if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
                            store != nullptr
                            && store->getPointerOperand()->stripPointerCasts() == alloca)
                            ++it->second;
            return it->second;
        };
        auto sameStableLoad = [&](const llvm::Value* left, const llvm::Value* right) {
            if (left == right) return true;
            auto* leftLoad = llvm::dyn_cast<llvm::LoadInst>(left);
            auto* rightLoad = llvm::dyn_cast<llvm::LoadInst>(right);
            if (leftLoad == nullptr || rightLoad == nullptr) return false;
            const llvm::Value* leftSlot = leftLoad->getPointerOperand()->stripPointerCasts();
            const llvm::Value* rightSlot = rightLoad->getPointerOperand()->stripPointerCasts();
            if (leftSlot != rightSlot || !llvm::isa<llvm::AllocaInst>(leftSlot)) return false;
            return storesToSlot(leftSlot) == 1;
        };
        auto equivalentCompareOperands = [&](const llvm::ICmpInst* left,
                                             const llvm::ICmpInst* right) {
            return left != nullptr && right != nullptr
                && sameStableLoad(left->getOperand(0), right->getOperand(0))
                && sameStableLoad(left->getOperand(1), right->getOperand(1));
        };
        auto addBranchFact = [&](llvm::BranchInst* branch, llvm::BasicBlock* successor) {
            if (branch == nullptr || !branch->isConditional()
                || branch->getSuccessor(0) == branch->getSuccessor(1))
                return true;
            BranchFact fact;
            fact.Condition = branch->getCondition();
            fact.Truth = branch->getSuccessor(0) == successor;
            fact.Comparison = llvm::dyn_cast<llvm::ICmpInst>(fact.Condition);
            fact.ComparisonTruth = fact.Truth;
            if (auto* inverted = llvm::dyn_cast<llvm::BinaryOperator>(fact.Condition);
                fact.Comparison == nullptr && inverted != nullptr
                && inverted->getOpcode() == llvm::Instruction::Xor)
            {
                if (auto* one = llvm::dyn_cast<llvm::ConstantInt>(inverted->getOperand(1));
                    one != nullptr && one->isOne())
                {
                    fact.Comparison = llvm::dyn_cast<llvm::ICmpInst>(inverted->getOperand(0));
                    fact.ComparisonTruth = !fact.Truth;
                }
            }
            for (const BranchFact& previous : branchFacts)
            {
                if (fact.Condition == previous.Condition && fact.Truth != previous.Truth)
                    return false;
                if (!equivalentCompareOperands(fact.Comparison, previous.Comparison)) continue;
                auto currentPredicate = fact.Comparison->getPredicate();
                auto previousPredicate = previous.Comparison->getPredicate();
                if (currentPredicate == previousPredicate
                    && fact.ComparisonTruth != previous.ComparisonTruth)
                    return false;
                if (currentPredicate == llvm::CmpInst::getInversePredicate(previousPredicate)
                    && fact.ComparisonTruth == previous.ComparisonTruth)
                    return false;
            }
            branchFacts.push_back(fact);
            return true;
        };
        std::function<void(llvm::BasicBlock*, llvm::Instruction*)> walk =
            [&](llvm::BasicBlock* block, llvm::Instruction* before) {
                if (exhausted || block == nullptr) return;
                if (++visitedNodes > kNodeBudget)
                {
                    exhausted = true;
                    return;
                }
                if (!path.insert(block).second) return;
                auto current = before != nullptr ? before->getIterator() : block->end();
                while (current != block->begin())
                {
                    --current;
                    auto* store = llvm::dyn_cast<llvm::StoreInst>(&*current);
                    if (store == nullptr
                        || store->getPointerOperand()->stripPointerCasts() != slot)
                        continue;
                    if (seenStores.insert(store).second) stores.push_back(store);
                    path.erase(block);
                    return;
                }
                for (llvm::BasicBlock* predecessor : llvm::predecessors(block))
                {
                    auto* branch = llvm::dyn_cast<llvm::BranchInst>(predecessor->getTerminator());
                    size_t factCount = branchFacts.size();
                    if (addBranchFact(branch, block)) walk(predecessor, nullptr);
                    branchFacts.resize(factCount);
                }
                path.erase(block);
            };
        walk(load->getParent(), load);
        return !exhausted;
}

static std::vector<llvm::StoreInst*> StoresReachingLoad(llvm::LoadInst* load,
                                                        llvm::AllocaInst* slot,
                                                        bool refineCorrelations = false)
{
        std::vector<llvm::StoreInst*> stores = StoresReachingLoadLinear(load, slot);
        if (!refineCorrelations) return stores;
        std::vector<llvm::StoreInst*> refined;
        if (RefineStoresByCorrelatedBranches(load, slot, refined)) return refined;
        // Exhaustion keeps the linear candidates, preserving the conservative answer.
        return stores;
}

void LLVMBackend::RunInterfaceReturnDangleCheck(llvm::Function* F)
{
        if (!F) return;
        auto it = pendingReturnDangleChecks_.find(F);
        if (it == pendingReturnDangleChecks_.end()) return;
        std::vector<PendingReturnDangleCheck> pending = std::move(it->second);
        pendingReturnDangleChecks_.erase(it);
        llvm::DominatorTree dominatorTree(*F);
        llvm::LoopInfo loopInfo(dominatorTree);

        for (const auto& rec : pending)
        {
            if (!rec.Slot || !rec.ReturnedLoad) continue;
            bool aliasOfOwnedLocal = false;
            bool sawNullReachingStore = false;
            std::string frameStorageClassName;
            std::unordered_set<const llvm::Value*> valuePath;
            std::unordered_map<const llvm::LoadInst*, std::vector<llvm::StoreInst*>> storeMemo;
            std::unordered_set<const llvm::LoadInst*> activeLoads;
            std::function<std::vector<llvm::StoreInst*>(llvm::LoadInst*, llvm::AllocaInst*)>
                storesForLoad;
            std::function<void(const llvm::Value*, int)> inspectValue =
                [&](const llvm::Value* value, int depth) {
                    if (value == nullptr || depth > 16 || !valuePath.insert(value).second) return;
                    if (llvm::isa<llvm::UndefValue>(value)
                        || llvm::isa<llvm::ConstantAggregateZero>(value)
                        || (llvm::isa<llvm::Constant>(value)
                            && llvm::cast<llvm::Constant>(value)->isNullValue()))
                    {
                        valuePath.erase(value);
                        return;
                    }
                    if (const InterfaceBoxRecord* box = FindInterfaceBoxByFatValue(value))
                    {
                        aliasOfOwnedLocal = aliasOfOwnedLocal || box->AliasOfOwnedLocal;
                        if (frameStorageClassName.empty()
                            && box->Source == InterfaceBoxSource::FrameStorage)
                            frameStorageClassName = box->SourceClassName;
                        valuePath.erase(value);
                        return;
                    }
                    if (const auto* phi = llvm::dyn_cast<llvm::PHINode>(value))
                    {
                        for (const llvm::Value* incoming : phi->incoming_values())
                            inspectValue(incoming, depth + 1);
                        valuePath.erase(value);
                        return;
                    }
                    if (const auto* select = llvm::dyn_cast<llvm::SelectInst>(value))
                    {
                        inspectValue(select->getTrueValue(), depth + 1);
                        inspectValue(select->getFalseValue(), depth + 1);
                        valuePath.erase(value);
                        return;
                    }
                    if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(value);
                        load != nullptr && load->getType() == GetFatPtrType())
                    {
                        auto* sourceSlot = llvm::dyn_cast<llvm::AllocaInst>(
                            load->getPointerOperand()->stripPointerCasts());
                        if (sourceSlot != nullptr)
                        {
                            std::vector<llvm::StoreInst*> sourceStores = storesForLoad(
                                const_cast<llvm::LoadInst*>(load),
                                const_cast<llvm::AllocaInst*>(sourceSlot));
                            for (const llvm::StoreInst* store : sourceStores)
                                inspectValue(store->getValueOperand(), depth + 1);
                        }
                    }
                    valuePath.erase(value);
                };
            storesForLoad = [&](llvm::LoadInst* load, llvm::AllocaInst* slot) {
                if (load == nullptr || slot == nullptr) return std::vector<llvm::StoreInst*>{};
                auto cached = storeMemo.find(load);
                if (cached != storeMemo.end()) return cached->second;
                std::vector<llvm::StoreInst*> linear = StoresReachingLoad(load, slot);
                if (!activeLoads.insert(load).second) return linear;

                bool savedAlias = aliasOfOwnedLocal;
                std::string savedFrame = frameStorageClassName;
                aliasOfOwnedLocal = false;
                frameStorageClassName.clear();
                valuePath.clear();
                for (const llvm::StoreInst* store : linear)
                    inspectValue(store->getValueOperand(), 0);
                bool hasDangerousStore = aliasOfOwnedLocal || !frameStorageClassName.empty();
                aliasOfOwnedLocal = savedAlias;
                frameStorageClassName = std::move(savedFrame);
                valuePath.clear();

                if (hasDangerousStore)
                    linear = StoresReachingLoad(load, slot, true);
                activeLoads.erase(load);
                storeMemo.emplace(load, linear);
                return linear;
            };

            std::vector<llvm::StoreInst*> reachingStores = storesForLoad(rec.ReturnedLoad, rec.Slot);
            for (const llvm::StoreInst* store : reachingStores)
            {
                const llvm::Value* value = store->getValueOperand();
                sawNullReachingStore = sawNullReachingStore
                    || llvm::isa<llvm::UndefValue>(value)
                    || llvm::isa<llvm::ConstantAggregateZero>(value)
                    || (llvm::isa<llvm::Constant>(value)
                        && llvm::cast<llvm::Constant>(value)->isNullValue());
                inspectValue(store->getValueOperand(), 0);
            }

            // Existing interface-return behavior treats a reachable null box as neutral evidence.
            if (sawNullReachingStore) frameStorageClassName.clear();

            // Loops are not unrolled; keep accepting when one can write a non-frame box.
            if (!frameStorageClassName.empty())
            {
                bool hasNonFrameLoopWriter = false;
                for (const llvm::User* user : rec.Slot->users())
                {
                    const auto* store = llvm::dyn_cast<llvm::StoreInst>(user);
                    if (store == nullptr
                        || store->getPointerOperand()->stripPointerCasts() != rec.Slot
                        || loopInfo.getLoopFor(store->getParent()) == nullptr)
                        continue;
                    const llvm::Value* value = store->getValueOperand();
                    bool isNullish = llvm::isa<llvm::UndefValue>(value)
                        || llvm::isa<llvm::ConstantAggregateZero>(value)
                        || (llvm::isa<llvm::Constant>(value)
                            && llvm::cast<llvm::Constant>(value)->isNullValue());
                    const InterfaceBoxRecord* box = FindInterfaceBoxByFatValue(value);
                    if (isNullish || box == nullptr || box->Source != InterfaceBoxSource::FrameStorage)
                    {
                        hasNonFrameLoopWriter = true;
                        break;
                    }
                }
                if (hasNonFrameLoopWriter) frameStorageClassName.clear();
            }

            if (aliasOfOwnedLocal)
            {
                SetSourceLocation(static_cast<size_t>(rec.Line), static_cast<size_t>(rec.Col));
                LogError(std::format(
                    "cannot return a borrow of a local owner as interface '{}' - the owning local "
                    "is freed when this function returns and a borrow cannot outlive its owner; "
                    "return the owning local itself ('return t;' from a 'move' function) or a heap "
                    "object", rec.InterfaceName));
            }
            if (!frameStorageClassName.empty())
            {
                SetSourceLocation(static_cast<size_t>(rec.Line), static_cast<size_t>(rec.Col));
                LogError(std::format(
                    "cannot return local value '{}' as interface '{}' - the interface fat "
                    "pointer would dangle once this function returns; allocate on the heap "
                    "('new {}') and return the pointer", frameStorageClassName,
                    rec.InterfaceName, frameStorageClassName));
            }
        }
    }

void LLVMBackend::RunDeferredEndOfBodyChecks(llvm::Function* F)
{
        if (F == nullptr) return;
        RunNullDerefDataflow(F);
        RunInterfaceReturnDangleCheck(F);
        RunNullIfaceDispatchCheck(F);
}

bool LLVMBackend::InterfaceSlotIsFrameLocal(const llvm::Value* slot) const
{
        llvm::SmallPtrSet<const llvm::Value*, 16> seen;
        llvm::SmallVector<const llvm::Value*, 16> work;
        seen.insert(slot);
        work.push_back(slot);
        unsigned budget = 1024;
        while (!work.empty())
        {
            if (budget-- == 0) return false;
            const llvm::Value* cur = work.pop_back_val();
            for (const llvm::User* u : cur->users())
            {
                if (llvm::isa<llvm::LoadInst>(u)) continue;
                if (const auto* st = llvm::dyn_cast<llvm::StoreInst>(u))
                {
                    // The address stored elsewhere is an escape, not a write.
                    if (st->getPointerOperand() == cur) continue;
                    return false;
                }
                if (const auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(u))
                {
                    if (!gep->hasAllConstantIndices()) return false;
                    if (seen.insert(gep).second) work.push_back(gep);
                    continue;
                }
                if (const auto* call = llvm::dyn_cast<llvm::CallBase>(u))
                    if (const llvm::Function* f = call->getCalledFunction(); f != nullptr
                        && (f->getName().starts_with("llvm.dbg.")
                            || f->getName().starts_with("llvm.lifetime.")
                            || f->getName().starts_with("_~"))) continue;
                return false;
            }
        }
        return true;
    }

bool LLVMBackend::StoreWritesInterfaceLoc(const llvm::StoreInst* st, const llvm::AllocaInst* base,
                                        llvm::ArrayRef<uint64_t> path,
                                        llvm::SmallVectorImpl<uint64_t>& storePath)
{
        // The declaration's zero splat is compiler-emitted storage hygiene, not an assignment of
        // an implementation; hiding it keeps every proof answering what it answered without it.
        if (st->getMetadata(kIfaceDeclSplatMD) != nullptr) return false;
        auto* dest = const_cast<llvm::Value*>(st->getPointerOperand());
        if (ResolveIfaceStorageLoc(dest, storePath) != base) return false;
        const size_t common = std::min(storePath.size(), path.size());
        for (size_t i = 0; i < common; ++i)
            if (storePath[i] != path[i]) return false;
        return true;
    }

const llvm::Constant* LLVMBackend::NullIfaceStoredConstant(const llvm::Value* v)
{
        if (const auto* c = llvm::dyn_cast<llvm::Constant>(v)) return c;
        const auto* call = llvm::dyn_cast<llvm::CallBase>(v);
        if (call == nullptr) return nullptr;
        const llvm::Function* callee = call->getCalledFunction();
        if (callee == nullptr || callee->isDeclaration()) return nullptr;
        const llvm::Constant* result = nullptr;
        for (const llvm::BasicBlock& b : *callee)
        {
            const auto* ret = llvm::dyn_cast_or_null<llvm::ReturnInst>(cflat_llvm::GetTerminatorOrNull(&b));
            if (ret == nullptr) continue;
            if (result != nullptr) return nullptr;
            const auto* rv = llvm::dyn_cast_or_null<llvm::Constant>(ret->getReturnValue());
            if (rv == nullptr) return nullptr;
            result = rv;
        }
        return result;
    }

bool LLVMBackend::NullIfaceStoreAffectsLoc(const llvm::StoreInst* st, const llvm::AllocaInst* base,
                                         llvm::ArrayRef<uint64_t> path, bool& leavesNull)
{
        leavesNull = false;
        llvm::SmallVector<uint64_t, 4> storePath;
        if (!StoreWritesInterfaceLoc(st, base, path, storePath)) return false;
        if (storePath.size() > path.size()) return true;
        const llvm::Constant* stored = NullIfaceStoredConstant(st->getValueOperand());
        for (size_t i = storePath.size(); stored != nullptr && i < path.size(); ++i)
            stored = stored->getAggregateElement(static_cast<unsigned>(path[i]));
        leavesNull = (stored != nullptr && stored->isNullValue());
        return true;
    }

std::vector<LLVMBackend::NullIfaceLocFacts> LLVMBackend::CollectNullIfaceLocFacts(
        llvm::Function* F, const std::vector<const PendingNullIfaceDispatch*>& live)
{
        std::vector<NullIfaceLocFacts> facts(live.size());
        for (llvm::BasicBlock& BB : *F)
            for (llvm::Instruction& inst : BB)
            {
                const auto* st = llvm::dyn_cast<llvm::StoreInst>(&inst);
                if (st == nullptr) continue;
                for (size_t i = 0; i < live.size(); ++i)
                {
                    bool leavesNull = false;
                    if (!NullIfaceStoreAffectsLoc(st, live[i]->Base, live[i]->Path, leavesNull))
                        continue;
                    facts[i].ByBlock[&BB] = { st, leavesNull };
                    if (leavesNull) facts[i].AnyNullWrite = true;
                }
            }
        return facts;
    }

void LLVMBackend::EnsureNullIfaceBlocks(NullIfaceCfgInfo& cfg, llvm::Function* F)
{
        if (cfg.Fn == F) return;
        cfg.Fn = F;
        cfg.Rpo = movedf::ComputeRpoIndex(F);
        cfg.Blocks.clear();
        for (llvm::BasicBlock& BB : *F) if (cfg.Rpo.count(&BB)) cfg.Blocks.push_back(&BB);
        std::sort(cfg.Blocks.begin(), cfg.Blocks.end(),
                  [&](llvm::BasicBlock* a, llvm::BasicBlock* b) { return cfg.Rpo[a] < cfg.Rpo[b]; });
    }

bool LLVMBackend::CrossBlockProvesNullIface(llvm::Function* F, const PendingNullIfaceDispatch& rec,
                                   const NullIfaceLocFacts& facts, NullIfaceCfgInfo& cfg)
{
        if (!facts.AnyNullWrite) return false;   // nothing in F ever proves this location null
        llvm::BasicBlock* anchorBB = rec.Anchor->getParent();
        EnsureNullIfaceBlocks(cfg, F);
        if (!cfg.Rpo.count(anchorBB)) return false;

        // The last write covering the location BEFORE the anchor in its own block decides on
        // its own; that is the same-block proof, which has already run and failed, so reaching
        // here with one means the location is not null at the access.
        for (llvm::Instruction& inst : *anchorBB)
        {
            if (&inst == rec.Anchor) break;
            const auto* st = llvm::dyn_cast<llvm::StoreInst>(&inst);
            if (st == nullptr) continue;
            bool leavesNull = false;
            if (NullIfaceStoreAffectsLoc(st, rec.Base, rec.Path, leavesNull) && !leavesNull)
                return false;
        }

        llvm::BasicBlock* entry = &F->getEntryBlock();
        std::unordered_map<llvm::BasicBlock*, char> outState;
        auto transfer = [&](llvm::BasicBlock* bb, char in) -> char
        {
            auto fit = facts.ByBlock.find(bb);
            if (fit == facts.ByBlock.end()) return in;
            return fit->second.second ? 1 : 0;
        };
        for (llvm::BasicBlock* bb : cfg.Blocks)
            outState[bb] = transfer(bb, bb == entry ? 0 : 1);

        auto meetIn = [&](llvm::BasicBlock* bb) -> char
        {
            if (bb == entry) return 0;
            bool any = false;
            for (llvm::BasicBlock* p : llvm::predecessors(bb))
            {
                if (!cfg.Rpo.count(p)) continue;
                any = true;
                if (!outState[p]) return 0;
            }
            return any ? 1 : 0;
        };

        // Monotone descent: a block's OUT only ever moves from null to not-null, so this
        // terminates in at most one pass per block. The budget is a pure safety net.
        unsigned budget = static_cast<unsigned>(cfg.Blocks.size()) + 8;
        bool changed = true;
        while (changed)
        {
            if (budget-- == 0) return false;
            changed = false;
            for (llvm::BasicBlock* bb : cfg.Blocks)
            {
                char out = transfer(bb, meetIn(bb));
                if (out != outState[bb]) { outState[bb] = out; changed = true; }
            }
        }
        if (!meetIn(anchorBB)) return false;

        // Blocks whose last covering write leaves the location null - the witnesses the
        // control-dependence test is taken against.
        std::vector<llvm::BasicBlock*> witnesses;
        for (const auto& [bb, f] : facts.ByBlock)
        {
            auto* mut = const_cast<llvm::BasicBlock*>(bb);
            if (f.second && cfg.Rpo.count(mut)) witnesses.push_back(mut);
        }
        if (witnesses.empty()) return false;

        if (!cfg.HaveCd)
        {
            cfg.Cd = nulldf::ControlDependence(cfg.Blocks, cfg.Rpo, &provenNoReturn_);
            cfg.HaveCd = true;
        }
        const nulldf::CdSet& cdAccess = cfg.Cd.Closure(anchorBB);
        for (llvm::BasicBlock* m : witnesses)
            if (!nulldf::IsSubset(cdAccess, cfg.Cd.Closure(m))) return false;
        return true;
    }

bool LLVMBackend::SameBlockProvesNullIface(const PendingNullIfaceDispatch& rec) const
{
        llvm::BasicBlock* bb = rec.Anchor->getParent();
        const llvm::StoreInst* last = nullptr;
        llvm::SmallVector<uint64_t, 4> storePath, lastPath;
        bool reachedAnchor = false;
        for (const llvm::Instruction& inst : *bb)
        {
            if (&inst == rec.Anchor) { reachedAnchor = true; break; }
            if (const auto* st = llvm::dyn_cast<llvm::StoreInst>(&inst))
                if (StoreWritesInterfaceLoc(st, rec.Base, rec.Path, storePath))
                { last = st; lastPath = storePath; }
        }
        if (!reachedAnchor || last == nullptr) return false;
        // A write reaching only PART of the receiver (e.g. just the vtable half of the fat
        // pointer) leaves the rest unaccounted for. A write covering a strict PREFIX of the
        // path - a whole-struct or whole-array store - is a full write of the sub-location.
        if (lastPath.size() > rec.Path.size()) return false;
        const llvm::Constant* stored = NullIfaceStoredConstant(last->getValueOperand());
        for (size_t i = lastPath.size(); stored != nullptr && i < rec.Path.size(); ++i)
            stored = stored->getAggregateElement(static_cast<unsigned>(rec.Path[i]));
        return stored != nullptr && stored->isNullValue();
    }

static std::string SpellPendingNullIfaceName(const LLVMBackend& compiler,
                                             const std::string& name)
{
        return SpellType(compiler, LLVMBackend::TypeAndValue{ .TypeName = name });
    }

void LLVMBackend::ReportNullIfaceAccess(const PendingNullIfaceDispatch& rec)
{
        SetSourceLocation(static_cast<size_t>(rec.Line), static_cast<size_t>(rec.Col));
        // One wording per role. "last set to null" covers BOTH origins the proof accepts:
        // an `= default` / `= nullptr` initializer, and a reassignment to null.
        if (rec.IsField)
            LogError(std::format(
                "member access on null interface value '{}' - '{}' has not been assigned an "
                "implementation since it was last set to null, so '{}.{}' would resolve its "
                "address through a null '{}' vtable; assign one before the access, or write "
                "'{}?.{}' to skip it when null",
                rec.VarName, rec.VarName, rec.VarName, rec.MemberName,
                SpellPendingNullIfaceName(*this, rec.IfaceName),
                rec.VarName, rec.MemberName));
        else
            LogError(std::format(
                "method call on null interface value '{}' - '{}' has not been assigned an "
                "implementation since it was last set to null, so '{}.{}()' would dispatch "
                "through a null '{}' vtable; assign one before the call, or write '{}?.{}()' "
                "to skip it when null",
                rec.VarName, rec.VarName, rec.VarName, rec.MemberName,
                SpellPendingNullIfaceName(*this, rec.IfaceName),
                rec.VarName, rec.MemberName));
    }

void LLVMBackend::ReportNullIfaceUninitAccess(const PendingNullIfaceDispatch& rec)
{
        SetSourceLocation(static_cast<size_t>(rec.Line), static_cast<size_t>(rec.Col));
        // Honest wording: nothing set this to null, it was never assigned an implementation at
        // all - a different fact from ReportNullIfaceAccess's "last set to null".
        if (rec.IsField)
            LogError(std::format(
                "member access on uninitialized interface value '{}' - '{}' is never assigned an "
                "implementation before this access, so '{}.{}' would resolve its address through "
                "an uninitialized '{}' vtable; assign one before the access",
                rec.VarName, rec.VarName, rec.VarName, rec.MemberName,
                SpellPendingNullIfaceName(*this, rec.IfaceName)));
        else
            LogError(std::format(
                "method call on uninitialized interface value '{}' - '{}' is never assigned an "
                "implementation before this call, so '{}.{}()' would dispatch through an "
                "uninitialized '{}' vtable; assign one before the call",
                rec.VarName, rec.VarName, rec.VarName, rec.MemberName,
                SpellPendingNullIfaceName(*this, rec.IfaceName)));
    }

void LLVMBackend::RunNullIfaceDispatchCheck(llvm::Function* F)
{
        if (!F) return;
        auto it = pendingNullIfaceDispatch_.find(F);
        if (it == pendingNullIfaceDispatch_.end()) return;
        std::vector<PendingNullIfaceDispatch> pending = std::move(it->second);
        pendingNullIfaceDispatch_.erase(it);

        // Escape hatch: skip the diagnostic entirely without a rebuild if it ever misfires.
        const char* off = std::getenv("CFLAT_NULL_IFACE_OFF");
        if (off && off[0] != '\0') return;
        // Second hatch for the cross-block half alone, so it can be disabled without also
        // giving up the straight-line proof.
        const char* xoff = std::getenv("CFLAT_NULL_IFACE_XBLOCK_OFF");
        const bool crossBlockOn = !(xoff && xoff[0] != '\0');
        // Third hatch for the never-initialised check alone - a different question from the
        // two above, so it gets its own toggle rather than sharing either one's.
        const char* uoff = std::getenv("CFLAT_NULL_IFACE_UNINIT_OFF");
        const bool uninitOn = !(uoff && uoff[0] != '\0');

        std::vector<const PendingNullIfaceDispatch*> live;
        for (const auto& rec : pending)
        {
            if (rec.Base == nullptr || rec.Anchor == nullptr) continue;
            llvm::BasicBlock* bb = rec.Anchor->getParent();
            if (bb == nullptr || bb->getParent() != F) continue;
            if (rec.Base->getFunction() != F) continue;
            if (!InterfaceSlotIsFrameLocal(rec.Base)) continue;
            live.push_back(&rec);
        }
        if (live.empty()) return;

        // Needed by both the cross-block proof and the never-initialised check, so it runs
        // unconditionally - crossBlockOn only gates whether its own proof is consulted.
        std::vector<NullIfaceLocFacts> facts = CollectNullIfaceLocFacts(F, live);
        NullIfaceCfgInfo cfg;

        for (size_t i = 0; i < live.size(); ++i)
        {
            const PendingNullIfaceDispatch& rec = *live[i];
            bool proven = SameBlockProvesNullIface(rec);
            if (!proven && crossBlockOn)
                proven = CrossBlockProvesNullIface(F, rec, facts[i], cfg);
            if (proven) { ReportNullIfaceAccess(rec); continue; }

            // Never-initialised: the whole receiver (empty path) has no covering store anywhere
            // in the function. MUST-uninit: any store on any path already populated ByBlock.
            if (uninitOn && rec.Path.empty() && facts[i].ByBlock.empty())
            {
                // The declaration witness sits in the entry block, whose CD set is empty, so
                // require the access itself to be control-dependent on nothing (i.e. reached
                // unconditionally) before reporting - a guarded access may legitimately never run.
                EnsureNullIfaceBlocks(cfg, F);
                llvm::BasicBlock* ab = rec.Anchor->getParent();
                if (!cfg.Rpo.count(ab)) continue;
                if (!cfg.HaveCd)
                {
                    cfg.Cd = nulldf::ControlDependence(cfg.Blocks, cfg.Rpo, &provenNoReturn_);
                    cfg.HaveCd = true;
                }
                if (cfg.Cd.Closure(ab).empty()) ReportNullIfaceUninitAccess(rec);
            }
        }
    }

bool LLVMBackend::InterfaceGlobalNeverWritten(const llvm::GlobalVariable* gv) const
{
        llvm::SmallPtrSet<const llvm::Value*, 16> seen;
        llvm::SmallVector<const llvm::Value*, 16> work;
        seen.insert(gv);
        work.push_back(gv);
        unsigned budget = 1024;
        while (!work.empty())
        {
            if (budget-- == 0) return false;
            const llvm::Value* cur = work.pop_back_val();
            for (const llvm::User* u : cur->users())
            {
                if (llvm::isa<llvm::LoadInst>(u)) continue;
                if (const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(u))
                {
                    if (seen.insert(gep).second) work.push_back(gep);
                    continue;
                }
                if (const auto* call = llvm::dyn_cast<llvm::CallBase>(u))
                    if (const llvm::Function* f = call->getCalledFunction(); f != nullptr
                        && (f->getName().starts_with("llvm.dbg.")
                            || f->getName().starts_with("llvm.lifetime."))) continue;
                return false;
            }
        }
        return true;
    }

void LLVMBackend::RunNullIfaceGlobalCheck()
{
        std::vector<PendingNullIfaceGlobalAccess> pending;
        pending.swap(pendingNullIfaceGlobal_);
        if (pending.empty() || module == nullptr) return;

        // Escape hatches: the shared one, plus a third for the global half alone.
        const char* off = std::getenv("CFLAT_NULL_IFACE_OFF");
        if (off && off[0] != '\0') return;
        const char* goff = std::getenv("CFLAT_NULL_IFACE_GLOBAL_OFF");
        if (goff && goff[0] != '\0') return;

        // Interop caveat, handled rather than documented away: cflat globals get ExternalLinkage,
        // so a user C translation unit or a bound import library could write one with no store
        // anywhere in this module, which would make fact 1 false. Skip the whole check whenever
        // user C code is linked in. System frameworks and libc are not user code and cannot name
        // a cflat global, so they do not count.
        if (!cObjectFiles_.empty() || !cLinkLibs_.empty() || positionalCSource_) return;

        std::unordered_map<const llvm::GlobalVariable*, bool> neverWritten;
        std::unordered_map<llvm::Function*, NullIfaceCfgInfo> cfgs;
        for (const auto& rec : pending)
        {
            auto* gv = llvm::dyn_cast_or_null<llvm::GlobalVariable>(NullIfaceHandleValue(rec.Global));
            auto* anchor = llvm::dyn_cast_or_null<llvm::Instruction>(NullIfaceHandleValue(rec.Anchor));
            if (gv == nullptr || anchor == nullptr) continue;
            if (gv->getParent() != module.get()) continue;
            llvm::BasicBlock* bb = anchor->getParent();
            if (bb == nullptr) continue;
            llvm::Function* F = bb->getParent();
            if (F == nullptr || F->isDeclaration() || F->empty()) continue;
            if (F->getParent() != module.get()) continue;

            // Fact 1. An extern declaration has no initializer and is never proven. For a
            // sub-object receiver, walk the constant path into the initializer the same way
            // SameBlockProvesNullIface walks a store's constant - anything that does not
            // resolve (a non-constant-aggregate initializer, an out-of-range index) accepts.
            if (!gv->hasInitializer()) continue;
            const llvm::Constant* init = gv->getInitializer();
            for (size_t i = 0; init != nullptr && i < rec.Path.size(); ++i)
                init = init->getAggregateElement(static_cast<unsigned>(rec.Path[i]));
            if (init == nullptr || !init->isNullValue()) continue;
            auto nw = neverWritten.find(gv);
            if (nw == neverWritten.end())
            {
                // Proving a global is never written must see EVERY writer, so a lazily
                // loaded core module has to be materialized first - a materialized-only
                // survey could miss a core-body store and report a false positive.
                MaterializeCoreIfLazy();
                nw = neverWritten.emplace(gv, InterfaceGlobalNeverWritten(gv)).first;
            }
            if (!nw->second) continue;

            // Fact 2. Witness = entry block, so this asks that the access be reached on every
            // path through the function - exactly what a guard around it makes false.
            NullIfaceCfgInfo& cfg = cfgs[F];
            EnsureNullIfaceBlocks(cfg, F);
            llvm::BasicBlock* entry = &F->getEntryBlock();
            if (!cfg.Rpo.count(bb) || !cfg.Rpo.count(entry)) continue;
            if (!cfg.HaveCd)
            {
                cfg.Cd = nulldf::ControlDependence(cfg.Blocks, cfg.Rpo, &provenNoReturn_);
                cfg.HaveCd = true;
            }
            if (!nulldf::IsSubset(cfg.Cd.Closure(bb), cfg.Cd.Closure(entry))) continue;

            PendingNullIfaceDispatch out;
            out.Anchor = anchor;
            out.VarName = rec.VarName;
            out.MemberName = rec.MemberName;
            out.IfaceName = rec.IfaceName;
            out.IsField = rec.IsField;
            out.Line = rec.Line;
            out.Col = rec.Col;
            ReportNullIfaceAccess(out);   // LogError THROWS - nothing after this runs
        }
    }

void LLVMBackend::RunMoveDataflow()
{
        llvm::TimeTraceScope moveDfScope("MoveDataflow");
        // Module end: no aggregate body will appear after this point.
        ReportUnresolvedProvisionalDeclarations();

        // Sweep any function whose null-state log was not consumed at end-of-body (lambdas and
        // synthesized bodies do not go through the named-function completion point).
        if (module)
            for (auto& F : *module)
                if (!F.isDeclaration() && !F.empty()) RunNullDerefDataflow(&F);
        nullEventLog_.clear();

        // Leftover pending return-dangle checks belong to a function this pass never saw
        // consumed at end-of-body - by module end interfaceBoxRecords_ no longer describes
        // that function (it is cleared/parked per-function), so the ledger lookups the check
        // depends on would be answering for the WRONG function. Drop them unanalyzed (accept).
        pendingReturnDangleChecks_.clear();

        // Leftover null-interface dispatch records belong to a body that never reached the
        // end-of-body hook (a lambda, a synthesized body, or an erased global-init temp
        // function whose blocks are gone). Drop them unanalyzed - accept, never reject.
        pendingNullIfaceDispatch_.clear();

        // The GLOBAL-receiver half, by contrast, can only be answered here: the whole-module
        // "never assigned" fact needs every function emitted. LogError throws out of this.
        RunNullIfaceGlobalCheck();

        // Escape hatch: skip the pass entirely without a rebuild if it ever misfires.
        const char* off = std::getenv("CFLAT_MOVE_DF_OFF");
        if (off && off[0] != '\0') { moveEventLog_.clear(); return; }

        const char* gate = std::getenv("CFLAT_MOVE_DF_VERIFY");
        bool verify = gate && gate[0] != '\0';

        movedf::Divergence earliest;
        bool haveEarliest = false;
        int total = 0;
        if (module)
        {
            for (auto& F : *module)
            {
                if (F.isDeclaration() || F.empty()) continue;
                auto it = moveEventLog_.find(&F);
                if (it == moveEventLog_.end()) continue;
                auto diverged = movedf::AnalyzeFunction(&F, it->second);
                total += (int)diverged.size();
                for (const auto& d : diverged)
                {
                    if (verify)
                        fprintf(stderr, "MOVE-DF: %s: maybe-moved use of '%s' at %d:%d\n",
                                F.getName().str().c_str(), d.path.c_str(), d.line, d.col);
                    // Only report a use with a real source location; pick globally earliest.
                    if (d.line <= 0) continue;
                    if (!haveEarliest || d.line < earliest.line ||
                        (d.line == earliest.line && d.col < earliest.col))
                        { earliest = d; haveEarliest = true; }
                }
            }
        }
        if (verify)
            fprintf(stderr, "MOVE-DF: summary: %d maybe-moved use(s)\n", total);

        // Clear BEFORE LogError - it throws, so this is the only reliable clear point on the
        // single-file CLI path (ResetForReanalysis also clears, but may not be reached here).
        moveEventLog_.clear();

        if (haveEarliest)
        {
            SetSourceLocation(earliest.line, earliest.col);
            LogError(std::format(
                "use of moved variable '{}' (moved on an earlier loop iteration)", earliest.path));
        }
    }

void LLVMBackend::MarkVariableOwningString(const std::string& name)
{
        if (name.empty()) return;
        for (auto& frame : std::ranges::reverse_view(stackNamedVariable))
        {
            if (auto it = frame.namedVariable.find(name); it != frame.namedVariable.end())
                { it->second.IsOwningString = true; return; }
            if (auto it = frame.functionArgument.find(name); it != frame.functionArgument.end())
                { it->second.IsOwningString = true; return; }
        }
    }

LLVMBackend::MovedStateSnapshot LLVMBackend::SaveMovedState() const
{
        MovedStateSnapshot state;
        for (const auto& frame : stackNamedVariable)
        {
            for (const auto& [name, nv] : frame.functionArgument)
                { state.moved[name] = nv.IsMoved; state.movedFields[name] = nv.MovedFields; }
            for (const auto& [name, nv] : frame.namedVariable)
                { state.moved[name] = nv.IsMoved; state.movedFields[name] = nv.MovedFields; }
        }
        return state;
    }

void LLVMBackend::RestoreMovedState(const MovedStateSnapshot& state)
{
        auto restoreOne = [&](const std::string& name, NamedVariable& nv) {
            if (auto it = state.moved.find(name); it != state.moved.end())
                nv.IsMoved = it->second;
            if (auto it = state.movedFields.find(name); it != state.movedFields.end())
                nv.MovedFields = it->second;
        };
        for (auto& frame : stackNamedVariable)
        {
            for (auto& [name, nv] : frame.functionArgument) restoreOne(name, nv);
            for (auto& [name, nv] : frame.namedVariable) restoreOne(name, nv);
        }
    }

void LLVMBackend::MergeMovedStateInto(const MovedStateSnapshot& state)
{
        auto mergeOne = [&](const std::string& name, NamedVariable& nv) {
            if (auto it = state.moved.find(name); it != state.moved.end() && it->second)
                nv.IsMoved = true;
            if (auto it = state.movedFields.find(name); it != state.movedFields.end())
                nv.MovedFields = movedf::MergeFieldPaths(nv.MovedFields, it->second);
        };
        for (auto& frame : stackNamedVariable)
        {
            for (auto& [name, nv] : frame.functionArgument) mergeOne(name, nv);
            for (auto& [name, nv] : frame.namedVariable) mergeOne(name, nv);
        }
    }

void LLVMBackend::MergeMovedStates(const MovedStateSnapshot& thenState,
                          const MovedStateSnapshot& elseState)
{
        auto mergeOne = [&](const std::string& name, NamedVariable& nv) {
            auto t = thenState.moved.find(name);
            auto e = elseState.moved.find(name);
            if (t != thenState.moved.end() && e != elseState.moved.end())
                nv.IsMoved = t->second || e->second;
            auto tf = thenState.movedFields.find(name);
            auto ef = elseState.movedFields.find(name);
            if (tf != thenState.movedFields.end() && ef != elseState.movedFields.end())
                nv.MovedFields = movedf::MergeFieldPaths(tf->second, ef->second);
        };
        for (auto& frame : stackNamedVariable)
        {
            for (auto& [name, nv] : frame.functionArgument) mergeOne(name, nv);
            for (auto& [name, nv] : frame.namedVariable) mergeOne(name, nv);
        }
    }

llvm::GlobalVariable* LLVMBackend::GetGlobalVariable(const std::string& name)
{
        auto result = globalNamedVariable.find(name);
        if (result != globalNamedVariable.end())
        {
            return result->second;
        }

        return nullptr;
    }

LLVMBackend::NamedVariable LLVMBackend::GetGlobalVariableNV(const std::string& name)
{
        auto gIt = globalNamedVariable.find(name);
        if (gIt == globalNamedVariable.end())
            return {};

        NamedVariable nv;
        nv.Storage  = gIt->second;
        nv.BaseType = gIt->second->getValueType();

        auto tvIt = globalVariableTypes.find(name);
        if (tvIt != globalVariableTypes.end())
            nv.TypeAndValue = tvIt->second;

        return nv;
    }

llvm::Constant* LLVMBackend::GetPlatformConstant()
{
        return llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), platformValue);
    }

void LLVMBackend::SetCompileTimeMacro(const std::string& name, llvm::Constant* value, const std::string& type)
{
        compileTimeMacros[name] = {name, value, type};
    }

void LLVMBackend::SetConstGlobalInt(const std::string& name, int64_t value)
{
        constGlobalInts_[name] = value;
    }

bool LLVMBackend::TryGetConstGlobalInt(const std::string& name, int64_t& out) const
{
        auto it = constGlobalInts_.find(name);
        if (it == constGlobalInts_.end()) return false;
        out = it->second;
        return true;
    }

void LLVMBackend::SetConstGlobalFP(const std::string& name)
{
        if (!name.empty()) constGlobalFPs_.insert(name);
    }

bool LLVMBackend::IsConstGlobalFP(const std::string& name) const
{
        return constGlobalFPs_.count(name) != 0;
    }

void LLVMBackend::SetPlatformMacros()
{
        auto i32 = llvm::Type::getInt32Ty(*context);
        SetCompileTimeMacro("__PLATFORM__", llvm::ConstantInt::get(i32, platformValue),            "int");
        SetCompileTimeMacro("__WIN64__",    llvm::ConstantInt::get(i32, platformValue == 64 ? 1 : 0), "int");
        SetCompileTimeMacro("__WIN32__",    llvm::ConstantInt::get(i32, platformValue == 32 ? 1 : 0), "int");
        SetCompileTimeMacro("__WINDOWS__",  llvm::ConstantInt::get(i32, targetWindows_ ? 1 : 0),   "int");
        // POSIX/Linux/macOS counterparts for the non-Windows targets. Both Linux
        // and macOS are POSIX; __LINUX__ and __MACOS__ are mutually exclusive and
        // select the os.posix.cb vs os.macos.cb core library in os.cb.
        const bool macos = targetMacOS_;
        const bool linux = !targetWindows_ && !macos;
        SetCompileTimeMacro("__POSIX__",    llvm::ConstantInt::get(i32, targetWindows_ ? 0 : 1),   "int");
        SetCompileTimeMacro("__LINUX__",    llvm::ConstantInt::get(i32, linux ? 1 : 0),            "int");
        SetCompileTimeMacro("__MACOS__",    llvm::ConstantInt::get(i32, macos ? 1 : 0),            "int");
        SetCompileTimeMacro("__DARWIN__",   llvm::ConstantInt::get(i32, macos ? 1 : 0),            "int");
        // Target architecture: every prior target was x86; arm64 is macOS-only for now.
        SetCompileTimeMacro("__X86__",      llvm::ConstantInt::get(i32, targetArm64_ ? 0 : 1),     "int");
        SetCompileTimeMacro("__ARM64__",    llvm::ConstantInt::get(i32, targetArm64_ ? 1 : 0),     "int");
        // --sanitize=ownership doubles as cflat's "runtime safety checks on" mode, so core
        // libraries can gate debug-only instrumentation (array.init_capacity poison fill) on it.
        SetCompileTimeMacro("__SANITIZE_OWNERSHIP__",
            llvm::ConstantInt::get(i32, sanitizeOwnership_ ? 1 : 0), "int");

        // Command-line -D defines land last, in command-line order, so a later -D of the
        // same name wins. Reserved names were rejected in SetUserDefines, so nothing above
        // can be shadowed here.
        for (const auto& def : userDefines_)
        {
            if (def.isInt)
            {
                SetCompileTimeMacro(def.name, llvm::ConstantInt::get(i32, def.intValue), "int");
                continue;
            }
            // String define: a private constant global, same shape as __FILE__. The symbol is
            // prefixed so a define can never collide with a real global of the same name.
            const std::string symbol = "__cflat_define_" + def.name;
            auto* global = module->getOrInsertGlobal(symbol,
                llvm::ArrayType::get(llvm::Type::getInt8Ty(*context), def.stringValue.size() + 1));
            if (auto* gv = llvm::dyn_cast<llvm::GlobalVariable>(global))
            {
                gv->setInitializer(llvm::ConstantDataArray::getString(*context, def.stringValue, true));
                gv->setConstant(true);
                SetCompileTimeMacro(def.name, gv, "string");
            }
        }
    }

bool LLVMBackend::IsReservedMacroName(const std::string& name)
{
        static const std::unordered_set<std::string> reserved = {
            "__PLATFORM__", "__WIN64__", "__WIN32__", "__WINDOWS__",
            "__POSIX__", "__LINUX__", "__MACOS__", "__DARWIN__",
            "__X86__", "__ARM64__", "__SANITIZE_OWNERSHIP__",
            "__FILE__", "__LINE__", "__FUNCTION__",
        };
        return reserved.count(name) != 0;
    }

bool LLVMBackend::SetUserDefines(const std::vector<std::string>& defines)
{
        userDefines_.clear();
        for (const auto& raw : defines)
        {
            auto eq = raw.find('=');
            std::string name  = (eq == std::string::npos) ? raw : raw.substr(0, eq);
            std::string value = (eq == std::string::npos) ? std::string("1") : raw.substr(eq + 1);
            if (name.empty() || (!std::isalpha((unsigned char)name[0]) && name[0] != '_'))
            {
                LogError(std::format("-D define name '{}' is not a valid identifier.", name));
                return false;
            }
            for (char c : name)
            {
                if (!std::isalnum((unsigned char)c) && c != '_')
                {
                    LogError(std::format("-D define name '{}' is not a valid identifier.", name));
                    return false;
                }
            }
            if (IsReservedMacroName(name))
            {
                LogError(std::format("-D cannot redefine the builtin compile-time macro '{}'.", name));
                return false;
            }

            UserDefine def;
            def.name = name;
            // A fully-consumed integer literal (base 0, so 0x/0 prefixes work) in int range
            // becomes an int macro; anything else is kept as a string macro.
            const char* begin = value.c_str();
            char* end = nullptr;
            errno = 0;
            long long parsed = std::strtoll(begin, &end, 0);
            if (!value.empty() && end != nullptr && *end == '\0' && errno == 0
                && parsed >= INT32_MIN && parsed <= INT32_MAX)
            {
                def.isInt = true;
                def.intValue = (int64_t)parsed;
            }
            else
            {
                def.stringValue = value;
            }
            // Later duplicates win: drop any earlier definition of the same name.
            std::erase_if(userDefines_, [&](const UserDefine& d) { return d.name == def.name; });
            userDefines_.push_back(std::move(def));
        }
        return true;
    }

LLVMBackend::CompileTimeMacro LLVMBackend::GetCompileTimeMacro(const std::string& name)
{
        auto it = compileTimeMacros.find(name);
        if (it != compileTimeMacros.end())
            return it->second;
        return {"", nullptr, ""};
    }

LLVMBackend::StructData LLVMBackend::GetDataStructure(const std::string& structName)
{
        EnsureCxxRecordProjected(structName, false);
        auto result = dataStructures.find(structName);
        if (result != dataStructures.end())
        {
            return result->second;
        }

        return {};
    }

LLVMBackend::StructData LLVMBackend::GetDataStructure(llvm::StructType* structType)
{
        for (const auto& [_, structData] : dataStructures)
        {
            if (structData.StructType == structType)
            {
                return structData;
            }
        }

        return {};
    }

void LLVMBackend::ApplyAbiCallAttributes(llvm::CallBase* ci, const AbiRecipe& recipe)
{
        unsigned attrIdx = 0;
        if (recipe.retSlot.kind == AbiSlot::SRetReturn)
        {
            const unsigned sretIdx = SRetArgIndex(recipe);
            ci->addParamAttr(sretIdx, llvm::Attribute::getWithStructRetType(*context, recipe.retSlot.structTy));
            ci->addParamAttr(sretIdx, llvm::Attribute::NoAlias);
            if (recipe.retSlot.align > 0)
                ci->addParamAttr(sretIdx, llvm::Attribute::getWithAlignment(*context, llvm::Align(recipe.retSlot.align)));
            if (sretIdx == 0) ++attrIdx;
        }
        for (size_t i = 0; i < recipe.paramSlots.size(); ++i)
        {
            const AbiSlot& s = recipe.paramSlots[i];
            if (i == 1 && SRetArgIndex(recipe) == 1) ++attrIdx;   // skip the sret slot behind `this`
            if (s.kind == AbiSlot::ByVal)
            {
                if (s.indirectByVal)
                    ci->addParamAttr(attrIdx, llvm::Attribute::getWithByValType(*context, s.structTy));
                if (s.align > 0)
                    ci->addParamAttr(attrIdx, llvm::Attribute::getWithAlignment(*context, llvm::Align(s.align)));
            }
            else if (s.kind == AbiSlot::Direct)
            {
                if (s.signExt) ci->addParamAttr(attrIdx, llvm::Attribute::SExt);
                if (s.zeroExt) ci->addParamAttr(attrIdx, llvm::Attribute::ZExt);
            }
            attrIdx += SlotLLVMParamCount(s); // CoercePair expands to two LLVM params
        }
    }

/*
 * Per-slot argument lowering shared by every ABI-lowered call: the extern/C++ symbol door
 * (EmitAbiLoweredCall) and a call through a thin function<> value (the C ABI of its signature).
 * Appends the lowered LLVM arguments for argList to loweredArgs; the sret slot is NOT added here.
 */
void LLVMBackend::LowerAbiArgs(const AbiRecipe& recipe, const std::vector<llvm::Value*>& argList,
                               const std::vector<llvm::Value*>* indirectArgAddrs,
                               const std::vector<llvm::Value*>* rawArrayCounts,
                               std::vector<llvm::Value*>& loweredArgs)
{
        for (size_t i = 0; i < argList.size() && i < recipe.paramSlots.size(); ++i)
        {
            const AbiSlot& s = recipe.paramSlots[i];
            llvm::Value* v = argList[i];
            if (s.kind == AbiSlot::Direct)
            {
                loweredArgs.push_back(v);
                if (!s.countAfter && rawArrayCounts != nullptr && i < rawArrayCounts->size()
                    && (*rawArrayCounts)[i] != nullptr)
                    loweredArgs.push_back((*rawArrayCounts)[i]);
            }
            else if (s.kind == AbiSlot::Ignore)
            {
                // Clang passes nothing for this argument.
            }
            else if (s.kind == AbiSlot::CoerceToInt)
            {
                // v is a struct value. Place in an alloca, then load the eightbyte through a
                // bitcast - portable across all element layouts and lets LLVM coalesce. The
                // coerce type may be integer or SSE (float/double/<2 x float>) under SysV.
                auto* slot = AllocaForCoerce(s.structTy, s.coerceTy, s.align, "abi.coerce");
                builder->CreateStore(v, slot);
                loweredArgs.push_back(LoadCoerceAt(slot, s.coerceTy, 0));
            }
            else if (s.kind == AbiSlot::CoerceFlat)
            {
                // Clang's general direct coercion: store the record, then hand over one argument
                // per element of the coerce struct, read at THAT struct's own field offsets.
                auto* slot = AllocaForCoerce(s.structTy, s.coerceStructTy, s.align, "abi.coerce");
                builder->CreateStore(v, slot);
                const llvm::StructLayout* sl = module->getDataLayout().getStructLayout(s.coerceStructTy);
                for (unsigned e = 0; e < s.coerceStructTy->getNumElements(); ++e)
                    loweredArgs.push_back(LoadCoerceAt(slot, s.coerceStructTy->getElementType(e),
                                                       sl->getElementOffset(e)));
            }
            else if (s.kind == AbiSlot::CoercePair)
            {
                // SysV: two eightbytes passed as two separate scalar params (eightbyte 0 at
                // byte offset 0, eightbyte 1 at byte offset 8).
                auto* slot = AllocaAtEntry(s.structTy, nullptr, "abi.coerce", s.align);
                builder->CreateStore(v, slot);
                loweredArgs.push_back(LoadCoerceAt(slot, s.coerceTy, 0));
                loweredArgs.push_back(LoadCoerceAt(slot, s.coerceTy2, 8));
            }
            else // ByVal
            {
                // A caller-prepared address (a nontrivial C++ temp that was copy- or
                // move-CONSTRUCTED) is handed over as is: storing a loaded struct over it would
                // be exactly the byte copy the C++ type forbids.
                llvm::Value* ready = indirectArgAddrs != nullptr && i < indirectArgAddrs->size()
                    ? (*indirectArgAddrs)[i] : nullptr;
                if (ready != nullptr)
                {
                    loweredArgs.push_back(ready);
                }
                else
                {
                    auto* slot = AllocaAtEntry(s.structTy, nullptr, "abi.byval", s.align);
                    builder->CreateStore(v, slot);
                    loweredArgs.push_back(slot);
                }
            }
            // The C-ABI count of a counted parameter follows its slots; a missing count is the
            // unknown-count sentinel, as at every other call that cannot derive one.
            if (s.countAfter)
            {
                llvm::Value* count = rawArrayCounts != nullptr && i < rawArrayCounts->size()
                    ? (*rawArrayCounts)[i] : nullptr;
                loweredArgs.push_back(count != nullptr ? count : builder->getInt64(-1));
            }
        }

}

/*
 * Decodes the register-returned value of an ABI-lowered call into the natural CFlat value. An sret
 * return is decoded by the caller (its slot is the result); Ignore is decided by the caller too.
 */
llvm::Value* LLVMBackend::DecodeAbiRegisterReturn(const AbiRecipe& recipe, llvm::Value* ci)
{
        if (recipe.retSlot.kind == AbiSlot::CoerceToInt)
        {
            auto* slot = AllocaForCoerce(recipe.retSlot.structTy, recipe.retSlot.coerceTy,
                                         recipe.retSlot.align, "abi.ret");
            StoreCoerceAt(slot, ci, 0);
            return builder->CreateLoad(recipe.retSlot.structTy, slot);
        }
        if (recipe.retSlot.kind == AbiSlot::CoerceFlat)
        {
            const AbiSlot& rs = recipe.retSlot;
            auto* slot = AllocaForCoerce(rs.structTy, rs.coerceStructTy, rs.align, "abi.ret");
            const llvm::StructLayout* sl = module->getDataLayout().getStructLayout(rs.coerceStructTy);
            for (unsigned e = 0; e < rs.coerceStructTy->getNumElements(); ++e)
                StoreCoerceAt(slot, builder->CreateExtractValue(ci, e), sl->getElementOffset(e));
            return builder->CreateLoad(rs.structTy, slot);
        }
        if (recipe.retSlot.kind == AbiSlot::CoercePair)
        {
            // SysV: result is a { eightbyte0, eightbyte1 } aggregate; scatter each half back
            // into the struct slot at byte offsets 0 and 8, then reload the natural struct.
            auto* slot = AllocaAtEntry(recipe.retSlot.structTy, nullptr, "abi.ret", recipe.retSlot.align);
            StoreCoerceAt(slot, builder->CreateExtractValue(ci, 0), 0);
            StoreCoerceAt(slot, builder->CreateExtractValue(ci, 1), 8);
            return builder->CreateLoad(recipe.retSlot.structTy, slot);
        }
        return ci; // Direct return
}

llvm::Value* LLVMBackend::EmitAbiLoweredCall(const FunctionSymbol& candidate, std::vector<llvm::Value*>& argList,
                                            llvm::Value* sretDest,
                                            const std::vector<llvm::Value*>* indirectArgAddrs,
                                            llvm::Value* calleeOverride,
                                            const std::vector<llvm::Value*>* rawArrayCounts,
                                            bool mayUnwind,
                                            const AbiRecipe* callRecipe)
{
        const AbiRecipe& recipe = callRecipe != nullptr ? *callRecipe : candidate.Recipe;
        std::vector<llvm::Value*> loweredArgs;
        loweredArgs.reserve(argList.size() + (recipe.retSlot.kind == AbiSlot::SRetReturn ? 1 : 0));

        llvm::Value* sretSlot = nullptr;
        const bool sretIntoCallerSlot = sretDest != nullptr
            && recipe.retSlot.kind == AbiSlot::SRetReturn;
        if (recipe.retSlot.kind == AbiSlot::SRetReturn)
        {
            sretSlot = sretIntoCallerSlot
                ? sretDest
                : AllocaAtEntry(recipe.retSlot.structTy, nullptr, "sret", recipe.retSlot.align);
            loweredArgs.push_back(sretSlot);
        }

        if (callRecipe != nullptr)
        {
            std::vector<llvm::Value*> promotedArgs = argList;
            if (candidate.Variadic)
                for (size_t t = candidate.Parameters.size(); t < promotedArgs.size(); ++t)
                    promotedArgs[t] = PromoteVariadicArgument(promotedArgs[t]);
            LowerAbiArgs(recipe, promotedArgs, indirectArgAddrs, rawArrayCounts, loweredArgs);
        }
        else
        {
            LowerAbiArgs(recipe, argList, indirectArgAddrs, rawArrayCounts, loweredArgs);
        }
        // MS ABI instance method: the hidden slot was pushed first, the ABI wants it after `this`.
        if (SRetArgIndex(recipe) == 1 && loweredArgs.size() > 1)
            std::swap(loweredArgs[0], loweredArgs[1]);
        // Without a call-site arrangement only scalar varargs can be appended to the declaration
        // recipe. Aggregate variadic tails use the full Clang call arrangement above.
        if (candidate.Variadic && callRecipe == nullptr)
            for (size_t t = recipe.paramSlots.size(); t < argList.size(); ++t)
                loweredArgs.push_back(PromoteVariadicArgument(argList[t]));

        // A virtual member is reached through the pointer loaded out of the receiver's vptr; the
        // SIGNATURE still comes from the declaration, which carries clang's own arrangement.
        auto* ci = CreateCallOrInvoke(candidate.Function->getFunctionType(),
            calleeOverride != nullptr ? calleeOverride : candidate.Function, loweredArgs, mayUnwind);
        ci->setCallingConv(candidate.Function->getCallingConv());
        ApplyAbiCallAttributes(ci, recipe);

        if (recipe.retSlot.kind == AbiSlot::SRetReturn)
        {
            // Constructed straight into the caller's storage: the slot IS the result.
            if (sretIntoCallerSlot) return sretSlot;
            return builder->CreateLoad(recipe.retSlot.structTy, sretSlot);
        }
        if (recipe.retSlot.kind == AbiSlot::Ignore)
            return llvm::Constant::getNullValue(GetCCompatibleType(candidate.ReturnType));
        return DecodeAbiRegisterReturn(recipe, ci);
    }

/*
 * Scratch slot for a coercion. Clang may coerce a record to a type WIDER than the record itself
 * (a 12-byte aggregate to [2 x i64] on AArch64), so a plain alloca of the record would be read
 * past its end. Allocate whichever of the two is larger, and at the stricter alignment.
 */
llvm::AllocaInst* LLVMBackend::AllocaForCoerce(llvm::Type* structTy, llvm::Type* coerceTy,
                                               uint64_t align, const char* name)
{
        const llvm::DataLayout& dl = module->getDataLayout();
        llvm::Type* holder = structTy;
        if (coerceTy != nullptr
            && (uint64_t)dl.getTypeAllocSize(coerceTy) > (uint64_t)dl.getTypeAllocSize(structTy))
            holder = coerceTy;
        uint64_t want = std::max<uint64_t>(align, dl.getABITypeAlign(structTy).value());
        if (coerceTy != nullptr) want = std::max<uint64_t>(want, dl.getABITypeAlign(coerceTy).value());
        return AllocaAtEntry(holder, nullptr, name, want);
    }

llvm::Value* LLVMBackend::LoadCoerceAt(llvm::Value* structSlot, llvm::Type* coerceTy, uint64_t byteOff)
{
        llvm::Value* p = structSlot;
        if (byteOff != 0)
        {
            auto* i8p = builder->CreateBitCast(structSlot, cflat_llvm::PointerTo(builder->getInt8Ty()));
            p = builder->CreateInBoundsGEP(builder->getInt8Ty(), i8p, builder->getInt64(byteOff));
        }
        auto* cp = builder->CreateBitCast(p, cflat_llvm::PointerTo(coerceTy));
        return builder->CreateLoad(coerceTy, cp);
    }

void LLVMBackend::StoreCoerceAt(llvm::Value* structSlot, llvm::Value* val, uint64_t byteOff)
{
        llvm::Value* p = structSlot;
        if (byteOff != 0)
        {
            auto* i8p = builder->CreateBitCast(structSlot, cflat_llvm::PointerTo(builder->getInt8Ty()));
            p = builder->CreateInBoundsGEP(builder->getInt8Ty(), i8p, builder->getInt64(byteOff));
        }
        auto* cp = builder->CreateBitCast(p, cflat_llvm::PointerTo(val->getType()));
        builder->CreateStore(val, cp);
    }

/*
 * C default argument promotions for one variadic argument: i1 zero-extends, 8/16-bit ints
 * sign-extend to i32, float and half widen to double. Every variadic tail goes through here,
 * the plain call path and the ABI-lowered (sret) path alike.
 */
std::string LLVMBackend::AggregateVariadicArgumentType(const NamedVariable& arg) const
{
        if (arg.TypeAndValue.Pointer) return "";
        if (arg.Primary != nullptr
            && arg.Primary->getType() == llvm::StructType::getTypeByName(*context, "string"))
            return "";
        std::string name = arg.TypeAndValue.TypeName;
        if (name.empty())
            if (auto* st = llvm::dyn_cast_or_null<llvm::StructType>(
                    arg.Primary != nullptr ? arg.Primary->getType() : arg.BaseType))
                name = st->getName().str();
        if (name.empty()) return "";
        if (IsDataStructure(name)
            || (arg.Primary != nullptr && arg.Primary->getType()->isStructTy()))
            return name;
        return "";
}

llvm::Value* LLVMBackend::PromoteVariadicArgument(llvm::Value* value)
{
        auto valueType = value->getType();
        // bool (i1) is zero-extended: sign-extending it would pass true as -1.
        if (valueType->isIntegerTy(1))
            return builder->CreateZExt(value, builder->getInt32Ty(), "conv");
        if (valueType->isIntegerTy(8) || valueType->isIntegerTy(16))
            return builder->CreateSExt(value, builder->getInt32Ty(), "conv");
        if (valueType->is16bitFPTy() || valueType->isFloatTy())
            return builder->CreateFPExt(value, builder->getDoubleTy(), "conv");
        if (valueType->isVectorTy())
        {
            // A simd vector has no default argument promotion, so printf would read
            // whatever the ABI put in the first slot - 1065353216 for a 1.0f splat.
            auto* fv = llvm::dyn_cast<llvm::FixedVectorType>(valueType);
            LogError(std::format(
                "cannot pass simd vector storage with {} lanes to a variadic function - "
                "pass one lane with 'v[i]', or reduce it with 'simd<T,N>.reduce_add(v)'.",
                fv != nullptr ? (uint64_t)fv->getNumElements() : (uint64_t)0));
        }
        return value;
}

llvm::Value* LLVMBackend::CreateFunctionCall(llvm::Function* func, const std::vector<llvm::Value*>& arg,
                                            bool mayUnwind)
{
        // Perform Default Argument Promotions for Variadic arguments
        std::vector<llvm::Value*> callArgs;
        if (func->isVarArg())
        {
            size_t varArgStart = func->arg_size();
            for (auto value : arg)
            {
                if (varArgStart > 0)
                {
                    auto destArgument = func->getArg(static_cast<unsigned int>(func->arg_size() - varArgStart));
                    callArgs.push_back(Upconvert(value, destArgument));
                    varArgStart--;
                    continue;
                }
                callArgs.push_back(PromoteVariadicArgument(value));
            }
        }
        else
        {
            for (int i = 0; i < (int)arg.size(); i++)
                callArgs.push_back(Upconvert(arg[i], func->getArg(i)));
        }

        auto* ci = CreateCallOrInvoke(func->getFunctionType(), func, callArgs, mayUnwind);
        ci->setCallingConv(func->getCallingConv());
        return ci;
    }

bool LLVMBackend::IsOwningValue(llvm::Value* value) const
{
        if (!value) return false;
        auto* loadInst = llvm::dyn_cast<llvm::LoadInst>(value);
        if (!loadInst) return false;
        auto* srcAlloca = loadInst->getPointerOperand();
        for (const auto& frame : stackNamedVariable)
        {
            for (const auto& [varName, nv] : frame.namedVariable)
                if (nv.Storage == srcAlloca && nv.IsOwning) return true;
            for (const auto& [varName, nv] : frame.functionArgument)
                if (nv.Storage == srcAlloca && nv.IsOwning) return true;
        }
        return false;
    }

const LLVMBackend::NamedVariable* LLVMBackend::FindVariableByStorage(const llvm::Value* slot) const
{
        if (slot == nullptr) return nullptr;
        for (const auto& frame : stackNamedVariable)
        {
            for (const auto& [varName, nv] : frame.namedVariable)
                if (nv.Storage == slot) return &nv;
            for (const auto& [varName, nv] : frame.functionArgument)
                if (nv.Storage == slot) return &nv;
        }
        return nullptr;
    }

bool LLVMBackend::IsNativeConstObject(const TypeAndValue& tv) const
{
        return tv.IsConstObject() && !IsCxxRecord(tv.TypeName);
    }

bool LLVMBackend::IsNativeConstGlobal(const NamedVariable& globalNV) const
{
        auto* global = llvm::dyn_cast_or_null<llvm::GlobalVariable>(globalNV.Storage);
        if (global == nullptr || !IsNativeConstObject(globalNV.TypeAndValue)) return false;
        const std::string symbol = global->getName().str();
        return cGlobalSymbols_.count(symbol) == 0 && cxxHeaderGlobalSymbols_.count(symbol) == 0;
    }

std::optional<LLVMBackend::ConstObjectRoot> LLVMBackend::FindConstObjectRoot(
    const llvm::Value* address) const
{
        if (address == nullptr) return std::nullopt;
        // A binding whose storage IS the address or a GEP base of it first: a range-for element
        // borrow, or a lambda's by-value env slot (the slot is itself a GEP off the env).
        for (const llvm::Value* step = address; step != nullptr;)
        {
            for (auto frame = stackNamedVariable.rbegin(); frame != stackNamedVariable.rend(); ++frame)
                for (const auto& [varName, nv] : frame->namedVariable)
                    if (nv.Storage == step && nv.TypeAndValue.IsConstObject())
                        return ConstObjectRoot{ varName, nv.Storage, nv.BaseType };
            if (auto* gep = llvm::dyn_cast<llvm::GEPOperator>(step)) step = gep->getPointerOperand();
            else step = nullptr;
        }
        const llvm::Value* root = llvm::getUnderlyingObject(address, 0);
        if (llvm::isa<llvm::AllocaInst>(root) || llvm::isa<llvm::Argument>(root))
        {
            for (auto frame = stackNamedVariable.rbegin(); frame != stackNamedVariable.rend(); ++frame)
            {
                for (const auto& [varName, nv] : frame->namedVariable)
                    if (nv.Storage == root && nv.TypeAndValue.IsConstObject())
                        return ConstObjectRoot{ varName, nv.Storage, nv.BaseType };
                for (const auto& [varName, nv] : frame->functionArgument)
                    if (nv.Storage == root && nv.TypeAndValue.IsConstObject())
                        return ConstObjectRoot{ varName, nv.Storage, nv.BaseType };
            }
            return std::nullopt;
        }
        // A lambda's by-reference capture of a const object: its storage (struct) or decayed
        // element pointer (array) is the address loaded from the env, so match that load exactly.
        if (llvm::isa<llvm::LoadInst>(root))
        {
            for (auto frame = stackNamedVariable.rbegin(); frame != stackNamedVariable.rend(); ++frame)
                for (const auto& [varName, nv] : frame->namedVariable)
                    if (nv.IsClosureRefCapture && nv.TypeAndValue.IsConstObject()
                        && (nv.Storage == root || (nv.Storage == nullptr && nv.Primary == root)))
                        return ConstObjectRoot{ varName, const_cast<llvm::Value*>(root), nv.BaseType };
            return std::nullopt;
        }
        auto* global = llvm::dyn_cast<llvm::GlobalVariable>(root);
        if (global == nullptr) return std::nullopt;
        const std::string symbol = global->getName().str();
        if (cGlobalSymbols_.count(symbol) != 0 || cxxHeaderGlobalSymbols_.count(symbol) != 0)
            return std::nullopt;
        // The symbol name is the usual key; scan only when a namespaced key spells it differently.
        if (auto direct = globalNamedVariable.find(symbol);
            direct != globalNamedVariable.end() && direct->second == global)
        {
            auto tv = globalVariableTypes.find(symbol);
            if (tv != globalVariableTypes.end() && tv->second.IsConstObject())
                return ConstObjectRoot{ symbol, direct->second, global->getValueType() };
            return std::nullopt;
        }
        for (const auto& [varName, slot] : globalNamedVariable)
        {
            if (slot != global) continue;
            auto tv = globalVariableTypes.find(varName);
            if (tv != globalVariableTypes.end() && tv->second.IsConstObject())
                return ConstObjectRoot{ varName, slot, slot->getValueType() };
        }
        return std::nullopt;
    }

namespace
{
    // The pointee level of a pointer (or view) slot is one bit below the object bit.
    bool ConstSlotPointeeIsConst(const LLVMBackend::TypeAndValue& tv)
    {
        const int depth = tv.ValuePointerDepth();
        return depth >= 1 && ((tv.ConstMask >> (depth - 1)) & 1u) != 0;
    }
}

/*
 * The declared type of the slot a pointer was loaded from: a live binding, a native or C header
 * global, a native struct field (through any array steps), or an element of an array binding.
 * Null when the slot is anything else (pointer arithmetic, a temporary).
 */
const LLVMBackend::TypeAndValue* LLVMBackend::DeclaredConstSlot(const llvm::Value* slot,
    std::string& name, int depth) const
{
        if (slot == nullptr || depth > 8) return nullptr;
        if (const auto* nv = FindVariableByStorage(slot))
        {
            name = nv->TypeAndValue.VariableName;
            return &nv->TypeAndValue;
        }
        if (auto* global = llvm::dyn_cast<llvm::GlobalVariable>(slot))
        {
            // A C or C++ header global spells its levels in ConstMask.
            for (const auto& [varName, storage] : globalNamedVariable)
            {
                if (storage != global) continue;
                auto tv = globalVariableTypes.find(varName);
                if (tv == globalVariableTypes.end()) return nullptr;
                name = varName;
                return &tv->second;
            }
            return nullptr;
        }
        auto* gep = llvm::dyn_cast<llvm::GEPOperator>(slot);
        if (gep == nullptr || gep->getNumIndices() < 2) return nullptr;
        const LLVMBackend::TypeAndValue* field = nullptr;
        bool firstIndex = true;
        for (auto it = llvm::gep_type_begin(gep), end = llvm::gep_type_end(gep); it != end; ++it)
        {
            auto* index = llvm::dyn_cast<llvm::ConstantInt>(it.getOperand());
            if (firstIndex)
            {
                firstIndex = false;
                if (index == nullptr || !index->isZero()) return nullptr;
                continue;
            }
            auto* structType = it.getStructTypeOrNull();
            if (structType == nullptr) continue;     // a fixed-array element keeps the declaration
            if (index == nullptr) return nullptr;
            if (!structType->hasName())
            {
                // A view's fat {ptr, len} slot: the view binding's own declaration.
                std::string baseName;
                const auto* base = DeclaredConstSlot(gep->getPointerOperand(), baseName, depth + 1);
                if (field != nullptr || base == nullptr || !base->IsArrayView) return nullptr;
                name = baseName;
                return base;
            }
            field = FindDeclaredFieldOfStructType(structType, index->getZExtValue());
            if (field == nullptr) return nullptr;
            name = field->VariableName;
        }
        if (field != nullptr) return field;
        return DeclaredConstSlot(gep->getPointerOperand(), name, depth + 1);
}

LLVMBackend::ConstWriteCause LLVMBackend::FindConstWriteCause(const llvm::Value* address) const
{
        ConstWriteCause cause;
        if (address == nullptr) return cause;
        // A C++ mutable member is writable inside a const object.
        if (IsCxxMutableFieldAccess(const_cast<llvm::Value*>(address))) return cause;
        // Const native fields on the GEP chain, innermost first. The running constructor's own
        // fields (first struct step off its `this`) are still being built.
        const llvm::Value* base = address;
        bool fieldStep = false;     // the address is a field (of a field ...) of what `base` holds
        const llvm::GEPOperator* ownerGep = nullptr;   // the step taken directly off `base`
        while (auto* gep = llvm::dyn_cast<llvm::GEPOperator>(base))
        {
            ownerGep = gep;
            const bool ownThis = IsConstructingStorage(gep->getPointerOperand());
            const TypeAndValue* constField = nullptr;
            bool cxxField = false;
            bool cxxOwner = false;
            if (const llvm::MDNode* md = llvm::isa<llvm::Instruction>(gep)
                    ? llvm::cast<llvm::Instruction>(gep)->getMetadata("cflat.cxx.field") : nullptr)
            {
                // Offset-layout field address: one field, named by the metadata.
                fieldStep = true;
                auto* owner = md->getNumOperands() == 2 ? llvm::dyn_cast<llvm::MDString>(md->getOperand(0)) : nullptr;
                const TypeAndValue* field = FindDeclaredFieldTypeAndValueForStorage(gep);
                if (owner != nullptr && field != nullptr && field->IsConstObject() && !ownThis)
                {
                    constField = field;
                    cxxOwner = IsCxxRecord(owner->getString().str());
                    cxxField = cxxOwner && IsCxxRecord(field->TypeName);
                }
            }
            else
            {
                bool firstStruct = true;
                for (auto it = llvm::gep_type_begin(gep), end = llvm::gep_type_end(gep); it != end; ++it)
                {
                    auto* structType = it.getStructTypeOrNull();
                    if (structType == nullptr) continue;
                    fieldStep = true;
                    const bool exempt = ownThis && firstStruct;
                    firstStruct = false;
                    auto* index = llvm::dyn_cast<llvm::ConstantInt>(it.getOperand());
                    if (index == nullptr || !structType->hasName()) continue;
                    const TypeAndValue* field = FindDeclaredFieldOfStructType(structType, index->getZExtValue());
                    if (field != nullptr && field->IsConstObject() && !exempt)
                    {
                        constField = field;
                        cxxOwner = IsCxxRecord(structType->getName().str());
                        cxxField = cxxOwner && IsCxxRecord(field->TypeName);
                    }
                }
            }
            if (constField != nullptr)
            {
                cause.Kind = ConstWriteCause::CauseKind::Field;
                cause.Name = constField->VariableName;
                cause.CxxObject = cxxField;
                cause.CxxShallow = cxxOwner;
                return cause;
            }
            base = gep->getPointerOperand();
        }
        if (auto root = FindConstObjectRoot(address))
        {
            cause.Kind = ConstWriteCause::CauseKind::Object;
            cause.Root = *root;
            llvm::Type* objectType = root->Type;
            while (auto* array = llvm::dyn_cast_or_null<llvm::ArrayType>(objectType))
                objectType = array->getElementType();
            auto* structType = llvm::dyn_cast_or_null<llvm::StructType>(objectType);
            cause.CxxObject = structType != nullptr && structType->hasName()
                && IsCxxRecord(structType->getName().str());
            cause.CxxShallow = cause.CxxObject;
            return cause;
        }
        // An explicit cast decides the qualification of everything behind it (R7 A).
        std::string pointerName;
        if (auto* inst = llvm::dyn_cast<llvm::Instruction>(base);
            inst != nullptr && inst->getMetadata("cflat.const.cast") != nullptr)
        {
            if ((ValueConstLevels(base, pointerName) & 1u) == 0) return cause;
            cause.Kind = ConstWriteCause::CauseKind::ThroughPointer;
            cause.Name = pointerName;
            return cause;
        }
        /*
         * A pointer value P. Refused on a field path off it (`ps->n`, `get()->n`, `(c ? p : q)->n`)
         * when P's type has a const pointee, when P was read out of a slot lying strictly inside
         * const storage (transitive const: `*s.p`), or (bare `*p`, `p[i]`, `**pp`) when the
         * pointee level P reaches is const.
         */
        // A pointer a C++ declaration produced (a C++ field's value, a C++ call result): C++ const.
        const auto cxxPointer = [&]() {
            if (auto* load = llvm::dyn_cast<llvm::LoadInst>(base))
                if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(load->getPointerOperand()))
                {
                    if (const llvm::MDNode* md = gep->getMetadata("cflat.cxx.field"))
                        if (md->getNumOperands() == 2)
                            if (auto* owner = llvm::dyn_cast<llvm::MDString>(md->getOperand(0)))
                                return IsCxxRecord(owner->getString().str());
                    auto* type = llvm::dyn_cast<llvm::StructType>(gep->getSourceElementType());
                    return type != nullptr && type->hasName() && IsCxxRecord(type->getName().str());
                }
            return false;
        };
        // A field path into a C++ object behind P: C++ const is shallow whatever produced P.
        const auto cxxOwner = [&]() {
            if (ownerGep == nullptr) return false;
            if (auto* inst = llvm::dyn_cast<llvm::Instruction>(ownerGep))
                if (const llvm::MDNode* md = inst->getMetadata("cflat.cxx.field"))
                    if (md->getNumOperands() == 2)
                        if (auto* owner = llvm::dyn_cast<llvm::MDString>(md->getOperand(0)))
                            return IsCxxRecord(owner->getString().str());
            llvm::Type* type = ownerGep->getSourceElementType();
            while (auto* array = llvm::dyn_cast<llvm::ArrayType>(type)) type = array->getElementType();
            auto* structType = llvm::dyn_cast<llvm::StructType>(type);
            return structType != nullptr && structType->hasName() && IsCxxRecord(structType->getName().str());
        };
        if (fieldStep && PointerValuePointeeConst(base, pointerName))
        {
            cause.Kind = ConstWriteCause::CauseKind::ThroughPointer;
            cause.Name = pointerName;
            cause.CxxShallow = cxxPointer() || cxxOwner();
            return cause;
        }
        if (auto* load = llvm::dyn_cast<llvm::LoadInst>(base))
        {
            const llvm::Value* slot = load->getPointerOperand();
            ConstWriteCause holder = FindConstWriteCause(slot);
            // A const slot reached through P's holder pointer protects P's pointee only if its
            // levels say so (`R* const*` keeps **P writable; native const fills every level).
            std::string holderName;
            if (holder.Kind == ConstWriteCause::CauseKind::ThroughPointer
                && ((StoredConstLevels(slot, holderName) >> 1) & 1u) == 0)
                holder.Kind = ConstWriteCause::CauseKind::None;
            if (holder.Kind != ConstWriteCause::CauseKind::None && !holder.CxxShallow
                && !(holder.Kind == ConstWriteCause::CauseKind::Object && holder.Root.Storage == slot))
                return holder;
        }
        if (!fieldStep && PointerValuePointeeConst(base, pointerName))
        {
            cause.Kind = ConstWriteCause::CauseKind::ThroughPointer;
            cause.Name = pointerName;
            cause.CxxShallow = cxxPointer();
        }
        return cause;
}

namespace
{
    // Top-relative const levels of a declared type: bit 0 = the declared object, bit k = the
    // k-th level reached through it. Spelled masks are downward closed, so a set bit fills deeper.
    uint8_t DeclaredTopLevels(const LLVMBackend::TypeAndValue& tv)
    {
        uint8_t levels = 0;
        const int depth = tv.ValuePointerDepth();
        for (int j = 0; j <= depth; ++j)
            if (((tv.ConstMask >> (depth - j)) & 1u) != 0) levels |= static_cast<uint8_t>(1u << j);
        // A mask that is not downward closed is a C/C++ spelling (`R* const*`): shallow, exact.
        const uint8_t spelled = static_cast<uint8_t>(tv.ConstMask & ((2u << depth) - 1u));
        if ((spelled & (spelled + 1u)) != 0) return levels;
        for (int j = 0; j < 8; ++j)
            if (((levels >> j) & 1u) != 0) return static_cast<uint8_t>(0xFFu << j);
        return 0;
    }

    // C/C++ (shallow) const: exactly the levels spelled, never filled (`int* const` leaves *p writable).
    uint8_t ExactTopLevels(const LLVMBackend::TypeAndValue& tv)
    {
        uint8_t levels = 0;
        const int depth = tv.ValuePointerDepth();
        for (int j = 0; j <= depth; ++j)
            if (((tv.ConstMask >> (depth - j)) & 1u) != 0) levels |= static_cast<uint8_t>(1u << j);
        return levels;
    }

    uint8_t LevelsBelow(uint8_t levels) { return static_cast<uint8_t>((levels >> 1) | (levels & 0x80u)); }
}

uint8_t LLVMBackend::DeclaredPointeeConstLevels(const TypeAndValue& tv) const
{
        return LevelsBelow(DeclaredTopLevels(tv));
}

// Whether a string literal (or a join arm that is one) names another element type than the
// one-level pointer target: a type mismatch, reported by the type check rather than as #5-#7.
bool LLVMBackend::LiteralMismatchesPointee(const llvm::Value* value, const TypeAndValue& source,
    const TypeAndValue& target, int depth, int guard) const
{
        if (value == nullptr || guard > 8) return false;
        if (auto* select = llvm::dyn_cast<llvm::SelectInst>(value))
            return LiteralMismatchesPointee(select->getTrueValue(), source, target, depth, guard + 1)
                || LiteralMismatchesPointee(select->getFalseValue(), source, target, depth, guard + 1);
        if (auto* phi = llvm::dyn_cast<llvm::PHINode>(value))
        {
            for (const llvm::Value* arm : phi->incoming_values())
                if (LiteralMismatchesPointee(arm, source, target, depth, guard + 1)) return true;
            return false;
        }
        if (!IsStringLiteralGlobal(value)) return false;
        const std::string targetName = CanonicalPrimitiveTypeName(ResolveTypeAlias(target.TypeName));
        if (depth != 1) return true;
        if (targetName == "void") return false;
        // A prefixed literal's own type names it (`u8"x"` is c8, same width as char); otherwise
        // compare the element type (a join arm or a defaulted argument carries no literal type).
        if (guard == 0 && source.Pointer)
        {
            const std::string sourceName = CanonicalPrimitiveTypeName(ResolveTypeAlias(source.TypeName));
            if (sourceName == "c8" || sourceName == "c16" || sourceName == "c32" || sourceName == "wchar")
                return sourceName != targetName;
        }
        const auto* global = llvm::dyn_cast<llvm::GlobalVariable>(value->stripPointerCasts());
        const auto* array = global != nullptr ? llvm::dyn_cast<llvm::ArrayType>(global->getValueType()) : nullptr;
        if (array == nullptr) return true;
        TypeAndValue pointee = target;
        pointee.Pointer = false;
        pointee.ElemPointer = false;
        pointee.PointerDepth = 0;
        pointee.IsArrayView = false;
        pointee.ConstMask = 0;
        return GetType(pointee) != array->getElementType();
}

bool LLVMBackend::IsStringLiteralGlobal(const llvm::Value* value) const
{
        // Constant expressions only: a cast instruction (`(char*)"x"`) carries its own levels.
        return value != nullptr && !llvm::isa<llvm::Instruction>(value)
            && stringLiteralGlobals_.count(value->stripPointerCasts()) != 0;
}

/*
 * Const levels of the object stored at `address` (bit 0) and of what its value reaches (bit k):
 * a const write cause on the address makes everything behind it const (transitive, R11);
 * otherwise a declared slot gives its spelled levels, and a computed address its value's.
 */
uint8_t LLVMBackend::StoredConstLevels(const llvm::Value* address, std::string& name, int depth) const
{
        if (address == nullptr || depth > 8) return 0;
        if (IsStringLiteralGlobal(address)) return ValueConstLevels(address, name, depth + 1);
        if (auto* inst = llvm::dyn_cast<llvm::Instruction>(address);
            inst != nullptr && inst->getMetadata("cflat.const.cast") != nullptr)
            return ValueConstLevels(address, name, depth + 1);
        if (auto cause = FindConstWriteCause(address); cause.Kind != ConstWriteCause::CauseKind::None)
        {
            name = cause.Kind == ConstWriteCause::CauseKind::Object ? cause.Root.Name : cause.Name;
            // `*P` (no field step): exactly P's own levels, so `R* const*` leaves **P writable.
            if (!cause.CxxShallow && cause.Kind == ConstWriteCause::CauseKind::ThroughPointer
                && !llvm::isa<llvm::GEPOperator>(address))
                return ValueConstLevels(address, name, depth + 1);
            if (!cause.CxxShallow) return 0xFF;
            // Shallow C++ const: the slot itself, and what its own declaration spells below it.
            std::string slotName;
            const auto* tv = DeclaredConstSlot(address, slotName);
            return static_cast<uint8_t>(1u | (tv != nullptr ? ExactTopLevels(*tv) : 0u));
        }
        // Storage inside a const C/C++ header global (the object, an element or a field): shallow const.
        if (llvm::isa<llvm::GlobalVariable>(address) || llvm::isa<llvm::GEPOperator>(address))
            if (auto* global = llvm::dyn_cast<llvm::GlobalVariable>(llvm::getUnderlyingObject(address));
                IsConstHeaderGlobal(global) && !IsCxxMutableFieldAccess(const_cast<llvm::Value*>(address)))
            {
                name = global->getName().str();
                std::string slotName;
                const auto* tv = DeclaredConstSlot(address, slotName);
                return static_cast<uint8_t>(1u | (tv != nullptr ? ExactTopLevels(*tv) : 0u));
            }
        if (llvm::isa<llvm::AllocaInst>(address) || llvm::isa<llvm::GlobalVariable>(address)
            || llvm::isa<llvm::GEPOperator>(address))
        {
            std::string slotName;
            if (const auto* tv = DeclaredConstSlot(address, slotName))
            {
                name = slotName;
                return DeclaredTopLevels(*tv);
            }
            // An unnamed spill slot (a parenthesized pointer expression): what was stored in it.
            if (auto* spill = llvm::dyn_cast<llvm::AllocaInst>(address))
            {
                const llvm::StoreInst* only = nullptr;
                for (const llvm::User* user : spill->users())
                    if (auto* store = llvm::dyn_cast<llvm::StoreInst>(user);
                        store != nullptr && store->getPointerOperand() == spill)
                    {
                        if (only != nullptr) return 0;
                        only = store;
                    }
                if (only == nullptr || !only->getValueOperand()->getType()->isPointerTy()) return 0;
                return static_cast<uint8_t>(StoredConstLevels(only->getValueOperand(), name, depth + 1) << 1);
            }
            if (!llvm::isa<llvm::GEPOperator>(address)) return 0;
        }
        return ValueConstLevels(address, name, depth + 1);
}

/*
 * Const levels behind pointer value `pointer` (bit 0 = `*pointer`, bit 1 = `**pointer`), from the
 * expression that produced it: an explicit cast or member-access receiver stamp, the slot it was
 * loaded from, a call's return type, pointer arithmetic over a pointer, or any arm of a join.
 */
uint8_t LLVMBackend::ValueConstLevels(const llvm::Value* pointer, std::string& name, int depth) const
{
        if (pointer == nullptr || depth > 8) return 0;
        // A string literal is a `(const char)*` (R11): its characters are read-only.
        if (IsStringLiteralGlobal(pointer))
        {
            name = "string literal";
            return 0xFF;
        }
        if (auto* inst = llvm::dyn_cast<llvm::Instruction>(pointer))
        {
            if (const llvm::MDNode* md = inst->getMetadata("cflat.const.cast"))
            {
                uint8_t levels = 0;
                if (md->getNumOperands() == 2)
                {
                    if (auto* text = llvm::dyn_cast<llvm::MDString>(md->getOperand(0)))
                        name = text->getString().str();
                    if (auto* c = llvm::mdconst::dyn_extract<llvm::ConstantInt>(md->getOperand(1)))
                        levels = static_cast<uint8_t>(c->getZExtValue());
                }
                return levels;
            }
            if (inst->getMetadata("cflat.pointee.mutable") != nullptr) return 0;
            if (const llvm::MDNode* md = inst->getMetadata("cflat.pointee.const"))
            {
                if (md->getNumOperands() == 1)
                    if (auto* text = llvm::dyn_cast<llvm::MDString>(md->getOperand(0)))
                        name = text->getString().str();
                return 0xFF;
            }
        }
        if (auto* load = llvm::dyn_cast<llvm::LoadInst>(pointer))
            return LevelsBelow(StoredConstLevels(load->getPointerOperand(), name, depth + 1));
        if (auto* call = llvm::dyn_cast<llvm::CallInst>(pointer))
        {
            const auto* symbol = FindSymbolForFunction(call->getCalledFunction());
            if (symbol == nullptr) return 0;
            const uint8_t levels = DeclaredPointeeConstLevels(symbol->ReturnType);
            if (levels == 0) return 0;
            name = call->getCalledFunction()->getName().str();
            if (std::string spelled = SpellFunctionSymbol(*this, name); !spelled.empty()) name = spelled;
            // A C++ function's IR name is mangled (Itanium "_Z", Microsoft "?"): name it by its
            // CFlat table key.
            if (const llvm::StringRef irName = call->getCalledFunction()->getName();
                irName.starts_with("_Z") || irName.starts_with("?"))
                for (const auto& [key, overloads] : functionTable)
                    for (const auto& candidate : overloads)
                        if (&candidate == symbol) name = key + "()";
            return levels;
        }
        // A join is const where either arm is (C++: the composite pointer type).
        if (auto* phi = llvm::dyn_cast<llvm::PHINode>(pointer))
        {
            uint8_t levels = 0;
            for (const llvm::Value* arm : phi->incoming_values())
            {
                std::string armName;
                const uint8_t armLevels = StoredConstLevels(arm, armName, depth + 1);
                if (armLevels != 0 && levels == 0) name = armName;
                levels |= armLevels;
            }
            return levels;
        }
        if (auto* select = llvm::dyn_cast<llvm::SelectInst>(pointer))
        {
            std::string trueName, falseName;
            const uint8_t t = StoredConstLevels(select->getTrueValue(), trueName, depth + 1);
            const uint8_t f = StoredConstLevels(select->getFalseValue(), falseName, depth + 1);
            name = t != 0 ? trueName : falseName;
            return static_cast<uint8_t>(t | f);
        }
        // Pointer arithmetic and element addressing over a pointer keep its qualification.
        if (auto* gep = llvm::dyn_cast<llvm::GEPOperator>(pointer))
        {
            for (auto it = llvm::gep_type_begin(gep), end = llvm::gep_type_end(gep); it != end; ++it)
                if (it.getStructTypeOrNull() != nullptr) return 0;
            return StoredConstLevels(gep->getPointerOperand(), name, depth + 1);
        }
        return 0;
}

bool LLVMBackend::PointerValuePointeeConst(const llvm::Value* pointer, std::string& name, int depth) const
{
        return (ValueConstLevels(pointer, name, depth) & 1u) != 0;
}

/*
 * C++ [conv.qual] over pointee levels 0..depth-1 (top-relative): a level const in the source
 * must be const in the target, and a target that adds const below the first level needs every
 * level above it const too (`int** -> (const int)**` is refused, `-> (const int*)*` is not).
 */
bool LLVMBackend::QualificationDiscardsConst(uint8_t source, uint8_t target, int depth)
{
        for (int j = 0; j < depth; ++j)
        {
            const bool s = ((source >> j) & 1u) != 0;
            const bool t = ((target >> j) & 1u) != 0;
            if (s && !t) return true;
            if (t && !s)
                for (int k = 0; k < j; ++k)
                    if (((target >> k) & 1u) == 0) return true;
        }
        return false;
}

/*
 * Whether the implicit conversion of pointer value `value` (expression type `source`) to the
 * declared pointer type `target` discards const (#5-#7). Fills both spellings for the message;
 * a value's own top-level const is not part of it. C++ const is shallow (CxxShallow causes).
 */
bool LLVMBackend::ConversionDiscardsConst(const llvm::Value* value, const TypeAndValue& source,
    const TypeAndValue& target, std::string& sourceSpelled, std::string& targetSpelled,
    bool exactTargetLevels) const
{
        if (value == nullptr || !value->getType()->isPointerTy()
            || llvm::isa<llvm::ConstantPointerNull>(value))
            return false;
        // A view passes its element pointer; `alias T*` binds a copy of the pointer value.
        if (!(target.Pointer || target.IsArrayView) || target.IsFunctionPointer || target.IsInterface
            || (target.IsAlias && !target.Pointer) || target.IsRvalueRef || target.IsCxxRefToPointer
            || target.CxxConstRef()
            || (IsCxxRecord(source.TypeName) && !source.Pointer && source.ConstArraySize == 0))
            return false;
        const int depth = target.IsArrayView ? (target.ElemPointer ? 2 : 1) : target.ValuePointerDepth();
        if (depth <= 0) return false;
        // A literal (or a join of literals) into a pointer to another element type is a type
        // mismatch, reported as one; `u8"x"` is a `c8` literal.
        if (LiteralMismatchesPointee(value, source, target, depth)) return false;
        std::string name;
        const uint8_t from = StoredConstLevels(value, name);
        uint8_t to = 0;
        if (exactTargetLevels)
        {
            // Exact top-relative pointee levels: bit j = ConstMask bit (depth - 1 - j), no fill.
            for (int j = 0; j < depth; ++j)
                if (((target.ConstMask >> (depth - 1 - j)) & 1u) != 0) to |= static_cast<uint8_t>(1u << j);
        }
        else
            to = DeclaredPointeeConstLevels(target);
        // A C++ `const char*` alias (`using Name = const char*`) keeps its pointee const.
        if (target.PointeeConstBit() && depth == 1) to |= 1u;
        if (!QualificationDiscardsConst(from, to, depth)) return false;
        TypeAndValue shown = source.TypeName.empty() || target.IsArrayView ? target : source;
        shown.DiagnosticTypeName.clear();
        if (target.IsArrayView)
        {
            // Shown as a view of the source's element qualification.
            shown.ConstMask = 0;
            for (int j = 0; j < depth; ++j)
                if (((from >> j) & 1u) != 0) shown.ConstMask |= static_cast<uint8_t>(1u << (depth - 1 - j));
        }
        else if (!shown.Pointer || shown.ValuePointerDepth() != depth)
        {
            // An array decays, `&x` and the like: show the source at the target's depth.
            shown.Pointer = true;
            shown.ElemPointer = depth >= 2;
            shown.PointerDepth = depth;
            shown.ConstArraySize = 0;
            shown.ConstInnerDimensions.clear();
            shown.IsArrayView = false;
        }
        if (!target.IsArrayView)
        {
            shown.ConstMask = 0;
            for (int j = 0; j < depth; ++j)
                if (((from >> j) & 1u) != 0) shown.ConstMask |= static_cast<uint8_t>(1u << (depth - 1 - j));
        }
        sourceSpelled = SpellTypeForSymbolDump(*this, shown);
        TypeAndValue declared = target;
        declared.DiagnosticTypeName.clear();
        targetSpelled = SpellTypeForSymbolDump(*this, declared);
        // A C level set no CFlat spelling covers (`int *const *`): spell it the C way.
        const uint8_t pointeeMask = static_cast<uint8_t>(target.ConstMask & ((1u << depth) - 1u));
        if (exactTargetLevels && (pointeeMask & (pointeeMask + 1u)) != 0)
        {
            TypeAndValue base = target;
            base.DiagnosticTypeName.clear();
            base.Pointer = false;
            base.ElemPointer = false;
            base.PointerDepth = 0;
            base.ConstMask = 0;
            targetSpelled = ((pointeeMask & 1u) != 0 ? "const " : "") + SpellTypeForSymbolDump(*this, base);
            for (int k = 1; k <= depth; ++k)
                targetSpelled += k < depth && ((pointeeMask >> k) & 1u) != 0 ? "* const" : "*";
        }
        return true;
}

void LLVMBackend::MarkPointeeQualification(llvm::Value* pointer, const TypeAndValue& tv,
                                           const std::string& name)
{
        auto* inst = llvm::dyn_cast_or_null<llvm::Instruction>(pointer);
        if (inst == nullptr || !inst->getType()->isPointerTy() || tv.TypeName.empty()) return;
        // Casts reuse the operand's instruction, so this access's own (outermost) type decides:
        // a const pointee re-qualifies even after an inner cast dropped it.
        if (ConstSlotPointeeIsConst(tv))
        {
            inst->setMetadata("cflat.const.cast", nullptr);
            inst->setMetadata("cflat.pointee.mutable", nullptr);
            inst->setMetadata("cflat.pointee.const",
                llvm::MDNode::get(*context, { llvm::MDString::get(*context, name) }));
        }
        else if (inst->getMetadata("cflat.const.cast") == nullptr)
        {
            inst->setMetadata("cflat.pointee.const", nullptr);
            inst->setMetadata("cflat.pointee.mutable", llvm::MDNode::get(*context, {}));
        }
}

llvm::Value* LLVMBackend::MarkCastQualification(llvm::Value* pointer, const TypeAndValue& tv,
                                                const std::string& text)
{
        if (pointer == nullptr || !pointer->getType()->isPointerTy()) return pointer;
        auto* inst = llvm::dyn_cast<llvm::Instruction>(pointer);
        // Storage itself (a slot, a global, a parameter) is shared by every use: stamp a copy.
        const uint8_t levels = DeclaredPointeeConstLevels(tv);
        if (inst == nullptr || llvm::isa<llvm::AllocaInst>(inst))
        {
            std::string current;
            const int depth = tv.IsArrayView ? (tv.ElemPointer ? 2 : 1) : tv.ValuePointerDepth();
            const uint8_t depthMask = static_cast<uint8_t>((1u << depth) - 1u);
            if (builder->GetInsertBlock() == nullptr
                || ((StoredConstLevels(pointer, current) ^ levels) & depthMask) == 0)
                return pointer;
            // `(Point*)"nn"` stays the literal itself: the reinterpret is refused where it lands.
            if (LiteralMismatchesPointee(pointer, TypeAndValue{}, tv, depth)) return pointer;
            llvm::Value* zero = builder->getInt64(0);
            inst = llvm::GetElementPtrInst::Create(llvm::Type::getInt8Ty(*context), pointer,
                llvm::ArrayRef<llvm::Value*>(zero), llvm::Twine("cast.qual"));
            builder->Insert(inst);
            pointer = inst;
        }
        inst->setMetadata("cflat.pointee.const", nullptr);
        inst->setMetadata("cflat.pointee.mutable", nullptr);
        inst->setMetadata("cflat.const.cast", llvm::MDNode::get(*context,
            { llvm::MDString::get(*context, text),
              llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), levels)) }));
        return pointer;
}

bool LLVMBackend::FindNativeConstField(const std::string& typeName, std::string& field) const
{
        if (IsCxxRecord(typeName)) return false;
        auto it = dataStructures.find(typeName);
        if (it == dataStructures.end()) return false;
        for (const auto& f : it->second.StructFields)
        {
            // A const C++ class member counts too (clang deletes the copy assignment).
            if (f.IsConstObject())
            {
                field = f.VariableName;
                return true;
            }
            // By-value members only: a pointer or view refers to someone else's object.
            if (!f.Pointer && !f.IsArrayView && f.TypeName != typeName
                && FindNativeConstField(f.TypeName, field))
                return true;
        }
        return false;
}

bool LLVMBackend::BorrowProofRetiredByRebind(const NamedVariable& nv) const
{
        if (!nv.ReboundToOwnedValue || nv.ReboundBlock == nullptr) return false;
        auto* here = builder->GetInsertBlock();
        // Parent pairing so a recycled block address cannot match across functions.
        if (here == nullptr || nv.ReboundFunction != here->getParent()) return false;
        return nv.ReboundBlock == here;
    }

bool LLVMBackend::OwningLocalCopyStillAliases(const NamedVariable& nv) const
{
        if (!nv.BorrowsOwningLocal || nv.OwningLocalOrigin.empty()) return false;
        if (nv.IsOwning || (nv.PointerRebound && nv.AssignBorrowBlock == nullptr)) return false;
        const NamedVariable* src = FindVariableByStorage(nv.OwningLocalStorage);
        return src != nullptr && src->IsOwning && !src->PointerRebound;
    }

void LLVMBackend::RetargetOwningLocalCopies(const llvm::Value* fromSlot,
        const std::string& newOwner, llvm::Value* newSlot)
{
        if (fromSlot == nullptr || newSlot == nullptr) return;
        for (auto& frame : stackNamedVariable)
            for (auto& [varName, nv] : frame.namedVariable)
                if (nv.BorrowsOwningLocal && nv.OwningLocalStorage == fromSlot)
                {
                    nv.OwningLocalStorage = newSlot;
                    nv.OwningLocalOrigin = newOwner;
                }
    }

std::vector<llvm::Value*> LLVMBackend::JoinArmValues(const llvm::Value* joined) const
{
        // Leaf arms of a join, flattened through nested ?: / ?? / phi joins (visited guards phi cycles).
        std::vector<llvm::Value*> arms;
        std::unordered_set<const llvm::Value*> visited;
        std::function<void(const llvm::Value*, bool)> collect = [&](const llvm::Value* value, bool top) {
            if (!visited.insert(value).second) return;
            std::vector<llvm::Value*> direct;
            if (const auto* select = llvm::dyn_cast_or_null<llvm::SelectInst>(value))
            {
                direct.push_back(const_cast<llvm::Value*>(select->getTrueValue()));
                direct.push_back(const_cast<llvm::Value*>(select->getFalseValue()));
            }
            else if (const auto* phi = llvm::dyn_cast_or_null<llvm::PHINode>(value))
            {
                for (unsigned i = 0; i < phi->getNumIncomingValues(); i++)
                    direct.push_back(phi->getIncomingValue(i));
            }
            else if (const auto* join = FindNullCoalesceJoin(value))
            {
                for (const auto& arm : join->Arms) direct.push_back(arm.Value);
            }
            else
            {
                if (!top) arms.push_back(const_cast<llvm::Value*>(value));
                return;
            }
            for (const llvm::Value* arm : direct) collect(arm, false);
        };
        collect(joined, true);
        return arms;
    }

const LLVMBackend::NamedVariable* LLVMBackend::FindOwningLocalCopyJoinArm(const llvm::Value* joined) const
{
        for (const llvm::Value* arm : JoinArmValues(joined))
        {
            const auto* load = llvm::dyn_cast_or_null<llvm::LoadInst>(arm);
            if (load == nullptr) continue;
            const NamedVariable* copy = FindVariableByStorage(load->getPointerOperand());
            if (copy != nullptr && copy->BorrowsOwningLocal && OwningLocalCopyStillAliases(*copy))
                return copy;
        }
        return nullptr;
    }

void LLVMBackend::ConsumeOwningLocalJoinArms(llvm::Value* joined)
{
        if (joined == nullptr || !joined->getType()->isPointerTy()) return;
        const NullCoalesceJoin* coalesce = FindNullCoalesceJoin(joined);
        // Only a direct (unnested) '??' LHS load is taken whenever non-null; see MarkVariableMoved below.
        const llvm::Value* coalesceLhs = coalesce != nullptr && !coalesce->Arms.empty()
            ? coalesce->Arms.front().Value : nullptr;
        const auto arms = JoinArmValues(joined);
        for (size_t i = 0; i < arms.size(); i++)
        {
            auto* load = llvm::dyn_cast_or_null<llvm::LoadInst>(arms[i]);
            if (load == nullptr || load->getType() != joined->getType() || IsMovedOutPtrValue(arms[i]))
                continue;
            llvm::Value* slot = load->getPointerOperand();
            const NamedVariable* owner = FindVariableByStorage(slot);
            if (owner == nullptr || !owner->IsOwning || owner->IsBorrowed || owner->IsAliasBorrow
                || owner->TypeAndValue.IsAlias || owner->BorrowsOwningLocal
                || !owner->TypeAndValue.Pointer)
                continue;
            const std::string name = FindVariableNameByStorage(slot);
            if (name.empty()) continue;
            // Null the slot only when the join yielded this arm; the other arm keeps its owner.
            llvm::Value* current = builder->CreateLoad(joined->getType(), slot);
            llvm::Value* taken = builder->CreateICmpEQ(joined, current);
            builder->CreateStore(builder->CreateSelect(taken,
                llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(joined->getType())),
                current), slot);
            // The '??' LHS is taken whenever it is non-null, so its compile-time move is exact; a
            // ternary or RHS arm may stay live, so only its runtime slot is nulled.
            if (coalesceLhs != nullptr && arms[i] == coalesceLhs) MarkVariableMoved(name);
        }
    }

std::string LLVMBackend::FindVariableNameByStorage(const llvm::Value* slot) const
{
        if (slot == nullptr) return {};
        for (const auto& frame : stackNamedVariable)
        {
            for (const auto& [varName, nv] : frame.namedVariable)
                if (nv.Storage == slot) return varName;
            for (const auto& [varName, nv] : frame.functionArgument)
                if (nv.Storage == slot) return varName;
        }
        return {};
    }

bool LLVMBackend::IsProvablyNonOwningPointerLoad(llvm::Value* value) const
{
        auto* load = llvm::dyn_cast_or_null<llvm::LoadInst>(value);
        if (load == nullptr || !load->getType()->isPointerTy()) return false;
        auto* slot = load->getPointerOperand();
        for (const auto& frame : stackNamedVariable)
        {
            for (const auto& [varName, nv] : frame.namedVariable)
                if (nv.Storage == slot) return !nv.IsOwning;
            for (const auto& [varName, nv] : frame.functionArgument)
                if (nv.Storage == slot) return !nv.IsOwning;
        }
        return false;
    }

bool LLVMBackend::IsOwningFieldAddress(const llvm::GetElementPtrInst* fieldAddr) const
{
        auto* st = llvm::dyn_cast<llvm::StructType>(fieldAddr->getSourceElementType());
        if (st == nullptr || !st->hasName() || fieldAddr->getNumIndices() == 0) return true;
        // The core `unique<T>` wrapper owns the pointer it holds: a load through it is owning.
        const std::string name = st->getStructName().str();
        if (name.rfind("unique", 0) == 0 || IsCoreUniqueType(name)) return true;
        auto* idx = llvm::dyn_cast<llvm::ConstantInt>(fieldAddr->getOperand(fieldAddr->getNumOperands() - 1));
        if (idx == nullptr) return true;
        return IsOwningStructField(name, static_cast<unsigned>(idx->getZExtValue()));
    }

bool LLVMBackend::IsOwningStructField(const std::string& structName, unsigned index) const
{
        auto it = dataStructures.find(structName);
        if (it == dataStructures.end() || index >= it->second.StructFields.size()) return true;
        const auto& field = it->second.StructFields[index];
        if (field.IsAlias) return false;  // a borrowed reference to storage owned elsewhere
        // `unique T*` owns its pointee (a core `unique<T>` field too); a plain T* is a borrow.
        return field.IsUnique || IsCoreUniqueType(field.TypeName);
    }

bool LLVMBackend::IsFunctionArgumentStorage(llvm::Value* storage) const
{
        if (storage == nullptr) return false;
        for (const auto& frame : stackNamedVariable)
            for (const auto& [varName, nv] : frame.functionArgument)
                if (nv.Storage == storage) return true;
        return false;
    }

bool LLVMBackend::IsBorrowStringParamStorage(llvm::Value* storage)
{
        if (storage == nullptr) return false;
        for (auto& frame : stackNamedVariable)
        {
            for (auto& [varName, nv] : frame.functionArgument)
                if (nv.Storage == storage)
                    return nv.TypeAndValue.TypeName == "string"
                        && !nv.TypeAndValue.IsMove
                        && !nv.IsOwningString && !nv.IsOwning;
        }
        return false;
    }

void LLVMBackend::CreateReturnCall(llvm::Value* value, llvm::Value* returnedLocalStorage,
                                   const std::string& interfaceReturnStructName,
                                   bool srcIsUnsigned,
                                   const std::string& sourceTypeName)
{
        if (!IsInsertBlockLive())
            return;

        // If returning an owned variable loaded from a local alloca, suppress its cleanup
        // so we don't free the value before the caller receives it.
        // The loaded snapshot in `value` already captures the pointer; the caller takes ownership.
        NamedVariable* ownedStringReturnVar = nullptr;
        NamedVariable* ownedPtrReturnVar    = nullptr;
        // The returned owner was a borrow-suppressed one (only its runtime flag is cleared on return).
        bool ownedPtrReturnSuppressed = false;
        if (value)
        {
            if (auto* loadInst = llvm::dyn_cast<llvm::LoadInst>(value))
            {
                auto* srcAlloca = loadInst->getPointerOperand();
                [&]() {
                    for (auto& frame : stackNamedVariable)
                    {
                        for (auto& [varName, nv] : frame.namedVariable)
                        {
                            // Returning a whole string LOCAL is a MOVE to the caller: zero the
                            // source so its always-run scope-exit destructor is a no-op on the
                            // moved-out value (the runtime owned bit rides along in `value`, so
                            // the caller frees iff the local owned its buffer) - the same
                            // zero-the-source discipline every other value type uses. A local
                            // that merely BORROWS an owned field (BorrowsOwnedString) is left
                            // alone: the owning struct frees that buffer, and the borrow's bit
                            // was already cleared at the return site.
                            if (nv.Storage == srcAlloca && nv.TypeAndValue.TypeName == "string"
                                && !nv.BorrowsOwnedString)
                            {
                                ownedStringReturnVar = &nv;
                                builder->CreateStore(
                                    llvm::Constant::getNullValue(loadInst->getType()), srcAlloca);
                                return;
                            }
                            if (nv.Storage == srcAlloca && IsOwnedAtScopeExit(nv))
                            {
                                ownedPtrReturnVar = &nv;
                                ownedPtrReturnSuppressed = !nv.IsOwning;
                                if (nv.IsOwning) nv.IsOwning = false;  // suppress cleanup on return path
                                else nv.BorrowSuppressedOwner = false;
                                return;
                            }
                        }
                        for (auto& [varName, nv] : frame.functionArgument)
                        {
                            if (nv.Storage == srcAlloca && nv.IsOwningString
                                && nv.TypeAndValue.IsConsumeInferredSink
                                && ResolveTypeAlias(nv.TypeAndValue.TypeName) == "string")
                            {
                                ownedStringReturnVar = &nv;
                                builder->CreateStore(
                                    llvm::Constant::getNullValue(loadInst->getType()), srcAlloca);
                                return;
                            }
                            if (nv.Storage == srcAlloca && IsOwnedAtScopeExit(nv))
                            {
                                ownedPtrReturnVar = &nv;
                                ownedPtrReturnSuppressed = !nv.IsOwning;
                                if (nv.IsOwning) nv.IsOwning = false;  // suppress cleanup on return path
                                else nv.BorrowSuppressedOwner = false;
                                return;
                            }
                        }
                    }
                }();
            }

            // Interface locals are represented as SSA fat values rather than load-backed
            // slots. A returned local still transfers its box to the caller, so suppress
            // the scope cleanup using the storage identity threaded by the return site.
            if (returnedLocalStorage != nullptr
                && value->getType() == GetFatPtrType())
            {
                for (auto& frame : stackNamedVariable)
                {
                    for (auto& [varName, nv] : frame.namedVariable)
                    {
                        if (nv.Storage == returnedLocalStorage
                            && nv.TypeAndValue.IsFatInterfaceValue() && nv.IsOwning)
                        {
                            ownedPtrReturnVar = &nv;
                            nv.IsOwning = false;
                            break;
                        }
                    }
                    if (ownedPtrReturnVar != nullptr) break;
                }
            }
        }

        // Returning a struct VALUE local whose full-destructor frees members (owned string
        // fields, value containers, ...): the by-value snapshot in `value` carries those member
        // pointers to the caller, so the local's destructor must be skipped here or the returned
        // value dangles (and double-frees when the caller destroys it). Suppress only for the
        // return-path destructor walk. Mirrors the owned-string / owned-pointer cases above.
        llvm::Value* prevStructSkip = returnedStructDtorSkipAlloca;
        if (value && !ownedStringReturnVar && !ownedPtrReturnVar)
        {
            llvm::Value* srcAlloca = returnedLocalStorage;
            if (srcAlloca == nullptr)
                if (auto* loadInst = llvm::dyn_cast<llvm::LoadInst>(value))
                    srcAlloca = loadInst->getPointerOperand();
            if (srcAlloca != nullptr)
            {
                bool found = false;
                for (auto& frame : stackNamedVariable)
                {
                    for (auto& [varName, nv] : frame.namedVariable)
                    {
                        if (nv.Storage == srcAlloca && !nv.TypeAndValue.Pointer
                            && nv.TypeAndValue.TypeName != "string"
                            && GetOrCreateFullDestructor(nv.TypeAndValue.TypeName) != nullptr)
                        {
                            returnedStructDtorSkipAlloca = srcAlloca;
                            found = true;
                            break;
                        }
                    }
                    if (found) break;
                }
            }
        }

        // Emit destructors for all scopes from innermost out to the function boundary, through
        // the shared cleanup chain when the frame is large enough to pay for it.
        if (!ChainReturnCleanups(value))
            for (auto it = stackNamedVariable.rbegin(); it != stackNamedVariable.rend(); ++it)
            {
                EmitDestructorsForScope(*it);
                if (it->isFunction) break;
            }

        // Restore flags (clean state, though the scope is about to be popped anyway).
        // The returned string local was moved out by zeroing its storage (not by toggling
        // IsOwningString), so nothing to restore for it - its always-run dtor now no-ops on
        // the zeroed value.
        if (ownedPtrReturnVar && ownedPtrReturnSuppressed)
            ownedPtrReturnVar->BorrowSuppressedOwner = true;
        else if (ownedPtrReturnVar)
            ownedPtrReturnVar->IsOwning = true;
        returnedStructDtorSkipAlloca = prevStructSkip;

        // Auto return-type inference: record the (BB, value) pair so the caller can
        // unify types after body emission. Terminate the BB with a placeholder
        // 'unreachable' that will be replaced with a real 'ret' once the inferred
        // signature is known and BBs are spliced onto the new function.
        if (autoReturnCapture)
        {
            auto* placeholder = builder->CreateUnreachable();
            autoReturnCapture->push_back(
                { builder->GetInsertBlock(), value, placeholder, sourceTypeName });
            return;
        }

        if (autoVaListAlloca)
            CreateVaEnd(autoVaListAlloca);

        // Interface return: a returned concrete-implementer POINTER reaches here as a bare
        // pointer (the loaded owning local). Its scope-exit free was already suppressed by the
        // owned-pointer block above (the load made the variable look returned), so ownership
        // moves to the caller exactly as a plain `return ptr;` would. Box it into the interface
        // fat pointer { vtable, data } now, after the destructor walk. The value-operand and
        // already-boxed-interface cases are handled at the return site (MainListener).
        if (!interfaceReturnStructName.empty() && value)
        {
            auto* fatTy = GetFatPtrType();
            if (currentFunction->getReturnType() == fatTy && value->getType() != fatTy)
            {
                auto* vtable = GetOrCreateVTable(interfaceReturnStructName, currentFunctionReturnTypeName);
                value = BuildInterfaceFatValue(vtable, value);
            }
        }

        if (value == nullptr)
            builder->CreateRetVoid();
        else
        {
            auto* retTy = currentFunction->getReturnType();
            // Wrap raw i8* string literals into string struct when returning string
            auto* strTy = llvm::StructType::getTypeByName(*context, "string");
            llvm::Type* abiRetStructTy = currentFunctionAbiRecipe.hasLowering
                ? currentFunctionAbiRecipe.retSlot.structTy : nullptr;
            if (strTy && (retTy == strTy || abiRetStructTy == strTy) && value->getType() != strTy)
            {
                auto* ptrTy = cflat_llvm::PointerTo(builder->getInt8Ty());
                if (value->getType() == ptrTy)
                {
                    if (auto* c = llvm::dyn_cast<llvm::Constant>(value); c && IsStringLiteralConstant(c))
                        value = WrapStringLiteralAsString(value);
                    else if (GetFunction("operator string"))
                    {
                        NamedVariable argNV;
                        argNV.Primary = value;
                        argNV.BaseType = ptrTy;
                        argNV.TypeAndValue = { "char", "", true, false };
                        value = CreateOverloadedFunctionCall("operator string", { argNV });
                    }
                }
            }
            if (currentFunctionAbiRecipe.hasLowering
                && currentFunctionAbiRecipe.retSlot.kind != AbiSlot::Direct)
            {
                const AbiSlot& s = currentFunctionAbiRecipe.retSlot;
                if (s.structTy == nullptr)
                    LogErrorMessage("cannot lower extern return value for the active C ABI recipe");
                value = Upconvert(value, s.structTy, srcIsUnsigned);
                if (value == nullptr || value->getType() != s.structTy)
                    LogErrorMessage("cannot return this value from extern function: it does not match the C ABI return aggregate");
                if (s.kind == AbiSlot::SRetReturn)
                {
                    const unsigned sretIdx = SRetArgIndex(currentFunctionAbiRecipe);
                    if (currentFunction->arg_size() <= sretIdx)
                        LogErrorMessage("cannot lower extern sret return: missing hidden return slot");
                    builder->CreateStore(value, currentFunction->getArg(sretIdx));
                    builder->CreateRetVoid();
                    return;
                }
                auto* slot = AllocaAtEntry(s.structTy, nullptr, "abi.ret", s.align);
                builder->CreateStore(value, slot);
                if (s.kind == AbiSlot::CoerceToInt)
                {
                    builder->CreateRet(LoadCoerceAt(slot, s.coerceTy, 0));
                    return;
                }
                if (s.kind == AbiSlot::CoercePair)
                {
                    llvm::Value* aggregate = llvm::UndefValue::get(retTy);
                    aggregate = builder->CreateInsertValue(aggregate, LoadCoerceAt(slot, s.coerceTy, 0), { 0u });
                    aggregate = builder->CreateInsertValue(aggregate, LoadCoerceAt(slot, s.coerceTy2, 8), { 1u });
                    builder->CreateRet(aggregate);
                    return;
                }
                LogErrorMessage("cannot lower extern return value for the active C ABI recipe");
            }
            // `return x` initializes the return object under the ASSIGNMENT rules, so a scalar
            // returned from a simd<T,N> function splats across the lanes just like a store does.
            if (auto* rvt = llvm::dyn_cast<llvm::FixedVectorType>(retTy);
                rvt != nullptr && !value->getType()->isVectorTy()
                && (value->getType()->isIntegerTy() || value->getType()->isFloatingPointTy()))
            {
                value = SplatScalarToVectorType(value, rvt, srcIsUnsigned,
                    currentFunctionReturnTV.IsUnsignedInteger() != -1);
            }
            value = Upconvert(value, retTy, srcIsUnsigned);
            // Upconvert only widens; handle narrowing int -> bool explicitly (same as CreateAssignment).
            // Warn: CFlat requires explicit narrowing - write "return expr != 0;" instead.
            if (retTy == builder->getInt1Ty() && value->getType()->isIntegerTy() && value->getType() != retTy)
            {
                LogErrorMessage("implicit int-to-bool conversion on return - use '!= 0' to make narrowing explicit");
                value = builder->CreateICmpNE(value, llvm::ConstantInt::get(value->getType(), 0), "tobool");
            }
            // Narrowing integer return (`u8 f(u8 x) { return -x; }`, whose operand C-promotes to
            // i32): truncate into the declared type exactly as CreateAssignment does for a store.
            else if (retTy->isIntegerTy() && value->getType()->isIntegerTy()
                     && value->getType() != retTy)
            {
                value = CreateCast(value, retTy, !srcIsUnsigned);
            }
            // Backstop: anything still mismatched would be an unreadable module-verifier dump.
            if (value->getType() != retTy)
            {
                LogErrorMessage("cannot return this value: its type does not match the declared "
                                "return type of function '{}'", { SpellType(*this,
                                    TypeAndValue{ .TypeName = FindFunctionSourceName(currentFunction) }) });
            }
            builder->CreateRet(value);
        }
    }

void LLVMBackend::BeginAutoReturnCapture()
{ autoReturnCapture.emplace(); }

std::vector<LLVMBackend::AutoReturnSite> LLVMBackend::EndAutoReturnCapture()
{
        std::vector<AutoReturnSite> sites;
        if (autoReturnCapture) sites = std::move(*autoReturnCapture);
        autoReturnCapture.reset();
        return sites;
    }

bool LLVMBackend::IsAutoReturnCaptureActive() const
{ return autoReturnCapture.has_value(); }

namespace
{
    using ReceiverPath = LLVMBackend::ReceiverPath;

    // Where a receiver path lands: its declared type when the walk could follow it, and how many
    // pointers it went through (0 = the receiver's own storage, 1 = what its pointers reach).
    struct ReceiverWalk
    {
        bool Known = false;
        LLVMBackend::TypeAndValue Type;
        int Derefs = 0;
        // A call result codegen has not decided yet that may be writable: any use is a write.
        bool Undecided = false;
    };

    bool ReceiverPointerLike(const LLVMBackend::TypeAndValue& tv)
    {
        if (tv.IsInterface || tv.IsFunctionPointer) return false;
        return tv.Pointer || tv.PointerDepth > 0 || tv.IsArrayView || tv.ConstArraySize > 0;
    }

    LLVMBackend::TypeAndValue ReceiverPointee(const LLVMBackend::TypeAndValue& tv)
    {
        LLVMBackend::TypeAndValue e = tv;
        e.ConstArraySize = 0;
        e.ConstMask = 0;
        if (tv.IsArrayView)
        {
            e.IsArrayView = false;
            e.Pointer = tv.ElemPointer;
            e.PointerDepth = tv.ElemPointer ? 1 : 0;
        }
        else if (tv.ElemPointer || tv.PointerDepth >= 2)
        {
            e.Pointer = true;
            e.PointerDepth = tv.PointerDepth >= 2 ? tv.PointerDepth - 1 : 1;
        }
        else
        {
            e.Pointer = false;
            e.PointerDepth = 0;
        }
        e.ElemPointer = false;
        return e;
    }

    // The level a pointer parameter reaches first is const (C spelled levels included).
    bool ReceiverParamPointeeConst(const LLVMBackend::TypeAndValue& p)
    {
        if (p.PointeeConstBit()) return true;
        const int depth = p.ValuePointerDepth();
        return depth >= 1 && ((p.ConstMask >> (depth - 1)) & 1u) != 0;
    }
}

void LLVMBackend::RecordReceiverScanFacts(const std::string& uniqueName, ReceiverScanFacts facts)
{
    if (uniqueName.empty()) return;
    readOnlyMethodMemo_.clear();
    if (facts.File.empty()) facts.File = sourceFileName;
    receiverScanFacts_[uniqueName] = std::move(facts);
}

bool LLVMBackend::IsNativeReceiverMethod(const FunctionSymbol& method) const
{
    if (method.IsCxx || method.Parameters.empty()) return false;
    if (!method.IsMethod && receiverScanFacts_.count(method.UniqueName) == 0
        && method.ConstMethodState == 0)
        return false;
    const TypeAndValue& self = method.Parameters.front();
    return self.Pointer && self.VariableName == self.TypeName + "__"
        && dataStructures.count(self.TypeName) != 0 && !IsCxxRecord(self.TypeName);
}

bool LLVMBackend::IsReadOnlyMethod(const FunctionSymbol& method)
{
    if (method.IsCxx) return method.CxxConst;
    // Explicit `const`: its body was checked against a `(const S)*` this (#9), so it is read-only.
    if (method.IsExplicitConstMethod()) return true;
    if (auto memo = readOnlyMethodMemo_.find(method.UniqueName); memo != readOnlyMethodMemo_.end())
        return memo->second;
    // No body facts (a cached core method): the cache's answer. A native method nothing ever
    // scanned is not presumed read-only.
    if (receiverScanFacts_.count(method.UniqueName) == 0) return method.ConstMethodState == 1;

    auto fieldOf = [&](const std::string& structName, const std::string& name,
                       TypeAndValue& out) -> bool {
        auto sd = dataStructures.find(structName);
        if (sd == dataStructures.end()) return false;
        for (const auto& field : sd->second.StructFields)
            if (field.VariableName == name) { out = field; return true; }
        for (const auto& bit : sd->second.Bitfields)
            if (bit.Name == name) { out = TypeAndValue{}; out.TypeName = bit.TypeName; return true; }
        return false;
    };
    auto nativeStruct = [&](const std::string& name) {
        return dataStructures.count(name) != 0 && !IsCxxRecord(name) && !IsInterfaceType(name);
    };
    // Codegen's own overload choice at a call site of `owner`'s body: null until that body was
    // emitted, or when the site never reached overload resolution (the caller falls back).
    auto chosen = [&](const std::string& owner, const void* site) -> const FunctionSymbol* {
        if (receiverBodiesEmitted_.count(owner) == 0) { ++receiverFallbackConsults_; return nullptr; }
        if (site == nullptr) return nullptr;
        auto body = receiverCallResolution_.find(owner);
        if (body == receiverCallResolution_.end()) return nullptr;
        auto at = body->second.find(site);
        if (at == body->second.end()) return nullptr;
        auto symbolIn = [&](const std::vector<FunctionSymbol>& list) -> const FunctionSymbol* {
            for (const auto& sym : list)
                if (sym.UniqueName == at->second.second) return &sym;
            return nullptr;
        };
        if (auto it = functionTable.find(at->second.first); it != functionTable.end())
            if (const auto* sym = symbolIn(it->second)) return sym;
        for (const auto& [key, list] : functionTable)
            if (const auto* sym = symbolIn(list)) return sym;
        return nullptr;
    };
    auto methodsOf = [&](const std::string& typeName, const std::string& member, size_t arity) {
        std::vector<const FunctionSymbol*> out;
        auto it = functionTable.find(member);
        if (it == functionTable.end()) return out;
        for (const auto& sym : it->second)
            if (IsNativeReceiverMethod(sym) && sym.Parameters.front().TypeName == typeName
                && (sym.Parameters.size() - 1 >= arity || sym.Variadic))
                out.push_back(&sym);
        return out;
    };
    // A call result that is an alias or a pointer to non-const can be written through.
    auto resultWritable = [](const FunctionSymbol& callee) {
        if (callee.ReturnsAlias) return !callee.ReturnType.IsConstObject();
        return ReceiverPointerLike(callee.ReturnType) && !ReceiverParamPointeeConst(callee.ReturnType);
    };
    // The native struct a member call reaches through at most one pointer, with that pointer count.
    auto callObject = [&](const ReceiverWalk& w, std::string& typeName, int& derefs) {
        derefs = w.Derefs;
        if (w.Type.Pointer && w.Type.PointerDepth <= 1 && !w.Type.ElemPointer) derefs++;
        else if (w.Type.Pointer) return false;
        if (derefs >= 2 || w.Type.IsInterface || IsInterfaceType(w.Type.TypeName)) return false;
        typeName = w.Type.TypeName;
        return true;
    };

    // Null when the path is not rooted at the receiver (a bare name the struct has no field for,
    // a by-value copy, a call result that cannot reach the object).
    std::function<std::optional<ReceiverWalk>(const std::string&, const ReceiverScanFacts&,
                                              const ReceiverPath&, int)> walk;
    /*
     * Return provenance: an alias / pointer result is the receiver's storage unless every return
     * (each `?:` arm on its own) is rooted elsewhere (global, unassigned parameter, literal,
     * `new`). Solved as a least fixed point over returned calls, so a cycle adds nothing; unknown,
     * no body facts or nesting past 256 count as the receiver's (fail closed).
     */
    std::unordered_map<std::string, bool> provenance;
    int provenanceNesting = 0;
    bool provenanceChanged = false;
    auto evaluateProvenance = [&](const std::string& unique) {
        const ReceiverScanFacts& facts = receiverScanFacts_.at(unique);
        for (const auto& r : facts.Returns)
        {
            if (r.Kind == 2) return true;
            if (r.Kind == 1) continue;
            if (walk(unique, facts, r.Source, 0).has_value()) return true;
        }
        return false;
    };
    std::function<bool(const FunctionSymbol&)> resultIsReceiver = [&](const FunctionSymbol& callee) -> bool {
        const std::string& unique = callee.UniqueName;
        if (receiverScanFacts_.count(unique) == 0 || provenanceNesting > 256) return true;
        if (auto it = provenance.find(unique); it != provenance.end()) return it->second;
        provenance[unique] = false;
        ++provenanceNesting;
        const bool value = evaluateProvenance(unique);
        --provenanceNesting;
        if (value)
        {
            provenance[unique] = true;
            provenanceChanged = true;
        }
        // Outermost query: re-evaluate the still-false summaries until nothing changes (one of
        // them may have read another's in-progress answer).
        while (provenanceNesting == 0 && provenanceChanged)
        {
            provenanceChanged = false;
            std::vector<std::string> open;
            for (const auto& [name, isReceiver] : provenance)
                if (!isReceiver) open.push_back(name);
            ++provenanceNesting;
            for (const auto& name : open)
                if (!provenance[name] && evaluateProvenance(name))
                {
                    provenance[name] = true;
                    provenanceChanged = true;
                }
            --provenanceNesting;
        }
        return provenance[unique];
    };
    walk = [&](const std::string& owner, const ReceiverScanFacts& facts, const ReceiverPath& path,
               int depth) -> std::optional<ReceiverWalk> {
        const std::string& structName = facts.StructName;
        ReceiverWalk w;
        if (path.Result >= 0)
        {
            if (depth > 16 || static_cast<size_t>(path.Result) >= facts.Results.size())
            {
                w.Undecided = true;
                return w;
            }
            const auto& call = facts.Results[path.Result];
            std::string typeName = structName;
            int derefs = 0;
            if (call.Implicit)
            {
                TypeAndValue field;
                if (fieldOf(structName, call.Member, field)) return std::nullopt;
            }
            else
            {
                auto r = walk(owner, facts, call.Receiver, depth + 1);
                if (!r.has_value() || r->Undecided) return r;
                if (!r->Known || !callObject(*r, typeName, derefs) || !nativeStruct(typeName)) return std::nullopt;
            }
            if (const auto* callee = chosen(owner, call.Site))
            {
                if (!IsNativeReceiverMethod(*callee) || !resultWritable(*callee)
                    || !resultIsReceiver(*callee))
                    return std::nullopt;
                w.Type = callee->ReturnType;
                w.Type.IsAlias = false;
                w.Derefs = derefs;
            }
            else
            {
                // Not decided by codegen yet: any overload handing back a writable result counts.
                bool writable = false;
                for (const auto* callee : methodsOf(typeName, call.Member, call.Arity))
                    writable = writable || resultWritable(*callee);
                if (!writable) return std::nullopt;
                w.Undecided = true;
                return w;
            }
        }
        else if (path.IsThis) w.Type.TypeName = structName;
        else if (!fieldOf(structName, path.Root, w.Type)) return std::nullopt;
        w.Known = true;
        for (const auto& step : path.Steps)
        {
            if (step == "=")
            {
                // A by-value copy keeps provenance only when what was copied is a pointer or view.
                if (w.Type.IsArrayView || (ReceiverPointerLike(w.Type) && w.Type.ConstArraySize == 0)) continue;
                return std::nullopt;
            }
            const bool member = step.starts_with(".") || step.starts_with("->");
            const bool arrow = step.starts_with("->");
            if (member)
            {
                if (arrow || (w.Type.Pointer && w.Type.ConstArraySize == 0))
                {
                    if (!w.Type.Pointer) { w.Known = false; return w; }
                    w.Derefs++;
                    w.Type = ReceiverPointee(w.Type);
                }
                TypeAndValue field;
                if (w.Type.Pointer || !nativeStruct(w.Type.TypeName)
                    || !fieldOf(w.Type.TypeName, step.substr(arrow ? 2 : 1), field))
                {
                    w.Known = false;
                    return w;
                }
                w.Type = field;
                continue;
            }
            if (step == "[]" && w.Type.ConstArraySize > 0 && !w.Type.Pointer)
            {
                TypeAndValue e = w.Type;
                e.ConstArraySize = 0;
                e.Pointer = w.Type.ElemPointer;
                e.PointerDepth = w.Type.ElemPointer ? 1 : 0;
                e.ElemPointer = false;
                w.Type = e;
                continue;
            }
            if (!w.Type.Pointer && !w.Type.IsArrayView) { w.Known = false; return w; }
            w.Derefs++;
            w.Type = ReceiverPointee(w.Type);
        }
        return w;
    };
    /*
     * An argument keeps the receiver's const when the parameter it binds does. With codegen's
     * choice there is one candidate; otherwise every viable overload must keep it (fail closed).
     */
    auto keepsConst = [&](const std::vector<const FunctionSymbol*>& candidates, bool method, size_t argIndex,
                          size_t arity, bool pointerRelevant, bool aliasRelevant) {
        bool viable = false;
        for (const auto* sym : candidates)
        {
            const size_t base = method ? 1 : 0;
            if (sym->Parameters.size() - base < arity && !sym->Variadic) continue;
            viable = true;
            const size_t index = argIndex + base;
            if (index >= sym->Parameters.size()) continue;
            const TypeAndValue& p = sym->Parameters[index];
            const bool pointerParam = p.Pointer || p.PointerDepth > 0 || p.IsArrayView;
            if (pointerParam && !p.IsAlias)
            {
                if (pointerRelevant && !ReceiverParamPointeeConst(p)) return false;
                continue;
            }
            if (p.IsAlias && !p.IsConstObject() && aliasRelevant) return false;
        }
        return viable;
    };

    // Cause: what the first mutation found writes, for the #10 wording ("n", "&n", "Set()").
    struct Node { bool Mutating = false; std::string Cause; std::vector<std::string> Deps, DepNames; };
    std::unordered_map<std::string, Node> nodes;
    std::vector<std::string> pending{ method.UniqueName };
    auto render = [](const ReceiverScanFacts& facts, const ReceiverPath& path) {
        std::string text = path.Result >= 0 && static_cast<size_t>(path.Result) < facts.Results.size()
            ? facts.Results[path.Result].Member + "()" : path.IsThis ? std::string("this") : path.Root;
        int derefs = 0;
        for (const auto& step : path.Steps)
        {
            if (step == "*") ++derefs;
            else if (step != "=") text += step;
        }
        return std::string(derefs, '*') + text;
    };
    auto calleeText = [](const FunctionSymbol& callee) {
        return (callee.SourceName.empty() ? callee.UniqueName : callee.SourceName) + "()";
    };
    auto depend = [&](Node& node, const FunctionSymbol& callee) {
        if (callee.IsCxx)
        {
            if (!callee.CxxConst) { node.Mutating = true; node.Cause = calleeText(callee); }
            return;
        }
        if (callee.IsExplicitConstMethod()) return;
        if (auto memo = readOnlyMethodMemo_.find(callee.UniqueName); memo != readOnlyMethodMemo_.end())
        {
            if (!memo->second) { node.Mutating = true; node.Cause = calleeText(callee); }
            return;
        }
        if (receiverScanFacts_.count(callee.UniqueName) == 0)
        {
            if (callee.ConstMethodState != 1) { node.Mutating = true; node.Cause = calleeText(callee); }
            return;
        }
        node.Deps.push_back(callee.UniqueName);
        node.DepNames.push_back(calleeText(callee));
        if (nodes.count(callee.UniqueName) == 0) pending.push_back(callee.UniqueName);
    };
    while (!pending.empty())
    {
        const std::string name = pending.back();
        pending.pop_back();
        if (nodes.count(name) != 0) continue;
        Node& node = nodes[name];
        const ReceiverScanFacts& facts = receiverScanFacts_.at(name);
        const std::string& self = facts.StructName;
        for (const auto& write : facts.Writes)
            if (auto w = walk(name, facts, write.Target, 0); w.has_value() && w->Derefs <= 1)
            {
                node.Mutating = true;
                node.Cause = render(facts, write.Target);
                break;
            }
        if (node.Mutating) continue;
        for (const auto& escape : facts.Escapes)
        {
            auto w = walk(name, facts, escape.Source, 0);
            if (!w.has_value()) continue;
            const bool pointerRelevant = escape.AddressOf ? w->Derefs <= 1
                : w->Derefs <= 0 && (!w->Known || ReceiverPointerLike(w->Type));
            const bool aliasRelevant = !escape.AddressOf && w->Derefs <= 1;
            if (escape.Kind == 0)
            {
                if (escape.AliasBind ? aliasRelevant : pointerRelevant) node.Mutating = true;
            }
            else
            {
                // Unresolvable callee: only a definite pointer into the receiver counts.
                const bool definite = escape.AddressOf ? w->Derefs <= 1
                    : (w->Known || w->Undecided) && w->Derefs <= 0
                        && (w->Undecided || ReceiverPointerLike(w->Type));
                std::vector<const FunctionSymbol*> candidates;
                bool method = false;
                bool resolved = escape.Kind != 3;
                const FunctionSymbol* callee = escape.Kind != 3 ? chosen(name, escape.Site) : nullptr;
                if (callee != nullptr && !callee->IsCxx)
                {
                    candidates.push_back(callee);
                    method = IsNativeReceiverMethod(*callee);
                }
                else if (escape.Kind == 1)
                {
                    TypeAndValue field;
                    if (fieldOf(self, escape.Callee, field)) resolved = false;
                    else if (candidates = methodsOf(self, escape.Callee, escape.Arity); !candidates.empty())
                        method = true;
                    else if (auto it = functionTable.find(escape.Callee); it != functionTable.end())
                        for (const auto& sym : it->second)
                            if (!IsNativeReceiverMethod(sym)) candidates.push_back(&sym);
                }
                else if (escape.Kind == 2)
                {
                    auto r = walk(name, facts, escape.CalleeReceiver, 0);
                    std::string typeName;
                    if (r.has_value() && r->Known && r->Type.PointerDepth <= 1 && !r->Type.ElemPointer)
                        typeName = r->Type.TypeName;
                    if (!r.has_value() && escape.CalleeReceiver.Result < 0 && !escape.CalleeReceiver.IsThis
                        && escape.CalleeReceiver.Steps.empty())
                    {
                        // Not a field: a namespace-qualified free function (`os.f(...)`).
                        if (auto it = functionTable.find(escape.CalleeReceiver.Root + "." + escape.Callee);
                            it != functionTable.end())
                            for (const auto& sym : it->second)
                                if (!IsNativeReceiverMethod(sym)) candidates.push_back(&sym);
                        resolved = !candidates.empty();
                    }
                    else if (typeName.empty() || !nativeStruct(typeName)) resolved = false;
                    else { candidates = methodsOf(typeName, escape.Callee, escape.Arity); method = true; }
                }
                if (!resolved || candidates.empty())
                    node.Mutating = definite;
                else if ((pointerRelevant || aliasRelevant)
                    && !keepsConst(candidates, method, escape.ArgIndex, escape.Arity, pointerRelevant, aliasRelevant))
                    node.Mutating = true;
            }
            if (node.Mutating)
            {
                node.Cause = (escape.AddressOf ? "&" : "") + render(facts, escape.Source);
                break;
            }
        }
        if (node.Mutating) continue;
        for (const auto& call : facts.Calls)
        {
            std::string typeName;
            if (call.Implicit)
            {
                TypeAndValue field;
                if (fieldOf(self, call.Member, field)) continue;
                typeName = self;
            }
            else
            {
                auto w = walk(name, facts, call.Receiver, 0);
                if (w.has_value() && w->Undecided)
                {
                    node.Mutating = true;
                    node.Cause = call.Member + "()";
                    break;
                }
                int derefs = 0;
                if (!w.has_value() || !w->Known || !callObject(*w, typeName, derefs)) continue;
                if (IsCxxRecord(typeName))
                {
                    if (CxxMemberIsOnlyNonConst(typeName, call.Member, call.Arity + 1))
                    {
                        node.Mutating = true;
                        node.Cause = call.Member + "()";
                    }
                    if (node.Mutating) break;
                    continue;
                }
                if (!nativeStruct(typeName)) continue;
            }
            if (const auto* callee = chosen(name, call.Site))
            {
                // Codegen's choice: a free function an implicit call resolved to is no dependency.
                if (IsNativeReceiverMethod(*callee)) depend(node, *callee);
            }
            else
                for (const auto* candidate : methodsOf(typeName, call.Member, call.Arity))
                    depend(node, *candidate);
            if (node.Mutating) break;
        }
    }
    // Greatest fixed point: everything starts read-only unless a direct cause was found; a
    // node depending on a mutating node turns mutating until nothing changes.
    for (bool changed = true; changed;)
    {
        changed = false;
        for (auto& [name, node] : nodes)
        {
            if (node.Mutating) continue;
            for (size_t d = 0; d < node.Deps.size(); ++d)
                if (auto it = nodes.find(node.Deps[d]); it != nodes.end() && it->second.Mutating)
                {
                    node.Mutating = true;
                    node.Cause = node.DepNames[d];
                    changed = true;
                    break;
                }
        }
    }
    for (const auto& [name, node] : nodes)
    {
        readOnlyMethodMemo_[name] = !node.Mutating;
        if (node.Mutating) readOnlyCauseMemo_[name] = node.Cause;
    }
    return readOnlyMethodMemo_[method.UniqueName];
}

void LLVMBackend::ResolveConstMethodStates()
{
    for (auto& [key, overloads] : functionTable)
        for (auto& sym : overloads)
            if (receiverScanFacts_.count(sym.UniqueName) != 0)
                sym.ConstMethodState = IsReadOnlyMethod(sym) ? 1 : 2;
}

bool LLVMBackend::ConstReceiverReason(const NamedVariable& self, std::string& object,
                                      std::string& reason) const
{
    using Kind = ConstWriteCause::CauseKind;
    ConstWriteCause cause;
    llvm::Value* address = nullptr;
    if (self.ConstReceiverStorage != nullptr)
        address = self.ConstReceiverStorage;
    else if (self.TypeAndValue.Pointer && self.Primary == nullptr && self.Storage != nullptr)
    {
        // A pointer receiver still in its slot: the slot's declared pointee decides.
        std::string name;
        if (const auto* declared = DeclaredConstSlot(self.Storage, name);
            declared != nullptr && ConstSlotPointeeIsConst(*declared))
        {
            cause.Kind = Kind::ThroughPointer;
            cause.Name = name;
        }
    }
    else
        address = self.TypeAndValue.Pointer || self.Storage == nullptr ? self.Primary : self.Storage;
    if (address != nullptr) cause = FindConstWriteCause(address);
    if (cause.Kind == Kind::None) return false;
    // The implicit `this` of a const member function: its slot is named `S__`.
    if (cause.Kind == Kind::ThroughPointer && cause.Name.ends_with("__")) cause.Name = "this";
    // First spelled name; the implicit `this` slot (`S__`) names nothing the user wrote.
    object.clear();
    for (const std::string* name : { &self.FieldPathText, &self.CallerName, &self.TypeAndValue.VariableName })
        if (!name->empty() && !name->ends_with("__")) { object = *name; break; }
    if (object.empty()) object = cause.Kind == Kind::Object ? cause.Root.Name : cause.Name;
    if (cause.Kind == Kind::Field)
        reason = std::format("field '{}' is const", cause.Name);
    else if (cause.Kind == Kind::ThroughPointer)
        reason = std::format("it is reached through a pointer to const ('{}')", cause.Name);
    else if (cause.Root.Storage == address || object == cause.Root.Name)
        reason = "it is declared const";
    else
        reason = std::format("'{}' is const", cause.Root.Name);
    return true;
}

void LLVMBackend::RefuseConstReceiverMethodCall(const FunctionSymbol& method, const NamedVariable& self,
                                                const std::string& functionName, const std::string& receiverText)
{
    std::string object, reason;
    if (!ConstReceiverReason(self, object, reason)) return;
    // The spelled receiver names an implicit-this field ('w') that the value itself cannot.
    if (!receiverText.empty()) object = receiverText;
    const std::string member = method.SourceName.empty() ? functionName : method.SourceName;
    PendingConstReceiverCall entry;
    entry.Callee = method;
    entry.Message = std::format(
        "cannot call non-const member '{}' on '{}': {}, so it is read-only and only a const "
        "member function may be called on it. {}", member, object, reason,
        std::format("'{}' is not read-only: its body writes the object, or calls a member that does.",
                    member));
    entry.File = sourceFileName;
    entry.Line = currentLine;
    entry.Column = currentColumn;
    // Owned by the scoped / bare expect_error armed here; the file-scope one is module level.
    if (!expectedError.empty() && expectedError != fileScopeExpectedError_) entry.Owner = activeExpectScope_;
    pendingConstReceiverCalls_.push_back(std::move(entry));
}

void LLVMBackend::RecordConstInterfaceImplementer(const FunctionSymbol& impl, const std::string& structName,
                                                  const std::string& ifaceName, const std::string& member)
{
    if (impl.IsCxx || impl.IsExplicitConstMethod()) return;
    PendingConstReceiverCall entry;
    entry.Callee = impl;
    entry.Message = std::format("'{}.{}' implements const interface method '{}.{}' but writes '",
        SpellType(*this, TypeAndValue{ .TypeName = structName }), member,
        SpellType(*this, TypeAndValue{ .TypeName = ifaceName }), member);
    entry.AppendsCause = true;
    entry.File = sourceFileName;
    entry.Line = currentLine;
    entry.Column = currentColumn;
    // The implementing method's own definition, not wherever the class walk stands (a generic
    // interface instantiation leaves it at the interface template).
    if (auto it = receiverScanFacts_.find(impl.UniqueName); it != receiverScanFacts_.end() && it->second.Line != 0)
    {
        if (!it->second.File.empty()) entry.File = it->second.File;
        entry.Line = it->second.Line;
        entry.Column = it->second.Column;
    }
    if (!expectedError.empty() && expectedError != fileScopeExpectedError_) entry.Owner = activeExpectScope_;
    pendingConstReceiverCalls_.push_back(std::move(entry));
}

void LLVMBackend::RefuseConstInterfaceReceiverCall(const NamedVariable& receiver, const std::string& ifaceName,
                                                   const std::string& member)
{
    std::string object, reason;
    if (!ConstReceiverReason(receiver, object, reason)) return;
    LogError(std::format(
        "cannot call non-const member '{}' on '{}': {}, so it is read-only and only a const "
        "member function may be called on it. {}", member, object, reason,
        std::format("'{}.{}' is not declared const in the interface.",
                    SpellType(*this, TypeAndValue{ .TypeName = ifaceName }), member)));
}

// The #8 / #10 text of a pending entry; #10 names what the implementer's body writes.
static std::string PendingConstMessage(const LLVMBackend::PendingConstReceiverCall& entry,
    const std::unordered_map<std::string, std::string>& causes)
{
    if (!entry.AppendsCause) return entry.Message;
    auto it = causes.find(entry.Callee.UniqueName);
    return entry.Message + (it == causes.end() ? std::string("the object") : it->second) + "'";
}

bool LLVMBackend::DeferExpectationVerdict()
{
    const size_t scope = activeExpectScope_;
    if (scope == 0 || expectedError.empty()
        || std::none_of(pendingConstReceiverCalls_.begin(), pendingConstReceiverCalls_.end(),
                        [&](const auto& e) { return e.Owner == scope; }))
        return false;
    deferredExpectations_.push_back(activeExpectPosition_);
    RestoreExpectScope(scope);
    return true;
}

// `aborted`: an expected error ended the walk early. Only the deferred expectations are settled
// (never dropped); a refusal that needed the no-body fallback cannot answer one.
void LLVMBackend::ResolvePendingConstReceiverCalls(bool aborted)
{
    std::vector<PendingConstReceiverCall> pending;
    pending.swap(pendingConstReceiverCalls_);
    std::vector<DeferredExpectation> expectations;
    expectations.swap(deferredExpectations_);
    // Deferred expectations first, in source order: each is answered by the first refusal it
    // owns (matched: PASS; another message: the usual mismatch), or did not occur.
    for (const auto& expectation : expectations)
    {
        bool answered = false;
        bool undecided = false;
        for (const auto& entry : pending)
        {
            if (entry.Owner != expectation.Scope) continue;
            if (aborted) { readOnlyMethodMemo_.clear(); receiverFallbackConsults_ = 0; }
            if (IsReadOnlyMethod(entry.Callee)) continue;
            if (aborted && receiverFallbackConsults_ != 0) { undecided = true; continue; }
            expectedError = expectation.Expected;
            expectedErrorScopeDepth = SIZE_MAX;
            activeExpectScope_ = 0;
            try
            {
                ReportingFileScope fileScope(this, entry.File, entry.Line, entry.Column);
                LogError(PendingConstMessage(entry, readOnlyCauseMemo_));
            }
            catch (const ExpectedErrorReceived&) {}
            RestoreFileScopeExpectedError();
            answered = true;
            break;
        }
        if (answered) continue;
        ReportingFileScope fileScope(this, expectation.File, expectation.Line, expectation.Column);
        if (undecided)
            std::cout << std::format("FAIL: expected error '{}' could not be decided: the compile stopped before every called body was emitted\n", expectation.Expected);
        else
            std::cout << std::format("FAIL: expected error '{}' did not occur\n", expectation.Expected);
        if (diagnosticSink_)
            throw CompilerAbortException{ "expected error did not occur", sourceFileName, currentLine, currentColumn };
        FailCompilation("expected error did not occur");
    }
    if (aborted) return;
    // Module level (and any owner that neither caught nor deferred), in call order; LogError
    // ends the compile (or answers the file-scope expectation) at the first refusal.
    std::unordered_set<size_t> deferredScopes;
    for (const auto& expectation : expectations) deferredScopes.insert(expectation.Scope);
    for (const auto& entry : pending)
    {
        if (deferredScopes.count(entry.Owner) != 0 || IsReadOnlyMethod(entry.Callee)) continue;
        ReportingFileScope fileScope(this, entry.File, entry.Line, entry.Column);
        LogError(PendingConstMessage(entry, readOnlyCauseMemo_));
    }
}

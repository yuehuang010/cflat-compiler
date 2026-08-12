#pragma warning(push)
#pragma warning(disable: 4244 4267)
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/Dominators.h>
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
#include <map>
#include <set>

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

void LLVMBackend::RunInterfaceReturnDangleCheck(llvm::Function* F)
{
        if (!F) return;
        auto it = pendingReturnDangleChecks_.find(F);
        if (it == pendingReturnDangleChecks_.end()) return;
        std::vector<PendingReturnDangleCheck> pending = std::move(it->second);
        pendingReturnDangleChecks_.erase(it);

        for (const auto& rec : pending)
        {
            if (!rec.Slot) continue;
            bool hasNonFrameWriter = false;
            for (llvm::User* u : rec.Slot->users())
            {
                const auto* store = llvm::dyn_cast<llvm::StoreInst>(u);
                if (store == nullptr || store->getPointerOperand() != rec.Slot) continue;
                const llvm::Value* v = store->getValueOperand();
                bool isNullish = llvm::isa<llvm::UndefValue>(v)
                    || llvm::isa<llvm::ConstantAggregateZero>(v)
                    || (llvm::isa<llvm::Constant>(v)
                        && llvm::cast<llvm::Constant>(v)->isNullValue());
                if (isNullish) { hasNonFrameWriter = true; break; }
                const InterfaceBoxRecord* box = FindInterfaceBoxByFatValue(v);
                if (box == nullptr || box->Source != InterfaceBoxSource::FrameStorage)
                {
                    hasNonFrameWriter = true;
                    break;
                }
            }
            if (rec.FrameStorageProvenance && !rec.ProvenanceUnknown && !hasNonFrameWriter)
            {
                SetSourceLocation(static_cast<size_t>(rec.Line), static_cast<size_t>(rec.Col));
                if (rec.FrameStorageClassName.empty())
                    LogError(std::format(
                        "cannot return a local value as interface '{}' - the interface fat pointer "
                        "would dangle once this function returns; allocate the object on the heap "
                        "and return the pointer", rec.InterfaceName));
                else
                    LogError(std::format(
                        "cannot return local value '{}' as interface '{}' - the interface fat "
                        "pointer would dangle once this function returns; allocate on the heap "
                        "('new {}') and return the pointer", rec.FrameStorageClassName,
                        rec.InterfaceName, rec.FrameStorageClassName));
            }
            bool tainted = false;
            bool accepted = false;
            std::string taintClassName;
            for (llvm::User* u : rec.Slot->users())
            {
                if (llvm::isa<llvm::LoadInst>(u)) continue;
                if (const auto* call = llvm::dyn_cast<llvm::CallBase>(u))
                {
                    // Only debug/lifetime markers are inert for a SLOT. Deliberately NOT
                    // CallIsPointerOpaqueIntrinsic: that helper also admits llvm.mem*, which
                    // is sound for its question (a pointer VALUE's escape) but not for this
                    // one - a memcpy into the slot is a real write of a possibly non-frame
                    // value, so it must fall through to accept.
                    if (const llvm::Function* callee = call->getCalledFunction();
                        callee != nullptr && (callee->getName().starts_with("llvm.dbg.")
                            || callee->getName().starts_with("llvm.lifetime.")))
                        continue;
                    accepted = true;
                    break;
                }
                if (auto* store = llvm::dyn_cast<llvm::StoreInst>(u))
                {
                    // The slot's ADDRESS stored elsewhere (not a store INTO the slot) is an
                    // escape, not a write - fall through to the catch-all accept below.
                    if (store->getPointerOperand() != rec.Slot) { accepted = true; break; }
                    llvm::Value* v = store->getValueOperand();
                    bool isNullish = llvm::isa<llvm::UndefValue>(v)
                        || llvm::isa<llvm::ConstantAggregateZero>(v)
                        || (llvm::isa<llvm::Constant>(v) && llvm::cast<llvm::Constant>(v)->isNullValue());
                    if (isNullish)
                    {
                        if (kNullStoreIsAcceptEvidence) { accepted = true; break; }
                        continue;
                    }
                    if (const InterfaceBoxRecord* box = FindInterfaceBoxByFatValue(v);
                        box != nullptr && box->Source == InterfaceBoxSource::FrameStorage)
                    {
                        tainted = true;
                        taintClassName = box->SourceClassName;
                        continue;
                    }
                    // No ledger record at all, or Heap / Parameter / Global / Unknown, or a
                    // load of another slot / call result / phi / argument - accept evidence.
                    accepted = true;
                    break;
                }
                // Any other user whatsoever - a call argument (handled above), a GEP with
                // any non-load user, the address stored anywhere, memcpy/memmove, an
                // AtomicRMW, anything not explicitly whitelisted above - accept.
                accepted = true;
                break;
            }
            if (!tainted || accepted) continue;

            SetSourceLocation(static_cast<size_t>(rec.Line), static_cast<size_t>(rec.Col));
            if (taintClassName.empty())
                LogError(std::format(
                    "cannot return a local value as interface '{}' - the interface fat pointer "
                    "would dangle once this function returns; allocate the object on the heap "
                    "and return the pointer", rec.InterfaceName));
            else
                LogError(std::format(
                    "cannot return local value '{}' as interface '{}' - the interface fat "
                    "pointer would dangle once this function returns; allocate on the heap "
                    "('new {}') and return the pointer", taintClassName, rec.InterfaceName, taintClassName));
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
                            || f->getName().starts_with("llvm.lifetime."))) continue;
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
        const auto* call = llvm::dyn_cast<llvm::CallInst>(v);
        if (call == nullptr) return nullptr;
        const llvm::Function* callee = call->getCalledFunction();
        if (callee == nullptr || callee->isDeclaration()) return nullptr;
        const llvm::Constant* result = nullptr;
        for (const llvm::BasicBlock& b : *callee)
        {
            const auto* ret = llvm::dyn_cast_or_null<llvm::ReturnInst>(b.getTerminator());
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
            cfg.Cd = nulldf::ComputeControlDependence(cfg.Blocks, cfg.Rpo, &provenNoReturn_);
            cfg.HaveCd = true;
        }
        const nulldf::CdSet& cdAccess = cfg.Cd[anchorBB];
        for (llvm::BasicBlock* m : witnesses)
            if (!nulldf::IsSubset(cdAccess, cfg.Cd[m])) return false;
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
                rec.VarName, rec.VarName, rec.VarName, rec.MemberName, rec.IfaceName,
                rec.VarName, rec.MemberName));
        else
            LogError(std::format(
                "method call on null interface value '{}' - '{}' has not been assigned an "
                "implementation since it was last set to null, so '{}.{}()' would dispatch "
                "through a null '{}' vtable; assign one before the call, or write '{}?.{}()' "
                "to skip it when null",
                rec.VarName, rec.VarName, rec.VarName, rec.MemberName, rec.IfaceName,
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
                rec.VarName, rec.VarName, rec.VarName, rec.MemberName, rec.IfaceName));
        else
            LogError(std::format(
                "method call on uninitialized interface value '{}' - '{}' is never assigned an "
                "implementation before this call, so '{}.{}()' would dispatch through an "
                "uninitialized '{}' vtable; assign one before the call",
                rec.VarName, rec.VarName, rec.VarName, rec.MemberName, rec.IfaceName));
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
                    cfg.Cd = nulldf::ComputeControlDependence(cfg.Blocks, cfg.Rpo, &provenNoReturn_);
                    cfg.HaveCd = true;
                }
                if (cfg.Cd[ab].empty()) ReportNullIfaceUninitAccess(rec);
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
                nw = neverWritten.emplace(gv, InterfaceGlobalNeverWritten(gv)).first;
            if (!nw->second) continue;

            // Fact 2. Witness = entry block, so this asks that the access be reached on every
            // path through the function - exactly what a guard around it makes false.
            NullIfaceCfgInfo& cfg = cfgs[F];
            EnsureNullIfaceBlocks(cfg, F);
            llvm::BasicBlock* entry = &F->getEntryBlock();
            if (!cfg.Rpo.count(bb) || !cfg.Rpo.count(entry)) continue;
            if (!cfg.HaveCd)
            {
                cfg.Cd = nulldf::ComputeControlDependence(cfg.Blocks, cfg.Rpo, &provenNoReturn_);
                cfg.HaveCd = true;
            }
            if (!nulldf::IsSubset(cfg.Cd[bb], cfg.Cd[entry])) continue;

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
                nv.MovedFields.insert(it->second.begin(), it->second.end());
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
            {
                std::unordered_set<std::string> merged = tf->second;
                merged.insert(ef->second.begin(), ef->second.end());
                nv.MovedFields = std::move(merged);
            }
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

void LLVMBackend::ApplyAbiCallAttributes(llvm::CallInst* ci, const AbiRecipe& recipe)
{
        unsigned attrIdx = 0;
        if (recipe.retSlot.kind == AbiSlot::SRetReturn)
        {
            ci->addParamAttr(attrIdx, llvm::Attribute::getWithStructRetType(*context, recipe.retSlot.structTy));
            ci->addParamAttr(attrIdx, llvm::Attribute::NoAlias);
            if (recipe.retSlot.align > 0)
                ci->addParamAttr(attrIdx, llvm::Attribute::getWithAlignment(*context, llvm::Align(recipe.retSlot.align)));
            ++attrIdx;
        }
        for (size_t i = 0; i < recipe.paramSlots.size(); ++i)
        {
            const AbiSlot& s = recipe.paramSlots[i];
            if (s.kind == AbiSlot::ByVal)
            {
                ci->addParamAttr(attrIdx, llvm::Attribute::getWithByValType(*context, s.structTy));
                if (s.align > 0)
                    ci->addParamAttr(attrIdx, llvm::Attribute::getWithAlignment(*context, llvm::Align(s.align)));
            }
            attrIdx += SlotLLVMParamCount(s); // CoercePair expands to two LLVM params
        }
    }

llvm::Value* LLVMBackend::EmitAbiLoweredCall(const FunctionSymbol& candidate, std::vector<llvm::Value*>& argList)
{
        const AbiRecipe& recipe = candidate.Recipe;
        std::vector<llvm::Value*> loweredArgs;
        loweredArgs.reserve(argList.size() + (recipe.retSlot.kind == AbiSlot::SRetReturn ? 1 : 0));

        llvm::AllocaInst* sretSlot = nullptr;
        if (recipe.retSlot.kind == AbiSlot::SRetReturn)
        {
            sretSlot = AllocaAtEntry(recipe.retSlot.structTy, nullptr, "sret", recipe.retSlot.align);
            loweredArgs.push_back(sretSlot);
        }

        for (size_t i = 0; i < argList.size() && i < recipe.paramSlots.size(); ++i)
        {
            const AbiSlot& s = recipe.paramSlots[i];
            llvm::Value* v = argList[i];
            if (s.kind == AbiSlot::Direct)
            {
                loweredArgs.push_back(v);
            }
            else if (s.kind == AbiSlot::CoerceToInt)
            {
                // v is a struct value. Place in an alloca, then load the eightbyte through a
                // bitcast - portable across all element layouts and lets LLVM coalesce. The
                // coerce type may be integer or SSE (float/double/<2 x float>) under SysV.
                auto* slot = AllocaAtEntry(s.structTy, nullptr, "abi.coerce", s.align);
                builder->CreateStore(v, slot);
                loweredArgs.push_back(LoadCoerceAt(slot, s.coerceTy, 0));
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
                auto* slot = AllocaAtEntry(s.structTy, nullptr, "abi.byval", s.align);
                builder->CreateStore(v, slot);
                loweredArgs.push_back(slot);
            }
        }

        auto* ci = builder->CreateCall(candidate.Function, loweredArgs);
        ci->setCallingConv(candidate.Function->getCallingConv());
        ApplyAbiCallAttributes(ci, recipe);

        if (recipe.retSlot.kind == AbiSlot::SRetReturn)
            return builder->CreateLoad(recipe.retSlot.structTy, sretSlot);
        if (recipe.retSlot.kind == AbiSlot::CoerceToInt)
        {
            auto* slot = AllocaAtEntry(recipe.retSlot.structTy, nullptr, "abi.ret", recipe.retSlot.align);
            StoreCoerceAt(slot, ci, 0);
            return builder->CreateLoad(recipe.retSlot.structTy, slot);
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

llvm::Value* LLVMBackend::LoadCoerceAt(llvm::Value* structSlot, llvm::Type* coerceTy, uint64_t byteOff)
{
        llvm::Value* p = structSlot;
        if (byteOff != 0)
        {
            auto* i8p = builder->CreateBitCast(structSlot, builder->getInt8Ty()->getPointerTo());
            p = builder->CreateInBoundsGEP(builder->getInt8Ty(), i8p, builder->getInt64(byteOff));
        }
        auto* cp = builder->CreateBitCast(p, coerceTy->getPointerTo());
        return builder->CreateLoad(coerceTy, cp);
    }

void LLVMBackend::StoreCoerceAt(llvm::Value* structSlot, llvm::Value* val, uint64_t byteOff)
{
        llvm::Value* p = structSlot;
        if (byteOff != 0)
        {
            auto* i8p = builder->CreateBitCast(structSlot, builder->getInt8Ty()->getPointerTo());
            p = builder->CreateInBoundsGEP(builder->getInt8Ty(), i8p, builder->getInt64(byteOff));
        }
        auto* cp = builder->CreateBitCast(p, val->getType()->getPointerTo());
        builder->CreateStore(val, cp);
    }

llvm::Value* LLVMBackend::CreateFunctionCall(llvm::Function* func, const std::vector<llvm::Value*>& arg)
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
                auto valueType = value->getType();
                // Convert 8/16-bit int to 32-bit and 16-bit/float to double (default argument promotions).
                // bool (i1) is zero-extended: sign-extending it would pass true as -1.
                if (valueType->isIntegerTy(1))
                    callArgs.push_back(builder->CreateZExt(value, builder->getInt32Ty(), "conv"));
                else if (valueType->isIntegerTy(8) || valueType->isIntegerTy(16))
                    callArgs.push_back(builder->CreateSExt(value, builder->getInt32Ty(), "conv"));
                else if (valueType->is16bitFPTy() || valueType->isFloatTy())
                    callArgs.push_back(builder->CreateFPExt(value, builder->getDoubleTy(), "conv"));
                else
                    callArgs.push_back(value);
            }
        }
        else
        {
            for (int i = 0; i < (int)arg.size(); i++)
                callArgs.push_back(Upconvert(arg[i], func->getArg(i)));
        }

        auto* ci = builder->CreateCall(func, callArgs);
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
        if (nv.IsOwning || nv.PointerRebound) return false;
        const NamedVariable* src = FindVariableByStorage(nv.OwningLocalStorage);
        return src != nullptr && src->IsOwning && !src->PointerRebound;
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

void LLVMBackend::CreateReturnCall(llvm::Value* value, llvm::Value* returnedLocalStorage, const std::string& interfaceReturnStructName)
{
        if (!IsInsertBlockLive())
            return;

        // If returning an owned variable loaded from a local alloca, suppress its cleanup
        // so we don't free the value before the caller receives it.
        // The loaded snapshot in `value` already captures the pointer; the caller takes ownership.
        NamedVariable* ownedStringReturnVar = nullptr;
        NamedVariable* ownedPtrReturnVar    = nullptr;
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
                                    llvm::ConstantAggregateZero::get(loadInst->getType()), srcAlloca);
                                return;
                            }
                            if (nv.Storage == srcAlloca && nv.IsOwning)
                            {
                                ownedPtrReturnVar = &nv;
                                nv.IsOwning = false;  // suppress cleanup on return path
                                return;
                            }
                        }
                        for (auto& [varName, nv] : frame.functionArgument)
                        {
                            if (nv.Storage == srcAlloca && nv.IsOwning)
                            {
                                ownedPtrReturnVar = &nv;
                                nv.IsOwning = false;  // suppress cleanup on return path
                                return;
                            }
                        }
                    }
                }();
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

        // Emit destructors for all scopes from innermost out to the function boundary
        for (auto it = stackNamedVariable.rbegin(); it != stackNamedVariable.rend(); ++it)
        {
            EmitDestructorsForScope(*it);
            if (it->isFunction) break;
        }

        // Restore flags (clean state, though the scope is about to be popped anyway).
        // The returned string local was moved out by zeroing its storage (not by toggling
        // IsOwningString), so nothing to restore for it - its always-run dtor now no-ops on
        // the zeroed value.
        if (ownedPtrReturnVar)    ownedPtrReturnVar->IsOwning = true;
        returnedStructDtorSkipAlloca = prevStructSkip;

        // Auto return-type inference: record the (BB, value) pair so the caller can
        // unify types after body emission. Terminate the BB with a placeholder
        // 'unreachable' that will be replaced with a real 'ret' once the inferred
        // signature is known and BBs are spliced onto the new function.
        if (autoReturnCapture)
        {
            auto* placeholder = builder->CreateUnreachable();
            autoReturnCapture->push_back({ builder->GetInsertBlock(), value, placeholder });
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
            if (strTy && retTy == strTy && value->getType() != strTy)
            {
                auto* ptrTy = builder->getInt8Ty()->getPointerTo();
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
            value = Upconvert(value, retTy);
            // Upconvert only widens; handle narrowing int -> bool explicitly (same as CreateAssignment).
            // Warn: CFlat requires explicit narrowing - write "return expr != 0;" instead.
            if (retTy == builder->getInt1Ty() && value->getType()->isIntegerTy() && value->getType() != retTy)
            {
                LogErrorMessage("implicit int-to-bool conversion on return - use '!= 0' to make narrowing explicit");
                value = builder->CreateICmpNE(value, llvm::ConstantInt::get(value->getType(), 0), "tobool");
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

// Move-state dataflow analyzer (Stage 2: owns the loop-carried use-after-move diagnostic).
//
// Runs the Rust-style MaybeInitialized (MAY / union) fixpoint over the LLVM CFG
// that the code-gen walker already emits. Move-events (kills / gens / uses) are
// recorded by LLVMBackend in emission order, each tagged with the llvm::BasicBlock
// it was emitted into. This solver replays them per-block against a solved IN-state
// and collects any USE of a maybe-moved move-path as a Divergence.
//
// LLVMBackend::RunMoveDataflow turns the earliest divergence into a real LogError.
// The inline linear checker still owns straight-line + if/else and aborts before this.
#pragma once

#include <algorithm>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CFG.h>       // llvm::predecessors
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>   // llvm::CallBase
#include <llvm/IR/Instructions.h>  // llvm::ReturnInst

namespace movedf
{
    // A movepath is "name" (whole var) or "name.field" (field subpath). Index/deref/any
    // non-name-or-static-field lvalue carries no path and is never recorded.
    enum class EventKind
    {
        KillWhole,        // insert "name"
        KillField,        // insert "name.field"
        GenReviveWhole,   // erase "name" (leave "name.*")
        GenReviveField,   // erase "name.field"
        GenBind,          // erase "name" AND all "name.*" (fresh binding)
        Use,              // check-only, no state change
    };

    struct Event
    {
        llvm::BasicBlock* block = nullptr;
        EventKind kind = EventKind::Use;
        std::string name;
        std::string field;
        int line = 0;
        int col = 0;
    };

    struct Divergence
    {
        std::string path;
        int line = 0;
        int col = 0;
    };

    // Maybe-moved lattice element: the set of maybe-moved move-paths.
    using MovedSet = std::unordered_set<std::string>;

    // USE-check rule (mirrors LLVMBackend::MovedUseSubject): path "name" is moved if
    // "name" is in the set; path "name.field" is moved if "name" OR "name.field" is.
    inline bool IsPathMoved(const MovedSet& s, const std::string& name, const std::string& field)
    {
        if (s.count(name)) return true;
        if (!field.empty() && s.count(name + "." + field)) return true;
        return false;
    }

    // Apply one event's gen/kill to the running set (Use is a no-op).
    inline void ApplyEvent(MovedSet& s, const Event& e)
    {
        switch (e.kind)
        {
        case EventKind::KillWhole:
            s.insert(e.name);
            break;
        case EventKind::KillField:
            s.insert(e.name + "." + e.field);
            break;
        case EventKind::GenReviveWhole:
            s.erase(e.name);
            break;
        case EventKind::GenReviveField:
            s.erase(e.name + "." + e.field);
            break;
        case EventKind::GenBind:
        {
            // Fresh binding clears the whole var and every field subpath.
            s.erase(e.name);
            std::string prefix = e.name + ".";
            for (auto it = s.begin(); it != s.end(); )
            {
                if (it->compare(0, prefix.size(), prefix) == 0)
                    it = s.erase(it);
                else
                    ++it;
            }
            break;
        }
        case EventKind::Use:
            break;
        }
    }

    // Reverse-post-order index for F's reachable blocks (entry first). An edge u->v with
    // rpo[v] <= rpo[u] is a retreating (back) edge - the loop signal we key on.
    inline std::unordered_map<llvm::BasicBlock*, int> ComputeRpoIndex(llvm::Function* F)
    {
        std::vector<llvm::BasicBlock*> post;
        std::unordered_set<llvm::BasicBlock*> visited;
        // Iterative post-order DFS from entry (stack of block + child-iteration cursor).
        std::vector<std::pair<llvm::BasicBlock*, llvm::succ_iterator>> stack;
        llvm::BasicBlock* entry = &F->getEntryBlock();
        visited.insert(entry);
        stack.push_back({ entry, llvm::succ_begin(entry) });
        while (!stack.empty())
        {
            auto& [bb, it] = stack.back();
            if (it == llvm::succ_end(bb)) { post.push_back(bb); stack.pop_back(); continue; }
            llvm::BasicBlock* succ = *it; ++it;
            if (visited.insert(succ).second)
                stack.push_back({ succ, llvm::succ_begin(succ) });
        }
        std::unordered_map<llvm::BasicBlock*, int> rpo;
        int idx = 0;
        for (auto rit = post.rbegin(); rit != post.rend(); ++rit)
            rpo[*rit] = idx++;
        return rpo;
    }

    // Forward MAY/union fixpoint over F's CFG using its recorded events, then replay each
    // block against its solved IN-state and collect USE events whose path is maybe-moved
    // ONLY via a loop back-edge. A use that is already maybe-moved in a purely forward
    // (acyclic, no back-edge) pass mirrors what the inline linear checker owns, so it is
    // NOT reported here - only loop-carried uses the inline checker misses are.
    inline std::vector<Divergence> AnalyzeFunction(llvm::Function* F, const std::vector<Event>& events)
    {
        std::vector<Divergence> out;
        if (!F || F->isDeclaration() || F->empty()) return out;

        // Group events by block, preserving emission (walk) order within each block.
        std::unordered_map<llvm::BasicBlock*, std::vector<const Event*>> byBlock;
        for (const auto& e : events)
            if (e.block && e.block->getParent() == F)
                byBlock[e.block].push_back(&e);

        // Analyze ONLY entry-reachable blocks. Unreachable blocks left by an aborted
        // scoped-block expect_error carry stale moved-state that must not leak into a use.
        std::unordered_map<llvm::BasicBlock*, int> rpo = ComputeRpoIndex(F);

        std::unordered_map<llvm::BasicBlock*, MovedSet> blockOut;
        for (auto& BB : *F) if (rpo.count(&BB)) blockOut[&BB]; // default-empty OUT

        llvm::BasicBlock* entry = &F->getEntryBlock();

        // Round-robin until no OUT changes. Finite path set => terminates.
        bool changed = true;
        while (changed)
        {
            changed = false;
            for (auto& BB : *F)
            {
                if (!rpo.count(&BB)) continue;
                MovedSet in;
                if (&BB != entry)
                    for (llvm::BasicBlock* pred : llvm::predecessors(&BB))
                    {
                        if (!rpo.count(pred)) continue;
                        const MovedSet& po = blockOut[pred];
                        in.insert(po.begin(), po.end());
                    }

                MovedSet result = in;
                if (auto it = byBlock.find(&BB); it != byBlock.end())
                    for (const Event* e : it->second)
                        ApplyEvent(result, *e);

                if (result != blockOut[&BB])
                {
                    blockOut[&BB] = std::move(result);
                    changed = true;
                }
            }
        }

        // Acyclic OUT: single pass in RPO taking only FORWARD-edge predecessors (rpo<current).
        // This reproduces the inline checker's linear + if/else merge view (no back-edges).
        std::vector<llvm::BasicBlock*> rpoOrder;
        for (auto& BB : *F) if (rpo.count(&BB)) rpoOrder.push_back(&BB);
        std::sort(rpoOrder.begin(), rpoOrder.end(),
                  [&](llvm::BasicBlock* a, llvm::BasicBlock* b){ return rpo[a] < rpo[b]; });
        std::unordered_map<llvm::BasicBlock*, MovedSet> acyclicOut;
        for (llvm::BasicBlock* bb : rpoOrder)
        {
            MovedSet in;
            for (llvm::BasicBlock* pred : llvm::predecessors(bb))
                if (rpo.count(pred) && rpo[pred] < rpo[bb])
                    in.insert(acyclicOut[pred].begin(), acyclicOut[pred].end());
            MovedSet result = in;
            if (auto it = byBlock.find(bb); it != byBlock.end())
                for (const Event* e : it->second)
                    ApplyEvent(result, *e);
            acyclicOut[bb] = std::move(result);
        }

        // Diagnose: replay each block's events against BOTH its fixpoint IN and its acyclic IN.
        // Report a use maybe-moved in the fixpoint but NOT in the acyclic (linear) view.
        std::unordered_set<std::string> seen; // dedup identical path+location report keys
        for (auto& BB : *F)
        {
            if (!rpo.count(&BB)) continue;
            MovedSet in, acyclicIn;
            if (&BB != entry)
                for (llvm::BasicBlock* pred : llvm::predecessors(&BB))
                {
                    if (!rpo.count(pred)) continue;
                    in.insert(blockOut[pred].begin(), blockOut[pred].end());
                    if (rpo[pred] < rpo[&BB])
                        acyclicIn.insert(acyclicOut[pred].begin(), acyclicOut[pred].end());
                }

            auto it = byBlock.find(&BB);
            if (it == byBlock.end()) continue;
            MovedSet running = in, acyclicRunning = acyclicIn;
            for (const Event* e : it->second)
            {
                if (e->kind == EventKind::Use)
                {
                    bool movedFix = IsPathMoved(running, e->name, e->field);
                    bool movedLinear = IsPathMoved(acyclicRunning, e->name, e->field);
                    if (movedFix && !movedLinear)
                    {
                        std::string path = e->field.empty() ? e->name : (e->name + "." + e->field);
                        // One source site can be tapped by two USE sites (load + call-arg);
                        // report each distinct path+location once.
                        if (seen.insert(path + ":" + std::to_string(e->line) + ":" +
                                        std::to_string(e->col)).second)
                            out.push_back({ path, e->line, e->col });
                    }
                }
                else
                {
                    ApplyEvent(running, *e);
                    ApplyEvent(acyclicRunning, *e);
                }
            }
        }

        return out;
    }
}


// Explicit-move null-state analysis: the cross-block half of the "dereference of moved variable"
// diagnostic. Deliberately a SEPARATE event stream from movedf above, because an explicitly
// moved-out local stays legally readable-as-null by design, so its dereferences must not join
// movedf's plain-read Use stream.
//
// The reporting rule is CONTROL-DEPENDENCE CONTAINMENT, chosen because it is false-positive-free
// BY CONSTRUCTION rather than by enumerating guard shapes:
//
//   report a dereference of x at block D, witnessed by a move of x at block M, only when
//   CD*(D) is a SUBSET of CD*(M) - that is, D is not guarded by any branch edge the move
//   was not already guarded by - and x is neither read, revived nor address-escaped on the
//   path from M to D.
//
// A guard is, by definition, a control-flow decision that can let the dereference be skipped
// while the move still happened. Such a decision necessarily puts an extra edge in the
// dereference's control-dependence set, so it necessarily suppresses the report - whatever the
// guard is written in terms of (the pointer itself, a bool, an int, a struct field, a global, a
// correlated scrutinee, or an expression the compiler cannot interpret at all). No guard
// recognition, and therefore no guard-shape enumeration, is involved.
//
// Conditions are compared by BRANCH EDGE IDENTITY. Two syntactically identical conditions in
// two different `if` statements are different edges, so `if (c) { move a; } if (c) { a->v; }`
// is not reported. That is a false negative, and false negatives are the acceptable direction.
namespace nulldf
{
    enum class EventKind
    {
        SetNull,    // explicit 'move x' of a whole local - x is null from here on this path
        ClearNull,  // fresh binding or reassignment - x is live again
        Escape,     // '&x' - the pointee may be rewritten through the escaped address
        Read,       // any NON-dereference read of x - the only way a guard can consult it
        Deref,      // '->' / '.' / '*' / '[]' on x (check-only)
    };

    struct Event
    {
        llvm::BasicBlock* block = nullptr;
        EventKind kind = EventKind::Deref;
        std::string name;
        int line = 0;
        int col = 0;
    };

    struct Divergence
    {
        std::string name;
        int line = 0;
        int col = 0;
    };

    // A control-dependence edge is the (branch block, chosen successor) pair.
    using CdEdge = std::pair<llvm::BasicBlock*, llvm::BasicBlock*>;

    struct CdEdgeHash
    {
        size_t operator()(const CdEdge& e) const noexcept
        {
            return std::hash<const void*>{}(e.first) * 31u + std::hash<const void*>{}(e.second);
        }
    };
    using CdSet = std::unordered_set<CdEdge, CdEdgeHash>;

    // Blocks still pending as move witnesses for one name, keyed by the move's block.
    using WitnessSet = std::unordered_set<llvm::BasicBlock*>;
    using NullState = std::map<std::string, WitnessSet>;

    // Functions already proven to never return (see FunctionNeverReturns). A backend-side side
    // table rather than the LLVM 'noreturn' attribute on purpose: the attribute would also
    // license optimizations, a far wider blast radius than this diagnostic needs.
    using NoReturnSet = std::unordered_set<const llvm::Function*>;

    // Does this block terminate the program rather than fall through? An LLVM 'noreturn'
    // callee (or call site) is authoritative; then the proven set; then a name fallback,
    // because this compiler never sets the attribute on its own externs.
    inline bool BlockTerminatesProgram(llvm::BasicBlock* bb, const NoReturnSet* proven)
    {
        for (llvm::Instruction& inst : *bb)
        {
            auto* call = llvm::dyn_cast<llvm::CallBase>(&inst);
            if (call == nullptr) continue;
            if (call->doesNotReturn()) return true;
            llvm::Function* callee = call->getCalledFunction();
            if (callee == nullptr) continue;
            if (callee->doesNotReturn()) return true;
            if (proven != nullptr && proven->count(callee)) return true;
            llvm::StringRef n = callee->getName();
            if (n == "exit" || n == "abort" || n == "_Exit" || n == "quick_exit") return true;
        }
        return false;
    }

    // True when no 'ret' in F is reachable without first hitting a program-terminating call.
    // A terminating call always precedes its block's terminator, so a terminating block never
    // contributes a live return and its successors are not explored. Note this proves NEVER
    // returns, not "may not return": a conditionally-terminating helper
    // (`void dieIf(bool c) { if (c) exit(1); }`) has a live return and is correctly rejected.
    inline bool FunctionNeverReturns(llvm::Function* F, const NoReturnSet* proven)
    {
        if (!F || F->isDeclaration() || F->empty()) return false;
        std::unordered_set<llvm::BasicBlock*> seen;
        std::vector<llvm::BasicBlock*> work{ &F->getEntryBlock() };
        seen.insert(&F->getEntryBlock());
        while (!work.empty())
        {
            llvm::BasicBlock* bb = work.back();
            work.pop_back();
            if (BlockTerminatesProgram(bb, proven)) continue;
            if (llvm::isa<llvm::ReturnInst>(cflat_llvm_compat::GetTerminatorOrNull(bb))) return false;
            for (llvm::BasicBlock* s : llvm::successors(bb))
                if (seen.insert(s).second) work.push_back(s);
        }
        return true;
    }

    // Post-dominator sets over the reachable sub-CFG, by iterative intersection.
    //
    // Complexity is O(B^2) in blocks (a block set per block, plus the control-dependence
    // closure that consumes it). That is fine here only because AnalyzeFunction bails on the
    // 'haveSet' fast path first, so a function containing no explicit 'move' costs nothing at
    // all; a generated function with thousands of blocks AND a move would be measurable.
    //
    // Three shapes are treated as ends of the program rather than fall-throughs, so that
    // nothing spuriously post-dominates code they guard: a terminator with no successors, a
    // block that cannot reach any such terminator (an infinite loop), and a block that calls a
    // noreturn function (see BlockTerminatesProgram - `if (flag) { exit(1); } return x->v;`
    // must not leave the return post-dominating the branch).
    inline std::unordered_map<llvm::BasicBlock*, std::unordered_set<llvm::BasicBlock*>>
    ComputePostDominators(const std::vector<llvm::BasicBlock*>& blocks,
                          const std::unordered_map<llvm::BasicBlock*, int>& rpo,
                          const NoReturnSet* proven)
    {
        std::unordered_map<llvm::BasicBlock*, std::unordered_set<llvm::BasicBlock*>> pd;

        std::unordered_set<llvm::BasicBlock*> terminal;
        for (llvm::BasicBlock* bb : blocks)
            if (llvm::succ_begin(bb) == llvm::succ_end(bb) || BlockTerminatesProgram(bb, proven))
                terminal.insert(bb);

        // Blocks that can reach an end of the program.
        std::unordered_set<llvm::BasicBlock*> reachesExit;
        bool grew = true;
        while (grew)
        {
            grew = false;
            for (llvm::BasicBlock* bb : blocks)
            {
                if (reachesExit.count(bb)) continue;
                bool ok = terminal.count(bb) > 0;
                if (!ok)
                    for (llvm::BasicBlock* s : llvm::successors(bb))
                        if (rpo.count(s) && reachesExit.count(s)) { ok = true; break; }
                if (ok) { reachesExit.insert(bb); grew = true; }
            }
        }

        std::unordered_set<llvm::BasicBlock*> all(blocks.begin(), blocks.end());
        for (llvm::BasicBlock* bb : blocks)
        {
            if (terminal.count(bb) || !reachesExit.count(bb)) pd[bb] = { bb };
            else pd[bb] = all;
        }

        bool changed = true;
        while (changed)
        {
            changed = false;
            for (auto it = blocks.rbegin(); it != blocks.rend(); ++it)
            {
                llvm::BasicBlock* bb = *it;
                if (terminal.count(bb) || !reachesExit.count(bb)) continue;
                std::unordered_set<llvm::BasicBlock*> meet;
                bool first = true;
                for (llvm::BasicBlock* s : llvm::successors(bb))
                {
                    if (!rpo.count(s)) continue;
                    if (first) { meet = pd[s]; first = false; continue; }
                    for (auto mit = meet.begin(); mit != meet.end(); )
                        mit = pd[s].count(*mit) ? std::next(mit) : meet.erase(mit);
                }
                meet.insert(bb);
                if (meet != pd[bb]) { pd[bb] = std::move(meet); changed = true; }
            }
        }
        return pd;
    }

    // Transitive control-dependence set of every reachable block. CD(X) holds edge (B -> S)
    // when X post-dominates S but not B; the closure then folds in CD*(B) for each such B, so a
    // block nested two levels deep carries both enclosing decisions (this is what makes the
    // loop-carried case work - the loop body is control-dependent on the loop condition).
    inline std::unordered_map<llvm::BasicBlock*, CdSet>
    ComputeControlDependence(const std::vector<llvm::BasicBlock*>& blocks,
                             const std::unordered_map<llvm::BasicBlock*, int>& rpo,
                             const NoReturnSet* proven)
    {
        auto pd = ComputePostDominators(blocks, rpo, proven);
        auto postDominates = [&](llvm::BasicBlock* p, llvm::BasicBlock* b)
            { auto it = pd.find(b); return it != pd.end() && it->second.count(p) > 0; };

        std::vector<CdEdge> branchEdges;
        for (llvm::BasicBlock* bb : blocks)
        {
            size_t n = 0;
            for (llvm::BasicBlock* s : llvm::successors(bb)) { (void)s; n++; }
            if (n < 2) continue;
            std::unordered_set<llvm::BasicBlock*> seen;
            for (llvm::BasicBlock* s : llvm::successors(bb))
                if (rpo.count(s) && seen.insert(s).second) branchEdges.push_back({ bb, s });
        }

        std::unordered_map<llvm::BasicBlock*, CdSet> direct;
        for (llvm::BasicBlock* x : blocks)
        {
            CdSet cd;
            for (const CdEdge& e : branchEdges)
                if (postDominates(x, e.second) && !postDominates(x, e.first))
                    cd.insert(e);
            direct[x] = std::move(cd);
        }

        // Closure: an edge's own branch block carries its enclosing decisions too.
        std::unordered_map<llvm::BasicBlock*, CdSet> closure = direct;
        bool changed = true;
        while (changed)
        {
            changed = false;
            for (llvm::BasicBlock* x : blocks)
            {
                std::vector<llvm::BasicBlock*> branches;
                for (const CdEdge& e : closure[x]) branches.push_back(e.first);
                for (llvm::BasicBlock* b : branches)
                    for (const CdEdge& e : closure[b])
                        if (closure[x].insert(e).second) changed = true;
            }
        }
        return closure;
    }

    inline bool IsSubset(const CdSet& small, const CdSet& big)
    {
        if (small.size() > big.size()) return false;
        for (const CdEdge& e : small) if (!big.count(e)) return false;
        return true;
    }

    inline void ApplyEvent(NullState& s, const Event& e)
    {
        switch (e.kind)
        {
        case EventKind::SetNull:   s[e.name].insert(e.block); break;
        case EventKind::ClearNull:
        case EventKind::Read:      s.erase(e.name);           break;
        case EventKind::Escape:
        case EventKind::Deref:                                break;
        }
    }

    inline void MergeInto(NullState& into, const NullState& from)
    {
        for (const auto& [name, w] : from) into[name].insert(w.begin(), w.end());
    }

    // Forward union fixpoint carrying, per name, the set of move blocks still un-read and
    // un-revived on some path to here; then a replay that reports a dereference whose
    // control-dependence set is contained in some surviving witness's.
    inline std::vector<Divergence> AnalyzeFunction(llvm::Function* F, const std::vector<Event>& events,
                                                   const NoReturnSet* proven)
    {
        std::vector<Divergence> out;
        if (!F || F->isDeclaration() || F->empty()) return out;

        std::unordered_set<std::string> escaped;
        bool haveSet = false;
        for (const auto& e : events)
        {
            if (!e.block || e.block->getParent() != F) continue;
            if (e.kind == EventKind::Escape) escaped.insert(e.name);
            else if (e.kind == EventKind::SetNull) haveSet = true;
        }
        if (!haveSet) return out; // fast path: nothing in F was ever explicitly moved out

        std::unordered_map<llvm::BasicBlock*, std::vector<const Event*>> byBlock;
        for (const auto& e : events)
        {
            if (!e.block || e.block->getParent() != F) continue;
            if (e.kind == EventKind::Escape || escaped.count(e.name)) continue;
            byBlock[e.block].push_back(&e);
        }
        if (byBlock.empty()) return out;

        // Entry-reachable blocks only - an aborted scoped expect_error can leave stale,
        // unreachable blocks whose state must not leak into a live dereference.
        std::unordered_map<llvm::BasicBlock*, int> rpo = movedf::ComputeRpoIndex(F);
        std::vector<llvm::BasicBlock*> blocks;
        for (auto& BB : *F) if (rpo.count(&BB)) blocks.push_back(&BB);
        std::sort(blocks.begin(), blocks.end(),
                  [&](llvm::BasicBlock* a, llvm::BasicBlock* b) { return rpo[a] < rpo[b]; });

        auto cd = ComputeControlDependence(blocks, rpo, proven);

        std::unordered_map<llvm::BasicBlock*, NullState> blockOut;
        for (llvm::BasicBlock* bb : blocks) blockOut[bb];
        llvm::BasicBlock* entry = &F->getEntryBlock();

        bool changed = true;
        while (changed)
        {
            changed = false;
            for (llvm::BasicBlock* bb : blocks)
            {
                NullState in;
                if (bb != entry)
                    for (llvm::BasicBlock* pred : llvm::predecessors(bb))
                        if (rpo.count(pred)) MergeInto(in, blockOut[pred]);
                NullState result = std::move(in);
                if (auto it = byBlock.find(bb); it != byBlock.end())
                    for (const Event* e : it->second) ApplyEvent(result, *e);
                if (result != blockOut[bb]) { blockOut[bb] = std::move(result); changed = true; }
            }
        }

        std::unordered_set<std::string> seen;
        for (llvm::BasicBlock* bb : blocks)
        {
            auto it = byBlock.find(bb);
            if (it == byBlock.end()) continue;
            NullState running;
            if (bb != entry)
                for (llvm::BasicBlock* pred : llvm::predecessors(bb))
                    if (rpo.count(pred)) MergeInto(running, blockOut[pred]);
            for (const Event* e : it->second)
            {
                if (e->kind != EventKind::Deref) { ApplyEvent(running, *e); continue; }
                if (e->line <= 0) continue;
                auto wit = running.find(e->name);
                if (wit == running.end()) continue;
                bool guarded = true;
                for (llvm::BasicBlock* m : wit->second)
                    if (IsSubset(cd[bb], cd[m])) { guarded = false; break; }
                if (guarded) continue;
                if (seen.insert(e->name + ":" + std::to_string(e->line) + ":" +
                                std::to_string(e->col)).second)
                    out.push_back({ e->name, e->line, e->col });
            }
        }
        return out;
    }
}

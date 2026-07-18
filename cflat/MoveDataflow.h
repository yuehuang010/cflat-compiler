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
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CFG.h>       // llvm::predecessors
#include <llvm/IR/Function.h>

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
    using MovedSet = std::set<std::string>;

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
        std::set<llvm::BasicBlock*> visited;
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
        std::set<std::string> seen; // dedup identical path+location report keys
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

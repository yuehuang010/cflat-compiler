# C++ converting returns of a named local / param pick the const& ctor (C++20 moves); by-value C++ params take an extra move

**RULED 2026-10-21:** C++20 parity: return of a named local / by-value param tries the && overload first. See fix-issue-lessons.md digest.

Filed 2026-10-14 from the T177 Opus review (PRE-EXISTING on master; T177 carries item 1 into a newly accepted shape).
NEEDS A MAINTAINER RULING before build (item 1).

## 1. Implicit move of a named return source (RULING NEEDED)
C++20 treats `return local;` / `return param;` as an xvalue first, so a converting ctor taking `Src&&` is chosen. CFlat picks
the `const Src&` overload: `cm_local` 107 vs clang 207, `cm_param` 307 vs clang 207 (master too); `dstm_local` 6 vs clang 7
(master refused; accepted since T177). The 2026-09-04 ruling "return converts like copy-init" arguably endorses const&;
C++ parity (bridge transparency direction) argues for the xvalue-first rule.
- Option A (C++ parity): named local/param return source tried as an xvalue first, before ConvertThroughCxxConvertingCtor
  at MainListener_Statements.cpp ~1222 (sret) and ~1863 (direct). The local is moved-from; its dtor runs once.
- Option B (status quo): keep const&; `return move local;` is the spelling that selects the && ctor (already works, matches clang).
CFlat's "explicit move is the sanctioned transfer" (m10 ruling 4) leans toward B for CFlat-owned values, but these are C++ types.

## 2. By-value C++ param takes an extra move instead of eliding the prvalue (no ruling needed)
`dst_param`, `cm_param`: one extra move ctor + dtor vs clang (counts only exceed clang). When the move-like ctor is a template
the selected ctor differs: `cm_generic` 407 vs clang 207 (master refused).

Repros: scratch/repro_keep/t177_rv1/ (probe.sh, rv1.hpp, c/ with clang twins).

## 3. `return move f;` of a by-value C++ param (T178 Opus review, PRE-EXISTING)
`tk.Trk m(tk.Trk f) { return move f; }` called with `tk.Trk(5)`: cflat ctor/copy/move/dtor 2,1,2,5 vs clang 2,0,1,3 (same on
pre-T173). Same family as item 2 (by-value param materialization). Repro scratch/repro_keep/t178_rv1/.

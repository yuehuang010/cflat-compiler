# P2: Remaining const C++ receiver gaps

## Active gaps

- Virtual-base const/non-const twin resolution: member lookup still misses the twin, and the virtual-base layout is rejected before lookup.

## T225 disposition

Fixed: loop back-edge const-pointer dataflow now marks a local C++ record pointer maybe-const before loop receiver resolution when a loop assignment can supply a const pointee. Verified for `while`, `do/while`, classic `for`, conditional pointer joins, and const-pointer return expressions. The virtual-base item remains active because CFlat rejects virtual inheritance during layout import, before member lookup; lifting that layout refusal is separate work.

## W13 disposition

Fixed in `fix/cpp-const-receiver-leftovers` (2026-09-30):

- `const auto` copies of nontrivial C++ records now mark the slot on the `TryDeclareForeignCxxAutoLocal` path, so template members resolve against the const receiver.
- CFlat file-scope `const` C++ records and fixed arrays now carry const receiver state.
- Passing `const C++ record*` to a selected mutable C++ record-pointer parameter is rejected, including forwarding from a const-pointer parameter. C++ signature mapping now records const pointees on free and member parameters.

The W13 value legs and gate results are recorded in `scratch/briefs/w13_report.md` in the worktree. The loop back-edge and virtual-base items remain open.

## W6 round 2 disposition

Fixed in W6 (2026-09-30): elided `const auto` template call, static/instance `mix` through an object, shared receiver-kind routing for template calls, address-of a const slot / const array element, `const C*` parameter and struct field. The const-copy slot marker, file-scope const records and const-pointer argument conversion were resolved in W13. Probes are archived in the main checkout at `scratch/repro_keep/w6/`.

T225 review 1 (pre-existing, also straight-line): an implicit-this FIELD or a GLOBAL pointer assigned a
const C++ object (`p = &crConst; p->get();`) is never marked const (only locals are -
MainListener_Expressions.cpp ~4309 requires FieldName.empty()), so the mutable twin runs and writes
read-only storage (bus error 138; clang 3).

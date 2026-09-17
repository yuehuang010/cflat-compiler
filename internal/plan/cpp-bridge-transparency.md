# CFlat bridge: use-site transparency over C++ types

Parent plan: `internal/plan/cpp-direct-abi-interop.md` (M0-M10 built the mechanisms; this plan
is the contract they must add up to). Sibling: `internal/plan/cpp-struct-m10.md` (reverse
direction, a CFlat struct that IS a C++ class).
Status: DESIGN DRAFT 2026-09-17. Written from the maintainer's goal statement and the open
issue list, deliberately WITHOUT a code exploration pass. Every "today" claim below is from
issue files and landed history, not from a fresh audit; phase 0 is that audit.
Home for tests: `Test/test_cpp_interop_bridge.cb`.

## Goal (maintainer, 2026-09-17)

CFlat code that USES a type must not need to know whether the type is a CFlat type or a C++
type. `obj.method()` reads, type-checks, and behaves the same either way. The compiler carries
enough understanding of the C++ side, and enough hidden sugar, that the bridge never shows at a
use site.

Counterweight: CFlat is an improvement over the C++ legacy model. Where the two disagree,
safety and performance win - CFlat's rules apply, not C++'s. Transparency means "a C++ type
behaves like a CFlat type", never "CFlat code inherits C++ hazards".

## Principles

1. **The distinction lives at the declaration, never at the use.** `import cpp`, `[cpp] struct`,
   `override`, `--c-include` are the only places the word C++ appears. After that, expressions,
   statements, generics, and diagnostics treat the type as a type.
2. **One ownership model.** A C++ class with nontrivial special members is, to the CFlat
   analyses, an owning CFlat struct whose constructor / destructor / copy / move bodies happen
   to be foreign. Borrow by default; the compiler infers copy vs move; `move` is explicit and
   consumes; destruction is exactly-once. There is no second, C++-flavoured rule set.
3. **CFlat rules win on conflict.** No implicit narrowing at call arguments, no prvalue bound to
   a non-const reference, no implicit use of an `explicit` constructor, no implicit copy of a
   move-only type. These are CFlat rules applied uniformly, so they are not a transparency leak:
   the same spelling is refused on a CFlat type too.
4. **Never silently wrong.** Where the bridge cannot give the C++ type CFlat semantics yet, the
   compiler refuses with a diagnostic. A bitwise copy of a nontrivial C++ class is a p1 defect,
   not a supported degraded mode.
5. **Zero-cost sugar.** Sugar is resolved at compile time onto the direct ABI call. No runtime
   adapter objects, no boxing, no extra copy to cross the boundary. A wrapper emitted through
   clang is acceptable only where the ABI or a template instantiation needs one, and it must be
   inlinable.
6. **Protocol sugar, not type-name sugar.** The 2026-09-07 ruling stands: `std::` types are just
   types, no per-type CFlat mapping (`std::string` is not `string`). Sugar attaches to a SHAPE
   (has `begin()`/`end()`, has `operator[]`, has `operator bool`, has `operator->`), and a
   user's own C++ class gets it exactly as a libc++ class does.
7. **Symmetric.** Whatever a CFlat type can do at a use site, a C++ type can; whatever a C++
   type can do, a `[cpp] struct` written in CFlat can. Both directions share the one test matrix.

## What "transparent" covers - the use-site matrix

Status column is from landed history and open issues; phase 0 replaces it with measured rows.

| Use site | CFlat type | C++ type today | Gap |
|----------|-----------|----------------|-----|
| `obj.m()`, `p->m()`, static, field read/write | yes | yes | method call through a pointer MEMBER crashes (p2); std-specialization field still opaque (p3) |
| Namespaces, `using`, qualified names | yes | yes | namespace alias unbound (p3); using-directive shadowing (p2) |
| Construct: `T(args)`, `= default`, brace | yes | mostly | `T&&` ctor matches nothing (p2); alias template not ctor-callable (p3); chained-method ctor ambiguity (p2); explicit ctor used implicitly (p3) |
| Local lifetime, early exits, arrays, fields | yes | yes | file-scope objects never constructed/destroyed (p2) |
| **Copy / pass by value / return by value** | inferred | **bitwise in several paths** | HELD family: by-value into a CFlat function (p1), struct copy skips field copy ctor (p2), copy from field (p2), brace-init array element (p2), return by value uses synth copy (p2), std::function returned by value segfaults (p2) |
| `move` | yes | yes | - |
| Reference params (`T&`, `const T&`) | borrow | yes | literal to `const int&` free/member param (p3, p2 segfault on folded constant); non-const ref accepts a prvalue (p2); variadic/inheriting ctor `U&` (p3) |
| Operators incl. `[]`, `()`, `->`, conversions | yes | yes (parity ruling) | std::string free operator templates (p2); vector<bool> proxy (p3); iterator -> const_iterator (p2) |
| `for (x : c)` | yes | **no** | range-for over a C++ container (cppinterop bucket) |
| `?.` null-safe | yes | ruled (null check + smart forward) | verify on smart pointers |
| CFlat generic over a C++ type (`list<std.string>`) | yes | partial | audit: copy/move/destroy inside generic bodies ride on the held copy family |
| C++ template over a CFlat type | n/a | partial | template member not instantiated for a CFlat type (p2); `[cpp] struct` by value in a std container (p3) |
| CFlat `interface` satisfied by a C++ class | yes | **unknown** | design question below |
| Callbacks / `std::function` from CFlat callables | yes | yes (M7, M100) | loose free-parameter match (p2) |
| Exceptions | none in CFlat | `program` boundary only | unwind skips CFlat frame destructors (p2); M8 iteration 2 |
| Ownership handoff | `unique<T>` | raw | pointer deleted by C++ callee destroyed again at scope exit (p2); `unique_ptr` vs `unique<T>` open |
| Atomics, streams | own lib | **no** | std::atomic members unbound (p2); standard streams unusable (p2) |
| Diagnostics, hover, completion | yes | partial | relayed clang text not localized (p4); M9 tooling |

The matrix says it plainly: calls, members, operators, and lifetime are already transparent.
The bridge shows in three places - **value semantics** (copy), **protocol sugar** (iteration),
and **generic crossing** (templates over CFlat types). Those are the plan.

## Design

### D1. Value semantics - one copy/move/destroy contract (lifts the held family)

Every path in the compiler that duplicates, relocates, or drops a value asks ONE question of the
type - "how do you copy / move / destroy" - and the answer for a C++ class is its C++ special
member, for a CFlat struct its CFlat one, for a trivially copyable type a memcpy. No path may
memcpy first and ask later. Concretely the capability set on a type is:

- `trivially_copyable` -> bitwise is correct, nothing to call.
- `copyable` (C++ copy ctor / CFlat copy) -> the inferred copy calls it.
- `move_only` -> inferred copy is an error with the CFlat wording; `move` is required. Same
  diagnostic text as a `[unique]` CFlat struct.
- `immovable` (deleted copy and move, e.g. `std::mutex`, `std::atomic`) -> lives only where it
  was constructed; by-value use is an error; borrow is fine.

Because CFlat is borrow-by-default, most by-value C++ traffic disappears rather than turning
into copy-ctor calls: a by-value parameter of a CFlat function that is only read is a borrow of
the caller's object (performance win over C++, which would copy). A copy is materialized only
where the analyses already materialize one for an owning CFlat struct. This is the point where
CFlat is strictly better than the legacy model and the plan should measure it (exact
instrumented copy counts, `cppi.Tracked` style).

Sites to route through the contract (the held issues, one per site): CFlat function by-value
parameter, CFlat struct copy with a C++ field, read of a C++ field into an owning local, brace
initialized array element, by-value return from a CFlat function, `std::function` by-value
return. Needs the ruling in R1.

### D1a. Moved-from staleness - the C++ destructor on a moved struct/class

The one place the two models genuinely differ. A CFlat `move` is DESTRUCTIVE: the source is
consumed, it has no state left, and no destructor runs for it. A C++ move is NOT: the move
constructor leaves the source "valid but unspecified", and C++ still owes it a destructor call.
That stale source may hold real resources (a moved-from container may keep its buffer, a class
with a user move ctor may keep anything), so skipping its destructor on the CFlat "consumed"
rule is a leak, and running a CFlat-style bitwise relocate instead of the move ctor is wrong
for any self-referential class (SSO `std::string`, `std::function`, a node with a back pointer).

Contract for a type whose move is a C++ move constructor (imported class, or `[cpp] struct`
per M10 ruling 4):

- `move x` calls the C++ move constructor into the destination. Never a memcpy relocate, unless
  clang reports the type trivially relocatable / trivially copyable.
- The stale source is still destroyed exactly once by its C++ destructor. CFlat's compile-time
  "consumed" state governs what the USER may do with `x` (nothing; same diagnostic as a CFlat
  type), not whether the destructor is owed.
- Default: the stale source is destroyed at scope exit, as C++ does. Destroying it AT THE MOVE
  POINT instead (the analyses already prove `x` dead there, so a stale buffer or lock would not
  linger until the brace) is a potential OPTIMIZATION to explore later, not a goal of this plan;
  it needs the drop-flag handling for conditional moves and its own measurement.
- Explicit `move x` leaves `x` readable-as-null for CFlat types (existing ruling). For a C++
  class there is no null state to read, so a read of `x` after `move` is a compile error rather
  than a read of stale C++ state. A re-assignment `x = T(...)` is a C++ move-assignment onto
  the stale object (C++ semantics; the stale object is still alive until scope exit).
- Members: moving a CFlat struct that HAS a C++ field moves the field through its move ctor
  and owes the stale field its destructor, by the same rule, even though the enclosing CFlat
  struct is consumed. Moving a single C++ field out of a live struct leaves a stale field whose
  destructor runs with the struct's.
- Move sinks into C++ (`T&&` parameter, by-value parameter of a move-only type): the callee
  move-constructs from the argument, the caller's stale source follows the rule above.

Tests pin this with exact counts (`cppi.Tracked` style): one move ctor, one destructor for the
stale source, one for the destination, and a class whose moved-from state still owns an
allocation so a skipped stale destructor shows as a leak count, not as silence.

### D2. Lifetime completeness

Exactly-once construction and destruction everywhere a CFlat owning struct gets it: file-scope
objects (follow the global/static ruling: constructed at start-up, no exit-time destruction),
frames unwound by a C++ exception between the throw and `program` (M8 iteration 2), and
ownership taken by a C++ callee (a `delete`-ing callee or a sink parameter must consume the
CFlat owner; needs an annotation source - see R3).

### D3. Protocol sugar

Shape-driven, resolved at compile time, identical for CFlat and C++ types:

- **Iteration**: `for (x : c)` binds `begin()`/`end()` + `operator!=`, `operator++`, unary `*`.
  A CFlat type with the same members gets the same loop. `x` is a borrow of the element.
- **Indexing, call, arrow, bool, conversion**: already landed under the operator parity rulings;
  close the listed gaps (proxy results, iterator conversions, free operator templates).
- **Null-safe**: `?.` on anything with `operator bool` / comparison to `nullptr` + `operator->`.
- **Sized / contiguous**: `size()` + `data()` lets a C++ container decay to a `T[]` view where a
  CFlat API takes a view. Borrowed, not copied. Needs R4.

No sugar keyed on a type NAME. If `std::vector` iterates, so does any class with that shape.

### D4. Generic crossing

- CFlat generic over a C++ type: falls out of D1 once generic bodies use the contract.
- C++ template over a CFlat type: a plain CFlat struct used as a template argument needs a C++
  view of itself (layout + special members clang can call). Proposal: synthesize the same
  clang-side declaration `[cpp] struct` already produces, on demand, for trivially-copyable
  CFlat structs first; owning CFlat structs second (their copy/move/dtor exported as the C++
  special members). `std.vector<MyCflatStruct>` then needs no attribute. Needs R2.

### D5. Interfaces

A C++ class whose members satisfy a CFlat `interface` should box into it like a CFlat struct.
The VTable is CFlat's, built from thunks onto the C++ members; no relation to the C++ vtable.
Cheap once calls are uniform; listed last because nothing blocks on it.

### D6. Diagnostics and tooling

Errors name the type and member the way the user spelled them; clang text is relayed only when
the root cause is inside a C++ header. Hover, completion, go-to-definition over a C++ type show
the same shape as over a CFlat type (M9). `[cpp]`-ness is visible on hover as a fact, never
required to understand an error.

## Where CFlat stays visibly different from C++ (on purpose)

Not leaks - these are the improvement, and they apply to CFlat types equally:

- Borrow by default; copies are inferred and rare, never requested by a `T` vs `const T&`
  spelling choice.
- `move` is explicit and consuming; a moved-from object is not usable. The stale C++ source
  still gets its destructor (D1a) - the user never sees it.
- No implicit narrowing at call arguments; `explicit` means explicit.
- No try/catch. `program` is the only catch boundary.
- No prefix `++`; C++ prefix `operator++` binds to CFlat postfix.
- Raw borrowed-pointer lifetimes are untracked (safety is at function boundaries).

## Rulings needed

- **R1 (blocks D1).** Confirm "C++ class = owning CFlat struct with foreign special members" as
  the single rule for the held copy family, including: read-only by-value parameter is a borrow
  (no copy ctor call), and copy-from-field is a COPY when the type is copyable and an ERROR
  (needs `move`) when it is move-only.
- **R2 (blocks D4).** May a plain CFlat struct be used as a C++ template argument without
  `[cpp]`, by synthesizing its C++ view on demand?
- **R3 (D2).** Where does "this C++ callee takes ownership" come from: `unique_ptr`/`T&&`
  signatures only (safe, incomplete), or also a CFlat-side annotation at the import?
- **R4 (D3).** Container -> `T[]` view decay: implicit at a call, or an explicit spelling?
- **R5.** `std::unique_ptr<T>` vs `unique<T>` (open since M10): under this plan they stay two
  types that both obey D1; confirm no mapping.
- **R6.** Exceptions stay as ruled 2026-09-16 (no try/catch). Confirm that a throwing C++ call
  is NOT a transparency leak to be hidden further.

- **R7 (D1a).** Confirm a read of a moved-from C++ object is an error, not "valid but
  unspecified". (Destroy-at-move-point is an optimization candidate, not up for ruling here.)

## Phases

0. **Audit as a test.** In `Test/test_cpp_interop_bridge.cb`, a "twin" harness: one CFlat
   generic function body per use-site row, instantiated over a CFlat struct and over its C++
   twin from an in-repo header, with instrumented counts. A row that needs two different source
   texts, or whose counts differ, is a leak. Rows that fail today are written as issue files,
   not as disabled tests. Output: the matrix above with measured status.
1. **D1 value semantics** after R1. One site per change, each pinned by exact copy/move/dtor
   counts and an `err_` test for the move-only refusal. Includes the `--init` serializer rule
   for any new capability field.
2. **D2 lifetime completeness**: globals, unwind cleanup (M8 iteration 2), callee-owned pointers.
3. **D3 protocol sugar**: range-for first, then the operator gaps, then view decay after R4.
4. **D4 generic crossing** after R2.
5. **D5 interfaces, D6 tooling** (D6 overlaps M9).

Each phase lands through the existing fix-issue flow (isolated worktree, review until clean,
single-parent commit, host suite green). No libtorch in tests; in-repo headers only.

## Acceptance

- The twin harness compiles the SAME CFlat source over the CFlat type and the C++ type for
  every matrix row, with identical observable behaviour and copy counts no higher than C++'s.
- No open issue in the "bitwise copy of a nontrivial class" family.
- A libtorch-scale validation program (outside `Test/`) contains no bridge-motivated spelling:
  no manual copy-ctor call, no index loop where range-for would do, no `[cpp]` on a struct
  only to put it in a container.

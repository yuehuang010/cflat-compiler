# `move`ing an owning local into `list<unique T*>.add` is rejected inside a struct method

Found while dogfooding a JSON parser (`scratch/dogfood/lang/json.cb`). The identical
function body compiles at file scope and is rejected verbatim when it is a struct member
function. The diagnostic also names a heap ARRAY when the local is a scalar pointer.

## Repro A - method vs free function (the false rejection)

Identical bodies. `repro_d` compiles; `repro_c` does not.

```cflat
// REJECTED
import "list.cb";
struct Node { int v = default; };
move Node* make() { return new Node(); }
struct Maker
{
    void build()
    {
        list<unique Node*> l = default;
        unique Node* n = make();
        l.add(move n);
    }
};
extern int main() { Maker m = default; m.build(); return 0; }
```

```
repro_c.cb(10,8): cannot pass the owning heap array 'n' to parameter 'value' of 'add':
unique<T> cannot own arrays.
```

```cflat
// ACCEPTED - same body, free function
void build()
{
    list<unique Node*> l = default;
    unique Node* n = make();
    l.add(move n);
}
```

The receiver does not matter: a `list<unique Node*>` FIELD reached through `p->kids`
behaves the same (accepted from a free function, rejected from a method).

## Repro B - wrong wording for a scalar pointer local

At file scope, with a raw (not `unique`) local holding the result of a move-returning
call, the same message appears:

```cflat
extern int main()
{
    list<unique Node*> l = default;
    Node* n = make();      // `make` returns `move Node*`, so `n` owns the object
    l.add(move n);         // cannot pass the owning heap array 'n' ... unique<T> cannot own arrays
    return 0;
}
```

`n` is a single object, never `new T[count]`. Declaring the local `unique Node*` makes
this form compile, so the message should say that ("hold the result in a `unique T*`
local"), not talk about arrays.

## Root cause (hypothesis, not verified)

The "owning heap array" check keys off a pointer local that is known-owning but whose
declared slot is not a `unique<T>` wrapper, and treats "owning raw pointer" as
"counted heap array". Inside a struct member function the owning/unique provenance of the
local appears to be lost (or the member-function `this` frame is not consulted), so even a
`unique T*` local falls into the same bucket.

## Impact

Any recursive owning tree built by a parser-style struct (`JParser::parseArray` ->
`v->items.add(move child)`) cannot use `list<unique T*>` at all. The workaround is to give
up compiler-managed ownership: a plain `list<T*>` plus a hand-written recursive destructor.

## Fix direction

1. Find the check emitting "cannot pass the owning heap array ... unique<T> cannot own
   arrays" and split it: a local whose declared type is `unique T*` (or whose initializer
   is a move-returning call of a scalar `T*`) is NOT an array.
2. Make the owning/unique provenance of a local visible inside member-function bodies so
   Repro A and `repro_d` agree.
3. Regression: extend `Test/test_list_ownership.cb` with the method-scope shape.

Confirmed independently 2026-09-16 on master 24b86c32: repro_c rejected with the array wording, repro_d accepted.

FIXED 2026-09-16 in worktree /Users/felixhuang/source/cflat-move (branch wip/move, uncommitted, host-verified): IsFunctionBodyDeclaration returned false on the enclosing AggregateMember before noticing the FunctionDefinition, so method-body locals skipped the unique<T> desugar; scalar wording split from the array wording. Rows in Test/test_move.cb. Delete this file when the branch lands.

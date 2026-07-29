# `as` boxing into an interface skips the ownership transfer -> double free

Filed 2026-07-28 by an adversarial review of the stack-value `as` fix.
PRE-EXISTING on the concrete-POINTER branch of `GenerateSafeCast`; not introduced by that fix.

Severity: double free (asan-confirmed), silent in a plain build.

## Repro

```cflat
interface IShape { int area(); };
class Circle : IShape { int r = 0; int area() { return r * r; } ~Circle() {} };

extern int main()
{
    Circle* p = new Circle(); p.r = 3;
    IShape s = p as IShape;    // plain `IShape s = p;` nulls p and marks it moved
    printf("area=%d\n", s.area());
    delete s;                  // now who owns it?
    printf("done\n");
    return 0;
}
```

Built with `--asan`, this prints `area=9`, `done`, then:

```
==ERROR: AddressSanitizer: attempting double-free on 0x... in thread T0
```

The same program with the plain spelling `IShape s = p;` is asan-clean, which isolates the
`as` cast as the difference.

## Second, worse manifestation: `move IShape f() { ... return p as IShape; }`

Found while spot-checking the return path. Same root cause, but the use-after-free happens
inside the callee rather than as a double free at the call site.

```cflat
interface IShape { int area(); };
class Square : IShape { int s = 0; int area() { return s * s; } };

move IShape makeHeap(int s)
{
    Square* sq = new Square();
    sq.s = s;
    return sq as IShape;    // plain `return sq;` is correct
}

extern int main()
{
    IShape a = makeHeap(7);
    printf("heap=%d\n", a.area());   // prints garbage
    delete a;
    return 0;
}
```

The native exe prints `heap=-1888966400` and exits 127; the plain spelling `return sq;`
prints `heap=49` and exits 0. The emitted callee is unambiguous - it frees the object and
then returns a fat pointer into the freed block:

```llvm
move.cleanup:
  call void @Square.dtordeferred(ptr nonnull %0)
  call void @"_operator delete_void_U8Ptr_"(ptr nonnull %0)
  br label %move.after
move.after:
  %3 = insertvalue %__iface_fat_ptr { ptr @Square_IShape_vtable, ptr undef }, ptr %0, 1
  ret %__iface_fat_ptr %3
```

## Third manifestation: the non-`move` return, which bypasses a THIRD guard

This is the more common spelling of the one above, and it trips a different guard again -
the one that rejects handing a heap object to a caller that has no way to know it owns it.

```cflat
IShape f(int n) { Square* sq = new Square(); sq.s = n; return sq as IShape; }   // accepted
IShape f(int n) { Square* sq = new Square(); sq.s = n; return sq; }             // hard error
```

The plain spelling errors with:

```
returning a heap object boxed into interface 'IShape' from a non-'move' function transfers
ownership the caller cannot see - it will leak. Declare the return type 'move IShape' so the
caller knows to 'delete' it.
```

The `as` spelling is accepted, and it is not even the leak that message describes: under
`--asan` it is a heap-use-after-free (READ of size 4 on a freed 4-byte region), because the
callee still runs its own scope-exit free on `sq`.

Whoever picks this up: fixing the `move` case above does NOT fix this one, and vice versa.
They are three guards (assignment-path move-marking, return-path `interfaceReturnStructName`,
non-`move` ownership-escape rejection) that the `as` path skips for one shared reason.

## Fourth manifestation: a `?:` join frees EVERY arm

Found 2026-07-28 during the round-3 review. A `?:` joining two heap-boxed arms is
accepted by the frame-lifetime check (correctly - no arm is frame storage), but the
lowering emits a `move.cleanup` block per arm, so BOTH heap objects are freed before
the shared `insertvalue` runs:

```cflat
IShape pick(int c)
{
    Square* x = new Square(); x.s = 3;
    Square* y = new Square(); y.s = 3;
    return c > 0 ? (x as IShape) : (y as IShape);
}
```

asan reports a heap-use-after-free on the surviving arm. This is strictly worse than
the single-object repro above: the count of leaked/freed objects scales with the
number of arms, and the arm NOT taken is freed too. Confirmed by running, and visible
in the emitted IR as one `move.cleanup` per arm ahead of the join.

## Root cause

The declaration/assignment path transfers ownership when it boxes an owning pointer into an
interface: `MainListener.h:8175-8189` stores null back into the source local, calls
`MarkVariableMoved` and `MarkVariableMovedIntoInterface`, so the source's scope-exit free
cannot run against the object the interface box now owns.

The return path does the equivalent by handing `CreateReturnCall` a non-empty
`interfaceReturnStructName` (`MainListener.h:5744`), which suppresses the owning local's
scope-exit free because ownership moves to the caller.

`GenerateSafeCast` triggers neither. It boxes and returns a ready-made fat value, so the
assignment path's transfer never runs (`p` keeps its owning flag; `delete s` plus `p`'s
scope exit is the double free) and the return path's boxing block is skipped entirely
(the operand is already a fat pointer, so `interfaceReturnStructName` stays empty and the
callee frees what it just handed out).

## Exposure in-tree

None as of filing. Every `as <Interface>` site in `cflat/core/` and `example/` casts from an
interface-typed value (`IView v = node as IView;` and friends), which is the runtime-checked
interface-to-interface path and carries no ownership. The gap only opens when the operand is
a concrete OWNING pointer, which nothing in the tree does today - so this is a latent trap for
users, not a live breakage. `example.bat` is green.

## Fix direction

The narrow fix is to perform the same nulling + move-marking in the concrete branch of
`GenerateSafeCast`. That needs the source `NamedVariable` (its `Storage`, `IsOwning` and
`CallerName`), which `ParseTypeCheckExpression` currently discards - it only threads the
loaded value and its elemType down. So the operand plumbing has to carry the NamedVariable
before the transfer can be written.

## The structural argument (stated here once, referenced from the sibling issues)

The durable fix is structural, and is the real recommendation: **class-to-interface boxing
bookkeeping is duplicated across four places that do not agree** -

1. the assignment/declaration path (`MainListener.h:8149-8193`),
2. the return path (`MainListener.h:5697+`),
3. `GenerateSafeCast` (`MainListener.h:12130+`), and
4. the frame-lifetime check added alongside the return path (`FrameLocalDataOfFatValue`,
   `MainListener.h:5066`), which recovers by walking emitted IR precisely because the boxing
   site did not record what it boxed.

Each carries its own subset of: implements check, pointer-shape rejection, data-pointer
selection, ownership transfer, non-`move` ownership-escape rejection, frame-lifetime check.
Shapes keep falling through the gaps between them - every issue filed alongside this one is
an instance, and item 4 above is a fourth copy being added rather than the duplication being
removed. Consolidating into one boxing helper that records the provenance of what it boxed
(frame storage / heap / parameter / global) and applies every guard once is the answer;
adding a fifth copy is not.

## Related

- [[as-boxing-skips-pointer-shape-rejection]] - the same gap, different guard.
- [[as-cast-array-shaped-source-no-diagnostic]] - an operand shape none of the paths cover.
- [[interface-return-dangle-defeated-by-intermediate-local]] - the frame-lifetime check's own
  blind spot, and why provenance-at-the-boxing-site would close it by construction.

# `(new T(x))?.f` never frees the temporary

Bucket: p3 (bounded leak, never an early free). Found beside the `??`-in-`?:` arm fix.

## Repro

`bv((new T(1))?.v);`, `int x = (new T(10))?.v;`, `bt((new T(7))?.next ?? nt);` - one new,
zero dtors, every context (call argument or not, inside a `?:` arm or not). The `->` form
`bv((new T(2))->v)` frees once.

## Root cause (suspected)

The `?.` lowering branches on the receiver and joins; the `new` temp is never ledgered as an
owned pointer temp for the full-expression cleanup the way `->` on a `new` receiver is.

## Fix direction

Ledger the `?.` receiver `new` temp before the null branch (it runs on both paths), same as a
bare-`new` `??` LHS in a call argument (MainListener_Expressions.cpp `??` lowering).

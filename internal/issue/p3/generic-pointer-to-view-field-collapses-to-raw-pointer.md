# `T*` in a user generic instantiated with `T = int[]` silently becomes a raw `int*`

## Summary

`struct P<T> { T* p = default; }` with `P<int[]>` compiles on master 029f1bb: the field that
should be a (forbidden) pointer-to-array-view is typed as a plain `int*`, with no diagnostic. The
user then sees a confusing downstream error (`holder.p = &data;` -> "cannot bind a raw pointer
'T*' to an array-view 'T[]'", probe scratch/q02m3_ptrfield.cb) or, if they only ever store raw
pointers there, a program that runs with a different type than they wrote. Core templates fail
loudly instead (they spell `sizeof(T)` / `(T*)` casts, which the q02 member-3 fix now reports at
the argument site); a template that merely declares a `T*` field or parameter never reaches that
failure.

## Root cause

Substituting a view spelling into a pointer declarator goes through the same decay that
`int[] -> int*` legitimately takes at call boundaries, so the declared type is silently rewritten
instead of rejected. Located during the member-3 round-1 review (2026-09-03); the pre-scan that
tried to catch it by scanning template tokens was withdrawn (false rejects on `T * n` products
and dead `if const` arms).

## Fix direction (needs a ruling on the rule first)

Ruling: is `T*` with `T` an array view an ERROR at instantiation (consistent with
doc/LANGUAGE.md ~461 "pointer-to-array-view is not a valid type"), or should it decay to the
element pointer by design? If error: in both ParseDeclarationSpecifiers copies, when the resolved
base type is a view (`IsArrayView`) and the declarator adds `*`, reject with the member-3 message
("'int[]' cannot be a type argument of 'P': instantiating the body forms 'int[]*', which is not a
valid type") re-attributed to the instantiation origin (the mechanism landed in member 3:
gts.activeInstantiationOrigin). Legs in Test/errors/err_generic_array_view_arg.cb.

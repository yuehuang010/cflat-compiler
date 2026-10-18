# p3: move of a C++ unique field into another unique field is refused as "source must be a variable"

Needs a ruling: is a field path a valid source for a C++ class move-assignment (`h.w = move g.w;`)?
The CFlat twin accepts `t.p = move s.p;` (Test/errors/err_unique_field_consume_read.cb legs).

## Repro (master tip, 2026-10-16)
```cflat
import cpp "library/cpp_interop_uniqueptr.h";
struct H { unique cpuq.Widget* w = default; };
extern int main() { H h = default; H g = default; g.w = cpuq.make(3); h.w = move g.w; return cpuq.peek(h.w); }
```
Error: `cannot assign to C++ class 'std.unique_ptr<cpuq.Widget>' from this expression; the source must be a
'std.unique_ptr<cpuq.Widget>' variable, optionally written 'move <variable>'`.

## Root cause (not traced)
The source check in the foreign-class assignment path (cflat/MainListener_Expressions.cpp, the
`IsBareIdentifierText(srcName)` gates around 3315-3340 and the trivial-move path ~3626) only accepts a bare
identifier source. A field path source is therefore never consumed as a move.

## Fix direction
After the ruling: accept a field path source the way the CFlat `=` tail does (consume the source field path,
then revive the destination via the T292 revive at the foreign-class store).

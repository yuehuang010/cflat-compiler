# An assignment used as a value drops the stack-buffer borrow

## Summary

The value of an assignment expression does not carry the stack-buffer borrow of its right-hand side, so
`return v = string_view(buf, 2);` returns a dangling view without the "cannot store a string borrowed from stack
buffer" refusal that `return string_view(buf, 2);` gets (after the stack-borrow gate landing, 2026-11-03).

## Minimal repro

```cflat
string_view mk() { char[8] buf = default; buf[0] = 'h'; buf[1] = 'i'; string_view v = default; return v = string_view(buf, 2); }
extern int main() { string_view s = mk(); printf("%d\n", (int)s.length()); return 0; }
```

## Observed vs expected

Observed (master 6a8ea8b4): compiles, prints 2 (reads a dead frame). Expected: refused with the stack-buffer diagnostic,
the same as `return string_view(buf, 2);`. Same for `string` (`return s = (string)buf;`) and chained `a = b = ...`.

## Fix direction

The assignment expression's result value must carry the right-hand side's StackCharBufferBorrow / Source / ScopeDepth
(the destination's provenance after the store), so the existing escape sinks see it. Found by the strretgate review
(box 10, 2026-11-03).

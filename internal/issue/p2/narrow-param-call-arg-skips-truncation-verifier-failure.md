# Narrow parameter call argument skips truncation - module verification failure

## Summary

Overload resolution accepts a wider integer argument for an `i8`/`u8`/`i16`/`u16` parameter, but the
call-argument conversion never narrows the value, so the IR passes an `i16`/`i32` to an `i8` slot and
the module verifier rejects the whole program with no `LogError`.

## Repro (master cb3f71b, measured 2026-09-03)

```
int takeU8(u8 v) { return (int)v; }
extern int main() { u8 a = 200; return takeU8(a * 1000); }
```

`a * 1000` is a narrow binary result of width 16 (LANGUAGE.md narrow-back rule), overload resolution
matches `takeU8(u8)`, codegen emits `call @takeU8(i16 %3)` and the verifier fails:
"Call parameter type does not match function signature!".

By contrast an `int` argument (`takeU8(-w)` after 5691668 promotes unary results to `i32`, or
`takeU8(anyInt)` on master) is rejected cleanly by overload resolution: "no overload of 'takeU8'
matches the given arguments." The two paths disagree about whether implicit integer narrowing at a
call is legal.

## Root cause

Overload scoring treats the sub-int widths as mutually convertible but the argument-materialisation
step only converts on a scored conversion it recognises; a narrow-to-narrower integer step has no
conversion emitted. Locate: overload resolution in LLVMBackend functionTable scoring, argument
conversion in MainListener_PostfixExpression.cpp call emission.

## Fix direction

Ruling needed: EITHER implicit narrowing at calls is legal (then emit the truncation like assignment
does, for i32 and for sub-int sources alike, and `takeU8(-w)` compiles again), OR it is illegal (then
overload resolution must reject the sub-int-to-narrower case too, with the existing "no overload"
diagnostic). Either way the verifier failure must become a `LogError` or a conversion. Related: the
return-path sibling is being fixed in the narrow-promotion branch (return int into narrow return type
converts like assignment).

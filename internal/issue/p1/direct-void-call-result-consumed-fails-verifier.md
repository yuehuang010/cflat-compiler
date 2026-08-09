# Consuming a DIRECT void call's result dies in the module verifier (or aborts the compiler)

Filed 2026-08-09 during the review of `fix/voidcall`
([[void-closure-call-result-consumed-reads-garbage]], now fixed). That fix gated the CLOSURE
call path: `int r = g();` on a `Lambda<void()>` is now a located diagnostic. The DIRECT
spelling - `int r = f();` on a plain `void f()`, and the same through a class method or an
interface method - was measured pre and post and is **byte-identical on both**, so it is
pre-existing and untouched.

Severity: **P1**. An LLVM verifier failure reachable from plain source is P1 by the standing
rule in `internal/fix-issue-lessons.md`, and one spelling (`auto r = f();`) does not even reach
the verifier - it aborts the compiler with rc 133 and **no output at all**. Calling a void
function and binding its result is one of the most ordinary slips a C programmer makes, so the
reachability is as high as it gets.

## Repro

Measured Release, macOS arm64, warm `--init-local`, on the merge base `75b4275`
(`/Users/felixhuang/source/cflat-compiler/x64/Release/cflat`) and on `fix/voidcall` -
identical on both. Corpus: `scratch/rev_d1_decl.cb` ... `scratch/rev_d10_method.cb`,
`scratch/rev_s03_iface_void_method.cb`.

```cflat
int h = 0;
void bump() { h = h + 1; }
extern int main() { int r = bump(); return r; }
```

| spelling | file | result (BOTH binaries) |
|---|---|---|
| `int r = bump();` | rev_d1_decl | `Invalid bitcast / %0 = bitcast void <badref> to i32`, rc 1, NO location |
| `int r = bump() + 1;` | rev_d2_binop | `Both operands to a binary operator are not of the same type! / add void <badref>, i8 1`, rc 1, NO location |
| `auto r = bump();` | rev_d3_auto | **rc 133, no output whatsoever** |
| `take(bump());` | rev_d4_arg | located `no overload of 'take' matches ... [0] arg=void param=int` - misleading but survivable |
| `r = bump();` | rev_d5_assign | `Invalid bitcast`, rc 1, NO location |
| `if (bump())` | rev_d6_cond | located `condition must be a scalar ... not 'value'` - survivable |
| `s.n = bump();` | rev_d7_field | `Invalid bitcast`, rc 1, NO location |
| `printf("%d\n", bump());` | rev_d8_printf | `Instruction operands must be first-class values! / call void (ptr, ...) @printf(ptr @27, void <badref>)`, rc 1, NO location |
| `h == 0 ? bump() : 5` | rev_d9_ternary | located `ternary branches have incompatible types 'value' and 'i8'` - survivable |
| `int r = c.bump();` (class method) | rev_d10_method | `Invalid bitcast`, rc 1, NO location |
| `int r = i.bump();` (INTERFACE method through the vtable) | rev_s03_iface_void_method | `module verification failed`, rc 1, NO location |

So six of eleven positions give a locationless verifier dump, one aborts the compiler, and only
three produce a diagnostic that names a line.

**The WinRT vtable dispatch is a third un-gated door**, unmeasurable on macOS. `LLVMBackend_WinRT.cpp:266`
calls `CreateIndirectCall` and its `if (!nonVoidIface)` arm (line 268) explicitly returns the raw
`callRes` for an "infra method or **void interface method**" - which is `nullptr` for a void slot,
exactly the shape `fix/voidcall` gates in the listener. Unchanged by that fix in either direction,
but it is not "can never produce a user-consumable void result": a winmd method whose slot type is
`void` consumed in a value context lands here, not at the gated site.

## Root cause

A direct void call *does* produce an `llvm::Value` - the `CallInst` itself, of LLVM type `void`
(that is the `void <badref>` in every dump). It is handed back as an ordinary result and stored
into the declarator / operand / argument, and nothing on the way refuses it. This is the exact
mirror of the closure defect, with the polarity of the underlying value flipped: the closure
path produced `nullptr` and read garbage; the direct path produces a void-typed value and hands
it to the verifier.

The reason the closure fix does not reach here is structural, and is the whole cost of this
issue: the closure result `NamedVariable` is built at ONE site
(`MainListener_PostfixExpression.cpp`, the sole listener `CreateIndirectCall` caller), whereas
the direct-call result is built at roughly a dozen `lastCallReturnType` sites across the direct
call, method call, interface-vtable dispatch and overload-resolution paths.

## Fix direction

Do NOT enumerate consumers - that is the site-enumeration failure mode this repo has paid for
repeatedly, and it is exactly what the closure fix avoided. Funnel the direct-call result
construction through one helper (the `lastCallReturnType` sites already all read the same
state), and reject there on the same rule the closure gate uses: the callee's return type is
`void`, it is not a pointer, and the `ResultUse` handed down is `Value`.

That means the `ResultUse` threading introduced by `fix/voidcall`
(`MainListener.h`, `ResultUse { Value, Discard, ReturnOperand }`) is a prerequisite and is
already in place - `ReturnOperand` must keep deferring to `EmitReturnExpression`, which already
diagnoses a direct void call in return position
([[return-value-void-mismatch-fails-module-verification]]).

Note the discard-threading holes recorded in
[[discard-position-not-threaded-through-parens-and-ternary]]: whatever gate lands here inherits
them, so the two should converge together rather than each growing its own false rejections.

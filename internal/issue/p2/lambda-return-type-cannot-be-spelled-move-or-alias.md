# A lambda's return type cannot be spelled `move T*` / `alias T*`, so the fresh-allocation diagnostic's remedy is invalid there

Filed 2026-08-09 by the review of `fix/lamtemp`, while building the accept set for the
expression-body lambda's newly-active return gates. Measured on master `0669ebc` and on
`fix/lamtemp` (Release, macOS arm64, warm `--init-local`).

Severity: **P2, diagnostic remedy that does not compile** - the rejection itself is correct and
agrees with the free-function and block-body twins; what is missing is any way to follow the
advice it prints at this destination.

## Repro

```cflat
class C { int v = default; };
extern int main()
{
    Lambda<C*()> f = () => new C();
    C* p = f();
    p->v = 14;
    printf("%d\n", p->v);
    delete p;
    return 0;
}
```

On `fix/lamtemp`:

```
(2,43): returning a fresh allocation from a function whose return type is the bare pointer 'C*'
gives the caller no signal that it owns the result - a forgotten 'delete' is a silent leak.
Declare the return type 'move C*' to transfer ownership to the caller (which adopts it with
'unique C* x = f();'), or 'alias C*' to opt out of ownership tracking and manage the lifetime
by hand.
```

Both named remedies are unspellable, in either closure type:

| spelling                          | result                                                       |
|-----------------------------------|--------------------------------------------------------------|
| `Lambda<move C*()>`               | `error: extraneous input '<' expecting {'move', '(', Identifier}` |
| `Lambda<alias C*()>`              | same parse error                                              |
| `function<move C*()>`             | same parse error                                              |
| `function<alias C*()>`            | same parse error                                              |
| `Lambda<unique C*()>`             | same parse error                                              |

The only working remedy is to move the allocation into a NAMED function whose return type can
carry the qualifier, and call that from the lambda - verified compiling and running (`14`,
rc 0) on both binaries:

```cflat
move C* mkC() { return new C(); }
...
Lambda<C*()> f = () => mkC();
```

## Population

Pre-existing for the BLOCK body (`() => { return new C(); }` is rejected identically on master),
so this is not introduced by `fix/lamtemp`. That commit widens the population by one spelling:
`Lambda<C*()> f = () => new C();` and `function<C*()> f = () => new C();` compiled and ran
correctly on master (`14`, rc 0, no leak because the caller `delete`s) and are hard errors now.
That is the intended convergence - the three spellings answer alike - but it is the largest
behaviour change in that commit, and this file is where the remedy gap it exposes is recorded.

## Fix direction

Two independent halves; the first is cheap and the second is the real one.

1. **The message.** At a lambda / `function<>` destination the two named remedies are invalid
   syntax. Per the standing rule ("compile the remedy before shipping the message, per
   destination spelling"), the reject site should detect that the enclosing function is a lambda
   invoker and print the remedy that does compile - allocate in a named `move T*` function and
   call it - rather than advice the grammar refuses.
2. **The grammar.** `functionPointerSpecifier` accepts no ownership qualifier on the return
   type, so a closure can never transfer ownership of a fresh allocation to its caller. Whether
   that is worth implementing is a language decision; the enumeration to do first is which of
   `move` / `alias` / `unique` the closure ABI can actually honour, since the invoker is
   synthesized.

Related: [[nodiscard-residual-gaps]], [[void-expression-body-lambda-fails-module-verification]].

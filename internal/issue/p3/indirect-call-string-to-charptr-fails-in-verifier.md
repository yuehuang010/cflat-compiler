# A `string` passed to a `char*` parameter through a function POINTER dies in the module verifier

Filed 2026-08-06 by `fix/brace-literal`'s per-site audit of the string-into-`char*` lowering.

Severity: diagnostic quality. The program is rejected (rc 1), so nothing miscompiles; the message is
`Module verification failed:` with no source location, which is the worst possible wording for a
plain type error. Measured identical on `56ebc52` and on `fix/brace-literal`, so not a regression.

## Repro

```cflat
int f(char* p){ return 1; }
extern int main(){ function<int(char*)> fp = f; string s = "z"; return fp(s); }
```

```
Module verification failed:
```

## Root cause

The direct-call path lowers arguments in `CreateOverloadedFunctionCall`
(`cflat/LLVMBackend_Overloads.cpp`), where `fix/brace-literal` added the diagnostic: a `string`
value bound to a `char*` parameter is rejected with a located message. The INDIRECT paths
(`cflat/LLVMBackend_ControlFlowAndFunctions.cpp`, the C-function-pointer and closure-invoker arms)
do not go through it - they call `Upconvert` and then `CheckIndirectCallArgShape`, neither of which
knows about the `{ptr,len}` -> `char*` case, so the mismatched argument reaches `CreateCall` and the
verifier catches it.

## Fix direction

Teach `CheckIndirectCallArgShape` the same representation-keyed test the direct path uses: the
argument's LLVM type is the named `string` struct and the declared parameter is `char*`/`i8*`. Reuse
the direct path's wording so both spellings of the same mistake read the same.

## Test coverage

None. `Test/errors/err_string_vararg.cb` covers the two DIRECT spellings (fixed `char*` parameter and
the variadic slot); the indirect one would be a third leg there once it has a real diagnostic.

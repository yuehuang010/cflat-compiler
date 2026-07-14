# `delete` on a value-type (non-pointer) local emits invalid IR instead of an error

Status: OPEN. Found 2026-07-14 (incidental, while writing a repro for the Lambda-field
owned-string leak).

## Summary

`delete x` where `x` is a value-type local (a `list<T>` / `dictionary<K,V>` / any struct
VALUE, not a pointer) is accepted by the front end and lowered as if `x` were a pointer.
Module verification then fails with three errors and the compile aborts with no source
location - the user sees LLVM-level noise instead of a diagnostic.

Value-type locals are auto-destructed at scope exit, so `delete` on one is always a user
error; it should be rejected with a message that says so.

## Repro

```cflat
import "string.cb";
import "list.cb";

extern int main()
{
    list<string> l = default;
    l.add("a");
    delete l;          // value type - auto-destructed at scope exit
    return 0;
}
```

Output:

```
Module verification failed:
Invalid operand types for ICmp instruction
  %del_isnull = icmp eq %list__string %4, null
Call parameter type does not match function signature!
  %4 = load %list__string, ptr %l, align 8
 ptr  call void @"_~list__string_void_list__stringPtr_"(%list__string %4)
Invalid bitcast
  %freeptr = bitcast %list__string %4 to ptr
```

## Root cause

The delete path loads the operand and emits the null check / destructor call / free without
first requiring the operand's type to be a pointer, so a struct VALUE flows into
instructions that demand a pointer (`icmp ... null`, the `T*` destructor parameter, and the
bitcast to `i8*` for `operator delete`).

## Fix direction

In `ParseDeleteExpression`, reject a non-pointer operand up front with `LogErrorContext`
(e.g. "cannot 'delete' value-type local 'l' of type 'list<string>' - value types are
destructed automatically at scope exit; 'delete' applies to pointers from 'new'"). This is
also the CLAUDE.md rule for LLVM asserts/verification failures: diagnose in the compiler
rather than letting invalid IR reach LLVM.

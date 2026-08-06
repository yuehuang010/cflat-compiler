# A closure stored BY VALUE into a generic struct field is rejected / fails the verifier

Filed 2026-08-05 out of the Phase A coverage matrix for
`lambda-pointer-as-generic-type-arg-bypasses-guard` (landed as the fat-reject / thin-support
asymmetry - see the design record in `interface-issue-queue.md`). Both sub-cases below were
measured IDENTICAL on the pre-fix binary (master `4c06cce`) and on the post-fix binary, so this
is a neighbouring gap, not a regression, and the pointer fix does not touch it.

The pointer spelling `Box<function<T>*>` now works; these are the BY-VALUE spellings, which have
a different root (the encoded closure's backing value type), and they are the reason the
"by-value must keep working" accept-set could only be frozen for `Lambda<T>`, not `function<T>`.

## Sub-case 1 - thin `function<T>` by value into a generic field: hard error

```cflat
import "function.cb";
int dbl(int x) { return x * 2; }
struct Box<T> { T item = default; };
extern int main() {
    function<int(int)> g = dbl;
    Box<function<int(int)>> b = default;
    b.item = g;                      // <-- rejected here
    printf("byval=%d\n", b.item(6)); return 0; }
```

Both binaries:
```
(7,4): cannot store a pointer value into struct storage - a fixed array is not assignable
from a pointer or a string literal. ...
```

The message is misleading: there is no fixed array. `RegisterEncodedClosureType`
(`LLVMBackend.h:5077`) gives a THIN encoded closure a `{ i8* }` STRUCT backing type so it
"stores/copies like a normal value-type element", while a `function<>` VALUE is a bare machine
`ptr`. The generic field therefore has struct storage and the store gate sees a pointer going
into it. The container spelling avoids this - `list<function<int(int)>>` with `.add(dbl)` /
`.get(0)(6)` compiles and returns `12` on both binaries - because the element goes in through a
parameter, not through a field store.

The FAT twin works on both binaries (`Box<Lambda<int(int)>> b; b.item = f;` -> `6`), since the
fat encoded name aliases `__closure_fat_ptr`, which really is a struct.

## Sub-case 2 - a lambda LITERAL into a fat generic field: unlocated verifier failure

```cflat
import "function.cb";
struct Box<T> { T item = default; };
extern int main() {
    Box<Lambda<int(int)>> b = default;
    b.item = (int x) => x + 1;       // <-- literal, not a variable
    printf("byval=%d\n", b.item(5)); return 0; }
```

Both binaries:
```
Module verification failed:
Found return instr that returns non-void in Function of void return type!
  ret i32 %1
 void
Error: module verification failed.
```
No `file(line,col):` prefix. Assigning the SAME lambda through a named local first
(`Lambda<int(int)> f = (int x) => x + 1; b.item = f;`) compiles and prints `6` on both binaries,
and the literal works into a NON-generic struct field
(`struct P { Lambda<int(int)> item = default; }; b.item = (int x) => x + 1;` -> `6`).
So the defect is specific to a lambda literal whose target type came from generic substitution:
the literal's return type is inferred against the substituted field type and comes out `void`.

## Fix direction

Sub-case 1: either give a thin encoded closure a bare-pointer backing repr rather than a
`{ i8* }` wrapper, or teach the field store gate that a thin encoded closure destination accepts
a code pointer. The second is narrower. Whichever is chosen, keep `list<function<T>>` working -
it is the spelling in `Test/test_function_ptr.cb` (`testClosureListThin`).

Sub-case 2: resolve the lambda literal's expected signature through the substituted field type
before inferring its return type. It needs a located diagnostic either way - an unlocated
verifier dump is the worst outcome for a user.

Related: [[interface-issue-queue]]

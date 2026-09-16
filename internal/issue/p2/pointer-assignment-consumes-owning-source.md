# Plain pointer-to-pointer ASSIGNMENT consumes the owning source; declaration-init does not

Found while dogfooding `doc/LANGUAGE.md` (2026-09-16, macOS Release).

`int* p = arr;` at a DECLARATION borrows (arr stays usable). The same value flowing through an
ASSIGNMENT to an already-declared pointer (`p = arr;`) is treated as a move, so every later read
of `arr` is rejected with "use of moved variable". This contradicts the borrow-by-default ruling
(implicit consume should not happen for a raw borrowed pointer), and it makes the entire
"Pointer Arithmetic" section of `doc/LANGUAGE.md` (line 2958) fail to compile as written.

## Repro

```cflat
extern int main()
{
    int* arr = new int[4];
    arr[0] = 10;
    int* p = arr + 1;
    p = arr;                 // <- treated as a move of arr
    p++;
    int* start = arr;        // error: use of moved variable 'arr'
    printf("%d\n", start[0]);
    return 0;
}
```

`x64/Release/cflat scratch/.../t_ptr.cb -o ...` -> `t_ptr.cb(8,17): use of moved variable 'arr'`,
exit 1.

Control (compiles clean, exit 0): same program with `int* p = arr;` as the declaration and no
later re-assignment.

## Expected

`p = arr;` where `p` is a plain (non-`move`, non-`unique`) `int*` local should borrow exactly as
`int* p = arr;` does. Only an explicit `move` should consume `arr`.

## Fix direction

Find the assignment path in `MainListener.h` that sets `TypeAndValue::IsMove` / clears the source
`NamedVariable::IsOwning` for a bare pointer RHS and make it match the declaration-init path's
borrow decision. Regression test: extend `Test/test_pointer.cb` (or the nearest pointer-arithmetic
test) with the repro above.

Main-session note 2026-09-16: confirmed on master 24b86c32. The defect is the asymmetry: `int* p = arr;` (declaration) borrows, `p = arr;` (assignment) consumes. Needs a ruling on which side is right for an owning raw heap pointer assigned to a plain pointer variable (assignment-transparency ruling says the compiler infers copy vs move; declaration currently infers borrow). Fix the losing side, then the doc Pointer Arithmetic section compiles as written.

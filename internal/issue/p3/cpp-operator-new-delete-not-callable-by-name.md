# The global allocation functions operator new / operator delete cannot be called by name

C++ code calls `::operator new(n)`, `::operator new(n, std::nothrow)`, `::operator new(n, std::align_val_t{64})` and
the matching `operator delete`. CFlat has no spelling for them today: `operator new(64)` / `operator delete(p)` fails with "cannot understand the code at 'operator new'". `std.operator new(64)` should remain an error because these functions are global, not std members.
CFlat's own `new` / `delete` expressions cover the plain case, but nothrow, aligned and raw-bytes
(no constructor) allocation are unreachable.

## Repro

```cflat
import cpp "new";
extern int main()
{
    void* p = operator new(64);     // expected: non-null raw storage
    operator delete(p);
    return 0;
}
```

Case: test_libs/std_full/std_full_11_924_operator_new_calls.cb (3 legs).

## Root cause (GUESS)

`operator` followed by `new`/`delete` is only recognised as a declaration name in a struct/operator-overload
position, not as a call-expression identifier after a namespace qualifier; the bridge also may not register the
global replaceable allocation functions as callables.

## Fix direction

Accept `ns.operator new` / `operator new` / `operator delete` (and `[]` forms) as call names resolving to the
imported C++ allocation functions, including the `std::nothrow_t` and `std::align_val_t` overloads.

Found by: test_libs/std_full/std_full_11_new_initializer_list (2026-10-02).

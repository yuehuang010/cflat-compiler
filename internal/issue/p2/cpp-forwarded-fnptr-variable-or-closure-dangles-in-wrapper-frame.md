# A function-pointer variable or closure forwarded to a C++ `T&&` dangles in the wrapper frame

Sibling of the fixed literal case (t28: `std.forward_as_tuple("abc")`, and a function NAME, which now
crosses as the function lvalue). Still WRONG CODE when the forwarded reference escapes through the
result (forward_as_tuple-shaped templates, std::forward_as_tuple):

## Repro

```cflat
import cpp "library/cpp_t28_fwdref.hpp";   // Test/library fixture
extern int main()
{
    int k = 4;
    Lambda<int(int)> f = (int v) => v * k;
    printf("a %d\n", std.cflat_t28.call5(std.cflat_t28.fwd(f)));   // clang 20, CFlat 5
    function<int(int)> g = (int v) => v + 1;
    printf("b %d\n", std.cflat_t28.call5(std.cflat_t28.fwd(g)));   // clang 6, CFlat 5
    return 0;
}
```

(Compile with `-i Test`.) Same through the template path (`t28.fwd1(g)` + `t28.call5`).

## Root cause

- `function<>` variable: both wrapper spelling sites (RequestCxxFunctionTemplate and
  RequestCxxFreeFunction in LLVMBackend_CInterop.cpp) pass a thin function pointer BY VALUE and
  forward `static_cast<F &&>(pN)`, so `T&&` binds the wrapper's own parameter. C++ deduces
  `R(*&)(A)` against the caller's variable, but the binder refuses a reference-to-function-pointer
  parameter ("parameter ... is a reference to a function pointer"), so the caller slot cannot be
  handed over as `F &`.
- Closure (`Lambda<>` / capturing): CxxClosureCallArgument builds the owning `__cflat_closure<>`
  prvalue INSIDE the wrapper; the forwarded `closure&&` refers to that wrapper temporary.

## Fix direction

Function pointer: pass the caller slot's address (a `void *` or `F *` wrapper parameter, the call
spelled `*static_cast<F *>(pN)` for a proven lvalue, a caller-frame temp for an rvalue), or teach the
binder `R (*&)(A)` / `R (*&&)(A)` via LowerAliasByPointerArg. Closure: needs the closure object in a
caller-frame slot (C++ type), which no current path materializes. Check every cell against
`clang++ -std=c++20`.

Found by: t28 sibling audit (2026-10-03).

## T56 status (2026-10-06)

Thin function-pointer variables now cross generated template and free-function wrappers as
caller-slot addresses; the C++ wrapper dereferences the slot with the original pointer type and
preserves lvalue/xvalue category. Function-name designators keep the T28 `(*pN)` path. The
function-pointer half is covered by the added T56 legs in `Test/test_cpp_interop.cb`.

CFlat closures forwarded through reference-returning templates are refused when the extracted
result specializes on the C++ closure wrapper and contains an opaque pointer field. CFlat cannot
provide a caller-frame object of the generated C++ closure type, so allowing the call would keep
the known dangling-reference bug. Scalar-returning, non-escaping closure calls remain accepted.
This is a safety refusal, not full clang parity: materializing and destroying the required
caller-frame C++ closure object remains open. The refusal is covered by
`Test/errors/err_cpp_forwarded_closure_dangles.cb`.

The free non-template reference-returning `fwd` shape remains blocked by the existing importer
refusal for its function-pointer-reference result type; the original fixture has no such target.
See `scratch/t56_matrix.md` for the cell-by-cell scope and oracle record. Keep this issue active
until caller-frame C++ closure materialization and the free non-template target shape are handled.

## T56 and T69 status (2026-10-07)

- Fixed: thin function-pointer variables cross generated template and free-function wrappers as caller-slot
  addresses; the C++ wrapper dereferences the slot with the original pointer type and preserves value category.
- Fixed (records with VISIBLE fields only): reference-retaining template results are refused when the projected
  result record has a pointer or reference to the generated closure wrapper, including nested records and arrays. The shared layout predicate is
  independent of whether a by-value closure projection happened earlier. Pointer-field and `const F&` cases are
  covered before and after that projection; an empty `Tag<F>` with no retaining field remains accepted.
- Fixed: matching ternaries carry a common thin-function signature through variable and function-name arms into
  generated C++ template wrappers. Overloaded function-name arms remain ambiguous as in C++.
- Still open: CFlat cannot materialize and destroy the generated C++ closure object in the caller frame, so a
  reference-returning closure call remains a safety refusal. The free non-template reference-returning `fwd` shape
  also remains blocked by the importer refusal for its function-pointer-reference result type.
- Pre-existing and still open: a nullptr ternary arm into the C++ template is refused although clang accepts it;
  the non-template type-erased result `Erased { void* p; ... }` is accepted and dangles in both orders (master does
  the same); mismatched-signature ternaries are rejected by clang too, but the CFlat diagnostic prints `<unknown>`.
- Still open (T69 review 2, master same, accepted in both orders, clang 20): results projected as an opaque blob
  (no public fields - CClangExtract.cpp ~3760-3779 clears the field list) are invisible to the shared predicate:
  a holder whose only field is a private `F* p` dangles (rc 138/139); a returned `std::tuple<F&>` and
  `std::optional<std::reference_wrapper<F>>` print 5. Fix direction: keep field types (or a "holds a closure
  pointer/reference" flag) for blob records in the extractor (cached data - header cache version bump).
  Same type-erased family: `std::function<int(int)>(std::ref(f))` returned from the template prints 5.
  Repros: scratch/repro_keep/t69_rev/r2/ (p_holdpriv_*, p_holdtup_*, p_holdoptrw_*, q_holdprivnoctor.cb).
- Pre-existing: a generic-instantiation ternary arm `rv.c5(p ? dbl<int> : add1)` is refused with an empty
  argument-type list in the message.

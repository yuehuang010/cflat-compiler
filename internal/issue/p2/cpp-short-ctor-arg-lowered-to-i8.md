# C++ ctor/call argument for a `short` parameter is lowered as i8

## Summary

Calling an imported C++ constructor whose parameter is `short` (`Tagged<short>(short v)`,
mangled `...C1Es`) with an int literal emits `i8 signext 7` against the `i16` signature:

```
Module verification failed:
Call parameter type does not match function signature!
i8 7
 i16  %0 = call ptr @_ZN4cppt6TaggedIsvEC1Es(ptr %v, i8 signext 7)
```

## Repro

`Test/library/cpp_interop_tpl.h` has `template <typename T, typename Tag = void> class Tagged`
with `explicit Tagged(T v)`. Add `using TaggedShort = Tagged<short>;` and compile:

```
import cpp "cpp_interop_tpl.h";
extern int main()
{
    cppt.TaggedShort v = cppt.TaggedShort(7);
    return v.get() != 7;
}
```

Found 2026-09-11 while adding the M21 lazy-alias-temporary case; the fixture was switched to
`Tagged<double>` to keep that case independent of this bug.

## Root cause (suspected)

The C/C++ primitive re-typing (fd10176b: C spellings as aliases, c8/c16/c32) maps the C++
`short` parameter onto a CFlat type whose call-site narrowing of an int literal produces an
i8 while the extracted signature keeps clang's i16. Check the literal-to-param conversion for
the `short` spelling on the C++ ctor call path (`LLVMBackend_CInterop.cpp`, ctor candidate
argument coercion), and whether a plain C `short` parameter in `Test/test_c_interop.cb` shows
the same. `Tagged<int>` and `Tagged<long>` are fine.

## Fix direction

Coerce the argument to the parameter's extracted width (i16), same as int/long; add a
`Tagged<short>` case next to the M21 rows in `Test/test_cpp_interop.cb` once fixed.

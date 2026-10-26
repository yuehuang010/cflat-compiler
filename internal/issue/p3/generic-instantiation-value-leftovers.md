# Generic instantiation used as a value: remaining leftovers (T201 narrowed)

T191 (Opus r1, 2026-10-14) found these; T201 fixed the original publish-site cases. T434 fixed the
assignment crash, qualified non-generic and wrong-arity handling, unresolved C++-import values, and
the bare generic-method diagnostic. Remaining:

- Overloaded generic `ov<int>` (1-param and 2-param `ov<T>`): generic templates are stored one per name
  (`genericFunctionTemplates[name]`), so the second definition replaces the first. `ov<int>(3)` also fails
  as a CALL ("no overload of 'ov<int>' matches"). The slot-arity pick needs a template overload set first.
  Current result: `function<int(int)> g = ov<int>;` is refused ("cannot bind ... takes 2 parameter(s)").
  Consider a refusal at the second template's declaration if an overload set is not planned.
- A real C++ function-template instance used as a CFlat function value is now refused with a diagnostic,
  so it no longer reaches the runtime trap. Binding the C++ specialization as a function value remains open;
  implement it only if the C++ call path can instantiate it cheaply.

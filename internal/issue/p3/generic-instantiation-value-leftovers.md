# Generic instantiation used as a value: remaining leftovers (T201 narrowed)

T191 (Opus r1, 2026-10-14) found these; T201 fixed the rest (non-generic and wrong-count refusals, bool
conversion, return-site binding). Still open, all reproduced on the T201 pre-fix build:

- Overloaded generic `ov<int>` (1-param and 2-param `ov<T>`): generic templates are stored one per name
  (`genericFunctionTemplates[name]`), so the second definition replaces the first. `ov<int>(3)` also fails
  as a CALL ("no overload of 'ov<int>' matches"). The slot-arity pick needs a template overload set first.
  Current result: `function<int(int)> g = ov<int>;` is refused ("cannot bind ... takes 2 parameter(s)").
  Consider a refusal at the second template's declaration if an overload set is not planned.
- `bool b = false; b = wrap<int>;` (assignment of a generic instance to a bool, not a declaration) crashes the
  compiler (exit 139, CreateAssignment with a null value from ParseAssignmentExpression). The named form
  `b = t1;` compiles and gives true. Fix: bind the instance in the assignment RHS as the declaration does.
- Namespace-qualified NON-generic `NsB.plain<int>` (value and call) still compiles and runs (`plain(1)`
  returns the plain result; type args ignored). The bare spelling is refused by the T201 publish-site check;
  the dotted spelling reaches a different path.
- Namespace-qualified wrong count `NsA.dbl<int, int>` is refused, but as "'dbl' does not name a value here",
  not the arity message the bare spelling gives.
- With any `import cpp` in scope, an undefined `nosuch<int>` as a value (`function<int(int)> f = nosuch<int>;`)
  still compiles and traps at run time (exit 133); a real C++ function template as a value (`ctwice<int>`) also
  traps 133. The T201 refusal only fires under a C++ import when the name is a known CFlat function. Fix:
  refuse an unresolved spelling after the C++ lookup misses; bind a C++ template instance as a function value.
- A generic METHOD named bare inside its struct (`m<int>` without `this.`) is refused as "'m' is not a generic
  function" (was a 133 trap on master): wrong wording, should name the generic method or bind it.

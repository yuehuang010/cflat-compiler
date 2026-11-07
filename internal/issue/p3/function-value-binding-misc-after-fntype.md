# Function-value leftovers after the fntype landing

Found 2026-11-03 by the fntype review (box 10), both PRE-EXISTING.

- `--symbol-dump` spells a function<> parameter as `int u(<c_fn_ptr> f)` - an internal placeholder in user output; it
  should print the signature (`function<int(int)> f`).
- Binding the overload set `{ fe(Color), fe(int) }` (Color an enum) to `function<int(Color)>` reports "cannot choose which
  'fe' to use"; clang picks fe(Color) (exact match beats the enum -> int promotion).

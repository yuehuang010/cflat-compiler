# Function-table method vs top-level collision (IsMethod)

`functionTable` is keyed by plain function name. Struct methods are registered under their plain name (not qualified), so a method `atomic_counter::add` can shadow a user-defined top-level `add`.

**Design:** `bool IsMethod` on `FunctionSymbol` marks struct/class methods. `GetFunctionForFuncPtr(name, expectedParamCount)` prefers `!IsMethod` overloads when resolving a function name as a value (function pointer assignment). Primary-expression identifier resolution uses this instead of `GetFunction`.

**How to apply:** If future core library additions add methods with common names (e.g., `init`, `add`, `get`), they are registered under the plain name. The `IsMethod` flag prevents them from shadowing user-defined top-level functions when used as function pointers.

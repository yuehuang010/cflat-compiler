# C++ namespace inline constexpr variables are absent from imported lookup

Summary: `import cpp "numbers"` does not expose `std::numbers::pi` or `std::numbers::pi_v<double>` to CFlat lookup, although both are declared by the header and compile in C++20. This blocks reaching standard constants through namespace lookup.

Repro:
```cflat
import cpp "numbers";
extern int main()
{
    double value = std.numbers.pi;
    return value < 3.14159;
}
```

Observed: compile error: `'pi' is not a member of namespace 'std.numbers'`. The natural `std.numbers.pi_v<double>` spelling also fails with `no imported C++ header declares 'std::numbers::pi_v'`.
Expected: resolve the inline constexpr variable and variable template declared in `<numbers>` and allow their values to cross the CFlat boundary.
Suspected area: namespace-scope C++ variable registration and variable-template lookup.

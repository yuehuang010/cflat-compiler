# C++ class with only a private default constructor is default-initialized to zero

## Summary

A trivially copyable C++ class whose only default constructor is non-public is accepted by
`T x = default;` and left zero-filled. C++ rejects the declaration (the constructor is
inaccessible); cflat neither calls a constructor nor reports anything.

## Repro

```cpp
// h.h
#include <memory>
namespace dac {
    struct Priv {
        long v;
        long get() const noexcept { return v; }
    private:
        Priv(const std::allocator<char>& a = {}) noexcept : v(20) {}
    };
}
```

```cflat
import cpp "h.h";
extern int main()
{
    dac.Priv x = default;
    return (int)x.get();   // exits 0; C++ would not compile
}
```

## First error

None - compiles and runs, exit 0.

## Root-cause guess

`TryBindCxxImplicitDefaultCtor` correctly refuses (non-public default ctor), but the local
`= default` path for a trivially copyable record falls back to zero-initialization instead of
reporting "no default constructor cflat can call". Found while probing the defaulted-argument
constructor fix (probe dac_p21); same result before and after that fix.

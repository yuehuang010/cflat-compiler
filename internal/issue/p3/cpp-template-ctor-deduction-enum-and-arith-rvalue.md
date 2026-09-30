# P3: C++ template constructor deduction - enum lvalue and arithmetic rvalue deduce the wrong T

Found by B4 review 1 (pre-existing, master same). A C++ constructor template called from CFlat
deduces a different `T` than clang for two scalar kinds:

```cpp
#include <type_traits>
namespace rw {
template <class T> constexpr long tid() { return std::is_same_v<T,unsigned>?2:std::is_same_v<T,long>?3:
  std::is_same_v<T,long long>?5:std::is_enum_v<T>?13:99; }
struct W { long id; template <class T> W(T& x) : id(tid<T>()) {}
  template <class T> W(T&& x) requires (!std::is_lvalue_reference_v<T>) : id(tid<T>()) {} };
enum E { EA = 4 };
}
```
```cflat
rw.E e = rw.EA; long l = 3;
long a = rw.W(e).id;       // clang 13 (T = rw::E), cflat 2 (T = unsigned)
long b = rw.W(l + 1).id;   // clang 3 (T = long), cflat 5 (T = long long)
```
Direction: the wrapper deduction maps the CFlat argument type to a C++ spelling; an enum-typed
lvalue must spell its C++ enum, and the result type of `long` arithmetic must stay `long` (likely the
CFlat `long` -> C++ `long` spelling is lost once the binary result is typed i64 - unverified).

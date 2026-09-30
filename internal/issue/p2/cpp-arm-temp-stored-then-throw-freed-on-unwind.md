# [P2] C++ arm temp stored then throw is freed during unwind

Found 2026-09-29 in E2 round-1 review; pre-existing on master.

Summary: A C++ callee can store a joined temporary pointer and throw, after which CFlat frees the
stored object while unwinding. The retained pointer then refers to a destroyed object.

Repro (`rv.h`):

```cpp
#include <cstddef>
#include <cstdlib>
#include <stdexcept>
namespace rv {
struct E {
    int v;
    explicit E(int x) : v(x) {}
    ~E() { v = -777; }
    static void* operator new(std::size_t n) { return std::malloc(n); }
    static void operator delete(void* p) { std::free(p); }
};
inline const E* kept = nullptr;
inline int thrKeep(const E* p, int t) { kept = p; throw t; return p ? p->v : -1; }
typedef int (*Cb)(int);
inline int guarded(Cb f, int x) { try { return f(x); } catch (int e) { return -e; } }
}
```

Repro (`main.cb`):

```cflat
import cpp "rv.h";
rv.E* en = nullptr;
int pick = 1;
int t(int x) { return rv.thrKeep(pick > 0 ? new rv.E(x) ?? en : en, x); }
extern int main() { int r = rv.guarded(t, 42); return rv.kept->v == 42 ? 0 : 1; }
```

The review probe also confirms `rv.thrKeep(en ?? new rv.E(x), x)` has the same unwind failure,
while direct `rv.thrKeep(new rv.E(x), x)` leaves the pointer alive. On master and the E2 branch,
the joined form frees the selected object during unwind and the retained pointer is invalid
(the repro exits 1: `kept->v` reads freed memory, observed 2).

Root cause pointer from review: `DropRetainedJoinArmPtrTemps` gates normal cleanup for a retaining
C++ callee, but its conditional-arm temporary is still in the unwind cleanup path; that path frees
the arm before the gate can preserve the escaped pointer.

Fix direction: preserve retaining C++ join arms on exceptional edges using the same proof-based
escape gate as the direct-`new` argument path; account for both `?:` and `??` arm spellings.

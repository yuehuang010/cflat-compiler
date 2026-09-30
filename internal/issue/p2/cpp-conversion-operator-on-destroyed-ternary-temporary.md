# C++ conversion operator runs after ternary temporary destruction

Status: active. At -O0 and -O2, the selected rvalue member temporary is
destroyed before its `operator bool` call. This is a use-after-destroy. clang
calls the operator before the destructor; master and the B15 branch exhibit
the same defect. The O2 run still records `dtAtCall=1`; optimization only
removes the reloaded `self_` observation, so it does not mask the lifetime bug.

Repro (`pd2.cb`):

```cflat
import cpp "pd.h";
extern int main()
{
    for (int f = 0; f < 2; f++) {
        bool ff = f != 0;
        int r = 0;
        pd.reset(); pd.dead = 0; pd.dtAtCall = -1;
        if (ff ? pd.mkh(1).b : pd.mkh(2).b) r = 1;
        printf("f=%d r=%d bad=%d dead=%d dtAtCall=%d dt=%d cp=%d\n",
               f, r, pd.bad, pd.dead, pd.dtAtCall, pd.dtors, pd.copies);
    }
    return 0;
}
```

Header (`pd.h`):

```cpp
#pragma once
namespace pd {
inline int dead = 0, dtAtCall = -1;
inline int hits = 0, dtors = 0, copies = 0, bad = 0, lastid = 0;
inline void reset() { hits = 0; dtors = 0; copies = 0; bad = 0; lastid = 0; }
class B {
public:
    B() : self_(this), id(0) {}
    B(const B& o) : self_(this), id(o.id) { ++copies; }
    ~B() { ++dtors; self_ = nullptr; }
    operator bool() {
        ++hits; lastid = id; if (self_ != this) ++bad;
        dead = (self_ == nullptr); dtAtCall = dtors; return id % 2 == 1;
    }
    B* self_; int id;
};
class D : public B { public: int extra = 0; };
struct H { B b; };
inline B mk(int id) { B b; b.id = id; return b; }
inline H mkh(int id) { H h; h.b.id = id; return h; }
inline B& thr(B& x, bool t) { if (t) return x; return x; }
}
```

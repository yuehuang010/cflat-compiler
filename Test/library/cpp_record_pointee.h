// CPPRECPTR: unrelated C++ records for Test/errors/err_implicit_unrelated_pointer_store.cb.
struct UrpB { int v = 7; };
struct UrpU { long long w = 9; };
struct UrpBase { int b = 5; };
struct UrpDer : UrpBase { int d = 6; };
inline int urp_take(UrpB* p) { return p->v; }

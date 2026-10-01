static inline int same_static_helper(int x) { return x + 20; }
static inline int c_inline_b(int x) { return same_static_helper(x); }

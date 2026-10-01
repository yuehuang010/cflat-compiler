static inline int same_static_helper(int x) { return x + 10; }
static inline int c_inline_a(int x) { return same_static_helper(x); }

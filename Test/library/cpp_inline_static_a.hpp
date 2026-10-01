static int cpp_inline_static_calls = 0;
static const int cpp_inline_const_table[3] = {1, 2, 3};
static inline void cpp_record_static_call() { ++cpp_inline_static_calls; }
inline int cpp_inline_const_table_a(int i) { return cpp_inline_const_table[i]; }
static inline int cpp_same_static_helper(int x) { return x + 1; }
inline int cpp_inline_static_a(int x) { cpp_record_static_call(); return cpp_same_static_helper(x) * 10; }
inline int cpp_inline_static_count_a() { return cpp_inline_static_calls; }
static inline int cpp_same_static_g(int x) { return x + 1; }
static inline int cpp_same_static_f(int x) { return cpp_same_static_g(x); }
inline int cpp_inline_static_fa(int x) { return cpp_same_static_f(x) * 10; }

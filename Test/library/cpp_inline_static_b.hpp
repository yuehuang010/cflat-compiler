#define CPP_GROUP_SHARED_B 9
#define CPP_GROUP_SHARED_Z -0.0
#include "cpp_group_shared.hpp"
static int cpp_inline_static_calls = 0;
static const int cpp_inline_const_table[3] = {7, 8, 9};
static inline void cpp_record_static_call() { ++cpp_inline_static_calls; }
inline int cpp_inline_const_table_b(int i) { return cpp_inline_const_table[i]; }
static inline int cpp_same_static_helper(int x) { return x + 2; }
inline int cpp_inline_static_b(int x) { cpp_record_static_call(); return cpp_same_static_helper(x) * 10; }
inline int cpp_inline_static_count_b() { return cpp_inline_static_calls; }
static inline int cpp_same_static_g(int x) { return x + 2; }
static inline int cpp_same_static_f(int x) { return cpp_same_static_g(x); }
inline int cpp_inline_static_fb(int x) { return cpp_same_static_f(x) * 10; }
constexpr int cpp_group_k = 2;
namespace cpp_group_ns { static int counter = 20; }
inline constexpr int cpp_group_inline_k = 6;

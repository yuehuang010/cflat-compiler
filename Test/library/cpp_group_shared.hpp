#pragma once
// Reached through two import groups (cpp_inline_static_a/b.hpp). A const of this one declaration
// binds when both groups evaluate it alike; cpp_inline_static_b.hpp sets its own field value.
#ifndef CPP_GROUP_SHARED_B
#define CPP_GROUP_SHARED_B 8
#endif
constexpr int cpp_group_shared_k = 5;
struct CppGroupSharedPair { int a; int b; };
constexpr CppGroupSharedPair cpp_group_shared_p{7, CPP_GROUP_SHARED_B};
constexpr CppGroupSharedPair cpp_group_shared_q{7, 8};
// An address names each group's own object, however alike it prints.
static int cpp_group_shared_store = 0;
constexpr int* cpp_group_shared_ptr = &cpp_group_shared_store;
// Floats compare by bits: 0.0 vs -0.0 differ (b sets -0.0); one NaN declaration agrees.
#ifndef CPP_GROUP_SHARED_Z
#define CPP_GROUP_SHARED_Z 0.0
#endif
constexpr double cpp_group_shared_z = CPP_GROUP_SHARED_Z;
constexpr double cpp_group_shared_nan = __builtin_nan("");

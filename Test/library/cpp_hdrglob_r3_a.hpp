#ifndef CFLAT_TEST_CPP_HDRGLOB_R3_A_HPP
#define CFLAT_TEST_CPP_HDRGLOB_R3_A_HPP

#include "cpp_hdrglob_r3_common.hpp"

namespace hdrglob_r3 {
static int S = note_same_name(3);
inline int same_named_static_a() { return S; }
inline int* same_named_static_ptr_a() { return &S; }
inline int* plain_ptr_a() { return &P; }
inline int* out_of_line_ptr_a() { return &Q::m; }
inline int* anonymous_ptr_a() { return anonymous_address(); }
inline int* static_ptr_a() { return static_address(); }
inline int* shared_static_ptr_a() { return &SharedStatic; }
inline int* shared_anonymous_ptr_a() { return &SharedAnonymous; }
inline int* shared_anonymous_member_ptr_a() { return &SharedAnonymousRecord::m; }
}

#endif

#ifndef CFLAT_TEST_CPP_HDRGLOB_R3_B_HPP
#define CFLAT_TEST_CPP_HDRGLOB_R3_B_HPP

#include "cpp_hdrglob_r3_common.hpp"

namespace hdrglob_r3 {
static int S = note_same_name(4);
inline int same_named_static_b() { return S; }
inline int* same_named_static_ptr_b() { return &S; }
inline int* plain_ptr_b() { return &P; }
inline int* out_of_line_ptr_b() { return &Q::m; }
inline int* anonymous_ptr_b() { return anonymous_address(); }
inline int* static_ptr_b() { return static_address(); }
inline int* shared_static_ptr_b() { return &SharedStatic; }
inline int* shared_anonymous_ptr_b() { return &SharedAnonymous; }
inline int* shared_anonymous_member_ptr_b() { return &SharedAnonymousRecord::m; }
}

#endif

#ifndef CFLAT_TEST_CPP_HDRGLOB_B_HPP
#define CFLAT_TEST_CPP_HDRGLOB_B_HPP

#include "cpp_hdrglob_common.hpp"

namespace hdrglob {
inline int b_value = (++counter, sequence = sequence * 10 + 7, 62);

template <class T> inline int explicit_variable = note_new_initializer(sizeof(T) == 4 ? 5 : 6);
template int explicit_variable<int>;
template int explicit_variable<long long>;
inline int explicit_variable_referenced_now() { return explicit_variable<int>; }

template <class T> struct ExplicitStatic
{
    static int value;
};
template <class T> int ExplicitStatic<T>::value = note_new_initializer(sizeof(T) == 4 ? 7 : 8);
template struct ExplicitStatic<int>;
template struct ExplicitStatic<long long>;
inline int explicit_static_referenced_now() { return ExplicitStatic<int>::value; }
}

#endif

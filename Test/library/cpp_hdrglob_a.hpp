#ifndef CFLAT_TEST_CPP_HDRGLOB_A_HPP
#define CFLAT_TEST_CPP_HDRGLOB_A_HPP

#include "cpp_hdrglob_common.hpp"

namespace hdrglob {
inline int a_value = (++counter, sequence = sequence * 10 + 6, 61);
inline int new_initializer_sequence;
inline int new_initializer_count;
inline int note_new_initializer(int digit)
{
    ++new_initializer_count;
    new_initializer_sequence = new_initializer_sequence * 10 + digit;
    return digit;
}
namespace {
int anonymous_unreferenced = note_new_initializer(1);
int anonymous_referenced = note_new_initializer(2);
}
inline int anonymous_referenced_now() { return anonymous_referenced; }

struct OutOfLineStatic
{
    static int unreferenced;
    static int referenced;
};
int OutOfLineStatic::unreferenced = note_new_initializer(3);
int OutOfLineStatic::referenced = note_new_initializer(4);
inline int out_of_line_static_referenced_now() { return OutOfLineStatic::referenced; }
inline int new_initializer_sequence_now() { return new_initializer_sequence; }
inline int new_initializer_count_now() { return new_initializer_count; }
}

#endif

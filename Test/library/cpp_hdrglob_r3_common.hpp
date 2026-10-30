#ifndef CFLAT_TEST_CPP_HDRGLOB_R3_COMMON_HPP
#define CFLAT_TEST_CPP_HDRGLOB_R3_COMMON_HPP

namespace hdrglob_r3 {
inline int plain_init_count;
inline int out_of_line_init_count;
inline int anonymous_init_count;
inline int static_init_count;
inline int anonymous_member_init_count;
inline int explicit_member_init_count;
inline int same_name_init_count;
inline int shared_static_init_count;
inline int shared_anonymous_init_count;
inline int shared_anonymous_member_init_count;

inline int note_plain(int value) { ++plain_init_count; return value; }
inline int note_out_of_line(int value) { ++out_of_line_init_count; return value; }
inline int note_anonymous(int value) { ++anonymous_init_count; return value; }
inline int note_static(int value) { ++static_init_count; return value; }
inline int note_same_name(int value) { ++same_name_init_count; return value; }
inline int note_anonymous_member(int value)
{
    ++anonymous_member_init_count;
    return value;
}
inline int note_explicit_member(int value)
{
    ++explicit_member_init_count;
    return value;
}
inline int note_shared_static(int value)
{
    ++shared_static_init_count;
    return value;
}
inline int note_shared_anonymous(int value)
{
    ++shared_anonymous_init_count;
    return value;
}
inline int note_shared_anonymous_member(int value)
{
    ++shared_anonymous_member_init_count;
    return value;
}

static int SharedStatic = note_shared_static(8);
namespace {
int SharedAnonymous = note_shared_anonymous(6);
struct SharedAnonymousRecord { static int m; };
int SharedAnonymousRecord::m = note_shared_anonymous_member(7);
}

int P = note_plain(5);
struct Q { static int m; };
int Q::m = note_out_of_line(7);

namespace { int AC = note_anonymous(1); }
static int SC = note_static(2);

namespace {
struct AQ { static int m; };
int AQ::m = note_anonymous_member(9);
}

template <class T> struct H { static int m; };
template <class T> int H<T>::m = note_explicit_member(6);
template int H<short>::m;

inline int plain_count() { return plain_init_count; }
inline int out_of_line_count() { return out_of_line_init_count; }
inline int anonymous_count() { return anonymous_init_count; }
inline int static_count() { return static_init_count; }
inline int anonymous_member_count() { return anonymous_member_init_count; }
inline int explicit_member_count() { return explicit_member_init_count; }
inline int same_name_count() { return same_name_init_count; }
inline int* anonymous_address() { return &AC; }
inline int* static_address() { return &SC; }
inline int shared_static_count() { return shared_static_init_count; }
inline int shared_anonymous_count() { return shared_anonymous_init_count; }
inline int shared_anonymous_member_count() { return shared_anonymous_member_init_count; }
}

#endif

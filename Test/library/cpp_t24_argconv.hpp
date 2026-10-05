#pragma once

namespace t24arg
{
struct VoidCtor
{
    void* value;
    explicit VoidCtor(void* p) : value(p) {}
    int get() const { return value != nullptr ? 1 : 0; }
};

inline int pick_char_or_void(void*) { return 1; }
inline int pick_char_or_void(char*) { return 2; }
inline int pick_void_or_bool(void*) { return 3; }
inline int pick_void_or_bool(bool) { return 4; }
inline int take_char(char* p) { return p != nullptr ? 11 : 0; }
inline int take_const_char(const char* p) { return p != nullptr ? 12 : 0; }
inline int take_void(void* p) { return p != nullptr ? 13 : 0; }
inline int take_bool(bool b) { return b ? 14 : 0; }
inline int take_int_pointer(int* p) { return p != nullptr ? *p : 0; }
inline int take_int_pointer_pointer(int** p) { return p != nullptr && *p != nullptr ? **p : 0; }
}

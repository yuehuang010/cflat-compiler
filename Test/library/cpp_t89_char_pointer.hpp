#pragma once

// char, signed char and unsigned char are distinct C++ pointee types (T89).
inline int t89_take_char(char* value) { return value != nullptr ? 1 : 0; }
inline int t89_take_schar(signed char* value) { return value != nullptr ? 2 : 0; }
inline int t89_take_uchar(unsigned char* value) { return value != nullptr ? 3 : 0; }
inline int t89_take_char_pp(char** value) { return value != nullptr ? 4 : 0; }
inline int t89_pick(char* value) { return 11; }
inline int t89_pick(void* value) { return 22; }
int t89_variadic(char* value, ...) { return 55; }

#pragma once

inline int t43_over_char(const char*) { return 1; }
inline int t43_over_char(const char8_t*) { return 2; }
inline int t43_over_char(const char16_t*) { return 3; }
inline int t43_over_char(const char32_t*) { return 4; }
inline int t43_take_char(const char*) { return 5; }
inline int t43_over_c16(const char16_t*) { return 6; }
inline int t43_over_c32(const char32_t*) { return 7; }
inline int t43_take_c16(const char16_t*) { return 8; }
inline int t43_take_c32(const char32_t*) { return 9; }
inline int t43_take_plain_char(const char*) { return 10; }

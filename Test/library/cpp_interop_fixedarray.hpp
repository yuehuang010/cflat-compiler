#pragma once

#include <cstddef>
#include <cstring>
#include <cwchar>
#include <type_traits>

namespace cflat_t6 {

struct ArrayText {
    std::size_t length;
    ArrayText(const char* text) : length(std::strlen(text)) {}
    ArrayText(const char* text, std::size_t count) : length(count) { (void)text; }
    std::size_t size() const { return length; }
    std::size_t copy(const char* text, std::size_t count) const
    { return std::strlen(text) < count ? std::strlen(text) : count; }
};

inline std::size_t char_count(const char* text, std::size_t add)
{ return std::strlen(text) + add; }

inline int int_first(const int* values, int add)
{ return values[0] + add; }

inline std::size_t wide_count(const wchar_t* text) { return std::wcslen(text); }
inline double double_first(const double* values) { return values[0]; }
inline std::ptrdiff_t range_count(const int* first, const int* last)
{ return last - first; }

inline int pointer_preferred(int) { return 1; }
inline int pointer_preferred(const int*) { return 2; }

struct IntPointerPreferred {
    int value;
    IntPointerPreferred(int scalar) : value(-scalar) {}
    IntPointerPreferred(const int* values) : value(values[2]) {}
};

struct CharPointerPreferred {
    int value;
    CharPointerPreferred(char scalar) : value(-scalar) {}
    CharPointerPreferred(const char* text) : value(text[1]) {}
};

struct CountedCharPointerPreferred {
    int value;
    CountedCharPointerPreferred(char scalar, int count) : value(-scalar - count) {}
    CountedCharPointerPreferred(const char* text, int count) : value(text[count]) {}
};

inline int record_first(const struct Record* values);
struct Record { int value; };
inline int record_first(const Record* values) { return values[0].value; }

template<class T>
inline std::size_t pointer_element_size(T*) { return sizeof(T); }

template<class T>
inline bool by_value_pointer(T) { return std::is_pointer<T>::value; }

template<class T, std::size_t N>
inline std::size_t array_extent(const T (&)[N]) { return N; }

inline std::size_t operator+(const ArrayText& text, const char* suffix)
{ return text.length + std::strlen(suffix); }

inline bool operator==(const ArrayText& text, const char* value)
{ return text.length == std::strlen(value); }

}

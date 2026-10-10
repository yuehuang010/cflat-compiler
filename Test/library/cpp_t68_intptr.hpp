#pragma once
#include <cstddef>

#undef NULL
#define NULL 0
#define T68_ZERO 0
#define T68_NONZERO 5
namespace cpi_t68 {
inline int only(int* p) { return p == nullptr ? 5 : 6; }
inline int over(int* p) { return p == nullptr ? 50 : 51; }
inline int over(int value) { return 100 + value; }
struct FnBox {
    bool null_value;
    FnBox(void (*value)()) : null_value(value == nullptr) {}
    bool is_null() const { return null_value; }
};
}

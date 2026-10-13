#pragma once
// Expression statement at namespace scope in a header: clang refuses it (T101 A).
namespace cpp_header_less_than_stmt { int x, y; x < y; }

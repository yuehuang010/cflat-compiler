#pragma once
// Expression statement at global scope in a header: clang refuses it (T101 B).
int cpp_header_global_gx;
cpp_header_global_gx + 1;

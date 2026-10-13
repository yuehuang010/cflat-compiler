#pragma once
// A namespace-scope `<` statement, then an unfinished parameter list whose default argument
// nests a brace inside a paren: clang refuses both; the header parse must neither crash nor spin.
namespace cpp_header_unfinished_param_a { int a, b; a < b; }
namespace cpp_header_unfinished_param { int f(int x = int{(

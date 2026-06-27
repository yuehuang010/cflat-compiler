#pragma once
// nlohmann_json's char_traits::eof() uses the EOF macro but does not include
// <cstdio> itself. Several headers this project pulls in (antlr's runtime and
// some LLVM headers) #undef EOF, so under GCC it can be absent at the point
// json.hpp is parsed. Guarantee EOF is defined before including json.
#include <cstdio>
#ifndef EOF
#  define EOF (-1)
#endif
#include <nlohmann/json.hpp>

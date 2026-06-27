#pragma once
// EOF-macro-safe access to antlr4::Token::EOF.
//
// glibc's <cstdio> defines EOF as an object-like macro. At an antlr token-type
// comparison site (`... == antlr4::Token::EOF`) GCC expands the macro and the
// qualified name breaks. We cannot simply #undef EOF globally: nlohmann_json's
// char_traits::eof() uses the EOF macro and, under GCC, binds it at template
// instantiation - removing it breaks json. So capture the antlr constant once
// here with the macro locally suppressed (push/pop is supported by MSVC and
// GCC/Clang), leaving the EOF macro intact for every other translation unit.
#include <antlr4-runtime.h>
#include <cstddef>

#pragma push_macro("EOF")
#ifdef EOF
#  undef EOF
#endif

namespace cflat {
inline constexpr std::size_t kTokenEOF = antlr4::Token::EOF;
}

#pragma pop_macro("EOF")

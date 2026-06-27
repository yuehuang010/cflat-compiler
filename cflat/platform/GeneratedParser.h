#pragma once
// Single entry point for the antlr-generated parser/lexer/listener headers.
//
// Those generated headers declare an accessor named EOF() and reference
// antlr4::Token::EOF. When the EOF macro is defined (it is whenever <cstdio>
// has been seen - e.g. via the precompiled header) the preprocessor expands
// those names and the headers fail to compile. Undef EOF only across the
// generated includes here, then restore it (nlohmann_json and MSVC's <fstream>
// rely on the EOF macro). antlr4-runtime.h is included with EOF intact because
// it pulls in <fstream>.
#include <antlr4-runtime.h>
#include "AntlrCompat.h" // cflat::kTokenEOF (EOF-macro-safe Token::EOF)

#pragma push_macro("EOF")
#undef EOF
#include <CFlatParser.h>
#include <CFlatLexer.h>
#include <CFlatBaseListener.h>
#pragma pop_macro("EOF")

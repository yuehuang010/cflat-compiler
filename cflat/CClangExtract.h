// C-interop extraction via the clang C++ API (single parse).
//
// This header is deliberately free of any clang/LLVM types so it can be included
// by LLVMBackend.h without dragging the heavy clang C++ headers into that (already
// /bigobj) translation unit. All clang C++ usage lives in CClangExtract.cpp.
//
// The extractor parses a C translation unit ONCE and emits plain-data "spellings":
// function signatures, enum constants, records (structs/unions), typedefs, object-like
// macros (value-folded in-process), and function-like macros. The cflat backend then
// maps those spellings to its TypeAndValue/LLVM types via MapCTypeToTypeAndValue and
// feeds the existing Register*/cache machinery - none of which changes.
#pragma once

#include <string>
#include <vector>

namespace cflat_cinterop
{
    // A C function signature. Types are canonical C spellings (e.g. "int", "unsigned long long",
    // "struct Point *", "int (*)(int, int)") so the backend's string-based mapper consumes them
    // exactly as it did the libclang DesugaredSpelling.
    struct RawSig
    {
        std::string name;
        std::string retType;
        std::vector<std::string> paramTypes;
        std::vector<std::string> paramNames;   // aligned with paramTypes (may be empty strings)
        bool variadic = false;
        std::string file;
        int line = 1;
        int col = 0;
    };

    struct RawEnum
    {
        std::string name;
        long long value = 0;
        std::string file;
        int line = 1;
        int col = 0;
    };

    struct RawField
    {
        std::string name;
        std::string ctype;          // canonical C spelling of the field type
        bool isBitfield = false;
        unsigned bitWidth = 0;
    };

    struct RawRecord
    {
        std::string name;           // tag name; empty for anonymous (caller synthesizes)
        bool isUnion = false;
        std::vector<RawField> fields;
        std::string file;
        int line = 1;
        int col = 0;
        // True when the record's definition is under the in-scope dirs (the bound header's own
        // dir / the --c-include roots). Records are collected regardless of scope so that an
        // in-scope struct's by-value dependency types (e.g. POINT in MSG, defined in the SDK's
        // shared/ dir) can still be registered; the backend keeps the transitive closure of
        // in-scope records and drops the rest. Always true on the requireInScope=false (.c) path.
        bool inScope = true;
    };

    // typedef Name = canonical spelling of the aliased type. Mirrors CollectCTypedefsLibclang.
    struct RawTypedef
    {
        std::string name;
        std::string underlying;
    };

    // An object-like macro folded in-process. Exactly one of int/float/string is meaningful,
    // selected by `kind`. naturalType carries the canonical spelling of the macro's value type
    // (e.g. "double", "char[8]", "int (*)(int, int)", "void *") so the backend can classify
    // pointer / function-pointer / float / string the same way the old __typeof__ probe did.
    struct RawMacro
    {
        enum Kind { Skip, Int, Float, String };
        std::string name;
        Kind kind = Skip;
        long long intValue = 0;
        double floatValue = 0.0;
        std::string stringValue;     // decoded characters (string kind)
        std::string naturalType;     // canonical type spelling of the folded expression
        std::string file;
        int line = 1;
        int col = 0;
    };

    // An externally-linkable global variable a header declares (`extern int x;`) or a .c file
    // defines (`int x = 5;`). ctype is the canonical C spelling of the variable's type, consumed
    // by the same string-based mapper as RawSig param/return types. Bound mutable, C-style.
    struct RawGlobalVar
    {
        std::string name;
        std::string ctype;
        std::string file;
        int line = 1;
        int col = 0;
    };

    struct RawFuncMacro
    {
        std::string name;
        std::vector<std::string> params;
        std::string body;
        std::string file;
        int line = 1;
        int col = 0;
    };

    struct ExtractRequest
    {
        // Virtual main-file name for the in-memory stub (e.g. "cflat_hdr_stub.c"). When
        // `source` is empty, `realPath` names a real .c file on disk to parse instead.
        std::string mainFileName;
        std::string source;
        std::string realPath;

        // Driver args (target, -I, -D, ...) - same list BuildLibclangArgs produced for libclang.
        std::vector<std::string> args;

        bool wantMacros = false;            // header-bind path harvests macros; .c path does not
        bool requireInScope = false;        // keep only decls whose file is under inScopeDirs
        std::vector<std::string> inScopeDirs;
        bool definitionsOnly = false;       // .c auto-extern: only functions defined in this TU
        bool wantIncludes = false;          // deep header-cache: record every transitively included file
        bool skipFunctionBodies = false;    // header bind: parse declarations only, skip function bodies
    };

    struct ExtractResult
    {
        std::vector<RawSig> sigs;
        std::vector<RawEnum> enums;
        std::vector<RawRecord> records;
        std::vector<RawTypedef> typedefs;
        std::vector<RawGlobalVar> globals;
        std::vector<RawMacro> macros;
        std::vector<RawFuncMacro> funcMacros;
        std::vector<std::string> includedFiles;  // populated only when req.wantIncludes
    };

    // Parse the TU once and fill `out`. Returns false only on a hard failure to build a TU;
    // a TU produced with diagnostics still returns true (per-decl error recovery, like the
    // old -ferror-limit=0 path). `err` carries a human-readable reason on hard failure.
    bool ExtractCInterop(const ExtractRequest& req, ExtractResult& out, std::string& err);
}

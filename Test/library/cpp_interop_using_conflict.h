#pragma once

// Conflicting `using` re-exports split across separately imported headers. Each `import cpp` line
// is its own translation unit, so clang never sees the clash; including both halves in one C++ TU
// is ill-formed ("target of using declaration conflicts with declaration already in scope").
// _own.h declares cppuc::E / cppuc::Red / cppuc::S; _type.h re-exports a different E, _value.h a
// different Red (an enumerator), _class.h a different S, _template.h a different
// class template TT. This file holds the re-export targets.
namespace cppuc_src
{
    enum class E { A = 10, B = 20 };
    enum Col { Red = 7, Green = 8 };
    struct S { int v; };
    template<class T> struct TT { T v; };
}

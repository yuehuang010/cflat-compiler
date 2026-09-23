#pragma once

namespace cpplist {

inline int countedCtorCalls = 0;

struct Counted {
    int value;
    Counted() : value(70) { ++countedCtorCalls; }
    Counted(int v) : value(v) { ++countedCtorCalls; }
};
using CountedAlias = Counted;

struct Plain {
    int value;
    int marker;
};

inline int nontrivialCtorCalls = 0;
inline int nontrivialDtorCalls = 0;

struct Nontrivial {
    int value;
    Nontrivial(int v) : value(v) { ++nontrivialCtorCalls; }
    Nontrivial(const Nontrivial& other) : value(other.value) { ++nontrivialCtorCalls; }
    Nontrivial(Nontrivial&& other) noexcept : value(other.value) { ++nontrivialCtorCalls; }
    Nontrivial& operator=(const Nontrivial& other) { value = other.value; return *this; }
    Nontrivial& operator=(Nontrivial&& other) noexcept { value = other.value; return *this; }
    ~Nontrivial() { ++nontrivialDtorCalls; }
};

}

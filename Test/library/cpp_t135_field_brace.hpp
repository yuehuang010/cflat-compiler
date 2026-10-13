#pragma once
#include <initializer_list>

namespace t135 {
// Counted list-only class: only an initializer_list constructor.
inline int listOnlyCtors = 0;
inline int listOnlyDtors = 0;
struct ListOnly {
    int n;
    ListOnly(std::initializer_list<int> values) : n((int)values.size()) { ++listOnlyCtors; }
    ~ListOnly() { ++listOnlyDtors; }
};
inline void resetListOnly() { listOnlyCtors = 0; listOnlyDtors = 0; }
inline int listOnlyCtorCount() { return listOnlyCtors; }
inline int listOnlyDtorCount() { return listOnlyDtors; }

// Counted default + list class: the default constructor must win for an empty brace.
inline int defListDefaults = 0;
inline int defListLists = 0;
inline int defListDtors = 0;
struct DefList {
    int n;
    int which;
    DefList() : n(-1), which(0) { ++defListDefaults; }
    DefList(std::initializer_list<int> values) : n((int)values.size()), which(1) { ++defListLists; }
    ~DefList() { ++defListDtors; }
};
inline void resetDefList() { defListDefaults = 0; defListLists = 0; defListDtors = 0; }
inline int defListDefaultCount() { return defListDefaults; }
inline int defListListCount() { return defListLists; }
inline int defListDtorCount() { return defListDtors; }

// No default constructor and no list constructor: an empty brace has no constructor to call.
struct IntOnly {
    int n;
    IntOnly(int a, int b) : n(a + b) {}
};

// Self-pointing class: an array element built in a temporary keeps a dangling `me`.
inline int selfRefCtors = 0;
inline void resetSelfRef() { selfRefCtors = 0; }
inline int selfRefCtorCount() { return selfRefCtors; }
struct SelfRef {
    SelfRef* me;
    SelfRef() { me = this; ++selfRefCtors; }
    bool valid() const { return me == this; }
};

// T136: a list-only class as file-scope objects and array elements; counted so every
// construction and destruction is observable.
inline int t136Ctors = 0;
inline int t136Dtors = 0;
inline void resetT136() { t136Ctors = 0; t136Dtors = 0; }
inline int t136CtorCount() { return t136Ctors; }
inline int t136DtorCount() { return t136Dtors; }
struct T136List {
    int n;
    T136List(std::initializer_list<int> values) : n((int)values.size()) { ++t136Ctors; }
    ~T136List() { ++t136Dtors; }
};
}

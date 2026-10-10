#pragma once
#include <initializer_list>

namespace t63 {
struct VecPick {
    int first;
    int second;
    int which;
    VecPick(std::initializer_list<int> values) : first(0), second(0), which(1) {
        auto it = values.begin();
        if (it != values.end()) first = *it++;
        if (it != values.end()) second = *it;
    }
    // The default ctor makes the pre-fix silent default-construct arm observable (which == 0).
    VecPick() : first(-1), second(-1), which(0) {}
    VecPick(int n, int value) : first(n), second(value), which(2) {}
};

struct Aggregate {
    int first;
    int second;
};

struct TwoOnly {
    int first;
    int second;
    TwoOnly(int a, int b) : first(a), second(b) {}
};

struct ExplicitOnly {
    int first;
    int second;
    explicit ExplicitOnly(int a, int b) : first(a), second(b) {}
};

inline int countedCopies = 0;
inline int countedMoves = 0;
inline int countedDtors = 0;
struct Counted {
    int value;
    Counted(int n) : value(n) {}
    Counted(const Counted& other) : value(other.value) { ++countedCopies; }
    Counted(Counted&& other) : value(other.value) { ++countedMoves; other.value = -1; }
    ~Counted() { ++countedDtors; }
};
// A class-scope operator new hides the global placement form unless the thunk spells ::new.
struct ClassNew {
    int count;
    ClassNew(std::initializer_list<int> values) : count((int)values.size()) {}
    static void* operator new(decltype(sizeof(0)) size);
};
inline void resetCounted() { countedCopies = 0; countedMoves = 0; countedDtors = 0; }
inline int countedCopyCount() { return countedCopies; }
inline int countedMoveCount() { return countedMoves; }
inline int countedDtorCount() { return countedDtors; }
}

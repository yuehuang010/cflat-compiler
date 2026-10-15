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

inline int emptyOnlyCtors = 0;
inline int emptyOnlyDtors = 0;
struct EmptyOnly {
    int n;
    EmptyOnly(std::initializer_list<int> values) : n((int)values.size()) { ++emptyOnlyCtors; }
    ~EmptyOnly() { ++emptyOnlyDtors; }
};
inline void resetEmptyOnlyCounts() { emptyOnlyCtors = 0; emptyOnlyDtors = 0; }
inline int emptyOnlyCtorCount() { return emptyOnlyCtors; }
inline int emptyOnlyDtorCount() { return emptyOnlyDtors; }

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
inline int callCtors = 0;
inline int callCopies = 0;
inline int callMoves = 0;
inline int callDtors = 0;
inline int callSide = 0;
struct CallEvent {
    int value;
    CallEvent(int n) : value(n) { ++callCtors; callSide += 10; }
    CallEvent(const CallEvent& other) : value(other.value) { ++callCopies; }
    CallEvent(CallEvent&& other) : value(other.value) { ++callMoves; }
    ~CallEvent() { ++callDtors; }
};
struct CallSnapshot {
    int value;
    CallSnapshot(int n) : value(n) { ++callCtors; }
    CallSnapshot(const CallSnapshot& other) : value(other.value) { ++callCopies; }
    CallSnapshot(CallSnapshot&& other) : value(other.value) { ++callMoves; }
    ~CallSnapshot() { ++callDtors; }
};
inline void resetCallCounts() { callCtors = callCopies = callMoves = callDtors = callSide = 0; }
inline int callCtorCount() { return callCtors; }
inline int callCopyCount() { return callCopies; }
inline int callMoveCount() { return callMoves; }
inline int callDtorCount() { return callDtors; }
inline int callSideCount() { return callSide; }
inline int call_next_side() { return ++callSide; }
inline int call_bump_scalar(int* value) { ++*value; ++callSide; return *value; }
inline int call_event_pair(CallEvent x, CallEvent y) { return x.value * 1000 + y.value; }
inline int call_event_then_int(CallEvent x, int y) { return x.value * 1000 + y; }
inline int call_int_then_event(int x, CallEvent y) { return x * 1000 + y.value; }
inline int call_snapshot_then_int(CallSnapshot x, int y) { return x.value * 1000 + y; }
inline int call_event_value(CallEvent x) { return x.value; }
struct CallHost {
    int member(CallEvent x) { return x.value + 200; }
    int member_event(CallEvent x, int y) { return x.value * 1000 + y; }
    template<class T> int template_event(T x, int y) { return x.value * 1000 + y; }
};
// A class-scope operator new hides the global placement form unless the thunk spells ::new.
struct ClassNew {
    int count;
    ClassNew(std::initializer_list<int> values) : count((int)values.size()) {}
    static void* operator new(decltype(sizeof(0)) size);
};
// T146 r3: a `Fn&` constructor parameter binds the caller's function-pointer slot (nulls it).
using CallFnPtr = int(*)(int);
inline int call_plus_one(int x) { return x + 1; }
struct CallFnRef { int value; CallFnRef(CallFnPtr& f) : value(4) { f = nullptr; } };
inline int call_fnref_value(CallFnRef x) { return x.value; }
template<class T> int call_fnref_template(T x) { return x.value; }
// T146 r4: volatile and const volatile referents also bind the caller's slot.
struct CallFnVolRef { int value; CallFnVolRef(CallFnPtr volatile& f) : value(4) { f = nullptr; } };
inline CallFnPtr* cvExpected = nullptr;
inline void call_set_cv(CallFnPtr* p) { cvExpected = p; }
struct CallFnCvRef {
    int value;
    CallFnCvRef(CallFnPtr const volatile& f) : value((&f == cvExpected ? 100 : 200) + f(3)) {}
};
inline int call_fnref_volatile(CallFnVolRef x) { return x.value; }
inline int call_fnref_cv(CallFnCvRef x) { return x.value; }
inline void resetCounted() { countedCopies = 0; countedMoves = 0; countedDtors = 0; }
inline int countedCopyCount() { return countedCopies; }
inline int countedMoveCount() { return countedMoves; }
inline int countedDtorCount() { return countedDtors; }
inline int countedByVal(Counted value) { return value.value; }
}

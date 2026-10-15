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
struct CallChoice {
    int value;
    CallChoice(int a, int b) : value(100 + a * 10 + b) { ++callCtors; }
    CallChoice(const int& a, double b) : value(200 + a * 10 + (int)b) { ++callCtors; }
    CallChoice(const CallChoice& other) : value(other.value) { ++callCopies; }
    CallChoice(CallChoice&& other) : value(other.value) { ++callMoves; }
    ~CallChoice() { ++callDtors; }
};
inline void resetCallCounts() { callCtors = callCopies = callMoves = callDtors = callSide = 0; }
inline int callCtorCount() { return callCtors; }
inline int callCopyCount() { return callCopies; }
inline int callMoveCount() { return callMoves; }
inline int callDtorCount() { return callDtors; }
inline int callSideCount() { return callSide; }
inline int call_next_side() { return ++callSide; }
inline int call_bump_scalar(int* value) { ++*value; ++callSide; return *value; }
inline int call_bump_choice(int* value) { return ++*value; }
inline int call_event_pair(CallEvent x, CallEvent y) { return x.value * 1000 + y.value; }
inline int call_event_then_int(CallEvent x, int y) { return x.value * 1000 + y; }
inline int call_int_then_event(int x, CallEvent y) { return x * 1000 + y.value; }
inline int call_snapshot_then_int(CallSnapshot x, int y) { return x.value * 1000 + y; }
inline int call_event_value(CallEvent x) { return x.value; }
inline int call_choice_value(CallChoice x) { return x.value; }
template<class T> int call_choice_template(T x) { return x.value; }
struct CallT232BaseSel {
    int value;
    CallT232BaseSel(int a, int b) : value(100 + a * 10 + b) {}
    CallT232BaseSel(const int& a, double b) : value(200 + a * 10 + (int)b) {}
};
struct CallT232DefSel {
    int value;
    CallT232DefSel(int a, int b, int c = 0) : value(100 + a * 10 + b + c) {}
    CallT232DefSel(const int& a, double b) : value(200 + a * 10 + (int)b) {}
};
struct CallT232InhSel : CallT232BaseSel { using CallT232BaseSel::CallT232BaseSel; };
struct CallT232TplSel {
    int value;
    template<class U> CallT232TplSel(U a, int b) : value(100 + a * 10 + b) {}
    CallT232TplSel(const int& a, double b) : value(200 + a * 10 + (int)b) {}
};
struct CallT232VarSel {
    int value;
    CallT232VarSel(int a, int b, ...) : value(100 + a * 10 + b) {}
    CallT232VarSel(const int& a, double b) : value(200 + a * 10 + (int)b) {}
};
inline int call_t232_tplsel(CallT232TplSel x) { return x.value; }
inline int call_t232_varsel(CallT232VarSel x) { return x.value; }
// T232 r2: a converting W pick at a scalar position is built in the parameter (no copy).
inline int call_t232_w_copies = 0;
inline int call_t232_w_copy_count() { return call_t232_w_copies; }
struct CallT232W {
    int value;
    CallT232W(int x) : value(x) {}
    CallT232W(const CallT232W& o) : value(o.value) { ++call_t232_w_copies; }
};
struct CallT232WSel {
    int value;
    CallT232WSel(CallT232W a, int b) : value(100 + a.value * 10 + b) {}
    CallT232WSel(const int& a, const char* s) : value(200 + a) {}
};
struct CallHost {
    int member(CallEvent x) { return x.value + 200; }
    int member_event(CallEvent x, int y) { return x.value * 1000 + y; }
    int overloaded(CallEvent x, int y) { return 400 + x.value + y; }
    int overloaded(CallEvent x, double y) { return 500 + x.value + (int)y; }
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
struct CallT232FnPick {
    int value;
    CallT232FnPick(CallFnPtr& f) : value(100 + f(3)) {}
    CallT232FnPick(const CallFnPtr& f) : value(200 + f(3)) {}
    CallT232FnPick(CallFnPtr&& f) : value(300 + f(3)) {}
};
inline int call_t232_fn_pick(CallT232FnPick x) { return x.value; }
template<class T> int call_t232_fn_pick_template(T x) { return x.value; }
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
struct CallFnRvalueRef { int value; CallFnRvalueRef(CallFnPtr&& f) : value(300 + f(3)) { f = nullptr; } };
struct CallFnRvaluePick {
    int value;
    CallFnRvaluePick(CallFnPtr& f) : value(100 + f(3)) { f = nullptr; }
    CallFnRvaluePick(CallFnPtr&& f) : value(300 + f(3)) { f = nullptr; }
};
inline int call_fn_rvalue_ref(CallFnRvalueRef x) { return x.value; }
template<class T> int call_fn_rvalue_ref_template(T x) { return x.value; }
inline int call_fn_rvalue_pick(CallFnRvaluePick x) { return x.value; }
struct CallT248BoolRv {
    int value;
    CallT248BoolRv(bool b) : value(500 + (b ? 1 : 0)) {}
    CallT248BoolRv(CallFnPtr&& f) : value(300 + f(3)) {}
};
inline int call_t248_boolrv(CallT248BoolRv x) { return x.value; }
struct CallT248ValRv {
    int value;
    CallT248ValRv(CallFnPtr f) : value(400 + f(3)) {}
    CallT248ValRv(CallFnPtr&& f) : value(300 + f(3)) {}
};
inline int call_t248_valrv(CallT248ValRv x) { return x.value; }
inline int call_t248_free_pick(CallFnPtr& f) { return 100 + f(3); }
inline int call_t248_free_pick(const CallFnPtr& f) { return 200 + f(3); }
struct CallT248DataPick {
    int value;
    CallT248DataPick(int*& p) : value(100 + *p) {}
    CallT248DataPick(const int*& p) : value(200 + *p) {}
    CallT248DataPick(int*&& p) : value(300 + *p) {}
};
inline int call_t248_data_pick(CallT248DataPick p) { return p.value; }
struct CallFnRvalueTpl {
    int value;
    template<class F> CallFnRvalueTpl(F&& f) : value(600 + f(3)) {}
    CallFnRvalueTpl(CallFnPtr&& f) : value(300 + f(3)) {}
};
inline int call_fn_rvalue_tpl(CallFnRvalueTpl x) { return x.value; }
inline void resetCounted() { countedCopies = 0; countedMoves = 0; countedDtors = 0; }
inline int countedCopyCount() { return countedCopies; }
inline int countedMoveCount() { return countedMoves; }
inline int countedDtorCount() { return countedDtors; }
inline int countedByVal(Counted value) { return value.value; }
}

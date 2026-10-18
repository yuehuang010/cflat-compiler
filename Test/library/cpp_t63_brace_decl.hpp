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

// T278 macro: an integer macro constant that fits (clang oracle 1).
#define T278_ONE 1
#define T278_BIG 300

// T276: unscoped enum elements of brace lists (clang oracle in the leg comments).
namespace t276 {
enum E { e0, e1 };                       // unscoped, no fixed type: promotes to int
enum CU : unsigned char { cu0, cu1 };    // fixed: the underlying type matches exactly
enum FU : unsigned { fu0, fu1 };         // fixed unsigned: stays unsigned
enum FL : long long { fl0, fl1 };       // fixed long long: stays long long
enum class SC { s0, s1 };                // scoped: no promotion
enum SH : short { sh0, sh1 };            // fixed: the underlying short matches exactly
enum W16 : char16_t { w0, w1 };          // fixed char-type: keeps its own spelling
enum WW : wchar_t { ww0, ww1 };          // fixed char-type: keeps its own spelling
enum HG { h0, h1 = 0x100000000ll };      // no fixed type, value above UINT_MAX: promotes to long

struct A {
    int w;
    A(const int (&)[2]) : w(1) {}
    A(const unsigned (&)[2]) : w(2) {}
};
struct AL {
    int w;
    AL(const int (&)[2]) : w(1) {}
    AL(const long long (&)[2]) : w(3) {}
};
struct AC {
    int w;
    AC(const int (&)[2]) : w(1) {}
    AC(const unsigned char (&)[2]) : w(5) {}
};
struct SA {
    int w;
    SA(const int (&)[2]) : w(1) {}
    SA(const SC (&)[2]) : w(4) {}
};
struct L {
    int w;
    L(std::initializer_list<int>) : w(1) {}
    L(std::initializer_list<long>) : w(2) {}
};
struct A16 {
    int w;
    A16(const int (&)[2]) : w(1) {}
    A16(const char16_t (&)[2]) : w(7) {}
};
struct AW {
    int w;
    AW(const int (&)[2]) : w(1) {}
    AW(const wchar_t (&)[2]) : w(8) {}
};
struct AH {
    int w;
    AH(const int (&)[2]) : w(1) {}
#if !defined(_MSC_VER)
    // MSVC keeps a non-fixed enum int (value truncated), so the twin would redeclare AH(int[2]).
    AH(const decltype(+h1) (&)[2]) : w(3) {}   // LP64: the promoted 64-bit type
#endif
};
inline int fa(const int (&)[2]) { return 1; }
inline int fa(const unsigned (&)[2]) { return 2; }
// T278: an enumerator constant that fits is not narrowing in a brace element; `enum : bool` ranks as bool.
enum K65 { k0, k65 = 65 };                // unscoped, no fixed type: promotes to int
enum KBig { kb0, kb1 = 300 };             // 300 does not fit a char
enum KNeg { kn0 = -1, kn1 = 1 };          // -1 does not fit an unsigned char
enum KBool : bool { kf, kt };             // fixed bool: clang ranks bool over int
struct KChar {
    int w;
    KChar(const char (&a)[1]) : w(a[0]) {}
};
struct KUChar {
    int w;
    KUChar(const unsigned char (&a)[1]) : w(a[0]) {}
};
struct KBoolArr {
    int w;
    KBoolArr(const int (&)[2]) : w(1) {}
    KBoolArr(const bool (&)[2]) : w(11) {}
};
struct KBoolList {
    int w;
    KBoolList(std::initializer_list<int>) : w(1) {}
    KBoolList(std::initializer_list<bool>) : w(11) {}
};
inline int kChar(const char (&a)[1]) { return a[0]; }
inline int kUChar(const unsigned char (&a)[1]) { return a[0]; }
inline int ol(std::initializer_list<int>) { return 1; }
inline int ol(std::initializer_list<bool>) { return 11; }
inline int ol2(std::initializer_list<bool>) { return 11; }
inline int ol2(std::initializer_list<int>) { return 1; }
inline int ci(std::initializer_list<char>) { return 1; }
inline int ci(std::initializer_list<int>) { return 2; }
inline int li(std::initializer_list<long>) { return 1; }
inline int li(std::initializer_list<int>) { return 2; }
inline int iu(std::initializer_list<int>) { return 1; }
inline int iu(std::initializer_list<unsigned>) { return 2; }
enum class T308SC : int { s0, s1 };
inline int t308ScopedList(std::initializer_list<T308SC> values)
{
    return (int)*values.begin() + 7;
}
inline int t308ScopedIntList(std::initializer_list<int> values)
{
    return (int)*values.begin() + 20;
}
inline int t308ScopedUnsignedList(std::initializer_list<unsigned>) { return 3; }
inline int t308ScopedArithmeticList(std::initializer_list<int>) { return 1; }
inline int t308ScopedArithmeticList(std::initializer_list<unsigned>) { return 2; }
enum T308E : int { t308E0, t308E1 = 1 };
inline int t308EnumRank(std::initializer_list<T308E>) { return 101; }
inline int t308EnumRank(std::initializer_list<int>) { return 201; }
inline int t308UnscopedIntList(std::initializer_list<int> values)
{
    return (int)*values.begin() + 40;
}
inline int t308IntList(std::initializer_list<int> values)
{
    return (int)*values.begin() + 50;
}
// T308 round 2: enum elements never convert to another enum, and a scoped element never converts to a scalar.
enum T308E2 : int { t308F0 };
enum class T308C8 : char { c0, c1 };
enum class T308SC2 : int { u0, u1 };
inline int t308EnumOnlyList(std::initializer_list<T308E>) { return 301; }
inline int t308OtherEnumList(std::initializer_list<T308E2>) { return 302; }
inline int t308ScopedOtherList(std::initializer_list<T308SC2>) { return 303; }
inline int t308LLList(std::initializer_list<long long>) { return 304; }
inline int t308CharList(std::initializer_list<char>) { return 305; }
}

// T286: a CFlat-declared enum value (no C++ spelling) binds as its backing integer; an imported enum keeps its own overload.
namespace t286 {
enum IE : int { ie0, ie1 = 1 };
inline int ee(IE e) { return 100 + (int)e; }
inline int ee(int x) { return 200 + x; }
inline int si1(int x) { return x; }
inline int si1(const char (&a)[1]) { return 50 + a[0]; }
inline int mx(int a, int b) { return a * 10 + b; }
inline int mx(int a, const char (&b)[1]) { return a + b[0]; }
inline int sc1(const char (&a)[1]) { return a[0]; }
inline int sc7(const char (&a)[7]) { return 72 + a[0] - 'C'; }
inline int sc9(const char (&a)[9]) { return 92 + a[0] - 'C'; }
inline int ci(std::initializer_list<char>) { return 1; }
inline int ci(std::initializer_list<int>) { return 2; }
}
// T314: an equal-rank brace-list tie is ambiguous in clang (CFlat refuses it); a strictly better
// element match still wins. `long long` keeps the shapes the same on LLP64 Windows.
namespace t314 {
inline int ldb(std::initializer_list<long long>) { return 1; }
inline int ldb(std::initializer_list<double>) { return 2; }
inline int dll(std::initializer_list<double>) { return 1; }
inline int dll(std::initializer_list<long long>) { return 2; }
inline int ui(std::initializer_list<unsigned>) { return 1; }
inline int ui(std::initializer_list<long long>) { return 2; }
inline int id(std::initializer_list<int>) { return 1; }
inline int id(std::initializer_list<double>) { return 2; }
inline int fd(std::initializer_list<float>) { return 1; }
inline int fd(std::initializer_list<double>) { return 2; }
inline int bd(std::initializer_list<bool>) { return 1; }
inline int bd(std::initializer_list<double>) { return 2; }
struct S {
    int m_ldb(std::initializer_list<long long>) { return 1; }
    int m_ldb(std::initializer_list<double>) { return 2; }
    int m_ui(std::initializer_list<unsigned>) { return 1; }
    int m_ui(std::initializer_list<long long>) { return 2; }
    int m_pb(int, std::initializer_list<int>) { return 1; }
    int m_pb(long, std::initializer_list<int>) { return 2; }
    int m_rq(std::initializer_list<long>) & { return 1; }
    int m_rq(std::initializer_list<double>) && { return 2; }
};
inline int pb(int, std::initializer_list<int>) { return 1; }
inline int pb(long, std::initializer_list<int>) { return 2; }
}

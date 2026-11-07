#pragma once
// T78: C++ call arguments widen integer / bool / unscoped enum to floating, never narrow.
namespace t78 {
enum UA { UAV = 3 };
enum UB { UBV = 4 };
inline int takeD(double d) { return (int)d; }
inline int takeInt(int x) { return x; }
inline int takeUB(UB b) { return (int)b; }
struct Fl { int v; Fl(double x) : v((int)x) {} };
struct Mem { int dummy; int m(double d) { return (int)d; } };
}
// T78 round 2 (review 1): reference, variadic, template and arity shapes of the scalar table.
namespace t78 {
inline int takeCD(const double& x) { return (int)x; }
inline int takeMutD(double& x) { return (int)x; }
struct NC { int v; NC(const double& x) : v((int)x) {} };
int takeVarD(double d, ...) { return (int)d; }
enum TE { TEV = 3 };
enum TF { TFV = 4 };
inline int takeTF(TF, ...) { return 53; }
inline int takeShort(short x, ...) { return x; }
struct RefCtor { int v; RefCtor(const short& x) : v(x) {} };
struct Tmpl { int v; Tmpl(short x) : v(x) {} template <class T> Tmpl(T* p) : v(99) {} };
struct Arity { int v; Arity(short) : v(71) {} Arity(double, int) : v(72) {} };
}
namespace t78 {
// Round 3: a template parameter without '*' deduces a pointer; a template next to a short listed
// constructor; a concrete short template parameter; a pointer constructor beside a short one.
struct DeducePtr { int v; DeducePtr(int, int*) : v(99) {} template <class T> DeducePtr(double x, T) : v((int)x) {} };
struct FixedS { int v; template <class T> FixedS(short x, T) : v(x) {} };
struct PtrAlt { int v; PtrAlt(short x) : v(x) {} PtrAlt(int*) : v(99) {} };
inline int takeRvD(double&& x) { return (int)x; }
inline int takeRvI(int&& x) { return x; }
// Round 4: a typedef-spelled short template parameter; a null constant takes a pointer constructor.
using T78Small = short;
struct AliasS { int v; template <class T> AliasS(T78Small x, T) : v(x) {} };
struct NullAlt { int v; NullAlt(short, int) : v(11) {} NullAlt(int*, double) : v(99) {} };
// T109: fixed int-backed unscoped enums (every unscoped enum under MSVC) keep their identity.
enum FA : int { FAV = 5 };
enum FB : int { FBV = 6 };
inline int takeFB(FB b) { return (int)b; }
inline int takeFBInt(int x) { return x + 100; }
}
// CTORVIAB: a constructor template decides a narrowing only when clang selects it. Each class is one
// accept-set or refusal shape; test_cpp_interop.cb (accept legs) and err_cpp_ctor_selected_narrowing.cb.
namespace t78 {
template <bool B> struct CvIfT { };
template <> struct CvIfT<true> { using type = int; };
template <class T> struct CvIdentity { using type = T; };
struct CvAway { int v; CvAway(short x) : v(x) {} template <class T> requires (sizeof(T) == 1) CvAway(T) : v(99) {} };
struct CvEnableAway { int v; CvEnableAway(short x) : v(x) {} template <class T, typename CvIfT<sizeof(T) == 1>::type = 0> CvEnableAway(T) : v(99) {} };
struct CvExact { int v; CvExact(short x) : v(x) {} template <class T, typename CvIfT<sizeof(T) == 4>::type = 0> CvExact(T x) : v((int)x + 1000) {} };
struct CvReq { int v; CvReq(short x) : v(x) {} template <class T> requires (sizeof(T) == 4) CvReq(T x) : v((int)x + 2000) {} };
struct CvDeduce { int v; CvDeduce(short x) : v(x) {} template <class T> CvDeduce(T x) : v((int)x + 50) {} };
struct CvIdent { int v; CvIdent(short, int) : v(11) {} template <class T> CvIdent(typename CvIdentity<T>::type* p, T x) : v((int)x + 50) {} };
struct CvNull { int v; CvNull(short, int) : v(11) {} template <class T> CvNull(T*, double) : v(99) {} };
}

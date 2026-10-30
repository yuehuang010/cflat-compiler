#pragma once
#include <type_traits>

namespace cppamp {

struct CtorTemplate {
    int selected;
    CtorTemplate(int*&) : selected(1) {}
    template<class U> CtorTemplate(U&&) : selected(2) {}
    int get() const { return selected; }
};

struct CtorVoid {
    int selected;
    CtorVoid(int*&) : selected(1) {}
    CtorVoid(void*) : selected(3) {}
    int get() const { return selected; }
};

struct CtorBool {
    int selected;
    CtorBool(int*&) : selected(1) {}
    CtorBool(bool) : selected(4) {}
    int get() const { return selected; }
};

struct CtorConstVoid {
    int selected;
    CtorConstVoid(int*&) : selected(1) {}
    CtorConstVoid(const void*) : selected(5) {}
    int get() const { return selected; }
};

struct CtorPointerConst {
    int selected;
    CtorPointerConst(int*&) : selected(1) {}
    CtorPointerConst(int* const*) : selected(6) {}
    int get() const { return selected; }
};

struct CtorExact {
    int selected;
    CtorExact(int*&) : selected(1) {}
    CtorExact(int**) : selected(8) {}
    int get() const { return selected; }
};

struct SlotOnly {
    int*& p;
    explicit SlotOnly(int*& x) : p(x) {}
    int get() const { return *p; }
};

inline int freeTemplate(int*&) { return 1; }
template<class U> inline int freeTemplate(U&&) { return 2; }
inline int freeVoid(int*&) { return 1; }
inline int freeVoid(void*) { return 3; }
inline int freeBool(int*&) { return 1; }
inline int freeBool(bool) { return 4; }
inline int freeConstVoid(int*&) { return 1; }
inline int freeConstVoid(const void*) { return 5; }
inline int freePointerConst(int*&) { return 1; }
inline int freePointerConst(int* const*) { return 6; }
inline int freeExact(int*&) { return 1; }
inline int freeExact(int**) { return 8; }
inline int freeSlot(int*& p) { p = nullptr; return 7; }

struct Methods {
    int pickTemplate(int*&) { return 1; }
    template<class U> int pickTemplate(U&&) { return 2; }
    int pickVoid(int*&) { return 1; }
    int pickVoid(void*) { return 3; }
    int pickBool(int*&) { return 1; }
    int pickBool(bool) { return 4; }
    int pickConstVoid(int*&) { return 1; }
    int pickConstVoid(const void*) { return 5; }
    int pickPointerConst(int*&) { return 1; }
    int pickPointerConst(int* const*) { return 6; }
    int pickExact(int*&) { return 1; }
    int pickExact(int**) { return 8; }
    int pickSlot(int*& p) { p = nullptr; return 7; }
};

struct Multi {
    int selected;
    Multi(int*& p, int) : selected(9) { p = nullptr; }
    Multi(void*, const char*) : selected(3) {}
    int get() const { return selected; }
};
inline int oracleMulti() { int y = 9; int* p = &y; Multi o(p, 7); return o.get() + (p == nullptr ? 10 : 0); }

inline int oracleCtorTemplateDecl() {
    int y = 9; int* p = &y; CtorTemplate x = CtorTemplate(&p); return x.get();
}
inline int oracleCtorTemplateAuto() {
    int y = 9; int* p = &y; auto x = CtorTemplate(&p); return x.get();
}
inline int oracleCtorTemplateNew() {
    int y = 9; int* p = &y; auto* x = new CtorTemplate(&p); int v = x->get(); delete x; return v;
}
inline int oracleCtorTemplatePostfix() {
    int y = 9; int* p = &y; return CtorTemplate(&p).get();
}
inline int oracleCtorVoidDecl() {
    int y = 9; int* p = &y; CtorVoid x = CtorVoid(&p); return x.get();
}
inline int oracleCtorVoidNew() {
    int y = 9; int* p = &y; auto* x = new CtorVoid(&p); int v = x->get(); delete x; return v;
}
inline int oracleCtorBool() {
    int y = 9; int* p = &y; return CtorBool(&p).get();
}
inline int oracleCtorConstVoid() {
    int y = 9; int* p = &y; return CtorConstVoid(&p).get();
}
inline int oracleCtorPointerConst() {
    int y = 9; int* p = &y; return CtorPointerConst(&p).get();
}
inline int oracleCtorExact() {
    int y = 9; int* p = &y; return CtorExact(&p).get();
}
inline int oracleFreeTemplate() { int y = 9; int* p = &y; return freeTemplate(&p); }
inline int oracleFreeVoid() { int y = 9; int* p = &y; return freeVoid(&p); }
inline int oracleFreeBool() { int y = 9; int* p = &y; return freeBool(&p); }
inline int oracleFreeConstVoid() { int y = 9; int* p = &y; return freeConstVoid(&p); }
inline int oracleFreePointerConst() { int y = 9; int* p = &y; return freePointerConst(&p); }
inline int oracleFreeExact() { int y = 9; int* p = &y; return freeExact(&p); }
inline int oracleMethodTemplate() {
    int y = 9; int* p = &y; Methods m; return m.pickTemplate(&p);
}
inline int oracleMethodVoid() { int y = 9; int* p = &y; Methods m; return m.pickVoid(&p); }
inline int oracleMethodBool() { int y = 9; int* p = &y; Methods m; return m.pickBool(&p); }
inline int oracleMethodConstVoid() {
    int y = 9; int* p = &y; Methods m; return m.pickConstVoid(&p);
}
inline int oracleMethodPointerConst() {
    int y = 9; int* p = &y; Methods m; return m.pickPointerConst(&p);
}
inline int oracleMethodExact() { int y = 9; int* p = &y; Methods m; return m.pickExact(&p); }
inline int oracleSlotCtor() { int y = 9; int* p = &y; SlotOnly x(p); return x.get(); }
inline int oracleFreeSlot() {
    int y = 9; int* p = &y; int v = freeSlot(p); return v + (p == nullptr ? 10 : 0);
}
inline int oracleMethodSlot() {
    int y = 9; int* p = &y; Methods m; int v = m.pickSlot(p); return v + (p == nullptr ? 10 : 0);
}

struct Amp2Payload { int value; };

struct Amp2MethodCases {
    int urefInt(int*&) { return 1; }
    template<class U> int urefInt(U&&) { return 2; }
    int ustarInt(int*&) { return 1; }
    template<class U> int ustarInt(U*) { return 3; }
    int urefClass(Amp2Payload*&) { return 1; }
    template<class U> int urefClass(U&&) { return 2; }
    int ustarClass(Amp2Payload*&) { return 1; }
    template<class U> int ustarClass(U*) { return 3; }
    int mixedInt(int*& p, int** q) { p = nullptr; return **q; }
    int mixedClass(Amp2Payload*& p, Amp2Payload** q) { p = nullptr; return (*q)->value; }
};

struct Amp2StaticCases {
    static int urefInt(int*&) { return 1; }
    template<class U> static int urefInt(U&&) { return 2; }
    static int ustarInt(int*&) { return 1; }
    template<class U> static int ustarInt(U*) { return 3; }
    static int urefClass(Amp2Payload*&) { return 1; }
    template<class U> static int urefClass(U&&) { return 2; }
    static int ustarClass(Amp2Payload*&) { return 1; }
    template<class U> static int ustarClass(U*) { return 3; }
    static int mixedInt(int*& p, int** q) { p = nullptr; return **q; }
    static int mixedClass(Amp2Payload*& p, Amp2Payload** q) { p = nullptr; return (*q)->value; }
};

struct Amp2CtorUStarArityInt {
    int selected;
    Amp2CtorUStarArityInt(int*& p) : selected(9) { p = nullptr; }
    template<class U> Amp2CtorUStarArityInt(U*, int) : selected(2) {}
};
struct Amp2CtorUStarArityClass {
    int selected;
    Amp2CtorUStarArityClass(Amp2Payload*& p) : selected(9) { p = nullptr; }
    template<class U> Amp2CtorUStarArityClass(U*, int) : selected(2) {}
};
struct Amp2CtorURefArityInt {
    int selected;
    Amp2CtorURefArityInt(int*& p) : selected(9) { p = nullptr; }
    template<class U> Amp2CtorURefArityInt(U&&, int) : selected(2) {}
};
struct Amp2CtorMixedInt {
    int selected;
    Amp2CtorMixedInt(int*& p, int** q) : selected(**q) { p = nullptr; }
};
struct Amp2CtorMixedClass {
    int selected;
    Amp2CtorMixedClass(Amp2Payload*& p, Amp2Payload** q) : selected((*q)->value) { p = nullptr; }
};

inline int amp2FreeUStarArityInt(int*& p) { p = nullptr; return 9; }
template<class U> inline int amp2FreeUStarArityInt(U*, int) { return 2; }
inline int amp2FreeUStarArityClass(Amp2Payload*& p) { p = nullptr; return 9; }
template<class U> inline int amp2FreeUStarArityClass(U*, int) { return 2; }
inline int amp2FreeURefArityInt(int*& p) { p = nullptr; return 9; }
template<class U> inline int amp2FreeURefArityInt(U&&, int) { return 2; }
inline int amp2FreeMixedInt(int*& p, int** q) { p = nullptr; return **q; }
inline int amp2FreeMixedClass(Amp2Payload*& p, Amp2Payload** q) { p = nullptr; return (*q)->value; }

inline int oracleAmp2CtorUStarArityInt() {
    int x = 4; int* p = &x; Amp2CtorUStarArityInt o(p); return o.selected + (p == nullptr ? 10 : 0);
}
inline int oracleAmp2CtorUStarArityClass() {
    Amp2Payload x{4}; Amp2Payload* p = &x; Amp2CtorUStarArityClass o(p); return o.selected + (p == nullptr ? 10 : 0);
}
inline int oracleAmp2CtorURefArityInt() {
    int x = 4; int* p = &x; Amp2CtorURefArityInt o(p); return o.selected + (p == nullptr ? 10 : 0);
}
inline int oracleAmp2FreeUStarArityInt() {
    int x = 4; int* p = &x; int v = amp2FreeUStarArityInt(p); return v + (p == nullptr ? 10 : 0);
}
inline int oracleAmp2FreeUStarArityClass() {
    Amp2Payload x{4}; Amp2Payload* p = &x; int v = amp2FreeUStarArityClass(p); return v + (p == nullptr ? 10 : 0);
}
inline int oracleAmp2FreeURefArityInt() {
    int x = 4; int* p = &x; int v = amp2FreeURefArityInt(p); return v + (p == nullptr ? 10 : 0);
}
inline int oracleAmp2MethodURefInt() {
    int x = 4; int* p = &x; Amp2MethodCases m; return m.urefInt(&p);
}
inline int oracleAmp2MethodUStarInt() {
    int x = 4; int* p = &x; Amp2MethodCases m; return m.ustarInt(&p);
}
inline int oracleAmp2MethodURefClass() {
    Amp2Payload x{4}; Amp2Payload* p = &x; Amp2MethodCases m; return m.urefClass(&p);
}
inline int oracleAmp2MethodUStarClass() {
    Amp2Payload x{4}; Amp2Payload* p = &x; Amp2MethodCases m; return m.ustarClass(&p);
}
inline int oracleAmp2StaticURefInt() {
    int x = 4; int* p = &x; return Amp2StaticCases::urefInt(&p);
}
inline int oracleAmp2StaticUStarInt() {
    int x = 4; int* p = &x; return Amp2StaticCases::ustarInt(&p);
}
inline int oracleAmp2StaticURefClass() {
    Amp2Payload x{4}; Amp2Payload* p = &x; return Amp2StaticCases::urefClass(&p);
}
inline int oracleAmp2StaticUStarClass() {
    Amp2Payload x{4}; Amp2Payload* p = &x; return Amp2StaticCases::ustarClass(&p);
}
inline int oracleAmp2CtorMixedInt() {
    int a = 4; int b = 7; int* p = &a; int* q = &b; Amp2CtorMixedInt o(p, &q); return o.selected + (p == nullptr ? 10 : 0);
}
inline int oracleAmp2CtorMixedClass() {
    Amp2Payload a{4}; Amp2Payload b{7}; Amp2Payload* p = &a; Amp2Payload* q = &b; Amp2CtorMixedClass o(p, &q); return o.selected + (p == nullptr ? 10 : 0);
}
inline int oracleAmp2MethodMixedInt() {
    int a = 4; int b = 7; int* p = &a; int* q = &b; Amp2MethodCases m; int v = m.mixedInt(p, &q); return v + (p == nullptr ? 10 : 0);
}
inline int oracleAmp2MethodMixedClass() {
    Amp2Payload a{4}; Amp2Payload b{7}; Amp2Payload* p = &a; Amp2Payload* q = &b; Amp2MethodCases m; int v = m.mixedClass(p, &q); return v + (p == nullptr ? 10 : 0);
}
inline int oracleAmp2StaticMixedInt() {
    int a = 4; int b = 7; int* p = &a; int* q = &b; int v = Amp2StaticCases::mixedInt(p, &q); return v + (p == nullptr ? 10 : 0);
}
inline int oracleAmp2StaticMixedClass() {
    Amp2Payload a{4}; Amp2Payload b{7}; Amp2Payload* p = &a; Amp2Payload* q = &b; int v = Amp2StaticCases::mixedClass(p, &q); return v + (p == nullptr ? 10 : 0);
}
inline int oracleAmp2FreeMixedInt() {
    int a = 4; int b = 7; int* p = &a; int* q = &b; int v = amp2FreeMixedInt(p, &q); return v + (p == nullptr ? 10 : 0);
}
inline int oracleAmp2FreeMixedClass() {
    Amp2Payload a{4}; Amp2Payload b{7}; Amp2Payload* p = &a; Amp2Payload* q = &b; int v = amp2FreeMixedClass(p, &q); return v + (p == nullptr ? 10 : 0);
}

struct Amp2CtorPack {
    int selected;
    Amp2CtorPack(int*& p) : selected(1) { p = nullptr; }
    template<class U, class... R> Amp2CtorPack(U*, R...) : selected(3) {}
};
inline int oracleAmp2CtorPack() {
    int y = 4; int* p = &y; Amp2CtorPack c(&p);
    return c.selected + (p == nullptr ? 10 : 0);
}

struct Amp2R5CtorTwoLead {
    int selected;
    Amp2R5CtorTwoLead(int*& p) : selected(1) { p = nullptr; }
    template<class T, class U, class... R> Amp2R5CtorTwoLead(T, U*, R...) : selected(3) {}
};
struct Amp2R5CtorRequiredMid {
    int selected;
    Amp2R5CtorRequiredMid(int*& p) : selected(1) { p = nullptr; }
    template<class U, class... R> Amp2R5CtorRequiredMid(U*, int, R...) : selected(3) {}
};
struct Amp2R5CtorPackLRef {
    int selected;
    Amp2R5CtorPackLRef(int*& p) : selected(1) { p = nullptr; }
    template<class... A> Amp2R5CtorPackLRef(A&...) : selected(3) {}
};
inline int amp2R5FreeFallback(int*& p) { p = nullptr; return 1; }
template<class U> inline int amp2R5FreeFallback(U&) { return 2; }
inline int amp2R5FreeSuccess(int*& p) { p = nullptr; return 1; }
template<class U> inline int amp2R5FreeSuccess(U*) { return 3; }
struct Amp2R5CtorMultiFallback {
    int selected;
    Amp2R5CtorMultiFallback(int*& p) : selected(1) { p = nullptr; }
    template<class U> Amp2R5CtorMultiFallback(U&) : selected(2) {}
};
inline int oracleAmp2R5CtorTwoLead() {
    int y = 4; int* p = &y; Amp2R5CtorTwoLead c(p);
    return c.selected + (p == nullptr ? 10 : 0);
}
inline int oracleAmp2R5CtorRequiredMid() {
    int y = 4; int* p = &y; Amp2R5CtorRequiredMid c(p);
    return c.selected + (p == nullptr ? 10 : 0);
}
inline int oracleAmp2R5CtorPackLRef() {
    int y = 4; int* p = &y; Amp2R5CtorPackLRef c(p);
    return c.selected + (p == nullptr ? 10 : 0);
}
inline int oracleAmp2R5MultiFallbackSuccessFallback() {
    int y = 4; int* p = &y; int* q = &y; int* r = &y;
    int a = amp2R5FreeFallback(p);
    int b = amp2R5FreeSuccess(&q);
    Amp2R5CtorMultiFallback c(r);
    return a + (p == nullptr ? 10 : 0) + b * 20 + (q != nullptr ? 0 : 100)
        + c.selected * 3 + (r != nullptr ? 0 : 50);
}

inline int amp2FreeURefFallback(int*& p) { p = nullptr; return 1; }
template<class U> inline int amp2FreeURefFallback(U&) { return 2; }
inline int oracleAmp2FreeURefFallback() {
    int y = 4; int* p = &y; int v = amp2FreeURefFallback(p);
    return v + (p == nullptr ? 10 : 0);
}

inline int amp2RequiresFreeFallback(int*& p) { p = nullptr; return 1; }
template<class U> requires (!std::is_pointer_v<std::remove_cvref_t<U>>)
inline int amp2RequiresFreeFallback(U&&) { return 2; }
inline int oracleAmp2RequiresFreeFallback() {
    int y = 4; int* p = &y; int v = amp2RequiresFreeFallback(p);
    return v + (p == nullptr ? 10 : 0);
}

struct Amp2RequiresCtorFallback {
    int selected;
    Amp2RequiresCtorFallback(int*& p) : selected(1) { p = nullptr; }
    template<class U> requires (!std::is_pointer_v<std::remove_cvref_t<U>>)
    Amp2RequiresCtorFallback(U&&) : selected(2) {}
};
inline int oracleAmp2RequiresCtorFallback() {
    int y = 4; int* p = &y; Amp2RequiresCtorFallback c(p);
    return c.selected + (p == nullptr ? 10 : 0);
}

struct Amp2Field { int* fp; };
inline int amp2FieldTemplate(int*& p) { p = nullptr; return 1; }
template<class U> inline int amp2FieldTemplate(U&&) { return 2; }
inline int amp2FieldSlot(int*& p) { p = nullptr; return 1; }
inline int oracleAmp2FieldTemplate() {
    int y = 4; Amp2Field h{&y}; int v = amp2FieldTemplate(&h.fp);
    return v + (h.fp == nullptr ? 10 : 0);
}
inline int oracleAmp2FieldSlot() {
    int y = 4; Amp2Field h{&y}; int v = amp2FieldSlot(h.fp);
    return v + (h.fp == nullptr ? 10 : 0);
}

}

// AMP2 round 3: refusal shapes (ambiguous/deleted/static_assert/private templates, const pointer field).

namespace amp2r3 {
inline int free_ambiguous(int*& p) { p = nullptr; return 1; }
template<class U> int free_ambiguous(U*, int = 0) { return 3; }
template<class U> int free_ambiguous(U*, long = 0) { return 4; }

inline int free_deleted(int*& p) { p = nullptr; return 1; }
template<class U> int free_deleted(U&&) = delete;

inline int free_body(int*& p) { p = nullptr; return 1; }
template<class U> int free_body(U* u) {
    static_assert(sizeof(U) == 0, "no");
    return 3;
}

struct CtorDeleted {
    int value;
    CtorDeleted(int*& p) : value(1) { p = nullptr; }
    template<class U> CtorDeleted(U&&) = delete;
};

struct CtorPrivate {
    int value;
    CtorPrivate(int*& p) : value(1) { p = nullptr; }
private:
    template<class U> CtorPrivate(U*) : value(3) {}
};

struct MethodDeleted {
    int f(int*& p) { p = nullptr; return 1; }
    template<class U> int f(U&&) = delete;
};

struct CtorConstPointerSlot { CtorConstPointerSlot(int*& p) { p = nullptr; } };
struct MethodConstPointerSlot { int f(int*& p) { p = nullptr; return 1; } };

struct H { int* const fp; };
inline H make_h(int* p) { return H{p}; }
inline int free_const_pointer(int*& p) { p = nullptr; return 1; }

inline int nested_helper(int) { return 0; }

inline int free_nested_nomatch(int*& p) { p = nullptr; return 1; }
template<class U> int free_nested_nomatch(U* u) { return nested_helper(u) + 3; }

struct CtorNestedNomatch {
    int value;
    CtorNestedNomatch(int*& p) : value(1) { p = nullptr; }
    template<class U> CtorNestedNomatch(U* u) : value(nested_helper(u) + 3) {}
};

struct MethodNestedNomatch {
    int f(int*& p) { p = nullptr; return 1; }
    template<class U> int f(U* u) { return nested_helper(u) + 3; }
};

inline int free_l10(int*& p, int k) { p = nullptr; return 1; }
template<class U> int free_l10(U*, int*) { return 3; }
inline int oracle_free_l10() {
    int value = 4;
    int* p = &value;
    int result = free_l10(p, 2);
    return result + (p == nullptr ? 10 : 0);
}
}

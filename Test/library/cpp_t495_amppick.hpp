#pragma once

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

}

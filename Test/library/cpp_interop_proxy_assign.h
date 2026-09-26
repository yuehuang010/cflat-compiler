#pragma once

namespace cpppa {
inline int proxy_slots[4] = {};
inline int proxy_dtor_count = 0;

struct AssignProxy {
    int* slot;
    AssignProxy(int* p) : slot(p) {}
    ~AssignProxy() { proxy_dtor_count++; }
    AssignProxy& operator=(int value) { *slot = value; return *this; }
    operator int() const { return *slot; }
};
struct AssignBox {
    AssignProxy operator[](int index) { return AssignProxy(&proxy_slots[index]); }
};

struct VoidProxy {
    int* slot;
    VoidProxy(int* p) : slot(p) {}
    void operator=(int value) { *slot = value; }
    operator int() const { return *slot; }
};
struct VoidBox {
    VoidProxy operator[](int index) { return VoidProxy(&proxy_slots[index]); }
};

struct CompoundProxy {
    int* slot;
    CompoundProxy(int* p) : slot(p) {}
    void operator|=(int value) { *slot |= value; }
    operator int() const { return *slot; }
};
struct CompoundBox {
    CompoundProxy operator[](int index) { return CompoundProxy(&proxy_slots[index]); }
};

struct TemplateAssignProxy {
    int* slot;
    TemplateAssignProxy(int* p) : slot(p) {}
    template <typename T> void operator=(T value) { *slot = (int)value; }
    operator int() const { return *slot; }
};
struct TemplateAssignBox {
    TemplateAssignProxy operator[](int index) { return TemplateAssignProxy(&proxy_slots[index]); }
};

struct ImplicitProxy { int value = 0; ~ImplicitProxy() {} };
struct ImplicitBox {
    ImplicitProxy operator[](int) { return {}; }
};

struct AmbiguousProxy {
    int value;
    AmbiguousProxy(int x) : value(x) {}
    operator int() const { return value; }
    operator long() const { return value; }
};
struct AmbiguousBox {
    AmbiguousProxy operator[](int index) { return AmbiguousProxy(index); }
};

struct MemberEqProxy {
    int value;
    operator bool() const { return value != 0; }
    bool operator==(bool rhs) const { return (value != 0) == rhs; }
};
struct MemberEqBox {
    MemberEqProxy operator[](int index) { return {index}; }
};

struct ConvertProxy {
    int value;
    operator bool() const { return value != 0; }
};
struct ConvertBox {
    ConvertProxy operator[](int index) { return {index}; }
};

struct BoolOnlyProxy {
    int value;
    operator bool() const { return value != 0; }
};
struct BoolOnlyBox {
    BoolOnlyProxy operator[](int index) { return {index}; }
};

struct IntOnlyProxy {
    int value;
    operator int() const { return value; }
};
struct IntOnlyBox {
    IntOnlyProxy operator[](int index) { return {index}; }
};

inline int proxy_int_slots[4] = {};
struct ProxyInt {
    int* slot;
    ProxyInt(int* p) : slot(p) {}
    ProxyInt& operator=(int value) { *slot = value; return *this; }
    operator int() const { return *slot; }
};
struct ProxyIntBox {
    ProxyInt operator[](int index) { return ProxyInt(&proxy_int_slots[index]); }
};

struct FreeEqProxy {
    int value;
    operator bool() const { return value != 0; }
};
inline bool operator==(FreeEqProxy lhs, bool rhs) { return (bool)lhs == rhs; }
struct FreeEqBox {
    FreeEqProxy operator[](int index) { return {index}; }
};
// --- Per-argument overload ranking ([over.match.best]): every argument's conversion sequence is
// compared on its own; better at one argument and worse at another is ambiguous.
struct RankP { int v; RankP() : v(1) {} operator int() const { return 5; } };
struct RankB { int v; RankB() : v(1) {} operator int() const { return 0; } operator bool() const { return true; } };
inline int rank_mix(int a, double b) { return 1; }
inline int rank_mix(const RankP& a, float b) { return 2; }
inline int rank_mxi(int a, int b) { return 80; }
inline int rank_mxi(const RankP& a, long b) { return 81; }
inline int rank_mxf(int a, float b) { return 82; }
inline int rank_mxf(const RankP& a, double b) { return 83; }
inline int rank_euf(int a, float b) { return 7; }
inline int rank_euf(int a, double b) { return 8; }
inline int rank_euf2(int a, double b) { return 8; }
inline int rank_euf2(int a, float b) { return 7; }
inline int rank_fd(double x) { return 10; }
inline int rank_fd(float x) { return 11; }
inline int rank_hl(long x) { return 30; }
inline int rank_hl(double x) { return 31; }
inline int rank_sx(float a, long b) { return 92; }
inline int rank_sx(double a, int b) { return 93; }
inline int rank_amb(double x) { return 40; }
inline int rank_amb(char x) { return 41; }
// C++ picks rank_k(double, int) for (int, int); CFlat refuses int -> double at a call.
inline int rank_k(long a, long b) { return 50; }
inline int rank_k(double a, int b) { return 51; }
// Member forms: the implicit object parameter is ranked like any other argument.
struct RankM {
    int bf(bool b) { return 1; }
    int bf(int i) { return 2; }
    int e(int a, float b) { return 3; }
    int e(int a, double b) { return 4; }
    int d(double x) { return 5; }
    int d(float x) { return 6; }
    int g(int a, double b) { return 7; }
    int g(long a, float b) { return 8; }
};
// Same-rank [over.ics.rank] tie-breakers: qualification and reference cv (3.2.5 / 3.2.6),
// derived-to-base distance and base over void* (4.4), the implicit object parameter; and the
// cells clang calls ambiguous because nothing tells the candidates apart.
struct RkW { int v; RkW() : v(1) {} };
struct RkBase { int b; RkBase() : b(1) {} };
struct RkMid : RkBase { int m; RkMid() : m(2) {} };
struct RkBottom : RkMid { int t; RkBottom() : t(3) {} };
inline int rk_qp(int* p) { return 31; }
inline int rk_qp(const int* p) { return 32; }
inline int rk_pv(void* p) { return 95; }
inline int rk_pv(const RkW* p) { return 96; }
inline int rk_bv(RkBase* p) { return 97; }
inline int rk_bv(void* p) { return 98; }
inline int rk_ul(unsigned x) { return 1; }
inline int rk_ul(long x) { return 2; }
inline int rk_wc(RkW& a, const RkW& b) { return 13; }
inline int rk_wc(const RkW& a, RkW& b) { return 14; }
inline int rk_dx(RkBase* a, RkMid* b) { return 25; }
inline int rk_dx(RkMid* a, RkBase* b) { return 26; }
inline int rk_qc(int* a, const int* b) { return 33; }
inline int rk_qc(const int* a, int* b) { return 34; }
inline int rk_qn(RkW* p) { return 35; }
inline int rk_qn(const RkW* p) { return 36; }
inline int rk_us(long x) { return 41; }
inline int rk_us(double x) { return 42; }
inline int rk_dflt(int, int = 0) { return 81; }
inline int rk_dflt(int, int = 0, int = 0) { return 82; }
struct RkObj {
    int f(int x) const { return 51; }
    int f(long x) { return 52; }
};
struct RkHB {
    int h(unsigned x) { return 61; }
    int h(long x) { return 62; }
};
struct RkHD : RkHB { int own; RkHD() : own(0) {} };
struct RkTgt;
struct RkSrc { int v; RkSrc() : v(0) {} operator RkTgt() const; };
struct RkTgt { int t; RkTgt() : t(0) {} RkTgt(const RkSrc&) : t(1) {} };
inline RkSrc::operator RkTgt() const { return {}; }
inline int rk_conv(RkTgt t) { return 71 + t.t; }
inline int rk_conv_ref(const RkTgt& t) { return 73 + t.t; }
// Qualification on conversion sequences: T* -> void* vs T* -> const void*, and derived-to-base
// into Base* / const Base* or Base& / const Base&; the less qualified target wins (3.2.5 / 3.2.6).
inline int rk_vp(void* p) { return 21; }
inline int rk_vp(const void* p) { return 22; }
inline int rk_bc(RkBase* p) { return 50; }
inline int rk_bc(const RkBase* p) { return 51; }
inline int rk_br(RkBase& b) { return 52; }
inline int rk_br(const RkBase& b) { return 53; }
// Remaining pick cells: string literal, unscoped enum promotion, member name hiding, template
// vs non-template, non-const conversion operator vs const-reference converting constructor.
inline int rk_cc(char* p) { return 5; }
inline int rk_cc(const char* p) { return 6; }
enum RkE { RK_EA, RK_EB };
enum RkSmall : unsigned char { RK_SA };
inline int rk_en(int x) { return 9; }
inline int rk_en(long x) { return 10; }
inline int rk_sm(unsigned char x) { return 11; }
inline int rk_sm(int x) { return 12; }
struct RkHideBase { int f(int) { return 60; } int f(double) { return 61; } int g(int) { return 62; } };
struct RkHide : RkHideBase { int f(long) { return 63; } };
struct RkHideMore : RkHide { int h() { return 64; } };
struct RkUsing : RkHideBase { using RkHideBase::f; int f(long) { return 65; } };
template <typename T> inline int rk_tp(T x) { return 70; }
inline int rk_tp(long x) { return 71; }
template <typename T> inline int rk_tn(T x) { return 72; }
inline int rk_tn(int x) { return 73; }
struct RkSource;
struct RkTarget { int k; RkTarget() : k(0) {} RkTarget(const RkSource&) : k(74) {} };
struct RkSource { int s; RkSource() : s(0) {} operator RkTarget() { RkTarget t; t.k = 75; return t; } };
inline int rk_take(RkTarget t) { return t.k; }
}

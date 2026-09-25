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
}

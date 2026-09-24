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
}

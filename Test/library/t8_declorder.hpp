#pragma once
#include <cstddef>
namespace t8 {
namespace msvc {
template<class T> struct Shared { T* p; Shared(std::nullptr_t): p(nullptr) {} };
template<class T1, class T2> int operator==(const Shared<T1>& a, const Shared<T2>& b) { return a.p == b.p ? 101 : 100; }
template<class T> int operator==(const Shared<T>& a, std::nullptr_t) { return a.p == nullptr ? 102 : 103; }
template<class T1, class T2> int operator!=(const Shared<T1>& a, const Shared<T2>& b) { return a.p != b.p ? 104 : 105; }
template<class T> int operator!=(const Shared<T>& a, std::nullptr_t) { return a.p != nullptr ? 106 : 107; }
template<class T1, class T2> int operator<(const Shared<T1>& a, const Shared<T2>& b) { return a.p < b.p ? 108 : 109; }
template<class T> int operator<(const Shared<T>& a, std::nullptr_t) { return a.p == nullptr ? 110 : 111; }
inline Shared<int> shared(nullptr);
struct Ref { int tag = 9; };
inline int f(const Ref&) { return 120; }
inline int f(std::nullptr_t) { return 121; }
inline int operator==(const Ref& a, const Ref& b) { return a.tag == b.tag ? 101 : 100; }
inline int operator==(const Ref& a, std::nullptr_t) { return a.tag == 9 ? 102 : 103; }
inline int operator==(const Ref& a, int* p) { return p == nullptr ? 112 : 113; }
inline int operator!=(const Ref& a, const Ref& b) { return a.tag != b.tag ? 104 : 105; }
inline int operator!=(const Ref& a, std::nullptr_t) { return a.tag != 9 ? 106 : 107; }
inline int operator<(const Ref& a, const Ref& b) { return a.tag < b.tag ? 108 : 109; }
inline int operator<(const Ref& a, std::nullptr_t) { return a.tag == 9 ? 110 : 111; }
inline Ref ref{};
struct RefConv { int tag; RefConv(std::nullptr_t): tag(160) {} };
inline int f_conv(const RefConv& x) { return x.tag; }
}
namespace libcxx {
template<class T> struct Shared { T* p; Shared(std::nullptr_t): p(nullptr) {} };
template<class T> int operator==(const Shared<T>& a, std::nullptr_t) { return a.p == nullptr ? 102 : 103; }
template<class T1, class T2> int operator==(const Shared<T1>& a, const Shared<T2>& b) { return a.p == b.p ? 101 : 100; }
template<class T> int operator!=(const Shared<T>& a, std::nullptr_t) { return a.p != nullptr ? 106 : 107; }
template<class T1, class T2> int operator!=(const Shared<T1>& a, const Shared<T2>& b) { return a.p != b.p ? 104 : 105; }
template<class T> int operator<(const Shared<T>& a, std::nullptr_t) { return a.p == nullptr ? 110 : 111; }
template<class T1, class T2> int operator<(const Shared<T1>& a, const Shared<T2>& b) { return a.p < b.p ? 108 : 109; }
struct Ref { int tag = 9; };
inline int f(std::nullptr_t) { return 121; }
inline int f(const Ref&) { return 120; }
inline int operator==(const Ref& a, std::nullptr_t) { return a.tag == 9 ? 102 : 103; }
inline int operator==(const Ref& a, int* p) { return p == nullptr ? 112 : 113; }
inline int operator==(const Ref& a, const Ref& b) { return a.tag == b.tag ? 101 : 100; }
inline int operator!=(const Ref& a, std::nullptr_t) { return a.tag != 9 ? 106 : 107; }
inline int operator!=(const Ref& a, const Ref& b) { return a.tag != b.tag ? 104 : 105; }
inline int operator<(const Ref& a, std::nullptr_t) { return a.tag == 9 ? 110 : 111; }
inline int operator<(const Ref& a, const Ref& b) { return a.tag < b.tag ? 108 : 109; }
inline Ref ref{};
struct RefConv { int tag; RefConv(std::nullptr_t): tag(160) {} };
inline int f_conv(const RefConv& x) { return x.tag; }
}
struct CtorPick { int tag; CtorPick(std::nullptr_t): tag(150) {} CtorPick(int*): tag(151) {} };
inline CtorPick ctorPick(nullptr);
struct IntBox { int n; };
inline IntBox constBox{40};
inline IntBox mutBox{41};
struct MsvcOrder {
    int get() const { return 130; } int get() { return 131; }
    int begin() const { return 130; } int begin() { return 131; }
    int operator[](std::size_t) const { return 130; } int operator[](std::size_t) { return 131; }
    int operator*() const { return 130; } int operator*() { return 131; }
    IntBox* operator->() const { return &constBox; } IntBox* operator->() { return &mutBox; }
};
struct LibcxxOrder {
    int get() { return 141; } int get() const { return 140; }
    int begin() { return 141; } int begin() const { return 140; }
    int operator[](std::size_t) { return 141; } int operator[](std::size_t) const { return 140; }
    int operator*() { return 141; } int operator*() const { return 140; }
    IntBox* operator->() { return &mutBox; } IntBox* operator->() const { return &constBox; }
};
struct Holder { const MsvcOrder msvc; const LibcxxOrder libcxx; Holder(): msvc(), libcxx() {} };
inline MsvcOrder mutableMsvc;
inline LibcxxOrder mutableLibcxx;
inline const MsvcOrder constMsvc;
inline const LibcxxOrder constLibcxx;
inline const Holder constHolder;
}

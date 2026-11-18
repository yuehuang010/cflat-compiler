#pragma once
#include <cstring>

// `const char *const &` beside `T (&)[N]`, a by-value pointer and a template, on the free,
// member and constructor paths (PTRREF). Each result is 100 * overload + strlen when it reads p.
namespace cpp_ptrref {
inline int len(const char* const& p) { return p ? (int)std::strlen(p) : 9; }
struct Sv { const char* d; Sv(const char* s) : d(s) {} };
inline int f3(const char* const& p) { return 100 + len(p); }
inline int f3(const char (&)[4]) { return 200; }
inline int f4(const char* const& p) { return 100 + len(p); }
inline int f4(const char* p) { return 200; }
inline int f5(const char* const& p) { return 100 + len(p); }
template<class T> int f5(const T&) { return 200; }
inline int f6(const char (&)[4]) { return 100; }
inline int f6(const char*) { return 200; }
inline int f7(char (&)[4]) { return 100; }
inline int f7(const char* const& p) { return 200 + len(p); }
inline int f8(const char* const& p) { return 100 + len(p); }
inline int f8(Sv) { return 200; }
template<class T> int f8(const T&) { return 300; }
inline int f9(const char* const& p, int) { return 100 + len(p); }
inline int f9(const char (&)[4], long) { return 200; }
inline int fb(const char* const& p) { return 100 + len(p); }
inline int fb(const char (&)[4]) { return 200; }
inline int fb(Sv) { return 300; }
struct S {
    int m3(const char* const& p) { return 100 + len(p); }
    int m3(const char (&)[4]) { return 200; }
    int m4(const char* const& p) { return 100 + len(p); }
    int m4(const char* p) { return 200; }
    int m6(const char (&)[4]) { return 100; }
    int m6(const char*) { return 200; }
    int m7(char (&)[4]) { return 100; }
    int m7(const char* const& p) { return 200 + len(p); }
    int m9(const char* const& p, int) { return 100 + len(p); }
    int m9(const char (&)[4], long) { return 200; }
    int ma(const char (&)[4]) { return 100; }
    int ma(Sv) { return 200; }
    int mb(const char* const& p) { return 100 + len(p); }
    int mb(const char (&)[4]) { return 200; }
    int mb(Sv) { return 300; }
};
struct C3 { int r; C3(const char* const& p) : r(100 + len(p)) {} C3(const char (&)[4]) : r(200) {} };
struct C4 { int r; C4(const char* const& p) : r(100 + len(p)) {} C4(const char*) : r(200) {} };
struct C6 { int r; C6(const char (&)[4]) : r(100) {} C6(const char*) : r(200) {} };
struct C7 { int r; C7(char (&)[4]) : r(100) {} C7(const char* const& p) : r(200 + len(p)) {} };
struct C9 { int r; C9(const char* const& p, int) : r(100 + len(p)) {} C9(const char (&)[4], long) : r(200) {} };
struct Ca { int r; Ca(const char (&)[4]) : r(100) {} Ca(Sv) : r(200) {} };
struct Cb { int r; Cb(const char* const& p) : r(100 + len(p)) {} Cb(const char (&)[4]) : r(200) {} Cb(Sv) : r(300) {} };
inline int cv_array_pick(int (&)[3]) { return 1; }
inline int cv_array_pick(const int (&)[3]) { return 2; }
struct CvArrayMember {
    int pick(int (&)[3]) { return 1; }
    int pick(const int (&)[3]) { return 2; }
};
struct CvArrayCtor {
    int r;
    CvArrayCtor(int (&)[3]) : r(1) {}
    CvArrayCtor(const int (&)[3]) : r(2) {}
};
inline int farr_mut1(int (&)[3]) { return 11; }
inline int farr_const1(const int (&)[3]) { return 12; }
inline int farr_both1(int (&)[3]) { return 13; }
inline int farr_both1(const int (&)[3]) { return 14; }
inline int farr_mut2(int (&)[2][3]) { return 21; }
inline int farr_const2(const int (&)[2][3]) { return 22; }
inline int farr_both2(int (&)[2][3]) { return 23; }
inline int farr_both2(const int (&)[2][3]) { return 24; }
inline int farr_mix(const int (&)[2]) { return 1; }
inline int farr_mix(int) { return 2; }
struct ArrayMember {
    int farr_mut1(int (&)[3]) { return 31; }
    int farr_const1(const int (&)[3]) { return 32; }
    int farr_both1(int (&)[3]) { return 33; }
    int farr_both1(const int (&)[3]) { return 34; }
    int farr_mut2(int (&)[2][3]) { return 41; }
    int farr_const2(const int (&)[2][3]) { return 42; }
    int farr_both2(int (&)[2][3]) { return 43; }
    int farr_both2(const int (&)[2][3]) { return 44; }
};
struct ArraySubscriptMember {
    int operator[](int (&)[3]) { return 31; }
    int operator[](const int (&)[3]) { return 32; }
};
struct ArraySubscriptPointerMember { int operator[](const int* p) { return p[2] + 100; } };
struct ArraySubscriptMutablePointerMember { int operator[](int* p) { p[0] = 9; return p[1] + 200; } };
struct ArraySubscriptGridMember {
    int operator[](int (&)[2][3]) { return 41; }
    int operator[](const int (&)[2][3]) { return 42; }
};
struct ArraySubscriptRowPointerMember { int operator[](int (*p)[3]) { return 50 + p[1][2]; } };
struct ArraySubscriptExtentMember {
    int operator[](const int (&)[4]) { return 61; }
    int operator[](const int*) { return 62; }
};
struct ArraySubscriptRowMember {
    int operator[](const int (&a)[3]) { return a[0] * 100 + a[1] * 10 + a[2]; }
};
struct ArraySubscriptRowWriteMember {
    int operator[](int (&a)[3]) { a[2] = 7; return a[0] * 100 + a[1] * 10 + a[2]; }
};
struct ArraySubscriptBoolMember { int operator[](bool) { return 84; } };
struct ArrayScalarSubscriptMember { int operator[](long) { return 81; } };
enum ArraySubscriptElem { ArraySubscriptElemA = 1, ArraySubscriptElemB = 2, ArraySubscriptElemC = 3 };
struct ArraySubscriptEnumPointerMember {
    int operator[](const ArraySubscriptElem* p) { return 100 + (int)p[2]; }
};
struct ArraySubscriptEnumRowMember {
    int operator[](const ArraySubscriptElem (*p)[3]) { return 300 + (int)p[1][2]; }
};
enum class ArraySubscriptScopedElem { A = 1, B = 2, C = 3 };
enum class ArraySubscriptSmallElem : unsigned char { A = 1, B = 2, C = 3 };
struct ArraySubscriptEnumCvMember {
    int operator[](ArraySubscriptElem (&)[3]) { return 31; }
    int operator[](const ArraySubscriptElem (&)[3]) { return 32; }
    int operator[](ArraySubscriptScopedElem (&)[3]) { return 33; }
    int operator[](const ArraySubscriptScopedElem (&)[3]) { return 34; }
    int operator[](ArraySubscriptSmallElem (&)[3]) { return 35; }
    int operator[](const ArraySubscriptSmallElem (&)[3]) { return 36; }
    int array_subscript_enum_cv_member(ArraySubscriptElem (&)[3]) { return 41; }
    int array_subscript_enum_cv_member(const ArraySubscriptElem (&)[3]) { return 42; }
    int array_subscript_enum_cv_member(ArraySubscriptScopedElem (&)[3]) { return 43; }
    int array_subscript_enum_cv_member(const ArraySubscriptScopedElem (&)[3]) { return 44; }
    int array_subscript_enum_cv_member(ArraySubscriptSmallElem (&)[3]) { return 45; }
    int array_subscript_enum_cv_member(const ArraySubscriptSmallElem (&)[3]) { return 46; }
};
inline int array_subscript_enum_cv(ArraySubscriptElem (&)[3]) { return 51; }
inline int array_subscript_enum_cv(const ArraySubscriptElem (&)[3]) { return 52; }
inline int array_subscript_enum_cv(ArraySubscriptScopedElem (&)[3]) { return 53; }
inline int array_subscript_enum_cv(const ArraySubscriptScopedElem (&)[3]) { return 54; }
inline int array_subscript_enum_cv(ArraySubscriptSmallElem (&)[3]) { return 55; }
inline int array_subscript_enum_cv(const ArraySubscriptSmallElem (&)[3]) { return 56; }
struct ArraySubscriptEnumOnlyMutable {
    int operator[](ArraySubscriptElem (&)[3]) { return 61; }
    int operator[](ArraySubscriptScopedElem (&)[3]) { return 61; }
    int operator[](ArraySubscriptSmallElem (&)[3]) { return 61; }
};
struct ArraySubscriptEnumOnlyConst {
    int operator[](const ArraySubscriptElem (&)[3]) { return 62; }
    int operator[](const ArraySubscriptScopedElem (&)[3]) { return 62; }
    int operator[](const ArraySubscriptSmallElem (&)[3]) { return 62; }
};
struct ArraySubscriptEnumConstPointer {
    int operator[](const ArraySubscriptElem*) { return 63; }
    int operator[](const ArraySubscriptScopedElem*) { return 63; }
    int operator[](const ArraySubscriptSmallElem*) { return 63; }
};
struct ArraySubscriptEnumMutablePointer {
    int operator[](ArraySubscriptElem* p) { p[0] = ArraySubscriptElemC; return 64; }
    int operator[](ArraySubscriptScopedElem* p) { p[0] = ArraySubscriptScopedElem::C; return 64; }
    int operator[](ArraySubscriptSmallElem* p) { p[0] = ArraySubscriptSmallElem::C; return 64; }
};
struct ArraySubscriptEnumLong { int operator[](long) { return 65; } };
struct ArraySubscriptEnumByValue {
    int operator[](ArraySubscriptElem) { return 66; }
    int operator[](ArraySubscriptScopedElem) { return 66; }
    int operator[](ArraySubscriptSmallElem) { return 66; }
};
struct ArrayScalarPairSubscriptMember {
    int operator[](long) { return 1; }
    int operator[](double) { return 2; }
};
struct ArrayConversion { ArrayConversion(const int*) {} };
inline int array_conversion_only_pick(ArrayConversion) { return 61; }
inline int array_conversion_pick(ArrayConversion) { return 61; }
inline int array_conversion_pick(const int (&)[2]) { return 60; }
inline int array_conversion_exact_pick(ArrayConversion) { return 61; }
inline int array_conversion_exact_pick(const int (&)[3]) { return 62; }
inline int array_conversion_void_pick(ArrayConversion) { return 61; }
inline int array_conversion_void_pick(void*) { return 90; }
struct ArrayConversionMutable { ArrayConversionMutable(int*) {} };
inline int array_conversion_mutable_pick(ArrayConversionMutable) { return 64; }
struct ArrayConversionExplicit { explicit ArrayConversionExplicit(const int*) {} };
inline int array_conversion_explicit_pick(ArrayConversionExplicit) { return 65; }
struct ArrayConversionLong { ArrayConversionLong(long*) {} };
inline int array_conversion_long_pick(ArrayConversionLong) { return 66; }
struct ArrayConversionIntPtrPtr { ArrayConversionIntPtrPtr(int**) {} };
inline int array_conversion_intpp_pick(ArrayConversionIntPtrPtr) { return 69; }
struct ArrayConversionChar { ArrayConversionChar(char*) {} };
inline int array_conversion_char_pick(ArrayConversionChar) { return 70; }
struct ArrayConversionTwinA { ArrayConversionTwinA(const int*) {} };
struct ArrayConversionTwinB { ArrayConversionTwinB(const int*) {} };
inline int array_conversion_twins(ArrayConversionTwinA) { return 67; }
inline int array_conversion_twins(ArrayConversionTwinB) { return 68; }
inline int array_conversion_dual(ArrayConversion) { return 71; }
inline int array_conversion_dual(const ArrayConversion&) { return 72; }
struct ArrayConversionUnsigned { ArrayConversionUnsigned(const unsigned int*) {} };
inline int array_conversion_unsigned_pick(ArrayConversionUnsigned) { return 101; }
struct ArrayConversionEnum { ArrayConversionEnum(const ArraySubscriptElem*) {} };
inline int array_conversion_enum_pick(ArrayConversionEnum) { return 102; }
struct ArrayConversionRow { ArrayConversionRow(const int (*)[3]) {} };
inline int array_conversion_row_pick(ArrayConversionRow) { return 103; }
struct ArrayConversionMutableRow { ArrayConversionMutableRow(int (*)[3]) {} };
inline int array_conversion_mutable_row_pick(ArrayConversionMutableRow) { return 104; }
inline int array_conversion_free(ArrayConversion) { return 81; }
struct ArrayConversionMember { int call(ArrayConversion) { return 82; } };
struct ArrayConversionOuter { int value; ArrayConversionOuter(ArrayConversion) : value(83) {} };
struct ArrayCtorMut1 {
    int r;
    ArrayCtorMut1(int (&)[3]) : r(51) {}
};
struct ArrayCtorConst1 {
    int r;
    ArrayCtorConst1(const int (&)[3]) : r(52) {}
};
struct ArrayCtorBoth1 {
    int r;
    ArrayCtorBoth1(int (&)[3]) : r(53) {}
    ArrayCtorBoth1(const int (&)[3]) : r(54) {}
};
struct ArrayCtorMut2 {
    int r;
    ArrayCtorMut2(int (&)[2][3]) : r(61) {}
};
struct ArrayCtorConst2 {
    int r;
    ArrayCtorConst2(const int (&)[2][3]) : r(62) {}
};
struct ArrayCtorBoth2 {
    int r;
    ArrayCtorBoth2(int (&)[2][3]) : r(63) {}
    ArrayCtorBoth2(const int (&)[2][3]) : r(64) {}
};
// A literal at a template `const char *&&` that keeps its address: the caller's slot, as clang.
__attribute__((noinline)) inline int scribble() { volatile char b[2048]; for (int i = 0; i < 2048; ++i) b[i] = 0x5a; return b[0]; }
__attribute__((noinline)) inline int consume(const char* const* p, int) { return (int)std::strlen(*p); }
template<class U> const char* const* rr(const char*&& p, const U&) { return &p; }
// A constructor keeping the address of a literal bound at `const char *const &` beside a template.
struct Keep { const char* const* q; Keep(const char* const& p) : q(&p) {}
              template<class T> Keep(const T&) { static const char* z = "compete"; q = &z; } };
}

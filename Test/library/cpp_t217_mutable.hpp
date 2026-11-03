#pragma once

struct T217KM
{
    mutable int m;
    int n;
    int bump() { m += 100; return m + 1000; }
    int bump() const { m += 10; return m; }
};

constexpr T217KM t217_constexpr{1, 2};
inline const T217KM t217_const{3, 4};
inline const T217KM* t217_const_ptr() { return &t217_const; }

struct T217Inner { mutable int m; int n; };
struct T217Outer { T217Inner inner; };
inline const T217Outer t217_outer{{5, 6}};

struct T217ConstCompound
{
    int v;
    mutable int selected;
    int operator+=(int d) { selected = 1; return v + d + 101; }
    int operator+=(int d) const { selected = 2; return v + d + 1; }
};
struct T217NonConstCompound
{
    int v;
    int operator+=(int d) { return v + d + 101; }
};
struct T217CompoundHolder { const T217ConstCompound* mcp; };

struct T335Base { mutable int bm; int bn; };
struct T335Derived : T335Base { int d; };
inline const T335Derived t335Derived{{10, 11}, 12};

union T335Union { mutable int a; int f; };
inline const T335Union t335Union{13};

struct T335Array { mutable int a[2]; int b[2]; };
inline const T335Array t335Array{{1, 2}, {3, 4}};

struct T335Inner { int x; };
struct T335Member { mutable T335Inner in; int k; };
inline const T335Member t335Member{{5}, 6};

inline int t335Value = 5;
struct T335Pointer { mutable const int* p; int n; };
inline const T335Pointer t335Pointer{&t335Value, 9};

struct T335ConstInner { const int k; };
struct T335ConstOuter { mutable T335ConstInner m; };
inline const T335ConstOuter t335ConstOuter{{3}};

struct T335ConstArrayOuter { mutable T335ConstInner a[2]; };
inline const T335ConstArrayOuter t335ConstArrayOuter{{{3}, {4}}};

struct T335ConstChild { int x; };
struct T335ConstRecord { const T335ConstChild child; };
struct T335ConstRecordOuter { mutable T335ConstRecord m; };
inline const T335ConstRecordOuter t335ConstRecordOuter{{{{3}}}};

struct T335ConstBase { const int k; };
struct T335ConstMiddle : T335ConstBase {};
struct T335ConstInheritedOuter { mutable T335ConstMiddle m; };
inline const T335ConstInheritedOuter t335ConstInheritedOuter{{{3}}};

struct T335ConstMutableChild { mutable int m; };
struct T335ConstMutableHolder { const T335ConstMutableChild child; };
inline const T335ConstMutableHolder t335ConstMutableHolder{{3}};

struct T335AnonymousUnion { union { mutable int m; int n; }; };
inline const T335AnonymousUnion t335AnonymousUnion{1};

struct T450Inner
{
    mutable int mark = 0;
    int operator+(int) { return 31; } int operator+(int) const { return 42; }
    int operator+=(int) { mark = 131; return 131; }
    int operator+=(int) const { mark = 142; return 142; }
    int operator-() { return 231; } int operator-() const { return 242; }
    int operator[](int) { return 331; } int operator[](int) const { return 342; }
    int operator()(int) { return 431; } int operator()(int) const { return 442; }
    int operator==(int) { return 531; } int operator==(int) const { return 542; }
};

struct T450Item
{
    mutable int mark = 0;
    int pick() { return 11; } int pick() const { return 22; }
    int operator+(int) { return 31; } int operator+(int) const { return 42; }
    int operator+=(int) { mark = 131; return 131; }
    int operator+=(int) const { mark = 142; return 142; }
    int operator-() { return 231; } int operator-() const { return 242; }
    int operator[](int) { return 331; } int operator[](int) const { return 342; }
    int operator()(int) { return 431; } int operator()(int) const { return 442; }
    int operator==(int) { return 531; } int operator==(int) const { return 542; }
    T450Inner inner{};
};

struct T450Holder { T450Item normal; mutable T450Item mut; const T450Item fixed; };
inline const T450Holder t450ConstHolder{};
inline T450Holder t450MutableHolder{};

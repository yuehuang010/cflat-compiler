#pragma once
struct T84Double { double v; T84Double(double x):v(x) {} operator double() const { return v; } };
inline unsigned t84Take(unsigned x) { return x; }

namespace t300 {
struct IntCtor { int value; IntCtor(int x) : value(10 + x) {} ~IntCtor() {} };
struct CharCtor { int value; CharCtor(signed char x) : value(20 + (int)x) {} };
struct LongCtor { int value; LongCtor(long x) : value(30 + (int)x) {} };
struct Pick { int value; explicit Pick(int x) : value(60 + x) {} Pick(signed char x) : value(70 + (int)x) {} };
struct DoubleCtor {
    int value;
    DoubleCtor(int x) : value(10 + x) {}
    DoubleCtor(double x) : value(50 + (int)(x * 10)) {}
};
struct BoolCtor { int value; BoolCtor(bool x) : value(40 + (int)x) {} };
struct IntParam { int value; IntParam(int x) : value(10 + x) {} };
inline int takeInt(IntParam x) { return x.value; }
inline int takeDouble(DoubleCtor x) { return x.value; }
struct AssignInt {
    int value = 0;
    AssignInt() = default;
    AssignInt& operator=(int x) { value = 10 + x; return *this; }
};
}

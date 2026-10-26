#pragma once
namespace t496 {
struct ByInt { int chosen; int value; ByInt(int x) : chosen(11), value(x) {} int f() const { return chosen * 100 + value; } };
struct ByLong { int chosen; long value; ByLong(long x) : chosen(12), value(x) {} int f() const { return chosen * 100 + (int)value; } };
struct CIntRef { int chosen; int value; CIntRef(const int& x) : chosen(13), value(x) {} int f() const { return chosen * 100 + value; } };
struct CLongRef { int chosen; long value; CLongRef(const long& x) : chosen(14), value(x) {} int f() const { return chosen * 100 + (int)value; } };
struct RIntRef { int chosen; int value; RIntRef(int&& x) : chosen(15), value(x) {} int f() const { return chosen * 100 + value; } };
struct RLongRef { int chosen; long value; RLongRef(long&& x) : chosen(16), value(x) {} int f() const { return chosen * 100 + (int)value; } };
struct NF { int chosen; int value; NF(int x) : chosen(21), value(x) {} NF(double x) : chosen(22), value((int)x) {} int f() const { return chosen * 100 + value; } };
struct IntToFloat { int chosen; int value; IntToFloat(float x) : chosen(23), value((int)x) {} int f() const { return chosen * 100 + value; } };
struct PtrToBool { int chosen; int value; PtrToBool(bool x) : chosen(24), value(x ? 1 : 0) {} int f() const { return chosen * 100 + value; } };
}

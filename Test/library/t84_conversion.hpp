#pragma once
struct T84Double { double v; T84Double(double x):v(x) {} operator double() const { return v; } };
inline unsigned t84Take(unsigned x) { return x; }

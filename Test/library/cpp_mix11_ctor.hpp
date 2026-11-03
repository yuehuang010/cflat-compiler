#pragma once
// MIX11: a by-value and a const& constructor parameter tie for an lvalue V2 (clang: ambiguous).
struct Mx11V2 { int x; Mx11V2(int n) : x(n) {} };
struct Mx11Amb
{
    int r;
    Mx11Amb(const Mx11V2& v) : r(v.x + 10) {}
    Mx11Amb(Mx11V2 v) : r(v.x + 20) {}
};

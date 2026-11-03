#pragma once

// Mixed-access overload sets: each name has a public overload and a protected one. Access is decided
// by the overload that overload resolution SELECTS, not by the name as a whole.
namespace mixacc {
struct M {
    int operator-(double) const { return 1; }
    int g(double) const { return 3; }
protected:
    int operator-(int) const { return 2; }
    int g(int) const { return 4; }
};
}

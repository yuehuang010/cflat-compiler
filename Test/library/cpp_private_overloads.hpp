#pragma once

// Private overload sets: each name has a public overload and a private one. Overload resolution
// ranks the private overload too (as clang does), so a private pick is refused and a public pick runs.
namespace privacc {
struct P {
    int h(double) const { return 1; }
    int operator-(double) const { return 1; }
private:
    int h(int) const { return 2; }
    int operator-(int) const { return 2; }
};
struct PD : P {};
}

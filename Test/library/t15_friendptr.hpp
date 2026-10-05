#pragma once
namespace t15 {
struct W {
    int value;
    W(int input): value(input) {}
    friend bool operator==(const W& a, const W& b) { return a.value == b.value; }
    friend bool operator!=(const W& a, const W& b) { return a.value != b.value; }
    friend bool operator<(const W& a, const W& b) { return a.value < b.value; }
    friend int operator+(const W& a, const W& b) { return a.value + b.value + 100; }
};
struct PointerW {
    int value;
    PointerW(int input): value(input) {}
    friend bool operator==(const PointerW&, const PointerW&) { return false; }
    friend int operator==(const PointerW&, char* pointer) { return pointer == nullptr ? 31 : 32; }
};
}

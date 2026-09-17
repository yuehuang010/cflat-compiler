// Nested-specialization fixture (section M101). This header must NOT include any standard
// header: the defect it pins is a std container over a std specialization over a class owned by
// a USER import group, so this group must not itself be able to declare the std template.
#pragma once

namespace nest {

struct Cell
{
    int a;
    int b;
    Cell() : a(1), b(2) {}
    Cell(int x, int y) : a(x), b(y) {}
    int sum() const { return a + b; }
};

template <class T>
struct Box
{
    T item;
    Box() : item() {}
    int tag() const { return 7; }
};

}

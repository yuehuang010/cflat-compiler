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

// A user class in a NESTED namespace: the owning-group union keys on the leading namespace
// component, so a two-component name must still find this header.
namespace deep {
struct Node
{
    int n;
    Node() : n(5) {}
    Node(int v) : n(v) {}
    int val() const { return n; }
};
}

template <class T>
struct Box
{
    T item;
    Box() : item() {}
    int tag() const { return 7; }
};

// Section M102: a REFERENCE to a pointer-to-pointer. Three declarator levels, two of which the
// CFlat type model holds; the reference itself is the ABI's extra level.
inline int ppcref(Cell **const &pp) { return (*pp)->b * 10 + 1; }
inline int ppref(Cell **&pp)        { return (*pp)->b * 10 + 2; }
inline int ppval(Cell **pp)         { return (*pp)->b * 10 + 3; }
inline int pconstp(Cell *const *pp) { return (*pp)->b * 10 + 4; }

template <class T>
inline int ppdeduce(T **const &pp) { return (*pp)->sum() * 10 + 5; }

struct PpHolder
{
    Cell **slot;
    PpHolder() : slot(0) {}
    int take(Cell **const &pp)   { slot = pp; return (*pp)->a * 10 + 6; }
    int take_ref(Cell **&pp)     { slot = pp; return (*pp)->a * 10 + 7; }
    Cell **&ref_out()            { return slot; }
    Cell **const &cref_out() const { return slot; }
};

}

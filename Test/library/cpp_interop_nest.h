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

inline Cell *constref_cell_slot = nullptr;
inline const Cell *constref_const_cell_slot = nullptr;
inline Cell *constref_static_slot = nullptr;
inline int constref_static_int_value = 8;
inline int *constref_static_int_slot = &constref_static_int_value;
inline void constref_set_slot(Cell *p) { constref_cell_slot = p; }
inline void constref_set_const_slot(const Cell *p) { constref_const_cell_slot = p; }

inline int constref_param(Cell *const &p) { return p->a * 10 + p->b; }
inline int constref_param_cv(Cell *const volatile &p) { return p->a * 10 + p->b; }
inline int constref_param_const(const Cell *const &p) { return p->a * 10 + p->b; }
inline int constref_rvalue(Cell *const &&p) { return p->a * 10 + p->b; }

template <class T>
inline int constref_deduce(T *const &p) { return p->a * 10 + p->b; }

inline Cell *const &constref_return()
{
    return constref_cell_slot;
}

inline const Cell *const &constref_const_return()
{
    return constref_const_cell_slot;
}

template <class T>
inline T *const &constref_template_return(T *const &p) { return p; }

struct ConstRefPtrHolder
{
    Cell *slot;
    const Cell *const_slot;
    int *int_slot;
    ConstRefPtrHolder() : slot(nullptr), const_slot(nullptr), int_slot(nullptr) {}
    explicit ConstRefPtrHolder(Cell *const &p) : slot(p), const_slot(nullptr), int_slot(nullptr) {}

    void set_slot(Cell *p) { slot = p; }
    void set_const_slot(const Cell *p) { const_slot = p; }
    void set_int_slot(int *p) { int_slot = p; }
    int param(Cell *const &p) { return p->a * 10 + p->b; }
    int param_cv(Cell *const volatile &p) { return p->a * 10 + p->b; }
    int param_const(const Cell *const &p) { return p->a * 10 + p->b; }
    int param_int(int *const &p) { return *p * 10 + 6; }
    // `T*&` and `T*const&` are distinct overloads: a modifiable lvalue pointer picks the
    // non-const one whichever order they are declared in, a pointer rvalue picks the const one.
    int pick_a(Cell *&p) { p = nullptr; return 1; }
    int pick_a(Cell *const &p) { return 2; }
    int pick_b(Cell *const &p) { return 20; }
    int pick_b(Cell *&p) { p = nullptr; return 10; }
    Cell *const &out() const { return slot; }
    const Cell *const &const_out() const { return const_slot; }
    int *const &int_out() const { return int_slot; }

    static int static_param(Cell *const &p) { return p->a * 10 + p->b; }
    static int static_param_cv(Cell *const volatile &p) { return p->a * 10 + p->b; }
    static int static_param_const(const Cell *const &p) { return p->a * 10 + p->b; }
    static int static_param_int(int *const &p) { return *p * 10 + 6; }
    static Cell *const &static_out()
    {
        return constref_static_slot;
    }
    static void static_set(Cell *p) { constref_static_slot = p; }
    static int *const &static_int_out() { return constref_static_int_slot; }
};

struct ConstRefPtrArray
{
    Cell *slot;
    ConstRefPtrArray() : slot(nullptr) {}
    void set(Cell *p) { slot = p; }
    Cell *const &operator[](int) const { return slot; }
};

}

// A default member initializer that constructs a class template whose constructor body fails
// for the instantiated argument: the implicit default constructor of DM<int> cannot be bound.
#pragma once
namespace cppi_dmf
{
    template <class T> struct Q
    {
        T v;
        Q(const T& x) : v(x) { T::nope; }
        Q(double x) : v(11) {}
    };
    template <class T> struct DM { Q<T> q{5}; int z = 3; };
}

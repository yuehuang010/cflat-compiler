#pragma once

namespace cppitdiff
{
    template<class T>
    struct Iter
    {
        long position;

        Iter operator+(long offset) const { return Iter{ position + offset }; }
        Iter& operator+=(long offset) { position += offset; return *this; }
        long operator-(long offset) const { return position - offset; }

        template<class U>
        friend long operator-(const Iter<T>& left, const Iter<U>& right)
        {
            return left.position - right.position;
        }
    };

    inline Iter<int> makeIter(long position) { return Iter<int>{ position }; }
}

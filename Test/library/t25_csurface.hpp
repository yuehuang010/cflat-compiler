#pragma once

// C surfaces reached through a C++ namespace, shaped like <cstdlib> div/qsort/bsearch,
// <new> new_handler and <csignal> signal. Self-contained: no system header.
namespace t25surface
{
    typedef struct { int quot; int rem; } Pair;
    typedef struct { long long quot; long long rem; } WidePair;
    typedef struct t25_tagged_pair { int quot; int rem; } TaggedPair;   // MSVC's _div_t shape
    typedef int (*CompareC)(const void*, const void*);
    typedef int (*CompareM)(void*, void*);
    typedef void (*Handler)();
    typedef int (*Compare)(const void*, const void*);

    inline Pair pair(int n, int d) { Pair p; p.quot = n / d; p.rem = n % d; return p; }
    inline TaggedPair tagged_pair(int n, int d)
    {
        TaggedPair p; p.quot = n / d; p.rem = n % d; return p;
    }
    // Overloads on a callback's pointee const, and a template that forwards to them.
    inline int pick(CompareC) { return 1; }
    inline int pick(CompareM) { return 2; }
    template <class F> inline int choose(F f) { return pick(f); }
    // Imported C++ callbacks: a named one keeps its spelled pointee const for pick.
    inline int cb_const(const void*, const void*) { return 7; }
    inline int cb_mutable(void*, void*) { return 8; }
    struct Callbacks
    {
        static int by_const(const void*, const void*) { return 5; }
        static int by_mutable(void*, void*) { return 6; }
    };
    inline WidePair wide_pair(long long n, long long d)
    {
        WidePair p; p.quot = n / d; p.rem = n % d; return p;
    }

    inline Handler g_handler = nullptr;
    inline Handler set_handler(Handler h) { Handler old = g_handler; g_handler = h; return old; }
    inline Handler get_handler() noexcept { return g_handler; }

    // signal's shape: a function returning a function pointer, spelled without a typedef.
    inline void (*g_signal)(int) = nullptr;
    inline void (*set_signal(int, void (*f)(int)))(int)
    {
        void (*old)(int) = g_signal;
        g_signal = f;
        return old;
    }

    inline void sort(void* base, unsigned long n, unsigned long size, Compare cmp)
    {
        char* b = static_cast<char*>(base);
        for (unsigned long i = 1; i < n; ++i)
            for (unsigned long j = i; j > 0 && cmp(b + (j - 1) * size, b + j * size) > 0; --j)
                for (unsigned long k = 0; k < size; ++k)
                {
                    char t = b[(j - 1) * size + k];
                    b[(j - 1) * size + k] = b[j * size + k];
                    b[j * size + k] = t;
                }
    }
    // A template takes the comparator through a generated wrapper, which spells the CFlat
    // function's type for clang: its `const void*` parameters must survive.
    template <class T>
    inline void sort_typed(T* b, unsigned long n, Compare cmp)
    {
        for (unsigned long i = 1; i < n; ++i)
            for (unsigned long j = i; j > 0 && cmp(&b[j - 1], &b[j]) > 0; --j)
            {
                T t = b[j - 1];
                b[j - 1] = b[j];
                b[j] = t;
            }
    }
    inline void* search(const void* key, const void* base, unsigned long n,
                        unsigned long size, Compare cmp)
    {
        const char* b = static_cast<const char*>(base);
        for (unsigned long i = 0; i < n; ++i)
            if (cmp(key, b + i * size) == 0) return const_cast<char*>(b + i * size);
        return nullptr;
    }
}

// Deliberately ill-formed C++ header for the bind-refusal error test. It (a) declares a class
// clang rejects - a field of `cppbroken::Incomplete`, which is declared but never defined - and
// (b) instantiates its own class template over that invalid record. Both halves matter: the
// invalid field cascades into every template instantiation that touches the record, which used
// to take the companion-CodeGen pass down with an access violation instead of reporting the
// diagnostic. Self-contained on purpose: no platform header is included, so whether it is broken
// never depends on what a standard library happens to declare transitively. The error must not
// be an unknown name either - that reads as a missing prerequisite header and takes the
// "does not compile on its own" path instead. Never bound by a passing test.
#pragma once

namespace cppbroken
{
    struct Incomplete;

    template <class T>
    struct Holder
    {
        T value;
        T* data() { return &value; }
        const T& get() const { return value; }
        void set(const T& v) { value = v; }
    };

    struct Broken
    {
        Incomplete name;
        int get() const { return 1; }
    };

    inline Holder<Broken> uses_template() { return Holder<Broken>(); }
    inline int reads_through(Holder<Broken>& h) { return h.get().get(); }
}

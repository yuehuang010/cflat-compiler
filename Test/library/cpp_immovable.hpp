#pragma once

// T90: a C++ class whose move AND copy constructors are deleted maps to CFlat [immovable].
namespace immt90 {
class Pinned {
public:
    int v;
    Pinned() : v(3) {}
    Pinned(const Pinned&) = delete;
    Pinned(Pinned&&) = delete;
};

class Plain {
public:
    int v;
    Plain() : v(4) {}
};
}

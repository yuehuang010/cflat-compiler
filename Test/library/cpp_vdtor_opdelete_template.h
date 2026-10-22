#pragma once
#include <new>

namespace cppvdtemplate {
inline int deleteTag = 0;

template<class T> struct Deleted {
    Deleted() = default;
    Deleted(const Deleted&) = default;
    virtual ~Deleted() {}
    static void operator delete(void*) = delete;
    int value() const { return 19; }
};

struct PrivateDeleteBase {
private:
    static void operator delete(void* p) { ::operator delete(p); }
};

template<class T> struct Private : PrivateDeleteBase {
    virtual ~Private() {}
    int value() const { return 23; }
};

struct DeleteBaseA { static void operator delete(void* p) { ::operator delete(p); } };
struct DeleteBaseB { static void operator delete(void* p) { ::operator delete(p); } };

template<class T> struct Ambiguous : DeleteBaseA, DeleteBaseB {
    virtual ~Ambiguous() {}
    int value() const { return 29; }
};

template<class T> struct Usable {
    virtual ~Usable() {}
    static void operator delete(void* p) { deleteTag = 7; ::operator delete(p); }
    int value() const { return 37; }
};

template<class T> struct Uninstantiated {
    virtual ~Uninstantiated() {}
    static void operator delete(void*) = delete;
};

inline int RunUsable() {
    deleteTag = 0;
    Usable<int>* p = new Usable<int>();
    int value = p->value();
    delete p;
    return value * 100 + deleteTag;
}
}

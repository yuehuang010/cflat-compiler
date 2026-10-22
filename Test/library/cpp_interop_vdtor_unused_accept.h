#pragma once
#include <new>
namespace cppvdaccept {
inline int public_delete_tag = 0;
inline int public_dtor_count = 0;
inline int private_delete_tag = 0;
inline int private_dtor_count = 0;

struct UnusedValid {
    virtual ~UnusedValid() {}
    static void operator delete(void* p) { ::operator delete(p); }
};

struct UsedValid {
    virtual ~UsedValid() { ++public_dtor_count; }
    static void operator delete(void* p) {
        public_delete_tag = 7;
        ::operator delete(p);
    }
};

struct OwnPrivateDelete {
    virtual ~OwnPrivateDelete() { ++private_dtor_count; }
private:
    static void operator delete(void* p) {
        private_delete_tag = 5;
        ::operator delete(p);
    }
public:
    static int Run() {
        OwnPrivateDelete* p = new OwnPrivateDelete;
        delete p;
        return private_delete_tag * 100 + private_dtor_count;
    }
};

template<class T> struct UninstantiatedBadDelete {
    virtual ~UninstantiatedBadDelete() {}
    static void operator delete(void*) = delete;
};

inline int RunValid() {
    public_delete_tag = 0;
    public_dtor_count = 0;
    UsedValid* p = new UsedValid;
    delete p;
    return public_delete_tag * 100 + public_dtor_count;
}

inline int RunTemplateBoundary() { return 29; }
}

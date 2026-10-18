#pragma once
// Virtual destructors whose class-scope operator delete clang cannot use (T294). Kept in their own
// header: a refused header group is shared by every file in one suite process, so these must not
// sit in a header other error tests import. Only err_cpp_new_private_operator.cb imports this one.
#include <cstdlib>
namespace cppvd {
struct Deleted { virtual ~Deleted() {} static void operator delete(void*) = delete; };
struct PrivBase { private: static void operator delete(void* p) { std::free(p); } };
struct PrivDerived : PrivBase { virtual ~PrivDerived() {} };
struct AmbigA { static void operator delete(void* p) { std::free(p); } };
struct AmbigB { static void operator delete(void* p) { std::free(p); } };
struct Ambig : AmbigA, AmbigB { virtual ~Ambig() {} };
// Usable class-scope delete: the same shape must keep compiling.
struct Usable { virtual ~Usable() {} static void operator delete(void* p) { std::free(p); } };
}

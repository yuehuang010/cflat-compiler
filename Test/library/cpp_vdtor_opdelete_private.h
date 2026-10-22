#pragma once
// Virtual destructor reaching a private operator delete through a base (T333). Own header: a
// refused header group is shared by every file in one suite process.
#include <cstdlib>
struct VdtorPrivBase { private: static void operator delete(void* p) { std::free(p); } };
struct VdtorPrivDerived : VdtorPrivBase { virtual ~VdtorPrivDerived() {} };

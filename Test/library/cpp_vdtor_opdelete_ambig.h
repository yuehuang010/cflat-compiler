#pragma once
// Virtual destructor whose operator delete is found in two unrelated bases (T333). Own header: a
// refused header group is shared by every file in one suite process.
#include <cstdlib>
struct VdtorAmbigA { static void operator delete(void* p) { std::free(p); } };
struct VdtorAmbigB { static void operator delete(void* p) { std::free(p); } };
struct VdtorAmbig : VdtorAmbigA, VdtorAmbigB { virtual ~VdtorAmbig() {} };

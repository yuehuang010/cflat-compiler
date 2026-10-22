#pragma once
// Virtual destructor whose class-scope operator delete is `= delete` (T333). Its own header:
// a refused header group is shared by every file in one suite process.
struct VdtorDeleted { virtual ~VdtorDeleted() {} static void operator delete(void*) = delete; };

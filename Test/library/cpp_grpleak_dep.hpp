#pragma once
// GRPLEAK dependency line: macros named like other lines' declarations, plus strong definitions
// a host line's TU sees once a request with this line as a dependency includes it.
#define GrpleakHostMember (1-1)
#define GrpleakDep2Rec (1-1)
#define GRPLEAK_DEPC 11
namespace grpleak_dep { struct Rec { int v; }; }
int grpleak_variadic(char* value, ...) { return 55; }

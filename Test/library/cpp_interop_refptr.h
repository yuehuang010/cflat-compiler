#pragma once

namespace refptr {
struct C { int v; };
// Takes a pointer by reference and returns the pointee: a constant argument can reach a local.
inline C& deref(C*& pp) { return *pp; }
}

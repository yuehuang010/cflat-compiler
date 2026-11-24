#pragma once
// A bitfield inside a class hierarchy: cflat refuses to flatten this layout (opaque shell).
namespace cpprefused {
    struct BfBase { int a = 1; };
    struct BfDerived : BfBase { int bits : 3; int g = 2; };
}

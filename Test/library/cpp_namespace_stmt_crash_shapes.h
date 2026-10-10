#pragma once

namespace t66_ns01 { int x; t66_ns01::x = 5; }
namespace t66_ns02 { int *p; *p = 5; }
#define T66_NS03_ASSIGN x = 5;
namespace t66_ns03 { int x; T66_NS03_ASSIGN }
namespace t66_ns04 { int x; sizeof(x); }
namespace t66_ns05 { []{}(); }
namespace t66_ns06 { int x; &x; }
namespace t66_ns10 { int x; static_cast<int>(x); }
namespace t66_ns12 { int x; }
namespace t66_ns12 { ::t66_ns12::x = 1; }
namespace t66_ns21 { int x; int y; x != y; }
namespace t66_ns22 { int x; int y; x, y; }
namespace t66_ns23 { int x; int y; x }
namespace t66_ns24 { int x; typeid(x); }
namespace t66_ns25 { int x; alignof(int); }
namespace t66_ns26 { int x; noexcept(x); }
namespace t66_ns27 { struct B { virtual ~B() {} }; struct D : B {}; B* p; dynamic_cast<D*>(p); }
namespace t66_ns28 { int* p; reinterpret_cast<long>(p); }
namespace t66_ns29 { int* p; const_cast<const int*>(p); }
// Deliberately leave the final namespace and statement incomplete at EOF (n17).
namespace t66_ns17 { int x; x

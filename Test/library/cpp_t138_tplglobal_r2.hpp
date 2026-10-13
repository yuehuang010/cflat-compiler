#pragma once

#include "cpp_t138_tplglobal_r2_transitive.hpp"

#line 1 "foreign/t138_alias.hpp"
namespace outerAlias { namespace real { enum E { value = 18 }; } namespace other = real; }

#line 1 "foreign/t138_inline.hpp"
namespace outerInline { inline namespace ver { namespace inner { enum E { value = 19 }; } } }

#line 1 "foreign/t138_nested.hpp"
namespace outerNested::inner { enum E { value = 20 }; }

#define T138_R2_OPEN namespace inner {
#line 1 "foreign/t138_macro.hpp"
namespace outerMacro { T138_R2_OPEN enum E { value = 17 }; } }

#line 1 "foreign/t138_dead.hpp"
namespace outerDead { template<class T> struct P { int v; }; P<int> item{24}; }
#if 0
namespace outerDead { namespace item {} }
#endif

namespace outerTransitive {}
// namespace inner

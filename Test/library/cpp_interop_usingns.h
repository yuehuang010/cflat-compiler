#pragma once

// M99: using-directives, with the nominated namespace nested INSIDE the nominating one first.
// That shape rewrites a prefix into a LONGER name that still starts with the same key, so a
// rewrite loop with no progress rule never terminates - the compiler used to hang here.
namespace cppu_nest
{
    namespace detail { inline long helper() noexcept { return 7; } }
    using namespace detail;
    inline long via_using() noexcept { return helper() + 10; }
}

// Two levels down, nominated by a qualified name.
namespace cppu_deep
{
    namespace mid { namespace inner { inline long helper2() noexcept { return 9; } } }
    using namespace mid::inner;
    inline long via_using2() noexcept { return helper2() + 20; }
}

// Sibling: the nominated namespace is NOT a descendant of the nominating one.
namespace cppu_sib
{
    namespace a { inline long helper3() noexcept { return 11; } }
    namespace b { using namespace a; }
}

// A chain of directives, resolved in two hops.
namespace cppu_chain_c { inline long helper4() noexcept { return 13; } }
namespace cppu_chain_b { using namespace cppu_chain_c; }
namespace cppu_chain_a { using namespace cppu_chain_b; }

// A cycle: each namespace nominates the other, so the walk must terminate on `visited`.
namespace cppu_cyc_a { inline long helper5() noexcept { return 17; } }
namespace cppu_cyc_b { using namespace cppu_cyc_a; inline long helper6() noexcept { return 19; } }
namespace cppu_cyc_a { using namespace cppu_cyc_b; }

// A namespace that nominates ITSELF.
namespace cppu_self
{
    inline long helper7() noexcept { return 23; }
    using namespace cppu_self;
    inline long via_using7() noexcept { return helper7() + 100; }
}

// A NAMESPACE ALIAS (`namespace a = b;`) is a second spelling for an existing namespace, and
// every kind of member has to resolve through it: a function, a namespace-scope VARIABLE (the
// one spelling that bound nothing before), a class type and a scoped-enum member. Covered at a
// top-level target, a NESTED target, and an alias of an alias.
namespace cppu_alias_target
{
    inline long alias_fn() noexcept { return 29; }
    inline long alias_counter = 43;   // header-only object, no companion .cpp here
    struct AliasBox { long v; };
    enum class AliasE { A = 31, B = 37 };
    namespace deep { inline long alias_deep_fn() noexcept { return 41; } }
}
namespace cppu_al = cppu_alias_target;
namespace cppu_al_deep = cppu_alias_target::deep;
namespace cppu_al2 = cppu_al;

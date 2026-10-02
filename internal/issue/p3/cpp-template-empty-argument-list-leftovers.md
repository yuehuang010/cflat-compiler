# Empty template argument list `<>`: leftovers after N63

N63 landed `X<>` as the all-defaulted instantiation (ruling 2026-10-02: `std.barrier<>` allowed, bare name
stays legal; native `Box<>` without defaults refused, `<>` on a non-generic refused). A round-4 review
(probes were in cflat-fix-n63 scratch/rv4/) left three edge cases. None is a regression vs master, where
every `<>` was refused.

1. Native non-generic inside a namespace accepts `<>` when written unqualified:
   `namespace ns { struct Plain { int x = 0; }; int f() { Plain<> x = default; return 0; } }` compiles
   (also field, global, pointer param/return inside `ns`). `ns.Plain<>` is refused correctly.
   Cause: MainListener_Declarations.cpp (~943) tests `IsDataStructure(ResolveTypeAlias(baseName))` on
   the unscoped name; resolve the scoped native type first.
2. C++ namespace alias + `<>` false rejection, order dependent:
   `namespace n63 { template<class T = int> struct Box { T value = 7; }; } namespace wrap { namespace nested = n63; }`
   then `wrap.nested.Box x = default; auto y = wrap.nested.Box<>();` -> "'n63.Box' is not a generic type".
   Cause: LLVMBackend::IsCxxTemplateSpecializationOf compares spelling prefixes; the recorded spelling can
   be the alias spelling while the expression path checks the resolved namespace. Compare canonical
   record identity instead.
3. Alias template with a required parameter borrows the target's defaults:
   `template<class T> using Required = n63::Box<T>;` and `template<class T> using FixedRequired = n63::Box<int>;`
   accept `req.Required<>` / `req.FixedRequired<>`; clang: "too few template arguments for alias template".
   Cause: ApplyCxxAliasPattern (LLVMBackend_Interfaces.cpp ~418-500) runs before the empty-list check
   and drops/ignores the unbound alias parameter.

Also seen (not N63): a CFlat alias of a C++ template `using G = dz.Gate; G g = G(1)` -> "the function 'G'
is not known", bare and explicit.

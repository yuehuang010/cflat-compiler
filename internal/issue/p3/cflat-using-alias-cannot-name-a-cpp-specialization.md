Bucket: p3

# A CFlat `using` alias cannot name a C++ template specialization
(compile failure on a spelling that works everywhere else).

## Summary

Filed 2026-09-17 from the alias-template-pattern fix. A C++ specialization is usable as a
declaration type, as a constructor call, as a parameter/return type and as a generic argument -
but NOT as the right-hand side of a CFlat `using` alias:

```cflat
import cpp "cpp_interop_tpl.h";
using V = std.vector<int>;              // using alias 'V' = 'std.vector<int>': 'std.vector' is not a generic type
using B = alnp.NBox<int,7>;             // same message for a user header template
extern int main() { V v = default; return 0; }
```

The plain declaration `std.vector<int> v = default;` compiles in the same file. Independent of
alias templates: the alias-template spelling (`using C = alna.NBdef<int>;`) fails with the
identical message, and so does the direct target spelling.

## Repro

`scratch/aln_c18_stdusing.cb`, `scratch/aln_c13d.cb`, `scratch/aln_c13_using_alias.cb` in the
fix-cpp-alias-nontype worktree. Refused identically on master 884a0c08 and on the branch.

## Root cause

`ParseUsingDeclaration` (`MainListener_Declarations.cpp`, the generic-RHS arm around line 2075)
accepts a generic RHS only when the base is a CFlat template (`genericStructTemplates` /
`genericClassTemplates` / `genericInterfaceTemplates`), a winmd generic, or the one hard-coded
`std.function` special case; a C++ class template matches none of those and falls through to the
"is not a generic type" LogError. `ForwardRefScanner.cpp`'s copy of the same arm has the same
shape. No `TryRequestCxxType` call exists on the general path.

## Fix direction

Extend the RHS arm (BOTH copies) to request a C++ specialization the way the declaration path
does, and alias to the returned identity. Accept set: the existing "is not a generic type"
diagnostic must still fire for a genuinely unknown base - `Test/errors/` pins that message.

# Struct method plain type name prefers namespace type over nested type

Summary: Inside a struct in a namespace, an unqualified type name resolves to the namespace declaration instead of the struct nested type.
Repro: Inside `S` in namespace `N`, plain `Mid` with both `N.Mid` and `S.Mid` declared.
Master vs expected: master returns 44; clang returns 176.
Found: T465 r1 opus review, 2026-10-24 box, PRE-EXISTING

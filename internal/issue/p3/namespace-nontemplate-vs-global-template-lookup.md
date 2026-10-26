# Namespace non-template is ignored in favor of global template

Summary: A global generic type template wins over a same-named non-template type in the current namespace.
Repro: Non-generic `Item` in a namespace and global generic `Item<T>`; use `Item<int>` inside the namespace.
Master vs expected: master compiles; clang finds the inner non-template and refuses the template spelling.
Found: T458 r1 opus review, 2026-10-24 box, PRE-EXISTING

# Unexpanded trailing alias-template pack limits argument count

Summary: An alias template with an unexpanded trailing parameter pack still caps accepted template arguments.
Repro: `template <class T = int, class... Ts> using PD = Box<T>;` then `pb.PD<long, int, char>`.
Master vs expected: master says the template takes 2 arguments but 3 were given; clang accepts.
Root cause hint: LLVMBackend_Interfaces.cpp:650 checks only expanded `packParam`.
Found: T458 r2 opus review, 2026-10-24 box, PRE-EXISTING

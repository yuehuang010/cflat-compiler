# Coverage gaps: C++ expression-template constructors

Round-2 review (2026-09-27) of fix/eigen-expr found no defects, only unpinned paths (clang++
-std=c++20 oracle passed them; CFlat behaviour not asserted):

- Expression-template constructors: constrained (`requires`) and `enable_if` constructor templates,
  lvalue-copy vs prvalue copy/move/destruction counts, a by-value assignment operator, same-named
  records in two namespaces (empty-name recovery collision).

Direction: fixtures in Test/library/cpp_interop_basic.h, legs after the Eigen-shape block in
Test/test_cpp_interop.cb (7460-7465). (The inherited-operator half was pinned on
fix/inherited-members, codes 7251-7257 plus five expect_error blocks.)

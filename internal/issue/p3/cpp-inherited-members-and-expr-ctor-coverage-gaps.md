# Coverage gaps: inherited C++ member lookup and expression-template constructors

Round-2 review (2026-09-27) of fix/inherited-static + fix/eigen-expr found no defects, only unpinned
paths (clang++ -std=c++20 oracle passed them; CFlat behaviour not asserted):

- Inherited operators: hiding in a MIDDLE class, `using Base::operator+` in a middle class, operator
  inherited through a private base, unary vs binary `operator-` hiding (the `arguments.size() >= 2` gate
  in LLVMBackend_Overloads.cpp near inheritedOperatorIsVisible).
- Expression-template constructors: constrained (`requires`) and `enable_if` constructor templates,
  lvalue-copy vs prvalue copy/move/destruction counts, a by-value assignment operator, same-named
  records in two namespaces (empty-name recovery collision).

Direction: fixtures in Test/library/cpp_interop_basic.h, legs after the inherited-operator block in
Test/test_cpp_interop.cb (~7230-7242) and the Eigen-shape block (7460-7465).

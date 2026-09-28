# Inherited C++ members: private/protected inheritance and virtual bases give misleading diagnostics

Found 2026-09-27 by the fix/inherited-static review (pre-existing on master).
- Private/protected inheritance: `struct D : private B` with `B::K` static -> CFlat reports "'K' is not a
  member"; clang reports inaccessible base member. Refusal is right, message is wrong.
  Repro scratch/rev_ih/rev_ih_priv.cb.
- Virtual inheritance: a class with a virtual base is refused ("layout cannot be reproduced",
  LLVMBackend_CInterop.cpp ~14229) so an inherited operator through a virtual base is unusable; clang
  accepts. Repro scratch/rev_ih/rev_ih_edges.h. Virtual-base layout support is the real gap (bigger).

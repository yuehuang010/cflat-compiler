Bucket: P (plan: virtual-base layout)

# C++ class with a virtual base: refused, so members inherited through it are unusable

Found 2026-09-27 by the fix/inherited-static review (pre-existing on master).
A class with a virtual base is refused ("layout cannot be reproduced", LLVMBackend_CInterop.cpp
~14229), so an inherited operator through a virtual base is unusable; clang accepts.
Repro: scratch/rev_ih/rev_ih_edges.h (`I1` / `I2 : virtual I1` / `V : I2`, `V + V`).
Virtual-base layout support (vbase offset lookup, construction order) is the real gap - plan-sized.
(The private/protected-base "'K' is not a member" message half was fixed on fix/inherited-members.)

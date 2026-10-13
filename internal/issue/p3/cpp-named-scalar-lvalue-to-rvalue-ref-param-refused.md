# Named int/float lvalue passed to a C++ double&& parameter is refused

Found in T78 review 3 (pre-existing on master). Repros: scratch/repro_keep/t78/rev_t78_r3_rv_named.cb,
rev_t78_r3_rv_float.cb (+ .cpp oracles, header rev_t78_r3.h).

`void f(double&&)`; `int i = 3; f(i);` - C++ binds a converted temporary (the source is not moved
from); CFlat refuses through the ownership/move checks. An integer temporary `f(3)` works since T78.

Fix direction: when the scalar table says Widen (or any conversion) into an rvalue-reference
parameter, the argument binds a fresh temporary; skip the move/ownership requirement for that case.

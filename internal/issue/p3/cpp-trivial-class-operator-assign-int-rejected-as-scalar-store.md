# Trivial C++ class with only `operator=(int)`: `a = 9;` rejected as a scalar store into struct storage

Found 2026-09-28 by run A7 (pre-existing). A trivially copyable C++ class that declares `operator=(int)`
(and nothing else user-provided) refuses `a = 9;` with "cannot store a single scalar value into struct
storage": the trivial-class assignment path treats the class as plain struct storage and never looks for the
declared `operator=`. clang calls `operator=(int)`. Fix direction: consult C++ `operator=` overloads before the
trivial-struct scalar-store diagnostic (the non-trivial path already does - TryDirectCxxAssignOperator).

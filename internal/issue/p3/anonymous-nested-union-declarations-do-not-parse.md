# Anonymous and nested union declarations do not parse

Summary: Anonymous union members and a named nested-union field are rejected by the parser.
Repro: `union { ... };` inside a struct and a named nested-union field.
Master vs expected: master does not parse these declarations; expected C++ union member support.
Found: T440 r1 opus review, 2026-10-24 box, PRE-EXISTING

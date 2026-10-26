# Symbol dump field heading exposes internal instantiation key

Summary: `--symbol-dump` prints the internal instantiation key for a generic field heading.
Repro: `Box$_f$int$.0@2:10.K.value` for a generic over a function-local enum.
Master vs expected: master exposes the internal key in the heading; expected a readable field name as on the following line.
Found: T462 r1 opus review, 2026-10-24 box, PRE-EXISTING

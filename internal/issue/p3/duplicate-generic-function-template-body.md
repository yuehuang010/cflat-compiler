# Duplicate generic function template bodies are accepted

Summary: The template registry bypasses duplicate function body diagnosis.
Repro: Define `T h<T>(T a){return a;}` twice.
Master vs expected: master compiles and the second body wins; expected a duplicate definition diagnostic.
Root cause: template registry bypasses `DiagnoseDuplicateFunctionBody`.
Found: T431 r1 opus review, 2026-10-24 box, PRE-EXISTING

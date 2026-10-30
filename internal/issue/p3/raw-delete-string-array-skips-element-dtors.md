# p3: raw delete[_] of a string* array skips element destructors (R21 residue)

Filed at R21 landing (2026-10-27). Per the R21 ruling, an unproven `string*` subscript read now copies; in raw-new
arrays the copied-from element then leaks because raw `delete[_]` never runs `string` destructors.

- p02: `string* g; g = new string[2]; g[0] = mk(); string t = g[0]; delete[_] g;` -> allocs/frees/live 3/2/1 (was a
  double free before R21).
- p04: the same with `g` set by a called setter -> 3/2/1.
- Already on master before R21: a raw-new local `string* raw = new string[2]; ... string t = raw[0];` borrows -> 2/1/1.

Belongs to the raw-array desugar direction (2026-09-03): raw `delete[]` of an array of a destructor-bearing type must
run element destructors (C++ meaning) or be refused. Probes: R21 worktree scratch/r21/ (copied to
scratch/repro_keep/r21/).

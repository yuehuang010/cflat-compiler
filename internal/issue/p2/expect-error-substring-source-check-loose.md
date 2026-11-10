# expect_error substring-source lint (check 2) accepts nearly any text

Filed 2026-11-07, residue of the expect_error leg-lint landing (`utilities/lint_test_legs.py`). Test-infra,
not a compiler defect.

Summary: check 2 must prove every `expect_error` substring occurs in some expansion of a cflat/ diagnostic
format string. `TemplateIndex.accepts` (lint_test_legs.py, `accepts` / `_match`) lets a placeholder absorb
any text, and the index holds every `std::format` / `std::vformat` in cflat/, including non-diagnostic ones
(`"{} {}"` in symbol-dump code, MainListener_Declarations.cpp ~14267, MainListener_Initializers.cpp ~5605;
`"{}:{}"`, `"{}.{}"`). So `'zzqq wwvv kkjj'`, `'x, y'`, `'Foo.Bar'` and the old wrong text
`expected ';' at 'string'` are all accepted. Only the targeted en.json-copy half of check 2 bites.

Measured 2026-11-07 (3149 live substrings): five tighter rules tried by the implementer each refused 42-448
legitimate substrings (messages assembled from several format strings, relayed clang text, pure type
spellings). Dropping format strings with fewer than 3/5/8 letters from the index refused 0 legit substrings
and still accepted every nonsense probe above - the absorb-anything placeholder is the hole, not the index.

Repro: in utilities/, `python3 -c "import lint_test_legs as L; ix=L.TemplateIndex('../cflat');
print(ix.accepts('zzqq wwvv kkjj'))"` -> True.

Fix direction: index only diagnostic sinks (`LogError*` catalog entries plus `std::format` results that flow
into one), and match nested templates (a placeholder may expand to another template, not to free text);
relayed clang text needs its own source (clang's diagnostic table, or the `clang: ` prefix as the only
allowed free text). Placeholder text matching a type spelling may need a type-name grammar.

Acceptance: the probes above are refused; 0 false refusals over the current Test/ corpus; mutation-proven
like the other checks; runtime of the static phase stays under ~10 s.

## Two attempts 2026-11-07 (box 12, Codex Luna; not landed, patches scratch/box12/lintsub_r2.patch, gitignored)

- Recursive placeholder expansion (a placeholder expands only to another template or a bounded
  type/identifier atom): over 30 s per static run, and '{} {} {}' still absorbs 'zzqq wwvv kkjj'
  word by word. Do not retry.
- Literal-run floor `need = min(3, len(sub)//2)` (the matcher already supports `need`): 42 false
  refusals over 3151 live substrings, 2 of 4 probes refused, ~7 s.
  - 29 are value-only substrings (no space outside quotes: 'Box<Col>', 'ConstArrayRef'). Rule that
    works: such a substring must occur in its own fixture's source or a direct Test/ import (legs'
    own arguments blanked; `::` and `->` normalized to `.`). 29/29 proven, and it refuses
    'Foo.Bar' / 'x, y'.
  - 13 are prose: 7 joined overload-candidate lists ('f(a), f(b)'), 3 cflat phrases the index
    misses, 3 relayed clang texts ('invokes a deleted function', 'failed to instantiate ...').
- With those rules: 4/4 probes refused, 0 false refusals, static phase 10.7 -> 15.2 s. Not landed:
  the clang relays were matched by a hardcoded phrase list (overfit to today's tests), and only
  21/300 (7%) of legit prose substrings with one word replaced by 'zzqq' were refused - a leading
  or trailing placeholder absorbs the edited word, so the floor proves little.

Open decisions before another round: where relayed clang text comes from on every host (the Mac
has no LLVM source tree; vendoring a generated list of Diagnostic*Kinds.td strings into utilities/
needs a ruling), and whether check 2 should anchor both ends of the substring on literal text
(refuse placeholder absorption at the head and tail), which the 7% figure points at.

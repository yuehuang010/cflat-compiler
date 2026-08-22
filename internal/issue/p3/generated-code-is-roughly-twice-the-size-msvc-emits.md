# Generated code is roughly 2x the size MSVC emits for the same logic

Filed 2026-08-21 from an external report (MemPressMonitor Win32 port, v0.11.0 - measured in their
README's size comparison, not in their issue list). Not independently reproduced; the measurement
is theirs, on a functional clone of the same app written twice.

## The measurement

Section-level comparison of the two GUI binaries, C++ `/MD` vs CFlat, same app, Release / `-O2`:

| Section | C++ GUI | CFlat GUI | delta |
|---------|---------|-----------|-------|
| `.text` | 74,240 | 121,344 | **+47,104** |
| `.rdata` | 25,088 | 7,168 | -17,920 |
| `.rsrc` | 9,728 | 1,024 | -8,704 |
| `.data` | 512 | 7,680 | +7,168 |
| `.pdata` | 3,072 | 1,024 | -2,048 |
| other | 512 | 2,048 | +1,536 |
| **total** | **113,152** | **139,776** | +26,624 |

Reading, and it is a careful one - the reporter's own conclusions, worth keeping:

- **`.text` is the whole story.** Every other section is a wash or favours CFlat.
- **It is not runtime bloat.** A `printf` hello-world has a 1,536-byte `.text` in CFlat against
  MSVC's 3,584 (13,312 once the STL is touched). CFlat's fixed runtime is genuinely SMALLER. The
  extra `.text` in a real app is generated code.
- The `.data` gain and `.rsrc` loss are the same 6,960 bytes moving: the app icon is an
  `RT_GROUP_ICON` resource in C++ and a `u8[]` array in CFlat, because of
  [[no-resource-embedding-or-resource-compiler]]. Discount both from the comparison.
- On the headline number CFlat wins once deployment is equalized: 112 KB vs C++ `/MT`'s 246 KB for
  the CLI, with no VC++ redistributable. **This issue is only about the `.text` ratio**, not about
  binary size overall, which is fine.

## Why it is filed, and why p3

Nothing here is a defect - the app runs, and their runtime measurement was a tie (724 ms vs 713 ms
end-to-end, both dominated by PDH and `NtQuerySystemInformation`, not by generated code). It is a
codegen-quality datapoint that nobody has measured against a real second implementation before, and
it is worth having on record before someone tries to explain a future performance gap.

One contextual factor the reporter flagged: **CFlat's backend is LLVM 18**, so its codegen is
roughly Clang-18-era and does not benefit from four LLVM releases of improvements. They could not
run a three-way comparison (the only Clang on that machine is the `clang-cl` 18.1.6 bundled inside
CFlat, and MSVC 14.51's STL hard-rejects it - it requires Clang 20+), so **the ratio is against
MSVC only and is not attributed**. A Clang-18 leg is what would separate "LLVM 18 vs MSVC 2026" from
"CFlat's IR is fatter than clang's".

## Fix direction

Investigate before proposing anything - there is no diagnosed root cause here, only a ratio.

The cheapest first step is a like-for-like IR comparison on one hot function from their `memcore`
against clang's `-O2` output for the C++ equivalent, looking for the usual CFlat-specific suspects:
inlining decisions on the `returnBlockTable` bodies, per-element destructor and ownership
bookkeeping that C++ elides, container method instantiations that never get merged, and
`-O2` running a narrower pass pipeline than clang's default. If a Clang-18 leg becomes available
(installing LLVM 20+ satisfies the STL floor and gives a three-way build), run that first - it may
resolve most of the gap without any compiler work.

Adjacent: the completed q16 bucket ("Codegen folding and determinism").

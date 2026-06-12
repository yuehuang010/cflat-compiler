# C interop: anonymous records, bitfields, folded-constant display

Outcome notes from the 2026-06 windows.h coverage survey (4 example probes:
run_capture, mmap_hexdump, vmmap, tcp_http_get) and the fixes it drove.
Living regression: `Test/test_windows.cb` (+ `test_windows_cache.cb` for the
disk-cache round-trip). Read this before touching the C record mapper.

## Anonymous nested struct/union members (the LARGE_INTEGER fix)

A C field that is (a) a truly anonymous member, (b) a NAMED member whose type
is an unnamed record (`struct {...} u;`), or (c) an ARRAY of an unnamed record
(`struct {...} Extents[1];`) is handled by synthesizing a `<tag>__anonN`
sub-record in `CollectFields` (CClangExtract.cpp): peel ConstantArrayType
extents FIRST, run the unnamed-record check on the element type, emit
`rf.ctype = "struct <synTag> [N]"` (suffix matches clang's `char [260]`
spelling). RegisterCRecords needs no special casing - StripFixedArrayDims +
ConstArraySize lay the synthetic element out inline. Never let a partially
mapped record change size: unmappable still means opaque shell, errors on use.

Transparent C11 access (`li.LowPart` without naming the anonymous hop) is
`ResolveTransparentAnonField` in MainListener.h: DFS over `__anonN` members
ONLY (named members of unnamed type stay explicit - `li.u.LowPart` - to keep
the ambiguity rule), builds the chained GEP, returns an lvalue NamedVariable.

## Bitfields

PackBitfields (RegisterCRecords) replaces bitfield fields with `__bfN` storage
slots in StructFields and fills the `StructData.Bitfields` side-table
(StorageFieldIndex/BitOffset/BitWidth/IsUnsigned). Field access must check
`dataStructure.Bitfields` BEFORE StructFields. `EmitBitfieldAccess`
(MainListener.h) is the single source of truth for the read mask/shift AND the
NamedVariable bitfield metadata; the assignment path (`bitfieldAssign`) does
the read-modify-write off that metadata. The transparent DFS also scans each
anonymous member's Bitfields (capture by value - GetDataStructure returns a
copy). Multi-hop works (PROCESS_MITIGATION_ASLR_POLICY: union -> unnamed
struct -> 1/28-bit fields). Constraint: transparency needs an addressable base.

## cheaders disk cache discipline

Schema version lives in TWO places in LLVMBackend.h (read guard ~11116, writer
~11175). Bump it whenever the serialized record shape changes (v5->6 anon
synthesis, v6->7 array-of-unnamed). Delete `%USERPROFILE%\.cflat\cheaders\`
when testing extractor changes - the key is header/defines-based, NOT compiler-
binary-based, so a stale entry can mask a fix even across rebuilds. Prove
round-trips with a cold (writes) + warm (reads) run of test_windows_cache.cb.

## Folded macro/enum constant display

`--symbol` and LSP hover both print `SymbolDef::signatureMarkdown` verbatim, so
constant type+value display is set once at registration: RegisterCMacros /
RegisterCEnums append a value suffix via `ConstIntValueSuffix` (unsigned ->
hex masked to width, signed -> decimal; pointer sentinels -> 64-bit hex;
strings/floats literal). Radix follows the registered CFlat type, not the
original C spelling (not retained) - `MEM_COMMIT` (int) prints 4096, not
0x1000. Cache needed no bump (v7 already stored value + intTypeName).

## Still unmappable, by design (don't "fix" without a feature decision)

- MIDL/RPC wire unions (`__MIDL_IWinTypes_*`, `_wireSAFEARRAY`) and their
  embedders (userCLIPFORMAT, ...): marshaling-only types, nobody hand-writes
  them. Negative test `err_c_struct_incomplete_by_value.cb` targets
  userCLIPFORMAT - if extraction ever improves, that test needs a new target.
- COM vtable structs whose fn-ptr fields take structs by value
  (IViewObjectVtbl): the real gap is callable C fn-ptr fields (COM invocation),
  a feature, not a mapper bug. Fn-ptr fields are bare void* by design (a
  16-byte function<T> closure would corrupt C ABI layout).

## --symbol / LSP member listing for C records (2026-06-12)

C-extracted records get per-field SymbolKind::Field entries emitted in
RegisterCRecords (LLVMBackend.h) - the field list is captured BEFORE
PackBitfields swaps in __bfN slots, so bitfields display with their real names
and :width. Display-time only; no cache schema involvement. PrintSymbolDetail
finds members by the `def.name + "."` prefix scan, so anything not in the
symbol sink is invisible there. Known skip: typedef names (FILE_NOTIFY_INFORMATION
vs tag _FILE_NOTIFY_INFORMATION) are not index entries - RegisterRecordAliases
fills only the typeAliases string map; deemed not worth a three-file change.

## Survey verdicts worth keeping

- WSADATA(408), STARTUPINFOA(104), MEMORY_BASIC_INFORMATION(48),
  RETRIEVAL_POINTERS_BUFFER(32) all size exactly; the tiling self-check in
  example/windows/vmmap.cb independently proves pointer-width field layout.
- Function-like macros (MAKEWORD, htons-when-macro, s_addr) never fold -
  expected; htons resolved as a real function on SDK 26100.
- Inline `lib "ws2_32.lib"` pass-through to lld-link works with no --c-lib.
- MSYS2/Git Bash mangles `/`-style argv (`/` -> `C:/Program Files/Git/`) and
  outer `cmd /c "..."` - run Windows exes with such args via pwsh/cmd, not the
  Bash tool. Also mangles `\\.\pipe\...` paths.
- WCHAR maps to u16 (2-byte, verified WIN32_FIND_DATAW==592 vs A==320); a
  trailing `WCHAR FileName[1]` registers as inline [1 x u16]
  (FILE_NOTIFY_INFORMATION==16) and indexing past [0] via pointer decay is
  unchecked, which is what the trailing-array idiom needs (dirwatch.cb).
- OVERLAPPED(32) maps with two-hop transparent anon access (ov.Offset) and
  union-arm aliasing (ov.Pointer); `void*[N]` decays to a HANDLE* param
  (WaitForMultipleObjects); ERROR_IO_PENDING/WAIT_*/PIPE_* all fold
  (pipe_echo.cb, no lib clause - kernel32 default).
- INPUT_RECORD imports by value since the __anonN fix; its `union {...} Event;`
  and `uChar` are NAMED members of unnamed types, so the access path is the
  explicit MSDN spelling rec.Event.KeyEvent.uChar.AsciiChar (console_raw.cb,
  which no longer needs core's flattened os.windows._INPUT_RECORD).
- Probes #5 (dirwatch, wide-char) and #6 (pipe_echo, overlapped/events) found
  ZERO mapper gaps - the post-fix mapper covers the typical Win32 program
  surface; remaining parked items are the by-design ones above plus COM
  invocation (declined 2026-06-12, "No COM").

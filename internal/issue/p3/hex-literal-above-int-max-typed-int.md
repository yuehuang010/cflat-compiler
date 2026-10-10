# Hex literal above INT_MAX is typed int and sign-extends (RULED: C/C++ ladder)

Found by T63 review 2 (pre-existing CFlat literal typing). Probed on master 2026-10-09 (Release,
`--run`).

## Ruling (maintainer, 2026-10-09)
Follow C/C++ (C99/C++11 [lex.icon], identical in both). An unsuffixed hex/octal/binary literal takes
the first type that holds its value: int, unsigned int, long, unsigned long, long long, unsigned
long long (`long` = target long, per the 2026-09-30 L ruling). Decimal: int, long, long long, never
unsigned. Suffixes narrow the ladder as in C. The "typeless bit pattern until it lands" idea was
considered and dropped the same day.
C23 support is coming soon (maintainer, 2026-10-09). C23 keeps this ladder for unsuffixed literals;
its literal additions (`wb`/`uwb` -> _BitInt, `'` digit separators) arrive with that work. Keep the
ladder a single table-driven step so a `wb` suffix slots in without a second typing path.
Conversions after typing follow C++: `int x = 0xFFFFFFFF;` stays legal (-1, modular), brace
initialization of a value that does not fit is a narrowing error (clang++ parity:
`std.vector<int> v{0xFFFFFFFF}` refused). Other implicit-conversion contexts keep their existing
CFlat rules (e.g. no implicit narrowing at call args, 2026-09-04).

## Measured today - only HEX is wrong; decimal, octal, binary already follow the ladder
| Source | Today | Ruled (C/C++) |
|--------|-------|---------------|
| `u64 m = 0x80000000;` | ffffffff80000000 | 0x80000000 |
| `i64 b = 0xFFFFFFFF;` | -1 | 4294967295 |
| `auto a = 0xFFFFFFFF; i64 b = a;` | -1 | 4294967295 (a is u32) |
| `u64 v; v & 0xFFFFFFFF` | v unchanged (mask is all ones) | low 32 bits |
| `u64 m = 0xFFFFFFFF << 4;` | fffffffffffffff0 | 0xFFFFFFF0 (u32 shift, then widen) |
| `0xFFFFFFFF == -1` | true | true (usual arithmetic conversions: -1 -> u32) |
| `std.vector<int> v{0xFFFFFFFF};` | element -1 | narrowing error |
| `int a = 0xFFFFFFFF;` / `i8 w = 0xFF;` | -1 / -1 | -1 / -1 (unchanged) |
| `i64 d = 3000000000;` | 3000000000 | same (already right) |
| `u64 m = 020000000000;` / `0b1` + 31 zeros | 80000000 | same (already right) |

## Fix direction
Type hex literals by the ladder in BOTH the ForwardRefScanner constant folder and codegen (they must
agree - see the comment at Test/test_interface.cb:4128; that `if const (0xFFFFFFFF == -1)` stays
true under C rules). Find why the hex path differs from the octal/binary path and route it through
the same ladder. Test/test_c.cb:445 (switch on u64) already assumes C values. Audit uncast high-bit
hex in core/Test/example (scratch/hex_uncast.txt lists 41 sites, most already `u` suffixed or cast).
Extend existing legs (test_basic.cb literal section, test_c.cb hex switch); add a brace-narrowing
refusal leg to an existing Test/errors file if one covers brace narrowing.

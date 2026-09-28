# C flexible array member binds as a pointer field: access loads garbage and crashes

Found 2026-09-28 during the Windows `#pragma pack` fix (pre-existing on master, not a regression).
Imported C `struct FlexA { int n; int data[]; };` registers `data` as an `int*` field
(`--symbol FlexA.data` prints `int* data`), so `a->data[i]` LOADS a pointer from the record at the
member's offset and indexes through it -> access violation. `unsigned char data[0]` behaves the same.
Before the pack fix the record was also mis-sized (FlexA 16 bytes, packed FlexP 12); since the fix,
zero-length/flexible arrays ride the zero-size layout path, so size and offset now match clang
(FlexA 4, data at +4) but the pointer load remains.
Repro: scratch/flex/flex.cb (+ flex.h), `-i scratch/flex -o ...`; exits 0xC0000005 at the first
`a->data[0] = 10`.
Fix direction: map a C incomplete/zero-length array member to an array-typed view at the member's
offset (address-of, no load) - element access = GEP from the member address, like a fixed array field.
Must stay zero-size in the record layout. Add the case to Test/test_c_interop.cb.

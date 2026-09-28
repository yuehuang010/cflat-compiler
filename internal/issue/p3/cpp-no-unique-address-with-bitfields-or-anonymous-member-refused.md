# C++ bitfield record whose bitfield run is cut short or shares bytes with a narrow member is refused

[[no_unique_address]] beside bitfields and inside anonymous members now takes the clang-offset
layout (NUA members lifted out before PackBitfields, appended after the physical slots). What
stays refused, cleanly, by the layout verifier:

- a [[no_unique_address]] member BETWEEN two bitfields (`unsigned a:3; [[no_unique_address]] E e;
  unsigned b:3;`): Itanium ends the run and starts `b` at the next byte; PackBitfields starts a
  new storage unit at the unit type's alignment instead. Pinned by `cppi.NuaBitBetween` in
  `Test/errors/err_cpp_record_by_value.cb`.
- the same packer gap without any NUA: a bitfield unit that shares bytes with a narrower
  non-bitfield member (`unsigned a:3; unsigned b:13; char c; int d:9;` is 8 bytes in clang,
  12 in cflat; also `int x; unsigned a:3; unsigned b:7; short s;`, `char c; unsigned a:2;`).

Refused, not miscompiled. Not seen in a real library yet.

## Fix direction

Teach PackBitfields' Itanium mode byte-granular placement: a unit may start at any byte that
keeps the bitfield inside one naturally aligned unit of its declared type, and a non-bitfield
member (or zero-size member) closes the run at the next byte. Then flip the NuaBitBetween leg
into a value leg in `Test/test_cpp_interop.cb`.

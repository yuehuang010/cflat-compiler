# A C++ class temporary in an array brace initializer is copied bitwise into the slot and destroyed twice

Found 2026-09-17 during fix/cpp-class-array-ctor (macOS arm64, Release). NOT STARTED: same family as internal/issue/p2/cpp-class-copy-from-field-skips-copy-ctor.md (CFlat-side copy of a C++ class), HELD by maintainer ruling; needs the same ruling.

## Summary

`ac.Trk[2] a = { ac.Trk(), ac.Trk() };` constructs two temporaries, memcpy's each into its slot with no copy constructor, destroys the temporaries, and then destroys the two slots at scope exit: ctor=2, copy=0, dtor=4. With the short-list form `ac.Trk[3] a = { ac.Trk(), ac.Trk() };` the tail slot is now default-constructed (ctor=3) but the counts are still dtor=5. Measured identically on master 2798eb1a and on the array-ctor fix binary. A class owning a resource releases it twice per listed element.

## Fix direction

At EmitPositionalFixedArrayIntoSlot / EmitBraceElementIntoFixedSlot, when the element type is a foreign C++ class and the brace element is an rvalue temporary, move-construct (or copy-construct) into the slot via EmitCxxCopyOrMoveConstruct and skip the temporary's destructor, or construct the temporary directly in the slot. Same mechanism the by-value parameter and struct-copy issues need.

Suggested bucket: p2.

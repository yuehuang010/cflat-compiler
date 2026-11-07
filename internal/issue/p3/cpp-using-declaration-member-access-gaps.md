# C++ using-declaration over virtual base remains unsupported

The C++ record flattener refuses records with virtual bases. Clang accepts a virtual base
field re-exposed by `using`, but CFlat does not yet model the virtual-base storage layout.

Repro: `scratch/repro_keep/t99/rev_t99_r3_virtual_base.cb` and its `.cpp` oracle
(`virtual_base`, clang++ -std=c++20 result 13).

Root cause: `FlattenCxxLayout` returns false when the record has virtual bases because the
current flattened layout computes base offsets only for non-virtual subobjects. Resolving a
using-shadow target to a field slot does not provide the runtime virtual-base offset needed by
the flattened representation.

Fix direction: handle virtual-base layout and member access as part of the P4 virtual-base
layout plan. Keep this item active until that layout work lands.

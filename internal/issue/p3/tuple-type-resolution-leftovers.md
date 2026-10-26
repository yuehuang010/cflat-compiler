# Function-local enum pointer generic argument remains after T433

`Box<K*>` inside a function still reports `cannot find the type 'K'` when `K` is a function-local enum. Repro: `scratch/t433/b6_local_enum_generic_forms.cb` (copied from the T369 review probe). This path is not a tuple element.

Root cause: generic argument resolution retains the pointer suffix while resolving the enum base. The main pass later runs enum resolution on `K*`, which does not match the local enum `K`; the forward scanner has a parallel type-argument path. T433's shared tuple-element resolver does not flow through this non-tuple generic argument spelling.

Fix direction: peel pointer/view suffixes before resolving a generic argument's base type in both passes, then reapply the suffix. Preserve the current function-local enum scope and generic instantiation key.

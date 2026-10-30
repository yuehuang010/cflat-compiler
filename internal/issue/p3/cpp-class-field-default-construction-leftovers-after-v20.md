# C++-class fields of CFlat structs: default-construction and initializer leftovers after V20

4. (p3) A field initializer that names an earlier field is "Undefined variable" (C++ allows it).
   RULED 2026-10-29 (main session, from precedent; maintainer may veto): C++ rule - an EARLIER field is visible and readable in a later field's initializer (declaration-order
   initialization); the field itself or a LATER field is a located use-before-initialization error; members hide outer
   names. Same ruling as call-name-shadowing-leftovers-after-t484 "Field default calls its own field name".

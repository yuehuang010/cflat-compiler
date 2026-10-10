# `return t + 0;` (pointer arithmetic on a class pointer) into an interface return is refused

From T36 (landed 393f0bd9, 2026-10-05). `Thing* t`, `class Thing : IThing`: declaration
(`IThing a = t + 1;`), assignment and argument spellings convert like C++; only RETURN refuses,
via the return-path backstop "cannot convert this expression to interface 'IThing': its concrete
class cannot be determined". Pinned by the last leg of
Test/errors/err_nullcoalesce_iface_arm_unresolved.cb (`IThing returnPointerArithmetic(Thing* t)
{ return t + 0; }`).

## Ruling (maintainer, 2026-10-09)
Accept. `t + 1` is a `Thing*` (e.g. the next element of a Thing array) and converts to the
interface on return like every other position. Authorized: edit that leg - keep the backstop
pinned with a shape whose class is genuinely unknown if one still reaches it, else drop the leg
(and say so in the report).

# q13: Fixed arrays and aggregate initialization

4 active items remain. Q13 fixed the array rejection and field default-construction legs;
two related failures remain: the array EXTENT is dropped in declarator handling (so rejection
guards and mangling misbehave), and element-wise CONSTRUCTION is replaced by a splat or memcpy
that skips constructors and side effects.

## Shared root cause

- **Extent lost.** Parameter declaration drops the extent, so the mangled signature does not match
  the decayed-pointer argument; several rejection guards are unreachable because an earlier branch
  exits the declarator loop first.
- **Splat instead of construction.** Default and override emitters call
  `GenerateDefaultValue`/`getNullValue` or evaluate an override once and memcpy it, never walking
  element constructors. Correct for POD, wrong for anything with a constructor or an owning field.

## Members

Extent / rejection: fixed in Q13; see the fixed list below.

Construction / initialization:
- `p3/array-default-splat-drops-ctor-side-effects`
- `p3/named-override-expression-evaluated-per-slot-only-for-owning-elements` - POD arm evaluates
  once and memcpys; the owning arm needs single-eval plus an owning copy, not a re-emit.
- `p2/brace-override-of-an-owning-field-leaks-the-constructed-value` - plain store into an already
  default-constructed owning field, no destruct first.
- `p2/raw-heap-struct-array-element-read-double-frees` - reading an owning struct element off a
  raw heap array materializes a destructible temp.

Fixed in Q13:

- `p2/fixed-array-parameter-not-callable` (already landed; reconciled with this bucket)
- `p3/function-pointer-to-fixed-array-not-rejected`
- `p3/interface-and-struct-member-fixed-array-return-not-rejected`
- `p2/multidim-fixed-array-has-no-brace-initializer`
- `p2/fixed-array-field-skips-element-default-construction`

## RULING 2026-08-11: nested braces only, plus string rows for char arrays

Decided by the maintainer. Two legal spellings for a multi-dim fixed array, and only two:

```cflat
int[2][3]  a = {{1,2,3},{4,5,6}};   // nested braces - the general form
char[2][8] b = {"ab","cd"};          // string element per row - char arrays only
```

A FLAT list for a multi-dim array (`int[2][3] a = {1,2,3,4,5,6};`) is NOT legal. Reject it with an
error that names the expected shape, so the diagnostic teaches the nested form. This is the point
of choosing nested-only: a wrong-length flat list is easy to write and hard to diagnose well, and
allowing it doubles the grammar surface for no expressive gain.

The string-row form is a per-element-type rule on the initializer (it applies where the innermost
element type is `char`), not a general flat-list allowance. Do not let implementing it re-open the
flat case: `char[2][8] b = {'a','b',...}` flat is still an error.

## Fix direction

0. Q13 landed the ruling above as the shape rule the initializer validates against, BEFORE the
   element-construction work in step 2 - the reject and the string-row case both need the per-
   dimension mapping to be correct first, and `p2/multidim-fixed-array-has-no-brace-initializer` is
   where that mapping lives.
1. Q13 added the remaining member/funcptr rejection funnels and retained the existing parameter
   rejection. It also accepts nested shapes and char string rows; flat multi-dimensional lists
   remain errors.
2. Carry the extent through the declarator into the parameter/return type, and move the fixed-array
   rejection to a single funnel every declarator path reaches (the funcptr and member paths
   currently bypass it by returning early).
3. Q13 routes fixed-array field defaults through the existing per-element construction walk. The
   remaining side-effect and owning-value semantics are still active.
4. Replace the splat with one element-init routine parameterized by POD-ness: evaluate the override
   ONCE, then per slot either memcpy (POD) or copy-construct (owning). That single routine covers
   the default-construction, side-effect, and per-slot-override items together.
5. The two owning items (`brace-override...`, `raw-heap-struct-array-element...`) interact with the
   owned-temp ledgers - sequence them after q01.

Mostly disjoint from ownership; items 1 and 2 are good parallel work at the sonnet tier.

Bucket: p3 (usable-surface gap, conservative refusal; no wrong code)

# Chained `owner = <inner> = new PS()` still refused for a field or ternary inner

T203 made `owner = raw = new PS();` (raw local, direct non-array `new`) behave exactly as
`raw = new PS(); owner = raw;`. Still refused while the two-statement form compiles:
- field inner: `owner = r.raw = new PS();`
- ternary-new inner: `owner = raw = (c ? new PS() : nullptr);`
- parenthesised inner: `owner = (raw = new PS());` (carry the slot through the paren path, MainListener_Expressions.cpp ~723)
- declaration init: `unique PS* owner = raw = new PS();`
T203 records the inner slot only for a direct non-array `new` into a local (NamedVariable transient
slot, read only by an outer unique `=`); extend that record to these inners with the same
consume semantics. Acceptance: each compiles, one destructor runs, a later read of the inner
reports use after move, matching the two-statement form.

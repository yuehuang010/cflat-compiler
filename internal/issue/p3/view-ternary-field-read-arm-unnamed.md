# A view '?:' whose arm is a struct FIELD read stays unnamed and escapes the element gate

Bucket: batch mode (closes an accept hole; freeze the accept-set first). Filed 2026-09-04 by the
q19 fix/review (30263a18), which made a view ternary carry its element from the arms' declared
storage, a callee's return type, or an inner join.

## Repro

```
struct H { u8[] items; };
H h; h.items = new u8[4];  u8[] other = new u8[4];
int[] v = cond ? h.items : other;   // accepted; strides by int over u8 storage
```

`TernaryArmViewType` (cflat/MainListener_Expressions.cpp ~4722) resolves an arm through
`FindDeclaredTypeAndValueForStorage`; a field read has no declared local storage, so the arm is
unnamed, nothing is stamped on the join, and every door accepts it. Function-result and local
arms are covered.

## Fix direction

Resolve a field-read arm through the base struct's `StructFields` (the same recovery the call
door uses for field arguments, cflat/LLVMBackend_Overloads.cpp ~1245) and stamp the join. Legs:
Test/errors/err_array_view_element_mismatch.cb ternary section; accept legs beside
`iface_view_ternary_field` in Test/test_interface.cb (same-element field arms).

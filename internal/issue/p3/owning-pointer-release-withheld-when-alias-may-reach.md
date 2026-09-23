Bucket: p3 (leak, no unsafety; measured 2026-09-23 on fix/raw-owning-pointer-runtime-flag)

# An owning pointer/view reassignment releases nothing when an alias of the old block may reach it

Reassigning an owning `T*` / `T[]` local releases the old block only when a post-walk scan
(LLVMBackend::ResolveOwnedReleaseGates / OwnedSlotAliasPoints) proves no alias-leaving use of
the slot can reach the reassignment. Anything unproven withholds the release and the old block
leaks, as it did before the runtime owns-flag landed:

```cflat
cppon.Cls* p = new cppon.Cls(1); int g = p->get(); p = new cppon.Cls(2);  // C++ method: callee is
                                                                          // a declaration, so the
                                                                          // receiver "may be retained"
S* p = new S(); S* first = p; p = new S();     // local borrow of the old block (by design: releasing
                                               // it is a use-after-free when `first` is read/deleted)
```

Measured leaks (cflat-raw-ptr-own scratch/rp_matrix.md): cpp_call_before, save_old_read, esc_keep,
view_alias, alias loops, addr_out, this_escape, lambda_cap. Fix direction: a retain summary for
C++ methods (clang knows whether `this` escapes), or a borrow-checker-style rejection of
reassigning an owner while a local borrow of it is live.

Swap through a temporary leaks too (test_cpp_interop leg 3446): `tmp = s; s = t; t = tmp;` withholds
s's release (tmp aliases it), so s's first object is orphaned, and `t = tmp` does not adopt it -
a borrow whose owner no longer holds the block may point at a block that moved to another owner
or is still read through the borrow (review round 2: b5/b7b were use-after-free when it adopted).
Direction: record what a withheld release abandons in a per-owner orphan slot, adopt only when
the new value equals it (clearing the slot), and gate the adopter's later releases on the
adoptedFrom closure of the borrow slot it adopted through.

# `?:` arm `&(new T(x))->f` outside a call argument leaks the temporary

Bucket: p3 (bounded leak). Pre-existing for plain `?:`; a `??` inside the arm now matches it.

## Repro

`bool b = (c > 0 ? &(new T(1))->v : np) == np;` - one new, zero dtors (also with
`&(new T(2))->v ?? np` as the arm). Stores are refused by err_address_into_new_temp.cb.

## Root cause

FlushOwnedTempsSince claims a temp the kept arm value points into (ClaimOwnedPtrTempsUnder ->
addrClaimedPtrTemps_) so it outlives the arm, but nothing frees a claimed temp. Inside a call
argument FinishTernaryArm hoists it into a conditional slot instead (b3a6e009).

## Fix direction

Hoist into the conditional slot for any full expression, not only inCallArgument_, so the
end-of-statement flush frees it once on the taken path.

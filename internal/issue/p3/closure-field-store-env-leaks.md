# Closure field stores leak the old env / the moved param (pre-existing, T196 Opus r1)

- Field overwrite through `this`: `struct NB { Lambda<int()> v; void set(Lambda<int()> x){ v = x; } }` called 1000x leaks
  1998 blocks (old env + its string capture per overwrite), master and after T196 (generic path now matches). Cause:
  cflat/MainListener_Expressions.cpp ~5662 deliberately skips freeing the old env for a field reached through a pointer.
  Fix direction: free the old env when the base is `this` of a value-type receiver (fields initialized); keep the skip for
  raw heap packets. Repro scratch/repro_keep/t196_rv1/rev_t196_opus_this_field_env_leak.cb.
- `void take(move T x) { v = move x; }` called 500x leaks 998 blocks (direct `b.v = move x` leaks 0).
  Repro scratch/repro_keep/t196_rv1/rev_t196_opus_take_move_leak.cb.
- `dictionary<int, Lambda<int()>>` does not compile: "cannot return an 'alias' value without addressable storage" in a
  dictionary.cb instance (~line 478); list<Lambda<..>> works.
Measure with `leaks --atExit -- ./x`.

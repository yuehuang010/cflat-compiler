# alias of a call-result field leaks the owner instead of extending it to scope end

Summary: since T211, `alias int x = mk()->v;` (and `alias int x = *mk()->heap;`, nested
`mk()->f->v`) never frees the owning call result: the alias reads valid storage, but a 1000x loop
runs 0 destructors (master was a use-after-free). Same no-free path T205 uses for a pointer
result (`int* p = mk()->heap;`). Also unfreed (conservatively): an alias declaration whose arrow
temp sits in a call argument (`alias int x = idA(mk()->v);` must stay alive, but
`alias int x = byval(mk()->v);` leaks too - argument positions are not told apart syntactically).

Repro (T211 review): scratch/rev_t211_opus_probe.sh cases in the T211 worktree, or
`for (...) { alias int x = mk()->v; s += x; }` with a dtor counter -> 0 dtors.

Fix direction: register the receiver temp for destruction at the alias's scope exit (Rust extends
the temporary to the enclosing block), instead of unregistering it. For a call-argument position,
only extend when the callee returns alias; a by-value argument dies at the statement end.

# `unique` FIELD migration: candidate survey

Survey date 2026-07-20, against a clean tree at commit `54d6803` ("List<T> is now a borrow by
default...", the container borrow-by-default migration). Analysis only - no source file was
modified to produce this.

Scope: every scalar pointer FIELD of every struct/class in `cflat/core/**/*.cb`.

## Summary

**The migratable candidate set is EMPTY. Zero fields in `cflat/core` qualify.**

This contradicts the figure carried in `internal/plan/unique-ownership.md` ("the ~34 verified-owning
pointer fields (of 254 scalar pointer fields across 144 structs)"). That list was never recorded, and
it does not reconstruct. The reason is mechanical and easy to check:

A field is migratable only if a hand-written destructor frees it with a plain scalar `delete`,
because that is the only thing the synthesized release emits. A repo-wide grep for a scalar
`delete <ident>;` statement anywhere in `cflat/core` returns 43 sites. **Exactly two of them are
inside a destructor** - `json.cb:219` and `xml.cb:223` - and both delete a *local loop variable*
while walking a sibling chain, not a field. The other 41 are frees of locals in ordinary functions.

Every genuinely-owning pointer field in core is freed by one of:

- `free()` / `malloc`-paired deallocation (`stream._bufA`/`_bufB`, `Surface._buf`,
  `TerminalSnapshot._buf`, `channel._seq`, `BlockAllocator._pages`, `EntryAllocator._pages`,
  every `_first_child` allocator chain),
- `delete[]` / `delete[_]` array deallocation (`array._ptr`, `list._data`, `queue._data`,
  `stack._data`, `hashset._keys`/`_status`, `dictionary._keys`/`_values`/`_status`,
  `stringbuilder._data`, `wstring._ptr`, `Bitmap._pixels`, `Directory._pathPtr`),
- an OS / allocator release call (`VMemRegion.base` via `os.vm_release`, `File._handle` via
  `_fs_fclose`, `Socket` via `close()`, `Terminal` via `restore()`, `page_arena._head` via
  `g_page_pool.release`, all `_largeHead` chains via `vmem_free`),
- a refcount (`ComPtr.ptr` via `reset()`/`Release`),
- or a recursive/intrusive chain walk the synthesized single-`delete` cannot replicate
  (`JsonNode`, `XmlNode`, `btree._root`, every allocator `_first_child`/`_head`).

None of these is a `delete field;`. The `unique` field mechanism, as implemented
(`MainListener.h:6659-6681` - single-indirection pointer only, no unions, no fixed arrays, no
array views), has no target in the current core library.

### Reconciled counts

| Metric | This survey | Plan's figure |
|---|---|---|
| struct/class declarations in `cflat/core` | 243 (16 are forward decls) | - |
| structs with >= 1 scalar pointer field | 105 | 144 |
| scalar pointer fields | 203 | 254 |
| fixed-array pointer fields (ineligible by rule) | 1 (`btree_node.children[17]`) | - |
| hand-written destructors in all of core | 35 (34 on structs with pointer fields) | - |
| fields freed by a scalar `delete` in a dtor | **0** | ~34 |

The count gap (203 vs 254, 105 vs 144) is partly real drift - the container migration at `54d6803`
removed and reshaped fields - but the shape of the 34-vs-0 gap is not a drift effect. It is a
classification-criterion gap: the earlier survey appears to have counted "the struct frees this
pointer somewhere in its teardown" as owning, without checking that the free was a scalar `delete`.
Under that looser criterion the owning count is roughly 30-35, which matches. Under the criterion
that actually determines migratability, it is zero.

Method note: the inventory was produced mechanically (struct-body depth-1 field extraction over all
88 `.cb` files), then every one of the 34 destructors was read. The per-field allocation-site tracing
was done exhaustively for the allocator/memory family and the container/string family; for the
OS/system, UI, and tree families the destructors were read in full and no candidate emerged, so
assign-site enumeration was not carried further for fields already excluded by their free mechanism.

## MIGRATABLE candidates

**None.** The table is empty. This is the finding, not a gap in the survey.

Do not pad this list. Adding `unique` to any field below would replace a correct `free()`,
`delete[]`, `vmem_free`, `Release`, or chain-walk with a single `delete` of one pointer - a
heap-corruption or mass-leak bug in every case.

## OWNING, but opts out

Grouped by reason. Every entry here is genuinely owned; none is migratable.

### Freed with `delete[]` / `delete[_]` - a buffer of N elements, not one object

| file:line | struct.field | type | Dtor also does |
|---|---|---|---|
| `array.cb:15` | `array<T>._ptr` | `T*` | per-element release loop for `unique` elements first (`:58-66`) |
| `list.cb:29` | `list<T>._data` | `T*` | per-element release loop (`:308`) |
| `queue.cb:21` | `queue<T>._data` | `T*` | `_releaseAt` loop (`:163-168`) |
| `stack.cb:21` | `stack<T>._data` | `T*` | element release loop |
| `hashset.cb:16,17` | `hashset<T>._keys`, `._status` | `T*`, `char*` | per-slot destruct loop (`:241-248`) |
| `dictionary.cb:22,23,24` | `dictionary<K,V>._keys`, `._values`, `._status` | `K*`,`V*`,`char*` | per-slot key/value release loop (`:425-443`) |
| `string.cb:632` | `stringbuilder._data` | `i8*` | nothing else |
| `wstring.cb:110` | `wstring._ptr` | `u16*` | nothing else |
| `graphic/bitmap.cb` | `Bitmap._pixels` | (pixel buffer) | nothing else (`~Bitmap` is one `delete[_]`) |
| `filesystem.cb` | `Directory._pathPtr` | `i8*` | nothing else (`~Directory` is one `delete[]`) |

### Freed with `free()` - malloc-paired, `delete` would corrupt the heap

| file:line | struct.field | Dtor also does |
|---|---|---|
| `stream.cb` | `stream._bufA`, `stream._bufB` | `cv_destroy` x2 + `_mu.destroy()` (`:208-215`) |
| `ui_native.cb:295` | `Surface._buf` | nothing else |
| `terminal.cb:707` | `TerminalSnapshot._buf` | nothing else |
| `channel.cb:54` | `channel._seq` | via `destroy()`, which also `delete[_]`s `_buf` |
| `block_allocator.cb:36` | `BlockAllocator._pages` | `vmem_free`s every page `_data` first (`:241-250`) |
| `entry_allocator.cb:46` | `EntryAllocator._pages` | via `cleanup()`; `vmem_free`s every page first |
| `arena_allocator.cb:286` | `ArenaAllocator._first_child` | also `cleanup()` on each child and on self |
| `bucket_allocator.cb:211` | `BucketAllocator._first_child` | same shape (`:617-631`) |

### Freed by an allocator / OS release call, not `delete`

| file:line | struct.field | Release mechanism |
|---|---|---|
| `vmem.cb:88` | `VMemRegion.base` | `os.vm_release` (VirtualFree/munmap). Cleanest dtor in core - it does nothing else - and still not migratable. |
| `page_arena.cb:45` | `page_arena._head` | `g_page_pool.release` or `vmem_free`, dual path (arena duality) |
| `arena_allocator.cb:277` | `ArenaAllocator._largeHead` | `vmem_free`, chain walk |
| `bucket_allocator.cb:200` | `BucketAllocator._largeHead` | `vmem_free`, chain walk |
| `fit_allocator.cb:67` | `FitAllocator._largeHead` | `vmem_free`, chain walk |
| `fit_allocator.cb:65` | `FitAllocator._segments` | `vmem_free(_base)` then `free(s)` per node |
| `arena.cb:26` | `Arena._head` | `free(blk->_data)` + `delete[1] blk` per node |
| `filesystem.cb` | `File._handle` | `__handle_tracker_remove` + `_fs_fclose` |
| `network/socket.cb:220` | `Socket` handle | `close()` |
| `terminal.cb:486` | `Terminal` console handles | `restore()` |
| `numa.cb:138` | `NumaThread._os` | OS thread kill/detach/join; **no destructor exists** |

### Shared / refcounted

| file:line | struct.field | Reason |
|---|---|---|
| `com.cb:186` | `ComPtr.ptr` | `~ComPtr()` calls `reset()`, which `Release()`s a COM refcount. `unique` models sole ownership; this is shared. |

### Conditional ownership

| file:line | struct.field | Reason |
|---|---|---|
| `numa.cb:139` | `NumaThread._pkt` | Freed on `join()`, freed on Windows-only paths in `release(true)`/`releaseThread()`, deliberately leaked on POSIX detach/kill (documented `numa.cb:231-232`). Platform- and path-gated. No destructor exists. |
| `threadpool.cb:804` | `ThreadPool._workers` | `~ThreadPool()` calls `shutdown()` only `if (_workers != nullptr \|\| _inlineMode)`; teardown joins threads before any free. |

### D2 recursion - the dtor walks a chain or tree of its own type

| file:line | struct.field | Reason |
|---|---|---|
| `json.cb:206` | `JsonNode.firstChild` | `~JsonNode()` (`:213-224`) walks the child list and `delete child` per node; `next` carries the chain. A scalar `delete firstChild` frees one node and leaks the rest. |
| `json.cb:208` | `JsonNode.next` | Owning (the chain walk frees through it), but only reachable via the recursive walk. Name looks like a borrow; it is not. |
| `xml.cb:211` | `XmlNode._firstChild` | `~XmlNode()` (`:217-227`), identical shape. |
| `xml.cb:213` | `XmlNode._nextSibling` | Same as `JsonNode.next` - owning, name-misleading. |
| `hpc/btree.cb:136` | `btree._root` | `~btree()` -> `clear()` -> `_freeSubtree` (`:1263-1290`), recursive over `children[]`. Scalar `delete _root` would leak every subtree. |

### Fixed-array field (rejected by `MainListener.h:6671-6672`)

| file:line | struct.field | Reason |
|---|---|---|
| `hpc/btree.cb:90` | `btree_node.children[17]` | `unique T* v[N]` is rejected today; also D2-recursive. See `internal/issue/unique-field-check-skipped-on-substituted-generic-type.md`. |

## BORROWING

**Counted, not itemized, as instructed.** Of the 203 scalar pointer fields, **78 are pure
borrowing** - nothing anywhere frees them on any path.

The full split:

| Category | Fields |
|---|---|
| BORROWING (never freed by anything) | 78 |
| OWNING, opts out (freed, but not by a scalar `delete` in this struct's dtor) | 125 |
| OWNING, migratable | 0 |
| **Total scalar pointer fields** | **203** |

Of the 134 fields on the 71 destructor-less structs, 65 are pure borrowing and ~56 are freed by
something other than their own struct: an owning outer struct's destructor (all the allocator-internal
chunk structs - `_ArenaBlock`, `_BA_*`, `_FA_*`, `_pa_chunk`, `btree_node` - are freed by
`Arena`/`BucketAllocator`/`FitAllocator`/`page_arena`/`btree`), or a hand-called
`destroy()`/`dispose()`/`cleanup()`/`close()`. None of those are migratable: the synthesized release
only runs at struct teardown, and these structs have no teardown to hook.

The remaining borrowing fields sit on destructor-bearing structs (`ArenaAllocator._current`, `._next_sibling`,
`BucketAllocator._next_sibling`, `EntryAllocator._currentPage`, `FitAllocator._freeHead`,
`page_arena._pool`, `page_arena._next`, `btree_node.next`, `btree_cursor._leaf`,
`string_view._ptr`, `view._ptr`, `span._ptr`, `Regex._src`, and the UI handle fields).

Note the plan's warning holds in both directions here and was re-confirmed:
`ArenaAllocator._first_child` owns while its adjacent `._next_sibling` borrows
(`arena_allocator.cb:286-287`, same type, same struct) - the child chain is freed by the *parent's*
walk, so a node never frees itself through `_next_sibling`.

## UNCLEAR

Nothing was left genuinely undetermined for migratability purposes - the scalar-`delete`-in-a-dtor
test is decisive and returned zero, so no field's verdict depends on a judgement call. Two items
where the evidence is genuinely incomplete:

1. **`ui_native/win32.cb:509` `Window.tooltip`** (assigned `:1497`). It is created and never
   explicitly destroyed anywhere in the file - the only one of `Window`'s HWND-ish fields with no
   teardown call in `dispose()`. Whether this is correct depends on Win32 owned-popup auto-destroy
   semantics, which was inferred, not verified. Not migratable either way (an HWND is not
   `delete`-freed), but it is worth a maintainer look. See findings below.
2. **`os.cb` `__OsThreadHandle._packet`, `numa.cb:139` `NumaThread._pkt`.** Both are deliberately
   leaked on the detach / POSIX-kill paths with an in-code rationale (a still-running thread may
   still read them). The design intent is documented; what was not verified is whether any caller
   relies on the *non*-leaking path exclusively.

Everything else in the survey resolved to a definite verdict.

Coverage note for the two largest UI/OS files, both of which resolved cleanly: `ui_native/win32.cb`
(23 pointer fields), `ui_native/cocoa.cb`, and `ui_native/winui.cb` contain **no destructor at all**
(grep for `^\s*~` over `cflat/core/ui_native/` returns nothing) - teardown is a hand-called
`dispose()`, and every owning field there is released by `DestroyWindow`, `DeleteObject`, an objc
`release`, or a custom refcount. `process.cb`'s 18 pointer fields are all OS handles or Win32
ABI-mirror parameter blocks; `~process()` (`:519-527`) terminates I/O threads then calls
`_closeHandles()`.

## Findings worth acting on independently

These are worth attention regardless of whether the `unique` migration ever happens.

1. **The plan's "~34 verified-owning fields" figure is wrong and should be corrected in
   `internal/plan/unique-ownership.md`.** NEXT item 1 describes work that has no targets. Left as
   written, it invites a future agent to migrate fields that must not be migrated - and the failure
   mode is a `free()`-vs-`delete` mismatch or a mass leak, both silent at compile time. This is the
   single most valuable output of this survey.

2. **`unique` on a FIELD currently has no use in core, and the mechanism's real value has already
   been captured elsewhere.** The Part II type-argument work (`list<unique T*>`,
   `list<unique IShape>`, `btree<K, unique V>`) is where ownership actually lives in this library.
   Core structs manage buffers and OS resources, not single owned objects. Consider whether the
   field qualifier should be documented as an application-level feature rather than a core-library
   migration target.

3. **`block_pool<T>` / `arena_channel<T>` have no destructor and require a manual
   `destroy_arenas()` / `destroy()` call.** A caller who drops an `arena_channel` without calling
   `destroy()` leaks every pooled arena and the whole bucket (`arena_channel.cb:158-169`, `:246-257`).
   Pre-existing lifecycle gap, unrelated to this migration.

4. **`page_pool` (`page_pool.cb:32-162`) has no destructor;** the global `g_page_pool` (`:165`)
   relies on an explicit `cleanup()` (`:140-154`) at shutdown to `vmem_free` cached pages.

5. **`NumaThread._pkt` leaks by design on POSIX detach/kill paths** (`numa.cb:231-232`). Documented
   and deliberate - recorded here so it is not "rediscovered" as a bug later.

6. **`threadpool.cb:861-871` leaks `cont.ctx` on the queue-saturation continuation-drop path.** When
   a continuation cannot be enqueued because the queue is saturated, the path decrements the inflight
   counter but never frees the continuation context (originally `TaskHandle._contCtx`). This is a
   real leak on a real (if uncommon) path, and the most concrete bug this survey turned up.

7. **Two double-init / double-start leaks with no guard:**
   - `stream.init()` has no re-init guard - calling it twice leaks the prior `_bufA`, `_bufB`,
     `_cvFull`, `_cvEmpty`.
   - `Thread.start()` (`thread.cb:208-209`, `:235-236`) has no guard against a second call - starting
     twice before joining leaks the prior OS handle and `_args`.

8. **`stop_token._state` (`stop_token.cb:5`) is a shared alias that looks owning.** Every token
   returned by `stop_source.get_token()` (`:27`) copies the same raw pointer, which
   `stop_source.dispose()` (`:44-49`) `free()`s. Tokens outliving a disposed source see a freed
   pointer - documented as by-design (`:43`), with no refcount or generation guard. Flagged because
   it is exactly the field a name-based or shape-based scan would misclassify as OWNING; migrating it
   would be actively wrong.

9. **`event.destroy()` (`event.cb:9`) calls `os.event_destroy(_h)` with no null check,** unlike the
   otherwise-parallel `semaphore.destroy()` (`semaphore.cb:27`), which is guarded. Minor asymmetry,
   cheap to fix.

10. **`ui_native/win32.cb:509` `Window.tooltip` has no teardown call** in `dispose()`, unlike every
    other window-handle field on that struct. See UNCLEAR item 1.

11. **`rwlock._lock` (`rwlock.cb:28`) is never destroyed anywhere in the repo.** On POSIX,
    `ensure_rwlock` (`os.posix.cb:275-281`) heap-allocates a `pthread_rwlock_t`. `rwlock.destroy()`
    exists (`rwlock.cb:62-64`) but **grep finds no caller** - not in core, not in `Test/`, not in
    `example/`. Every `rwlock` instance leaks it (`Test/test_sync.cb`,
    `Test/errors/err_lock_write_under_read.cb`). Invisible on Windows, where the SRWLOCK is inline
    and nothing is heap-allocated - which is likely why it was never noticed. This is the highest
    -value find in the survey after the corrected candidate count, and it is a good `internal/issue/`
    candidate.

12. **`CocoaHost.contentView` (`ui_native/cocoa.cb:150`) leaks per window on macOS.** It is created
    with `alloc/initWithFrame:` (`:3406`, +1 retain), but `dispose()` (`:1574-1601`) only nulls it at
    `:1600` - it never sends `release`, unlike every sibling field (`window`, `target`, `fontUI`,
    `fontTitle`, `fontCaption`, released at `:1593`, `:1596-1599`). Inferred from the asymmetry with
    the siblings plus a grep of every `release` send site in the file; there is no comment claiming
    the omission is deliberate.

13. **`__ProgramTLS.stdout_tls_buf` (`cruntime.cb:78`) and `.stdin_line` (`:82`) are never freed at
    program exit.** `stdout_tls_buf` is malloc'd/regrown at `:133`/`:139`; the trampoline exit sweep
    (`MainListener.h:21035-21106`) does not touch either. Bounded (one buffer each, reclaimed by the
    OS at process exit) but genuinely missing frees.

14. **No double-free risk and no reassignment-without-free was found anywhere in core.** Every grow /
   rehash / re-init path checked (`list._grow` `:45-46`, `queue._grow` `:38-39`, `stack._grow`,
   `hashset._rehash` `:76-100`, `dictionary._rehash` `:85-113`, `BlockAllocator._growPages` `:66-67`,
   `EntryAllocator` `:74-75`, `_AR_ChunkSet._rehash` `:101-109`, `VMemRegion.reserve_commit` `:94-97`)
   saves or frees the old buffer before reassigning. This is a genuinely clean result and is worth
   recording as such.

## Recommended batching

There is nothing to batch - the candidate list is empty, so there is no migration to sequence.

If the intent is still to exercise the `unique` field mechanism, the recommended order of work is
not a migration at all:

1. **Correct the plan.** Replace NEXT item 1 in `internal/plan/unique-ownership.md` with this
   survey's finding. One commit, no code change, no regression risk.
2. **Decide whether `unique` fields need a core consumer.** If yes, the only structurally plausible
   candidates would be *new* code, or a deliberate redesign of `JsonNode`/`XmlNode` to hold children
   in a `list<unique JsonNode*>` instead of a hand-rolled intrusive chain. That is a real design
   change with real behavior risk (allocation count, traversal order, parser hot path) and should be
   proposed on its own merits, not as "the field migration". It would also be verifiable: the
   HeapAudit oracle plus `Test/test_json.cb` / `Test/test_xml.cb` would pin it.
3. **Close the open field-shape hole** (NEXT item 3,
   `internal/issue/unique-field-check-skipped-on-substituted-generic-type.md`) independently. That is
   a live correctness gap and does not depend on any core field migration.

If a future change does introduce a migratable field, the verification bar stays as the plan states:
`./test.sh Release` green on macOS (or `test.bat` Release on Windows) plus a HeapAudit leak-count
check on the specific type, in its own commit.

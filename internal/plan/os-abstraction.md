# OS abstraction refactor (os.cb)

## Goal

Stop smearing platform dispatch across the stdlib. Today ~20 core files each carry
their own `if const (__WINDOWS__) { os.windows.X } else { os.posix.Y }` branches.
The `if const` should live in exactly one place per capability: `os.cb`.

## Four homes (decided)

1. Backend externs - `os.windows.cb` (`os.windows` ns) / `os.posix.cb` (`os.posix` ns):
   RAW OS-specific externs ONLY + pure platform-detail constants (struct sizes, flag
   values). Win32 API / POSIX syscalls. NO CRuntime externs. Only `os.cb` imports these.

2. CRuntime - `cruntime.cb` (the "except"): all portable C stdio + memory externs
   (fopen/fread/fwrite/fclose/fseek/ftell/feof, malloc/calloc/free, and the
   printf/scanf format primitives __stdio_common_* / __vsnprintf_chk / *_libc), plus
   the existing hook-aware printf/sprintf wrappers. These do NOT move into the backend
   files. cruntime.cb keeps its own `if const` where CRT vs libc names diverge.

3. os.cb (namespace `os`) - "what's remaining": neutral, STATELESS free-function
   dispatch. Does the `if const (__WINDOWS__)` exactly once per operation and calls the
   backend externs. Holds no long-lived state itself - state is owned by caller structs.

4. Stateful wrapper structs - their OWN top-level .cb files, OUTSIDE os.*.cb. Each owns
   its state (void* slot/handle, saved-termios buffer, ...) and calls os.cb free
   functions. They NEVER name a backend namespace.

## FS boundary (decided)

Only the portable C stdio + memory family is CRuntime (stays in cruntime.cb).
Directory/metadata ops - mkdir, stat, opendir/readdir, getcwd, chdir, unlink, rename,
access, realpath, Find*, GetFileAttributes, and MSVC's _mkdir/_access/_unlink/_getcwd/
_chdir - are OS-specific: externs -> backend files, neutral wrappers (os.mkdir,
os.is_dir, os.readdir_name, os.getcwd, os.rename, ...) -> os.cb. CRuntime = truly
portable C only.

## os.cb surface (stateless free functions)

Time:      os.sleep_ms, os.monotonic_nanos, os.monotonic_freq, os.wallclock_filetime
Sysinfo:   os.page_size, os.num_processors, os.cache_line_size,
           os.process_working_set_bytes, os.process_peak_working_set_bytes,
           os.process_private_bytes
Vmem:      os.vm_reserve, os.vm_commit, os.vm_reserve_commit, os.vm_release
           (os.cb owns the POSIX header-page length-stash so vm_release(ptr) is
            symmetric; vmem.cb loses its branches)
Mutex:     os.mutex_lock(void** slot), os.mutex_unlock, os.mutex_destroy
Cond:      os.cond_init(void** slot), os.cond_wait(void** cv, void** mtx),
           os.cond_signal, os.cond_broadcast, os.cond_destroy
Rwlock:    os.rwlock_read_lock(void** slot), os.rwlock_read_unlock,
           os.rwlock_write_lock, os.rwlock_write_unlock, os.rwlock_destroy
Sem:       os.sem_new(i32 initial, i32 max) -> void*, os.sem_wait, os.sem_post(n),
           os.sem_destroy
Event:     os.event_new -> void*, os.event_set, os.event_wait, os.event_destroy
           (Win real Event; POSIX backs it with mutex+cond+flag INSIDE os.cb)
Thread:    os.thread_create(function<void(void*)> start, void* arg) -> void* handle,
           os.thread_join(h) -> int, os.thread_timed_join(h, ms, bool* fin) -> int,
           os.thread_detach, os.thread_kill, os.thread_set_affinity(h, mask),
           os.thread_yield, os.thread_current_id, os.thread_hardware_concurrency
           (os.cb owns the two per-platform trampoline shims that adapt the neutral
            `start` to stdcall-int(win) / cdecl-void*(posix); removes thread.cb's 4
            duplicated trampolines)
Terminal:  os.term_make_raw(fd, void* saved), os.term_restore(fd, void* saved),
           os.term_poll_in(fd, ms), os.term_read_byte(fd), os.term_get_size(w, h)
           os.std_write(fd, buf, len), os.std_stream(which)
Filesystem:os.mkdir, os.rmdir, os.is_dir, os.access, os.unlink, os.rename, os.getcwd,
           os.chdir, os.opendir, os.readdir_name, os.closedir, os.realpath, temp path,
           dir enumeration (Find* wrapped neutrally)

Rationale for the void** slot params: the stateful STRUCT (mutex, rwlock, ...) lives in
its own file and owns the storage; os.cb only provides the stateless op that dispatches.
This honors "opaque wrappers, but not inside os.cb".

## Stateful wrapper structs (own top-level .cb)

Existing homes (keep): mutex.cb, rwlock.cb, semaphore.cb, latch.cb, thread.cb,
terminal.cb, filesystem.cb, process.cb, channel.cb, threadpool.cb.

NEW top-level .cb (decided):
- event.cb    - `event` struct (Win Event / POSIX mutex+cond+flag). Extract from
                latch.cb's inline hand-roll; latch.cb then just HOLDS an `event`.
- vmem.cb     - add a `VMemRegion { base, size }` struct with a destructor for RAII
                release, wrapping the os.vm_* free functions. Allocators can adopt it.

Stays put (decided):
- condvar - remains a struct INSIDE mutex.cb (a condvar is meaningless without its
            mutex, so co-locate). Promote the cv_* free funcs there to a `condvar`
            struct; it calls os.cond_* under the hood.

## Deferred (known exception)

- process.cb - NOT converted in this pass. Its Windows (CreateProcessA + handle
  plumbing) and POSIX (fork/exec/dup2/waitpid) paths are structurally divergent, not a
  thin call-swap, and it declares its own Win32 externs bound to local structs
  (_StartupInfoA/_ProcessInfo). Cleanly abstracting process-spawn is a separate design.
  It keeps naming os.windows/os.posix directly for now - the one acknowledged exception
  to "only os.cb names backends." Follow-up: design an os.proc_* surface.
- stream.cb, barrier.cb - need NO edits: they use only the signature-stable cv_* shims
  + the mutex/semaphore public API, no direct backend refs.

## Outcome (implemented)

Green on Darwin: `test.sh Release` = 147 passed, 0 failed, 18 skipped. The whole
POSIX/Darwin path is on os.* - the ONLY files still naming a backend namespace are
the two documented deferrals (process.cb; terminal.cb's Windows-console branches,
all inside dead `if const (__WINDOWS__)` blocks). New files: event.cb. New os.cb
additions beyond the original plan: heap_validate, temp_dir, make_temp_file
(closed the filesystem/allocator gaps the agents flagged).

Compiler bug found + fixed during integration (MainListener.h, the lambda/lock(this)
seeding path): the callee for a method call was resolved by BARE method name via
`functionTable[name].front()`, ignoring the receiver type. Adding VMemRegion.release()
reordered the overload list so a non-lock(this) `release` became front(), which
silently disabled guard-seeding and made atomic `release(val, lock(this) body)`
wrongly reject writes to guarded fields (broke test_sync / test_parallel /
test_hpc_kernels). Fixed to select the overload by receiver type + arity. Regression
decoy added to Test/test_sync.cb (ReleaseDecoy with a release() method).

Deferred / follow-up (Windows-side, not verifiable on Darwin here):
- process.cb - unconverted (structurally divergent spawn paths; see above).
- terminal.cb Windows console surface - GetConsoleMode/ReadConsoleInputA/_INPUT_RECORD
  decoding/screen-buffer/codepage. Needs a neutral os console-event API. POSIX side
  fully on os.*; Windows branches still name os.windows (dead on Darwin).
- Backend-extern prune - the CRT format primitives + fopen family were duplicated into
  cruntime.cb (canonical CRuntime home); the originals remain in os.windows.cb/os.posix.cb
  (harmless, share the one linkage symbol). A prune pass should remove the now-dead
  backend copies, but that needs a Windows build to confirm nothing else references them.
- MUST verify on Windows: `test.bat Release` (this refactor's Windows branches were not
  exercised on Darwin).

## Verification
- `test.sh Release` green on Darwin (here) per logical group.
- Later: `test.bat Release` green on Windows (branch not exercisable here).
- Watch: field-type churn - grep `._srw` / `._handle` / `._lock` / `._event` / `._cv`.

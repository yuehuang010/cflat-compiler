# UI Test Framework (ui_test) - design + plan

Status: T1-T3 DONE (2026-07-07). T4 stretch not started (needs separate approval).
Created: 2026-07-07
Module name decided: core/ui_test.cb (the recommended option).

## T1 status (2026-07-07)

Implemented `core/ui_test.cb` (UiTest facade + UiTestSuite runner + asserts +
waitUntil + per-case launch/teardown + --filter/--list) and migrated
`example/ui/win32_native_settings.cb` onto it. Gates all green:
- example.bat Release: 86 passed / 0 failed / 21 skipped (settings native
  self-test PASS + leak-clean under --heap-audit, exit 0).
- test.bat Release: all passed. test_lsp.bat: 198/0 (ui_test.cb swept clean; NO
  skip-list entry needed). ui_test.cb + cocoa_native_settings.cb both --check
  clean under --platform macos.

Hardening kits (reconcileStress/themeStorm/resizeStorm) and shot() are NOT built
(T2); the surface is left out entirely rather than stubbed, as instructed.

### API deviations (and why)

1. Driver name resolution = FORWARD DECLARATIONS. ui_test.cb imports only
   "ui_native.cb" (+ "time.cb" for waitUntil's sleep) and forward-declares every
   host/native* function it calls (prototype, no body). This mirrors gallery_app.cb's
   shared-import-closure mechanism (the backend a launcher imports supplies the
   bodies) but ALSO makes ui_test.cb `--check` clean standalone - a body-less
   prototype is legal, and prototype+backend-definition link without collision
   (verified). gallery_app.cb itself does NOT --check standalone; forward decls are
   the small improvement that satisfies the "ui_test.cb --check clean" gate.
2. Cases are stored via a `UiCase` wrapper class holding a
   `function<void(UiTest*)>` FIELD, kept in `list<UiCase*>`. Reason:
   `list<function<void(UiTest*)>>` does NOT monomorphize ("unknown type
   'function<...>'"); a function type is legal as a field/param but not as a generic
   container element. This is the sanctioned fallback (interface UiTestCase was the
   other option; the wrapper is lighter for consumers - no class-per-case).
3. UiTestSuite has NO destructor. `list<UiCase*>` OWNS its element objects and
   deletes each UiCase (destructing its name string + body closure) at teardown; an
   explicit `delete` loop double-freed and crashed the real CRT heap (0xC0000374) on
   exit even though it ran clean under --run. (Note: the "function<> field guard gap"
   hypothesis recorded here was later DISPROVEN 2026-07-07: the crash was `delete` of
   the `alias` borrow returned by `list.get(i)` - a double-free for ANY element type;
   closures were already guard-covered. DONE 2026-07-07: the compiler now rejects
   `delete <call>` when the callee returns an `alias` borrow (e.g. `delete l.get(i)`)
   at compile time - see Test/errors/err_delete_alias_result.cb and the positive
   coverage in Test/test_core.cb testListPtrOwnership.)
4. Two NEW host-neutral reader drivers added to ui_native_win32.cb ONLY:
   `bool nativeIsEnabled(string keyPath)` (IsWindowEnabled) and
   `int nativeTooltipCount()` (TTM_GETTOOLCOUNT on the window tooltip control). The
   facade's isDisabled()/tooltipCount() need them; the plan's reader list already
   anticipated isDisabled. Cocoa/WinUI parity for these two is DEFERRED to when their
   consumers migrate (T2/T3) - adding untested bodies to backends I cannot run would
   risk those gates, and no T1 consumer needs them off-Win32 (ui_test.cb only
   forward-declares them, which --checks clean everywhere).
5. require semantics: a failed requireTrue sets an `aborted` flag; subsequent
   asserts/actions on that UiTest become no-ops (skipped, uncounted) rather than
   aborting the process - "skips the rest of the case" without needing exceptions.
6. Settings migration expanded the original 8 sequential checks into 13 asserts
   across 3 cases (each with a fresh launch: created+initial-state, interaction-flow,
   tooltip). Coverage preserved/grown, same --selftest flag + exit-code contract,
   every raw SendMessageA/BM_CLICK/TBM_SETPOS/IsWindowEnabled/GetWindowTextA deleted.
7. cocoa_native_settings.cb was NOT migrated. Its self-test uses cocoa-only host
   internals (createNativeWindow/_nCtx/_nHost, raw obj-c msg sends) and covers
   cocoa-only concerns (menu routing, windowShouldClose veto, resize) that have no
   host-neutral driver; the cocoa backend also lacks nativeIsEnabled/nativeTooltipCount.
   Forcing it onto the runner now would SHRINK coverage. It stays green + unchanged.
   Full cocoa migration folds into T2/T3 alongside the driver-parity work.

## T2 status (2026-07-07)

Extracted the hardening kits + `shot()` into `core/ui_test.cb` as library API, migrated the
gallery (27) and fedit (14) self-tests onto the runner, and added cocoa+winui driver parity.
All gates green:
- example.bat Release: 86 passed / 0 failed / 21 skipped. gallery (27/27 + 3-case Win32
  hardening 4/4), fedit (9/9 + 6/6), win32_native_settings (T1, still green), and
  winui_gallery (27/27) workers all PASS + leak-clean under `--heap-audit`.
- test.bat Release: all pass. test_lsp.bat: 198/0 (ui_test.cb + ui_native_win32/cocoa all
  sweep clean; NO new skip-list entry). cocoa `--check --platform macos` clean for ui_test.cb,
  ui_native_cocoa.cb, cocoa_native_settings.cb, gallery.cb, fedit.cb.
- gallery `--shots out` still writes gallery_light.bmp + gallery_dark.bmp (now via the library).

Kit signatures (all in ui_test.cb):
- `bool reconcileStress(int steps, Lambda<void()> mutate, Lambda<list<string>()> expectedKeys)` -
  expectedKeys returns a `list<string>` of key paths (deviation 3 FIXED - see below).
- `bool themeStorm(int flips, string themeBtnKey, Lambda<bool()> modelDark)`.
- `bool resizeStorm(string rootKey, list<UiSize> sizes, Lambda<bool()> sanity)` +
  `struct UiSize { int w; int h; }` / `UiSize uiSize(int w, int h)`.
- `bool uiShot(string bmpPath)` + `bool UiTest.shot(string path)`; `UiTestSuite.run` gained
  `--shot-dir <dir>` (auto-capture a per-case shot on failure).

### T2 deviations (and why)

1. shot() is NOT physically in ui_test.cb. The Win32 PrintWindow+GetDIBits+BMP grab lives in
   the Win32 backend as `nativeSaveWindowBmp(string)` (ui_native_win32.cb, which already imports
   windows.h + gained `import "graphic/bitmap.cb"`); cocoa/winui define it returning false;
   ui_test.cb forward-declares it and exposes the portable `shot()`/`uiShot()` verb. Reason:
   putting windows.h/GDI in ui_test.cb would (a) collide GDI externs with the win32 backend's
   windows.h binding (the "extern name collision now errors" rule), (b) need curWin()/Window
   which are not host-neutral types, and (c) force ui_test.cb onto the LSP-sweep skip-list
   (windows.h-importing core files are skipped) - losing T1's "no skip needed" property. The
   delegation keeps ui_test.cb windows.h-free, --check-clean, swept clean (198/0), and still
   meets the intent (portable verb; gallery --shots calls it; Win32-only; false elsewhere).
2. The kit closure params are `Lambda<...>`, NOT `function<...>`. In this codebase `function<T>`
   is the non-capturing C-function-pointer type and `Lambda<T>` is the capturing owning closure
   (ui_native.cb's onPress/post/onChange are all `Lambda<>`). The kits receive capturing lambdas
   (they close over the app `g`), so they must be `Lambda<>`; passing a capturing lambda to a
   `function<>` param errors ("cannot pass to C function-pointer parameter"). waitUntil's pred was
   also changed function<bool()> -> Lambda<bool()> for the same reason. UiCase.body stays
   `function<void(UiTest*)>` (case bodies are non-capturing; they read app state via a local
   downcast and hand Lambda predicates that capture it to the kits).
3. FIXED - now `list<string>`. reconcileStress's expectedKeys formerly returned a comma-separated
   STRING as a workaround: a generic type argument to a closure type (`Lambda<list<string>()>` /
   `function<list<string>()>`) did not monomorphize ("unknown type 'list<string>'"). That gap
   (closure-generics monomorphization gaps a+b) is now closed - see
   internal/plan/closure-generics-monomorphization.md - so expectedKeys returns a real
   `list<string>` and the kit no longer splits a CSV. The related T1 deviation 2
   (`list<function<...>>`) is likewise now expressible.
4. ui_test.cb no longer `import "time.cb"` - it forward-declares `void sleep(i64 ms)`. Reason:
   time.cb's `Duration` type collides with the WinUI winmd projection, and the WinUI gallery now
   links ui_test.cb (gallery_app.cb imports it). Only waitUntil() calls sleep; a consumer using
   waitUntil must have sleep in scope (import "time.cb"). No current consumer calls waitUntil, so
   the uncalled method's sleep reference is DCE'd - Win32 gallery/fedit/settings link fine.
5. The gallery suite is ONE case (full element set + both hardening kits in one launch), not
   case-per-concern. The WinUI host CRASHES (exit 127) on a second `startHeadlessWindow` after
   `nativeTeardownForTest` within one `Application.Start` session (no relaunch support); a
   multi-case suite = multiple launches. gallery_app.cb is shared by both hosts, so it stays
   single-launch. The Win32-only win32HardeningSelfTest (3 cases), fedit (2 cases), and settings
   (3 cases, T1) DO use multiple launches - the Win32 host supports relaunch.
6. The WinUI backend gained 8 driver bodies so the ui_test.cb facade links into the WinUI gallery:
   `pumpNative`/`hostDrainPosted` (map onto pumpWinui/the DispatcherQueue) and `nativeTestFireMenu`
   (routeMenu) are real; `nativeResizeClient`/`nativeAccessibleName`/`nativeFocusFirst`/`nativeFocusNext`/
   `nativeFocusedKey` are stubs backing Win32-only hardening drivers that no WinUI gate exercises.
7. cocoa+winui `nativeIsEnabled` = element-model / live-control read (cocoa NSControl.isEnabled;
   winui per-kind element `disabled`); `nativeTooltipCount` returns a documented 0 on BOTH (their
   tooltips are per-view/attached, not a per-window registry; the settings tooltip assert that
   exercises the count is Win32-only). These only need to compile+link for the shared facade.
8. fedit calls setupMenu()+wireHandlers() per case (before each launch) - nativeTeardownForTest
   moves out the menu model + command/close handlers, so a later case must re-establish them or
   its menu-routed asserts (new tab / new window / context) no-op.
9. fedit's Win32-only P9 block switched from `if const (!__MACOS__)` (folds neither branch;
   type-scans both) to the correct `if const (__MACOS__){}else{...}` simple-ident gating.

## T3 status (2026-07-07)

Shipped the customer-facing surface: the copy-me template `example/ui/testing/` (todo_app.cb +
todo_test.cb), the `example.bat` `--worker-uitest` gate, and the doc/UI.md "Testing your app"
chapter. All gates green:
- Template: `x64/Release/cflat.exe example/ui/testing/todo_test.cb -i example/ui/testing
  --heap-audit -o out/todo_test.exe` then `out\todo_test.exe --selftest <nul` -> 6/6 cases,
  exit 0, leak-clean. `--list` prints the 6 case names; `--filter add` runs the 2 matching cases.
- example.bat Release: 87 passed / 0 failed / 23 skipped (was 86/0/21 in T1-T2). +1 passed is the
  new `--worker-uitest` PASS (`todo_test.cb` - uitest template self-test + leak-clean); +2 skipped
  is todo_app.cb + todo_test.cb, both in the plain-sweep EXCLUDE list (the sweep prints a SKIP line
  per excluded file), so they are not double-run as ordinary examples.
- test.bat Release: all pass. test_lsp.bat: 198/0 (todo_test.cb sweeps clean; todo_app.cb added
  to the bulk-sweep skip list next to gallery_app.cb - same host-neutral-module reason).
- Cocoa `--check --platform macos` clean for todo_test.cb (which pulls todo_app.cb).

### T3 case list (todo_test.cb)

1. initial state - keyed controls exist, empty list, "No tasks yet", Add + Delete disabled.
2. add a task - Add disabled->enabled on type, click Add, row lands, field clears, status "1 task(s)".
3. add several tasks - three tasks; virtualized cell text readback per row.
4. select, delete, and complete - listSelect enables Delete, delete shifts rows, listActivate completes.
5. background load lands via ctx.post - the waitUntil case: "Load sample" spawns a worker thread that
   posts sample items back; the test waits for rowCount == 3.
6. theme storm hardening kit - themeStorm(12) keeps hostDark() in lockstep with the model.

### T3 deviations (and why)

1. The todo TEST target is a PURE test runner (main = `s.run(argc, argv)`), NOT an app+test hybrid
   like settings/gallery/fedit. This is the cleanest illustration of the app-module / test-target
   split for the customer to copy; the live app is `todo_app.cb`'s Component, launched by any host.
2. The app module `todo_app.cb` imports `thread.cb` + `time.cb` (not `ui_test.cb`): the background
   "Load sample" worker sleeps then `ctx.post`s items back, the seam the waitUntil case demonstrates.
   The test target imports `time.cb` explicitly (waitUntil needs `sleep()` in scope) even though
   todo_app already drags it in - a copied test file must stand on its own imports.
3. `_todoLoader` (the worker) is defined AFTER `class TodoApp`, not before: a free function that
   reads a class FIELD (`app.uiCtx`) needs the class fully defined ("Unknown identifier 'uiCtx'"
   otherwise). Mirrors fedit's `_postWorker` placement. Method->free-function forward refs are fine
   (the scanner registers signatures); it is the field ACCESS that needs the body first.
4. The worker joins itself indirectly: the posted closure runs `finishLoad()` on the UI thread,
   which `join()`s the (now-finished) worker and frees its thread packet, so the case is leak-clean
   under `--heap-audit`. A `loaderStarted` guard + a destructor join is the safety net (never fires
   in the tested flow, since waitUntil guarantees the post drained before teardown).
5. `this.todos.count().toString()` did not parse (a call-result `.toString()` chain bound the whole
   receiver, not the int); bound `int n = this.todos.count();` then `n.toString()`. Cosmetic, but
   worth noting in the template so the copy-me code is clean.
6. Only `todo_app.cb` is added to the LSP bulk-sweep skip list (it calls backend-only `hostWidth()`
   standalone); `todo_test.cb` resolves `import "todo_app.cb"` via same-directory search (the sweep
   passes `-i example/ui`, not `example/ui/testing`, exactly as gallery.cb resolves gallery_app.cb),
   so it needs no skip entry.

## Problem

The ui_native framework ships with strong test *primitives* but no test *framework*:

- The host-neutral `native*` drivers (nativeClickButton/nativeTypeText/nativeListSelect/
  nativeFocusNext/... - full list in doc/UI.md "Host-neutral test drivers") are shipped
  core surface in the three backends, resolved by key path via UiContext.nativeHandle.
- Our own self-tests (gallery 27, fedit 14, settings 8) each hand-roll the same
  boilerplate: startHeadlessWindow, pass++/total++ counters, printf("FAIL..."),
  return exit code, drain posted work, teardown. win32_native_settings.cb still
  uses raw SendMessageA (the pattern the ui-native plan says to retire).
- A CFlat customer who builds an app on ui_native has NO documented way to automate
  it: no runner, no assertion helpers, no waits, no template to copy.

Goal: one library that (a) our example.bat UI gates migrate onto, and (b) customers
import to automate their own ui_native apps - same code path, so the framework is
dogfooded by every gate run.

## Design

### Scope decision: in-process automation first

Tests compile INTO a test target alongside the app's Component (the gallery_app.cb /
gallery.cb split is the proven pattern: shared app module + launcher). The test target
creates a headless (invisible) window, drives it through the native* drivers, and
asserts on control + model state. This is deterministic (no OS input synthesis, no
timing flake), works in CI with no display, and reuses everything P0-P14 built.

Out-of-process automation (attach to a running exe, UIA/record-replay) is explicitly
deferred to T4-stretch. The in-process API is designed so an out-of-process transport
could later implement the same UiDriver surface.

### Layering

```
Layer 2  UiTestRunner   - case registration, per-case setup/teardown, filter,
                          summary report, process exit code        (NEW, core/ui_test.cb)
Layer 1  UiDriver +     - fluent facade over the native* drivers: click/type/
         UiAssert         select/read + rich failure messages, waitUntil,
                          hardening kits (stress soak, theme storm) (NEW, core/ui_test.cb)
Layer 0  native* drivers - host-neutral state read/write, SendMessage + route*
                          on invisible windows                     (EXISTS, backends)
```

Layer 1/2 are host-neutral by construction: they call only native* drivers and
ui_native.cb APIs, so they run on Win32 + WinUI and compile-check on Cocoa.

### Module: core/ui_test.cb

One new core module, `import "ui_test.cb";`. Opt-in (not in the runtime.cb
auto-import closure, so the "20 core libraries" --init count is unaffected).
Deployed by the existing CMake cflat/core/* glob - no build-system work.

### API sketch (Layer 1 + 2)

```cflat
import "ui_test.cb";
import "myapp.cb";            // the customer's shared app module

int main(int argc, char** argv)
{
    UiTestSuite s = uiTestSuite("myapp");

    s.test("save button enables after edit", (UiTest* t) => {
        t.launch(new MyApp(), 80, 30);          // headless window, fresh per case
        t.type("root/name", "hello");
        t.expectTrue("save enabled", !t.isDisabled("root/save"));
        t.click("root/save");
        t.expectText("status", "root/status", "Saved");
    });

    s.test("background post lands on UI thread", (UiTest* t) => {
        t.launch(new MyApp(), 80, 30);
        t.click("root/refresh");                 // kicks a worker + ctx.post
        t.expectTrue("refreshed",
            t.waitUntil(() => t.controlText("root/status") == "Done", 2000));
    });

    return s.run(argc, argv);                    // exit code = gate
}
```

UiTest (per-case context; wraps a launched headless window):
- launch(Component app, int wDip, int hDip) / relaunch() - startHeadlessWindow +
  registers teardown (nativeTeardownForTest) even when the case fails early.
- Actions (thin facades, one per native* driver, same key-path addressing):
  click(key), type(key, s), comboSelect(key, i), listSelect(key, row),
  listActivate(key, row), tabSelect(key, i), sliderSet(key, v), fireMenu(cmd),
  fireContextMenu(key, i), splitterDrag(key, dx, dy), resize(w, h),
  focusFirst()/focusNext(), setText(key, s), post-drain via pump().
- Readers: controlText(key), isChecked(key), comboSelected(key), listRowCount(key),
  listCellText(key,row,col), progressValue(key), statusText(key), bounds(key),
  accessibleName(key), focusedKey(), exists(key), accentBg(key), customDrawFill(key).
- Asserts (record pass/fail with the KEY PATH + expected/actual in the message,
  never abort the process; a failed require() skips the rest of the case):
  expectText/expectInt/expectBool/expectTrue(name, ...), requireTrue(name, cond).
- waitUntil(function<bool> pred, int timeoutMs) - loop: pump() + hostDrainPosted()
  + pred(), sleep 10ms between polls; returns bool. The ONLY place time appears;
  everything else stays synchronous.
- Hardening kits (generalized from gallery_app.cb):
  reconcileStress(key prefix, u32 seed, int steps, function<void> mutate) - the
  48-step census+identity soak as a library call; themeStorm(int flips);
  resizeStorm(list<Pair<int,int>> sizes).
- Screenshot: shot(const char* bmpPath) - promotes gallery.cb saveWindowBmp
  (PrintWindow + GetDIBits) into the library, Win32-gated (Cocoa/WinUI: no-op
  returning false, documented). Auto-shot on first failure when --shot-dir is set.
  Pixel probes stay per-key (customDrawFill); NO golden-image compare in v1 -
  font/DPI variance makes it flaky; document the caveat and leave byte-compares
  to the customer.

UiTestSuite (runner):
- test(const char* name, function<void(UiTest*)> body) - registration.
- run(argc, argv) - executes cases sequentially (one window at a time - the
  backends are single-UI-thread), each in a fresh launch/teardown; prints
  per-case PASS/FAIL lines + "SUITE myapp: N/M passed"; returns 0/1.
- Flags parsed by run(): --filter <substr> (run matching cases), --list
  (print case names, exit 0), --shot-dir <dir> (failure screenshots).
  Unknown flags ignored so app-specific flags coexist.
- Output convention preserved: printf FAIL lines + exit code, so existing
  example.bat mechanics (findstr on heap-audit LEAK + errorlevel) work unchanged.

### What our gates migrate to (dogfooding)

- win32_native_settings.cb: rewrite its 8 asserts on UiTestSuite, deleting every
  raw SendMessageA - this retires the legacy pattern flagged in the ui-native plan.
- gallery_app.cb selfTest + win32HardeningSelfTest: port the 27 asserts; the
  stress soak + theme storm become calls into the library kits (the library
  versions are extracted verbatim first, so behavior is identical).
- fedit.cb selfTest: port the 14 asserts (menus via t.fireMenu).
- example.bat: worker invocations unchanged (same exes, same --selftest flags,
  same <nul + heap-audit gating). Only the .cb internals change.

### Customer-facing story

- doc/UI.md gains a "Testing your app" chapter: the shared-app-module pattern,
  key discipline (every element you want to automate gets setKey - unkeyed nodes
  fall to positional #index paths which break on reorder), the UiTest API table,
  the waitUntil rule (only for post/thread work), CI recipe (compile test target
  with --heap-audit, run with <nul, gate on exit code + LEAK grep), and the
  headless caveats (NM_CUSTOMDRAW accents invisible to PrintWindow; WinUI needs
  the WinAppSDK runtime and cannot run under --heap-audit).
- New template example example/ui/testing/: a small todo app (todo_app.cb, shared
  module) + todo_test.cb (suite with ~6 cases incl. one waitUntil case) - the
  copy-me starting point. Wired into example.bat as --worker-uitest.

### Explicitly deferred (T4 stretch, do not block)

- Out-of-process automation: a --automation named-pipe channel in the host
  (serve the UiDriver verbs over JSON) OR UIA provider work. The Layer 1 verb
  set is the wire protocol candidate; nothing in v1 may preclude it.
- Record/replay of interaction traces.
- Golden-image visual regression.
- Unifying Test/test_helper.cb (compiler unit tests) with ui_test - different
  audience and conventions; NOT in scope.

## Phases

- T1 core/ui_test.cb: UiTestSuite/UiTest/asserts/waitUntil/teardown + migrate
  win32_native_settings.cb as the proving consumer. Gate: example.bat green
  (settings worker 8/8 on the new runner, leak-clean), test.bat + test_lsp
  green (new core module analyzes clean; add LSP sweep entry only if the
  os.posix-style host-alternate collision appears).
- T2 migrate gallery (27) + fedit (14) onto the runner; extract stress/theme/
  resize kits + shot() into the library (extract-then-call, byte-equal asserts).
  Gate: example.bat counts unchanged, doc/UI.md hardening section updated.
- T3 customer surface: doc/UI.md "Testing your app" chapter + example/ui/testing/
  template + --worker-uitest. Gate: template suite green under heap-audit;
  WinUI + Cocoa compile-check of ui_test.cb + template.
- T4 (stretch, separate approval): out-of-process channel spike.

Compiler work expected: NONE. function<void(UiTest*)> closures and owned-string
key paths ride machinery proven in P0-P14; if a compiler gap surfaces, file it
in internal/issue/ and escalate rather than working around silently.

## Risks / open items

1. Closure-heavy runner under --heap-audit: every case body is an owning
   function<> - lifetime audit is part of the T1 gate (same discipline as the
   ui-native menu/post closures).
2. One-window-at-a-time invariant: the runner enforces sequential cases; parallel
   suites are out of scope (hosts are single-UI-thread).
3. Cocoa remains compile-verified only (no mac box) - ui_test.cb inherits that
   debt; runtime verification joins the existing pre-existing mac TODO.
4. WinUI: no heap-audit (WinAppSDK), some drivers read the element model rather
   than real control state - the doc chapter states the per-host guarantee level.
5. Naming: module name `ui_test.cb` (recommended; short, room to later cover
   canvas-host testing) vs `ui_native_test.cb` (family-consistent). USER DECIDES
   before T1.

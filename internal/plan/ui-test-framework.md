# UI Test Framework (ui_test) - design + plan

Status: PLANNED (design approved pending user review; no implementation started)
Created: 2026-07-07

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

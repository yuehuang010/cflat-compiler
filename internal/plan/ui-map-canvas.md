# UI framework: map canvas (infinite pan/zoom surface with on-demand tiles)

Status: M1+M2+M3+M4 DONE, M5 NOT STARTED (2026-07-12, all uncommitted; see
per-milestone status blocks below). Next milestone after ui-visual-polish.md. Goal:
the framework can host a Google-Maps-style control - an unbounded 2D world
viewed through a pan/zoom camera, rendered from 256px tiles that are loaded
on demand, cached, and evicted.

Status - M1+M2: DONE (2026-07-12). Interactive CanvasView input (pointer/wheel/
pinch, POINTS coords, opt-in via setOn*/has* flags) + canvas image handles
(canvasCreateImage/canvasReleaseImage + Canvas.drawImage) landed on the Cocoa
host; TUI host + drivers headless-testable; Win32/WinUI recorded as a parity gap
(internal/issue/ui-native-canvas-input-images-win32-winui.md). Gates on the mac
box: tui_demo 22/22 (was 19), gallery 35/35, fedit pass, test.sh 160/0,
example_mac.sh 34/0; live cocoa event path smoke-verified (no crash). Docs in
doc/UI.md. See the annotated M1/M2 sections below for the shipped seam.

## Framing

This is NOT a ScrollView (there is no finite content extent). The public
construct is a camera: center (double world coords), zoom, viewport. The
deliverable splits into reusable framework primitives (an interactive,
image-capable CanvasView + a timer seam) and a pure-.cb tile engine on top.
RN vocabulary note: RN itself has no map primitive (react-native-maps wraps
the native map control); our shape is "interactive canvas + library control",
which also unlocks custom widgets generally (charts, editors, games).

## Verified gaps in today's framework

1. CanvasView is output-only: it has onPaint(Canvas) and a false dispatch();
   native hosts never route pointer input to elements, and no wheel/pinch
   event exists anywhere in the framework (same finding as the ScrollView
   work). The Cocoa CfCanvasView is already a custom NSView subclass with a
   drawRect: bridge (cocoa.cb ~2891), so adding mouseDown:/mouseDragged:/
   mouseUp:/scrollWheel:/magnifyWithEvent: overrides that call boxed cflat
   closures follows an existing pattern.
2. Canvas has exactly drawText + drawRect (ui_native.cb ~330). No image
   blitting, and no retained image objects - tiles need a handle API so the
   host wraps pixels into a CGImage/HBITMAP once, not per frame.
3. Layout cells (8x26pt on Cocoa) are far too coarse for panning. Canvas
   paint is already CG-backed points internally; the new input events and
   draw calls must carry point (double) precision. Cells stay for layout.
4. No timer: ctx.post() marshals worker->UI (solved), but nothing fires
   LATER - needed for inertia, animated zoom, and retry backoff.
5. Async loading itself is already solved: thread<T> worker + ctx.post to
   deliver a tile + ctx.invalidate() is the established pattern. Only
   dedupe/cancel logic is new, and that is library code.

## M1: interactive CanvasView (framework + Cocoa host)  [DONE 2026-07-12]

Shipped: handlers are opt-in via `cv.setOnPointerDown/Move/Up/setOnWheel/setOnPinch`
(each arms a `has*` flag; the hosts gate on it - an unset handler is never called,
a no-handler canvas is byte-identical in toJson/propsEqual). Coordinates are POINTS.
Boxing was NOT needed for input: the Cocoa IMPs resolve the live element per event
(like `_onListClickImp`) and call the flagged handler; only the paint callback stays
boxed (it fires on OS-driven drawRect: outside the pump). Drivers: `nativeCanvasPointer/
Wheel/Pinch` (cocoa + term hosts; win32 no-op). NSPoint reads use a 2-double HFA struct
return (PointD) like RectD; NSPoint args are decomposed to scalar doubles. Win32/WinUI
parity gap recorded.

- CanvasView gains optional handlers (all default, feature is opt-in):
  onPointerDown/onPointerMove/onPointerUp(double x, double y, int buttons),
  onWheel(double x, double y, double dx, double dy, bool precise),
  onPinch(double x, double y, double scale)  // trackpad magnify
  Coordinates are POINTS relative to the canvas origin (not cells).
- Handlers cross the host seam via heap-boxed closures, mirroring the
  existing onPaint/_boxCanvas mechanism (monomorphizer limit: no Lambda
  across the NativeHost interface).
- Cocoa: CfCanvasView overrides the five NSResponder methods; drag capture
  comes free (Cocoa keeps sending mouseDragged: to the mouseDown view).
  scrollWheel uses hasPreciseScrollingDeltas for trackpads.
- Interaction with the native ScrollView host mapping: a CanvasView with a
  wheel handler consumes the wheel itself (it will typically not live inside
  a ScrollView; document that nesting sends wheel to the innermost view).
- Host-neutral test drivers (ui host seam + all hosts' test backends):
  nativeCanvasPointer(path, phase, x, y), nativeCanvasWheel(path, dx, dy),
  nativeCanvasPinch(path, scale) - so self-tests can pan/zoom headlessly.
- Canvas/TUI host: pointer events map from the existing cell-based Event
  dispatch (multiply by cell size); wheel/pinch are no-ops there.
- Win32/WinUI: record parity gap in internal/issue/ (extend the visual-
  polish parity file's pattern; one file per issue).

## M2: canvas image handles (framework + Cocoa host)  [DONE 2026-07-12]

Shipped: `u64 canvasCreateImage(u64 bgraPixels, int w, int h)` (COPIES the bytes via
NSBitmapImageRep; caller keeps its buffer) + `void canvasReleaseImage(u64 img)` as host
free functions (forward-declared in ui_native.cb, defined per host); `Canvas.drawImage(
u64 img, double dx, double dy, double dw, double dh)` added to the interface (POINTS,
linear). Cocoa wraps once into a retained NSImage and blits with `drawInRect:...:
respectFlipped:YES` into the flipped CfCanvasView context (handles CG y-up vs flipped
view). TUI/Win32: drawImage no-op, canvasCreateImage returns a nonzero DUMMY. Kept fully
separate from the Image element's setImageData path. Started with linear scaling only
(no src-rect overload yet).

- Host seam additions:
  u64  canvasCreateImage(u64 bgraPixels, int w, int h)   // copies pixels
  void canvasReleaseImage(u64 img)
- Canvas draw addition:
  void drawImage(u64 img, double dx, double dy, double dw, double dh)
  optionally a src-rect overload for atlas use; nearest-vs-linear scaling
  flag (maps zoom wants linear while animating, nearest at rest is fine -
  start with linear only).
- Cocoa: wrap pixels into a CGImage once at create; drawImage is
  CGContextDrawImage into the CfCanvasView's CG context. Release frees.
- drawRect/drawText gain nothing; existing cell-based calls unchanged.
- Canvas/TUI host: drawImage is a documented no-op (or dithered block fill
  later - out of scope).
- Regression risk: Image element already uploads bitmaps (setImageData
  seam); keep the two paths separate - Image is a native control, canvas
  images are paint-time objects.

## M3: tile engine library (pure .cb, host-independent - can run in
## parallel with M1/M2)

New library file core/ui_map.cb (or example-local first, promoted later;
decide at implementation - core promotion follows the ui_native precedent).
All logic headless-testable with no host:

- Camera: centerX/centerY (double, world units at zoom 0), zoom (double,
  fractional; tiles render from level floor(zoom) scaled by 2^frac).
  Functions: worldToScreen/screenToWorld, panBy(points), zoomAt(anchor
  point, dz) - anchor-preserving zoom math (the cursor stays over the same
  world point).
- Tile addressing: slippy scheme - level z is a 2^z x 2^z grid of 256px
  tiles; key (z, tx, ty). visibleTiles(camera, viewportW, viewportH) ->
  list of keys + destination rects (points).
- TileCache: LRU keyed by (z,tx,ty), capacity in tiles; visible tiles are
  pinned (never evicted while on screen); stores canvas image handles (u64)
  plus a host-release callback so eviction frees GPU/CG objects.
- TileScheduler: dedupes in-flight requests, cancels requests that scroll
  offscreen before starting, caps concurrent fetches, retry-with-backoff
  seam (needs M5 timer; until then failed tiles refetch on next paint).
- Placeholder policy: while (z,tx,ty) is missing, draw the best available
  ancestor tile scaled up (crisp-enough Google-style progressive load).
- App contract (the "loads on demand" seam):
  tileSource: function<void(int z, int tx, int ty)> - the app starts a
  worker fetch/generate and later calls mapTileReady(z, tx, ty, pixels)
  from the UI thread (worker uses ctx.post), which uploads via
  canvasCreateImage, inserts into the cache, invalidates.

### M3 landed

- File: `example/ui/09-map/map_engine.cb` (new). Pure host-independent
  library - no main(), imports only `math.cb`, `list.cb`, `dictionary.cb`.
  Not wired into `example.bat`/`example_mac.sh` yet (no launcher/self-test
  entry there; M4 will add one following the 05-gallery split).
- API surface: `ScreenPt`/`WorldPt` (struct-return, not out-params) for
  `MapCamera.worldToScreen`/`screenToWorld`/`panBy`/`zoomAt`/`visibleTiles()`;
  `TileKey{z,tx,ty}` with `pack()`/`static unpack(u64)` (u64 layout: bits
  [63:44]=z, [43:22]=tx, [21:0]=ty - safe to z~20); `VisibleTile{key,destX,
  destY,destW,destH}`; `TileCache` (capacity LRU, `tryGet(key,u64*)`,
  `insert`, `markVisible(list<TileKey>)` resets pinning each call,
  `hitCount`/`missCount`/`evictCount`, `init(cap, onEvict)` release
  callback); `TileScheduler` (`request(key, TileCache*)` dedupes vs.
  cache+queued+in-flight, `drainReady() -> list<TileKey>` pulls newly
  dispatchable keys up to `maxInFlight` - chosen over a bool return so a
  frame's completions and new requests can be batched into one drain call;
  `cancelNotIn(list<TileKey>)` only drops QUEUED requests, never in-flight;
  `tileReady(key,u64,TileCache*) -> bool`, no retry/backoff, an in-flight
  tile that never completes is simply forgotten until M5's timer seam);
  free function `bestAncestor(TileCache*, TileKey) -> AncestorResult`
  (found flag + ancestor key + sx/sy/sw/sh sub-rect fractions).
- Scratch harness (35 assertions, all pass, exit 0):
  `/private/tmp/claude-501/-Users-felixhuang-source-cflat-compiler/5a29d9c1-c81a-4bd6-b349-4312d353c44c/scratchpad/map_engine_check.cb`.
  Lives OUTSIDE the repo intentionally - fold its assertions into the real
  M4 self-test (`ui_test.cb` pattern) once MapView exists; do not leave the
  scratch file as the only coverage.
- Compiler quirk found and worked around (NOT a compiler bug fix - none
  made, per scope): inside a struct method, `field = param;` silently
  no-ops (field keeps its default/null value) when `param`'s name is an
  exact prefix of `field`'s name - e.g. `releaseImage = release;` never
  actually stored the callback, so `TileCache.init`'s callback parameter is
  named `onEvict` instead of `release` to avoid the collision (see comment
  at `TileCache.init` in map_engine.cb). Worth a real compiler fix later;
  out of scope for this M3 pass (no C++ touched here).

## M4: MapView control + example app

- MapView (library control wrapping CanvasView + camera + cache +
  scheduler): pan on pointer drag, wheel = zoomAt(cursor) (precise deltas
  pan on trackpads, discrete wheel zooms - match Google Maps), pinch =
  zoomAt, double-click zoom-in. Exposes onRegionChange callback and
  minZoom/maxZoom props.
- Example: example/ui/09-map/ launcher + shared app module following the
  05-gallery split (host-neutral app + thin launcher). Tile source is
  PROCEDURAL (checkerboard with "z/tx/ty" labels drawn via a tiny
  software rasterizer, or a Mandelbrot renderer - infinite and zoomable)
  behind a thread<T> worker with simulated latency, so tests stay offline
  and deterministic while genuinely exercising on-demand loading.
- Self-test (extend the ui_test.cb suite pattern; no new test files beyond
  the example itself, which is a new EXAMPLE, allowed): drive
  nativeCanvasPointer/Wheel to pan/zoom; oracles are camera state, the
  set of requested tile keys (dedupe proof), cache hit/evict counters,
  placeholder-then-real transitions, and pinned-visible eviction safety.
- M3 unit checks (camera math round-trip, anchor-zoom invariant, visible
  set at known cameras, LRU order) ride the same self-test.

### M4 landed

- Files: `example/ui/09-map/map_app.cb` (new - host-neutral MapApp Component
  + MapView control + mapSelfTest, no main) and `example/ui/09-map/map.cb`
  (new - thin Win32/Cocoa launcher via ui_native/host.cb, owns main,
  forwards --selftest). map_engine.cb untouched (tested M3 API reused
  as-is). Wired into `example_mac.sh` (selftest_case "map") and
  `example.bat` (--worker-map block, --heap-audit + headless selftest;
  map/map_app/map_engine added to the plain-sweep EXCLUDE).
- MapView public surface (class, owned by MapApp): `camera`/`tileCache`/
  `scheduler` (the field is `tileCache`, NOT `cache` - `cache` is the
  import-clause soft keyword and does not parse as a member name);
  `init(vpWpts, vpHpts)`; `setTileSource(Lambda<void(int z,int tx,int ty)>)`;
  `onRegionChange` (Lambda + has-flag); `build(ctx) -> CanvasView*`
  (FACTORY canvasView() form + setOn* wiring, rebuilt each render);
  `paint(Canvas)`; public input methods `pointerDown/Move/Up(x,y,buttons)`,
  `wheel(x,y,dx,dy,precise)` (precise=pan, discrete=zoomAt cursor at
  0.25/line - Google Maps semantics), `pinch(x,y,scale)` (dz=log2(scale)),
  `zoomCenter(dz)` (the HUD +/- buttons); diagnostics `paintCount`/
  `lastResolvedDraws`/`lastAncestorDraws`. minZoom/maxZoom ride the camera.
- Tile source seam: MapApp.startFetch spawns one Thread per dispatched
  key (TileJob wrapper, joined on the UI thread at completion); the worker
  generates deterministic checkerboard+coordinate-bit-encoded 256px BGRA
  pixels (makeTilePixels), sleeps ~3ms (simulated latency), then
  ctx.post()s finishTile: canvasCreateImage -> scheduler.tileReady ->
  chain drainReady dispatch -> ctx.invalidate.
- Placeholder approach chosen (b): paint = flat gray base coat, then each
  UNIQUE cached ancestor of any missing tile drawn ONCE at its full scaled
  extent, then resolved tiles on top. Chosen because drawImage has no
  src-rect overload, so a per-missing-tile ancestor crop is not
  expressible and drawing the ancestor per-child would blit it 4+ times.
- Self-test: 13 cases via the ui_test.cb runner (58 asserts on macOS, 56
  on Win32 where the 2 nativeCanvasPointer/Pinch driver asserts are gated
  out - the drivers are a recorded Win32 no-op parity gap). Cases 1-10
  fold in the 35 M3 scratch-harness asserts (map_engine_check.cb is now
  redundant); case 11 tile-pixel determinism; case 12 windowed pan/zoom
  (exact drag/wheel/pinch camera math, fractional zoom 2.5, anchor
  invariants, HUD buttons via nativeClickButton, onRegionChange); case 13
  windowed tile pipeline (first-paint request cap, repaint dedupe,
  ctx.post completion drain via waitUntil, all-resolved repaint through a
  counting Canvas fake, ancestor placeholders after zoom, cancelNotIn
  after a big pan). Windowed input drives MapView's public methods so the
  suite is host-neutral; the Cocoa drivers are exercised in the gated block.
- Gates (mac box): map selftest 13/13, example_mac.sh 35/0 (map joined),
  test.sh Release 160/0; live map_v1 launched, survived >4s rendering
  tiles unattended (paint+worker+post loop verified), then killed.
  example.bat owed on the Windows box (worker added, not yet run there).
- Compiler quirk found (workaround, no C++ change): `Math.pow(2.0, 0.5)`
  with LITERAL args resolves to the float overload (float-precision
  result); pass the exponent via a double variable. Same workaround the
  M3 harness used.
- Deferred to M5: double-click zoom (pointer events carry no clickCount),
  src-rect drawImage overload (proper placeholder crops), inertia +
  animated zoom + retry backoff (timer seam).

### M4 live-only bug: grey box / never-repainting tiles (FIXED 2026-07-12)

- Symptom (live Cocoa window only; headless 13/13 stayed green): the map
  showed a permanent GREY BOX (base coat only, 0 tiles), and interacting
  froze the app -> force-quit (exit 143, no .ips = a stall, not a signal
  crash). The self-test never covered it because it pumps posted completions
  manually and paints through a counting fake; the live path (real NSApp run
  loop driving drawRect + worker ctx.post) was untested.
- Root cause (seam, NOT app): the ctx.post completion delivered fine on the
  main thread (instrumented log: 8 dispatched / 8 worker-posted / 8
  finishTile-delivered) and populated the cache, but the canvas never
  repainted - only paint #1 (resolved=0) ever ran. finishTile calls
  ctx.invalidate() (dirty=true) but nothing consumed it: the Cocoa post-box
  runner `_runPostBoxImp` (cflat/core/ui_native/cocoa.cb) ran the boxed
  closure and returned WITHOUT pumping, whereas every native event handler
  does `handler(); pumpNative();`. A posted closure is morally an event
  handler (it mutates UI state and expects a repaint); the seam forgot to
  pump. /usr/bin/sample confirmed the main thread was idle in
  nextEventMatchingMask at rest (no deadlock) - just no repaint.
- Fix 1 (repaint seam): `_runPostBoxImp` now calls `pumpNative()` after
  `__uiRunPostBox`. pumpNative consumes dirty -> re-render -> applyTree ->
  ELEM_CANVAS setNeedsDisplay:YES -> drawRect repaints. Live log after the
  fix: paint #2 resolved=4, paint #3 resolved=8/8. This also fixes any other
  ctx.post user whose closure only invalidate()s (e.g. todo_app finishLoad).
- Fix 2 (delivery during gestures): `_cocoaPostEnqueue` scheduled the perform
  in NSDefaultRunLoopMode only, so a worker completion could not deliver while
  the main thread was in NSEventTrackingRunLoopMode (mouse drag / pinch) -
  tiles froze mid-gesture and workers piled up until the drag ended. It now
  uses `performSelectorOnMainThread:withObject:waitUntilDone:modes:` with the
  concrete modes [kCFRunLoopDefaultMode, NSEventTrackingRunLoopMode] (the
  kCFRunLoopCommonModes pseudo-string is NOT expanded by performSelector...
  modes:, so it must be listed explicitly). Default mode is still in the list,
  so hostDrainPosted (default-mode drain) keeps the headless self-tests green.
- Parity: Win32 has the same `WM_APP_CALL` runs-without-pump shape
  (win32.cb:3033) and default-mode-only PostMessage delivery; the map SKIPs on
  Win32 (canvas-input parity gap) so it was NOT changed here - if a Win32
  ctx.post repaint bug surfaces, mirror both fixes there. Owed on the Windows
  box.
- Verification oracle added (closes the gap that let this slip): `map.cb
  --probe [secs]` (default 3s) runs the LIVE window unattended, lets NSApp
  drive drawRect + completions for N seconds, then on the UI thread records
  lastResolvedDraws vs. the visible-tile count and closes the window
  (nativeCloseWindow ends the run loop). Exits 0 only if every visible tile
  resolved on screen, else 1 ("PROBE FAIL: 0/8 ... grey-box regression").
  Launch in the background and assert on the exit code:
    x64/Release/cflat.exe example/ui/09-map/map.cb -i example/ui -o out/map
    out/map --probe        # exit 0 = tiles resolved live; 1 = grey box
  Kept OUT of --selftest (needs a visible window). Validated as a real
  discriminator: against a core with Fix 1 reverted it exits 1 (0/8); with the
  fix it exits 0 (8/8). Implementation: mapLiveProbe + ProbeWatch watchdog
  thread in map_app.cb.
- Gates after the fix (mac box): map --selftest 13/13, map --probe 8/8 exit 0,
  example_mac.sh Release 35/0, test.sh Release 160/0 (core touched).
- Human-verified only: interactive pan/zoom smoothness + anchor correctness in
  the live window (map_v2 handed off). The probe proves tiles resolve live and
  the run loop stays responsive; it does not judge visual pan quality.

### M4 hang after zoom+pan: dictionary tombstone saturation (FIXED 2026-07-12)

- Symptom (live window, map_v2): after the user zoomed then panned, the app
  hung hard. /usr/bin/sample of the hung pid: the MAIN THREAD spent 3402/3402
  samples (100% of a 4s sample) with top-of-stack
  `_set_void_dictionary__u64__boolPtru64boolM_` (dictionary<u64,bool>.set),
  called from CfCanvasView drawRect -> the map paint path. An infinite loop
  inside dictionary.set during paint - not a deadlock (no lock in the stack).
- Root cause (CORE LIBRARY, not the map): `cflat/core/dictionary.cb` (and
  `hashset.cb`, same storage scheme) are open-addressed with linear probing and
  a 3-state status array (0=empty, 1=occupied, 2=deleted/tombstone). The
  load-factor gate in add()/set() tested only the LIVE entry count
  (`if (_count * 4 >= _capacity * 3) _rehash();`), never the tombstones.
  remove() sets status=2 and DECREMENTS `_count`, so churn keeps `_count` low
  and the rehash never fires. Empty slots are only ever consumed (an insert
  takes an empty slot or reuses a tombstone; a remove converts occupied ->
  tombstone) and are only restored by _rehash - so under insert/remove churn
  the empty-slot count decreases monotonically to ZERO. Every probe loop is
  `while (_status[slot] != 0)`, which terminates only on an empty slot: with no
  empty slot left and the key absent, set()/add()/get()/contains()/remove()
  spin forever. The map churns exactly this way every frame - TileCache.
  markVisible clears+repopulates `pinned` and TileScheduler.cancelNotIn removes
  from `queued` (map_engine.cb:207, :337) - and a zoom+pan storm maximizes the
  cycles, so some paint-time set() eventually never returned.
- Fix (core, no compiler change): both containers now track `_tombstones` and
  count it toward the load factor. `_maybeRehash()` fires when
  `(_count + _tombstones) * 4 >= _capacity * 3`, and `_rehash(newCap)` rebuilds
  the table dropping tombstones - GROWING (capacity * 2) only when the live
  count is itself dense, otherwise rehashing IN PLACE at the same capacity to
  purge tombstones. So churn with a bounded live set no longer grows memory, and
  an empty slot always exists. Tombstone accounting is kept exact:
  remove() increments, an insert that reuses a tombstone decrements, clear()
  and _rehash() reset to 0, copy() carries it (the status array is copied
  verbatim, tombstones included).
- Regression tests (no new files): `Test/test_core.cb` gains testDictChurn (12
  asserts) and testHashsetChurn (6) - thousands of insert/remove rounds with
  shifting keys, asserting they COMPLETE and that size/membership/values stay
  correct around the churn. Validated as a real discriminator: with the pre-fix
  core deployed, test_core.cb TIMES OUT (exit 124); with the fix it is
  483/483. map_app.cb mapSelfTest gains case 14 "tile churn terminates" (3
  asserts: 2000 markVisible/request/cancelNotIn rounds terminate, pinned holds
  the last window, cancelNotIn(empty) drains the queue) -> suite is now 14/14.
- Gates after the fix (mac box): test.sh Release 160/0 and Debug 160/0, map
  --selftest 14/14, map --probe 8/8 exit 0, example_mac.sh Release 35/0.
  Compiler C++ untouched, so no rebuild was needed; the fixed core .cb files
  were deployed to x64/Release/core/ and x64/Debug/core/.
- Owed on the Windows box: test.bat (core touched - dictionary/hashset are used
  everywhere, so this is a broad-blast change) and example.bat.
- Note (not a bug): `cache` is a reserved grammar keyword (CFlat.g4:753, the
  `import ... cache;` clause) and cannot be used as a variable name. Using it as
  a local inside a lambda body reports a confusing ANTLR error at the lambda's
  `=>` (the parser backtracks the whole lambda), not at the offending line. The
  MapView field is already named `tileCache` for this reason.

## M5: polish (needs the timer seam)

- Host timer: u64 hostStartTimer(int ms, bool repeating, closure) /
  hostCancelTimer(u64) - NSTimer on Cocoa, SetTimer on Win32; boxed
  closure like ctx.post.
- Inertial panning (velocity sample on pointer-up, decay ticks), animated
  zoom (zoom eases toward target over ~150ms), tile retry backoff.
- Overlays: marker list (world-anchored, drawn post-tiles, hit-testable ->
  onMarkerPress), polyline layer. HUD zoom +/- buttons: ordinary Button
  elements floated over the canvas - needs no new machinery if the map
  region and buttons are siblings in a column; true overlay stacking can
  reuse the backdrop z-order work if needed.
- Defer beyond M5: rotation/tilt, vector tiles, text collision, network
  tile sources (an http example can come later - socket layer exists).

## Sequencing and delegation

- M1+M2 together (one opus agent: both edit ui_native.cb + cocoa.cb; the
  boxed-closure host seams are the risky part).
- M3 in parallel (sonnet: pure logic + headless tests, new library file,
  no host contact).
- M4 after both (opus first pass, sonnet acceptable if M1-M3 land clean
  seams). M5 last (sonnet, mechanical once the timer seam is specced;
  timer seam itself may fold into M4 if inertia is wanted sooner).

## Verification gates

All .cb, no compiler C++ expected. Mac box: new map example self-test,
tui_demo/gallery/fedit self-tests stay green, bash test.sh Release,
bash example_mac.sh Release (map example joins the sweep). Deploy edited
core files to x64/<Config>/core/ when iterating. Owed on the Windows box
before commit: test.bat + example.bat (map example will SKIP on Win32
until the canvas-input parity issue is closed - record in the issue file).
Live visual review on the mac box: pan smoothness, anchor-zoom correctness,
progressive tile fill.

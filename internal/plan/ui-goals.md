# ui_native goals - large works that guide framework development

Status: goals (direction-setting, not a staged plan). Created 2026-07-13.

These are customer-driven milestones for the ui_native framework: each is framed
around what an app developer evaluating or shipping on the framework needs, not
around parity with the existing hosts. Host parity debt (ScrollView on
Win32/WinUI, canvas input/images, visual-polish parity, WinUI CanvasView,
tooltips on WinUI) is tracked in internal/issue/ and is being worked separately;
it is deliberately NOT a goal here. When a goal below first needs a parity item,
that item becomes a prerequisite of that goal.

Context at time of writing: element model + reconciler + testing story +
host-neutrality are proven (example/UI chapters 01-09, fedit flagship, gallery
on three hosts, doc/UI.md parity matrix v17).

## Goal 1: Desktop citizenship (app-shell completeness)

A customer's second day with any UI framework is spent on things that are not
widgets. Deliver, as one designed surface over the INativeHost + IUiContext
seams:

- Modal dialogs: message box, confirm, custom modal component.
- Clipboard (text first).
- Drag-and-drop of files into the window.
- Global keyboard shortcuts / accelerators on every host.
- Multi-window on all hosts (Win32-only today).
- Window icon, title, min-size control.
- Tray / dock presence.
- Open-URL / reveal-file-in-shell helpers.

Every app category needs these; each missing one reads as "framework cannot do
X" during evaluation. Land as one coherent API, not ad hoc per-host additions.

## Goal 2: The data-app milestone (editable grid + blessed async pattern)

Line-of-business tools are the biggest audience for a native no-Electron
framework. Two halves:

- A real grid: ListView is read-only report mode today. Add sortable columns
  (click header), column resize, inline cell editing, multi-select, while
  keeping the 100k-row virtualization oracle. Either ListView v2 or a new
  DataGrid element.
- A blessed async pattern: ctx.post is a primitive, not a pattern. Provide a
  documented, tested idiom for "run work off the UI thread, show progress,
  allow cancel, update state on completion" - likely a small ui_task.cb layer
  over thread<T> + ctx.post - plus timers/debounce (needed for
  search-as-you-type).

Forcing function and proof: a chapter-10 example, e.g. a CSV/SQLite browser
(open file, filter box, sortable grid, background load with progress).

## Goal 3: Visual identity (icons, wrapped text, a theme that looks designed)

Customers evaluate with their eyes before their editor. Missing pieces that are
visible in the first screenshot:

- Icons on buttons, tabs, tree nodes, list cells - a small built-in icon set,
  via .bmp or vector path glyphs through the existing Image/Canvas seams.
- Wrapping multi-line Text with layout-aware measurement in the layout
  protocol.
- Hyperlink text.
- One polished default theme, light and dark.

Subsumes the visual-polish parity debt (container backgrounds, font variants on
Win32/WinUI) but aims higher than parity.

## Goal 4: The shipping milestone (from cflat app.cb to a distributable app)

The self-contained-native-exe pitch currently ends at a bare .exe / Mach-O.
Cover the path from build to distributable:

- App icon + version resources on Windows.
- .app bundle generation on macOS (Info.plist, icon) - possibly a
  cflat --bundle mode.
- Doc chapter on signing / notarization.
- Settings persistence: a tiny ui_settings.cb (window position, recent files,
  user prefs).
- A project scaffold: cflat new ui-app, or a copyable template chapter.

This is what turns an evaluator into a shipper; nothing else planned covers it.

## Goal 5: A second flagship that is not an editor

fedit proves the IDE-shell shape; the map example (09) covers the
canvas/graphics axis. Customers pattern-match on "has anyone built MY kind of
app with this". Build one more flagship on the data/async axis that exercises
the weak spots above:

- Option A: chat/mail-style app (async network + list virtualization + wrapped
  text + images + notifications).
- Option B: the CSV/SQLite browser from Goal 2, grown to flagship polish.

Flagships discover the missing API before customers do.

## Priority

Goals 1 and 2 first: together they change "can I build my company's internal
tool with this" from "mostly" to "yes" - and that customer tolerates a young
framework in exchange for small native binaries. Goal 3 is the best third
because it makes the screenshots sell the other two.

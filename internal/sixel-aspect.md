# Sixel pixel aspect-ratio correction

Sixel pixels in Windows Terminal are NOT square — each pixel renders at the font cell's pixel aspect ratio (cellH:cellW). On a 10×20 cell font this is 2:1 (twice as tall as wide), so a circle drawn with equal x/y radius in Sixel space appears 2× taller on screen.

**Fix applied (2026-05-10):** Use an ellipse inclusion test in `drawCircle`:
```
dx² + dy² × (cellH/cellW)² ≤ r²
```
Pre-compute `g_pixAR2 = (cellH * cellH) / (cellW * cellW)` once from the live `getCellSize()` query (CSI 16 t).

**How to apply:** Any Sixel renderer that draws circular shapes must compensate for this. The correction factor comes from `Terminal.getCellSize()` in `terminal.cb`. The diagnostic flag (`--diag`) reports the pixel aspect ratio so it can be verified per-terminal.

**Related:** Separate from the viewport aspect ratio fix (use `hw * FOV` for x-projection and `hh * FOV` for y-projection so stars fill the full terminal width and height symmetrically).

# C interop alias follow-ups

The function-like macro body alias issue is fixed by retrying rejected function-like macros after
object-like aliases are registered. These related items remain open from the same investigation:

- Across separate import statements, the "never alias over an existing name" guard is
  order-dependent. If an alias-bearing header arrives first, a later real declaration with the
  same name is appended as a duplicate-signature overload. Today both orders resolve to the real
  function, but a future overload tie-break change could silently flip that result. When a real
  declaration arrives for a name currently bound by an alias, replace the alias entry instead of
  appending it.
- `FindFunctionSourceName(fn)` can return the alias name in diagnostics because two keys can share
  one `llvm::Function`; the result depends on `unordered_map` iteration order. This is cosmetic.
- The alias retry is deliberately best-effort: a macro that still fails to translate after the
  rewrite (cycle, arity mismatch, unsupported tokens) is dropped silently, exactly as the first
  registration pass drops it. Escalating any of those to a hard error breaks real headers.
- Struct-tag and type aliases are not registered with the `--symbol` / LSP symbol sink.

The separate-import and grouped-import probes remain in `scratch/fb_order_*.cb`; the implementation
does not change their existing behavior.

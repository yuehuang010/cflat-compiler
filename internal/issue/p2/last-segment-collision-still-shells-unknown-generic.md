# A bare unknown generic name is still shelled when it collides with a namespaced template's last segment

Filed 2026-08-06 by the round-1 review of `fix/generic-shell` (landed record in
[[interface-issue-queue]]). NOT a regression: measured identical on `b18ae7f` (the parent) and on
the post-fix binary.

Severity: **silent accept, no diagnostic.** A name that is declared nowhere at the use site
compiles, links and runs clean, where its non-generic twin gets `unknown type`. This is the
residue of the exact face `fix/generic-shell` set out to close; that commit closed it for a name
with no evidence anywhere and left it open for a name that happens to collide.

## Repros

Both compile, link and run clean, printing `ok`, with NO diagnostic - on both binaries.

`scratch/rv_b_ns_collision_sig.cb` - a bare, unqualified, un-aliased `Box<int>` in a top-level
signature, where top-level `Box` is declared nowhere:

```cflat
namespace N { class Box<T> { T v = default; }; }
int useIt(Box<int> b) { return 1; }
extern int main() { printf("ok\n"); return 0; }
```

`scratch/rv_m_cross_ns_collision.cb` - the cross-namespace variant, where the use site is inside a
DIFFERENT namespace than the template:

```cflat
namespace A { class Tag<T> { T v = default; }; }
namespace B {
    int useTag(Tag<int> t) { return 1; }
}
extern int main() { printf("ok\n"); return 0; }
```

| Repro | b18ae7f | post-`fix/generic-shell` |
|---|---|---|
| `rv_b_ns_collision_sig.cb` | runs, prints `ok`, exit 0 | identical |
| `rv_m_cross_ns_collision.cb` | runs, prints `ok`, exit 0 | identical |

Contrast: rename `Box` to a name that collides with nothing and the same file reports
`unknown type 'Box__i32'` on the post-fix binary. The collision is the whole difference.

## Root cause

`LLVMBackend::AnyGenericTypeTemplateNamed` (`cflat/LLVMBackend_Interfaces.cpp`) is the gate the two
`ForwardRefScanner` shell sites consult. Its last clause is deliberate: it accepts a spelling that
matches the LAST DOTTED SEGMENT of any template key.

```cpp
std::string tail = "." + spelledBase;
auto endsWithTail = [&](const std::string& n) { return n.ends_with(tail); };
for (const auto& kv : gts.genericStructTemplates) if (endsWithTail(kv.first)) return true;
// ... class / interface maps and the scanned + uncertain name sets, same shape
```

So `Box` matches the key `N.Box`, the shell is created, and the by-value use in the signature
resolves against it. The gate is accept-on-doubt by design (refusing only falls through to
`unknown type`), and this clause is the widest part of that doubt. It ignores namespace scoping
entirely - `rv_m` shows it matching across sibling namespaces, not just from an enclosing one.

## Constraint on any fix

The clause is LOAD-BEARING for three ratified messages, confirmed correct by the maintainer
2026-08-05. Their whole mechanism is this shell, for a bare `IV<int>` naming the namespaced key
`NS.IV`:

- `Test/errors/err_namespaced_generic_iface_bare_single_ns.cb` - `Unknown identifier 'Width'.`
- `Test/errors/err_namespaced_generic_iface_collide_identical.cb` - `Unknown identifier 'Width'.`
- `Test/errors/err_namespaced_generic_iface_collide_differing.cb` - `Unknown identifier 'Tag'.`

Built without the clause, all three report `unknown type 'IV__i32'` instead. Any narrowing must
leave those three byte-identical, or must be landed together with a re-ratification of the new
wording - which is a maintainer decision, not a fix-time one.

## Fix direction

**Unknown / deferred.** The obvious narrowing - require the matched key's namespace prefix to be
the current namespace or an enclosing one - would fix `rv_m` (sibling namespaces) but NOT `rv_b`,
where the use is at top level and `N.Box` is not enclosing; and `rv_b` is the shape the ratified
tests are built on, so that narrowing is exactly what moves them. The two cannot obviously be
separated by scoping alone.

A plausible direction that keeps both is to make the accept a DEFERRED one: shell the name but
record that it was accepted only by last-segment collision, and let the main pass decide - it has
the real namespace context the forward scan lacks, and it is where the `Unknown identifier`
messages are actually emitted. That is a larger change than a predicate tweak and has not been
prototyped.

## Test coverage

None. The two cells above live only as scratch probes. A regression leg for the DESIRED behaviour
cannot be added until the desired behaviour is decided, since it would have to be an
`expect_error` for a message that does not exist yet.

Related: [[interface-issue-queue]] (the `fix/generic-shell` landed record),
[[incomplete-layout-message-blames-c-interop]]

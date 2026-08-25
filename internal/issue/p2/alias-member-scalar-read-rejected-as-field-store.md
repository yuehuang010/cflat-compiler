# Storing a SCALAR member read through an alias-bound local into a field is rejected

Filed 2026-08-25, found during Windows verification of the ui-native controls tiers.

## Repro

```cflat
class Win
{
    void* hwnd = nullptr;
    string title = default;   // <- any owning field arms the false positive
};

struct Args
{
    void* owner = nullptr;
};

list<unique Win*> _wins = default;

alias Win* cur() { return _wins.get(0); }

extern int main()
{
    _wins.add(new Win());
    Win* wnd = cur();
    Args a = default;
    a.owner = wnd.hwnd;   // rejected
    printf("%p\n", a.owner);
    return 0;
}
```

```
repro.cb(21,4): cannot store an 'alias' value 'wnd' into a field; it borrows storage it
does not own and would dangle. Use '.copy()' for an independent owned copy.
```

Remove the `string title` field and the same program compiles and runs. The value being
stored is `wnd.hwnd` - a plain `void*` copied BY VALUE out of the aliased object - not the
alias pointer itself and not the owning field, so nothing can dangle.

## Root cause (hypothesis, unverified in code)

The alias-into-field check classifies the store by the PROVENANCE of the base variable
(`wnd`, alias) rather than by what is actually stored (a scalar member read). It only arms
when the pointee type is an owning type (has an owning field), which is why the win32.cb
`Window` (many owning fields) hit it while trivial repros did not.

## Impact

`cc.hwndOwner = wnd.hwnd;` in `cflat/core/ui_native/win32.cb` (routeColorWell) had to be
rewritten as `cc.hwndOwner = curWin().hwnd;` - same semantics, checker-shaped. Any code
that binds `alias T*` to a local and copies a scalar member into a struct field trips this.

## Fix direction

When the stored expression is a member read producing a non-owning scalar (int/ptr/bool),
do not propagate the base variable's alias classification to the stored value. The check
should key on the stored VALUE's type/ownership, not the access path's provenance.

## Workaround

Call through the alias-returning function directly at the use site
(`a.owner = cur().hwnd;`), or copy via a plain local scalar first.

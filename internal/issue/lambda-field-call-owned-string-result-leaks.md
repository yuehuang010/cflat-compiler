# Calling a `Lambda<string(...)>` CLASS FIELD leaks the returned owned string

Status: OPEN. Found 2026-07-13 while validating interface fields for Phase 3 of
`internal/plan/ui-interface-refactor.md`. PRE-EXISTING (reproduces on a clean tree);
NOT caused by interface fields.

## Summary

Invoking a closure through a **class field** and binding the owned `string` result to a
local leaks the string buffer. Invoking the *same* closure through a **local variable**
does not. The call-site kind (field vs local) is the only difference.

## Repro (leak-clean except for the one field call)

```cflat
import "string.cb";
import "function.cb";
import "diagnostic/heap_audit.cb";

class W { Lambda<string(int)> f = default; };

extern int main()
{
    HeapAudit.enable();
    {
        Lambda<string(int)> loc = (int r) => { return "x" + r.toString(); };
        string a = loc(1);                 // LOCAL closure -> no leak
        printf("a=%s\n", a.data());

        W* w = new W();
        w.f = (int r) => { return "x" + r.toString(); };
        string b = w.f(1);                 // FIELD closure -> LEAKS the 3-byte buffer
        printf("b=%s\n", b.data());
        delete w;
    }
    HeapAudit.reportLeaks();
    return 0;
}
```

`cflat pa.cb -o pa.exe && pa.exe` prints exactly one
`*** cflat heap-audit: LEAK ... size=3 ***`.

Also reproduces when the result is a temporary (`printf("%s", w.f(1).data())`) and when
the object is reached through a downcast - concrete (`W* w = e as W;`) and interface
(`IW w = e as IW;`) forms leak identically, which is what rules interface fields out as
the cause.

## Root cause (hypothesis, not yet confirmed in the backend)

The owned-string result of a call is registered for destruction based on how the CALLEE
is named. A call through a plain local closure goes down the path that registers the
returned owned temp; a call whose callee is a member-expression (`w.f(...)`) appears to
be classified as a member/method call and the owned return value is never registered,
so nothing destructs it. Look at the call-emission path in `MainListener.h` where a
Lambda-typed field is loaded and invoked, and compare the owned-return registration with
the local-closure path.

## Why it is not currently biting the UI framework

The three `ui_native.cb` closure seams that call a Lambda field
(`__uiListRowText`, `__uiTreeLabel`) *forward* the result out of a `move string`
function (`return b.cb(row, col);`) rather than binding it to a local, so ownership
transfers to the caller and `example.bat` stays leak-clean. The bug is one `string s =`
away at any time.

## Fix direction

Make the owned-return registration independent of the callee's expression shape. A
regression case belongs in `Test/test_function.cb` (Lambda field returning `string`,
asserted leak-free under HeapAudit).

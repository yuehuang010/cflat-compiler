# Method call on a parenthesized expression fails: "the function '(...)' is not known"

Filed 2026-08-25, found during Windows verification of the ui-native controls tiers.

## Repro

```cflat
extern int main()
{
    string a = "hello";
    string b = "world";
    printf("%s\n", (a + " " + b).data());
    return 0;
}
```

```
repro.cb(5,20): the function '(a + " " + b).data()' is not known.
```

The same member access without parentheses on a plain variable works; the failure is
specific to `(<expression>).method()` - the postfix member-call path treats the
parenthesized primary expression's text as a function name instead of evaluating it and
dispatching the method on the result.

## Impact

Natural spellings like `("<a>" + l.text + "</a>").data()` or `(x + y).toString()` do not
compile. Hit twice in macOS-authored core code (`ui_native/win32.cb`, `ui_native/winui.cb`)
because the pattern compiles nowhere, so authors keep re-discovering it.

## Fix direction

In the member-call handler (MainListener.h postfix expression path), when the callee base
is a parenthesized expression, evaluate the inner expression to a value and resolve the
method against its type, same as a named-variable base. Remember temp ownership: the
parenthesized result may be an owned temporary (string concat), so it must be registered
for destruction like other call-site temps.

## Workaround

Bind to a local first: `string t = a + " " + b; printf("%s\n", t.data());` (applied in
win32.cb and winui.cb).

# Method call on a parenthesized 'is'/'as' expression is not parsed

Filed 2026-07-28, found while fixing the stack-value `as` crash (that issue is closed).
PRE-EXISTING, and independent of the operand shape.

Severity: rejected valid source, clean diagnostic (no crash).

## Repro

```cflat
interface IMore { int more(); };
class Impl : IMore { int v = 7; int more() { return v; } };

extern int main()
{
    Impl* p = new Impl();
    printf("%d\n", (p as IMore).more());   // error
    delete p;
    return 0;
}
```

```
repro.cb(7,19): unknown function '(pasIMore)'
```

A plain parenthesized operand is fine - `(p).more()` compiles and prints 7 - and the
stack-value form `(s as IMore).more()` fails identically, so this is about the `as`
sub-expression in postfix position, not about the source being a pointer or a value.

## Root cause

Partially diagnosed. The postfix-expression path does not recognise a parenthesized
`typeCheckExpression` as a callable receiver: it falls through to the "call a function by
name" branch and uses the whitespace-stripped source text of the whole parenthesized
expression as the function name, which is what produces `'(pasIMore)'`. Not traced any
further than that.

## Fix direction

Handle a parenthesized expression whose content is a `typeCheckExpression` in
`ParsePostfixExpression` the same way a parenthesized primary is handled today, so the
`is`/`as` result becomes the receiver of the member access. Workaround for users is to
bind the cast to a local first (`IMore m = p as IMore; m.more();`).

# Nested named brace initializers are not supported outside json_const

## Summary

The fieldInit grammar alternative added for `json_const` also parses ordinary
declaration initializers, but those initializers must currently reject the shape.

## Repro

```cflat
struct Inner { int count = default; };
struct Cfg { Inner inner = default; };
extern int main() { Cfg c = { inner = { count = 3 } }; return 0; }
```

## Root cause

Ordinary initializer codegen predates the alternative and assumes every named
fieldInit has an assignmentExpression. The new form has only initializerList.

## Fix direction

Implement recursive named-brace initialization in declaration codegen. Reuse the
initializer shape settled by Phase B/C of internal/plan/windows-manifest-design.md.

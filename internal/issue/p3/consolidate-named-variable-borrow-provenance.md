# Consolidate fragmented borrow provenance and source classification

Filed 2026-08-13 during the integrated Q01-Q15 review.

## Summary

The Q fixes correctly preserved more ownership facts, but represented each new fact as another
parallel group of fields on `NamedVariable` and another set of source-classification locals in the
listeners. The representation now makes propagation, retirement, shadowing, reset, and diagnostic
wording separate obligations for every borrow kind.

Examples on `NamedVariable` include:

- `BorrowsOwnedString` plus `OwnedStringBorrowBlock` and `OwnedStringBorrowFunction`.
- `BorrowsOwnedElement`, its container/external-owner fields, block, and function.
- `IsBonded`, `BondByAddress`, `BondedSources`, block, and function.
- Interface-box provenance, borrowed-by-value parameter provenance, field-path provenance, and
  raw-array provenance as independent flags and strings.

`MainListener_Declarations.cpp` and `MainListener_Expressions.cpp` then reconstruct overlapping
facts into large groups of `srcIs...`, `srcBorrows...`, `srcPointsTo...`, and `srcRaw...` locals.
Adding a new assignment door requires manually copying the right subset.

This is a maintainability issue with correctness impact: missing one propagation or retirement arm
silently changes an ownership decision, and the compiler has no type-level way to require that all
members of one provenance fact move together.

## Simplification direction

Introduce a small value object for binding provenance, for example:

```cpp
struct BorrowProvenance
{
    BorrowKind Kind = BorrowKind::None;
    std::string Origin;
    std::vector<std::string> Sources;
    llvm::BasicBlock* EstablishedIn = nullptr;
    llvm::Function* Function = nullptr;
    bool ThroughField = false;
};
```

The exact shape needs design, but the important properties are:

1. One assignment copies or clears a whole provenance fact.
2. One helper decides whether a rebind may retire it.
3. Declaration initialization and reassignment consume the same classified RHS result.
4. Diagnostics render from the provenance kind and origin rather than unrelated flags.
5. Runtime ownership state, such as a raw-array count, stays out of this compile-time record.

Do this incrementally by migrating one already-paired family first (owned string and owned element
borrow facts are the closest twins), keeping behavior unchanged and running the full host suite.


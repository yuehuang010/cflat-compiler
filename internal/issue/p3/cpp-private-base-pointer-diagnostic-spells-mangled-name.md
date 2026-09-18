# The private-base pointer conversion diagnostic prints the mangled specialization name

Found 2026-09-17 by the review of fix/batch-cpp-bind1 (macOS arm64, Release). Pre-existing:
reproduces identically on the untouched pointer-to-pointer path.

## Repro

A C++ class `Priv` deriving privately from `Box<int>`; `Box<int>* b = &priv;` (or the
reference-return form) is refused with

    cannot convert 'rvd.Priv*' to 'rvd.Box$int*'

`Box$int` is the internal mangled spelling; the invertible-mangling ruling requires every
user-facing surface to demangle (`rvd.Box<int>*`).

## Fix direction

`AdjustCxxPointerForStore` (`cflat/LLVMBackend_CInterop.cpp`) formats the destination with the
raw type name; route it through the demangling spelling helper used by other diagnostics.

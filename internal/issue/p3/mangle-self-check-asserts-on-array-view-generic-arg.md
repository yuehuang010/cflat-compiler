# Mangler self-check asserts on an `int[]` generic argument (Debug only)

Bucket: Debug-only assertion. Filed 2026-09-07 during the `--check` materialization audit
(24a873b1 and its follow-up); found by a Debug sweep over every error fixture.

## Repro

    x64/Debug/cflat Test/errors/err_generic_array_view_arg.cb -i Test/library

aborts (exit 134) with `Assertion failed: (DemangleType(compiler, result, spelling)),
function MangleFunctionType, file TypeMangling.cpp, line 487`. Same under `--check`.
Release prints `PASS: expected error received` because the self-check is `#ifndef NDEBUG`.

## Root cause (hypothesis, not yet verified)

The fixture instantiates a template with an array-view argument (`int[]`, and `int[]*`
from a `T*` in the body). The mangler emits a symbol for the ill-formed instantiation
before the argument-side diagnostic fires, and `DemangleType` cannot parse the view
spelling back, so the invertibility self-check trips. Either the mangler must not be
reached for a rejected instantiation, or the view spelling needs a demangle rule
(see internal/plan/invertible-type-mangling.md for the scheme).

## Fix direction

Reproduce under lldb at TypeMangling.cpp:487, dump `result`, decide whether the
instantiation should have been refused before mangling (preferred: the fixture expects the
error at the argument) or whether `DemangleType` lacks a case. Keep the assertion.

Related, unexercised here: a Windows-only test aborts in Debug on
`CastInst::castIsValid` (Constants.cpp:2376); noted, not investigated.

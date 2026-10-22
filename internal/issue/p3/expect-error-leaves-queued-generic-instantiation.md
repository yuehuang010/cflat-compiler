# expect_error block leaves a queued generic instantiation, raising a second "never completed" error at module end

Summary: when an error inside a scoped expect_error block happens while a generic instantiation it requested is
queued (e.g. `Buf<int, k>` with an unfoldable k), the block passes but the queued instantiation is reported at module
end as never completed, so the file fails. Found while writing a local-typedef shadow leg in const phase 1 round 4.

Fix direction: an expect_error scope that swallows an error must also drop instantiation requests queued inside it.

## T354 Phase A narrowing (2026-10-20)

The initial run did not reproduce the reported second "never completed" error in
three probes: a nonconstant `Buf<T, N>` argument under file-scope `expect_error`,
a matching-substring version of that probe, and a queued pointer-type request
followed by a swallowed local error. The generic-parameter variant errors during
generic-body pre-scan, before its local expectation is armed. The file-scope
recovery already clears pending instantiations, so that path is not sufficient.

Keep this issue active and narrow the missing evidence to a statement-local
expectation with a generic request whose source site queues after the expectation
arms, then a caught diagnostic that abandons the queue before its drain. The
local scoped recovery in `MainListener_Statements.cpp` does not rewind
`pendingInstantiations`; the file-scope catch in `MainListener_Declarations.cpp`
does. Do not broaden the fix until such a request is demonstrated.

# expect_error block leaves a queued generic instantiation, raising a second "never completed" error at module end

Summary: when an error inside a scoped expect_error block happens while a generic instantiation it requested is
queued (e.g. `Buf<int, k>` with an unfoldable k), the block passes but the queued instantiation is reported at module
end as never completed, so the file fails. Found while writing a local-typedef shadow leg in const phase 1 round 4.

Fix direction: an expect_error scope that swallows an error must also drop instantiation requests queued inside it.

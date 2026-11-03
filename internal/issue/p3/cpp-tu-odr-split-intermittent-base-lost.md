# Intermittent: a C++ TU sees a [cpp] struct without its C++ base (ODR split), freeing through the wrong delete

Summary: one test.sh Release run on master 3deaad3e (box 7 run 2, 2026-10-31) built a test_cpp_interop.bin that returns
3421 (leg ~15739, T320UniqN: `[cpp] struct T320UN : cppon.Cls` with its own operator new only, `unique<T320UN>` drop must
free through cppon::Cls::operator delete). In that binary the default_delete<T320UN> / ~T320UN code (C++ side) calls the
global ::operator delete(void*, size_t) and T320UN's D2 does not call cppon::Cls::~Cls, while the CFlat side and the
constructor do see the base: two C++ TUs disagreed about T320UN's declaration.
Not reproduced: the same exe, same source, cold fresh CFLAT_CACHE_BUILD_STAMP, warm bar stamp, warm + --error-on-cpp-reparse,
and a second full test.sh run all pass; an isolated repro passes cold and warm. The bad compile ran under parallel
test.sh load with the other C++ interop tests sharing the header/request cache.
Evidence: scratch/repro_keep/odr_t320/ (bad_rc93.bin, its compile log, bad.dis vs good.dis call-chain diff).
Fix direction: find where the C++ request for unique_ptr<T> / default_delete<T> gets the [cpp] struct prelude (incremental
executor TU vs request-cache entry); check that the prelude text for a [cpp] struct always includes its base list and that a
cache entry keyed without the base cannot be reused; loop the full test.sh interop tier under load to reproduce.
2026-11-01: second sample - the fix/mix18ctor torch bar (test.sh -j 4 under load ~6.5, another bar's Luna run in parallel) built a
test_cpp_interop.bin returning 3421 (rc 93) on every run; binary kept as scratch/repro_keep/odr_t320/bad2_mix18ctor_rc93.bin.
2026-11-01 Opus investigation (1 h, report scratch/repro_keep/odr_t320/odr_investigation_2026-11-01.md): no repro in 6 setups (fresh,
warm, full test.sh, shared-group siblings cold, err tests, aged markers). Only the cpp_interop_basic.h group companion defines ~T320UN, so
the bad compile saw T320UN without its base INSIDE that one group - not a cross-TU split. Both failing bars ran on a bloated local cache
(bar_wt.sh uses --init-local, 18 min prewarm vs 2 min clean). Hypothesis (unverified): a failed chunk attempt defines a [cpp] struct while
::cppon::Cls is not visible; the rollback keeps that class, the retry hits "redefinition of 'cf5x...'" (seen for enable_shared_from_this
structs), drops it and reuses the base-less class silently. Next: reproduce with the bloated cache + -v and grep "dropped declarations after"
/ "redefinition of 'cf6x54333230554e'"; candidate guard = refuse a retry whose dropped part holds a generated __cflat_user record.

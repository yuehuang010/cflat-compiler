# test.sh Release exceeds the suite time budget (~200 s warm / ~300 s cold) on an idle Mac

Filed 2026-11-17 (box 16), test infrastructure. Maintainer budget 2026-09-25: under ~200 s warm, ~300 s cold.

Measured on master after the box-16 landings, no agents running, `bash test.sh Release -j 4`:
- cold (C++ header cache just wiped by scratch/libs_perf.sh, no prewarm): Elapsed 675 s, load ~2-3.
- immediate warm rerun: Elapsed 357 s, load ~2.7. 1998 passed, 0 failed.
- Landing-bar runs (prewarmed) have sat at 370-470 s every day since 2026-11-14 (500-590 s on 2026-11-10), so
  this is not a regression from one landing; the suite drifted past budget as legs were added.

Where the time goes (warm run, out-linux/results/*.time): the per-test times sum to 533 s over 1998 tests
(133 s at -j 4), but the slowest single test is test_cpp_interop at 132 s, then cxx_request_cache_consistency
48 s, test_cpp_interop_bridge 18 s, global_default_fold_warm_parity 12 s. test.sh already starts the C++
interop fixtures first (test.sh ~424). The remaining ~220 s of Elapsed is outside the per-test .time windows:
the warm-cache second pass (test.sh ~365-465, --init-local-prune then every Test/errors fixture again), the
leg lint (static + dynamic phases) and serial setup.

Fix direction (measure each before changing): (1) time the phases separately (cold pass, prune, warm pass,
lint) with date stamps in test.sh; (2) split test_cpp_interop.cb into two or three fixtures so the critical
path drops below the parallel floor - fixture split needs the maintainer's OK (no-new-test-files rule); (3)
run the warm pass in parallel with the remaining cold tests instead of after them.
